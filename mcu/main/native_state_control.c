#include "native_state_control.h"
#include "native_state_shortcuts.h"
#include "native_state_ui.h"
#include "virtual_cart.h"
#include "fpga_tx.h"
#include "osd.h"
#include "pc_backup.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
static portMUX_TYPE Lock=portMUX_INITIALIZER_UNLOCKED;
static NativeStateView View;
static NativeShortcutState Keys;
static unsigned Pending, PendingSlot, ShortcutEvent;
static uint32_t Session, PendingSession, ShortcutSession;
static bool Wanted, InFlight, FromShortcut, CancelDismiss;
static uint16_t RawButtons=0x200;
static uint32_t DismissAt;
static int Setting=-1, SlotSetting=-1;
static uint32_t Millis(void) { return (uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS); }
static void Read(NativeStateView *Out) { portENTER_CRITICAL(&Lock);*Out=View;Wanted=true;portEXIT_CRITICAL(&Lock); }
static bool SlotValid(unsigned Slot) { return Slot>=1 && Slot<=kNativeStateSlots && (View.ValidMask>>(Slot-1)&1); }
static bool Request(bool Load,unsigned Slot) {
    bool Accepted=false;portENTER_CRITICAL(&Lock);
    if(Slot>=1 && Slot<=kNativeStateSlots && !Pending && !InFlight && Session && View.Available && View.Supported && (!Load || SlotValid(Slot))) {
        DismissAt=0;CancelDismiss=false;PendingSession=Session;PendingSlot=Slot;Pending=Load?2:1;View.Phase=Load?kStateUILoading:kStateUISaving;Accepted=true;
        // The slot used last becomes the shortcuts' slot.
        if(View.Slot!=Slot){View.Slot=(uint8_t)Slot;View.Valid=SlotValid(Slot);SlotSetting=(int)Slot;}
    }
    portEXIT_CRITICAL(&Lock);return Accepted;
}
static void SetShortcuts(bool Enable) { portENTER_CRITICAL(&Lock);Setting=Enable?1:0;portEXIT_CRITICAL(&Lock); }
static void Waiting(void) { portENTER_CRITICAL(&Lock);if(InFlight)View.Phase=kStateUIWaiting;portEXIT_CRITICAL(&Lock); }
uint16_t NativeStateControl_Buttons(uint16_t Raw) {
    uint32_t Now=Millis();bool Consume=false;portENTER_CRITICAL(&Lock);
    // Only a new press keeps the menu open: the A press that chose Load (or
    // the shortcut's keys) may still be held while the action runs.
    const uint16_t Pressed=Raw&~RawButtons&0xff;RawButtons=Raw;
    unsigned Event=NativeShortcut_Update(&Keys,Raw,Now,View.Shortcuts,&Consume);
    if(Event && !ShortcutEvent && !Pending && !InFlight){ShortcutEvent=Event;ShortcutSession=Session;}
    if(Pressed && (InFlight || DismissAt))CancelDismiss=true;
    portEXIT_CRITICAL(&Lock);
    return Consume?(Raw&0x300):Raw;
}
static void Error(const char *Text) { portENTER_CRITICAL(&Lock);View.Phase=kStateUIError;snprintf(View.Message,sizeof(View.Message),"%s",Text);portEXIT_CRITICAL(&Lock); }
static void Step(void) {
        portENTER_CRITICAL(&Lock);
        bool Poll=Wanted || View.Shortcuts;Wanted=false;
        unsigned Event=ShortcutEvent;uint32_t EventSession=ShortcutSession;ShortcutEvent=0;
        int Option=Setting;Setting=-1;
        int SlotOption=SlotSetting;SlotSetting=-1;
        portEXIT_CRITICAL(&Lock);
        if(SlotOption>0) {
            uint8_t Value=(uint8_t)SlotOption;nvs_handle_t N;
            if(nvs_open("native_states",NVS_READWRITE,&N)==ESP_OK){if(nvs_set_blob(N,"slot",&Value,sizeof(Value))==ESP_OK)(void)nvs_commit(N);nvs_close(N);}
        }
        if(Option>=0) {
            uint8_t Value=(uint8_t)Option;nvs_handle_t N;
            esp_err_t R=nvs_open("native_states",NVS_READWRITE,&N);
            if(R==ESP_OK){R=nvs_set_blob(N,"shortcuts",&Value,sizeof(Value));if(R==ESP_OK)R=nvs_commit(N);nvs_close(N);}
            if(R==ESP_OK){portENTER_CRITICAL(&Lock);View.Shortcuts=Value!=0;memset(&Keys,0,sizeof(Keys));portEXIT_CRITICAL(&Lock);}
            else Error("Could not save setting");
        }
        if(Poll) {
            bool Supported=false,Uncertain=false;uint8_t Mask=0;uint32_t NewSession=0;
            esp_err_t R=VirtualCart_NativeSlotInfo(&Supported,&Mask,&Uncertain,&NewSession);
            bool Available=!PCBackup_IsEnabled();
            portENTER_CRITICAL(&Lock);
            if(R==ESP_OK){
                if(!Pending && !InFlight && (Session!=NewSession || View.Supported!=Supported || View.ValidMask!=Mask))View.Phase=kStateUIIdle;
                if(Session!=NewSession)memset(&Keys,0,sizeof(Keys));
                Session=NewSession;View.Supported=Supported;View.ValidMask=Mask;View.Valid=SlotValid(View.Slot);View.Available=Available;
            }
            else if(R!=ESP_ERR_NOT_FINISHED){View.Available=false;View.Valid=false;View.ValidMask=0;}
            if(!Available)View.Available=false;
            if(Uncertain){View.Available=false;View.Phase=kStateUIError;snprintf(View.Message,sizeof(View.Message),"Power off to recover");}
            bool Enabled=View.Shortcuts;portEXIT_CRITICAL(&Lock);
            if(Enabled)FPGA_Tx_RequestButtons();
        }
        if(Event) {
            NativeStateUI_RequestOpen();
            portENTER_CRITICAL(&Lock);
            bool Closed=(RawButtons&0x200)!=0;
            bool SameGame=EventSession!=0 && EventSession==Session;
            FromShortcut=true;CancelDismiss=false;
            portEXIT_CRITICAL(&Lock);
            if(Closed)FPGA_Tx_PulseButtons(0x100);
            portENTER_CRITICAL(&Lock);unsigned Slot=View.Slot;portEXIT_CRITICAL(&Lock);
            if(!SameGame || !Request(Event==2,Slot)) {
                portENTER_CRITICAL(&Lock);FromShortcut=false;portEXIT_CRITICAL(&Lock);
                char Text[32];snprintf(Text,sizeof(Text),"Slot %u is empty",Slot);
                Error(Event==2?Text:"State saving unavailable");
            }
        }
        portENTER_CRITICAL(&Lock);unsigned Operation=Pending,OperationSlot=PendingSlot;uint32_t OperationSession=PendingSession;Pending=0;InFlight=Operation!=0;portEXIT_CRITICAL(&Lock);
        if(Operation) {
            esp_err_t R=VirtualCart_StateCaptureSlot(Operation==2,OperationSlot,OperationSession);
            portENTER_CRITICAL(&Lock);InFlight=false;
            if(R==ESP_OK){View.ValidMask|=1u<<(OperationSlot-1);View.Valid=SlotValid(View.Slot);}
            if(R==ESP_OK && Operation==1 && VirtualCart_LastStateSdFailed()){
                View.Phase=kStateUIError;
                snprintf(View.Message,sizeof(View.Message),"Saved, but not on\nthe SD card");
            }
            else if(R==ESP_OK){View.Phase=Operation==2?kStateUILoaded:kStateUISaved;}
            else {View.Phase=kStateUIError;snprintf(View.Message,sizeof(View.Message),"%s",
                R==ESP_ERR_TIMEOUT?"Save busy - try again":
                (R==ESP_ERR_INVALID_CRC || R==ESP_ERR_INVALID_VERSION || R==ESP_ERR_INVALID_SIZE)?"State file on SD\ncard is damaged":
                R==ESP_ERR_NOT_FOUND?"No state available":"State action failed");}
            // A successful load closes the menu so play resumes at once; a
            // shortcut action also closes it after briefly showing the result.
            if(R==ESP_OK && (FromShortcut || Operation==2))DismissAt=Millis()+(FromShortcut ? 1200 : 300);
            FromShortcut=false;portEXIT_CRITICAL(&Lock);
        }
        portENTER_CRITICAL(&Lock);
        // Bit 9 set: the menu is already closed (pulsing would reopen it).
        // Buttons still held: wait for their release, then close.
        bool Close=DismissAt && (int32_t)(Millis()-DismissAt)>=0;
        if(Close && !CancelDismiss && !(RawButtons&0x200) && (RawButtons&0x1ff))Close=false;
        else if(Close){Close=!CancelDismiss && !(RawButtons&0x3ff);DismissAt=0;}
        portEXIT_CRITICAL(&Lock);
        if(Close)FPGA_Tx_PulseButtons(0x100);
}
static void Worker(void *Arg) {
    (void)Arg;
    for(;;) { vTaskDelay(pdMS_TO_TICKS(40)); Step(); }
}
esp_err_t NativeStateControl_Init(void) {
    /* Shortcuts default ON; only an explicit stored 0 (user chose OFF) disables them. */
    uint8_t Value=1;size_t Size=sizeof(Value);nvs_handle_t N;
    if(nvs_open("native_states",NVS_READONLY,&N)==ESP_OK){if(nvs_get_blob(N,"shortcuts",&Value,&Size)!=ESP_OK || Size!=1)Value=1;nvs_close(N);}
    View.Shortcuts=Value!=0;View.Available=true;
    /* The shortcuts' slot survives power-off; slot 1 unless another was used. */
    uint8_t Slot=1;Size=sizeof(Slot);
    if(nvs_open("native_states",NVS_READONLY,&N)==ESP_OK){if(nvs_get_blob(N,"slot",&Slot,&Size)!=ESP_OK || Size!=1 || Slot<1 || Slot>kNativeStateSlots)Slot=1;nvs_close(N);}
    View.Slot=Slot;
    NativeStateUI_Register(Read,Request,SetShortcuts);VirtualCart_RegisterStateWait(Waiting);
    return xTaskCreate(Worker,"native_states",6144,NULL,2,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
