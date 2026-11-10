from pathlib import Path
import shutil,re

def apply(dst,root):
    common=Path(__file__).resolve().parent
    shutil.copy2(common/'rtl/gg_cheat_loader.sv',dst/'gg_cheat_loader.sv')
    files={'emu_system_top.v':root/'gamegear-prototype/board/rtl/emu_system_top.v',
           'gg_board_core.sv':root/'gamegear-prototype/board/rtl/gg_board_core.sv',
           'gg_system.v':root/'gamegear-prototype/build/system.v',
           'gg_cheatcodes.sv':root/'gamegear-prototype/rtl/cheatcodes.sv'}
    sources=(dst/'sources.tcl').read_text()
    for name,original in files.items():
        shutil.copy2(original,dst/name)
        assert str(original) in sources,original
        sources=sources.replace(str(original),name)
    sources+='\nadd_file -type verilog "gg_cheat_loader.sv"\n';(dst/'sources.tcl').write_text(sources)
    p=dst/'gg_cheatcodes.sv';s=p.read_text().replace('module CODES(', 'module CODES #(parameter ADDR_WIDTH=16, DATA_WIDTH=8, MAX_CODES=32)(')
    s=re.sub(r'^parameter (?:ADDR_WIDTH|DATA_WIDTH|MAX_CODES).*?;[^\n]*\n','',s,flags=re.M);p.write_text(s)
    p=dst/'emu_system_top.v';s=p.read_text().replace('    input               hclk,','    input [128:0] gg_code_x,\n    input gg_clear_x,\n    input               hclk,',1)
    s=s.replace('gg_board_core core(.clk(xclk),','gg_board_core core(.gg_code(gg_code_x), .gg_clear(gg_clear_x), .clk(xclk),');p.write_text(s)
    p=dst/'gg_board_core.sv';s=p.read_text().replace(' input clk, reset_n, rom_ready, quiesce,',' input [128:0] gg_code,\n input gg_clear,\n input clk, reset_n, rom_ready, quiesce,').replace(".GG_CODE('0)",'.GG_CODE(gg_code)').replace(".GG_RESET('0)",'.GG_RESET(gg_clear)');p.write_text(s)
    p=dst/'gg_system.v';s=p.read_text();assert s.count('.enable(n112)')==1;s=s.replace('.enable(n112)',".enable(n112 & ~mreq_n & ~rd_n & (a < 16'hc000))");p.write_text(s)
    p=dst/'src/rtl/BSP/system_monitor.sv';s=p.read_text().replace('module system_monitor(','module system_monitor(\n    output gg_cheat_request,\n    output [47:0] gg_cheat_payload,',1)
    s=s.replace('endmodule',"    assign gg_cheat_request = rx_data_val && rx_address==7'h12 && rx_length==6;\n    assign gg_cheat_payload = rx_data[47:0];\nendmodule");p.write_text(s)
    p=dst/'src/top.v';s=p.read_text();s=s.replace('    wire core_reload_hold_g;', '''    wire gg_cheat_request,gg_clear_x;
    wire [47:0] gg_cheat_payload;
    wire [128:0] gg_code_x;
    wire [31:0] gg_cheat_status_g;
    gg_cheat_loader u_gg_cheat_loader(.gclk(gClk),.xclk(xClk),.reset(~lock_o),
        .allowed(!virtual_enable_g && !virtual_start_pending_g && !cart_maintenance_request_g && !core_reload_hold_g),
        .request(gg_cheat_request),.payload(gg_cheat_payload),.status(gg_cheat_status_g),
        .code_x(gg_code_x),.clear_x(gg_clear_x));
    wire core_reload_hold_g;''')
    s=s.replace('system_monitor u_system_monitor(','system_monitor u_system_monitor(\n        .gg_cheat_request(gg_cheat_request), .gg_cheat_payload(gg_cheat_payload),',1)
    s=s.replace('emu_system_top u_emu_system_top(','emu_system_top u_emu_system_top(\n        .gg_code_x(gg_code_x), .gg_clear_x(gg_clear_x),',1)
    assert '.gg_code_x(gg_code_x)' in s
    s=s.replace('                    VIRTUAL_CART_STATUS: ;',"                    VIRTUAL_CART_STATUS: ;\n                    7'h30: virtual_response_data <= gg_cheat_status_g;",1)
    p.write_text(s)

if __name__=='__main__':
    common=Path(__file__).resolve().parent
    apply(common/'gg-core',common.parents[1])
