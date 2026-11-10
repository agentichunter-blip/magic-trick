// Existing LCD/UVC line buffer, explicitly packed into three 16-bit Gowin DPBs.
// This is output staging RAM; native restoration rebuilds the displayed frame.
module snapshot_display_ram(
 input wclk,rclk,we,input[9:0] wa,ra,input[35:0] data,output[35:0] q
);
`ifdef __ICARUS__
 reg[35:0] mem[0:1023];reg[35:0] qr;
 always @(posedge wclk) if(we) mem[wa]<=data;
 always @(posedge rclk) qr<=mem[ra];
 assign q=qr;
`else
 wire[47:0] din={12'd0,data};wire[47:0] dout;
 for(genvar i=0;i<3;i=i+1)begin: banks
  DPB #(.READ_MODE0(1'b0),.READ_MODE1(1'b0),.WRITE_MODE0(2'b00),.WRITE_MODE1(2'b00),
        .BIT_WIDTH_0(16),.BIT_WIDTH_1(16),.BLK_SEL_0(3'b000),.BLK_SEL_1(3'b000),.RESET_MODE("SYNC")) ram(
   .DOA(),.DOB(dout[i*16+:16]),.DIA(din[i*16+:16]),.DIB(16'd0),.BLKSELA(3'b000),.BLKSELB(3'b000),
   .ADA({wa,4'b0011}),.ADB({ra,4'b0000}),.WREA(we),.WREB(1'b0),.CLKA(wclk),.CLKB(rclk),
   .CEA(1'b1),.CEB(1'b1),.OCEA(1'b0),.OCEB(1'b0),.RESETA(1'b0),.RESETB(1'b0));
 end
 assign q=dout[35:0];
`endif
endmodule
