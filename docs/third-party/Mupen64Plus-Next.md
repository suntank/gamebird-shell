# Mupen64Plus-Next distribution notes

GameBird retains the Mupen64Plus-Next libretro core as a compatibility fallback
for Nintendo 64 games. The lighter RetroPie lr-mupen64plus core is the default
on Pi Zero 2 W hardware.

- Project: https://github.com/libretro/mupen64plus-libretro-nx
- License: GNU General Public License, version 2 (GPL-2.0)
- Source revision: `f275caf4b2bfa1e6d1c51636746ea793f3d80320`
- Source archive SHA-256: `6d408d5bb52569310487e151540b6a294b6167e005da4e76a1c9742faa9d4ac0`
- Build target: `platform=rpi3-mesa` (32-bit ARM Cortex-A53, NEON, GLES2)
- Installed core: `mupen64plus_next_libretro.so`

When distributing a GameBird image or console containing the compiled core,
include its complete corresponding source and GPL license. The production
image stores the downloaded source archive and license in the user-visible
GameBird data partition under `licenses/Mupen64Plus-Next/`.
