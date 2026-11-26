// Multiplier-free Game Gear audio conditioning on hclk (16.777 MHz).
// Two cascaded one-pole low-pass stages (alpha = 1/8 at hclk/32 = 524 kHz,
// ~11 kHz each, ~7 kHz combined -3 dB, close to the GB/GBC core's 7.056 kHz
// filter) followed by a ~5 Hz DC blocker. Input/output: signed 16-bit.
// bass_cut (Master System on the speaker): the DC blocker becomes a ~160 Hz high-pass.
// Master System music is mono square waves with bass lines around 110-130 Hz; the
// speaker buzzes on them (clean in headphones). About -5 dB at 110 Hz, -1 dB at
// 330 Hz, so the bass line is still heard. (A second stage did not fit the chip.)
module gg_audio_lpf(
    input  wire        clk,
    input  wire        reset,
    input  wire        bass_cut,
    input  wire [15:0] in_l,
    input  wire [15:0] in_r,
    output wire [15:0] out_l,
    output wire [15:0] out_r
);
    reg [4:0] div;
    wire ce = (div == 5'd0);
    always @(posedge clk or posedge reset)
        if (reset) div <= 5'd0; else div <= div + 1'd1;

    gg_audio_lpf_ch u_l(.clk(clk), .reset(reset), .ce(ce), .bass_cut(bass_cut), .din(in_l), .dout(out_l));
    gg_audio_lpf_ch u_r(.clk(clk), .reset(reset), .ce(ce), .bass_cut(bass_cut), .din(in_r), .dout(out_r));
endmodule

module gg_audio_lpf_ch(
    input  wire        clk,
    input  wire        reset,
    input  wire        ce,
    input  wire        bass_cut,
    input  wire [15:0] din,
    output reg  [15:0] dout
);
    // 8 fractional bits; 26-bit signed state covers +-2^23 with headroom.
    reg signed [25:0] a1, a2, dc;
    wire signed [25:0] x  = {{2{din[15]}}, din, 8'd0};
    wire signed [25:0] d1 = x  - a1;
    wire signed [25:0] d2 = a1 - a2;
    wire signed [25:0] d3 = a2 - dc;
    wire signed [25:0] y  = a2 - dc;
    wire signed [17:0] y16 = y[25:8];
    always @(posedge clk or posedge reset) begin
        if (reset) begin
            a1 <= 26'sd0; a2 <= 26'sd0; dc <= 26'sd0; dout <= 16'd0;
        end else if (ce) begin
            a1 <= a1 + (d1 >>> 3);
            a2 <= a2 + (d2 >>> 3);
            dc <= dc + (bass_cut ? (d3 >>> 9) : (d3 >>> 14));
            dout <= (y16 >  18'sd32767) ? 16'h7fff :
                    (y16 < -18'sd32768) ? 16'h8000 : y16[15:0];
        end
    end
endmodule
