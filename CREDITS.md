# Credits and source provenance

Magic Trick is a modification of existing work. The neutral name used for this repository's import commit identifies the packaging of this source snapshot; it does not claim authorship of the upstream code.

## ChroMagic and Chromatic

- **CursedToast** — creator of [ChroMagic](https://github.com/cursedtoast2/ChroMagic), the firmware this mod builds on. The starting point is ChroMagic 1.0.1, commit [`e611a1e`](https://github.com/cursedtoast2/ChroMagic/commit/e611a1ef36eb9aa93f706d836f5b0e46129d7137). Consult that repository for the original development history and contributor attribution.
- **ModRetro** — original Chromatic firmware and FPGA design on which ChroMagic is based.
- **CursedToast** — also creator of [ChroMagician](https://github.com/cursedtoast2/ChroMagician). The Magic Trick installer bundles ChroMagician 1.0.3's programmer tools on Windows and Linux, and installs the unmodified ChroMagic 1.0.1 FPGA image downloaded from the ChroMagic releases.
- **Magic Trick (this repository)** — artwork and gameplay-preview browsing, Recent folders, last-game startup, the Game Genie selector/compiler, related navigation/loading UI, media conversion tools, the desktop installer, the project website, and associated fixes/tests described in the README. It is an independent modification, not an official ChroMagic or ModRetro release.

## FPGA, emulation, and boot ROM work

The upstream acknowledgments in [NOTICE](NOTICE) are retained:

- [Gameboy_MiSTer contributors](https://github.com/MiSTer-devel/Gameboy_MiSTer), whose core is included in the FPGA source.
- [Gameboy for MiST contributors](https://github.com/mist-devel/gameboy), from which the MiSTer core was ported.
- [SameBoy contributors](https://github.com/LIJI32/SameBoy/), whose boot ROM sources were adapted upstream; their MIT notices remain in the relevant source files.
- **rayjt9**, credited upstream for boot ROM palette improvements.
- **GOWIN Semiconductor**, acknowledged in the upstream FPGA notices.

Additional authors are identified in the source headers. Those notices are preserved; this list is not a replacement for them or an exhaustive list of contributors.

## Dependencies and visual assets

- **Espressif ESP-IDF** and **LVGL** provide the firmware framework and UI library. Build dependencies retain their own licenses and notices.
- The desktop installer bundles **[esptool](https://github.com/espressif/esptool)** (Espressif), **[openFPGALoader](https://github.com/trabucayre/openFPGALoader)** (Gwenhael Goavec-Merou and contributors), **[libusb](https://libusb.info/)**, **Python** and **PyInstaller** output. Their licenses ship in the installer's `programmer/licenses` folder; corresponding sources are linked from each release.
- Included Game Boy / Game Boy Color wordmark sources and provenance are documented in [the system logo credits](mcu/tools/platform-logos/README.md). Nintendo trademarks remain Nintendo's.
- **libretro-database contributors** provide the optional external cheat database used by the pack compiler. See [cheat preparation and attribution](docs/cheats.md). No database or generated cheat pack is bundled.
- No third-party cover or gameplay-video pack is included in this repository.

## Licenses and provenance

See [mcu/LICENSE](mcu/LICENSE), [fpga/LICENSE](fpga/LICENSE), and individual source notices. This mod does not replace upstream license terms or remove upstream attribution.

[SOURCE_SNAPSHOT.json](SOURCE_SNAPSHOT.json) records the upstream component revisions. This repository starts with a clean source import rather than reproducing the upstream Git history; the original ChroMagic repository linked above remains the reference for that history.
