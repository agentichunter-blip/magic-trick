# Credits and source provenance

Magic Trick is a modification of existing work. The neutral name used for this repository's import commit identifies the packaging of this source snapshot; it does not claim authorship of the upstream code.

## ChroMagic and Chromatic

- **CursedToast** — creator of [ChroMagic](https://github.com/cursedtoast2/ChroMagic), the firmware this mod builds on. The starting point is ChroMagic 1.0.1, commit [`e611a1e`](https://github.com/cursedtoast2/ChroMagic/commit/e611a1ef36eb9aa93f706d836f5b0e46129d7137). Consult that repository for the original development history and contributor attribution.
- **ModRetro** — original Chromatic firmware and FPGA design on which ChroMagic is based.
- **CursedToast** — also creator of [ChroMagician](https://github.com/cursedtoast2/ChroMagician). The Magic Trick installer bundles ChroMagician 1.0.3's programmer tools on Windows and Linux, and installs the unmodified ChroMagic 1.0.1 FPGA image downloaded from the ChroMagic releases.
- **Magic Trick (this repository)** — artwork and gameplay-preview browsing, Recent folders, last-game startup, the Game Genie selector/compiler, related navigation/loading UI, media conversion tools, the desktop installer, the project website, and associated fixes/tests described in the README. The three-system firmware adds Game Gear browsing and launching, automatic core switching, native save-state controls, Wi-Fi file transfers, the boot animation (made for this project and released with it under GPL-3.0), and the FPGA board integration in [`fpga/cores`](fpga/cores). It is an independent modification, not an official ChroMagic or ModRetro release.

## FPGA, emulation, and boot ROM work

The upstream acknowledgments in [NOTICE](NOTICE) are retained:

- [Gameboy_MiSTer contributors](https://github.com/MiSTer-devel/Gameboy_MiSTer), whose core is included in the FPGA source.
- [Gameboy for MiST contributors](https://github.com/mist-devel/gameboy), from which the MiSTer core was ported.
- [SameBoy contributors](https://github.com/LIJI32/SameBoy/), whose boot ROM sources were adapted upstream; their MIT notices remain in the relevant source files.
- **rayjt9**, credited upstream for boot ROM palette improvements.
- **GOWIN Semiconductor**, acknowledged in the upstream FPGA notices.

## Game Gear core and save states (`fpga/cores`)

- [SMS_MiSTer](https://github.com/MiSTer-devel/SMS_MiSTer) contributors, revision `66f16d5c656fd8906d62570da4d258875bcee684`, a MiSTer port of Ben's Papilio SMS core. It provides the Game Gear VDP, I/O, mappers and system logic. Upstream has no license file and most of its VHDL files carry no license header. It is included with credit, and its authors can ask for it to be removed.
- **T80** Z80 CPU: Daniel Wallner (2001–2002), with later work by MikeJ, TobiFlex, Sean Riddle and Sorgelig. BSD-style license; the notice required for synthesized forms is reproduced in [NOTICE](NOTICE).
- **[jt89](https://github.com/jotego/jt89)** SN76489 sound chip by Jose Tejada (jotego), GPL-3.0-or-later.
- `AudioMix.vhd` by Alastair M. Robinson. `MC8123.v` by blackwine. `SEGASYS1_PRGDEC.v` and `parts.v` by MiSTer-X. These are SMS_MiSTer files, included unchanged.
- Game Genie `CODES` engine by **Kitrinx**, also used by the Game Boy core.
- Native Game Boy save states build on the save-state engine from [Gameboy_MiSTer](https://github.com/MiSTer-devel/Gameboy_MiSTer) (`gb_savestates.vhd` and related files).
- **GHDL** and **Yosys** generated the Verilog netlists from the VHDL sources. The tools are not included; see [fpga/cores/README.md](fpga/cores/README.md).

Additional authors are identified in the source headers. Those notices are preserved; this list is not a replacement for them or an exhaustive list of contributors.

## Dependencies and visual assets

- **Espressif ESP-IDF** and **LVGL** provide the firmware framework and UI library. Build dependencies retain their own licenses and notices.
- The desktop installer bundles **[esptool](https://github.com/espressif/esptool)** (Espressif), **[openFPGALoader](https://github.com/trabucayre/openFPGALoader)** (Gwenhael Goavec-Merou and contributors), **[libusb](https://libusb.info/)**, **Python** and **PyInstaller** output. Their licenses ship in the installer's `programmer/licenses` folder; corresponding sources are linked from each release.
- **Unscii** by viznut (public domain): the pixel font in the installer window (`installer/assets/unscii-8.ttf`).
- The system picker and boot animation name Game Boy, Game Boy Color and Game Gear in plain text; no Nintendo or Sega logo artwork is included. Nintendo and Sega trademarks remain theirs.
- The Wi-Fi transfer page's pairing words come from the [EFF short wordlist](https://www.eff.org/files/2016/09/08/eff_short_wordlist_1.txt) (CC BY 4.0, Electronic Frontier Foundation). The Wi-Fi server uses ESP-IDF's HTTP server and cJSON (MIT).
- **libretro-database contributors** provide the optional external cheat database used by the pack compiler. See [cheat preparation and attribution](docs/cheats.md). No database or generated cheat pack is bundled.
- No third-party cover or gameplay-video pack is included in this repository.

## Licenses and provenance

See [mcu/LICENSE](mcu/LICENSE), [fpga/LICENSE](fpga/LICENSE), and individual source notices. This mod does not replace upstream license terms or remove upstream attribution.

[SOURCE_SNAPSHOT.json](SOURCE_SNAPSHOT.json) records the upstream component revisions. This repository starts with a clean source import rather than reproducing the upstream Git history; the original ChroMagic repository linked above remains the reference for that history.
