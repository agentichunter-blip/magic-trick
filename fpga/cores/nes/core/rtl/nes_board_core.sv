// MiSTer NES core (rtl/gen/, from NES_MiSTer) on the Chromatic's 67.108864 MHz xclk.
//
// CLOCKING. The core keeps its native assumption that every clock edge is one NES master
// clock. It runs on nclk = xclk gated by nes_clock_gate (Gowin DCE). ce_reg is the gate's
// enable: high in the xclk cycle BEFORE an xclk edge that is also a NES edge. A 26-bit
// accumulator adds 21,441,960 per xclk (2^26 = 67,108,864), i.e. exactly 357,366 master
// clocks x 60 frames per second. NES edges are at least 3 xclk apart (4 before an edge that
// consumes a ROM byte), and every path from a NES register has two xclk periods (multicycle
// 2, to nclk and to xclk logic alike; see TIMING below and README). The enable is held
// low (the whole NES - CPU, PPU, APU, mappers - stops between two edges) while a ROM read
// the next edge would consume misses the cache, while `pause` is high, before the info block
// is decoded and while the battery import runs; then exactly 255 NES clocks of reset follow
// (the first 128 with the mapper number hidden so every mapper module initialises from the
// header flags, e.g. MMC3 mirroring and fixed banks),
// so the start-up does not depend on PSRAM latency. Lost edges are made up later (credit,
// at most 1/3 of xclk), so frames average 60.000 Hz.
//
// MEMORY. As on MiSTer, the core has two byte ports (CPU and PPU) with level strobes; an
// access starts on a strobe's rising edge and *_din holds the last byte read (or written)
// until the next access (the latches are savestate register 28, SS_Ext). Here every access
// completes in exactly one NES clock, whatever the source, because the clock is stalled
// until the data is there; NES-visible behaviour does not depend on PSRAM latency.
//   CPU 0x000000-0x1FFFFF PRG ROM (PSRAM, PRG cache)   0x38xxxx CPU RAM 2 KiB (BSRAM)
//       0x3Cxxxx          PRG RAM 8 KiB (BSRAM, battery; second port for the MCU)
//   PPU 0x2xxxxx          CHR ROM (PSRAM, CHR cache) or CHR RAM 8 KiB (BSRAM) if CHR RAM
//       0x3Axxxx          CIRAM 2 KiB (BSRAM)
//       0x3FE000-0x3FFFFF MMC3 four-screen / TQROM RAM: the CHR RAM block (CHR-ROM games)
// PSRAM ROM window (byte offsets; PSRAM address = ROM_BASE + offset, ROM_BASE = 0x020000):
//   0x000000 info block (16 bytes, written by the MCU):
//            0-3 'N','E','S','1'
//            4-5 mapper number (little endian, iNES/NES 2.0, 0-4095)
//            6   submapper (NES 2.0, else 0)
//            7   bit 0 vertical mirroring (iNES flags 6 bit 0), bit 1 four-screen (flags 6
//                bit 3), bit 2 battery (flags 6 bit 1), bit 3 CHR RAM (no CHR ROM),
//                bit 4 NES 2.0 header
//            8-9   PRG ROM size in KiB (little endian, >= 16)
//            10-11 CHR ROM size in KiB (0 = CHR RAM)
//            12  NES 2.0 header byte 10 (PRG-RAM / PRG-NVRAM shift counts), else 0
//            13  NES 2.0 header byte 11 (CHR-RAM shift counts), else 0
//            14-15 0
//   0x001000 PRG ROM, then CHR ROM immediately after (0x1000 + PRG size)
//   ........ PRG + CHR must end below 0x3F0000 (3.93 MiB)
//   0x3F0000 save-state staging area (21 KiB, nes_state_engine.sv)
//   0x3F8000 battery-RAM import area (8 KiB)
module nes_board_core #(parameter [22:0] ROM_BASE = 23'h020000) (
	input             clk,              // xclk, 67.108864 MHz
	input             reset_n,          // low: core in reset, caches invalidated, info re-read
	input             pause,            // shell quiesce: stops the NES clock at once (invisible to the
	                                    // NES) and issues no new PSRAM request; safe when paused_now && x_idle
	input       [7:0] buttons,          // controller 1, 1 = pressed: A,B,Select,Start,Up,Down,Left,Right (bit 0..7)
	// PSRAM port (same signals as the Chromatic virtual xport): 64-byte read bursts for the
	// caches, 8-byte writes for save-state staging. x_address is a PSRAM byte address.
	output            x_request,        // one-cycle pulse
	output            x_read,
	output     [22:0] x_address,
	output     [10:0] x_length,         // bytes
	output     [15:0] x_write_data,
	input             x_write_next,
	input             x_done,
	input             x_read_valid,
	input      [15:0] x_read_data,
	output            x_idle,           // no PSRAM transaction in flight or about to start
	// video: one strobe per visible pixel, x 0-255, y 0-239, BGR555 (R in bits 4:0)
	output reg        pix_strobe = 1'b0,
	output reg  [7:0] pix_x = 8'd0,
	output reg  [7:0] pix_y = 8'd0,
	output     [14:0] pix_rgb,
	output reg        frame_strobe = 1'b0, // start of vertical blank (after line 239)
	// audio: APU output, unsigned 16-bit, new value with each audio_strobe (1.789773 MHz nominal)
	output     [15:0] audio_sample,
	output reg        audio_strobe = 1'b0,
	// status
	output            running,          // loaded, out of reset
	output            paused_now,       // pause is in effect: the NES clock is stopped
	output reg        bad_image = 1'b0, // info block magic or sizes wrong
	output            rom_fault,        // PSRAM transfer timeout
	output     [63:0] mapper_flags_o,
	// save states (see nes_state_engine.sv)
	input             freeze_toggle, release_toggle, load_toggle, block_toggle,
	input       [6:0] block_number, input [15:0] block_sequence,
	output            block_ready, output [15:0] block_ready_sequence,
	output            state_busy, state_fault, state_frozen, state_loaded,
	input             qclk, input [9:0] qaddress, output [7:0] qdata,
	input             video_resume_ok,  // RELEASE waits for this (tie high if unused)
	// battery RAM
	input             cart_import,      // hold high across reset to import the staged .sav
	output            cart_imported,
	output reg        cart_dirty = 1'b0,
	input             cart_dirty_clear, // one-cycle pulse when the MCU starts copying the battery RAM:
	                                    // cart_snapshot <= cart_written (plus a write in this very cycle),
	                                    // cart_written <= 0, cart_dirty <= 0 (no write is lost)
	output reg  [7:0] cart_written = 8'd0,  // 1 KiB blocks ($6000+n*1K) written since the last clear
	output reg  [7:0] cart_snapshot = 8'd0,
	// Game Genie (CODES in rtl/gen/cheatcodes.sv, 16 codes): the shell's gg_cheat_loader drives these
	// on xclk while the core is stopped; tie to 0 for no codes.
	input     [128:0] gg_code,
	input             gg_clear
);
	// ------------------------------------------------------------------ info block loader
	localparam [21:0] PRG_BASE = 22'h001000;
	reg [3:0] ld_state = 4'd0;        // 0 read info, 1 run, 2 bad
	reg [3:0] ld_index = 4'd0;
	reg [7:0] info [0:15];
	reg ld_read = 1'b0;
	reg [63:0] mapper_flags = 64'd0;
	reg [9:0] prg_mask = 10'd0, chr_mask = 10'd0;
	reg [21:0] chr_base = 22'd0;
	reg chr_ram = 1'b0;
	reg [7:0] rst_count = 8'd0;
	reg flags_hide = 1'b1;   // first 128 reset clocks: mapper number hidden (see loader)
	wire [63:0] nes_flags = flags_hide ? {mapper_flags[63:21], 4'd0, mapper_flags[16:8], 8'hFF} : mapper_flags;
	assign mapper_flags_o = mapper_flags;

	function automatic [9:0] mask10(input [10:0] size);   // GameLoader mask()
		integer i;
		for (i = 0; i < 10; i = i + 1) mask10[i] = size > (11'd1 << i);
	endfunction
	function automatic [2:0] size_class(input [15:0] kib, input [15:0] unit);
		size_class = kib <= unit ? 3'd0 : kib <= unit*2 ? 3'd1 : kib <= unit*4 ? 3'd2 :
			kib <= unit*8 ? 3'd3 : kib <= unit*16 ? 3'd4 : kib <= unit*32 ? 3'd5 :
			kib <= unit*64 ? 3'd6 : 3'd7;
	endfunction
	wire [15:0] i_prg_kib = {info[9], info[8]};
	wire [15:0] i_chr_kib = {info[11], info[10]};
	wire [7:0] i_fl = info[7];

	// ------------------------------------------------------------------ NES clock enable
	reg [25:0] acc = 26'd0;
	reg [10:0] credit = 11'd0;
	wire ce_reg;              // registered enable (in nes_clock_gate: sync-cleared flip-flop)
	reg [1:0] since = 2'd0;
	wire nclk, go;
	// The enable flip-flop is cleared (synchronous reset pin) in the edge's cycle and the next
	// one: nclk->xclk paths are two-cycle paths, and its D input may still be settling then.
	nes_clock_gate gate(.clk_in(clk), .go(go), .clear(ce_reg || since == 2'd0), .ce(ce_reg), .clk_out(nclk));
	wire nes_edge = ce_reg;   // this xclk edge is also a NES edge

	// ------------------------------------------------------------------ NES core
	wire [24:0] cpumem_addr; wire cpumem_read, cpumem_write; wire [7:0] cpumem_dout;
	wire [21:0] ppumem_addr; wire ppumem_read, ppumem_write; wire [7:0] ppumem_dout;
	reg  [7:0] cpu_din = 8'd0, ppu_din = 8'd0;
	wire [15:0] sample; wire [5:0] color; wire [2:0] emphasis; wire [8:0] cycle, scanline;
	wire [1:0] joypad_clock; wire [2:0] joypad_out; wire [4:0] joypad1_data, joypad2_data;
	wire corepaused, sleep_savestate, nes_state_loaded, apu_ce, vblank, cpu_rom_hint, ppu_dot;
	wire [24:0] ss_ram_addr; wire ss_ram_rd, ss_ram_wr; wire [7:0] ss_ram_wdata; reg [7:0] ss_ram_rdata;
	wire [63:0] ssx_din; wire [9:0] ssx_adr; wire ssx_wren, ssx_rst, ssx_load; wire [63:0] ssx_dout;
	wire [63:0] save_din, save_dout; wire [25:0] save_adr; wire save_rnw, save_ena, save_done;
	wire save_req, load_req, eng_hold, boot_hold;
	reg nes_reset = 1'b1;
	wire pausecore = sleep_savestate || eng_hold;

	NES nes (
		.clk(nclk), .reset_nes(nes_reset), .ppu_rst_behavior(1'b0), .cold_reset(nes_reset),
		.pausecore(pausecore), .corepaused(corepaused), .sys_type(2'b00), .vs_dip_switches(8'd0),
		.nes_div(), .mapper_flags(nes_flags), .sample(sample), .color(color),
		.joypad_clock(joypad_clock), .joypad_out(joypad_out),
		.joypad1_data(joypad1_data), .joypad2_data(joypad2_data),
		.fds_busy(1'b0), .fds_eject(1'b0), .fds_auto_eject(1'b0), .max_diskside(2'd0), .fds_fast(1'b0),
		.diskside(), .audio_channels(5'b11111), .ex_sprites(1'b0), .mask(2'b00), .dejitter_timing(1'b0),
		.cpumem_addr(cpumem_addr), .cpumem_read(cpumem_read), .cpumem_write(cpumem_write),
		.cpumem_dout(cpumem_dout), .cpumem_din(cpu_din),
		.ppumem_addr(ppumem_addr), .ppumem_read(ppumem_read), .ppumem_write(ppumem_write),
		.ppumem_dout(ppumem_dout), .ppumem_din(ppu_din), .refresh(),
		.prg_mask({11'd0, prg_mask}), .chr_mask({10'd0, chr_mask}),
		.bram_addr(), .bram_din(8'd0), .bram_dout(), .bram_write(), .bram_override(),
		.cycle(cycle), .scanline(scanline), .int_audio(1'b1), .ext_audio(1'b0), .apu_ce(apu_ce),
		.gg(1'b0), .gg_code(gg_code), .gg_avail(), .gg_reset(gg_clear), .gg_clk(clk), .emphasis(emphasis),
		.save_written(), .mapper_has_flashsaves(), .debug_dots(1'b0),
		.mapper_has_savestate(), .increaseSSHeaderCount(1'b1), .save_state(save_req), .load_state(load_req),
		.savestate_number(2'd0), .sleep_savestate(sleep_savestate), .state_loaded(nes_state_loaded),
		.hsync(), .hblank(), .vsync(), .vblank(vblank), .composite_a(), .composite_b(),
		.Savestate_SDRAMAddr(ss_ram_addr), .Savestate_SDRAMRdEn(ss_ram_rd), .Savestate_SDRAMWrEn(ss_ram_wr),
		.Savestate_SDRAMWriteData(ss_ram_wdata), .Savestate_SDRAMReadData(ss_ram_rdata),
		.SaveStateExt_Din(ssx_din), .SaveStateExt_Adr(ssx_adr), .SaveStateExt_wren(ssx_wren),
		.SaveStateExt_rst(ssx_rst), .SaveStateExt_Dout(ssx_dout), .SaveStateExt_load(ssx_load),
		.SAVE_out_Din(save_din), .SAVE_out_Dout(save_dout), .SAVE_out_Adr(save_adr),
		.SAVE_out_rnw(save_rnw), .SAVE_out_ena(save_ena), .SAVE_out_be(), .SAVE_out_done(save_done),
		.cpu_rom_hint(cpu_rom_hint), .ppu_dot(ppu_dot)
	);

	// ------------------------------------------------------------------ address decode
	wire cpu_is_rom    = cpumem_addr[24:21] == 4'd0;
	wire cpu_is_cpuram = cpumem_addr[21:17] == 5'b11100;
	wire [21:0] prg_rom_addr = PRG_BASE + {1'b0, cpumem_addr[20:0]};
	wire ppu_is_ciram  = ppumem_addr[21:17] == 5'b11101;
	wire ppu_is_chrrom = ppumem_addr[21:20] == 2'b10 && !chr_ram;
	wire [21:0] chr_rom_addr = chr_base + {2'b00, ppumem_addr[19:0]};
	// savestate memory side (only while the core is paused for a save/load)
	wire ss_owner = sleep_savestate && corepaused;
	wire ss_is_chr    = ss_ram_addr[24:20] == 5'b00010;   // 0x200000
	wire ss_is_ciram  = ss_ram_addr[21:17] == 5'b11101;   // 0x3A0000
	wire ss_is_cpuram = ss_ram_addr[21:17] == 5'b11100;   // 0x380000
	wire ss_is_prgram = ss_ram_addr[21:18] == 4'b1111;    // 0x3C0000

	// NES-clock-domain edge detectors (an access = rising edge of a strobe)
	reg cpu_rd_q = 1'b0, cpu_wr_q = 1'b0, ppu_rd_q = 1'b0, ppu_wr_q = 1'b0;
	wire cpu_wr_rise = cpumem_write && !cpu_wr_q;
	wire cpu_rd_rise = cpumem_read && !cpu_rd_q && !cpu_wr_rise;
	wire ppu_wr_rise = ppumem_write && !ppu_wr_q;
	wire ppu_rd_rise = ppumem_read && !ppu_rd_q && !ppu_wr_rise;

	// ------------------------------------------------------------------ on-chip RAMs (xclk)
	// Written on NES edges only (nes_edge), read every xclk (data needed >= 3 xclk later).
	reg [7:0] cpuram [0:2047]; reg [7:0] cpuram_q;
	reg [7:0] ciram  [0:2047]; reg [7:0] ciram_q;
	reg [7:0] chrram [0:8191]; reg [7:0] chrram_q;
	reg [7:0] prgram [0:8191]; reg [7:0] prgram_q, prgram_qb;
	wire [10:0] cpuram_a = ss_owner ? ss_ram_addr[10:0] : cpumem_addr[10:0];
	wire [10:0] ciram_a  = ss_owner ? ss_ram_addr[10:0] : ppumem_addr[10:0];
	wire [12:0] chrram_a = ss_owner ? ss_ram_addr[12:0] : ppumem_addr[12:0];
	wire [12:0] prgram_a = ss_owner ? ss_ram_addr[12:0] : cpumem_addr[12:0];
	wire ss_we = ss_owner && ss_ram_wr && nes_edge;
	wire cpuram_we = ss_owner ? ss_we && ss_is_cpuram : nes_edge && cpu_wr_rise && cpu_is_cpuram;
	wire prgram_we = ss_owner ? ss_we && ss_is_prgram : nes_edge && cpu_wr_rise && !cpu_is_rom && !cpu_is_cpuram;
	wire ciram_we  = ss_owner ? ss_we && ss_is_ciram  : nes_edge && ppu_wr_rise && ppu_is_ciram;
	wire chrram_we = ss_owner ? ss_we && ss_is_chr    : nes_edge && ppu_wr_rise && !ppu_is_ciram && !ppu_is_chrrom;
	wire [7:0] cpu_wd = ss_owner ? ss_ram_wdata : cpumem_dout;
	wire [7:0] ppu_wd = ss_owner ? ss_ram_wdata : ppumem_dout;
	// Write-through RAMs (GW5A BSRAM has no read-before-write mode in PnR): q is only
	// consumed at the next NES edge, at least 3 xclk later, after it re-reads the RAM.
	always @(posedge clk) begin
		if (cpuram_we) begin cpuram[cpuram_a] <= cpu_wd; cpuram_q <= cpu_wd; end
		else cpuram_q <= cpuram[cpuram_a];
	end
	always @(posedge clk) begin
		if (ciram_we) begin ciram[ciram_a] <= ppu_wd; ciram_q <= ppu_wd; end
		else ciram_q <= ciram[ciram_a];
	end
	always @(posedge clk) begin
		if (chrram_we) begin chrram[chrram_a] <= ppu_wd; chrram_q <= ppu_wd; end
		else chrram_q <= chrram[chrram_a];
	end
	// PRG RAM: true dual port; port B belongs to the battery transfer (state engine).
	wire [12:0] cart_addr; wire cart_we; wire [7:0] cart_wdata;
	always @(posedge clk) begin
		if (prgram_we) begin prgram[prgram_a] <= cpu_wd; prgram_q <= cpu_wd; end
		else prgram_q <= prgram[prgram_a];
	end
	always @(posedge clk) begin
		if (cart_we) begin prgram[cart_addr] <= cart_wdata; prgram_qb <= cart_wdata; end
		else prgram_qb <= prgram[cart_addr];
	end
	always @(*) begin
		ss_ram_rdata = ss_is_cpuram ? cpuram_q : ss_is_ciram ? ciram_q : ss_is_prgram ? prgram_q : chrram_q;
	end
	// battery dirty tracking (game writes only)
	wire game_prg_write = !ss_owner && prgram_we;
	wire [7:0] cart_write_block = game_prg_write ? (8'd1 << prgram_a[12:10]) : 8'd0;
	always @(posedge clk) begin
		if (!reset_n) begin cart_dirty <= 1'b0; cart_written <= 8'd0; cart_snapshot <= 8'd0; end
		else if (cart_dirty_clear) begin
			cart_snapshot <= cart_written | cart_write_block; cart_written <= 8'd0; cart_dirty <= 1'b0;
		end else begin
			cart_written <= cart_written | cart_write_block;
			if (game_prg_write) cart_dirty <= 1'b1;
		end
	end

	// ------------------------------------------------------------------ ROM caches + PSRAM arbiter
	wire eng_owns_rom, eng_rom_read, eng_invalidate; wire [21:0] eng_rom_address;
	wire ld_owns = ld_state == 4'd0;
	wire [21:0] prg_lookup_addr = eng_owns_rom ? eng_rom_address : ld_owns ? {18'd0, ld_index} : prg_rom_addr;
	wire prg_lookup = eng_owns_rom ? eng_rom_read : ld_owns ? ld_read :
	                  cpu_is_rom && (cpu_rom_hint || cpumem_read);
	wire chr_lookup = ld_state == 4'd1 && ppu_is_chrrom;
	wire prg_hit, chr_hit, prg_idle, chr_idle; wire [7:0] prg_data, chr_data;
	wire prg_fill_req, chr_fill_req; wire [21:0] prg_fill_addr, chr_fill_addr;
	reg prg_fill_start = 1'b0, chr_fill_start = 1'b0;
	reg [1:0] owner = 2'd0;  // 0 none, 1 PRG, 2 CHR, 3 state engine write
	reg bus_busy = 1'b0;
	reg req_q = 1'b0; reg [22:0] addr_q = 23'd0; reg read_q = 1'b0;
	reg [17:0] bus_timer = 18'd0; reg fault_q = 1'b0; reg eng_grant = 1'b0;
	wire cache_invalidate = !reset_n || eng_invalidate;
	wire eng_owns_bus, eng_x_request; wire [21:0] eng_x_address; wire [15:0] eng_x_write_data;
	nes_rom_cache #(.LINE_BITS(7)) prg_cache(.clk(clk), .invalidate(cache_invalidate),
		.lookup(prg_lookup), .addr(prg_lookup_addr), .hit(prg_hit), .data(prg_data), .idle(prg_idle),
		.fill_req(prg_fill_req), .fill_addr(prg_fill_addr), .fill_start(prg_fill_start),
		.fill_valid(x_read_valid && owner == 2'd1), .fill_data(x_read_data), .fill_done(x_done && owner == 2'd1));
	nes_rom_cache #(.LINE_BITS(7)) chr_cache(.clk(clk), .invalidate(cache_invalidate),
		.lookup(chr_lookup), .addr(chr_rom_addr), .hit(chr_hit), .data(chr_data), .idle(chr_idle),
		.fill_req(chr_fill_req), .fill_addr(chr_fill_addr), .fill_start(chr_fill_start),
		.fill_valid(x_read_valid && owner == 2'd2), .fill_data(x_read_data), .fill_done(x_done && owner == 2'd2));
	assign x_request = req_q;
	assign x_read = read_q;
	assign x_address = addr_q;
	assign x_length = owner == 2'd3 ? 11'd8 : 11'd64;
	assign x_write_data = eng_x_write_data;
	assign rom_fault = fault_q;
	always @(posedge clk) begin
		req_q <= 1'b0; prg_fill_start <= 1'b0; chr_fill_start <= 1'b0; eng_grant <= 1'b0;
		if (bus_busy) begin
			bus_timer <= bus_timer + 18'd1;
			if (x_done) begin bus_busy <= 1'b0; owner <= 2'd0; end
			else if (&bus_timer) fault_q <= 1'b1;   // stays busy: never issue a competing request
		end else if (!prg_fill_start && !chr_fill_start && !req_q && !pause) begin
			bus_timer <= 18'd0;
			if (eng_owns_bus && eng_x_request && !eng_grant) begin
				owner <= 2'd3; bus_busy <= 1'b1; req_q <= 1'b1; read_q <= 1'b0; eng_grant <= 1'b1;
				addr_q <= ROM_BASE + {1'b0, eng_x_address};
			end else if (chr_fill_req && !eng_owns_bus) begin
				owner <= 2'd2; bus_busy <= 1'b1; req_q <= 1'b1; read_q <= 1'b1; chr_fill_start <= 1'b1;
				addr_q <= ROM_BASE + {1'b0, chr_fill_addr};
			end else if (prg_fill_req && !eng_owns_bus) begin
				owner <= 2'd1; bus_busy <= 1'b1; req_q <= 1'b1; read_q <= 1'b1; prg_fill_start <= 1'b1;
				addr_q <= ROM_BASE + {1'b0, prg_fill_addr};
			end
		end
		if (!reset_n) begin bus_busy <= 1'b0; owner <= 2'd0; fault_q <= 1'b0; end
	end
	wire rom_idle = prg_idle && chr_idle && !bus_busy;
	assign x_idle = !bus_busy && !req_q && (pause || (!prg_fill_req && !chr_fill_req && !(eng_owns_bus && eng_x_request)));

	// ------------------------------------------------------------------ clock enable / stall
	// The next NES edge would consume a ROM byte that is not in the cache: hold the clock.
	wire cpu_need_rom = cpu_rd_rise && cpu_is_rom;
	wire ppu_need_rom = ppu_rd_rise && ppu_is_chrrom;
	wire stall = (cpu_need_rom && (!prg_hit || eng_owns_rom)) || (ppu_need_rom && !chr_hit) || boot_hold ||
	             ld_state != 4'd1;   // no NES clock before the info block is decoded
	wire acc_carry;
	wire [26:0] acc_next = {1'b0, acc} + 27'd21441960;
	assign acc_carry = acc_next[26];
	// TIMING. Everything the NES drives (addresses, strobes) is a two-cycle path into xclk logic
	// (README): it is settled at the second xclk edge after a NES edge, not the first. A
	// non-ROM edge may come three xclk after the last one (enable decided in the cycle before
	// the second edge, registered on it). A ROM byte comes through a cache whose RAMs sample the
	// address on every edge, so its hit is trusted only from the second edge's sample: such an
	// edge comes at least four xclk after the last (since >= 2 when deciding). Credit is spent
	// one cycle later, from the registered enable, so it never depends on a settling path.
	wire rom_wait = cpu_need_rom || ppu_need_rom;
	assign go = (credit != 11'd0) && !stall && !pause && (since >= 2'd2 || !rom_wait);
	always @(posedge clk) begin
		acc <= acc_next[25:0];
		case ({acc_carry && credit != 11'h7FF, ce_reg})
			2'b10: credit <= credit + 11'd1;
			2'b01: credit <= credit - 11'd1;
			default: ;
		endcase
		since <= ce_reg ? 2'd0 : (since == 2'd3 ? 2'd3 : since + 2'd1);
	end

	// ------------------------------------------------------------------ NES-domain glue (nclk)
	// Data latches (MiSTer sdram ch0/ch1 dout), joypads, savestate register 28.
	wire [63:0] ss_ext;
	reg [23:0] joy1_bits = 24'd0, joy2_bits = 24'd0;
	reg [1:0] last_joypad_clock = 2'd0;
	wire [63:0] ss_ext_back = {joy2_bits, joy1_bits, cpu_din, ppu_din};
	eReg_SavestateV #(10'd28, 64'd0) ss_reg_ext(nclk, ssx_din, ssx_adr, ssx_wren, ssx_rst, ssx_dout, ss_ext_back, ss_ext);
	wire [7:0] cpu_rdata = cpu_is_rom ? prg_data : cpu_is_cpuram ? cpuram_q : prgram_q;
	wire [7:0] ppu_rdata = ppu_is_ciram ? ciram_q : ppu_is_chrrom ? chr_data : chrram_q;
	always @(posedge nclk) begin
		cpu_rd_q <= cpumem_read; cpu_wr_q <= cpumem_write;
		ppu_rd_q <= ppumem_read; ppu_wr_q <= ppumem_write;
		if (ssx_load) begin
			ppu_din <= ss_ext[7:0];
			cpu_din <= ss_ext[15:8];
		end else begin
			if (cpu_wr_rise) cpu_din <= cpumem_dout;
			else if (cpu_rd_rise) cpu_din <= cpu_rdata;
			if (ppu_wr_rise) ppu_din <= ppumem_dout;
			else if (ppu_rd_rise) ppu_din <= ppu_rdata;
		end
	end
	// Controllers as NES.sv: shift registers loaded while $4016 bit 0 is set, shifted on the
	// falling edge of the read strobe; port 2 is an empty controller.
	always @(posedge nclk) begin
		if (nes_reset) begin
			joy1_bits <= 24'd0; joy2_bits <= 24'd0; last_joypad_clock <= 2'd0;
		end else if (ssx_load) begin
			joy1_bits <= ss_ext[39:16]; joy2_bits <= ss_ext[63:40];
		end else begin
			if (joypad_out[0]) begin
				joy1_bits <= {16'hFFFF, buttons};
				joy2_bits <= {16'hFFFF, 8'h00};
			end
			if (!joypad_clock[0] && last_joypad_clock[0]) joy1_bits <= {1'b0, joy1_bits[23:1]};
			if (!joypad_clock[1] && last_joypad_clock[1]) joy2_bits <= {1'b0, joy2_bits[23:1]};
			last_joypad_clock <= joypad_clock;
		end
	end
	assign joypad1_data = {4'b0000, joy1_bits[0]};
	assign joypad2_data = {4'b0000, joy2_bits[0]};

	// ------------------------------------------------------------------ loader / reset sequencing (xclk)
	always @(posedge clk) begin
		if (!reset_n) begin
			ld_state <= 4'd0; ld_index <= 4'd0; ld_read <= 1'b0; nes_reset <= 1'b1; bad_image <= 1'b0; rst_count <= 8'd0;
			flags_hide <= 1'b1;
		end else begin
			case (ld_state)
			4'd0: begin
				if (!ld_read) ld_read <= 1'b1;
				else if (prg_hit) begin
					ld_read <= 1'b0;
					info[ld_index] <= prg_data;
					ld_index <= ld_index + 4'd1;
					if (ld_index == 4'd15) ld_state <= 4'd3;
				end
			end
			4'd3: begin   // decode (registered, one cycle)
				if (info[0] == "N" && info[1] == "E" && info[2] == "S" && info[3] == "1" &&
				    i_prg_kib >= 16'd8 && i_prg_kib <= 16'd2048 && i_chr_kib <= 16'd1024 &&
				    {6'd0, i_prg_kib, 10'd0} + {6'd0, i_chr_kib, 10'd0} <= 32'h3EF000) begin
					mapper_flags <= {28'd0, i_fl[4], info[12][7:4], info[12][3:0], i_fl[2],
						info[6][3:0], info[5][3:0], i_fl[1], i_fl[3], i_fl[0],
						size_class(i_chr_kib, 16'd8), size_class(i_prg_kib, 16'd16), info[4]};
					prg_mask <= mask10(i_prg_kib[11:1]);
					chr_mask <= mask10(i_chr_kib != 16'd0 && !i_fl[3] ? i_chr_kib[11:1] : 11'd4);
					chr_ram <= i_fl[3] || i_chr_kib == 16'd0;
					chr_base <= PRG_BASE + {i_prg_kib[11:0], 10'd0};
					ld_state <= 4'd1;
				end else begin
					ld_state <= 4'd2; bad_image <= 1'b1;
				end
			end
			4'd1: begin
				// out of reset once the battery import (if any) is done and the core has had
				// exactly 255 NES clocks of reset: the NES clock does not run before that,
				// so the start-up is the same whatever the PSRAM latency was
				// The mapper modules initialise themselves while they are NOT selected (MiSTer
				// feeds them flags of 0 while downloading): give them 128 clocks of the real
				// flags with mapper number 255 (no module), then the real mapper number.
				if (!boot_hold && nes_edge) begin
					if (rst_count == 8'd127) flags_hide <= 1'b0;
					if (rst_count != 8'hFF) rst_count <= rst_count + 8'd1;
					else nes_reset <= 1'b0;
				end
			end
			default: nes_reset <= 1'b1;
			endcase
		end
	end
	assign running = ld_state == 4'd1 && !nes_reset;
	assign paused_now = pause && !ce_reg;

	// ------------------------------------------------------------------ video
	reg pix_take = 1'b0, pix_take2 = 1'b0, pix_ok = 1'b0, vbl_q = 1'b0;
	reg [8:0] pal_addr = 9'd0;
	nes_palette_rom palette(.clk(clk), .addr(pal_addr), .q(pix_rgb));
	// PPU dot happens on this NES edge: sample colour/position two xclk later (post-edge; a
	// two-cycle path), look the colour up one more xclk later.
	// MiSTer video.sv shows cycles 1..256 with the colour one dot behind its blanking: the
	// colour sampled together with cycle c is pixel x = c - 2.
	localparam [8:0] X_OFFSET = 9'd2;
	wire [8:0] px = cycle - X_OFFSET;
	always @(posedge clk) begin
		pix_take <= nes_edge && ppu_dot && running && !corepaused;   // (pause stops nes_edge)
		pix_take2 <= pix_take;
		pix_ok <= 1'b0; pix_strobe <= pix_ok; frame_strobe <= 1'b0;
		if (pix_take2) begin
			pal_addr <= {emphasis, color};
			pix_ok <= px < 9'd256 && scanline < 9'd240;
			pix_x <= px[7:0]; pix_y <= scanline[7:0];
			vbl_q <= vblank;
			if (vblank && !vbl_q) frame_strobe <= 1'b1;
		end
	end
	// ------------------------------------------------------------------ audio
	assign audio_sample = sample;
	// strobe one xclk late, so the sample is read two xclk after the NES edge that set it
	reg audio_take = 1'b0;
	always @(posedge clk) begin audio_take <= nes_edge && apu_ce && running; audio_strobe <= audio_take; end

	// ------------------------------------------------------------------ save states
	wire eng_busy, eng_frozen, eng_loaded, eng_fault;
	assign state_busy = eng_busy; assign state_frozen = eng_frozen; assign state_loaded = eng_loaded;
	assign state_fault = eng_fault;
	wire nes_ss_busy = sleep_savestate;
	nes_state_engine engine(.clk(clk), .reset(!reset_n), .nes_edge(nes_edge), .bus_hold(pause),
		.video_resume_ok(video_resume_ok),
		.freeze_toggle(freeze_toggle), .release_toggle(release_toggle), .load_toggle(load_toggle),
		.block_toggle(block_toggle), .block_number(block_number), .block_sequence(block_sequence),
		.block_ready(block_ready), .block_ready_sequence(block_ready_sequence),
		.busy(eng_busy), .fault(eng_fault), .frozen(eng_frozen), .loaded(eng_loaded), .hold_core(eng_hold),
		.save_req(save_req), .load_req(load_req), .state_loaded(nes_state_loaded), .state_busy(nes_ss_busy),
		.ss_din(save_din), .ss_dout(save_dout), .ss_adr(save_adr), .ss_rnw(save_rnw), .ss_ena(save_ena),
		.ss_done(save_done),
		.owns_rom(eng_owns_rom), .cache_invalidate(eng_invalidate), .rom_read(eng_rom_read),
		.rom_address(eng_rom_address), .rom_data(prg_data), .rom_hit(prg_hit), .rom_fault(fault_q),
		.rom_idle(rom_idle),
		.owns_bus(eng_owns_bus), .x_request(eng_x_request), .x_address(eng_x_address), .x_grant(eng_grant),
		.x_write_data(eng_x_write_data), .x_write_next(x_write_next && owner == 2'd3), .x_done(x_done && owner == 2'd3),
		.cart_import(cart_import), .loader_ready(ld_state == 4'd1), .boot_hold(boot_hold), .imported(cart_imported),
		.cart_addr(cart_addr), .cart_we(cart_we), .cart_wdata(cart_wdata), .cart_q(prgram_qb),
		.qclk(qclk), .qaddress(qaddress), .qdata(qdata));
endmodule
