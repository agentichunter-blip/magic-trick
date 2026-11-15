// Chromatic: block RAM for the Lynx core (replaces Intel altsyncram and
// lynx-rtl's former VHDL dpram, which Gowin mapped to an unsupported
// read-before-write mode). Port A steps only on its clock enable. A write
// leaves the port's output unchanged (Gowin "normal" mode); the Lynx never
// uses read data from a write cycle.
//
// single=1 (the 64 KiB RAM): one physical port. Port B (BS93 preload and
// save states) is used only while b_enable is high, which the core asserts
// only while it is held in reset or asleep for a save state, i.e. while
// clken_a is low; q_a and q_b are the same output register. Half the
// connections of a true dual-port RAM across 32 block RAMs (routing).
module lynx_dpram #(parameter integer addr_width=8, parameter integer data_width=8,
                    parameter integer single=0)(
 input clock_a, input clken_a, input [addr_width-1:0] address_a,
 input [data_width-1:0] data_a, input wren_a, output [data_width-1:0] q_a,
 input clock_b, input b_enable, input [addr_width-1:0] address_b,
 input [data_width-1:0] data_b, input wren_b, output [data_width-1:0] q_b);
 reg [data_width-1:0] mem[0:(1<<addr_width)-1];
 generate if(single) begin : g_single
  reg [data_width-1:0] q;
  wire use_b=!clken_a && b_enable;
  wire [addr_width-1:0] address=use_b ? address_b : address_a;
  wire [data_width-1:0] data=use_b ? data_b : data_a;
  wire wren=use_b ? wren_b : wren_a;
  always @(posedge clock_a) if(clken_a || use_b) begin
   if(wren) mem[address]<=data; else q<=mem[address];
  end
  assign q_a=q; assign q_b=q;
 end else begin : g_dual
  reg [data_width-1:0] qa, qb;
  always @(posedge clock_a) if(clken_a) begin
   if(wren_a) mem[address_a]<=data_a; else qa<=mem[address_a];
  end
  always @(posedge clock_b) begin
   if(wren_b) mem[address_b]<=data_b; else qb<=mem[address_b];
  end
  assign q_a=qa; assign q_b=qb;
 end endgenerate
endmodule
