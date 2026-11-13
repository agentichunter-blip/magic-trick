source sources.tcl
set_device GW5A-EV25UG256CC1/I0 -device_version A 
set_option -synthesis_tool gowinsynthesis
set_option -top_module top
set_option -verilog_std sysv2017
set_option -vhdl_std vhd2008
set_option -rw_check_on_ram 1
set_option -use_sspi_as_gpio 1
set_option -power_on_reset_monitor 1
set_option -use_i2c_as_gpio 1
set_option -use_cpu_as_gpio 1
set_option -multi_boot 1
set_option -bit_format bin
set_option -bg_programming userlogic
set_option -output_base_name magic_trick_game_gear
set_option -use_mspi_as_gpio 1
add_file -type cst      "src/board/evt1_x2/evt1_x2.cst"
#add_file -type gao      "src/psram.rao"

#add_file -type verilog "src/rtl/USB/USBUVC/usbuvc_top.v"
#add_file -type verilog "src/rtl/USB/USBUVC/Gowin_PLL_UVC/Gowin_PLL_UVC.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb_video/usb_defs.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb_video/usb_descriptor_video.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb_video/uvc_defs.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb_device_controller/usb_device_controller.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb2_0_softphy/usb2_0_softphy_top.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb2_0_softphy/usb2_0_softphy_name.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb2_0_softphy/usb2_0_softphy_encryption.v"
#add_file -type verilog "src/rtl/USB/USBUVC/usb2_0_softphy/static_macro_define.v"

add_file -type verilog "src/rtl/USB/USBUVCUART/usbuvcuart_top.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/Gowin_PLL_UVC/Gowin_PLL_UVC.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/color_space_convertor/color_space_convertor.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/fifo_video/fifo_video.v"
add_file -type verilog "src/rtl/USB/USBUVCUART/usb_video/usb_defs.v"
add_file -type verilog "src/rtl/USB/USBUVCUART/usb_video/usb_descriptor_video.v"
add_file -type verilog "src/rtl/USB/USBUVCUART/usb_video/uvc_defs.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/usb_device_controller/usb_device_controller.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/usb2_0_softphy/usb2_0_softphy_top.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/usb2_0_softphy/usb2_0_softphy_name.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/usb2_0_softphy/usb2_0_softphy_encryption.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/usb2_0_softphy/static_macro_define.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/uart/uart.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/sync_fifo/usb_fifo.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/sync_fifo/sync_rx_pkt_fifo.v"
add_file -type verilog "../../esp32t/src/rtl/USB/USBUVCUART/sync_fifo/sync_tx_pkt_fifo.v"

add_file -type verilog "../../esp32t/src/gowin_pll_preevt/gowin_pll.v"
add_file -type verilog "src/top.v"
# Placement option 3 meets timing with the save-state engine (0, 1, 2 and
# route option 1 did not); the core runs within ~0.1 ns of its 67.1 MHz clock.
set_option -place_option 3
set_option -route_option 0
add_file -type sdc "src/board/evt1_x2/gg_board.sdc"
add_file -type verilog "core_reload.sv"
set_option -multiboot_address_width 24
set_option -multiboot_mode single
set_option -multiboot_spi_flash_address 0
run all
