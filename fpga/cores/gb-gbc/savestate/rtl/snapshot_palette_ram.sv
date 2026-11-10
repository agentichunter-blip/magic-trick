// Eight byte lanes allow a whole snapshot word to be transferred in one cycle.
// Two asynchronous read copies preserve concurrent CPU and pixel access.
module snapshot_palette_ram #(
 parameter BASE=60, OFFSET=133, GPD_FIRST=2
)(input clk,input cpu_we,input[5:0] cpu_addr,input[7:0] cpu_data,
 input[5:0] pixel_addr,output[7:0] cpu_q,output[15:0] pixel_q,
 input[9:0] ss_addr,input ss_we,input[63:0] ss_data,output[63:0] ss_q,
 output[31:0] gpd,output[511:0] debug_bytes);
`ifdef __ICARUS__
 reg[7:0] mem[0:63];
 initial for(integer i=0;i<64;i=i+1) mem[i]=0;
 always @(posedge clk) begin
  if(cpu_we) mem[cpu_addr]<=cpu_data;
  for(integer i=0;i<64;i=i+1)for(integer j=0;j<8;j=j+1)
   if(ss_we && ss_addr==BASE+(OFFSET+i*8+j)/64)
    mem[i][j]<=ss_data[(OFFSET+i*8+j)%64];
 end
 assign cpu_q=mem[cpu_addr];
 assign pixel_q={mem[{pixel_addr[5:1],1'b1}],mem[{pixel_addr[5:1],1'b0}]};
 assign gpd={mem[GPD_FIRST+3],mem[GPD_FIRST+2],mem[GPD_FIRST+1],mem[GPD_FIRST]};
 reg[63:0] state_q;
 always @* begin
  state_q=0;
  for(integer i=0;i<64;i=i+1)for(integer j=0;j<8;j=j+1)
   if(ss_addr==BASE+(OFFSET+i*8+j)/64)state_q[(OFFSET+i*8+j)%64]=mem[i][j];
 end
 assign ss_q=state_q;
 for(genvar i=0;i<64;i=i+1) assign debug_bytes[i*8+:8]=mem[i];
`else
 wire[7:0] cpu_lanes[0:7],pixel_lanes[0:7];
 wire[63:0] read_bits;
 for(genvar lane=0;lane<8;lane=lane+1)begin: lanes
  for(genvar segment=0;segment<(lane==7 ? 3 : 2);segment=segment+1)begin: groups
   localparam START=lane==7 ? (segment==0 ? 0 : segment==1 ? 3 : 7) : segment*4;
   localparam WIDTH=lane==7 ? (segment==0 ? 3 : segment==1 ? 4 : 1) : 4;
   localparam WORD=BASE+(OFFSET+lane*8+START)/64;
   localparam BITPOS=(OFFSET+lane*8+START)%64;
   wire selected=ss_addr>=WORD && ss_addr<WORD+8;
   wire[2:0] state_row=ss_addr-WORD;
   wire state_write=ss_we && selected;
   wire we=state_write || (cpu_we && cpu_addr[2:0]==lane);
   wire[3:0] wa={1'b0,state_write ? state_row : cpu_addr[5:3]};
   wire[3:0] data_in=state_write ? {{(4-WIDTH){1'b0}},ss_data[BITPOS+:WIDTH]} : {{(4-WIDTH){1'b0}},cpu_data[START+:WIDTH]};
   wire[3:0] ra={1'b0,selected ? state_row : cpu_addr[5:3]};
   wire[3:0] cq,pq;
   RAM16SDP4 cpu_ram(.DO(cq),.DI(data_in),.WAD(wa),.RAD(ra),.WRE(we),.CLK(clk));
   RAM16SDP4 pixel_ram(.DO(pq),.DI(data_in),.WAD(wa),.RAD({1'b0,pixel_addr[5:3]}),.WRE(we),.CLK(clk));
   assign cpu_lanes[lane][START+:WIDTH]=cq[WIDTH-1:0];
   assign pixel_lanes[lane][START+:WIDTH]=pq[WIDTH-1:0];
`ifdef VERILATOR
   for(genvar row=0;row<8;row=row+1)begin: debug_rows
    wire[3:0] stored={cpu_ram.mem3[row],cpu_ram.mem2[row],cpu_ram.mem1[row],cpu_ram.mem0[row]};
    assign debug_bytes[(row*8+lane)*8+START+:WIDTH]=stored[WIDTH-1:0];
   end
`endif
   assign read_bits[BITPOS+:WIDTH]=selected ? cq[WIDTH-1:0] : {WIDTH{1'b0}};
  end
 end
 assign ss_q=read_bits;
 assign cpu_q=cpu_lanes[cpu_addr[2:0]];
 assign pixel_q={pixel_lanes[{pixel_addr[2:1],1'b1}],pixel_lanes[{pixel_addr[2:1],1'b0}]};
 reg[31:0] colors=0;
 for(genvar i=0;i<4;i=i+1)begin: colors_copy
  for(genvar j=0;j<8;j=j+1)begin: bits
   localparam PACKED=OFFSET+(GPD_FIRST+i)*8+j;
   always @(posedge clk)begin
    if(cpu_we && cpu_addr==GPD_FIRST+i)colors[i*8+j]<=cpu_data[j];
    if(ss_we && ss_addr==BASE+PACKED/64)colors[i*8+j]<=ss_data[PACKED%64];
   end
  end
 end
 assign gpd=colors;
`ifndef VERILATOR
 assign debug_bytes=0;
`endif
`endif
endmodule
