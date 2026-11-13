// emu_system_top.v

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
    output              virtual_core_reset_h,
    
    input               customPaletteEna,
    input [63:0]        paletteBGIn,
    input [63:0]        paletteOBJ0In,
    input [63:0]        paletteOBJ1In,
    input               paletteOff,
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

    // All game logic runs on existing xclk. Never change the board PLL here.
    wire stop_h=!reset_n || !POWER_GOOD || maintenance_reset_h ||
        virtual_attempt_reset_h || !virtual_enable || !LCD_INIT_DONE;
    (* ASYNC_REG="TRUE" *) reg [1:0] stop_x=2'b11;
    (* ASYNC_REG="TRUE" *) reg [1:0] quiet_x=0;
    (* ASYNC_REG="TRUE" *) reg [6:0] buttons_meta=0,buttons_x=0;
    wire [6:0] keys_h={BTN_START,BTN_B,BTN_A,BTN_DPAD_RIGHT,
        BTN_DPAD_LEFT,BTN_DPAD_DOWN,BTN_DPAD_UP} & {7{MENU_CLOSED && !BTN_MENU}};
    always @(posedge xclk or posedge stop_h) begin
        if(stop_h) begin stop_x<=2'b11;buttons_meta<=0;buttons_x<=0;end
        else begin stop_x<={stop_x[0],1'b0};buttons_meta<=keys_h;buttons_x<=buttons_meta;end
    end
    always @(posedge xclk or posedge xreset) begin
        if(xreset) quiet_x<=0;else quiet_x<={quiet_x[0],virtual_quiesce_h};
    end
    wire rom_ready,rom_idle,rom_fault,video_overflow;
    reg fault_latched=0;
    always @(posedge xclk or posedge xreset) begin
        if(xreset) fault_latched<=0;
        else if(stop_x[1] && rom_idle) fault_latched<=0;
        else if(rom_fault || video_overflow) fault_latched<=1;
    end
    wire core_reset_x=stop_x[1] || fault_latched;
    (* ASYNC_REG="TRUE" *) reg [1:0] reset_h_sync=2'b11;
    always @(posedge hclk or posedge core_reset_x) begin
        if(core_reset_x) reset_h_sync<=2'b11;
        else reset_h_sync<={reset_h_sync[0],1'b0};
    end
    wire [23:0] rom_address;wire rom_read;wire [7:0] rom_byte;
    // Native save state: the engine owns the ROM cache while the game is frozen.
    wire state_request,state_release,state_apply,state_frozen;
    wire [759:0] state_in,state_out;
    wire [1:0] state_mem_select;wire [14:0] state_mem_address;
    wire state_mem_write;wire [11:0] state_mem_data;wire [7:0] state_mem_q;
    wire state_owns_rom,state_rom_read,state_cache_invalidate;wire [23:0] state_rom_address;
    wire state_busy,state_fault,state_is_frozen,state_loaded,state_freeze_active,video_resume_ok;
    wire state_boot_hold,state_cart_dirty_clear,state_cart_dirty,state_imported;wire [7:0] state_cart_q;
    gg_rom_bridge rom_bridge(.clk(xclk),.reset(xreset),
        .enable(!core_reset_x && !quiet_x[1]),.invalidate(stop_x[1] || state_cache_invalidate),
        .read(state_owns_rom ? state_rom_read : rom_read),
        .address(state_owns_rom ? state_rom_address : rom_address),.data(rom_byte),.ready(rom_ready),
        .idle(rom_idle),.fault(rom_fault),.request(virtual_xrequest),
        .bus_read(virtual_xread),.bus_address(virtual_xaddress),
        .bus_write_data(virtual_xwritedata),.bus_length(virtual_xlength),
        .done(virtual_xdone),.valid(virtual_xread_valid),.read_data(virtual_xreaddata));
    wire [11:0] color;wire [8:0] pixel_x,pixel_y;wire pixel;
    wire [15:0] audio_left,audio_right;
    wire [15:0] gg_left_raw,gg_right_raw;
    gg_board_core core(.gg_code(gg_code_x), .gg_clear(gg_clear_x), .clk(xclk),.reset_n(!core_reset_x),
        .rom_ready(rom_ready),.quiesce(quiet_x[1]),.buttons(buttons_x),
        .rom_do(rom_byte),.rom_a(rom_address),.rom_rd(rom_read),
        .color(color),.x(pixel_x),.y(pixel_y),.ce_pix(pixel),
        .audioL(audio_left),.audioR(audio_right),.hblank(),.vblank(),
        .ram_we(),.ram_a(),.ram_d(),
        .state_request(state_request),.state_release(state_release),.state_apply(state_apply),
        .state_in(state_in),.state_out(state_out),.state_frozen(state_frozen),
        .mem_select(state_mem_select),.mem_address(state_mem_address),.mem_write(state_mem_write),
        .mem_data(state_mem_data),.mem_q(state_mem_q),.freeze_active(state_freeze_active),
        .boot_hold(state_boot_hold),.cart_dirty_clear(state_cart_dirty_clear),
        .cart_dirty(state_cart_dirty),.cart_q(state_cart_q),
        .cart_written(virtual_cart_written),.cart_snapshot(virtual_cart_snapshot));
    gg_state_engine state_engine(.clk(xclk),.reset(core_reset_x),
        .freeze_toggle(virtual_state_freeze_toggle),.release_toggle(virtual_state_release_toggle),
        .load_toggle(virtual_state_load_toggle),.block_toggle(virtual_snapshot_request_toggle),
        .block_number(virtual_snapshot_block),.block_sequence(virtual_snapshot_sequence),
        .block_ready(virtual_snapshot_ready),.block_ready_sequence(virtual_snapshot_ready_sequence),
        .busy(state_busy),.fault(state_fault),.frozen(state_is_frozen),.loaded(state_loaded),
        .state_request(state_request),.state_release(state_release),.state_apply(state_apply),
        .state_in(state_in),.state_out(state_out),.state_frozen(state_frozen),
        .mem_select(state_mem_select),.mem_address(state_mem_address),.mem_write(state_mem_write),
        .mem_data(state_mem_data),.mem_q(state_mem_q),
        .owns_rom(state_owns_rom),.cache_invalidate(state_cache_invalidate),.rom_address(state_rom_address),.rom_read(state_rom_read),
        .rom_data(rom_byte),.rom_ready(rom_ready),.rom_fault(rom_fault),
        .qclk(virtual_snapshot_qclk),.qaddress(virtual_snapshot_qaddress),.qdata(virtual_snapshot_qdata),
        .video_resume_ok(video_resume_ok),
        .cart_import(virtual_cart_import),.boot_hold(state_boot_hold),
        .cart_dirty_clear(state_cart_dirty_clear),.cart_q(state_cart_q),.imported(state_imported));
    assign virtual_state_status={state_imported,state_cart_dirty,state_loaded,state_fault,state_is_frozen,state_busy};
    gg_video_bridge video_cdc(.xclk(xclk),.xreset(core_reset_x),
        .hclk(hclk),.hreset(reset_h_sync[1]),.enable(!core_reset_x),
        .hold(state_freeze_active),.resume_ok(video_resume_ok),
        .pixel(pixel),.x(pixel_x),.y(pixel_y),.color(color),
        .overflow(video_overflow),.lcd_pixel(gb_lcd_clkena),.lcd_data(gb_lcd_data),
        .lcd_repeat(gb_lcd_repeat),
        .lcd_mode(gb_lcd_mode),.lcd_on(gb_lcd_on),.lcd_vsync(gb_lcd_vsync));
    gg_audio_bridge audio_cdc(.xclk(xclk),.xreset(core_reset_x),
        .stereo({audio_left,audio_right}),.hclk(hclk),.hreset(reset_h_sync[1]),
        .sample({gg_left_raw,gg_right_raw}));
    // Low-pass (~7 kHz, like the GB/GBC core's filter) + DC blocker, no multipliers.
    // Without it the raw PSG square waves reach the 131 kHz I2S output unfiltered.
    gg_audio_lpf u_audio_lpf(
        .clk(hclk),.reset(reset_h_sync[1]),
        .in_l(gg_left_raw),.in_r(gg_right_raw),
        .out_l(left),.out_r(right));
    (* ASYNC_REG="TRUE" *) reg [1:0] idle_h=0,initialized_h=0,reset_drained_h=0;
    always @(posedge hclk or negedge reset_n) begin
        if(!reset_n) begin idle_h<=0;initialized_h<=0;reset_drained_h<=0;end
        else begin
            idle_h<={idle_h[0],quiet_x[1] && rom_idle};
            // A reset acknowledgement authorizes ROM upload. Sample the joint
            // condition so a stale idle sample cannot acknowledge a new reset.
            reset_drained_h<={reset_drained_h[0],core_reset_x && rom_idle && !rom_fault};
            initialized_h<={initialized_h[0],!xreset && !rom_fault};
        end
    end
    assign virtual_quiesced_h=idle_h[1];
    assign virtual_core_reset_h=reset_drained_h[1];
    assign virtual_initialized=initialized_h[1];
    // Persistent saves/RTC/snapshot upload are intentionally unsupported here.
    assign virtual_save_dirty=0;
    assign virtual_rtc_state=0;
    assign gbc_mode=1;
    assign gpd=0;
    assign boot_rom_enabled=0;
    assign lcd_on_int=gb_lcd_on;
    assign lcd_off_overwrite=0;
    // GG software has no access to cartridge, link, or IR output enables.
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
