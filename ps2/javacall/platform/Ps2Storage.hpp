// PS2 JavaCall port — platform layer.
//
// Ps2Storage: physical storage bring-up and the on-disk icon-tile cache. One place
// owns loading the ps2_drivers USB/fileXio IRX (so the program can be launched and run
// from a USB stick), deciding where games are read from, and caching decoded icon
// tiles so they aren't re-extracted from the JAR every boot.
//
// Everything is anchored to the ELF's own launch directory (argv[0], which the PS2
// hands the program): games live in <bootdir>/games and the cache in <bootdir>/cache.
// So a stick holding PS2ME/j2me-midp.elf + PS2ME/games/*.jar just works, and the cache
// lands beside it. The cache is only enabled when that location is actually writable
// (USB / host: yes, cdvd: ROM no), probed at mount time -- otherwise icons decode from
// the JAR as before and everything degrades gracefully.
//
// All I/O here touches SIF (host:/mass: file I/O is SIF RPC); callers on the icon
// worker thread must hold IconCache::sifLock while calling readTile/writeTile.
#ifndef PS2_JAVACALL_PLATFORM_PS2STORAGE_HPP
#define PS2_JAVACALL_PLATFORM_PS2STORAGE_HPP

namespace ps2 {
namespace platform {

class Ps2Storage {
public:
    static Ps2Storage& instance();

    /// Bring up the storage drivers once (SBV patches + fileXio + USB mass storage)
    /// and resolve the launch directory from @p argv0 (the ELF path the loader passed
    /// to main; may be null). Probes the cache location for writability. Runs at boot,
    /// before the icon worker exists, so it needs no external locking.
    void mount(const char* argv0);

    /// The directory games are read from: "<bootdir>/games" when that folder exists,
    /// otherwise "host:games" (dev fallback under PCSX2). Decided once, then cached.
    const char* gamesDir();

    /// Path of the wavetable sound bank: "<bootdir>/bank.bin" (beside games/), or
    /// "host:bank.bin" when the boot dir wasn't resolved (dev fallback under PCSX2).
    /// Writes into @p out (capacity @p cap); returns false if it would not fit.
    bool bankPath(char* out, int cap);

    /// Path of the favourites list file: "<bootdir>favorites.txt" (beside games/), or
    /// "host:favorites.txt" when the boot dir wasn't resolved (dev fallback under
    /// PCSX2). Writes into @p out (capacity @p cap); returns false if it would not fit.
    bool favoritesPath(char* out, int cap);

    /// Whether the on-disk tile cache is available (its location is writable).
    bool cacheEnabled() const { return cacheOk_; }

    /// Multi-line record of what mount() found (argv0, device, each probe, result).
    /// Shown on-screen when no games are found -- the EE console is invisible on real
    /// hardware booted standalone, so this is how a HW test surfaces the diagnosis.
    const char* diagText() const { return diag_; }

    /// Cache-first read: return a freshly malloc'd @p iconSize x @p iconSize RGBA8
    /// tile for this game if one is cached, else 0. @p jarSize keys the entry so a
    /// replaced JAR (different size) misses and is re-decoded. Caller frees.
    unsigned char* readTile(const char* jarName, int jarSize, int iconSize);

    /// Store a decoded tile for next time. No-op if the cache is disabled.
    void writeTile(const char* jarName, int jarSize, int iconSize,
                   const unsigned char* tile);

private:
    Ps2Storage();
    Ps2Storage(const Ps2Storage&);
    Ps2Storage& operator=(const Ps2Storage&);

    // Split the ELF path in @p argv0 into a device prefix ("mass0:") and a directory
    // suffix ("/PS2ME/"), stored in device_ / subDir_.
    void splitBootPath(const char* argv0);
    // Probe base directory @p base (ends with '/'): open <base> and <base>games, log
    // the result, and on the first games hit record baseDir_/gamesDir_. Returns "found".
    bool tryBase(const char* base, bool already);
    // Build the cache file path for one game into @p out (capacity @p cap). Returns
    // false if it would not fit. Path = <bootdir>cache/<sanitized>_<jar>_<size>.tl
    bool cachePath(char* out, int cap, const char* jarName, int jarSize, int iconSize);

    // Append one printf-style line to diag_ and also emit it via javacall_print.
    void note(const char* fmt, ...);

    bool mounted_;
    bool cacheOk_;              // the cache location is writable
    bool gamesResolved_;        // gamesDir() has run
    bool baseIsMass_;           // booted from USB mass storage

    char device_[32];           // device prefix incl. colon, e.g. "mass0:"
    char subDir_[128];          // directory suffix incl. separators, e.g. "/PS2ME/"
    char baseDir_[192];         // resolved "<prefix><subDir>", e.g. "mass:/PS2ME/"
    char cacheDir_[208];        // "<baseDir>cache"
    char gamesDir_[208];        // "<baseDir>games" (or "host:games")

    char diag_[1024];           // human-readable mount() trace (shown on empty screen)
    int  diagLen_;
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2STORAGE_HPP
