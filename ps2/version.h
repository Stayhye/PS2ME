/* PS2ME — launcher version (single source of truth).
 *
 * Semantic versioning (https://semver.org): MAJOR.MINOR.PATCH.
 *   MAJOR  incompatible / milestone releases.
 *   MINOR  backwards-compatible features.
 *   PATCH  backwards-compatible fixes (a "hotfix" is a PATCH bump).
 *
 * Do NOT hand-edit the version string: it is derived from the three numbers below.
 * Bump the numbers with docker/../version.sh {major|minor|patch|hotfix}; every place
 * that shows the version (launcher header, boot log, release ELF name) reads from here.
 */
#ifndef PS2ME_VERSION_H
#define PS2ME_VERSION_H

#define PS2ME_VERSION_MAJOR 1
#define PS2ME_VERSION_MINOR 1
#define PS2ME_VERSION_PATCH 0

/* Derive "vMAJOR.MINOR.PATCH" from the numbers above (two-step stringize). */
#define PS2ME_VERSION_STR2(x) #x
#define PS2ME_VERSION_STR(x)  PS2ME_VERSION_STR2(x)
#define PS2ME_VERSION_STRING \
    "v" PS2ME_VERSION_STR(PS2ME_VERSION_MAJOR) \
    "." PS2ME_VERSION_STR(PS2ME_VERSION_MINOR) \
    "." PS2ME_VERSION_STR(PS2ME_VERSION_PATCH)

#endif /* PS2ME_VERSION_H */
