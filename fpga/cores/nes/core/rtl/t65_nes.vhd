-- T65 configured as the NES 2A03 CPU (6502 mode, no decimal), flattened by GHDL --synth
-- into rtl/gen/t65_nes_netlist.v (tools/regen.sh). rtl/t65_wrap.sv presents it to nes.v
-- as the `T65` module. The VHDL sources come unmodified from NES_MiSTer/rtl/t65 and
-- NES_MiSTer/rtl/bus_savestates.vhd.
library IEEE;
use IEEE.std_logic_1164.all;
use work.T65_Pack.all;

entity t65_nes is
  port(
    clk      : in  std_logic;
    enable   : in  std_logic;
    rdy      : in  std_logic;
    res_n    : in  std_logic;
    pwr_n    : in  std_logic;
    irq_n    : in  std_logic;
    nmi_n    : in  std_logic;
    r_w_n    : out std_logic;
    addr     : out std_logic_vector(15 downto 0);
    din      : in  std_logic_vector(7 downto 0);
    dout     : out std_logic_vector(7 downto 0);
    instrnew : out std_logic;
    ss_din   : in  std_logic_vector(63 downto 0);
    ss_adr   : in  std_logic_vector(9 downto 0);
    ss_wren  : in  std_logic;
    ss_rst   : in  std_logic;
    ss_load  : in  std_logic;
    ss_dout  : out std_logic_vector(63 downto 0)
  );
end t65_nes;

architecture rtl of t65_nes is
  signal a24 : std_logic_vector(23 downto 0);
begin
  cpu : entity work.T65
    port map(
      Mode => "00", BCD_en => '0',
      Res_n => res_n, Pwr_n => pwr_n, Enable => enable, Clk => clk, Rdy => rdy,
      Abort_n => '1', IRQ_n => irq_n, NMI_n => nmi_n, SO_n => '1',
      R_W_n => r_w_n, Sync => open, EF => open, MF => open, XF => open, ML_n => open,
      VP_n => open, VDA => open, VPA => open,
      A => a24, DI => din, DO => dout, Regs => open, DEBUG => open, NMI_ack => open,
      Instrnew => instrnew,
      SaveStateBus_Din => ss_din, SaveStateBus_Adr => ss_adr, SaveStateBus_wren => ss_wren,
      SaveStateBus_rst => ss_rst, SaveStateBus_load => ss_load, SaveStateBus_Dout => ss_dout);
  addr <= a24(15 downto 0);
end rtl;
