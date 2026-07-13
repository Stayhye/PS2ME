// PS2 JavaCall port — platform layer. Ps2Framebuffer implementation + registration.
#include "Ps2Framebuffer.hpp"
#include "GsDisplay.hpp"
#include "Ps2Frontend.hpp"
#include "Settings.hpp"              // opt-in in-game FPS overlay
#include "../hal/LcdDevice.hpp"
#include "../hal/SystemClock.hpp"    // per-second FPS measurement

#include <malloc.h>   // memalign / free (128-byte aligned for DMA)

#ifdef PS2ME_PROFILE_FRAME
#include "../hal/Logger.hpp"         // A/B/C frame-profiler dump sink (PASSO 0a)
#include <cstdio>                    // snprintf for the profiler line
#endif

namespace ps2 {
namespace platform {

javacall_pixel* Ps2Framebuffer::map(int width, int height) {
    // Clamp the logical size to what the pipeline supports.
    if (width  > MAX_W) width  = MAX_W;
    if (height > MAX_H) height = MAX_H;
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    if (raster_ == 0) {
        // Allocate the backing store once at the max size and never reallocate: the MIDP
        // screen buffer points straight into raster_, so the pointer must stay stable
        // across per-game resolution changes. 128-byte aligned (gsBuf_ is DMA'd; match).
        const size_t count = static_cast<size_t>(MAX_W) * MAX_H;
        const size_t bytes = count * sizeof(javacall_pixel);
        raster_ = static_cast<javacall_pixel*>(memalign(128, bytes));
        gsBuf_  = static_cast<javacall_pixel*>(memalign(128, bytes));
        if (raster_ == 0 || gsBuf_ == 0) {
            unmap();
            return 0;
        }
        for (size_t i = 0; i < count; ++i) {
            raster_[i] = 0;
            gsBuf_[i]  = 0x8000;    // RGBA5551 black with alpha set
        }
    } else if (width != width_ || height != height_) {
        // Resolution change: clear the (stable) backing store so a smaller game does not
        // show the previous game's leftovers around its image.
        const size_t count = static_cast<size_t>(MAX_W) * MAX_H;
        for (size_t i = 0; i < count; ++i) {
            raster_[i] = 0;
        }
    }
    width_  = width;
    height_ = height;
    return raster_;
}

void Ps2Framebuffer::unmap() {
    if (raster_ != 0) { free(raster_); raster_ = 0; }
    if (gsBuf_  != 0) { free(gsBuf_);  gsBuf_  = 0; }
    width_  = 0;
    height_ = 0;
}

namespace {

// FPS state: updated once per presented frame on the single VM thread, so no lock needed.
// The overlay itself is drawn by GsDisplay (on the letterboxed screen, OUTSIDE the game
// canvas); here we only measure and hand the value over.
long long g_fpsLastMs = 0;
int       g_fpsFrames = 0;
int       g_fpsValue  = 0;

// Count a game frame and recompute the FPS once a second has elapsed.
void fpsTick() {
    ++g_fpsFrames;
    const long long now = ps2::hal::SystemClock::instance().elapsedMillis();
    if (g_fpsLastMs == 0) { g_fpsLastMs = now; return; }
    const long long dt = now - g_fpsLastMs;
    if (dt >= 1000) {
        g_fpsValue  = (int)((g_fpsFrames * 1000LL) / dt);
        g_fpsFrames = 0;
        g_fpsLastMs = now;
    }
}

#ifdef PS2ME_PROFILE_FRAME
// ---- PASSO 0a frame profiler (opt-in, compile-time; production is byte-identical) -----
// Splits each game frame into (A+B) interpret+draw vs (C) present, and further splits (C)
// into the RGB565->RGBA5551 convert loop and the GS upload (which carries the 2 DMA waits
// in GsDisplay::blit). NOTE: (B) native drawing is inlined into interpretation -- the gxj
// rasterizer writes straight into raster_ (jcapp aliases gxj_system_screen_buffer.pixelData
// to javacall_lcd_get_screen), so there is NO backbuffer/blit boundary to bracket here and
// (A) interpret and (B) draw cannot be separated without instrumenting gxj (PASSO 0b). This
// pass reports (A+B) lumped. Clock: SystemClock monotonic counter (PS2 = EE bus timer,
// kBUSCLK ~= 6.8 ns/tick). Dumped ~1x/s via the Logger sink (PCSX2 console / EE TTY).
//
// Timeline per frame:  [present N-1: conv+upload] -> [interpret+draw = A+B] -> [present N]
// so at present N entry:  (A+B) = (now - lastEntry) - C(previous present).
namespace prof {
    typedef javacall_int64 i64;
    i64 lastEntry = 0;      // monotonic counter at the previous present() entry
    i64 prevConv  = 0;      // convert-loop ticks measured in the previous present()
    i64 prevUp    = 0;      // GS-upload ticks measured in the previous present()
    // native-draw ticks the gxj hooks accumulate in the CURRENT frame, by category:
    // [0]=fill(fillRect) [1]=blit(drawImage/Region) [2]=other(line/tri/copyArea) [3]=pixel(drawRGB/getRGB)
    i64 drawTicks[4] = { 0, 0, 0, 0 };
    // poll/idle ticks in the CURRENT frame, split by source -- NOT computing bytecode:
    // [0]=event_receive (event wait/poll) [1]=Thread.sleep (time-based pacing). Splitting the
    // two answers whether idle time is an evitable event wait or unrecoverable pacing sleep.
    i64 pollTicks[2] = { 0, 0 };
    i64 accFrame = 0, accComp = 0, accConv = 0, accUp = 0;   // ~1 s window accumulators
    i64 accPoll[2] = { 0, 0 };   // [0]=event_receive [1]=Thread.sleep
    // event_receive diagnostic: sum of REQUESTED wait (ms) + number of calls, to compare
    // what the VM asked to wait against ev (actual busy-spin) -- exposes overshoot vs genuine idle.
    i64 reqMsFrame = 0; int evCallsFrame = 0;
    i64 accReqMs = 0; i64 accEvCalls = 0;
    i64 accB[4] = { 0, 0, 0, 0 };
    int frames = 0;

