// PS2 JavaCall port — platform layer. Ps2Audio implementation.
#include "Ps2Audio.hpp"
#include "SifLock.hpp"

#include <audsrv.h>              // audsrv_init/set_format/set_volume/play_audio/wait_audio
#include <ps2_audio_driver.h>    // init_audio_driver (loads libsd.irx + audsrv.irx)
#include <javacall_logging.h>    // javacall_print (boot sign-of-life)

extern "C" {
#include <kernel.h>       // threads + semaphores
#include <delaythread.h>  // DelayThread (EE microsecond sleep, no SIF)
}
#include <malloc.h>   // memalign
#include <stdlib.h>   // malloc / free
#include <stdio.h>    // snprintf
#include <string.h>   // (silence buffer is BSS-zeroed; no memset needed)

// Linker-provided global pointer; EE threads must be created with it.
extern "C" void* _gp;

namespace ps2 {
namespace platform {

namespace {
const int MIXER_STACK = 16 * 1024;   // mixer stack (tiny: just feeds the ring)

// Little-endian readers for the WAV header (bytes come straight off the Java array).
inline unsigned int le16(const unsigned char* p) {
    return (unsigned int)(p[0] | (p[1] << 8));
}
inline unsigned int le32(const unsigned char* p) {
    return (unsigned int)(p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24));
}

// Fetch source sample k as signed 16-bit mono (average stereo, sign-extend 8-bit).
inline int wavSample(const unsigned char* pcm, int k, int channels, int bits) {
    if (bits == 16) {
        const unsigned char* f = pcm + k * channels * 2;
        int l = (short)(f[0] | (f[1] << 8));
        if (channels == 2) {
            int r = (short)(f[2] | (f[3] << 8));
            return (l + r) >> 1;
        }
        return l;
    }
    // 8-bit unsigned PCM -> signed 16-bit.
    const unsigned char* f = pcm + k * channels;
    int l = ((int)f[0] - 128) << 8;
    if (channels == 2) {
        int r = ((int)f[1] - 128) << 8;
        return (l + r) >> 1;
    }
    return l;
}

// Square-wave amplitude for synthesized tones (full scale is 32767).
const short AMP = 6000;

// --- Nokia OTA ringing-tone (Smart Messaging 2.0.0, sec 3.8) tables ----------------
// Note-value 1..12 -> frequency (Hz) at scale-1 (A = 440). Index 0 = pause.
const int NOTE_HZ[13] = { 0, 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494 };
// Beats-per-minute by 5-bit index (Table 3.8-11).
const int OTA_BPM[32] = {
    25,  28,  31,  35,  40,  45,  50,  56,  63,  70,  80,  90, 100, 112, 125, 140,
   160, 180, 200, 225, 250, 285, 320, 355, 400, 450, 500, 565, 635, 715, 800, 900 };

// MSB-first bit reader over the OTA byte stream. Reads past the end yield zero bits.
struct BitReader {
    const unsigned char* b;
    int len;    // bytes
    int pos;    // bit position
    BitReader(const unsigned char* d, int l) : b(d), len(l), pos(0) {}
    int read(int n) {
        int v = 0;
        while (n-- > 0) {
            int bit = 0;
            if (pos < len * 8) {
                bit = (b[pos >> 3] >> (7 - (pos & 7))) & 1;
            }
            ++pos;
            v = (v << 1) | bit;
        }
        return v;
    }
    void align() { pos = (pos + 7) & ~7; }   // advance to the next octet boundary
};
} // namespace

// Static voice PCM storage (BSS): the mixer only ever reads these; no heap involved.
short Ps2Audio::storage_[Ps2Audio::VOICES][Ps2Audio::VOICE_SAMPLES];
Ps2Audio::Note Ps2Audio::noteStore_[Ps2Audio::VOICES][Ps2Audio::MAX_NOTES];

Ps2Audio& Ps2Audio::instance() {
    static Ps2Audio inst;
    return inst;
}

Ps2Audio::Ps2Audio()
    : ready_(false), mixerStarted_(false), mixerId_(0), mixerStack_(0),
      voiceSema_(0), nextId_(0) {}

bool Ps2Audio::init() {
    if (ready_) {
        return true;
    }

    char msg[96];

    // Loads libsd.irx + audsrv.irx onto the IOP and runs audsrv_init() on the EE. Negative
    // AUDIO_INIT_STATUS_* means an IRX or the EE server failed to come up (see the enum).
    int rc = (int)init_audio_driver();
    snprintf(msg, sizeof msg, "[audio] init_audio_driver = %d\n", rc);
    javacall_print(msg);
    if (rc < 0) {
        javacall_print("[audio] driver load FAILED -> silent\n");
        return false;
    }

    // audsrv is up (init_audio_driver already called audsrv_init); set our playback format.
    audsrv_fmt_t fmt;
    fmt.freq = SAMPLE_RATE;
    fmt.bits = 16;
    fmt.channels = 1;
    int fr = audsrv_set_format(&fmt);
    snprintf(msg, sizeof msg, "[audio] set_format(%d/16/mono) = %d\n", SAMPLE_RATE, fr);
    javacall_print(msg);
    if (fr < 0) {
        javacall_print("[audio] set_format FAILED -> silent\n");
        return false;
    }

    audsrv_set_volume(MAX_VOLUME);
    ready_ = true;
    javacall_print("[audio] ready\n");
    return true;
}

void Ps2Audio::playPcmMonoBlocking(const short* pcm, int samples) {
    if (!ready_ || pcm == 0 || samples <= 0) {
        return;
    }
    const char* buf = (const char*)pcm;
    const int total = samples * 2;   // 16-bit samples
    // audsrv's IOP ring is tiny (feed_size*10; ~4700 B at 22050/16/mono) and starts with
    // only feed_size*5 (~2350 B) free. audsrv_wait_audio(n) blocks until n bytes are free,
    // but the SPU2 only starts draining once the FIRST audsrv_play_audio runs -- so the
    // very first wait must ask for less than that initial free space or it blocks forever
    // (the black-screen hang). Keep the chunk well under it; wait-then-play from there on
    // drains fine as playback advances readpos.
    const int CHUNK = 1024;
    int off = 0;
    while (off < total) {
        int want = total - off;
        if (want > CHUNK) {
            want = CHUNK;
        }
        audsrv_wait_audio(want);                       // room guaranteed >= want
        int done = audsrv_play_audio(buf + off, want);
        if (done <= 0) {                               // guard against a stalled server
            break;
        }
        off += done;
    }
}

void Ps2Audio::beep(int freq, int ms) {
    if (!ready_ || ms <= 0) {
        return;
    }
    if (freq < 20) {
        freq = 20;
    }
    int samples = (int)((long)SAMPLE_RATE * ms / 1000);
    if (samples <= 0) {
        return;
    }
    short* buf = (short*)malloc((size_t)samples * sizeof(short));
    if (buf == 0) {
        return;
    }

    // Square wave: cheap and unambiguously audible for a self-test (no libm needed).
    int halfPeriod = SAMPLE_RATE / (freq * 2);
    if (halfPeriod < 1) {
        halfPeriod = 1;
    }
    const short AMP = 6000;   // moderate level (full scale is 32767)
    for (int i = 0; i < samples; ++i) {
        buf[i] = ((i / halfPeriod) & 1) ? AMP : (short)-AMP;
    }

    // ~5 ms linear fades on each end to avoid the click of a hard start/stop.
    int fade = SAMPLE_RATE / 200;
    if (fade > samples / 2) {
        fade = samples / 2;
    }
    for (int i = 0; i < fade; ++i) {
        buf[i]               = (short)(buf[i] * i / fade);
        buf[samples - 1 - i] = (short)(buf[samples - 1 - i] * i / fade);
    }

    playPcmMonoBlocking(buf, samples);
    free(buf);
}

void Ps2Audio::mixerTrampoline(void* arg) {
    static_cast<Ps2Audio*>(arg)->mixerRun();
}

void Ps2Audio::mixerRun() {
    // Keep the audsrv ring topped up. We mix the active voices into a block, then hand it
    // to audsrv_play_audio() -- which is non-blocking (queues only what currently fits and
    // returns), so we hold the SIF lock for just that quick RPC, never across a blocking
    // wait, then sleep to pace the next feed. The ring is ~106 ms; feeding ~2 KB every
    // 20 ms keeps it comfortably full without starving the controller reads that share
    // the lock. The voice table is read under voiceSema_ (independent of SifLock, never
    // nested) so the whole per-block mix is a consistent snapshot vs submit()/stop().
    static short outBlock[BLOCK];   // single mixer thread -> static is safe
    const int blockBytes = BLOCK * 2;
    int sent = blockBytes;          // == blockBytes means "mix a fresh block"

    for (;;) {
        // Mix a NEW block only once the previous one has been fully queued. audsrv_play_
        // audio is non-blocking: it accepts only what currently fits in the IOP ring and
        // returns that count. If we mixed every tick and ignored the count, we'd advance
        // the voices faster than the ring drains and the extra samples would be dropped ->
        // playback ~2x too fast and distorted. Pacing to what the ring accepts makes the
        // voices advance at exactly the real 22050 Hz consumption rate.
        if (sent >= blockBytes) {
            WaitSema(voiceSema_);
            for (int i = 0; i < BLOCK; ++i) {
                int acc = 0;
                for (int v = 0; v < VOICES; ++v) {
                    Voice& vc = voices_[v];
                    if (!vc.active) {
                        continue;
                    }
                    int s;
                    bool ended = false;
                    if (vc.kind == KIND_PCM) {
                        s = vc.buf[vc.pos];
                        ended = (++vc.pos >= vc.samples);
                        if (ended) {
                            vc.pos = 0;
                        }
                    } else {   // KIND_SEQ: synthesize the current note's square wave
                        Note& nt = vc.notes[vc.noteIdx];
                        int gate = (nt.samples * vc.gatePercent) / 100;   // sounded portion
                        if (nt.phaseInc == 0 || vc.notePos >= gate) {
                            s = 0;                                        // rest / inter-note gap
                        } else {
                            // Q16 phase accumulator -> exact frequency (no pitch quantization).
                            s = ((vc.phase >> 15) & 1) ? AMP : (short)-AMP;
                        }
                        vc.phase += nt.phaseInc;
                        if (++vc.notePos >= nt.samples) {
                            vc.notePos = 0;
                            vc.phase   = 0;                               // restart phase per note
                            if (++vc.noteIdx >= vc.noteCount) {
                                vc.noteIdx = 0;
                                ended = true;                             // sequence wrapped
                            }
                        }
                    }
                    acc += (s * vc.vol) >> 8;

                    if (ended) {                    // one full pass done (PCM or sequence)
                        if (vc.infinite) {
                            /* keep playing from the top (pos/noteIdx already reset) */
                        } else if (vc.loopsLeft > 1) {
                            vc.loopsLeft--;
                        } else {
                            vc.active = false;      // finished (no free: static storage)
                        }
                    }
                }
                if (acc > 32767) {
                    acc = 32767;
                } else if (acc < -32768) {
                    acc = -32768;
                }
                outBlock[i] = (short)acc;           // silence when no voice is active
            }
            SignalSema(voiceSema_);
            sent = 0;
        }

        // Queue as much of the current block as the ring will take right now; keep the
        // remainder for the next tick (never dropped). SifLock held only for the quick RPC.
        {
            SifGuard guard;
            int n = audsrv_play_audio((const char*)outBlock + sent, blockBytes - sent);
            if (n > 0) {
                sent += n;
            }
        }
        DelayThread(10000);   // 10 ms: comfortably faster than the ring drains
    }
}

int Ps2Audio::activateVoice(int v, int vol, int loop) {
    if (vol < 0) {
        vol = 0;
    } else if (vol > 255) {
        vol = 255;
    }
    WaitSema(voiceSema_);
    int id = ++nextId_;
    if (id <= 0) {                 // wrap guard: ids stay positive (0/-1 mean "none")
        nextId_ = 1;
        id = 1;
    }
    Voice& vc = voices_[v];
    vc.vol       = vol;
    vc.infinite  = (loop == 0);
    vc.loopsLeft = (loop <= 0) ? 1 : loop;
    vc.id        = id;
    vc.pos       = 0;          // PCM cursor
    vc.noteIdx   = 0;          // SEQ cursors
    vc.notePos   = 0;
    vc.phase     = 0;
    vc.active    = true;       // kind / buf / notes were set by the caller while offline
    SignalSema(voiceSema_);
    return id;
}

int Ps2Audio::submitTone(int freq, int durMs, int vol, int loop) {
    if (!mixerStarted_ || durMs <= 0) {
        return -1;
    }
    if (freq < 20) {
        freq = 20;
    }
    int samples = (int)((long)SAMPLE_RATE * durMs / 1000);
    if (samples <= 0) {
        return -1;
    }

    // A single tone is just a 1-note sequence voice (no PCM buffer, any duration).
    const int v = 0;   // MVP: single voice, last-wins
    WaitSema(voiceSema_);
    voices_[v].active = false;   // offline before we rewrite its note list
    SignalSema(voiceSema_);

    Note* n = noteStore_[v];
    n[0].phaseInc = (unsigned int)((unsigned long long)freq * 65536ULL / (unsigned)SAMPLE_RATE);
    n[0].samples  = samples;
    voices_[v].kind        = KIND_SEQ;
    voices_[v].notes       = noteStore_[v];
    voices_[v].noteCount   = 1;
    voices_[v].gatePercent = 100;   // single sustained tone: no internal gap
    return activateVoice(v, vol, loop);
}

int Ps2Audio::decodeOta(const unsigned char* data, int len, Note* out, int maxNotes,
                        int* gateOut) {
    BitReader br(data, len);
    int commandLen = br.read(8);       // number of command parts
    if (commandLen <= 0) {
        return 0;                       // command-end / empty
    }

    int scale  = 2;    // default scale-2 (A = 880 Hz) per Table 3.8-9
    int bpmIdx = 8;    // default 63 BPM per Table 3.8-11
    int gate   = 90;   // natural style (rest between notes) is the default
    int count  = 0;

    // Note-duration fraction of a quarter note: full=4q, 1/2=2q, 1/4=1q, 1/8..1/32.
    static const int FRAC_NUM[6] = { 4, 2, 1, 1, 1, 1 };
    static const int FRAC_DEN[6] = { 1, 1, 1, 2, 4, 8 };

    for (int part = 0; part < commandLen; ++part) {
        br.align();                    // each command-part is octet-aligned
        int cid = br.read(7);
        if (cid == 0x25) {             // ringing-tone-programming: marker, no body
            continue;
        }
        if (cid != 0x1D) {             // unicode / cancel / unknown: not handled
            break;
        }
        // <sound>: song-type then the song.
        int songType = br.read(3);
        if (songType == 0x1) {         // basic-song: <song-title> then temporary-song
            int titleLen = br.read(4);
            br.read(titleLen * 8);     // skip the title (default 8-bit charset)
        } else if (songType != 0x2) {  // 0x2 = temporary-song (no title); others unsupported
            break;
        }
        int seqLen = br.read(8);       // number of song patterns
        for (int pat = 0; pat < seqLen; ++pat) {
            br.read(3);                // pattern-header-id (000)
            br.read(2);                // pattern-id
            br.read(4);                // loop-value (we loop at the voice level)
            int spec = br.read(8);     // pattern-specifier = instruction count
            if (spec == 0) {
                continue;              // already-defined pattern (reuse): skip
            }
            for (int ins = 0; ins < spec; ++ins) {
                int iid = br.read(3);
                if (iid == 0x1) {              // note-instruction
                    int nv  = br.read(4);      // note value (0 = pause, 1..12 = C..B)
                    int nd  = br.read(3);      // note duration
                    int nds = br.read(2);      // duration specifier
                    int bpm = OTA_BPM[bpmIdx & 31];
                    int quarterMs = 60000 / bpm;
                    int durMs = (nd < 6) ? (quarterMs * FRAC_NUM[nd] / FRAC_DEN[nd])
                                         : quarterMs;
                    if (nds == 1) {
                        durMs = durMs * 3 / 2;         // dotted
                    } else if (nds == 2) {
                        durMs = durMs * 7 / 4;         // double dotted
                    } else if (nds == 3) {
                        durMs = durMs * 2 / 3;         // 2/3 length
                    }
                    int samples = SAMPLE_RATE * durMs / 1000;
                    if (samples < 1) {
                        samples = 1;
                    }
                    if (count < maxNotes) {
                        if (nv >= 1 && nv <= 12) {
                            int freq = NOTE_HZ[nv] << (scale - 1);
                            out[count].phaseInc = (unsigned int)((unsigned long long)freq
                                                     * 65536ULL / (unsigned)SAMPLE_RATE);
                        } else {
                            out[count].phaseInc = 0;    // pause / reserved -> rest
                        }
                        out[count].samples = samples;
                        ++count;
                    }
                } else if (iid == 0x2) {      // scale-instruction
                    scale = br.read(2) + 1;
                } else if (iid == 0x3) {      // style-instruction
                    int st = br.read(2);
                    gate = (st == 1) ? 100 : (st == 2 ? 55 : 90);  // cont / staccato / natural
                } else if (iid == 0x4) {      // tempo-instruction
                    bpmIdx = br.read(5);
                } else if (iid == 0x5) {      // volume-instruction
                    br.read(4);
                } else {                      // pattern-header-id / unknown: stop pattern
                    break;
                }
            }
        }
    }

    *gateOut = gate;
    return count;
}

int Ps2Audio::submitOta(const unsigned char* data, int len, int vol, int loop) {
    if (!mixerStarted_ || data == 0 || len < 3) {
        return -1;
    }

    const int v = 0;
    // Offline before touching this voice's note list (decode writes into it).
    WaitSema(voiceSema_);
    voices_[v].active = false;
    SignalSema(voiceSema_);

    int gate = 90;
    int count = decodeOta(data, len, noteStore_[v], MAX_NOTES, &gate);
    if (count <= 0) {
        return -1;                  // not valid OTA / no notes -> stay silent
    }
    voices_[v].kind        = KIND_SEQ;
    voices_[v].notes       = noteStore_[v];
    voices_[v].noteCount   = count;
    voices_[v].gatePercent = gate;
    return activateVoice(v, vol, loop);
}

int Ps2Audio::submitWav(const unsigned char* data, int len, int vol, int loop) {
    // Minimum viable header: RIFF(4) size(4) WAVE(4) + a fmt chunk + a data chunk.
    if (!mixerStarted_ || data == 0 || len < 44) {
        return -1;
    }
    if (!(data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
          data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E')) {
        return -1;
    }

    // Walk the chunks for "fmt " (format) and "data" (samples). Chunks are word-aligned.
    unsigned int fmt = 0, channels = 0, bits = 0, srcRate = 0;
    const unsigned char* pcm = 0;
    int pcmLen = 0;
    int p = 12;
    while (p + 8 <= len) {
        const unsigned char* cid = data + p;
        unsigned int csz = le32(data + p + 4);
        const unsigned char* body = data + p + 8;
        int avail = len - (p + 8);
        if ((int)csz > avail) {
            csz = (unsigned int)avail;   // clamp a bogus/oversized length to the buffer
        }
        if (cid[0] == 'f' && cid[1] == 'm' && cid[2] == 't' && cid[3] == ' ' && csz >= 16) {
            fmt      = le16(body);
            channels = le16(body + 2);
            srcRate  = le32(body + 4);
            bits     = le16(body + 14);
        } else if (cid[0] == 'd' && cid[1] == 'a' && cid[2] == 't' && cid[3] == 'a') {
            pcm    = body;
            pcmLen = (int)csz;
        }
        long np = (long)p + 8 + (long)csz + (csz & 1);   // pad odd sizes to even
        if (np <= p) {
            break;
        }
        p = (int)np;
    }

    if (fmt != 1 || pcm == 0 || pcmLen <= 0 || srcRate == 0 ||
        !(bits == 8 || bits == 16) || !(channels == 1 || channels == 2)) {
        return -1;   // not plain PCM we can handle -> stay silent
    }

    int frameBytes = (int)(bits / 8) * (int)channels;
    int srcSamples = pcmLen / frameBytes;
    if (srcSamples <= 0) {
        return -1;
    }

    const int v = 0;
    WaitSema(voiceSema_);
    voices_[v].active = false;
    SignalSema(voiceSema_);

    // Linear resample srcRate -> 22050 into the voice buffer (Q16 fixed-point position).
    short* buf = storage_[v];
    unsigned long long posQ16  = 0;
    unsigned long long stepQ16 = ((unsigned long long)srcRate << 16) / (unsigned)SAMPLE_RATE;
    int out = 0;
    while (out < VOICE_SAMPLES) {
        int k = (int)(posQ16 >> 16);
        if (k >= srcSamples) {
            break;
        }
        unsigned int frac = (unsigned int)(posQ16 & 0xFFFF);
        int a = wavSample(pcm, k, (int)channels, (int)bits);
        int b = (k + 1 < srcSamples) ? wavSample(pcm, k + 1, (int)channels, (int)bits) : a;
        buf[out++] = (short)(a + (((b - a) * (int)frac) >> 16));
        posQ16 += stepQ16;
    }
    if (out <= 0) {
        return -1;
    }
    voices_[v].kind    = KIND_PCM;
    voices_[v].buf     = storage_[v];
    voices_[v].samples = out;
    return activateVoice(v, vol, loop);
}

void Ps2Audio::stop(int id) {
    if (!mixerStarted_ || id <= 0) {
        return;
    }
    WaitSema(voiceSema_);
    for (int v = 0; v < VOICES; ++v) {
        if (voices_[v].active && voices_[v].id == id) {
            voices_[v].active = false;
        }
    }
    SignalSema(voiceSema_);
}

void Ps2Audio::startMixer() {
    if (!ready_ || mixerStarted_) {
        return;
    }
    mixerStack_ = memalign(16, MIXER_STACK);
    if (mixerStack_ == 0) {
        return;
    }

    // One-time: the voice-table lock + wire each voice to its static buffer (all idle).
    // Done before the thread starts, so the mixer never sees a half-set-up table.
    ee_sema_t vs;
    vs.init_count = 1;
    vs.max_count  = 1;
    vs.attr       = 0;
    vs.option     = 0;
    voiceSema_ = CreateSema(&vs);
    for (int v = 0; v < VOICES; ++v) {
        voices_[v].kind   = KIND_SEQ;
        voices_[v].buf    = storage_[v];
        voices_[v].notes  = noteStore_[v];
        voices_[v].active = false;
        voices_[v].id     = 0;
    }

    // Audio is real-time: the audsrv ring (~106 ms) underruns -> choppy/silent audio if the
    // feeder can't run in time. The VM runs on this (main) thread, which the EE starts at
    // priority 0 -- the highest -- so nothing can preempt it and a background feeder is
    // starved during gameplay. There is no priority above 0, so we DEMOTE the VM to a mid
    // priority and run the mixer ABOVE it: the mixer then preempts the VM briefly every
    // 20 ms to top up the ring, then yields via DelayThread. The icon worker is created
    // later at (VM prio + 8), so it naturally sits below both. Long file reads release the
    // SifLock between chunks, so the mixer is never blocked on the lock past the ring depth.
    const int VM_PRIO    = 16;
    const int MIXER_PRIO = 8;
    ChangeThreadPriority(GetThreadId(), VM_PRIO);   // demote the VM/main thread from 0
    int prio = MIXER_PRIO;

    ee_thread_t t;
    t.func             = (void*)&Ps2Audio::mixerTrampoline;
    t.stack            = mixerStack_;
    t.stack_size       = MIXER_STACK;
    t.gp_reg           = &_gp;
    t.initial_priority = prio;
    t.current_priority = prio;
    t.attr             = 0;
    t.option           = 0;
    mixerId_ = CreateThread(&t);
    if (mixerId_ >= 0) {
        StartThread(mixerId_, this);
        mixerStarted_ = true;
        javacall_print("[audio] mixer thread started\n");
    }
}

} // namespace platform
} // namespace ps2
