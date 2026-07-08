// PS2 JavaCall port — platform layer.
//
// Ps2Framebuffer: the presentation backend for LcdDevice. It owns two 16-bit
// rasters — raster_ in RGB565 (the buffer handed to the MIDP software rasterizer,
// which get_screen mandates as RGB565) and gsBuf_ in RGBA5551 (the GS-native CT16
// format). Each flush converts the current width_ x height_ region RGB565 -> RGBA5551
// and hands gsBuf_ to GsDisplay, which uploads it and draws the aspect-preserved sprite.
// Both buffers are 128-byte aligned because gsBuf_ is DMA'd to the GS.
//
// The backing store is allocated once at the MAX supported size and never reallocated:
// map() only records the current (per-game) logical size. This keeps raster_ stable so
// the MIDP screen buffer (gxj_system_screen_buffer.pixelData, which points straight into
// raster_) never dangles when the launcher changes a game's resolution.
#ifndef PS2_JAVACALL_PLATFORM_PS2FRAMEBUFFER_HPP
#define PS2_JAVACALL_PLATFORM_PS2FRAMEBUFFER_HPP

#include "../hal/IFramebuffer.hpp"

namespace ps2 {
namespace platform {

class Ps2Framebuffer : public hal::IFramebuffer {
public:
    // Max logical screen the pipeline supports; the backing store and the GS game texture
    // are sized to this. Covers every launcher preset (each dimension <= 512).
    static const int MAX_W = 512;
    static const int MAX_H = 512;

    Ps2Framebuffer() : raster_(0), gsBuf_(0), width_(0), height_(0) {}
    virtual ~Ps2Framebuffer() { unmap(); }

    virtual javacall_pixel* map(int width, int height);
    virtual void unmap();
    virtual void present();
    virtual void presentRegion(int ystart, int yend);

private:
    javacall_pixel* raster_;   // RGB565, filled by the MIDP rasterizer (MAX_W x MAX_H)
    javacall_pixel* gsBuf_;    // RGBA5551, uploaded to the GS (MAX_W x MAX_H)
    int             width_;    // current logical width (<= MAX_W)
    int             height_;   // current logical height (<= MAX_H)
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2FRAMEBUFFER_HPP
