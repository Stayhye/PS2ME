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

    /// Load the wavetable sound bank (produced offline by tools/sf2bank) from @p path
    /// into a single block OUTSIDE the Java heap pool, once at boot. Idempotent. The
    /// block is never freed (it lives for the whole run, so it can't fragment). On any
    /// failure -- missing file, bad magic, wrong layout, out of memory -- it leaves the
    /// bank unloaded and returns false; the synth then stays on the square-wave path.
    /// Only validates and pins the section pointers here (Phase-4 Stage 1); the mixer
    /// starts using the samples in Stage 2. Logs one [bank] line via javacall_print.
    bool loadBank(const char* path);

    /// Whether a bank is loaded and available to the synth.
    bool bankLoaded() const { return bank_ != 0; }

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

    /// Parse an MMAPI tone sequence (audio/x-tone-seq, ToneControl format) into a note
    /// sequence and play it. Returns a voice id (>0) or -1 if it isn't a valid sequence.
    int submitToneSeq(const unsigned char* data, int len, int vol, int loop);

    /// Parse a Standard MIDI File (audio/midi, "MThd") and play it on the internal
    /// polyphonic square-wave synth (up to POLY simultaneous notes). Percussion (channel
    /// 10) is skipped in this cut. Returns a voice id (>0) or -1 if it isn't valid MIDI.
    int submitMidi(const unsigned char* data, int len, int vol, int loop);

    /// Stop the voice with this id (no-op if it already ended / never existed).
    void stop(int id);

    /// Set the volume (0..255) of the voice with this id (VolumeControl). No-op if it
    /// already ended. Takes effect on the next mixed block.
    void setVolume(int id, int vol);

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

    // MIDI synth: a bank of square-wave oscillators driven by a time-sorted note event
    // list, so the one active voice plays a whole polyphonic Standard MIDI File.
    static const int POLY            = 24;     // simultaneous MIDI notes
    static const int MAX_MIDI_EVENTS = 32768;  // note-on/off events per song (else truncated)
    static const int MIDI_ATTACK     = 110;    // ~5 ms attack ramp (avoids note-on click)
    static const int MIDI_RELEASE    = 660;    // ~30 ms release ramp (avoids note-off click)

    enum SourceKind { KIND_SEQ = 0, KIND_PCM = 1, KIND_MIDI = 2 };

    // One scheduled MIDI note event. During decode it carries the absolute tick + a stable
    // sequence number (sort keys) and, for tempo meta events, the microseconds-per-quarter.
    // After the tempo pass, note events carry an absolute sample time (atSample).
    enum MidiEvType { MEV_OFF = 0, MEV_ON = 1, MEV_TEMPO = 2, MEV_PROG = 3 };
    struct MidiEv {
        int          atTick;    // absolute tick (decode sort key)
        int          atSample;  // absolute sample time (filled by the tempo pass)
        unsigned int tempo;     // microseconds per quarter note (MEV_TEMPO only)
        int          seq;       // insertion order (stable secondary sort key)
        unsigned char type;     // MEV_OFF / MEV_ON / MEV_TEMPO
        unsigned char note;     // MIDI note 0..127
        unsigned char vel;      // note-on velocity
        unsigned char chan;     // MIDI channel 0..15 (9 = percussion, skipped)
    };

    // One playing MIDI oscillator. Two synthesis paths share this slot:
    //   - square (wave == false): a square wave (melodic) or noise/low-tone burst
    //     (percussion), shaped by a linear envelope. The fallback when no bank is loaded.
    //   - wavetable (wave == true): a bank sample resampled by a Q16 step with a loop,
    //     shaped by an ADSR (melodic) or a declick (one-shot / percussion). Phase 4.
    struct Osc {
        bool          active;
        unsigned char note;
        unsigned char env;      // square: 0 = attack, 1 = sustain, 2 = release/decay
        unsigned char drum;     // square: 0 = melodic square, 1 = noise, 2 = kick
        unsigned int  phase;    // square: Q16 phase accumulator
        unsigned int  phaseInc;
        unsigned int  lfsr;     // square: noise generator state (percussion)
        int           peak;     // square: target amplitude (from velocity)
        int           cur;      // square: current envelope amplitude
        int           envPos;   // square: samples into the current envelope stage
        int           relStart; // square: amplitude when the release/decay began
        int           relLen;   // square: release/decay length in samples

        // --- wavetable path (Phase 4 Stage 2) ------------------------------------
        bool          wave;     // true = wavetable voice (uses the fields below)
        unsigned char chan;     // source MIDI channel (to match note-off)
        bool          loop;     // sample loops (melodic sustain)
        bool          oneshot;  // plays once and frees itself (drum / unlooped)
        unsigned int  sOff;     // sample start in the bank PCM pool (samples)
        unsigned int  sLen;     // sample length (samples)
        unsigned int  sLs;      // loop start (samples)
        unsigned int  sLe;      // loop end (samples)
        unsigned long long wpos; // Q16 position within the sample
        unsigned int  wstep;    // Q16 resample step (freq ratio)
        float         wgain;    // amplitude scale (velocity * attenuation)
        // envelope: melodic ADSR (estage 0=att,1=hold,2=decay,3=sustain,4=release,
        // 5=done) or one-shot declick (fade in, play, fade out at sample end)
        unsigned char estage;
        int           aLen, hLen, dLen, rLen;  // stage lengths in samples
        float         susLvl;   // sustain level (0..1)
        float         envCur;   // current envelope value (0..1)
        float         relFrom;  // envelope value when release began
        int           stagePos; // samples into the current envelope stage
    };

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

    // Decode an MMAPI tone sequence (ToneControl audio/x-tone-seq) into out[] (up to
    // maxNotes), expanding REPEAT and PLAY_BLOCK. Returns the note count (0 if invalid).
    // *gateOut gets the sounded % (notes sustain full, like the MIDI reference).
    static int decodeToneSeq(const unsigned char* data, int len, Note* out, int maxNotes,
                             int* gateOut);

    // Parse a Standard MIDI File into a time-sorted note event list in out[] (up to
    // maxEvents), with absolute sample times (tempo map applied). Drops tempo/meta and
    // percussion. Returns the note-event count (0 if invalid); *songSamplesOut gets the
    // total length in samples (including a short release tail).
    static int decodeMidi(const unsigned char* data, int len, MidiEv* out, int maxEvents,
                          int* songSamplesOut);

    // qsort comparator: order events by absolute tick, then insertion order (stable).
    static int midiEvCmp(const void* a, const void* b);

    // Render one sample of the MIDI voice: dispatch any events due at the current sample,
    // advance the oscillators + envelopes, and return the mixed sample. *ended is set true
    // once the song has fully played out. Called per sample from the mixer for KIND_MIDI.
    int  midiRenderSample(bool* ended);
    void midiAllocOsc(int note, int vel, int chan);  // start a melodic note (bank or square)
    void midiAllocDrum(int note, int vel);  // trigger a percussion hit (channel 10)
    void midiReleaseOsc(int note, int chan);// begin the release of a held melodic note
    void midiReset();                       // rewind to the song start, silence all osc

    // One decoded bank zone (a sample + how to play it). Filled from the on-disk
    // ZONE_BYTES record by readZone(); mirrors tools/sf2bank's Zone.
    struct BankZone {
        unsigned int off, length, ls, le;   // sample region in the PCM pool (samples)
        int          cents;                 // fine tune (added to note-root)
        int          root, klo, khi;        // root key, key range
        int          pan, atten;            // pan (-500..500), attenuation (centibels)
        int          a, h, d, r, susp;      // ADSR: attack/hold/decay/release ms, sustain permille
        int          mode;                  // 0 = no loop (one-shot), 1/3 = loop
        int          fc, fq;                // low-pass cutoff (Hz, 0=open) + resonance (cB)
    };
    int  midiPickSlot();                            // free osc slot, else steal quietest
    // Sample index j clamped/loop-wrapped for oscillator o (for cubic interpolation).
    static int wrapIdx(const Osc& o, int j);
    void readZone(int idx, BankZone& z) const;      // decode zone #idx from zoneTab_
    int  bankMelodicZone(int prog, int note) const; // zone index for (prog,note), -1 none
    int  bankDrumZone(int note) const;              // zone index for a drum note, -1 none
    // Fill oscillator @p o to play bank zone @p z for (note, vel) on @p chan.
    void startWaveOsc(Osc& o, const BankZone& z, int note, int vel, int chan);

    // Commit a filled voice live under the table lock; returns its new id.
    int activateVoice(int v, int vol, int loop);

    // --- Phase 4: wavetable bank (loaded once at boot, outside the Java pool) --------
    // On-disk layout (little-endian, EE is LE so it maps directly): a 24-byte header
    // ("PS2B", ver, flags, rate, nProg=128, nDrum=128, nZones, nPcm), then ProgramDir
    // [128] and DrumDir[128] (4 bytes each: u16 zoneStart, u8 count, u8 pad), then
    // Zone[nZones] (ZONE_BYTES each), then the PCM16 sample pool (nPcm samples).
    static const int ZONE_BYTES = 38;   // must match tools/sf2bank ZONE_FMT
    unsigned char* bank_;               // whole blob (memalign, never freed); 0 = none
    int   bankSize_;
    int   bankRate_;
    int   bankZones_;
    const unsigned char* progDir_;      // 128 * 4 bytes
    const unsigned char* drumDir_;      // 128 * 4 bytes
    const unsigned char* zoneTab_;      // bankZones_ * ZONE_BYTES
    const short*         bankPcm_;      // sample pool

    bool  ready_;
    bool  mixerStarted_;
    int   mixerId_;
    void* mixerStack_;

    Voice voices_[VOICES];
    int   voiceSema_;               // binary sema guarding the voice table (not SifLock)
    int   nextId_;
    static short storage_[VOICES][VOICE_SAMPLES];   // PCM buffers (WAV)
    static Note  noteStore_[VOICES][MAX_NOTES];     // note sequences (tone/OTA)

    // MIDI synth state (single song at a time, since the mixer is last-wins). The event
    // list is static; the playback cursors + oscillator bank live here and are driven by
    // the mixer thread only (submitMidi fills the list while the voice is offline).
    static MidiEv midiEvents_[MAX_MIDI_EVENTS];
    int   midiCount_;               // number of note events in the current song
    int   midiEventIdx_;            // next event to dispatch
    int   midiCurSample_;           // playback position in samples
    int   midiSongSamples_;         // total song length in samples
    Osc   midiOsc_[POLY];
    int   chanProg_[16];            // current GM program per channel (set by MEV_PROG)
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2AUDIO_HPP
