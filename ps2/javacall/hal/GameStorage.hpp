// PS2 JavaCall port — HAL layer.
//
// GameStorage: the singleton facade the KNI bridge (com.j2meps2.loader.GameLoader)
// talks to. A platform backend registers itself via setBackend() at startup
// (mirrors hal::Keypad::setPad). Delegates every call to the active backend, so the
// device (host:/mass:/mc0:) is swappable without touching the bridge or the MIDlet.
#ifndef PS2_JAVACALL_HAL_GAMESTORAGE_HPP
#define PS2_JAVACALL_HAL_GAMESTORAGE_HPP

#include "IGameStorage.hpp"

namespace ps2 {
namespace hal {

class GameStorage {
public:
    static GameStorage& instance();

    /// Install the storage backend. Ownership stays with the caller.
    void setBackend(IGameStorage* backend) { backend_ = backend; }

    int         list()              { return backend_ ? backend_->list()      : -1; }
    const char* nameAt(int i)       { return backend_ ? backend_->nameAt(i)   : 0;  }
    int         openAt(int i)       { return backend_ ? backend_->openAt(i)   : -1; }
    int         read(void* b, int m){ return backend_ ? backend_->read(b, m)  : -1; }
    int         readAt(unsigned int off, void* b, int m)
                                    { return backend_ ? backend_->readAt(off, b, m) : -1; }
    void        close()             { if (backend_) backend_->close(); }

private:
    GameStorage() : backend_(0) {}
    GameStorage(const GameStorage&);
    GameStorage& operator=(const GameStorage&);

    IGameStorage* backend_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_GAMESTORAGE_HPP
