// emu_system_top.v

module emu_system_top
(
    input state_gclk, state_greset, state_request_g, state_load_g, state_import_g,
    output state_busy_g, output state_hold_h, output [31:0] state_status_g,
    output [6:0] state_debug_h,
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
    input               CART_RST,   // read only: cartridge_interface owns the pin
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
    // [8]=1 reads the native state slot (256 KiB) instead of battery RAM.
    input   [8:0]       virtual_snapshot_block,
    input   [15:0]      virtual_snapshot_sequence,
    output              virtual_snapshot_ready,
    output  [15:0]      virtual_snapshot_ready_sequence,
    input               virtual_snapshot_qclk,
    input   [9:0]       virtual_snapshot_qaddress,
    output  [7:0]       virtual_snapshot_qdata,

    input               IR_RX,
    output              IR_LED,

    inout               LINK_CLK,
    output              LINK_CLK_DIR_LV,
    input               LINK_IN,
    output              LINK_OUT,

    output [15:0]       left,
    output [15:0]       right,

    output lcd_on_int,
    output lcd_off_overwrite,
    
    input               LCD_INIT_DONE,
    output              gb_lcd_clkena,
    output [14:0]       gb_lcd_data,
    output [1:0]        gb_lcd_mode,
    output              gb_lcd_on,
    output              gb_lcd_vsync
    
);

    parameter SRSIZE = 15;

    reg [SRSIZE-1:0] BTN_DPAD_DOWN_sr;
    reg [SRSIZE-1:0] BTN_DPAD_UP_sr;
    reg [SRSIZE-1:0] BTN_DPAD_LEFT_sr;
    reg [SRSIZE-1:0] BTN_DPAD_RIGHT_sr;

    reg [SRSIZE-1:0] BTN_START_sr;
    reg [SRSIZE-1:0] BTN_SEL_sr;
    reg [SRSIZE-1:0] BTN_B_sr;
    reg [SRSIZE-1:0] BTN_A_sr;

    reg BTN_DPAD_DOWN_filtered;
    reg BTN_DPAD_UP_filtered;
    reg BTN_DPAD_LEFT_filtered;
    reg BTN_DPAD_RIGHT_filtered;
    reg BTN_START_filtered;
    reg BTN_SEL_filtered;
    reg BTN_B_filtered;
    reg BTN_A_filtered;
    
    reg BTN_DPAD_DOWN_filtered_dir;
    reg BTN_DPAD_UP_filtered_dir;
    reg BTN_DPAD_LEFT_filtered_dir;
    reg BTN_DPAD_RIGHT_filtered_dir;

    reg menu_input_guard = 1'b0;
    wire game_keys_held = BTN_DPAD_DOWN | BTN_DPAD_UP | BTN_DPAD_LEFT |
        BTN_DPAD_RIGHT | BTN_START | BTN_SEL | BTN_A | BTN_B;
    always @(posedge pclk) begin
        menu_input_guard <= BTN_MENU || !MENU_CLOSED || (menu_input_guard && (game_keys_held || BTN_DPAD_DOWN_filtered | BTN_DPAD_UP_filtered | BTN_DPAD_LEFT_filtered | BTN_DPAD_RIGHT_filtered | BTN_START_filtered | BTN_SEL_filtered | BTN_B_filtered | BTN_A_filtered));
    end

    always@(posedge pclk)
    begin
        BTN_DPAD_DOWN_sr <= {BTN_DPAD_DOWN_sr[SRSIZE-2:0], BTN_DPAD_DOWN&~BTN_MENU&MENU_CLOSED};
        BTN_DPAD_UP_sr <= {BTN_DPAD_UP_sr[SRSIZE-2:0], BTN_DPAD_UP&~BTN_MENU&MENU_CLOSED};
        BTN_DPAD_LEFT_sr <= {BTN_DPAD_LEFT_sr[SRSIZE-2:0], BTN_DPAD_LEFT&~BTN_MENU&MENU_CLOSED};
        BTN_DPAD_RIGHT_sr <= {BTN_DPAD_RIGHT_sr[SRSIZE-2:0], BTN_DPAD_RIGHT&~BTN_MENU&MENU_CLOSED};
        BTN_START_sr <= {BTN_START_sr [SRSIZE-2:0], BTN_START&~BTN_MENU&MENU_CLOSED};
        BTN_SEL_sr <= {BTN_SEL_sr [SRSIZE-2:0], BTN_SEL&~BTN_MENU&MENU_CLOSED};
        BTN_A_sr <= {BTN_A_sr [SRSIZE-2:0], BTN_A&~BTN_MENU&MENU_CLOSED};
        BTN_B_sr <= {BTN_B_sr [SRSIZE-2:0], BTN_B&~BTN_MENU&MENU_CLOSED};

        BTN_DPAD_DOWN_filtered <= &BTN_DPAD_DOWN_sr[SRSIZE-1:1];
        BTN_DPAD_UP_filtered <= &BTN_DPAD_UP_sr[SRSIZE-1:1];
        BTN_DPAD_LEFT_filtered <= &BTN_DPAD_LEFT_sr[SRSIZE-1:1];
        BTN_DPAD_RIGHT_filtered <= &BTN_DPAD_RIGHT_sr[SRSIZE-1:1];
        BTN_START_filtered <= &BTN_START_sr[SRSIZE-1:1];
        BTN_SEL_filtered <= &BTN_SEL_sr[SRSIZE-1:1];
        BTN_A_filtered <= &BTN_A_sr[SRSIZE-1:1];
        BTN_B_filtered <= &BTN_B_sr[SRSIZE-1:1];
        
        if (BTN_NODIAGONAL) begin
            BTN_DPAD_DOWN_filtered_dir  <= BTN_DPAD_DOWN_filtered  & ~BTN_DPAD_UP_filtered & ~BTN_DPAD_LEFT_filtered_dir & ~BTN_DPAD_RIGHT_filtered_dir;
            BTN_DPAD_UP_filtered_dir    <= BTN_DPAD_UP_filtered    & ~BTN_DPAD_DOWN_filtered & ~BTN_DPAD_LEFT_filtered_dir & ~BTN_DPAD_RIGHT_filtered_dir;
            BTN_DPAD_LEFT_filtered_dir  <= BTN_DPAD_LEFT_filtered  & ~BTN_DPAD_RIGHT_filtered & ~BTN_DPAD_UP_filtered_dir & ~BTN_DPAD_DOWN_filtered_dir;
            BTN_DPAD_RIGHT_filtered_dir <= BTN_DPAD_RIGHT_filtered & ~BTN_DPAD_LEFT_filtered & ~BTN_DPAD_UP_filtered_dir & ~BTN_DPAD_DOWN_filtered_dir;
        end else begin
            BTN_DPAD_DOWN_filtered_dir  <= BTN_DPAD_DOWN_filtered  & ~BTN_DPAD_UP_filtered;
            BTN_DPAD_UP_filtered_dir    <= BTN_DPAD_UP_filtered    & ~BTN_DPAD_DOWN_filtered;
            BTN_DPAD_LEFT_filtered_dir  <= BTN_DPAD_LEFT_filtered  & ~BTN_DPAD_RIGHT_filtered;
            BTN_DPAD_RIGHT_filtered_dir <= BTN_DPAD_RIGHT_filtered & ~BTN_DPAD_LEFT_filtered;
        end
        
    end

    wire [3:0] btn_key = {
        BTN_DPAD_DOWN_filtered_dir, 
        BTN_DPAD_UP_filtered_dir,
        BTN_DPAD_LEFT_filtered_dir, 
        BTN_DPAD_RIGHT_filtered_dir
    }; 
     
    wire [3:0] dpad_key = {
        BTN_START_filtered,
        BTN_SEL_filtered,
        BTN_B_filtered,
        BTN_A_filtered
    };

    reg [3:0] joy_din;
    wire [1:0] joy_p54;
    always@(posedge hclk)
    begin
        if (menu_input_guard) joy_din <= 4'hF;
        else case(joy_p54)
            2'b00: joy_din <= (~btn_key) & (~dpad_key); 
            2'b01: joy_din <= ~dpad_key;
            2'b10: joy_din <= ~btn_key; 
            2'b11: joy_din <= 4'hF;
        endcase
    end

    wire wr;
    wire rd;
    wire [7:0] CART_DOUT;
    wire nCS;

    wire cpu_speed;
    wire cpu_halt;
    wire cpu_stop;
    
    wire [7:0] CART_DIN_r1;
    wire [15:0] a;
    wire [2:0] TSTATEo;
    wire sleep_savestate;
    // Retain snapshot clock pause until the firmware explicitly resumes.
    // A subsequent save must let the PPU reach its capture boundary first.
    reg native_snapshot_hold = 1'b0;
    always @(posedge hclk) begin
      if(!reset_n || !virtual_enable || gbreset_effective) native_snapshot_hold <= 1'b0;
      else if(state_save) native_snapshot_hold <= 1'b0;
      else if(sleep_savestate && virtual_quiesced_h) native_snapshot_hold <= 1'b1;
      else if(!virtual_quiesce_h && !state_hold) native_snapshot_hold <= 1'b0;
    end

    speedcontrol u_speedcontrol
    (
        .clk_sys     (hclk),
        .pause       (sleep_savestate | native_snapshot_hold),
        .speedup     (1'b0),
        .cart_act    (!virtual_pause_quiesced_h),
        .DMA_on      (DMA_on),
        .ce          (ce),
        .ce_2x       (ce_2x),
        .refresh     (),
        .ff_on       ()
    );

    wire sel_cram = a[15:13] == 3'b101;           // 8k cart ram at $a000
    wire cart_oe = (rd & ~a[15]) | (sel_cram & rd);


    reg gbreset;
    reg gbreset_ungated;
    reg CART_RST_r1;
    reg CART_RST_r2;
    reg ce_2x_r1;
    always@(posedge hclk)
        ce_2x_r1 <= ce_2x;
        
    always@(posedge hclk or negedge reset_n)
    begin
        if(~reset_n)
        begin
            gbreset <= 1'd1;
            gbreset_ungated <= 1'd1;
        end
        else
        begin
            CART_RST_r1 <= CART_RST;
            CART_RST_r2 <= CART_RST_r1;
            gbreset_ungated <= ~LCD_INIT_DONE ? 1'b1 :
                              (~CART_RST_r2 && !virtual_enable);
            if(~ce_2x_r1 & ce_2x & ce)
                gbreset <= gbreset_ungated;
                
            if (MENU_CLOSED & BTN_MENU & BTN_A & BTN_B & BTN_START & BTN_SEL) gbreset <= 1'd1;
            
            if (~POWER_GOOD) gbreset <= 1'b1;
        end
    end
    
    wire DMA_on;
    wire hdma_active;
    reg virtual_enable_d;
    reg [3:0] virtual_mode_reset_hold;
    always @(posedge hclk or negedge reset_n) begin
        if (!reset_n) begin
            virtual_enable_d <= 1'b0;
            virtual_mode_reset_hold <= 4'd0;
        end else begin
            virtual_enable_d <= virtual_enable;
            if (virtual_enable != virtual_enable_d)
                virtual_mode_reset_hold <= 4'hf;
            else if (virtual_mode_reset_hold != 0)
                virtual_mode_reset_hold <= virtual_mode_reset_hold - 1'b1;
        end
    end
    wire virtual_mode_reset = virtual_mode_reset_hold != 0;
    wire gbreset_effective = gbreset | maintenance_reset_h |
        virtual_attempt_reset_h |
        virtual_mode_reset | (virtual_enable && !virtual_initialized);
    assign virtual_core_reset_h = gbreset_effective;
    cart u_cart
    (
       .hclk            (hclk           ),
       .pclk            (pclk           ),
       .ce              (ce             ),
       .ce_2x           (ce_2x          ),
       .gbreset         (gbreset_effective),
       .cpu_speed       (cpu_speed      ),
       .cpu_halt        (cpu_halt       ),
       .cpu_stop        (cpu_stop       ),
       .wr              (wr             ),
       .rd              (rd             ),
       .a               (a              ),
       .CART_DOUT       (CART_DOUT      ),
       .nCS             (nCS            ),
       .TSTATEo         (TSTATEo        ),
       .DMA_on          (DMA_on         ),
       .hdma_active     (hdma_active    ),
                                        
       .CART_A          (CART_A         ),
       .CART_CLK        (CART_CLK       ),
       .CART_CS         (CART_CS        ),
       .CART_D_IN       (CART_D_IN      ),
       .CART_D_OUT_PHY  (CART_D_OUT     ),
       .CART_D_OE       (CART_D_OE      ),
       .CART_RD         (CART_RD        ),
       .CART_WR         (CART_WR        ),
       .CART_DIN_r1     (CART_DIN_r1    )
    );

    wire sc_int_clock2;
    wire serial_clk_in;
    wire serial_clk_out;
    wire serial_data_out;
    
    reg LINK_IN_r1;
    reg LINK_CLK_r1;
    reg serial_clk_out_r1;
    reg sc_int_clock2_r1;
    reg serial_data_out_r1;
    always@(posedge hclk)
    begin
        LINK_IN_r1         <= LINK_IN;
        LINK_CLK_r1        <= LINK_CLK;
        serial_clk_out_r1  <= serial_clk_out;
        sc_int_clock2_r1   <= sc_int_clock2;
        serial_data_out_r1 <= serial_data_out;
    end

    // ModRetro 18.9: link clock level-shifter direction follows the clock source.
    assign LINK_CLK_DIR_LV = sc_int_clock2_r1;
    assign LINK_CLK = sc_int_clock2_r1 ? serial_clk_out_r1 : 1'bZ;
    assign serial_clk_in = LINK_CLK_r1;

    wire serial_data_in = LINK_IN_r1;
    assign LINK_OUT = serial_data_out_r1;

    wire [63:0] SAVE_out_Din  ;
    wire [63:0] SAVE_out_Dout ;
    wire [25:0] SAVE_out_Adr  ;
    wire SAVE_out_rnw  ;                    
    wire SAVE_out_ena  ;                                    
    wire SAVE_out_done ; 
    
    wire [15:0] snd_l;  
    wire [15:0] snd_r;  

    wire [7:0] virtual_cart_do;
    wire virtual_cart_oe;
    wire virtual_cart_wait;
    wire cart_wr_pulse;
    wire [31:0] virtual_rtc_timestamp;
    wire [47:0] virtual_rtc_saved_time;
    wire virtual_rtc_in_use;
    wire core_ce_cpu = cpu_speed ? ce_2x : ce;

    // Quiescence is held by the external pause request. Register its status
    // before masking cart activity, breaking the CPU/cache -> pause feedback.
    reg virtual_pause_quiesced_h;
    always @(posedge hclk) begin
      if(!reset_n || !virtual_enable) virtual_pause_quiesced_h <= 1'b0;
      else virtual_pause_quiesced_h <= virtual_quiesced_h;
    end
    wire state_save,state_load,state_hold,state_engine_busy,state_loaded,state_fault;
    assign state_hold_h=state_hold;
    // Read-only diagnostics: reset, engine sleep, external pause, quiesced,
    // cartridge wait, CPU halt, LCD enabled. No control feedback.
    assign state_debug_h={gbreset_effective,sleep_savestate,virtual_quiesce_h,
                          virtual_quiesced_h,virtual_cart_wait,cpu_halt,gb_lcd_on};
    wire [7:0] state_be;
    wire [63:0] ss_din; wire [9:0] ss_adr; wire ss_wr,ss_rst;
    wire [15:0] mapper_back;reg [15:0] mapper_restore=16'h0001;
    // Slot 39: mapper extension (MBC3 RTC latch state). Zero for other mappers.
    wire [63:0] mapper_ext_back;reg [63:0] mapper_ext_restore=64'd0;
    always @(posedge hclk) begin
      if(ss_rst) begin mapper_restore<=16'h0001;mapper_ext_restore<=64'd0;end
      else begin
        if(ss_wr && ss_adr==32) mapper_restore<=ss_din[15:0];
        if(ss_wr && ss_adr==39) mapper_ext_restore<=ss_din;
      end
    end
    wire [63:0] mapper_bus=ss_adr==32 ? {48'd0,mapper_back} : ss_adr==39 ? mapper_ext_back : 64'd0;
    wire cram_request,cram_done,cram_write;
    wire [19:0] cram_addr;wire [7:0] cram_wdata,cram_rdata;
    wire [7:0] state_ram_size=!virtual_has_ram ? 8'hff :
       virtual_ram_mask==1 ? 8'd1 : virtual_ram_mask==0 ? 8'd2 :
       virtual_ram_mask==3 ? 8'd3 : virtual_ram_mask==7 ? 8'd5 : 8'd4;
    wire state_supported=virtual_enable && virtual_initialized &&
       (virtual_mapper_select==1 || virtual_mapper_select==4 ||
        (virtual_mapper_select==3 && !virtual_mbc30)); // MBC30 (64 KiB RAM) untested
    snapshot_control state_control(
      .gclk(state_gclk),.greset(state_greset),.hclk(hclk),.hreset(~reset_n),
      .request_g(state_request_g),.load_g(state_load_g),.import_g(state_import_g),.busy_g(state_busy_g),.status_g(state_status_g),
      .enabled_h(virtual_enable),.reset_core_h(gbreset_effective),.supported_h(state_supported),
      .quiesced_h(virtual_quiesced_h),.engine_busy_h(state_engine_busy),.engine_loaded_h(state_loaded),
      .memory_fault_h(state_fault),.save_h(state_save),.load_h(state_load),.hold_h(state_hold));
    wire cache_request,cache_read,cache_next,cache_done,cache_rvalid;
    wire [22:0] cache_address;wire [15:0] cache_wdata,cache_rdata;wire [10:0] cache_length;
    snapshot_memory state_memory(
      .hclk(hclk),.hreset(~reset_n),.xclk(xclk),.xreset(xreset),
      .hrequest(SAVE_out_ena),.hread(SAVE_out_rnw),.haddress(SAVE_out_Adr),
      .hwdata(SAVE_out_Din),.hbe(state_be),.hdone(SAVE_out_done),.hrdata(SAVE_out_Dout),.fault(state_fault),
      .crequest(cache_request),.cread(cache_read),.caddress(cache_address),.cwdata(cache_wdata),.clength(cache_length),
      .cnext(cache_next),.cdone(cache_done),.crvalid(cache_rvalid),.crdata(cache_rdata),
      .mrequest(virtual_xrequest),.mread(virtual_xread),.maddress(virtual_xaddress),
      .mwdata(virtual_xwritedata),.mlength(virtual_xlength),.mnext(virtual_xwrite_next),
      .mdone(virtual_xdone),.mrvalid(virtual_xread_valid),.mrdata(virtual_xreaddata));
    virtual_cart_runtime u_virtual_cart_runtime (
        .quiesce_requested(virtual_quiesce_h | state_hold),
        .ss_mode(sleep_savestate),.ss_request(cram_request),.ss_write(cram_write),
        .ss_address(cram_addr),.ss_wdata(cram_wdata),.ss_rdata(cram_rdata),.ss_done(cram_done),
        .ss_load(state_loaded),.ss_mapper_data(mapper_restore),.ss_mapper_back(mapper_back),
        .ss_mapper_ext_data(mapper_ext_restore),.ss_mapper_ext_back(mapper_ext_back),
        .hclk(hclk),
        .xclk(xclk),
        .hreset(~reset_n),
        .xreset(xreset),
        .enable(virtual_enable),
        .quiesced(virtual_quiesced_h),
        .mapper_select(virtual_mapper_select),
        .mbc1m(virtual_mbc1m),
        .mbc30(virtual_mbc30),
        .cart_mbc_type(virtual_cart_type),
        .has_ram(virtual_has_ram),
        .ram_mask(virtual_ram_mask),
        .rom_mask(virtual_rom_mask),
        .rtc_time(virtual_rtc_time),
        .rtc_restore_write(virtual_rtc_restore_write),
        .rtc_restore_address(virtual_rtc_restore_address),
        .rtc_restore_data(virtual_rtc_restore_data),
        .ce_cpu(core_ce_cpu),
        .cart_addr(a[14:0]),
        .cart_a15(a[15]),
        .cart_rd(rd),
        .cart_wr_cycle(wr),
        .cart_wr_pulse(cart_wr_pulse),
        .cart_di(CART_DOUT),
        .nCS(nCS),
        .cart_do(virtual_cart_do),
        .cart_oe(virtual_cart_oe),
        .cart_wait(virtual_cart_wait),
        .initialized(virtual_initialized),
        .save_dirty(virtual_save_dirty),
        .xrequest(cache_request),
        .xread(cache_read),
        .xaddress(cache_address),
        .xwritedata(cache_wdata),
        .xlength(cache_length),
        .xwrite_next(cache_next),
        .xdone(cache_done),
        .xread_valid(cache_rvalid),
        .xreaddata(cache_rdata),
        .xsnapshot_request_toggle(virtual_snapshot_request_toggle),
        .xsnapshot_block(virtual_snapshot_block),
        .xsnapshot_sequence(virtual_snapshot_sequence),
        .xsnapshot_ready(virtual_snapshot_ready),
        .xsnapshot_ready_sequence(virtual_snapshot_ready_sequence),
        .qclk(virtual_snapshot_qclk),
        .qsnapshot_address(virtual_snapshot_qaddress),
        .qsnapshot_data(virtual_snapshot_qdata),
        .rtc_timestamp(virtual_rtc_timestamp),
        .rtc_saved_time(virtual_rtc_saved_time),
        .rtc_in_use(virtual_rtc_in_use)
    );

    assign virtual_rtc_state = virtual_rtc_saved_time[28:0];

    wire [7:0] selected_cart_do = virtual_enable
        ? virtual_cart_do : CART_DIN_r1;
    wire selected_cart_oe = virtual_enable ? virtual_cart_oe : cart_oe;

    gb u_gb(
        .reset(gbreset_effective),
        .clk_sys(hclk),
        .ce(ce),
        .ce_2x(ce_2x),

        .isGBC(1'd1),
        .real_cgb_boot(1'd0),
        .customPaletteEna(customPaletteEna),
        .paletteBGIn(paletteBGIn),
        .paletteOBJ0In(paletteOBJ0In),
        .paletteOBJ1In(paletteOBJ1In),

        .paletteOff(paletteOff),
        .gbc_mode(gbc_mode),
        .gpd(gpd),

        // cartridge interface
        // can adress up to 1MB ROM
        .ext_bus_addr(a[14:0]),
        .ext_bus_a15(a[15]),
        .cart_rd(rd),
        .cart_wr(wr),
        .cart_wr_pulse(cart_wr_pulse),
        .cart_do(selected_cart_do),
        .cart_di(CART_DOUT),
        .cart_oe(selected_cart_oe),
        .cart_wait(virtual_enable && virtual_cart_wait),
        .virtual_cart_mode(virtual_enable),
        .virtual_quiesce(virtual_quiesce_h | state_hold),
        .virtual_quiesced(virtual_quiesced_h),
        .TSTATEo(TSTATEo),
        .TSTATE1(TSTATE1),
        .TSTATE2(TSTATE2),
        .TSTATE3(TSTATE3),
        .TSTATE4(TSTATE4),
        // WRAM or Cart RAM CS
        .nCS(nCS),

        .cgb_boot_download(1'd0),
        .dmg_boot_download(1'd0),
        .ioctl_wr(1'd0),
        .ioctl_addr(25'd0),
        .ioctl_dout(16'd0),

        // Bootrom features
        .boot_rom_enabled(boot_rom_enabled),

        .boot_gba_en(1'd0),
        .fast_boot_en(1'd0),
        // audio
        .audio_l(snd_l),
        .audio_r(snd_r),

        // Megaduck?
        .megaduck(1'd0),

        .IR_RX(IR_RX),
        .IR_LED(IR_LED),

        // lcd interface
        .lcd_clkena(gb_lcd_clkena),
        .lcd_data(gb_lcd_data),
        //      .lcd_data_gb(),
        .lcd_mode(gb_lcd_mode),
        .lcd_on(gb_lcd_on),
        .lcd_vsync(gb_lcd_vsync),

        .joy_p54(joy_p54),
        .joy_din(joy_din),

        .lcd_on_int(lcd_on_int),
        .lcd_off_overwrite(lcd_off_overwrite),

        .speed(cpu_speed),   //GBC
        .cpu_stop(cpu_stop),
        .cpu_halt(cpu_halt),
        .DMA_on(DMA_on),
        .hdma_active(hdma_active),
        .gg_reset(1'd0),
        .gg_en(1'd0),
        .gg_code(129'd0),
        .gg_available(),

        //serial port
        .sc_int_clock2(sc_int_clock2),
        .serial_clk_in(serial_clk_in),
        .serial_clk_out(serial_clk_out),
        .serial_data_in(serial_data_in),
        .serial_data_out(serial_data_out),

        // savestates
        .increaseSSHeaderCount(1'd1),
        .cart_ram_size(state_ram_size),
        .save_state(state_save),
        .load_state(state_load),
        .savestate_number(2'd0),
        .sleep_savestate(sleep_savestate),

        .SaveStateExt_Din(ss_din), 
        .SaveStateExt_Adr(ss_adr), 
        .SaveStateExt_wren(ss_wr),
        .SaveStateExt_rst(ss_rst), 
        .SaveStateExt_Dout(mapper_bus),
        .SaveStateExt_load(state_loaded),

        .Savestate_CRAMAddr(cram_addr),     
        .Savestate_CRAMRWrEn(cram_write),    
        .Savestate_CRAMWriteData(cram_wdata),
        .Savestate_CRAMReadData(cram_rdata),
        .Savestate_CRAMRequest(cram_request),.Savestate_CRAMDone(cram_done),
        .state_busy(state_engine_busy),.SAVE_out_be(state_be),

        .SAVE_out_Din  (SAVE_out_Din ),    // data read from savestate
        .SAVE_out_Dout (SAVE_out_Dout),  // data written to savestate
        .SAVE_out_Adr  (SAVE_out_Adr ),    // all addresses are DWORD addresses!
        .SAVE_out_rnw  (SAVE_out_rnw ),     // read = 1, write = 0
        .SAVE_out_ena  (SAVE_out_ena ),     // one cycle high for each action
        .SAVE_out_done (SAVE_out_done),    // should be one cycle high when write is done or read value is valid

        .rewind_on(1'd0),
        .rewind_active(1'd0)
    );
    
    audio_filter_deterministic u_audio_filter
    (
       .reset    (~reset_n | state_hold),
       .clk      (hclk),

       .core_l   (snd_l),
       .core_r   (snd_r),

       .filter_l (left),
       .filter_r (right)
    );
    

endmodule
