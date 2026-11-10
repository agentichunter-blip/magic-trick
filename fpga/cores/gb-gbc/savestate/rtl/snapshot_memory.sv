// One held request crosses hclk -> xclk. Responses remain stable until next request.
// Reserved slot: 0x440000..0x47ffff; engine addresses are DWORDs.
// Cache requests are queued once and retain ownership through done.
module snapshot_memory #(parameter TIMEOUT_BITS=24)(
 input hclk,hreset,xclk,xreset,
 input hrequest, hread, input [25:0] haddress, input [63:0] hwdata,
 input [7:0] hbe, output reg hdone, output [63:0] hrdata, output fault,
 input crequest,cread,input [22:0] caddress,input [15:0] cwdata,input [10:0] clength,
 output cnext,cdone,crvalid,output [15:0] crdata,
 output reg mrequest,output reg mread,output reg [22:0] maddress,
 output [15:0] mwdata,output reg [10:0] mlength,
 input mnext,mdone,mrvalid,input [15:0] mrdata
);
 reg req,ack,ack1,ack2,seen,req1,req2;
 reg [25:0] held_addr; reg [63:0] held_data,response;
 reg held_read; reg [7:0] held_be;
 reg hfault,xfault,xf1,xf2;
 assign fault=hfault|xf2; assign hrdata=response;
 always @(posedge hclk) begin
  hdone<=0;
  if(hreset) begin req<=0;ack1<=0;ack2<=0;seen<=0;hfault<=0;xf1<=0;xf2<=0;
   held_addr<=0;held_data<=0;held_read<=0;held_be<=0;
  end else begin
   ack1<=ack;ack2<=ack1;xf1<=xfault;xf2<=xf1;
   if(ack2!=seen) begin seen<=ack2;hdone<=1;end
   if(hrequest) begin
    if(req!=ack2 || fault) hfault<=1;
    else begin held_addr<=haddress;held_data<=hwdata;held_read<=hread;held_be<=hbe;req<=!req;end
   end
  end
 end
 reg cpending;reg c_read;reg [22:0] c_addr;reg [10:0] c_len;
 reg [1:0] owner; // 0 idle, 1 cache, 2 snapshot; never release on timeout.
 reg [2:0] word_count;reg [1:0] write_index;
 reg [TIMEOUT_BITS-1:0] timer;
 assign cnext=owner==1 && mnext;assign cdone=owner==1 && mdone;
 assign crvalid=owner==1 && mrvalid;assign crdata=mrdata;
 assign mwdata=owner==1 ? cwdata : held_data[write_index*16 +:16];
 always @(posedge xclk) begin
  mrequest<=0;
  if(xreset) begin
   req1<=0;req2<=0;ack<=0;response<=0;owner<=0;cpending<=0;
   xfault<=0;timer<=0;word_count<=0;write_index<=0;mread<=0;maddress<=0;mlength<=0;
   c_read<=0;c_addr<=0;c_len<=0;
  end else begin
   req1<=req;req2<=req1;
   if(crequest) begin
    if(cpending) xfault<=1;
    else begin cpending<=1;c_read<=cread;c_addr<=caddress;c_len<=clength;end
   end
   if(owner==0 && !xfault) begin
    timer<=0;word_count<=0;write_index<=0;
    if(cpending) begin
     owner<=1;cpending<=0;mrequest<=1;mread<=c_read;maddress<=c_addr;mlength<=c_len;
    end else if(req2!=ack) begin
     if(held_addr<26'h110000 || held_addr>26'h11fffe || held_addr[0] || (!held_read && held_be!=8'hff)) xfault<=1;
     else begin owner<=2;mrequest<=1;mread<=held_read;maddress<={held_addr[20:0],2'b00};mlength<=8;end
    end
   end else if(owner!=0 && !xfault) begin
    timer<=timer+1'b1;
    if(&timer) xfault<=1;
    if(owner==2) begin
     if(mnext) write_index<=write_index+1'b1;
     if(mrvalid) begin
      if(word_count>=4) xfault<=1;
      else response[word_count*16 +:16]<=mrdata;
      word_count<=word_count+1'b1;
     end
    end
    if(mdone) begin
     if(owner==2 && held_read && (word_count+{2'b00,mrvalid})!=4) xfault<=1;
     else begin owner<=0;if(owner==2) ack<=req2;end
    end
   end
  end
 end
endmodule
