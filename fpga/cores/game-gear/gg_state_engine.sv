// Native Game Gear save state transfer on xclk. The firmware validates and
// stores files; this module only moves bytes between the frozen core and the
// MCU, so it stays small (no CRC, header or identity logic in the FPGA).
//
// Payload, 57 blocks of 1 KiB:
//   block 0: bytes 0-7 header (4,5 = 'g','g'; 6 = layout version 2),
//            8-102 registers (gg_board_core state byte order),
//            128-191 palette, 32 little-endian 12-bit words; rest zero
//   blocks 1-8 work RAM, 9-40 cartridge RAM (32 KiB), 41-56 VRAM
// (Version 1 had 41 blocks with 16 KiB of cartridge RAM.)
// SAVE: freeze, then the MCU reads blocks (command 14) into this buffer.
// LOAD: freeze, the MCU stages the payload in PSRAM at 0x410000, then this
// engine reads it back through the ROM cache (ROM address 0x3F0000, inside
// the 4 MiB ROM window but above the largest 1 MiB Game Gear ROM).
// BATTERY SAVE: with cart_import set while the core is in reset (before
// start), the core comes out of reset frozen and this engine writes blocks
// 9-40 of the staged payload into cartridge RAM, then releases the game.
// Block reads of 9-40 also work while the game runs (second RAM port); a read
// of block 9 clears the core's dirty flag, so the MCU can tell a torn copy.
module gg_state_engine #(parameter integer BLOCKS=57)(
 input clk, reset,
 input freeze_toggle, release_toggle, load_toggle, block_toggle,
 input [6:0] block_number, input [15:0] block_sequence,
 output reg block_ready=0, output reg [15:0] block_ready_sequence=0,
 output busy, output reg fault=0, output frozen, output reg loaded=0,
 output reg state_request=0, output reg state_release=0, output reg state_apply=0,
 output reg [759:0] state_in=0, input [759:0] state_out, input state_frozen,
 output reg [1:0] mem_select=0, output reg [14:0] mem_address=0, output reg mem_write=0,
 output reg [11:0] mem_data=0, input [7:0] mem_q,
 // Releasing clears the ROM cache, so a game continued after SAVE and one
 // resumed after LOAD start from the same (cold) cache: identical timing.
 output owns_rom, output reg cache_invalidate=0, output reg [23:0] rom_address=0, output reg rom_read=0,
 input [7:0] rom_data, input rom_ready, input rom_fault,
 input qclk, input [9:0] qaddress, output [7:0] qdata,
 // The video bridge's stand-in frames are in vertical blank: release now.
 input video_resume_ok,
 input cart_import, output reg boot_hold=0, output reg cart_dirty_clear=0,
 input [7:0] cart_q, output reg imported=0
);
 localparam [3:0] IDLE=0,FREEZING=1,CAP_SET=2,CAP_WAIT=3,CAP_TAKE=4,
  LOAD_SET=5,LOAD_WAIT=6,LOAD_PUT=7,APPLY=8,SETTLE=9,RELEASE_WAIT=10;
 localparam [15:0] LAST=BLOCKS*1024-1, CART_FIRST=9*1024, CART_LAST=41*1024-1;
 (* ASYNC_REG="TRUE" *) reg [1:0] import_sync=0;
 always @(posedge clk) import_sync<={import_sync[0],cart_import};
 reg importing=0;
 wire [15:0] last_position=importing ? CART_LAST : LAST;
 reg [3:0] state=IDLE;
 assign busy=state!=IDLE;
 assign frozen=state_frozen;
 assign owns_rom=state_frozen;

 // Command toggles come from the QSPI command clock; operands are held stable.
 (* ASYNC_REG="TRUE" *) reg [3:0] t_meta=0,t_sync=0;
 reg [3:0] t_seen=0;
 always @(posedge clk) begin t_meta<={block_toggle,load_toggle,release_toggle,freeze_toggle};t_sync<=t_meta;end
 wire [3:0] t_edge=t_sync^t_seen;

 // Capture view: the live frozen outputs, except the free-running mixer
 // phase, which is latched when the freeze completes.
 wire [759:0] capture={state_out[759:753],state_in[752],state_out[751:0]};

 reg [6:0] block=0; reg [15:0] request_sequence=0;
 reg [15:0] position=0;            // byte offset in the payload
 reg [2:0] wait_count=0;
 reg [7:0] low_byte=0, register_byte=0;
 wire [5:0] p_block=position[15:10];
 wire [9:0] p_offset=position[9:0];
 wire in_registers=p_block==0 && p_offset>=8 && p_offset<103;
 wire in_palette=p_block==0 && p_offset>=128 && p_offset<192;
 wire [6:0] register_index=7'(p_offset-10'd8);
 wire [5:0] palette_offset=6'(p_offset-10'd128);

 // Memory region of a payload position (block 0 handled separately).
 reg [1:0] region_select; reg [14:0] region_address; reg region_valid;
 always @* begin
  region_valid=1;region_select=0;region_address=0;
  if(p_block>=1 && p_block<=8) begin region_select=0;region_address={2'b0,3'(p_block-6'd1),p_offset}; end
  else if(p_block>=9 && p_block<=40) begin region_select=1;region_address={5'(p_block-6'd9),p_offset}; end
  else if(p_block>=41 && p_block<=56) begin region_select=2;region_address={1'b0,4'(p_block-6'd41),p_offset}; end
  else region_valid=0;
 end

 reg [7:0] capture_value;
 wire cart_block=p_block>=9 && p_block<=40;
 always @* begin
  if(block>=BLOCKS) capture_value=0;
  else if(!state_frozen) capture_value=cart_block ? cart_q : 8'h00;
  else if(p_block==0) capture_value=p_offset==4 || p_offset==5 ? 8'h67 : p_offset==6 ? 8'h02 :
   in_registers ? register_byte : in_palette ? mem_q : 8'h00;
  else capture_value=region_valid ? mem_q : 8'h00;
 end

 // 1 KiB buffer: 512 x 16-bit words, written here, read by QSPI.
 reg buffer_write=0; reg [8:0] buffer_address=0; reg [15:0] buffer_data=0;
 wire [15:0] buffer_q; reg q_high=0;
 gowin_dpb_snapshot buffer(.write_clock(clk),.write_enable(buffer_write),
  .write_address(buffer_address),.write_data(buffer_data),
  .read_clock(qclk),.read_address(qaddress[9:1]),.read_data(buffer_q));
 always @(posedge qclk) q_high<=qaddress[0];
 assign qdata=q_high ? buffer_q[15:8] : buffer_q[7:0];

 always @(posedge clk) begin
  if(reset) begin
   state<=IDLE;t_seen<=t_sync;fault<=0;loaded<=0;block_ready<=0;
   boot_hold<=import_sync[1];imported<=0;importing<=0;cart_dirty_clear<=0;
   state_request<=0;state_release<=0;state_apply<=0;mem_write<=0;rom_read<=0;buffer_write<=0;
  end else begin
   state_release<=0;state_apply<=0;mem_write<=0;buffer_write<=0;cache_invalidate<=0;cart_dirty_clear<=0;
   case(state)
    IDLE: begin
     rom_read<=0;
     if(boot_hold) begin
      // The core left reset frozen: write the battery save before it runs.
      if(state_frozen) begin boot_hold<=0;importing<=1;position<=CART_FIRST;cache_invalidate<=1;state<=LOAD_SET;end
     end else if(t_edge[0]) begin
      t_seen[0]<=t_sync[0];
      if(!state_frozen) begin fault<=0;loaded<=0;state_request<=1;state<=FREEZING;end
     end else if(t_edge[1]) begin
      t_seen[1]<=t_sync[1];
      if(state_frozen) state<=RELEASE_WAIT;
     end else if(t_edge[3]) begin
      t_seen[3]<=t_sync[3];
      block<=block_number;request_sequence<=block_sequence;block_ready<=0;
      if(!state_frozen && block_number==7'd9) cart_dirty_clear<=1;
      position<={block_number[5:0],10'd0};
      state<=CAP_SET;
     end else if(t_edge[2]) begin
      t_seen[2]<=t_sync[2];
      // Each load reports its own outcome. The firmware staged the payload
      // in PSRAM behind the cache, so drop any lines of an earlier payload.
      loaded<=0;fault<=0;
      if(state_frozen) begin position<=0;cache_invalidate<=1;state<=LOAD_SET;end
     end
    end
    FREEZING: if(state_frozen) begin
     state_request<=0;state_in[752]<=state_out[752];state<=IDLE;
    end
    // SAVE: one byte per five cycles into the buffer (1 KiB in ~76 us).
    CAP_SET: begin
     if(in_palette) begin mem_select<=3;mem_address<={9'd0,palette_offset}; end
     else begin mem_select<=region_select;mem_address<=region_address; end
     register_byte<=capture[register_index*8 +: 8];
     wait_count<=0;state<=CAP_WAIT;
    end
    CAP_WAIT: begin wait_count<=wait_count+1'b1;if(wait_count==3) state<=CAP_TAKE; end
    CAP_TAKE: begin
     if(!p_offset[0]) low_byte<=capture_value;
     else begin buffer_write<=1;buffer_address<=p_offset[9:1];buffer_data<={capture_value,low_byte}; end
     if(p_offset==10'd1023) begin
      block_ready_sequence<=request_sequence;block_ready<=1;state<=IDLE;
     end else begin position<=position+1'b1;state<=CAP_SET; end
    end
    // LOAD: stream the staged payload through the ROM cache.
    LOAD_SET: begin rom_address<=24'h3F0000+{8'd0,position};rom_read<=1;state<=LOAD_WAIT; end
    LOAD_WAIT: begin
     if(rom_fault) begin fault<=1;rom_read<=0;importing<=0;state<=IDLE; end
     else if(rom_ready) begin register_byte<=rom_data;rom_read<=0;state<=LOAD_PUT; end
    end
    LOAD_PUT: begin
     // Refuse a payload that is not a Game Gear state before the first write.
     if((position==4 || position==5) && register_byte!=8'h67) begin fault<=1;state<=IDLE; end
     else begin
      // Register bytes arrive in order. Shifting them in from the top needs no
      // per-bit multiplexer; after all 95 bytes the first one is in bits 7:0.
      if(in_registers) state_in<={register_byte,state_in[759:8]};
      else if(in_palette) begin
       if(!palette_offset[0]) low_byte<=register_byte;
       else begin mem_select<=3;mem_address<={9'd0,palette_offset};mem_data<={register_byte[3:0],low_byte};mem_write<=1; end
      end else if(p_block!=0 && region_valid) begin
       mem_select<=region_select;mem_address<=region_address;mem_data<={4'd0,register_byte};mem_write<=1;
      end
      if(position==last_position) begin
       if(importing) begin importing<=0;imported<=1;state<=RELEASE_WAIT; end
       else begin state_apply<=1;wait_count<=0;state<=APPLY; end
      end
      else begin position<=position+1'b1;state<=LOAD_SET; end
     end
    end
    // Apply twice: the vendor mapper samples the restored BIOS flag's previous
    // value on the first apply and would see a false BIOS-handoff edge.
    APPLY: begin state_apply<=1;position<=0;state<=SETTLE; end
    // The prototype replay showed 64 frozen cycles after apply are enough.
    SETTLE: begin position<=position+1'b1;if(position==63) begin loaded<=1;state<=IDLE; end end
    // Wait at most one stand-in frame so the display sees no partial frame.
    RELEASE_WAIT: if(video_resume_ok) begin state_release<=1;cache_invalidate<=1;state<=IDLE; end
    default: state<=IDLE;
   endcase
  end
 end
endmodule
