# PS2 JavaCall Port

Our own implementation of the phoneME Feature **JavaCall** porting layer for the
PlayStation 2 (Emotion Engine, MIPS little-endian, 32-bit), built with `ps2sdk`.

JavaCall is the C API contract the phoneME VM (CLDC/MIDP) calls into for every
platform service: display, input, events, files, time, networking, lifecycle, etc.
This directory is *our* port of that contract. The complete `win32` port in
`references/phoneme/javacall/implementation/win32` and PSPKVM's MIPS `psp/` port are
used only as **references/north stars** — the code here is written from scratch,
choosing the best design for the PS2.

## Architecture — three layers, clear responsibilities

```
Contract layer  (C linkage)   contract/javacall_*.cpp
      |   thin extern "C" shims that the VM links against. No hardware logic;
      |   each function just marshals arguments and delegates to the HAL.
      v
HAL layer       (C++ / OOP)   hal/*.hpp, hal/*.cpp
      |   cohesive device/service classes behind narrow interfaces
      |   (IDisplay, IInputSource, ...). Portable, unit-testable, no ps2sdk here.
      v
Platform layer  (C++ / ps2sdk) platform/ps2_*.cpp
          concrete hardware backends (libdraw/libgraph, libpad, SPU2, memcard)
          implementing the HAL interfaces. This is where M0 (video) and M1
          (input) get reorganized into classes.
```

Rules that keep the layers honest:

- **Only `contract/` exposes `extern "C" javacall_*` symbols.** The VM has C linkage,
  so these are the ABI boundary. They are compiled as C++ (so they can call HAL
  objects directly) but export C symbols via the `extern "C"` in the javacall headers.
- **`hal/` never includes ps2sdk headers.** It talks to hardware only through
  interfaces it defines (e.g. `IOutputSink`, `IDisplay`, `IInputSource`). This is what
  lets us mock/host-test the HAL and swap hardware backends.
- **`platform/` is the only place ps2sdk appears.** It implements the HAL interfaces.
- **`include/` holds the JavaCall platform contract** required of every port
  (`javacall_platform_defs.h`: sized-integer typedefs and platform limits).

## Directory layout

```
ps2/javacall/
  include/    javacall_platform_defs.h        # required by javacall; PS2 target defs
  contract/   javacall_logging.cpp, ...        # extern "C" shims (ABI boundary)
  hal/        Logger.{hpp,cpp}, IOutputSink.hpp # C++ device/service classes
  platform/   StdoutSink.{hpp,cpp}, ...         # ps2sdk-backed implementations
```

## Roadmap

**Milestone A — CLDC on PS2 with console output.** Smallest end-to-end path: the VM
runs bytecode and `System.out.println` reaches the screen/console. Needs the minimal
javacall set: logging/print, os/memory, time, and lifecycle entry.

**Milestone B — a Canvas MIDlet (video + input).** Adds LCD/display, key/pointer
input, the event queue, and file/dir (RMS). This is where M0 (`libdraw` flush) wires
into `javacall_lcd` and M1 (`libpad`) into `javacall_keypress`, reorganized as HAL
classes.

Optional/stubbed initially: DOM, chapi, cbs, carddevice, sms/mms, media/mmapi, most
AMS beyond basic lifecycle, security.

## Current status

Scaffold + the first exemplar module (**logging**) that establishes the
contract→HAL→platform pattern. Remaining modules follow the same shape.
