#include "native_state_ui.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
static NativeStateRead Read;
static NativeStateRequest Request;
static NativeStateShortcuts Shortcuts;
static lv_obj_t *Panel, *Title, *Status, *Rows[4], *Note, *Footer;
static bool Expanded;
/* Slot: 0 on the slot list, 1-3 while a slot's actions are shown. */
static unsigned Slot;
static atomic_bool OpenRequested;
void NativeStateUI_RequestOpen(void) { atomic_store(&OpenRequested,true); }
bool NativeStateUI_TakeOpenRequest(void) { return atomic_exchange(&OpenRequested,false); }
static unsigned Choice;
static const uint32_t Accent=0xff3399;
enum { kRowSave, kRowLoad, kRowCancel };
static bool Busy(const NativeStateView *S) { return S->Phase>=kStateUIWaiting && S->Phase<=kStateUILoading; }
static bool Saved(const NativeStateView *S,unsigned N) { return N>=1 && N<=kNativeStateSlots && (S->ValidMask>>(N-1)&1); }
static void Rebuild(void) { if(Panel)lv_obj_del(Panel);Panel=NULL; }
void NativeStateUI_Register(NativeStateRead R,NativeStateRequest Q,NativeStateShortcuts H) { Read=R;Request=Q;Shortcuts=H; }
bool NativeStateUI_IsOpen(void) { return Expanded; }
void NativeStateUI_Open(void) { Expanded=true;Slot=0;Choice=0;Rebuild(); }
bool NativeStateUI_HandlesButton(Button_t B) { return atomic_load(&OpenRequested) || (Panel && (Expanded || B==kButton_A)); }
static lv_obj_t *Label(int X,int Y,int W,bool Large) {
    lv_obj_t *L=lv_label_create(Panel);lv_obj_add_style(L,Large?OSD_GetStyleTextWhite_L():OSD_GetStyleTextWhite(),0);
    lv_obj_set_pos(L,X,Y);lv_obj_set_width(L,W);lv_label_set_long_mode(L,LV_LABEL_LONG_WRAP);return L;
}
static void Show(lv_obj_t *L,bool V) { if(V)lv_obj_clear_flag(L,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(L,LV_OBJ_FLAG_HIDDEN); }
static void Row(unsigned I,const char *Text,bool Enabled,bool Selected) {
    Show(Rows[I],true);
    lv_label_set_text(Rows[I],Text);lv_obj_set_style_text_color(Rows[I],lv_color_hex(Enabled?0xffffff:0x777777),0);
    lv_obj_set_style_border_color(Rows[I],lv_color_hex(Accent),0);lv_obj_set_style_border_width(Rows[I],Selected?1:0,0);
    lv_obj_set_style_pad_left(Rows[I],3,0);lv_obj_set_style_pad_top(Rows[I],1,0);
}
/* Opening a slot puts the cursor on Load when it holds a state, so A, A never overwrites it. */
static void OpenSlot(const NativeStateView *S,unsigned N) { Slot=N;Choice=Saved(S,N)?kRowLoad:kRowSave; }
static void CloseSlot(void) { Choice=Slot?Slot-1:0;Slot=0; }
OSD_Result_t NativeStateUI_Draw(void *Screen) {
    if(!Screen)return kOSD_Result_Err_NullDataPtr;
    NativeStateView S={0};if(Read)Read(&S);
    if(!Panel) {
        Panel=lv_obj_create(Screen);lv_obj_remove_style_all(Panel);lv_obj_clear_flag(Panel,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(Panel,Expanded?14:77,Expanded?27:43);lv_obj_set_size(Panel,Expanded?132:70,Expanded?110:86);
        lv_obj_set_style_bg_color(Panel,lv_color_black(),0);lv_obj_set_style_bg_opa(Panel,LV_OPA_COVER,0);
        Title=Label(Expanded?0:1,0,Expanded?132:68,false);
        Status=Label(4,Expanded?17:19,Expanded?124:64,false);
        Note=Label(4,Expanded?83:43,Expanded?124:64,false);
        Footer=Label(4,Expanded?99:74,Expanded?124:64,false);
        if(Expanded) {
            lv_obj_set_style_border_width(Panel,1,0);lv_obj_set_style_border_color(Panel,lv_color_hex(Accent),0);
            lv_obj_set_size(Title,132,13);lv_obj_set_style_bg_color(Title,lv_color_hex(Accent),0);
            lv_obj_set_style_bg_opa(Title,LV_OPA_COVER,0);lv_obj_set_style_text_color(Title,lv_color_black(),0);
            lv_obj_set_style_pad_left(Title,4,0);lv_obj_set_style_pad_top(Title,3,0);
            for(unsigned I=0;I<4;I++){Rows[I]=Label(3,29+I*17,126,true);lv_obj_set_height(Rows[I],17);}
        }
    }
    lv_obj_move_foreground(Panel);
    char Text[40];
    if(!Expanded) {
        lv_label_set_text_static(Title,"3 SLOTS");
        snprintf(Text,sizeof(Text),"SLOT %u\n%s",S.Slot,S.Valid?"READY":"EMPTY");lv_label_set_text(Status,Text);
        lv_label_set_text_static(Note,"Kept on\nSD card");lv_label_set_text_static(Footer,"A: OPEN");return kOSD_Result_Ok;
    }
    bool InFlight=Busy(&S);
    for(unsigned I=0;I<4;I++)Show(Rows[I],false);
    lv_label_set_text_static(Footer,"A: CHOOSE   B: BACK");
    if(InFlight) {
        Slot=0;Choice=S.Slot?S.Slot-1:0;
        lv_label_set_text_static(Title,"SAVE STATES");
        lv_obj_set_pos(Status,6,30);lv_obj_set_width(Status,120);
        if(S.Phase==kStateUIWaiting)lv_label_set_text_static(Status,"Waiting for\ngame save...");
        else{snprintf(Text,sizeof(Text),"%s slot %u...",S.Phase==kStateUISaving?"Saving to":"Loading",S.Slot);lv_label_set_text(Status,Text);}
        Show(Note,true);lv_label_set_text_static(Note,"Keep the game open.");lv_label_set_text_static(Footer,"PLEASE WAIT");return kOSD_Result_Ok;
    }
    lv_obj_set_pos(Status,4,17);lv_obj_set_width(Status,124);
    bool Usable=S.Available && S.Supported;
    if(Slot && !Usable)CloseSlot();
    if(Slot) {
        bool Full=Saved(&S,Slot);
        snprintf(Text,sizeof(Text),"SLOT %u",Slot);lv_label_set_text(Title,Text);
        lv_label_set_text_static(Status,Full?"Holds a saved state":"Empty");
        if(!Full && Choice==kRowLoad)Choice=kRowSave;
        Row(kRowSave,"Save here",true,Choice==kRowSave);
        Row(kRowLoad,"Load",Full,Choice==kRowLoad);
        Row(kRowCancel,"Cancel",true,Choice==kRowCancel);
        Show(Note,Choice!=kRowCancel && (Full || Choice==kRowLoad));
        lv_label_set_text_static(Note,Choice==kRowLoad?"Newer game saves\nwill be kept separately.":"Replaces the state\nin this slot.");
        return kOSD_Result_Ok;
    }
    lv_label_set_text_static(Title,"SAVE STATES");Show(Note,false);
    const char *StatusText=S.Phase==kStateUISaved?"State saved":S.Phase==kStateUILoaded?"State restored":
        S.Phase==kStateUIError?S.Message:!S.Available?"Unavailable right now":!S.Supported?"No supported game":NULL;
    if(StatusText)lv_label_set_text(Status,StatusText);
    else if(Choice==3)lv_label_set_text_static(Status,"MENU+UP / hold DOWN");
    else{snprintf(Text,sizeof(Text),"Shortcuts use slot %u",S.Slot);lv_label_set_text(Status,S.Shortcuts?Text:"Kept on the SD card");}
    for(unsigned I=0;I<kNativeStateSlots;I++) {
        snprintf(Text,sizeof(Text),"Slot %u: %s",I+1,Saved(&S,I+1)?"Saved":"Empty");
        Row(I,Text,Usable,Choice==I);
    }
    Row(3,S.Shortcuts?"Shortcuts: ON":"Shortcuts: OFF",true,Choice==3);
    return kOSD_Result_Ok;
}
OSD_Result_t NativeStateUI_OnButton(Button_t B,ButtonState_t State,void *Arg) {
    (void)Arg;if(atomic_load(&OpenRequested))return kOSD_Result_Ok;
    if(State!=kButtonState_Pressed)return kOSD_Result_Ok;
    if(!Expanded){if(B==kButton_A)NativeStateUI_Open();return kOSD_Result_Ok;}
    NativeStateView S={0};if(Read)Read(&S);if(Busy(&S))return kOSD_Result_Ok;
    bool Usable=S.Available && S.Supported;
    if(B==kButton_B){if(Slot)CloseSlot();else{Expanded=false;Rebuild();}return kOSD_Result_Ok;}
    unsigned Count=Slot?3:4;
    if(B==kButton_Up)Choice=(Choice+Count-1)%Count;
    if(B==kButton_Down)Choice=(Choice+1)%Count;
    if(Slot && Choice==kRowLoad && !Saved(&S,Slot))Choice=B==kButton_Up?kRowSave:kRowCancel;
    if(B!=kButton_A)return kOSD_Result_Ok;
    if(Slot) {
        unsigned N=Slot;
        if(Choice==kRowSave && Usable && Request)Request(false,N);
        else if(Choice==kRowLoad && Usable && Saved(&S,N) && Request)Request(true,N);
        CloseSlot();
    }
    else if(Choice<kNativeStateSlots){if(Usable)OpenSlot(&S,Choice+1);}
    else if(Shortcuts)Shortcuts(!S.Shortcuts);
    return kOSD_Result_Ok;
}
OSD_Result_t NativeStateUI_OnTransition(void *Arg) { (void)Arg;Rebuild();Expanded=false;Slot=0;Choice=0;return kOSD_Result_Ok; }
