// PS2 JavaCall port — platform layer. Ps2Storage implementation.
//
// Storage bring-up + on-disk icon-tile cache, both anchored to the ELF's launch
// directory (argv[0]). The USB device prefix the loader used (mass:/mass0:/usb0:) may
// differ from what our own modules mount, so the games directory is resolved by
// probing candidate prefixes. See Ps2Storage.hpp for the model.
#include "Ps2Storage.hpp"

extern "C" {
#include <sifrpc.h>
#include <iopcontrol.h>               // SifIopReset / SifIopSync
#include <sbv_patches.h>
#include <ps2_fileXio_driver.h>       // init_fileXio_driver
#include <ps2_usb_driver.h>           // init_usb_driver (usbd + bdm + fatfs)
#include <ps2_filesystem_driver.h>    // waitUntilDeviceIsReady, getBootDeviceID, rootDevicePath
#include <javacall_logging.h>         // javacall_print (boot sign-of-life)
}
#include <fcntl.h>      // open, O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC
#include <unistd.h>     // read, write, close
#include <sys/stat.h>   // mkdir
#include <dirent.h>     // opendir / closedir
#include <stdio.h>      // snprintf, remove, vsnprintf
#include <stdlib.h>     // malloc / free
#include <string.h>     // strcmp, strncmp
#include <stdarg.h>     // va_list (diagnostic notes)

