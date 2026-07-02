#!/bin/sh
# Run a command inside the phoneME host build container (Stage A).
# The project root is mounted at /work, so the phoneME source lives at
# /work/references/phoneme. Nothing SDK-sized lives in the image.
#
# Usage:
#   ./docker/phoneme-host/run.sh                 # interactive shell
#   ./docker/phoneme-host/run.sh "cd /work/references/phoneme/pcsl && make ..."
set -e

DIR="$(cd "$(dirname "$0")/../.." && pwd)"

# On Windows/Git Bash, bind mounts need a Windows-style path.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) MP="$(cd "$DIR" && pwd -W)";;
  *)                    MP="$DIR";;
esac

MSYS_NO_PATHCONV=1 exec docker run --rm -it \
  -v "$MP":/work -w /work phoneme-host \
  bash -c "${*:-bash}"
