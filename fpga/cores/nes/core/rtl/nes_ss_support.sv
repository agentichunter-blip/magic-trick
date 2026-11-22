// SystemVerilog versions of MiSTer's VHDL savestate helpers used by nes.v and the
// mappers (bus_savestates.vhd: eReg_SavestateV; statemanager.vhd without rewind).
// Same behaviour; written in SV so the only VHDL left in the core is T65 (GHDL netlist).

// One 64-bit savestate register on the 10-bit-address savestate bus.
module eReg_SavestateV #(parameter [9:0] Adr = 10'd0, parameter [63:0] def = 64'd0) (
	input         clk,
	input  [63:0] BUS_Din,
	input   [9:0] BUS_Adr,
	input         BUS_wren,
	input         BUS_rst,
	output [63:0] BUS_Dout,
	input  [63:0] Din,
	output [63:0] Dout
);
	reg [63:0] buffer = def;
	always @(posedge clk) begin
		if (BUS_rst) buffer <= def;
		else if (BUS_Adr == Adr && BUS_wren) buffer <= BUS_Din;
	end
	assign Dout = buffer;
	assign BUS_Dout = (BUS_Adr == Adr) ? Din : 64'd0;
endmodule

// statemanager.vhd reduced to the save/load request path (no rewind, no slots: the
// Chromatic keeps one staged image, at savestate address 0).
module statemanager #(parameter integer Softmap_SaveState_ADDR = 0, parameter integer Softmap_Rewind_ADDR = 0) (
	input         clk,
	input         reset,
	input         rewind_on,
	input         rewind_active,
	input   [1:0] savestate_number,
	input         save,
	input         load,
	output reg    sleep_rewind = 1'b0,
	input         vsync,
	output reg    request_savestate = 1'b0,
	output reg    request_loadstate = 1'b0,
	output [31:0] request_address,
	input         request_busy
);
	reg save_1 = 0, load_1 = 0, save_buffer = 0, load_buffer = 0;
	assign request_address = 32'd0;
	always @(posedge clk) begin
		request_savestate <= 1'b0;
		request_loadstate <= 1'b0;
		sleep_rewind <= 1'b0;
		save_1 <= save;
		if (save && !save_1) save_buffer <= 1'b1;
		load_1 <= load;
		if (load && !load_1) load_buffer <= 1'b1;
		if (!reset && !request_busy) begin
			if (save_buffer) begin
				request_savestate <= 1'b1;
				save_buffer <= 1'b0;
			end else if (load_buffer) begin
				request_loadstate <= 1'b1;
				load_buffer <= 1'b0;
			end
		end
	end
endmodule
