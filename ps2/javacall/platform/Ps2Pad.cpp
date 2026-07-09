// PS2 JavaCall port — platform layer. Ps2Pad implementation + input registration.
//
// Controller bring-up uses the ps2_drivers library (init_joystick_driver), which
// loads the embedded sio2man/mtapman/padman IRX via SifExecModuleBuffer -- far more
// robust than the rom0: modules the M1 prototype used. SifExecModuleBuffer needs
// the SBV "load module from buffer" patches applied first. We deliberately skip
// SifIopReset (the ps2_drivers sample does it) so we don't tear down the IOP mid-run
// and lose the EE Console; patching the running IOP is enough to load the pad IRX.
#include "Ps2Pad.hpp"
#include "../hal/Keypad.hpp"
#include "../hal/EventQueue.hpp"
#include "SifLock.hpp"

#include <tamtypes.h>
#include <sifrpc.h>
#include <sbv_patches.h>
#include <libpad.h>
#include <ps2_joystick_driver.h>   // init_joystick_driver (ps2_drivers, ports/lib)

namespace ps2 {
namespace platform {

namespace {
const int PORT = 0;   // controller connector 1
const int SLOT = 0;   // 0 when no multitap

// libpad requires a user-provided, 64-byte-aligned 256-byte state buffer per pad.
char g_padBuf[256] __attribute__((aligned(64)));
} // namespace

bool Ps2Pad::open() {
    SifGuard guard;   // pad bring-up loads IRX over SIF
    SifInitRpc(0);
    // Allow SifExecModuleBuffer to load the IRX embedded in ps2_drivers.
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    // Loads sio2man/mtapman/padman IRX and runs mtapInit + padInit. true = also
    // bring up the sio2man dependency.
    if (init_joystick_driver(true) != JOYSTICK_INIT_STATUS_OK) {
        return false;
    }
    if (padPortOpen(PORT, SLOT, g_padBuf) == 0) {
        return false;
    }
    opened_ = true;
    return true;
}

bool Ps2Pad::stableState() {
    SifGuard guard;   // padGetState is a padman SIF call
    const int state = padGetState(PORT, SLOT);
    return state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1;
}

// Ask a DualShock to enter locked analog mode, once, so read() gets live stick axes.
// Fire-and-forget: padSetMainMode kicks off an async command (the pad goes busy, so the
// next stableState() is briefly false and read() skips a cycle -- harmless). Runs only
// when the pad is already stable. A digital-only pad has no analog mode in its table, so
// we skip the request but still mark it done (its axes stay centred -> no effect).
void Ps2Pad::enableAnalog() {
    SifGuard guard;   // padInfoMode / padSetMainMode are padman SIF calls
    const int modes = padInfoMode(PORT, SLOT, PAD_MODETABLE, -1);
    if (modes <= 0) {
        return;       // mode table not identified yet -> retry next cycle
    }
    bool hasAnalog = false;
    for (int i = 0; i < modes; ++i) {
        if (padInfoMode(PORT, SLOT, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK) {
            hasAnalog = true;
            break;
        }
    }
    if (hasAnalog) {
        padSetMainMode(PORT, SLOT, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    }
    analogInit_ = true;   // don't probe again regardless (digital pads have no analog)
}

bool Ps2Pad::ensureReady() {
    if (!opened_ && !open()) {
        return false;
    }
    // Don't block: report ready only once the pad has left its transient state.
    if (!stableState()) {
        return false;
    }
    if (!analogInit_) {
        enableAnalog();
    }
    return true;
}

bool Ps2Pad::read(hal::PadButtons* out) {
    if (out == 0) {
        return false;
    }
    SifGuard guard;   // recursive: covers the nested stableState() below + padRead
    if (!stableState()) {
        return false;
    }
    struct padButtonStatus b;
    if (padRead(PORT, SLOT, &b) == 0) {
        return false;
    }
    // Pad bits are active-low; invert to active-high.
    const u32 d = 0xffff ^ b.btns;
    out->up    = (d & PAD_UP)    != 0;
    out->down  = (d & PAD_DOWN)  != 0;
    out->left  = (d & PAD_LEFT)  != 0;
    out->right = (d & PAD_RIGHT) != 0;
    out->cross    = (d & PAD_CROSS)    != 0;
    out->circle   = (d & PAD_CIRCLE)   != 0;
    out->triangle = (d & PAD_TRIANGLE) != 0;
    out->square   = (d & PAD_SQUARE)   != 0;
    out->l1       = (d & PAD_L1)       != 0;
    out->r1       = (d & PAD_R1)       != 0;
    out->l2       = (d & PAD_L2)       != 0;
    out->r2       = (d & PAD_R2)       != 0;
    out->select   = (d & PAD_SELECT)   != 0;
    out->start    = (d & PAD_START)    != 0;
    out->l3       = (d & PAD_L3)        != 0;
    out->r3       = (d & PAD_R3)        != 0;
    // Analog axes (0..255, 128 = centre). On a digital pad or before analog mode is
    // negotiated these read as the neutral centre, so the sticks contribute nothing.
    out->lx = b.ljoy_h;
    out->ly = b.ljoy_v;
    out->rx = b.rjoy_h;
    out->ry = b.rjoy_v;
    return true;
}

namespace {
// Wire the input path at startup, before the VM enters the event loop: the pad
// backend feeds the Keypad, which is installed as the EventQueue's poll hook.
// Mirrors the StdoutSink / Ps2Framebuffer registrars.
Ps2Pad g_ps2Pad;

struct Ps2PadRegistrar {
    Ps2PadRegistrar() {
        hal::Keypad::instance().setPad(&g_ps2Pad);
        hal::EventQueue::instance().setPollHook(&hal::Keypad::instance());
    }
};
Ps2PadRegistrar g_ps2PadRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
