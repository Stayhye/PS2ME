// PS2 JavaCall port — platform layer. Favorites implementation.
//
// A game is stored by JAR name (one per line) in <bootdir>favorites.txt, resolved to
// game indices against the current list at load(). See Favorites.hpp for the model.
#include "Favorites.hpp"

#include "Ps2Storage.hpp"
#include "SifLock.hpp"
#include "../hal/GameStorage.hpp"

#include <fcntl.h>      // open, O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC
#include <unistd.h>     // read, write, close
#include <string.h>     // strcmp, strlen

namespace ps2 {
namespace platform {

Favorites& Favorites::instance() {
    static Favorites inst;
    return inst;
}

Favorites::Favorites() : loaded_(false), gameCount_(0) {
    for (int i = 0; i < MAX_GAMES; ++i) {
        fav_[i] = false;
    }
}

void Favorites::load(int gameCount) {
    gameCount_ = gameCount < 0 ? 0 : (gameCount > MAX_GAMES ? MAX_GAMES : gameCount);
    for (int i = 0; i < MAX_GAMES; ++i) {
        fav_[i] = false;
    }
    loaded_ = true;

    char path[256];
    if (!Ps2Storage::instance().favoritesPath(path, (int)sizeof(path))) {
        return;
    }

    SifGuard guard;   // file I/O touches SIF
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return;       // no file yet -> no favourites
    }
    static char buf[64 * 1024];       // the list is small; read it whole
    int total = 0, r = 0;
    while (total < (int)sizeof(buf) - 1 &&
           (r = (int)::read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
    }
    ::close(fd);
    buf[total] = '\0';

    // One JAR name per line; resolve each to a game index (linear -- few favourites).
    int i = 0;
    while (i < total) {
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
                fav_[g] = true;
                break;
            }
        }
        buf[start + len] = saved;
    }
}

bool Favorites::isFavorite(int game) const {
    return (game >= 0 && game < MAX_GAMES) ? fav_[game] : false;
}

void Favorites::toggle(int game) {
    if (game < 0 || game >= MAX_GAMES) {
        return;
    }
    fav_[game] = !fav_[game];
    save();
}

int Favorites::count() const {
    int n = 0;
    for (int g = 0; g < gameCount_; ++g) {
        if (fav_[g]) ++n;
    }
    return n;
}

int Favorites::list(int* out, int cap) const {
    int n = 0;
    for (int g = 0; g < gameCount_ && n < cap; ++g) {
        if (fav_[g]) {
            out[n++] = g;
        }
    }
    return n;
}

void Favorites::save() const {
    char path[256];
    if (!Ps2Storage::instance().favoritesPath(path, (int)sizeof(path))) {
        return;
    }
    SifGuard guard;   // serialize with the icon worker's SIF I/O
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    for (int g = 0; g < gameCount_; ++g) {
        if (!fav_[g]) {
            continue;
        }
        const char* nm = hal::GameStorage::instance().nameAt(g);
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
