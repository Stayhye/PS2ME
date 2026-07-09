<div align="center">

<img src="assets/PS2ME_ICON.png" width="96" alt="PS2ME logo">

# PS2ME

**Run J2ME (CLDC 1.1 / MIDP 2.0) games on the PlayStation 2.**

[![Release](https://img.shields.io/github/v/release/Wellinator/PS2ME?sort=semver)](https://github.com/Wellinator/PS2ME/releases)
[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](LICENSE)
[![Website](https://img.shields.io/badge/website-GitHub%20Pages-2ea44f)](https://wellinator.github.io/PS2ME/)

</div>

PS2ME brings the huge library of Java **MIDlets** — the games that shipped on feature
phones through the 2000s — to the **PlayStation 2**. It boots into a native launcher,
lists the JARs on your USB drive with their own icons, and runs them through a port of
the open-source **phoneME Feature** virtual machine, with PS2 controller input, SPU2
audio, and per-game saves on the memory card.

> Download the latest ELF from the [**Releases**](https://github.com/Wellinator/PS2ME/releases)
> page, or visit the [**project website**](https://wellinator.github.io/PS2ME/).

## Features

- **Native launcher** — a full-screen 640×448 dashboard with a 4×5 grid of game icons
  decoded straight from each JAR, tabs (All Games / Favorites / Settings), an alphabet
  sidebar, sorting, favorites, and recents.
- **Per-game settings** — canvas resolution and orientation overrides so both portrait
  and landscape games display correctly.
- **Audio** — menu background music played on a hardware SPU2 ADPCM voice, the Nokia
  Sound API (tone / OTA melody / WAV), and MMAPI MIDI through an offline-built wavetable
  synth.
- **Memory-card saves** — MIDlet RecordStore data is persisted per game, and each save
  shows up in the console's OSD browser with its own icon and title.
- **Quality-of-life** — friendly loading screen, optional debug split view, and an
  opt-in in-game FPS counter.

## Requirements

- **Real hardware:** a PlayStation 2 able to run homebrew ELFs (e.g. launched from USB
  via uLaunchELF / wLaunchELF), plus a FAT32 USB drive and a memory card for saves.
- **Emulator:** [PCSX2](https://pcsx2.net/) works well for testing.
- **Games:** standard J2ME `.jar` MIDlets.

## Install & run

1. Download `PS2ME-vX.Y.Z.elf` (and `bgm.adpcm`) from the
   [Releases](https://github.com/Wellinator/PS2ME/releases) page.
2. On your USB drive, create a `PS2ME` folder and copy the files so you have:

   ```
   mass:/PS2ME/PS2ME-vX.Y.Z.elf
   mass:/PS2ME/bgm.adpcm          (optional — the menu is silent without it)
   mass:/PS2ME/games/*.jar        (your MIDlets)
   ```

3. Launch the ELF from your homebrew loader (or in PCSX2: *Run ELF…*).
4. Pick a game from the grid and press **✕**.

**Controls (launcher):** D-pad navigates · **✕** launches · **○** back · **△** favorite ·
**□** sort · **L1/R1** switch tabs · **L2/R2** page · **Select** opens per-game options.

## Building from source

The build runs entirely inside Docker. It is a two-stage toolchain: a host image
(`phoneme-host`, JDK 8 + `gcc-multilib`) that romizes the class library, and a
cross image (`phoneme-cross`) that adds the PS2 EE toolchain (`mips64r5900el-ps2-elf-*`,
from a `rsdk-ps2-builder` image) and links the final ELF.

> **Note:** the phoneME Feature source tree and the PS2 SDK are **not** vendored in this
> repository. They are bind-mounted from the host at `references/phoneme` and provided by
> the `rsdk-ps2-builder` image. See the Dockerfiles under `docker/` for the expected
> layout. Because of this, builds are produced locally rather than in CI.

Once the images and `references/phoneme` are in place, produce a release ELF with:

```sh
docker run --rm \
  -v "$(pwd)":/work -v phoneme_build:/build \
  phoneme-cross bash /work/docker/phoneme-cross/build-release-ps2.sh
# -> build/ps2/PS2ME-vX.Y.Z.elf (stripped) + build/ps2/bgm.adpcm
```

For a debug build that keeps the EE console alive (useful under PCSX2), add
`-e PS2ME_NO_IOP_RESET=1` and run `build-elf-midp-ps2.sh`.

## Project layout

```
ps2/                Our PS2 port (the code that lives in this repo)
  version.h           Single source of truth for the version
  vm/                 Entrypoint (Ps2MidpMain.cpp) + KNI native-method bridges
  javacall/           JavaCall port: contract / hal / platform (ps2sdk backends)
    platform/           Frontend, storage, audio, pad, memory card, display, ...
  phoneme/            Build-system overlay for the ps2_mips target
docker/               Multi-stage Docker build (host + cross) and build scripts
tools/                Offline tooling (SF2 wavetable bank builder, mkbgm, ...)
assets/               Brand icon + menu background music source
docs/                 Project website (GitHub Pages)
```

## Versioning & releases

Versions follow [SemVer](https://semver.org/). Bump with `./version.sh {major|minor|patch}`
(edits `ps2/version.h`), rebuild, tag `vX.Y.Z`, and publish. See
[`CHANGELOG.md`](CHANGELOG.md) and [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

PS2ME is released under the **GNU General Public License v2.0** — see [`LICENSE`](LICENSE).
The distributed ELF incorporates the **phoneME Feature** VM and class library (GPLv2),
which is why the project as a whole is GPLv2. It also builds against the **PS2 SDK**
(ps2dev) and includes **stb_image** (public domain / MIT).

## Acknowledgements

- [phoneME Feature](https://github.com/OpenJDK/phoneme) — the CLDC/MIDP reference VM.
- [PSPKVM](https://sourceforge.net/projects/pspkvm/) — prior art for a MIPS J2ME port.
- [ps2dev / ps2sdk](https://github.com/ps2dev/ps2sdk) — the PlayStation 2 homebrew SDK.
