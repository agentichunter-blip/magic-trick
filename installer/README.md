# Magic Trick Installer — experimental preview

Free desktop upgrade assistant for the Magic Trick artwork/gameplay-preview mod of ChroMagic. Requires an existing ChroMagic 1.0.1/art-series MCU 0.13.4 and FPGA 18.38, with a working `fwversion` console command. Older bases without that command are rejected. This is not a stock-to-ChroMagic installer.

Extract the entire archive. Windows: open Magic-Trick-Installer.exe in its folder. macOS: open the .app. Linux: run Magic-Trick-Installer from its folder (Ubuntu 22.04+ x64 target). Select the device port, confirm your SD backup, and choose a private backup destination. Linux serial access may require distribution-specific device permissions. Close other USB applications first.

Downloads the pinned art31 MCU from GitHub, checks SHA-256, queries firmware versions, reads and verifies a full 4 MiB backup, validates the partition table, writes only the 1 MiB application at 0x10000, verifies flash and checks running versions. No ROMs, saves, media, FPGA or bootloader are distributed or written. Backups remain local and private.

Recovery: https://github.com/agentichunter-blip/chromagic-preview-mod/blob/main/docs/install-release.md
Keep backups. Never erase flash. If verification after flashing fails, power-cycle and inspect Status before retrying. Use upstream ChroMagician for stock/upstream restoration on supported hosts. The backup is not an SD or FPGA backup.

These initial packages are unsigned and not Apple-notarized. OS security prompts may appear. No end-to-end installer flash has been tested on physical hardware; automated tests cover safety gates, not all USB/driver conditions. The art31 firmware remains experimental. The marketing name is Magic Trick; device Status still reports the original art31 identifiers.

Source and credits: https://github.com/agentichunter-blip/chromagic-preview-mod
Based on CursedToast's ChroMagic and ModRetro firmware. Independent, not affiliated with ModRetro or Nintendo. Installer source is GPL-3.0; see included LICENSE.txt. esptool and other bundled dependencies retain their respective licenses.

Optional support: https://ko-fi.com/ryansag
