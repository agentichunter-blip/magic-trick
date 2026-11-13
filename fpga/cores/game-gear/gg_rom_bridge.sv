// Read-only ROM cache on the EXISTING xVirtual PSRAM controller port.
// Direct-mapped, 64-byte lines: 32 lines = 2 KiB (1024 x 16-bit words, one
// BSRAM; 128 lines/8 KiB before 32 KiB cartridge RAM needed the blocks). In
// Sonic gameplay the CPU waits ~13.1k xclk per frame against ~12.2k with
// 8 KiB (of 1.12M per frame). Tags are read one cycle after the address, like the data. Replaces the one-line cache, which missed on nearly
// every access whenever code and data lived in different 64-byte lines and
// stalled the CPU in bursts (warped CPU-timed audio such as sample playback).
// Bus protocol, timeout/fault and invalidate/discard semantics are unchanged:
// one 64-byte read burst per miss; never drives PSRAM pins.
module gg_rom_bridge #(parameter integer TIMEOUT_CYCLES=262144, parameter integer LINE_BITS=5)(
 input wire clk, reset, enable, invalidate,
 input wire read, input wire [23:0] address,
 output wire [7:0] data, output wire ready,
 output wire idle, output reg fault,
 output reg request, output wire bus_read,
 output reg [22:0] bus_address, output wire [15:0] bus_write_data,
 output wire [10:0] bus_length,
 input wire done, valid, input wire [15:0] read_data);
 localparam LINES=1<<LINE_BITS, IB=LINE_BITS, TB=24-6-LINE_BITS; // index/tag widths
 reg busy,discard,just_filled;
 reg [IB-1:0] fill_index;
 reg [TB-1:0] pending_tag;
 reg [5:0] words;
 reg [31:0] timeout_count;
 reg [LINES-1:0] line_valid;
 // 32 x 13 bits in flip-flops (this FPGA has no logic-cell RAM): frees the
 // tags' block RAM for cartridge RAM.
 (* syn_ramstyle="registers" *) reg [TB-1:0] tags[0:LINES-1];
 reg [15:0] cmem[0:LINES*32-1];

 wire in_range=address[23:22]==0; // ROM region 0x020000..0x41ffff, max 4 MiB

 // Registered read path (block RAM). Data is valid for the address sampled
 // on the previous edge; ready also requires that address to be current.
 // The tag is read the same way, so a hit is judged for the registered
 // address; a line is unusable for the cycle in which its tag is written.
 reg [15:0] rd_word;
 reg [23:0] rd_addr;
 reg [TB-1:0] rd_tag;
 always @(posedge clk) begin
  rd_word<=cmem[address[IB+5:1]];
  rd_addr<=address;
  rd_tag<=tags[address[IB+5:6]];
 end
 wire rd_current=rd_addr==address;
 wire [IB-1:0] rd_index=rd_addr[IB+5:6];
 wire rd_hit=line_valid[rd_index] && rd_tag==rd_addr[23:IB+6] && !just_filled;
 // Ready one cycle after the hit is seen: the byte the CPU latches has then
 // been stable for two cycles, which the constraints file relies on.
 reg rd_settled;
 always @(posedge clk) rd_settled<=rd_current && rd_hit && read;
 assign data=!in_range ? 8'hff : rd_addr[0] ? rd_word[15:8] : rd_word[7:0];
 assign ready=!read || (enable && !invalidate && !fault &&
   (!in_range || (rd_current && rd_hit && rd_settled)));
 assign idle=!busy && !request;
 assign bus_read=1'b1;
 assign bus_write_data=0;
 assign bus_length=64;

 always @(posedge clk) begin
  if(busy && valid && words<32) cmem[{fill_index,words[4:0]}]<=read_data;
  // Tag RAM write port: the line's tag is stored when its fill completes.
  if(busy && done && words+(valid ? 6'd1 : 6'd0)==32 && !discard && !invalidate && enable && !fault)
   tags[fill_index]<=pending_tag;
 end

 always @(posedge clk or posedge reset) begin
  if(reset) begin busy<=0;line_valid<=0;discard<=0;fill_index<=0;pending_tag<=0;
   words<=0;timeout_count<=0;request<=0;bus_address<=23'h020000;fault<=0;just_filled<=0;end
  else begin
   request<=0;just_filled<=0;
   if(invalidate || !enable) begin line_valid<=0;if(busy) discard<=1;end
   if(busy) begin
    timeout_count<=timeout_count+1'b1;
    if(valid) begin
     if(words<32) words<=words+1'b1;
     else fault<=1;
    end
    if(done) begin
     busy<=0;
     if(words+(valid ? 6'd1 : 6'd0)!=32) begin fault<=1;line_valid<=0;end
     else if(!discard && !invalidate && enable && !fault) begin
      line_valid[fill_index]<=1'b1;
     end
     just_filled<=1'b1;
    end else if(timeout_count==TIMEOUT_CYCLES-1) begin
     // Remain busy until reset/done; never issue a competing request.
     fault<=1;line_valid<=0;
    end
   end else if(enable && !invalidate && !fault && read && in_range && rd_current && !rd_hit && !just_filled) begin
    busy<=1;discard<=0;words<=0;timeout_count<=0;
    line_valid[rd_index]<=1'b0;fill_index<=rd_index;pending_tag<=rd_addr[23:IB+6];
    bus_address<=23'h020000+{rd_addr[21:6],6'b0};request<=1;
   end
  end
 end
endmodule
