// PS2 JavaCall port — platform layer.
//
// IconCache: a background game-icon loader with a bounded, persistent cache. The menu
// must scroll freely, but extracting an icon means reading a JAR and decoding a PNG --
// far too slow to do on the render thread. So a low-priority worker thread decodes the
// icons the menu asks for (only the ones on screen), and the render thread just blits
// whatever is already cached and draws a placeholder for the rest. Decoded tiles are
// kept (evicting the least-recently-seen when full) so the work is never repeated.
//
// Threading model: the worker runs at a lower priority than the render thread, so it
// only gets the CPU while the render thread is blocked waiting for vsync. During a menu
// session the render thread is the only one that touches the GS and the raster; the
// worker is the only one that allocates (JAR buffers, decoded tiles) -- the render
// thread never allocates in its loop -- so the heap has a single writer at a time.
// Before the menu hands the CPU to the VM, the worker is quiesced (queue cleared, any
// in-flight decode finished) so it stays blocked and never runs alongside the VM.
#ifndef PS2_JAVACALL_PLATFORM_ICONCACHE_HPP
#define PS2_JAVACALL_PLATFORM_ICONCACHE_HPP

namespace ps2 {
namespace platform {

class IconCache {
public:
    static IconCache& instance();

    /// Prepare for a menu session: start the worker (once) and drop stale pending
    /// requests. The decoded-tile cache persists across sessions. @p iconSize is the
    /// square tile size the icons are downscaled to.
    void begin(int count, int iconSize);

    /// Advance the frame clock; call once per rendered frame (drives LRU eviction).
    void tick();

    /// If game's icon is cached, alpha-blend it into the RGBA5551 @p raster at (x,y)
    /// and return true. Otherwise queue it for the worker (once) and return false, so
    /// the caller draws a placeholder this frame.
    bool draw(int game, unsigned short* raster, int rw, int rh, int x, int y);

    /// Drop queued (not-yet-started) requests. Pair with busy() to quiesce the worker
    /// before handing the CPU to the VM.
    void clearPending();

    /// Ask the worker to back off briefly (called by the render thread on each D-pad
    /// navigation). While backed off the worker starts no new decode, so its SIF I/O
    /// never contends with the pad reads -- navigation stays smooth during loading.
    /// The backoff decays on its own once navigation stops.
    void pause();

    /// Whether a decode is currently in flight on the worker.
    bool busy() const { return working_; }

    /// True (once) if the worker installed a new tile since the last call. Lets the
    /// render thread stay asleep -- yielding the CPU to the worker -- while the screen
    /// is unchanged, and wake to redraw only when a freshly decoded icon is ready.
    bool takeDirty() { bool d = dirty_; dirty_ = false; return d; }

    /// Serialize SIF RPC use. host: file I/O (worker) and the EE console
    /// (javacall_print, any thread) both talk to the IOP over SIF, which is not
    /// reentrant -- two concurrent transactions from different threads deadlock.
    /// Everything that touches SIF must hold this. No-op until the worker is armed.
    static void sifLock();
    static void sifUnlock();

private:
    IconCache();
    IconCache(const IconCache&);
    IconCache& operator=(const IconCache&);

    void           ensureWorker();
    void           run();                 // worker loop (never returns)
    unsigned char* loadTile(int game);    // JAR -> icon -> downscaled tile (worker)
    void           installLocked(int game, unsigned char* tile);
    static void    trampoline(void* arg);

    static const int MAX_GAMES = 2048;    // matches Ps2HostStorage's cap
    static const int CACHE_MAX = 128;     // decoded tiles kept resident
    static const int QUEUE_MAX = 256;     // pending requests ring

    enum State { ST_NONE = 0, ST_REQUESTED, ST_LOADED, ST_NOICON };

    struct Slot { int game; unsigned char* tile; unsigned seen; };

    int  count_;
    int  iconSize_;
    bool started_;
    volatile bool working_;               // worker is mid-decode
    volatile bool dirty_;                 // worker installed a tile since last takeDirty
    volatile int  pauseTicks_;            // >0: worker backs off (decremented by worker)

    int   workerId_;
    int   mutex_;                          // guards the cache + queue below
    int   workSema_;                       // wakes the worker when requests arrive
    void* stack_;

    unsigned frame_;
    signed char state_[MAX_GAMES];
    int         gameToSlot_[MAX_GAMES];
    Slot        slots_[CACHE_MAX];
    int         slotCount_;

    int queue_[QUEUE_MAX];
    int qHead_;
    int qTail_;
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_ICONCACHE_HPP
