library IEEE;
use IEEE.std_logic_1164.all;  
use IEEE.numeric_std.all;     

use work.pBus_savestates.all;

entity gb_savestates is
   port 
   (
      clk                     : in     std_logic;  
      reset_in                : in     std_logic;
      reset_out               : out    std_logic := '0';
            
      load_done               : out    std_logic := '0';
            
      increaseSSHeaderCount   : in     std_logic; 
      save                    : in     std_logic;  
      load                    : in     std_logic;
      savestate_address       : in     integer;
      savestate_busy          : out    std_logic;
      
      cart_ram_size           : in     std_logic_vector(7 downto 0);
      
      lcd_vsync               : in     std_logic;
            
      BUS_Din                 : out    std_logic_vector(BUS_buswidth-1 downto 0) := (others => '0');
      BUS_Adr                 : buffer std_logic_vector(BUS_busadr-1 downto 0) := (others => '0');
      BUS_wren                : out    std_logic := '0';
      BUS_rst                 : out    std_logic := '0';
      BUS_Dout                : in     std_logic_vector(BUS_buswidth-1 downto 0);
            
      loading_savestate       : out    std_logic := '0';
      saving_savestate        : out    std_logic := '0';
      sleep_savestate         : out    std_logic := '0';
      clock_ena_in            : in     std_logic;
            
      CRAM_request : out std_logic := '0';
      CRAM_done : in std_logic;
      Save_RAMAddr            : buffer std_logic_vector(19 downto 0) := (others => '0');
      Save_RAMWrEn            : out    std_logic_vector(4 downto 0) := (others => '0');
      Save_RAMWriteData       : out    std_logic_vector(7 downto 0) := (others => '0');
      Save_RAMReadData_WRAM   : in     std_logic_vector(7 downto 0);
      Save_RAMReadData_VRAM   : in     std_logic_vector(7 downto 0);
      Save_RAMReadData_ORAM   : in     std_logic_vector(7 downto 0);
      Save_RAMReadData_ZRAM   : in     std_logic_vector(7 downto 0);
      Save_RAMReadData_CRAM   : in     std_logic_vector(7 downto 0);
      
      bus_out_Din             : out    std_logic_vector(63 downto 0) := (others => '0');
      bus_out_Dout            : in     std_logic_vector(63 downto 0);
      bus_out_Adr             : buffer std_logic_vector(25 downto 0) := (others => '0');
      bus_out_rnw             : out    std_logic := '0';
      bus_out_ena             : out    std_logic := '0';
      bus_out_be              : out    std_logic_vector(7 downto 0) := (others => '0');
      bus_out_done            : in     std_logic
   );
end entity;

architecture arch of gb_savestates is

   -- Native slot address increment: base is DWORD 0x110000, aligned to
   -- 64K DWORDs. The maximum supported payload (128 KiB cartridge RAM plus
   -- core RAM/registers/header) is below 256 KiB, so no valid transfer carries
   -- out of the low 16 address bits. Upper bits are loaded from the slot base.
   constant STATESIZE      : integer := 16#B0CE#;
   
   constant SETTLECOUNT    : integer := 100;
   constant HEADERCOUNT    : integer := 2;
   constant INTERNALSCOUNT : integer := 128; -- not all used, room for some more
   
   constant SAVETYPESCOUNT : integer := 5;
   signal savetype_counter : integer range 0 to SAVETYPESCOUNT;
   -- Fixed section sizes must be explicit logic, not initial values of
   -- undriven elements in an otherwise writable array (Gowin EX1998).
   signal cart_save_bytes : integer range 0 to 131072 := 131072;

   type tstate is
   (
      IDLE,
      SAVE_WAITVSYNC,
      SAVE_WAITSETTLE,
      SAVEINTERNALS_WAIT,
      SAVEINTERNALS_WRITE,
      SAVEMEMORY_NEXT,
      SAVEMEMORY_FIRST,
      SAVEMEMORY_READ,
      SAVEMEMORY_WRITE,
      SAVESIZEAMOUNT,
      LOAD_WAITSETTLE,
      LOAD_HEADERAMOUNTCHECK,
      LOADINTERNALS_READ,
      LOADINTERNALS_WRITE,
      LOADMEMORY_NEXT,
      LOADMEMORY_READ,
      LOADMEMORY_WRITE,
      SAVECRAM_WAIT, LOADCRAM_WAIT, LOADMEMORY_ADVANCE
   );
   signal state : tstate := IDLE;
   
   -- Internal count uses BUS_Adr; RAM count is always a multiple of eight.
   signal count               : integer range 0 to 131072 := 0;
   signal maxcount            : integer range 0 to 131072;
               
   signal settle              : integer range 0 to SETTLECOUNT := 0;
   
   signal bytecounter         : integer range 0 to 7 := 0;
   signal Save_RAMReadData    : std_logic_vector(7 downto 0);
   signal RAMAddrNext         : std_logic_vector(19 downto 0) := (others => '0');
   
   constant header_amount     : unsigned(31 downto 0) := (others => '0');

