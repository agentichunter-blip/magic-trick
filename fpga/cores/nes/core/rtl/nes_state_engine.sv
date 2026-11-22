// NES save states and battery-RAM transfer on xclk. Same MCU command interface as
// lynx_state_engine.sv (freeze/release/load/block toggles, block number + sequence, block
// ready + sequence, 1 KiB qaddress/qdata buffer, busy/fault/frozen/loaded), plus the Game
// Gear engine's battery import (cart_import/imported) and live cartridge-RAM block reads.
//
// The state itself is MiSTer's (rtl/nes_savestates.sv): 21 blocks of 1 KiB, staged in PSRAM
// at ROM-window offset STAGE (0x3F0000 = PSRAM 0x410000 with the 0x020000 window base).
//   FREEZE  the core pauses (vblank, instruction boundary), writes its image to the staging
//           area over the 64-bit SAVE bus (8-byte PSRAM writes), and stays paused ("frozen").
//   BLOCK n 0..20: copies staging block n into the buffer the MCU reads (needs no freeze,
//           but only frozen data is consistent).
//           64..71: copies 1 KiB of LIVE battery RAM (PRG RAM $6000 + (n-64)*1024) through
//           the second RAM port, any time, without pausing the game. (Dirty tracking:
//           nes_board_core's cart_dirty_clear input.) Anything else reads zeros.
//   LOAD    (frozen) the MCU staged an image at STAGE; the core checks the size word
//           (header bytes 4-7 = 0x00001500), loads it and stays frozen. A refused image
//           sets fault and leaves the game unchanged.
//   RELEASE the game continues (from the saved or the loaded state) as soon as
//           video_resume_ok is high.
//   BATTERY with cart_import high while the core is in reset, the 8 KiB staged at
//           IMPORT (0x3F8000) are copied into PRG RAM before the first instruction
//           (boot_hold keeps the NES in reset until then); imported goes high.
// The NES runs on the gated clock; its SAVE bus strobes are one NES clock long, so they are
// sampled with nes_edge (the xclk edge that is also a NES edge), and ss_done is held until a
// NES edge has seen it.
module nes_state_engine #(parameter [21:0] STAGE = 22'h3F0000, parameter [21:0] IMPORT = 22'h3F8000,
                          parameter integer BLOCKS = 21) (
	input             clk, reset,
	input             nes_edge,
	input             bus_hold,         // shell pause: no PSRAM traffic now, timeouts stopped
	input             video_resume_ok,  // release only while this is high (tie to 1 if unused)
	input             freeze_toggle, release_toggle, load_toggle, block_toggle,
	input       [6:0] block_number, input [15:0] block_sequence,
	output reg        block_ready = 1'b0, output reg [15:0] block_ready_sequence = 16'd0,
	output            busy, output reg fault = 1'b0, output reg frozen = 1'b0, output reg loaded = 1'b0,
	output            hold_core,        // keep the NES paused (save/load in progress or frozen)
	// NES savestate request + 64-bit bus (NES clock domain signals)
	output reg        save_req = 1'b0, output reg load_req = 1'b0,
	input             state_loaded, input state_busy,
	input      [63:0] ss_din, output reg [63:0] ss_dout = 64'd0, input [25:0] ss_adr,
	input             ss_rnw, input ss_ena, output reg ss_done = 1'b0,
	// PRG cache read port while owns_rom (the NES is paused or in reset then)
	output            owns_rom, output reg cache_invalidate = 1'b0,
	output reg        rom_read = 1'b0, output reg [21:0] rom_address = 22'd0,
	input       [7:0] rom_data, input rom_hit, input rom_fault, input rom_idle,
	// PSRAM write port (8-byte bursts) while owns_bus
	output reg        owns_bus = 1'b0, output reg x_request = 1'b0, output reg [21:0] x_address = 22'd0,
	input             x_grant,          // the arbiter started the requested write
	output     [15:0] x_write_data, input x_write_next, input x_done,
	// battery RAM (PRG RAM second port)
	input             cart_import, input loader_ready, output reg boot_hold = 1'b0,
	output reg        imported = 1'b0,
	output reg [12:0] cart_addr = 13'd0, output reg cart_we = 1'b0, output reg [7:0] cart_wdata = 8'd0,
	input       [7:0] cart_q,
	// MCU block buffer
	input             qclk, input [9:0] qaddress, output [7:0] qdata
);
	localparam [3:0] IDLE = 0, SAVE_START = 1, RUN = 2, BUS_WRITE_WAIT = 3, BUS_WRITE = 4,
		BUS_READ = 5, BLOCK_READ = 6, LOAD_START = 7, CART_READ = 8, IMPORT_READ = 9;
	reg [3:0] state = IDLE;
	reg loading = 1'b0, importing = 1'b0, release_pending = 1'b0;
	assign busy = state != IDLE;
	assign hold_core = frozen || (state != IDLE && state != BLOCK_READ && state != CART_READ);
	assign owns_rom = state == BUS_READ || state == BLOCK_READ || state == IMPORT_READ;

	(* ASYNC_REG = "TRUE" *) reg [3:0] t_meta = 4'd0, t_sync = 4'd0;
	(* ASYNC_REG = "TRUE" *) reg [1:0] import_sync = 2'd0;
	reg [3:0] t_seen = 4'd0;
	always @(posedge clk) begin
		t_meta <= {block_toggle, load_toggle, release_toggle, freeze_toggle};
		t_sync <= t_meta;
		import_sync <= {import_sync[0], cart_import};
	end
	wire [3:0] t_edge = t_sync ^ t_seen;

	// 1 KiB buffer: 512 x 16-bit words, written here, read by the MCU interface clock.
	reg buffer_write = 1'b0; reg [8:0] buffer_address = 9'd0; reg [15:0] buffer_data = 16'd0;
	wire [15:0] buffer_q; reg q_high = 1'b0;
	gowin_dpb_snapshot buffer(.write_clock(clk), .write_enable(buffer_write),
		.write_address(buffer_address), .write_data(buffer_data),
		.read_clock(qclk), .read_address(qaddress[9:1]), .read_data(buffer_q));
	always @(posedge qclk) q_high <= qaddress[0];
	assign qdata = q_high ? buffer_q[15:8] : buffer_q[7:0];

	reg [25:0] timer = 26'd0;
	reg busy_seen = 1'b0, load_seen = 1'b0, done_wait = 1'b0;
	reg [63:0] bus_data = 64'd0; reg [14:0] bus_byte = 15'd0; reg [2:0] byte_i = 3'd0; reg [1:0] word_i = 2'd0;
	reg [12:0] offset = 13'd0; reg [6:0] block = 7'd0; reg [15:0] request_sequence = 16'd0;
	reg [7:0] low_byte = 8'd0; reg cart_phase = 1'b0;
	assign x_write_data = bus_data[word_i*16 +: 16];
	wire block_valid = block < BLOCKS;
	wire [21:0] block_address = STAGE + {5'd0, block[4:0], 10'd0} + {12'd0, offset[9:0]};
	// The image lies at 32-bit word addresses 0 .. 5375 (savestate address 0).
	wire [14:0] ss_byte = {ss_adr[12:0], 2'b00};

	always @(posedge clk) begin
		buffer_write <= 1'b0; cache_invalidate <= 1'b0;
		cart_we <= 1'b0;
		// ss_done: one NES clock wide
		if (ss_done && nes_edge) ss_done <= 1'b0;
		if (reset) begin
			state <= IDLE; t_seen <= t_sync; fault <= 1'b0; loaded <= 1'b0; frozen <= 1'b0;
			block_ready <= 1'b0; save_req <= 1'b0; load_req <= 1'b0; rom_read <= 1'b0;
			owns_bus <= 1'b0; x_request <= 1'b0; loading <= 1'b0; ss_done <= 1'b0; imported <= 1'b0; release_pending <= 1'b0;
			importing <= 1'b0; boot_hold <= import_sync[1];
		end else begin
			if (state != IDLE && state != BLOCK_READ && state != CART_READ && !bus_hold) timer <= timer + 26'd1;
			case (state)
			IDLE: begin
				rom_read <= 1'b0; timer <= 26'd0; busy_seen <= 1'b0; load_seen <= 1'b0;
				if (boot_hold) begin
					// battery import before the first instruction
					if (loader_ready) begin
						importing <= 1'b1; offset <= 13'd0; cart_phase <= 1'b0;
						cache_invalidate <= 1'b1; state <= IMPORT_READ;
					end
				end else if (release_pending) begin
					// unfreeze once the video path is ready for the game's frames again
					if (!frozen) release_pending <= 1'b0;
					else if (video_resume_ok) begin frozen <= 1'b0; release_pending <= 1'b0; cache_invalidate <= 1'b1; end
				end else if (t_edge[0]) begin
					t_seen[0] <= t_sync[0];
					if (!frozen) begin fault <= 1'b0; loaded <= 1'b0; loading <= 1'b0; save_req <= 1'b1; state <= SAVE_START; end
				end else if (t_edge[1]) begin
					t_seen[1] <= t_sync[1];
					if (frozen) release_pending <= 1'b1;
				end else if (t_edge[3]) begin
					t_seen[3] <= t_sync[3];
					block <= block_number; request_sequence <= block_sequence; block_ready <= 1'b0;
					offset <= 13'd0;
					if (block_number[6:3] == 4'b1000) begin
						cart_phase <= 1'b0; cart_addr <= {block_number[2:0], 10'd0};
						state <= CART_READ;
					end else state <= BLOCK_READ;
				end else if (t_edge[2]) begin
					t_seen[2] <= t_sync[2];
					loaded <= 1'b0; fault <= 1'b0;
					// The MCU staged the payload behind the cache: drop stale lines.
					if (frozen) begin loading <= 1'b1; cache_invalidate <= 1'b1; load_req <= 1'b1; state <= LOAD_START; end
				end
			end
			SAVE_START, LOAD_START: begin
				// statemanager takes the request on a rising edge (NES clock) once not busy.
				if (state_busy) begin save_req <= 1'b0; load_req <= 1'b0; busy_seen <= 1'b1; state <= RUN; end
				else if (timer[25]) begin save_req <= 1'b0; load_req <= 1'b0; fault <= 1'b1; state <= IDLE; end
			end
			RUN: begin
				if (state_loaded) load_seen <= 1'b1;
				if (ss_ena && nes_edge && !ss_done) begin
					bus_byte <= ss_byte; timer <= 26'd0;
					if (ss_rnw) begin byte_i <= 3'd0; state <= BUS_READ; end
					else begin bus_data <= ss_din; word_i <= 2'd0; state <= BUS_WRITE_WAIT; end
				end else if (!state_busy && !ss_done) begin
					// Finished. A load that never reported completion was refused (size
					// word mismatch): the game state is unchanged, still frozen.
					if (loading) begin
						if (load_seen || state_loaded) loaded <= 1'b1; else fault <= 1'b1;
					end
					frozen <= 1'b1;
					cache_invalidate <= 1'b1; state <= IDLE;
				end else if (timer[25]) begin fault <= 1'b1; state <= IDLE; end
			end
			BUS_WRITE_WAIT: if (rom_idle && !bus_hold) begin
				owns_bus <= 1'b1; x_request <= 1'b1; x_address <= STAGE + {7'd0, bus_byte};
				state <= BUS_WRITE;
			end
			BUS_WRITE: begin
				if (x_grant) x_request <= 1'b0;
				if (x_write_next) word_i <= word_i + 2'd1;
				if (x_done) begin owns_bus <= 1'b0; x_request <= 1'b0; ss_done <= 1'b1; state <= RUN; end
				else if (timer[20]) begin owns_bus <= 1'b0; x_request <= 1'b0; fault <= 1'b1; state <= IDLE; end
			end
			BUS_READ: begin
				if (rom_fault) begin rom_read <= 1'b0; fault <= 1'b1; state <= IDLE; end
				else if (!rom_read) begin rom_read <= 1'b1; rom_address <= STAGE + {7'd0, bus_byte} + {19'd0, byte_i}; end
				else if (rom_hit) begin
					rom_read <= 1'b0;
					ss_dout[byte_i*8 +: 8] <= rom_data;
					if (byte_i == 3'd7) begin ss_done <= 1'b1; state <= RUN; end
					else byte_i <= byte_i + 3'd1;
				end
			end
			BLOCK_READ: begin
				if (rom_fault) begin rom_read <= 1'b0; fault <= 1'b1; state <= IDLE; end
				else if (!rom_read) begin rom_read <= 1'b1; rom_address <= block_address; end
				else if (rom_hit) begin
					rom_read <= 1'b0;
					if (!offset[0]) low_byte <= block_valid ? rom_data : 8'h00;
					else begin
						buffer_write <= 1'b1; buffer_address <= offset[9:1];
						buffer_data <= {block_valid ? rom_data : 8'h00, low_byte};
					end
					if (offset[9:0] == 10'd1023) begin
						block_ready_sequence <= request_sequence; block_ready <= 1'b1; state <= IDLE;
					end else offset <= offset + 13'd1;
				end
			end
			CART_READ: begin
				// second RAM port: address now, data one cycle later
				cart_phase <= !cart_phase;
				if (!cart_phase) begin
					// address presented this cycle
				end else begin
					if (!offset[0]) low_byte <= cart_q;
					else begin
						buffer_write <= 1'b1; buffer_address <= offset[9:1];
						buffer_data <= {cart_q, low_byte};
					end
					if (offset[9:0] == 10'd1023) begin
						block_ready_sequence <= request_sequence; block_ready <= 1'b1; state <= IDLE;
					end else begin
						offset <= offset + 13'd1;
						cart_addr <= cart_addr + 13'd1;
					end
				end
			end
			IMPORT_READ: begin
				if (rom_fault) begin rom_read <= 1'b0; fault <= 1'b1; boot_hold <= 1'b0; importing <= 1'b0; state <= IDLE; end
				else if (!rom_read) begin rom_read <= 1'b1; rom_address <= IMPORT + {9'd0, offset}; end
				else if (rom_hit) begin
					rom_read <= 1'b0;
					cart_we <= 1'b1; cart_addr <= offset; cart_wdata <= rom_data;
					if (offset == 13'h1FFF) begin
						boot_hold <= 1'b0; importing <= 1'b0; imported <= 1'b1;
						cache_invalidate <= 1'b1; state <= IDLE;
					end else offset <= offset + 13'd1;
				end
			end
			default: state <= IDLE;
			endcase
		end
	end
endmodule
