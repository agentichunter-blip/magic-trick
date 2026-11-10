#include "native_state_ui.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "menu_mgr.h"
#include "tab_glyphs.h"
LV_FONT_DECLARE(chromatic_banner);
#include "wifi_files_ui.h"

#include "osd_shared.h"
#include "lvgl.h"
#include "esp_log.h"
#include "button.h"
#include "osd.h"
#include "battery.h"
#include "rom_browser_ui.h"

#include <stddef.h>
#include <stdint.h>

LV_IMG_DECLARE(menu_status);
LV_IMG_DECLARE(menu_backups_accent);

static const uint8_t BackupGlyphData[] = {
    0x7c, 0x82, 0xb9, 0x81, 0xbd, 0xa5, 0xbd, 0x81, 0xbd, 0x7e,
};
static const lv_img_dsc_t BackupGlyph = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.w = 8,
    .header.h = 10,
    .data_size = sizeof(BackupGlyphData),
    .data = BackupGlyphData,
};

typedef struct MenuMgrCtx
{
    TabID_t eCurTab;
    MenuTab_t* pMenus[kNumTabIDs];
    lv_obj_t *pBackupTab;
    lv_obj_t *pBackupFrameAccent;
    lv_obj_t *pInactiveSystem;
    lv_obj_t *pBackupTitle;
    lv_obj_t *pWifiStatus;
} MenuMgrCtx_t;

const lv_point_t _MenuOrigin_px = {
    .x = 8,
    .y = 7,
};

static MenuMgrCtx_t _Ctx;
static const char* TAG = "MenuMgr";
static OSD_Result_t MenuMgr_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
static OSD_Result_t MenuMgr_Draw(void* arg);
static OSD_Result_t MenuMgr_OnTransition(void *arg);
static void MenuMgr_NextTab(void);
static void MenuMgr_PrevTab(void);

static lv_obj_t *Box(lv_obj_t *pScreen, int X, int Y, int Width, int Height)
{
    lv_obj_t *pBox = lv_obj_create(pScreen);
    lv_obj_remove_style_all(pBox);
    lv_obj_set_pos(pBox, X, Y);
    lv_obj_set_size(pBox, Width, Height);
    lv_obj_clear_flag(pBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pBox, LV_SCROLLBAR_MODE_OFF);
    return pBox;
}

/* Preserve the stock header/corners; repeat only a straight frame row. */
static lv_obj_t *BackupFrame(lv_obj_t *Screen, const lv_img_dsc_t *Base, lv_color_t Accent)
{
    enum { Width = 143, Top = 80, Extra = 27, Bottom = 26 };
    static lv_img_dsc_t Slices[2][3];
    const lv_img_dsc_t *Images[2] = {Base, &menu_backups_accent};
    lv_obj_t *Frame = Box(Screen, 8, 7, Width, Top + Extra + Bottom);
    for (unsigned Layer = 0; Layer < 2; ++Layer)
    {
        const unsigned Stride = Layer ? (Width + 7) / 8 : Width * sizeof(lv_color_t);
        for (unsigned Part = 0; Part < 3; ++Part)
        {
            const unsigned SourceY = Part == 0 ? 0 : Part == 1 ? 90 : Top;
            const unsigned Height = Part == 0 ? Top : Part == 1 ? 2 : Bottom;
            lv_img_dsc_t *Slice = &Slices[Layer][Part];
            *Slice = *Images[Layer];
            Slice->header.h = Height;
            Slice->data_size = Height * Stride;
            Slice->data += SourceY * Stride;
            lv_obj_t *Image = lv_img_create(Frame);
            lv_img_set_src(Image, Slice);
            lv_obj_set_pos(Image, 0, Part == 0 ? 0 : Part == 1 ? Top : Top + Extra);
            /* LVGL 8 tile loops exclude the final origin; overlap the bottom by one row. */
            if (Part == 1) lv_obj_set_size(Image, Width, Extra + 1);
            if (Layer)
            {
                lv_obj_set_style_img_recolor(Image, Accent, 0);
                lv_obj_set_style_img_recolor_opa(Image, LV_OPA_COVER, 0);
            }
        }
    }
    return Frame;
}

