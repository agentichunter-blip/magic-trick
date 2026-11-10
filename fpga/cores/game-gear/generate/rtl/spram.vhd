LIBRARY ieee;
USE ieee.std_logic_1164.all;

use ieee.numeric_std.all;

ENTITY spram IS
	GENERIC
	(
		init_file			: string := "none";
		widthad_a			: natural;
		width_a				: natural := 8;
    outdata_reg_a : string := "UNREGISTERED"
	);
	PORT
	(
		address		: IN STD_LOGIC_VECTOR (widthad_a-1 DOWNTO 0);
		clock		: IN STD_LOGIC ;
		data		: IN STD_LOGIC_VECTOR (width_a-1 DOWNTO 0);
		wren		: IN STD_LOGIC ;
		q		: OUT STD_LOGIC_VECTOR (width_a-1 DOWNTO 0)
	);
END spram;


architecture prototype of spram is
type mem_t is array (0 to 2**widthad_a-1) of std_logic_vector(width_a-1 downto 0);
signal mem : mem_t := (others => (others => '0'));
begin
process(clock) begin
 if rising_edge(clock) then
  q <= mem(to_integer(unsigned(address)));
  if wren='1' then mem(to_integer(unsigned(address))) <= data; q <= data; end if;
 end if;
end process;
end;
