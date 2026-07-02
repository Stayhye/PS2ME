# j2me-ps2

Runtime **J2ME (CLDC 1.1 / MIDP 2.0)** para o **PlayStation 2** — permitir jogar a
biblioteca de jogos Java (MIDlets) em hardware PS2. Inspirado no que a **PSPKVM**
fez no PSP.

## Estratégia

Não escrevemos uma JVM do zero. Portamos o **phoneME Feature** (implementação de
referência open-source de CLDC/MIDP da Sun), usando a **PSPKVM como mapa** — PSP e
PS2 são ambos **MIPS little-endian 32-bit**, então boa parte da camada de plataforma
é adaptável em vez de reescrita.

O trabalho concentra-se na **camada nativa (HAL)**: vídeo, input, áudio, storage,
ciclo de vida do MIDlet. A VM e as bibliotecas de classe são C portável + ROM image.

Ver análise completa e roadmap no histórico do projeto.

## Estrutura

```
src/            código EE (HAL + bring-up)
include/        headers do projeto
references/     ps2sdk-master, ps2_drivers-main (somente leitura)
Makefile        build EE (usa gsKit + libgraph/dma/packet)
build.sh        wrapper: compila dentro do container Docker
```

## Build

Requer Docker com a imagem `rsdk-ps2-builder` (ps2dev + make + gsKit).

```sh
./build.sh          # compila -> j2me.elf
./build.sh clean
```

No Windows (Git Bash), direto:

```sh
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W)":/src -w /src \
  rsdk-ps2-builder:latest sh -c "make clean; make all"
```

## Rodar / Testar

**PCSX2:**
```sh
"/c/Program Files/PCSX2/pcsx2-qt.exe" -elf "D:\PS2DEV\ports\j2me\j2me.elf"
```

## Roadmap

- [x] **M0 — Prova do caminho de vídeo.** Framebuffer software RGBA5551 (tela virtual
  240×320) via `libdraw` → DMA → GS como sprite escalonada. *(feito e testado)*
- [x] **M1 — Input via libpad.** Módulo `src/input.c` mapeia controle PS2 → keypad
  J2ME; caixa controlável pelo D-pad, Cross muda cor. *(feito)*
- [ ] **M1.5 — Ciclo de vida + double-buffer.** Flip de framebuffer (sem tearing),
  tela virtual redimensionável, abstração de eventos.
- [ ] **M2 — Integrar phoneME.** Compilar o interpretador CLDC no toolchain do
  ps2sdk; rodar bytecode Java simples (stdout).
- [ ] **M3 — LCDUI real.** Plugar o rasterizador do phoneME (gxj) no flush do M0;
  rodar um MIDlet com `Canvas`.
- [ ] **M4 — Compatibilidade.** MIDP 2.0 Game API, RMS (save), loader de JAR/JAD.
- [ ] **M5 — Áudio.** PCM via SPU2; MIDI depois.
```
