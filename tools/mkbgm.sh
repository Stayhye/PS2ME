#!/usr/bin/env bash
# Convert the launcher menu BGM (assets/bgm.mp3) into a looping PlayStation 2 SPU2 ADPCM
# sample (assets/bgm.adpcm). The sample is uploaded once into the 2 MB SPU2 local RAM and
# played on a hardware voice via audsrv -- it loops in hardware with ZERO EE CPU and ZERO
# EE RAM (the EE-side buffer is freed right after the upload). That is why the menu can play
# music without touching the software mixer or the Java heap budget.
#
# The real limit is the IOP heap, NOT SPU2 RAM: audsrv_load_adpcm DMA-stages the whole
# sample through a temporary IOP buffer of its size, and the IOP has only 2 MB (mostly the
# USB / memory-card / audio IRX). A 1.3 MB sample (22050 Hz, ~1:45) fails there with
# -AUDSRV_ERR_OUT_OF_MEMORY (-4). Ps2Audio::loadBgm logs the free IOP heap ("[bgm] IOP heap
# free ~N KB"); size the track under it. At 11025 Hz mono a ~1:45 track is ~650 KB. Size
# ~= duration * RATE * 16/28 bytes. The pitch is written into the .adp header by ps2adpcm
# (from RATE) and read back on load, so the player does NOT hard-code any rate.
#
# Runs inside the phoneme-cross container (ships ps2adpcm; installs ffmpeg on demand):
#   MSYS_NO_PATHCONV=1 docker run --rm -v "D:/PS2DEV/ports/j2me":/work \
#       phoneme-cross bash /work/tools/mkbgm.sh
set -e

SRC=${SRC:-/work/assets/bgm.mp3}
OUT=${OUT:-/work/assets/bgm.adpcm}
RATE=${RATE:-11025}

[ -f "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || {
    echo "+ installing ffmpeg"; apt-get update -qq && apt-get install -y -qq ffmpeg; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# MP3 -> raw signed 16-bit little-endian, mono, RATE Hz (what ps2adpcm consumes).
ffmpeg -v error -y -i "$SRC" -ac 1 -ar "$RATE" -f s16le "$TMP/bgm.pcm"
# PCM -> SPU2 ADPCM, loop from sample 0 (-l0) so the SPU2 voice loops the whole track.
/usr/local/ps2dev/ps2sdk/bin/ps2adpcm "$TMP/bgm.pcm" "$OUT" -l0

echo "wrote $OUT ($(( $(stat -c%s "$OUT") / 1024 )) KB, ${RATE} Hz mono, looping)"
