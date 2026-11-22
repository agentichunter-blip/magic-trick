// NES picture to the Chromatic panel stream (same protocol as the Game Gear
// bridge: row-start event, 160 pixel events, row-end event; the first row of
// a frame carries VSYNC).
//
// The NES shows 256x240; lines 8-231 (the 224 lines a TV shows) are scaled by
// 5/8 in both directions with a box filter (as gg_video_bridge.sv does for
// the Master System): 160x140, with 2 black rows above and 2 below. Each
// output column covers 1.6 input pixels and each output row 1.6 input lines,
// weights in eighths; a 160-entry accumulator row in block RAM holds the
// output row being built, so no frame buffer is needed.
//
// Row timing in NES lines: top black rows 0-1 (frame start on line 0),
// picture rows from lines 8-231 (one output row completes on lines 9, 11,
// 12, 14, 15 of every 8), bottom black rows 232-233. A save state pauses the
// NES in vertical blank (line 241 or later), between frames.
//
// While a save state holds the game (hold), a local timing generator keeps
// the same 144 rows coming with every pixel flagged repeat, so the panel shows
// the previous frame; resume_ok marks its vertical blank (lines 240-260).
//
// Input: one xclk-cycle pixel strobe per visible dot (x 0-255, y 0-239; more
// are ignored), at least 9 xclk apart, with its colour as {blue[4:0], green[4:0], red[4:0]} (the panel's
// format), all valid in the strobe cycle.
module nes_video_bridge(
 input wire xclk,xreset,input wire hclk,hreset,input wire enable,
 input wire hold,output wire resume_ok,
 input wire pixel,input wire [8:0] x,y,input wire [14:0] rgb,
 output wire overflow,output reg lcd_pixel,output reg [14:0] lcd_data,
 output reg lcd_repeat,
 output reg [1:0] lcd_mode,output reg lcd_on,output reg lcd_vsync);
 // Stand-in timing: 341 dots x 262 lines at the NES dot rate (12.5 xclk).
 reg [3:0] tick; reg odd; reg [8:0] sx,sy; reg spixel;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin tick<=0;odd<=0;sx<=0;sy<=241;spixel<=0;end
  else if(!hold) begin tick<=0;odd<=0;sx<=0;sy<=241;spixel<=0;end
  else begin
   spixel<=0;
   if(tick==(odd ? 4'd12 : 4'd11)) begin
    tick<=0;odd<=!odd;spixel<=1;
    if(sx==340) begin sx<=0;sy<=sy==261 ? 9'd0 : sy+1'b1;end else sx<=sx+1'b1;
   end else tick<=tick+1'b1;
  end
 end
 assign resume_ok=hold && sy>=240 && sy<=260;
 wire src_pixel=hold ? spixel : pixel;
 wire [8:0] px=hold ? sx : x, py=hold ? sy : y;
 wire [4:0] c2=rgb[14:10],c1=rgb[9:5],c0=rgb[4:0];

 // Row filter weights for picture line py (phase (py-8) mod 8 = py[2:0]):
 // wc to the output row being built, emit when it completes on this line,
 // wn to the next output row.
 reg [2:0] wc,wn; reg emit;
 always @* case(py[2:0])
  3'd0: begin wc=5;emit=0;wn=0;end
  3'd1: begin wc=3;emit=1;wn=2;end
  3'd2: begin wc=5;emit=0;wn=0;end
  3'd3: begin wc=1;emit=1;wn=4;end
  3'd4: begin wc=4;emit=1;wn=1;end
  3'd5: begin wc=5;emit=0;wn=0;end
  3'd6: begin wc=2;emit=1;wn=3;end
  default: begin wc=5;emit=1;wn=0;end
 endcase
 wire picture_line=py>=8 && py<232;
 wire black_line=py<2 || (py>=232 && py<234);
 wire row_line=(picture_line && emit) || black_line;
 // Column filter weights for dot px (phase px[2:0]).
 reg [2:0] cw,nw; reg out_here;
 always @* case(px[2:0])
  3'd0: begin out_here=0;cw=0;nw=5;end
  3'd1: begin out_here=1;cw=3;nw=2;end
  3'd2: begin out_here=0;cw=5;nw=0;end
  3'd3: begin out_here=1;cw=1;nw=4;end
  3'd4: begin out_here=1;cw=4;nw=1;end
  3'd5: begin out_here=0;cw=5;nw=0;end
  3'd6: begin out_here=1;cw=2;nw=3;end
  default: begin out_here=1;cw=5;nw=0;end
 endcase
 // Products as shift-and-add (Gowin synthesis crashed on small multipliers).
 function [7:0] wh(input [2:0] w,input [4:0] s);
  wh=(w[0] ? {3'd0,s} : 8'd0)+(w[1] ? {2'd0,s,1'b0} : 8'd0)+(w[2] ? {1'b0,s,2'b0} : 8'd0);
 endfunction
 function [10:0] wv(input [2:0] w,input [7:0] h);
  wv=(w[0] ? {3'd0,h} : 11'd0)+(w[1] ? {2'd0,h,1'b0} : 11'd0)+(w[2] ? {1'b0,h,2'b0} : 11'd0);
 endfunction
 reg [7:0] ha2,ha1,ha0;           // partial column sums (eighths, 0..248)
 reg hv; reg [7:0] hcol,hout; reg [7:0] h2,h1,h0; reg [2:0] hwc,hwn; reg hemit;
 // Accumulator row: 160 x 3 x 11 bits (weights total 64 per output).
 (* syn_ramstyle="block_ram" *) reg [32:0] acc[0:159];
 reg [7:0] acc_ra; reg [32:0] acc_q;
 reg [7:0] wa; reg [32:0] wd; reg we;
 always @(posedge xclk) begin
  if(we) acc[wa]<=wd;
  acc_q<=acc[acc_ra];
 end
 reg s1,s2; reg [7:0] s1col,s2col; reg [7:0] s1h2,s1h1,s1h0,s2h2,s2h1,s2h0;
 reg [2:0] s1wc,s1wn,s2wc,s2wn; reg s1emit,s2emit;
 wire [10:0] n2=acc_q[32:22]+wv(s2wc,s2h2), n1=acc_q[21:11]+wv(s2wc,s2h1), n0=acc_q[10:0]+wv(s2wc,s2h0);
 wire [10:0] r2=n2+11'd32, r1=n1+11'd32, r0=n0+11'd32;   // 0..1984 -> 0..31
 reg event_valid; reg [19:0] event_data;
 reg [2:0] end_delay;  // row end: 6 cycles after dot 255, after its pixel event
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin
   event_valid<=0;event_data<=0;
   ha2<=0;ha1<=0;ha0<=0;hv<=0;hcol<=0;hout<=0;h2<=0;h1<=0;h0<=0;hwc<=0;hwn<=0;hemit<=0;
   s1<=0;s2<=0;s1col<=0;s2col<=0;s1h2<=0;s1h1<=0;s1h0<=0;s2h2<=0;s2h1<=0;s2h0<=0;
   s1wc<=0;s1wn<=0;s2wc<=0;s2wn<=0;s1emit<=0;s2emit<=0;
   acc_ra<=0;we<=0;wa<=0;wd<=0;end_delay<=0;
  end else begin
   event_valid<=0;hv<=0;we<=0;
   if(end_delay!=0) end_delay<=end_delay-1'b1;
   if(end_delay==1) begin event_valid<=1;event_data<={2'b10,18'd0};end
   s1<=hv;s2<=s1;
   if(hv) begin acc_ra<=hout;s1col<=hout;s1h2<=h2;s1h1<=h1;s1h0<=h0;s1wc<=hwc;s1wn<=hwn;s1emit<=hemit;end
   if(s1) begin s2col<=s1col;s2h2<=s1h2;s2h1<=s1h1;s2h0<=s1h0;s2wc<=s1wc;s2wn<=s1wn;s2emit<=s1emit;end
   if(s2) begin
    we<=1;wa<=s2col;
    wd<=s2emit ? {wv(s2wn,s2h2),wv(s2wn,s2h1),wv(s2wn,s2h0)} : {n2,n1,n0};
    if(s2emit && enable) begin
     event_valid<=1;
     event_data<={5'b0,r2[10:6],r1[10:6],r0[10:6]};
    end
   end
   if(src_pixel) begin
    if(!hold && picture_line && px<256) begin
     if(out_here) begin
      h2<=ha2+wh(cw,c2);h1<=ha1+wh(cw,c1);h0<=ha0+wh(cw,c0);
      hv<=1;hout<=hcol;hcol<=hcol+1'b1;hwc<=wc;hwn<=wn;hemit<=emit;
      ha2<=wh(nw,c2);ha1<=wh(nw,c1);ha0<=wh(nw,c0);
     end else if(px[2:0]==0) begin
      ha2<=wh(nw,c2);ha1<=wh(nw,c1);ha0<=wh(nw,c0);
      if(px==0) hcol<=0;
     end else begin
      ha2<=ha2+wh(cw,c2);ha1<=ha1+wh(cw,c1);ha0<=ha0+wh(cw,c0);
     end
    end
    // Row framing on the dot strobe: start at dot 0 (bit 15 = first row),
    // end after dot 255. Black rows, and every row while held, get their 160
    // pixels from dots 1-160 (black, or repeat the framebuffer's pixel).
    if(enable && row_line) begin
     if(px==0) begin event_valid<=1;event_data<={2'b01,2'b0,py==0,15'd0};end
     else if((black_line || hold) && px<=160) begin event_valid<=1;event_data<={3'b0,hold && !black_line,16'd0};end
     else if(px==255) end_delay<=3'd6;
    end
   end
  end
 end
 wire take;wire [19:0] value;
 gg_async_fifo fifo(.wclk(xclk),.wrst(xreset),.wvalid(event_valid),.wdata(event_data),
  .full(),.overflow(overflow),.rclk(hclk),.rrst(hreset),.rvalid(take),.rdata(value));
 always @(posedge hclk or posedge hreset) begin
  if(hreset) begin lcd_pixel<=0;lcd_data<=0;lcd_repeat<=0;lcd_mode<=0;lcd_on<=0;lcd_vsync<=0;end
  else begin
   lcd_pixel<=0;
   if(take) case(value[19:18])
    2'b01: begin lcd_mode<=2'b11;lcd_on<=1;lcd_vsync<=value[15];end
    2'b10: begin lcd_mode<=0;end
    2'b00: begin lcd_data<=value[14:0];lcd_repeat<=value[16];lcd_pixel<=1;end
    default: begin lcd_pixel<=0;end
   endcase
  end
 end
endmodule