static void PaintTabs(lv_event_t *Event)
{
    lv_draw_ctx_t *Context=lv_event_get_draw_ctx(Event);
    lv_area_t Bounds;lv_obj_get_coords(lv_event_get_target(Event),&Bounds);
    const lv_color_t Accent=_Ctx.pMenus[_Ctx.eCurTab]->Accent;
    const bool PickerFocus=_Ctx.eCurTab==kTabID_Backups && RomBrowserUI_HasPickerFocus();
    for(unsigned I=0;I<kNumTabIDs;I++)
    {
        const bool Selected=I==(unsigned)_Ctx.eCurTab;
        const int X=Bounds.x1+I*18;
        lv_area_t Cell={X+1,Bounds.y1+1,X+16,Bounds.y1+14};
        lv_draw_rect_dsc_t Rect;lv_draw_rect_dsc_init(&Rect);
        Rect.bg_color=lv_color_black();Rect.bg_opa=LV_OPA_COVER;
        Rect.border_width=1;Rect.radius=3;
        Rect.border_color=Selected && !PickerFocus?Accent:lv_color_hex(0x666666);
        lv_draw_rect(Context,&Rect,&Cell);
        const lv_img_dsc_t *Glyph=I==kTabID_Backups?&BackupGlyph:&NativeTabGlyphs[I-1];
        lv_area_t Image={X+5,Bounds.y1+(I==kTabID_Backups?3:4),X+5+Glyph->header.w-1,Bounds.y1+(I==kTabID_Backups?3:4)+Glyph->header.h-1};
        lv_draw_img_dsc_t Ink;lv_draw_img_dsc_init(&Ink);
        Ink.recolor=lv_color_hex(Selected?0xffffff:0x999999);Ink.recolor_opa=LV_OPA_COVER;
        lv_draw_img(Context,&Ink,&Image,Glyph);
    }
    /* This line belongs to the page frame, never to an inactive tab. */
    lv_area_t Line={Bounds.x1,Bounds.y1+15,Bounds.x2,Bounds.y1+15};
    lv_draw_rect_dsc_t Ink;lv_draw_rect_dsc_init(&Ink);
    Ink.bg_color=Accent;Ink.bg_opa=LV_OPA_COVER;lv_draw_rect(Context,&Ink,&Line);
}
static void DrawBackupTab(lv_obj_t *Screen)
{
    if(!_Ctx.pBackupTab)
    {
        _Ctx.pBackupTab=Box(Screen,16,7,108,16);
        lv_obj_set_style_bg_color(_Ctx.pBackupTab,lv_color_hex(0xff00ff),0);
        lv_obj_set_style_bg_opa(_Ctx.pBackupTab,LV_OPA_COVER,0);
        lv_obj_add_event_cb(_Ctx.pBackupTab,PaintTabs,LV_EVENT_DRAW_MAIN,NULL);
    }
    lv_obj_invalidate(_Ctx.pBackupTab);
    if(_Ctx.eCurTab==kTabID_Backups && !_Ctx.pBackupTitle)
    {
        _Ctx.pBackupTitle=Box(Screen,20,29,76,8);
        lv_obj_set_style_bg_color(_Ctx.pBackupTitle,_Ctx.pMenus[kTabID_Backups]->Accent,0);
        lv_obj_set_style_bg_opa(_Ctx.pBackupTitle,LV_OPA_COVER,0);
        lv_obj_t *Title=lv_label_create(_Ctx.pBackupTitle);
        lv_obj_set_style_text_font(Title,&chromatic_banner,0);
        lv_obj_set_style_text_color(Title,lv_color_black(),0);
        lv_label_set_text_static(Title,"GAMES");
    }
}

