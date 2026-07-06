#!/bin/bash
# Link the PlayStation 2 MIDP ELF (j2me-midp.elf) for Milestone B1: the phoneME
# Feature MIDP + CLDC VM booting into the AMS event loop on the Emotion Engine.
#
# Same shape as build-elf-ps2.sh (Milestone A), but links against the merged MIDP
# archive libjvm.a (= libmidp + libcldc_vm + PCSL) instead of the CLDC-only libs,
# and our entry hands control to JavaTask() rather than JVM_Start().
#
# Pipeline:
#   1. EXE_OBJS   - ROMImage.o / nativeFunctionTable.o, already compiled with the
#                   target flags by build-midp-ps2.sh (reused from the volume).
#   2. properties - generate javacall_static_properties.c from the merged
#                   jwc_properties.ini and compile the reused share/properties +
#                   share/utils C sources (static-properties path, no file I/O).
#   3. libjavacall- our contract+hal+platform port (Milestone A modules + the B1
#                   lcd/events/lifecycle/dir/random modules + stubs), compiled for
#                   MIPS. Objects link directly (not archived) so the static
#                   backend registrars are never dropped as unreferenced.
#   4. Ps2MidpMain.o - our PS2 entrypoint (main() -> platform init -> JavaTask).
#   5. link       - ps2sdk crt0 + everything in one --start-group.
#
# Usage (inside container): docker/phoneme-cross/build-elf-midp-ps2.sh
set -e

cd /work

PS2SDK=${PS2SDK:-/usr/local/ps2dev/ps2sdk}
GNU_TOOLS_DIR=${GNU_TOOLS_DIR:-/usr/local/ps2dev/ee-crosswrap}
EE_CXX=$GNU_TOOLS_DIR/bin/g++
EE_CC=$GNU_TOOLS_DIR/bin/gcc

JC_OUT=/build/javacall_ps2
PCSL_OUT=/build/pcsl_ps2_out/linux_mips
MIDP_OUT=/build/midp_ps2_out
CLDC_DIST=/build/cldc_ps2_out/ps2_mips/dist   # kni.h for the GameLoader KNI bridge
LIBJVM=$MIDP_OUT/bin/mips/libjvm.a
EXE_OBJ_DIR=$MIDP_OUT/obj/mips
JC_SHARE=/work/references/phoneme/javacall/implementation/share

OUT=/work/build/ps2
mkdir -p "$OUT"

echo "=================================================================="
echo " j2me-ps2 : linking Milestone B1 MIDP ELF"
echo "=================================================================="

# --- 0) Refresh the git-ignored phoneME tree (patches + our overlay) ----------
bash ./docker/phoneme-host/patch-phoneme.sh references/phoneme >/dev/null 2>&1 || true
cp -r ps2/phoneme/. references/phoneme/ 2>/dev/null || true

# --- 1) EXE_OBJS: reuse the target-flag objects from the MIDP build ------------
echo "+ [1/5] EXE_OBJS (ROMImage / nativeFunctionTable)"
cp "$EXE_OBJ_DIR"/ROMImage.o "$EXE_OBJ_DIR"/nativeFunctionTable.o "$OUT/"

# --- 2) properties: static-properties table + reused share C sources ----------
# MAIN_MEMORY_CHUNK_SIZE is the single contiguous pool PCSL mallocs and out of
# which BOTH the Java heap and all MIDP C structures are carved. The stock 8 MB
# overflows during midpInitialize() on this build (VM heap + chameleon LCDUI),
# yielding "out of memory". The EE has ~28 MB free after our 3.3 MB ELF loads, so
# enlarge the pool. Applied by rewriting the value as the static table is generated
# (getInternalProperty -> javacall_get_property reads exactly this table).
echo "+ [2/5] properties (static table + share/properties + share/utils)"
# EE RAM is 32 MB. The ELF occupies ~0x100000..0x3ba000 (_end ~3.9 MB); the newlib
# heap (MEMORY_MODULE=malloc puts the Java heap, all MIDP structures AND the rmfs
# there) runs from _end up to the stack at the top of RAM -> ~28 MB usable. The 20 MB
# pool + 8 MB rmfs = 28 MB left ZERO headroom, so a video/install malloc grew the
# heap into the stack and corrupted chunk headers -> crash in _free_r's unlink.
# 12 MB pool + rmfs + ELF leaves a wide margin; J2ME games target 1-4 MB of Java heap.
MIDP_HEAP_BYTES=${MIDP_HEAP_BYTES:-12582912}    # 12 MB pool
STATIC_C="$OUT/javacall_static_properties.c"
sed "s/^MAIN_MEMORY_CHUNK_SIZE.*/MAIN_MEMORY_CHUNK_SIZE = $MIDP_HEAP_BYTES/" \
    "$JC_OUT/jwc_properties.ini" \
    | awk -f docker/phoneme-cross/gen-static-properties.awk > "$STATIC_C"
echo "    MAIN_MEMORY_CHUNK_SIZE = $MIDP_HEAP_BYTES bytes"

C_FLAGS="-D_EE -DMIPS -G0 -O2 -std=gnu99 -fpermissive \
    -I$JC_OUT/inc -I$JC_SHARE/properties -I$JC_SHARE/properties/inc \
    -I$JC_SHARE/utils -I$JC_SHARE/utils/inc \
    -I$PS2SDK/ee/include -I$PS2SDK/common/include"
