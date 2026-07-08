// PS2 JavaCall port — platform layer. Favorites implementation.
//
// A game is stored by JAR name (one per line) in "favorites.txt", persisted on the memory
// card (falling back to the boot directory) via Ps2MemCard, resolved to game indices
// against the current list at load(). See Favorites.hpp for the model.
#include "Favorites.hpp"

#include "Ps2MemCard.hpp"
#include "../hal/GameStorage.hpp"

#include <stdio.h>      // snprintf
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

    static char buf[64 * 1024];       // the list is small; read it whole
    int total = Ps2MemCard::instance().configRead("favorites.txt", buf, (int)sizeof(buf) - 1);
    if (total <= 0) {
        return;       // no file yet -> no favourites
    }
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

void Favorites::clear() {
    for (int i = 0; i < MAX_GAMES; ++i) {
        fav_[i] = false;
    }
    save();
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
    // Build the whole "name\n" list, then persist it in one write (the memory card has no
    // streaming append; it also lets an empty list write a legitimately empty file).
    static char buf[64 * 1024];
    int len = 0;
    for (int g = 0; g < gameCount_; ++g) {
        if (!fav_[g]) {
            continue;
        }
        const char* nm = hal::GameStorage::instance().nameAt(g);
        if (nm == 0 || nm[0] == '\0') {
            continue;
        }
        const int nl = (int)strlen(nm);
        if (len + nl + 1 > (int)sizeof(buf)) {
            break;                              // out of room (pathological); keep what fits
        }
        memcpy(buf + len, nm, nl);
        len += nl;
        buf[len++] = '\n';
    }
    Ps2MemCard::instance().configWrite("favorites.txt", buf, len);
}

} // namespace platform
} // namespace ps2
