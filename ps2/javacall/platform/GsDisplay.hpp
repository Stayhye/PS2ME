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
    /// Process-wide instance. One GS/VRAM bring-up shared by the native front-end
    /// (Ps2Frontend) and the MIDP framebuffer (Ps2Framebuffer), so graph_initialize
    /// and the VRAM allocations happen exactly once regardless of who presents first.
    static GsDisplay& instance();

    ~GsDisplay();

    /// One-time GS/DMA/VRAM bring-up (the 640x448 TV framebuffer + draw environment
    /// + DMA packets, shared by both present paths). Safe to call repeatedly; only the
    /// first successful call does work. Returns false on VRAM/packet setup failure.
    bool init();

    bool ready() const { return ready_; }

    /// Upload @p rgba5551 (@p w x @p h, 128-byte aligned) and draw it as an
    /// aspect-preserved, pillarboxed sprite -- the path the MIDP games use (portrait
    /// 240x320 on the TV). No-op until init() has succeeded. The game texture is
    /// allocated on the first call.
    void present(const u16* rgba5551, int w, int h);

    /// Upload @p rgba5551 (@p w x @p h, 128-byte aligned) and draw it stretched to
    /// FILL the whole native TV screen (no pillarbox) -- the path the native
    /// front-end uses. No-op until init() has succeeded. The fullscreen texture is
    /// allocated on the first call.
    void presentFullscreen(const u16* rgba5551, int w, int h);

private:
    GsDisplay()
        : ready_(false), gameTexReady_(false), fsTexReady_(false),
          xfer_(0), draw_(0) {}
    GsDisplay(const GsDisplay&);
    GsDisplay& operator=(const GsDisplay&);

    bool ready_;         // framebuffer + environment up
    bool gameTexReady_;  // pillarbox game texture allocated
    bool fsTexReady_;    // fullscreen UI texture allocated

    framebuffer_t frame_;
    zbuffer_t     z_;
    lod_t         lod_;    // NEAREST sampling, shared by both textures
    clutbuffer_t  clut_;   // no CLUT, shared

    texbuffer_t   texbuf_;   // pillarbox game texture (256x512)
    texrect_t     rect_;     // pillarbox destination rect

    texbuffer_t   fsTexbuf_; // fullscreen UI texture (1024x512)
    texrect_t     fsRect_;   // fullscreen destination rect (whole TV)

    packet_t* xfer_;   // texture-upload DMA chain
    packet_t* draw_;   // draw packet (clear + sprite)
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_GSDISPLAY_HPP
