#!/bin/bash
# Cross-build the phoneME Feature CLDC VM for the PlayStation 2 (ps2_mips target).
#
# Runs inside the `phoneme-cross` container (host glibc toolchain + Java + the
# ps2sdk EE toolchain). Produces the MIPS VM archives (libcldc_vm*.a) + ROMImage;
# the final PS2 ELF is linked separately by the EE Makefile against our javacall.
#
# Source stays on the bind mount (/work); build space is the container-native
# persistent volume (/build), same rationale as the host build (see build-cldc.sh).
#
# Usage (inside container): docker/phoneme-cross/build-cldc-ps2.sh [make-target...]
set -e

cd /work

# 1) Toolchain/GCC-strictness patches to the git-ignored phoneME tree.
./docker/phoneme-host/patch-phoneme.sh references/phoneme

# 2) Materialize our versioned overlay (the ps2_mips build target, and any other
#    additions we keep under ps2/phoneme/) into the phoneME tree. Idempotent.
cp -r ps2/phoneme/. references/phoneme/

export JDK_DIR=/opt/java/openjdk
export GNU_TOOLS_DIR=${GNU_TOOLS_DIR:-/usr/local/ps2dev/ee-crosswrap}
WS=/work/references/phoneme/cldc
BS=${JVMBUILDSPACE:-/build/cldc_ps2_out}
mkdir -p "$BS"

# 2b) Cross-build PCSL for MIPS. The CLDC javacall os-family links its OsFile/
#     Stream layer against PCSL, so the target needs PCSL's headers (to compile)
#     and its libs (at the final ELF link). Built with the EE toolchain; portable
#     modules over newlib (posix file, stub network, heap chunk memory).
#     File module = ram: an in-memory RAM filesystem (rmfs). The posix module needs
#     sys/statvfs.h (absent in newlib) and, worse, a mounted writable FS the PS2
#     lacks; the stubs module's pcsl_file_init() returns -1, so MIDP storage init
#     aborts ("out of memory") before the AMS can start. rmfs is self-contained and
#     gives MIDP a working (ephemeral) storage + RMS without any hardware I/O.
export PCSL_OUTPUT_DIR=${PCSL_OUTPUT_DIR:-/build/pcsl_ps2_out}
PCSL_FILE_MODULE=${PCSL_FILE_MODULE:-ram}
PCSL_STAMP="$PCSL_OUTPUT_DIR/.pcsl_file_module"
if [ ! -f "$PCSL_OUTPUT_DIR/linux_mips/inc/pcsl_file.h" ] || \
   [ "$(cat "$PCSL_STAMP" 2>/dev/null)" != "$PCSL_FILE_MODULE" ]; then
    echo "+ building PCSL (FILE_MODULE=$PCSL_FILE_MODULE)"
    rm -rf "$PCSL_OUTPUT_DIR/linux_mips"
    make -C references/phoneme/pcsl \
        PCSL_PLATFORM=ps2_mips_gcc \
        PCSL_OUTPUT_DIR="$PCSL_OUTPUT_DIR" \
        GNU_TOOLS_DIR="$GNU_TOOLS_DIR" \
        JDK_DIR="$JDK_DIR" \
        NETWORK_MODULE=stubs \
        FILE_MODULE="$PCSL_FILE_MODULE"
    echo "$PCSL_FILE_MODULE" > "$PCSL_STAMP"
fi

# 3) Assemble JAVACALL_OUTPUT_DIR = the javacall port as the build expects it:
#      inc/  — interface headers the os/javacall glue compiles against
#      lib/  — libjavacall.a + cldc_javanotify_stubs.o (consumed at the final ELF
#              link, which the EE Makefile does; not needed to build the archives)
JC_OUT=${JAVACALL_OUTPUT_DIR:-/build/javacall_ps2}
mkdir -p "$JC_OUT/inc" "$JC_OUT/lib"
cp references/phoneme/javacall/interface/*.h        "$JC_OUT/inc/" 2>/dev/null || true
cp references/phoneme/javacall/interface/common/*.h "$JC_OUT/inc/" 2>/dev/null || true
cp references/phoneme/javacall/interface/midp/*.h   "$JC_OUT/inc/" 2>/dev/null || true
cp ps2/javacall/include/javacall_platform_defs.h    "$JC_OUT/inc/"

# 4) Cross-build. jvm.make hard-codes i386's `--32` into ASM_FLAGS unconditionally;
#    the host passes (romgen) genuinely need it, and the EE `as` wrapper strips it
#    for the MIPS target pass — so ASM_FLAGS is left untouched here.
make -C references/phoneme/cldc/build/ps2_mips \
    JVMWorkSpace="$WS" \
    JVMBuildSpace="$BS" \
    JDK_DIR="$JDK_DIR" \
    JAVACALL_OUTPUT_DIR="$JC_OUT" \
    PCSL_OUTPUT_DIR="$PCSL_OUTPUT_DIR" \
    "$@"
