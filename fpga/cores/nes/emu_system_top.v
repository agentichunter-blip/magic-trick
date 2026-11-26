// emu_system_top.v: NES core (nes_board_core.sv, MiSTer NES) on the Game Gear board shell.

module emu_system_top
(
    input [128:0] gg_code_x,
    input gg_clear_x,
    input               hclk,
    input               pclk,
    input               xclk,
    input               reset_n,
    input               xreset,
    input               POWER_GOOD,
    input               maintenance_reset_h,
    input               virtual_attempt_reset_h,
    input               virtual_quiesce_h,
    output              virtual_quiesced_h,
    output reg          video_cut_toggle = 1'b0, // flips when a quiesce hand-back cut a frame short (xclk)
    output              virtual_core_reset_h,
    
    input               customPaletteEna,
    input [63:0]        paletteBGIn,
    input [63:0]        paletteOBJ0In,
    input [63:0]        paletteOBJ1In,
    input               paletteOff,
    input               smsStretch,     // settings bit 15: SMS picture fills 160x144
    output              gbc_mode,
    output [63:0]       gpd,
    
    input               BTN_NODIAGONAL,
    input               BTN_A,
    input               BTN_B,
    input               BTN_DPAD_DOWN,
    input               BTN_DPAD_LEFT,
    input               BTN_DPAD_RIGHT,
    input               BTN_DPAD_UP,
    input               BTN_MENU,
    input               BTN_SEL,
    input               BTN_START,
    input               MENU_CLOSED,
    output              boot_rom_enabled,
    output  [15:0]      CART_A,
    output              CART_CLK,
    output              CART_CS,
    input   [7:0]       CART_D_IN,
    output  [7:0]       CART_D_OUT,
    output              CART_D_OE,
    output              CART_RD,
    input               CART_RST,   // unused; cartridge_interface owns the pin
    output              CART_WR,

    input               virtual_enable,
    input   [2:0]       virtual_mapper_select,
    input               virtual_mbc1m,
    input               virtual_mbc30,
    input   [7:0]       virtual_cart_type,
    input               virtual_has_ram,
    input   [3:0]       virtual_ram_mask,
    input   [8:0]       virtual_rom_mask,
    input  [32:0]       virtual_rtc_time,
    input               virtual_rtc_restore_write,
    input   [2:0]       virtual_rtc_restore_address,
    input  [15:0]       virtual_rtc_restore_data,
    output [28:0]       virtual_rtc_state,
    output              virtual_initialized,
    output              virtual_save_dirty,
    output              virtual_xrequest,
    output              virtual_xread,
    output [22:0]       virtual_xaddress,
    output [15:0]       virtual_xwritedata,
    output [10:0]       virtual_xlength,
    input               virtual_xwrite_next,
    input               virtual_xdone,
    input               virtual_xread_valid,
    input  [15:0]       virtual_xreaddata,
    input               virtual_snapshot_request_toggle,
    input   [6:0]       virtual_snapshot_block,
    input   [15:0]      virtual_snapshot_sequence,
    output              virtual_snapshot_ready,
    output  [15:0]      virtual_snapshot_ready_sequence,
    input               virtual_state_freeze_toggle,
    input               virtual_state_release_toggle,
    input               virtual_state_load_toggle,
    input               virtual_cart_import,     // level, set before start
    output  [5:0]       virtual_state_status,    // imported, cart dirty, loaded, fault, frozen, busy (xclk)
    output  [31:0]      virtual_cart_written,    // 1 KiB cartridge RAM blocks written (xclk)
    output  [31:0]      virtual_cart_snapshot,   // ... before the current copy
    input               virtual_snapshot_qclk,
    input   [9:0]       virtual_snapshot_qaddress,
    output  [7:0]       virtual_snapshot_qdata,

    input               IR_RX,
    output              IR_LED,

    inout               LINK_CLK,
    input               LINK_IN,
    output              LINK_OUT,

    output [15:0]       left,
    output [15:0]       right,

    output lcd_on_int,
    output lcd_off_overwrite,
    
    input               LCD_INIT_DONE,
    output              gb_lcd_clkena,
    output [14:0]       gb_lcd_data,
    output              gb_lcd_repeat,     // show the framebuffer's previous pixel
    output [1:0]        gb_lcd_mode,
    output              gb_lcd_on,
    output              gb_lcd_vsync
    
);

    // All game logic runs on existing xclk (the NES itself on a gated copy of it).
    wire stop_h=!reset_n || !POWER_GOOD || maintenance_reset_h ||
        virtual_attempt_reset_h || !virtual_enable || !LCD_INIT_DONE;
    (* ASYNC_REG="TRUE" *) reg [1:0] stop_x=2'b11;
    (* ASYNC_REG="TRUE" *) reg [1:0] quiet_x=0;
    (* ASYNC_REG="TRUE" *) reg [7:0] buttons_meta=0,buttons_x=0;
    // NES controller bits 0-7: A, B, Select, Start, Up, Down, Left, Right.
    wire [7:0] keys_h={BTN_DPAD_RIGHT,BTN_DPAD_LEFT,BTN_DPAD_DOWN,BTN_DPAD_UP,
        BTN_START,BTN_SEL,BTN_B,BTN_A} & {8{MENU_CLOSED && !BTN_MENU}};
    always @(posedge xclk or posedge stop_h) begin
        if(stop_h) begin stop_x<=2'b11;buttons_meta<=0;buttons_x<=0;end
        else begin stop_x<={stop_x[0],1'b0};buttons_meta<=keys_h;buttons_x<=buttons_meta;end
    end
    always @(posedge xclk or posedge xreset) begin
        if(xreset) quiet_x<=0;else quiet_x<={quiet_x[0],virtual_quiesce_h};
    end
    (* ASYNC_REG="TRUE" *) reg [1:0] import_x=0;
    always @(posedge xclk) import_x<={import_x[0],virtual_cart_import};
    wire x_idle,core_paused,rom_fault,video_overflow,bad_image,running;
    reg fault_latched=0;
    always @(posedge xclk or posedge xreset) begin
        if(xreset) fault_latched<=0;
        else if(stop_x[1] && x_idle) fault_latched<=0;
        else if(rom_fault || video_overflow) fault_latched<=1;
    end
    wire core_reset_x=stop_x[1] || fault_latched;
    (* ASYNC_REG="TRUE" *) reg [1:0] reset_h_sync=2'b11;
    always @(posedge hclk or posedge core_reset_x) begin
        if(core_reset_x) reset_h_sync<=2'b11;
        else reset_h_sync<={reset_h_sync[0],1'b0};
    end
    wire pix_strobe; wire [7:0] pix_x,pix_y; wire [14:0] pix_rgb;
    wire [15:0] audio_sample; wire frame_strobe;
    wire state_busy,state_fault,state_frozen,state_loaded,cart_imported,cart_dirty;
    wire [7:0] cart_written8,cart_snapshot8;
    // A battery copy starts with live block 64 (PRG RAM $6000): move the
    // written-block record to the snapshot then, as the Game Gear engine does
    // for its block 9, so writes during the copy are kept for the next one.
    (* ASYNC_REG="TRUE" *) reg [2:0] block_t_x=0; reg cart_dirty_clear=0;
    always @(posedge xclk) begin
        block_t_x<={block_t_x[1:0],virtual_snapshot_request_toggle};
        cart_dirty_clear<=(block_t_x[2]!=block_t_x[1]) && virtual_snapshot_block==7'd64 && !state_frozen;
    end
    wire video_resume_ok,video_cut;
    always @(posedge xclk) if(video_cut) video_cut_toggle<=!video_cut_toggle;
    nes_board_core core(.gg_code(gg_code_x),.gg_clear(gg_clear_x),.clk(xclk),.reset_n(!core_reset_x),.pause(quiet_x[1]),.buttons(buttons_x),
        .x_request(virtual_xrequest),.x_read(virtual_xread),.x_address(virtual_xaddress),
        .x_length(virtual_xlength),.x_write_data(virtual_xwritedata),.x_write_next(virtual_xwrite_next),
        .x_done(virtual_xdone),.x_read_valid(virtual_xread_valid),.x_read_data(virtual_xreaddata),
        .x_idle(x_idle),
        .pix_strobe(pix_strobe),.pix_x(pix_x),.pix_y(pix_y),.pix_rgb(pix_rgb),.frame_strobe(frame_strobe),
        .audio_sample(),.audio_strobe(),.audio_out(audio_sample),
        .running(running),.paused_now(core_paused),.bad_image(bad_image),.rom_fault(rom_fault),.mapper_flags_o(),
        .video_resume_ok(video_resume_ok),
        .freeze_toggle(virtual_state_freeze_toggle),.release_toggle(virtual_state_release_toggle),
        .load_toggle(virtual_state_load_toggle),.block_toggle(virtual_snapshot_request_toggle),
        .block_number(virtual_snapshot_block),.block_sequence(virtual_snapshot_sequence),
        .block_ready(virtual_snapshot_ready),.block_ready_sequence(virtual_snapshot_ready_sequence),
        .state_busy(state_busy),.state_fault(state_fault),.state_frozen(state_frozen),.state_loaded(state_loaded),
        .qclk(virtual_snapshot_qclk),.qaddress(virtual_snapshot_qaddress),.qdata(virtual_snapshot_qdata),
        .cart_import(import_x[1]),.cart_imported(cart_imported),.cart_dirty(cart_dirty),
        .cart_dirty_clear(cart_dirty_clear),
        .cart_written(cart_written8),.cart_snapshot(cart_snapshot8));
    assign virtual_state_status={cart_imported,cart_dirty,state_loaded,state_fault,state_frozen,state_busy};
    assign virtual_cart_written={24'd0,cart_written8};
    assign virtual_cart_snapshot={24'd0,cart_snapshot8};
    // Picture: NES lines 8-231 scaled 5/8 to 160x140, black rows above and below.
    nes_video_bridge video_cdc(.xclk(xclk),.xreset(core_reset_x),
        .hclk(hclk),.hreset(reset_h_sync[1]),.enable(!core_reset_x),
        .hold(state_frozen),.resume_ok(video_resume_ok),   // as Game Gear: hold while a state freeze holds the game
        // The quiesce pause stops the NES mid-frame: stand-in frames carry on (see the bridge).
        .qhold(quiet_x[1]),.cut(video_cut),
        .pixel(pix_strobe),.x({1'b0,pix_x}),.y({1'b0,pix_y}),.rgb(pix_rgb),
        .overflow(video_overflow),.lcd_pixel(gb_lcd_clkena),.lcd_data(gb_lcd_data),
        .lcd_repeat(gb_lcd_repeat),
        .lcd_mode(gb_lcd_mode),.lcd_on(gb_lcd_on),.lcd_vsync(gb_lcd_vsync));
    // APU output is unsigned: centre it and halve it. MiSTer's mixer uses nearly the whole
    // 16-bit range (Kirby peaks at 26.6k after filtering, Game Boy and Game Gear at about
    // 16k), which overdrove the speaker; the high-pass below removes the rest.
    reg [15:0] audio_x=0;
    always @(posedge xclk) audio_x<={~audio_sample[15],~audio_sample[15],audio_sample[14:1]};
    wire [15:0] nes_left_raw,nes_right_raw;
    gg_audio_bridge audio_cdc(.xclk(xclk),.xreset(core_reset_x),
        .stereo({audio_x,audio_x}),.hclk(hclk),.hreset(reset_h_sync[1]),
        .sample({nes_left_raw,nes_right_raw}));
    gg_audio_lpf #(.DC_SHIFT(10)) u_audio_lpf(
        .clk(hclk),.reset(reset_h_sync[1]),
        .in_l(nes_left_raw),.in_r(nes_right_raw),
        .out_l(left),.out_r(right));
    (* ASYNC_REG="TRUE" *) reg [1:0] idle_h=0,initialized_h=0,reset_drained_h=0;
    always @(posedge hclk or negedge reset_n) begin
        if(!reset_n) begin idle_h<=0;initialized_h<=0;reset_drained_h<=0;end
        else begin
            idle_h<={idle_h[0],quiet_x[1] && core_paused && x_idle};
            reset_drained_h<={reset_drained_h[0],core_reset_x && x_idle && !rom_fault};
            initialized_h<={initialized_h[0],!xreset && !rom_fault};
        end
    end
    assign virtual_quiesced_h=idle_h[1];
    assign virtual_core_reset_h=reset_drained_h[1];
    assign virtual_initialized=initialized_h[1];
    assign virtual_save_dirty=0;
    assign virtual_rtc_state=0;
    assign gbc_mode=1;
    assign gpd=0;
    assign boot_rom_enabled=0;
    assign lcd_on_int=gb_lcd_on;
    assign lcd_off_overwrite=0;
    assign CART_A=16'hffff;
    assign CART_CLK=0;
    assign CART_CS=1;
    assign CART_RD=1;
    assign CART_WR=1;
    assign CART_D_OUT=0;
    assign CART_D_OE=0;
    assign LINK_CLK=1'bz;
    assign LINK_OUT=1;
    assign IR_LED=0;

endmodule
