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

# 6) Modern binutils (2.4x) no longer enable the x87 FPU implicitly under
#    `.arch i486`, so the i386 float-support stubs (AsmStubs_i386.s: fld1/fmul/…,
#    used by the HOST romizer) fail to assemble: "`fld1' is not supported on
#    `i486'". Explicitly enable the 387 coprocessor extension right after the
#    .arch directive. Idempotent. (File is CRLF; the inserted line is LF, which
#    the assembler accepts.)
grep -q '\.arch \.387' "$PHONEME/cldc/src/vm/cpu/c/AsmStubs_i386.s" || \
  sed -i 's/^\.arch i486/.arch i486\n.arch .387/' \
      "$PHONEME/cldc/src/vm/cpu/c/AsmStubs_i386.s"

# 7) PCSL's stub network module declares a `javacall_result` local where it only
#    means an int — it calls no javacall function, just assigns a PCSL return
#    value — so it fails to compile unless the javacall headers happen to be on
#    the include path. Make the stub self-contained by using int. Idempotent
#    (re-running finds nothing to change). Only NETWORK_MODULE=stubs (the PS2
#    build) hits this; the host build uses bsd/generic.
sed -i 's/javacall_result res;/int res;/' \
    "$PHONEME/pcsl/network/stubs/pcsl_network.c"

# 8) MIDP security: `javacall_policy_load.c : policy_files` (midp_permissions/
#    lib.gmk) makes the make treat the checked-in source javacall_policy_load.c as
#    a build-dir target to (re)generate, which defeats the `vpath ... reference/
#    native` lookup -> "javacall_policy_load.c: No such file or directory" at
#    compile. policy_files only copies the runtime _policy/_function_groups files;
#    it does not affect the .c. Make it an order-only prerequisite (| policy_files)
#    so it still runs but the .c is resolved from the source tree. Idempotent.
grep -q 'javacall_policy_load.c : | policy_files' \
    "$PHONEME/midp/src/security/midp_permissions/lib.gmk" || \
  sed -i 's/^javacall_policy_load.c : policy_files/javacall_policy_load.c : | policy_files/' \
      "$PHONEME/midp/src/security/midp_permissions/lib.gmk"

echo "phoneME patches applied to: $PHONEME"
