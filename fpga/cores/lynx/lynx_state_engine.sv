// Atari Lynx save states on xclk, driven by the same MCU commands as the Game
// Gear engine (top.v: 10 freeze, 14 block, 11 load, 16 release, 12 status).
//
// The state itself is MiSTer's (lynx-rtl/savestates.vhd): a 66,560-byte image
// (65 blocks of 1 KiB) made of a header (bytes 4-7 = 0x00004100, the size in
// 32-bit words), the core's internal registers, the 512-byte register shadow
// and the 64 KiB RAM. The core reads and writes it over a 64-bit bus; here
// that bus is PSRAM at ROM-window address 0x200000 (PSRAM 0x220000), above
// any Lynx cartridge (at most 1 MiB, at 0x1000).
//
// FREEZE: the core saves itself into the staging area (it waits for an idle
//   bus, then sleeps while saving), and is then held paused ("frozen").
// BLOCK n: copies staging block n into the 1 KiB buffer the MCU reads.
// LOAD (frozen): the MCU has staged a payload at the same address; the core
//   checks its size word, resets and loads itself, and stays paused.
// RELEASE: the game continues.
// Diagnostics: BLOCK 64-108 reads the panel framebuffer instead (PSRAM
// 0x10000, 160x144 BGR555 words, 320-byte rows: 45 KiB), any time.
module lynx_state_engine #(parameter [23:0] STAGE=24'h200000, parameter integer BLOCKS=65)(
 input clk, reset,
 input freeze_toggle, release_toggle, load_toggle, block_toggle,
 input [6:0] block_number, input [15:0] block_sequence,
 output reg block_ready=0, output reg [15:0] block_ready_sequence=0,
 output busy, output reg fault=0, output reg frozen=0, output reg loaded=0,
 output saving, // the core may run (to reach an idle bus) even if paused
 // core
 output reg save_req=0, output reg load_req=0,
 input state_loaded, input state_busy,
 input [63:0] ss_din, output reg [63:0] ss_dout=0, input [25:0] ss_adr,
 input ss_rnw, input ss_ena, output reg ss_done=0,
 // ROM cache read port (while owns_rom)
 output owns_rom, output reg cache_invalidate=0,
 output reg rom_read=0, output reg [23:0] rom_address=0,
 input [7:0] rom_data, input rom_ready, input rom_fault, input rom_idle,
 // PSRAM write port (while owns_bus)
 output reg owns_bus=0, output reg x_request=0, output reg [22:0] x_address=0,
 output [15:0] x_write_data, input x_write_next, input x_done,
 // MCU block buffer
 input qclk, input [9:0] qaddress, output [7:0] qdata
);
 localparam [3:0] IDLE=0,SAVE_START=1,RUN=2,BUS_WRITE_WAIT=3,BUS_WRITE=4,BUS_READ=5,
  BLOCK_READ=6,LOAD_START=7;
 reg [3:0] state=IDLE, resume=IDLE;
 reg loading=0;
 assign busy=state!=IDLE;
 assign saving=state==SAVE_START || (state!=IDLE && !loading && !frozen);
 assign owns_rom=state!=IDLE;

 (* ASYNC_REG="TRUE" *) reg [3:0] t_meta=0,t_sync=0;
 reg [3:0] t_seen=0;
 always @(posedge clk) begin t_meta<={block_toggle,load_toggle,release_toggle,freeze_toggle};t_sync<=t_meta;end
 wire [3:0] t_edge=t_sync^t_seen;

 // 1 KiB buffer: 512 x 16-bit words, written here, read by QSPI.
 reg buffer_write=0; reg [8:0] buffer_address=0; reg [15:0] buffer_data=0;
 wire [15:0] buffer_q; reg q_high=0;
 gowin_dpb_snapshot buffer(.write_clock(clk),.write_enable(buffer_write),
  .write_address(buffer_address),.write_data(buffer_data),
  .read_clock(qclk),.read_address(qaddress[9:1]),.read_data(buffer_q));
 always @(posedge qclk) q_high<=qaddress[0];
 assign qdata=q_high ? buffer_q[15:8] : buffer_q[7:0];

 reg [25:0] timer=0;
 reg busy_seen=0, load_seen=0;
 reg [63:0] bus_data=0; reg [16:0] bus_byte=0; reg [2:0] byte_i=0; reg [1:0] word_i=0;
 reg [9:0] offset=0; reg [6:0] block=0; reg [15:0] request_sequence=0;
 reg [7:0] low_byte=0;
 assign x_write_data=bus_data[word_i*16 +: 16];
 wire screen_block=block>=7'd64;
 wire block_valid=screen_block ? block<7'd109 : block<BLOCKS;
 wire [23:0] block_address=screen_block ? 24'h800000+24'h010000+{8'd0,block-7'd64,10'd0}+{14'd0,offset}
                                        : STAGE+{7'd0,block,10'd0}+{14'd0,offset};
 // The state lies at 32-bit word addresses base + 0..16639 (base has zero
 // low bits), so the low 15 bits are the offset.
 wire [16:0] ss_byte={ss_adr[14:0],2'b00};

 always @(posedge clk) begin
  ss_done<=0; buffer_write<=0; cache_invalidate<=0; x_request<=0;
  if(reset) begin
   state<=IDLE;t_seen<=t_sync;fault<=0;loaded<=0;frozen<=0;block_ready<=0;
   save_req<=0;load_req<=0;rom_read<=0;owns_bus<=0;loading<=0;
  end else begin
   if(state!=IDLE && state!=BLOCK_READ) timer<=timer+1'b1;
   case(state)
    IDLE: begin
     rom_read<=0;timer<=0;busy_seen<=0;load_seen<=0;
     if(t_edge[0]) begin
      t_seen[0]<=t_sync[0];
      if(!frozen) begin fault<=0;loaded<=0;loading<=0;save_req<=1;state<=SAVE_START; end
     end else if(t_edge[1]) begin
      t_seen[1]<=t_sync[1];
      if(frozen) begin frozen<=0;cache_invalidate<=1; end
     end else if(t_edge[3]) begin
      t_seen[3]<=t_sync[3];
      block<=block_number;request_sequence<=block_sequence;block_ready<=0;
      // The framebuffer changes under the cache: never serve stale lines.
      if(block_number>=7'd64) cache_invalidate<=1;
      offset<=0;state<=BLOCK_READ;
     end else if(t_edge[2]) begin
      t_seen[2]<=t_sync[2];
      loaded<=0;fault<=0;
      // The MCU staged the payload behind the cache: drop stale lines.
      if(frozen) begin loading<=1;cache_invalidate<=1;load_req<=1;state<=LOAD_START; end
     end
    end
    SAVE_START, LOAD_START: begin
     // statemanager.vhd takes the request on a rising edge once not busy.
     if(state_busy) begin save_req<=0;load_req<=0;busy_seen<=1;state<=RUN; end
     else if(timer[25]) begin save_req<=0;load_req<=0;fault<=1;state<=IDLE; end
    end
    RUN: begin
     if(state_loaded) load_seen<=1;
     if(ss_ena) begin
      bus_byte<=ss_byte;timer<=0;
      if(ss_rnw) begin byte_i<=0;state<=BUS_READ; end
      else begin bus_data<=ss_din;word_i<=0;state<=BUS_WRITE_WAIT; end
     end else if(!state_busy) begin
      // Finished. A load that never reported completion was refused (size
      // word mismatch): the game state is unchanged, still frozen.
      if(loading) begin
       if(load_seen || state_loaded) loaded<=1; else fault<=1;
      end else frozen<=1;
      cache_invalidate<=1;state<=IDLE;
     end else if(timer[25]) begin fault<=1;state<=IDLE; end
    end
    BUS_WRITE_WAIT: if(rom_idle) begin
     owns_bus<=1;x_request<=1;x_address<=23'h020000+STAGE[22:0]+{6'd0,bus_byte};
     state<=BUS_WRITE;
    end
    BUS_WRITE: begin
     if(x_write_next) word_i<=word_i+1'b1;
     if(x_done) begin owns_bus<=0;ss_done<=1;state<=RUN; end
     else if(timer[20]) begin owns_bus<=0;fault<=1;state<=IDLE; end
    end
    BUS_READ: begin
     rom_read<=1;rom_address<=STAGE+{7'd0,bus_byte}+{21'd0,byte_i};
     if(rom_fault) begin rom_read<=0;fault<=1;state<=IDLE; end
     else if(rom_read && rom_ready && rom_address==STAGE+{7'd0,bus_byte}+{21'd0,byte_i}) begin
      ss_dout[byte_i*8 +: 8]<=rom_data;
      if(byte_i==7) begin rom_read<=0;ss_done<=1;state<=RUN; end
      else byte_i<=byte_i+1'b1;
     end
    end
    BLOCK_READ: begin
     rom_read<=1;rom_address<=block_address;
     if(rom_fault) begin rom_read<=0;fault<=1;state<=IDLE; end
     else if(rom_read && rom_ready && rom_address==block_address) begin
      // Blocks past the image (and past the framebuffer) read as zero.
      if(!offset[0]) low_byte<=block_valid ? rom_data : 8'h00;
      else begin buffer_write<=1;buffer_address<=offset[9:1];
       buffer_data<={block_valid ? rom_data : 8'h00,low_byte}; end
      if(offset==10'd1023) begin
       rom_read<=0;block_ready_sequence<=request_sequence;block_ready<=1;state<=IDLE;
      end else offset<=offset+1'b1;
     end
    end
    default: state<=IDLE;
   endcase
  end
 end
endmodule
