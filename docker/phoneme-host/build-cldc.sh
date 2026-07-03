#!/bin/bash
# Build the CLDC host romizer (Stage A) and run romization.
#
# Source stays on the Windows bind mount (/work); the build space lives on a
# CONTAINER-NATIVE path (/build, backed by the persistent `phoneme_build` named
# volume). Two reasons:
#   - Persistence: the built VM/romgen survives `docker run --rm`, so later runs
#     re-run only the romize step (fast iteration).
#   - Native stat(): preverify enumerates its tmpclasses output dir via
#     stat()/S_ISDIR; on the bind mount a 32-bit stat() fails with EOVERFLOW, so
#     the build space must NOT be on the bind mount. romgen reading the source
#     config files over the bind mount is handled by patch #4 (_FILE_OFFSET_BITS).
#
# Usage (invoked inside the container; see run.sh / the -v flags):
#   build-cldc.sh                # full build + romize
#   build-cldc.sh <make-target>  # e.g. a specific target to re-run
set -e

cd /work
./docker/phoneme-host/patch-phoneme.sh references/phoneme

export JDK_DIR=/opt/java/openjdk
WS=/work/references/phoneme/cldc
BS=${JVMBUILDSPACE:-/build/cldc_out}
mkdir -p "$BS"

make -C references/phoneme/cldc/build/linux_i386 \
    JVMWorkSpace="$WS" \
    JVMBuildSpace="$BS" \
    JDK_DIR="$JDK_DIR" \
    "$@"
