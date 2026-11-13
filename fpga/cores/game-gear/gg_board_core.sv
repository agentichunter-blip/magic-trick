// Game Gear core logic driven by the existing 67.108864 MHz xclk. No physical IO.
module gg_board_core(
 input [128:0] gg_code,
 input gg_clear,
 input clk, reset_n, rom_ready, quiesce,
 input [6:0] buttons,
 input [7:0] rom_do,
 output [23:0] rom_a,
 output rom_rd,
 output [11:0] color,
 output [8:0] x,y,
 output [15:0] audioL,audioR,
 output ce_pix, hblank, vblank,
 output ram_we, output [13:0] ram_a, output [7:0] ram_d,
 // Native save state (see gg_state_engine.sv). Byte layout of state_in/out:
 // z80 0..28, vdp 29..44, psg 45..51, psg_ext 52..63, psg_div 64,
 // audio_control 65..66, mapper 67..74, eeprom 75..82, io 83..86,
 // video 87..89, clock 90..93, audio phase 94 (bit 0).
 input state_request, state_release, state_apply,
 input [759:0] state_in, output [759:0] state_out,
 output reg state_frozen=0, output freeze_active,
 // While frozen: 0 work RAM, 1 cartridge RAM, 2 VRAM, 3 palette (12-bit word).
 input [1:0] mem_select, input [14:0] mem_address, input mem_write,
 input [11:0] mem_data, output [7:0] mem_q,
 // Battery save (cartridge RAM, also the EEPROM games' storage). boot_hold
 // starts the core frozen before its first instruction so the engine can
 // write the save first. cart_q reads cartridge RAM at mem_address through a
 // second port while the game runs; cart_dirty records game writes, and
 // cart_written which 1 KiB blocks they hit. cart_dirty_clear (a copy starts)
 // moves cart_written to cart_snapshot and clears it in the same edge, so no
 // write is lost; a write in that edge counts as before the copy.
 input boot_hold, input cart_dirty_clear, output reg cart_dirty=0, output reg [7:0] cart_q=0,
 output reg [31:0] cart_written=0, output reg [31:0] cart_snapshot=0
);
wire [229:0] z80_out; wire [127:0] vdp_out; wire [383:0] cram_out;
wire [55:0] psg_out; wire [95:0] psg_ext_out; wire [3:0] psg_div_out;
wire [10:0] audio_control_out; wire [63:0] mapper_out,eeprom_out;
wire [31:0] io_out,clock_out; wire [21:0] video_out;
wire audio_quiescent,audio_phase_out,z80_m1_n,z80_mreq_n;
wire [1:0] z80_iset; wire [7:0] vram_q;
assign state_out={7'd0,audio_phase_out,clock_out,2'd0,video_out,io_out,eeprom_out,
 mapper_out,5'd0,audio_control_out,4'd0,psg_div_out,psg_ext_out,psg_out,vdp_out,2'd0,z80_out};
wire [229:0] z80_in=state_in[0 +: 230];
wire [127:0] vdp_in=state_in[232 +: 128];
wire [55:0] psg_in=state_in[360 +: 56];
wire [95:0] psg_ext_in=state_in[416 +: 96];
wire [3:0] psg_div_in=state_in[512 +: 4];
wire [10:0] audio_control_in=state_in[520 +: 11];
wire [63:0] mapper_in=state_in[536 +: 64];
wire [63:0] eeprom_in=state_in[600 +: 64];
wire [31:0] io_in=state_in[664 +: 32];
wire [21:0] video_in=state_in[696 +: 22];
wire [31:0] clock_in=state_in[720 +: 32];
wire phase_in=state_in[752];
// Freeze on a clean Z80 opcode fetch in vertical blank, then hold every clock
// enable. The first frozen cycle rewrites the CPU registers with themselves,
// which normalizes hidden fetch context; SAVE and LOAD then resume from the
// same entry point (validated by the replay harness).
reg frozen=0; reg [7:0] settle=0;
// Restore strobes come from dedicated, replicated registers (timing): a set is
// held at least two cycles while every clock enable is stopped, so the
// constraints file may give these paths two cycles.
reg apply_q=0 /* synthesis syn_maxfan=24 */;
reg z80_set_q=0 /* synthesis syn_maxfan=24 */;
wire apply=apply_q;
wire ce_cpu,ce_vdp,ce_sp;
// Freeze between CPU steps while the Z80 sits at a clean opcode fetch in
// vertical blank. The decode is registered so it stays out of the CPU enable
// path; it reflects the previous cycle, so also require that the CPU did not
// step on this edge or the previous one (its outputs change only on steps).
// x<250 keeps a one-cycle-old video position inside the same blank line.
wire cpu_step=ce_cpu && rom_ready && !quiesce && !frozen;
reg boundary_q=0, cpu_step_q=0;
always @(posedge clk) begin
 boundary_q<=!z80_m1_n && !z80_mreq_n && z80_iset==0 && vblank && x<250;
 cpu_step_q<=cpu_step;
end
wire capture_boundary=state_request && boundary_q && !cpu_step && !cpu_step_q && !quiesce;
wire canonicalize_cpu=frozen && !state_frozen && settle<2;
assign freeze_active=frozen;
always @(posedge clk) begin
 apply_q<=state_apply && state_frozen;
 z80_set_q<=(state_apply && state_frozen) || canonicalize_cpu;
end
always @(posedge clk) begin
 if(!reset_n) begin frozen<=boot_hold;state_frozen<=boot_hold;settle<=0;end
 else if(state_release && state_frozen) begin frozen<=0;state_frozen<=0;end
 else if(!frozen) begin
  if(capture_boundary) begin frozen<=1;settle<=0;end
 end else if(!state_frozen) begin
  if(settle<64) settle<=settle+1'b1;
  else if(audio_quiescent) state_frozen<=1;
 end
end
gg_state_clock clock_enables(.clk(clk),.reset_n(reset_n),.hold(frozen),.restore(apply),
 .state_in(clock_in),.state_out(clock_out),.cpu(ce_cpu),.vdp(ce_vdp),.pixel(ce_pix),.sprite(ce_sp));
wire [7:0] vcounter_cpu;
wire mask_column,smode_M1,smode_M2,smode_M3,smode_M4;
wire [14:0] nvram_a;
wire [7:0] nvram_d;
wire nvram_we;
reg [7:0] ram_q,nvram_q;
// Game Gear drives ram_a[13] low (system.vhd); 8 KiB frees block RAM.
reg [7:0] ram[0:8191];
// 32 KiB cartridge RAM: two 16 KiB banks ($FFFC bit 2). Shining Force keeps
// work RAM and its stack in the second bank, so aliasing the banks (16 KiB
// until 2026-11) crashed it at boot.
reg [7:0] nvram[0:32767];
wire dma=state_frozen;
wire [12:0] ra=dma ? mem_address[12:0] : ram_a[12:0];
wire [14:0] na=dma ? mem_address : nvram_a[14:0];
wire rw=dma ? mem_write && mem_select==0 : ram_we && !frozen;
wire nw=dma ? mem_write && mem_select==1 : nvram_we && !frozen;
wire [7:0] rd=dma ? mem_data[7:0] : ram_d, nd=dma ? mem_data[7:0] : nvram_d;
always @(posedge clk) begin
 ram_q <= rw ? rd : ram[ra];
 nvram_q <= nw ? nd : nvram[na];
 if(rw) ram[ra] <= rd;
 if(nw) nvram[na] <= nd;
end
always @(posedge clk) cart_q <= nvram[mem_address];
wire [31:0] cart_write_block=(!dma && nw) ? 32'd1<<na[14:10] : 32'd0;
always @(posedge clk)
 if(!reset_n) begin cart_dirty<=0;cart_written<=0;cart_snapshot<=0;end
 else if(cart_dirty_clear) begin cart_snapshot<=cart_written|cart_write_block;cart_written<=0;cart_dirty<=0;end
 else begin cart_written<=cart_written|cart_write_block;if(!dma && nw) cart_dirty<=1;end
wire [11:0] cram_word=cram_out[12*mem_address[5:1] +: 12];
assign mem_q=mem_select==0 ? ram_q : mem_select==1 ? nvram_q : mem_select==2 ? vram_q :
 mem_address[0] ? {4'd0,cram_word[11:8]} : cram_word[7:0];
system core(
 .clk_sys(clk),
 .ce_cpu(cpu_step),
 // PSG runs at the true CPU rate; only pause/quiesce stops it. Gating it by
 // rom_ready froze tone counters on every PSRAM cache miss (garbled audio).
 .ce_psg(ce_cpu && !quiesce && !frozen),
 .ce_vdp(ce_vdp && !frozen),
 .ce_pix(ce_pix && !frozen),
 .ce_sp(ce_sp && !frozen),
 .turbo('0),
 .gg(1'b1),
 .ggres(1'b1),
 .systeme('0),
 .bios_en('0),
 .ext_bios_sel('0),
 .ext_bios_loaded('0),
 .gg_bios_en('0),
 .ext_gg_bios_loaded('0),
 .GG_BIOSWEN('0),
 .GG_EN('0),
 .GG_CODE(gg_code),
 .GG_RESET(gg_clear),
 .GG_AVAIL(),
 .gg_link_en('0),
 .gg_link_in(7'h7f),
 .gg_link_out(),
 .RESET_n(reset_n),
 .rom_rd(rom_rd),
 .rom_a(rom_a),
 .rom_do(rom_do),
 .j1_up(~buttons[0]),
 .j1_down(~buttons[1]),
 .j1_left(~buttons[2]),
 .j1_right(~buttons[3]),
 .j1_tl(~buttons[4]),
 .j1_tr(~buttons[5]),
 .j1_th(1'b1),
 .j1_start(buttons[6]),
 .j1_coin('0),
 .j1_a3('0),
 .j2_up(1'b1),
 .j2_down(1'b1),
 .j2_left(1'b1),
 .j2_right(1'b1),
 .j2_tl(1'b1),
 .j2_tr(1'b1),
 .j2_th(1'b1),
 .j2_start('0),
 .j2_coin('0),
 .j2_a3('0),
 .pause(~buttons[6]),
 .soft_reset('0),
 .E0Type('0),
 .E1Use('0),
 .E2Use('0),
 .E0('0),
 .F2('0),
 .F3('0),
 .has_paddle('0),
 .has_pedal('0),
 .paddle('0),
 .paddle2('0),
 .pedal('0),
 .sc3000_en('0),
 .sc_multicart_en('0),
 .sc_megacart_en('0),
 .sc_cart_ram('0),
 .sk1100_en('0),
 .sk1100_row_sel(),
 .sk1100_row_data('0),
 .j1_tr_out(),
 .j1_th_out(),
 .j2_tr_out(),
 .j2_th_out(),
 .x(x),
 .y(y),
 .vcounter_cpu(vcounter_cpu),
 .color(color),
 .palettemode('0),
 .mask_column(mask_column),
 .black_column('0),
 .smode_M1(smode_M1),
 .smode_M2(smode_M2),
 .smode_M3(smode_M3),
 .smode_M4(smode_M4),
 .ysj_quirk('0),
 .pal('0),
 .region('0),
 .mapper_lock('0),
 .mapper_codies_force('0),
 .mapper_dahjee_a_force('0),
 .mapper_linear_force('0),
 .mapper_zemina_force('0),
 .mapper_evolution_force('0),
 .evolution_gg_active(),
 .evolution_active(),
 .mapper_eeprom_out(),
 .vdp_enables('0),
 .psg_enables('0),
 .audioL(audioL),
 .audioR(audioR),
 .fm_ena('0),
 .dbr(1'b1),
 .sp64('0),
 .ram_a(ram_a),
 .ram_d(ram_d),
 .ram_we(ram_we),
 .ram_q(ram_q),
 .nvram_a(nvram_a),
 .nvram_d(nvram_d),
 .nvram_we(nvram_we),
 .nvram_q(nvram_q),
 .encrypt('0),
 .key_a(),
 .key_d('0),
 .ROMCL(clk),
 .ROMAD('0),
 .ROMDT('0),
 .ROMEN('0),
 .BIOSWEN('0),
 .z80_reg_out(z80_out),
 .z80_dir(apply_q ? z80_in : z80_out),
 .z80_set(z80_set_q),
 .vdp_regs_out(vdp_out),
 .vdp_regs_in(vdp_in),
 .vdp_regs_set(apply),
 .vdp_cram_out(cram_out),
 .ss_cram_wr(dma && mem_write && mem_select==3),
 .ss_cram_A(mem_address[5:1]),
 .ss_cram_D(mem_data),
 .ss_vram_en(dma),
 .ss_vram_A({1'b0,mem_address[13:0]}),
 .ss_vram_D(vram_q),
 .ss_vram_WE(dma && mem_write && mem_select==2),
 .ss_vram_WA({1'b0,mem_address[13:0]}),
 .ss_vram_WD(mem_data[7:0]),
 .psg_out(psg_out),
 .psg_in(psg_in),
 .psg_set(apply),
 .psg_ext_out(psg_ext_out),
 .psg2_ext_out(),
 .psg_ext_in(psg_ext_in),
 .psg2_ext_in('0),
 .psg_div_out(psg_div_out),
 .psg_div_in(psg_div_in),
 .audio_control_out(audio_control_out),
 .audio_control_in(audio_control_in),
 .audio_quiescent(audio_quiescent),
 .audio_phase_out(audio_phase_out),
 .audio_phase_in(phase_in),
 .audio_phase_set(state_release && state_frozen),
 .audio_ext_set(apply),
 .mapper_out(mapper_out),
 .mapper_in(mapper_in),
 .mapper_set(apply),
 .evolution_ss_out(),
 .evolution_ss_in('0),
 .evolution_ss_set('0),
 .eeprom_ss_out(eeprom_out),
 .eeprom_ss_in(eeprom_in),
 .eeprom_ss_set(apply),
 .z80_m1_n(z80_m1_n),
 .z80_mreq_n(z80_mreq_n),
 .z80_iset(z80_iset),
 .vdp2_regs_out(),
 .vdp2_regs_in('0),
 .vdp2_regs_set('0),
 .vdp2_cram_out(),
 .ss_cram2_wr('0),
 .ss_cram2_A('0),
 .ss_cram2_D('0),
 .ss_vram2_en('0),
 .ss_vram2_A('0),
 .ss_vram2_D(),
 .ss_vram2_WE('0),
 .ss_vram2_WA('0),
 .ss_vram2_WD('0),
 .psg2_out(),
 .psg2_in('0),
 .psg2_set('0),
 .io_state_out(io_out),
 .io_state_in(io_in),
 .io_state_set(apply),
 .se_pause('0),
 .ss_freeze(frozen)
);
video timing(
 .clk(clk), .ce_pix(ce_pix && !frozen), .pal(1'b0), .border(1'b0), .ggres(1'b1),
 .mask_column(mask_column),.cut_mask(1'b0),
 .smode_M1(smode_M1),.smode_M2(smode_M2),.smode_M3(smode_M3),.smode_M4(smode_M4),
 .video_state_out(video_out),.video_state_in(video_in),.video_state_set(apply),
 .x(x),.y(y),.vcounter_cpu(vcounter_cpu),.hsync(),.vsync(),.hblank(hblank),.vblank(vblank)
);
endmodule
