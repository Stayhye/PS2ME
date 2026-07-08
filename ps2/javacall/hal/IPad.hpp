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

/// Active-high digital button snapshot. Covers the buttons the J2ME keypad maps
/// plus the face buttons the native front-end uses for its menu actions.
struct PadButtons {
    bool up;
    bool down;
    bool left;
    bool right;
    bool cross;      // primary action / fire / launch
    bool circle;     // back / cancel (native menu)
    bool triangle;   // favourite (native menu)
    bool square;     // sort (native menu)
    bool l1;         // left soft key / prev tab
    bool r1;         // right soft key / next tab
    bool l2;         // alphabet jump (prev section)
    bool r2;         // alphabet jump (next section)
    bool select;     // per-game options (native menu: screen size panel)
    bool start;      // reserved (native menu)

    PadButtons()
        : up(false), down(false), left(false), right(false),
          cross(false), circle(false), triangle(false), square(false),
          l1(false), r1(false), l2(false), r2(false),
          select(false), start(false) {}
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
