// Lynx video to the Chromatic's GB framebuffer/panel protocol.
//
// The panel reads one 160-pixel row every 912 gclk (7296 xclk, 108.7 us) and
// restarts its frame on each lcd_vsync. A 60 Hz Lynx frame instead produces one
// of its 102 rows every ~158.7 us (105 lines per frame). The Lynx picture is
// letterboxed into the 160x144 panel frame (21 black rows above, 21 below), so
// the panel shows Lynx row k as row 21+k and consumes the 102 rows faster than
// the Lynx makes them. The bridge therefore:
//  - stores every Lynx row as 4-bit pens in a 48-row ring, with a copy of the
//    16-entry palette taken at the row's first pixel (3 block RAMs instead of
//    11 for full color);
//  - emits its own panel frames, one row per ROW_XCLK (a little faster than the
//    panel reads, so the panel's small line ring always has data);
//  - starts a frame (lcd_vsync) as late into the Lynx frame as the last Lynx
//    row allows: start = previous Lynx frame length - (21+101) rows of
//    emission + margin, measured from the current Lynx frame's first pixel.
//    Up to ~45 Lynx rows are buffered at 50 Hz, ~35 at 60 Hz.
// Stretch mode (stretch=1, latched per panel frame) fills all 144 panel rows
// instead: panel row r covers Lynx rows [r*17/24, (r+1)*17/24). A panel row
// inside one Lynx row shows it unchanged; one that straddles two Lynx rows
// shows both mixed by coverage in quarters (SMOOTH=1) or the row under its
// centre (SMOOTH=0). Each Lynx row then lasts 1.41 panel rows (10121 xclk),
// still faster than the Lynx makes them (~10650 xclk at 60 Hz), so the frame
// starts so that Lynx row 101 is ready at panel row 142, the first that needs it.
// Lynx frames that arrive while a panel frame is still being emitted are not
// shown (only possible above ~64 Hz, i.e. with the 60 Hz lock off).
// Rotated games (rotation=1/2, set by the MCU from the .lnx header or its list
// of sideways games) are drawn for a Lynx held on its side. Here the picture
// is turned upright instead: rotation 1 ("left", e.g. Gauntlet, Klax) turns it
// clockwise, 2 ("right", e.g. Centipede, Raiden) anticlockwise, and the
// 102x160 result is scaled by 9/10 to 92x144 (34 black columns each side),
// each panel pixel mixing 2x2 Lynx pixels by coverage in quarters. A panel row
// needs a whole Lynx frame, so the pens go to two full frame buffers instead of
// the ring: the Lynx writes one while the panel shows the other, and a panel
// frame starts as soon as a Lynx frame is complete (one frame of latency). Its
// palette is the one in effect at the frame's first pixel.
// While the core is paused (menu) or produces no frames, the bridge repeats the
// previous picture from the PSRAM framebuffer (lcd_repeat) at 60 Hz so the menu
// overlay keeps drawing.
module lynx_video_bridge #(
 parameter integer ROW_XCLK=7168,     // panel row period is 7296 xclk
 parameter integer PIXEL_XCLK=12,     // one pixel event per 3 hclk
 parameter integer TOP=21,
 parameter integer MARGIN=16384,
 parameter integer REPEAT_XCLK=1118481, // 60 Hz (2^26/60)
 parameter integer TIMEOUT_XCLK=2013265, // 30 ms without a Lynx frame
 parameter integer SMOOTH=1
)(
 input wire xclk,xreset,hclk,hreset,enable,
 input wire paused,
 input wire stretch,         // any clock domain; applied from the next frame
 input wire [1:0] rotation,  // xclk, constant while a game runs: 0 none, 1 left, 2 right
 input wire pen_we, input wire [3:0] pen, input wire [7:0] pen_x, input wire [6:0] pen_y,
 input wire [191:0] palette,
 output wire overflow,
 output reg lcd_pixel,output reg [14:0] lcd_data,output reg lcd_repeat,
 output reg [1:0] lcd_mode,output reg lcd_on,output reg lcd_vsync);

 // ---------------- Lynx side: pen ring + palette snapshots ----------------
 // Pen ring: 48 rows x 160 (Lynx row y in slot y mod 48), address
 // slot*160 + x (< 7680), two 4Kx4 banks. At 50 Hz up to ~45 rows wait.
 function [5:0] slot_of(input [6:0] y);
  slot_of=y>=7'd96 ? 6'(y-7'd96) : y>=7'd48 ? 6'(y-7'd48) : y[5:0];
 endfunction
 function [12:0] pen_index(input [5:0] slot, input [7:0] x);
  pen_index={slot,7'd0}+{2'd0,slot,5'd0}+{5'd0,x};
 endfunction
 // Rotated: two frame buffers, pen (x,y) at {buffer, y*160+x}; the ring and
 // buffer 0 share the same block RAM. Palettes: ring slots 0-47, frame
 // buffers slots 48/49.
 function [13:0] frame_index(input [6:0] y, input [7:0] x);
  frame_index={y,7'd0}+{2'd0,y,5'd0}+{6'd0,x};
 endfunction
 wire rotated=rotation!=2'd0;
 reg wbuf=0;                 // frame buffer the Lynx writes (rotated)
 reg [3:0] pens[0:32767];
 reg [11:0] pal_ram[0:1023];
 wire [14:0] wr_index=rotated ? {wbuf,frame_index(pen_y,pen_x)} : {2'b00,pen_index(slot_of(pen_y),pen_x)};
 always @(posedge xclk) if(pen_we) pens[wr_index]<=pen;
 // Palette copy: 16 entries over 16 cycles from the row's first pixel
 // (rotated: the frame's first pixel).
 reg copying=0; reg [3:0] copy_i=0; reg [5:0] copy_slot=0;
 always @(posedge xclk) begin
  if(copying) begin
   pal_ram[{copy_slot,copy_i}]<=palette[copy_i*12 +: 12];
   copy_i<=copy_i+1'b1;
   if(copy_i==15) copying<=0;
  end
  if(pen_we && pen_x==0 && (!rotated || pen_y==0)) begin
   copying<=1; copy_i<=0; copy_slot<=rotated ? {5'b11000,wbuf} : slot_of(pen_y);
  end
 end

 // ---------------- Lynx frame timing ----------------
 localparam integer EMIT_SPAN=(TOP+101)*ROW_XCLK;
 localparam integer EMIT_SPAN_STRETCH=142*ROW_XCLK;
 (* ASYNC_REG="TRUE" *) reg [1:0] stretch_sync=0;
 always @(posedge xclk) stretch_sync<={stretch_sync[0],stretch};
 reg [23:0] since0=0;        // xclk since the current Lynx frame's first pixel
 reg [23:0] frame_len=24'd1086300; // first pixel to last pixel, last frame
 reg in_frame=0, triggered=0;
 wire first_pixel=pen_we && pen_x==0 && pen_y==0;
 wire last_pixel=pen_we && pen_x==159 && pen_y==101;
 wire [24:0] start_delay_raw={1'b0,frame_len}+MARGIN-(stretch_sync[1] ? EMIT_SPAN_STRETCH : EMIT_SPAN);
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
    else if(rotated) begin if(last_pixel && in_frame) start_live<=1; end // whole frame stored
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
 // Row mapping. Letterbox: Lynx row row-TOP, black outside. Stretch: panel row
 // r starts acc/48 into Lynx row k (acc = r*34 mod 48); weight_a is Lynx row
 // k's share of the row in quarters, the rest is row k+1.
 reg stretch_f=0;            // stretch, latched at the frame start
 reg [5:0] acc=0; reg [6:0] k=0;
 wire lynx_row=stretch_f || (row>=TOP && row<TOP+102);
 wire [6:0] lynx_k=stretch_f ? k : 7'(row-TOP);
 wire [5:0] lynx_slot=slot_of(lynx_k);
 wire [5:0] lynx_slot_b=slot_of(lynx_k+1'b1);
 wire [5:0] span=6'd48-acc;  // part of row k under panel row r, in 48ths
 reg [2:0] weight_a;
 always @* begin
  if(!stretch_f || acc<=6'd14) weight_a=3'd4;       // inside row k
  else if(SMOOTH==0) weight_a=span>6'd17 ? 3'd4 : 3'd0;
  else if(span<=6'd4) weight_a=3'd0;                // round(span*4/34)
  else if(span<=6'd12) weight_a=3'd1;
  else if(span<=6'd21) weight_a=3'd2;
  else if(span<=6'd29) weight_a=3'd3;
  else weight_a=3'd4;
 end
 reg [12:0] rd_index=0;
 reg [3:0] q_pen; reg [11:0] pal_q; reg [9:0] pal_addr=0;
 reg [11:0] color_a=0;
 // Rotated read position: panel row r covers rotated rows [10r/9, 10r/9+10/9),
 // i.e. ra=floor(10r/9) and ra+1, ra's share wr in quarters (rj = 10r mod 9);
 // likewise columns ca/ca+1 with wc for panel column 34+p.
 reg rot_f=0, rot_left=0, dbuf=0; // latched per frame; dbuf = buffer shown
 reg [7:0] ra=0; reg [3:0] rj=0; reg [6:0] ca=0; reg [3:0] cj=0;
 reg [7:0] xa=0, xb=0;       // Lynx x of rotated rows ra, ra+1 (this panel row)
 reg [13:0] ya=0, yb=0;      // y*160 of rotated columns ca, ca+1
 reg [14:0] rd_rot=0;
 reg [17:0] m0=0;            // 2-row mix of column ca (6 bits per channel)
 reg [11:0] c0=0, c2=0;
 function [2:0] quarter(input [3:0] j); // round((9-j)/10*4): share of the first tap
  case(j) 4'd0: quarter=3'd4; 4'd1,4'd2: quarter=3'd3; 4'd3,4'd4,4'd5: quarter=3'd2;
   4'd6,4'd7: quarter=3'd1; default: quarter=3'd0; endcase
 endfunction
 function [5:0] vmix(input [3:0] a, input [3:0] b, input [2:0] w); // a*w+b*(4-w)
  vmix={2'b00,a}*{3'b000,w}+{2'b00,b}*{3'b000,3'd4-w};
 endfunction
 function [4:0] hmix(input [5:0] a, input [5:0] b, input [2:0] w); // (a*w+b*(4-w))/16 -> 5 bits
  reg [7:0] t;
  begin t={2'b00,a}*{5'd0,w}+{2'b00,b}*{5'd0,3'd4-w}; hmix=t[7:3]|{4'd0,t[7]}; end
 endfunction
 wire [2:0] wr_q=quarter(rj), wc_q=quarter(cj);
 wire [7:0] ra_b=ra==8'd159 ? ra : ra+1'b1;
 wire [6:0] ca_b=ca==7'd101 ? ca : ca+1'b1;
 function [13:0] y160(input [6:0] y); y160={y,7'd0}+{2'd0,y,5'd0}; endfunction
 wire [17:0] m_now={vmix(c2[11:8],pal_q[11:8],wr_q),vmix(c2[7:4],pal_q[7:4],wr_q),vmix(c2[3:0],pal_q[3:0],wr_q)};
 // m0/m_now: {G,B,R} 6-bit sums (GBR444 channel order kept)
 wire [14:0] rgb_rot={hmix(m0[11:6],m_now[11:6],wc_q),hmix(m0[17:12],m_now[17:12],wc_q),hmix(m0[5:0],m_now[5:0],wc_q)};
 wire rot_visible=col>=8'd34 && col<8'd126;
 always @(posedge xclk) begin
  q_pen<=pens[rot_f ? rd_rot : {2'b00,rd_index}];
  pal_q<=pal_ram[pal_addr];
 end
 // a*w + b*(4-w) for one 4-bit channel, then 4-bit-scale -> 5 bits: a pure
 // colour c becomes {c,c[3]} as before, a mix keeps its half step.
 function [4:0] mix(input [3:0] a, input [3:0] b, input [2:0] w);
  reg [5:0] sum;
  begin
   sum=(w[2] ? {a,2'b00} : 6'd0)+(w[1] ? {1'b0,a,1'b0} : 6'd0)+(w[0] ? {2'b00,a} : 6'd0)
      +(w[1:0]==2'd0 ? (w[2] ? 6'd0 : {b,2'b00}) : w[1:0]==2'd1 ? ({1'b0,b,1'b0}+{2'b00,b})
        : w[1:0]==2'd2 ? {1'b0,b,1'b0} : {2'b00,b});
   mix=sum[5:1]|{4'd0,sum[5]};
  end
 endfunction
 reg [2:0] weight_q=4;
 // GBR444 -> BGR555 (framebuffer format: B 14..10, G 9..5, R 4..0)
 wire [14:0] rgb={mix(color_a[7:4],pal_q[7:4],weight_q),mix(color_a[11:8],pal_q[11:8],weight_q),
  mix(color_a[3:0],pal_q[3:0],weight_q)};
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin busy<=0;live<=0;have_picture<=0;row<=0;row_t<=0;col<=0;pix_t<=0;
   idle_t<=0;event_valid<=0;event_data<=0;rd_index<=0;pal_addr<=0;
   stretch_f<=0;acc<=0;k<=0;color_a<=0;weight_q<=4;
   rot_f<=0;rot_left<=0;dbuf<=0;wbuf<=0;ra<=0;rj<=0;ca<=0;cj<=0;end
  else begin
   event_valid<=0;
   if(!busy) begin
    if(idle_t!=24'hffffff) idle_t<=idle_t+1'b1;
    if(!enable) begin idle_t<=0;have_picture<=0;end
    else if(start_live && !paused) begin
     busy<=1;live<=1;have_picture<=1;row<=0;row_t<=0;idle_t<=0;
     stretch_f<=stretch_sync[1] && !rotated;acc<=0;k<=0;
     // Rotated: show the frame just completed, write the next into the other.
     rot_f<=rotated;rot_left<=rotation==2'd1;ra<=0;rj<=0;
     if(rotated) begin dbuf<=wbuf;wbuf<=!wbuf;end
    end else if((paused && idle_t>=REPEAT_XCLK) || idle_t>=TIMEOUT_XCLK) begin
     // Repeat the last picture (black if there never was one).
     busy<=1;live<=0;row<=0;row_t<=0;idle_t<=0;rot_f<=0;
    end
   end else begin
    row_t<=row_t+1'b1;
    if(row_t==0) begin
     // Row start; bit 15 marks the first row (lcd_vsync).
     event_valid<=1;event_data<={2'b01,2'b00,row==0,15'd0};
     col<=0;pix_t<=0;ca<=0;cj<=0;
     // left: x = rotated row; right: x = 159 - rotated row
     xa<=rot_left ? ra : 8'd159-ra; xb<=rot_left ? ra_b : 8'd159-ra_b;
    end else if(row_t>=8 && col<160 && rot_f) begin
     // Rotated pixel: taps (ra,ca) (ra+1,ca) (ra,ca+1) (ra+1,ca+1), one RAM
     // read each from pix_t 1, palette reads one cycle later.
     pix_t<=pix_t==PIXEL_XCLK-1 ? 4'd0 : pix_t+1'b1;
     case(pix_t)
      // left: y = 101 - rotated column; right: y = rotated column
      0: begin ya<=y160(rot_left ? 7'd101-ca : ca); yb<=y160(rot_left ? 7'd101-ca_b : ca_b); end
      1: rd_rot<={dbuf,ya+{6'd0,xa}};
      2: rd_rot<={dbuf,ya+{6'd0,xb}};
      3: begin rd_rot<={dbuf,yb+{6'd0,xa}}; pal_addr<={5'b11000,dbuf,q_pen}; end
      4: begin rd_rot<={dbuf,yb+{6'd0,xb}}; pal_addr<={5'b11000,dbuf,q_pen}; end
      5: begin c2<=pal_q; pal_addr<={5'b11000,dbuf,q_pen}; end
      6: begin m0<={vmix(c2[11:8],pal_q[11:8],wr_q),vmix(c2[7:4],pal_q[7:4],wr_q),vmix(c2[3:0],pal_q[3:0],wr_q)};
          pal_addr<={5'b11000,dbuf,q_pen}; end
      7: c2<=pal_q;
      // 8: pal_q = tap 3 -> m_now
      8: begin
       event_valid<=1;
       if(!live) event_data<={3'b0,have_picture,1'b0,15'd0};
       else event_data<={3'b0,1'b0,1'b0,rot_visible ? rgb_rot : 15'd0};
       if(rot_visible) begin
        if(cj==4'd8) begin cj<=0; ca<=ca+7'd2; end else begin cj<=cj+1'b1; ca<=ca+1'b1; end
       end
      end
      PIXEL_XCLK-1: col<=col+1'b1;
      default: ;
     endcase
    end else if(row_t>=8 && col<160) begin
     pix_t<=pix_t==PIXEL_XCLK-1 ? 4'd0 : pix_t+1'b1;
     case(pix_t)
      0: rd_index<=pen_index(lynx_slot,col);
      // 1: pen RAM read (row k)
      2: begin pal_addr<={lynx_slot,q_pen}; rd_index<=pen_index(lynx_slot_b,col); end
      // 3: palette read (row k), pen RAM read (row k+1)
      4: begin color_a<=pal_q; pal_addr<={lynx_slot_b,q_pen}; weight_q<=weight_a; end
      // 5: palette read (row k+1)
      6: begin
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
     if(acc>=6'd14) begin acc<=acc-6'd14; k<=k+1'b1; end // acc+34 wraps past 48
     else acc<=acc+6'd34;
     if(rj==4'd8) begin rj<=0; ra<=ra+8'd2; end else begin rj<=rj+1'b1; ra<=ra+1'b1; end
    end
   end
  end
 end

 `ifdef LYNX_BRIDGE_CHECK
 // Simulation: every row a live panel frame shows must have been written in
 // the Lynx frame it started in, and not yet overwritten by the next one.
 integer lynx_frame=0, panel_frame=0, cur_y=-1, cur_x=0, bad_reads=0;
 always @(posedge xclk) begin
  if(pen_we) begin
   if(first_pixel) lynx_frame<=lynx_frame+1;
   cur_y<=pen_y; cur_x<=pen_x;
  end
  if(!busy && enable && start_live && !paused) panel_frame<=lynx_frame;
  if(paused) panel_frame<=-2; // a frame cut by a pause or save state is not checked
  if(busy && live && !rot_f && panel_frame>=0 && lynx_row && row_t>=8 && col<160 && (pix_t==0 || (pix_t==2 && weight_a!=4))) begin : chk
   integer ky; reg ok;
   ky=pix_t==0 ? lynx_k : lynx_k+1;
   if(lynx_frame==panel_frame) ok=(ky<cur_y || (ky==cur_y && col<=cur_x)) && cur_y-ky<48;
   else if(lynx_frame==panel_frame+1) ok=cur_y<(ky%48) && ky<102;
   else ok=0;
   if(!ok) begin
    if(bad_reads<10) $display("lynx_video_bridge: row %0d col %0d (panel row %0d) not ready: lynx y=%0d x=%0d frames %0d/%0d",
     ky,col,row,cur_y,cur_x,lynx_frame,panel_frame);
    bad_reads<=bad_reads+1;
   end
  end
 end
 `endif

 wire take;wire [19:0] value;
 // At most one event per 3 hclk against one read per hclk: 8 entries.
 gg_async_fifo #(.ABITS(3)) fifo(.wclk(xclk),.wrst(xreset),.wvalid(event_valid),.wdata(event_data),
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
