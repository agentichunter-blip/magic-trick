// Atari Lynx core: the same ~2 ms debounce as esp32t's button_debouncer
// (2^14 gclk at 8.39 MHz), counted in 64 steps of a shared 256-cycle tick
// (button_debounce_tick) instead of a 15-bit counter per button.
module button_debounce_tick(input clk, output reg tick);
   reg [7:0] div = 8'd0;
   always @(posedge clk) begin
      div  <= div + 1'b1;
      tick <= div == 8'd0;
   end
endmodule

module button_debouncer
(
   input      clk,
   input      tick,
   input      unfiltered,
   output reg filtered
);
   reg [6:0] count = 7'd0;
   reg [2:0] input_sampling;
   always @(posedge clk) begin
      input_sampling <= {input_sampling[1:0], unfiltered};
      if (input_sampling[2] == filtered)
         count <= 7'd0;
      else if (count[6]) begin
         filtered <= input_sampling[2];
         count <= 7'd0;
      end else if (tick)
         count <= count + 1'b1;
   end
endmodule
