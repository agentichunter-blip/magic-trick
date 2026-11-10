"""Create isolated multiboot builds; never edits the qualified source cores."""
from pathlib import Path
import shutil,re,json,hashlib
from usb_serial_only import apply as serial_only
from integrate_gg_cheats import apply as gg_cheats
root=Path(__file__).resolve().parents[2]
out=Path(__file__).resolve().parent
for kind,source,script,minor in [('gb',root/'save-state-prototype/board/platform','build_menu_map1.tcl',1),('gg',root/'gamegear-prototype/board/platform','build_evt1_x2.tcl',2)]:
 dst=out/(kind+'-core');dst.mkdir(exist_ok=True)
 shutil.copytree(source/'src',dst/'src',dirs_exist_ok=True)
 shutil.copy2(out/'rtl/core_reload.sv',dst/'core_reload.sv')
 sources=(source/'build.tcl').read_text()
 # Platform-local files are copied; preserve original external RTL references.
 sources=re.sub(r'"(\.\./[^"]+)"',lambda m:'"'+str((source/m[1]).resolve())+'"',sources)
 (dst/'sources.tcl').write_text(sources)
 build=(source/script).read_text().replace('source build.tcl','source sources.tcl')
 build=build.replace('-multi_boot 0','-multi_boot 1').replace('-bg_programming jtag_sspi_qsspi','-bg_programming userlogic')
 if kind=='gb':
  # Serial-only GB/GBC closes timing with map2/place1. The earlier map1/place3
  # run exhausted its routing timeout; native snapshot RTL stays unchanged.
  build=re.sub(r'set_option -map_option .*','set_option -map_option 2',build)
  build=re.sub(r'set_option -place_option .*','set_option -place_option 1',build)
 build=re.sub(r'set_option -output_base_name .*',f'set_option -output_base_name chromatic_{kind}_multiboot_UNQUALIFIED',build)
 build=build.replace('run all','add_file -type verilog "core_reload.sv"\nset_option -multiboot_address_width 24\nset_option -multiboot_mode single\nset_option -multiboot_spi_flash_address 0\nrun all')
 (dst/'build.tcl').write_text(build)
 cst=dst/'src/board/evt1_x2/evt1_x2.cst'
 cst.write_text(cst.read_text()+'\nIO_LOC "GW_BACKGROUND_RECONFIG_N" H5;\nIO_PORT "GW_BACKGROUND_RECONFIG_N" IO_TYPE=LVCMOS33 DRIVE=4 OPEN_DRAIN=ON PULL_MODE=UP;\n')
 parser=dst/'src/rtl/BSP/uart_packet_wrapper_rx.sv';s=parser.read_text().replace('output  reg         rx_data_val','output  reg [7:0]   rx_length,\n    output  reg         rx_data_val')
 s=s.replace('rx_state <= RX_IDLE;\n        end else','rx_state <= RX_IDLE;\n            rx_data_val <= 0; rx_length <= 0;\n        end else',1).replace('rx_count <= uart_rx_data;','rx_count <= uart_rx_data;\n                         rx_length <= uart_rx_data;')
 parser.write_text(s)
 monitor=dst/'src/rtl/BSP/system_monitor.sv';s=monitor.read_text().replace('module system_monitor(','module system_monitor(\n    input reload_allowed,\n    output reload_n, reload_hold,')
 s=s.replace('wire    [6:0]   rx_address;','wire    [7:0]   rx_length;\n    wire    [6:0]   rx_address;')
 s=s.replace('.rx_address(rx_address),','.rx_address(rx_address),\n        .rx_length(rx_length),')
 s=re.sub(r'reg \[13:0\] version = .*?;',f"wire [13:0] version = {{1'd0,1'd1,6'd{minor},6'd50}};",s)
 s=s.replace('endmodule', '''    wire [23:0] core_boot_address;
    core_reload u_core_reload(.clk(clk), .reset(reset), .allowed(reload_allowed),
        .rx_valid(rx_data_val), .rx_address(rx_address), .rx_length(rx_length), .rx_data(rx_data),
        .reload_n(reload_n), .hold_core(reload_hold), .boot_address(core_boot_address));
    SAMB #(.MODE(2'b00)) u_core_boot(.LOAD(1'b1), .SPIAD(core_boot_address), .ADWSEL(1'b0));
endmodule''')
 monitor.write_text(s)
 top=dst/'src/top.v';s=top.read_text().replace('    output              ADC_SEL,','    output GW_BACKGROUND_RECONFIG_N,\n    output              ADC_SEL,',1)
 assert 'output GW_BACKGROUND_RECONFIG_N' in s
 # Keep the core held and cartridge pins inactive before the configuration pulse.
 at='    wire cart_maintenance_reset_h;'
 assert at in s
 s=s.replace(at,at+'''\n    wire core_reload_hold_g;
    reg core_reload_hold_meta = 0, core_reload_hold_h = 0;
    always @(posedge hClk or negedge lock_o)
        if (!lock_o) begin core_reload_hold_meta <= 0; core_reload_hold_h <= 0; end
        else begin core_reload_hold_meta <= core_reload_hold_g; core_reload_hold_h <= core_reload_hold_meta; end
''')
 allowed='!virtual_enable_g && !virtual_start_pending_g && !cart_maintenance_request_g'
 if kind=='gb':allowed+=' && !state_busy_g && !state_status_g[2]'
 s=s.replace('system_monitor u_system_monitor(','system_monitor u_system_monitor(\n        .reload_allowed('+allowed+'),\n        .reload_n(GW_BACKGROUND_RECONFIG_N), .reload_hold(core_reload_hold_g),')
 # Only the emulator input, not the output of the ownership CDC.
 s=s.replace('.maintenance_reset_h(cart_maintenance_reset_h),','.maintenance_reset_h(cart_maintenance_reset_h | core_reload_hold_h),')
 # Physical cartridge isolation (GG already ties physical outputs inactive).
 for pin in ['core_address','core_clock','core_cs_n','core_read_n','core_write_n','core_data_oe']:
  s=s.replace('.'+pin+'(virtual_enable_h ?', '.'+pin+'((virtual_enable_h | core_reload_hold_h) ?')
 top.write_text(s)
 serial_only(dst)
 if kind=='gb':
  # Remove the old mapping-specific HDMA packing constraint. Keep PSRAM reset
  # placement constraints and all timing constraints intact.
  cst.write_text('\n'.join(line for line in cst.read_text().splitlines()
                         if 'native_hdma_target_enable' not in line)+'\n')
 if kind=='gg':gg_cheats(dst,root)
 (dst/'source-manifest.json').write_text(json.dumps({'kind':kind,'source':str(source),'qualified':False,'native_state_rtl_modified':False},indent=2)+'\n')
 print(dst)
