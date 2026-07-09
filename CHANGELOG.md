# Changelog

All notable changes to **PS2ME** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The version number lives in a single source of truth, `ps2/version.h`, and is bumped
with `./version.sh {major|minor|patch}`.

## [1.2.0] - 2026-07-09

### Added
- Looping menu background music, played straight from the SPU2 as a hardware ADPCM
  voice (built offline by `tools/mkbgm.sh`), so it costs zero EE CPU and RAM.
- Opt-in in-game FPS counter (Settings toggle, default off), drawn outside the game
  canvas on the pillarbox bar.
- A "Menu music" toggle in Settings (default on) to enable or mute the background music,
  applied immediately and persisted to the memory card.

### Changed
- The Settings list now scrolls: it shows the rows that fit and follows the selection,
  with up/down chevrons, so new options no longer overflow into the footer.

### Removed
- Dead Milestone A scaffolding (`src/`, root `Makefile`, `build.sh`) that predated the
  phoneME MIDP pipeline. Remaining boot log strings were rebranded to `PS2ME`.

## [1.1.0] - 2026-07-08

### Changed
- Returning from a game restores the grid cursor onto the game you just exited.
- The memory-card "Saving" spinner label is now in English (the UI is fully en-US).

## [1.0.0] - 2026-07-08

First tagged release. A self-contained PlayStation 2 ELF that boots into a native
launcher and runs J2ME (CLDC 1.1 / MIDP 2.0) MIDlets through a ported phoneME Feature VM.

### Added
- **Native launcher** (runs from `main()` before the VM): a full-screen 640×448
  "console dashboard" with a 4×5 icon grid decoded from each JAR, tabs
  (All Games / Favorites / Settings), an alphabet sidebar, a game-details panel,
  favorites and recents, per-game canvas resolution/orientation overrides, sorting,
  and a friendly loading screen with an optional debug split view.
- **USB storage**: games are loaded from `<boot>/PS2ME/games/`, with a background,
  SIF-serialized icon worker and an on-disk icon cache.
- **Memory-card integration**: launcher config plus a proper per-game RecordStore save,
  each shown in the console OSD browser with its own icon and title.
- **Audio (SPU2)**: an audio foundation over `audsrv`, a shared SIF lock with a mixer
  thread, the Nokia Sound API (tone / OTA melody / WAV), and MMAPI playback with an
  offline-built HL4MGM wavetable MIDI synth (per-voice low-pass, cubic interpolation).
- **Game-compatibility APIs**: Nokia UI (`com.nokia.mid.ui`) and Nokia Sound
  (`com.nokia.mid.sound`).
- **Semantic versioning** (`ps2/version.h` + `version.sh`) and 1-bit icon transparency.

### Fixed
- Double buffering in the display path to eliminate tearing on hardware.
- Unaligned 64-bit field access and framebuffer stores on the EE (R5900).
- A multi-game crash caused by a use-after-free of the RAM filesystem backing store.

[1.2.0]: https://github.com/OWNER/PS2ME/releases/tag/v1.2.0
[1.1.0]: https://github.com/OWNER/PS2ME/releases/tag/v1.1.0
[1.0.0]: https://github.com/OWNER/PS2ME/releases/tag/v1.0.0
