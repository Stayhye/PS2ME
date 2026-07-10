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
#include <fcntl.h>    // open, O_RDONLY (bank load)
#include <unistd.h>   // read, close, lseek (bank load)
#include <math.h>     // pow (wavetable step / attenuation gain)

// Linker-provided global pointer; EE threads must be created with it.
extern "C" void* _gp;

namespace ps2 {
namespace platform {

namespace {
const int MIXER_STACK = 16 * 1024;   // mixer stack (tiny: just feeds the ring)

// --- Menu BGM (SPU2-direct ADPCM voice) -------------------------------------------
// State for the single looping background-music voice. Touched only from the main/menu
// thread (loadBgm at boot, start/stopBgm from the launcher) -- never the mixer thread --
// so it needs no lock. Kept here (file scope) so the audsrv.h type stays out of the header.
audsrv_adpcm_t g_bgm;
bool           g_bgmLoaded = false;
int            g_bgmCh     = -1;              // SPU2 voice channel (-1 = not playing yet)
const int      BGM_VOL     = 0x3000;         // ~75% of MAX_VOLUME (0x3fff): ambient level

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

// Master gain for the wavetable MIDI mix. Each voice is a full-scale int16 sample, and
// the offline render measured a summed mono peak around 1.4x full-scale on dense songs;
// the real-time mixer can't peak-normalize, so scale down for headroom against clipping.
const float WAVE_MASTER = 0.55f;

// Brightness: HL4MGM's per-voice low-pass filters are dark (cutoffs 0.8-1.7 kHz), which
// sounded too muffled faithfully applied. Shift every cutoff up by this factor so the
// filter still tames the harsh top but keeps presence (the "filt30" preview).
const float WAVE_FC_SCALE = 3.0f;

// --- Nokia OTA ringing-tone (Smart Messaging 2.0.0, sec 3.8) tables ----------------
// Note-value 1..12 -> frequency (Hz) at scale-1 (A = 440). Index 0 = pause.
const int NOTE_HZ[13] = { 0, 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494 };
// Beats-per-minute by 5-bit index (Table 3.8-11).
const int OTA_BPM[32] = {
    25,  28,  31,  35,  40,  45,  50,  56,  63,  70,  80,  90, 100, 112, 125, 140,
   160, 180, 200, 225, 250, 285, 320, 355, 400, 450, 500, 565, 635, 715, 800, 900 };

// MIDI note number (0..127) -> frequency (Hz), rounded to the nearest integer:
// freq = 440 * 2^((note-69)/12). Note 69 = A4 = 440, note 60 = C4 (ToneControl.C4).
// Used by the MMAPI tone-sequence decoder (ToneControl) and Manager.playTone. Integer Hz
// feeds the Q16 phase accumulator, which then produces the exact pitch (same as OTA).
const int MIDI_HZ[128] = {
        8,     9,     9,    10,    10,    11,    12,    12,    13,    14,    15,    15,
       16,    17,    18,    19,    21,    22,    23,    24,    26,    28,    29,    31,
       33,    35,    37,    39,    41,    44,    46,    49,    52,    55,    58,    62,
       65,    69,    73,    78,    82,    87,    92,    98,   104,   110,   117,   123,
      131,   139,   147,   156,   165,   175,   185,   196,   208,   220,   233,   247,
      262,   277,   294,   311,   330,   349,   370,   392,   415,   440,   466,   494,
      523,   554,   587,   622,   659,   698,   740,   784,   831,   880,   932,   988,
     1047,  1109,  1175,  1245,  1319,  1397,  1480,  1568,  1661,  1760,  1865,  1976,
     2093,  2217,  2349,  2489,  2637,  2794,  2960,  3136,  3322,  3520,  3729,  3951,
     4186,  4435,  4699,  4978,  5274,  5588,  5920,  6272,  6645,  7040,  7459,  7902,
     8372,  8870,  9397,  9956, 10548, 11175, 11840, 12544 };

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
Ps2Audio::MidiEv Ps2Audio::midiEvents_[Ps2Audio::MAX_MIDI_EVENTS];

namespace {
// Big-endian readers for the Standard MIDI File header/chunk sizes.
inline unsigned int be16(const unsigned char* p) {
    return (unsigned int)((p[0] << 8) | p[1]);
}
inline unsigned int be32(const unsigned char* p) {
    return (unsigned int)(((unsigned int)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}
// Read a MIDI variable-length quantity (7 bits/byte, MSB = continue). Advances *pos.
inline int readVlq(const unsigned char* d, int* pos, int end) {
    int v = 0, n = 0;
    while (*pos < end && n < 4) {
        unsigned char b = d[(*pos)++];
        v = (v << 7) | (b & 0x7F);
        ++n;
        if (!(b & 0x80)) {
            break;
        }
    }
    return v;
}
} // namespace

Ps2Audio& Ps2Audio::instance() {
    static Ps2Audio inst;
    return inst;
}

Ps2Audio::Ps2Audio()
    : bank_(0), bankSize_(0), bankRate_(0), bankZones_(0),
      progDir_(0), drumDir_(0), zoneTab_(0), bankPcm_(0),
      ready_(false), mixerStarted_(false), mixerPaused_(false), mixerId_(0), mixerStack_(0),
      voiceSema_(0), nextId_(0),
      midiCount_(0), midiEventIdx_(0), midiCurSample_(0), midiSongSamples_(0) {}

bool Ps2Audio::loadBank(const char* path) {
    if (bank_ != 0) {
        return true;                              // already loaded (idempotent)
    }
    if (path == 0 || path[0] == '\0') {
        return false;
    }
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        javacall_print("[bank] not found (staying on square-wave synth)\n");
        return false;
    }
    // Whole-file size from a seek; the block is small enough to slurp at once.
    const long size = (long)::lseek(fd, 0, SEEK_END);
    ::lseek(fd, 0, SEEK_SET);
    if (size < 24 || size > 12 * 1024 * 1024) {   // header present, not absurd
        ::close(fd);
        javacall_print("[bank] bad size\n");
        return false;
    }
    // Allocate OUTSIDE the Java pool (plain newlib malloc; the pool is a separate
    // JVM_Start chunk that doesn't exist yet at boot). 64-byte aligned for the DMA-
    // friendly access the synth will do later. Never freed -> lives the whole run.
    unsigned char* buf = (unsigned char*)memalign(64, (size_t)size);
    if (buf == 0) {
        ::close(fd);
        javacall_print("[bank] out of memory\n");
        return false;
    }
    long off = 0;
    while (off < size) {
        const long want = (size - off > 262144) ? 262144 : (size - off);
        const int r = (int)::read(fd, buf + off, (size_t)want);
        if (r <= 0) break;
        off += r;
    }
    ::close(fd);
    if (off != size ||
        buf[0] != 'P' || buf[1] != 'S' || buf[2] != '2' || buf[3] != 'B') {
        free(buf);
        javacall_print("[bank] read/verify failed\n");
        return false;
    }
    const unsigned int rate   = le32(buf + 8);
    const unsigned int nzones = le32(buf + 16);
    const unsigned int npcm   = le32(buf + 20);
    // Section pointers into the blob (EE is little-endian, so the on-disk structs map
    // directly -- no byte swapping). Verify the computed layout matches the file size
    // before trusting the counts.
    const unsigned char* progDir = buf + 24;
    const unsigned char* drumDir = progDir + 128 * 4;
    const unsigned char* zoneTab = drumDir + 128 * 4;
    const long expect = 24 + 128 * 4 + 128 * 4 +
                        (long)nzones * ZONE_BYTES + (long)npcm * 2;
    if (expect != size) {
        free(buf);
        char m[96];
        snprintf(m, sizeof(m), "[bank] layout mismatch expect=%ld size=%ld\n",
                 expect, size);
        javacall_print(m);
        return false;
    }
    bank_      = buf;
    bankSize_  = (int)size;
    bankRate_  = (int)rate;
    bankZones_ = (int)nzones;
    progDir_   = progDir;
    drumDir_   = drumDir;
    zoneTab_   = zoneTab;
    bankPcm_   = (const short*)(zoneTab + (long)nzones * ZONE_BYTES);
    char m[128];
    snprintf(m, sizeof(m), "[bank] loaded %ld bytes, %u Hz, %u zones, %u pcm\n",
             size, rate, nzones, npcm);
    javacall_print(m);
    if (rate != (unsigned)SAMPLE_RATE) {
        // Not fatal for Stage 1 (we only load here); the synth expects SAMPLE_RATE.
        snprintf(m, sizeof(m), "[bank] WARNING rate %u != output %d\n",
                 rate, SAMPLE_RATE);
        javacall_print(m);
    }
    return true;
}

bool Ps2Audio::loadBgm(const char* path) {
    if (g_bgmLoaded) {
        return true;                          // idempotent
    }
    if (!ready_ || path == 0 || path[0] == '\0') {
        return false;
    }
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        javacall_print("[bgm] not found (menu stays silent)\n");
        return false;
    }
    const long rawSize = (long)::lseek(fd, 0, SEEK_END);
    ::lseek(fd, 0, SEEK_SET);
    // We prepend a 16-byte header (see below), so the SPU2 payload is rawSize + 16.
    if (rawSize < 16 || rawSize + 16 > 2 * 1024 * 1024) {   // must fit the SPU2's 2 MB local RAM
        ::close(fd);
        javacall_print("[bgm] bad size\n");
        return false;
    }
    // ps2adpcm emits RAW PS-ADPCM blocks with NO header (it targets the ps2snd IRX). But audsrv's
    // IRX assumes a 16-byte header which it SKIPS, reading pitch from word[2] and loop/channels
    // from word[1]. With no real header, word[2] == 0 => SPU2 pitch 0 => the voice never advances
    // => dead silence (this was the bug). So we build our own 16-byte header here and put the raw
    // ADPCM right after it. The loop is already baked into the block flags by ps2adpcm -l0, so the
    // 'loop'/'channels' header fields are cosmetic (audsrv's play path never reads them); only
    // 'pitch' actually drives the hardware.
    const long size = rawSize + 16;
    // Read the ADPCM file into a temporary EE buffer just to hand it to the SPU2; audsrv_load_adpcm
    // copies it into SPU2 RAM, so we free the EE copy right after -- the music then costs ZERO EE
    // RAM (and ZERO EE CPU: the SPU2 loops it in hardware).
    unsigned char* buf = (unsigned char*)memalign(64, (size_t)size);
    if (buf == 0) {
        ::close(fd);
        javacall_print("[bgm] out of memory\n");
        return false;
    }
    long off = 16;                               // raw ADPCM lands after the 16-byte header
    while (off < size) {
        const long want = (size - off > 262144) ? 262144 : (size - off);
        const int r = (int)::read(fd, buf + off, (size_t)want);
        if (r <= 0) break;
        off += r;
    }
    ::close(fd);
    if (off != size) {
        free(buf);
        javacall_print("[bgm] read failed\n");
        return false;
    }
    // SPU2 pitch: 0x1000 (4096) == 48000 Hz native rate, so pitch = rate * 4096 / 48000 (rounded).
    // BGM_RATE must match tools/mkbgm.sh RATE; bump it here if the track ever plays fast/slow.
    const int      BGM_RATE  = 11025;
    const unsigned bgmPitch  = (unsigned)((BGM_RATE * 4096 + 24000) / 48000);
    unsigned* hdr = (unsigned*)buf;              // EE is little-endian; audsrv reads these as LE u32
    hdr[0] = 0;                                  // not read by the IRX
    hdr[1] = (1u << 16) | (1u << 8);             // loop=1, channels=1 (cosmetic)
    hdr[2] = bgmPitch;                           // THE fix: real pitch (was 0 -> silent)
    hdr[3] = 0;

