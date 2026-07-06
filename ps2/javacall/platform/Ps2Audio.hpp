// PS2 JavaCall port — platform layer.
//
// Ps2Audio: the console's audio bring-up and PCM output. Sound lives in the SPU2 on the
// IOP; the EE reaches it over SIF through the audsrv server. init() loads the IOP sound
// modules (libsd.irx + audsrv.irx) via ps2_drivers -- exactly mirroring the USB/fileXio
// bring-up in Ps2Storage -- and starts the EE audio server at a fixed PCM format.
//
// This is the Phase-1 foundation: enough to load the drivers and stream PCM, with a
// boot self-test beep to prove the whole SPU2/audsrv chain works on the target before
// the Java sound API is wired to it. Concurrent, non-blocking game audio (a mixer thread
// feeding several sounds, SIF serialized against the icon worker / prints) comes next.
//
// audsrv is SIF RPC. init()/beep() run once at boot in a single-threaded context (after
// Ps2Storage::mount, before the menu/icon worker), so they need no SIF lock; the future
// mixer thread will share IconCache::sifLock like the rest of the SIF users.
#ifndef PS2_JAVACALL_PLATFORM_PS2AUDIO_HPP
#define PS2_JAVACALL_PLATFORM_PS2AUDIO_HPP

namespace ps2 {
namespace platform {

class Ps2Audio {
public:
    static Ps2Audio& instance();

    /// Load the IOP sound modules (libsd + audsrv via ps2_drivers) and start the EE
    /// audio server at the fixed playback format. Idempotent. Call once at boot AFTER
    /// Ps2Storage::mount (audio IRX must load after the USB-boot IOP reset). Returns true
    /// on success; on failure the port simply stays silent. Logs each step via
    /// javacall_print (the storage-style boot trace).
    bool init();

    bool ready() const { return ready_; }

    /// Fixed output format: sample rate in Hz, 16-bit signed, mono.
    int sampleRate() const { return SAMPLE_RATE; }

    /// Queue a 16-bit signed mono PCM buffer, blocking until it is fully handed to
    /// audsrv. Intended for the boot self-test and short one-shots from a single caller;
    /// the Phase-2 mixer thread will replace this for concurrent game audio. Caller owns
    /// @p pcm.
    void playPcmMonoBlocking(const short* pcm, int samples);

    /// Synthesize and play a square-wave tone (freq Hz, duration ms) -- the boot beep.
    void beep(int freq, int ms);

private:
    Ps2Audio();
    Ps2Audio(const Ps2Audio&);
    Ps2Audio& operator=(const Ps2Audio&);

    static const int SAMPLE_RATE = 22050;

    bool ready_;
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2AUDIO_HPP
