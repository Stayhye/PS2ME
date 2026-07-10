#!/bin/bash
# Fast incremental rebuild for C/C++-only changes -- the iteration-loop companion to
# build-midp-ps2.sh + build-elf-midp-ps2.sh.
#
# WHY IT IS FAST (measured: ~80s vs ~10min, ~8x): the full build-midp-ps2.sh (a) re-copies the
# ps2/phoneme overlay with `cp -r ps2/phoneme/. references/phoneme/`, which bumps the mtime of
# hundreds of tree files and forces `make` to recompile most of libmidp; (b) runs `make midp`
# TWICE (a properties-seeding pass + the real pass); and (c) builds libjavacall once here and
# AGAIN in build-elf. build-fast does none of that: no overlay re-copy, no re-patch, a single
# `make midp -j`, and the libjavacall compile only in build-elf. It recompiles just the .c
# files whose timestamps actually changed, re-archives libmidp.a, re-merges libjvm.a, relinks.
#
# NOTE: `make midp` may still re-run the Java romizer + recompile ROMImage.o (it rebuilds
# classes.zip mid-run, which invalidates ROMImage.cpp). That is only a fraction of the time and
# is functionally harmless for a C-only change (the ROM is identical); the bulk of the speedup
# comes from NOT mass-recompiling the tree. If you changed the romized Java classes / native
# table / heap sizes, use the full build-midp-ps2.sh instead.
#
# WHEN TO USE: after at least one full build (build-cldc-ps2.sh + build-midp-ps2.sh) has
# populated the phoneme_build volume, for any C/C++ edit under:
#   - ps2/javacall/**          (recompiled by build-elf-midp-ps2.sh; make midp is a no-op)
#   - references/phoneme/**    (a libmidp/libcldc native .c; make midp recompiles + re-merges)
# DO NOT use it when the romized MIDlet Java classes, the native-function table, or the heap
# sizes change -- those need the full build-midp-ps2.sh (which re-romizes). It also does NOT
# re-run patch-phoneme.sh or re-copy the ps2/phoneme overlay (the persistent tree already has
# them from the last full build), so a fresh volume still needs the full scripts first.
#
# Usage (inside the phoneme-cross container):
#   docker run --rm -v D:/PS2DEV/ports/j2me:/work -v phoneme_build:/build phoneme-cross \
#       bash /work/docker/phoneme-cross/build-fast.sh
# Extra args are forwarded to `make` (e.g. a specific target).
set -e

cd /work

export GNU_TOOLS_DIR=${GNU_TOOLS_DIR:-/usr/local/ps2dev/ee-crosswrap}
export JDK_DIR=${JDK_DIR:-/opt/java/openjdk}

CLDC_DIST=/build/cldc_ps2_out/ps2_mips/dist
PCSL_OUT=/build/pcsl_ps2_out
JC_OUT=/build/javacall_ps2
MIDP_OUT=/build/midp_ps2_out
MIDP_SRC=/work/references/phoneme/midp/build/javacall

# Guard: the fast path reuses artefacts from a prior full build; bail out with a clear
# message instead of a confusing make error if the volume was never fully built.
if [ ! -f "$JC_OUT/jwc_properties.ini" ] || [ ! -f "$MIDP_OUT/bin/mips/libmidp.a" ]; then
    echo "build-fast: no prior full build in the volume (missing jwc_properties.ini or libmidp.a)." >&2
    echo "            Run build-cldc-ps2.sh + build-midp-ps2.sh once first." >&2
    exit 1
fi

NPROC=$(nproc 2>/dev/null || echo 4)

echo "=================================================================="
echo " build-fast: incremental rebuild (NO romization, make -j$NPROC)"
echo "=================================================================="

# mergelib is consumed by the libjvm.a merge rule; put it on PATH (build-midp installs it,
# but a fresh container invoking build-fast directly would not have it).
install -m 0755 docker/phoneme-cross/mergelib /usr/local/bin/mergelib

# Recompile only the changed native sources, re-archive libmidp.a and re-merge libjvm.a.
# NB: this does NOT re-run patch-phoneme.sh, so a legacy cosmetic "-fwrapv -fwrapv -fwrapv" in
# the persistent jvm.make (a harmless, redundant flag) is left as-is; a full build normalises it.
echo "+ [1/2] make midp (incremental, -j$NPROC)"
make -C "$MIDP_SRC" -j"$NPROC" \
    JAVACALL_PLATFORM=ps2_mips_gcc \
    JAVACALL_OUTPUT_DIR="$JC_OUT" \
    CLDC_DIST_DIR="$CLDC_DIST" \
    PCSL_OUTPUT_DIR="$PCSL_OUT" \
    MIDP_OUTPUT_DIR="$MIDP_OUT" \
    TOOLS_DIR=/work/references/phoneme/tools \
    GNU_TOOLS_DIR="$GNU_TOOLS_DIR" \
    JDK_DIR="$JDK_DIR" \
    "${@:-midp}"

# Relink the PS2 ELF: this recompiles the javacall port (ps2/javacall/**) and links against
# the freshly merged libjvm.a. ROMImage.o is reused from the MIDP build as-is.
echo "+ [2/2] build-elf-midp-ps2.sh (relink)"
bash /work/docker/phoneme-cross/build-elf-midp-ps2.sh

echo "=================================================================="
echo " build-fast: done -> build/ps2/j2me-midp.elf"
echo "=================================================================="
