// Gray-pointer asynchronous FIFO. Only synchronized pointers cross domains.
// Data memory has synchronous read latency; out_valid accompanies registered data.
module gg_async_fifo #(parameter WIDTH=20, parameter ABITS=8)(
 input wire wclk,wrst,wvalid,input wire [WIDTH-1:0] wdata,
 output wire full, output reg overflow,
 input wire rclk,rrst, output reg rvalid,output reg [WIDTH-1:0] rdata);
 reg [WIDTH-1:0] mem[0:(1<<ABITS)-1];
 reg [ABITS:0] wbin,wgray,rbin,rgray;
 (* ASYNC_REG="TRUE" *) reg [ABITS:0] rm,rs,wm,ws;
 wire [ABITS:0] wn=wbin+1'b1;
 wire [ABITS:0] rn=rbin+1'b1;
 // Keep RAM read/write registers free of asynchronous reset so the vendor
 // tools can infer true dual-clock block RAM. rvalid guards unread data.
 always @(posedge wclk) if(!wrst && wvalid && !full) mem[wbin[ABITS-1:0]]<=wdata;
 always @(posedge rclk) if(!rrst && rgray!=ws) rdata<=mem[rbin[ABITS-1:0]];
 assign full=wgray=={~rs[ABITS:ABITS-1],rs[ABITS-2:0]};
 always @(posedge wclk or posedge wrst) begin
  if(wrst) begin wbin<=0;wgray<=0;rm<=0;rs<=0;overflow<=0;end
  else begin rm<=rgray;rs<=rm;
   if(wvalid) begin
    if(full) overflow<=1;
    else begin wbin<=wn;wgray<=(wn>>1)^wn;end
   end
  end
 end
 always @(posedge rclk or posedge rrst) begin
  if(rrst) begin rbin<=0;rgray<=0;wm<=0;ws<=0;rvalid<=0;end
  else begin wm<=wgray;ws<=wm;rvalid<=0;
   if(rgray!=ws) begin rvalid<=1;rbin<=rn;rgray<=(rn>>1)^rn;end
  end
 end
endmodule
