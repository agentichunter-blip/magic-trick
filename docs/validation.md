# Validation status

This is an experimental MCU modification based on ChroMagic 1.0.1, tested with FPGA 18.38. The FPGA/core source is unchanged.

The art31 development installation passed flash checksum and running-version checks: MCU `v0.13.4-art31`, Chromatic `1.0.1-art31 (4.2)`, FPGA `18.38`. Physical checks confirmed that the held-scroll selector remains open after release, the first A reveals the preview, and the second A launches.

## Current behavior and coverage

- The held-scroll selector remains open during idle time and subsequent taps. Tests cover delayed/missing art, endpoint holds, preview confirmation, video suppression, and cleanup.
- Metadata-tagged Demo/Kiosk games are excluded from browsing, Recent, and last-game startup. Similarly named full games remain available. Installing the firmware does not move or delete SD files.
- Separate persistent Recent folders retain 20 games per system. Tests cover ordering, deduplication, eviction, missing files, failed launches/storage operations, and original-path routing. A physical GB check confirmed the history entry, artwork, and launch. Persistence across power cycles and separate GBC history have host coverage; those physical checks remain pending.
- Last-game startup retains cartridge priority and a B-button bypass. Physical cold-start checks confirmed automatic SD startup, B bypass, and cartridge startup without the late maintenance reset.
- Game Genie selections apply at the next SD game launch. Tests cover exact-ROM matching, compiler decoding, conflicts, persistence and failed commits, asynchronous UI work, and original-file preservation. A physical check verified one invincibility option in Super Mario Land (World); other database entries are not all gameplay-tested. See [cheat scope](cheats.md).

The 24-test host suite also covers menu rendering/lifecycle, startup frame priming, folder scanning, media formats and conversion, SD access coordination, and save/load paths. It has passed normally and with address/undefined-behavior sanitizers. Tests use synthetic fixtures and do not establish compatibility with every game or SD card.

Sustained preview performance has not been benchmarked. Extended gameplay/save testing across hardware and libraries remains incomplete. Build and host-test results do not replace those checks.

Build and test scripts do not flash hardware. The art31 prebuilt release includes SHA-256 checksums, matching tagged source, and [installation instructions](install-release.md). Its packaged image was built from the tested source and passed image checksum/version checks; that exact packaged build has not been reflashed onto hardware. No personal media or library packs are distributed.
