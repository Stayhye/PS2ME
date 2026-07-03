#!/bin/bash
# Link the final PlayStation 2 ELF (j2me.elf) for Milestone A: the phoneME
# Feature CLDC VM booting on the Emotion Engine.
#
# Runs inside the `phoneme-cross` container, which carries BOTH the host build
# tools (to produce the EXE_OBJS with the exact target flags) and the ps2sdk EE
# toolchain + libraries (to link a real PS2 ELF). Consumes the VM archives and
# ROM image already built into the `phoneme_build` volume by build-cldc-ps2.sh.
#
# Pipeline:
#   1. EXE_OBJS   - ROMImage.o / NativesTable.o / jvmspi.o, asked of the phoneME
#                   build system itself so they carry the identical target flags.
#   2. libjavacall- our contract+hal+platform port, compiled for MIPS. Objects are
#                   linked directly (not archived) so the static registrars that
#                   wire up the backends are never dropped as unreferenced.
#   3. Ps2Main.o  - our PS2 entrypoint, compiled with the VM's flag set so jvm.h's
#                   JvmPathChar / ABI matches the archives.
#   4. link       - ps2sdk crt0 + everything, in one --start-group to resolve the
#                   VM <-> javacall <-> PCSL circular references.
#
# Usage (inside container): docker/phoneme-cross/build-elf-ps2.sh
set -e

cd /work

PS2SDK=${PS2SDK:-/usr/local/ps2dev/ps2sdk}
GNU_TOOLS_DIR=${GNU_TOOLS_DIR:-/usr/local/ps2dev/ee-crosswrap}
EE_CXX=$GNU_TOOLS_DIR/bin/g++
EE_AR=$GNU_TOOLS_DIR/bin/ar

BS=/build/cldc_ps2_out
WS=/work/references/phoneme/cldc
JC_OUT=/build/javacall_ps2
PCSL_OUT=/build/pcsl_ps2_out/linux_mips
GEN=$BS/ps2_mips/target/generated
PROD=$BS/ps2_mips/target/product
VMLIB=$BS/ps2_mips/dist/lib/libcldc_vm.a
VMXLIB=$BS/ps2_mips/dist/lib/libcldc_vmx.a       # BSDSocket etc (socket natives)
VMTESTLIB=$BS/ps2_mips/dist/lib/libcldc_vmtest.a # ReflectNatives (parity w/ ref link)

OUT=/work/build/ps2        # scratch for our objects + the ELF
mkdir -p "$OUT"

echo "=================================================================="
echo " j2me-ps2 : linking Milestone A ELF"
echo "=================================================================="

# --- 0) Refresh the git-ignored phoneME tree (patches + our overlay) ----------
bash ./docker/phoneme-host/patch-phoneme.sh references/phoneme >/dev/null 2>&1 || true
cp -r ps2/phoneme/. references/phoneme/

# --- 0.5) Optionally romize an app into the ROM image (APP_JAVA=path/to.java) --
# Ps2Main.cpp launches "HelloWorld" by name, so it must be in the ROM. Regenerates
# target/generated/ROMImage.cpp with the app; step [1] then compiles the fresh one.
APP_JAVA=${APP_JAVA:-ps2/vm/apps/HelloWorld.java}
if [ -n "$APP_JAVA" ] && [ -f "$APP_JAVA" ]; then
    echo "+ [0.5] romize app: $APP_JAVA"
    GNU_TOOLS_DIR="$GNU_TOOLS_DIR" JDK_DIR=/opt/java/openjdk \
        bash ./docker/phoneme-cross/romize-app.sh "/work/$APP_JAVA"
fi

# --- 1) EXE_OBJS: let the build system compile them with the target flags ------
echo "+ [1/4] EXE_OBJS (ROMImage / NativesTable / jvmspi)"
make -C "$PROD" \
    JAVACALL_OUTPUT_DIR="$JC_OUT" PCSL_OUTPUT_DIR="/build/pcsl_ps2_out" \
    GNU_TOOLS_DIR="$GNU_TOOLS_DIR" JDK_DIR=/opt/java/openjdk \
    jvmspi.o NativesTable.o ROMImage.o >/dev/null
cp "$PROD"/{jvmspi.o,NativesTable.o,ROMImage.o} "$OUT/"

