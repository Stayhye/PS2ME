// PS2 JavaCall port — HAL layer.
//
// Keypad: maps the PS2 controller to J2ME key events. On each poll cycle it reads
// an IPad, diffs against the previous snapshot, and emits javanotify_key_event
// (KEYPRESSED / KEYRELEASED) for every button that changed — the platform->VM
// direction of the input contract. It is the EventQueue's poll hook, so input is
// pumped exactly when the AMS loop waits for events.
//
// Mapping: D-pad -> UP/DOWN/LEFT/RIGHT, Cross -> SELECT (the FIRE game action),
// L1/R1 -> SOFT1/SOFT2 (menu soft keys).
#ifndef PS2_JAVACALL_HAL_KEYPAD_HPP
#define PS2_JAVACALL_HAL_KEYPAD_HPP

#include "IPollHook.hpp"
#include "IPad.hpp"

namespace ps2 {
namespace hal {

class Keypad : public IPollHook {
public:
    static Keypad& instance();

    /// Install the controller backend. Ownership stays with the caller.
    void setPad(IPad* pad) { pad_ = pad; }
    /// The shared controller backend, for code (e.g. the native front-end) that
    /// needs to read the pad directly instead of through the key-event mapping.
    IPad* pad() const { return pad_; }

    /// IPollHook: read the pad and emit key events for changed buttons.
    virtual void onPoll();

private:
    Keypad() : pad_(0), havePrev_(false) {}
    Keypad(const Keypad&);
    Keypad& operator=(const Keypad&);

    /// Emit a press/release for one button given its now/previous state.
    void emit(bool now, bool before, int javacallKey);

    IPad*      pad_;
    PadButtons prev_;
    bool       havePrev_;   // suppress spurious edges on the very first read
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_KEYPAD_HPP
