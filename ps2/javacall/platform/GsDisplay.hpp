// PS2 JavaCall port — platform layer.
//
// GsDisplay: the PS2 "video card" behind the LCD path. It owns the Graphics
// Synthesizer setup (a 32-bit TV framebuffer + a 16-bit VRAM texture holding the
// virtual phone screen) and the libdraw/DMA pipeline that uploads an RGBA5551
// raster and draws it as an aspect-preserved, pillarboxed sprite on the TV. This
// is the only class the LCD path uses that touches ps2sdk.
//
// Reuses the Milestone-0 video path (src/main.c): graph_initialize + a GS_PSM_16
// texture + draw_texture_transfer + draw_rect_textured, NEAREST-filtered. Single
// buffered for now (the tear-free flip is roadmap M1.5).
#ifndef PS2_JAVACALL_PLATFORM_GSDISPLAY_HPP
#define PS2_JAVACALL_PLATFORM_GSDISPLAY_HPP

#include <tamtypes.h>
#include <draw.h>      // framebuffer_t / zbuffer_t / texbuffer_t / lod_t / clutbuffer_t
#include <draw2d.h>    // texrect_t
#include <packet.h>    // packet_t

namespace ps2 {
namespace platform {

class GsDisplay {
public:
    GsDisplay() : ready_(false), lcdW_(0), lcdH_(0), xfer_(0), draw_(0) {}
    ~GsDisplay();

    /// One-time GS/DMA/VRAM bring-up for an @p lcdW x @p lcdH source screen. Safe to
    /// call repeatedly; only the first successful call does work. Returns false if
    /// VRAM allocation or packet setup fails.
    bool init(int lcdW, int lcdH);

    bool ready() const { return ready_; }

    /// Upload @p rgba5551 (lcdW x lcdH, 128-byte aligned) to VRAM and draw it as the
    /// pillarboxed sprite, then wait for vsync. No-op until init() has succeeded.
    void present(const u16* rgba5551);

private:
    GsDisplay(const GsDisplay&);
    GsDisplay& operator=(const GsDisplay&);

    bool ready_;
    int  lcdW_;
    int  lcdH_;

    framebuffer_t frame_;
    zbuffer_t     z_;
    texbuffer_t   texbuf_;
    lod_t         lod_;
    clutbuffer_t  clut_;
    texrect_t     rect_;

    packet_t* xfer_;   // texture-upload DMA chain
    packet_t* draw_;   // draw packet (clear + sprite)
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_GSDISPLAY_HPP
