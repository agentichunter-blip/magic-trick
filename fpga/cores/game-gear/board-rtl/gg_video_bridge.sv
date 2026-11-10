// Translate the core's cropped stream to the existing GB framebuffer protocol.
// row-start -> mode[1] high, exactly 160 pixel strobes, row-end -> mode[1] low.
// The original panel scanout/PSRAM framebuffer remains the display-clock owner.
module gg_video_bridge(
 input wire xclk,xreset,input wire hclk,hreset,input wire enable,
 input wire pixel,input wire [8:0] x,y,input wire [11:0] color,
 output wire overflow,output reg lcd_pixel,output reg [14:0] lcd_data,
 output reg [1:0] lcd_mode,output reg lcd_on,output reg lcd_vsync);
 // VDP selects Game Gear content at x=49..208. The one-pixel
 // coordinate delay maps that to framebuffer columns 0..159.
 reg [8:0] px0,py0;
 reg event_valid;
 reg [19:0] event_data;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin px0<=0;py0<=0;event_valid<=0;event_data<=0;end
  else begin
   event_valid<=0;
   if(pixel) begin
    px0<=x;py0<=y;
    if(enable && py0>=24 && py0<168) begin
     // Type 01=start; bit 15 marks first row. 10=end. 00=pixel.
     if(px0==47) begin event_valid<=1;event_data<={2'b01,2'b0,py0==24,15'd0};end
     else if(px0>=48 && px0<208) begin
      event_valid<=1;
      event_data<={5'b0,color[11:8],color[11],color[7:4],color[7],color[3:0],color[3]};
     end else if(px0==208) begin event_valid<=1;event_data<={2'b10,18'd0};end
    end
   end
  end
 end
 wire take;wire [19:0] value;
 gg_async_fifo fifo(.wclk(xclk),.wrst(xreset),.wvalid(event_valid),.wdata(event_data),
  .full(),.overflow(overflow),.rclk(hclk),.rrst(hreset),.rvalid(take),.rdata(value));
 always @(posedge hclk or posedge hreset) begin
  if(hreset) begin lcd_pixel<=0;lcd_data<=0;lcd_mode<=0;lcd_on<=0;lcd_vsync<=0;end
  else begin
   lcd_pixel<=0;
   if(take) case(value[19:18])
    2'b01: begin lcd_mode<=2'b11;lcd_on<=1;lcd_vsync<=value[15];end
    2'b10: begin lcd_mode<=0;end // Keep first-row VSYNC through line flush.
    2'b00: begin lcd_data<=value[14:0];lcd_pixel<=1;end
    default: begin lcd_pixel<=0;end
   endcase
  end
 end
endmodule
