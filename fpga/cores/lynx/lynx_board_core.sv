// Atari Lynx core (MiSTer AtariLynx, lynx-rtl/) on the Chromatic's 67.108864 MHz
// xclk. No physical IO.
//
// The MCU stages one image in PSRAM, read through the ROM cache (byte offsets):
//   0x0000  info block: bytes 0-3 = 'L','X','C','1'
//           4-7   file size in bytes (little endian, at most 1 MiB)
//           8     flags: bit 0 boot ROM present, bit 1 60 Hz frame lock
//   0x0200  boot ROM (512 bytes) when flag bit 0 is set
//   0x1000  the game file as it is on the SD card: .lnx (with its 64-byte
//           header), a headerless cartridge image, or a BS93 homebrew file
// After reset the loader copies the boot ROM, then replays the start of the
// file through the core's ROM-download port, exactly as MiSTer's loader does:
// header.vhd picks up the cartridge bank size from a LYNX header, or copies a
// BS93 program into RAM and starts it without a boot ROM. Then the core runs
// and reads the cartridge from PSRAM on demand (cart.vhd waits for rom_ack).
module lynx_board_core(
 input clk, reset_n, quiesce,
 input [8:0] buttons, // up, down, left, right, A, B, pause, option 1, option 2
 // ROM cache port
 output reg rom_read=0, output reg [23:0] rom_address=0,
 input [7:0] rom_data, input rom_ready,
 // video (see lynx_video_bridge.sv)
 output pen_we, output [3:0] pen, output [7:0] pen_x, output [6:0] pen_y,
 output [191:0] palette,
 output running, // loaded and out of reset
 output [15:0] audio_l, audio_r,
 output reg bad_image=0,
 // save states (lynx_state_engine.sv)
 input save_req, load_req, output state_loaded, output state_busy,
 output [63:0] ss_din, input [63:0] ss_dout, output [25:0] ss_adr,
 output ss_rnw, output ss_ena, input ss_done
);
 localparam [23:0] FILE_BASE=24'h001000, BIOS_BASE=24'h000200;
 // 16 MHz clock enable: 16e6/2^26 = 15625/65536 exactly. Ticks are 4 or 5
 // clocks apart, never closer (the core's 64 MHz design assumes 4).
 reg [15:0] ce_acc=0; reg ce_tick=0;
 always @(posedge clk) begin
  {ce_tick,ce_acc}<={1'b0,ce_acc}+17'd15625;
 end

 // ---- loader ----
 localparam S_INFO=0,S_BIOS=1,S_FILE=2,S_RUN=3,S_BAD=4;
 reg [2:0] state=S_INFO;
 reg [19:0] offset=0;       // byte index within the current section
 reg [7:0] info[0:8];
 reg [31:0] file_size=0;
 reg lock60=0, has_bios=0;
 reg [7:0] low_byte=0;
 reg bios_wr=0; reg [8:0] bios_addr=0; reg [7:0] bios_data=0;
 reg rw_wren=0; reg [19:0] rw_addr=0; reg [15:0] rw_data=0;
 reg [1:0] gap=0;           // spaces ROM-download words (header.vhd needs 2)
 wire [19:0] replay_len=file_size>32'h10010 ? 20'h10010 : file_size[19:0];
 // Cartridge reads (after loading)
 wire [19:0] cart_addr; wire cart_req; reg cart_ack=0; reg [7:0] cart_byte=0;
 reg cart_busy=0;
 always @(posedge clk) begin
  bios_wr<=0; rw_wren<=0; cart_ack<=0;
  if(!reset_n) begin
   state<=S_INFO; offset<=0; rom_read<=0; cart_busy<=0; bad_image<=0; gap<=0;
  end else case(state)
   S_INFO: begin
    rom_read<=1; rom_address<=offset;
    if(rom_read && rom_ready && rom_address==offset) begin
     info[offset[3:0]]<=rom_data;
     if(offset==8) begin
      rom_read<=0; offset<=0;
      state<=(info[0]==8'h4c && info[1]==8'h58 && info[2]==8'h43 && info[3]==8'h31 &&
              {info[7],info[6],info[5],info[4]}!=0 && {info[7],info[6],info[5],info[4]}<=32'h100000)
              ? (rom_data[0] ? S_BIOS : S_FILE) : S_BAD;
      file_size<={info[7],info[6],info[5],info[4]};
      has_bios<=rom_data[0]; lock60<=rom_data[1];
     end else offset<=offset+1'b1;
    end
   end
   S_BIOS: begin
    rom_read<=1; rom_address<=BIOS_BASE+offset;
    if(rom_read && rom_ready && rom_address==BIOS_BASE+offset) begin
     bios_wr<=1; bios_addr<=offset[8:0]; bios_data<=rom_data;
     if(offset==511) begin rom_read<=0; offset<=0; state<=S_FILE; end
     else offset<=offset+1'b1;
    end
   end
   S_FILE: begin
    if(gap!=0) gap<=gap-1'b1;
    else if(offset>=replay_len) begin rom_read<=0; state<=S_RUN; end
    else begin
     rom_read<=1; rom_address<=FILE_BASE+offset;
     if(rom_read && rom_ready && rom_address==FILE_BASE+offset) begin
      rom_read<=0;
      if(!offset[0]) low_byte<=rom_data;
      // Words go out little endian at even addresses, like MiSTer's ioctl.
      if(offset[0] || offset+1==replay_len) begin
       rw_wren<=1; rw_addr<={offset[19:1],1'b0};
       rw_data<=offset[0] ? {rom_data,low_byte} : {8'h00,rom_data};
       gap<=3;
      end
      offset<=offset+1'b1;
     end
    end
   end
   S_RUN: begin
    // cart.vhd pulses rom_req; hold the cache read until the byte is ready.
    if(!cart_busy) begin
     if(cart_req) begin cart_busy<=1; rom_read<=1; rom_address<=FILE_BASE+cart_addr; end
    end else if(rom_read && rom_ready) begin
     rom_read<=0; cart_busy<=0; cart_ack<=1; cart_byte<=rom_data;
    end
   end
   default: begin rom_read<=0; bad_image<=1; end
  endcase
 end
 assign running=state==S_RUN && reset_n;
 // The core comes out of reset one cycle after the last download word.
 reg core_reset=1;
 always @(posedge clk) core_reset<=!(state==S_RUN && reset_n);

 // header.vhd reads romsize only for headerless images (bank size guess).
 wire [19:0] romsize=file_size>32'hfffff ? 20'hfffff : file_size[19:0];
 lynx_chromatic lynx(
  .clk(clk), .ce_tick(ce_tick), .lock60(lock60),
  .reset_in(core_reset), .pause_in(quiesce),
  .rom_addr(cart_addr), .rom_byte(cart_byte), .rom_req(cart_req), .rom_ack(cart_ack),
  .romsize(romsize), .romwrite_data(rw_data), .romwrite_addr(rw_addr), .romwrite_wren(rw_wren),
  .bios_wraddr(bios_addr), .bios_wrdata(bios_data), .bios_wr(bios_wr),
  .pen_we(pen_we), .pen(pen), .pen_x(pen_x), .pen_y(pen_y), .palette(palette),
  .audio_l(audio_l), .audio_r(audio_r),
  .JoyUP(buttons[0]), .JoyDown(buttons[1]), .JoyLeft(buttons[2]), .JoyRight(buttons[3]),
  .Option1(buttons[7]), .Option2(buttons[8]), .KeyB(buttons[5]), .KeyA(buttons[4]),
  .KeyPause(buttons[6]),
  .save_state(save_req), .load_state(load_req), .state_loaded(state_loaded), .state_busy(state_busy),
  .ss_din(ss_din), .ss_dout(ss_dout), .ss_adr(ss_adr), .ss_rnw(ss_rnw), .ss_ena(ss_ena), .ss_done(ss_done)
 );
endmodule
