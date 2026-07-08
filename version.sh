#!/usr/bin/env bash
# PS2ME semantic-versioning helper. The version lives in ONE place -- ps2/version.h,
# as three integers (MAJOR/MINOR/PATCH). Everything that shows the version (launcher
# header, boot log, release ELF name) derives from that header, so this script only
# has to edit the three numbers.
#
# Usage:
#   ./version.sh                 show the current version
#   ./version.sh show            same
#   ./version.sh major           X.y.z -> (X+1).0.0   (milestone / breaking)
#   ./version.sh minor           x.Y.z -> x.(Y+1).0   (backwards-compatible feature)
#   ./version.sh patch           x.y.Z -> x.y.(Z+1)   (backwards-compatible fix)
#   ./version.sh hotfix          alias of patch
set -e

VH="$(cd "$(dirname "$0")" && pwd)/ps2/version.h"
[ -f "$VH" ] || { echo "version.h not found at $VH" >&2; exit 1; }

field() { sed -nE "s/.*define PS2ME_VERSION_$1[[:space:]]+([0-9]+).*/\1/p" "$VH" | head -1; }
MAJ=$(field MAJOR); MIN=$(field MINOR); PAT=$(field PATCH)

case "${1:-show}" in
    major)        MAJ=$((MAJ + 1)); MIN=0; PAT=0 ;;
    minor)        MIN=$((MIN + 1)); PAT=0 ;;
    patch|hotfix) PAT=$((PAT + 1)) ;;
    show)         echo "v$MAJ.$MIN.$PAT"; exit 0 ;;
    *) echo "usage: $0 {major|minor|patch|hotfix|show}" >&2; exit 1 ;;
esac

sed -i -E \
    -e "s/(define PS2ME_VERSION_MAJOR ).*/\1$MAJ/" \
    -e "s/(define PS2ME_VERSION_MINOR ).*/\1$MIN/" \
    -e "s/(define PS2ME_VERSION_PATCH ).*/\1$PAT/" \
    "$VH"

echo "bumped to v$MAJ.$MIN.$PAT  (rebuild to apply)"
