// PS2 JavaCall port — platform layer.
//
// SifLock: the one lock that serializes ALL SIF RPC use across EE threads. SIF is not
// reentrant -- two SIF transactions issued from different threads at once collide and
// hang or fail. Every place that talks to the IOP must hold this lock while it does:
//   - controller reads (padman)          -- Ps2Pad
//   - host:/mass: file I/O (fileXio)      -- Ps2HostStorage / Ps2Storage
//   - EE-console prints (SIF tty)         -- StdoutSink
//   - audio streaming (audsrv)            -- Ps2Audio mixer thread
//   - icon-tile loading                   -- IconCache worker
//
// The lock is RECURSIVE (owner thread id + depth): the same thread may re-acquire it,
// so a high-level SIF sequence that already holds the lock can call a lower-level helper
// that also guards itself without deadlocking. Cross-thread it is a plain mutex.
//
// Created once by init() at the very top of main(), before any SIF user or worker thread
// exists; until then acquire()/release() are no-ops (there is only one thread at boot).
#ifndef PS2_JAVACALL_PLATFORM_SIFLOCK_HPP
#define PS2_JAVACALL_PLATFORM_SIFLOCK_HPP

namespace ps2 {
namespace platform {

namespace SifLock {
    void init();       // create the lock once, before any SIF user/thread
    void acquire();    // recursive: re-acquiring on the owning thread just bumps depth
    void release();
}

// Scoped acquire/release for a SIF critical section.
struct SifGuard {
    SifGuard()  { SifLock::acquire(); }
    ~SifGuard() { SifLock::release(); }
private:
    SifGuard(const SifGuard&);
    SifGuard& operator=(const SifGuard&);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_SIFLOCK_HPP
