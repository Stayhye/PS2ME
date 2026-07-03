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

# 4) Large-file support for the 32-bit host tools (romgen/loopgen).
#    romgen is built -m32. On a Docker Desktop (Windows) bind mount the shared
#    files get 64-bit inode numbers, so a 32-bit stat() over the mount fails with
#    EOVERFLOW ("Value too large for defined data type"). romgen resolves the
#    `Include cldcx_rom.cfg` directive via OsFile_exists() -> stat()+S_ISREG(),
#    so it reported "Cannot find included ROM configuration file" even though the
#    file opened fine with fopen(). -D_FILE_OFFSET_BITS=64 makes stat() use the
#    64-bit stat64 path and resolves the include. (fopen already worked, which is
#    why the main -romconfig loaded but the Include failed.)
# NOTE: jvm.make has CRLF line endings, so do NOT anchor on '$' (the line really
# ends in "-DGCC\r"). Insert the flag before -pipe so the trailing \r stays where
# it already is.
grep -q '_FILE_OFFSET_BITS' "$PHONEME/cldc/build/share/jvm.make" || \
  sed -i 's/+= -pipe -DGCC/+= -D_FILE_OFFSET_BITS=64 -pipe -DGCC/' \
      "$PHONEME/cldc/build/share/jvm.make"

# 5) The generated ROMImage.cpp initializes int[] arrays with word values > INT_MAX
#    (e.g. 0xFFFFFFFF = 4294967295). In C++11+ brace-init, that is a narrowing
#    conversion, which modern gcc reports as a hard ERROR. The 2009 code is C++03,
#    where this is fine. -Wno-narrowing restores the old (accepting) behaviour.
#    (Same CRLF caveat: insert before -pipe, not at end of line.)
grep -q 'Wno-narrowing' "$PHONEME/cldc/build/share/jvm.make" || \
  sed -i 's/+= -D_FILE_OFFSET_BITS=64 -pipe -DGCC/+= -D_FILE_OFFSET_BITS=64 -Wno-narrowing -pipe -DGCC/' \
      "$PHONEME/cldc/build/share/jvm.make"

echo "phoneME patches applied to: $PHONEME"
