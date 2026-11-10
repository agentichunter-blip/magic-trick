#include "native_state_ui.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
static NativeStateRead Read;
static NativeStateRequest Request;
static NativeStateShortcuts Shortcuts;
static lv_obj_t *Panel, *Title, *Status, *Rows[3], *Note, *Footer;
static bool Expanded, Confirm;
static atomic_bool OpenRequested;
void NativeStateUI_RequestOpen(void) { atomic_store(&OpenRequested,true); }
bool NativeStateUI_TakeOpenRequest(void) { return atomic_exchange(&OpenRequested,false); }
static unsigned Choice;
static const uint32_t Accent=0xff3399;
static bool Busy(const NativeStateView *S) { return S->Phase>=kStateUIWaiting && S->Phase<=kStateUILoading; }
static void Rebuild(void) { if(Panel)lv_obj_del(Panel);Panel=NULL; }
void NativeStateUI_Register(NativeStateRead R,NativeStateRequest Q,NativeStateShortcuts H) { Read=R;Request=Q;Shortcuts=H; }
bool NativeStateUI_IsOpen(void) { return Expanded; }
void NativeStateUI_Open(void) { Expanded=true;Confirm=false;Choice=0;Rebuild(); }
bool NativeStateUI_HandlesButton(Button_t B) { return atomic_load(&OpenRequested) || (Panel && (Expanded || B==kButton_A)); }
static lv_obj_t *Label(int X,int Y,int W,bool Large) {
    lv_obj_t *L=lv_label_create(Panel);lv_obj_add_style(L,Large?OSD_GetStyleTextWhite_L():OSD_GetStyleTextWhite(),0);
    lv_obj_set_pos(L,X,Y);lv_obj_set_width(L,W);lv_label_set_long_mode(L,LV_LABEL_LONG_WRAP);return L;
}
static void Show(lv_obj_t *L,bool V) { if(V)lv_obj_clear_flag(L,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(L,LV_OBJ_FLAG_HIDDEN); }
static void Row(unsigned I,const char *Text,bool Enabled,bool Selected) {
    lv_label_set_text(Rows[I],Text);lv_obj_set_style_text_color(Rows[I],lv_color_hex(Enabled?0xffffff:0x777777),0);
    lv_obj_set_style_border_color(Rows[I],lv_color_hex(Accent),0);lv_obj_set_style_border_width(Rows[I],Selected?1:0,0);
    lv_obj_set_style_pad_left(Rows[I],3,0);lv_obj_set_style_pad_top(Rows[I],1,0);
}
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
            for(unsigned I=0;I<3;I++){Rows[I]=Label(3,29+I*17,126,true);lv_obj_set_height(Rows[I],17);}
        }
    }
    lv_obj_move_foreground(Panel);
    if(!Expanded) {
        lv_label_set_text_static(Title,"ONE SLOT");lv_label_set_text(Status,S.Valid?"STATE READY":"NO STATE YET");
        lv_label_set_text_static(Note,"Temporary\nPower off\nclears state");lv_label_set_text_static(Footer,"A: OPEN");return kOSD_Result_Ok;
    }
    bool InFlight=Busy(&S);
    for(unsigned I=0;I<3;I++)Show(Rows[I],!InFlight);
    lv_label_set_text_static(Title,Confirm?"LOAD STATE?":"SAVE STATES");
    lv_label_set_text_static(Note,"Lost on power off,\nreset or game change.");
    lv_label_set_text_static(Footer,"A: CHOOSE   B: BACK");
    if(InFlight) {
        Confirm=false;lv_obj_set_pos(Status,6,30);lv_obj_set_width(Status,120);
        lv_label_set_text(Status,S.Phase==kStateUIWaiting?"Waiting for\ngame save...":S.Phase==kStateUISaving?"Saving state...":"Restoring state...");
        lv_label_set_text_static(Note,"Keep the game open.");lv_label_set_text_static(Footer,"PLEASE WAIT");return kOSD_Result_Ok;
    }
    lv_obj_set_pos(Status,4,17);
    if(Confirm && (!S.Valid || !S.Available || !S.Supported)) { Confirm=false;Choice=1; }
    if(Confirm) {
        lv_label_set_text_static(Status,"Return to saved progress?");
        Row(0,"Cancel",true,Choice==0);Row(1,"Load State",true,Choice==1);Show(Rows[2],false);
        lv_label_set_text_static(Note,"Newer game saves\nwill be kept separately.");return kOSD_Result_Ok;
    }
    const char *StatusText=S.Phase==kStateUISaved?"State saved":S.Phase==kStateUILoaded?"State restored":
        S.Phase==kStateUIError?S.Message:!S.Available?"Unavailable right now":!S.Supported?"No supported game":S.Valid?"Temporary state ready":"No state saved yet";
    lv_label_set_text(Status,StatusText);
    Row(0,"Save State",S.Available && S.Supported,Choice==0);
    Row(1,"Load State",S.Available && S.Supported && S.Valid,Choice==1);
    Row(2,S.Shortcuts?"Shortcuts: ON":"Shortcuts: OFF",true,Choice==2);
    if(Choice==2)lv_label_set_text_static(Note,"MENU+UP: save\nMENU+DOWN: hold 1 sec");
    return kOSD_Result_Ok;
}
OSD_Result_t NativeStateUI_OnButton(Button_t B,ButtonState_t State,void *Arg) {
    (void)Arg;if(atomic_load(&OpenRequested))return kOSD_Result_Ok;
    if(State!=kButtonState_Pressed)return kOSD_Result_Ok;
    if(!Expanded){if(B==kButton_A)NativeStateUI_Open();return kOSD_Result_Ok;}
    NativeStateView S={0};if(Read)Read(&S);if(Busy(&S))return kOSD_Result_Ok;
    if(B==kButton_B){if(Confirm){Confirm=false;Choice=1;}else{Expanded=false;Rebuild();}return kOSD_Result_Ok;}
    unsigned Count=Confirm?2:3;
    if(B==kButton_Up)Choice=(Choice+Count-1)%Count;
    if(B==kButton_Down)Choice=(Choice+1)%Count;
    if(B!=kButton_A)return kOSD_Result_Ok;
    if(Confirm){if(Choice==1 && S.Valid && S.Supported && S.Available && Request)Request(true);Confirm=false;Choice=1;}
    else if(Choice==0 && S.Available && S.Supported && Request)Request(false);
    else if(Choice==1 && S.Available && S.Supported && S.Valid){Confirm=true;Choice=0;}
    else if(Choice==2 && Shortcuts)Shortcuts(!S.Shortcuts);
    return kOSD_Result_Ok;
}
OSD_Result_t NativeStateUI_OnTransition(void *Arg) { (void)Arg;Rebuild();Expanded=Confirm=false;Choice=0;return kOSD_Result_Ok; }
