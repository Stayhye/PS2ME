// PS2 JavaCall port — platform layer. Ps2Framebuffer implementation + registration.
#include "Ps2Framebuffer.hpp"
#include "GsDisplay.hpp"
#include "Ps2Frontend.hpp"
#include "../hal/LcdDevice.hpp"

#include <malloc.h>   // memalign / free (128-byte aligned for DMA)

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

void Ps2Framebuffer::present() {
    if (raster_ == 0 || gsBuf_ == 0) {
        return;
    }
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
    for (size_t i = 0; i < count; ++i) {
        const unsigned p = raster_[i];
        const unsigned r = (p >> 11) & 0x1F;
        const unsigned g = (p >> 5)  & 0x3F;
        const unsigned b =  p        & 0x1F;
        gsBuf_[i] = static_cast<javacall_pixel>(r | ((g >> 1) << 5) | (b << 10) | 0x8000);
    }

    gs.present(reinterpret_cast<const u16*>(gsBuf_), width_, height_);
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
