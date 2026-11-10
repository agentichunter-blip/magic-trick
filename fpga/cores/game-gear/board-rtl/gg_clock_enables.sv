// Preserve the board PLL. xclk = 2 * 33.554432 MHz; emulate the SMS
// 53.693175 MHz enable schedule with bounded one-xclk quantization.
module gg_clock_enables #(parameter integer HOST_HZ=67108864,
 parameter integer MASTER_HZ=53693175)(
 input wire clk,reset_n, output reg cpu, vdp, pixel, sprite);
 reg [26:0] accumulator;
 reg [4:0] phase;
 wire [27:0] sum={1'b0,accumulator}+28'(MASTER_HZ);
 always @(negedge clk or negedge reset_n) begin
  if(!reset_n) begin accumulator<=0;phase<=0;cpu<=0;vdp<=0;pixel<=0;sprite<=0;end
  else begin
   cpu<=0;vdp<=0;pixel<=0;sprite<=0;
   if(sum>=HOST_HZ) begin
    accumulator<=27'(sum-HOST_HZ);
    phase<=phase==29 ? 0 : phase+1'b1;
    sprite<=phase[0];
    cpu<=phase==24 || phase==9;
    vdp<=phase==29 || phase==24 || phase==19 || phase==14 || phase==9 || phase==4;
    pixel<=phase==29 || phase==19 || phase==9;
   end else accumulator<=sum[26:0];
  end
 end
endmodule
