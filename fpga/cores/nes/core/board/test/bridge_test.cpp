// nes_video_bridge: frames of 144 rows x 160 pixels; the picture matches a
// reference 5/8 box filter of NES lines 8-231; held frames are all repeat.
#include "Vnes_video_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static void check(bool c,const char*s){if(!c){printf("FAIL %s\n",s);exit(1);}}
static unsigned colour(int x,int y,int f){int r=(x*7+y*3+f)&31,g=((x^y)*5)&31,b=(y*11+x/3)&31;return (b<<10)|(g<<5)|r;}
int main(){
 Vnes_video_bridge d; d.xclk=d.hclk=0;d.xreset=d.hreset=1;d.enable=1;d.hold=0;d.qhold=0;d.pixel=0;d.x=d.y=0;d.rgb=0;
 auto tick=[&]{d.xclk=d.hclk=0;d.eval();d.xclk=d.hclk=1;d.eval();};
 for(int i=0;i<10;i++)tick(); d.xreset=d.hreset=0;
 std::vector<unsigned> panel(160*144,0); int row=-1,col=0,rows=0,last=0,frames=0,good=0; bool repeat_all=true; int repeat_pixels=0;
 auto run_frame=[&](int f,bool hold){
  d.hold=hold;
  for(int y=0;y<262;y++)for(int x=0;x<341;x++){
   for(int k=0;k<12+((x+y)&1);k++){
    d.pixel=(k==0)&&!hold&&x<256&&y<240; d.x=x;d.y=y;d.rgb=colour(x,y,f); tick();
    if(d.lcd_mode==3&&last!=3){ if(d.lcd_vsync){ if(row>=0){frames++; if(rows==144)good++;} row=0;rows=0;} else row++; col=0; rows++; }
    last=d.lcd_mode;
    if(d.lcd_pixel&&row>=0&&row<144&&col<160){ if(!d.lcd_repeat) panel[row*160+col]=d.lcd_data; else repeat_pixels++; col++; }
    check(!d.overflow,"overflow");
   }
  }
 };
 for(int f=0;f<3;f++) run_frame(f,false);
 // Reference for frame 2 (the last full frame shown).
 int mism=0;
 for(int oy=0;oy<144;oy++)for(int ox=0;ox<160;ox++){
  unsigned want=0;
  if(oy>=2&&oy<142){
   int r=oy-2; double out[3]={0,0,0};
   // output row r covers lines 8+1.6r .. 8+1.6(r+1); column ox covers 1.6ox..1.6(ox+1)
   for(int c=0;c<3;c++){double s=0;
    for(int yy=0;yy<256;yy++){double y0=1.6*r,y1=y0+1.6; double ov=std::max(0.0,std::min(y1,(double)yy+1)-std::max(y0,(double)yy)); if(ov<=0)continue;
     for(int xx=0;xx<256;xx++){double x0=1.6*ox,x1=x0+1.6; double oh=std::max(0.0,std::min(x1,(double)xx+1)-std::max(x0,(double)xx)); if(oh<=0)continue;
      s+=ov*oh*((colour(xx,yy+8,2)>>(5*c))&31);}}
    out[c]=s/2.56;}
   for(int c=0;c<3;c++){int v=(int)(out[c]+0.5); if(v>31)v=31; want|=v<<(5*c);}
  }
  unsigned got=panel[oy*160+ox];
  for(int c=0;c<3;c++){int a=(got>>(5*c))&31,b=(want>>(5*c))&31; if(abs(a-b)>1){ if(mism<5)printf("pixel %d,%d ch%d got %d want %d\n",ox,oy,c,a,b); mism++;}}
 }
 printf("frames=%d good=%d mismatches=%d\n",frames,good,mism);
 check(good>=2 && mism==0,"picture");
 int rp=repeat_pixels; for(int f=0;f<2;f++) run_frame(f,true);
 printf("held repeat pixels=%d (140x160 per frame expected)\n",repeat_pixels-rp);
 check(repeat_pixels-rp>=140*160,"held frames repeat");
 // Quiesce: the NES stops at once mid-frame (here at line Y0, dot X0) for about
 // two frames, then carries on from the same dot. Every frame the panel gets
 // must still be 144 rows of exactly 160 pixels, except a stand-in frame the
 // hand-back cuts short, which is flagged by `cut` (the shell darkens it).
 for(int Y0: {100,9,236,0}) for(int X0: {0,120,250}) {
  for(int f=0;f<2;f++) run_frame(f,false);
  int nrows=-1,npx=0,bad=0,fr=0,cuts=0; std::vector<int> rowsz; int lastm=d.lcd_mode;
  auto obs=[&]{
   if(d.lcd_mode==3&&lastm!=3){
    if(nrows>=0) rowsz.push_back(npx);
    if(d.lcd_vsync){ if(nrows>=0){ fr++; if(rowsz.size()!=144) bad++; else for(int v:rowsz) if(v!=160) {bad++;break;} } rowsz.clear(); nrows=0; }
    npx=0;
   }
   lastm=d.lcd_mode; if(d.lcd_pixel) npx++; if(d.cut) cuts++;
   check(!d.overflow,"overflow (quiesce)");
  };
  int y=0,x=0; long frozen=0; bool paused=false,done=false; int k=0;
  for(long t=0;t<6L*262*341*13;t++){
   if(!paused && !done && y==Y0 && x==X0 && k==0 && fr>=1) { paused=true; d.qhold=1; frozen=0; }
   if(paused && ++frozen>2L*262*341*13+77777) { paused=false; done=true; d.qhold=0; }
   d.pixel=!paused && k==0 && x<256 && y<240; d.x=x; d.y=y; d.rgb=colour(x,y,5); tick(); obs();
   if(!paused){ if(++k>=12+((x+y)&1)){k=0; if(++x==341){x=0; if(++y==262)y=0;}} }
  }
  printf("quiesce at line %d dot %d: frames=%d bad=%d cuts=%d\n",Y0,X0,fr,bad,cuts);
  check(fr>=4 && bad<=cuts && cuts<=1,"quiesce frames whole");
 }
 printf("PASS nes_video_bridge\n"); return 0;}
