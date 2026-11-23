# Magic Trick Installer 1.0.0-beta.15

Installs Magic Trick firmware 1.0 beta (MCU 1.0.0-beta.10). New in installer 1.0.0-beta.15: on a Mac, installing or updating the FPGA cores works on macOS 11 and later (Intel Macs: macOS 10.15 and later); the FPGA programmer bundled in 1.0.0-beta.13 and 1.0.0-beta.14 needed macOS 15. This changes only the installer app, not the firmware. Since 1.0.0-beta.14: fixed the CARTRIDGE section stopping at 97% (READING CART) after the first cart read since the app was opened; the backup was saved, but the cart's games never appeared (fix by ryanfoxeth, PR #8; thanks!). Cart reads over USB now ask again for any part that arrives damaged (on some Windows PCs the USB link drops a byte now and then, which stopped a read with "stopped answering over USB" or "USB transfer failed"), and when a cart operation stops, the message now names the first problem rather than a later one. This changes only the installer app, not the firmware: a Chromatic already on 1.0.0-beta.13 has nothing new to install. Since 1.0.0-beta.13: **game carts**. The new **CARTRIDGE** section puts Game Gear, Atari Lynx and Game Boy games on a rewritable Game Boy cart (a ModRetro cart or most flash carts), with box art, and the Chromatic plays them from **Games > Cartridge** with or without an SD card. The cart's own game can be swapped for a Game Boy or Game Boy Color game of your choice, so an original Game Boy plays it too, and on the Chromatic the cart's own game now has save states (see [Game carts](#game-carts)). Without an SD card, a cart's games keep their saves and save states in the Chromatic's internal memory. The boot animation is shorter (about 2.5 s), and SERIAL # fits inside the System menu again. This updates only the MCU firmware, so an update from 1.0.0-beta.12 takes about 5 minutes. Since 1.0.0-beta.12: a cartridge no longer starts twice at power-on (flash carts such as the EverDrive GB X7 could hang); it now starts once, after the boot animation, so its music no longer plays underneath. Game Gear buttons match a real Game Gear: B is button 1 and A is button 2 (they were mirrored). Game Gear games that keep the screen dark for a while at start, such as Battletoads, no longer fail when picked from a Game Boy game. A new **START-UP** setting (System tab) chooses what a power-on without a cartridge does: LAST GAME (as before) or GAMES MENU. Long folder names such as Game Boy Color fit in the game list. The **SD card library** can now **remove games** (see [SD card library](#sd-card-library)). On Windows, when the FPGA programmer fails the installer now says why. This updates the Game Boy and Game Gear FPGA cores, so an update from 1.0.0-beta.11 takes 15 to 20 minutes. Since 1.0.0-beta.11: Atari Lynx games made to be played with the Lynx held sideways (Centipede, Klax, Gauntlet: The Third Encounter, Lexis, NFL Football, Raiden) turn upright on their own, centred on the screen, with the D-pad turned so Up moves up. This updates the Atari Lynx FPGA core, so an update from 1.0.0-beta.10 takes about 12 minutes. Since 1.0.0-beta.10: **Atari Lynx** games (`.lnx`, `.lyx`, `.o`) on a third FPGA core, with save states, a 60 Hz frame lock and no Lynx BIOS needed. Wi-Fi Files and USB TRANSFERS aren't available while a Lynx game is loaded: play a Game Boy game first. The **SD card library** adds games, box art and gameplay clips to your SD card over a card reader, Wi-Fi or USB (see [SD card library](#sd-card-library)). Wi-Fi Files has about 10 KB more memory, so big folders no longer stall it or drop the Chromatic off the network, and SD folders list in one pass (a 400-game folder took 25 s over USB, now under a second). Fixed: each Wi-Fi Files session used to lose about 6 KB of memory, so a fourth session without a restart could fail to start, and after Wi-Fi Files box art could stop showing until the Chromatic was restarted. This writes all three FPGA cores, so an update takes about 20 minutes. Since 1.0.0-beta.9: hardware support from ModRetro's FPGA 18.9–18.14 in both cores. The firmware detects whether a Chromatic has the original or the revised circuit board. On every board the charger is now configured as in ModRetro's firmware (1024 mA, watchdog off; until now it ran on its factory defaults), the white charging light follows the charger's own status, and a Chromatic switched off on USB power stays dark. On revised boards the cartridge is powered up and down in sequence, battery levels and warnings use that board's values, and charging slows when the battery is cold. Also from ModRetro: support for an alternative PSRAM chip and audio codec and LCD setup fixes. This updates both FPGA cores, so an update takes about 15 minutes. Since 1.0.0-beta.8: color temperature for Game Boy Color and Game Gear games, ported from ModRetro's stock firmware 4.6. In color games a **Color Temp** tab replaces the Palette tab (Neutral to Warm 5; like ModRetro's, it warms bright areas such as white text). **The save-state shortcuts moved:** Menu+Select saves and Menu+Start loads (no hold), and a shortcut shows **State saved** or **State loaded** before returning to the game. Menu+Up/Down now changes color temperature during play, or cycles the palette in Game Boy games, as on stock firmware. Controls → hot keys lists every shortcut, as in ModRetro's firmware. This updates both FPGA cores, so an update takes about 15 minutes. Since 1.0.0-beta.7: Game Gear in-game saves are kept on the SD card (before, they were lost when the Chromatic was turned off); Game Gear games have save states, with the same three slots, SD card files and shortcuts as Game Boy games; Shining Force – The Sword of Hajya and other games with 32 KiB of cartridge RAM now run; and Game Gear games get a Recent list. Fixed: after loading a Game Boy save state, later in-game saves went to a separate file that the next start didn't read, so the game seemed to lose its progress (see [Lost in-game progress after loading a state](#lost-in-game-progress-after-loading-a-state)). Also fixed: Load in the Save States menu closes the menu, and the Game Gear picture no longer smears while a state saves or loads. This updates the Game Gear FPGA core, so an update takes about 15 minutes. Since 1.0.0-beta.6: save states are kept on the SD card, so they survive turning the Chromatic off; each game has three save-state slots; and MBC3 games such as Pokémon Gold, Silver and Crystal can use save states, clock included. Since 1.0.0-beta.5: the firmware reports its FPGA version with USB TRANSFERS on (on a Mac the installer's final check used to fail in that mode), and an update skips FPGA cores that are already the bundled version. Since 1.0.0-beta.4: ChroMagician recognises Magic Trick, so its SD file manager works over USB; Wi-Fi uploads skip files already on the SD card, retry a failed file and keep going; and the Wi-Fi Files page has the Magic Trick look. Since 1.0.0-beta.3: tested on Windows and Linux, a clearer retro-style app, an **ALMOST DONE** ending with **CHECK AGAIN** when the final check can't reach the firmware, and Linux USB permission instructions. Since 1.0.0-beta.1: **Restore backup** for recovery, a stop when the USB programmer stops responding, and the Chromatic is found whatever its player number, or when its MCU can't start.

> **Beta firmware, installed at your own risk.** Magic Trick is an independent community mod, not made, endorsed or supported by ModRetro. It rewrites your Chromatic's MCU and FPGA firmware and may void your warranty. Read the [recovery guide](../docs/recovery.md) before you install.

One app installs Magic Trick 1.0 beta on a ModRetro Chromatic, updates an earlier Magic Trick 1.0 install, or uninstalls it. Python, esptool, openFPGALoader and all firmware are bundled. No internet connection is needed.

**What you get:** Game Boy, Game Boy Color, Game Gear and Atari Lynx on one device, switching cores automatically. Cover art and gameplay previews, Game Genie cheats, Recent games, last-game startup, native save states for all four systems (three slots per game, kept on the SD card), Game Gear in-game saves on the SD card, Wi-Fi file transfers, game carts that play without an SD card, and the Magic Trick boot animation.

## Before you start

- Back up your SD card yourself, especially saves.
- After your first install, copy its backup folder somewhere safe. Its `mcu-backup.bin` is your way back to how the device was (see [recovery](../docs/recovery.md)).
- Use a data-capable USB cable. Power the Chromatic on and close other device apps (ModRetro's updater, ChroMagician, serial monitors).
- **Windows:** you may need ModRetro's MRUpdater USB driver setup before the programmer can reach the device. **Linux:** allow USB access once first (see [Linux: USB permissions](#linux-usb-permissions)). The app checks access before it writes anything.
- Supported starting points: stock 4.2, 4.5 and 4.6, ChroMagic 1.0.1 and art-series firmware, or Magic Trick 1.0 beta. Anything else is refused before anything is written.

## Linux: USB permissions

Most Linux systems don't let a normal user open the Chromatic's serial port or its FPGA programmer. Without access, the installer stops before writing anything. To allow it once, run these in a terminal (sudo asks for your password):

```bash
sudo tee /etc/udev/rules.d/70-modretro-chromatic.rules <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="33aa", ATTR{idProduct}=="0120", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="374e", MODE="0660", TAG+="uaccess"
EOF
sudo udevadm control --reload && sudo udevadm trigger
```

Then unplug the Chromatic and plug it back in. You don't need to log out, reboot or join a group. The rule gives the person signed in at the computer access to the Chromatic's FPGA programmer (USB ID 33aa:0120) and its serial port (USB vendor 374e).

## Install or update

Extract the whole archive and open Magic Trick Installer. **Mac:** open the .app. **Windows:** run Magic-Trick-Installer.exe from its folder. **Linux:** run Magic-Trick-Installer from its folder (Ubuntu 22.04+ x64). Select the Chromatic, confirm your SD backup, choose a private backup folder, then choose **Back up & install / update**.

The installer:

1. Saves and verifies a full 4 MiB backup of the MCU flash in your folder. Keep it private: it contains your device's settings.
2. Writes and verifies the Magic Trick MCU application in an unused area. Until step 5 your old application still starts.
3. Writes and verifies the Game Gear FPGA core, then the Game Boy / Game Boy Color core. Updating an existing Magic Trick 1.0 install? The installer first puts the ChroMagic FPGA in the Game Boy slot so the Game Gear slot can be rewritten safely. It keeps a small record of the cores it wrote (in an unused MCU flash sector at 0x25F000), so a later update skips a core that is already the bundled version. An update that only changes the MCU firmware skips the FPGA writes and takes about 10 minutes instead of 15; most of that is the MCU backup.
4. FPGA writes sometimes fail a few times before they go through. That's expected: the installer restarts the FPGA and retries, up to 12 times at about 80 seconds per attempt. Every completed write is read back and verified.
5. Switches the MCU partition table to the new layout, last.
6. Restarts the Chromatic and checks the running versions.

It never writes the SD card or the bootloader, and never erases the whole flash. When it says it's done, turn the Chromatic off and on once. Status shows **Magic Trick 1.0 beta** and MCU **v1.0.0-beta.9**.

## Lost in-game progress after loading a state

Firmware before 1.0.0-beta.4 (installer 1.0.0-beta.7) wrote a Game Boy game's in-game saves to a separate file after you loaded a save state, and the next start read the old save instead. Your progress isn't lost: it's in the newest `<game>.SAV.state-NNNN.sav` next to the game on the SD card. After updating, copy your current `<game>.SAV` somewhere safe, then rename that newest `.state-NNNN.sav` file to `<game>.SAV`. Updated firmware keeps saving to `<game>.SAV`, and keeps the save you had before a state load as `<game>.SAV.before-load-0001.sav`.

## Uninstall

Choose **Uninstall…**. The installer writes the ChroMagic 1.0.1 FPGA and restores the standard partition table, so the application you had before Magic Trick 1.0 runs again. It's kept untouched at 0x10000. To return fully to stock afterwards, use ModRetro's official updater.

ModRetro's updater and ChroMagician can't reflash the FPGA of a device running Magic Trick 1.0: their FPGA writes fail and they don't retry. Uninstall with this app first.

## Restore backup

Choose **Restore backup…** and pick `mcu-backup.bin` from your **first** install. It works whatever state the device is in, even with no USB serial port: it writes the ChroMagic 1.0.1 FPGA first, then puts back your saved MCU flash. Use it when install, update or uninstall can't finish. Afterwards ModRetro's updater works normally.

## SD card library

**SD card library** adds games, box art and gameplay clips to your SD card, and removes games you no longer want. Everything goes in the right folder with the right name, and nothing on the card is deleted unless you use **Remove games**. It doesn't change the firmware, so no backup is needed.

**1. Choose how to reach your SD card**

| | Speed | What to do |
|---|---|---|
| **Card reader** | Fastest | Take the SD card out of the Chromatic, put it in your computer and choose it. |
| **Wi-Fi** | About 40 KB/s | On the Chromatic open **Settings > Wi-Fi Files**. Type the address it shows, then press A and type the 6-number WEB CODE. |
| **USB** | Very slow, about 3 KB/s | On the Chromatic turn Wi-Fi off and **USB TRANSFERS** on. One box art takes about 8 s, one game about 5 min and one clip about 12 min, so use it for a few files only. |

**2. Choose what to do**

- **Add games:** pick game files (`.gb`, `.gbc`, `.gg`, `.lnx`, `.lyx`), zip files with games inside, or a folder of them. You don't need to sort them first: the installer puts each game in its system's folder for you, Game Boy games in the Game Boy folder (`BACKUPS/GB`), Game Gear games in the Game Gear folder (`BACKUPS/GG`), and so on. If that folder is split into A–Z folders (like `0-F`, `G-L`, `M-R`, `S-Z`), it picks the right one of those too. A game that's already on the card is skipped. Game Boy Color-only games go to `GBC` even when they end in `.gb`.
- **Get box art:** press **START**. It finds box art for every game on the card that has none, by file name or by checking the game's contents, so renamed games are found too. Homebrew and rare games often have no box art. Box art comes from [ScreenScraper](https://www.screenscraper.fr), through the [MiSTer Artwork Pack](https://github.com/chipster6502/MiSTer_artwork_pack) by chipster6502. It's downloaded to your computer when you ask; the installer doesn't include any.
- **Add gameplay clips:** pick video clips 10 seconds or longer, or a folder of them. Name each clip like its game file (`Tetris (World).mp4`) or just its title (`Tetris.mp4`, used for every version of that game). See **Getting gameplay clips** below. The installer includes the video converter (FFmpeg), so there's nothing else to install.
- **Remove games:** press **CHOOSE**. The installer reads the card and lists its games; type to filter, click the games to delete, then press **DELETE** and confirm. Each game's box art and gameplay clip go with it. Its saves and save states stay on the card, so if you add the game again later it carries on where you left off. Deleting can't be undone. Over Wi-Fi this needs the Magic Trick firmware that came with this installer.

**Getting gameplay clips.** Most people use [EmuMovies](https://emumovies.com), which has gameplay clips for thousands of games. Magic Trick can't include them, so you download them yourself (the app's **HOW DO I GET CLIPS?** link shows the same steps):

1. Make an account at emumovies.com.
2. Find the **Video Snaps (SQ)** pack for each system you play: Game Boy, Game Boy Color, Game Gear or Atari Lynx.
3. Download every part of the pack (files ending in `.part1.rar`, `.part2.rar` and so on) into one folder.
4. Open the first part to unzip it. Windows 11 can do this by itself; on a Mac, use a free app such as The Unarchiver.
5. In the installer, press **FOLDER** next to **ADD GAMEPLAY CLIPS** and pick the unzipped folder.

The clips are already named like the games, so they match by themselves; clips for games that aren't on your card are skipped.

Box art and clips already on the card are kept. Tick **Replace box art and clips already there** to replace them (card reader and USB only; Wi-Fi can't replace files).

If it stops partway, nothing is lost: run it again and it carries on, skipping what's already there.

From the command line: `app.py --library games|covers|previews TARGET [FILES…] [--replace]`, where `TARGET` is the card's folder, `wifi:ADDRESS:CODE` or `usb:PORT`. `app.py --library list TARGET` prints the games on the card, and `app.py --library remove TARGET PATH…` deletes the ones named (paths as `list` prints them).

## Game carts

**CARTRIDGE** turns a rewritable Game Boy cart into a multi-game cart for the Chromatic. ModRetro carts (MR.TETRIS has 2 MB) and most flash carts with AMD-style flash chips work; the 4 MB Codex cart should work but hasn't been tested yet. The Chromatic lists the cart's games under **Games > Cartridge**, with their box art, and plays them **without an SD card**. The only limit on how many games fit is the cart's size.

**Connect.** Put the cart in and switch the Chromatic on. It reaches the cart only from its Game Boy side: the cart's own Game Boy game or the menu. If the cart starts a Game Gear or Lynx game by itself (a Magic Trick cart with one game), switch off and on holding **B** to stay in the menu. Then either turn Wi-Fi off and **USB TRANSFERS** on (System tab) and plug in a USB cable, or open **Settings > Wi-Fi Files** for Wi-Fi (faster: a 2 MB cart reads in about 80 s instead of about 110 s). The installer picks USB when a Chromatic is plugged in.

**1. READ CART.** The whole cart, and its save memory, are copied to your computer first (`Documents/Magic Trick/Cartridge backups`, a `.bin` and a `.sav`). **RESTORE A CART BACKUP** (or **RESTORE BACKUP…** once a cart is read) puts one back exactly, save included. **BACKUP FOLDER** opens the folder; the files are for the installer, not to be opened.

**2. Choose what kind of cart.**

- **KEEP IT:** the cart's own game stays first (it still plays on any Game Boy), and your games go in its free space.
- **OWN GAME:** put a Game Boy or Game Boy Color game of your choice first, instead of the cart's game, and an original Game Boy or Game Boy Color plays it (not tested on original hardware yet). The cart acts like an MBC5 cartridge, the chip most Game Boy Color games use; the installer warns when a game was made for another chip (MBC1 games may not run, MBC2 and special chips won't, an MBC3 game's clock won't work) or saves more than the cart's 32 KB of save memory. **BLANK SAVE** wipes the old game's save (it's in your backup). You can still add more games for the Chromatic.
- **MAGIC TRICK CART:** only games for the Chromatic, behind a title card. With one game, the Chromatic starts straight into it.

**3. Add games** (`.gb`, `.gbc`, `.gg`, `.lnx`, `.lyx`, or zips with them). The first bar shows how full the cart is; the second shows how much of the Chromatic's internal memory the games' saves and save states could need when there's no SD card (about 1.2 MB).

**4. Box art.** **FIND ART** looks up box art for released games (the same catalogue as the SD card library), including the cart's own game. Homebrew isn't in it: select the game and use **PICK ART…** with any picture. Gameplay clips can't go on a cart.

**5. WRITE TO CART.** Only the parts that change are written and every block is checked. Writing is slow, about 4 to 5 KB/s: about 5 minutes for 1 MB.

**On the Chromatic.** At power-on the cart's own game starts by itself, straight from the cart, like on any Game Boy. Choosing it in **Games > Cartridge** instead copies it into the Chromatic's memory first (about 7 s for 256 KB, 30 s for 1 MB), so it has save states. Its in-game save always stays on the cart: it's copied in when the game loads and written back when you leave it or at the next power-on. Game Gear and Lynx games are always copied in. With an SD card in, the cart's games keep their saves and states on the card (`CHROMAGIC/CART`); without one, in the Chromatic's internal memory. A cart with no Magic Trick games, such as a stock MR.TETRIS, also shows its game in **Games > Cartridge**, so a Chromatic without an SD card has something to play and save states for it.

**Good to know.**

- Switching off within a few seconds of an in-game save can lose that save, as with SD games: the power switch cuts the power at once.
- Saves and states made with and without an SD card are kept apart; a game finds the ones from where it's playing now.
- Lynx games can't use Wi-Fi Files or USB TRANSFERS while loaded: start the cart's Game Boy game or open the menu first.

## If something goes wrong

The full step-by-step is in the [recovery guide](../docs/recovery.md).

- Keep the backup folder and its `installation.json`. It records how far the installer got.
- If a step fails, run the installer again. The Chromatic's FPGA can always be reached over USB, even when an FPGA write was interrupted. If the screen stays dark, the FPGA slot is incomplete; running the installer again rewrites it.
- If the Chromatic isn't detected, unplug it, power-cycle it and reconnect. Don't erase flash and don't overwrite your only backup.
- If the installer refuses to start ("partition table checksum", "unsupported layout") or Uninstall finds no earlier application, use **Restore backup**.
- Still stuck? [Open a GitHub issue](https://github.com/agentichunter-blip/magic-trick/issues/new) with `installation.json` and the log. Never attach the backup itself, ROMs or saves.

## Status

**Beta.** Hardware-tested on one Chromatic with the Mac app: install from stock 4.6 and from ChroMagic, updates, uninstall (to ChroMagic and to stock), Restore backup, and ModRetro's updater returning the device to stock afterwards, with switching between Game Boy and Game Gear games in both directions. Game carts were tested on a ModRetro MR.TETRIS cart over USB and Wi-Fi: backup and restore with save memory, Game Gear, Lynx and Game Boy games with box art, a 1 MB own game, own-game save states and its save written back to the cart, with and without an SD card. The Windows app was also tested on hardware for install, update, uninstall and Restore backup, and the Linux app for install and uninstall. The packages are unsigned and the Mac apps aren't notarized. One Game Gear FPGA timing path is marginal; the core runs correctly on the test device.

Source and credits: https://github.com/agentichunter-blip/magic-trick. Based on CursedToast's ChroMagic and ModRetro's open-source firmware. Installer source is GPL-3.0; bundled tools keep their own licenses (programmer/licenses). Support my work: https://ko-fi.com/ryansag
