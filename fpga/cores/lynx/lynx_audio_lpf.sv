// Lynx audio conditioning on hclk (16.777 MHz): the Game Gear core's low-pass
// (gg_audio_lpf.sv: two one-pole stages, alpha 1/8 at hclk/32, ~7 kHz
// combined), with one set of adders shared by the left and right channels on
// alternate cycles (the filter steps once per 32 clocks, so there is time).
// No DC blocker: the Lynx mixes signed channels around zero, silence is 0.
// Signed 16-bit in/out.
module lynx_audio_lpf(
    input  wire        clk,
    input  wire        reset,
    input  wire [15:0] in_l,
    input  wire [15:0] in_r,
    output reg  [15:0] out_l,
    output reg  [15:0] out_r
);
    reg [4:0] div;
    always @(posedge clk or posedge reset)
        if (reset) div <= 5'd0; else div <= div + 1'd1;
    wire step_l = div == 5'd0, step_r = div == 5'd1;
    reg signed [25:0] a1_l, a2_l, a1_r, a2_r;
    wire right = step_r;
    wire [15:0] din = right ? in_r : in_l;
    wire signed [25:0] a1 = right ? a1_r : a1_l;
    wire signed [25:0] a2 = right ? a2_r : a2_l;
    wire signed [25:0] x  = {{2{din[15]}}, din, 8'd0};
    wire signed [25:0] n1 = a1 + ((x  - a1) >>> 3);
    wire signed [25:0] n2 = a2 + ((a1 - a2) >>> 3);
    wire signed [17:0] y16 = a2[25:8];
    wire [15:0] sat = (y16 >  18'sd32767) ? 16'h7fff :
                      (y16 < -18'sd32768) ? 16'h8000 : y16[15:0];
    always @(posedge clk or posedge reset) begin
        if (reset) begin
            a1_l <= 0; a2_l <= 0; a1_r <= 0; a2_r <= 0;
            out_l <= 16'd0; out_r <= 16'd0;
        end else if (step_l) begin
            a1_l <= n1; a2_l <= n2; out_l <= sat;
        end else if (step_r) begin
            a1_r <= n1; a2_r <= n2; out_r <= sat;
        end
    end
endmodule
