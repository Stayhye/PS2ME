#!/usr/bin/env bash
# One-command local release for PS2ME.
#
# The release ELF is built locally (the phoneME toolchain is not available in CI), so this
# helper ties the pieces together: it verifies the built stripped ELF exists, tags the current
# commit vX.Y.Z, pushes main + the tag (which triggers .github/workflows/release.yml), and
# uploads the ELF (+ bgm.adpcm) to the GitHub Release. It is idempotent with the workflow:
# whichever creates the release first wins; this script then uploads/refreshes the assets.
#
# Prerequisites: a stripped build (docker/phoneme-cross/build-release-ps2.sh) and an
# authenticated GitHub CLI (`gh auth login`). Run from a clean `main`.
#
# Usage:  tools/publish-release.sh
set -euo pipefail
cd "$(dirname "$0")/.."

VER="$(./version.sh show)"          # e.g. v1.2.0
ELF="build/ps2/PS2ME-$VER.elf"
BGM="build/ps2/bgm.adpcm"

if [ ! -f "$ELF" ]; then
  echo "error: $ELF not found -- run docker/phoneme-cross/build-release-ps2.sh first" >&2
  exit 1
fi

# Release notes = this version's CHANGELOG section.
NOTES="$(mktemp)"
trap 'rm -f "$NOTES"' EXIT
awk -v ver="${VER#v}" '
  $0 ~ "^## \\[" ver "\\]" { p = 1; next }
  p && /^## \[/ { exit }
  p { print }
' CHANGELOG.md > "$NOTES"
[ -s "$NOTES" ] || echo "PS2ME $VER" > "$NOTES"

# Tag (annotated) if it does not exist yet, then push main and the tag.
if ! git rev-parse -q --verify "refs/tags/$VER" >/dev/null; then
  git tag -a "$VER" -m "PS2ME $VER"
fi
git push origin main
git push origin "$VER"

# Assemble the asset list (bgm.adpcm is optional).
assets=("$ELF")
[ -f "$BGM" ] && assets+=("$BGM")

# Create the release, or update assets/notes if the workflow already created it.
if gh release view "$VER" >/dev/null 2>&1; then
  gh release upload "$VER" "${assets[@]}" --clobber
  gh release edit "$VER" --notes-file "$NOTES"
else
  gh release create "$VER" "${assets[@]}" --title "PS2ME $VER" --notes-file "$NOTES"
fi

echo "Published PS2ME $VER"
