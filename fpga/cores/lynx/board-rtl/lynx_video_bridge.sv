// Lynx video to the Chromatic's GB framebuffer/panel protocol.
//
// The panel reads one 160-pixel row every 912 gclk (7296 xclk, 108.7 us) and
// restarts its frame on each lcd_vsync. A 60 Hz Lynx frame instead produces one
// of its 102 rows every ~158.7 us (105 lines per frame). The Lynx picture is
// letterboxed into the 160x144 panel frame (21 black rows above, 21 below), so
// the panel shows Lynx row k as row 21+k and consumes the 102 rows faster than
// the Lynx makes them. The bridge therefore:
//  - stores every Lynx row as 4-bit pens in a 64-row ring, with a copy of the
//    16-entry palette taken at the row's first pixel (4 block RAMs instead of
//    11 for full color);
//  - emits its own panel frames, one row per ROW_XCLK (a little faster than the
//    panel reads, so the panel's small line ring always has data);
//  - starts a frame (lcd_vsync) as late into the Lynx frame as the last Lynx
//    row allows: start = previous Lynx frame length - (21+101) rows of
//    emission + margin, measured from the current Lynx frame's first pixel.
//    Up to ~45 Lynx rows are buffered at 50 Hz, ~35 at 60 Hz.
// Lynx frames that arrive while a panel frame is still being emitted are not
// shown (only possible above ~64 Hz, i.e. with the 60 Hz lock off).
// While the core is paused (menu) or produces no frames, the bridge repeats the
// previous picture from the PSRAM framebuffer (lcd_repeat) at 60 Hz so the menu
// overlay keeps drawing.
module lynx_video_bridge #(
 parameter integer ROW_XCLK=7168,     // panel row period is 7296 xclk
 parameter integer PIXEL_XCLK=12,     // one pixel event per 3 hclk
 parameter integer TOP=21,
 parameter integer MARGIN=16384,
 parameter integer REPEAT_XCLK=1118481, // 60 Hz (2^26/60)
 parameter integer TIMEOUT_XCLK=2013265 // 30 ms without a Lynx frame
)(
 input wire xclk,xreset,hclk,hreset,enable,
 input wire paused,
 input wire pen_we, input wire [3:0] pen, input wire [7:0] pen_x, input wire [6:0] pen_y,
 input wire [191:0] palette,
 output wire overflow,
 output reg lcd_pixel,output reg [14:0] lcd_data,output reg lcd_repeat,
 output reg [1:0] lcd_mode,output reg lcd_on,output reg lcd_vsync);

 // ---------------- Lynx side: pen ring + palette snapshots ----------------
 // Pen ring: 64 rows x 160, address = slot*160 + x (< 10240), three 4Kx4 banks.
 function [13:0] pen_index(input [5:0] slot, input [7:0] x);
  pen_index={slot,7'd0}+{2'd0,slot,5'd0}+{6'd0,x};
 endfunction
 reg [3:0] bank0[0:4095], bank1[0:4095], bank2[0:4095];
 reg [11:0] pal_ram[0:1023];
 wire [13:0] wr_index=pen_index(pen_y[5:0],pen_x);
 always @(posedge xclk) if(pen_we) begin
  if(wr_index[13:12]==2'd0) bank0[wr_index[11:0]]<=pen;
  if(wr_index[13:12]==2'd1) bank1[wr_index[11:0]]<=pen;
  if(wr_index[13:12]==2'd2) bank2[wr_index[11:0]]<=pen;
 end
 // Palette copy: 16 entries over 16 cycles from the row's first pixel.
 reg copying=0; reg [3:0] copy_i=0; reg [5:0] copy_slot=0;
 always @(posedge xclk) begin
  if(copying) begin
   pal_ram[{copy_slot,copy_i}]<=palette[copy_i*12 +: 12];
   copy_i<=copy_i+1'b1;
   if(copy_i==15) copying<=0;
  end
  if(pen_we && pen_x==0) begin copying<=1; copy_i<=0; copy_slot<=pen_y[5:0]; end
 end

 // ---------------- Lynx frame timing ----------------
 localparam integer EMIT_SPAN=(TOP+101)*ROW_XCLK;
 reg [23:0] since0=0;        // xclk since the current Lynx frame's first pixel
 reg [23:0] frame_len=24'd1086300; // first pixel to last pixel, last frame
 reg in_frame=0, triggered=0;
 wire first_pixel=pen_we && pen_x==0 && pen_y==0;
 wire last_pixel=pen_we && pen_x==159 && pen_y==101;
 wire [24:0] start_delay_raw={1'b0,frame_len}+MARGIN-EMIT_SPAN;
 wire [23:0] start_delay=start_delay_raw[24] ? 24'd0 : start_delay_raw[23:0];
 reg start_live=0;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin since0<=0;in_frame<=0;triggered<=0;start_live<=0;frame_len<=24'd1086300;end
  else begin
   start_live<=0;
   if(!enable) begin in_frame<=0;triggered<=0;end
   else begin
    if(since0!=24'hffffff) since0<=since0+1'b1;
    if(first_pixel) begin since0<=0;in_frame<=1;triggered<=0;end
    else if(in_frame && !triggered && since0>=start_delay) begin triggered<=1;start_live<=1;end
    if(last_pixel && in_frame) begin
     in_frame<=0;
     // Ignore implausible lengths (a frame interrupted by pause or reset).
     if(since0>24'd600000 && since0<24'd2400000) frame_len<=since0;
    end
   end
  end
 end

 // ---------------- emission ----------------
 reg busy=0, live=0, have_picture=0;
 reg [7:0] row=0;            // 0..143
 reg [12:0] row_t=0;         // xclk within the row slot
 reg [7:0] col=0;            // pixel being emitted
 reg [3:0] pix_t=0;
 reg [23:0] idle_t=0;
 reg event_valid=0; reg [19:0] event_data=0;
 wire lynx_row=row>=TOP && row<TOP+102;
 wire [6:0] lynx_k=row-TOP;
 reg [13:0] rd_index=0; reg [1:0] rd_bank=0;
 reg [3:0] q0,q1,q2; reg [11:0] pal_q; reg [9:0] pal_addr=0;
 always @(posedge xclk) begin
  q0<=bank0[rd_index[11:0]]; q1<=bank1[rd_index[11:0]]; q2<=bank2[rd_index[11:0]];
  pal_q<=pal_ram[pal_addr];
 end
 wire [3:0] q_pen=rd_bank==2'd0 ? q0 : rd_bank==2'd1 ? q1 : q2;
 // GBR444 -> BGR555 (framebuffer format: B 14..10, G 9..5, R 4..0)
 wire [14:0] rgb={pal_q[7:4],pal_q[7],pal_q[11:8],pal_q[11],pal_q[3:0],pal_q[3]};
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin busy<=0;live<=0;have_picture<=0;row<=0;row_t<=0;col<=0;pix_t<=0;
   idle_t<=0;event_valid<=0;event_data<=0;rd_index<=0;rd_bank<=0;pal_addr<=0;end
  else begin
   event_valid<=0;
   if(!busy) begin
    if(idle_t!=24'hffffff) idle_t<=idle_t+1'b1;
    if(!enable) begin idle_t<=0;have_picture<=0;end
    else if(start_live && !paused) begin
     busy<=1;live<=1;have_picture<=1;row<=0;row_t<=0;idle_t<=0;
    end else if((paused && idle_t>=REPEAT_XCLK) || idle_t>=TIMEOUT_XCLK) begin
     // Repeat the last picture (black if there never was one).
     busy<=1;live<=0;row<=0;row_t<=0;idle_t<=0;
    end
   end else begin
    row_t<=row_t+1'b1;
    if(row_t==0) begin
     // Row start; bit 15 marks the first row (lcd_vsync).
     event_valid<=1;event_data<={2'b01,2'b00,row==0,15'd0};
     col<=0;pix_t<=0;
    end else if(row_t>=8 && col<160) begin
     pix_t<=pix_t==PIXEL_XCLK-1 ? 4'd0 : pix_t+1'b1;
     case(pix_t)
      0: begin rd_index<=pen_index(lynx_k[5:0],col); rd_bank<=pen_index(lynx_k[5:0],col)>>12; end
      // 1: pen RAM read
      2: pal_addr<={lynx_k[5:0],q_pen};
      // 3: palette read
      4: begin
       event_valid<=1;
       // Bit 16: show the framebuffer's previous pixel.
       if(!live) event_data<={3'b0,have_picture,1'b0,15'd0};
       else event_data<={3'b0,1'b0,1'b0,lynx_row ? rgb : 15'd0};
      end
      PIXEL_XCLK-1: col<=col+1'b1;
      default: ;
     endcase
    end else if(col==160 && pix_t==0) begin
     // Row end.
     event_valid<=1;event_data<={2'b10,18'd0};col<=161;
    end
    if(row_t==ROW_XCLK-1) begin
     row_t<=0;
     if(row==143) begin busy<=0;live<=0;idle_t<=0;end
     else row<=row+1'b1;
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
    default: lcd_pixel<=0;
   endcase
  end
 end
endmodule
