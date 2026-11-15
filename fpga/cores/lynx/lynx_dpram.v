// Chromatic: dual-port block RAM for the Lynx core (replaces Intel altsyncram
// and lynx-rtl's former VHDL dpram, which Gowin mapped to an unsupported
// read-before-write mode). Port A steps only on its clock enable. A write
// leaves that port's output unchanged (Gowin "normal" mode); the Lynx never
// uses read data from a write cycle.
module lynx_dpram #(parameter integer addr_width=8, parameter integer data_width=8)(
 input clock_a, input clken_a, input [addr_width-1:0] address_a,
 input [data_width-1:0] data_a, input wren_a, output reg [data_width-1:0] q_a,
 input clock_b, input [addr_width-1:0] address_b,
 input [data_width-1:0] data_b, input wren_b, output reg [data_width-1:0] q_b);
 reg [data_width-1:0] mem[0:(1<<addr_width)-1];
 always @(posedge clock_a) if(clken_a) begin
  if(wren_a) mem[address_a]<=data_a; else q_a<=mem[address_a];
 end
 always @(posedge clock_b) begin
  if(wren_b) mem[address_b]<=data_b; else q_b<=mem[address_b];
 end
endmodule
