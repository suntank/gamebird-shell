# lr-Mupen64Plus distribution notes

GameBird uses RetroPie's older lr-mupen64plus core as the default for Nintendo
64 games because it performs better than Mupen64Plus-Next on the Pi Zero 2 W.

- Project: https://github.com/RetroPie/mupen64plus-libretro
- License: GNU General Public License, version 2 (GPL-2.0)
- Source revision: `3d195fc025c99d0d644bc30742431b65b5f5b7e5`
- Source archive SHA-256: `f3f80fd37b9197559198cfc865e01ace36b934e09adc6fa50e5bdde9eba2e085`
- RetroPie Setup revision: `67d193cdcc8308c80312b3ce60bf6365a8e72001`
- Build target: `platform=rpi3-mesa` (32-bit ARM Cortex-A53, NEON, GLES2)
- Compatibility flag: explicit `-marm` for every C/C++ translation unit. This
  keeps the core's libco ARM context switch compatible with modern compilers,
  which otherwise default parts of the build to Thumb mode.
- Installed core: `mupen64plus_libretro.so`
- Installed core SHA-256: `81616bec8a0ca8c6edb7a6607d3bdef58bcfaa6119c06e0765a7960f32bc0d4f`

The source archive and the three current RetroPie build patches are stored in
the user-visible GameBird data partition under `licenses/lr-mupen64plus/`.
Include that source and the GPL license when distributing a GameBird image or
console containing the compiled core.

Mupen64Plus-Next remains installed as a fallback for games that require its
newer compatibility fixes, but GameBird Shell does not select it by default.
