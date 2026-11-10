//
// video.v
//
// Gameboy for the MIST board https://github.com/mist-devel
//
// Copyright (c) 2015 Till Harbaum <till@harbaum.org>
//
// This source file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This source file is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

module video (
	input  reset,
	input  clk,
	input  ce, // 4 Mhz cpu clock
	input  ce_cpu, // 4 or 8Mhz
	input  dma_wait,
	input  isGBC,
	input  isGBC_mode,
	input  megaduck,

	input  boot_rom_en,

   input customPaletteEna,
   input [63:0] paletteBGIn,
   input [63:0] paletteOBJ0In,
   input [63:0] paletteOBJ1In,
   output [63:0] gpd_out,

	// cpu register adn oam interface
	input  cpu_sel_oam,
	input  cpu_sel_reg,
	input [7:0] cpu_addr,
	input  cpu_wr,
	input [7:0] cpu_di,
	output [7:0] cpu_do,

	// output to lcd
	output lcd_on,
	output lcd_clkena,
	output [14:0] lcd_data,
	output  [1:0] lcd_data_gb,
	output lcd_vsync,
	output snapshot_safe_blank,

	output irq,
	output vblank_irq,

	// vram connection
	output [1:0] mode,
	output oam_cpu_allow,
	output vram_cpu_allow,
	output vram_rd,
	output [12:0] vram_addr,
	input [7:0] vram_data,

	// vram connection bank1 (GBC)
	input [7:0] vram1_data,

	// dma connection
	output dma_rd,
	output [15:0] dma_addr,
	input [7:0] dma_data,

	// savestates
	input [7:0] Savestate_OAMRAMAddr,     
	input       Savestate_OAMRAMRWrEn,    
	input [7:0] Savestate_OAMRAMWriteData,
	output[7:0] Savestate_OAMRAMReadData,
	         
	input  [63:0] SaveStateBus_Din, 
	input  [9:0]  SaveStateBus_Adr, 
	input         SaveStateBus_wren,
	input         SaveStateBus_rst, 
	output [63:0] SaveStateBus_Dout
);

// savestates
localparam SAVESTATE_MODULES    = 19;
wire [63:0] SaveStateBus_wired_or[0:SAVESTATE_MODULES-1];

wire [60:0] SS_Video1;
wire [60:0] SS_Video1_BACK;
wire [63:0] SS_Video2;
wire [63:0] SS_Video2_BACK;
wire [63:0] SS_Video3;
wire [63:0] SS_Video3_BACK;

assign SS_Video1 = 64'h0000000000000000;
assign SaveStateBus_wired_or[0] = 64'd0;  
assign SS_Video2 = 64'h00000000FFFFFC00;
assign SaveStateBus_wired_or[1] = 64'd0;  
assign SS_Video3 = 64'h0000000000000000;
assign SaveStateBus_wired_or[18] = 64'd0;  

assign SaveStateBus_wired_or[2] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[3] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[4] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[5] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[6] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[7] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[8] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[9] = 64'd0; // retired palette slot

assign SaveStateBus_wired_or[10] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[11] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[12] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[13] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[14] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[15] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[16] = 64'd0; // retired palette slot
assign SaveStateBus_wired_or[17] = 64'd0; // retired palette slot

assign SaveStateBus_Dout  = SaveStateBus_wired_or[ 0] | SaveStateBus_wired_or[ 1] | SaveStateBus_wired_or[ 2] | SaveStateBus_wired_or[ 3] | 
							SaveStateBus_wired_or[ 4] | SaveStateBus_wired_or[ 5] | SaveStateBus_wired_or[ 6] | SaveStateBus_wired_or[ 7] | 
							SaveStateBus_wired_or[ 8] | SaveStateBus_wired_or[ 9] | SaveStateBus_wired_or[10] | SaveStateBus_wired_or[11] |
							SaveStateBus_wired_or[12] | SaveStateBus_wired_or[13] | SaveStateBus_wired_or[14] | SaveStateBus_wired_or[15] | 
							SaveStateBus_wired_or[16] | SaveStateBus_wired_or[17] | SaveStateBus_wired_or[18] | ss_extra_dout | sprite_state_dout | bg_state_q | ob_state_q;


wire [7:0] oam_do;
wire [10:0] sprite_addr;

wire sprite_found;
wire [7:0] sprite_attr;
wire [3:0] sprite_index;

wire oam_eval_end;

reg dma_active;
reg [7:0] dma;
reg [9:0] dma_cnt;     // dma runs 4*160 clock cycles = 160us @ 4MHz

// give dma access to oam
wire [7:0] oam_addr = dma_active?dma_addr[7:0]:cpu_addr;
wire oam_wr = dma_active?((dma_cnt[1:0] == 2) && !dma_wait):(cpu_wr && cpu_sel_oam && oam_cpu_allow);
wire [7:0] oam_di = dma_active?dma_data:cpu_di;

// $ff40 LCDC
reg [7:0] lcdc;
wire lcdc_on               = megaduck ? lcdc[7] : lcdc[7];
wire lcdc_win_tile_map_sel = megaduck ? lcdc[3] : lcdc[6];
wire lcdc_win_ena          = megaduck ? lcdc[5] : lcdc[5];
wire lcdc_tile_data_sel    = megaduck ? lcdc[4] : lcdc[4];
wire lcdc_bg_tile_map_sel  = megaduck ? lcdc[2] : lcdc[3];
wire lcdc_spr_siz          = megaduck ? lcdc[1] : lcdc[2];
wire lcdc_spr_ena          = megaduck ? lcdc[0] : lcdc[1];
// "CGB in CGB Mode: BG and Window Master Priority
//  When Bit 0 is cleared, the background and window lose their priority
//  - the sprites will be always displayed on top of background and window,
//  independently of the priority flags in OAM and BG Map attributes."
wire lcdc_bg_ena           =  (megaduck ? lcdc[6] : lcdc[0]) | (isGBC&&isGBC_mode);
wire lcdc_bg_prio          =  megaduck ? lcdc[6] : lcdc[0];

assign lcd_on = lcdc_on;