begin 

   savestate_busy <= '0' when state = IDLE else '1';
   
   Save_RAMReadData <= Save_RAMReadData_WRAM when savetype_counter = 0 else
                       Save_RAMReadData_VRAM when savetype_counter = 1 else
                       Save_RAMReadData_ORAM when savetype_counter = 2 else
                       Save_RAMReadData_ZRAM when savetype_counter = 3 else
                       Save_RAMReadData_CRAM;

   process (clk)
   begin
      if rising_edge(clk) then
   
         CRAM_request <= '0';
         Save_RAMWrEn <= (others => '0');
         bus_out_ena   <= '0';
         BUS_wren      <= '0';
         BUS_rst       <= '0';
         reset_out     <= '0';
         load_done     <= '0';
         
         bus_out_be    <= x"FF";
         
         case (cart_ram_size) is
            when x"00"  => cart_save_bytes <=    512; -- for MBC2
            when x"01"  => cart_save_bytes <=   2048; -- 2   KByte
            when x"02"  => cart_save_bytes <=   8192; -- 8   KByte
            when x"03"  => cart_save_bytes <=  32768; -- 32  KByte
            when x"05" => cart_save_bytes <= 65536;
            when others => cart_save_bytes <= 131072; -- 128 KByte 
         end case;
   
         case state is
         
            when IDLE =>
               savetype_counter <= 0;
               if (reset_in = '1') then
                  reset_out <= '1';
				  BUS_rst   <= '1';
               elsif (save = '1') then
                  state                <= SAVE_WAITVSYNC;
                  null;
               elsif (load = '1') then
                  state                <= LOAD_WAITSETTLE;
                  settle               <= 0;
                  sleep_savestate      <= '1';
               end if;
               
            -- #################
            -- SAVE
            -- #################
            
            when SAVE_WAITVSYNC =>
               if (lcd_vsync = '1') then
                  state                <= SAVE_WAITSETTLE;
                  settle               <= 0;
                  sleep_savestate      <= '1';
               end if;
            
            when SAVE_WAITSETTLE =>
               if (clock_ena_in = '1') then
                  settle <= 0;
               elsif (settle < SETTLECOUNT) then
                  settle <= settle + 1;
               else
                  state            <= SAVEINTERNALS_WAIT;
                  bus_out_Adr      <= std_logic_vector(to_unsigned(savestate_address + HEADERCOUNT, 26));
                  bus_out_rnw      <= '0';
                  BUS_adr          <= (others => '0');

                  saving_savestate <= '1';
               end if;            
            
            when SAVEINTERNALS_WAIT =>

               bus_out_ena    <= '1';
               state          <= SAVEINTERNALS_WRITE;
            
            when SAVEINTERNALS_WRITE => 
               if (bus_out_done = '1') then
                  bus_out_Adr <= bus_out_Adr(25 downto 16) & std_logic_vector(unsigned(bus_out_Adr(15 downto 0)) + 2);
                  if (unsigned(BUS_Adr) < INTERNALSCOUNT-1) then
                     state       <= SAVEINTERNALS_WAIT;

                     BUS_adr     <= std_logic_vector(unsigned(BUS_adr) + 1);
                  else 
                     state       <= SAVEMEMORY_NEXT;
                     count       <= 8;
                  end if;
               end if;
            
            when SAVEMEMORY_NEXT =>
               if (savetype_counter = 4 and cart_ram_size = x"FF") then
                  savetype_counter <= savetype_counter + 1;
               elsif (savetype_counter < SAVETYPESCOUNT) then
                  state        <= SAVEMEMORY_FIRST;
                  bytecounter  <= 0;
                  count        <= 8;
                  case savetype_counter is
                     when 0 => maxcount <= 32768;
                     when 1 => maxcount <= 16384;
                     when 2 => maxcount <= 160;
                     when 3 => maxcount <= 128;
                     when others => maxcount <= cart_save_bytes;
                  end case;
                  Save_RAMAddr <= (others => '0');
               else
                  state          <= SAVESIZEAMOUNT;
                  bus_out_Adr    <= std_logic_vector(to_unsigned(savestate_address, 26));

                  bus_out_ena    <= '1';
                  if (increaseSSHeaderCount = '0') then
                     bus_out_be  <= x"F0";
                  end if;
               end if;
               
            when SAVEMEMORY_FIRST =>
               if savetype_counter = 4 then
                  CRAM_request <= '1';
                  state <= SAVECRAM_WAIT;
               else
                  state <= SAVEMEMORY_READ;
                  Save_RAMAddr <= std_logic_vector(unsigned(Save_RAMAddr) + 1);
               end if;         
            
            when SAVECRAM_WAIT =>
               if CRAM_done = '1' then
                  Save_RAMAddr <= std_logic_vector(unsigned(Save_RAMAddr) + 1);
                  if bytecounter < 7 then
                     bytecounter <= bytecounter + 1;
                     state <= SAVEMEMORY_FIRST;
                  else
                     state <= SAVEMEMORY_WRITE;
                     bus_out_ena <= '1';
                  end if;
               end if;
            when SAVEMEMORY_READ =>
               if (bytecounter < 7) then
                  bytecounter    <= bytecounter + 1;
                  Save_RAMAddr   <= std_logic_vector(unsigned(Save_RAMAddr) + 1);
               else
                  state          <= SAVEMEMORY_WRITE;
                  bus_out_ena    <= '1';
               end if;
               
            when SAVEMEMORY_WRITE =>
               if (bus_out_done = '1') then
                  bus_out_Adr <= bus_out_Adr(25 downto 16) & std_logic_vector(unsigned(bus_out_Adr(15 downto 0)) + 2);
                  if (count < maxcount) then
                     state        <= SAVEMEMORY_FIRST;
                     bytecounter  <= 0;
                     count        <= count + 8;
                  else 
                     savetype_counter <= savetype_counter + 1;
                     state            <= SAVEMEMORY_NEXT;
                  end if;
               end if;
            
            when SAVESIZEAMOUNT =>
               if (bus_out_done = '1') then
                  state            <= IDLE;
                  saving_savestate <= '0';
                  sleep_savestate  <= '0';
               end if;
            
            
            -- #################
            -- LOAD
            -- #################
            
            when LOAD_WAITSETTLE =>
               if (clock_ena_in = '1') then
                  settle <= 0;
               elsif (settle < SETTLECOUNT) then
                  settle <= settle + 1;
               else
                  state                <= LOAD_HEADERAMOUNTCHECK;
                  bus_out_Adr          <= std_logic_vector(to_unsigned(savestate_address, 26));
                  bus_out_rnw          <= '1';
                  bus_out_ena          <= '1';
               end if;
               
            when LOAD_HEADERAMOUNTCHECK =>
               if (bus_out_done = '1') then
                  if (bus_out_Dout(63 downto 32) = std_logic_vector(to_unsigned(STATESIZE, 32))) then
                     null;
                     state                <= LOADINTERNALS_READ;
                     bus_out_Adr          <= std_logic_vector(to_unsigned(savestate_address + HEADERCOUNT, 26));
                     bus_out_ena          <= '1';
                     BUS_adr              <= (others => '0');

                     loading_savestate    <= '1';
                  else
                     state                <= IDLE;
                     sleep_savestate      <= '0';
                  end if;
               end if;
            
            when LOADINTERNALS_READ =>
               if (bus_out_done = '1') then
                  state           <= LOADINTERNALS_WRITE;
                  BUS_Din         <= bus_out_Dout;
                  BUS_wren        <= '1';
               end if;
            
            when LOADINTERNALS_WRITE => 
               bus_out_Adr <= bus_out_Adr(25 downto 16) & std_logic_vector(unsigned(bus_out_Adr(15 downto 0)) + 2);
               if (unsigned(BUS_Adr) < INTERNALSCOUNT-1) then
                  state          <= LOADINTERNALS_READ;

                  bus_out_ena    <= '1';
                  BUS_adr        <= std_logic_vector(unsigned(BUS_adr) + 1);
               else 
                  state              <= LOADMEMORY_NEXT;
                  count              <= 8;
               end if;
            
            when LOADMEMORY_NEXT =>
               if (savetype_counter = 4 and cart_ram_size = x"FF") then
                  savetype_counter <= savetype_counter + 1;
               elsif (savetype_counter < SAVETYPESCOUNT) then
                  state          <= LOADMEMORY_READ;
                  count          <= 8;
                  case savetype_counter is
                     when 0 => maxcount <= 32768;
                     when 1 => maxcount <= 16384;
                     when 2 => maxcount <= 160;
                     when 3 => maxcount <= 128;
                     when others => maxcount <= cart_save_bytes;
                  end case;
                  Save_RAMAddr   <= (others => '0');
                  RAMAddrNext    <= (others => '0');
                  bytecounter    <= 0;
                  bus_out_ena    <= '1';
               else
                  state             <= IDLE;
                  reset_out         <= '1';
                  loading_savestate <= '0';
                  sleep_savestate   <= '0';
                  load_done         <= '1';
               end if;
            
            when LOADMEMORY_READ =>
               if (bus_out_done = '1') then
                  state             <= LOADMEMORY_WRITE;
               end if;
               
            when LOADMEMORY_WRITE =>
               RAMAddrNext                    <= std_logic_vector(unsigned(RAMAddrNext) + 1);
               Save_RAMAddr                   <= RAMAddrNext;
               Save_RAMWrEn(savetype_counter) <= '1';
               Save_RAMWriteData              <= bus_out_Dout(bytecounter * 8 + 7 downto bytecounter * 8);
               if savetype_counter = 4 then
                  CRAM_request <= '1';
                  state <= LOADCRAM_WAIT;
               else
                  state <= LOADMEMORY_ADVANCE;
               end if;
            when LOADCRAM_WAIT =>
               if CRAM_done = '1' then state <= LOADMEMORY_ADVANCE; end if;
            when LOADMEMORY_ADVANCE =>
               if (bytecounter < 7) then
                  bytecounter       <= bytecounter + 1;
                  state <= LOADMEMORY_WRITE;
               else
                  bus_out_Adr  <= bus_out_Adr(25 downto 16) & std_logic_vector(unsigned(bus_out_Adr(15 downto 0)) + 2);
                  if (count < maxcount) then
                     state          <= LOADMEMORY_READ;
                     count          <= count + 8;
                     bytecounter    <= 0;
                     bus_out_ena    <= '1';
                  else 
                     savetype_counter <= savetype_counter + 1;
                     state            <= LOADMEMORY_NEXT;
                  end if;
               end if;
         
         end case;
         
      end if;
   end process;
   


   -- Shift-packed capture data. Only the complete word is issued to memory;
   -- after eight byte reads, the first byte is in the least-significant lane.
   process(clk)
   begin
      if rising_edge(clk) then
         if state = SAVEINTERNALS_WAIT then
            bus_out_Din <= BUS_Dout;
         elsif state = SAVEMEMORY_NEXT and savetype_counter = SAVETYPESCOUNT then
            bus_out_Din <= std_logic_vector(to_unsigned(STATESIZE,32)) & std_logic_vector(header_amount);
         elsif state = SAVEMEMORY_READ then
            bus_out_Din <= Save_RAMReadData & bus_out_Din(63 downto 8);
         elsif state = SAVECRAM_WAIT and CRAM_done = '1' then
            bus_out_Din <= Save_RAMReadData_CRAM & bus_out_Din(63 downto 8);
         end if;
      end if;
   end process;

end architecture;





