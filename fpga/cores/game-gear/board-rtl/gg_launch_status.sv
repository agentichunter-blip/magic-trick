// GG-specific launch telemetry. No GB boot-ROM flags are fabricated.
module gg_launch_status(input hclk,gclk,reset_h,reset_g,enable_h,enable_g,
 input core_reset_h,frame_valid_h, output ready_g,reset_active_g);
 reg ready_h=0,armed_h=0;
 always @(posedge hclk or posedge reset_h) begin
  if(reset_h) begin ready_h<=0;armed_h<=0;end
  else if(!enable_h || core_reset_h) begin ready_h<=0;armed_h<=0;end
  else begin
   if(!frame_valid_h) armed_h<=1;
   if(armed_h && frame_valid_h) ready_h<=1;
  end
 end
 (* ASYNC_REG="TRUE" *) reg [1:0] ready_sync=0,reset_sync=3;
 always @(posedge gclk or posedge reset_g) begin
  if(reset_g) begin ready_sync<=0;reset_sync<=3;end
  else begin ready_sync<={ready_sync[0],ready_h};reset_sync<={reset_sync[0],core_reset_h};end
 end
 assign ready_g=enable_g && ready_sync[1] && !reset_sync[1];
 assign reset_active_g=enable_g && reset_sync[1];
endmodule
