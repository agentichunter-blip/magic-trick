// Shared CPU/snapshot storage for both palettes, plus independent pixel copies.
// CPU reads are used only outside snapshot addresses; execution is paused during
// snapshot transfers. The packed layout is unchanged (BG followed by OBJ).
module snapshot_palettes_ram #(parameter BASE=60,OFFSET=133)(
 input clk,cpu_we,cpu_bank,input[5:0] cpu_addr,input[7:0] cpu_data,
 input[5:0] bg_pixel_addr,ob_pixel_addr,output[7:0] cpu_q,
 output[15:0] bg_pixel_q,ob_pixel_q,
 input[9:0] ss_addr,input ss_we,input[63:0] ss_data,output[63:0] ss_q,
 output[31:0] bg_colors,ob_colors,output[1023:0] debug_bytes);
`ifdef __ICARUS__
 reg[7:0] mem[0:127];
 initial for(integer i=0;i<128;i=i+1)mem[i]=0;
 always @(posedge clk)begin
  if(cpu_we)mem[{cpu_bank,cpu_addr}]<=cpu_data;
  for(integer i=0;i<128;i=i+1)for(integer j=0;j<8;j=j+1)
   if(ss_we && ss_addr==BASE+(OFFSET+i*8+j)/64)mem[i][j]<=ss_data[(OFFSET+i*8+j)%64];
 end
 assign cpu_q=mem[{cpu_bank,cpu_addr}];
 assign bg_pixel_q={mem[{1'b0,bg_pixel_addr[5:1],1'b1}],mem[{1'b0,bg_pixel_addr[5:1],1'b0}]};
 assign ob_pixel_q={mem[{1'b1,ob_pixel_addr[5:1],1'b1}],mem[{1'b1,ob_pixel_addr[5:1],1'b0}]};
 assign bg_colors={mem[5],mem[4],mem[3],mem[2]};
 assign ob_colors={mem[77],mem[76],mem[75],mem[74]};
 reg[63:0] state_q;
 always @* begin
  state_q=0;
  for(integer i=0;i<128;i=i+1)for(integer j=0;j<8;j=j+1)
   if(ss_addr==BASE+(OFFSET+i*8+j)/64)state_q[(OFFSET+i*8+j)%64]=mem[i][j];
 end
 assign ss_q=state_q;
 for(genvar i=0;i<128;i=i+1)assign debug_bytes[i*8+:8]=mem[i];
`else
 wire[7:0] cpu_lanes[0:7],bg_lanes[0:7],ob_lanes[0:7];
 wire[63:0] read_bits;
 for(genvar lane=0;lane<8;lane=lane+1)begin: lanes
  for(genvar segment=0;segment<(lane==7 ? 3 : 2);segment=segment+1)begin: groups
   localparam START=lane==7 ? (segment==0 ? 0 : segment==1 ? 3 : 7) : segment*4;
   localparam WIDTH=lane==7 ? (segment==0 ? 3 : segment==1 ? 4 : 1) : 4;
   localparam WORD=BASE+(OFFSET+lane*8+START)/64;
   localparam BITPOS=(OFFSET+lane*8+START)%64;
   wire selected=ss_addr>=WORD && ss_addr<WORD+16;
   wire[3:0] state_row=ss_addr-WORD;
   wire state_write=ss_we && selected;
   wire we=state_write || (cpu_we && cpu_addr[2:0]==lane);
   wire[3:0] wa=state_write ? state_row : {cpu_bank,cpu_addr[5:3]};
   wire[3:0] data_in=state_write ? {{(4-WIDTH){1'b0}},ss_data[BITPOS+:WIDTH]} : {{(4-WIDTH){1'b0}},cpu_data[START+:WIDTH]};
   wire[3:0] ra=selected ? state_row : {cpu_bank,cpu_addr[5:3]};
   wire[3:0] cq,bq,oq;
   RAM16SDP4 cpu_ram(.DO(cq),.DI(data_in),.WAD(wa),.RAD(ra),.WRE(we),.CLK(clk));
   RAM16SDP4 bg_ram(.DO(bq),.DI(data_in),.WAD({1'b0,wa[2:0]}),.RAD({1'b0,bg_pixel_addr[5:3]}),.WRE(we && !wa[3]),.CLK(clk));
   RAM16SDP4 ob_ram(.DO(oq),.DI(data_in),.WAD({1'b0,wa[2:0]}),.RAD({1'b0,ob_pixel_addr[5:3]}),.WRE(we && wa[3]),.CLK(clk));
   assign cpu_lanes[lane][START+:WIDTH]=cq[WIDTH-1:0];
   assign bg_lanes[lane][START+:WIDTH]=bq[WIDTH-1:0];
   assign ob_lanes[lane][START+:WIDTH]=oq[WIDTH-1:0];
   assign read_bits[BITPOS+:WIDTH]=selected ? cq[WIDTH-1:0] : {WIDTH{1'b0}};
`ifdef VERILATOR
   for(genvar row=0;row<16;row=row+1)begin: debug_rows
    wire[3:0] stored={cpu_ram.mem3[row],cpu_ram.mem2[row],cpu_ram.mem1[row],cpu_ram.mem0[row]};
    assign debug_bytes[(row*8+lane)*8+START+:WIDTH]=stored[WIDTH-1:0];
   end
`endif
  end
 end
 assign ss_q=read_bits;
 assign cpu_q=cpu_lanes[cpu_addr[2:0]];
 assign bg_pixel_q={bg_lanes[{bg_pixel_addr[2:1],1'b1}],bg_lanes[{bg_pixel_addr[2:1],1'b0}]};
 assign ob_pixel_q={ob_lanes[{ob_pixel_addr[2:1],1'b1}],ob_lanes[{ob_pixel_addr[2:1],1'b0}]};
 reg[63:0] colors=0;
 for(genvar bank=0;bank<2;bank=bank+1)begin: colors_bank
  for(genvar i=0;i<4;i=i+1)begin: colors_byte
   for(genvar j=0;j<8;j=j+1)begin: colors_bit
    localparam ADDR=bank==0 ? 2+i : 74+i;
    localparam PACKED=OFFSET+ADDR*8+j;
    always @(posedge clk)begin
     if(cpu_we && {cpu_bank,cpu_addr}==ADDR)colors[bank*32+i*8+j]<=cpu_data[j];
     if(ss_we && ss_addr==BASE+PACKED/64)colors[bank*32+i*8+j]<=ss_data[PACKED%64];
    end
   end
  end
 end
 assign bg_colors=colors[31:0];assign ob_colors=colors[63:32];
`ifndef VERILATOR
 assign debug_bytes=0;
`endif
`endif
endmodule
