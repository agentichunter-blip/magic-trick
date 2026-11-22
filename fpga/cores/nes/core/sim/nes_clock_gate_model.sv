// Behavioural model of rtl/nes_clock_gate.sv: ce is a synchronously cleared flip-flop
// (Gowin DFFR); the DCE latches it while the input clock is low and outputs the input clock
// ANDed with the latched enable (prim_sim.v).
module nes_clock_gate(input clk_in, input go, input clear, output reg ce = 1'b0, output clk_out);
	always @(posedge clk_in) ce <= clear ? 1'b0 : go;
	reg ce_l = 1'b0;
	always @(clk_in or ce) if (!clk_in) ce_l = ce;
	assign clk_out = clk_in & ce_l;
endmodule
