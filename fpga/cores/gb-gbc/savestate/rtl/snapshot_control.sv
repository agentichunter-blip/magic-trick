// Command mailbox. H-domain status is captured only when the request completes.
// Status: [31:16]=0x5353, bit0=busy,1=valid,2=fault,3=supported.
// A failed/ambiguous operation holds pause until board reset; no unsafe resume.
module snapshot_control #(parameter TIMEOUT_BITS=28)(
 input gclk,greset,hclk,hreset,input request_g,load_g,
 output busy_g,output [31:0] status_g,
 input enabled_h,reset_core_h,supported_h,quiesced_h,
 input engine_busy_h,engine_loaded_h,memory_fault_h,
 output reg save_h,load_h, output hold_h
);
 reg req,op,ack,ack1,ack2,req1,req2;
 reg valid_h,failed_h,active_h,started_h,loaded_h;
 reg [2:0] phase;reg [TIMEOUT_BITS-1:0] timer;
 reg [2:0] stat1,stat2;
 assign busy_g=req!=ack2;
 assign status_g={16'h5353,12'd0,stat2[2],stat2[1],stat2[0],busy_g};
 assign hold_h=active_h || failed_h;
 always @(posedge gclk) begin
  if(greset) begin req<=0;op<=0;ack1<=0;ack2<=0;stat1<=0;stat2<=0;end
  else begin
   ack1<=ack;ack2<=ack1;stat1<={supported_h,failed_h,valid_h};stat2<=stat1;
   if(request_g && !busy_g) begin op<=load_g;req<=!req;end
  end
 end
 always @(posedge hclk) begin
  save_h<=0;load_h<=0;
  if(hreset) begin
   req1<=0;req2<=0;ack<=0;valid_h<=0;failed_h<=0;active_h<=0;
   started_h<=0;loaded_h<=0;phase<=0;timer<=0;
  end else begin
   req1<=req;req2<=req1;
   if(!enabled_h || reset_core_h) valid_h<=0;
   if(memory_fault_h || (active_h && (!enabled_h || reset_core_h))) begin
    failed_h<=1;valid_h<=0;active_h<=0;ack<=req2;
   end else if(active_h) begin
    timer<=timer+1'b1;
    if(&timer) begin failed_h<=1;valid_h<=0;active_h<=0;ack<=req2;end
    else case(phase)
     0: if(quiesced_h) begin
       save_h<=!op;load_h<=op;phase<=1;
       if(!op) valid_h<=0;
     end
     1: if(engine_busy_h) begin started_h<=1;phase<=2;end
     2: begin
      if(engine_loaded_h) loaded_h<=1;
      if(!engine_busy_h) begin
       if(op && !(loaded_h||engine_loaded_h)) begin failed_h<=1;valid_h<=0;end
       else valid_h<=1;
       active_h<=0;ack<=req2;
      end
     end
     default: begin failed_h<=1;active_h<=0;ack<=req2;end
    endcase
   end else if(req2!=ack) begin
    if(failed_h || !enabled_h || reset_core_h || !supported_h || (op && !valid_h)) ack<=req2;
    else begin active_h<=1;phase<=0;timer<=0;started_h<=0;loaded_h<=0;end
   end
  end
 end
endmodule
