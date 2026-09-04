# PCSX-ReARMed redistribution record

GameBird uses the PCSX-ReARMed libretro core from:

- Project: https://github.com/notaz/pcsx_rearmed
- Revision: `2e3c103eed6e81f5ee2e480df1bdcf4bc03c06b5`
- libchdr revision: `9ccd3a78e239246ee6e1166e1be82ab0ff0dcddb`
- libretro-common revision: `692124dbd8a0b2f5526b48f3bc2026421754e3c5`
- License: GNU General Public License version 2 (GPLv2)
- Build target: `platform=rpi3` (ARM32 Cortex-A53, ari64 dynarec, NEON GPU)

The reproducible build command is captured in
`scripts/build-pcsx-rearmed.sh`. When distributing a console containing the
binary, provide the complete corresponding source and GPLv2 license to the
customer using one of the methods allowed by GPLv2 section 3. Preserve this
record, the build script, all upstream copyright notices, and the exact source
revision used for the shipped binary.

No Sony BIOS or game content is part of GameBird. PCSX-ReARMed can use its HLE
BIOS, but compatibility improves when an owner supplies a BIOS obtained from
their own hardware. User-provided BIOS files belong in RetroArch's `system`
directory on the writable GameBird data partition. GameBird's PlayStation
profile points this to `GameBird/roms/psx/bios` so it is accessible over Samba.

On the console, the complete checked-out source corresponding to the installed
binary is included in the Samba share under
`GameBird/third-party-source/pcsx-rearmed-2e3c103`.
