# ChroMagic — artwork and gameplay preview mod

An experimental modification of [CursedToast's ChroMagic](https://github.com/cursedtoast2/ChroMagic) for the ModRetro Chromatic. Browse Game Boy and Game Boy Color backups with cover art, looping gameplay previews, and a compact scrolling game list.

**Version:** Chromatic `1.0.1-art31 (4.2)` / MCU `v0.13.4-art31`. Based on ChroMagic 1.0.1; tested with FPGA **18.38**. This mod changes the MCU application only. There is no modified FPGA `.fs` file to install.

This is a personal, experimental project with no guaranteed support or release schedule. No game ROMs, personal saves, cover packs, or gameplay video packs are included. Supply your own games and media.

## Features

- Game Boy / Game Boy Color system picker and nested folder navigation.
- Cover art followed by an optional silent, looping gameplay preview.
- Five-row game overlay when holding Up/Down, accelerated navigation, and scrolling titles.
- Game title and progress indicator during loading.
- Background media reads, cancellation during navigation, and SD access coordination.
- Automatically reopen the last successfully launched SD game when no cartridge is inserted; hold B at power-on to skip.
- Files beginning with `[BIOS]` are hidden in the browser without being deleted.

ChroMagic's cartridge backup and save features remain available. An installed SD card is required for on-device backups and this browser.

## Install

These instructions are for a Chromatic already running **ChroMagic 1.0.1 with FPGA 18.38**. If starting from stock, first install upstream ChroMagic with [ChroMagician](https://github.com/cursedtoast2/ChroMagician), then verify the versions in the device's status menu. Selecting upstream ChroMagic in that app installs upstream firmware, not this mod. Upstream ChroMagician currently provides Windows and Linux installers; use a supported host for the initial installation or rollback. The source build and application-only flashing commands below can be run on macOS/Linux once the required base firmware is installed.

Download/extract this repository or clone it, then open a terminal in its root directory (the folder containing this README). All relative paths below start there. These commands use the esptool 4.x bundled with ESP-IDF v5.3; see [Espressif's command reference](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/esptool/basic-commands.html).

1. Back up your SD card, especially saves. Build the application using [the build guide](docs/building.md). The output is `mcu/build/mcu_fw.bin`.
2. Keep the device powered on and connect it using a data-capable USB cable. Close ChroMagician, serial monitors, and other applications using the device.
3. With ESP-IDF v5.3 activated, list serial ports and set `PORT` to your Chromatic's actual port. The shell commands below use macOS/Linux syntax; Windows users can run them in an ESP-IDF shell with the appropriate port/variable syntax.

   ```sh
   python -m serial.tools.list_ports
   PORT=/dev/cu.usbmodemYOUR_DEVICE
   ```

4. Save a private backup of the existing 4 MiB MCU flash. Choose a new filename if a backup already exists so you do not overwrite your original backup. Keep this outside the repository and do not share it. This backs up the MCU only, not the SD card or FPGA.

   ```sh
   python -m esptool --chip esp32 --port "$PORT" --baud 460800 \
     read_flash 0x0 0x400000 ../chromatic-mcu-backup.bin
   ```

5. Continue only after the backup finishes successfully and produces a 4,194,304-byte file. Inspect the application image, then write **only the application at `0x10000`**. Run from the repository root. Do not use `erase_flash` or the general `idf.py flash` command for this upgrade.

   ```sh
   python -m esptool --chip esp32 image_info --version 2 mcu/build/mcu_fw.bin
   ```

   Check that `image_info` reports project `mcu_fw`, app version `0.13.4-art31`, ESP-IDF `v5.3`, and a valid checksum/validation hash. Only then run:

   ```sh
   python -m esptool --chip esp32 --port "$PORT" --baud 460800 \
     --before default_reset --after hard_reset write_flash \
     --flash_mode dio --flash_freq 40m --flash_size 4MB \
     0x10000 mcu/build/mcu_fw.bin
   ```

6. Wait for `Hash of data verified.` before disconnecting. Power-cycle the device and confirm MCU `v0.13.4-art31`, Chromatic `1.0.1-art31 (4.2)`, and FPGA `18.38`. Leave **C. MAGICIAN** off for on-device browsing and gameplay.
7. Add optional [covers and gameplay previews](docs/media.md) to the SD card. Games remain in `CHROMAGIC/BACKUPS`. The browser works without media.

The application occupies a 1 MiB partition. These commands leave the existing bootloader, partition table, and FPGA in place. Compatibility with other firmware layouts or FPGA versions has not been established.

### Troubleshooting and rollback

- If the port cannot open, close other device apps and check the cable and USB drivers. Follow the USB setup guidance linked by upstream ChroMagician. Do not guess a different flash offset or erase the device to fix a connection issue.
- If artwork is missing, check that paths mirror the full game filename, including `.gb` or `.gbc`, as shown in the media guide.
- To return to upstream ChroMagic or stock firmware, use ChroMagician's firmware installation/restore workflow. Restoring upstream replaces this custom browser. Keep your SD/save backups until you have verified the restored setup.

## Game Genie cheats

Show an SD game’s preview in BACKUPS and press **Select**. If the selector is open, press **A** first to reveal the preview. Use Up/Down to browse,
**A** to toggle, and **B** to return. **Disable all** clears that game's options.
A gold lamp inside the green frame identifies available cheat packs. It uses the
lower-right black padding when available, or sits beside the title for wider
artwork and video. Media size stays unchanged. An outlined
lamp means options are available, and a filled lamp means options are selected
for the next launch. The cheat menu has a matching lamp-and-wordmark header.
Choices are remembered and take effect the next time the game starts, including
automatic startup. All options default off; the original ROM files are unchanged.

Generate `CHROMAGIC/CHEATS` data for your own games using the preparation tool.
No cheat database or prebuilt library pack is bundled in this repository. Unsupported games show a clear
no-cheats message. GameShark/Pro Action Replay and live toggling are not part of
this MCU-only release. See [coverage, installation and limitations](docs/cheats.md).

## Recent games

Inside GB and GBC, **Recent** appears above the alphabetical folders after you
successfully launch an SD game for that system. It keeps the last **20** games
per system, newest first, across power cycles. Replaying a game moves it to the
top without duplicates. Failed launches do not enter the history, and missing
files are skipped. History starts with games launched on art30 or later.

Recent uses the original game paths, so covers, video previews, cheats, saves,
and the B button's return navigation work as usual. It does not copy or move
SD files. History is stored in MCU settings, with no writes while browsing and
no repeated write when the newest game is launched again.

## Last-game startup

Cartridge detection runs as soon as the MCU transports are ready, before menu
setup and the startup delay. A detected cartridge is not probed again afterward.
This moves the existing core-resetting check earlier. Tetris cold-start testing
on the physical device confirmed the copyright-screen restart is gone.

After installing this build, launch a game from BACKUPS once to remember it.
On later startups, an inserted cartridge takes priority. With no cartridge, the
MCU checks live button state and loads the remembered SD game through the normal
loader. Hold **B while turning the device on** for five seconds, then release it
to open BACKUPS instead.
Cartridge detection cross-checks two read-only header samples when the presence
flag is asserted, because this device also asserts it with an empty slot.
Opening the menu manually or enabling C. MAGICIAN also prevents automatic loading.

A missing card/game, an unreadable record, or failed cartridge detection
falls back to the menu once the buttons are released. If live button telemetry
is unavailable, automatic loading is cancelled; use the physical Menu button. Only a successful load replaces the remembered path;
selecting a game or a failed launch does not. The path is stored in MCU settings,
so it survives power-off and does not add files to the SD card. Replaying the same
game does not rewrite the record. First boot without a record opens BACKUPS.

This reopens the game with its normal saved progress; it is not an exact-position
save state. Existing save behavior is unchanged. The FPGA/core is unchanged.

## Controls

At the two-system picker, Left/Right selects a system and A opens it. B or Up returns focus to the tabs; A or Down returns to the picker. Roots with additional entries use the normal folder list.

In a folder, tapping Up/Down selects the next cover directly. Holding for 350 milliseconds opens the five-row selector and starts repeating; continuing to hold accelerates scrolling. The selector stays open after release, and taps fine-tune the highlighted game. Press **A** to reveal that game’s cover/video preview, then **A** again to launch. **B** goes back. Videos start after the cover has been unobscured for 1.5 seconds. Missing clips leave the cover visible. Press **Select** from the preview to open cheats.

ROMs explicitly tagged **Demo** or **Kiosk** are excluded from browsing, Recent, and last-game startup. Full-game titles such as Demolition Man are unaffected.

## Development and testing

See [building and testing](docs/building.md), [media preparation](docs/media.md), and [validation status](docs/validation.md). Firmware source lives in `mcu/`; upstream FPGA source is retained in `fpga/`. Converter tools live in `mcu/tools/`.

When reporting an issue, include your MCU/FPGA versions, steps to reproduce, and whether it occurs with previews removed. Do not attach ROMs, saves, private flash dumps, or full personal library inventories.

## Credits and licenses

ChroMagic is by **[CursedToast](https://github.com/cursedtoast2/ChroMagic)**, based on **ModRetro's** firmware and FPGA design. This repository adds the artwork/gameplay browser modifications; it is not an official upstream release and is not affiliated with ModRetro or Nintendo.

See [CREDITS.md](CREDITS.md) for upstream authors, source provenance, and the scope of this mod. The clean import commit does not claim authorship of upstream work.

Existing licenses and notices are retained: [MCU license](mcu/LICENSE), [FPGA license](fpga/LICENSE), and [NOTICE](NOTICE). See [system logo credits](mcu/tools/platform-logos/README.md) for the included wordmarks. `SOURCE_SNAPSHOT.json` records upstream component snapshots; the mod's base commit is `e611a1ef36eb9aa93f706d836f5b0e46129d7137`.