    audsrv_adpcm_init();

    // CRITICAL (HW-only bug): audsrv_load_adpcm ships this buffer to the IOP via sceSifSetDma
    // WITHOUT writing back the EE data cache first. The 16-byte header we just built with CPU
    // stores still sits in the EE's write-back cache, so the EE->IOP DMA reads STALE main RAM
    // (pitch == 0 => SPU2 voice never advances => silent). The ADPCM payload survives because
    // ::read() DMA'd it straight to RAM, but the CPU-written header does not. PCSX2 hid this by
    // treating the cache as coherent. Write the EE data cache back to RAM so the real header
    // reaches the IOP (WRITEBACK_DCACHE flushes dirty lines, covering our CPU-written header).
    FlushCache(WRITEBACK_DCACHE);

    const int lr = audsrv_load_adpcm(&g_bgm, buf, (int)size);   // upload to SPU2 RAM
    free(buf);                                                  // EE copy no longer needed
    if (lr != AUDSRV_ERR_NOERROR) {
        char m[64];
        snprintf(m, sizeof m, "[bgm] load_adpcm failed (%d) -> menu stays silent\n", lr);
        javacall_print(m);
        return false;
    }
    g_bgmLoaded = true;
    char m[80];
    snprintf(m, sizeof m, "[bgm] loaded %ld bytes -> SPU2 (pitch=%d, loop=%d)\n",
             size, g_bgm.pitch, g_bgm.loop);
    javacall_print(m);
    return true;
}

void Ps2Audio::startBgm() {
    if (!g_bgmLoaded) {
        return;
    }
    // audsrv is SIF RPC and the mixer thread is already feeding audsrv concurrently, so
    // serialize on the shared SifLock (the non-reentrant SIF bus would otherwise clash).
    SifGuard guard;
    if (g_bgmCh < 0) {
        g_bgmCh = audsrv_ch_play_adpcm(-1, &g_bgm);   // allocate a voice, start looping
    }
    if (g_bgmCh >= 0) {
        audsrv_adpcm_set_volume_and_pan(g_bgmCh, BGM_VOL, 0);   // (un)mute to full
    }
}

void Ps2Audio::stopBgm() {
    if (g_bgmCh >= 0) {
        SifGuard guard;
        audsrv_adpcm_set_volume_and_pan(g_bgmCh, 0, 0);   // pause: mute, keep the voice looping
    }
}

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
    bool paused = false;            // tracks the pause edge so we mute/re-mix exactly once

