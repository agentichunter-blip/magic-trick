// Atari Lynx core (MiSTer AtariLynx, lynx-rtl/) on the Chromatic's 67.108864 MHz
// xclk. No physical IO.
//
// The MCU stages one image in PSRAM, read through the ROM cache (byte offsets):
//   0x0000  info block: bytes 0-3 = 'L','X','C','1'
//           4-7   file size in bytes (little endian, at most 1 MiB)
//           8     flags: bit 0 boot ROM present, bit 1 60 Hz frame lock
//           9-10  cartridge bank 0 page size (256..2048; 0 for BS93)
//           11    bit 0: the file starts with a 64-byte .lnx header,
//                 bit 1: BS93 homebrew (loaded into RAM, started directly)
//           12-13 BS93 load and start address
//   0x0200  boot ROM (512 bytes) when flag bit 0 is set: the Atari boot ROM,
//           or Magic Trick's replacement stub (MCU lynx_hle.c)
//   0x1000  the game file as it is on the SD card
// The MCU parses the file header (lynx_rom.h); MiSTer's header.vhd is not
// built. After reset the loader copies the boot ROM and, for BS93, the
// program into RAM; then the core runs and reads the cartridge from PSRAM on
// demand (cart.vhd waits for rom_ack).
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
 localparam S_INFO=0,S_BIOS=1,S_BS93=2,S_RUN=3,S_BAD=4;
 reg [2:0] state=S_INFO;
 reg [19:0] offset=0;       // byte index within the current section
 reg [7:0] info[0:13];
 reg [31:0] file_size=0;
 reg lock60=0;
 reg [15:0] bank0size=0, load_address=0;
 reg has_header=0, bs93=0;
 reg bios_wr=0; reg [8:0] bios_addr=0; reg [7:0] bios_data=0;
 reg ram_wr=0; reg [15:0] ram_addr=0; reg [7:0] ram_data=0;
 // Cartridge reads (after loading)
 wire [19:0] cart_addr; wire cart_req; reg cart_ack=0; reg [7:0] cart_byte=0;
 reg cart_busy=0;
 wire [31:0] info_size={info[7],info[6],info[5],info[4]};
 // One cache read at a time: the address is set together with rom_read, and
 // the cache cannot report ready for it before the next edge (it compares
 // against its registered address), so no address compare is needed here.
 reg pending=0;
 wire [23:0] load_address_pos=(state==S_INFO ? 24'd0 : state==S_BIOS ? BIOS_BASE : FILE_BASE)+{4'd0,offset};
 wire fetched=pending && rom_ready;
 always @(posedge clk) begin
  bios_wr<=0; ram_wr<=0; cart_ack<=0;
  if(!reset_n) begin
   state<=S_INFO; offset<=0; rom_read<=0; pending<=0; cart_busy<=0; bad_image<=0;
  end else begin
   if(fetched) begin rom_read<=0; pending<=0; end
   case(state)
   S_INFO, S_BIOS, S_BS93: begin
    if(state==S_BS93 && (offset>=file_size[19:0] || offset>=20'h1000a)) begin
     if(!pending) state<=S_RUN;
    end else if(!pending && !rom_read) begin
     rom_address<=load_address_pos; rom_read<=1; pending<=1;
    end else if(fetched) begin
     offset<=offset+1'b1;
     case(state)
      S_INFO: begin
       info[offset[3:0]]<=rom_data;
       if(offset==13) begin
        offset<=0;
        file_size<=info_size; lock60<=info[8][1];
        bank0size<={info[10],info[9]}; has_header<=info[11][0]; bs93<=info[11][1];
        load_address<={rom_data,info[12]};
        state<=(info[0]==8'h4c && info[1]==8'h58 && info[2]==8'h43 && info[3]==8'h31 &&
                info_size!=0 && info_size<=32'h100000)
                ? (info[8][0] ? S_BIOS : info[11][1] ? S_BS93 : S_RUN) : S_BAD;
       end
      end
      S_BIOS: begin
       bios_wr<=1; bios_addr<=offset[8:0]; bios_data<=rom_data;
       if(offset==511) begin offset<=10; state<=bs93 ? S_BS93 : S_RUN; end
      end
      default: begin // S_BS93: program bytes follow the 10-byte header
       ram_wr<=1; ram_addr<=load_address+offset[15:0]-16'd10; ram_data<=rom_data;
      end
     endcase
    end
   end
   S_RUN: begin
    // cart.vhd pulses rom_req; hold the cache read until the byte is ready.
    if(!cart_busy) begin
     if(cart_req) begin cart_busy<=1; rom_read<=1; pending<=1; rom_address<=FILE_BASE+cart_addr; end
    end else if(fetched) begin
     cart_busy<=0; cart_ack<=1; cart_byte<=rom_data;
    end
   end
   default: begin rom_read<=0; bad_image<=1; end
   endcase
  end
 end
 assign running=state==S_RUN && reset_n;
 // The core comes out of reset one cycle after loading.
 reg core_reset=1;
 always @(posedge clk) core_reset<=!(state==S_RUN && reset_n);

 lynx_chromatic lynx(
  .clk(clk), .ce_tick(ce_tick), .lock60(lock60),
  .reset_in(core_reset), .pause_in(quiesce),
  .rom_addr(cart_addr), .rom_byte(cart_byte), .rom_req(cart_req), .rom_ack(cart_ack),
  .hdr_bank0size(bank0size), .hdr_hasHeader(has_header), .hdr_pc(load_address), .hdr_pc_use(bs93),
  .hdr_ram_addr(ram_addr), .hdr_ram_data(ram_data), .hdr_ram_wren(ram_wr),
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
