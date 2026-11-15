source sources.tcl
set_device GW5A-EV25UG256CC1/I0 -device_version A 
set_option -synthesis_tool gowinsynthesis
set_option -top_module top
set_option -verilog_std sysv2017
set_option -vhdl_std vhd2008
# No read/write collision bypass logic: no Lynx-core RAM reads an address
# while the other port writes it, and every block of logic counts (routing).
set_option -rw_check_on_ram 0
set_option -use_sspi_as_gpio 1
set_option -power_on_reset_monitor 1
set_option -use_i2c_as_gpio 1
set_option -use_cpu_as_gpio 1
set_option -multi_boot 1
set_option -bit_format bin
set_option -bg_programming userlogic
set_option -output_base_name magic_trick_lynx
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


add_file -type verilog "../../esp32t/src/gowin_pll_preevt/gowin_pll.v"
add_file -type verilog "src/top.v"
# Placement algorithm 1 (slower, better layout): routing is congested.
set_option -place_option 1
set_option -route_option 0
add_file -type sdc "src/board/evt1_x2/lynx_board.sdc"
add_file -type verilog "core_reload.sv"
set_option -multiboot_address_width 24
set_option -multiboot_mode single
# Lynx boots GB/GBC next (slot 0) until three-core switching is designed.
set_option -multiboot_spi_flash_address 0
run all
