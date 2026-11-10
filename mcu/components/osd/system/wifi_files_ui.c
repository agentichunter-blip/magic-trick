#include "wifi_files_ui.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static WifiFilesToggle Toggle;
static WifiFilesGetStatus GetStatus;
static bool (*RequestScan)(void), (*Join)(const char *, const char *);
static void (*GetScan)(WifiScanStatus *);
static lv_obj_t *Panel, *Title, *Info, *Detail, *Footer, *Icon;
static lv_obj_t *Rows[4], *Keypad;
static enum { Home, Networks, Keyboard } View;
static WifiScanStatus Scan;
static unsigned Choice, Key, Bank;
static bool PendingScan, EditingSSID, Expanded;
LV_FONT_DECLARE(jf_dot_k14);
static char SSID[33], Input[65], Error[48];
/* Every printable ASCII character is available, including a space. */
static const char *Banks[]={"0123456789abcdefghijklmnopqrstuvwxyz",
                          "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                          " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"};
static void Rebuild(void) { if(Panel)lv_obj_del(Panel);Panel=NULL; }
bool WifiFilesUI_IsOpen(void) { return Expanded; }
bool WifiFilesUI_HandlesButton(Button_t B)
{
    if(!Panel)return false;
    if(!Expanded)return B==kButton_A || B==kButton_Select;
    return View!=Home || (B!=kButton_Left && B!=kButton_Right);
}
bool WifiFilesUI_IsConnected(void)
{ WifiFilesStatus S={0};if(GetStatus)GetStatus(&S);return S.Ready && S.Network.Connected; }


lv_obj_t *WifiFilesUI_CreateIcon(lv_obj_t *Parent,bool Dark)
{
    static const uint8_t Pixels[]={0x3f,0x00,0x40,0x80,0x9e,0x40,0x21,0x00,0x0c,0x00,0x12,0x00,0x00,0x00,0x0c,0x00};
    static const lv_img_dsc_t Image={.header={.cf=LV_IMG_CF_ALPHA_1BIT,.w=10,.h=8},.data_size=sizeof(Pixels),.data=Pixels};
    lv_obj_t *ImageObj=lv_img_create(Parent);lv_img_set_src(ImageObj,&Image);
    lv_obj_set_style_img_recolor(ImageObj,Dark?lv_color_black():lv_color_white(),0);
    lv_obj_set_style_img_recolor_opa(ImageObj,LV_OPA_COVER,0);
    return ImageObj;
}

/* Draw the keyboard as one object to fit the device's 32 KiB UI arena. */
static void DrawKeyboard(lv_event_t *Event)
{
    lv_draw_ctx_t *Context=lv_event_get_draw_ctx(Event);
    lv_area_t Bounds;lv_obj_get_coords(lv_event_get_target(Event),&Bounds);
    for(unsigned I=0;I<strlen(Banks[Bank]);I++)
    {
        lv_area_t Cell={Bounds.x1+2+(I%10)*12,Bounds.y1+1+(I/10)*15,
                        Bounds.x1+2+(I%10)*12+11,Bounds.y1+1+(I/10)*15+14};
        if(I==Key)
        {
            lv_draw_rect_dsc_t Rect;lv_draw_rect_dsc_init(&Rect);
            Rect.bg_color=lv_color_hex(0xff3399);Rect.bg_opa=LV_OPA_COVER;
            lv_draw_rect(Context,&Rect,&Cell);
        }
        lv_draw_label_dsc_t Text;lv_draw_label_dsc_init(&Text);
        Text.font=&jf_dot_k14;Text.align=LV_TEXT_ALIGN_CENTER;
        Text.color=lv_color_hex(I==Key?0x000000:0xffffff);
        char C[2]={Banks[Bank][I],0};
        if(C[0]==' ')Text.font=lv_obj_get_style_text_font(Footer,0);
        if(C[0]=='\\' || C[0]=='~')Text.font=&lv_font_montserrat_14;
        lv_draw_label(Context,&Text,&Cell,C[0]==' '?"SP":C,NULL);
    }
}

void WifiFilesUI_Register(WifiFilesToggle T, WifiFilesGetStatus G) { Toggle=T; GetStatus=G; }
void WifiFilesUI_RegisterNetwork(bool (*S)(void), void (*G)(WifiScanStatus *),
                                 bool (*J)(const char *,const char *))
{ RequestScan=S; GetScan=G; Join=J; }
bool WifiFilesUI_IsSelected(void) { return Panel!=NULL; }
bool WifiFilesUI_IsBusy(void)
{ WifiFilesStatus S={0}; if(GetStatus)GetStatus(&S); return S.Busy; }

static lv_obj_t *Label(int X,int Y,int W,bool Large)
{
    lv_obj_t *L=lv_label_create(Panel);
    lv_obj_add_style(L,OSD_GetStyleTextWhite(),0);
    if(Large)lv_obj_add_style(L,OSD_GetStyleTextWhite_L(),0);
    lv_obj_set_pos(L,X,Y);lv_obj_set_width(L,W);
    lv_label_set_long_mode(L,LV_LABEL_LONG_DOT);
    return L;
}
static void Highlight(lv_obj_t *L,bool Selected)
{
    lv_obj_set_style_bg_opa(L,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_color(L,lv_color_hex(0xff3399),0);
    lv_obj_set_style_border_width(L,Selected?1:0,0);
    lv_obj_set_style_pad_left(L,3,0);
    lv_obj_set_style_text_color(L,lv_color_hex(0xffffff),0);
}
static void Show(lv_obj_t *L,bool Visible)
{ if(Visible)lv_obj_clear_flag(L,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(L,LV_OBJ_FLAG_HIDDEN); }
static void BeginScan(void)
{
    if(!Expanded){Expanded=true;Rebuild();}
    View=Networks;Choice=0;PendingScan=true;Error[0]=0;memset(&Scan,0,sizeof(Scan));
    if(!WifiFilesUI_IsBusy() && Toggle)Toggle(true);
}
static void BeginKeyboard(bool Name)
{
    EditingSSID=Name;View=Keyboard;Input[0]=0;Error[0]=0;Bank=0;Key=10;
}
static void Submit(void)
{
    if(EditingSSID)
    {
        if(!Input[0]){snprintf(Error,sizeof(Error),"Enter a network name");return;}
        snprintf(SSID,sizeof(SSID),"%.32s",Input);BeginKeyboard(false);return;
    }
    if(!Join || !Join(SSID,Input))
    { snprintf(Error,sizeof(Error),"8+ characters");return; }
    memset(Input,0,sizeof(Input));View=Home;Choice=0;Error[0]=0;
}
OSD_Result_t WifiFilesUI_Draw(void *Arg)
{
    if(!Arg)return kOSD_Result_Err_NullDataPtr;
    WifiFilesStatus S={0};if(GetStatus)GetStatus(&S);
    if(!Panel)
    {
        Panel=lv_obj_create(Arg);lv_obj_remove_style_all(Panel);
        lv_obj_clear_flag(Panel,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(Panel,Expanded?14:77,Expanded?27:43);
        lv_obj_set_size(Panel,Expanded?132:70,Expanded?110:78);
        lv_obj_set_style_bg_color(Panel,lv_color_black(),0);
        lv_obj_set_style_bg_opa(Panel,LV_OPA_COVER,0);
        Title=Label(3,2,Expanded?110:67,false);
        Icon=WifiFilesUI_CreateIcon(Panel,true);lv_obj_set_pos(Icon,119,3);
        Info=Label(4,Expanded?18:18,Expanded?124:66,Expanded);
        Detail=Label(4,Expanded?37:31,Expanded?124:66,Expanded);
        Footer=Label(3,Expanded?99:70,Expanded?126:66,false);
        if(Expanded)
        {
            lv_obj_set_style_bg_color(Title,lv_color_hex(0xff3399),0);
            lv_obj_set_style_bg_opa(Title,LV_OPA_COVER,0);
            lv_obj_set_style_text_color(Title,lv_color_black(),0);
            lv_obj_set_style_pad_top(Title,3,0);lv_obj_set_style_pad_left(Title,3,0);
            lv_obj_set_pos(Title,0,0);lv_obj_set_size(Title,132,13);
            for(unsigned I=0;I<4;I++)Rows[I]=Label(3,18+I*19,126,true);
            Keypad=lv_obj_create(Panel);lv_obj_remove_style_all(Keypad);
            lv_obj_clear_flag(Keypad,LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(Keypad,4,34);lv_obj_set_size(Keypad,124,62);
            lv_obj_set_style_border_width(Keypad,1,0);
            lv_obj_set_style_border_color(Keypad,lv_color_hex(0xff3399),0);
            lv_obj_set_style_border_width(Panel,1,0);
            lv_obj_set_style_border_color(Panel,lv_color_hex(0xff3399),0);
            lv_obj_add_event_cb(Keypad,DrawKeyboard,LV_EVENT_DRAW_MAIN,NULL);
        }
    }
    if(!Expanded)
    {
        lv_obj_move_foreground(Panel);
        Show(Icon,false);
        lv_label_set_text(Title,S.Network.Connected?"CONNECTED":S.Busy?"CONNECTING":"WI-FI OFF");
        lv_label_set_text(Info,S.Network.Connected?S.Network.Address:"");
        if(S.Network.Connected)lv_label_set_text_fmt(Detail,"CODE %s",S.AccessCode);
        else lv_label_set_text(Detail,"A: OPEN WI-FI");
        lv_label_set_text(Footer,"A: OPEN / SETUP");
        return kOSD_Result_Ok;
    }
    /* The enclosing SYSTEM list raises its own labels each frame. */
    lv_obj_move_foreground(Panel);
    if(PendingScan && S.Ready && RequestScan)
    {
        PendingScan=false;
        if(!RequestScan())snprintf(Error,sizeof(Error),"Scan unavailable. Try again.");
    }
    if(View==Networks && GetScan && S.Ready)GetScan(&Scan);
    if(!S.Busy && View==Keyboard){View=Home;memset(Input,0,sizeof(Input));}
    Show(Icon,false);
    for(unsigned I=0;I<4;I++)Show(Rows[I],false);
    Show(Keypad,View==Keyboard);
    Show(Info,true);Show(Detail,true);
    if(View==Keyboard)
    {
        lv_label_set_text_fmt(Title,"%s  %s",EditingSSID?"NETWORK":"PASSWORD",Bank==0?"abc":Bank==1?"ABC":"#+=");
        size_t N=strlen(Input);char Display[16]={0};size_t Count=N>14?14:N;
        if(EditingSSID)memcpy(Display,Input+N-Count,Count);
        else { memset(Display,'*',Count);if(Count)Display[Count-1]=Input[N-1]; }
        lv_label_set_text(Info,Error[0]?Error:(N?Display:"A: add a letter"));
        Show(Detail,false);
        lv_obj_invalidate(Keypad);
        lv_label_set_text(Footer,EditingSSID?"SEL: CASE  B: DEL  START: NEXT":"SEL: CASE  B: DEL  START: JOIN");
    }
    else if(View==Networks)
    {
        lv_label_set_text(Title,"CHOOSE WI-FI");
        lv_label_set_text(Footer,"A: CHOOSE  B: BACK  SEL: SCAN");
        if(PendingScan || Scan.Busy || !S.Ready)
        { lv_label_set_text(Info,S.Busy?"Scanning...":"Wi-Fi is off");lv_label_set_text(Detail,S.Busy?"Looking for 2.4 GHz networks":S.Message); }
        else if(Error[0] || Scan.Error[0])
        { lv_label_set_text(Info,"Try scanning again");lv_label_set_text(Detail,Error[0]?Error:Scan.Error); }
        else
        {
            Show(Info,false);Show(Detail,false);
            unsigned Total=Scan.Count+1;if(Choice>=Total)Choice=Total-1;
            unsigned First=(Choice/4)*4;
            for(unsigned I=0;I<4 && First+I<Total;I++)
            {
                unsigned Index=First+I;Show(Rows[I],true);
                lv_label_set_text(Rows[I],Index==Scan.Count?"Other network...":Scan.Entries[Index].SSID);
                Highlight(Rows[I],Index==Choice);
                if(Index<Scan.Count && !Scan.Entries[Index].Supported)
                    lv_obj_set_style_text_color(Rows[I],lv_color_hex(0x788678),0);
            }
        }
    }
    else
    {
        lv_label_set_text(Title,S.Network.Connected && S.Ready?"WI-FI / CONNECTED":"WI-FI");
        if(S.Network.Connected && S.Ready)
        { lv_label_set_text(Info,S.Network.Address);lv_label_set_text_fmt(Detail,"Code %s",S.AccessCode); }
        else
        {
            lv_label_set_text(Info,S.Busy?(S.Ready?(S.Network.Configured?"Connecting...":"Choose a network"):"Starting Wi-Fi..."):"Wi-Fi is off");
            lv_label_set_text(Detail,S.Busy?(S.Network.Configured?S.Network.SSID:"Scan to get started"):S.Message);
        }
        Show(Rows[2],true);Show(Rows[3],true);
        lv_obj_set_y(Rows[2],60);lv_obj_set_y(Rows[3],79);
        lv_label_set_text(Rows[2],S.Busy?"Scan networks":"Turn Wi-Fi on");
        lv_label_set_text(Rows[3],S.Busy?"Turn Wi-Fi off":"Scan networks");
        Highlight(Rows[2],Choice==0);Highlight(Rows[3],Choice==1);
        lv_label_set_text(Footer,S.Busy?"A: CHOOSE  B: BACK  <>: TABS":"A: CHOOSE  B: BACK  <>: TABS");
    }
    /* Restore list positions after rendering Home. */
    if(View==Networks)for(unsigned I=0;I<4;I++)lv_obj_set_y(Rows[I],18+I*19);
    return kOSD_Result_Ok;
}
OSD_Result_t WifiFilesUI_OnButton(Button_t B,ButtonState_t State,void *Arg)
{
    (void)Arg;
    if(State!=kButtonState_Pressed || !Toggle || !GetStatus)return kOSD_Result_Ok;
    WifiFilesStatus S={0};GetStatus(&S);
    if(!Expanded)
    {
        if(B==kButton_A || B==kButton_Select){Expanded=true;View=Home;Choice=0;Rebuild();if(B==kButton_Select)BeginScan();}
        return kOSD_Result_Ok;
    }
    if(View==Keyboard)
    {
        unsigned Count=strlen(Banks[Bank]);size_t N=strlen(Input);Error[0]=0;
        if(B==kButton_Select){Bank=(Bank+1)%3;Key=Bank<2?10:0;}
        else if(B==kButton_Left)Key=(Key+Count-1)%Count;
        else if(B==kButton_Right)Key=(Key+1)%Count;
        else if(B==kButton_Up){if(Key>=10)Key-=10;else{unsigned K=Key+((Count-1)/10)*10;Key=K<Count?K:K-10;}}
        else if(B==kButton_Down)Key=Key+10<Count?Key+10:Key%10;
        else if(B==kButton_B){if(N)Input[N-1]=0;else{View=Networks;Choice=0;}}
        else if(B==kButton_Start)Submit();
        else if(B==kButton_A)
        {
            unsigned Limit=EditingSSID?32:64;
            if(N<Limit){Input[N]=Banks[Bank][Key];Input[N+1]=0;}
            else snprintf(Error,sizeof(Error),"Maximum length reached");
        }
    }
    else if(View==Networks)
    {
        if(B==kButton_B){View=Home;Choice=0;PendingScan=false;}
        else if(B==kButton_Select)BeginScan();
        else if(S.Ready && !PendingScan && !Scan.Busy)
        {
            unsigned Total=Scan.Count+1;
            if(B==kButton_Up)Choice=(Choice+Total-1)%Total;
            if(B==kButton_Down)Choice=(Choice+1)%Total;
            if(B==kButton_A)
            {
                if(Choice==Scan.Count)BeginKeyboard(true);
                else if(Scan.Entries[Choice].Supported)
                {
                    snprintf(SSID,sizeof(SSID),"%s",Scan.Entries[Choice].SSID);
                    if(Scan.Entries[Choice].Secure)BeginKeyboard(false);
                    else if(Join && Join(SSID,"")){View=Home;Choice=0;}
                }
            }
        }
    }
    else
    {
        if(B==kButton_Up || B==kButton_Down)Choice=1-Choice;
        if(B==kButton_Select)BeginScan();
        if(B==kButton_B){Expanded=false;Rebuild();return kOSD_Result_Ok;}
        if(B==kButton_A)
        {
            if(S.Busy){if(Choice==0)BeginScan();else Toggle(false);}
            else if(Choice==0)Toggle(true);else BeginScan();
        }
    }
    return kOSD_Result_Ok;
}
OSD_Result_t WifiFilesUI_OnTransition(void *Arg)
{
    (void)Arg;if(Panel)lv_obj_del(Panel);Panel=NULL;
    View=Home;Choice=Key=Bank=0;PendingScan=false;Expanded=false;
    memset(Input,0,sizeof(Input));memset(SSID,0,sizeof(SSID));Error[0]=0;
    return kOSD_Result_Ok;
}
