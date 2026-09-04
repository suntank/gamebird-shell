#!/bin/sh
# Reproducible GameBird build of the GPLv2 PCSX-ReARMed libretro core.
set -eu

revision=2e3c103eed6e81f5ee2e480df1bdcf4bc03c06b5
destination=${1:-./pcsx-rearmed-build}

if [ ! -d "$destination/.git" ]; then
  git clone https://github.com/notaz/pcsx_rearmed.git "$destination"
fi
git -C "$destination" fetch --depth 1 origin "$revision"
git -C "$destination" checkout --detach "$revision"
git -C "$destination" submodule update --init --depth 1 \
  deps/libchdr deps/libretro-common frontend/libpicofe frontend/warm
make -C "$destination" -f Makefile.libretro clean
# Pi Zero 2 uses the same 32-bit Cortex-A53/NEON target as Raspberry Pi 3.
make -C "$destination" -f Makefile.libretro platform=rpi3 -j1
