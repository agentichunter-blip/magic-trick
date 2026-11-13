create_generated_clock -name xclk2 -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 1 -multiply_by 4 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT0}]
create_generated_clock -name pclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 1 -multiply_by 1 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT1}]
create_generated_clock -name hclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 2 -multiply_by 1 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT2}]
create_generated_clock -name gclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 4 -multiply_by 1 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT3}]
create_generated_clock -name xclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 1 -multiply_by 2 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT4}]

// Audio serialization uses gclk with an alternate-cycle enable (ModRetro 18.11):
// there is no divided audio clock domain any more.

create_clock -name sclk -period 25 [get_ports {QSPI_CLK}]
create_clock -name exclk -period 29.802322 [get_ports {CLK_FPGA}]

set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {hclk}]
set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {gclk}]
set_clock_groups -asynchronous -group [get_clocks {hclk}] -group [get_clocks {gclk}]

set_max_delay -from [get_ports {CART_D[*]}] -to [get_clocks {hclk}] 13
set_max_delay -from [get_clocks {hclk}] -to  [get_ports {CART_A[*]}] 14
set_max_delay -from [get_clocks {hclk}] -to  [get_ports {CART_WR}] 14
set_max_delay -from [get_clocks {hclk}] -to  [get_ports {CART_RD}] 14
set_max_delay -from [get_clocks {hclk}] -to  [get_ports {CART_CS}] 14
set_max_delay -from [get_clocks {hclk}] -to  [get_ports {LINK_SD}] 14
set_max_delay -from [get_clocks {hclk}] -to  [get_ports {CART_D[*]}] 14

create_clock -name ck24 -period 41.666667 -waveform {0 20.833333} [get_ports {CLK_24MHz}]

set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {hclk}]
set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {gclk}]
set_clock_groups -asynchronous -group [get_clocks {hclk}] -group [get_clocks {gclk}]

// USB Clocks
create_generated_clock -name PHY_CLKOUT -source [get_ports {CLK_24MHz}] -master_clock ck24 -divide_by 16 -multiply_by 40 [get_pins {u_usb_top/u_Gowin_PLL_USB/PLLA_inst/CLKOUT1}]
create_generated_clock -name fclk_960M -source [get_ports {CLK_24MHz}] -master_clock ck24 -divide_by 1 -multiply_by 40 [get_nets {u_usb_top/fclk_960M}]
create_generated_clock -name clk24p -source [get_ports {CLK_24MHz}] -master_clock ck24 -divide_by 1 -multiply_by 1 [get_pins {u_usb_top/u_Gowin_PLL_USB/PLLA_inst/CLKOUT2}]
create_clock -name usbintsclk -period 8 -waveform {0 4} [get_nets {u_usb_top/u_USB_SoftPHY_Top/usb2_0_softphy/u_usb_20_phy_utmi/u_usb2_0_softphy/u_usb_phy_hs/sclk}] -add
set_clock_groups -asynchronous -group [get_clocks {PHY_CLKOUT}] -group [get_clocks {fclk_960M}]
set_clock_groups -asynchronous -group [get_clocks {PHY_CLKOUT}] -group [get_clocks {usbintsclk}]

# Game Gear prototype additions. Review resolved collections in vendor STA.
# Never false-path the bundled audio data bus: held data has a bounded budget.
set_max_delay -from [get_regs {u_emu_system_top/audio_cdc/held_*_s0}] -to [get_regs {u_emu_system_top/audio_cdc/sample_*_s0}] 20
# Gray pointer bits must arrive within one source cycle (xclk ~14.9 ns).
set_max_delay -from [get_regs {u_emu_system_top/video_cdc/fifo/wgray_*_s0}] -to [get_regs {u_emu_system_top/video_cdc/fifo/wm_*_s0}] 14
set_max_delay -from [get_regs {u_emu_system_top/video_cdc/fifo/rgray_*_s0}] -to [get_regs {u_emu_system_top/video_cdc/fifo/rm_*_s0}] 50

# Native save state (gg_state_engine.sv, gg_board_core.sv). The restore data
# changes only during a load, long before the apply, and every restore strobe
# is held for two cycles while all core clock enables are stopped (frozen), so
# a late arrival only repeats the same register load. The freeze flag itself is
# not relaxed: it switches while the game runs.
set_multicycle_path -from [get_regs {u_emu_system_top/state_engine/state_in_*}] -setup -end 2
set_multicycle_path -from [get_regs {u_emu_system_top/state_engine/state_in_*}] -hold -end 1
set_multicycle_path -from [get_regs {u_emu_system_top/core/z80_set_q*}] -setup -end 2
set_multicycle_path -from [get_regs {u_emu_system_top/core/z80_set_q*}] -hold -end 1
set_multicycle_path -from [get_regs {u_emu_system_top/core/apply_q*}] -setup -end 2
set_multicycle_path -from [get_regs {u_emu_system_top/core/apply_q*}] -hold -end 1
# Every T80 register updates only on the CPU clock enable (about one xclk cycle
# in 19) or on the save-state set while frozen, so Z80-internal paths have at
# least two cycles. Paths into the Z80 from elsewhere (ROM data, the enable)
# keep their single-cycle requirement. Gowin's * does not descend into
# sub-instances, so each Z80 level is listed. The one T80 exception, the NMI edge
# detector (OldNMI_n/NMI_s update every cycle), does not exist here: NMI_n is
# constant in Game Gear mode and synthesis removes it. Re-check if that changes.
set_multicycle_path -from [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -to [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -setup -end 2
set_multicycle_path -from [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -to [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -hold -end 1
# Memory data into the Z80: work and cartridge RAM outputs change only when the
# CPU changes the address (on its enable), and the CPU latches data only on a
# later enable edge. The ROM cache signals ready one cycle after its data is
# valid (gg_rom_bridge.sv), so its bytes are also two cycles old when latched.
set_multicycle_path -through [get_pins {u_emu_system_top/core/ram_ram_*/DO* u_emu_system_top/core/nvram_nvram_*/DO* u_emu_system_top/rom_bridge/cmem_cmem_*/DO*}] -to [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -setup -end 2
set_multicycle_path -through [get_pins {u_emu_system_top/core/ram_ram_*/DO* u_emu_system_top/core/nvram_nvram_*/DO* u_emu_system_top/rom_bridge/cmem_cmem_*/DO*}] -to [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -hold -end 1
# Mapper bank registers, mapper auto-detection flags and the BIOS flag
# (bootloader_n, netlist n1042) change only on a CPU write (an enable edge) or during a restore, so their paths into
# the Z80 also have two cycles. Input and VDP paths keep one cycle.
set_multicycle_path -from [get_regs {u_emu_system_top/core/core_0/mapper_banking/* u_emu_system_top/core/core_0/mapper_detect_inst/* u_emu_system_top/core/core_0/n1042_s*}] -to [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -setup -end 2
set_multicycle_path -from [get_regs {u_emu_system_top/core/core_0/mapper_banking/* u_emu_system_top/core/core_0/mapper_detect_inst/* u_emu_system_top/core/core_0/n1042_s*}] -to [get_regs {u_emu_system_top/core/core_0/z80_inst/* u_emu_system_top/core/core_0/z80_inst/u0/* u_emu_system_top/core/core_0/z80_inst/u0/regs/* u_emu_system_top/core/core_0/z80_inst/u0/alu/* u_emu_system_top/core/core_0/z80_inst/u0/mcode/*}] -hold -end 1