namespace ps2 {
namespace platform {

namespace {
// Copy @p src into @p dst (capacity @p cap) keeping only filesystem-safe characters;
// anything else becomes '_'. Keeps cache names short and FAT-legal.
void sanitize(char* dst, int cap, const char* src) {
    int n = 0;
    for (const char* p = src; *p != '\0' && n < cap - 1; ++p) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        dst[n++] = ok ? c : '_';
    }
    dst[n] = '\0';
}

} // namespace

Ps2Storage& Ps2Storage::instance() {
    static Ps2Storage inst;
    return inst;
}

Ps2Storage::Ps2Storage()
    : mounted_(false), cacheOk_(false), gamesResolved_(false), baseIsMass_(false),
      diagLen_(0) {
    device_[0]   = '\0';
    subDir_[0]   = '\0';
    baseDir_[0]  = '\0';
    cacheDir_[0] = '\0';
    gamesDir_[0] = '\0';
    diag_[0]     = '\0';
}

void Ps2Storage::note(const char* fmt, ...) {
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    // Append to the on-screen trace (leave room for '\n' + terminator).
    for (int i = 0; line[i] != '\0' && diagLen_ < (int)sizeof(diag_) - 2; ++i) {
        diag_[diagLen_++] = line[i];
    }
    if (diagLen_ < (int)sizeof(diag_) - 1) {
        diag_[diagLen_++] = '\n';
    }
    diag_[diagLen_] = '\0';
    // And to the EE console for anyone capturing it.
    javacall_print(line);
    javacall_print("\n");
}

void Ps2Storage::splitBootPath(const char* argv0) {
    device_[0] = '\0';
    subDir_[0] = '\0';
    baseIsMass_ = false;
    if (argv0 == 0 || argv0[0] == '\0') {
        return;
    }
    // Device prefix = everything up to and including the first ':'.
    int i = 0;
    while (argv0[i] != '\0' && argv0[i] != ':' && i < (int)sizeof(device_) - 2) {
        device_[i] = argv0[i];
        ++i;
    }
    const char* rest;
    if (argv0[i] == ':') {
        device_[i] = ':';
        device_[i + 1] = '\0';
        rest = argv0 + i + 1;               // path after "device:"
    } else {
        device_[0] = '\0';                  // no device prefix
        rest = argv0;
    }
    // Directory suffix, up to and including the last separator, NORMALIZED to always
    // start with '/' (and with backslashes converted). Some loaders pass the path
    // without a slash after the colon, e.g. "mass:PS2ME/app.elf" -- but fatfs opendir
    // needs "mass0:/PS2ME/games", so the separator is mandatory. Root ELF -> "/".
    if (*rest == '/' || *rest == '\\') {
        ++rest;                              // drop a leading separator; we re-add one
    }
    int n = 0, lastSep = 0;
    subDir_[n++] = '/';                       // always begin with a separator
    for (; *rest != '\0' && n < (int)sizeof(subDir_) - 1; ++rest) {
        const char c = (*rest == '\\') ? '/' : *rest;
        subDir_[n] = c;
        if (c == '/') {
            lastSep = n;
        }
        ++n;
    }
    subDir_[lastSep + 1] = '\0';              // keep through the last separator
    baseIsMass_ = (strncmp(device_, "mass", 4) == 0) ||
                  (strncmp(device_, "usb", 3) == 0);
}

bool Ps2Storage::tryBase(const char* base, bool already) {
    // base ends with '/'. Probe the dir itself AND its games/ subfolder, so the trace
    // distinguishes "device unreachable" from "games folder missing".
    char g[208];
    snprintf(g, sizeof(g), "%sgames", base);
    DIR* dr = ::opendir(base);
    DIR* dg = ::opendir(g);
    note("%s dir=%s games=%s", base, dr ? "y" : "n", dg ? "y" : "n");
    bool hit = false;
    if (dr) {
        ::closedir(dr);
    }
    if (dg) {
        ::closedir(dg);
        if (!already) {
            snprintf(baseDir_, sizeof(baseDir_), "%s", base);
            snprintf(gamesDir_, sizeof(gamesDir_), "%s", g);
            hit = true;
        }
    }
    return hit;
}

void Ps2Storage::mount(const char* argv0) {
    if (mounted_) {
        return;
    }
    mounted_ = true;
    splitBootPath(argv0);
    note("argv0=%s", (argv0 != 0 && argv0[0] != '\0') ? argv0 : "(null)");

    SifInitRpc(0);

    // When booted from USB, the loader (wLE/uLaunchELF) left its own USB/fileio IRX
    // resident on the IOP (that's how it read our ELF). Loading OUR iomanX/usbd on top
    // conflicts -- they fail (init fileXio=-2 usb=-5). Reset the IOP to a clean ROM
    // state so our modules load. This drops the SIF tty (EE console), but on real
    // hardware that's invisible and our diagnostics render on-screen. Skip it for host:
    // (PCSX2) so its console + HostFS survive. getcwd/__cwd is EE-side, unaffected.
    // Debug build (PS2ME_NO_IOP_RESET): skip the reset so the SIF tty (EE console)
    // survives on real hardware and we can capture the boot trace over TTY. The
    // tradeoff is that our iomanX/usb modules may collide with the loader's resident
    // ones (init fileXio/usb may fail) -- that's exactly what we're inspecting.
#ifndef PS2ME_NO_IOP_RESET
    if (baseIsMass_) {
        while (!SifIopReset("", 0)) {}
        while (!SifIopSync()) {}
        SifInitRpc(0);
        note("iop reset (usb boot)");
    }
#else
    note("iop reset SKIPPED (tty debug build)");
#endif

    // Load the storage IRX: needs the SBV "load module from buffer" patches applied
    // first. init_fileXio_driver() also installs the fileXio->newlib POSIX bridge
    // (fileXioInit -> _ps2sdk_fileXio_init) so opendir/open reach mass:; the USB driver
    // (with dependencies) brings usbd + bdm + usbmass_bd + bdmfs_fatfs.
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    const int fx = (int)init_fileXio_driver();
    const int ub = (int)init_usb_driver(true);
    note("init fileXio=%d usb=%d", fx, ub);

    // Canonical boot dir from the runtime CWD: crt0 chdir'd to the ELF's directory and
    // the libc normalized the "device:" prefix + slashes (so "mass:PS2ME/app.elf"
    // becomes cwd "mass:/PS2ME"). More reliable than parsing argv0 ourselves.
    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) == 0) {
        cwd[0] = '\0';
    }
    note("cwd=%s dev=%s sub=%s", cwd[0] ? cwd : "(none)",
         device_[0] ? device_ : "(none)", subDir_[0] ? subDir_ : "/");

    // USB enumerates asynchronously; wait ONCE for the boot dir to become reachable.
    // waitUntilDeviceIsReady already does ~500 internal stat retries, so calling it in
    // an outer loop makes the boot hang for minutes when the device never appears.
    char cwdBase[132];
    cwdBase[0] = '\0';
    if (cwd[0]) {
        int n = snprintf(cwdBase, sizeof(cwdBase), "%s", cwd);
        if (n > 0 && n < (int)sizeof(cwdBase) - 1 && cwd[n - 1] != '/' && cwd[n - 1] != '\\') {
            cwdBase[n] = '/';
            cwdBase[n + 1] = '\0';
        }
        // Only wait for USB-ish devices (each waitUntilDeviceIsReady call burns ~8.5s
        // of nopdelay if the device never appears; host: on PCSX2 is instant).
        const bool cwdIsMass = (strncmp(cwd, "mass", 4) == 0) ||
                               (strncmp(cwd, "usb", 3) == 0);
        if (cwdIsMass || baseIsMass_) {
            char w[132];
            snprintf(w, sizeof(w), "%s", cwd);
            const bool rdy = (waitUntilDeviceIsReady(w) != false);
            note("ready(%s)=%d", cwd, rdy ? 1 : 0);
        }
    }

    // Probe candidate base dirs (each ends with '/'), most likely first: the cwd, the
    // argv0-derived dir, then the common USB fatfs prefixes with the argv0 subdir.
    gamesResolved_ = true;
    bool found = false;
    if (cwdBase[0]) {
        found = tryBase(cwdBase, found) || found;
    }
    const char* prefixes[5] = { device_[0] ? device_ : "mass:",
                                "mass:", "mass0:", "usb0:", "mass1:" };
    for (int i = 0; i < 5; ++i) {
        char base[192];
        snprintf(base, sizeof(base), "%s%s", prefixes[i], subDir_);
        if (cwdBase[0] && strcmp(base, cwdBase) == 0) {
            continue;                            // already probed as cwd
        }
        bool skip = false;                       // cheap dedupe against earlier prefixes
        for (int j = 0; j < i; ++j) {
            if (strcmp(prefixes[j], prefixes[i]) == 0) { skip = true; break; }
        }
        if (skip) {
            continue;
        }
        if (tryBase(base, found)) {
            found = true;
        }
    }
    if (!found) {
        snprintf(gamesDir_, sizeof(gamesDir_), "host:games");   // dev fallback (PCSX2)
        note("games NOT found; using host:games");
    } else {
        note("games from %s", gamesDir_);
    }

    // Cache lives beside the games, but only where writable (USB/host yes, cdvd ROM
    // no). Probe by creating the cache dir and writing a byte. Needs a resolved base.
    if (baseDir_[0] != '\0') {
        snprintf(cacheDir_, sizeof(cacheDir_), "%scache", baseDir_);
        ::mkdir(cacheDir_, 0777);
        char probe[224];
        snprintf(probe, sizeof(probe), "%s/.pw", cacheDir_);
        const int fd = ::open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            const char b = 'x';
            ::write(fd, &b, 1);
            ::close(fd);
            ::remove(probe);
            cacheOk_ = true;
        }
    }
    if (cacheOk_) {
        note("cache enabled: %s", cacheDir_);
    } else {
        note("cache disabled (location not writable)");
    }
}

