// PS2 JavaCall port — HAL layer. Keypad implementation.
#include "Keypad.hpp"

extern "C" {
#include <javacall_keypress.h>   // javacall_key / javacall_keypress_type / javanotify_key_event
}

namespace ps2 {
namespace hal {

Keypad& Keypad::instance() {
    static Keypad inst;
    return inst;
}

void Keypad::emit(bool now, bool before, int javacallKey) {
    if (now == before) {
        return;
    }
    javanotify_key_event(static_cast<javacall_key>(javacallKey),
                         now ? JAVACALL_KEYPRESSED : JAVACALL_KEYRELEASED);
}

void Keypad::onPoll() {
    if (pad_ == 0 || !pad_->ensureReady()) {
        return;
    }
    PadButtons b;
    if (!pad_->read(&b)) {
        return;
    }

    // First successful read only seeds prev_, so an already-held button at startup
    // doesn't fire a bogus press and a fresh pad doesn't fire bogus releases.
    if (havePrev_) {
        emit(b.up,    prev_.up,    JAVACALL_KEY_UP);
        emit(b.down,  prev_.down,  JAVACALL_KEY_DOWN);
        emit(b.left,  prev_.left,  JAVACALL_KEY_LEFT);
        emit(b.right, prev_.right, JAVACALL_KEY_RIGHT);
        emit(b.cross, prev_.cross, JAVACALL_KEY_SELECT);
        emit(b.l1,    prev_.l1,    JAVACALL_KEY_SOFT1);
        emit(b.r1,    prev_.r1,    JAVACALL_KEY_SOFT2);
    }

    prev_     = b;
    havePrev_ = true;
}

} // namespace hal
} // namespace ps2
