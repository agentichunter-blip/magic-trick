# Install the prebuilt art31 firmware

This package contains the MCU application for ChroMagic `1.0.1-art31 (4.2)` / MCU `v0.13.4-art31`. No compilation or ESP-IDF installation is required. You need Python 3.12, esptool 4.13.0, a data-capable USB cable, and a Chromatic with an SD card.

## Required base firmware

Start with **ChroMagic 1.0.1 and FPGA 18.38**, or an earlier art-series MCU mod using the same base/layout. This package updates only the MCU application. It contains no FPGA `.fs`, bootloader, partition table, device dump, games, saves, or media packs.

If starting from stock, first use [ChroMagician](https://github.com/cursedtoast2/ChroMagician/releases) to install upstream ChroMagic and verify FPGA 18.38. Upstream currently provides Windows/Linux installers. Complete its USB driver setup, then close it before using esptool. If a different FPGA version is installed, stop: compatibility has not been established.

Back up your SD card, especially saves. Download **chromagic-1.0.1-art31-firmware.zip** from [this release](https://github.com/agentichunter-blip/chromagic-preview-mod/releases/tag/v1.0.1-art31) and extract it. Open a terminal in the extracted folder containing `mcu.bin` and `INSTALL.md`. GitHub's automatic “Source code” archives are for building, not installing this binary.

## 1. Install the flashing tool

Create a dedicated environment. Installing the tool downloads Python packages; the flash commands below communicate with the device locally. These steps follow [Espressif's esptool 4.x instructions](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/installation.html).

**macOS/Linux (Terminal):**

```sh
python3 -m venv .venv-flash
.venv-flash/bin/python -m pip install esptool==4.13.0
```

**Windows (PowerShell):**

```powershell
py -3 -m venv .venv-flash
.\.venv-flash\Scripts\python.exe -m pip install esptool==4.13.0
```

No activation or PowerShell execution-policy change is needed. On Linux, install your distribution's Python venv package if environment creation is unavailable.

## 2. Verify the download and image

Expected SHA-256 of `mcu.bin`:

```text
83a8cbc4137b1a4c0cd4457194a4e862a4c04d87db4a03af0b58c01a709e5144
```

On macOS run `shasum -a 256 mcu.bin`; on Linux run `sha256sum mcu.bin`; on Windows run `Get-FileHash .\mcu.bin -Algorithm SHA256`. Compare the full result with the value above and `SHA256SUMS`. Do not flash if they differ.

**macOS/Linux:**

```sh
.venv-flash/bin/python -m esptool --chip esp32 image_info --version 2 mcu.bin
```

**Windows:**

```powershell
.\.venv-flash\Scripts\python.exe -m esptool --chip esp32 image_info --version 2 mcu.bin
```

Confirm project `mcu_fw`, app version `0.13.4-art31`, ESP-IDF `v5.3`, and valid checksum and validation hash. This reads the file only.

## 3. Connect and back up the MCU

Power on the Chromatic and connect it by USB. Close ChroMagician, MRUpdater, serial monitors, and any other application using the device. List ports, then replace the example port with your actual Chromatic port. Linux ports may look like `/dev/ttyACM0`; macOS ports usually begin `/dev/cu.usbmodem`; Windows ports use `COM` numbers.

**macOS/Linux:**

```sh
.venv-flash/bin/python -m serial.tools.list_ports
PORT=/dev/cu.usbmodemYOUR_DEVICE
.venv-flash/bin/python -m esptool --chip esp32 --port "$PORT" --baud 460800 read_flash 0x0 0x400000 ../chromatic-mcu-backup.bin
```

**Windows:**

```powershell
.\.venv-flash\Scripts\python.exe -m serial.tools.list_ports
$devicePort = "COM7"
.\.venv-flash\Scripts\python.exe -m esptool --chip esp32 --port $devicePort --baud 460800 read_flash 0x0 0x400000 ..\chromatic-mcu-backup.bin
```

Choose a new backup filename if one already exists. Continue only if backup succeeds and the file is **4,194,304 bytes**. Keep it private and outside the release folder; it can contain device-specific settings and game history. This backs up neither the SD card nor the FPGA.

## 4. Flash only the application

Write `mcu.bin` at **`0x10000`**, DIO / 40 MHz / 4 MB. Do not use `erase_flash`, offset zero, or a whole-device flashing command. Keep power and USB connected until completion.

**macOS/Linux:**

```sh
.venv-flash/bin/python -m esptool --chip esp32 --port "$PORT" --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x10000 mcu.bin
```

**Windows:**

```powershell
.\.venv-flash\Scripts\python.exe -m esptool --chip esp32 --port $devicePort --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x10000 mcu.bin
```

Wait for **`Hash of data verified.`** Power-cycle, then confirm MCU `v0.13.4-art31`, Chromatic `1.0.1-art31 (4.2)`, and unchanged FPGA `18.38`. Leave **C. MAGICIAN** off for on-device gameplay. The application partition is 1 MiB; this update leaves the bootloader, partition table, settings, and FPGA in place.

If a remembered SD game starts automatically, hold **B** at power-on for five seconds, then release to open BACKUPS. With the held-scroll selector open, A first reveals the preview; A again launches.

## Media, cheats, and rollback

Existing ROMs, saves, covers and previews need no changes. Optional media and cheat packs are separate: see [media preparation](https://github.com/agentichunter-blip/chromagic-preview-mod/blob/v1.0.1-art31/docs/media.md) and [cheat preparation](https://github.com/agentichunter-blip/chromagic-preview-mod/blob/v1.0.1-art31/docs/cheats.md). No personal library or downloaded database is supplied.

If the port cannot open, close other apps and check the USB cable/drivers. If transfers fail, retry with `--baud 115200`. Do not erase the device or guess another offset. To return to upstream ChroMagic or stock, use ChroMagician's firmware installation/restore workflow on a supported host. Keep SD/save backups until the restored setup is verified.

## Source, credits, and validation

Matching source: [v1.0.1-art31](https://github.com/agentichunter-blip/chromagic-preview-mod/tree/v1.0.1-art31). ChroMagic is by CursedToast, based on ModRetro's firmware/design; see `CREDITS.md`, `NOTICE.txt`, and the included licenses. This is an independent experimental mod.

The source passed all 24 host tests normally and with address/undefined-behavior sanitizers. Art31 was previously verified on hardware. This release image was built from the matching tested source and its image checksum/version were checked; this exact packaged build has not been reflashed onto hardware. Sustained preview benchmarks, broad game/save compatibility, and every cheat's behavior remain unverified. No guaranteed support schedule.
