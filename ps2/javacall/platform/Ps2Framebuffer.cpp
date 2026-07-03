// PS2 JavaCall port — platform layer. Ps2Framebuffer implementation + registration.
#include "Ps2Framebuffer.hpp"
#include "../hal/LcdDevice.hpp"

#include <malloc.h>   // memalign / free (128-byte aligned for DMA)

namespace ps2 {
namespace platform {

javacall_pixel* Ps2Framebuffer::map(int width, int height) {
    if (raster_ != 0 && width == width_ && height == height_) {
        return raster_;             // idempotent for the same geometry
    }
    unmap();
    const size_t count = static_cast<size_t>(width) * height;
    const size_t bytes = count * sizeof(javacall_pixel);
    // 128-byte aligned: gsBuf_ is DMA'd to the GS; align raster_ the same way.
    raster_ = static_cast<javacall_pixel*>(memalign(128, bytes));
    gsBuf_  = static_cast<javacall_pixel*>(memalign(128, bytes));
    if (raster_ == 0 || gsBuf_ == 0) {
        unmap();
        return 0;
    }
    // Define the initial frames: black raster, opaque-black GS buffer.
    for (size_t i = 0; i < count; ++i) {
        raster_[i] = 0;
        gsBuf_[i]  = 0x8000;        // RGBA5551 black with alpha set
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
    // Bring up the GS on first use (on the VM thread, once the display exists).
    if (!gs_.ready() && !gs_.init(width_, height_)) {
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

    gs_.present(reinterpret_cast<const u16*>(gsBuf_));
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
