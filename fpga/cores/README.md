# Magic Trick FPGA cores (three systems, MB1)

Magic Trick ships two FPGA images that share the Chromatic's 4 MiB FPGA configuration flash. The MCU switches between them when you open a game for the other system.

| Flash offset | Core | Directory | Boots next |
|---|---|---|---|
| `0x000000` | Game Boy / Game Boy Color, with native save states | [`gb-gbc/`](gb-gbc) | `0x100000` |
| `0x100000` | Game Gear | [`game-gear/`](game-gear) | `0x000000` |

A real power-on always boots slot 0. A core switch asks the running image to reconfigure the FPGA, and the next image is the one named by the running bitstream's static `-multiboot_spi_flash_address`. On this device the SAMB primitive's dynamic address does **not** steer the reload, so each core must keep its static next-boot address. With two cores, a reload always means "the other core". A blank or invalid target falls back to slot 0.

## Layout of this directory

Each core directory contains only the files that differ from the shared upstream ChroMagic tree in [`../esp32t/src`](../esp32t/src). Its `sources.tcl` and `build.tcl` reference unchanged files there by relative path (`../../esp32t/src/...`). This keeps the vendor-encrypted Gowin IP files in a single place, as published upstream.

**`gb-gbc/`**
- `src/`: changed board and emulator files. Highlights: core reload (`core_reload.sv`), USB capture removed (CDC serial kept), save-state plumbing, and an OSD line-0 prefetch fix in `mm_burst_read_to_stream.v`.
- `savestate/rtl/`: snapshot memory and control used by native save states.
- `src/rtl/EMU/capture_engine_lowered.v`: a Verilog netlist generated from `src/Gameboy_MiSTer/rtl/gb_savestates.vhd` and `../esp32t/src/rtl/EMU/CORE/bus_savestates.vhd` by `generate/lower_capture.py` (GHDL `--synth`, then Yosys `proc; opt; fsm`). The VHDL is the source of truth.

**`game-gear/`**
- `gg_system.v` and `generated/video.v`: Verilog netlists generated from the SMS_MiSTer VHDL in `vendor/SMS_MiSTer/rtl` by `generate/build_sim.py`, using GHDL with the adapters in `generate/rtl`. The Game Gear cheat hook was then added by `generate/integrate_gg_cheats.py`, and the PSG clock and cartridge RAM size were changed by hand as recorded in `generate/psg-clock-and-cart-ram.patch`. Source-location comments in the netlists point at the corresponding VHDL file; line numbers refer to GHDL's preprocessed copies.
- `gg_rom_bridge.sv`: 8 KiB direct-mapped ROM cache in front of PSRAM. A one-line cache stalled the Z80 about 25,000 times a second and garbled CPU-timed audio.
- `gg_audio_lpf.sv`: multiplier-free low-pass filter and DC blocker.
- `src/rtl/BSP/ST7785_panel_master.v`: a 12,288-pixel line ring. The Game Gear bridge emits rows faster than the panel reads them.
- `board-rtl/`: Game Gear board bridges (video, audio, clock enables, launch status, async FIFO).
- `generate/prepare_cores.py` and `generate/usb_serial_only.py`: the scripts that derived both core trees from the development platforms (multiboot options, reload logic, USB-serial-only build). They are kept as a record of how the trees were produced. You don't need them to build.

SMS_MiSTer's arcade ROM decryptors (`MC8123.v` by blackwine, `SEGASYS1_PRGDEC.v` and `parts.v` by MiSTer-X) are instantiated by the upstream system design and are included unchanged, although Game Gear cartridges never enable them.

## Building

The released images were built with Gowin EDA **V1.9.12.04**. You need your own Gowin license. From a core directory:

```sh
gw_sh build.tcl
```

The bitstream lands in `impl/pnr/`. Write the GB/GBC image at flash offset `0x000000` and the Game Gear image at `0x100000`. The MCU firmware for these cores is in [`../../mcu`](../../mcu). FPGA and MCU must be installed as a matched pair.

## Status

- **Known issue:** while one of these cores sits in flash slot 0, the first USB/JTAG flash write after a reboot fails. That holds for openFPGALoader, ModRetro's updater, ChroMagician and Gowin's programmer. The cause is `-multi_boot 1`, which core switching needs. Re-running the write succeeds: on hardware it took 2 to 5 attempts, and every completed write verified. Tools that don't retry (ModRetro's updater, ChroMagician) will fail, so the Magic Trick installer must handle installs, upgrades and uninstalls with retries.

- **GB/GBC:** closes timing with no setup or hold violations.
- **Game Gear:** 26 setup endpoints in the `xclk` domain miss timing by up to 0.26 ns. All are T80 registers that only update on the CPU clock enable (about 1 cycle in 19), and the core runs correctly on hardware. Treat it as beta.
- Game Gear has no save states yet.

## Credits

- Game Gear core: [SMS_MiSTer](https://github.com/MiSTer-devel/SMS_MiSTer) (revision `66f16d5c656fd8906d62570da4d258875bcee684`), a MiSTer port of Ben's Papilio SMS core. Upstream has no license file and most of its VHDL files carry no license header. Credit belongs to its authors, and the authors can ask for it to be removed.
- [T80](https://opencores.org/projects/t80) CPU by Daniel Wallner, with fixes by MikeJ, TobiFlex and Sorgelig (BSD-style license; see the file headers).
- [jt89](https://github.com/jotego/jt89) SN76489 PSG by Jose Tejada (jotego), GPL-3.0-or-later.
- `AudioMix.vhd` by Alastair M. Robinson.
- Game Genie `CODES` engine by Kitrinx.
- Game Boy core, save states and board design: see [CREDITS.md](../../CREDITS.md).
