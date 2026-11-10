// Atomic stopped-core transfer to the existing SMS_MiSTer Game Genie engine.
// UART 0x12 payload: op(0 clear,1 append), address BE16, compare, value, compare-enable.
// Cart virtual command 0x30 reads {c7, acknowledged sequence, code count, busy}.
module gg_cheat_loader(
 input gclk,xclk,reset,allowed,request,
 input [47:0] payload,
 output [31:0] status,
 output reg [128:0] code_x=0,
 output reg clear_x=0
);
 reg req_toggle=0,ack_x=0;
 reg [47:0] held=0;
 (* ASYNC_REG="TRUE" *) reg [1:0] ack_sync=0,req_sync=0;
 reg ack_seen=0,req_seen=0;
 reg [7:0] sequence_g=0,count_g=0,count_x=0;
 reg [1:0] phase_x=0;
 wire busy=req_toggle!=ack_sync[1];
 wire encoding=(payload==48'd0) ||
   (payload[47:40]==1 && payload[39:24]<16'hc000 && payload[7:1]==0 && count_g<32);
 assign status={8'hc7,sequence_g,count_g,7'd0,busy};
 always @(posedge gclk or posedge reset) begin
  if(reset)begin req_toggle<=0;held<=0;ack_sync<=0;ack_seen<=0;sequence_g<=0;count_g<=0;end
  else begin
   ack_sync<={ack_sync[0],ack_x};
   if(ack_sync[1]!=ack_seen)begin
    ack_seen<=ack_sync[1];sequence_g<=sequence_g+1'b1;count_g<=count_x;
   end
   // Wait for status publication as well as acknowledgement before another request.
   if(request && allowed && !busy && ack_sync[1]==ack_seen && encoding)begin
    held<=payload;req_toggle<=~req_toggle;
   end
  end
 end
 always @(posedge xclk or posedge reset) begin
  if(reset)begin req_sync<=0;req_seen<=0;ack_x<=0;count_x<=0;phase_x<=0;code_x<=0;clear_x<=1;end
  else begin
   req_sync<={req_sync[0],req_toggle};clear_x<=0;code_x[128]<=0;
   case(phase_x)
    0: if(req_sync[1]!=req_seen)begin
     req_seen<=req_sync[1];phase_x<=1;
     if(held[47:40]==0)begin clear_x<=1;count_x<=0;code_x<=0;end
     else begin
      code_x<={1'b1,31'd0,held[0],16'd0,held[39:24],24'd0,held[23:16],24'd0,held[15:8]};
      count_x<=count_x+1'b1;
     end
    end
    1: phase_x<=2; // CODES samples the pulse here.
    2: begin ack_x<=req_seen;phase_x<=0;end
    default: phase_x<=0;
   endcase
  end
 end
endmodule
