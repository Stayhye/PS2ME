// PS2ME — CLDC-standalone ELF frontend stub (used by build-elf-ps2.sh).
//
// The platform StdoutSink (linked into every CLDC ELF) mirrors the VM's stdout
// to the native on-screen launch overlay via Ps2Frontend::instance().logWrite().
// The REAL Ps2Frontend (ps2/javacall/platform/Ps2Frontend.cpp, ~2.6k lines) is
// the game-launcher menu and transitively drags in the entire frontend
// (GsDisplay/framebuffer, pad, audio, storage, memory card, TTF, ...). The
// CLDC-standalone ELF (Milestone A smoke tests, and the JIT Fase 3 test harness
// JitTest) is deliberately frontend-free: it only needs a place for logWrite()
// to go. So provide no-op stand-ins for the two methods StdoutSink references.
// System.out still reaches the EE console through StdoutSink's fwrite(stdout).
//
// This object is linked ONLY by the CLDC ELF (build-elf-ps2.sh), never together
// with the real Ps2Frontend.cpp, so there is no duplicate-symbol clash.
#include "Ps2Frontend.hpp"

namespace ps2 {
namespace platform {

Ps2Frontend& Ps2Frontend::instance() {
    static Ps2Frontend f;   // private ctor is accessible from this member
    return f;
}

void Ps2Frontend::logWrite(const char* /*s*/, int /*len*/) {
    // No native overlay in the CLDC ELF; stdout still goes to the EE console.
}

} // namespace platform
} // namespace ps2
