#!/bin/bash
# Cross-build the phoneME Feature MIDP for the PlayStation 2 (ps2_mips_gcc).
#
# Produces the MIDP archives for MIPS -- libmidp.a and libjvm.a (libmidp merged
# with our cross-built libcldc_vm.a + the PCSL archives) -- plus the MIDP ROM
# image and native-function table. The final PS2 ELF is linked separately by our
# EE build, same split as the CLDC ps2_mips target (we target `make midp`, not the
# runMidlet executable).
#
# Runs inside the `phoneme-cross` container. Consumes the CLDC dist + PCSL built
# into the phoneme_build volume by build-cldc-ps2.sh (host romgen/preverify from
# the CLDC dist do the MIDP romization).
#
# Usage (inside container): docker/phoneme-cross/build-midp-ps2.sh [make-target...]
set -e

cd /work

PS2SDK=${PS2SDK:-/usr/local/ps2dev/ps2sdk}
export GNU_TOOLS_DIR=${GNU_TOOLS_DIR:-/usr/local/ps2dev/ee-crosswrap}
export JDK_DIR=${JDK_DIR:-/opt/java/openjdk}

CLDC_DIST=/build/cldc_ps2_out/ps2_mips/dist
PCSL_OUT=/build/pcsl_ps2_out
JC_OUT=/build/javacall_ps2
MIDP_OUT=/build/midp_ps2_out
MIDP_SRC=/work/references/phoneme/midp/build/javacall

echo "=================================================================="
echo " j2me-ps2 : cross-building MIDP for MIPS (ps2_mips_gcc)"
echo "=================================================================="

# --- 0) Refresh the git-ignored phoneME tree (patches + our overlay) ----------
bash ./docker/phoneme-host/patch-phoneme.sh references/phoneme >/dev/null 2>&1 || true
cp -r ps2/phoneme/. references/phoneme/

# --- 1) Install our `mergelib` (absent from the phoneME tree) on PATH ----------
install -m 0755 docker/phoneme-cross/mergelib /usr/local/bin/mergelib

