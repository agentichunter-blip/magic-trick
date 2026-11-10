# Magic Trick Installer 1.0.0-beta

> **Beta firmware, installed at your own risk.** Magic Trick is an independent community mod, not made, endorsed or supported by ModRetro. It rewrites your Chromatic's MCU and FPGA firmware and may void your warranty. Read the [recovery guide](../docs/recovery.md) before you install.

One app installs Magic Trick 1.0 beta on a ModRetro Chromatic, updates an earlier Magic Trick 1.0 install, or uninstalls it. Python, esptool, openFPGALoader and all firmware are bundled. No internet connection is needed.

**What you get:** Game Boy, Game Boy Color and Game Gear on one device, switching cores automatically. Cover art and gameplay previews, Game Genie cheats, Recent games, last-game startup, native Game Boy / Game Boy Color save states, Wi-Fi file transfers and the Magic Trick boot animation. Game Gear has no save states yet.

## Before you start

- Back up your SD card yourself, especially saves.
- After your first install, copy its backup folder somewhere safe. Its `mcu-backup.bin` is your way back to how the device was (see [recovery](../docs/recovery.md)).
- Use a data-capable USB cable. Power the Chromatic on and close other device apps (ModRetro's updater, ChroMagician, serial monitors).
- **Windows:** you may need ModRetro's MRUpdater USB driver setup before the programmer can reach the device. **Linux:** you may need serial and USB permissions (udev rules). The app checks access before it writes anything.
- Supported starting points: stock 4.2, 4.5 and 4.6, ChroMagic 1.0.1 and art-series firmware, or Magic Trick 1.0 beta. Anything else is refused before anything is written.

## Install or update

Extract the whole archive and open Magic Trick Installer. **Mac:** open the .app. **Windows:** run Magic-Trick-Installer.exe from its folder. **Linux:** run Magic-Trick-Installer from its folder (Ubuntu 22.04+ x64). Select the Chromatic, confirm your SD backup, choose a private backup folder, then choose **Back up & install / update**.

The installer:

1. Saves and verifies a full 4 MiB backup of the MCU flash in your folder. Keep it private: it contains your device's settings.
2. Writes and verifies the Magic Trick MCU application in an unused area. Until step 5 your old application still starts.
3. Writes and verifies the Game Gear FPGA core, then the Game Boy / Game Boy Color core. Updating an existing Magic Trick 1.0 install? The installer first puts the ChroMagic FPGA in the Game Boy slot so the Game Gear slot can be rewritten safely.
4. FPGA writes sometimes fail a few times before they go through. That's expected: the installer restarts the FPGA and retries, up to 12 times at about 80 seconds per attempt. Every completed write is read back and verified.
5. Switches the MCU partition table to the new layout, last.
6. Restarts the Chromatic and checks the running versions.

It never writes the SD card or the bootloader, and never erases the whole flash. When it says it's done, turn the Chromatic off and on once. Status shows **Magic Trick 1.0 beta** and MCU **v1.0.0-beta**.

## Uninstall

Choose **Uninstall…**. The installer writes the ChroMagic 1.0.1 FPGA and restores the standard partition table, so the application you had before Magic Trick 1.0 runs again. It's kept untouched at 0x10000. To return fully to stock afterwards, use ModRetro's official updater.

ModRetro's updater and ChroMagician can't reflash the FPGA of a device running Magic Trick 1.0: their FPGA writes fail and they don't retry. Uninstall with this app first.

## Restore backup

Choose **Restore backup…** and pick `mcu-backup.bin` from your **first** install. It works whatever state the device is in, even with no USB serial port: it writes the ChroMagic 1.0.1 FPGA first, then puts back your saved MCU flash. Use it when install, update or uninstall can't finish. Afterwards ModRetro's updater works normally.

## If something goes wrong

The full step-by-step is in the [recovery guide](../docs/recovery.md).

- Keep the backup folder and its `installation.json`. It records how far the installer got.
- If a step fails, run the installer again. The Chromatic's FPGA can always be reached over USB, even when an FPGA write was interrupted. If the screen stays dark, the FPGA slot is incomplete; running the installer again rewrites it.
- If the Chromatic isn't detected, unplug it, power-cycle it and reconnect. Don't erase flash and don't overwrite your only backup.
- If the installer refuses to start ("partition table checksum", "unsupported layout") or Uninstall finds no earlier application, use **Restore backup**.
- Still stuck? [Open a GitHub issue](https://github.com/agentichunter-blip/magic-trick/issues/new) with `installation.json` and the log. Never attach the backup itself, ROMs or saves.

## Status

**Beta.** Hardware-tested on one Chromatic: uninstall to ChroMagic, a fresh install from ChroMagic, and an update of an installed device, with switching between Game Boy and Game Gear games in both directions. A fresh install from stock 4.x firmware hasn't been tested on hardware. The packages are unsigned and the Mac apps aren't notarized. One Game Gear FPGA timing path is marginal; the core runs correctly on the test device.

Source and credits: https://github.com/agentichunter-blip/magic-trick. Based on CursedToast's ChroMagic and ModRetro's open-source firmware. Installer source is GPL-3.0; bundled tools keep their own licenses (programmer/licenses). Optional donations: https://ko-fi.com/ryansag
