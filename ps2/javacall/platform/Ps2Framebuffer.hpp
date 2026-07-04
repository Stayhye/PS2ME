// PS2 JavaCall port — platform layer.
//
// Ps2Framebuffer: the presentation backend for LcdDevice. It owns two 16-bit
// rasters — raster_ in RGB565 (the buffer handed to the MIDP software rasterizer,
// which get_screen mandates as RGB565) and gsBuf_ in RGBA5551 (the GS-native CT16
// format). Each flush converts RGB565 -> RGBA5551 and hands gsBuf_ to GsDisplay,
// which uploads it and draws the pillarboxed sprite. Both buffers are 128-byte
// aligned because gsBuf_ is DMA'd to the GS.
#ifndef PS2_JAVACALL_PLATFORM_PS2FRAMEBUFFER_HPP
#define PS2_JAVACALL_PLATFORM_PS2FRAMEBUFFER_HPP

#include "../hal/IFramebuffer.hpp"

namespace ps2 {
namespace platform {

class Ps2Framebuffer : public hal::IFramebuffer {
public:
    Ps2Framebuffer() : raster_(0), gsBuf_(0), width_(0), height_(0) {}
    virtual ~Ps2Framebuffer() { unmap(); }

    virtual javacall_pixel* map(int width, int height);
    virtual void unmap();
    virtual void present();
    virtual void presentRegion(int ystart, int yend);

private:
    javacall_pixel* raster_;   // RGB565, filled by the MIDP rasterizer
    javacall_pixel* gsBuf_;    // RGBA5551, uploaded to the GS
    int             width_;
    int             height_;
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2FRAMEBUFFER_HPP
