# Magic Trick Installer 0.2.0 — testing preview

One app installs Magic Trick directly onto a stock ModRetro Chromatic or upgrades a compatible ChroMagic installation. Includes Python, esptool and openFPGALoader. Internet access is required to download the pinned MCU and FPGA images; both SHA-256 hashes are checked before installation. You do not need to install ChroMagic separately.

Extract the entire archive. Mac: open the .app. Windows: open Magic-Trick-Installer.exe in its folder. Linux: run Magic-Trick-Installer from its folder (Ubuntu 22.04+ x64). Connect one powered-on Chromatic with a data USB cable; close other device apps. Back up your SD card yourself, select the detected Chromatic, and choose a private backup folder.

Windows may need the official MRUpdater USB driver setup before the programmer is accessible. Linux may need serial and USB device permissions. The app checks access before writing firmware. Mac tools are bundled and do not require Homebrew.

The installer reads and verifies a full 4 MiB MCU backup, validates its partition table and application descriptor, writes/verifies the Magic Trick MCU application, then writes/verifies the ChroMagic FPGA. It checks running versions before reporting success. It leaves the bootloader, partition table and SD card unchanged. The backup does NOT include FPGA or SD contents. Keep your backup private.

Supported source checks: stock 4.2, 4.5 and 4.6; compatible ChroMagic 1.0.1/art-series; and older consoles without fwversion when the backed-up app identifies as mcu_fw 0.13.0–0.13.8 with the expected layout. Unknown firmware/layouts are rejected. These are software checks, not a hardware compatibility certification.

If interrupted, keep the backup and installation.json. An MCU-success/FPGA-failure result is a partial installation, never success. If the console still responds, retry the paired installer. If it no longer responds, stop and use the recovery documentation; do not erase flash or overwrite the only backup. Stock restoration requires matching stock MCU AND FPGA firmware (official MRUpdater); an MCU backup alone does not restore the FPGA.

Recovery: https://github.com/agentichunter-blip/chromagic-preview-mod/blob/main/docs/install-release.md

These packages are unsigned and not Apple-notarized. No end-to-end hardware installation has been tested yet. Intended for testing; wait for hardware validation if you need a proven installation path. Device Status reports MCU v0.13.4-art31, FPGA 18.38, Chromatic 1.0.1-art31 (4.2). No games, saves, artwork or previews are bundled.

Source: https://github.com/agentichunter-blip/chromagic-preview-mod
Based on CursedToast's ChroMagic and ModRetro firmware. Independent community project. Installer source is GPL-3.0; bundled tools retain their own licenses, included under programmer/licenses. Programmer source archives are included for Mac; corresponding Linux/Windows sources are provided with the release. Firmware sources and full credits are in the repository.

Optional donations: https://ko-fi.com/ryansag