// ff41 STAT
reg [7:0] stat_r;
// DMG STAT bug: IRQs are enabled for 1 cycle during a write
wire [7:0] stat = (cpu_sel_reg & cpu_wr & cpu_addr == 8'h41 & ~isGBC) ? 8'hFF : stat_r;

// ff42, ff43 background scroll registers
reg [7:0] scy;
reg [7:0] scx;

// ff44 line counter
reg [6:0] h_cnt;            // 0-113 at 1MHz
reg [1:0] h_div_cnt;        // Divide by 4
reg [7:0] v_cnt;            // max 153
wire [7:0] ly = v_cnt;

// ff45 line counter compare
reg [7:0] lyc_r_dmg, lyc_r_gbc;
wire [7:0] lyc = (isGBC ? lyc_r_gbc : lyc_r_dmg);
wire lyc_match = (ly == lyc);

reg [7:0] bgp;
reg [7:0] obp0;
reg [7:0] obp1;

reg [7:0] wy;
reg [7:0] wx;

//ff68-ff6A GBC
//FF68 - BCPS/BGPI  - Background Palette Index
reg [5:0] bgpi; //Bit 0-5   Index (00-3F)
reg bgpi_ai;    //Bit 7     Auto Increment  (0=Disabled, 1=Increment after Writing)

//FF69 - BCPD/BGPD - Background Palette Data
wire[7:0] bgpd [63:0]; //64 bytes
wire [7:0] bug1 = bgpd[0];

//FF6A - OCPS/OBPI - Sprite Palette Index
reg [5:0] obpi; //Bit 0-5   Index (00-3F)
reg obpi_ai;    //Bit 7     Auto Increment  (0=Disabled, 1=Increment after Writing)

//FF6B - OCPD/OBPD - Sprite Palette Data
wire[7:0] obpd [63:0]; //64 bytes
wire [7:0] bug2 = obpd[0];

// Combined game palette data for games' default palette detection
assign gpd_out = {bg_colors[7:0],bg_colors[15:8],bg_colors[23:16],bg_colors[31:24],ob_colors[7:0],ob_colors[15:8],ob_colors[23:16],ob_colors[31:24]};

//FF6C Bit 0 OBJ priority mode select
// 0: smaller OBJ-NO has higher priority (GBC)
// 1: smaller X coordinate has higher priority (DMG)
// https://forums.nesdev.org/viewtopic.php?t=19888
reg ff6c_opri;
reg obj_prio_dmg_mode;

// --------------------------------------------------------------------
// ----------------------------- DMA engine ---------------------------
// --------------------------------------------------------------------

assign SS_Video1_BACK[    0] = dma_active;
assign SS_Video1_BACK[10: 1] = dma_cnt;

assign dma_addr = { dma, dma_cnt[9:2] };
assign dma_rd = dma_active;

always @(posedge clk) begin
	if(reset) begin
		dma_active <= SS_Video1[    0]; //1'b0;
		dma_cnt    <= SS_Video1[10: 1]; //10'd0;
	end else if (ce_cpu) begin
		// writing the dma register engages the dma engine
		if(cpu_sel_reg && cpu_wr && (cpu_addr == 8'h46)) begin
			dma_active <= 1'b1;
			dma_cnt <= 10'd0;
		end else if(!dma_wait) begin
			if(dma_cnt != 160*4-1)
				dma_cnt <= dma_cnt + 10'd1;
			else
				dma_active <= 1'b0;
		end
	end

    if (reset && ss_extra[0]) begin
        dma_active <= ss_extra[1];
        dma_cnt <= ss_extra[11:2];
    end
end

// --------------------------------------------------------------------
// ------------------------------- IRQs -------------------------------
// --------------------------------------------------------------------
//
// "The interrupt is triggered when transitioning from "No conditions met" to
// "Any condition met", which can cause the interrupt to not fire."
//
// Example: Altered Space enables OAM+Hblank+Vblank interrupt and requires the
// interrupt to trigger only on a Mode 3 to Hblank transition.
// OAM follows immediately after Hblank/Vblank so no OAM interrupt is triggered.

wire h_clk_en     = (lcd_on && h_div_cnt == 2'd0);
wire h_clk_en_neg = (lcd_on && h_div_cnt == 2'd2);

wire hcnt_end = (h_cnt == 7'd113);
wire vblank  = (v_cnt >= 144);
// Capture early in blanking, before the next line's sprite evaluation.
assign snapshot_safe_blank = (v_cnt == 8'd144);

reg vblank_l, vblank_t;
reg end_of_line, end_of_line_l;
reg lyc_match_l, lyc_match_t;

assign SS_Video3_BACK[0] = vblank_l;
assign SS_Video3_BACK[1] = end_of_line;
assign SS_Video3_BACK[2] = end_of_line_l;
assign SS_Video3_BACK[3] = 0;
assign SS_Video3_BACK[6] = vblank_t;

always @(posedge clk) begin
	if (reset) begin
		vblank_l       <= SS_Video3[0]; //1'b0;
		end_of_line    <= SS_Video3[1]; //1'b0;
		end_of_line_l  <= SS_Video3[2]; //1'b0;
		vblank_t       <= SS_Video3[6]; //1'b0;
	end else if (!lcd_on) begin
		vblank_l       <= 1'b0;
		end_of_line    <= 1'b0;
		end_of_line_l  <= 1'b0;
		vblank_t       <= 1'b0;
	end else if (ce) begin
		vblank_l <= vblank_t;

		if (h_clk_en_neg & hcnt_end)
			end_of_line <= 1'b1;
		else if (end_of_line) begin
			// Vblank is latched a few cycles after line end.
			// This causes an OAM interrupt at the beginning of line 144.
			// It also makes the OAM interrupt at line 0 after Vblank a few cycles late.
			if (h_clk_en) begin
				vblank_t <= vblank;
			end
			// end_of_line is active for 4 cycles
			if (h_clk_en_neg) begin
				end_of_line <= 1'b0;
			end
		end

		end_of_line_l <= end_of_line;
	end

    if (reset && ss_extra[0]) begin
        vblank_l <= ss_extra[12];
        end_of_line <= ss_extra[13];
        end_of_line_l <= ss_extra[14];
        vblank_t <= ss_extra[15];
    end
end

assign SS_Video3_BACK[4] = lyc_match_l;

always @(posedge clk) begin
	if (reset) begin
		lyc_match_l <= SS_Video3[4]; // 1'b0; // lyc_match_l does not reset when lcd is off
	end else if (ce) begin
		lyc_match_l <= lyc_match_t;

		if (h_clk_en) begin
			lyc_match_t <= lyc_match;
			if (lyc_match_t & ~lyc_match & ~isGBC) begin // DMG: lyc_match falling edge must be 1 cycle faster to pass Wilbertpol LYC tests.
				lyc_match_l <= lyc_match;
			end
		end
	end

    if (reset && ss_extra[0]) begin
        lyc_match_l <= ss_extra[16];
        lyc_match_t <= ss_extra[17];
    end
end

wire pcnt_reset, pcnt_end;
wire lcd_clk;
reg [7:0] pcnt;
wire mode3_end = (lcd_clk & pcnt == 8'd167) | pcnt_end;

reg mode3_end_l;

assign SS_Video3_BACK[5] = mode3_end_l;

always @(posedge clk) begin
	if (reset) begin
		mode3_end_l <= SS_Video3[5]; //1'b0;
	end else if (!lcd_on) begin
		mode3_end_l <= 1'b0;
	end else if (ce) begin
		if (pcnt_reset)
			mode3_end_l <= 1'b0;
		else
			mode3_end_l <= mode3_end;
	end

    if (reset && ss_extra[0]) begin
        mode3_end_l <= ss_extra[18];
    end
end

wire int_lyc = (stat[6] & lyc_match_l);
wire int_oam = (stat[5] & end_of_line_l & ~vblank_l);
wire int_vbl = (stat[4] & vblank_l);
wire int_hbl = (stat[3] & mode3_end_l & ~vblank_l);

assign irq = (int_lyc | int_oam | int_hbl | int_vbl);
assign vblank_irq = vblank_l;

wire oam_eval;
wire mode3   = lcd_on & ~mode3_end_l & oam_eval_end;

reg mode3_l, oam_eval_l;

assign SS_Video1_BACK[59] = mode3_l;
assign SS_Video1_BACK[60] = oam_eval_l;

// Delay mode 2/3 to pass Mooneye/Wilbertpol Lcd on timing tests.
// The CPU cannot read from OAM/VRAM 1 cycle before Mode 2/3 is read
always @(posedge clk) begin
	if (reset) begin
		mode3_l    <= SS_Video1[59]; // 1'b0;
		oam_eval_l <= SS_Video1[60]; // 1'b0;
	end else if (~lcd_on) begin
		mode3_l    <= 1'b0;
		oam_eval_l <= 1'b0;
	end else if (ce) begin
		mode3_l    <= mode3;
		oam_eval_l <= oam_eval;
	end

    if (reset && ss_extra[0]) begin
        mode3_l <= ss_extra[19];
        oam_eval_l <= ss_extra[20];
    end
end

// DMG: STAT reads mode 0 for 1 Tcycle between Vblank and mode 2
// but the interrupt signals of Vblank and mode 2 do overlap
wire mode_vblank = isGBC ? vblank_l : (vblank_l & vblank_t);

assign mode = 
	mode_vblank          ? 2'b01 :
	oam_eval_l           ? 2'b10 :
	mode3_l & ~mode3_end ? 2'b11 :
	                       2'b00;

assign oam_cpu_allow = ~(oam_eval | mode3);
assign vram_cpu_allow = ~mode3;

// --------------------------------------------------------------------
// --------------------- CPU register interface -----------------------
// --------------------------------------------------------------------

assign SS_Video1_BACK[18:11] = dma;
assign SS_Video1_BACK[26:19] = lcdc;
assign SS_Video1_BACK[34:27] = scy;
assign SS_Video1_BACK[42:35] = scx;
assign SS_Video1_BACK[50:43] = wy;
assign SS_Video1_BACK[58:51] = wx;

assign SS_Video2_BACK[ 7: 0] = stat_r;
assign SS_Video2_BACK[15: 8] = bgp;
assign SS_Video2_BACK[23:16] = obp0;
assign SS_Video2_BACK[31:24] = obp1;
assign SS_Video2_BACK[37:32] = bgpi;
assign SS_Video2_BACK[43:38] = obpi;
assign SS_Video2_BACK[   44] = bgpi_ai;
assign SS_Video2_BACK[   45] = obpi_ai;
assign SS_Video2_BACK[53:46] = lyc_r_gbc;
assign SS_Video2_BACK[61:54] = lyc_r_dmg;
assign SS_Video2_BACK[   62] = ff6c_opri;
assign SS_Video2_BACK[   63] = obj_prio_dmg_mode;

// Palette readback is supplied exclusively by bg_state_q / ob_state_q.


// Use negedge on some registers to pass tests
always @(negedge clk) begin
	if(reset) begin
		lcdc      <= SS_Video1[26:19]; // 8'h00;  // screen must be off since dmg rom writes to vram
		lyc_r_dmg <= SS_Video2[61:54]; // 8'h00;
	end else if (ce_cpu) begin
		if(cpu_sel_reg && cpu_wr) begin
			case(cpu_addr)
				8'h40:	lcdc <= cpu_di;
				8'h45:	lyc_r_dmg <= cpu_di;
			endcase
		end
	end


    if (reset && ss_extra[0]) begin
        lcdc <= ss_extra[28:21];
        lyc_r_dmg <= ss_extra[36:29];
    end
end

always @(posedge clk) begin

	if(reset) begin
	
		dma     <= SS_Video1[18:11]; // 8'h00;
		scy     <= SS_Video1[34:27]; // 8'h00;
		scx     <= SS_Video1[42:35]; // 8'h00;
		wy      <= SS_Video1[50:43]; // 8'h00;
		wx      <= SS_Video1[58:51]; // 8'h00;
		stat_r  <= SS_Video2[ 7: 0]; // 8'h00;
		bgp     <= SS_Video2[15: 8]; // 8'hfc;
		obp0    <= SS_Video2[23:16]; // 8'hff;
		obp1    <= SS_Video2[31:24]; // 8'hff;							     
		bgpi    <= SS_Video2[37:32]; // 6'h0;
		obpi    <= SS_Video2[43:38]; // 6'h0;
		bgpi_ai <= SS_Video2[   44]; // 1'b0;
		obpi_ai <= SS_Video2[   45]; // 1'b0;
		lyc_r_gbc <= SS_Video2[53:46]; // 8'h00;
		ff6c_opri <= SS_Video2[   62]; // 1'b0;
		obj_prio_dmg_mode <= SS_Video2[   63]; // 1'b0;

// synthesis translate_off
















      
















// synthesis translate_on

	end else if (ce_cpu) begin
		if(cpu_sel_reg && cpu_wr) begin
			case(cpu_addr)
				8'h41:	stat_r <= cpu_di;
				8'h42:	scy <= cpu_di;
				8'h43:	scx <= cpu_di;
				8'h45:	lyc_r_gbc <= cpu_di;
				8'h46:	dma <= cpu_di;
				8'h47:	bgp <= cpu_di;
				8'h48:	obp0 <= cpu_di;
				8'h49:	obp1 <= cpu_di;
				8'h4a:	wy <= cpu_di;
				8'h4b:	wx <= cpu_di;
			endcase

			//gbc
			if (isGBC) case(cpu_addr)
				8'h68: begin
							bgpi <= cpu_di[5:0];
							bgpi_ai <= cpu_di[7];
						 end
				8'h69: if (isGBC_mode) begin
							if (vram_cpu_allow) begin
								
							end
							//"Writing to FF69 during rendering still causes auto-increment to occur."
							if (bgpi_ai) bgpi <= bgpi + 6'h1;
						 end
				8'h6A: begin
							obpi <= cpu_di[5:0];
							obpi_ai <= cpu_di[7];
						 end
				8'h6B: if (isGBC_mode) begin
							if (vram_cpu_allow) begin
								
							end
							if (obpi_ai) obpi <= obpi + 6'h1;
						 end
				8'h6C: begin
							// Reportedly can be written to when boot rom is enabled or FF4C Bit2=0 (GBC mode)
							// but only affects OBJ prio if written when boot rom is enabled.
							if (boot_rom_en | isGBC_mode) begin
								ff6c_opri <= cpu_di[0];
							end
							if (boot_rom_en) obj_prio_dmg_mode <= cpu_di[0];
						end
			endcase
		end
	end

    if (reset && ss_extra[0]) begin
        dma <= ss_extra[44:37];
        scy <= ss_extra[52:45];
        scx <= ss_extra[60:53];
        wy <= ss_extra[68:61];
        wx <= ss_extra[76:69];
        stat_r <= ss_extra[84:77];
        bgp <= ss_extra[92:85];
        obp0 <= ss_extra[100:93];
        obp1 <= ss_extra[108:101];
        bgpi <= ss_extra[114:109];
        obpi <= ss_extra[120:115];
        bgpi_ai <= ss_extra[121];
        obpi_ai <= ss_extra[122];
        lyc_r_gbc <= ss_extra[130:123];
        ff6c_opri <= ss_extra[131];
        obj_prio_dmg_mode <= ss_extra[132];
































































































































    end
    // Stream palette restore into live storage; execution remains paused.
















































































































































end


assign cpu_do =
	cpu_sel_oam?oam_do:
	(cpu_addr == 8'h40)?lcdc:
	(cpu_addr == 8'h41)?{1'b1,stat[6:3], lyc_match_l, mode}:
	(cpu_addr == 8'h42)?scy:
	(cpu_addr == 8'h43)?scx:
	(cpu_addr == 8'h44)?ly:
	(cpu_addr == 8'h45)?lyc:
	(cpu_addr == 8'h46)?dma:
	(cpu_addr == 8'h47)?bgp:
	(cpu_addr == 8'h48)?obp0:
	(cpu_addr == 8'h49)?obp1:
	(cpu_addr == 8'h4a)?wy:
	(cpu_addr == 8'h4b)?wx:
	isGBC?
		(cpu_addr == 8'h68)?{bgpi_ai,1'd1,bgpi}:
		(cpu_addr == 8'h69 && isGBC_mode && vram_cpu_allow)?bg_cpu_q:
		(cpu_addr == 8'h6a)?{obpi_ai,1'd1,obpi}:
		(cpu_addr == 8'h6b && isGBC_mode && vram_cpu_allow)?ob_cpu_q:
		(cpu_addr == 8'h6c) ? { 7'h7f, ff6c_opri } :
		8'hff:
	8'hff;

// --------------------------------------------------------------------
// -------------- counters & background tilemap address----------------
// --------------------------------------------------------------------

reg skip_done;
reg [2:0] skip_cnt;
wire sprite_fetch_hold;
wire bg_shift_empty;
wire skip_end = (skip_cnt == scx[2:0] || &skip_cnt);
wire skip_en = ~skip_done & ~skip_end;

assign pcnt_end = ( pcnt == 8'd168 );
assign pcnt_reset = h_clk_en & end_of_line & ~vblank;

assign SS_Video3_BACK[ 9: 7] = skip_cnt;
assign SS_Video3_BACK[17:10] = pcnt;
assign SS_Video3_BACK[   18] = skip_done;
assign SS_Video3_BACK[21:19] = 0;

always @(posedge clk) begin
	if (reset) begin
		skip_cnt  <= SS_Video3[ 9: 7]; // 3'd0;
		pcnt      <= SS_Video3[17:10]; // 8'd0;
		skip_done <= SS_Video3[   18]; // 1'd0;
	end else if (!lcd_on) begin
		skip_cnt <= 3'd0;
		pcnt     <= 8'd0;
		skip_done <= 1'b0;
	end else if (ce) begin
		// Only skip when not paused for sprites and fifo is not empty.
		// Skipping pixels must happen at pcnt = 0 to pass Wilbertpol intr2_mode0_scx_timing tests.
		if (~sprite_fetch_hold & ~bg_shift_empty) begin

			if (~skip_done) begin
				if (~skip_end)
					skip_cnt <= skip_cnt + 1'd1;
				else
					skip_done <= 1'd1;
			end

			// Pixels 0-7 are for fetching partially offscreen sprites and window.
			// Pixels 8-167 are output to the display.
			if(~skip_en & ~pcnt_end)
				pcnt <= pcnt + 1'd1;

		end

		if (pcnt_reset) begin
			pcnt <= 8'd0;
			skip_done <= 1'b0;
			skip_cnt <= 3'd0;
		end

	end

    if (reset && ss_extra[0]) begin
        skip_cnt <= ss_extra[1159:1157];
        pcnt <= ss_extra[1167:1160];
        skip_done <= ss_extra[1168];
    end
end


wire line153 = (v_cnt == 8'd153);
reg vcnt_reset;
reg vsync;
reg vcnt_eol_l;

assign SS_Video3_BACK[   22] = vcnt_eol_l;
assign SS_Video3_BACK[   23] = vcnt_reset;
assign SS_Video3_BACK[31:24] = v_cnt;
assign SS_Video3_BACK[   32] = vsync;

always @(posedge clk) begin
	if (reset) begin
		vcnt_eol_l <= SS_Video3[   22]; // 1'b0;
		vcnt_reset <= SS_Video3[   23]; // 1'b0;
		v_cnt      <= SS_Video3[31:24]; // 8'd0;
		vsync      <= SS_Video3[   32]; // 1'b0;
	end else if (!lcdc_on) begin
		vcnt_eol_l <= 1'b0;
		vcnt_reset <= 1'b0;
		v_cnt      <= 8'd0;
		vsync      <= 1'b0;
	end else if (ce) begin
		if (~vcnt_reset & h_clk_en_neg & hcnt_end) begin
			v_cnt <= v_cnt + 1'b1;
		end

		// Line 153->0 reset happens a few cycles later on GBC
		if ( (~isGBC & h_clk_en) | (isGBC & h_clk_en_neg) ) begin
			vcnt_eol_l <= end_of_line;
			if (vcnt_eol_l & ~end_of_line) begin
				// vcnt_reset goes high a few cycles after v_cnt is incremented to 153.
				// It resets v_cnt back to 0 and keeps it in reset until the following line.
				// This results in v_cnt 0 lasting for almost 2 lines.
				vcnt_reset <= line153;
				if (line153) begin
					v_cnt <= 8'd0;
				end

				// VSync goes high on line 0 but it takes a full frame after the LCD is enabled
				// because the first line where end_of_line is high after LCD is enabled is line 1.
				vsync <= !v_cnt;
			end
		end

	end

    if (reset && ss_extra[0]) begin
        vcnt_eol_l <= ss_extra[1169];
        vcnt_reset <= ss_extra[1170];
        v_cnt <= ss_extra[1178:1171];
        vsync <= ss_extra[1179];
    end
end

assign lcd_vsync = vsync;

// line inside the background currently being drawn
wire [7:0] bg_line = v_cnt + scy;
wire [7:0] bg_col  = pcnt + scx;
wire [9:0] bg_map_addr = {bg_line[7:3], bg_col[7:3]};

reg  [4:0] win_col;
reg [7:0] win_line;
wire window_ena;
wire bg_fetch_done;
wire bg_reload_shift;

wire [9:0] win_map_addr = {win_line[7:3], win_col[4:0]};

wire [9:0] bg_tile_map_addr = window_ena ? win_map_addr : bg_map_addr;

wire [2:0] tile_line = window_ena ? win_line[2:0] : bg_line[2:0];

reg wy_match, window_match, window_ena_d;

wire win_start = ~window_match & mode3 && lcdc_win_ena && ~sprite_fetch_hold && ~skip_en && ~bg_shift_empty && wy_match && (pcnt == wx) && (wx < 8'hA7);
assign window_ena = window_match & ~pcnt_reset & lcdc_win_ena;

assign SS_Video3_BACK[39:33] = h_cnt       ;
assign SS_Video3_BACK[41:40] = h_div_cnt   ;
assign SS_Video3_BACK[   42] = window_match;
assign SS_Video3_BACK[   43] = window_ena_d;
assign SS_Video3_BACK[48:44] = win_col     ;
assign SS_Video3_BACK[56:49] = win_line    ;
assign SS_Video3_BACK[   63] = wy_match    ;

always @(posedge clk) begin

	if (reset) begin
		h_cnt        <= SS_Video3[39:33]; // 7'd0;
		h_div_cnt    <= SS_Video3[41:40]; // 2'd0;
		window_match <= SS_Video3[   42]; // 1'b0;
		window_ena_d <= SS_Video3[   43]; // 1'b0;
		win_col      <= SS_Video3[48:44]; // 5'd0;
		win_line     <= SS_Video3[56:49]; // 8'd0;
		wy_match     <= SS_Video3[   63]; // 1'b0;
	end else if (!lcdc_on) begin
		//reset counters
		h_cnt        <= 7'd0;
		h_div_cnt    <= 2'd0;
		window_match <= 1'b0;
		window_ena_d <= 1'b0;
		win_col      <= 5'd0;
		win_line     <= 8'd0;
		wy_match     <= 1'b0;
	end else if (ce) begin

		h_div_cnt <= h_div_cnt + 1'b1;
		if (h_clk_en) begin
			h_cnt <= hcnt_end ? 7'd0 : h_cnt + 1'b1;
		end

		if (vblank_l) begin
			wy_match <= 1'b0;
		end else if (h_clk_en & lcdc_win_ena & v_cnt == wy) begin
			wy_match <= 1'b1;
		end

		if(win_start) begin
			window_match <= 1'b1;
		end

		window_ena_d <= window_ena;
		if (vblank_l)
			win_line <= 8'd0;
		else if (window_ena_d & ~window_ena)
			win_line <= win_line + 1'b1;

		if (window_match) begin
			if (~mode3_end_l & mode3_end) begin
				// DMG glitch: If WX = A6 then window_match stays high through VBlank
				// until WY > v_cnt which means the window always appears on line 0.
				if (isGBC || wx != 8'hA6 || wy > v_cnt) begin
					window_match <= 1'b0;   // next line starts with background
				end
			end

			if (~lcdc_win_ena) window_match <= 1'b0;
		end

		// Increment when fetching is done and not waiting for sprites.
		if(window_ena && ~sprite_fetch_hold && bg_fetch_done && bg_reload_shift)
			win_col <= win_col + 1'b1;

		if (~lcdc_win_ena | pcnt_reset) win_col <= 5'd0;

	end

    if (reset && ss_extra[0]) begin
        h_cnt <= ss_extra[1186:1180];
        h_div_cnt <= ss_extra[1188:1187];
        window_match <= ss_extra[1189];
        window_ena_d <= ss_extra[1190];
        win_col <= ss_extra[1195:1191];
        win_line <= ss_extra[1203:1196];
        wy_match <= ss_extra[1204];
    end
end

// --------------------------------------------------------------------
// ------------------- bg, window and sprite fetch  -------------------
// --------------------------------------------------------------------

function [7:0] bit_reverse;
	input [7:0] a;
	begin
		bit_reverse = {a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]};
	end
endfunction

// output shift registers for both video data bits
reg [7:0] tile_shift_0;
reg [7:0] tile_shift_1;

// sprite output shift registers
reg [7:0] spr_tile_shift_0;
reg [7:0] spr_tile_shift_1;
reg [7:0] spr_pal_shift;
reg [7:0] spr_prio_shift;
reg [7:0] spr_cgb_pal_shift[0:2];
reg [7:0] spr_cgb_index_shift[0:3];

reg [7:0] bg_tile;
reg [7:0] bg_tile_data0;
reg [7:0] bg_tile_data1;

reg [7:0] spr_tile_data0;

reg [7:0] bg_tile_attr,bg_tile_attr_new; //GBC
//gbc check tile bank
wire [7:0] bg_vram_data_a = (isGBC & isGBC_mode & bg_tile_attr_new[3]) ? vram1_data : vram_data;
//gbc x-flip
wire [7:0] bg_vram_data_in = (isGBC & isGBC_mode & bg_tile_attr_new[5]) ? bit_reverse(bg_vram_data_a) : bg_vram_data_a;

// A bg or sprite fetch takes 6 cycles
reg [2:0] bg_fetch_cycle;
reg [2:0] sprite_fetch_cycle;

assign bg_fetch_done = (bg_fetch_cycle >= 3'd5);
wire sprite_fetch_done = (sprite_fetch_hold && sprite_fetch_cycle >= 3'd5);

// The first B01 cycle does not fetch sprites so wait until the bg shift register is not empty
assign sprite_fetch_hold = sprite_found & ~bg_shift_empty;

reg [3:0] bg_shift_cnt;
assign bg_shift_empty = (bg_shift_cnt == 0);
assign bg_reload_shift = (bg_shift_cnt <= 1);

wire spr_prio          = sprite_attr[7];
wire spr_attr_h_flip   = sprite_attr[5];
wire spr_pal           = sprite_attr[4];
wire spr_attr_cgb_bank = sprite_attr[3];
wire [2:0] spr_cgb_pal = sprite_attr[2:0];

wire [7:0] spr_vram_data = (isGBC & isGBC_mode & spr_attr_cgb_bank) ? vram1_data : vram_data;
wire [7:0] spr_tile_data_in = spr_attr_h_flip ? bit_reverse(spr_vram_data) : spr_vram_data;

// CGB sprite priority. Non-transparent pixels with lower sprite_index have priority.
function [7:0] spr_cgb_prio;
	input [7:0] a3,a2,a1,a0;
	integer i;
	begin
		for (i=0;i<8;i=i+1)
			spr_cgb_prio[i] = (sprite_index < {a3[i], a2[i], a1[i], a0[i]}) & (spr_tile_data_in[i] | spr_tile_data0[i]);
	end
endfunction

wire [7:0] spr_cgb_index_prio = spr_cgb_prio(spr_cgb_index_shift[3], spr_cgb_index_shift[2], spr_cgb_index_shift[1], spr_cgb_index_shift[0]);

// DMG sprite pixels are only loaded into the shift register if the old pixel is transparent.
// CGB will mask the old pixel to 0 if the new pixel has higher priority.
wire [7:0] spr_tile_mask = (spr_tile_shift_0 | spr_tile_shift_1) & ((isGBC & ~obj_prio_dmg_mode) ? ~spr_cgb_index_prio : 8'hFF);

// cycle through the B01s states
wire bg_tile_map_rd = (mode3 && bg_fetch_cycle[2:1] == 2'b00);
wire bg_tile_data0_rd = (mode3 && bg_fetch_cycle[2:1] == 2'b01);
wire bg_tile_data1_rd = (mode3 && bg_fetch_cycle[2:1] == 2'b10);
wire bg_tile_obj0_rd = (mode3 && sprite_fetch_cycle[2:1] == 2'b01);
wire bg_tile_obj1_rd = (mode3 && sprite_fetch_cycle[2:1] == 2'b10);

assign SS_Video3_BACK[59:57] = bg_fetch_cycle;
assign SS_Video3_BACK[62:60] = sprite_fetch_cycle;

always @(posedge clk) begin
	
	if (reset) begin
	
		bg_fetch_cycle     <= SS_Video3[59:57]; // 3'b000;
		sprite_fetch_cycle <= SS_Video3[62:60]; // 3'b000;

	end else if (ce) begin

		// every memory access is two pixel cycles
		if (bg_fetch_cycle[0]) begin
			if(bg_tile_map_rd) begin
				bg_tile <= vram_data;
				if (isGBC & isGBC_mode) bg_tile_attr_new <= vram1_data; //get tile attr from vram bank1
			end

			if(bg_tile_data0_rd) bg_tile_data0 <= bg_vram_data_in;
			if(bg_tile_data1_rd) bg_tile_data1 <= bg_vram_data_in;
		end

		// Shift sprite data out
		if (~sprite_fetch_hold & ~skip_en & ~bg_shift_empty) begin
			spr_tile_shift_0       <=  spr_tile_shift_0       << 1;
			spr_tile_shift_1       <=  spr_tile_shift_1       << 1;
			spr_pal_shift          <=  spr_pal_shift          << 1;
			spr_prio_shift         <=  spr_prio_shift         << 1;
			spr_cgb_pal_shift[0]   <=  spr_cgb_pal_shift[0]   << 1;
			spr_cgb_pal_shift[1]   <=  spr_cgb_pal_shift[1]   << 1;
			spr_cgb_pal_shift[2]   <=  spr_cgb_pal_shift[2]   << 1;
			spr_cgb_index_shift[0] <=  spr_cgb_index_shift[0] << 1;
			spr_cgb_index_shift[1] <=  spr_cgb_index_shift[1] << 1;
			spr_cgb_index_shift[2] <=  spr_cgb_index_shift[2] << 1;
			spr_cgb_index_shift[3] <=  spr_cgb_index_shift[3] << 1;
		end

		// Fetch sprite new data
		if (sprite_fetch_cycle[0]) begin
			if(bg_tile_obj0_rd) spr_tile_data0 <= spr_tile_data_in;

			if(bg_tile_obj1_rd) begin
				spr_tile_shift_0       <= (spr_tile_shift_0       & spr_tile_mask) | ( spr_tile_data0      & ~spr_tile_mask);
				spr_tile_shift_1       <= (spr_tile_shift_1       & spr_tile_mask) | ( spr_tile_data_in    & ~spr_tile_mask);
				spr_pal_shift          <= (spr_pal_shift          & spr_tile_mask) | ({8{spr_pal}}         & ~spr_tile_mask);
				spr_prio_shift         <= (spr_prio_shift         & spr_tile_mask) | ({8{spr_prio}}        & ~spr_tile_mask);
				spr_cgb_pal_shift[0]   <= (spr_cgb_pal_shift[0]   & spr_tile_mask) | ({8{spr_cgb_pal[0]}}  & ~spr_tile_mask);
				spr_cgb_pal_shift[1]   <= (spr_cgb_pal_shift[1]   & spr_tile_mask) | ({8{spr_cgb_pal[1]}}  & ~spr_tile_mask);
				spr_cgb_pal_shift[2]   <= (spr_cgb_pal_shift[2]   & spr_tile_mask) | ({8{spr_cgb_pal[2]}}  & ~spr_tile_mask);
				spr_cgb_index_shift[0] <= (spr_cgb_index_shift[0] & spr_tile_mask) | ({8{sprite_index[0]}} & ~spr_tile_mask);
				spr_cgb_index_shift[1] <= (spr_cgb_index_shift[1] & spr_tile_mask) | ({8{sprite_index[1]}} & ~spr_tile_mask);
				spr_cgb_index_shift[2] <= (spr_cgb_index_shift[2] & spr_tile_mask) | ({8{sprite_index[2]}} & ~spr_tile_mask);
				spr_cgb_index_shift[3] <= (spr_cgb_index_shift[3] & spr_tile_mask) | ({8{sprite_index[3]}} & ~spr_tile_mask);
			end
		end

		if (~&bg_fetch_cycle) begin
			bg_fetch_cycle <= bg_fetch_cycle + 1'b1;
		end

		if (~sprite_fetch_hold) begin
			// shift bg/window pixels out
			tile_shift_0 <= tile_shift_0 << 1;
			tile_shift_1 <= tile_shift_1 << 1;

			if (|bg_shift_cnt) bg_shift_cnt <= bg_shift_cnt - 1'b1;

			if (bg_fetch_done && bg_reload_shift) begin
				bg_tile_attr <= bg_tile_attr_new;
				tile_shift_0 <= bg_tile_data0;
				// bg_tile_data1 only has valid data at cycle 6 so at cycle 5 bg_vram_data_in is loaded instead.
				tile_shift_1 <= (bg_fetch_cycle == 3'd5) ? bg_vram_data_in : bg_tile_data1;
				bg_shift_cnt <= 4'd8;
				bg_fetch_cycle <= 0;
			end
		end

		// Start sprite fetch after background fetching is done
		// Sprite fetching continues until there are no more sprites on the current x position
		if (sprite_fetch_hold && bg_fetch_done) begin
			sprite_fetch_cycle <= sprite_fetch_cycle + 1'b1;
		end

		if (~sprite_fetch_hold || sprite_fetch_done) sprite_fetch_cycle <= 0;

		// Window start, reset fetching
		// DMG glitch: fetching is not reset if WX = A6. The following lines all show
		// the window from the start of the line.
		if ( (win_start && (isGBC || wx != 8'hA6)) || !mode3) begin
			bg_fetch_cycle <= 0;
			bg_shift_cnt <= 0;
		end

	end


    if (reset && ss_extra[0]) begin
        bg_fetch_cycle <= ss_extra[1207:1205];
        sprite_fetch_cycle <= ss_extra[1210:1208];
        bg_tile <= ss_extra[1218:1211];
        bg_tile_attr_new <= ss_extra[1226:1219];
        bg_tile_data0 <= ss_extra[1234:1227];
        bg_tile_data1 <= ss_extra[1242:1235];
        spr_tile_shift_0 <= ss_extra[1250:1243];
        spr_tile_shift_1 <= ss_extra[1258:1251];
        spr_pal_shift <= ss_extra[1266:1259];
        spr_prio_shift <= ss_extra[1274:1267];
        spr_cgb_pal_shift[0] <= ss_extra[1282:1275];
        spr_cgb_pal_shift[1] <= ss_extra[1290:1283];
        spr_cgb_pal_shift[2] <= ss_extra[1298:1291];
        spr_cgb_index_shift[0] <= ss_extra[1306:1299];
        spr_cgb_index_shift[1] <= ss_extra[1314:1307];
        spr_cgb_index_shift[2] <= ss_extra[1322:1315];
        spr_cgb_index_shift[3] <= ss_extra[1330:1323];
        spr_tile_data0 <= ss_extra[1338:1331];
        tile_shift_0 <= ss_extra[1346:1339];
        tile_shift_1 <= ss_extra[1354:1347];
        bg_shift_cnt <= ss_extra[1358:1355];
        bg_tile_attr <= ss_extra[1366:1359];
    end
end

assign vram_rd = lcdc_on && (bg_tile_map_rd || bg_tile_data0_rd ||
										bg_tile_data1_rd || bg_tile_obj0_rd || bg_tile_obj1_rd);

wire bg_tile_a12 = !lcdc_tile_data_sel?(~bg_tile[7]):1'b0;

wire tile_map_sel = window_ena?lcdc_win_tile_map_sel:lcdc_bg_tile_map_sel;

//GBC: check if flipped y
wire [2:0] tile_line_flip = (isGBC && isGBC_mode && bg_tile_attr_new[6]) ? ~tile_line : tile_line;

assign vram_addr =
	bg_tile_map_rd?{2'b11, tile_map_sel, bg_tile_map_addr}:
	bg_tile_data0_rd?{bg_tile_a12, bg_tile, tile_line_flip, 1'b0}:
	bg_tile_data1_rd?{bg_tile_a12, bg_tile, tile_line_flip, 1'b1}:
	bg_tile_obj0_rd ? {1'b0, sprite_addr, 1'b0} :
		{1'b0, sprite_addr, 1'b1};


wire [63:0] sprite_state_dout;
sprites_state sprites (.ss_load(reset),.ss_din(SaveStateBus_Din),.ss_adr(SaveStateBus_Adr),.ss_wr(SaveStateBus_wren),.ss_rst(SaveStateBus_rst),.ss_dout(sprite_state_dout),
	.clk      ( clk          ),
	.ce       ( ce           ),
	.ce_cpu   ( ce_cpu       ),
	.size16   ( lcdc_spr_siz ),
	.isGBC    ( isGBC        ),
	.sprite_en( lcdc_spr_ena ),
	.lcd_on   ( lcd_on       ),

	.v_cnt    ( v_cnt        ),
	.h_cnt    ( pcnt        ),

	.oam_eval       ( oam_eval     ),
	.oam_fetch      ( mode3        ),
	.oam_eval_reset ( pcnt_reset   ),
	.oam_eval_end   ( oam_eval_end ),

	.sprite_fetch (sprite_found),
	.sprite_addr ( sprite_addr                 ),
	.sprite_attr ( sprite_attr ),
	.sprite_index ( sprite_index ),
	.sprite_fetch_done ( sprite_fetch_done) ,

	.dma_active ( dma_active),
	.oam_wr     ( oam_wr       ),
	.oam_addr_in( oam_addr     ),
	.oam_di     ( oam_di       ),
	.oam_do     ( oam_do       ),

	.Savestate_OAMRAMAddr      (Savestate_OAMRAMAddr),     
	.Savestate_OAMRAMRWrEn     (Savestate_OAMRAMRWrEn),    
	.Savestate_OAMRAMWriteData (Savestate_OAMRAMWriteData),
	.Savestate_OAMRAMReadData  (Savestate_OAMRAMReadData)   
);

// --------------------------------------------------------------------
// ----------------------- lcd output stage   -------------------------
// --------------------------------------------------------------------

// https://forums.nesdev.com/viewtopic.php?f=20&t=10771&sid=8fdb6e110fd9b5434d4a567b1199585e#p122222
// priority list: BG0 < OBJL < BGL < OBJH < BGH
wire bg_piority = isGBC && isGBC_mode && bg_tile_attr[7];

reg sprite_pixel_visible;

wire [1:0] sprite_pixel_data = {spr_tile_shift_1[7], spr_tile_shift_0[7]};
wire sprite_pixel_prio = spr_prio_shift[7];

wire [1:0] bg_pix_data = { tile_shift_1[7], tile_shift_0[7] };
always @(*) begin
	sprite_pixel_visible = 1'b0;
	if (|sprite_pixel_data && lcdc_spr_ena) begin		// pixel active and sprites enabled

		if (isGBC&&isGBC_mode) begin
			if (sprite_pixel_data == 2'b00)
				sprite_pixel_visible = 1'b0;
			else if (bg_pix_data == 2'b00)
				sprite_pixel_visible = 1'b1;
			else if (!lcdc_bg_prio)
				sprite_pixel_visible = 1'b1;
			else if (bg_piority)
				sprite_pixel_visible = 1'b0;
			else if (!sprite_pixel_prio) //sprite pixel priority is enabled
				sprite_pixel_visible = 1'b1;

		end else begin
			if (sprite_pixel_data == 2'b00)
				sprite_pixel_visible = 1'b0;
			else if (bg_pix_data == 2'b00)
				sprite_pixel_visible = 1'b1;
			else if (!sprite_pixel_prio) //sprite pixel priority is enabled
				sprite_pixel_visible = 1'b1;
		end

	end

end

// If lcdc_bg_ena is off in Non-GBC mode then both background and window are blank. (BG color 0)
wire [1:0] bgp_data = (!lcdc_bg_ena || bg_pix_data == 2'b00) ? bgp[1:0] :
									  (bg_pix_data == 2'b01) ? bgp[3:2] :
									  (bg_pix_data == 2'b10) ? bgp[5:4] :
															bgp[7:6];

wire sprite_pixel_cmap = spr_pal_shift[7];
wire [7:0] obp = sprite_pixel_cmap?obp1:obp0;
wire [1:0] obp_data =   (sprite_pixel_data == 2'b00) ? obp[1:0] :
						(sprite_pixel_data == 2'b01) ? obp[3:2] :
						(sprite_pixel_data == 2'b10) ? obp[5:4] :
													obp[7:6];

wire [5:0] palette_index = isGBC_mode ? {bg_tile_attr[2:0], bg_pix_data, 1'b0} :  //GBC game
									{3'd0, bgp_data , 1'b0}; //GB game in GBC mode


wire [7:0] paletteCustomBG [7:0];
assign paletteCustomBG[0] = paletteBGIn[ 7: 0];
assign paletteCustomBG[1] = paletteBGIn[15: 8];
assign paletteCustomBG[2] = paletteBGIn[23:16];
assign paletteCustomBG[3] = paletteBGIn[31:24];
assign paletteCustomBG[4] = paletteBGIn[39:32];
assign paletteCustomBG[5] = paletteBGIn[47:40];
assign paletteCustomBG[6] = paletteBGIn[55:48];
assign paletteCustomBG[7] = paletteBGIn[63:56];

wire [7:0] paletteCustomOBJ [15:0];
assign paletteCustomOBJ[0]  = paletteOBJ0In[ 7: 0];
assign paletteCustomOBJ[1]  = paletteOBJ0In[15: 8];
assign paletteCustomOBJ[2]  = paletteOBJ0In[23:16];
assign paletteCustomOBJ[3]  = paletteOBJ0In[31:24];
assign paletteCustomOBJ[4]  = paletteOBJ0In[39:32];
assign paletteCustomOBJ[5]  = paletteOBJ0In[47:40];
assign paletteCustomOBJ[6]  = paletteOBJ0In[55:48];
assign paletteCustomOBJ[7]  = paletteOBJ0In[63:56];

assign paletteCustomOBJ[8]  = paletteOBJ1In[ 7: 0];
assign paletteCustomOBJ[9]  = paletteOBJ1In[15: 8];
assign paletteCustomOBJ[10] = paletteOBJ1In[23:16];
assign paletteCustomOBJ[11] = paletteOBJ1In[31:24];
assign paletteCustomOBJ[12] = paletteOBJ1In[39:32];
assign paletteCustomOBJ[13] = paletteOBJ1In[47:40];
assign paletteCustomOBJ[14] = paletteOBJ1In[55:48];
assign paletteCustomOBJ[15] = paletteOBJ1In[63:56];

// apply bg palette
wire [2:0] palette_index_gb = palette_index[2:0];
wire [14:0] gbc_paletteBG = isGBC ? bg_pixel_q[14:0] : // gbc
                                    {13'd0, bgp_data};
wire [14:0] pix_rgb_data = (customPaletteEna && ~isGBC_mode) ? {paletteCustomBG[palette_index_gb+1][6:0], paletteCustomBG[palette_index_gb]} : // custom
                                                                gbc_paletteBG;

// apply sprite palette
wire [2:0] spr_cgb_pal_out = {spr_cgb_pal_shift[2][7], spr_cgb_pal_shift[1][7], spr_cgb_pal_shift[0][7]};
wire [5:0] sprite_palette_index = isGBC_mode ? {spr_cgb_pal_out, sprite_pixel_data, 1'b0 } : //gbc game
                                               {sprite_pixel_cmap, obp_data, 1'b0}; //GB game in GBC mode

wire [14:0] gbc_paletteSprite = isGBC ? ob_pixel_q[14:0] : // gbc
                                        {13'd0, obp_data};
wire [14:0] sprite_pix = (customPaletteEna && ~isGBC_mode) ? {paletteCustomOBJ[sprite_palette_index+1][6:0], paletteCustomOBJ[sprite_palette_index]} : // custom
                                                              gbc_paletteSprite;

assign lcd_clk = mode3 && ~skip_en && ~sprite_fetch_hold && ~bg_shift_empty && (pcnt >= 8);

reg [14:0] lcd_data_out;
reg  [1:0] lcd_data_gb_out;
reg lcd_clk_out;
always @(posedge clk) begin
	if (ce) begin
		lcd_clk_out <= lcd_clk;
		if (lcd_clk) begin
			lcd_data_out <= (sprite_pixel_visible) ? sprite_pix : pix_rgb_data;
			lcd_data_gb_out <= (sprite_pixel_visible) ? obp_data : bgp_data;
		end

		// Output blank pixels if lcd is off.
		if (~lcd_on) lcd_data_out <= isGBC ? 15'h7FFF : 15'd0;
	end

    if (reset && ss_extra[0]) begin
        lcd_clk_out <= ss_extra[1367];
        lcd_data_out <= 15'd0;
        lcd_data_gb_out <= 2'd0;
    end
end

assign lcd_data = lcd_data_out;
assign lcd_data_gb = lcd_data_gb_out;
assign lcd_clkena = lcd_clk_out;


wire [1407:0] ss_extra, ss_extra_back;
wire [63:0] ss_extra_dout;
assign ss_extra_back[0] = 1'b1;
assign ss_extra_back[1407:1385] = 0;
assign ss_extra_back[1] = dma_active;
assign ss_extra_back[11:2] = dma_cnt;
assign ss_extra_back[12] = vblank_l;
assign ss_extra_back[13] = end_of_line;
assign ss_extra_back[14] = end_of_line_l;
assign ss_extra_back[15] = vblank_t;
assign ss_extra_back[16] = lyc_match_l;
assign ss_extra_back[17] = lyc_match_t;
assign ss_extra_back[18] = mode3_end_l;
assign ss_extra_back[19] = mode3_l;
assign ss_extra_back[20] = oam_eval_l;
assign ss_extra_back[28:21] = lcdc;
assign ss_extra_back[36:29] = lyc_r_dmg;
assign ss_extra_back[44:37] = dma;
assign ss_extra_back[52:45] = scy;
assign ss_extra_back[60:53] = scx;
assign ss_extra_back[68:61] = wy;
assign ss_extra_back[76:69] = wx;
assign ss_extra_back[84:77] = stat_r;
assign ss_extra_back[92:85] = bgp;
assign ss_extra_back[100:93] = obp0;
assign ss_extra_back[108:101] = obp1;
assign ss_extra_back[114:109] = bgpi;
assign ss_extra_back[120:115] = obpi;
assign ss_extra_back[121] = bgpi_ai;
assign ss_extra_back[122] = obpi_ai;
assign ss_extra_back[130:123] = lyc_r_gbc;
assign ss_extra_back[131] = ff6c_opri;
assign ss_extra_back[132] = obj_prio_dmg_mode;
assign ss_extra_back[140:133] = 8'd0;
assign ss_extra_back[148:141] = 8'd0;
assign ss_extra_back[156:149] = 8'd0;
assign ss_extra_back[164:157] = 8'd0;
assign ss_extra_back[172:165] = 8'd0;
assign ss_extra_back[180:173] = 8'd0;
assign ss_extra_back[188:181] = 8'd0;
assign ss_extra_back[196:189] = 8'd0;
assign ss_extra_back[204:197] = 8'd0;
assign ss_extra_back[212:205] = 8'd0;
assign ss_extra_back[220:213] = 8'd0;
assign ss_extra_back[228:221] = 8'd0;
assign ss_extra_back[236:229] = 8'd0;
assign ss_extra_back[244:237] = 8'd0;
assign ss_extra_back[252:245] = 8'd0;
assign ss_extra_back[260:253] = 8'd0;
assign ss_extra_back[268:261] = 8'd0;
assign ss_extra_back[276:269] = 8'd0;
assign ss_extra_back[284:277] = 8'd0;
assign ss_extra_back[292:285] = 8'd0;
assign ss_extra_back[300:293] = 8'd0;
assign ss_extra_back[308:301] = 8'd0;
assign ss_extra_back[316:309] = 8'd0;
assign ss_extra_back[324:317] = 8'd0;
assign ss_extra_back[332:325] = 8'd0;
assign ss_extra_back[340:333] = 8'd0;
assign ss_extra_back[348:341] = 8'd0;
assign ss_extra_back[356:349] = 8'd0;
assign ss_extra_back[364:357] = 8'd0;
assign ss_extra_back[372:365] = 8'd0;
assign ss_extra_back[380:373] = 8'd0;
assign ss_extra_back[388:381] = 8'd0;
assign ss_extra_back[396:389] = 8'd0;
assign ss_extra_back[404:397] = 8'd0;
assign ss_extra_back[412:405] = 8'd0;
assign ss_extra_back[420:413] = 8'd0;
assign ss_extra_back[428:421] = 8'd0;
assign ss_extra_back[436:429] = 8'd0;
assign ss_extra_back[444:437] = 8'd0;
assign ss_extra_back[452:445] = 8'd0;
assign ss_extra_back[460:453] = 8'd0;
assign ss_extra_back[468:461] = 8'd0;
assign ss_extra_back[476:469] = 8'd0;
assign ss_extra_back[484:477] = 8'd0;
assign ss_extra_back[492:485] = 8'd0;
assign ss_extra_back[500:493] = 8'd0;
assign ss_extra_back[508:501] = 8'd0;
assign ss_extra_back[516:509] = 8'd0;
assign ss_extra_back[524:517] = 8'd0;
assign ss_extra_back[532:525] = 8'd0;
assign ss_extra_back[540:533] = 8'd0;
assign ss_extra_back[548:541] = 8'd0;
assign ss_extra_back[556:549] = 8'd0;
assign ss_extra_back[564:557] = 8'd0;
assign ss_extra_back[572:565] = 8'd0;
assign ss_extra_back[580:573] = 8'd0;
assign ss_extra_back[588:581] = 8'd0;
assign ss_extra_back[596:589] = 8'd0;
assign ss_extra_back[604:597] = 8'd0;
assign ss_extra_back[612:605] = 8'd0;
assign ss_extra_back[620:613] = 8'd0;
assign ss_extra_back[628:621] = 8'd0;
assign ss_extra_back[636:629] = 8'd0;
assign ss_extra_back[644:637] = 8'd0;
assign ss_extra_back[652:645] = 8'd0;
assign ss_extra_back[660:653] = 8'd0;
assign ss_extra_back[668:661] = 8'd0;
assign ss_extra_back[676:669] = 8'd0;
assign ss_extra_back[684:677] = 8'd0;
assign ss_extra_back[692:685] = 8'd0;
assign ss_extra_back[700:693] = 8'd0;
assign ss_extra_back[708:701] = 8'd0;
assign ss_extra_back[716:709] = 8'd0;
assign ss_extra_back[724:717] = 8'd0;
assign ss_extra_back[732:725] = 8'd0;
assign ss_extra_back[740:733] = 8'd0;
assign ss_extra_back[748:741] = 8'd0;
assign ss_extra_back[756:749] = 8'd0;
assign ss_extra_back[764:757] = 8'd0;
assign ss_extra_back[772:765] = 8'd0;
assign ss_extra_back[780:773] = 8'd0;
assign ss_extra_back[788:781] = 8'd0;
assign ss_extra_back[796:789] = 8'd0;
assign ss_extra_back[804:797] = 8'd0;
assign ss_extra_back[812:805] = 8'd0;
assign ss_extra_back[820:813] = 8'd0;
assign ss_extra_back[828:821] = 8'd0;
assign ss_extra_back[836:829] = 8'd0;
assign ss_extra_back[844:837] = 8'd0;
assign ss_extra_back[852:845] = 8'd0;
assign ss_extra_back[860:853] = 8'd0;
assign ss_extra_back[868:861] = 8'd0;
assign ss_extra_back[876:869] = 8'd0;
assign ss_extra_back[884:877] = 8'd0;
assign ss_extra_back[892:885] = 8'd0;
assign ss_extra_back[900:893] = 8'd0;
assign ss_extra_back[908:901] = 8'd0;
assign ss_extra_back[916:909] = 8'd0;
assign ss_extra_back[924:917] = 8'd0;
assign ss_extra_back[932:925] = 8'd0;
assign ss_extra_back[940:933] = 8'd0;
assign ss_extra_back[948:941] = 8'd0;
assign ss_extra_back[956:949] = 8'd0;
assign ss_extra_back[964:957] = 8'd0;
assign ss_extra_back[972:965] = 8'd0;
assign ss_extra_back[980:973] = 8'd0;
assign ss_extra_back[988:981] = 8'd0;
assign ss_extra_back[996:989] = 8'd0;
assign ss_extra_back[1004:997] = 8'd0;
assign ss_extra_back[1012:1005] = 8'd0;
assign ss_extra_back[1020:1013] = 8'd0;
assign ss_extra_back[1028:1021] = 8'd0;
assign ss_extra_back[1036:1029] = 8'd0;
assign ss_extra_back[1044:1037] = 8'd0;
assign ss_extra_back[1052:1045] = 8'd0;
assign ss_extra_back[1060:1053] = 8'd0;
assign ss_extra_back[1068:1061] = 8'd0;
assign ss_extra_back[1076:1069] = 8'd0;
assign ss_extra_back[1084:1077] = 8'd0;
assign ss_extra_back[1092:1085] = 8'd0;
assign ss_extra_back[1100:1093] = 8'd0;
assign ss_extra_back[1108:1101] = 8'd0;
assign ss_extra_back[1116:1109] = 8'd0;
assign ss_extra_back[1124:1117] = 8'd0;
assign ss_extra_back[1132:1125] = 8'd0;
assign ss_extra_back[1140:1133] = 8'd0;
assign ss_extra_back[1148:1141] = 8'd0;
assign ss_extra_back[1156:1149] = 8'd0;
assign ss_extra_back[1159:1157] = skip_cnt;
assign ss_extra_back[1167:1160] = pcnt;
assign ss_extra_back[1168] = skip_done;
assign ss_extra_back[1169] = vcnt_eol_l;
assign ss_extra_back[1170] = vcnt_reset;
assign ss_extra_back[1178:1171] = v_cnt;
assign ss_extra_back[1179] = vsync;
assign ss_extra_back[1186:1180] = h_cnt;
assign ss_extra_back[1188:1187] = h_div_cnt;
assign ss_extra_back[1189] = window_match;
assign ss_extra_back[1190] = window_ena_d;
assign ss_extra_back[1195:1191] = win_col;
assign ss_extra_back[1203:1196] = win_line;
assign ss_extra_back[1204] = wy_match;
assign ss_extra_back[1207:1205] = bg_fetch_cycle;
assign ss_extra_back[1210:1208] = sprite_fetch_cycle;
assign ss_extra_back[1218:1211] = bg_tile;
assign ss_extra_back[1226:1219] = bg_tile_attr_new;
assign ss_extra_back[1234:1227] = bg_tile_data0;
assign ss_extra_back[1242:1235] = bg_tile_data1;
assign ss_extra_back[1250:1243] = spr_tile_shift_0;
assign ss_extra_back[1258:1251] = spr_tile_shift_1;
assign ss_extra_back[1266:1259] = spr_pal_shift;
assign ss_extra_back[1274:1267] = spr_prio_shift;
assign ss_extra_back[1282:1275] = spr_cgb_pal_shift[0];
assign ss_extra_back[1290:1283] = spr_cgb_pal_shift[1];
assign ss_extra_back[1298:1291] = spr_cgb_pal_shift[2];
assign ss_extra_back[1306:1299] = spr_cgb_index_shift[0];
assign ss_extra_back[1314:1307] = spr_cgb_index_shift[1];
assign ss_extra_back[1322:1315] = spr_cgb_index_shift[2];
assign ss_extra_back[1330:1323] = spr_cgb_index_shift[3];
assign ss_extra_back[1338:1331] = spr_tile_data0;
assign ss_extra_back[1346:1339] = tile_shift_0;
assign ss_extra_back[1354:1347] = tile_shift_1;
assign ss_extra_back[1358:1355] = bg_shift_cnt;
assign ss_extra_back[1366:1359] = bg_tile_attr;
assign ss_extra_back[1367] = lcd_clk_out;
assign ss_extra_back[1382:1368] = 0;
assign ss_extra_back[1384:1383] = 0;
wire [63:0] ss_word_0;
eReg_SavestateV #(0, 60, 63, 0, 64'h0) ss_reg_0 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_0, ss_extra_back[63:0], ss_extra[63:0]);
wire [63:0] ss_word_1;
eReg_SavestateV #(0, 61, 63, 0, 64'h0) ss_reg_1 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_1, ss_extra_back[127:64], ss_extra[127:64]);
wire [63:0] ss_word_2;
eReg_SavestateV #(0, 62, 63, 0, 64'h0) ss_reg_2 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_2, ss_extra_back[191:128], ss_extra[191:128]);
wire [63:0] ss_word_3;
eReg_SavestateV #(0, 63, 63, 0, 64'h0) ss_reg_3 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_3, ss_extra_back[255:192], ss_extra[255:192]);
wire [63:0] ss_word_4;
eReg_SavestateV #(0, 64, 63, 0, 64'h0) ss_reg_4 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_4, ss_extra_back[319:256], ss_extra[319:256]);
wire [63:0] ss_word_5;
eReg_SavestateV #(0, 65, 63, 0, 64'h0) ss_reg_5 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_5, ss_extra_back[383:320], ss_extra[383:320]);
wire [63:0] ss_word_6;
eReg_SavestateV #(0, 66, 63, 0, 64'h0) ss_reg_6 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_6, ss_extra_back[447:384], ss_extra[447:384]);
wire [63:0] ss_word_7;
eReg_SavestateV #(0, 67, 63, 0, 64'h0) ss_reg_7 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_7, ss_extra_back[511:448], ss_extra[511:448]);
wire [63:0] ss_word_8;
eReg_SavestateV #(0, 68, 63, 0, 64'h0) ss_reg_8 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_8, ss_extra_back[575:512], ss_extra[575:512]);
wire [63:0] ss_word_9;
eReg_SavestateV #(0, 69, 63, 0, 64'h0) ss_reg_9 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_9, ss_extra_back[639:576], ss_extra[639:576]);
wire [63:0] ss_word_10;
eReg_SavestateV #(0, 70, 63, 0, 64'h0) ss_reg_10 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_10, ss_extra_back[703:640], ss_extra[703:640]);
wire [63:0] ss_word_11;
eReg_SavestateV #(0, 71, 63, 0, 64'h0) ss_reg_11 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_11, ss_extra_back[767:704], ss_extra[767:704]);
wire [63:0] ss_word_12;
eReg_SavestateV #(0, 72, 63, 0, 64'h0) ss_reg_12 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_12, ss_extra_back[831:768], ss_extra[831:768]);
wire [63:0] ss_word_13;
eReg_SavestateV #(0, 73, 63, 0, 64'h0) ss_reg_13 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_13, ss_extra_back[895:832], ss_extra[895:832]);
wire [63:0] ss_word_14;
eReg_SavestateV #(0, 74, 63, 0, 64'h0) ss_reg_14 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_14, ss_extra_back[959:896], ss_extra[959:896]);
wire [63:0] ss_word_15;
eReg_SavestateV #(0, 75, 63, 0, 64'h0) ss_reg_15 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_15, ss_extra_back[1023:960], ss_extra[1023:960]);
wire [63:0] ss_word_16;
eReg_SavestateV #(0, 76, 63, 0, 64'h0) ss_reg_16 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_16, ss_extra_back[1087:1024], ss_extra[1087:1024]);
wire [63:0] ss_word_17;
eReg_SavestateV #(0, 77, 63, 0, 64'h0) ss_reg_17 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_17, ss_extra_back[1151:1088], ss_extra[1151:1088]);
wire [63:0] ss_word_18;
eReg_SavestateV #(0, 78, 63, 0, 64'h0) ss_reg_18 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_18, ss_extra_back[1215:1152], ss_extra[1215:1152]);
wire [63:0] ss_word_19;
eReg_SavestateV #(0, 79, 63, 0, 64'h0) ss_reg_19 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_19, ss_extra_back[1279:1216], ss_extra[1279:1216]);
wire [63:0] ss_word_20;
eReg_SavestateV #(0, 80, 63, 0, 64'h0) ss_reg_20 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_20, ss_extra_back[1343:1280], ss_extra[1343:1280]);
wire [63:0] ss_word_21;
eReg_SavestateV #(0, 81, 63, 0, 64'h0) ss_reg_21 (clk, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, ss_word_21, ss_extra_back[1407:1344], ss_extra[1407:1344]);
wire [2047:0] ss_read_words = {640'd0, ss_extra_back};
wire [4:0] ss_read_index = SaveStateBus_Adr - 60;
assign ss_extra_dout = (SaveStateBus_Adr >= 60 && SaveStateBus_Adr < 82) ? ss_read_words[ss_read_index*64 +: 64] : 64'd0;


wire [7:0] bg_cpu_q,ob_cpu_q;
wire [15:0] bg_pixel_q,ob_pixel_q;
wire [63:0] bg_state_q,ob_state_q;
wire [31:0] bg_colors,ob_colors;
wire [511:0] bg_debug,ob_debug;
wire palette_write = !reset && ce_cpu && cpu_sel_reg && cpu_wr && isGBC && isGBC_mode && vram_cpu_allow;
wire [7:0] palette_cpu_q;
assign bg_cpu_q=palette_cpu_q;assign ob_cpu_q=palette_cpu_q;
assign ob_state_q=64'd0;
snapshot_palettes_ram #(.BASE(60),.OFFSET(133)) palettes(
 .clk(clk),.cpu_we(palette_write && (cpu_addr==8'h69 || cpu_addr==8'h6b)),
 .cpu_bank(cpu_addr==8'h6b),.cpu_addr(cpu_addr==8'h6b ? obpi : bgpi),.cpu_data(cpu_di),
 .bg_pixel_addr(palette_index),.ob_pixel_addr(sprite_palette_index),.cpu_q(palette_cpu_q),
 .bg_pixel_q(bg_pixel_q),.ob_pixel_q(ob_pixel_q),.ss_addr(SaveStateBus_Adr),
 .ss_we(SaveStateBus_wren && !SaveStateBus_rst),.ss_data(SaveStateBus_Din),.ss_q(bg_state_q),
 .bg_colors(bg_colors),.ob_colors(ob_colors),.debug_bytes({ob_debug,bg_debug}));
for(genvar i=0;i<64;i=i+1)begin: palette_debug
 assign bgpd[i]=bg_debug[i*8+:8];assign obpd[i]=ob_debug[i*8+:8];
end

endmodule
