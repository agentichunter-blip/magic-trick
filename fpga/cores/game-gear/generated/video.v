module video
  (input  clk,
   input  ce_pix,
   input  pal,
   input  border,
   input  ggres,
   input  mask_column,
   input  cut_mask,
   input  smode_M1,
   input  smode_M2,
   input  smode_M3,
   input  smode_M4,
   output [21:0] video_state_out,
   input  [21:0] video_state_in,
   input  video_state_set,
   output [8:0] x,
   output [8:0] y,
   output [7:0] vcounter_cpu,
   output hsync,
   output vsync,
   output hblank,
   output vblank);
  reg [8:0] hcount;
  reg [8:0] vcount;
  reg [8:0] vcounter_cpu_reg;
  reg hsync_reg;
  reg vsync_reg;
  reg hblank_reg;
  reg vblank_reg;
  wire [8:0] vbl_st;
  wire [8:0] vbl_end;
  wire [8:0] hbl_st;
  wire [8:0] hbl_end;
  wire [8:0] n18;
  wire [8:0] n19;
  wire n20;
  wire n21;
  wire [8:0] n22;
  wire n24;
  wire [8:0] n25;
  wire n27;
  wire n28;
  wire n29;
  wire [8:0] n30;
  wire [31:0] n31;
  wire n33;
  wire [8:0] n34;
  wire [8:0] n36;
  wire [8:0] n38;
  wire n39;
  wire [8:0] n40;
  wire [31:0] n41;
  wire n43;
  wire [8:0] n44;
  wire [8:0] n46;
  wire [8:0] n48;
  wire [8:0] n49;
  wire [31:0] n50;
  wire n52;
  wire [8:0] n53;
  wire [8:0] n55;
  wire [8:0] n57;
  wire [8:0] n58;
  wire [8:0] n59;
  wire n60;
  wire [8:0] n61;
  wire [31:0] n62;
  wire n64;
  wire [8:0] n65;
  wire [8:0] n67;
  wire [8:0] n69;
  wire n70;
  wire [8:0] n71;
  wire [31:0] n72;
  wire n74;
  wire [8:0] n75;
  wire [8:0] n77;
  wire [8:0] n79;
  wire [8:0] n80;
  wire [31:0] n81;
  wire n83;
  wire [8:0] n84;
  wire [8:0] n86;
  wire [8:0] n88;
  wire [8:0] n89;
  wire [8:0] n90;
  wire [8:0] n91;
  wire [8:0] n92;
  wire [8:0] n93;
  wire n95;
  wire n96;
  wire [31:0] n97;
  wire n99;
  wire [8:0] n101;
  wire [8:0] n103;
  wire n104;
  wire [31:0] n105;
  wire n107;
  wire [8:0] n109;
  wire [8:0] n111;
  wire [31:0] n112;
  wire n114;
  wire [8:0] n116;
  wire [8:0] n118;
  wire [8:0] n119;
  wire [8:0] n120;
  wire n121;
  wire [31:0] n122;
  wire n124;
  wire [8:0] n126;
  wire [8:0] n128;
  wire n129;
  wire [31:0] n130;
  wire n132;
  wire [8:0] n134;
  wire [8:0] n136;
  wire [31:0] n137;
  wire n139;
  wire [8:0] n141;
  wire [8:0] n143;
  wire [8:0] n144;
  wire [8:0] n145;
  wire [8:0] n146;
  wire [8:0] n147;
  wire [31:0] n148;
  wire n150;
  wire [8:0] n152;
  wire n153;
  wire [31:0] n154;
  wire n156;
  wire [31:0] n157;
  wire n159;
  wire [31:0] n160;
  wire n162;
  wire n164;
  wire n166;
  wire [8:0] n168;
  wire n169;
  wire n170;
  wire [31:0] n171;
  wire n173;
  wire [31:0] n174;
  wire n176;
  wire [31:0] n177;
  wire n179;
  wire n181;
  wire n183;
  wire [8:0] n185;
  wire n186;
  wire [31:0] n187;
  wire n189;
  wire [31:0] n190;
  wire n192;
  wire [31:0] n193;
  wire n195;
  wire n197;
  wire n199;
  wire [8:0] n201;
  wire n202;
  wire [8:0] n203;
  wire n204;
  wire [8:0] n205;
  wire n206;
  wire n207;
  wire [31:0] n208;
  wire n210;
  wire [31:0] n211;
  wire n213;
  wire [31:0] n214;
  wire n216;
  wire n218;
  wire n220;
  wire [8:0] n222;
  wire n223;
  wire n224;
  wire [31:0] n225;
  wire n227;
  wire [31:0] n228;
  wire n230;
  wire [31:0] n231;
  wire n233;
  wire n235;
  wire n237;
  wire [8:0] n239;
  wire n240;
  wire [31:0] n241;
  wire n243;
  wire [31:0] n244;
  wire n246;
  wire [31:0] n247;
  wire n249;
  wire n251;
  wire n253;
  wire [8:0] n255;
  wire n256;
  wire [8:0] n257;
  wire n258;
  wire [8:0] n259;
  wire n260;
  wire [8:0] n261;
  wire n262;
  wire [8:0] n263;
  wire n264;
  wire [8:0] n266;
  wire [31:0] n267;
  wire n269;
  wire [8:0] n271;
  wire [31:0] n272;
  wire n274;
  wire [31:0] n275;
  wire n277;
  wire n279;
  wire n281;
  wire [8:0] n282;
  wire n283;
  wire n284;
  wire n285;
  wire n286;
  wire [8:0] n287;
  wire [8:0] n288;
  wire [8:0] n289;
  wire n290;
  wire n291;
  wire [7:0] n298;
  wire [17:0] n299;
  wire [18:0] n300;
  wire [19:0] n301;
  wire [20:0] n302;
  wire [21:0] n303;
  wire n305;
  wire n306;
  wire [8:0] n307;
  wire n309;
  wire [8:0] n310;
  wire n312;
  wire [8:0] n313;
  wire n315;
  wire n316;
  wire [8:0] n317;
  wire [8:0] n319;
  wire n321;
  wire [8:0] n322;
  wire n325;
  wire [8:0] n326;
  wire n328;
  wire n329;
  wire n330;
  wire n331;
  wire n332;
  wire n333;
  wire n334;
  wire [8:0] n335;
  wire n337;
  wire n338;
  wire [8:0] n339;
  wire [8:0] n341;
  wire n344;
  wire n345;
  wire [8:0] n346;
  wire n348;
  wire n349;
  wire [8:0] n350;
  wire n353;
  wire n354;
  wire [8:0] n355;
  wire n357;
  wire n358;
  wire n359;
  wire n360;
  wire [8:0] n361;
  wire n363;
  wire n364;
  wire [8:0] n365;
  wire n370;
  wire n371;
  wire n372;
  wire n373;
  wire n375;
  wire n377;
  wire n378;
  wire n379;
  wire n381;
  wire n383;
  wire n384;
  wire n385;
  wire n386;
  wire n387;
  reg [8:0] n391;
  reg [8:0] n392;
  reg [8:0] n393;
  reg n394;
  reg n395;
  reg n396;
  reg n397;
  assign video_state_out = n303; //(module output)
  assign x = hcount; //(module output)
  assign y = vcount; //(module output)
  assign vcounter_cpu = n298; //(module output)
  assign hsync = hsync_reg; //(module output)
  assign vsync = vsync_reg; //(module output)
  assign hblank = hblank_reg; //(module output)
  assign vblank = vblank_reg; //(module output)
  /*# vendor/SMS_MiSTer/rtl/video.vhd:35:16 */
  always @*
    hcount = n391; // (isignal)
  initial
    hcount = 9'b000000000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:36:16 */
  always @*
    vcount = n392; // (isignal)
  initial
    vcount = 9'b000000000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:41:16 */
  always @*
    vcounter_cpu_reg = n393; // (isignal)
  initial
    vcounter_cpu_reg = 9'b000000000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:43:16 */
  always @*
    hsync_reg = n394; // (isignal)
  initial
    hsync_reg = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:44:16 */
  always @*
    vsync_reg = n395; // (isignal)
  initial
    vsync_reg = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:45:16 */
  always @*
    hblank_reg = n396; // (isignal)
  initial
    hblank_reg = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:46:16 */
  always @*
    vblank_reg = n397; // (isignal)
  initial
    vblank_reg = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:48:16 */
  assign vbl_st = n307; // (signal)
  /*# vendor/SMS_MiSTer/rtl/video.vhd:48:23 */
  assign vbl_end = n326; // (signal)
  /*# vendor/SMS_MiSTer/rtl/video.vhd:49:16 */
  assign hbl_st = n346; // (signal)
  /*# vendor/SMS_MiSTer/rtl/video.vhd:49:23 */
  assign hbl_end = n355; // (signal)
  /*# vendor/SMS_MiSTer/rtl/video.vhd:56:57 */
  assign n18 = video_state_in[21:13]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:57:57 */
  assign n19 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:58:60 */
  assign n20 = video_state_in[3]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:59:60 */
  assign n21 = video_state_in[2]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:62:51 */
  assign n22 = video_state_in[21:13]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:62:66 */
  assign n24 = n22 == 9'b111100110;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:63:51 */
  assign n25 = video_state_in[21:13]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:63:66 */
  assign n27 = n25 == 9'b111100111;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:62:98 */
  assign n28 = n24 | n27;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:65:67 */
  assign n29 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:66:74 */
  assign n30 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:66:88 */
  assign n31 = {23'b0, n30};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:66:88 */
  assign n33 = n31 == 32'b00000000000000000000000100000010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:69:99 */
  assign n34 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:69:113 */
  assign n36 = n34 + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:66:57 */
  assign n38 = n33 ? 9'b111001010 : n36;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:71:70 */
  assign n39 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:72:74 */
  assign n40 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:72:88 */
  assign n41 = {23'b0, n40};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:72:88 */
  assign n43 = n41 == 32'b00000000000000000000000100001010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:75:99 */
  assign n44 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:75:113 */
  assign n46 = n44 + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:72:57 */
  assign n48 = n43 ? 9'b111100010 : n46;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:78:74 */
  assign n49 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:78:88 */
  assign n50 = {23'b0, n49};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:78:88 */
  assign n52 = n50 == 32'b00000000000000000000000011110010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:81:99 */
  assign n53 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:81:113 */
  assign n55 = n53 + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:78:57 */
  assign n57 = n52 ? 9'b110111010 : n55;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:71:49 */
  assign n58 = n39 ? n48 : n57;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:65:49 */
  assign n59 = n29 ? n38 : n58;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:85:67 */
  assign n60 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:86:74 */
  assign n61 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:86:88 */
  assign n62 = {23'b0, n61};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:86:88 */
  assign n64 = n62 == 32'b00000000000000000000000011101010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:89:99 */
  assign n65 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:89:113 */
  assign n67 = n65 + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:86:57 */
  assign n69 = n64 ? 9'b111100101 : n67;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:91:70 */
  assign n70 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:92:74 */
  assign n71 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:92:88 */
  assign n72 = {23'b0, n71};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:92:88 */
  assign n74 = n72 == 32'b00000000000000000000000100000101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:95:99 */
  assign n75 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:95:113 */
  assign n77 = n75 + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:92:57 */
  assign n79 = n74 ? 9'b000000000 : n77;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:98:74 */
  assign n80 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:98:88 */
  assign n81 = {23'b0, n80};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:98:88 */
  assign n83 = n81 == 32'b00000000000000000000000011011010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:101:99 */
  assign n84 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:101:113 */
  assign n86 = n84 + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:98:57 */
  assign n88 = n83 ? 9'b111010101 : n86;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:91:49 */
  assign n89 = n70 ? n79 : n88;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:85:49 */
  assign n90 = n60 ? n69 : n89;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:64:41 */
  assign n91 = pal ? n59 : n90;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:106:75 */
  assign n92 = video_state_in[12:4]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:62:33 */
  assign n93 = n28 ? n91 : n92;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:110:43 */
  assign n95 = hcount == 9'b111100101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:112:67 */
  assign n96 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:114:67 */
  assign n97 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:114:67 */
  assign n99 = n97 == 32'b00000000000000000000000100000010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:117:92 */
  assign n101 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:114:57 */
  assign n103 = n99 ? 9'b111001010 : n101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:120:70 */
  assign n104 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:122:67 */
  assign n105 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:122:67 */
  assign n107 = n105 == 32'b00000000000000000000000100001010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:125:92 */
  assign n109 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:122:57 */
  assign n111 = n107 ? 9'b111100010 : n109;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:130:67 */
  assign n112 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:130:67 */
  assign n114 = n112 == 32'b00000000000000000000000011110010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:133:92 */
  assign n116 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:130:57 */
  assign n118 = n114 ? 9'b110111010 : n116;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:120:49 */
  assign n119 = n104 ? n111 : n118;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:112:49 */
  assign n120 = n96 ? n103 : n119;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:138:67 */
  assign n121 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:140:67 */
  assign n122 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:140:67 */
  assign n124 = n122 == 32'b00000000000000000000000011101010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:143:92 */
  assign n126 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:140:57 */
  assign n128 = n124 ? 9'b111100101 : n126;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:146:70 */
  assign n129 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:148:67 */
  assign n130 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:148:67 */
  assign n132 = n130 == 32'b00000000000000000000000100000101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:151:92 */
  assign n134 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:148:57 */
  assign n136 = n132 ? 9'b000000000 : n134;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:156:67 */
  assign n137 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:156:67 */
  assign n139 = n137 == 32'b00000000000000000000000011011010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:159:92 */
  assign n141 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:156:57 */
  assign n143 = n139 ? 9'b111010101 : n141;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:146:49 */
  assign n144 = n129 ? n136 : n143;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:138:49 */
  assign n145 = n121 ? n128 : n144;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:111:41 */
  assign n146 = pal ? n120 : n145;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n147 = n284 ? n146 : vcounter_cpu_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:165:42 */
  assign n148 = {23'b0, hcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:165:42 */
  assign n150 = n148 == 32'b00000000000000000000000111100111;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:166:58 */
  assign n152 = vcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:169:65 */
  assign n153 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:170:67 */
  assign n154 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:170:67 */
  assign n156 = n154 == 32'b00000000000000000000000100000010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:172:70 */
  assign n157 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:172:70 */
  assign n159 = n157 == 32'b00000000000000000000000111001101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:174:70 */
  assign n160 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:174:70 */
  assign n162 = n160 == 32'b00000000000000000000000111010000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:174:57 */
  assign n164 = n162 ? 1'b0 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:172:57 */
  assign n166 = n159 ? 1'b1 : n164;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:170:57 */
  assign n168 = n156 ? 9'b111001010 : n152;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:170:57 */
  assign n169 = n156 ? vsync_reg : n166;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:177:68 */
  assign n170 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:178:67 */
  assign n171 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:178:67 */
  assign n173 = n171 == 32'b00000000000000000000000100001010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:180:70 */
  assign n174 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:180:70 */
  assign n176 = n174 == 32'b00000000000000000000000111100010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:182:70 */
  assign n177 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:182:70 */
  assign n179 = n177 == 32'b00000000000000000000000111100101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:182:57 */
  assign n181 = n179 ? 1'b0 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:180:57 */
  assign n183 = n176 ? 1'b1 : n181;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:178:57 */
  assign n185 = n173 ? 9'b111100010 : n152;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:178:57 */
  assign n186 = n173 ? vsync_reg : n183;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:187:67 */
  assign n187 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:187:67 */
  assign n189 = n187 == 32'b00000000000000000000000011110010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:189:70 */
  assign n190 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:189:70 */
  assign n192 = n190 == 32'b00000000000000000000000110111010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:191:70 */
  assign n193 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:191:70 */
  assign n195 = n193 == 32'b00000000000000000000000110111101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:191:57 */
  assign n197 = n195 ? 1'b0 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:189:57 */
  assign n199 = n192 ? 1'b1 : n197;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:187:57 */
  assign n201 = n189 ? 9'b110111010 : n152;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:187:57 */
  assign n202 = n189 ? vsync_reg : n199;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:177:49 */
  assign n203 = n170 ? n185 : n201;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:177:49 */
  assign n204 = n170 ? n186 : n202;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:169:49 */
  assign n205 = n153 ? n168 : n203;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:169:49 */
  assign n206 = n153 ? n169 : n204;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:197:65 */
  assign n207 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:198:67 */
  assign n208 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:198:67 */
  assign n210 = n208 == 32'b00000000000000000000000011101010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:200:70 */
  assign n211 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:200:70 */
  assign n213 = n211 == 32'b00000000000000000000000111100111;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:202:70 */
  assign n214 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:202:70 */
  assign n216 = n214 == 32'b00000000000000000000000111101010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:202:57 */
  assign n218 = n216 ? 1'b0 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:200:57 */
  assign n220 = n213 ? 1'b1 : n218;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:198:57 */
  assign n222 = n210 ? 9'b111100101 : n152;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:198:57 */
  assign n223 = n210 ? vsync_reg : n220;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:206:60 */
  assign n224 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:207:67 */
  assign n225 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:207:67 */
  assign n227 = n225 == 32'b00000000000000000000000100000101;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:209:70 */
  assign n228 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:209:70 */
  assign n230 = n228 == 32'b00000000000000000000000100000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:211:70 */
  assign n231 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:211:70 */
  assign n233 = n231 == 32'b00000000000000000000000100000100;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:211:57 */
  assign n235 = n233 ? 1'b0 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:209:57 */
  assign n237 = n230 ? 1'b1 : n235;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:207:57 */
  assign n239 = n227 ? 9'b000000000 : n152;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:207:57 */
  assign n240 = n227 ? vsync_reg : n237;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:216:67 */
  assign n241 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:216:67 */
  assign n243 = n241 == 32'b00000000000000000000000011011010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:218:70 */
  assign n244 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:218:70 */
  assign n246 = n244 == 32'b00000000000000000000000111010111;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:220:70 */
  assign n247 = {23'b0, vcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:220:70 */
  assign n249 = n247 == 32'b00000000000000000000000111011010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:220:57 */
  assign n251 = n249 ? 1'b0 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:218:57 */
  assign n253 = n246 ? 1'b1 : n251;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:216:57 */
  assign n255 = n243 ? 9'b111010101 : n152;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:216:57 */
  assign n256 = n243 ? vsync_reg : n253;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:206:41 */
  assign n257 = n224 ? n239 : n255;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:206:41 */
  assign n258 = n224 ? n240 : n256;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:197:49 */
  assign n259 = n207 ? n222 : n257;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:197:49 */
  assign n260 = n207 ? n223 : n258;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:167:41 */
  assign n261 = pal ? n205 : n259;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:167:41 */
  assign n262 = pal ? n206 : n260;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n263 = n283 ? n261 : vcount;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n264 = n286 ? n262 : vsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:227:50 */
  assign n266 = hcount + 9'b000000001;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:229:43 */
  assign n267 = {23'b0, hcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:229:43 */
  assign n269 = n267 == 32'b00000000000000000000000100100111;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:229:33 */
  assign n271 = n269 ? 9'b111010010 : n266;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:232:43 */
  assign n272 = {23'b0, hcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:232:43 */
  assign n274 = n272 == 32'b00000000000000000000000100011000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:234:46 */
  assign n275 = {23'b0, hcount};  // uext
  /*# vendor/SMS_MiSTer/rtl/video.vhd:234:46 */
  assign n277 = n275 == 32'b00000000000000000000000111011010;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:234:33 */
  assign n279 = n277 ? 1'b0 : hsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:232:33 */
  assign n281 = n274 ? 1'b1 : n279;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n282 = ce_pix ? n271 : hcount;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n283 = n150 & ce_pix;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n284 = n95 & ce_pix;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n285 = ce_pix ? n281 : hsync_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:108:25 */
  assign n286 = n150 & ce_pix;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:55:25 */
  assign n287 = video_state_set ? n18 : n282;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:55:25 */
  assign n288 = video_state_set ? n19 : n263;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:55:25 */
  assign n289 = video_state_set ? n93 : n147;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:55:25 */
  assign n290 = video_state_set ? n20 : n285;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:55:25 */
  assign n291 = video_state_set ? n21 : n264;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:243:41 */
  assign n298 = vcounter_cpu_reg[7:0]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:249:35 */
  assign n299 = {hcount, vcount};
  /*# vendor/SMS_MiSTer/rtl/video.vhd:249:44 */
  assign n300 = {n299, hsync_reg};
  /*# vendor/SMS_MiSTer/rtl/video.vhd:249:56 */
  assign n301 = {n300, vsync_reg};
  /*# vendor/SMS_MiSTer/rtl/video.vhd:249:68 */
  assign n302 = {n301, hblank_reg};
  /*# vendor/SMS_MiSTer/rtl/video.vhd:249:81 */
  assign n303 = {n302, vblank_reg};
  /*# vendor/SMS_MiSTer/rtl/video.vhd:251:84 */
  assign n305 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:251:101 */
  assign n306 = ggres & n305;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:251:65 */
  assign n307 = n306 ? 9'b010111000 : n310;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:252:80 */
  assign n309 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:252:25 */
  assign n310 = n309 ? 9'b011100000 : n313;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:253:80 */
  assign n312 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:253:25 */
  assign n313 = n312 ? 9'b011110000 : n317;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:254:85 */
  assign n315 = ~pal;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:254:77 */
  assign n316 = n315 & border;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:254:25 */
  assign n317 = n316 ? 9'b011011000 : n319;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:255:25 */
  assign n319 = border ? 9'b011110000 : n322;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:256:70 */
  assign n321 = ~ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:256:25 */
  assign n322 = n321 ? 9'b011000000 : 9'b010101000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:259:68 */
  assign n325 = ggres & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:259:49 */
  assign n326 = n325 ? 9'b000101000 : n335;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:80 */
  assign n328 = smode_M2 & smode_M1;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:119 */
  assign n329 = smode_M2 & smode_M3;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:100 */
  assign n330 = n328 | n329;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:150 */
  assign n331 = ~border;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:166 */
  assign n332 = ~ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:156 */
  assign n333 = n332 & n331;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:139 */
  assign n334 = n330 | n333;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:260:25 */
  assign n335 = n334 ? 9'b000000000 : n339;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:261:85 */
  assign n337 = ~pal;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:261:77 */
  assign n338 = n337 & border;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:261:25 */
  assign n339 = n338 ? 9'b111101000 : n341;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:262:25 */
  assign n341 = border ? 9'b111001010 : 9'b000011000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:265:77 */
  assign n344 = ~ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:265:67 */
  assign n345 = n344 & border;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:265:49 */
  assign n346 = n345 ? 9'b100001110 : n350;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:266:72 */
  assign n348 = border ^ ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:266:83 */
  assign n349 = ~n348;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:266:25 */
  assign n350 = n349 ? 9'b100000000 : 9'b011010000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:269:77 */
  assign n353 = ~ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:269:67 */
  assign n354 = n353 & border;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:269:49 */
  assign n355 = n354 ? 9'b111110100 : n361;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:270:72 */
  assign n357 = border ^ ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:270:83 */
  assign n358 = ~n357;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:270:89 */
  assign n359 = mask_column & n358;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:270:111 */
  assign n360 = cut_mask & n359;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:270:25 */
  assign n361 = n360 ? 9'b000001000 : n365;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:271:72 */
  assign n363 = border ^ ggres;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:271:83 */
  assign n364 = ~n363;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:271:25 */
  assign n365 = n364 ? 9'b000000000 : 9'b000110000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:278:61 */
  assign n370 = video_state_in[1]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:279:61 */
  assign n371 = video_state_in[0]; // extract
  /*# vendor/SMS_MiSTer/rtl/video.vhd:281:43 */
  assign n372 = hcount == hbl_end;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:283:46 */
  assign n373 = hcount == hbl_st;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:283:33 */
  assign n375 = n373 ? 1'b1 : hblank_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:281:33 */
  assign n377 = n372 ? 1'b0 : n375;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:287:43 */
  assign n378 = vcount == vbl_end;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:289:46 */
  assign n379 = vcount == vbl_st;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:289:33 */
  assign n381 = n379 ? 1'b1 : vblank_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:287:33 */
  assign n383 = n378 ? 1'b0 : n381;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:280:25 */
  assign n384 = ce_pix ? n377 : hblank_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:280:25 */
  assign n385 = ce_pix ? n383 : vblank_reg;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:277:25 */
  assign n386 = video_state_set ? n370 : n384;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:277:25 */
  assign n387 = video_state_set ? n371 : n385;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:54:17 */
  always @(posedge clk)
    n391 <= n287;
  initial
    n391 = 9'b000000000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:54:17 */
  always @(posedge clk)
    n392 <= n288;
  initial
    n392 = 9'b000000000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:54:17 */
  always @(posedge clk)
    n393 <= n289;
  initial
    n393 = 9'b000000000;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:54:17 */
  always @(posedge clk)
    n394 <= n290;
  initial
    n394 = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:54:17 */
  always @(posedge clk)
    n395 <= n291;
  initial
    n395 = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:276:17 */
  always @(posedge clk)
    n396 <= n386;
  initial
    n396 = 1'b0;
  /*# vendor/SMS_MiSTer/rtl/video.vhd:276:17 */
  always @(posedge clk)
    n397 <= n387;
  initial
    n397 = 1'b0;
endmodule

