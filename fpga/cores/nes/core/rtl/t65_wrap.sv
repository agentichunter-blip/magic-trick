// nes.v instantiates `T65` with the VHDL port names; this maps them onto the GHDL netlist
// of rtl/t65_nes.vhd (rtl/gen/t65_nes_netlist.v). mode/BCD_en are fixed in the wrapper
// (6502, no decimal mode = 2A03).
module T65 (
	input  [1:0]  mode,
	input         BCD_en,
	input         res_n,
	input         pwr_n,
	input         clk,
	input         enable,
	input         rdy,
	input         IRQ_n,
	input         NMI_n,
	output        R_W_n,
	output [15:0] A,
	input  [7:0]  DI,
	output [7:0]  DO,
	output        Instrnew,
	input  [63:0] SaveStateBus_Din,
	input  [9:0]  SaveStateBus_Adr,
	input         SaveStateBus_wren,
	input         SaveStateBus_rst,
	input         SaveStateBus_load,
	output [63:0] SaveStateBus_Dout
);
	t65_nes cpu (
		.clk(clk), .enable(enable), .rdy(rdy), .res_n(res_n), .pwr_n(pwr_n),
		.irq_n(IRQ_n), .nmi_n(NMI_n), .r_w_n(R_W_n), .addr(A), .din(DI), .dout(DO),
		.instrnew(Instrnew),
		.ss_din(SaveStateBus_Din), .ss_adr(SaveStateBus_Adr), .ss_wren(SaveStateBus_wren),
		.ss_rst(SaveStateBus_rst), .ss_load(SaveStateBus_load), .ss_dout(SaveStateBus_Dout));
endmodule
