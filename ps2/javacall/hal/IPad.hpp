// PS2 JavaCall port — HAL layer.
//
// IPad: the game controller abstraction the Keypad reads through. It reports the
// digital buttons the MIDP keypad mapping needs plus the two analog sticks and their
// clicks (used by the "Complete" control layout), decoupled from ps2sdk/libpad
// (whose access lives in platform::Ps2Pad).
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

    // Analog sticks: 0..255 per axis, 128 = centre (a digital-only pad reports the
    // centre, so these are inert on it). h grows rightward, v grows downward.
    int  lx, ly;     // left stick
    int  rx, ry;     // right stick
    bool l3, r3;     // stick clicks (L3/R3)

    PadButtons()
        : up(false), down(false), left(false), right(false),
          cross(false), circle(false), triangle(false), square(false),
          l1(false), r1(false), l2(false), r2(false),
          select(false), start(false),
          lx(128), ly(128), rx(128), ry(128), l3(false), r3(false) {}
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
