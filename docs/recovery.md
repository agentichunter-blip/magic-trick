# Recovery: getting back to a working Chromatic

> **Beta firmware, installed at your own risk.** Magic Trick is an independent community mod. It isn't made, endorsed or supported by ModRetro. Installing it rewrites your Chromatic's MCU and FPGA firmware. It may void your warranty. If something goes wrong, the steps below are the only official way back. There is no warranty (see the GPL-3.0 license).

Read this page **before** you install.

## The one file that matters

Every run of the installer saves a full MCU backup in a new folder: `Magic-Trick-backup-<date>/mcu-backup.bin`. The **first** one, made before Magic Trick was ever installed, is your way back to how the device was. Its `backup.json` says `"layout": "stock"`.

- Copy that folder somewhere safe (a second drive or a cloud folder you control) as soon as the install finishes.
- Keep it private. It holds your device's settings, including saved Wi-Fi credentials if you set any.
- Never share it in an issue or a chat.

## Why the device stays recoverable

- The installer never writes the MCU bootloader and never erases the whole flash. The MCU's built-in ROM loader can always be reached over USB.
- The FPGA is programmed through a separate USB programming chip. It works whatever the FPGA flash holds, even after an interrupted write.
- The Chromatic's USB serial port is provided by the FPGA. When the FPGA isn't running a working core, the serial port is missing, but the programming chip still works. **Restore backup** repairs the FPGA first for exactly this reason.
- The installer writes the MCU partition table last. Until then, your old firmware still starts.

## What happened → what to do

| What you see | Do this |
|---|---|
| The installer stopped with an error during install or update | Run **Back up & install / update** again. It picks up where it left off. |
| An FPGA write failed after all its attempts | Unplug, power-cycle, reconnect, then run the same operation again. |
| The screen stays dark after a failed FPGA write | Run the installer again. If no Chromatic port is listed, use **Restore backup** (it doesn't need one). |
| The installer refuses with "partition table checksum" or "unsupported layout" | Use **Restore backup**. |
| The Chromatic starts but Status shows the wrong versions | Run **Back up & install / update** again, or **Uninstall**. |
| You want Magic Trick gone | **Uninstall**, then ModRetro's updater if you want full stock. |
| Uninstall refuses ("no earlier application") | Use **Restore backup**. |
| ModRetro's updater or ChroMagician fails on the FPGA | Expected while Magic Trick is installed. **Uninstall** (or **Restore backup**) first. |

## The steps, in order

Try each step only if the one before it didn't work.

### 1. Run the same operation again

Every write is checked, and the installer works out what is already done. A rerun is always safe. FPGA writes often fail a few times before they go through; that's normal, and the installer retries up to 12 times on its own.

If the Chromatic isn't detected: unplug it, turn it off and on, plug it back in, close ModRetro's updater, ChroMagician and any serial monitor, then choose **Refresh USB ports**.

### 2. Uninstall

**Uninstall…** writes the ChroMagic 1.0.1 FPGA and switches the MCU back to the application you had before Magic Trick 1.0. That application was never touched by the install. Your settings stay as they are now.

### 3. Restore backup

**Restore backup…** puts the device back the way your first backup saved it, whatever state it's in now:

1. Choose `mcu-backup.bin` from your **first** install. The installer refuses a backup made while Magic Trick was installed, and a file that doesn't match its `backup.json`.
2. It saves a copy of the current flash, if it can be read, into a new `Magic-Trick-restore-<date>` folder.
3. It writes the ChroMagic 1.0.1 FPGA (retrying as needed), waits for the Chromatic to reconnect, then writes and verifies your saved MCU flash (everything from the partition table on, including your settings).

This takes 10–20 minutes. The device ends up with the ChroMagic 1.0.1 FPGA and the MCU application you had before. If you started from stock, the application is stock again, and step 4 finishes the job.

### 4. Back to full stock

After **Uninstall** or **Restore backup**, ModRetro's official updater works normally again. Use it to install stock firmware. Don't use it before then: it can't write the FPGA while Magic Trick is installed, and it doesn't retry.

### 5. Still stuck

Open an issue with your `installation.json` (from the newest backup or restore folder) and the installer's log. Say which operating system you used. **Never attach `mcu-backup.bin` or `pre-restore-flash.bin`**, ROMs or saves.

Please don't try `esptool erase_flash`, other people's flash dumps, or random FPGA images. Each one makes recovery harder. An erased flash loses the settings and calibration your backup holds.

## What has been tested on hardware

| Path | Tested on hardware |
|---|---|
| Install from ChroMagic 1.0.1 / art-series | Yes |
| Update an existing Magic Trick 1.0 install, including from a half-written FPGA slot | Yes |
| Uninstall to ChroMagic | Yes |
| Restore backup from an installed Magic Trick 1.0 | Yes |
| Restore backup after an interrupted partition-table write (installer refused the device) | Yes |
| Restore backup with a blank FPGA slot 0 (dark screen, no USB serial port) | Yes |
| Install from stock 4.x, and Uninstall/Restore back to stock firmware | Not yet |
| Windows and Linux installers | Not yet |

This is a beta tested on a small number of devices. If your situation isn't covered here, stop and open an issue before you try anything else.
