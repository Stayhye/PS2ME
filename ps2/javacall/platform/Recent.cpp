// PS2 JavaCall port — platform layer. Recent implementation.
//
// Most-recently-launched games stored by JAR name (one per line, most-recent first) in
// "recent.txt", persisted on the memory card (falling back to the boot directory) via
// Ps2MemCard, resolved to game indices at load(). See Recent.hpp.
#include "Recent.hpp"

#include "Ps2MemCard.hpp"
#include "../hal/GameStorage.hpp"

#include <stdio.h>      // snprintf
#include <string.h>     // strcmp, strlen

namespace ps2 {
namespace platform {

Recent& Recent::instance() {
    static Recent inst;
    return inst;
}

Recent::Recent() : gameCount_(0), count_(0) {}

void Recent::load(int gameCount) {
    gameCount_ = gameCount < 0 ? 0 : gameCount;
    count_ = 0;

    static char buf[16 * 1024];
    int total = Ps2MemCard::instance().configRead("recent.txt", buf, (int)sizeof(buf) - 1);
    if (total <= 0) {
        return;       // no history yet
    }
    buf[total] = '\0';

    // One JAR name per line, most-recent first; resolve each to a present game index.
    int i = 0;
    while (i < total && count_ < MAX_KEEP) {
        const int start = i;
        while (i < total && buf[i] != '\n' && buf[i] != '\r') {
            ++i;
        }
        const int len = i - start;
        while (i < total && (buf[i] == '\n' || buf[i] == '\r')) {
            ++i;
        }
        if (len <= 0) {
            continue;
        }
        const char saved = buf[start + len];
        buf[start + len] = '\0';
        const char* name = &buf[start];
        for (int g = 0; g < gameCount_; ++g) {
            const char* gn = hal::GameStorage::instance().nameAt(g);
            if (gn != 0 && strcmp(gn, name) == 0) {
                bool dup = false;
                for (int k = 0; k < count_; ++k) {
                    if (recent_[k] == g) { dup = true; break; }
                }
                if (!dup) recent_[count_++] = g;
                break;
            }
        }
        buf[start + len] = saved;
    }
}

int Recent::list(int* out, int cap) const {
    int n = 0;
    for (int i = 0; i < count_ && n < cap; ++i) {
        out[n++] = recent_[i];
    }
    return n;
}

void Recent::push(int game) {
    if (game < 0) {
        return;
    }
    // New order: the launched game first, then the previous entries minus it.
    int neworder[MAX_KEEP];
    int n = 0;
    neworder[n++] = game;
    for (int i = 0; i < count_ && n < MAX_KEEP; ++i) {
        if (recent_[i] != game) {
            neworder[n++] = recent_[i];
        }
    }
    for (int i = 0; i < n; ++i) {
        recent_[i] = neworder[i];
    }
    count_ = n;
    save();
}

void Recent::clear() {
    count_ = 0;
    save();
}

void Recent::save() const {
    // Build the whole "name\n" list (most-recent first), then persist it in one write.
    char buf[16 * 1024];
    int len = 0;
    for (int i = 0; i < count_; ++i) {
        const char* nm = hal::GameStorage::instance().nameAt(recent_[i]);
        if (nm == 0 || nm[0] == '\0') {
            continue;
        }
        const int nl = (int)strlen(nm);
        if (len + nl + 1 > (int)sizeof(buf)) {
            break;
        }
        memcpy(buf + len, nm, nl);
        len += nl;
        buf[len++] = '\n';
    }
    Ps2MemCard::instance().configWrite("recent.txt", buf, len);
}

} // namespace platform
} // namespace ps2
