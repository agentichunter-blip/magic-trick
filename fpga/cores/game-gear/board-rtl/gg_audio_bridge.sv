// Bundled-data toggle handshake: stereo pair stays stable until acknowledged.
module gg_audio_bridge(input wire xclk,xreset,input wire [31:0] stereo,
 input wire hclk,hreset,output reg [31:0] sample);
 reg [31:0] held;
 reg req,ack;
 (* ASYNC_REG="TRUE" *) reg am,as_,rm,rs;
 always @(posedge xclk or posedge xreset) begin
  if(xreset) begin held<=0;req<=0;am<=0;as_<=0;end
  else begin am<=ack;as_<=am;
   if(req==as_) begin held<=stereo;req<=!req;end
  end
 end
 always @(posedge hclk or posedge hreset) begin
  if(hreset) begin sample<=0;ack<=0;rm<=0;rs<=0;end
  else begin rm<=req;rs<=rm;
   if(rs!=ack) begin sample<=held;ack<=rs;end
  end
 end
endmodule
