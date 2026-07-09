// PS2 JavaCall port — platform layer. Settings implementation.
//
// A tiny key=value file "settings.txt" ("recent=0|1", "sort=0|1", "debug=0|1", "fps=0|1",
// "music=0|1", "controllayout=0|1"), persisted on the memory card (falling back to the
// boot directory) via Ps2MemCard. See Settings.hpp.
#include "Settings.hpp"

#include "Ps2MemCard.hpp"

#include <stdio.h>      // snprintf
#include <string.h>     // strncmp

namespace ps2 {
namespace platform {

Settings& Settings::instance() {
    static Settings inst;
    return inst;
}

Settings::Settings()
    : showRecent_(true), defaultSort_(0), debugMode_(false), fpsCounter_(false),
      bgmEnabled_(true), controlLayout_(0), loaded_(false) {}

void Settings::load() {
    showRecent_    = true;    // defaults
    defaultSort_   = 0;
    debugMode_     = false;
    fpsCounter_    = false;
    bgmEnabled_    = true;
    controlLayout_ = 0;
    loaded_ = true;

    char buf[512];
    int total = Ps2MemCard::instance().configRead("settings.txt", buf, (int)sizeof(buf) - 1);
    if (total <= 0) {
        return;             // no file -> defaults
    }
    buf[total] = '\0';

    // Parse "key=value" lines.
    int i = 0;
    while (i < total) {
        const int start = i;
        while (i < total && buf[i] != '\n' && buf[i] != '\r') ++i;
        const int len = i - start;
        while (i < total && (buf[i] == '\n' || buf[i] == '\r')) ++i;
        if (len < 3) continue;
        const char* line = &buf[start];
        if (strncmp(line, "recent=", 7) == 0 && len >= 8) {
            showRecent_ = (line[7] != '0');
        } else if (strncmp(line, "sort=", 5) == 0 && len >= 6) {
            defaultSort_ = (line[5] == '1') ? 1 : 0;
        } else if (strncmp(line, "debug=", 6) == 0 && len >= 7) {
            debugMode_ = (line[6] != '0');
        } else if (strncmp(line, "fps=", 4) == 0 && len >= 5) {
            fpsCounter_ = (line[4] != '0');
        } else if (strncmp(line, "music=", 6) == 0 && len >= 7) {
            bgmEnabled_ = (line[6] != '0');
        } else if (strncmp(line, "controllayout=", 14) == 0 && len >= 15) {
            controlLayout_ = (line[14] == '1') ? 1 : 0;
        }
    }
}

void Settings::setShowRecent(bool on) {
    showRecent_ = on;
    save();
}

void Settings::setDefaultSort(int mode) {
    defaultSort_ = (mode == 1) ? 1 : 0;
    save();
}

void Settings::setDebugMode(bool on) {
    debugMode_ = on;
    save();
}

void Settings::setFpsCounter(bool on) {
    fpsCounter_ = on;
    save();
}

void Settings::setBgmEnabled(bool on) {
    bgmEnabled_ = on;
    save();
}

void Settings::setControlLayout(int mode) {
    controlLayout_ = (mode == 1) ? 1 : 0;
    save();
}

void Settings::save() const {
    char buf[128];
    const int n = snprintf(buf, (int)sizeof(buf),
                           "recent=%d\nsort=%d\ndebug=%d\nfps=%d\nmusic=%d\ncontrollayout=%d\n",
                           showRecent_ ? 1 : 0, defaultSort_, debugMode_ ? 1 : 0,
                           fpsCounter_ ? 1 : 0, bgmEnabled_ ? 1 : 0, controlLayout_);
    if (n > 0) {
        Ps2MemCard::instance().configWrite("settings.txt", buf, n);
    }
}

} // namespace platform
} // namespace ps2
