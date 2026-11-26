// Steady audio output for the NES core. The NES clock is stalled for ROM reads and catches up
// later (nes_board_core), so APU samples arrive up to 2047 master clocks (95 us) late: played
// as they come, that is an audible 60 Hz pitch wobble (-8 to +3 % speed swings in every frame).
// Here 16 APU samples at a time are averaged (which also removes most of what would alias) and
// go through a 64-entry queue (one block RAM) read once per 192 nominal master clocks (111.7 kHz).
// The NES's lag (credit, at most 2047 clocks = 10.7 entries) only moves the queue level. Clocks
// the NES loses for good (credit full) become debt, repaid by playing a value twice at a rate
// proportional to the debt (debt/32768 per read), so a NES that keeps falling behind is followed
// at a steady, slightly lower pitch instead of in jumps. If the NES stops (save state, shell
// pause) the queue runs dry, the last value is held, and it fills to PRIME again.
module nes_audio_smooth(
	input             clk, reset,
	input             strobe,           // APU sample (one per 12 master clocks)
	input      [15:0] sample,
	input             nominal_clk,      // one pulse per nominal master clock (60.000 Hz frames)
	input             lost_clk,         // that clock was not granted to the NES (credit full)
	output reg [15:0] out = 16'd0,
	output reg        tick = 1'b0       // a value was played (debug)
);
	localparam [6:0] PRIME = 7'd40;
	reg [3:0] n = 4'd0; reg [19:0] sum = 20'd0;
	wire [19:0] sum_next = sum + {4'd0, sample};
	(* syn_ramstyle = "block_ram" *) reg [15:0] mem [0:63];
	reg [15:0] head = 16'd0;            // mem[rp], one cycle behind rp (reads are >= 192 cycles apart)
	reg [6:0] wp = 7'd0, rp = 7'd0;     // one extra bit: level = wp - rp, 0..64
	wire [6:0] level = wp - rp;
	reg running = 1'b0;
	reg [7:0] div = 8'd0;
	reg [13:0] debt = 14'd0;            // lost master clocks not yet repaid
	reg [14:0] frac = 15'd0;
	wire push = strobe && n == 4'd15 && !level[6];
	always @(posedge clk) begin
		if (push) mem[wp[5:0]] <= sum_next[19:4];
		head <= mem[rp[5:0]];
	end
	wire due = nominal_clk && div == 8'd191;
	wire [15:0] frac_next = {1'b0, frac} + {2'd0, debt};
	wire repay = frac_next[15] && debt >= 14'd192;
	always @(posedge clk) begin
		tick <= 1'b0;
		if (reset) begin
			n <= 4'd0; sum <= 20'd0; wp <= 7'd0; rp <= 7'd0; running <= 1'b0; div <= 8'd0;
			debt <= 14'd0; frac <= 15'd0;
		end else begin
			if (strobe) begin
				n <= n + 4'd1;
				sum <= n == 4'd15 ? 20'd0 : sum_next;
			end
			if (push) wp <= wp + 7'd1;
			if (nominal_clk) div <= div == 8'd191 ? 8'd0 : div + 8'd1;
			if (!running) begin
				running <= level >= PRIME; debt <= 14'd0; frac <= 15'd0;
			end else if (due) begin
				frac <= frac_next[14:0];
				if (repay) debt <= debt - 14'd192 + {13'd0, lost_clk};   // play the last value again
				else begin
					if (lost_clk && !(&debt)) debt <= debt + 14'd1;
					tick <= 1'b1;
					if (level == 7'd0) running <= 1'b0;
					else begin out <= head; rp <= rp + 7'd1; end
				end
			end else if (lost_clk && !(&debt)) debt <= debt + 14'd1;
		end
	end
endmodule
