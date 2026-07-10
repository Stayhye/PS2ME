#!/bin/bash
# build-host-bench.sh -- FASE 1 of the PS2ME interpreter-performance front:
# a fast HOST benchmark of the C bytecode interpreter, so we can iterate on
# interpreter optimizations (PGO, threaded dispatch, ...) in SECONDS on x86
# instead of minutes-per-cycle on PS2 hardware.
#
# WHY THIS IS A VALID PROXY: the `linux_c` CLDC target ("intended to validate the
# C interpreter loop") compiles the SAME cpu/c/Interpreter_c.cpp we ship on the PS2
# (ENABLE_C_INTERPRETER=true, INTERPRETER_GENERATOR=false, COMPILER=false, arch=c).
# We force host_arch=i386 so the host VM is 32-bit -- same pointer width / object
# layout as the r5900 (N32) -- making it as faithful as a non-MIPS host can be.
# Correctness/UB transfer 100% (identical C); the MAGNITUDE of a speedup does NOT
# transfer 1:1 (x86 has strong branch prediction / OoO that the in-order r5900 lacks),
# so the host FILTERS what helps and validates correctness in seconds -- the FINAL
# number always comes from HW (same -DPS2ME_PROFILE toggle, user runs & reports).
#
# METRIC = bytecodes/second. The executed-bytecode COUNT is deterministic for a fixed
# .class, so we measure it ONCE with a -DPS2ME_PROFILE build (N) and measure only TIME
# on a clean production build (T) -> bytecodes/s = N/T. That way the counter's own
# overhead never taxes the timed number, and N is identical across interpreter variants.
#
# UBSan remains the correctness GATE for any interpreter change (romgen proxy); this
# script is the THROUGHPUT gate. They are complementary.
#
# Usage (inside the phoneme-host or phoneme-cross container):
#   docker run --rm -i -v D:/PS2DEV/ports/j2me:/work -v phoneme_build:/build \
#       -w /work phoneme-host bash docker/phoneme-cross/build-host-bench.sh [label]
# Env knobs:  SCALE (iteration multiplier, default 1)   REPS (timed runs, default 5)
#             SKIP_BUILD=1 (reuse existing trees, just re-time)
set -e
cd /work

LABEL="${1:-baseline}"
SCALE="${SCALE:-1}"
REPS="${REPS:-5}"
export JDK_DIR="${JDK_DIR:-/opt/java/openjdk}"

CLEAN=/build/cldc_linuxc_out
PROF=/build/cldc_linuxc_prof
WS=/work/references/phoneme/cldc

build_tree() {  # $1=buildspace  $2..=extra make args
  local bs="$1"; shift
  make -C references/phoneme/cldc/build/linux_c \
      JVMWorkSpace="$WS" JVMBuildSpace="$bs" \
      JDK_DIR="$JDK_DIR" host_arch=i386 "$@" >/dev/null 2>&1
}

if [ -z "$SKIP_BUILD" ]; then
  echo "== [1/4] patch + build clean (time) and profile (count) trees =="
  ./docker/phoneme-host/patch-phoneme.sh references/phoneme >/dev/null
  build_tree "$CLEAN"
  build_tree "$PROF" PS2ME_PROFILE=true
fi

echo "== [2/4] compile + preverify InterpBench (fixed .class across variants) =="
BIN="$CLEAN/linux_c/dist/bin"
CLASSES="$CLEAN/classes.zip"
BW=/tmp/benchwork; rm -rf "$BW"; mkdir -p "$BW/src" "$BW/pre"
"$JDK_DIR/bin/javac" -bootclasspath "$CLASSES" -source 1.4 -target 1.4 \
    -d "$BW/src" bench/InterpBench.java 2>/dev/null
"$BIN/preverify" -classpath "$CLASSES" -d "$BW/pre" "$BW/src" >/dev/null 2>&1

echo "== [3/4] N = executed bytecodes (profile build, deterministic) =="
PBIN="$PROF/linux_c/dist/bin"
N=$("$PBIN/cldc_vm" -cp "$BW/pre" InterpBench "$SCALE" 2>&1 >/dev/null \
      | sed -n 's/.*bytecodes=\([0-9]*\).*/\1/p')
echo "   N = $N bytecodes (scale=$SCALE)"

echo "== [4/4] T = min user-time over $REPS clean-build runs =="
best=""
for i in $(seq 1 "$REPS"); do
  t=$( { TIMEFORMAT='%3U'; time "$BIN/cldc_vm" -cp "$BW/pre" InterpBench "$SCALE" >/dev/null 2>&1; } 2>&1 )
  echo "   run $i: user=${t}s"
  if [ -z "$best" ] || awk "BEGIN{exit !($t < $best)}"; then best="$t"; fi
done

echo "=================================================================="
echo " HOST BENCH  label=$LABEL  scale=$SCALE"
echo "   bytecodes N = $N"
echo "   min user T  = ${best}s"
awk "BEGIN{ if ($best>0) printf \"   throughput  = %.2f Mbytecodes/s\n\", $N/$best/1e6 }"
echo "=================================================================="