const char* Ps2Storage::gamesDir() {
    if (!gamesResolved_) {
        // mount() should have run; fall back defensively.
        gamesResolved_ = true;
        snprintf(gamesDir_, sizeof(gamesDir_), "host:games");
    }
    return gamesDir_;
}

bool Ps2Storage::bankPath(char* out, int cap) {
    // The bank sits beside the games (same resolved boot dir). If the boot dir was
    // never resolved (no games folder found), fall back to host: like gamesDir().
    const int n = (baseDir_[0] != '\0')
                      ? snprintf(out, cap, "%sbank.bin", baseDir_)
                      : snprintf(out, cap, "host:bank.bin");
    return n > 0 && n < cap;
}

bool Ps2Storage::favoritesPath(char* out, int cap) {
    // Sits beside the games (same resolved boot dir); host: fallback like bankPath().
    const int n = (baseDir_[0] != '\0')
                      ? snprintf(out, cap, "%sfavorites.txt", baseDir_)
                      : snprintf(out, cap, "host:favorites.txt");
    return n > 0 && n < cap;
}

bool Ps2Storage::recentPath(char* out, int cap) {
    const int n = (baseDir_[0] != '\0')
                      ? snprintf(out, cap, "%srecent.txt", baseDir_)
                      : snprintf(out, cap, "host:recent.txt");
    return n > 0 && n < cap;
}

bool Ps2Storage::settingsPath(char* out, int cap) {
    const int n = (baseDir_[0] != '\0')
                      ? snprintf(out, cap, "%ssettings.txt", baseDir_)
                      : snprintf(out, cap, "host:settings.txt");
    return n > 0 && n < cap;
}

bool Ps2Storage::resolutionsPath(char* out, int cap) {
    const int n = (baseDir_[0] != '\0')
                      ? snprintf(out, cap, "%sresolutions.txt", baseDir_)
                      : snprintf(out, cap, "host:resolutions.txt");
    return n > 0 && n < cap;
}

bool Ps2Storage::cachePath(char* out, int cap, const char* jarName,
                           int jarSize, int iconSize) {
    char safe[96];
    sanitize(safe, (int)sizeof(safe), jarName);
    const int n = snprintf(out, cap, "%s/%s_%d_%d.tl",
                           cacheDir_, safe, jarSize, iconSize);
    return n > 0 && n < cap;
}

unsigned char* Ps2Storage::readTile(const char* jarName, int jarSize, int iconSize) {
    if (!cacheOk_ || jarName == 0) {
        return 0;
    }
    char path[240];
    if (!cachePath(path, (int)sizeof(path), jarName, jarSize, iconSize)) {
        return 0;
    }
    const int bytes = iconSize * iconSize * 4;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return 0;                                   // cache miss
    }
    unsigned char* tile = (unsigned char*)malloc(bytes);
    int off = 0;
    if (tile != 0) {
        while (off < bytes) {
            const int r = (int)::read(fd, tile + off, bytes - off);
            if (r <= 0) break;
            off += r;
        }
    }
    ::close(fd);
    if (tile == 0 || off != bytes) {                // truncated/corrupt -> miss
        free(tile);
        return 0;
    }
    return tile;
}

void Ps2Storage::writeTile(const char* jarName, int jarSize, int iconSize,
                           const unsigned char* tile) {
    if (!cacheOk_ || jarName == 0 || tile == 0) {
        return;
    }
    char path[240];
    if (!cachePath(path, (int)sizeof(path), jarName, jarSize, iconSize)) {
        return;
    }
    const int bytes = iconSize * iconSize * 4;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    int off = 0;
    while (off < bytes) {
        const int w = (int)::write(fd, tile + off, bytes - off);
        if (w <= 0) break;
        off += w;
    }
    ::close(fd);
}

} // namespace platform
} // namespace ps2
