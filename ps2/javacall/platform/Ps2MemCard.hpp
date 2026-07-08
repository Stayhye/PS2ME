// PS2 JavaCall port — platform layer.
//
// Ps2MemCard: a dedicated service for reading and writing files on the PlayStation 2
// Memory Card. It exists so the launcher's own state (favourites, recents, settings,
// per-game resolutions) can live on the memory card instead of the USB stick -- which
// means the program can boot from CDVD (a read-only disc) and still remember the user's
// choices. The memory card is the one writable, always-present store on a stock console.
//
// The service is intentionally generic: on top of the raw card it exposes a small
// directory + whole-file API (ensureDir / readFile / writeFile / removeFile addressed by
// absolute card paths like "/PS2ME/saves/foo.dat"). The launcher config lives under the
// app directory "/PS2ME"; the same primitives will back per-game save data later, so this
// stays a pure memory-card service with no launcher-specific policy baked in.
//
// The one exception is the configRead/configWrite convenience pair, which encapsulates a
// single policy for the launcher's settings files: prefer the memory card, and fall back
// to the boot-directory disk file (via Ps2Storage) when no card is present -- so USB users
// keep working and no-card hardware degrades gracefully instead of silently losing state.
//
// Access model. Underneath, all I/O is libmc RPC (mcOpen/mcRead/mcWrite/... + mcSync),
// which is SIF traffic; every public method serializes on the shared SifLock, so it is
// safe to call from the menu thread while the icon worker is also using SIF. init() runs
// once at boot (single-threaded) after Ps2Storage::mount() has brought SIF up.
#ifndef PS2_JAVACALL_PLATFORM_PS2MEMCARD_HPP
#define PS2_JAVACALL_PLATFORM_PS2MEMCARD_HPP

namespace ps2 {
namespace platform {

class Ps2MemCard {
public:
    static Ps2MemCard& instance();

    /// Bring the memory card up once: load the mcman/mcserv IRX (ps2_drivers), init libmc,
    /// probe ports 0 and 1 for a usable (formatted PS2) card, and create the app directory
    /// "/PS2ME". Safe to call when no card is present (leaves available() false). Must run
    /// at boot, after Ps2Storage::mount() (SIF up) and before the icon worker starts, so it
    /// needs no external locking of its own for the probe.
    void init();

    /// Whether a usable (formatted PS2) memory card was found. When false, the config
    /// convenience methods transparently fall back to the boot-directory disk file.
    bool available() const { return available_; }

    /// One-line human-readable state ("MC slot 1: ready", "no card", "unformatted",
    /// "driver error N"). Shown in the launcher's diagnostics.
    const char* statusText() const { return status_; }

    // --- Launcher config policy: memory card preferred, boot-dir disk fallback ----------

    /// Read a launcher config file (@p name, e.g. "favorites.txt") into @p buf (capacity
    /// @p cap). From "/PS2ME/<name>" on the card, or "<bootdir><name>" on disk when there
    /// is no card. Returns the number of bytes read, or <= 0 when the file is absent.
    int  configRead(const char* name, void* buf, int cap);

    /// Replace a launcher config file (@p name) with @p len bytes from @p data. Writes to
    /// the card ("/PS2ME/<name>") when present, else the boot-dir disk file. Returns true
    /// on success.
    bool configWrite(const char* name, const void* data, int len);

    // --- Generic memory-card file API (absolute card paths; future game saves) ----------

    /// Create directory @p absDir (e.g. "/PS2ME") on the card. Succeeds if it already
    /// exists. No-op / false when no card is present.
    bool ensureDir(const char* absDir);

    /// Read the whole file at @p absPath into @p buf (capacity @p cap). Returns bytes read,
    /// or -1 on any error / missing file / no card.
    int  readFile(const char* absPath, void* buf, int cap);

    /// Replace the file at @p absPath with @p len bytes from @p data (delete + create, as
    /// the card has no O_TRUNC). Returns true on success; false on error / no card.
    bool writeFile(const char* absPath, const void* data, int len);

    /// Delete @p absPath from the card. Returns true if the file no longer exists after.
    bool removeFile(const char* absPath);

private:
    Ps2MemCard();
    Ps2MemCard(const Ps2MemCard&);
    Ps2MemCard& operator=(const Ps2MemCard&);

    // Block on the current async libmc operation and return its result code (the value
    // mcSync reports through its result pointer: fd / byte count / status). Caller holds
    // the SifLock.
    int syncResult();

    // Disk fallback for the config methods (used only when available_ is false): the
    // boot-directory file via Ps2Storage. Caller holds the SifLock.
    int  diskRead(const char* name, void* buf, int cap);
    bool diskWrite(const char* name, const void* data, int len);

    bool inited_;               // init() has run
    bool available_;            // a usable formatted PS2 card was found
    int  port_;                 // card port that answered (0 = slot 1, 1 = slot 2)
    int  slot_;                 // always 0 (no multitap)
    char status_[64];           // human-readable state for diagnostics
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2MEMCARD_HPP
