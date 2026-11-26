#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "preview_format.h"
#include "gfx.h"
#include "osd.h"

static lv_color_t Pixels[160*144];
static unsigned FrameStarts, InputPolls, Draws;
void OSD_HandleInputs(void) { ++InputPolls; }
void OSD_Draw(void *Arg) { (void)Arg; ++Draws; }
static unsigned ButtonResets;
void Button_ResetAll(void) { ++ButtonResets; }
static void Dump(const char *Name)
{
    const char *Dir=getenv("CHROMATIC_TEST_RENDER_DIR");
    if(!Dir)return;
    char Path[512]; snprintf(Path,sizeof(Path),"%s/%s.ppm",Dir,Name);
    FILE *File=fopen(Path,"wb"); assert(File);
    fprintf(File,"P6\n160 144\n255\n");
    for(size_t I=0;I<160*144;++I){
        lv_color32_t C={.full=lv_color_to32(Pixels[I])};
        const unsigned char RGB[]={C.ch.red,C.ch.green,C.ch.blue}; fwrite(RGB,1,3,File);
    }
    fclose(File);
}
/* The card's cover: a 128x92 CMAR v2 file named by CHROMATIC_TEST_CARD_ART, or a test pattern. */
static uint8_t *CardArt(void)
{
    uint8_t *Art=malloc(PREVIEW_BYTES); assert(Art);
    const char *Path=getenv("CHROMATIC_TEST_CARD_ART");
    FILE *File=Path ? fopen(Path,"rb") : NULL;
    if(File && fseek(File,16,SEEK_SET)==0 && fread(Art,1,PREVIEW_BYTES,File)==PREVIEW_BYTES) { fclose(File); return Art; }
    if(File) fclose(File);
    for(unsigned I=0;I<PREVIEW_BYTES/2;++I){ const uint16_t V=(uint16_t)(((I%128)>>2)<<11 | ((I/128)%64)<<5 | 8); Art[2*I]=(uint8_t)V; Art[2*I+1]=(uint8_t)(V>>8); }
    return Art;
}
static void Flush(lv_disp_drv_t *Driver, const lv_area_t *Area, lv_color_t *Data)
{
    (void)Data;
    /* The FPGA's qMenuInit gate requires two writes starting at address zero. */
    assert(Area->x1==0 && Area->y1==0 && Area->x2==159 && Area->y2==143);
    ++FrameStarts;
    lv_disp_flush_ready(Driver);
}
int main(void)
{
    lv_init();
    static lv_disp_draw_buf_t Buffer;
    lv_disp_draw_buf_init(&Buffer,Pixels,NULL,160*144);
    static lv_disp_drv_t Driver;
    lv_disp_drv_init(&Driver);
    Driver.hor_res=160; Driver.ver_res=144; Driver.direct_mode=1; Driver.full_refresh=1;
    Driver.draw_buf=&Buffer; Driver.flush_cb=Flush;
    lv_disp_drv_register(&Driver);
    Gfx_Start(lv_scr_act());
    lv_refr_now(NULL); /* drain the initial dirty frame without later widget changes */
    /* No battery update, menu input, timer tick, or library read should be needed. */
    printf("Startup frame writes: %u\n",FrameStarts); fflush(stdout);
    assert(FrameStarts>=2 && !InputPolls && !Draws && lv_tick_get()==0);
    puts("PASS: menu frame gate primed before startup timers or user input");

    /* Core-switch loading card: its own screen, no menu input or drawing
     * while it is up, and the menu screen back after Gfx_HideCard. */
    lv_obj_t *Menu=lv_scr_act();
    const unsigned Frames=FrameStarts;
    Gfx_ShowCard("SHINOBI", NULL);
    lv_obj_t *Card=lv_scr_act();
    assert(Card!=Menu && FrameStarts>Frames);
    lv_tick_inc(25); lv_timer_handler();
    assert(lv_scr_act()==Card && !InputPolls && !Draws);
    Gfx_HideCard();
    lv_tick_inc(25); lv_timer_handler();
    assert(lv_scr_act()==Menu && ButtonResets==1 && InputPolls==1 && Draws==1);
    puts("PASS: core-switch loading card");

    /* With the menu's 128x92 cover, and a long name that needs all three lines. */
    Gfx_ShowCard("TEENAGE MUTANT NINJA TURTLES II THE ARCADE GAME", CardArt());
    lv_refr_now(NULL); Dump("loading-card-art");
    Gfx_HideCard(); lv_tick_inc(25); lv_timer_handler();
    assert(lv_scr_act()==Menu);
    Gfx_ShowCard("SUPER MARIO BROS. 2", NULL);
    lv_refr_now(NULL); Dump("loading-card-no-art");
    Gfx_HideCard(); lv_tick_inc(25); lv_timer_handler();
    assert(lv_scr_act()==Menu);
    puts("PASS: loading card cover and three-line name");
}
