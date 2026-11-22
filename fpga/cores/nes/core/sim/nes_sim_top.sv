// Simulation top: nes_board_core with its PSRAM port exposed to the C++ model
// (sim/nes_test.cpp), plus debug taps into the on-chip RAMs.
module nes_sim_top (
	input         clk,
	input         reset_n,
	input         pause,
	input   [7:0] buttons,
	output        x_request, x_read,
	output [22:0] x_address,
	output [10:0] x_length,
	output [15:0] x_write_data,
	input         x_write_next, x_done, x_read_valid,
	input  [15:0] x_read_data,
	output        x_idle,
	input         video_resume_ok, cart_dirty_clear,
	output        pix_strobe,
	output  [7:0] pix_x, pix_y,
	output [14:0] pix_rgb,
	output        frame_strobe,
	output [15:0] audio_sample,
	output        audio_strobe,
	output        running, paused_now, bad_image, rom_fault,
	input         freeze_t, release_t, load_t, block_t,
	input   [6:0] block_n,
	input  [15:0] block_seq,
	output        block_ready,
	output [15:0] block_ready_seq,
	output        st_busy, st_fault, st_frozen, st_loaded,
	input   [9:0] qaddress,
	output  [7:0] qdata,
	input         cart_import,
	output        cart_imported, cart_dirty,
	output  [7:0] cart_written, cart_snapshot,
	// debug
	input  [12:0] dbg_addr,
	output  [7:0] dbg_prgram, dbg_cpuram, dbg_ciram, dbg_chrram,
	output        dbg_ce, dbg_stall,
	output [8:0] dbg_cyc, dbg_sl, dbg_cycp, dbg_slp, output dbg_cpaused, output [4:0] dbg_divcpu, output [2:0] dbg_divppu, output [1:0] dbg_ppu_tick, output dbg_odd,
	output [15:0] dbg_cpu_addr, output dbg_cpu_rnw, output dbg_cpu_ce, output [7:0] dbg_cpu_data
);
	nes_board_core dut (
		.clk(clk), .reset_n(reset_n), .pause(pause), .buttons(buttons),
		.x_request(x_request), .x_read(x_read), .x_address(x_address), .x_length(x_length),
		.x_write_data(x_write_data), .x_write_next(x_write_next), .x_done(x_done),
		.x_read_valid(x_read_valid), .x_read_data(x_read_data), .x_idle(x_idle),
		.pix_strobe(pix_strobe), .pix_x(pix_x), .pix_y(pix_y), .pix_rgb(pix_rgb), .frame_strobe(frame_strobe),
		.audio_sample(audio_sample), .audio_strobe(audio_strobe),
		.running(running), .paused_now(paused_now), .bad_image(bad_image), .rom_fault(rom_fault),
		.mapper_flags_o(),
		.freeze_toggle(freeze_t), .release_toggle(release_t), .load_toggle(load_t), .block_toggle(block_t),
		.block_number(block_n), .block_sequence(block_seq), .block_ready(block_ready),
		.block_ready_sequence(block_ready_seq),
		.state_busy(st_busy), .state_fault(st_fault), .state_frozen(st_frozen), .state_loaded(st_loaded),
		.qclk(clk), .qaddress(qaddress), .qdata(qdata), .video_resume_ok(video_resume_ok),
		.cart_import(cart_import), .cart_imported(cart_imported), .cart_dirty(cart_dirty),
		.cart_dirty_clear(cart_dirty_clear), .cart_written(cart_written), .cart_snapshot(cart_snapshot));
	assign dbg_prgram = dut.prgram[dbg_addr];
	assign dbg_cpuram = dut.cpuram[dbg_addr[10:0]];
	assign dbg_ciram = dut.ciram[dbg_addr[10:0]];
	assign dbg_chrram = dut.chrram[dbg_addr[12:0]];
	assign dbg_ce = dut.ce_reg;
	assign dbg_stall = dut.stall;
	assign dbg_cyc = dut.nes.ppu_cycle; assign dbg_sl = dut.nes.scanline_ppu;
	assign dbg_cycp = dut.nes.cycle_paused; assign dbg_slp = dut.nes.scanline_paused;
	assign dbg_cpaused = dut.nes.corepause_active; assign dbg_divcpu = dut.nes.div_cpu; assign dbg_divppu = dut.nes.div_ppu;
	assign dbg_ppu_tick = dut.nes.ppu_tick; assign dbg_odd = dut.nes.odd_or_even;
	assign dbg_cpu_addr = dut.nes.cpu_addr;
	assign dbg_cpu_rnw = dut.nes.cpu_rnw;
	assign dbg_cpu_ce = dut.nes.cpu_ce && dut.ce_reg;
	assign dbg_cpu_data = dut.nes.cpu_rnw ? dut.nes.internal_data_bus : dut.nes.cpu_dout;
endmodule
