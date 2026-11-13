// Translate the core's cropped stream to the existing GB framebuffer protocol.
// row-start -> mode[1] high, exactly 160 pixel strobes, row-end -> mode[1] low.
// The original panel scanout/PSRAM framebuffer remains the display-clock owner.
// While a save state freezes the game (hold), the panel still needs frames: a
// local timing generator keeps rows coming, and their pixels are flagged
// lcd_repeat so the display shows the previous frame from the framebuffer.
// resume_ok marks the generator's vertical blank, where the frozen game
// (stopped in its own vertical blank) can continue without a partial frame.
module gg_video_bridge(
 input wire xclk,xreset,input wire hclk,hreset,input wire enable,
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
 assign resume_ok=hold && sy>=176 && sy<=250;
 wire src_pixel=hold ? spixel : pixel;
 wire [8:0] src_x=hold ? sx : x, src_y=hold ? sy : y;
 // VDP selects Game Gear content at x=49..208. The one-pixel
 // coordinate delay maps that to framebuffer columns 0..159.
 reg [8:0] px0,py0;
 reg event_valid;
 reg [19:0] event_data;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin px0<=0;py0<=0;event_valid<=0;event_data<=0;end
  else begin
   event_valid<=0;
   if(src_pixel) begin
    px0<=src_x;py0<=src_y;
    if(enable && py0>=24 && py0<168) begin
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
