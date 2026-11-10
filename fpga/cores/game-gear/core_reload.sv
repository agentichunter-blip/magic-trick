// Dedicated UART 0x11: "CORE", op (a1 arm / a2 commit), target,
// nonce big-endian, one's complement nonce. Only allowlisted slots can boot.
module core_reload #(
    parameter integer ARM_CYCLES = 8388608,
    parameter integer SETTLE_CYCLES = 838861,
    parameter integer PULSE_CYCLES = 64
)(
    input clk, reset, allowed,
    input rx_valid,
    input [6:0] rx_address,
    input [7:0] rx_length,
    input [79:0] rx_data,
    output reload_n,
    output hold_core,
    output [23:0] boot_address
);
    localparam IDLE=3'd0, ARMED=3'd1, SETTLE=3'd2, PULSE=3'd3, DONE=3'd4;
    localparam integer MAX_COUNT = ARM_CYCLES > SETTLE_CYCLES ? ARM_CYCLES : SETTLE_CYCLES;
    localparam integer WIDTH = $clog2(MAX_COUNT + PULSE_CYCLES + 1);
    reg [2:0] state = IDLE;
    reg [WIDTH-1:0] count = 0;
    reg target = 0;
    reg [15:0] nonce = 0;
    wire packet = rx_valid && rx_address == 7'h11 && rx_length == 10 &&
        rx_data[79:48] == 32'h434f5245 && rx_data[39:33] == 0 &&
        rx_data[15:0] == ~rx_data[31:16];
    assign boot_address = target ? 24'h100000 : 24'h000000;
    assign hold_core = !reset && (state == SETTLE || state == PULSE || state == DONE);
    assign reload_n = reset || state != PULSE;
    always @(posedge clk or posedge reset) begin
        if (reset) begin state <= IDLE; count <= 0; target <= 0; nonce <= 0; end
        else case (state)
            IDLE: if (packet && allowed && rx_data[47:40] == 8'ha1) begin
                target <= rx_data[32]; nonce <= rx_data[31:16]; count <= 0; state <= ARMED;
            end
            ARMED: begin
                if (!allowed || count == ARM_CYCLES-1) begin state <= IDLE; count <= 0; end
                else if (packet && rx_data[47:40] == 8'ha2 &&
                         rx_data[32] == target && rx_data[31:16] == nonce) begin
                    state <= SETTLE; count <= 0;
                end else count <= count + 1'b1;
            end
            SETTLE: begin
                if (!allowed) begin state <= IDLE; count <= 0; end
                else if (count == SETTLE_CYCLES-1) begin state <= PULSE; count <= 0; end
                else count <= count + 1'b1;
            end
            PULSE: if (count == PULSE_CYCLES-1) begin state <= DONE; count <= 0; end
                   else count <= count + 1'b1;
            DONE: ; // one shot; keep game isolated if the physical reload fails
            default: state <= IDLE;
        endcase
    end
endmodule
