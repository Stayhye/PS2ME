// PS2 JavaCall port — platform layer. Ps2MemCard implementation.
//
// Memory-card file service over libmc RPC. The mcman/mcserv IRX are loaded by
// init_memcard_driver (ps2_drivers); libmc (mcInit/mcOpen/...) then talks to mcserv over
// SIF. Every libmc call is asynchronous: you issue it and then block on mcSync, whose
// result pointer carries the real return value (fd, byte count, or error). We wrap that
// issue+sync pair in syncResult() and serialize the whole thing on the shared SifLock.
//
// mcOpen's mode is the memory-card attribute mask, NOT POSIX fcntl bits:
//   sceMcFileAttrReadable  (0x01)  read
//   sceMcFileAttrWriteable (0x02)  write
//   sceMcFileCreateFile    (0x200) create if absent (O_CREAT equivalent)
// There is no O_TRUNC on the card, so replacing a file means delete-then-create.
#include "Ps2MemCard.hpp"

extern "C" {
#include <libmc.h>                 // mcInit / mcOpen / mcRead / mcWrite / mcSync / ...
#include <ps2_memcard_driver.h>    // init_memcard_driver (loads mcman + mcserv IRX)
#include <javacall_logging.h>      // javacall_print (boot sign-of-life)
}

#include "SifLock.hpp"
#include "Ps2Storage.hpp"          // boot-dir disk fallback for the config methods

#include <fcntl.h>                 // O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC (disk path)
#include <unistd.h>                // read / write / close (disk path)
#include <stdio.h>                 // snprintf
#include <string.h>                // strlen

