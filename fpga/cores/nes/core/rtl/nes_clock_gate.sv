// NES master-clock gate for the Gowin build. ce is a flip-flop on clk_in with a synchronous
// reset (DFFR): ce <= clear ? 0 : go. go comes from NES-domain logic that may still be
// settling while clear is high (two-cycle paths, nes_board_core.sv), so the reset pin, not
// LUT logic, keeps such a value out. The GW5A DCE primitive (glitch-free dynamic clock
// enable, CLKOUT = CLKIN & CE latched while CLKIN is low) then gates the clock: an xclk edge
// with ce = 1 is also a NES edge.
// Simulation uses sim/nes_clock_gate_model.sv (same function).
module nes_clock_gate(input clk_in, input go, input clear, output ce, output clk_out);
	DFFR ce_ff(.Q(ce), .D(go), .CLK(clk_in), .RESET(clear));
	DCE dce(.CLKIN(clk_in), .CE(ce), .CLKOUT(clk_out));
endmodule
