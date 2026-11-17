// Simulation top: the real Lynx emu_system_top with a behavioural PSRAM on its
// virtual port (reads: one word every 2 cycles after 12; writes: one
// write_next per word, data sampled with it). Not a board model.
module lynx_sim_top(
 input xclk, input hclk, input xreset, input reset_n,
 input [8:0] keys, // up down left right A B start select menu
 input quiesce,
 input stretch,
 input freeze_t, release_t, load_t, block_t, input [6:0] block_n, input [15:0] block_seq,
 output block_ready, output [15:0] block_ready_seq,
 input [9:0] qaddress, output [7:0] qdata,
 output [5:0] state_status,
 // host access to PSRAM (byte address, write when we)
 input host_we, input [22:0] host_addr, input [7:0] host_data, output [7:0] host_q,
 output lcd_pixel, output [14:0] lcd_data, output lcd_repeat, output [1:0] lcd_mode,
 output lcd_on, output lcd_vsync,
 output [15:0] left, output [15:0] right,
 output quiesced, output core_reset_ack, output initialized,
 // debug: CPU bus (trace)
 output [15:0] dbg_addr, output dbg_req, output dbg_rnw, output [7:0] dbg_wdata, output [7:0] dbg_rdata
);
 assign dbg_addr=dut.core.lynx.ilynx.cpu_bus_addr;
 assign dbg_req=dut.core.lynx.ilynx.cpu_bus_request;
 assign dbg_rnw=dut.core.lynx.ilynx.cpu_bus_rnw;
 assign dbg_wdata=dut.core.lynx.ilynx.cpu_bus_datawrite;
 assign dbg_rdata=dut.core.lynx.ilynx.cpu_bus_dataread;
 reg [7:0] psram[0:(1<<23)-1];
 assign host_q=psram[host_addr];
 wire xrequest,xread; wire [22:0] xaddress; wire [15:0] xwritedata; wire [10:0] xlength;
 reg xwrite_next=0,xdone=0,xread_valid=0; reg [15:0] xreaddata=0;
 reg active=0, rd=0; reg [22:0] addr=0; reg [10:0] words=0; reg [5:0] wait_c=0; reg phase=0;
 always @(posedge xclk) begin
  xwrite_next<=0;xdone<=0;xread_valid<=0;
  if(host_we) psram[host_addr]<=host_data;
  if(!active) begin
   if(xrequest) begin active<=1;rd<=xread;addr<=xaddress;words<=xlength>>1;wait_c<=12;phase<=0; end
  end else if(wait_c!=0) wait_c<=wait_c-1'b1;
  else if(words==0) begin xdone<=1;active<=0; end
  else begin
   phase<=!phase;
   if(phase) begin
    if(rd) begin xread_valid<=1;xreaddata<={psram[addr+1],psram[addr]}; end
    else begin xwrite_next<=1;psram[addr]<=xwritedata[7:0];psram[addr+1]<=xwritedata[15:8]; end
    addr<=addr+2;words<=words-1'b1;
   end
  end
 end
 emu_system_top dut(
  .gg_code_x(129'd0),.gg_clear_x(1'b0),
  .hclk(hclk),.pclk(hclk),.xclk(xclk),.reset_n(reset_n),.xreset(xreset),.POWER_GOOD(1'b1),
  .maintenance_reset_h(1'b0),.virtual_attempt_reset_h(1'b0),.virtual_quiesce_h(quiesce),
  .virtual_quiesced_h(quiesced),.virtual_core_reset_h(core_reset_ack),
  .customPaletteEna(1'b0),.paletteBGIn(64'd0),.paletteOBJ0In(64'd0),.paletteOBJ1In(64'd0),.paletteOff(1'b0),.lynxStretch(stretch),
  .gbc_mode(),.gpd(),
  .BTN_NODIAGONAL(1'b0),.BTN_A(keys[4]),.BTN_B(keys[5]),.BTN_DPAD_DOWN(keys[1]),.BTN_DPAD_LEFT(keys[2]),
  .BTN_DPAD_RIGHT(keys[3]),.BTN_DPAD_UP(keys[0]),.BTN_MENU(keys[8]),.BTN_SEL(keys[7]),.BTN_START(keys[6]),
  .MENU_CLOSED(1'b1),.boot_rom_enabled(),
  .CART_A(),.CART_CLK(),.CART_CS(),.CART_D_IN(8'hff),.CART_D_OUT(),.CART_D_OE(),.CART_RD(),.CART_RST(1'b0),.CART_WR(),
  .virtual_enable(1'b1),.virtual_mapper_select(3'd0),.virtual_mbc1m(1'b0),.virtual_mbc30(1'b0),
  .virtual_cart_type(8'd0),.virtual_has_ram(1'b0),.virtual_ram_mask(4'd0),.virtual_rom_mask(9'd0),
  .virtual_rtc_time(33'd0),.virtual_rtc_restore_write(1'b0),.virtual_rtc_restore_address(3'd0),
  .virtual_rtc_restore_data(16'd0),.virtual_rtc_state(),.virtual_initialized(initialized),.virtual_save_dirty(),
  .virtual_xrequest(xrequest),.virtual_xread(xread),.virtual_xaddress(xaddress),.virtual_xwritedata(xwritedata),
  .virtual_xlength(xlength),.virtual_xwrite_next(xwrite_next),.virtual_xdone(xdone),
  .virtual_xread_valid(xread_valid),.virtual_xreaddata(xreaddata),
  .virtual_snapshot_request_toggle(block_t),.virtual_snapshot_block(block_n),.virtual_snapshot_sequence(block_seq),
  .virtual_snapshot_ready(block_ready),.virtual_snapshot_ready_sequence(block_ready_seq),
  .virtual_state_freeze_toggle(freeze_t),.virtual_state_release_toggle(release_t),.virtual_state_load_toggle(load_t),
  .virtual_cart_import(1'b0),.virtual_state_status(state_status),.virtual_cart_written(),.virtual_cart_snapshot(),
  .virtual_snapshot_qclk(xclk),.virtual_snapshot_qaddress(qaddress),.virtual_snapshot_qdata(qdata),
  .IR_RX(1'b1),.IR_LED(),.LINK_CLK(),.LINK_IN(1'b1),.LINK_OUT(),
  .left(left),.right(right),.lcd_on_int(),.lcd_off_overwrite(),.LCD_INIT_DONE(1'b1),
  .gb_lcd_clkena(lcd_pixel),.gb_lcd_data(lcd_data),.gb_lcd_repeat(lcd_repeat),.gb_lcd_mode(lcd_mode),
  .gb_lcd_on(lcd_on),.gb_lcd_vsync(lcd_vsync));
endmodule

module gowin_dpb_snapshot(input write_clock,write_enable,input [8:0] write_address,input [15:0] write_data,
 input read_clock,input [8:0] read_address,output reg [15:0] read_data);
 reg [15:0] memory[0:511];
 always @(posedge write_clock) if(write_enable) memory[write_address]<=write_data;
 always @(posedge read_clock) read_data<=memory[read_address];
endmodule
