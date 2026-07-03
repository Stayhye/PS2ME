#!/bin/bash
# Romize a MIDlet into the MIDP ROM image as an *internal* suite, so it runs on
# the PlayStation 2 with an empty rmfs (no JAR/JAD install, no file I/O).
#
# This is the MIDP analogue of romize-app.sh (Milestone A, CLDC-only). The MIDP
# romizer romizes every class in $(MIDP_CLASSES_ZIP), which the build jars from
# $(CLASSBINDIR) = $MIDP_OUTPUT_DIR/classes (MIDP.gmk / Defs.gmk). Unlike the
# CLDC build there is no ROM_GEN_CLASSPATH_APPEND hook, so instead of appending
# to the romizer classpath we inject the MIDlet into the romizer *input*:
#
#   1. javac the MIDlet against the MIDP class library (classes.zip carries both
#      the MIDP profile javax.microedition.* and the CLDC java.lang.*).
#   2. preverify it (adds the StackMaps CLDC requires).
#   3. drop the preverified .class files into CLASSBINDIR (the compilefiles macro
#      is additive -- it never cleans CLASSBINDIR outside the `clean` target).
#   4. delete classes.zip + the romized artifacts so the next `make midp`
#      re-jars classes.zip (now including our MIDlet) and re-romizes ROMImage.
#
# The MIDlet then lives in the ROM as part of the internal suite and is launched
# by class name: runMidlet "internal" <class> maps to INTERNAL_SUITE_ID without
# touching _suites.dat (runMidlet.c), and CldcMIDletSuiteLoader builds it via
# InternalMIDletSuiteImpl -- no storage, no JAR. See Ps2MidpMain.cpp for the argv.
#
# Our class lands in ROMImage.o (an EXE object linked separately by the ELF
# build), not in libmidp/libjvm, so the merged archives are left untouched.
#
# Invoked by build-midp-ps2.sh between two `make midp` passes (it needs the MIDP
# class library from the first pass to compile against). Env in: JDK_DIR,
# MIDP_OUTPUT_DIR, CLDC_DIST_DIR.
#
# Usage (inside container): romize-midlet.sh <MIDlet.java> [<extra.java>...]
set -e

JDK=${JDK_DIR:-/opt/java/openjdk}
MIDP_OUT=${MIDP_OUTPUT_DIR:-/build/midp_ps2_out}
CLDC_DIST=${CLDC_DIST_DIR:-/build/cldc_ps2_out/ps2_mips/dist}

MIDP_CLASSES_ZIP=$MIDP_OUT/classes.zip
CLASSBINDIR=$MIDP_OUT/classes
PREVERIFY=$CLDC_DIST/bin/preverify

if [ $# -lt 1 ]; then
    echo "usage: romize-midlet.sh <MIDlet.java> [<extra.java>...]" >&2
    exit 2
fi
if [ ! -f "$MIDP_CLASSES_ZIP" ]; then
    echo "romize-midlet: $MIDP_CLASSES_ZIP missing (build MIDP first)" >&2
    exit 1
fi

WORK=/tmp/romize_midlet
rm -rf "$WORK"; mkdir -p "$WORK/tmp" "$WORK/pv"

echo "+ romize-midlet: javac $*"
$JDK/bin/javac -bootclasspath "$MIDP_CLASSES_ZIP" -source 1.4 -target 1.4 \
    -d "$WORK/tmp" "$@"

echo "+ romize-midlet: preverify (CLDC StackMaps)"
$PREVERIFY -classpath "$MIDP_CLASSES_ZIP" -d "$WORK/pv" "$WORK/tmp" >/dev/null

echo "+ romize-midlet: inject into CLASSBINDIR + invalidate ROM artifacts"
cp -r "$WORK/pv/." "$CLASSBINDIR/"
rm -f "$MIDP_CLASSES_ZIP" \
      "$MIDP_OUT/ROMImage.cpp" "$MIDP_OUT/nativeFunctionTable.cpp" \
      "$MIDP_OUT/obj/mips/ROMImage.o" "$MIDP_OUT/obj/mips/nativeFunctionTable.o"

echo "+ romize-midlet: done -> $(basename "$1") staged for the MIDP ROM"
