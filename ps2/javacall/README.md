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
  include/    javacall_platform_defs.h          # required by javacall; PS2 target defs
  contract/   javacall_logging.cpp              # extern "C" shims (ABI boundary):
              javacall_os.cpp                   #   logging, os, time, memory
              javacall_time.cpp
              javacall_memory.cpp
  hal/        IOutputSink.hpp  Logger.{hpp,cpp} # C++ device/service classes,
              ICpuCache.hpp    OsCore.{hpp,cpp} #   no ps2sdk here
              IClock.hpp       SystemClock.{hpp,cpp}
              ITimerBackend.hpp Timer.hpp TimerService.{hpp,cpp}
              IHeap.hpp        MemoryManager.{hpp,cpp}
  platform/   StdoutSink.{hpp,cpp}              # backends implementing the HAL:
              Ps2CpuCache.{hpp,cpp}             #   ps2sdk (EE-only)
              Ps2AlarmTimer.{hpp,cpp}           #   ps2sdk (EE-only)
              PosixClock.{hpp,cpp}              #   portable (host + ps2sdk newlib)
              PosixTickTimer.{hpp,cpp}          #   host validation backend
              SystemHeap.{hpp,cpp}              #   portable (host + ps2sdk newlib)
```

For each service the HAL defines an interface and the platform layer supplies
the backend; only one backend per interface is compiled into a given build. The
tick timer has two: `PosixTickTimer` (setitimer/SIGALRM) for the host validation
build and `Ps2AlarmTimer` (EE `SetAlarm`) for the PS2 target.

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

**Milestone A modules complete and validated:** logging, os, time, memory. Each
follows the contract→HAL→platform pattern; all compile clean under `-Wall
-Wextra` with both the host g++ and the ps2sdk `mips64r5900el-ps2-elf` toolchain.
A host harness exercises the whole extern "C" surface — allocation, the wall/
monotonic clocks, and the cyclic 30 ms scheduler tick (fire / suspend / resume /
finalize) — all green.

- **os** — `initialize`/`dispose` lifecycle + `flush_icache` via `ICpuCache`
  (`Ps2CpuCache` = EE `FlushCache`). Mutex/cond are omitted: the CLDC VM is green-
  threaded on one OS thread (the reference `win32_x86_cldc` port omits them too).
- **time** — `SystemClock` (wall + monotonic + sleep + timezone over `IClock`)
  and `TimerService`, which drives the **cyclic 30 ms tick** that pumps
  `real_time_tick()` — the heartbeat of the green-thread scheduler
  (`OS_javacall.cpp::start_ticks`), not optional.
- **memory** — `MemoryManager` over `IHeap` (`SystemHeap` = newlib malloc); the
  big VM heap plus malloc/realloc/free, with calloc/strdup composed portably.

Next: the `cldc/build/ps2_mips` target that links the CLDC VM against this port
with the ps2sdk toolchain, to run Milestone A on PCSX2.
