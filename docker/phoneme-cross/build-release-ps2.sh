#!/bin/bash
# Build a RELEASE PS2ME ELF, in-container. Unlike the day-to-day debug builds, this does
# NOT set PS2ME_NO_IOP_RESET, so the launcher performs the normal USB-boot IOP reset
# (loads our own iomanX/usb modules on a clean IOP). That drops the SIF tty / EE console,
# which is exactly what an end user's console does anyway -- diagnostics render on-screen.
#
# Runs the full pipeline (MIDP archives + ELF link) and stamps the output with the version
# from ps2/version.h, producing build/ps2/PS2ME-vMAJOR.MINOR.PATCH.elf alongside the plain
# j2me-midp.elf.
#
# Usage (inside container):
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD":/work -v phoneme_build:/build \
#       phoneme-cross bash /work/docker/phoneme-cross/build-release-ps2.sh
set -e
cd /work

# Release: IOP reset ENABLED. Make sure the debug knob is off even if the caller exported it.
unset PS2ME_NO_IOP_RESET

echo "=================================================================="
echo " PS2ME : RELEASE build (IOP reset enabled -- no EE tty)"
echo "=================================================================="

bash docker/phoneme-cross/build-midp-ps2.sh
bash docker/phoneme-cross/build-elf-midp-ps2.sh

# Version from the single source of truth (ps2/version.h).
field() { sed -nE "s/.*define PS2ME_VERSION_$1[[:space:]]+([0-9]+).*/\1/p" ps2/version.h | head -1; }
VER="v$(field MAJOR).$(field MINOR).$(field PATCH)"

OUT="build/ps2"
cp "$OUT/j2me-midp.elf" "$OUT/PS2ME-$VER.elf"

echo "=================================================================="
ls -la "$OUT/PS2ME-$VER.elf"
echo " RELEASE OK -> $OUT/PS2ME-$VER.elf"
echo "=================================================================="
