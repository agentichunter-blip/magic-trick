// Translate the core's cropped stream to the existing GB framebuffer protocol.
// row-start -> mode[1] high, exactly 160 pixel strobes, row-end -> mode[1] low.
// The original panel scanout/PSRAM framebuffer remains the display-clock owner.
// While a save state freezes the game (hold), the panel still needs frames: a
// local timing generator keeps rows coming, and their pixels are flagged
// lcd_repeat so the display shows the previous frame from the framebuffer.
// resume_ok marks the generator's vertical blank, where the frozen game
// (stopped in its own vertical blank) can continue without a partial frame.
//
// Master System (sms=1): the whole 256x192 picture is scaled down, like a
// Game Gear's own SMS mode. Columns: a box filter maps 8 pixels to 5 (each
// output covers 1.6 inputs; weights in eighths). Rows: the same 8-to-5 filter
// gives 160x120, letterboxed with 12 black rows above and below (fit), or a
// 4-to-3 filter fills all 144 rows (stretch). A 160-entry accumulator row in
// block RAM collects the weighted rows; a finished output row is emitted while
// the last input row that touches it streams in, so no frame buffer is needed.
// Row timing (VDP line numbers, NTSC: 0-218 then 469-511, 224-line mode 0-234
// then 485-511): picture rows from lines 0-191, bottom black rows 192-203,
// top black rows (frame start) 485-496. Save states freeze only in lines
// 204-234 (gg_board_core.sv), so a frozen game never cuts a frame in two.
module gg_video_bridge(
 input wire xclk,xreset,input wire hclk,hreset,input wire enable,
 input wire sms,input wire stretch,
 input wire hold,output wire resume_ok,
 input wire pixel,input wire [8:0] x,y,input wire [11:0] color,
 output wire overflow,output reg lcd_pixel,output reg [14:0] lcd_data,
 output reg lcd_repeat,
 output reg [1:0] lcd_mode,output reg lcd_on,output reg lcd_vsync);
 // 342 pixels x 262 lines at the Game Gear pixel rate (12.5 xclk per pixel).
 reg [3:0] tick; reg odd; reg [8:0] sx,sy; reg spixel;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin tick<=0;odd<=0;sx<=0;sy<=192;spixel<=0;end
  else if(!hold) begin tick<=0;odd<=0;sx<=0;sy<=192;spixel<=0;end
  else begin
   spixel<=0;
   if(tick==(odd ? 4'd12 : 4'd11)) begin
    tick<=0;odd<=!odd;spixel<=1;
    if(sx==341) begin sx<=0;sy<=sy==261 ? 9'd0 : sy+1'b1;end else sx<=sx+1'b1;
   end else tick<=tick+1'b1;
  end
 end
 assign resume_ok=hold && (sms ? (sy>=204 && sy<=234) : (sy>=176 && sy<=250));
 wire src_pixel=hold ? spixel : pixel;
 wire [8:0] src_x=hold ? sx : x, src_y=hold ? sy : y;
 // VDP selects Game Gear content at x=49..208. The one-pixel
 // coordinate delay maps that to framebuffer columns 0..159.
 // In SMS terms: px0 is the picture column, py0 the picture line.
 reg [8:0] px0,py0;
 reg event_valid;
 reg [19:0] event_data;
 // ---- Master System scaler ----
 wire scale=sms && !hold;
 wire [3:0] c2=color[11:8],c1=color[7:4],c0=color[3:0];
 // Small constant-range products as shift-and-add (Gowin synthesis crashed
 // inferring them as multipliers).
 function [6:0] w8(input [2:0] w,input [3:0] s);
  w8=(w[0] ? {3'd0,s} : 7'd0)+(w[1] ? {2'd0,s,1'b0} : 7'd0)+(w[2] ? {1'b0,s,2'b0} : 7'd0);
 endfunction
 function [9:0] wv(input [2:0] w,input [6:0] h);
  wv=(w[0] ? {3'd0,h} : 10'd0)+(w[1] ? {2'd0,h,1'b0} : 10'd0)+(w[2] ? {1'b0,h,2'b0} : 10'd0);
 endfunction
 // Row filter weights for picture line py0: wc to the output row being
 // built, emit when that row completes on this line, wn to the next row.
 reg [2:0] wc,wn; reg emit;
 always @* begin
  if(stretch) case(py0[1:0])
   2'd0: begin wc=6;emit=0;wn=0;end
   2'd1: begin wc=2;emit=1;wn=4;end
   2'd2: begin wc=4;emit=1;wn=2;end
   default: begin wc=6;emit=1;wn=0;end
  endcase else case(py0[2:0])
   3'd0: begin wc=5;emit=0;wn=0;end
   3'd1: begin wc=3;emit=1;wn=2;end
   3'd2: begin wc=5;emit=0;wn=0;end
   3'd3: begin wc=1;emit=1;wn=4;end
   3'd4: begin wc=4;emit=1;wn=1;end
   3'd5: begin wc=5;emit=0;wn=0;end
   3'd6: begin wc=2;emit=1;wn=3;end
   default: begin wc=5;emit=1;wn=0;end
  endcase
 end
 wire picture_line=py0<192;
 wire emit_line=picture_line && emit;
 // First output row: line 1 when stretched; the top black rows when fitted.
 wire first_row=stretch ? py0==1 : py0==485;
 wire black_line=!stretch && ((py0>=192 && py0<204) || (py0>=485 && py0<497));
 wire row_line=emit_line || black_line;
 // Column filter (phase px0[2:0]): partial sums ha*, finished column h*.
 reg [6:0] ha2,ha1,ha0;
 reg hv; reg [7:0] hcol,hout; reg [6:0] h2,h1,h0; reg [2:0] hwc,hwn; reg hemit;
 reg [2:0] cw,nw; reg out_here;
 always @* begin
  case(px0[2:0])
   3'd0: begin out_here=0;cw=0;nw=5;end
   3'd1: begin out_here=1;cw=3;nw=2;end
   3'd2: begin out_here=0;cw=5;nw=0;end
   3'd3: begin out_here=1;cw=1;nw=4;end
   3'd4: begin out_here=1;cw=4;nw=1;end
   3'd5: begin out_here=0;cw=5;nw=0;end
   3'd6: begin out_here=1;cw=2;nw=3;end
   default: begin out_here=1;cw=5;nw=0;end
  endcase
 end
 // Accumulator row: 160 x 3 x 10 bits (weights sum to 64 per output).
 (* syn_ramstyle="block_ram" *) reg [29:0] acc[0:159];
 reg [7:0] acc_ra; reg [29:0] acc_q;
 reg [7:0] wa; reg [29:0] wd; reg we;
 always @(posedge xclk) begin
  if(we) acc[wa]<=wd;
  acc_q<=acc[acc_ra];
 end
 reg s1,s2; reg [7:0] s1col,s2col; reg [6:0] s1h2,s1h1,s1h0,s2h2,s2h1,s2h0;
 reg [2:0] s1wc,s1wn,s2wc,s2wn; reg s1emit,s2emit;
 wire [9:0] m2=wv(s2wc,s2h2), m1=wv(s2wc,s2h1), m0=wv(s2wc,s2h0);
 wire [9:0] k2=wv(s2wn,s2h2), k1=wv(s2wn,s2h1), k0=wv(s2wn,s2h0);
 wire [9:0] n2=acc_q[29:20]+m2, n1=acc_q[19:10]+m1, n0=acc_q[9:0]+m0;
 // Output channel: 0..960 -> 0..30 (5 bits), rounded.
 wire [9:0] r2=n2+10'd16, r1=n1+10'd16, r0=n0+10'd16;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin
   px0<=0;py0<=0;event_valid<=0;event_data<=0;
   ha2<=0;ha1<=0;ha0<=0;hv<=0;hcol<=0;hout<=0;h2<=0;h1<=0;h0<=0;hwc<=0;hwn<=0;hemit<=0;
   s1<=0;s2<=0;s1col<=0;s2col<=0;s1h2<=0;s1h1<=0;s1h0<=0;s2h2<=0;s2h1<=0;s2h0<=0;
   s1wc<=0;s1wn<=0;s2wc<=0;s2wn<=0;s1emit<=0;s2emit<=0;
   acc_ra<=0;we<=0;wa<=0;wd<=0;
  end else begin
   event_valid<=0;hv<=0;we<=0;
   // Read the accumulator (two cycles), then add this row's share and
   // emit the pixel when the output row completes on this line.
   s1<=hv;s2<=s1;
   if(hv) begin acc_ra<=hout;s1col<=hout;s1h2<=h2;s1h1<=h1;s1h0<=h0;s1wc<=hwc;s1wn<=hwn;s1emit<=hemit;end
   if(s1) begin s2col<=s1col;s2h2<=s1h2;s2h1<=s1h1;s2h0<=s1h0;s2wc<=s1wc;s2wn<=s1wn;s2emit<=s1emit;end
   if(s2) begin
    we<=1;wa<=s2col;
    wd<=s2emit ? {k2,k1,k0} : {n2,n1,n0};
    if(s2emit && enable) begin
     event_valid<=1;
     event_data<={5'b0,r2[9:5],r1[9:5],r0[9:5]};
    end
   end
   if(src_pixel) begin
    px0<=src_x;py0<=src_y;
    if(scale) begin
     // Column filter on (px0, color).
     if(picture_line && px0<256) begin
      if(out_here) begin
       h2<=ha2+w8(cw,c2);h1<=ha1+w8(cw,c1);h0<=ha0+w8(cw,c0);
       hv<=1;hout<=hcol;hcol<=hcol+1'b1;hwc<=wc;hwn<=wn;hemit<=emit;
       ha2<=w8(nw,c2);ha1<=w8(nw,c1);ha0<=w8(nw,c0);
      end else if(px0[2:0]==0) begin
       ha2<=w8(nw,c2);ha1<=w8(nw,c1);ha0<=w8(nw,c0);
       if(px0==0) hcol<=0;
      end else begin
       ha2<=ha2+w8(cw,c2);ha1<=ha1+w8(cw,c1);ha0<=ha0+w8(cw,c0);
      end
     end
     // Row framing (and black letterbox pixels) on the pixel strobe.
     if(enable && row_line) begin
      if(px0==0) begin event_valid<=1;event_data<={2'b01,2'b0,first_row,15'd0};end
      else if(black_line && px0<=160) begin event_valid<=1;event_data<=20'd0;end
      else if(px0==256) begin event_valid<=1;event_data<={2'b10,18'd0};end
     end
    end else if(enable && py0>=24 && py0<168) begin
     // Type 01=start; bit 15 marks first row. 10=end. 00=pixel.
     if(px0==47) begin event_valid<=1;event_data<={2'b01,2'b0,py0==24,15'd0};end
     else if(px0>=48 && px0<208) begin
      event_valid<=1;
      // Bit 16: repeat the framebuffer's previous pixel (frozen game).
      event_data<={3'b0,hold,1'b0,color[11:8],color[11],color[7:4],color[7],color[3:0],color[3]};
     end else if(px0==208) begin event_valid<=1;event_data<={2'b10,18'd0};end
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
    2'b10: begin lcd_mode<=0;end // Keep first-row VSYNC through line flush.
    2'b00: begin lcd_data<=value[14:0];lcd_repeat<=value[16];lcd_pixel<=1;end
    default: begin lcd_pixel<=0;end
   endcase
  end
 end
endmodule