SHARE_SRCS="
    $JC_SHARE/properties/javacall_properties.c
    $JC_SHARE/properties/javacall_config_db.c
    $JC_SHARE/properties/javacall_db.c
    $JC_SHARE/utils/javautil_string.c
    $JC_SHARE/utils/javautil_unicode.c
    $JC_SHARE/utils/javautil_printf.c
    $JC_SHARE/utils/javautil_stdio.c
    $STATIC_C
"
SHARE_OBJS=""
for src in $SHARE_SRCS; do
    obj="$OUT/sh_$(basename "$src" .c).o"
    $EE_CC $C_FLAGS -c "$src" -o "$obj"
    SHARE_OBJS="$SHARE_OBJS $obj"
done

# --- 3) libjavacall: our port, compiled for MIPS ------------------------------
echo "+ [3/5] libjavacall (contract + hal + platform)"
JC=/work/ps2/javacall
JC_FLAGS="-D_EE -DMIPS -G0 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -I$JC_OUT/inc -I$PS2SDK/ee/include -I$PS2SDK/common/include -I$PS2SDK/ports/include \
    -I/work/vendors -I/work/assets"
JC_SRCS="
    contract/javacall_logging.cpp
    contract/javacall_os.cpp
    contract/javacall_time.cpp
    contract/javacall_memory.cpp
    contract/javacall_lcd.cpp
    contract/javacall_events.cpp
    contract/javacall_eventqueue.cpp
    contract/javacall_lifecycle.cpp
    contract/javacall_dir.cpp
    contract/javacall_random.cpp
    contract/javacall_string.cpp
    contract/javacall_stubs.cpp
    hal/Logger.cpp
    hal/OsCore.cpp
    hal/SystemClock.cpp
    hal/TimerService.cpp
    hal/MemoryManager.cpp
    hal/LcdDevice.cpp
    hal/EventQueue.cpp
    hal/MidletLifecycle.cpp
    hal/Storage.cpp
    hal/RandomSource.cpp
    hal/Keypad.cpp
    hal/GameStorage.cpp
    platform/StdoutSink.cpp
    platform/Ps2CpuCache.cpp
    platform/Ps2AlarmTimer.cpp
    platform/Ps2Clock.cpp
    platform/SystemHeap.cpp
    platform/GsDisplay.cpp
    platform/Ps2Framebuffer.cpp
    platform/NullEventLock.cpp
    platform/Ps2Pad.cpp
    platform/Ps2Storage.cpp
    platform/Ps2HostStorage.cpp
    platform/Ps2Audio.cpp
    platform/MidletIcon.cpp
    platform/IconCache.cpp
    platform/Ps2Frontend.cpp
"
JC_OBJS=""
for src in $JC_SRCS; do
    obj="$OUT/jc_$(echo "$src" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    $EE_CXX $JC_FLAGS -c "$JC/$src" -o "$obj"
    JC_OBJS="$JC_OBJS $obj"
done

# --- 4) Ps2MidpMain.o + GameLoaderKni.o: our entrypoint + KNI bridge -----------
echo "+ [4/5] Ps2MidpMain.o (entrypoint) + GameLoaderKni.o (KNI bridge)"
MAIN_FLAGS="-D_EE -DMIPS -G0 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -I$JC_OUT/inc -I$PS2SDK/ee/include -I$PS2SDK/common/include"
$EE_CXX $MAIN_FLAGS -c /work/ps2/vm/Ps2MidpMain.cpp -o "$OUT/Ps2MidpMain.o"
# The KNI bridge needs the VM's kni.h (from the CLDC dist). Its C symbols are what
# the generated nativeFunctionTable.cpp references for GameLoader's native methods.
$EE_CXX $MAIN_FLAGS -I$CLDC_DIST/include -c /work/ps2/vm/GameLoaderKni.cpp -o "$OUT/GameLoaderKni.o"

# --- 5) Link the ELF ----------------------------------------------------------
echo "+ [5/5] link j2me-midp.elf"
LINKFILE=$PS2SDK/ee/startup/linkfile
$EE_CXX -T"$LINKFILE" -O2 -o "$OUT/j2me-midp.elf" \
    -L$PS2SDK/ee/lib -L$PS2SDK/ports/lib -Wl,-zmax-page-size=128 \
    -Wl,--start-group \
        "$OUT/Ps2MidpMain.o" "$OUT/GameLoaderKni.o" \
        "$OUT/ROMImage.o" "$OUT/nativeFunctionTable.o" \
        $JC_OBJS $SHARE_OBJS \
        "$LIBJVM" \
        "$PCSL_OUT/lib/libpcsl_file.a" \
        "$PCSL_OUT/lib/libpcsl_memory.a" \
        "$PCSL_OUT/lib/libpcsl_print.a" \
        "$PCSL_OUT/lib/libpcsl_string.a" \
        "$PCSL_OUT/lib/libpcsl_network.a" \
        "$PCSL_OUT/lib/libpcsl_escfilenames.a" \
        -ldraw -lgraph -lpacket -ldma -lmath3d \
        -lps2_drivers -laudsrv -lmtap -lpad -lpatches \
        -lkernel -lc -lm -lgcc \
    -Wl,--end-group

echo "=================================================================="
ls -la "$OUT/j2me-midp.elf"
echo " OK -> build/ps2/j2me-midp.elf"
echo "=================================================================="
