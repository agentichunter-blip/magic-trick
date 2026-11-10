// Dual-clock, one-cycle read line storage. Native implementation uses one BSRAM.
module snapshot_line_ram(input wclk, rclk, we,
    input [14:0] wa, ra, input [15:0] data, output [15:0] q);
`ifdef __ICARUS__
    reg [15:0] mem[0:159];
    reg [15:0] qr;
    integer i;
    initial for(i=0;i<160;i=i+1) mem[i]=0;
    always @(posedge wclk) if(we) mem[wa]<=data;
    always @(posedge rclk) qr<=mem[ra];
    assign q=qr;
`else
    wire [15:0] unused_q;
    DPB #(.READ_MODE0(1'b0),.READ_MODE1(1'b0),
        .WRITE_MODE0(2'b00),.WRITE_MODE1(2'b00),
        .BIT_WIDTH_0(16),.BIT_WIDTH_1(16),
        .BLK_SEL_0(3'b000),.BLK_SEL_1(3'b000),.RESET_MODE("SYNC")) ram(
        .DOA(unused_q),.DOB(q),.DIA(data),.DIB(16'd0),
        .BLKSELA(3'b000),.BLKSELB(3'b000),
        .ADA({2'b00,wa[7:0],4'b0011}),.ADB({2'b00,ra[7:0],4'b0000}),
        .WREA(we && wa<160),.WREB(1'b0),.CLKA(wclk),.CLKB(rclk),
        .CEA(1'b1),.CEB(1'b1),.OCEA(1'b0),.OCEB(1'b0),
        .RESETA(1'b0),.RESETB(1'b0));
`endif
endmodule
