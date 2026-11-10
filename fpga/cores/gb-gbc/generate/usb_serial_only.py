"""Remove capture consumers and advertise only the working CDC serial function."""
import re

def apply(platform):
    base=platform/'src/rtl/USB/USBUVCUART'
    usb=base/'usbuvcuart_top.v';data=usb.read_text()
    assignments={
        'endpt0_dat':"cuart_txval ? cuart_txdat : 8'd0",
        'endpt0_txlen':"cuart_txval ? cuart_txdat_len : 12'd0",
        'endpt0_send':'cuart_txval',
        'usb_txdat':'(endpt_sel == EP_CTRL) ? endpt0_dat : uart_txdat',
        'usb_txdat_len':"(endpt_sel == EP_CTRL) ? endpt0_txlen : (endpt_sel == EP_UART) ? uart_txdat_len : 12'd0",
        'usb_txcork':"(endpt_sel == EP_CTRL) ? 1'b0 : (endpt_sel == EP_UART) ? uart_txcork : 1'b1",
        'interface_alter_i':"(interface_sel == `UART_DATA_IFACE) ? uart_iface_alter : 8'd0",
        'debugs':"8'd0"}
    for signal,value in assignments.items():
        data,n=re.subn(r'assign\s+'+signal+r'\s*=.*?;',f'assign {signal} = {value};',data,flags=re.S)
        assert n==1,(signal,n)
    data,n=re.subn(r'\.txiso_pid_i\s*\(iso_pid_data\s*\)',".txiso_pid_i(4'b0011)",data);assert n==1
    data=data.replace("`define UART_BASE_IFACE  8'd2","`define UART_BASE_IFACE  8'd0")
    usb.write_text(data)
    defs=base/'usb_video/uart_defs.v';defs.write_text(defs.read_text().replace("UART_BASE_IFACE  8'd2","UART_BASE_IFACE  8'd0"))
    desc=base/'usb_video/usb_descriptor_video.v';data=desc.read_text()
    data=data.replace('DESC_CDCIF_ADDR       = DESC_UAC_ADDR + DESC_UAC_LEN','DESC_CDCIF_ADDR       = DESC_FSCFG_ADDR + 9')
    data=data.replace('DESC_FSCFG_LEN        = DESC_UAC_LEN + 180 + DESC_CDCIF_LEN + DESC_MSOS_LEN','DESC_FSCFG_LEN        = 9 + DESC_CDCIF_LEN')
    data=data.replace("descrom[DESC_FSCFG_ADDR + 4] <= 8'h06;","descrom[DESC_FSCFG_ADDR + 4] <= 8'h02;")
    start=data.index('        //---------------- Interface Association Descriptor',data.index('//======USB Configuration Descriptor'))
    end=data.index('        descrom[DESC_CDCIF_ADDR',start)
    # Drop every video/audio descriptor assignment; CDC descriptors begin here.
    data=data[:start]+'        // CDC-only configuration; USB capture intentionally omitted.\n        if (DESC_CDCIF_LEN != 0) begin\n'+data[end:]
    desc.write_text(data)

    cst=platform/'src/board/evt1_x2/evt1_x2.cst'
    cst.write_text('\n'.join(line for line in cst.read_text().splitlines() if not line.startswith(('GROUP usb_uvc_fifo_read_timing', 'GRP_LOC usb_uvc_fifo_read_timing'))) + '\n')
