<p align="center">
  <img src="website/magic-trick/press/magic-trick-wordmark-1128x776.png" alt="Magic Trick" width="420">
</p>

<h3 align="center">Your Chromatic. More Magical.</h3>

<p align="center">
  <a href="https://ryansag.com/magic-trick/">Website</a> ·
  <a href="https://github.com/agentichunter-blip/magic-trick/releases/tag/v1.0.0-beta.8">Download</a> ·
  <a href="https://x.com/ryansagdotcom">X</a> ·
  <a href="https://www.youtube.com/@RyanSag">YouTube</a> ·
  <a href="https://ko-fi.com/ryansag">Support my work</a>
</p>

# Magic Trick

**Magic Trick** is a free, open-source game library mod for the ModRetro Chromatic, built on [CursedToast's ChroMagic](https://github.com/cursedtoast2/ChroMagic). It plays Game Boy, Game Boy Color and Game Gear games from the SD card and switches FPGA cores automatically. It turns the file list into a game library with cover art, looping gameplay previews, Game Genie cheats, a Recent folder and last-game startup, and adds native save states, Wi-Fi file transfers and a boot animation.

> **Standing on the shoulders of ChroMagic.** Magic Trick wouldn't exist without CursedToast's ChroMagic and ChroMagician, ModRetro's decision to open-source the Chromatic firmware and FPGA design, and the MiSTer Game Boy and SMS cores. Full credits are [below](#credits-and-licenses) and in [CREDITS.md](CREDITS.md).

**Version:** Magic Trick **1.0 beta**: MCU `v1.0.0-beta.5`, FPGA `50.1` (Game Boy / Game Boy Color) and `50.2` (Game Gear). Status reports "Magic Trick 1.0 beta".

This is an independent community project with no guaranteed support or release schedule. It isn't affiliated with ModRetro, Nintendo or Sega. No game ROMs, personal saves, cover packs or gameplay video packs are included. Supply your own games and media.

## Features

- **Three systems:** Game Boy, Game Boy Color and Game Gear (with sound) on FPGA cores. Opening a game for the other system reloads the right core automatically.
- **Native save states** for Game Boy, Game Boy Color and Game Gear: three slots per game in System → Save States, plus Menu+Select to save and Menu+Start to load the slot you used last; a shortcut shows "State saved" or "State loaded", then returns to the game. Each slot is written to the SD card next to the game (`<game>.state`, `.state2`, `.state3`), so you can load it again after turning the Chromatic off. Game Boy games with MBC1, MBC3 (including the clock in Pokémon Gold, Silver and Crystal) or MBC5 are supported, as are Game Gear games. Loading a state keeps the in-game save you had before as `<game>.SAV.before-load-0001.sav`.
- **Color temperature** for Game Boy Color and Game Gear games, ported from ModRetro's stock firmware 4.6. In color games the **Color Temp** tab takes the Palette tab's place and goes from Neutral to Warm 5. Like ModRetro's, it warms only bright areas such as white text and title screens, so dark scenes barely change. **Menu+Up** warms and **Menu+Down** cools during play; in Game Boy games the same keys cycle the palette, as on stock firmware. Controls → hot keys lists every shortcut.
- **In-game saves for Game Gear:** battery and EEPROM saves are kept on the SD card as `<game>.SAV` and loaded before the game starts, including games with 32 KiB of cartridge RAM such as Shining Force – The Sword of Hajya.
- **Game library:** cover art followed by an optional silent, looping gameplay preview, a five-row selector when holding Up/Down, accelerated navigation and scrolling titles.
- **Game Genie cheats** for Game Boy, Game Boy Color and Game Gear SD games. See [cheats](docs/cheats.md).
- **Recent games** and **last-game startup** (hold B at power-on to skip).
- **Wi-Fi file transfers** to and from the SD card in a web browser. Files already on the card are skipped, so you can drop a whole folder again after an interruption.
- **USB file transfers** with [ChroMagician](https://github.com/cursedtoast2/ChroMagician) (Windows and Linux): turn on **USB TRANSFERS** in the menu, with Wi-Fi off. It's the same switch ChroMagician calls **C. MAGICIAN**. USB uploads are slow (a few KB/s), so use Wi-Fi or a card reader for a big library.
- **Boot animation** with the Magic Trick logo.
- Files beginning with `[BIOS]` are hidden without being deleted. Demo and Kiosk ROMs are filtered out.

ChroMagic's cartridge backup and save features remain available. An installed SD card is required for the browser and on-device backups.

## Install

Download **[Magic Trick Installer 1.0.0-beta.8](https://github.com/agentichunter-blip/magic-trick/releases/tag/v1.0.0-beta.8)** for Mac, Windows or Linux. Firmware is bundled, so no internet connection is needed. It installs from stock 4.2/4.5/4.6, ChroMagic 1.0.1 or art-series firmware, updates an earlier 1.0 install, and can **uninstall** back to ChroMagic. It always makes and verifies a private MCU backup first. Read the [installer guide](installer/README.md) and the [recovery guide](docs/recovery.md) before you start. **Beta firmware, installed at your own risk.**

- **Updates take 10–15 minutes.** FPGA writes often need several attempts; the installer restarts the FPGA, retries and verifies every write on its own.
- **Going back:** ModRetro's updater and ChroMagician can't reflash the FPGA of a device running Magic Trick 1.0. Use **Uninstall** in the Magic Trick installer first. After that, ModRetro's updater works normally.
- **Beta:** hardware-tested on one Chromatic with the Mac app (install from stock 4.6 and ChroMagic, updates, uninstall, Restore backup, back to stock with ModRetro's updater, core switching). The Windows app (install, update, uninstall, Restore backup) and the Linux app (install, uninstall) were also tested on hardware.

### Build from source

- **MCU:** with ESP-IDF v5.3 activated, run `mcu/tools/build_wifi.sh`. It builds the app (`mcu_fw.bin`) for the Wi-Fi partition table in `mcu/partitions.wifi.csv` (application at `0x260000`) and runs the host tests. More in [docs/building.md](docs/building.md).
- **FPGA:** see [fpga/cores/README.md](fpga/cores/README.md). Both cores rebuild bit-for-bit from this repository with Gowin EDA V1.9.12.04.
- **Installing your own build:** the MCU app, both FPGA cores and the partition table have to go on together. Use the installer's code in [`installer/`](installer) as the reference sequence. Never run the multi-region `idf.py flash` command printed by the build: it would overwrite the bootloader.

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

At the system picker, Game Boy, Game Boy Color and Game Gear are stacked rows. Up/Down moves between them and A opens one. Up at the first row or B returns focus to the tabs. Roots with additional entries use the normal folder list.

In a folder, tapping Up/Down selects the next cover directly. Holding for 350 milliseconds opens the five-row selector and starts repeating; continuing to hold accelerates scrolling. The selector stays open after release, and taps fine-tune the highlighted game. Press **A** to reveal that game’s cover/video preview, then **A** again to launch. **B** goes back. Videos start after the cover has been unobscured for 1.5 seconds. Missing clips leave the cover visible. Press **Select** from the preview to open cheats.

ROMs explicitly tagged **Demo** or **Kiosk** are excluded from browsing, Recent, and last-game startup. Full-game titles such as Demolition Man are unaffected.

## Development and testing

See [building and testing](docs/building.md), [media preparation](docs/media.md), and [validation status](docs/validation.md). Firmware source lives in `mcu/`; upstream FPGA source is retained in `fpga/`. Converter tools live in `mcu/tools/`.

When reporting an issue, include your MCU/FPGA versions, steps to reproduce, and whether it occurs with previews removed. Do not attach ROMs, saves, private flash dumps, or full personal library inventories.

## Credits and licenses

Magic Trick is a modification of other people's work, and they deserve the credit for most of what runs on your Chromatic:

- **[CursedToast](https://github.com/cursedtoast2)**: creator of [ChroMagic](https://github.com/cursedtoast2/ChroMagic), the firmware Magic Trick is built on, and [ChroMagician](https://github.com/cursedtoast2/ChroMagician), whose installer tooling the Magic Trick installer bundles on Windows and Linux. Magic Trick starts from ChroMagic 1.0.1 and installs the ChroMagic 1.0.1 FPGA image unchanged. Please support the original project.
- **[ModRetro](https://modretro.com/)**: the Chromatic hardware, plus the original firmware and FPGA design they released as open source under GPL-3.0.
- **[Gameboy_MiSTer](https://github.com/MiSTer-devel/Gameboy_MiSTer)** and **[Gameboy for MiST](https://github.com/mist-devel/gameboy)** contributors: the Game Boy core inside the FPGA design.
- **[SameBoy](https://github.com/LIJI32/SameBoy/)** contributors: boot ROM sources adapted upstream (MIT).
- **rayjt9**: boot ROM palette improvements, credited upstream.
- **[GOWIN Semiconductor](https://www.gowinsemi.com/en/)**: acknowledged in the upstream FPGA notices.
- **[Espressif ESP-IDF and esptool](https://github.com/espressif)**, **[LVGL](https://lvgl.io/)**, **[openFPGALoader](https://github.com/trabucayre/openFPGALoader)**, **[libusb](https://libusb.info/)**, **Python** and **PyInstaller**: firmware framework, UI library, and the flashing tools bundled in the installer.
- **[libretro-database](https://github.com/libretro/libretro-database)** contributors: the optional external cheat data used by the cheat pack compiler. No database or generated pack is bundled.

Additional authors are named in the source file headers; those notices are preserved. See [CREDITS.md](CREDITS.md) for source provenance and exactly what Magic Trick adds. The repository's neutral commit author does not claim authorship of upstream work.

Magic Trick is released under the **GNU GPL v3.0**, the same license as the upstream firmware and FPGA design: see [LICENSE](LICENSE), [mcu/LICENSE](mcu/LICENSE), [fpga/LICENSE](fpga/LICENSE) and [NOTICE](NOTICE). Bundled tools keep their own licenses. `SOURCE_SNAPSHOT.json` records upstream component snapshots; the base ChroMagic commit is `e611a1ef36eb9aa93f706d836f5b0e46129d7137`.

Game Boy and Game Boy Color are trademarks of Nintendo. Game Gear is a trademark of Sega. Chromatic is a trademark of ModRetro. Magic Trick is not affiliated with or endorsed by any of them.

## Follow and support

Magic Trick is free. Development updates go out on [X](https://x.com/ryansagdotcom) and [YouTube](https://www.youtube.com/@RyanSag), and the project site is [ryansag.com/magic-trick](https://ryansag.com/magic-trick/). Tips on [Ko-fi](https://ko-fi.com/ryansag) are optional and help fund development. Bugs and feature requests go in [GitHub issues](https://github.com/agentichunter-blip/magic-trick/issues).