    for (;;) {
        // Paused on the menu: the hardware ADPCM BGM owns the SPU2 there, so we push no
        // streaming. On the pause EDGE, mute the streaming input (audsrv_stop_audio zeros
        // the Core-1 data-input volume): the ring's DMA keeps looping the last game block,
        // so without this the final game sound drones on under the BGM. Muting the input
        // does NOT touch the ADPCM voice (it has its own voice volume). Done on this thread
        // to avoid racing our own play_audio, which would re-activate the volume. play_audio
        // re-activates it automatically on resume.
        if (mixerPaused_) {
            if (!paused) {
                SifGuard guard;
                audsrv_stop_audio();
                paused = true;
            }
            DelayThread(10000);
            continue;
        }
        if (paused) {               // just resumed for a game: drop the pre-pause block
            paused = false;
            sent   = blockBytes;    // force a fresh mix (else we'd flush a stale block)
        }

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
                    } else if (vc.kind == KIND_MIDI) {   // polyphonic MIDI synth
                        s = midiRenderSample(&ended);
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

                    if (ended) {                    // one full pass done (PCM/seq/MIDI)
                        if (vc.kind == KIND_MIDI && (vc.infinite || vc.loopsLeft > 1)) {
                            midiReset();            // rewind the song for the next loop
                        }
                        if (vc.infinite) {
                            /* keep playing from the top (cursors already reset) */
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

int Ps2Audio::decodeToneSeq(const unsigned char* data, int len, Note* out, int maxNotes,
                            int* gateOut) {
    // MMAPI ToneControl tags (all negative; notes are 0..127, SILENCE = -1).
    const int VERSION = -2, TEMPO = -3, RESOLUTION = -4;
    const int BLOCK_START = -5, BLOCK_END = -6, PLAY_BLOCK = -7;
    const int SET_VOLUME = -8, REPEAT = -9;   // SILENCE (-1) falls through to the note branch

    if (len < 2 || (signed char)data[0] != VERSION) {
        return 0;                       // must start with {VERSION, 1}
    }
    int pos   = 2;                       // past VERSION + version number
    int tempo = 30;                      // default tempo byte (BPM = tempo * 4 = 120)
    int reso  = 64;                      // default resolution (pulses per whole note)
    if (pos + 1 < len && (signed char)data[pos] == TEMPO) {
        tempo = data[pos + 1];
        pos += 2;
    }
    if (pos + 1 < len && (signed char)data[pos] == RESOLUTION) {
        reso = data[pos + 1];
        pos += 2;
    }
    if (tempo < 1) {
        tempo = 30;
    }
    if (reso < 1) {
        reso = 64;
    }

    // note duration (pulses) -> samples: ms = dur*60000/(reso*tempo); samples = SR*ms/1000
    // = SR*dur*60/(reso*tempo). 64-bit to avoid overflow; notes sustain full (gate 100).
    int blockPos[128];
    for (int i = 0; i < 128; ++i) {
        blockPos[i] = -1;
    }
    int retStack[32], retSp = 0;
    int count = 0, guard = 0;
    const int GUARD_MAX = maxNotes * 8 + 4096;   // bound PLAY_BLOCK / malformed loops

    while (pos + 1 < len && count < maxNotes && guard++ < GUARD_MAX) {
        int t = (signed char)data[pos];        // tag or note (signed)
        int d = data[pos + 1];                 // duration / block id / multiplier (0..127)

        if (t == BLOCK_START) {
            blockPos[d & 127] = pos;           // remember where this block begins
            pos += 2;
        } else if (t == BLOCK_END) {
            if (retSp > 0) {
                pos = retStack[--retSp];       // came here via PLAY_BLOCK: return
            } else {
                pos += 2;                       // inline definition end: fall through
            }
        } else if (t == PLAY_BLOCK) {
            int bp = blockPos[d & 127];
            if (bp >= 0 && retSp < 32) {
                retStack[retSp++] = pos + 2;   // resume after PLAY_BLOCK
                pos = bp + 2;                   // jump past the BLOCK_START marker
            } else {
                pos += 2;                       // undefined block: skip
            }
        } else if (t == REPEAT) {
            int mult = d;
            pos += 2;
            if (pos + 1 < len) {
                int nv = (signed char)data[pos];
                int nd = data[pos + 1];
                pos += 2;
                long samples = (long)SAMPLE_RATE * nd * 60 / ((long)reso * tempo);
                if (samples < 1) {
                    samples = 1;
                }
                unsigned int inc = (nv >= 0 && nv <= 127)
                    ? (unsigned int)((unsigned long long)MIDI_HZ[nv] * 65536ULL
                                     / (unsigned)SAMPLE_RATE)
                    : 0;                        // SILENCE / out of range -> rest
                for (int k = 0; k < mult && count < maxNotes; ++k) {
                    out[count].phaseInc = inc;
                    out[count].samples  = (int)samples;
                    ++count;
                }
            }
        } else if (t == SET_VOLUME || t == TEMPO || t == RESOLUTION || t == VERSION) {
            if (t == TEMPO) {
                tempo = (d < 1) ? tempo : d;   // tolerate a mid-stream tempo change
            } else if (t == RESOLUTION) {
                reso = (d < 1) ? reso : d;
            }
            pos += 2;                           // SET_VOLUME ignored (player-level volume)
        } else {
            // A note (0..127) or SILENCE (-1): emit one tone/rest.
            long samples = (long)SAMPLE_RATE * d * 60 / ((long)reso * tempo);
            if (samples < 1) {
                samples = 1;
            }
            out[count].phaseInc = (t >= 0 && t <= 127)
                ? (unsigned int)((unsigned long long)MIDI_HZ[t] * 65536ULL
                                 / (unsigned)SAMPLE_RATE)
                : 0;                            // SILENCE (t == -1) -> rest
            out[count].samples = (int)samples;
            ++count;
            pos += 2;
        }
    }

    *gateOut = 100;   // tone sequences sustain the full note (like the MIDI reference)
    return count;
}

int Ps2Audio::submitToneSeq(const unsigned char* data, int len, int vol, int loop) {
    if (!mixerStarted_ || data == 0 || len < 2) {
        return -1;
    }

    const int v = 0;
    // Offline before touching this voice's note list (decode writes into it).
    WaitSema(voiceSema_);
    voices_[v].active = false;
    SignalSema(voiceSema_);

    int gate = 100;
    int count = decodeToneSeq(data, len, noteStore_[v], MAX_NOTES, &gate);
    if (count <= 0) {
        return -1;                  // not a valid tone sequence / no notes -> stay silent
    }
    voices_[v].kind        = KIND_SEQ;
    voices_[v].notes       = noteStore_[v];
    voices_[v].noteCount   = count;
    voices_[v].gatePercent = gate;
    return activateVoice(v, vol, loop);
}

int Ps2Audio::midiEvCmp(const void* a, const void* b) {
    const MidiEv* x = (const MidiEv*)a;
    const MidiEv* y = (const MidiEv*)b;
    if (x->atTick != y->atTick) {
        return (x->atTick < y->atTick) ? -1 : 1;
    }
    return (x->seq < y->seq) ? -1 : (x->seq > y->seq ? 1 : 0);
}

int Ps2Audio::decodeMidi(const unsigned char* data, int len, MidiEv* out, int maxEvents,
                         int* songSamplesOut) {
    *songSamplesOut = 0;
    if (len < 14 || data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd') {
        return 0;
    }
    unsigned int headerLen = be32(data + 4);
    int ntracks           = (int)be16(data + 10);
    unsigned int division = be16(data + 12);
    // PPQN division (bit 15 clear). SMPTE (bit 15 set) is rare in games -> approximate.
    int ticksPerQ = (division & 0x8000) ? 96 : (int)(division & 0x7FFF);
    if (ticksPerQ < 1) {
        ticksPerQ = 96;
    }

    int pos   = 8 + (int)headerLen;   // first MTrk (headerLen is normally 6 -> pos 14)
    int count = 0, seq = 0;

    for (int tr = 0; tr < ntracks && pos + 8 <= len && count < maxEvents; ++tr) {
        if (!(data[pos] == 'M' && data[pos + 1] == 'T' &&
              data[pos + 2] == 'r' && data[pos + 3] == 'k')) {
            break;                          // not a track chunk -> stop
        }
        int trkEnd = pos + 8 + (int)be32(data + pos + 4);
        if (trkEnd > len) {
            trkEnd = len;
        }
        pos += 8;
        int absTick = 0;
        int running = 0;
        while (pos < trkEnd && count < maxEvents) {
            absTick += readVlq(data, &pos, trkEnd);
            if (pos >= trkEnd) {
                break;
            }
            int status = data[pos];
            if (status < 0x80) {
                status = running;           // running status: reuse previous
            } else {
                ++pos;
            }
            int hi = status & 0xF0;

            if (status == 0xFF) {           // meta event
                if (pos >= trkEnd) {
                    break;
                }
                int metaType = data[pos++];
                int mlen = readVlq(data, &pos, trkEnd);
                if (metaType == 0x51 && mlen == 3 && pos + 3 <= trkEnd) {   // set tempo
                    out[count].atTick = absTick;
                    out[count].seq    = seq++;
                    out[count].type   = MEV_TEMPO;
                    out[count].tempo  = ((unsigned int)data[pos] << 16)
                                        | (data[pos + 1] << 8) | data[pos + 2];
                    out[count].note = 0; out[count].vel = 0; out[count].chan = 0;
                    ++count;
                }
                pos += mlen;
                running = 0;
                if (metaType == 0x2F) {     // end of track
                    break;
                }
            } else if (status == 0xF0 || status == 0xF7) {   // sysex: skip
                pos += readVlq(data, &pos, trkEnd);
                running = 0;
            } else {                        // channel voice message
                running = status;
                if (hi == 0x80 || hi == 0x90) {
                    if (pos + 2 > trkEnd) {
                        break;
                    }
                    int note = data[pos++];
                    int vel  = data[pos++];
                    out[count].atTick = absTick;
                    out[count].seq    = seq++;
                    out[count].type   = (hi == 0x90 && vel > 0) ? MEV_ON : MEV_OFF;
                    out[count].note   = (unsigned char)(note & 0x7F);
                    out[count].vel    = (unsigned char)(vel & 0x7F);
                    out[count].chan   = (unsigned char)(status & 0x0F);
                    out[count].tempo  = 0;
                    ++count;
                } else if (hi == 0xC0) {    // program change -> select the instrument
                    if (pos + 1 > trkEnd) {
                        break;
                    }
                    int prog = data[pos++];
                    out[count].atTick = absTick;
                    out[count].seq    = seq++;
                    out[count].type   = MEV_PROG;
                    out[count].note   = (unsigned char)(prog & 0x7F);
                    out[count].vel    = 0;
                    out[count].chan   = (unsigned char)(status & 0x0F);
                    out[count].tempo  = 0;
                    ++count;
                } else if (hi == 0xD0) {
                    pos += 1;               // channel pressure (ignored)
                } else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
                    pos += 2;               // aftertouch / control change / pitch bend
                } else {
                    break;                  // unknown status -> bail this track
                }
            }
        }
        pos = trkEnd;                       // next chunk, wherever parsing stopped
    }

    if (count <= 0) {
        return 0;
    }

    // Sort by (tick, seq), then one pass applying the tempo map to get sample times.
    qsort(out, (size_t)count, sizeof(MidiEv), &Ps2Audio::midiEvCmp);
    long long    anchorSample = 0;
    int          anchorTick   = 0;
    unsigned int usPerQ       = 500000;     // default 120 BPM
    for (int i = 0; i < count; ++i) {
        long long s = anchorSample
            + (long long)(out[i].atTick - anchorTick) * usPerQ * SAMPLE_RATE
              / ((long long)ticksPerQ * 1000000LL);
        if (out[i].type == MEV_TEMPO) {
            anchorSample = s;
            anchorTick   = out[i].atTick;
            usPerQ       = out[i].tempo ? out[i].tempo : usPerQ;
            out[i].atSample = -1;           // consumed by the tempo map
        } else {
            out[i].atSample = (int)s;
        }
    }

    // Compact: keep note on/off + program-change (drop only the tempo events; percussion
    // on channel 10 = index 9 is kept, synthesized from the drum kit / noise in the mixer).
    // Only note events extend the song length; a trailing program-change must not.
    int w = 0, last = 0;
    for (int i = 0; i < count; ++i) {
        if (out[i].type == MEV_TEMPO) {
            continue;
        }
        out[w] = out[i];
        if ((out[w].type == MEV_ON || out[w].type == MEV_OFF) && out[w].atSample > last) {
            last = out[w].atSample;
        }
        ++w;
    }
    *songSamplesOut = last + SAMPLE_RATE / 2;   // half-second tail for the final releases
    return w;
}

int Ps2Audio::submitMidi(const unsigned char* data, int len, int vol, int loop) {
    if (!mixerStarted_ || data == 0 || len < 14) {
        return -1;
    }

    const int v = 0;
    // Offline before rewriting the MIDI event list / cursors (mixer skips inactive voices).
    WaitSema(voiceSema_);
    voices_[v].active = false;
    SignalSema(voiceSema_);

    int songSamples = 0;
    int count = decodeMidi(data, len, midiEvents_, MAX_MIDI_EVENTS, &songSamples);
    if (count <= 0) {
        return -1;                  // not valid MIDI / no notes -> stay silent
    }
    midiCount_       = count;
    midiSongSamples_ = songSamples;
    midiReset();
    voices_[v].kind = KIND_MIDI;
    return activateVoice(v, vol, loop);
}

// Decode bank zone #idx into z (little-endian on-disk record; EE is LE). Layout must
// match tools/sf2bank ZONE_FMT: <IIII bB BB h H HHHH H B H B> = 38 bytes.
void Ps2Audio::readZone(int idx, BankZone& z) const {
    const unsigned char* p = zoneTab_ + (long)idx * ZONE_BYTES;
    z.off    = le32(p + 0);
    z.length = le32(p + 4);
    z.ls     = le32(p + 8);
    z.le     = le32(p + 12);
    z.cents  = (int)(signed char)p[16];
    z.root   = p[17];
    z.klo    = p[18];
    z.khi    = p[19];
    z.pan    = (int)(short)le16(p + 20);
    z.atten  = (int)le16(p + 22);
    z.a      = (int)le16(p + 24);
    z.h      = (int)le16(p + 26);
    z.d      = (int)le16(p + 28);
    z.r      = (int)le16(p + 30);
    z.susp   = (int)le16(p + 32);
    z.mode   = p[34];
    z.fc     = (int)le16(p + 35);
    z.fq     = p[37];
}

// Zone index for a melodic (prog, note): first zone whose key range covers note, else
// the program's last zone (fall back rather than go silent). -1 if the program is empty.
int Ps2Audio::bankMelodicZone(int prog, int note) const {
    if (prog < 0 || prog > 127) {
        prog = 0;
    }
    const unsigned char* d = progDir_ + prog * 4;
    int start = (int)le16(d);
    int count = d[2];
    int best = -1;
    for (int i = 0; i < count; ++i) {
        const int idx = start + i;
        const unsigned char* p = zoneTab_ + (long)idx * ZONE_BYTES;
        const int klo = p[18], khi = p[19];
        if (note >= klo && note <= khi) {
            return idx;
        }
        best = idx;
    }
    return best;
}

// Zone index for a GM drum note (one zone per key), -1 if the kit lacks that key.
int Ps2Audio::bankDrumZone(int note) const {
    if (note < 0 || note > 127) {
        return -1;
    }
    const unsigned char* d = drumDir_ + note * 4;
    return d[2] > 0 ? (int)le16(d) : -1;
}

// A free oscillator slot, else steal the quietest active one (so a new note always
// sounds). Amplitude is the wavetable env*gain or the square envelope level.
int Ps2Audio::midiPickSlot() {
    int quietest = -1, minAmp = 0x7FFFFFFF;
    for (int i = 0; i < POLY; ++i) {
        Osc& o = midiOsc_[i];
        if (!o.active) {
            return i;
        }
        const int amp = o.wave ? (int)(o.envCur * o.wgain * 32768.0f) : o.cur;
        if (amp < minAmp) {
            minAmp = amp;
            quietest = i;
        }
    }
    return quietest;
}

// Set up oscillator o to play bank zone z for (note, vel) on chan. Mirrors the offline
// render_wav _mk_voice + make_env: Q16 resample step from the pitch, EMU-scaled
// attenuation gain, and an ADSR (looped) or declick (one-shot) envelope.
void Ps2Audio::startWaveOsc(Osc& o, const BankZone& z, int note, int vel, int chan) {
    o.active  = true;
    o.wave    = true;
    o.drum    = 0;
    o.note    = (unsigned char)note;
    o.chan    = (unsigned char)chan;
    o.sOff    = z.off;
    o.sLen    = z.length;
    o.sLs     = z.ls;
    o.sLe     = z.le;
    o.loop    = (z.mode == 1 || z.mode == 3) && (z.le > z.ls + 1);
    o.oneshot = !o.loop;
    o.wpos    = 0;

    // step = 2^((note - root + cents/100) / 12), in Q16.
    const double semis = (double)(note - z.root) + (double)z.cents / 100.0;
    double st = pow(2.0, semis / 12.0) * 65536.0;
    if (st < 1.0) {
        st = 1.0;
    } else if (st > (double)0x7FFFFFFF) {
        st = (double)0x7FFFFFFF;
    }
    o.wstep = (unsigned int)(st + 0.5);

    // gain = velocity * attenuation. SF2 initialAttenuation is centibels, scaled by the
    // EMU 0.4 factor (as FluidSynth does); raw cB made high-atten voices near-silent.
    const double attenGain = pow(10.0, -(double)z.atten * 0.4 / 200.0);
    o.wgain = (float)((double)vel / 127.0 * attenGain);

    // envelope stage lengths (samples). One-shot uses only a short declick.
    if (o.oneshot) {
        o.aLen = SAMPLE_RATE / 500;          // ~2 ms fade-in
        o.rLen = SAMPLE_RATE / 166;          // ~6 ms fade-out at the sample end
        if (o.rLen < 1) o.rLen = 1;
    } else {
        o.aLen = z.a * SAMPLE_RATE / 1000;
        o.hLen = z.h * SAMPLE_RATE / 1000;
        o.dLen = z.d * SAMPLE_RATE / 1000;
        o.rLen = z.r * SAMPLE_RATE / 1000;
        if (o.rLen < 1) o.rLen = 1;
        o.susLvl = (float)z.susp / 1000.0f;
    }
    o.estage   = 0;
    o.stagePos = 0;
    o.envCur   = 0.0f;
    o.relFrom  = 0.0f;

    // Per-voice low-pass filter (RBJ biquad). fc == 0 means the zone left it wide open.
    // The SF2 Q is centibels of peak gain; FluidSynth uses Q = 10^(cB/200). Computed
    // once here at note-on (sin/cos off the hot path); applied per sample in the mixer.
    o.hasFilter = false;
    const int fcEff = (int)(z.fc * WAVE_FC_SCALE);
    if (z.fc > 0 && fcEff < (int)(SAMPLE_RATE * 0.49)) {
        const double PI_ = 3.14159265358979323846;
        const double w0   = 2.0 * PI_ * (double)fcEff / (double)SAMPLE_RATE;
        const double cosw = cos(w0), sinw = sin(w0);
        double Q = (z.fq > 0) ? pow(10.0, (double)z.fq / 200.0) : 0.70710678;
        if (Q < 0.70710678) {
            Q = 0.70710678;
        }
        const double alpha = sinw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        o.fb0 = (float)(((1.0 - cosw) / 2.0) / a0);
        o.fb1 = (float)((1.0 - cosw) / a0);
        o.fb2 = o.fb0;
        o.fa1 = (float)((-2.0 * cosw) / a0);
        o.fa2 = (float)((1.0 - alpha) / a0);
        o.fx1 = o.fx2 = o.fy1 = o.fy2 = 0.0f;
        o.hasFilter = true;
    }
}

void Ps2Audio::midiReset() {
    midiEventIdx_  = 0;
    midiCurSample_ = 0;
    for (int i = 0; i < POLY; ++i) {
        midiOsc_[i].active = false;
    }
    for (int i = 0; i < 16; ++i) {
        chanProg_[i] = 0;               // GM default: program 0 (piano) until a change
    }
}

void Ps2Audio::midiAllocOsc(int note, int vel, int chan) {
    if (note < 0 || note > 127) {
        return;
    }
    // A re-trigger of a still-held note: release the old voice first, else it drones to
    // song end (mush). Matters for dense lines that repeat the same pitch (mirrors the
    // offline render's retrigger handling).
    midiReleaseOsc(note, chan);

    const int slot = midiPickSlot();
    if (slot < 0) {
        return;
    }
    Osc& o = midiOsc_[slot];

    // Wavetable path: play the bank sample for this channel's program.
    if (bank_ != 0) {
        const int zi = bankMelodicZone(chanProg_[chan & 15], note);
        if (zi >= 0) {
            BankZone z;
            readZone(zi, z);
            startWaveOsc(o, z, note, vel, chan);
            return;
        }
        // no zone for this program -> fall through to the square synth
    }

    // Square fallback (no bank loaded, or program has no sample).
    o.active   = true;
    o.wave     = false;
    o.note     = (unsigned char)note;
    o.env      = 0;                 // attack
    o.drum     = 0;                 // melodic square
    o.envPos   = 0;
    o.phase    = 0;
    o.phaseInc = (unsigned int)((unsigned long long)MIDI_HZ[note] * 65536ULL
                                / (unsigned)SAMPLE_RATE);
    o.peak     = (vel * 2600) / 127;   // velocity-scaled amplitude
    o.cur      = 0;
    o.relStart = 0;
    o.relLen   = MIDI_RELEASE;
}

void Ps2Audio::midiAllocDrum(int note, int vel) {
    // Percussion (GM drum map on channel 10): a one-shot that decays and frees itself
    // (note-off is ignored).
    const int slot = midiPickSlot();
    if (slot < 0) {
        return;
    }
    Osc& o = midiOsc_[slot];

    // Wavetable path: play the kit sample for this drum key (builder forced root == key,
    // so the step is 1.0 and it plays as a one-shot).
    if (bank_ != 0) {
        const int zi = bankDrumZone(note);
        if (zi >= 0) {
            BankZone z;
            readZone(zi, z);
            startWaveOsc(o, z, note, vel, 9);
            return;
        }
        // no kit sample for this key -> fall through to the noise/low-tone synth
    }

    // Square/noise fallback: map the drum note to a rough character.
    int kind   = 1;                 // 1 = noise, 2 = kick (low square)
    int decay  = SAMPLE_RATE / 8;   // default ~125 ms
    int freqHz = 0;
    if (note == 35 || note == 36) {                 // acoustic / bass kick
        kind = 2; freqHz = 62; decay = SAMPLE_RATE / 12;      // ~83 ms low thump
    } else if (note == 38 || note == 40 || note == 37 || note == 39) {  // snare / clap
        kind = 1; decay = SAMPLE_RATE / 7;                    // ~140 ms
    } else if (note == 42 || note == 44) {          // closed hi-hat
        kind = 1; decay = SAMPLE_RATE / 24;                   // ~40 ms
    } else if (note == 46) {                        // open hi-hat
        kind = 1; decay = SAMPLE_RATE / 6;                    // ~165 ms
    } else if (note == 49 || note == 51 || note == 52 || note == 53 ||
               note == 55 || note == 57 || note == 59) {      // cymbals
        kind = 1; decay = SAMPLE_RATE / 2;                    // ~500 ms
    } else if (note >= 41 && note <= 50) {          // toms
        kind = 2; freqHz = 90 + (note - 41) * 12; decay = SAMPLE_RATE / 9;
    }

    o.active   = true;
    o.wave     = false;
    o.note     = (unsigned char)note;
    o.env      = 2;                 // straight to decay (percussive, no sustain)
    o.drum     = (unsigned char)kind;
    o.envPos   = 0;
    o.phase    = 0;
    o.phaseInc = freqHz ? (unsigned int)((unsigned long long)freqHz * 65536ULL
                                         / (unsigned)SAMPLE_RATE) : 0;
    o.peak     = (vel * 2800) / 127;
    o.cur      = (vel * 2800) / 127;
    o.relStart = o.cur;
    o.relLen   = decay;
    o.lfsr     = (unsigned int)(midiCurSample_ ^ (note * 2654435761u) ^ (slot << 13)) | 1u;
}

void Ps2Audio::midiReleaseOsc(int note, int chan) {
    // Release the first held (non-releasing) oscillator of this pitch on this channel.
    // Wavetable voices switch to the ADSR release stage; square voices to their decay.
    for (int i = 0; i < POLY; ++i) {
        Osc& o = midiOsc_[i];
        if (!o.active || o.note != note) {
            continue;
        }
        if (o.wave) {
            if (o.oneshot || o.estage >= 4 || o.chan != chan) {
                continue;           // one-shots ignore note-off; skip already-releasing
            }
            o.relFrom  = o.envCur;
            o.estage   = 4;         // release
            o.stagePos = 0;
            return;
        }
        if (o.env != 2) {
            o.env      = 2;         // square: release
            o.relStart = o.cur;
            o.envPos   = 0;
            return;
        }
    }
}

int Ps2Audio::wrapIdx(const Osc& o, int j) {
    if (o.loop) {
        const int loopLen = (int)o.sLe - (int)o.sLs;
        if (loopLen > 0 && j >= (int)o.sLs) {
            j = (int)o.sLs + (j - (int)o.sLs) % loopLen;
        }
    }
    if (j < 0) {
        j = 0;
    } else if (j >= (int)o.sLen) {
        j = (int)o.sLen - 1;
    }
    return j;
}

int Ps2Audio::midiRenderSample(bool* ended) {
    // Dispatch every event scheduled at or before the current sample.
    while (midiEventIdx_ < midiCount_ &&
           midiEvents_[midiEventIdx_].atSample <= midiCurSample_) {
        MidiEv& e = midiEvents_[midiEventIdx_++];
        if (e.type == MEV_PROG) {       // program change: pick the channel's instrument
            chanProg_[e.chan & 15] = e.note;
        } else if (e.chan == 9) {       // percussion: one-shot, note-off ignored
            if (e.type == MEV_ON) {
                midiAllocDrum(e.note, e.vel);
            }
        } else if (e.type == MEV_ON) {
            midiAllocOsc(e.note, e.vel, e.chan);
        } else {
            midiReleaseOsc(e.note, e.chan);
        }
    }

    // Mix the active oscillators: wavetable voices (bank samples, cubic-interpolated,
    // ADSR/declick) plus any square/noise fallback voices (linear envelope).
    float accf = 0.0f;
    for (int i = 0; i < POLY; ++i) {
        Osc& o = midiOsc_[i];
        if (!o.active) {
            continue;
        }
        if (o.wave) {
            // ---- envelope (0..1) ----
            float amp;
            if (o.oneshot) {
                amp = (o.stagePos < o.aLen && o.aLen > 0)
                          ? (float)o.stagePos / (float)o.aLen : 1.0f;
                const int left = (int)o.sLen - (int)(o.wpos >> 16);
                if (left < o.rLen) {
                    float f = (o.rLen > 0) ? (float)left / (float)o.rLen : 0.0f;
                    if (f < 0.0f) f = 0.0f;
                    amp *= f;
                }
                ++o.stagePos;
            } else {
                switch (o.estage) {
                case 0:                                     // attack
                    amp = (o.aLen > 0) ? (float)o.stagePos / (float)o.aLen : 1.0f;
                    if (++o.stagePos >= o.aLen) { o.estage = 1; o.stagePos = 0; }
                    break;
                case 1:                                     // hold
                    amp = 1.0f;
                    if (++o.stagePos >= o.hLen) { o.estage = 2; o.stagePos = 0; }
                    break;
                case 2:                                     // decay 1 -> sustain
                    amp = 1.0f + (o.susLvl - 1.0f) *
                          ((o.dLen > 0) ? (float)o.stagePos / (float)o.dLen : 1.0f);
                    if (++o.stagePos >= o.dLen) { o.estage = 3; o.stagePos = 0; }
                    break;
                case 3:                                     // sustain
                    amp = o.susLvl;
                    break;
                default:                                    // release
                    amp = o.relFrom *
                          (1.0f - ((o.rLen > 0) ? (float)o.stagePos / (float)o.rLen : 1.0f));
                    if (++o.stagePos >= o.rLen) { o.active = false; continue; }
                    break;
                }
                o.envCur = amp;                             // for a later note-off release
            }

            // ---- sample read: Catmull-Rom cubic with loop wrap ----
            const short* pool = bankPcm_ + o.sOff;
            const int   i0   = (int)(o.wpos >> 16);
            const float frac = (float)(o.wpos & 0xFFFF) * (1.0f / 65536.0f);
            const float sm1 = pool[wrapIdx(o, i0 - 1)];
            const float s0  = pool[wrapIdx(o, i0)];
            const float s1  = pool[wrapIdx(o, i0 + 1)];
            const float s2  = pool[wrapIdx(o, i0 + 2)];
            const float c0 = -0.5f * sm1 + 1.5f * s0 - 1.5f * s1 + 0.5f * s2;
            const float c1 = sm1 - 2.5f * s0 + 2.0f * s1 - 0.5f * s2;
            const float c2 = -0.5f * sm1 + 0.5f * s1;
            float interp = ((c0 * frac + c1) * frac + c2) * frac + s0;

            // per-voice low-pass biquad (Direct Form I)
            if (o.hasFilter) {
                const float y = o.fb0 * interp + o.fb1 * o.fx1 + o.fb2 * o.fx2
                                - o.fa1 * o.fy1 - o.fa2 * o.fy2;
                o.fx2 = o.fx1; o.fx1 = interp;
                o.fy2 = o.fy1; o.fy1 = y;
                interp = y;
            }
            accf += interp * amp * o.wgain * WAVE_MASTER;

            // ---- advance position, handle loop / end ----
            o.wpos += o.wstep;
            if (o.loop) {
                const unsigned long long leQ = (unsigned long long)o.sLe << 16;
                const unsigned long long lenQ =
                    (unsigned long long)((int)o.sLe - (int)o.sLs) << 16;
                while (lenQ > 0 && o.wpos >= leQ) {
                    o.wpos -= lenQ;
                }
            } else if ((unsigned int)(o.wpos >> 16) >= o.sLen) {
                o.active = false;
            }
            continue;
        }

        // ---- square/noise fallback voice ----
        if (o.env == 0) {                       // attack (melodic only)
            o.cur = (o.peak * o.envPos) / MIDI_ATTACK;
            if (++o.envPos >= MIDI_ATTACK) {
                o.env = 1;
                o.cur = o.peak;
            }
        } else if (o.env == 1) {                // sustain (melodic only)
            o.cur = o.peak;
        } else {                                // release / percussive decay
            o.cur = (o.relStart * (o.relLen - o.envPos)) / o.relLen;
            if (++o.envPos >= o.relLen) {
                o.active = false;
                continue;
            }
        }
        int sv;
        if (o.drum == 1) {                      // noise (snare / hat / cymbal)
            o.lfsr ^= o.lfsr << 13;
            o.lfsr ^= o.lfsr >> 17;
            o.lfsr ^= o.lfsr << 5;
            sv = (o.lfsr & 0x8000u) ? o.cur : -o.cur;
        } else {                                // square wave (melodic / kick)
            sv = ((o.phase >> 15) & 1) ? o.cur : -o.cur;
            o.phase += o.phaseInc;
        }
        accf += (float)sv;
    }
    int acc = (int)accf;
    if (acc > 32767) {
        acc = 32767;
    } else if (acc < -32768) {
        acc = -32768;
    }

    ++midiCurSample_;
    if (midiCurSample_ >= midiSongSamples_ && midiEventIdx_ >= midiCount_) {
        bool anyActive = false;
        for (int i = 0; i < POLY; ++i) {
            if (midiOsc_[i].active) {
                anyActive = true;
                break;
            }
        }
        if (!anyActive) {
            *ended = true;
        }
    }
    return acc;
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

void Ps2Audio::setVolume(int id, int vol) {
    if (!mixerStarted_ || id <= 0) {
        return;
    }
    if (vol < 0) {
        vol = 0;
    } else if (vol > 255) {
        vol = 255;
    }
    WaitSema(voiceSema_);
    for (int v = 0; v < VOICES; ++v) {
        if (voices_[v].active && voices_[v].id == id) {
            voices_[v].vol = vol;
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

void Ps2Audio::pauseMixer() {
    // Silence any game voice still sounding (e.g. the level music the player left playing)
    // so the mix immediately goes quiet -- called the instant the VM exits, before the
    // memory-card save, so the save runs in silence. The mixer thread then mutes the
    // streaming input and stops feeding the ring (see mixerRun). The ADPCM BGM is a
    // separate hardware voice and is untouched.
    if (mixerStarted_) {
        WaitSema(voiceSema_);
        for (int v = 0; v < VOICES; ++v) {
            voices_[v].active = false;
        }
        SignalSema(voiceSema_);
    }
    mixerPaused_ = true;
}
void Ps2Audio::resumeMixer() { mixerPaused_ = false; }

} // namespace platform
} // namespace ps2
