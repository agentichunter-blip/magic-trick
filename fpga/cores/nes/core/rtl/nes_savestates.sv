// MiSTer NES savestates.vhd, ported to SystemVerilog with the region table reduced to the
// memories that exist on the Chromatic (everything else is identical, state by state).
//
// Image (byte offsets inside the staged image; the 64-bit bus addresses are 32-bit-word
// addresses, +2 per beat):
//   0x0000  header: bytes 0-3 save counter (header_amount), 4-7 STATESIZE = 0x1500 words
//   0x0008  64 internal registers x 8 bytes (savestate bus index 0..63)
//   0x0208  OAM          256 B   (type 0, inside the PPU)
//   0x0308  CHR RAM     8 KiB    (type 2; also MMC3 four-screen / TQROM RAM; zeros otherwise)
//   0x2308  CIRAM       2 KiB    (type 3)
//   0x2B08  CPU RAM     2 KiB    (type 4)
//   0x3308  PRG RAM     8 KiB    (type 5, battery RAM)
//   0x5308  end (21,256 bytes); STATESIZE is rounded up to 21 x 1 KiB = 21,504 bytes.
// Upstream's type 1 (8 KiB mapper-internal RAM: MMC5/N163/FDS) is dropped, and types 2/5
// shrink from 1 MiB / 256 KiB to 8 KiB each. Save_RAMType keeps upstream's numbering.
module savestates (
	input             clk,
	input             reset_in,
	output reg        reset_ss = 1'b0,
	output reg        reset_delay = 1'b0,
	output reg        load_done = 1'b0,
	input             increaseSSHeaderCount,
	input             save,
	input             load,
	input      [31:0] savestate_address,
	output            savestate_busy,
	input             paused,
	output reg [63:0] BUS_Din = 64'd0,
	output reg  [9:0] BUS_Adr = 10'd0,
	output reg        BUS_wren = 1'b0,
	output reg        BUS_rst = 1'b0,
	input      [63:0] BUS_Dout,
	output reg        loading_savestate = 1'b0,
	output reg        saving_savestate = 1'b0,
	output reg        sleep_savestate = 1'b0,
	output reg [24:0] Save_RAMAddr = 25'd0,
	output reg        Save_RAMRdEn = 1'b0,
	output reg        Save_RAMWrEn = 1'b0,
	output reg  [7:0] Save_RAMWriteData = 8'd0,
	input       [7:0] Save_RAMReadData,
	output reg  [2:0] Save_RAMType = 3'd0,
	output reg [63:0] bus_out_Din = 64'd0,
	input      [63:0] bus_out_Dout,
	output reg [25:0] bus_out_Adr = 26'd0,
	output reg        bus_out_rnw = 1'b0,
	output reg        bus_out_ena = 1'b0,
	output reg  [7:0] bus_out_be = 8'd0,
	input             bus_out_done
);
	localparam [31:0] STATESIZE = 32'd5376;   // 32-bit words (21 KiB)
	localparam integer SETTLECOUNT = 100;
	localparam integer HEADERCOUNT = 2;
	localparam integer INTERNALSCOUNT = 64;
	localparam integer SAVETYPESCOUNT = 6;

	// region table: offset / size per type (size 0 = not present)
	function automatic [24:0] type_offset(input [2:0] ty);
		case (ty)
			3'd2: type_offset = 25'h200000;  // CHR RAM
			3'd3: type_offset = 25'h3A0000;  // CIRAM
			3'd4: type_offset = 25'h380000;  // CPU RAM
			3'd5: type_offset = 25'h3C0000;  // PRG RAM
			default: type_offset = 25'h000000;
		endcase
	endfunction
	function automatic [13:0] type_size(input [2:0] ty);
		case (ty)
			3'd0: type_size = 14'd256;
			3'd2: type_size = 14'd8192;
			3'd3: type_size = 14'd2048;
			3'd4: type_size = 14'd2048;
			3'd5: type_size = 14'd8192;
			default: type_size = 14'd0;
		endcase
	endfunction

	localparam [3:0] IDLE = 4'd0, SAVE_WAITSETTLE = 4'd1, SAVEINTERNALS_WAIT = 4'd2,
		SAVEINTERNALS_WRITE = 4'd3, SAVEMEMORY_NEXT = 4'd4, SAVEMEMORY_READ = 4'd5,
		SAVEMEMORY_WRITE = 4'd6, SAVESIZEAMOUNT = 4'd7, LOAD_WAITSETTLE = 4'd8,
		LOAD_HEADERAMOUNTCHECK = 4'd9, LOADINTERNALS_READ = 4'd10, LOADINTERNALS_WRITE = 4'd11,
		LOADMEMORY_RESET = 4'd12, LOADMEMORY_NEXT = 4'd13, LOADMEMORY_READ = 4'd14,
		LOADMEMORY_WRITE = 4'd15;
	reg [3:0] state = IDLE;
	reg [2:0] savetype_counter = 3'd0;
	reg [14:0] count = 15'd0;
	reg [14:0] maxcount = 15'd0;
	reg [6:0] settle = 7'd0;
	reg [2:0] bytecounter = 3'd0;
	reg [24:0] RAMAddrNext = 25'd0;
	reg [2:0] memory_slow = 3'd0;
	reg [31:0] header_amount = 32'd1;

	assign savestate_busy = state != IDLE;
	wire skip_type = type_size(savetype_counter) == 14'd0;

	always @(posedge clk) begin
		bus_out_ena <= 1'b0;
		BUS_wren    <= 1'b0;
		BUS_rst     <= 1'b0;
		reset_ss    <= 1'b0;
		reset_delay <= 1'b0;
		load_done   <= 1'b0;
		bus_out_be  <= 8'hFF;
		memory_slow <= memory_slow + 3'd1;
		if (memory_slow == 3'd6) begin
			Save_RAMRdEn <= 1'b0;
			Save_RAMWrEn <= 1'b0;
		end
		case (state)
		IDLE: begin
			savetype_counter <= 3'd0;
			if (reset_in) begin
				reset_delay <= 1'b1;
				reset_ss    <= 1'b1;
				BUS_rst     <= 1'b1;
			end else if (save) begin
				state <= SAVE_WAITSETTLE;
				sleep_savestate <= 1'b1;
				header_amount <= header_amount + 32'd1;
			end else if (load) begin
				state <= LOAD_WAITSETTLE;
				settle <= 7'd0;
				sleep_savestate <= 1'b1;
			end
		end
		// ---------------- SAVE
		SAVE_WAITSETTLE: begin
			if (!paused) settle <= 7'd0;
			else if (settle < SETTLECOUNT) settle <= settle + 7'd1;
			else begin
				state <= SAVEINTERNALS_WAIT;
				bus_out_Adr <= savestate_address[25:0] + HEADERCOUNT;
				bus_out_rnw <= 1'b0;
				BUS_Adr <= 10'd0;
				count <= 15'd1;
				saving_savestate <= 1'b1;
			end
		end
		SAVEINTERNALS_WAIT: begin
			bus_out_Din <= BUS_Dout;
			bus_out_ena <= 1'b1;
			state <= SAVEINTERNALS_WRITE;
		end
		SAVEINTERNALS_WRITE: if (bus_out_done) begin
			bus_out_Adr <= bus_out_Adr + 26'd2;
			if (count < INTERNALSCOUNT) begin
				state <= SAVEINTERNALS_WAIT;
				count <= count + 15'd1;
				BUS_Adr <= BUS_Adr + 10'd1;
			end else begin
				state <= SAVEMEMORY_NEXT;
				count <= 15'd8;
			end
		end
		SAVEMEMORY_NEXT: begin
			if (savetype_counter < SAVETYPESCOUNT && skip_type) begin
				savetype_counter <= savetype_counter + 3'd1;
			end else if (savetype_counter < SAVETYPESCOUNT) begin
				state <= SAVEMEMORY_READ;
				bytecounter <= 3'd0;
				count <= 15'd8;
				maxcount <= {1'b0, type_size(savetype_counter)};
				Save_RAMAddr <= type_offset(savetype_counter);
				Save_RAMRdEn <= 1'b1;
				memory_slow <= 3'd0;
				Save_RAMType <= savetype_counter;
			end else begin
				state <= SAVESIZEAMOUNT;
				bus_out_Adr <= savestate_address[25:0];
				bus_out_Din <= {STATESIZE, header_amount};
				bus_out_ena <= 1'b1;
				if (!increaseSSHeaderCount) bus_out_be <= 8'hF0;
			end
		end
		SAVEMEMORY_READ: if (memory_slow == 3'd7) begin
			bus_out_Din[bytecounter*8 +: 8] <= Save_RAMReadData;
			if (bytecounter < 3'd7) begin
				bytecounter <= bytecounter + 3'd1;
				Save_RAMAddr <= Save_RAMAddr + 25'd1;
				Save_RAMRdEn <= 1'b1;
			end else begin
				state <= SAVEMEMORY_WRITE;
				bus_out_ena <= 1'b1;
			end
		end
		SAVEMEMORY_WRITE: if (bus_out_done) begin
			bus_out_Adr <= bus_out_Adr + 26'd2;
			if (count < maxcount) begin
				state <= SAVEMEMORY_READ;
				bytecounter <= 3'd0;
				count <= count + 15'd8;
				Save_RAMAddr <= Save_RAMAddr + 25'd1;
				Save_RAMRdEn <= 1'b1;
				memory_slow <= 3'd0;
			end else begin
				savetype_counter <= savetype_counter + 3'd1;
				state <= SAVEMEMORY_NEXT;
			end
		end
		SAVESIZEAMOUNT: if (bus_out_done) begin
			state <= IDLE;
			saving_savestate <= 1'b0;
			sleep_savestate <= 1'b0;
		end
		// ---------------- LOAD
		LOAD_WAITSETTLE: begin
			if (!paused) settle <= 7'd0;
			else if (settle < SETTLECOUNT) settle <= settle + 7'd1;
			else begin
				state <= LOAD_HEADERAMOUNTCHECK;
				bus_out_Adr <= savestate_address[25:0];
				bus_out_rnw <= 1'b1;
				bus_out_ena <= 1'b1;
			end
		end
		LOAD_HEADERAMOUNTCHECK: if (bus_out_done) begin
			if (bus_out_Dout[63:32] == STATESIZE) begin
				header_amount <= bus_out_Dout[31:0];
				state <= LOADINTERNALS_READ;
				bus_out_Adr <= savestate_address[25:0] + HEADERCOUNT;
				bus_out_ena <= 1'b1;
				BUS_Adr <= 10'd0;
				count <= 15'd1;
				loading_savestate <= 1'b1;
			end else begin
				state <= IDLE;
				sleep_savestate <= 1'b0;
			end
		end
		LOADINTERNALS_READ: if (bus_out_done) begin
			state <= LOADINTERNALS_WRITE;
			BUS_Din <= bus_out_Dout;
			BUS_wren <= 1'b1;
		end
		LOADINTERNALS_WRITE: begin
			bus_out_Adr <= bus_out_Adr + 26'd2;
			if (count < INTERNALSCOUNT) begin
				state <= LOADINTERNALS_READ;
				count <= count + 15'd1;
				bus_out_ena <= 1'b1;
				BUS_Adr <= BUS_Adr + 10'd1;
			end else state <= LOADMEMORY_RESET;
		end
		LOADMEMORY_RESET: begin
			reset_ss <= 1'b1;
			state <= LOADMEMORY_NEXT;
			count <= 15'd8;
		end
		LOADMEMORY_NEXT: if (memory_slow == 3'd7) begin
			if (savetype_counter < SAVETYPESCOUNT && skip_type) begin
				savetype_counter <= savetype_counter + 3'd1;
			end else if (savetype_counter < SAVETYPESCOUNT) begin
				state <= LOADMEMORY_READ;
				count <= 15'd8;
				maxcount <= {1'b0, type_size(savetype_counter)};
				RAMAddrNext <= type_offset(savetype_counter);
				bytecounter <= 3'd0;
				bus_out_ena <= 1'b1;
				Save_RAMType <= savetype_counter;
			end else begin
				state <= IDLE;
				loading_savestate <= 1'b0;
				sleep_savestate <= 1'b0;
				load_done <= 1'b1;
			end
		end
		LOADMEMORY_READ: if (bus_out_done) state <= LOADMEMORY_WRITE;
		LOADMEMORY_WRITE: if (memory_slow == 3'd7) begin
			RAMAddrNext <= RAMAddrNext + 25'd1;
			Save_RAMAddr <= RAMAddrNext;
			Save_RAMWrEn <= 1'b1;
			Save_RAMWriteData <= bus_out_Dout[bytecounter*8 +: 8];
			memory_slow <= 3'd0;
			if (bytecounter < 3'd7) bytecounter <= bytecounter + 3'd1;
			else begin
				bus_out_Adr <= bus_out_Adr + 26'd2;
				if (count < maxcount) begin
					state <= LOADMEMORY_READ;
					count <= count + 15'd8;
					bytecounter <= 3'd0;
					bus_out_ena <= 1'b1;
				end else begin
					savetype_counter <= savetype_counter + 3'd1;
					state <= LOADMEMORY_NEXT;
				end
			end
		end
		endcase
	end
endmodule
