// PS2 JavaCall port — HAL layer.
//
// Keypad: maps the PS2 controller to J2ME key events. On each poll cycle it reads an
// IPad, computes the SET of J2ME keys held this frame (from the active layout + the
// pad/analog state), diffs it against the previous frame's set, and emits
// javanotify_key_event (KEYPRESSED / KEYRELEASED) for every key that changed — the
// platform->VM direction of the input contract. It is the EventQueue's poll hook, so
// input is pumped exactly when the AMS loop waits for events. The set model handles
// double emission (arrow + digit), diagonals and analog sectors uniformly.
//
// Two layouts (set per game before launch, see setLayout):
//   SIMPLE   — the classic mapping: D-pad/left stick -> arrows, Cross -> FIRE,
//              L1/R1 -> SOFT1/SOFT2.
//   COMPLETE — full phone keypad: D-pad emits arrow + 2/8/4/6, Cross -> FIRE + 5,
//              faces -> * 0 #, shoulders -> 1/3/7/9, Select/Start -> SOFT1/SOFT2,
//              right stick -> numpad by compass sector, R3 -> 5.
#ifndef PS2_JAVACALL_HAL_KEYPAD_HPP
#define PS2_JAVACALL_HAL_KEYPAD_HPP

#include "IPollHook.hpp"
#include "IPad.hpp"

namespace ps2 {
namespace hal {

class Keypad : public IPollHook {
public:
    enum Layout { LAYOUT_SIMPLE = 0, LAYOUT_COMPLETE = 1 };

    static Keypad& instance();

    /// Install the controller backend. Ownership stays with the caller.
    void setPad(IPad* pad) { pad_ = pad; }
    /// The shared controller backend, for code (e.g. the native front-end) that
    /// needs to read the pad directly instead of through the key-event mapping.
    IPad* pad() const { return pad_; }

    /// Select the active key mapping (resolved per game just before the VM runs it).
    void setLayout(Layout l) { layout_ = l; }

    /// IPollHook: read the pad and emit key events for the frame's key-set changes.
    virtual void onPoll();

private:
    Keypad()
        : pad_(0), layout_(LAYOUT_SIMPLE), prevCount_(0), havePrev_(false),
          lUp_(false), lDown_(false), lLeft_(false), lRight_(false), rSectorKey_(0) {}
    Keypad(const Keypad&);
    Keypad& operator=(const Keypad&);

    static const int MAX_KEYS = 32;   // upper bound on keys held in one frame

    /// Fill @p keys (up to MAX_KEYS) with the J2ME key codes active this frame.
    void computeActive(const PadButtons& b, int* keys, int* count);

    IPad*  pad_;
    Layout layout_;
    int    prevKeys_[MAX_KEYS];   // key-set from the previous frame
    int    prevCount_;
    bool   havePrev_;             // suppress spurious edges on the very first read

    // Analog hysteresis latches (persist across frames so a stick resting near a
    // threshold doesn't chatter). Left stick -> 8-way direction; right stick -> one
    // numpad key by sector.
    bool lUp_, lDown_, lLeft_, lRight_;
    int  rSectorKey_;   // 0 = none, else the ASCII digit currently held
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_KEYPAD_HPP