# --- 2) libjavacall: our port, compiled for MIPS -------------------------------
echo "+ [2/4] libjavacall (contract + hal + platform)"
JC=/work/ps2/javacall
JC_FLAGS="-D_EE -DMIPS -G0 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -I$JC_OUT/inc -I$PS2SDK/ee/include -I$PS2SDK/common/include"
JC_SRCS="
    contract/javacall_logging.cpp
    contract/javacall_os.cpp
    contract/javacall_time.cpp
    contract/javacall_memory.cpp
    hal/Logger.cpp
    hal/OsCore.cpp
    hal/SystemClock.cpp
    hal/TimerService.cpp
    hal/MemoryManager.cpp
    platform/StdoutSink.cpp
    platform/Ps2CpuCache.cpp
    platform/Ps2AlarmTimer.cpp
    platform/PosixClock.cpp
    platform/SystemHeap.cpp
"
JC_OBJS=""
for src in $JC_SRCS; do
    obj="$OUT/jc_$(echo "$src" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    $EE_CXX $JC_FLAGS -c "$JC/$src" -o "$obj"
    JC_OBJS="$JC_OBJS $obj"
done

# --- 3) Ps2Main.o: our entrypoint, with the VM's flag set ----------------------
echo "+ [3/4] Ps2Main.o (entrypoint)"
VM_DEFS="-DMIPS -G0 -DUSE_UNICODE_FOR_FILENAMES=1 -DSUPPORTS_MONOTONIC_CLOCK=0 \
    -DPRODUCT -D_FILE_OFFSET_BITS=64 -Wno-narrowing -DGCC -DLINUX \
    -DREQUIRES_JVMCONFIG_H=1 -DHARDWARE_LITTLE_ENDIAN=1 -DHOST_LITTLE_ENDIAN=1 \
    -DROMIZING=1 -DJVM_RELEASE_VERSION='\"1.1\"' -DJVM_BUILD_VERSION='\"internal\"' \
    -DJVM_NAME='\"phoneME Feature VM\"' \
    -fno-gnu-keywords -fno-operator-names -fno-exceptions -fno-optional-diags -fno-rtti"
VM_INCS="-I$GEN \
    -I$WS/src/vm/share/compiler -I$WS/src/vm/share/debugger \
    -I$WS/src/vm/share/handles -I$WS/src/vm/share/memory \
    -I$WS/src/vm/share/interpreter -I$WS/src/vm/share/isolate \
    -I$WS/src/vm/share/natives -I$WS/src/vm/share/reflection \
    -I$WS/src/vm/share/runtime -I$WS/src/vm/share/utilities \
    -I$WS/src/vm/share/ROM -I$WS/src/vm/share/verifier \
    -I$WS/src/vm/share/float -I$WS/src/vm/os/utilities \
    -I$WS/src/vm/share/memoryprofiler -I$WS/src/vm/os/javacall \
    -I$WS/src/midp -I$WS/src/vm/cpu/c \
    -I$PCSL_OUT/inc -I$JC_OUT/inc \
    -I$PS2SDK/ee/include -I$PS2SDK/common/include"
eval $EE_CXX -D_EE -O2 $VM_DEFS $VM_INCS -c /work/ps2/vm/Ps2Main.cpp -o "$OUT/Ps2Main.o"

# --- 4) Link the ELF -----------------------------------------------------------
echo "+ [4/4] link j2me.elf"
LINKFILE=$PS2SDK/ee/startup/linkfile
$EE_CXX -T"$LINKFILE" -O2 -o "$OUT/j2me.elf" \
    -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128 \
    -Wl,--start-group \
        "$OUT/Ps2Main.o" "$OUT/ROMImage.o" "$OUT/NativesTable.o" "$OUT/jvmspi.o" \
        $JC_OBJS \
        "$VMXLIB" "$VMTESTLIB" "$VMLIB" \
        "$PCSL_OUT/lib/libpcsl_file.a" \
        "$PCSL_OUT/lib/libpcsl_memory.a" \
        "$PCSL_OUT/lib/libpcsl_print.a" \
        "$PCSL_OUT/lib/libpcsl_string.a" \
        "$PCSL_OUT/lib/libpcsl_network.a" \
        "$PCSL_OUT/lib/libpcsl_escfilenames.a" \
        -lkernel -lc -lm -lgcc \
    -Wl,--end-group

echo "=================================================================="
ls -la "$OUT/j2me.elf"
echo " OK -> build/ps2/j2me.elf"
echo "=================================================================="
