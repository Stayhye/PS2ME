#!/bin/bash
# Romize a standalone Java app into the CLDC ROM image, so it runs on the PS2
# without any file I/O (the class lives in ROM alongside the system classes and
# is launched by name from ps2/vm/Ps2Main.cpp).
#
# Steps: javac (source/target 1.4 against the CLDC classes.zip) -> preverify
# (adds the StackMaps CLDC requires) -> pack into a zip (romgen accepts a jar/zip
# on the classpath, not a directory) -> regenerate target/generated/ROMImage.cpp
# with the app appended to the romizer classpath (the build system's built-in
# ROM_GEN_CLASSPATH_APPEND hook, so no phoneME files are touched).
#
# Runs inside the `phoneme-cross` container, after build-cldc-ps2.sh has produced
# romgen + preverify + classes.zip in the phoneme_build volume. The final ELF link
# (build-elf-ps2.sh) then compiles the refreshed ROMImage.cpp and relinks.
#
# Usage (inside container): romize-app.sh <path-to-.java>
set -e

APP_JAVA=${1:?usage: romize-app.sh <app.java>}
JDK=${JDK_DIR:-/opt/java/openjdk}
GNU_TOOLS_DIR=${GNU_TOOLS_DIR:-/usr/local/ps2dev/ee-crosswrap}

BS=/build/cldc_ps2_out
PROD=$BS/ps2_mips/target/product
GEN=$BS/ps2_mips/target/generated
PREVERIFY=$BS/ps2_mips/dist/bin/preverify
CLASSES_ZIP=$BS/classes.zip

WORK=/tmp/romize_app
rm -rf "$WORK"; mkdir -p "$WORK/tmp" "$WORK/pv"

echo "+ romize: javac $(basename "$APP_JAVA")"
$JDK/bin/javac -bootclasspath "$CLASSES_ZIP" -source 1.4 -target 1.4 \
    -d "$WORK/tmp" "$APP_JAVA" 2>/dev/null

echo "+ romize: preverify (CLDC StackMaps)"
$PREVERIFY -classpath "$CLASSES_ZIP" -d "$WORK/pv" "$WORK/tmp" >/dev/null

echo "+ romize: pack + regenerate ROMImage.cpp"
( cd "$WORK/pv" && $JDK/bin/jar cf "$WORK/app.zip" . )
rm -f "$GEN/ROMImage.cpp"
make -C "$PROD" \
    JAVACALL_OUTPUT_DIR=/build/javacall_ps2 PCSL_OUTPUT_DIR=/build/pcsl_ps2_out \
    JDK_DIR="$JDK" GNU_TOOLS_DIR="$GNU_TOOLS_DIR" \
    ROM_GEN_CLASSPATH_APPEND="$WORK/app.zip" \
    ../generated/ROMImage.cpp >/dev/null

echo "+ romize: done -> $(basename "$APP_JAVA") is in the ROM image"
