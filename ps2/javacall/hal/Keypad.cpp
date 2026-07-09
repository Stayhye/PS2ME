// PS2 JavaCall port — HAL layer. Keypad implementation.
#include "Keypad.hpp"

extern "C" {
#include <javacall_keypress.h>   // javacall_key / javacall_keypress_type / javanotify_key_event
}

namespace ps2 {
namespace hal {

namespace {

// Analog stick geometry. Axes are 0..255 with 128 the centre; a direction latches on
// once its offset from centre exceeds ON and releases only below OFF (hysteresis), so a
// stick resting near the edge doesn't chatter presses/releases.
const int AXIS_CENTRE = 128;
const int AXIS_ON     = 56;   // activate beyond this offset
const int AXIS_OFF    = 40;   // release below this offset

// Update one direction latch along an axis. @p positive tests the +offset side
// (rightward / downward); otherwise the -offset side (leftward / upward).
void latchDir(bool* latched, int value, bool positive) {
    const int off = positive ? (value - AXIS_CENTRE) : (AXIS_CENTRE - value);
    if (*latched) {
        if (off < AXIS_OFF) *latched = false;
    } else {
        if (off > AXIS_ON) *latched = true;
    }
}

// Map a right-stick offset (screen coords: +dy is downward) to the phone numpad by
// compass sector — N=2 S=8 E=6 W=4, and the diagonals NW=1 NE=3 SW=7 SE=9. Returns the
// ASCII digit. Diagonal band is the middle ~45deg of each quadrant (tan 22.5 ~= 0.414).
int octantKey(int dx, int dy) {
    const int uy = -dy;                       // north-positive
    const int ax = dx < 0 ? -dx : dx;
    const int ay = uy < 0 ? -uy : uy;
    const bool diag = (ax * 1000 > ay * 414) && (ay * 1000 > ax * 414);
    if (diag) {
        if (uy > 0) return dx > 0 ? '3' : '1';   // NE / NW
        else        return dx > 0 ? '9' : '7';   // SE / SW
    }
    if (ax > ay) return dx > 0 ? '6' : '4';       // E / W
    return uy > 0 ? '2' : '8';                    // N / S
}

bool contains(const int* arr, int n, int key) {
    for (int i = 0; i < n; ++i) {
        if (arr[i] == key) return true;
    }
    return false;
}

void addKey(int* arr, int* n, int cap, int key) {
    if (*n < cap && !contains(arr, *n, key)) {
        arr[(*n)++] = key;
    }
}

} // namespace

Keypad& Keypad::instance() {
    static Keypad inst;
    return inst;
}

void Keypad::computeActive(const PadButtons& b, int* keys, int* count) {
    int n = 0;

    // Left stick -> 8-way direction (both layouts), OR'd with the physical D-pad.
    latchDir(&lUp_,    b.ly, false);
    latchDir(&lDown_,  b.ly, true);
    latchDir(&lLeft_,  b.lx, false);
    latchDir(&lRight_, b.lx, true);
    const bool up    = b.up    || lUp_;
    const bool down  = b.down  || lDown_;
    const bool left  = b.left  || lLeft_;
    const bool right = b.right || lRight_;

    if (layout_ == LAYOUT_SIMPLE) {
        if (up)    addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_UP);
        if (down)  addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_DOWN);
        if (left)  addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_LEFT);
        if (right) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_RIGHT);
        if (b.cross) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_SELECT);
        if (b.l1)    addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_SOFT1);
        if (b.r1)    addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_SOFT2);
        *count = n;
        return;
    }

    // LAYOUT_COMPLETE: full phone keypad. D-pad double-emits arrow + digit; the digit
    // reaches getGameAction as well, so 2/4/6/8 cover number-driven and arrow-driven
    // games alike. (Phase 2 will gate the digit to Canvas foregrounds.)
    if (up)    { addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_UP);    addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_2); }
    if (down)  { addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_DOWN);  addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_8); }
    if (left)  { addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_LEFT);  addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_4); }
    if (right) { addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_RIGHT); addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_6); }

    if (b.cross) { addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_SELECT); addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_5); }
    if (b.square)   addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_ASTERISK);   // [] -> *
    if (b.triangle) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_0);          // /\ -> 0
    if (b.circle)   addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_POUND);      // O  -> #

    if (b.l1) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_1);   // = GAME_A
    if (b.r1) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_3);   // = GAME_B
    if (b.l2) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_7);   // = GAME_C
    if (b.r2) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_9);   // = GAME_D

    if (b.select) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_SOFT1);
    if (b.start)  addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_SOFT2);

    // Right stick -> one numpad key by sector (hysteresis on magnitude). While held past
    // the release radius it keeps re-aiming to the current sector.
    {
        const int dx = b.rx - AXIS_CENTRE, dy = b.ry - AXIS_CENTRE;
        const int mag2 = dx * dx + dy * dy;
        if (rSectorKey_ == 0) {
            if (mag2 > AXIS_ON * AXIS_ON) rSectorKey_ = octantKey(dx, dy);
        } else if (mag2 < AXIS_OFF * AXIS_OFF) {
            rSectorKey_ = 0;
        } else {
            rSectorKey_ = octantKey(dx, dy);
        }
        if (rSectorKey_ != 0) addKey(keys, &n, MAX_KEYS, rSectorKey_);
    }
    if (b.r3) addKey(keys, &n, MAX_KEYS, JAVACALL_KEY_5);   // right-stick click -> 5

    *count = n;
}

void Keypad::onPoll() {
    if (pad_ == 0 || !pad_->ensureReady()) {
        return;
    }
    PadButtons b;
    if (!pad_->read(&b)) {
        return;
    }

    int keys[MAX_KEYS];
    int n = 0;
    computeActive(b, keys, &n);

    // First successful read only seeds the previous set, so an already-held key at
    // startup doesn't fire a bogus press and a fresh pad doesn't fire bogus releases.
    if (havePrev_) {
        // Releases: keys held last frame but not this one.
        for (int i = 0; i < prevCount_; ++i) {
            if (!contains(keys, n, prevKeys_[i])) {
                javanotify_key_event(static_cast<javacall_key>(prevKeys_[i]),
                                     JAVACALL_KEYRELEASED);
            }
        }
        // Presses: keys held this frame but not the last.
        for (int i = 0; i < n; ++i) {
            if (!contains(prevKeys_, prevCount_, keys[i])) {
                javanotify_key_event(static_cast<javacall_key>(keys[i]),
                                     JAVACALL_KEYPRESSED);
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        prevKeys_[i] = keys[i];
    }
    prevCount_ = n;
    havePrev_  = true;
}

} // namespace hal
} // namespace ps2
