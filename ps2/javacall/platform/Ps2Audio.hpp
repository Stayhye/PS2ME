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
    /// Boot-only (single-threaded, before startMixer): does not take the SIF lock.
    void beep(int freq, int ms);

    /// Start the background mixer thread. It keeps the audsrv ring fed so audio never
    /// underruns, mixing the active voices (Phase 2b) -- pure silence when none play.
    /// Every ring feed goes through the shared SifLock, so it serializes against the
    /// controller reads / file I/O / prints on the non-reentrant SIF bus. Call once,
    /// after init() and after the boot chime (so the chime doesn't race the thread).
    /// Also wires the pre-allocated voice buffers and the voice-table lock.
    void startMixer();

    // --- Phase 2b: the Nokia Sound API submits game audio through these -------------
    //
    // A voice is one of two source kinds, both fed from pre-allocated static storage so
    // NO malloc/free ever runs on the mixer thread (the VM allocates continuously on the
    // main thread and newlib's malloc lock is effectively a no-op here, so a mixer-thread
    // free would race and corrupt the heap):
    //   - SEQ: a note sequence (tone / OTA melody). The voice holds a note list and the
    //          mixer synthesizes each note (square wave) on the fly -- unbounded length,
    //          a few KB of state. Tone and OTA both land here.
    //   - PCM: a decoded 22050/16/mono buffer (WAV). The mixer just reads samples.
    // Submitters (called from KNI on the main thread) fill a voice while it is offline,
    // then flip it live under voiceSema_; the mixer only ever reads. audsrv is never
    // touched here -- only the mixer talks to it -- so these add no SIF traffic.

    /// Play a single square-wave tone. loop: 0 = forever, N = play N times. vol 0..255.
    /// Returns a voice id (>0) or -1. (A tone is just a 1-note SEQ voice.)
    int submitTone(int freq, int durMs, int vol, int loop);

    /// Parse a Nokia OTA ringing-tone (Smart Messaging) melody and play it as a note
    /// sequence. Returns a voice id (>0) or -1 if it isn't valid OTA / has no notes.
    int submitOta(const unsigned char* data, int len, int vol, int loop);

    /// Parse a PCM WAV (8/16-bit, mono/stereo, any rate), resample to 22050/16/mono into
    /// a voice, and start it. Longer than a voice buffer is truncated. Returns id or -1.
    int submitWav(const unsigned char* data, int len, int vol, int loop);

    /// Stop the voice with this id (no-op if it already ended / never existed).
    void stop(int id);

    /// Number of concurrent voices the mixer supports (drives getConcurrentSoundCount).
    int voiceCount() const { return VOICES; }

private:
    Ps2Audio();
    Ps2Audio(const Ps2Audio&);
    Ps2Audio& operator=(const Ps2Audio&);

    static const int SAMPLE_RATE   = 22050;
    static const int VOICES        = 1;      // MVP: single voice, last-wins. Bump to mix N.
    static const int VOICE_SAMPLES = 65536;  // 128 KB/voice @ 16-bit ~= 2.97 s (WAV/PCM)
    static const int MAX_NOTES     = 1024;   // note-sequence capacity per voice (tone/OTA)
    static const int BLOCK         = 1024;   // samples fed to audsrv per mixer tick (~46 ms)

    enum SourceKind { KIND_SEQ = 0, KIND_PCM = 1 };

    // One note of a sequence. phaseInc == 0 means a rest (silence). samples is the
    // note's total length; the last (100 - gatePercent)% is silenced (staccato/rest).
    // phaseInc is a Q16 phase step (freq * 2^16 / SAMPLE_RATE): a fractional-phase
    // accumulator gives the exact frequency, unlike an integer half-period which
    // quantizes the pitch badly at 22050 Hz (e.g. 660 Hz -> 689 Hz, ~0.75 semitone off).
    struct Note {
        unsigned int phaseInc;   // freq * 2^16 / SAMPLE_RATE (Q16); 0 = rest
        int          samples;    // duration in samples
    };

    // One playing sound. SEQ uses notes[]/note*; PCM uses buf/samples/pos. loopsLeft
    // counts finite repeats; infinite loops until stop(). All storage is static.
    struct Voice {
        int   kind;        // KIND_SEQ or KIND_PCM
        int   vol;         // 0..255
        int   id;
        bool  infinite;
        int   loopsLeft;
        bool  active;
        // PCM
        short* buf;
        int    samples;
        int    pos;
        // SEQ
        Note*  notes;
        int    noteCount;
        int    noteIdx;
        int    notePos;
        unsigned int phase; // Q16 phase accumulator for the current note's square wave
        int    gatePercent; // % of each note that actually sounds (rest is the remainder)
    };

    static void mixerTrampoline(void* arg);
    void mixerRun();   // never returns

    // Decode a Nokia OTA ringing-tone (Smart Messaging) into out[] (up to maxNotes).
    // Returns the note count (0 if not decodable). *gateOut gets the style's sounded %.
    static int decodeOta(const unsigned char* data, int len, Note* out, int maxNotes,
                         int* gateOut);

    // Commit a filled voice live under the table lock; returns its new id.
    int activateVoice(int v, int vol, int loop);

    bool  ready_;
    bool  mixerStarted_;
    int   mixerId_;
    void* mixerStack_;

    Voice voices_[VOICES];
    int   voiceSema_;               // binary sema guarding the voice table (not SifLock)
    int   nextId_;
    static short storage_[VOICES][VOICE_SAMPLES];   // PCM buffers (WAV)
    static Note  noteStore_[VOICES][MAX_NOTES];     // note sequences (tone/OTA)
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2AUDIO_HPP
