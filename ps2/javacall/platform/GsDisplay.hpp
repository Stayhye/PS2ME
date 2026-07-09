// PS2 JavaCall port — platform layer.
//
// GsDisplay: the PS2 "video card" behind the LCD path. It owns the Graphics
// Synthesizer setup (a 32-bit TV framebuffer + a 16-bit VRAM texture holding the
// virtual phone screen) and the libdraw/DMA pipeline that uploads an RGBA5551
// raster and draws it as an aspect-preserved, pillarboxed sprite on the TV. This
// is the only class the LCD path uses that touches ps2sdk.
//
// Reuses the Milestone-0 video path (src/main.c): graph_initialize + a GS_PSM_16
// texture + draw_texture_transfer + draw_rect_textured, NEAREST-filtered. Double
// buffered: each present draws into the off-screen back buffer and flips it to the
// CRTC on vblank, so the TV never scans out a half-drawn frame (no tearing).
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

    /// Show @p rgba5551 (@p w x @p h, 128-byte aligned) full-screen as a boot splash with
    /// an ease-in-out fade IN and OUT, then return (blocks ~1.5s). Uses a DEDICATED VRAM
    /// texture that is freed before returning, so the splash costs no VRAM once the menu
    /// comes up. No-op until init() has succeeded.
    void showSplash(const u16* rgba5551, int w, int h);

    /// Set the in-game FPS overlay: when @p show is true, present() draws "<fps> FPS" on
    /// the pillarbox/letterbox border, OUTSIDE the game canvas (a small vector-drawn label,
    /// not part of the game texture). Off by default; the launcher toggles it via Settings.
    void setFpsOverlay(bool show, int fps) { fpsShow_ = show; fpsValue_ = fps; }

private:
    GsDisplay()
        : ready_(false), gameTexReady_(false), fsTexReady_(false),
          drawIdx_(0), fpsShow_(false), fpsValue_(0), xfer_(0), draw_(0) {}
    GsDisplay(const GsDisplay&);
    GsDisplay& operator=(const GsDisplay&);

    /// Upload @p raster into @p tb, draw @p rect over a cleared back buffer, then flip
    /// that buffer to the CRTC on vblank. Shared by both present paths. When @p overlay is
    /// true and the FPS overlay is enabled, the FPS label is drawn over the border.
    void blit(const u16* raster, int w, int h, texbuffer_t* tb, texrect_t* rect, bool overlay);

    bool ready_;         // framebuffer + environment up
    bool gameTexReady_;  // pillarbox game texture allocated
    bool fsTexReady_;    // fullscreen UI texture allocated

    int  drawIdx_;       // back-buffer index (the one we draw into this frame)
    bool fpsShow_;       // draw the in-game FPS overlay this frame
    int  fpsValue_;      // most recent measured FPS (drawn when fpsShow_)
    framebuffer_t frame_[2];  // double-buffered TV framebuffers (draw back, show front)
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