    inline i64 now() { return ps2::hal::SystemClock::instance().monotonicCounter(); }

    void dump() {
        const i64 freq = ps2::hal::SystemClock::instance().monotonicFrequency();
        if (frames <= 0 || freq <= 0) { return; }
        // Per-frame averages in microseconds (integer math; no float/FPU on the EE path).
        const i64 US = 1000000;
        const i64 fr = accFrame * US / freq / frames;
        const i64 cp = accComp  * US / freq / frames;   // compute: dispatch + GC + KNI (A minus poll)
        const i64 pe = accPoll[0] * US / freq / frames;   // poll/idle: event_receive (event wait)
        const i64 ps = accPoll[1] * US / freq / frames;   // poll/idle: Thread.sleep (pacing)
        const i64 pl = pe + ps;                           // total poll/idle
        const i64 bf = accB[0]  * US / freq / frames;   // fill
        const i64 bl = accB[1]  * US / freq / frames;   // blit
        const i64 bo = accB[2]  * US / freq / frames;   // other (line/tri/copyArea)
        const i64 bp = accB[3]  * US / freq / frames;   // pixel (drawRGB/getRGB)
        const i64 bt = bf + bl + bo + bp;
        const i64 cv = accConv  * US / freq / frames;
        const i64 up = accUp    * US / freq / frames;
        const i64 rq = accReqMs * 1000 / frames;   // avg REQUESTED event wait (us/frame)
        const i64 en = accEvCalls;                 // event_receive calls in this ~1 s window
        const i64 den = fr > 0 ? fr : 1;
        // comp = interpret compute (dispatch+GC+KNI). poll = idle, split ev=event_receive
        // sl=Thread.sleep pacing. B split: fl=fillRect bl=drawImage ot=line/tri/copy px=drawRGB/getRGB.
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "[PROF] FPS=%d fr=%lldus | comp=%lldus(%lld%%) poll=%lldus(%lld%%)[ev=%lld sl=%lld req=%lld n=%lld] B=%lldus(%lld%%)[fl=%lld bl=%lld ot=%lld px=%lld] conv=%lldus up=%lldus\n",
            frames,
            (long long)fr,
            (long long)cp, (long long)(cp * 100 / den),
            (long long)pl, (long long)(pl * 100 / den),
            (long long)pe, (long long)ps, (long long)rq, (long long)en,
            (long long)bt, (long long)(bt * 100 / den),
            (long long)bf, (long long)bl, (long long)bo, (long long)bp,
            (long long)cv, (long long)up);
        ps2::hal::Logger::instance().print(buf);
        accFrame = accComp = accConv = accUp = 0;
        accPoll[0] = accPoll[1] = 0;
        accReqMs = accEvCalls = 0;
        accB[0] = accB[1] = accB[2] = accB[3] = 0;
        frames = 0;
    }

