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
} // namespace

Ps2Audio& Ps2Audio::instance() {
    static Ps2Audio inst;
    return inst;
}

Ps2Audio::Ps2Audio()
    : ready_(false), mixerStarted_(false), mixerId_(0), mixerStack_(0) {}

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
    // Keep the audsrv ring topped up with silence. audsrv_play_audio() is non-blocking
    // (it queues only what currently fits and returns), so we hold the SIF lock for just
    // that quick RPC -- never across a blocking wait -- then sleep to pace the next feed.
    // The ring is ~106 ms; feeding ~2 KB every 20 ms keeps it comfortably full without
    // starving the controller reads that share the lock. Phase 2 mixes queued game
    // sounds into this buffer instead of pure silence.
    static short silence[1024];   // 2 KB, BSS-zeroed
    for (;;) {
        {
            SifGuard guard;
            audsrv_play_audio((const char*)silence, (int)sizeof(silence));
        }
        DelayThread(20000);   // 20 ms
    }
}

void Ps2Audio::startMixer() {
    if (!ready_ || mixerStarted_) {
        return;
    }
    mixerStack_ = memalign(16, MIXER_STACK);
    if (mixerStack_ == 0) {
        return;
    }

    // Run below the main thread (higher priority number == lower priority), like the
    // icon worker, so game/UI code is never preempted by the ring feed.
    ee_thread_status_t ts;
    ReferThreadStatus(GetThreadId(), &ts);
    int prio = ts.current_priority + 8;
    if (prio > 126) {
        prio = 126;
    }

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
