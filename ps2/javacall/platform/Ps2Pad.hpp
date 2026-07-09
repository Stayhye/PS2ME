// PS2 JavaCall port — platform layer.
//
// Ps2Pad: the IPad backend over ps2sdk libpad. It loads the IOP pad modules and
// opens controller port 0 on first use, then reads the digital D-pad and buttons.
// Ported from the Milestone-1 input path (src/input.c), reorganized behind the HAL
// interface. This is the only input class that touches ps2sdk.
#ifndef PS2_JAVACALL_PLATFORM_PS2PAD_HPP
#define PS2_JAVACALL_PLATFORM_PS2PAD_HPP

#include "../hal/IPad.hpp"

namespace ps2 {
namespace platform {

class Ps2Pad : public hal::IPad {
public:
    Ps2Pad() : opened_(false), analogInit_(false) {}

    virtual bool ensureReady();
    virtual bool read(hal::PadButtons* out);

private:
    bool open();          // one-time SIF/module/port bring-up
    bool stableState();   // pad is in a readable state
    void enableAnalog();  // one-shot: switch a DualShock into locked analog mode

    bool opened_;         // modules loaded + port opened
    bool analogInit_;     // analog-mode request already issued (or pad has no analog)
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2PAD_HPP
