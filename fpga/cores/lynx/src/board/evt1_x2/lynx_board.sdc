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

# Board bridge additions (from the Game Gear core). Review resolved collections in vendor STA.
# Never false-path the bundled audio data bus: held data has a bounded budget.
set_max_delay -from [get_regs {u_emu_system_top/audio_cdc/held_*_s0}] -to [get_regs {u_emu_system_top/audio_cdc/sample_*_s0}] 20
# Gray pointer bits must arrive within one source cycle (xclk ~14.9 ns).
set_max_delay -from [get_regs {u_emu_system_top/video_cdc/fifo/wgray_*_s0}] -to [get_regs {u_emu_system_top/video_cdc/fifo/wm_*_s0}] 14
set_max_delay -from [get_regs {u_emu_system_top/video_cdc/fifo/rgray_*_s0}] -to [get_regs {u_emu_system_top/video_cdc/fifo/rm_*_s0}] 50

# Atari Lynx: the core logic runs at single-cycle xclk until timing says otherwise.

