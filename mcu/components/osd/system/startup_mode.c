#include "startup_mode.h"

#include "esp_log.h"
#include "lvgl.h"

LV_IMG_DECLARE(img_option_dis);
LV_IMG_DECLARE(img_option_en);

/* Same two-option toggle as LYNX SCREEN and the other settings. */
enum {
    kToggleOptX_px = 77,
    kToggleOptTop_Y_px = 52,
    kToggleOptBtm_Y_px = 76,
};

static const char *TAG = "StartUp";
static const char *const kNames[kNumStartUpModes] = {"LAST GAME", "GAMES MENU"};

static struct {
    lv_obj_t *pImgTop, *pImgBtm, *pTextTop, *pTextBtm;
    StartUpMode_t eMode;
} _Ctx;

static lv_obj_t *Option(lv_obj_t *pScreen, bool Top, bool Enabled)
{
    lv_obj_t *pImg = lv_img_create(pScreen);
    lv_obj_align(pImg, LV_ALIGN_TOP_LEFT, kToggleOptX_px, Top ? kToggleOptTop_Y_px : kToggleOptBtm_Y_px);
    lv_img_set_src(pImg, Enabled ? &img_option_en : &img_option_dis);
    return pImg;
}

static lv_obj_t *Text(lv_obj_t *pParent, const char *pText)
{
    lv_obj_t *pLabel = lv_label_create(pParent);
    lv_obj_align(pLabel, LV_ALIGN_CENTER, 1, 0);
    lv_label_set_long_mode(pLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(pLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pLabel, img_option_en.header.w * 0.80);
    lv_obj_add_style(pLabel, OSD_GetStyleTextBlack(), 0);
    lv_label_set_text_static(pLabel, pText);
    return pLabel;
}

OSD_Result_t StartUp_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *)arg;
    /* LAST GAME on top, GAMES MENU below; the chosen one is highlighted. */
    const bool Last = _Ctx.eMode == kStartUpMode_LastGame;
    if (_Ctx.pImgTop == NULL) _Ctx.pImgTop = Option(pScreen, true, Last);
    if (_Ctx.pImgBtm == NULL) _Ctx.pImgBtm = Option(pScreen, false, !Last);
    if (_Ctx.pTextTop == NULL) _Ctx.pTextTop = Text(_Ctx.pImgTop, kNames[kStartUpMode_LastGame]);
    if (_Ctx.pTextBtm == NULL) _Ctx.pTextBtm = Text(_Ctx.pImgBtm, kNames[kStartUpMode_GamesMenu]);
    return kOSD_Result_Ok;
}

OSD_Result_t StartUp_OnTransition(void *arg)
{
    (void)arg;
    lv_obj_t **ToDelete[] = {&_Ctx.pTextTop, &_Ctx.pTextBtm, &_Ctx.pImgTop, &_Ctx.pImgBtm};
    for (size_t i = 0; i < ARRAY_SIZE(ToDelete); i++)
    {
        if (*ToDelete[i] != NULL) { lv_obj_del(*ToDelete[i]); *ToDelete[i] = NULL; }
    }
    return kOSD_Result_Ok;
}

OSD_Result_t StartUp_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed || (Button != kButton_A && Button != kButton_B)) return kOSD_Result_Ok;
    /* Two options: A and B both switch to the other one. */
    StartUp_OnTransition(NULL);
    _Ctx.eMode = (_Ctx.eMode + 1) % kNumStartUpModes;
    OSD_Result_t eResult = Settings_Update(kSettingKey_StartUp, (uint8_t)_Ctx.eMode);
    if (eResult != kOSD_Result_Ok) ESP_LOGE(TAG, "start-up save failed: %d", eResult);
    return kOSD_Result_Ok;
}

StartUpMode_t StartUp_GetMode(void)
{
    return _Ctx.eMode;
}

OSD_Result_t StartUp_ApplySetting(SettingValue_t const *const pValue)
{
    if (pValue == NULL) return kOSD_Result_Err_NullDataPtr;
    if (pValue->eType != kSettingDataType_U8) return kOSD_Result_Err_UnexpectedSettingDataType;
    _Ctx.eMode = pValue->U8 == kStartUpMode_GamesMenu ? kStartUpMode_GamesMenu : kStartUpMode_LastGame;
    return kOSD_Result_Ok;
}
