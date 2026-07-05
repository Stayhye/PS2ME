// PS2 JavaCall port — platform layer. IconCache implementation.
//
// A low-priority worker thread decodes requested icons off the render path; the render
// thread blits cached tiles and requests the rest. See IconCache.hpp for the model.
#include "IconCache.hpp"

#include "../hal/GameStorage.hpp"
#include "MidletIcon.hpp"
#include "Ps2Storage.hpp"

extern "C" {
#include <kernel.h>     // threads + semaphores
}
#include <malloc.h>     // memalign / malloc / free

// Linker-provided global pointer; EE threads must be created with it.
extern "C" void* _gp;

namespace ps2 {
namespace platform {

namespace {

const int STACK_SIZE = 64 * 1024;   // worker stack (JAR read + PNG decode)

// Binary semaphore serializing all SIF RPC use (see IconCache::sifLock). -1 until
// the worker is armed; while it's -1 there is only one thread, so lock is a no-op.
int g_sifSema = -1;

// GS-native RGBA5551 (R low, G, B, alpha top) -- same layout as the raster.
inline unsigned short rgba5551(int r, int g, int b) {
    return (unsigned short)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
                            (((b >> 3) & 0x1F) << 10) | 0x8000);
}
inline unsigned short blend5551(unsigned short dst, int sr, int sg, int sb, int a) {
    const int dr = ( dst        & 0x1F) << 3;
    const int dg = ((dst >> 5)  & 0x1F) << 3;
    const int db = ((dst >> 10) & 0x1F) << 3;
    const int r = dr + ((sr - dr) * a) / 255;
    const int g = dg + ((sg - dg) * a) / 255;
    const int b = db + ((sb - db) * a) / 255;
    return rgba5551(r, g, b);
}

int makeSema(int init, int maxc) {
    ee_sema_t s;
    s.count        = init;
    s.max_count    = maxc;
    s.init_count   = init;
    s.wait_threads = 0;
    s.attr         = 0;
    s.option       = 0;
    return CreateSema(&s);
}

} // namespace

IconCache& IconCache::instance() {
    static IconCache inst;
    return inst;
}

IconCache::IconCache()
    : count_(0), iconSize_(0), started_(false), working_(false), dirty_(false),
      workerId_(0), mutex_(0), workSema_(0), stack_(0),
      frame_(0), slotCount_(0), qHead_(0), qTail_(0) {}

void IconCache::sifLock() {
    if (g_sifSema >= 0) {
        WaitSema(g_sifSema);
    }
}
void IconCache::sifUnlock() {
    if (g_sifSema >= 0) {
        SignalSema(g_sifSema);
    }
}

void IconCache::ensureWorker() {
    mutex_    = makeSema(1, 1);
    workSema_ = makeSema(0, 1);
    g_sifSema = makeSema(1, 1);   // arm the SIF lock before the worker can run
    stack_    = memalign(16, STACK_SIZE);

    // Run below the render thread so the worker only gets the CPU while the render
    // thread is blocked on vsync (higher priority number == lower priority).
    ee_thread_status_t ts;
    ReferThreadStatus(GetThreadId(), &ts);
    int prio = ts.current_priority + 8;
    if (prio > 126) {
        prio = 126;
    }

    ee_thread_t t;
    t.func             = (void*)&IconCache::trampoline;
    t.stack            = stack_;
    t.stack_size       = STACK_SIZE;
    t.gp_reg           = &_gp;
    t.initial_priority = prio;
    t.current_priority = prio;
    t.attr             = 0;
    t.option           = 0;
    workerId_ = CreateThread(&t);
    StartThread(workerId_, this);
}

void IconCache::begin(int count, int iconSize) {
    count_    = count;
    iconSize_ = iconSize;
    if (!started_) {
        for (int i = 0; i < MAX_GAMES; ++i) {
            state_[i]      = ST_NONE;
            gameToSlot_[i] = -1;
        }
        slotCount_ = 0;
        frame_     = 0;
        qHead_     = qTail_ = 0;
        working_   = false;
        ensureWorker();
        started_ = true;
    } else {
        // New session: drop stale pending requests, keep the decoded-tile cache.
        WaitSema(mutex_);
        qHead_ = qTail_ = 0;
        for (int i = 0; i < MAX_GAMES; ++i) {
            if (state_[i] == ST_REQUESTED) {
                state_[i] = ST_NONE;
            }
        }
        SignalSema(mutex_);
    }
}

void IconCache::tick() {
    ++frame_;
}

void IconCache::clearPending() {
    if (!started_) {
        return;
    }
    WaitSema(mutex_);
    qHead_ = qTail_ = 0;
    SignalSema(mutex_);
}

void IconCache::trampoline(void* arg) {
    static_cast<IconCache*>(arg)->run();
}

void IconCache::run() {
    for (;;) {
        WaitSema(workSema_);            // sleep until the render thread posts work
        for (;;) {
            WaitSema(mutex_);
            int g = -1;
            if (qHead_ != qTail_) {
                g = queue_[qHead_];
                qHead_ = (qHead_ + 1) % QUEUE_MAX;
                working_ = true;        // set under the lock so quiesce sees it
            }
            SignalSema(mutex_);
            if (g < 0) {
                break;                  // queue drained
            }

            unsigned char* tile = loadTile(g);   // slow work, outside the lock

            WaitSema(mutex_);
            if (tile != 0) {
                installLocked(g, tile);
                state_[g] = ST_LOADED;
            } else {
                state_[g] = ST_NOICON;
            }
            working_ = false;
            SignalSema(mutex_);
        }
    }
}

unsigned char* IconCache::loadTile(int game) {
    hal::GameStorage& st = hal::GameStorage::instance();
    Ps2Storage&       disk = Ps2Storage::instance();
    const char* name = st.nameAt(game);
    const int   n    = iconSize_;

    // All of this touches SIF (host:/mass: I/O): hold the SIF lock so it never runs
    // concurrently with another thread's SIF (EE console, pad). Open the JAR only to
    // learn its size (open + lseek, no content read yet), then try the on-disk cache
    // first -- a hit skips both reading and decoding the JAR entirely.
    sifLock();
    const int size = st.openAt(game);
    unsigned char* cached = (size > 0) ? disk.readTile(name, size, n) : 0;
    unsigned char* jar = 0;
    int off = 0;
    if (cached == 0 && size > 0) {              // cache miss: read the whole JAR
        jar = (unsigned char*)malloc(size);
        if (jar != 0) {
            while (off < size) {
                const int r = st.read(jar + off, size - off);
                if (r <= 0) break;
                off += r;
            }
        }
    }
    st.close();
    sifUnlock();

    if (cached != 0) {
        return cached;                          // cache hit -- nothing to decode
    }
    if (size <= 0 || jar == 0) {
        return 0;
    }

    int w = 0, h = 0;
    unsigned char* rgba = (off == size) ? MidletIcon::load(jar, size, &w, &h) : 0;
    free(jar);
    if (rgba == 0 || w <= 0 || h <= 0) {
        return 0;
    }

    unsigned char* tile = (unsigned char*)malloc(n * n * 4);
    if (tile != 0) {
        for (int dy = 0; dy < n; ++dy) {
            const int sy = dy * h / n;
            for (int dx = 0; dx < n; ++dx) {
                const int sx = dx * w / n;
                const unsigned char* s = rgba + (sy * w + sx) * 4;
                unsigned char* d = tile + (dy * n + dx) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            }
        }
        // Persist the decoded tile so the next boot is a cache hit (SIF: under lock).
        sifLock();
        disk.writeTile(name, size, n, tile);
        sifUnlock();
    }
    MidletIcon::release(rgba);
    return tile;
}

void IconCache::installLocked(int game, unsigned char* tile) {
    int slot;
    if (slotCount_ < CACHE_MAX) {
        slot = slotCount_++;
    } else {
        // Evict the least-recently-seen tile (never a currently-visible one).
        slot = 0;
        unsigned oldest = slots_[0].seen;
        for (int i = 1; i < CACHE_MAX; ++i) {
            if (slots_[i].seen < oldest) {
                oldest = slots_[i].seen;
                slot = i;
            }
        }
        const int victim = slots_[slot].game;
        if (victim >= 0) {
            state_[victim]      = ST_NONE;
            gameToSlot_[victim] = -1;
        }
        free(slots_[slot].tile);
    }
    slots_[slot].game = game;
    slots_[slot].tile = tile;
    slots_[slot].seen = frame_;
    gameToSlot_[game] = slot;
    dirty_ = true;   // wake the render thread to show the newly decoded tile
}

bool IconCache::draw(int game, unsigned short* raster, int rw, int rh, int x, int y) {
    if (!started_ || game < 0 || game >= count_ || game >= MAX_GAMES) {
        return false;
    }
    bool drawn  = false;
    bool signal = false;

    WaitSema(mutex_);
    const int s = state_[game];
    if (s == ST_LOADED) {
        const int slot = gameToSlot_[game];
        if (slot >= 0 && slots_[slot].tile != 0) {
            slots_[slot].seen = frame_;
            const int n = iconSize_;
            const unsigned char* t = slots_[slot].tile;
            for (int dy = 0; dy < n; ++dy) {
                const int py = y + dy;
                if (py < 0 || py >= rh) continue;
                unsigned short* row = raster + py * rw;
                const unsigned char* src = t + dy * n * 4;
                for (int dx = 0; dx < n; ++dx) {
                    const int px = x + dx;
                    if (px < 0 || px >= rw) continue;
                    const int a = src[dx * 4 + 3];
                    if (a) {
                        row[px] = blend5551(row[px], src[dx * 4 + 0],
                                            src[dx * 4 + 1], src[dx * 4 + 2], a);
                    }
                }
            }
            drawn = true;
        }
    } else if (s == ST_NONE) {
        const int nt = (qTail_ + 1) % QUEUE_MAX;
        if (nt != qHead_) {              // enqueue once (skip silently if full)
            queue_[qTail_] = game;
            qTail_ = nt;
            state_[game] = ST_REQUESTED;
            signal = true;
        }
    }
    SignalSema(mutex_);

    if (signal) {
        SignalSema(workSema_);
    }
    return drawn;
}

} // namespace platform
} // namespace ps2
