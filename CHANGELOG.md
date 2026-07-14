# Changelog

All notable changes to **PS2ME** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The version number lives in a single source of truth, `ps2/version.h`, and is bumped
with `./version.sh {major|minor|patch}`.

## [1.4.0] - 2026-07-14

### Changed
- Games run faster: the CLDC bytecode interpreter now keeps its hottest state — the frame,
  stack, program-counter and locals pointers — pinned in dedicated MIPS registers
  ("global-reg") instead of memory, cutting per-bytecode overhead. Measured Zombie Infection
  18 → 23 FPS, crossing the 20 FPS mark, and validated on real hardware.
- The screen present path now converts pixels four at a time (SWAR), about 73% faster.

### Fixed
- The event wait could sleep past its deadline; the receive nap is now clamped to the frame
  deadline, recovering a few milliseconds per frame.

## [1.3.0] - 2026-07-11

### Added
- Configurable control layouts, Simple and Complete, with analog-stick support and
  per-game overrides, selectable from Settings and the per-game details panel.
- An on-screen controls guide with a live button tester.
- A first-run control-layout picker shown once after the splash.

### Changed
- Games run much faster: the CLDC bytecode interpreter is now built at `-O2 -fwrapv`
  instead of `-O0`, and the game framebuffer flips without blocking on vblank
  (measured roughly 3.5x higher in-game FPS).
- The Java heap cap was raised to 8 MB so heavier games finish loading.
- The Settings tab dropped its alphabet rail and centers its list.
- The boot splash is held a little longer so slow displays do not miss it.

### Fixed
- Menu background music stayed silent on real hardware.
- Zombie Infection and other titles froze right after loading when a game drew an image
  onto its own off-screen buffer's Graphics; this self-blit is now allowed, matching
  real handsets.

## [1.2.1] - 2026-07-09

### Added
- A boot splash screen, shown once at startup and fading in and out with an ease-in-out
  curve before the menu appears. Its VRAM texture is freed as soon as the splash ends.
- An author credit ("by Wellinator") beside the launcher wordmark in the header.
- A project website (`docs/`) with a feature overview, a screenshot carousel, and a
  releases list generated live from the GitHub API.

### Changed
- The launcher's version number moved from the header to the bottom-right of the footer.
- The "no games found" screen now shows a friendly empty state by default; the raw
  storage diagnostics (device probes and resolved paths) appear only with Debug mode on.

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

[1.2.0]: https://github.com/Wellinator/PS2ME/releases/tag/v1.2.0
[1.1.0]: https://github.com/Wellinator/PS2ME/releases/tag/v1.1.0
[1.0.0]: https://github.com/Wellinator/PS2ME/releases/tag/v1.0.0
