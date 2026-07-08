// PS2 JavaCall port — platform layer. Settings implementation.
//
// A tiny key=value file (<bootdir>settings.txt): "recent=0|1", "sort=0|1",
// "debug=0|1". See Settings.hpp for the model.
#include "Settings.hpp"

#include "Ps2Storage.hpp"
#include "SifLock.hpp"

#include <fcntl.h>      // open, O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC
#include <unistd.h>     // read, write, close
#include <stdio.h>      // snprintf
#include <string.h>     // strncmp

namespace ps2 {
namespace platform {

Settings& Settings::instance() {
    static Settings inst;
    return inst;
}

Settings::Settings() : showRecent_(true), defaultSort_(0), debugMode_(false), loaded_(false) {}

void Settings::load() {
    showRecent_  = true;    // defaults
    defaultSort_ = 0;
    debugMode_   = false;
    loaded_ = true;

    char path[256];
    if (!Ps2Storage::instance().settingsPath(path, (int)sizeof(path))) {
        return;
    }
    SifGuard guard;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return;             // no file -> defaults
    }
    char buf[512];
    int total = 0, r = 0;
    while (total < (int)sizeof(buf) - 1 &&
           (r = (int)::read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
    }
    ::close(fd);
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

void Settings::save() const {
    char path[256];
    if (!Ps2Storage::instance().settingsPath(path, (int)sizeof(path))) {
        return;
    }
    SifGuard guard;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    char line[32];
    int n = snprintf(line, (int)sizeof(line), "recent=%d\n", showRecent_ ? 1 : 0);
    ::write(fd, line, n);
    n = snprintf(line, (int)sizeof(line), "sort=%d\n", defaultSort_);
    ::write(fd, line, n);
    n = snprintf(line, (int)sizeof(line), "debug=%d\n", debugMode_ ? 1 : 0);
    ::write(fd, line, n);
    ::close(fd);
}

} // namespace platform
} // namespace ps2