    // Called at the very top of every present(), before any early return, so each presented
    // frame is counted exactly once regardless of which present path it takes.
    void onEntry() {
        // One-shot liveness marker: if THIS prints but the periodic dumps do not, the flag
        // is compiled in and the bug is in the accumulator; if it never prints, the profiler
        // was compiled out (PS2ME_PROFILE_FRAME not set at build time).
        if (lastEntry == 0) {
            ps2::hal::Logger::instance().print("[PROF] frame profiler active (dumps ~1x/s)\n");
        }
        const i64 t = now();
        // Snapshot + reset this frame's per-category native-draw ticks and poll/idle ticks.
        const i64 b0 = drawTicks[0], b1 = drawTicks[1], b2 = drawTicks[2], b3 = drawTicks[3];
        drawTicks[0] = drawTicks[1] = drawTicks[2] = drawTicks[3] = 0;
        const i64 pe = pollTicks[0], ps = pollTicks[1]; pollTicks[0] = pollTicks[1] = 0;
        const i64 poll = pe + ps;
        const i64 rq = reqMsFrame; reqMsFrame = 0;
        const int ec = evCallsFrame; evCallsFrame = 0;
        if (lastEntry != 0) {
            const i64 frame = t - lastEntry;
            const i64 tb = b0 + b1 + b2 + b3;
            i64 comp = frame - prevConv - prevUp - tb - poll;   // compute = A minus poll/idle
            if (comp < 0) { comp = 0; }                         // clamp measurement jitter
            accFrame += frame; accComp += comp;
            accPoll[0] += pe; accPoll[1] += ps;
            accReqMs += rq; accEvCalls += ec;
            accB[0] += b0; accB[1] += b1; accB[2] += b2; accB[3] += b3;
            accConv += prevConv; accUp += prevUp; ++frames;
            const i64 freq = ps2::hal::SystemClock::instance().monotonicFrequency();
            if (accFrame >= freq) { dump(); }   // ~1 s of frames accumulated
        }
        lastEntry = t;
        prevConv = 0; prevUp = 0;   // default 0: an early-return frame performs no convert/upload
    }
} // namespace prof
#endif // PS2ME_PROFILE_FRAME

} // namespace

#ifdef PS2ME_PROFILE_FRAME
// gxj (MIDP C, in libjvm.a) calls these to attribute native-draw time to (B). Defined in our
// layer so the single final --start-group link resolves them; the accumulator lives in the
// file-local prof namespace above. C linkage so the C gxj call sites match (no name mangling).
extern "C" long long ps2me_prof_now(void) {
    return (long long)ps2::hal::SystemClock::instance().monotonicCounter();
}
extern "C" void ps2me_prof_draw_add(long long startTick, int cat) {
    if (cat < 0 || cat > 3) { return; }
    prof::drawTicks[cat] += (long long)ps2::hal::SystemClock::instance().monotonicCounter() - startTick;
}
// Called by javacall_event_receive (which=0) / javacall_time_sleep (which=1) in our contract
// layer to attribute idle time to (poll), split by source so event-wait and Thread.sleep
// pacing are separable -- the deciding measure for whether the idle is evitable.
extern "C" void ps2me_prof_poll_add(long long startTick, int which) {
    if (which < 0 || which > 1) { return; }
    prof::pollTicks[which] += (long long)ps2::hal::SystemClock::instance().monotonicCounter() - startTick;
}
// Called by javacall_event_receive with the timeout the VM REQUESTED (ms). Compared against
// ev (actual busy-spin) it separates overshoot (fixable) from genuine idle (pacing/deadend).
extern "C" void ps2me_prof_evreq_add(long timeoutMs) {
    prof::reqMsFrame += (timeoutMs > 0 ? (long long)timeoutMs : 0);
    prof::evCallsFrame += 1;
}
#endif // PS2ME_PROFILE_FRAME

void Ps2Framebuffer::present() {
    if (raster_ == 0 || gsBuf_ == 0) {
        return;
    }
    // Count every presented game frame for the FPS metric (measured even in debug view).
    fpsTick();
#ifdef PS2ME_PROFILE_FRAME
    prof::onEntry();   // close out the previous frame's (A+B); start this frame's clock
#endif
    // Debug mode: the front-end composites a split view (game on the left, the full app
    // log on the right) and presents it itself -- we are done for this frame.
    if (Ps2Frontend::instance().debugPresent(
            reinterpret_cast<const unsigned short*>(raster_), width_, height_)) {
        return;
    }

    // The game is now drawing its own frames: stop the native launch-trace overlay so
    // it no longer clobbers the game's screen (no-op if it was never enabled).
    Ps2Frontend::instance().logEnable(false);

    // Bring up the shared GS on first use (idempotent: the native front-end may have
    // already initialized it before the VM started).
    GsDisplay& gs = GsDisplay::instance();
    if (!gs.ready() && !gs.init()) {
        return;
    }

    // Convert the MIDP RGB565 raster to the GS-native RGBA5551 (CT16): R and B swap
    // ends (RGB565 has R in the high bits, CT16 has R in the low bits), G drops its
    // least-significant bit (6 -> 5), and alpha is forced set.
    const size_t count = static_cast<size_t>(width_) * height_;
#ifdef PS2ME_PROFILE_FRAME
    const javacall_int64 _cvStart = prof::now();   // (C-convert) span begin
#endif
    // SWAR fast path: convert 4 px per iteration using the r5900's native 64-bit ld/sd.
    // Both buffers are memalign(128), so at any index that is a multiple of 4 the 8-byte
    // access is 8-aligned -- no EE unaligned-store trap, no peel needed. Per-lane masks
    // isolate each 16-bit pixel's bits BEFORE the shift, so shifted bits stay inside their
    // own lane (no cross-lane bleed; the bits that a 64-bit shift would carry across a lane
    // boundary are masked to zero). Mapping matches the scalar tail below exactly:
    //   R[15:11] >> 11 -> [4:0]   G[10:6] >> 1 -> [9:5] (drops green LSB)
    //   B[4:0]  << 10 -> [14:10]  A -> bit 15
    const size_t vwords = (count & ~static_cast<size_t>(3)) >> 2;   // # of 4-px 64-bit words
    const unsigned long long* src64 = reinterpret_cast<const unsigned long long*>(raster_);
    unsigned long long*       dst64 = reinterpret_cast<unsigned long long*>(gsBuf_);
    for (size_t v = 0; v < vwords; ++v) {
        const unsigned long long w = src64[v];
        dst64[v] = 0x8000800080008000ULL
                 | ((w & 0xF800F800F800F800ULL) >> 11)
                 | ((w & 0x07C007C007C007C0ULL) >> 1)
                 | ((w & 0x001F001F001F001FULL) << 10);
    }
    // Scalar tail for the remaining (count % 4) pixels.
    for (size_t i = vwords << 2; i < count; ++i) {
        const unsigned p = raster_[i];
        const unsigned r = (p >> 11) & 0x1F;
        const unsigned g = (p >> 5)  & 0x3F;
        const unsigned b =  p        & 0x1F;
        gsBuf_[i] = static_cast<javacall_pixel>(r | ((g >> 1) << 5) | (b << 10) | 0x8000);
    }
#ifdef PS2ME_PROFILE_FRAME
    prof::prevConv = prof::now() - _cvStart;
#endif

    // Opt-in performance metric (Settings -> FPS counter, default off): hand the frame rate
    // to GsDisplay, which draws it on the letterbox border OUTSIDE the game canvas.
    gs.setFpsOverlay(Settings::instance().fpsCounter(), g_fpsValue);

#ifdef PS2ME_PROFILE_FRAME
    const javacall_int64 _upStart = prof::now();   // (C-upload) span begin: GS upload + 2 DMA waits live in blit()
    gs.present(reinterpret_cast<const u16*>(gsBuf_), width_, height_);
    prof::prevUp = prof::now() - _upStart;
#else
    gs.present(reinterpret_cast<const u16*>(gsBuf_), width_, height_);
#endif
}

void Ps2Framebuffer::presentRegion(int /*ystart*/, int /*yend*/) {
    // B2 first cut: a partial flush repaints the whole frame. Band-limited upload
    // (only [ystart, yend]) is a later optimization.
    present();
}

namespace {
// Single backend instance, installed into LcdDevice at program startup, before
// the VM calls javacall_lcd_init. Mirrors the StdoutSink/Ps2AlarmTimer registrars.
Ps2Framebuffer g_ps2Framebuffer;

struct Ps2FramebufferRegistrar {
    Ps2FramebufferRegistrar() {
        hal::LcdDevice::instance().setFramebuffer(&g_ps2Framebuffer);
    }
};
Ps2FramebufferRegistrar g_ps2FramebufferRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
