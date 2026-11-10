// mm_burst_read_to_stream.v
//
// Generates an output stream aligned with the input timings
// by performing memory mapped burst reads

module mm_burst_read_to_stream #(
    parameter base_pointer = 23'h0
)
(
    input               hClk,
    input               hVsync,
    input               hHsync,
    input               hValid,
    input               xClk,    
        
    input               xRamReady,
    input               xStreamValid,
    input       [15:0]  xStreamData,
    input               xWrBurstDone,
    output  reg         xGbReqRead,
    
    output wire [15:0]  hWrBurstQ,
    output  reg [22:0]  xGbAddress
    
);

    localparam LINE_DEPTH = 160;

    reg [14:0] xLineRam_wa;
    reg [7:0] hLineRam_ra;

    reg hVsync_r1;
    always@(posedge hClk)
        hVsync_r1 <= hVsync;

    reg hHsync_r1;
    always@(posedge hClk)
        hHsync_r1 <= hHsync;
        
    wire hStartOfFrame = ~hVsync_r1 & hVsync;
    always@(posedge hClk)
        hHsync_r1 <= hHsync;
    wire hStartOfLine = ~hHsync_r1 & hHsync;

    always@(posedge hClk)
        if(hStartOfFrame)
            hLineRam_ra <= 'd0;
        else
            if(hValid)
            begin
                if (hLineRam_ra < LINE_DEPTH) 
                    hLineRam_ra <= hLineRam_ra + 1'd1;
            end
            else
                if(hStartOfLine)
                    hLineRam_ra <= 'd0;
        
    snapshot_line_ram line_storage(.wclk(xClk),.rclk(hClk),
        .we(xStreamValid),.wa(xLineRam_wa),.ra(hLineRam_ra),
        .data(xStreamData),.q(hWrBurstQ));

    always@(posedge xClk)
    begin
        if(xStreamValid)
            xLineRam_wa <= xLineRam_wa + 1'd1;
         
        if(xWrBurstDone)
            xLineRam_wa <= 'd0;
    end

    // TODO - cleanup CDC
            
    reg [3:0] xHsync_sr;
    always@(posedge xClk)
        xHsync_sr <= {xHsync_sr[2:0], hHsync};
    wire xEndOfLine = xHsync_sr[3:2] == 2'b10;

    reg [3:0] xVsync_sr;
    always@(posedge xClk)
        xVsync_sr <= {xVsync_sr[2:0], hVsync};
    wire xStartOfFrame = xVsync_sr[3:2] == 2'b01;

    // Line k+1 is fetched at the end of line k. Line 0 used to be fetched only
    // at the start of the frame, racing the first pixels (no lead time with the
    // Game Gear source, and PSRAM is shared with QSPI/framebuffer/ROM traffic),
    // so the start of row 0 showed stale line-RAM data (a streak at the top
    // left). Prefetch line 0 at the end of the last line, during vblank.
    localparam LAST_LINE = 143;
    reg [7:0] xLineCount = 8'd0;
    reg xPrefetched = 1'b0;
    always@(posedge xClk)
    begin
        xGbReqRead <= 1'd0;
        if(xStartOfFrame)
        begin
            xLineCount <= 8'd0;
            xPrefetched <= 1'b0;
            xGbAddress <= base_pointer;
            if(!xPrefetched)
                xGbReqRead <= xRamReady;
        end
        else if(xEndOfLine)
        begin
            xGbReqRead <= xRamReady;
            if(xLineCount == LAST_LINE)
            begin
                xGbAddress <= base_pointer;
                xPrefetched <= 1'b1;
            end
            else
                xGbAddress <= xGbAddress + 2*LINE_DEPTH;
            if(xLineCount != 8'hff)
                xLineCount <= xLineCount + 1'd1;
        end
    end

endmodule
