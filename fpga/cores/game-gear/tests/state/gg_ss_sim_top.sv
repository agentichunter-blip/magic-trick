// Simulation wrapper: board core + save-state engine + the real ROM cache.
// PSRAM is modelled in C++ behind the cache's bus interface.
module gg_ss_sim_top(
 input clk, reset_n, input [6:0] buttons,
 output bus_request, output [22:0] bus_address, input bus_done, bus_valid, input [15:0] bus_data,
 input freeze_t, release_t, load_t, block_t, input [6:0] block_number, input [15:0] block_seq,
 output block_ready, output [15:0] block_ready_seq, output busy, fault, frozen, loaded,
 input [9:0] qaddress, output [7:0] qdata,
 output [23:0] rom_a, output rom_rd, output [8:0] x,y, output ce_pix, output [11:0] color,
 output [15:0] audioL,audioR, output ram_we, output [13:0] ram_a, output [7:0] ram_d, output rom_ready,
 output dbg_rom_fault, output [3:0] dbg_state, output [15:0] dbg_position
);
 assign dbg_rom_fault=rom_fault; assign dbg_state=engine.state; assign dbg_position=engine.position;
 wire state_request,state_release,state_apply,state_frozen;
 wire [759:0] state_in,state_out;
 wire [1:0] mem_select;wire [13:0] mem_address;wire mem_write;wire [11:0] mem_data;wire [7:0] mem_q;
 wire owns_rom,eng_read,cache_invalidate;wire [23:0] eng_address;
 wire [7:0] rom_byte;wire rom_fault,rom_idle;
 gg_rom_bridge #(.TIMEOUT_CYCLES(65536)) bridge(.clk(clk),.reset(!reset_n),.enable(reset_n),
  .invalidate(!reset_n || cache_invalidate),
  .read(owns_rom ? eng_read : rom_rd),.address(owns_rom ? eng_address : rom_a),
  .data(rom_byte),.ready(rom_ready),.idle(rom_idle),.fault(rom_fault),
  .request(bus_request),.bus_read(),.bus_address(bus_address),.bus_write_data(),.bus_length(),
  .done(bus_done),.valid(bus_valid),.read_data(bus_data));
 gg_board_core core(.gg_code(129'd0),.gg_clear(1'b0),.clk(clk),.reset_n(reset_n),
  .rom_ready(rom_ready),.quiesce(1'b0),.buttons(buttons),.rom_do(rom_byte),.rom_a(rom_a),.rom_rd(rom_rd),
  .color(color),.x(x),.y(y),.audioL(audioL),.audioR(audioR),.ce_pix(ce_pix),.hblank(),.vblank(),
  .ram_we(ram_we),.ram_a(ram_a),.ram_d(ram_d),
  .state_request(state_request),.state_release(state_release),.state_apply(state_apply),
  .state_in(state_in),.state_out(state_out),.state_frozen(state_frozen),
  .mem_select(mem_select),.mem_address(mem_address),.mem_write(mem_write),.mem_data(mem_data),.mem_q(mem_q));
 gg_state_engine engine(.clk(clk),.reset(!reset_n),
  .freeze_toggle(freeze_t),.release_toggle(release_t),.load_toggle(load_t),.block_toggle(block_t),
  .block_number(block_number),.block_sequence(block_seq),
  .block_ready(block_ready),.block_ready_sequence(block_ready_seq),
  .busy(busy),.fault(fault),.frozen(frozen),.loaded(loaded),
  .state_request(state_request),.state_release(state_release),.state_apply(state_apply),
  .state_in(state_in),.state_out(state_out),.state_frozen(state_frozen),
  .mem_select(mem_select),.mem_address(mem_address),.mem_write(mem_write),.mem_data(mem_data),.mem_q(mem_q),
  .owns_rom(owns_rom),.cache_invalidate(cache_invalidate),.rom_address(eng_address),.rom_read(eng_read),
  .rom_data(rom_byte),.rom_ready(rom_ready),.rom_fault(rom_fault),
  .qclk(clk),.qaddress(qaddress),.qdata(qdata));
endmodule

module gowin_dpb_snapshot(input write_clock,write_enable,input [8:0] write_address,input [15:0] write_data,
 input read_clock,input [8:0] read_address,output reg [15:0] read_data);
 reg [15:0] memory[0:511];
 always @(posedge write_clock) if(write_enable) memory[write_address]<=write_data;
 always @(posedge read_clock) read_data<=memory[read_address];
endmodule
