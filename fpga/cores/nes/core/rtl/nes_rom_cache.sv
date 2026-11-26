// Read-only PSRAM line cache for the NES core (one instance for PRG ROM, one for CHR ROM).
// Direct-mapped, 64-byte lines, 2^LINE_BITS lines (LINE_BITS=7: 8 KiB of data = 4 BSRAM;
// {valid, tag} words in one more BSRAM). Address = byte offset in the 4 MiB ROM window.
//
// Lookup pipeline (xclk): the address is registered (addr_r) on every edge except the one
// after a NES edge (`unsettled`), where NES-driven addresses may still be settling; the RAMs
// and rd_addr only ever sample addr_r. Data, tag and rd_addr are registered together; `hit`
// (combinational) says that `data` is the byte at the CURRENT address. An address that did
// not change on the last NES edge was captured on that edge, so its hit is known one cycle
// later; one that did change takes two more cycles. A miss starts a fill (fill_req). A line is marked invalid when its fill starts and
// valid when the fill completes; the cycle after any tag write, hits are masked (the tag
// read in that cycle may be the old one). `invalidate` sweeps all tags to invalid (one per
// cycle, 2^LINE_BITS cycles; the cache misses meanwhile and starts no fill); a fill in
// flight is discarded. There is no timeout here: the arbiter (nes_board_core) has one.
module nes_rom_cache #(parameter integer LINE_BITS = 7) (
	input             clk,
	input             invalidate,       // drop all lines
	input             lookup,           // address is a cartridge ROM address worth caching
	input      [21:0] addr,
	input             unsettled,        // addr and lookup may still be settling on the next edge
	output            hit,
	output      [7:0] data,
	output            idle,
	// fill port (to the PSRAM arbiter)
	output reg        fill_req = 1'b0,  // level until fill_start
	output reg [21:0] fill_addr = 22'd0,// line address (low 6 bits zero)
	input             fill_start,       // arbiter issued the burst for this cache
	input             fill_valid,       // one 16-bit word
	input      [15:0] fill_data,
	input             fill_done
);
	localparam integer LINES = 1 << LINE_BITS;
	localparam integer TB = 22 - 6 - LINE_BITS;
	reg [TB:0] tags [0:LINES-1];          // {valid, tag}
	reg [15:0] cmem [0:LINES*32-1];
	reg busy = 1'b0, tag_written = 1'b0, discard = 1'b0;
	reg sweeping = 1'b1;                  // invalidate all lines after configuration
	reg [LINE_BITS-1:0] sweep_index = 0;
	reg [LINE_BITS-1:0] fill_index = 0;
	reg [TB-1:0] pending_tag = 0;
	reg [5:0] words = 6'd0;

	reg [15:0] rd_word = 16'd0;
	reg [21:0] rd_addr = 22'h3FFFFF, addr_r = 22'h3FFFFF;
	reg [TB:0] rd_tag = 0;
	always @(posedge clk) begin
		if (!unsettled) addr_r <= addr;
		rd_word <= cmem[addr_r[LINE_BITS+5:1]];
		rd_tag  <= tags[addr_r[LINE_BITS+5:6]];
		rd_addr <= addr_r;
	end
	wire rd_current = rd_addr == addr;
	wire [LINE_BITS-1:0] rd_index = rd_addr[LINE_BITS+5:6];
	wire tag_ok = rd_tag == {1'b1, rd_addr[21:LINE_BITS+6]} && !tag_written && !sweeping;
	assign hit = rd_current && tag_ok;
	assign data = rd_addr[0] ? rd_word[15:8] : rd_word[7:0];
	assign idle = !busy && !fill_req;

	wire last_word = fill_done && (words + (fill_valid ? 6'd1 : 6'd0)) == 6'd32;
	wire start_fill = !unsettled && !busy && !fill_req && !invalidate && !sweeping && lookup && rd_current && !tag_ok && !tag_written;
	wire finish_fill = busy && fill_done && last_word && !discard && !invalidate;
	// one tag write port: sweep, line invalidation at fill start, new tag at fill end
	wire tag_we = sweeping || start_fill || finish_fill;
	wire [LINE_BITS-1:0] tag_wa = sweeping ? sweep_index : start_fill ? rd_index : fill_index;
	wire [TB:0] tag_wd = finish_fill && !sweeping && !start_fill ? {1'b1, pending_tag} : {(TB+1){1'b0}};
	always @(posedge clk) begin
		if (busy && fill_valid && words < 6'd32) cmem[{fill_index, words[4:0]}] <= fill_data;
		if (tag_we) tags[tag_wa] <= tag_wd;
	end

	always @(posedge clk) begin
		tag_written <= tag_we;
		if (invalidate) begin
			sweeping <= 1'b1; sweep_index <= 0;
			if (busy || fill_req) discard <= 1'b1;
		end else if (sweeping) begin
			sweep_index <= sweep_index + 1'b1;
			if (&sweep_index) sweeping <= 1'b0;
		end
		if (fill_req && fill_start) begin
			fill_req <= 1'b0;
			busy <= 1'b1;
			words <= 6'd0;
		end
		if (busy) begin
			if (fill_valid && words < 6'd32) words <= words + 6'd1;
			if (fill_done) begin
				busy <= 1'b0;
				discard <= 1'b0;
				// a fill that completes during a sweep is dropped (its tag write is masked)
				if (sweeping && !invalidate) discard <= 1'b0;
			end
		end else if (start_fill) begin
			fill_req <= 1'b1;
			discard <= 1'b0;
			fill_index <= rd_index;
			pending_tag <= rd_addr[21:LINE_BITS+6];
			fill_addr <= {rd_addr[21:6], 6'd0};
		end
	end
endmodule
