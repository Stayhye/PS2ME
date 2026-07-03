// PS2 JavaCall port — HAL layer.
//
// IPad: the game controller abstraction the Keypad reads through. It reports the
// digital buttons the MIDP keypad mapping needs, decoupled from ps2sdk/libpad
// (whose access lives in platform::Ps2Pad). Analog sticks and pressure are out of
// scope for the J2ME keypad.
#ifndef PS2_JAVACALL_HAL_IPAD_HPP
#define PS2_JAVACALL_HAL_IPAD_HPP

namespace ps2 {
namespace hal {

/// Active-high digital button snapshot. Only the buttons the J2ME keypad maps.
struct PadButtons {
    bool up;
    bool down;
    bool left;
    bool right;
    bool cross;   // primary action / fire
    bool l1;      // left soft key
    bool r1;      // right soft key

    PadButtons()
        : up(false), down(false), left(false), right(false),
          cross(false), l1(false), r1(false) {}
};

class IPad {
public:
    virtual ~IPad() {}

    /// Bring up the controller (load IOP modules, open the port) if not already.
    /// Returns false while no usable pad is present. Cheap after the first success.
    virtual bool ensureReady() = 0;

    /// Read the current button state into @p out. Returns false if the pad is not
    /// readable this cycle (disconnected / transient state).
    virtual bool read(PadButtons* out) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IPAD_HPP