namespace ps2 {
namespace platform {

namespace {
// The launcher's directory on the card. Config files live directly under it, e.g.
// "/PS2ME/favorites.txt"; future per-game saves would sit under "/PS2ME/saves/...".
const char* const kAppDir = "/PS2ME";

// Open modes as memory-card attribute masks (see file header).
const int kModeRead   = sceMcFileAttrReadable;                        // 0x01
const int kModeCreate = sceMcFileCreateFile | sceMcFileAttrWriteable; // 0x202

// Build "/PS2ME/<name>" into @p out (capacity @p cap). Returns false if it would not fit.
bool appPath(char* out, int cap, const char* name) {
    const int n = snprintf(out, cap, "%s/%s", kAppDir, name);
    return n > 0 && n < cap;
}
} // namespace

Ps2MemCard& Ps2MemCard::instance() {
    static Ps2MemCard inst;
    return inst;
}

Ps2MemCard::Ps2MemCard()
    : inited_(false), available_(false), port_(0), slot_(0),
      ioActivity_(0), ioBusy_(0) {
    status_[0] = '\0';
}

void Ps2MemCard::ioBegin() {
    // Fire only on the outermost begin: a writeFile is one logical write even though it
    // issues delete + open + write + close underneath.
    if (++ioBusy_ == 1 && ioActivity_ != 0) {
        ioActivity_(kIoBegin);
    }
}

void Ps2MemCard::ioEnd() {
    if (ioBusy_ > 0 && --ioBusy_ == 0 && ioActivity_ != 0) {
        ioActivity_(kIoEnd);
    }
}

int Ps2MemCard::syncResultIo() {
    if (ioActivity_ == 0) {
        return syncResult();           // no UI to animate: just block (e.g. at boot)
    }
    // Poll completion non-blockingly (mcSync mode 1 returns 0 while the function is still
    // executing) and animate one spinner frame per poll, so the write never freezes the UI.
    int cmd = 0, res = 0;
    while (mcSync(1, &cmd, &res) == 0) {
        ioActivity_(kIoTick);
    }
    return res;
}

int Ps2MemCard::syncResult() {
    // mcSync(mode 0) blocks until the pending libmc call finishes and returns 1; the call's
    // own return value lands in `res`. (-1 from mcSync means nothing was registered.)
    int cmd = 0, res = 0;
    while (mcSync(0, &cmd, &res) == 0) {
        // mode 0 is blocking, so this rarely spins; guard against a stray 0 anyway.
    }
    return res;
}

void Ps2MemCard::init() {
    if (inited_) {
        return;
    }
    inited_ = true;
    available_ = false;

    // Load mcman + mcserv (and their sio2man dependency). Idempotent with the pad's own
    // sio2man bring-up (same ps2_drivers guard). Must run after any USB-boot IOP reset
    // (Ps2Storage::mount), which is why the launcher calls us right after mount().
    const enum MEMCARD_INIT_STATUS drv = init_memcard_driver(true);
    if (drv < MEMCARD_INIT_STATUS_OK) {
        snprintf(status_, sizeof(status_), "MC driver error %d", (int)drv);
        javacall_print("[mc] ");
        javacall_print(status_);
        javacall_print("\n");
        return;
    }
    if (mcInit(MC_TYPE_MC) < 0) {
        snprintf(status_, sizeof(status_), "MC lib init failed");
        javacall_print("[mc] MC lib init failed\n");
        return;
    }

    // Probe both card ports. mcGetInfo's mcSync result: 0 = same card as last seen,
    // -1 = a formatted card just appeared, -2 = unformatted card, <= -10 = no card.
    // A freshly booted probe reports -1 for a present formatted card. We accept a PS2-type
    // card that is present and formatted; we never auto-format (that would wipe the user's
    // real saves).
    for (int p = 0; p < 2 && !available_; ++p) {
        SifGuard guard;
        int type = 0, freeClusters = 0, format = 0;
        mcGetInfo(p, 0, &type, &freeClusters, &format);
        const int r = syncResult();
        if (r <= -10) {
            continue;                          // no card in this port
        }
        if (r == -2) {
            snprintf(status_, sizeof(status_), "MC slot %d: unformatted", p + 1);
            continue;                          // present but unusable; keep probing
        }
        if (type != sceMcTypePS2) {
            continue;                          // PS1 / PDA card -- not ours to use
        }
        port_ = p;
        slot_ = 0;
        available_ = true;
        // A PS2 memory-card cluster is 1 KB, so free clusters == free KB.
        snprintf(status_, sizeof(status_), "MC slot %d: ready (%d KB free)",
                 p + 1, freeClusters);
    }

    if (!available_) {
        if (status_[0] == '\0') {
            snprintf(status_, sizeof(status_), "no memory card");
        }
        javacall_print("[mc] ");
        javacall_print(status_);
        javacall_print(" (config falls back to disk)\n");
        return;
    }

    // Ensure the app directory exists so the first config write lands somewhere.
    ensureDir(kAppDir);
    javacall_print("[mc] ");
    javacall_print(status_);
    javacall_print("\n");
}

bool Ps2MemCard::ensureDir(const char* absDir) {
    if (!available_ || absDir == 0) {
        return false;
    }
    SifGuard guard;
    mcMkDir(port_, slot_, absDir);
    const int r = syncResult();
    // 0 = created. A negative result most commonly means "already exists", which is fine;
    // a genuine failure surfaces later when a file write can't open. Treat non-fatal.
    return r >= 0 || r == -4 /* sceMcResNoEntry: dir already present on some servers */;
}

int Ps2MemCard::readFile(const char* absPath, void* buf, int cap) {
    if (!available_ || absPath == 0 || buf == 0 || cap <= 0) {
        return -1;
    }
    SifGuard guard;
    mcOpen(port_, slot_, absPath, kModeRead);
    const int fd = syncResult();
    if (fd < 0) {
        return -1;                             // missing file (or open error)
    }
    int total = 0;
    while (total < cap) {
        mcRead(fd, (char*)buf + total, cap - total);
        const int n = syncResult();
        if (n <= 0) {
            break;                             // EOF or error
        }
        total += n;
    }
    mcClose(fd);
    syncResult();
    return total;
}

bool Ps2MemCard::writeFile(const char* absPath, const void* data, int len) {
    if (!available_ || absPath == 0 || (len > 0 && data == 0) || len < 0) {
        return false;
    }
    // Bracket the write with the activity begin/end so the spinner is set up before the
    // work and cleared after; the syncResultIo calls inside animate it while in flight.
    // (ioBegin runs before we take the SifLock, and the spinner's present is GS/GIF, not
    // SIF, so animating never contends with the memory-card traffic.)
    ioBegin();
    const bool ok = writeFileLocked(absPath, data, len);
    ioEnd();
    return ok;
}

bool Ps2MemCard::writeFileLocked(const char* absPath, const void* data, int len) {
    SifGuard guard;
    // No O_TRUNC on the card: drop any old copy so a shorter new file can't leave a stale
    // tail behind. Ignore the result (the file may not exist yet).
    mcDelete(port_, slot_, absPath);
    syncResultIo();

    mcOpen(port_, slot_, absPath, kModeCreate);
    const int fd = syncResultIo();
    if (fd < 0) {
        return false;
    }
    int written = 0;
    while (written < len) {
        mcWrite(fd, (const char*)data + written, len - written);
        const int n = syncResultIo();
        if (n <= 0) {
            break;
        }
        written += n;
    }
    mcClose(fd);
    syncResultIo();
    return written == len;                      // len 0 => empty file, still success
}

bool Ps2MemCard::removeFile(const char* absPath) {
    if (!available_ || absPath == 0) {
        return false;
    }
    ioBegin();
    const bool ok = removeFileLocked(absPath);
    ioEnd();
    return ok;
}

bool Ps2MemCard::removeFileLocked(const char* absPath) {
    SifGuard guard;
    mcDelete(port_, slot_, absPath);
    const int r = syncResultIo();
    return r >= 0;
}

int Ps2MemCard::configRead(const char* name, void* buf, int cap) {
    if (name == 0) {
        return -1;
    }
    if (!available_) {
        SifGuard guard;
        return diskRead(name, buf, cap);
    }
    char path[64];
    if (!appPath(path, (int)sizeof(path), name)) {
        return -1;
    }
    return readFile(path, buf, cap);
}

bool Ps2MemCard::configWrite(const char* name, const void* data, int len) {
    if (name == 0) {
        return false;
    }
    if (!available_) {
        SifGuard guard;
        return diskWrite(name, data, len);
    }
    char path[64];
    if (!appPath(path, (int)sizeof(path), name)) {
        return false;
    }
    return writeFile(path, data, len);
}

int Ps2MemCard::diskRead(const char* name, void* buf, int cap) {
    char path[256];
    if (!Ps2Storage::instance().diskPath(name, path, (int)sizeof(path))) {
        return -1;
    }
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int total = 0, r = 0;
    while (total < cap &&
           (r = (int)::read(fd, (char*)buf + total, cap - total)) > 0) {
        total += r;
    }
    ::close(fd);
    return total;
}

bool Ps2MemCard::diskWrite(const char* name, const void* data, int len) {
    char path[256];
    if (!Ps2Storage::instance().diskPath(name, path, (int)sizeof(path))) {
        return false;
    }
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }
    int written = 0, w = 0;
    while (written < len &&
           (w = (int)::write(fd, (const char*)data + written, len - written)) > 0) {
        written += w;
    }
    ::close(fd);
    return written == len;
}

} // namespace platform
} // namespace ps2
