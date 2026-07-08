// PS2 JavaCall port — platform layer. Recent implementation.
//
// Most-recently-launched games stored by JAR name (one per line, most-recent first) in
// <bootdir>recent.txt, resolved to game indices at load(). See Recent.hpp.
#include "Recent.hpp"

#include "Ps2Storage.hpp"
#include "SifLock.hpp"
#include "../hal/GameStorage.hpp"

#include <fcntl.h>      // open, O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC
#include <unistd.h>     // read, write, close
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

    char path[256];
    if (!Ps2Storage::instance().recentPath(path, (int)sizeof(path))) {
        return;
    }

    SifGuard guard;   // file I/O touches SIF
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return;       // no history yet
    }
    static char buf[16 * 1024];
    int total = 0, r = 0;
    while (total < (int)sizeof(buf) - 1 &&
           (r = (int)::read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
    }
    ::close(fd);
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
    char path[256];
    if (!Ps2Storage::instance().recentPath(path, (int)sizeof(path))) {
        return;
    }
    SifGuard guard;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    for (int i = 0; i < count_; ++i) {
        const char* nm = hal::GameStorage::instance().nameAt(recent_[i]);
        if (nm == 0 || nm[0] == '\0') {
            continue;
        }
        ::write(fd, nm, (int)strlen(nm));
        ::write(fd, "\n", 1);
    }
    ::close(fd);
}

} // namespace platform
} // namespace ps2