# --- 2) Assemble JAVACALL_OUTPUT_DIR: headers + properties.xml + lib dir -------
mkdir -p "$JC_OUT/inc" "$JC_OUT/lib"
# All javacall headers the MIDP C sources include: the interface contract
# (defs + common + midp profile) plus the share-module headers (properties, utils)
# whose .c live under implementation/share and are portable (linked at ELF time).
JC_IF=references/phoneme/javacall/interface
JC_SHARE=references/phoneme/javacall/implementation/share
cp "$JC_IF"/javacall_defs.h        "$JC_OUT/inc/" 2>/dev/null || true
cp "$JC_IF"/common/*.h             "$JC_OUT/inc/" 2>/dev/null || true
cp "$JC_IF"/midp/*.h               "$JC_OUT/inc/" 2>/dev/null || true
cp "$JC_SHARE"/properties/inc/*.h  "$JC_OUT/inc/" 2>/dev/null || true
cp "$JC_SHARE"/utils/inc/*.h       "$JC_OUT/inc/" 2>/dev/null || true
cp ps2/javacall/include/javacall_platform_defs.h "$JC_OUT/inc/"
cp ps2/javacall/config/properties.xml "$JC_OUT/properties.xml"
# properties.xml's <!DOCTYPE ... SYSTEM "configuration.dtd"> is resolved next to it.
cp references/phoneme/javacall/build/configuration.dtd "$JC_OUT/configuration.dtd"

# libjavacall.a is a `midp` prerequisite (MIDP_DEPS). It is not actually linked by
# `make midp` (only the runMidlet executable links it, which we don't build here),
# but the file must exist. Compile our port's current modules for MIPS into it.
EE_CXX=$GNU_TOOLS_DIR/bin/g++
EE_AR=$GNU_TOOLS_DIR/bin/ar
JC=/work/ps2/javacall
JC_FLAGS="-D_EE -DMIPS -G0 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -I$JC_OUT/inc -I$PS2SDK/ee/include -I$PS2SDK/common/include"
JC_SRCS="contract/javacall_logging.cpp contract/javacall_os.cpp
    contract/javacall_time.cpp contract/javacall_memory.cpp
    hal/Logger.cpp hal/OsCore.cpp hal/SystemClock.cpp hal/TimerService.cpp
    hal/MemoryManager.cpp platform/StdoutSink.cpp platform/Ps2CpuCache.cpp
    platform/Ps2AlarmTimer.cpp platform/PosixClock.cpp platform/SystemHeap.cpp"
JC_TMP=/tmp/libjavacall_obj; rm -rf "$JC_TMP"; mkdir -p "$JC_TMP"
JC_OBJS=""
for src in $JC_SRCS; do
    obj="$JC_TMP/$(echo "$src" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    $EE_CXX $JC_FLAGS -c "$JC/$src" -o "$obj"
    JC_OBJS="$JC_OBJS $obj"
done
rm -f "$JC_OUT/lib/libjavacall.a"
$EE_AR rcs "$JC_OUT/lib/libjavacall.a" $JC_OBJS

# --- 3) Build MIDP (libmidp.a + libjvm.a for MIPS) ----------------------------
mkdir -p "$MIDP_OUT"
run_make() {
    make -C "$MIDP_SRC" \
        JAVACALL_PLATFORM=ps2_mips_gcc \
        JAVACALL_OUTPUT_DIR="$JC_OUT" \
        CLDC_DIST_DIR="$CLDC_DIST" \
        PCSL_OUTPUT_DIR="$PCSL_OUT" \
        MIDP_OUTPUT_DIR="$MIDP_OUT" \
        TOOLS_DIR=/work/references/phoneme/tools \
        GNU_TOOLS_DIR="$GNU_TOOLS_DIR" \
        JDK_DIR="$JDK_DIR" \
        "$@"
}

# The libjvm.a step verifies JAVACALL_OUTPUT_DIR contains jwc_properties.ini, which
# is normally produced by the javacall port build. Here the MIDP build itself emits
# it (merged from every subsystem's properties.xml + ours) under generated/. So do a
# first pass to build libmidp.a (which generates the .ini), seed it into
# JAVACALL_OUTPUT_DIR, then run the requested target to completion.
set +e
run_make "${@:-midp}"
rc=$?
set -e
if [ $rc -ne 0 ] && [ -f "$MIDP_OUT/generated/jwc_properties.ini" ]; then
    echo "+ seeding jwc_properties.ini into JAVACALL_OUTPUT_DIR and retrying"
    cp "$MIDP_OUT/generated/jwc_properties.ini" "$JC_OUT/jwc_properties.ini"
    run_make "${@:-midp}"
fi

# --- 4) Romize an internal MIDlet suite into the MIDP ROM (Milestone B3) -------
# After the MIDP class library exists, compile+preverify our MIDlet against it,
# stage it into the romizer input, and re-run the ROM step so ROMImage.cpp picks
# it up. Launched by class name from the internal suite (see Ps2MidpMain.cpp).
#
# APP_JAVA also carries vendor-API compatibility classes that phoneME Feature does not
# ship but many commercial games require -- the Nokia UI API (com.nokia.mid.ui.*),
# implemented on top of MIDP 2.0. They romize into the system classpath so game MIDlets
# resolve them as platform classes (e.g. games extending com.nokia.mid.ui.FullCanvas).
NOKIA_API="ps2/vm/api/nokia/FullCanvas.java ps2/vm/api/nokia/DeviceControl.java \
ps2/vm/api/nokia/DirectGraphics.java ps2/vm/api/nokia/DirectUtils.java \
ps2/vm/api/nokia/DirectGraphicsImp.java \
ps2/vm/api/nokia/sound/SoundListener.java ps2/vm/api/nokia/sound/Sound.java \
ps2/vm/api/nokia/sound/AudioClip.java ps2/vm/api/nokia/sound/ToneClip.java \
ps2/vm/api/nokia/sound/OtaClip.java ps2/vm/api/nokia/sound/WavClip.java"
APP_JAVA=${APP_JAVA:-"ps2/vm/apps/GameLoader.java ps2/vm/apps/HelloCanvas.java $NOKIA_API"}
if [ -n "$APP_JAVA" ]; then
    echo "+ romizing MIDlet suite(s): $APP_JAVA"
    MIDP_OUTPUT_DIR="$MIDP_OUT" CLDC_DIST_DIR="$CLDC_DIST" JDK_DIR="$JDK_DIR" \
        bash ./docker/phoneme-cross/romize-midlet.sh $APP_JAVA
    run_make "${@:-midp}"
fi

echo "=================================================================="
echo " MIDP archives:"
ls -la "$MIDP_OUT"/bin/mips/*.a 2>/dev/null || echo "  (none yet)"
echo "=================================================================="
