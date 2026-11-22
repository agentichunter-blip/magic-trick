// Inferred stand-in for the shell's gowin_dpb_snapshot (1 KiB MCU block buffer, 512 x 16,
// write on clk, read on qclk) for sim/ and the standalone synthesis check. The Chromatic
// shell already has the real module (used by the Lynx/Game Gear engines): leave this file out
// of the integrated build.
module gowin_dpb_snapshot(input write_clock, write_enable, input [8:0] write_address, input [15:0] write_data,
	input read_clock, input [8:0] read_address, output reg [15:0] read_data);
	reg [15:0] memory [0:511];
	always @(posedge write_clock) if (write_enable) memory[write_address] <= write_data;
	always @(posedge read_clock) read_data <= memory[read_address];
endmodule
