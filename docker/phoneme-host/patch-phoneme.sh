#!/bin/sh
# Idempotent patches to the phoneME source tree so it builds with a modern
# toolchain (GCC 12+). The tree (references/phoneme) is git-ignored, so THIS
# script is the versioned record of every change we make to it. Re-runnable.
#
# Usage: docker/phoneme-host/patch-phoneme.sh [path-to-phoneme]   (default: references/phoneme)
set -e
PHONEME="${1:-references/phoneme}"
test -d "$PHONEME/cldc" || { echo "phoneME tree not found at $PHONEME"; exit 1; }

# 1) CLDC VM: modern GCC is much stricter; the VM's global -Werror turns spurious
#    warnings (maybe-uninitialized, etc.) into hard errors. Disable -Werror.
sed -i 's/^CPP_FLAGS[[:space:]]*+=[[:space:]]*-Werror/# CPP_FLAGS += -Werror  (disabled for modern gcc)/' \
    "$PHONEME/cldc/build/share/jvm.make"

# 2) CLDC VM: the build forces -fstrict-aliasing. Modern GCC's aliasing optimizer is
#    far more aggressive than 2009's and miscompiles the VM's heavy type-punning,
#    crashing the romizer (romgen SIGSEGV at addr 0x50 while generating ROMImage.cpp).
#    Force -fno-strict-aliasing.
sed -i 's/CPP_DEF_FLAGS[[:space:]]*+=[[:space:]]*-fstrict-aliasing/CPP_DEF_FLAGS += -fno-strict-aliasing/' \
    "$PHONEME/cldc/build/share/jvm.make"

# 3) THE romizer fix: romgen SIGSEGVs deterministically at addr 0x50 during class
#    romization when built at -O2 with modern gcc (the 2009 VM has undefined
#    behaviour that gcc 12's optimizer exploits). -O0 makes romgen run correctly
#    (confirmed: "Loading classes...Done!"). Build the VM at -O0.
#    NOTE: -O0 also slows the final cldc_vm; narrow this to the minimal -fno-* set
#    (or build only romgen at -O0) once the exact miscompiled pass is identified.
sed -i 's/-O2 /-O0 /g' "$PHONEME/cldc/build/share/jvm.make"

echo "phoneME patches applied to: $PHONEME"
