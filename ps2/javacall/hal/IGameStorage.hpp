// PS2 JavaCall port — HAL layer.
//
// IGameStorage: the contract a storage device implements so the runtime can
// enumerate and stream user game archives (.jar) from it. Designed for a future
// front-end: list() enumerates a games directory, and a chosen entry is streamed
// in chunks (one "current" open file at a time keeps the KNI surface small).
//
// Platform backends implement this over a device: host: (PCSX2 HostFS / ps2link),
// and later USB mass: and memory card mc0:. All sit behind hal::GameStorage.
#ifndef PS2_JAVACALL_HAL_IGAMESTORAGE_HPP
#define PS2_JAVACALL_HAL_IGAMESTORAGE_HPP

namespace ps2 {
namespace hal {

class IGameStorage {
public:
    virtual ~IGameStorage() {}

    /// Enumerate the .jar files in the games directory. Returns the count (>= 0),
    /// or -1 on error. Caches the listing so nameAt()/openAt() can index it.
    virtual int list() = 0;

    /// Basename (e.g. "game.jar") of entry i from the last list(), or 0 if out of
    /// range. The pointer stays valid until the next list().
    virtual const char* nameAt(int i) = 0;

    /// Open entry i for streaming. Returns its size in bytes, or -1 on error.
    virtual int openAt(int i) = 0;

    /// Read up to max bytes of the open entry into buf. Returns the number read
    /// (0 at end of file), or -1 on error.
    virtual int read(void* buf, int max) = 0;

    /// Close the current open entry (no-op if none is open).
    virtual void close() = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IGAMESTORAGE_HPP
