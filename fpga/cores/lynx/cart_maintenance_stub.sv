// Atari Lynx core: stands in for esp32t's cart_maintenance_engine (the
// physical Game Boy cartridge slot: backups, flashing, ChroMagician SD
// downloads). The Lynx core has no logic to spare for it (routing), and none
// of that is a Lynx function: every request is answered at once, a ping
// reports no cartridge and not active, everything else "no cartridge". The
// MCU then treats the slot as empty while a Lynx game runs; the GB/GBC and
// Game Gear cores keep the full engine.
module cart_maintenance_engine #(
    parameter integer ADDRESS_SETUP_CYCLES = 2, READ_STROBE_CYCLES = 4,
    parameter integer WRITE_LOW_CYCLES = 2, WRITE_CLOCK_HIGH_CYCLES = 1,
    parameter integer WRITE_RECOVERY_CYCLES = 2, READ_RECOVERY_CYCLES = 1,
    parameter integer WATCHDOG_CYCLES = 83_886_080, FLASH_POLL_CYCLES = 838_861
)(
    input  wire        clk, reset, cart_present,
    input  wire        request_valid, output wire request_ready,
    input  wire [2:0]  request_operation, input wire [7:0] request_tag,
    input  wire [15:0] request_address, input wire [7:0] request_value,
    input  wire [15:0] request_aux_address, input wire [7:0] request_aux_value,
    output reg         response_valid, input wire response_ready,
    output reg  [2:0]  response_operation, output reg [7:0] response_tag,
    output reg  [3:0]  response_status, output reg [2:0] response_count,
    output reg  [31:0] response_data,
    output wire        block_write, output wire [10:0] block_write_address,
    output wire [7:0]  block_write_data, input wire [7:0] block_read_data,
    output wire [1:0]  block_ready, output wire [15:0] block_sequence,
    output wire [63:0] block_crc32, output wire pc_usb_mode,
    output wire        stream_event_valid, input wire stream_event_ready,
    output wire [7:0]  stream_event_sequence, output wire [31:0] stream_event_crc32,
    input  wire        stream_ack_valid, input wire [7:0] stream_ack_sequence,
    output wire        active, input wire ownership_ack,
    output wire [15:0] cart_address, output wire cart_clock, output wire cart_cs_n,
    output wire        cart_read_n, output wire cart_write_n,
    output wire [7:0]  cart_data_out, output wire cart_data_oe, input wire [7:0] cart_data_in
);
    localparam [2:0] OP_PING = 3'h0;
    localparam [3:0] STATUS_OK = 4'h0, STATUS_NO_CARTRIDGE = 4'h5;
    assign request_ready = !response_valid;
    always @(posedge clk or posedge reset) begin
        if (reset) begin
            response_valid <= 1'b0; response_operation <= 3'd0; response_tag <= 8'd0;
            response_status <= STATUS_OK; response_count <= 3'd0; response_data <= 32'd0;
        end else begin
            if (response_valid && response_ready) response_valid <= 1'b0;
            if (request_valid && !response_valid) begin
                response_valid <= 1'b1;
                response_operation <= request_operation;
                response_tag <= request_tag;
                if (request_operation == OP_PING) begin
                    // Same layout as the engine: protocol 3, block size log2 10,
                    // cartridge absent (bit 1), not active (bit 0).
                    response_status <= STATUS_OK; response_count <= 3'd4;
                    response_data <= {8'd10, 8'd3, 6'd0, 1'b0, 1'b0, 8'h00};
                end else begin
                    response_status <= STATUS_NO_CARTRIDGE; response_count <= 3'd0;
                    response_data <= 32'd0;
                end
            end
        end
    end
    assign block_write = 1'b0; assign block_write_address = 11'd0; assign block_write_data = 8'd0;
    assign block_ready = 2'b00; assign block_sequence = 16'd0; assign block_crc32 = 64'd0;
    assign pc_usb_mode = 1'b0; assign stream_event_valid = 1'b0;
    assign stream_event_sequence = 8'd0; assign stream_event_crc32 = 32'd0;
    assign active = 1'b0;
    assign cart_address = 16'hffff; assign cart_clock = 1'b0; assign cart_cs_n = 1'b1;
    assign cart_read_n = 1'b1; assign cart_write_n = 1'b1;
    assign cart_data_out = 8'd0; assign cart_data_oe = 1'b0;
endmodule
