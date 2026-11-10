// dpramV.v

module dpramV #(
    parameter   addr_width  =   8,
    parameter   data_width  =   8
    )(  
    input   clock_a,
    input   ce_a,
    input   [addr_width-1:0]    address_a,
    input   [data_width-1:0]    data_a,
    input                       wren_a,
    output  reg [data_width-1:0]    q_a,
    
    input   clock_b,
    input   [addr_width-1:0]    address_b,
    input   [data_width-1:0]    data_b,
    input                       wren_b,
    output  reg [data_width-1:0]    q_b
);

`ifndef __ICARUS__
    // 8-bit core RAM split into 2 KiB banks. Preserve both read-enable holds and
    // the original A-write priority, including B writes when A's CE is low.
    localparam BANKS = addr_width > 11 ? (1 << (addr_width-11)) : 1;
    wire [14:0] aa = address_a;
    wire [14:0] ab = address_b;
    reg [3:0] read_bank_a = 0;
    reg [3:0] read_bank_b = 0;
    wire [7:0] bank_a [0:BANKS-1];
    wire [7:0] bank_b [0:BANKS-1];
    always @(posedge clock_a) if(ce_a && !wren_a) read_bank_a<=aa[14:11];
    always @(posedge clock_b) if(!wren_b) read_bank_b<=ab[14:11];
    always @* begin q_a=bank_a[read_bank_a];q_b=bank_b[read_bank_b];end
    for(genvar i=0;i<BANKS;i=i+1) begin: ram_banks
      wire [15:0] qa,qb;
      DPB #(.READ_MODE0(1'b0),.READ_MODE1(1'b0),.WRITE_MODE0(2'b00),.WRITE_MODE1(2'b00),
        .BIT_WIDTH_0(8),.BIT_WIDTH_1(8),.BLK_SEL_0(3'b000),.BLK_SEL_1(3'b000),.RESET_MODE("SYNC")) ram(
        .DOA(qa),.DOB(qb),.DIA({8'd0,data_a}),.DIB({8'd0,data_b}),
        .BLKSELA(3'b000),.BLKSELB(3'b000),.ADA({aa[10:0],3'b000}),.ADB({ab[10:0],3'b000}),
        .WREA(wren_a),.WREB(wren_b && !(wren_a && ce_a)),.CLKA(clock_a),.CLKB(clock_b),
        .CEA(ce_a && aa[14:11]==i),.CEB(ab[14:11]==i && !(wren_b && wren_a && ce_a)),
        .OCEA(1'b0),.OCEB(1'b0),.RESETA(1'b0),.RESETB(1'b0));
      assign bank_a[i]=qa[7:0];assign bank_b[i]=qb[7:0];
    end
`else
    reg [data_width-1:0]    dpram [2**addr_width-1:0];
    
    always@(posedge clock_a)
        if(wren_a&ce_a)
            dpram[address_a]    <=  data_a;
            
    always@(posedge clock_a)
        if((~wren_a) & ce_a)
            q_a <=  dpram[address_a];
            

    always@(posedge clock_b)
        if(wren_b & ~(wren_a & ce_a))
            dpram[address_b]    <=  data_b;
            
    always@(posedge clock_b)
        if(~wren_b)
            q_b <=  dpram[address_b];





`endif
endmodule
