// PS2 JavaCall port — platform layer.
//
// Ps2HostStorage: reads user game archives (.jar) from `host:games/` via the POSIX
// file API (open/read/lseek/close, opendir/readdir). The ps2sdk newlib port routes
// these through its default fileio backend, and host: is served by the emulator's
// HostFS (PCSX2: enable "Host Filesystem" + set a root path; games go in
// <root>/games/) or by ps2link on real hardware -- over the default IOP ioman, so
// NO extra IOP module is loaded and the EE Console survives (no SifIopReset, same
// policy as Ps2Pad). ps2sdk forbids calling fio*/fileXio* directly in the newlib
// port, hence POSIX here.
//
// Backend for hal::GameStorage. Later devices (USB mass:, memory card mc0:) plug in
// as sibling backends behind the same HAL (fileXio handles those prefixes).
#include "../hal/GameStorage.hpp"
#include "Ps2Storage.hpp"
#include "SifLock.hpp"

#include <sifrpc.h>
#include <fcntl.h>      // open, O_RDONLY
#include <unistd.h>     // read, lseek, close
#include <dirent.h>     // opendir, readdir, closedir
#include <string.h>

namespace ps2 {
namespace platform {

namespace {
const int   MAX_GAMES = 2048;           // large libraries / load testing
const int   NAME_CAP  = 64;             // basename cap (fits a typical <suite>.jar)

bool endsWithJar(const char* s) {
    const int n = (int)strlen(s);
    return n >= 4 && s[n - 4] == '.' &&
        (s[n - 3] == 'j' || s[n - 3] == 'J') &&
        (s[n - 2] == 'a' || s[n - 2] == 'A') &&
        (s[n - 1] == 'r' || s[n - 1] == 'R');
}
} // namespace

class Ps2HostStorage : public hal::IGameStorage {
public:
    Ps2HostStorage() : ready_(false), count_(0), fd_(-1) {}

    virtual int list() {
        SifGuard guard;   // opendir/readdir/closedir go to the IOP over SIF (fileXio)
        ensureReady();
        count_ = 0;
        DIR* dir = ::opendir(Ps2Storage::instance().gamesDir());
        if (dir == 0) {
            return -1;
        }
        struct dirent* de;
        while (count_ < MAX_GAMES && (de = ::readdir(dir)) != 0) {
            if (endsWithJar(de->d_name)) {
                int k = 0;
                while (de->d_name[k] != '\0' && k < NAME_CAP - 1) {
                    names_[count_][k] = de->d_name[k];
                    k++;
                }
                names_[count_][k] = '\0';
                count_++;
            }
        }
        ::closedir(dir);
        return count_;
    }

    virtual const char* nameAt(int i) {
        return (i >= 0 && i < count_) ? names_[i] : 0;
    }

    virtual int openAt(int i) {
        if (i < 0 || i >= count_) {
            return -1;
        }
        SifGuard guard;   // open/lseek go to the IOP over SIF (recursive: close() too)
        close();
        char path[128];
        int p = 0;
        for (const char* d = Ps2Storage::instance().gamesDir(); *d != '\0'; ++d) {
            path[p++] = *d;
        }
        path[p++] = '/';
        for (const char* nm = names_[i]; *nm != '\0' && p < (int)sizeof(path) - 1; ++nm) {
            path[p++] = *nm;
        }
        path[p] = '\0';

        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) {
            return -1;
        }
        const int size = (int)::lseek(fd_, 0, SEEK_END);
        ::lseek(fd_, 0, SEEK_SET);
        return size;
    }

    virtual int read(void* buf, int max) {
        if (fd_ < 0) {
            return -1;
        }
        SifGuard guard;   // ::read goes to the IOP over SIF (fileXio)
        return (int)::read(fd_, buf, max);
    }

    virtual int readAt(unsigned int off, void* buf, int len) {
        if (fd_ < 0) {
            return -1;
        }
        SifGuard guard;   // lseek + read go to the IOP over SIF (recursive guard)
        if ((int)::lseek(fd_, (int)off, SEEK_SET) < 0) {
            return -1;
        }
        return (int)::read(fd_, buf, len);
    }

    virtual void close() {
        SifGuard guard;   // ::close goes to the IOP over SIF (fileXio)
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    void ensureReady() {
        if (!ready_) {
            SifInitRpc(0);   // idempotent; Ps2Pad may also bring RPC up
            ready_ = true;
        }
    }

    bool ready_;
    int  count_;
    int  fd_;
    char names_[MAX_GAMES][NAME_CAP];
};

namespace {
// Register the host: backend at startup (mirrors the StdoutSink / Ps2Pad registrars).
Ps2HostStorage g_hostStorage;

struct Ps2HostStorageRegistrar {
    Ps2HostStorageRegistrar() {
        hal::GameStorage::instance().setBackend(&g_hostStorage);
    }
};
Ps2HostStorageRegistrar g_ps2HostStorageRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
