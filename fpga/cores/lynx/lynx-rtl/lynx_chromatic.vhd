-- Chromatic: LynxTop with only std_logic(_vector) ports for the Verilog board
-- code. Fast forward, turbo, FPS overlay, ComLynx, cheats and rewind are tied
-- off; savestate slot 0 is used (lynx_state_engine.sv).
library IEEE;
use IEEE.std_logic_1164.all;
use IEEE.numeric_std.all;

entity lynx_chromatic is
   port
   (
      clk            : in  std_logic;
      ce_tick        : in  std_logic;
      lock60         : in  std_logic;
      reset_in       : in  std_logic;
      pause_in       : in  std_logic;
      rom_addr       : out std_logic_vector(19 downto 0);
      rom_byte       : in  std_logic_vector(7 downto 0);
      rom_req        : out std_logic;
      rom_ack        : in  std_logic;
      hdr_bank0size  : in  std_logic_vector(15 downto 0);
      hdr_hasHeader  : in  std_logic;
      hdr_pc         : in  std_logic_vector(15 downto 0);
      hdr_pc_use     : in  std_logic;
      hdr_ram_addr   : in  std_logic_vector(15 downto 0);
      hdr_ram_data   : in  std_logic_vector(7 downto 0);
      hdr_ram_wren   : in  std_logic;
      bios_wraddr    : in  std_logic_vector(8 downto 0);
      bios_wrdata    : in  std_logic_vector(7 downto 0);
      bios_wr        : in  std_logic;
      pen_we         : out std_logic;
      pen            : out std_logic_vector(3 downto 0);
      pen_x          : out std_logic_vector(7 downto 0);
      pen_y          : out std_logic_vector(6 downto 0);
      palette        : out std_logic_vector(191 downto 0);
      audio_l        : out std_logic_vector(15 downto 0);
      audio_r        : out std_logic_vector(15 downto 0);
      JoyUP          : in  std_logic;
      JoyDown        : in  std_logic;
      JoyLeft        : in  std_logic;
      JoyRight       : in  std_logic;
      Option1        : in  std_logic;
      Option2        : in  std_logic;
      KeyB           : in  std_logic;
      KeyA           : in  std_logic;
      KeyPause       : in  std_logic;
      -- MiSTer savestates (slot 0): request edges, status and the 64-bit
      -- memory bus (32-bit word addresses, two per entry)
      save_state     : in  std_logic;
      load_state     : in  std_logic;
      state_loaded   : out std_logic;
      state_busy     : out std_logic;
      ss_din         : out std_logic_vector(63 downto 0);
      ss_dout        : in  std_logic_vector(63 downto 0);
      ss_adr         : out std_logic_vector(25 downto 0);
      ss_rnw         : out std_logic;
      ss_ena         : out std_logic;
      ss_done        : in  std_logic
   );
end entity;

architecture arch of lynx_chromatic is
   signal pixel_addr : integer range 0 to 16319;
   signal pixel_data : std_logic_vector(11 downto 0);
   signal pen_x_i    : integer range 0 to 159;
   signal pen_y_i    : integer range 0 to 101;
begin
   pen_x <= std_logic_vector(to_unsigned(pen_x_i, 8));
   pen_y <= std_logic_vector(to_unsigned(pen_y_i, 7));

   ilynx : entity work.LynxTop
   port map
   (
      clk                   => clk,
      ce_tick               => ce_tick,
      lock60                => lock60,
      reset_in              => reset_in,
      pause_in              => pause_in,
      rom_addr              => rom_addr,
      rom_byte              => rom_byte,
      rom_req               => rom_req,
      rom_ack               => rom_ack,
      romsize               => (others => '0'),
      romwrite_data         => (others => '0'),
      romwrite_addr         => (others => '0'),
      romwrite_wren         => '0',
      hdr_bank0size         => hdr_bank0size,
      hdr_hasHeader         => hdr_hasHeader,
      hdr_custom_PCAddr     => hdr_pc,
      hdr_custom_PCuse      => hdr_pc_use,
      hdr_ram_addr          => hdr_ram_addr,
      hdr_ram_data          => hdr_ram_data,
      hdr_ram_wren          => hdr_ram_wren,
      bios_wraddr           => bios_wraddr,
      bios_wrdata           => bios_wrdata,
      bios_wr               => bios_wr,
      pixel_out_addr        => pixel_addr,
      pixel_out_data        => pixel_data,
      pixel_out_we          => pen_we,
      pen_out               => pen,
      pen_x                 => pen_x_i,
      pen_y                 => pen_y_i,
      palette_out           => palette,
      audio_l               => audio_l,
      audio_r               => audio_r,
      fastforward           => '0',
      turbo                 => '0',
      speedselect           => "00",
      fpsoverlay_on         => '0',
      JoyUP                 => JoyUP,
      JoyDown               => JoyDown,
      JoyLeft               => JoyLeft,
      JoyRight              => JoyRight,
      Option1               => Option1,
      Option2               => Option2,
      KeyB                  => KeyB,
      KeyA                  => KeyA,
      KeyPause              => KeyPause,
      comm_rx               => '1',
      comm_tx               => open,
      increaseSSHeaderCount => '1',
      save_state            => save_state,
      load_state            => load_state,
      savestate_number      => 0,
      state_loaded          => state_loaded,
      savestate_busy_out    => state_busy,
      SAVE_out_Din          => ss_din,
      SAVE_out_Dout         => ss_dout,
      SAVE_out_Adr          => ss_adr,
      SAVE_out_rnw          => ss_rnw,
      SAVE_out_ena          => ss_ena,
      SAVE_out_be           => open,
      SAVE_out_done         => ss_done,
      rewind_on             => '0',
      rewind_active         => '0',
      cheat_clear           => '0',
      cheats_enabled        => '0',
      cheat_on              => '0',
      cheat_in              => (others => '0'),
      cheats_active         => open
   );
end architecture;