OSD_Result_t MenuMgr_Initialize(OSD_Widget_t* const pWidget, lv_obj_t *const pScreen)
{
    if (pScreen == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if (sys_dnode_is_linked(&pWidget->Node))
    {
        return kOSD_Result_Err_MenuAlreadyInit;
    }

    pWidget->fnDraw = MenuMgr_Draw;
    pWidget->fnOnButton = MenuMgr_OnButton;
    pWidget->fnOnTransition = MenuMgr_OnTransition;

    _Ctx.eCurTab = kTabID_Backups;

    return kOSD_Result_Ok;
}

OSD_Result_t MenuMgr_AddTab(TabID_t eID, MenuTab_t *const pTab)
{
    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    if ((pTab == NULL) || (pTab->pImageDesc == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    if ((_Ctx.pMenus[eID] != NULL) || (pTab->pImgObj != NULL))
    {
        return kOSD_Result_Err_MenuTabAlreadyInit;
    }

    _Ctx.pMenus[eID] = pTab;

    return kOSD_Result_Ok;
}

static OSD_Result_t MenuMgr_Draw(void* arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t *const pScreen = (lv_obj_t*)arg;

    if(NativeStateUI_TakeOpenRequest() && _Ctx.pMenus[kTabID_System]) {
        MenuMgr_OnTransition(NULL);_Ctx.eCurTab=kTabID_System;
        TabCollection_t *List=_Ctx.pMenus[kTabID_System]->Menu;
        if(List)List->pCurrent=(TabItem_t *)sys_dlist_peek_head(&List->WidgetList);
        NativeStateUI_Open();
    }

    const TabID_t eID = _Ctx.eCurTab;

    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    MenuTab_t *const pTab = _Ctx.pMenus[eID];
    if ((pTab == NULL) || (pTab->pImageDesc == NULL))
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    const bool TallFrame=eID==kTabID_Backups
#ifdef CONFIG_CHROMAGIC_WIFI_FILES
        || eID==kTabID_System
#endif
        ;
    Battery_SetCompact(TallFrame && OSD_IsVisible());
    if (eID == kTabID_Backups && !OSD_IsVisible())
    {
        if (pTab->pImgObj != NULL) MenuMgr_OnTransition(NULL);
        return kOSD_Result_Ok;
    }

    if (pTab->pImgObj == NULL)
    {
        if (TallFrame)
            pTab->pImgObj = BackupFrame(pScreen, pTab->pImageDesc, pTab->Accent);
        else
        {
            pTab->pImgObj = lv_img_create(pScreen);
            lv_obj_align(pTab->pImgObj , LV_ALIGN_TOP_LEFT, _MenuOrigin_px.x, _MenuOrigin_px.y);
        }
    }

    if (!TallFrame) lv_img_set_src(pTab->pImgObj, pTab->pImageDesc);
    DrawBackupTab(pScreen);

    if (pTab->Widget.fnDraw != NULL)
    {
        Tab_DrawCtx_t Ctx = { pTab->Menu, pScreen, pTab->Accent };
        pTab->Widget.fnDraw(&Ctx);
    }

#ifdef CONFIG_CHROMAGIC_WIFI_FILES
    if(WifiFilesUI_IsConnected())
    {
        if(!_Ctx.pWifiStatus)
        {
            _Ctx.pWifiStatus=WifiFilesUI_CreateIcon(pScreen,false);
            lv_obj_set_pos(_Ctx.pWifiStatus,148,1);
        }
        lv_obj_clear_flag(_Ctx.pWifiStatus,LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_Ctx.pWifiStatus);
    }
    else if(_Ctx.pWifiStatus)lv_obj_add_flag(_Ctx.pWifiStatus,LV_OBJ_FLAG_HIDDEN);
#endif
    return kOSD_Result_Ok;
}

static OSD_Result_t MenuMgr_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;

    /* Raw button polling continues during gameplay for optional shortcuts. */
    if (!OSD_IsVisible()) return kOSD_Result_Ok;

    if(NativeStateUI_HandlesButton(Button))
        return NativeStateUI_OnButton(Button,State,NULL);

#ifdef CONFIG_CHROMAGIC_WIFI_FILES
    /* Only the open editor captures controls. Wi-Fi can keep serving files
     * while the user navigates settings; SD-backed screens guard their lease. */
    if (WifiFilesUI_HandlesButton(Button))
        return WifiFilesUI_OnButton(Button, State, NULL);
#endif

    const TabID_t eID = _Ctx.eCurTab;

    if ((unsigned)eID >= kNumTabIDs)
    {
        return kOSD_Result_Err_InvalidTabID;
    }

    MenuTab_t *const pTab = _Ctx.pMenus[eID];
    if (pTab == NULL)
    {
        ESP_LOGE(TAG, "no tab in %s", __func__ );
        return kOSD_Result_Err_NullDataPtr;
    }

    switch (Button)
    {
        // Fall-through
        case kButton_Select:
            if (eID != kTabID_Backups) break;
            /* fall through: Select opens the selected game's cheat options. */
        case kButton_B:
        case kButton_A:
        case kButton_Down:
        case kButton_Up:
            if (pTab->Widget.fnOnButton != NULL)
            {
                const OSD_Result_t eResult = pTab->Widget.fnOnButton(Button, State, pTab->Menu);

                if (eResult != kOSD_Result_Ok)
                {
                    ESP_LOGE(TAG, "%s OnButton call failed with %d", pTab->Widget.Name, eResult);
                }
            }
            break;
        case kButton_Left:
            if (eID == kTabID_Backups && RomBrowserUI_HandleHorizontal(Button, State)) break;
            if (State == kButtonState_Pressed)
            {
                MenuMgr_PrevTab();
            }
            break;
        case kButton_Right:
            if (eID == kTabID_Backups && RomBrowserUI_HandleHorizontal(Button, State)) break;
            if (State == kButtonState_Pressed)
            {
                MenuMgr_NextTab();
            }
            break;
        default:
            break;
    }

    return kOSD_Result_Ok;
}

static void MenuMgr_NextTab(void)
{
    TabID_t eNextID = _Ctx.eCurTab + 1;
    if (eNextID >= kNumTabIDs)
    {
        eNextID = kTabID_First;
    }

    // Clean up the old tab data
    MenuMgr_OnTransition(NULL);

    _Ctx.eCurTab = eNextID;
}

static void MenuMgr_PrevTab(void)
{
    TabID_t ePrevID = _Ctx.eCurTab - 1;
    if ((signed) ePrevID < 0)
    {
        ePrevID = kTabID_Last;
    }

    // Clean up the old tab data
    MenuMgr_OnTransition(NULL);

    _Ctx.eCurTab = ePrevID;
}

static OSD_Result_t MenuMgr_OnTransition(void *arg)
{
    (void) arg;
    OSD_Result_t eResult = kOSD_Result_Ok;

    MenuTab_t *const pTab = _Ctx.pMenus[_Ctx.eCurTab];

    if (pTab->Widget.fnOnTransition != NULL)
    {
        eResult = pTab->Widget.fnOnTransition(pTab->Menu);
        if (eResult != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "%s OnTransition failed with %d", pTab->Widget.Name, eResult);
        }
    }

    if (pTab->pImgObj != NULL)
    {
        lv_obj_del(pTab->pImgObj);
        pTab->pImgObj = NULL;
    }

    lv_obj_t **Extra[] = {&_Ctx.pBackupTab, &_Ctx.pBackupFrameAccent,
                         &_Ctx.pInactiveSystem, &_Ctx.pBackupTitle, &_Ctx.pWifiStatus};
    for (size_t Index = 0; Index < ARRAY_SIZE(Extra); ++Index)
    {
        if (*Extra[Index] != NULL) lv_obj_del(*Extra[Index]);
        *Extra[Index] = NULL;
    }

    return eResult;
}
