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
    (* ASYNC_REG="TRUE" *) reg [8:0] buttons_meta=0,buttons_x=0;
    // Lynx keys: Start = Pause, Select = Option 1, Menu+B = Option 2.
    wire play_h=MENU_CLOSED && !BTN_MENU;
    wire [8:0] keys_h={MENU_CLOSED && BTN_MENU && BTN_B,
        BTN_SEL && play_h, BTN_START && play_h,
        {BTN_B,BTN_A,BTN_DPAD_RIGHT,BTN_DPAD_LEFT,BTN_DPAD_DOWN,BTN_DPAD_UP} & {6{play_h}}};
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
    wire [23:0] rom_address,core_rom_address,state_rom_address;
    wire rom_read,core_rom_read,state_rom_read;wire [7:0] rom_byte;
    wire state_owns_rom,state_cache_invalidate;
    wire state_owns_bus,state_x_request;wire [22:0] state_x_address;wire [15:0] state_x_write_data;
    wire bridge_xrequest,bridge_xread;wire [22:0] bridge_xaddress;
    wire [15:0] bridge_xwritedata;wire [10:0] bridge_xlength;
    assign rom_read=state_owns_rom ? state_rom_read : core_rom_read;
    assign rom_address=state_owns_rom ? state_rom_address : core_rom_address;
    // 8 lines (512 bytes): Lynx cartridge reads are sequential, and the tags
    // live in flip-flops (block RAM is full).
    gg_rom_bridge #(.LINE_BITS(3)) rom_bridge(.clk(xclk),.reset(xreset),
        .enable(!core_reset_x && !quiet_x[1]),.invalidate(stop_x[1] || state_cache_invalidate),
        .read(rom_read),.address(rom_address),.data(rom_byte),.ready(rom_ready),
        .idle(rom_idle),.fault(rom_fault),.request(bridge_xrequest),
        .bus_read(bridge_xread),.bus_address(bridge_xaddress),
        .bus_write_data(bridge_xwritedata),.bus_length(bridge_xlength),
        .done(virtual_xdone && !state_owns_bus),.valid(virtual_xread_valid && !state_owns_bus),
        .read_data(virtual_xreaddata));
    // Save-state writes (8 bytes each) borrow the PSRAM port while the cache is idle.
    assign virtual_xrequest=state_owns_bus ? state_x_request : bridge_xrequest;
    assign virtual_xread=state_owns_bus ? 1'b0 : bridge_xread;
    assign virtual_xaddress=state_owns_bus ? state_x_address : bridge_xaddress;
    assign virtual_xwritedata=state_owns_bus ? state_x_write_data : bridge_xwritedata;
    assign virtual_xlength=state_owns_bus ? 11'd8 : bridge_xlength;
    wire pen_we;wire [3:0] pen;wire [7:0] pen_x;wire [6:0] pen_y;wire [191:0] palette;
    wire running,bad_image;
    wire [15:0] audio_left,audio_right,lynx_left_raw,lynx_right_raw;
    wire save_req,load_req,state_loaded,state_busy,ss_rnw,ss_ena,ss_done;
    wire [63:0] ss_din,ss_dout;wire [25:0] ss_adr;
    wire engine_busy,engine_fault,engine_frozen,engine_loaded,engine_saving;
    // Paused for the menu (quiesce) or a save state (frozen). A save first lets
    // the core reach an idle bus, which takes microseconds.
    wire core_pause=(quiet_x[1] && !engine_saving) || engine_frozen;
    lynx_board_core core(.clk(xclk),.reset_n(!core_reset_x),.quiesce(core_pause),
        .buttons(buttons_x),
        .rom_read(core_rom_read),.rom_address(core_rom_address),.rom_data(rom_byte),
        .rom_ready(rom_ready && !state_owns_rom),
        .pen_we(pen_we),.pen(pen),.pen_x(pen_x),.pen_y(pen_y),.palette(palette),
        .running(running),.audio_l(audio_left),.audio_r(audio_right),.bad_image(bad_image),
        .save_req(save_req),.load_req(load_req),.state_loaded(state_loaded),.state_busy(state_busy),
        .ss_din(ss_din),.ss_dout(ss_dout),.ss_adr(ss_adr),.ss_rnw(ss_rnw),.ss_ena(ss_ena),.ss_done(ss_done));
    lynx_state_engine state_engine(.clk(xclk),.reset(core_reset_x),
        .freeze_toggle(virtual_state_freeze_toggle),.release_toggle(virtual_state_release_toggle),
        .load_toggle(virtual_state_load_toggle),.block_toggle(virtual_snapshot_request_toggle),
        .block_number(virtual_snapshot_block),.block_sequence(virtual_snapshot_sequence),
        .block_ready(virtual_snapshot_ready),.block_ready_sequence(virtual_snapshot_ready_sequence),
        .busy(engine_busy),.fault(engine_fault),.frozen(engine_frozen),.loaded(engine_loaded),
        .saving(engine_saving),
        .save_req(save_req),.load_req(load_req),.state_loaded(state_loaded),.state_busy(state_busy),
        .ss_din(ss_din),.ss_dout(ss_dout),.ss_adr(ss_adr),.ss_rnw(ss_rnw),.ss_ena(ss_ena),.ss_done(ss_done),
        .owns_rom(state_owns_rom),.cache_invalidate(state_cache_invalidate),
        .rom_read(state_rom_read),.rom_address(state_rom_address),
        .rom_data(rom_byte),.rom_ready(rom_ready),.rom_fault(rom_fault),.rom_idle(rom_idle),
        .owns_bus(state_owns_bus),.x_request(state_x_request),.x_address(state_x_address),
        .x_write_data(state_x_write_data),.x_write_next(virtual_xwrite_next),.x_done(virtual_xdone),
        .qclk(virtual_snapshot_qclk),.qaddress(virtual_snapshot_qaddress),.qdata(virtual_snapshot_qdata));
    assign virtual_state_status={2'b00,engine_loaded,engine_fault,engine_frozen,engine_busy};
    lynx_video_bridge video_cdc(.xclk(xclk),.xreset(core_reset_x),
        .hclk(hclk),.hreset(reset_h_sync[1]),.enable(running),.paused(core_pause),
        .pen_we(pen_we),.pen(pen),.pen_x(pen_x),.pen_y(pen_y),.palette(palette),
        .overflow(video_overflow),.lcd_pixel(gb_lcd_clkena),.lcd_data(gb_lcd_data),
        .lcd_repeat(gb_lcd_repeat),
        .lcd_mode(gb_lcd_mode),.lcd_on(gb_lcd_on),.lcd_vsync(gb_lcd_vsync));
    gg_audio_bridge audio_cdc(.xclk(xclk),.xreset(core_reset_x),
        .stereo({audio_left,audio_right}),.hclk(hclk),.hreset(reset_h_sync[1]),
        .sample({lynx_left_raw,lynx_right_raw}));
    // The Game Gear core's low-pass/DC blocker, adders shared by L and R.
    lynx_audio_lpf u_audio_lpf(
        .clk(hclk),.reset(reset_h_sync[1]),
        .in_l(lynx_left_raw),.in_r(lynx_right_raw),
        .out_l(left),.out_r(right));
    (* ASYNC_REG="TRUE" *) reg [1:0] idle_h=0,initialized_h=0,reset_drained_h=0;
    always @(posedge hclk or negedge reset_n) begin
        if(!reset_n) begin idle_h<=0;initialized_h<=0;reset_drained_h<=0;end
        else begin
            idle_h<={idle_h[0],quiet_x[1] && rom_idle};
            reset_drained_h<={reset_drained_h[0],core_reset_x && rom_idle && !rom_fault};
            initialized_h<={initialized_h[0],!xreset && !rom_fault && !bad_image};
        end
    end
    assign virtual_quiesced_h=idle_h[1];
    assign virtual_core_reset_h=reset_drained_h[1];
    assign virtual_initialized=initialized_h[1];
    // Lynx cartridges have no battery RAM.
    assign virtual_cart_written=0;
    assign virtual_cart_snapshot=0;
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
