LIBRARY ieee;
USE ieee.std_logic_1164.all;

use ieee.numeric_std.all;

ENTITY dpram IS
	GENERIC
	(
		init_file			: string := " ";
		widthad_a			: natural;
		width_a				: natural := 8;
		mixed_port_rdwr : string := "DONT_CARE";
    outdata_reg_a : string := "UNREGISTERED";
    outdata_reg_b : string := "UNREGISTERED"
	);
	PORT
	(
		address_a		: IN STD_LOGIC_VECTOR (widthad_a-1 DOWNTO 0);
		address_b		: IN STD_LOGIC_VECTOR (widthad_a-1 DOWNTO 0);
		clock_a		: IN STD_LOGIC ;
		clock_b		: IN STD_LOGIC ;
		data_a		: IN STD_LOGIC_VECTOR (width_a-1 DOWNTO 0);
		data_b		: IN STD_LOGIC_VECTOR (width_a-1 DOWNTO 0);
		wren_a		: IN STD_LOGIC  := '1';
		wren_b		: IN STD_LOGIC  := '1';
		q_a		: OUT STD_LOGIC_VECTOR (width_a-1 DOWNTO 0);
		q_b		: OUT STD_LOGIC_VECTOR (width_a-1 DOWNTO 0)
	);
END dpram;


architecture prototype of dpram is
type mem_t is array (0 to 2**widthad_a-1) of std_logic_vector(width_a-1 downto 0);
signal mem : mem_t := (others => (others => '0'));
begin
process(clock_a) begin
 if rising_edge(clock_a) then
  q_a <= mem(to_integer(unsigned(address_a)));
  q_b <= mem(to_integer(unsigned(address_b)));
  if wren_a='1' then mem(to_integer(unsigned(address_a))) <= data_a; q_a <= data_a; end if;
  if wren_b='1' then mem(to_integer(unsigned(address_b))) <= data_b; q_b <= data_b; end if;
 end if;
end process;
end;
