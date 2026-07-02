# M2 Porting Map — phoneME Feature (via PSPKVM) → PS2

Architecture map for integrating the Java VM. Derived from the PSPKVM source
layout (`github.com/vadosnaprimer/pspkvm`, a PSP port of Sun's phoneME Feature).
PSP (Allegrex) and PS2 (Emotion Engine) are both little-endian 32-bit MIPS, so
PSPKVM is a direct map, not a from-scratch port.

## How phoneME is layered

phoneME Feature separates portable VM/profile code from a platform abstraction.
The porting contract is the **`javacall`** API (device services) plus **`pcsl`**
(portable common services). Porting = implement those for PS2; the VM stays intact.

```
  Java MIDlet (.jar)
        |
  midp/     MIDP 2.0 profile: LCDUI, lifecycle, Game API   [KEEP]
  cldc/     CLDC-HI VM: interpreter + GC + verifier         [KEEP, force interpreter]
        |
  javacall/ + pcsl/   PLATFORM ABSTRACTION                  [REWRITE for PS2]
        |
  psp/  ->  ps2/       build + entry + video glue           [CREATE]
```

## PSPKVM top-level dirs

| Dir | Role | PS2 action |
|-----|------|-----------|
| `cldc/` | The VM (CLDC HotSpot Impl: bytecode interpreter, JIT, GC) | **Keep.** Cross-compile with `mips64r5900el-ps2-elf-gcc`. **Force interpreter** — the JIT has no R5900 backend (only ARM/x86/MIPS-Allegrex asm exists). |
| `midp/` | MIDP 2.0 profile (LCDUI, lifecycle, RMS, Game API) | **Keep.** Native hooks resolved via javacall. |
| `javacall/` | Platform abstraction API (the porting contract) | **Rewrite** the PS2 backend (see below). |
| `pcsl/` | Portable Common Services (file, memory, network, print, string) | **Port** to ps2sdk (fileXio, EE malloc). PSP port is the template. |
| `pisces/` | Vector/anti-aliased 2D rasterizer | Keep (portable C). |
| `jpeg/`, `jsr*/` | Image decode + optional JSR APIs (WMA, MMAPI, location…) | Keep/defer. `jsr135_mmapi` = audio (M5). |
| `psp/` | PSP glue: `pspkvm.c` (entry), `vram.c/h` (video), Makefiles, fonts | **Mirror as `ps2/`.** |
| `tools/`, `restricted_crypto/`, `ext/` | Build tools, crypto, extras | As needed. |

## javacall backend — file-by-file (this is 80% of the work)

Located under `javacall/interface/{common,midp,jsr135_mmapi}`. Implement a PS2
backend for each. The two milestones we already built plug in directly:

| javacall header | Provides | PS2 implementation | Status |
|-----------------|----------|--------------------|--------|
| `midp/javacall_lcd.h` | Frame/pixel buffer for the Canvas | **Our M0 flush** — phoneME hands us a 16-bit screen buffer; we already DMA a 16-bit buffer to the GS as a scaled sprite via libdraw. Near-direct match. | ✅ HAL ready |
| `midp/javacall_keypress.h` | Key events → MIDP keycodes | **Our M1 input** — `src/input.c` (libpad); map `j2me_keys_t` → javacall keycodes + post events. | ✅ HAL ready |
| `common/javacall_lifecycle.h` | MIDlet start/pause/destroy | `ps2/main.c` drives the lifecycle. | ⬜ M2 |
| `common/javacall_events.h` | Event queue VM ↔ platform | EE-side ring buffer feeding the VM. | ⬜ M2 |
| `common/javacall_memory.h` | Heap alloc | EE `memalign`/`malloc`. | ⬜ M2 |
| `common/javacall_file.h` + `javacall_dir.h` | File/dir I/O (JAR, RMS) | ps2sdk `fileXio` (USB/MC/HDD). | ⬜ M4 |
| `common/javacall_logging.h` | Debug logging | `printf`/SIO (already used). | ✅ trivial |
| `common/javacall_time.h` | Timers, ticks | EE timer/`GetTimerCounter`. | ⬜ M2 |
| `common/javacall_font.h` | System font glyphs | phoneME software font, or our bitmap font. | ⬜ M3 |
| `jsr135_mmapi/*` | MMAPI audio (WAV/MIDI/tones) | SPU2 via IOP (PCM first, MIDI later). | ⬜ M5 |
| `common/javacall_network.h` | Sockets/HTTP (GCF) | ps2ip/SMAP — optional. | ⬜ later |

## `ps2/` build+glue dir (mirror of `psp/`)

| PSP file | PS2 equivalent | Notes |
|----------|----------------|-------|
| `psp/pspkvm.c` | `ps2/main.c` | EE entry: init GS/pad, mount storage, load MIDlet, run VM. |
| `psp/vram.c` `vram.h` | `ps2/video.c` | Our libdraw flush from M0. |
| `psp/Makefile` `build.mak` `build.sh` | `ps2/Makefile` etc. | Integrate cross-compile with ps2sdk. |
| `build-psp-cldc.sh` | `build-ps2-cldc.sh` | Top-level VM+classes build driver. |

## Build pipeline — the #1 risk

phoneME's build is **two-stage** and needs an ancient host toolchain:

- **Stage A (HOST, Linux):** a JDK (PSPKVM uses **JDK 1.4.2**) + host GCC compile the
  build tools and **romize** the CLDC/MIDP class library into a ROM image. Runs
  unmodified — not PS2-specific.
- **Stage B (TARGET):** cross-compile the native VM + javacall/pcsl backend for EE.

Historic gotchas (from PSPKVM `BUILDING.TXT`): needs Make **3.80** (not 3.81+),
Cygwin path quirks, install at a filesystem root, FreeType 2.3.9 patch.

**Mitigation / first concrete task:** build a Docker image that reproduces Stage A
(JDK 1.4.2 + phoneME build tools), so the romizer pipeline is reproducible before any
PS2 code is written. Getting an unmodified **PSPKVM PSP build** to compile in that
container first validates the whole pipeline — then we retarget Stage B to ps2sdk.

## Sources

- PSPKVM (phoneME PSP port): https://github.com/vadosnaprimer/pspkvm ·
  https://github.com/ruitaomu/pspkvm · https://sourceforge.net/projects/pspkvm/
- phoneME Feature (archived Sun source, git): https://github.com/magicus/phoneME
- phoneME docs mirror: https://phonej2me.github.io
- Classic Sun KVM background: https://barrgroup.com/blog/kvm-small-java-virtual-machine-j2me
