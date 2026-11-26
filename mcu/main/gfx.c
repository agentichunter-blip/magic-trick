#include "gfx.h"
#include "preview_format.h"
#include <stdlib.h>

#include "osd.h"
#include "button.h"

LV_FONT_DECLARE(chromatic_banner);

static lv_style_t Background;
static lv_obj_t *pDisplayObj;

#define BG_COLOR 0xFF00FF

typedef struct
{
    lv_obj_t *pScreen;
} TimerCtx_t;

static TimerCtx_t _Ctx;

/* Core-switch loading card: its own screen, so the menu's widgets are left
 * alone underneath. Shown before the LVGL task starts; hidden from that task. */
static lv_obj_t *pCard;
static uint8_t *pCardArt;      /* the card's box art pixels (freed with the card) */
static lv_img_dsc_t CardArt;
static volatile bool CardHideRequested;

static void anim_timer_cb(lv_timer_t *timer)
{
    TimerCtx_t *timer_ctx = (TimerCtx_t *) timer->user_data;
    lv_obj_t *pScreen = timer_ctx->pScreen;

    if (pCard)
    {
        /* No menu input or drawing behind the card: its buttons are not
         * meant for a page nobody can see. */
        if (!CardHideRequested) return;
        lv_scr_load(pScreen);
        lv_obj_del(pCard);
        pCard = NULL;
        free(pCardArt);
        pCardArt = NULL;
        CardHideRequested = false;
        lv_obj_invalidate(pScreen);
        Button_ResetAll();
    }
    OSD_HandleInputs();
    OSD_Draw(pScreen);
}

void Gfx_Start(lv_obj_t *const pScreen)
{
    // Initialize a tile that act as pages
    pDisplayObj = lv_tileview_create(pScreen);
    lv_obj_align(pDisplayObj, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Create style to manipulate objects characteristics implicitly
    lv_style_init(&Background);

    // Change background color
    lv_obj_add_style(pDisplayObj, &Background, 0);
    lv_style_set_bg_color(&Background, lv_color_hex(BG_COLOR));

    _Ctx.pScreen = pScreen;

    /* qspi_slave enables the physical menu button after two frame-start writes.
     * A hidden, static Backups page may otherwise send just one frame and wait
     * seconds for the battery widget to change. Prime the gate now, before the
     * LVGL task starts, without drawing/opening the library or adding a delay. */
    lv_disp_t *Display = lv_obj_get_disp(pScreen);
    lv_refr_now(Display);
    lv_obj_invalidate(pScreen);
    lv_refr_now(Display);

    // Create timer for animation
    lv_timer_create(anim_timer_cb, 20, &_Ctx);
}

void Gfx_ShowCard(const char *pTitle, uint8_t *pArt)
{
    if (pCard) { free(pArt); return; }
    CardHideRequested = false;
    pCard = lv_obj_create(NULL);
    lv_obj_remove_style_all(pCard);
    lv_obj_clear_flag(pCard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(pCard, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pCard, LV_OPA_COVER, 0);

    lv_obj_t *pHeading = lv_label_create(pCard);
    lv_obj_set_width(pHeading, 160);
    lv_obj_set_style_text_font(pHeading, &chromatic_banner, 0);
    lv_obj_set_style_text_color(pHeading, lv_color_white(), 0);
    lv_obj_set_style_text_align(pHeading, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(pHeading, "LOADING GAME");
    /* With box art (the menu's 128x92 cover, RGB565): the cover on top, the
     * words below it. The banner font is 10 px a letter (9 with the tighter
     * spacing), so the name gets the full width and three lines. */
    const lv_coord_t Top = pArt ? 100 : 56;
    lv_obj_set_pos(pHeading, 0, Top);
    if (pArt)
    {
        pCardArt = pArt;
        CardArt = (lv_img_dsc_t){.header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = PREVIEW_WIDTH, .h = PREVIEW_HEIGHT},
                                 .data_size = PREVIEW_BYTES, .data = pCardArt};
        lv_obj_t *pImage = lv_img_create(pCard);
        lv_img_set_src(pImage, &CardArt);
        lv_obj_set_pos(pImage, (160 - PREVIEW_WIDTH) / 2, 4);
    }

    lv_obj_t *pName = lv_label_create(pCard);
    /* Up to three lines (17 letters each), then "...". */
    lv_obj_set_size(pName, 156, 30);
    lv_obj_set_style_text_line_space(pName, 3, 0);
    lv_obj_set_style_text_letter_space(pName, -1, 0);
    lv_obj_set_style_text_font(pName, &chromatic_banner, 0);
    lv_obj_set_style_text_color(pName, lv_color_hex(0xffcd48), 0);
    lv_obj_set_style_text_align(pName, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(pName, LV_LABEL_LONG_DOT);
    lv_label_set_text(pName, pTitle ? pTitle : "");
    lv_obj_set_pos(pName, 2, Top + 13);

    lv_scr_load(pCard);
    lv_refr_now(lv_obj_get_disp(pCard));
}

void Gfx_HideCard(void)
{
    CardHideRequested = true;
}
