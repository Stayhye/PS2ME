// PS2 JavaCall port — platform layer. GsDisplay implementation.
//
// Owns the GS setup shared by everything on screen: one 640x448 32-bit TV
// framebuffer + a 2D draw environment, brought up once by init(). Two present paths
// share it, each with its own VRAM texture (allocated lazily on first use):
//   - present()           uploads a small game raster and draws it as a portrait,
//                         aspect-preserved, PILLARBOXED sprite (the MIDP games).
//   - presentFullscreen() uploads a native-resolution UI raster and draws it FILLING
//                         the whole TV (the native front-end menu).
// Ported from the Milestone-0 video path (src/main.c).
#include "GsDisplay.hpp"

#include <kernel.h>     // SyncDCache
#include <dma.h>
#include <graph.h>
#include <gs_psm.h>

namespace ps2 {
namespace platform {

namespace {
// TV output. NTSC 640x448; displays fine on PAL consoles too (PAL could use 512).
const int SCR_W = 640;
const int SCR_H = 448;
// Power-of-two VRAM texture for the aspect-fit game screen. 512x512 covers every launcher
// resolution preset (each dimension <= 512), portrait or landscape.
const int GAME_TEX_W = 512;
const int GAME_TEX_H = 512;
// Power-of-two VRAM texture for the fullscreen UI (holds the 640x448 native raster).
const int FS_TEX_W = 1024;
const int FS_TEX_H = 512;
// Power-of-two VRAM texture for the boot splash (also holds the 640x448 raster). Only
// alive during the splash: allocated in showSplash() and freed before it returns.
const int SPLASH_TEX_W = 1024;
const int SPLASH_TEX_H = 512;

// Ease-in-out on [0,1]: the classic smoothstep 3t^2 - 2t^3 (zero slope at both ends).
float easeInOut(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Fill in the NEAREST-sampling, no-CLUT descriptors both textures use.
void initSampling(lod_t* lod, clutbuffer_t* clut) {
    lod->calculation = LOD_USE_K;
    lod->max_level   = 0;
    lod->mag_filter  = LOD_MAG_NEAREST;
    lod->min_filter  = LOD_MIN_NEAREST;
    lod->l           = 0;
    lod->k           = 0;

    clut->storage_mode = CLUT_STORAGE_MODE1;
    clut->start        = 0;
    clut->psm          = 0;
    clut->load_method  = CLUT_NO_LOAD;
    clut->address      = 0;
}

// Configure a 16-bit VRAM texture buffer of the given power-of-two size.
void initTexbuf(texbuffer_t* tb, int w, int h) {
    tb->width          = w;
    tb->psm            = GS_PSM_16;
    tb->address        = graph_vram_allocate(w, h, GS_PSM_16, GRAPH_ALIGN_BLOCK);
    tb->info.width     = draw_log2(w);
    tb->info.height    = draw_log2(h);
    tb->info.components = TEXTURE_COMPONENTS_RGBA;
    tb->info.function   = TEXTURE_FUNCTION_MODULATE;
}

// Set a textured-rect destination from screen coords + source UV extent.
void setRect(texrect_t* r, float x0, float y0, float x1, float y1,
             float uw, float vh) {
    r->v0.x = x0;  r->v0.y = y0;  r->v0.z = 0;
    r->t0.u = 0.0f; r->t0.v = 0.0f;
    r->v1.x = x1;  r->v1.y = y1;  r->v1.z = 0;
    r->t1.u = uw;  r->t1.v = vh;
    r->color.r = 0x80; r->color.g = 0x80; r->color.b = 0x80;
    r->color.a = 0x80; r->color.q = 1.0f;
}

// --- FPS overlay ---------------------------------------------------------------------
// 5x7 bitmap font: digits 0-9 then F, P, S. Each row is 5 bits, bit 4 = leftmost column.
const unsigned char kFont5x7[13][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
};

// Emit one filled screen-space rectangle.
qword_t* fpsRect(qword_t* q, int ctx, float x0, float y0, float x1, float y1,
                 int r, int g, int b) {
    rect_t rc;
    rc.v0.x = x0; rc.v0.y = y0; rc.v0.z = 0;
    rc.v1.x = x1; rc.v1.y = y1; rc.v1.z = 0;
    rc.color.r = (u8)r; rc.color.g = (u8)g; rc.color.b = (u8)b;
    rc.color.a = 0x80;  rc.color.q = 1.0f;
    return draw_rect_filled(q, ctx, &rc);
}

// Draw "<fps> FPS" as filled rectangles in the TOP-RIGHT of the screen, over the border
// (pillarbox/letterbox) outside the game canvas. Glyph pixels are coalesced into horizontal
// runs to keep the rectangle (and thus qword) count low. Returns the advanced packet ptr.
qword_t* drawFpsRects(qword_t* q, int ctx, int fps, int scrW, int /*scrH*/) {
    const int scale = 2;
    const int gadv  = 6 * scale;   // per-glyph horizontal advance
    const int gh    = 7 * scale;

    int gl[8];
    int n = 0;
    int v = fps;
    if (v < 0)   v = 0;
    if (v > 999) v = 999;
    if (v >= 100) { gl[n++] = v / 100; gl[n++] = (v / 10) % 10; gl[n++] = v % 10; }
    else if (v >= 10) { gl[n++] = v / 10; gl[n++] = v % 10; }
    else { gl[n++] = v; }
    gl[n++] = -1;                          // space
    gl[n++] = 10; gl[n++] = 11; gl[n++] = 12;   // F P S

    const int textW  = n * gadv - scale;
    const int margin = 10;
    const int x0 = scrW - textW - margin;
    const int y0 = margin;

    // dark backdrop for legibility
    q = fpsRect(q, ctx, (float)(x0 - 5), (float)(y0 - 4),
                (float)(x0 + textW + 5), (float)(y0 + gh + 4), 0, 0, 0);

    // bright-green glyphs, one rectangle per horizontal run
    int gx = x0;
    for (int i = 0; i < n; ++i) {
        const int idx = gl[i];
        if (idx >= 0) {
            const unsigned char* f = kFont5x7[idx];
            for (int ry = 0; ry < 7; ++ry) {
                const unsigned bits = f[ry];
                int rx = 0;
                while (rx < 5) {
                    if ((bits >> (4 - rx)) & 1) {
                        int run = 1;
                        while (rx + run < 5 && ((bits >> (4 - (rx + run))) & 1)) ++run;
                        const float px = (float)(gx + rx * scale);
                        const float py = (float)(y0 + ry * scale);
                        q = fpsRect(q, ctx, px, py, px + run * scale, py + scale, 80, 255, 90);
                        rx += run;
                    } else {
                        ++rx;
                    }
                }
            }
        }
        gx += gadv;
    }
    return q;
}
} // namespace

GsDisplay& GsDisplay::instance() {
    static GsDisplay inst;
    return inst;
}

GsDisplay::~GsDisplay() {
    if (xfer_ != 0) packet_free(xfer_);
    if (draw_ != 0) packet_free(draw_);
}

bool GsDisplay::init() {
    if (ready_) {
        return true;
    }

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // --- GS framebuffers (double-buffered) ---
    // Two identical TV framebuffers for tear-free double buffering. Each frame draws
    // into the back buffer (frame_[drawIdx_]) while the CRTC scans out the front one;
    // the 2D draw environment (FRAME/XYOFFSET/SCISSOR) is emitted per-frame in blit()
    // targeting the current back buffer, so no one-time environment packet is needed.
    for (int i = 0; i < 2; ++i) {
        frame_[i].width   = SCR_W;
        frame_[i].height  = SCR_H;
        frame_[i].psm     = GS_PSM_32;
        frame_[i].mask    = 0;
        frame_[i].address = graph_vram_allocate(SCR_W, SCR_H, GS_PSM_32, GRAPH_ALIGN_PAGE);
    }

    z_.enable  = 0;               // no z-buffer for 2D
    z_.method  = ZTEST_METHOD_GREATER;
    z_.address = 0;
    z_.mask    = 1;
    z_.zsm     = 0;

    // Show buffer 0 first; draw into buffer 1 this frame, then flip. The draw target
    // (FRAME) is (re)set to the current back buffer per-frame in blit().
    graph_initialize(frame_[0].address, SCR_W, SCR_H, GS_PSM_32, 0, 0);
    drawIdx_ = 1;

    initSampling(&lod_, &clut_);

    xfer_ = packet_init(128, PACKET_NORMAL);
    // Room for the clear + sprite plus the FPS overlay's filled rectangles (3 qwords each).
    draw_ = packet_init(512, PACKET_NORMAL);
    if (xfer_ == 0 || draw_ == 0) {
        return false;
    }

    ready_ = true;
    return true;
}

// Shared upload + textured-sprite draw + tear-free flip. Uploads a w x h RGBA5551
// raster into @p tb and draws @p rect over a cleared BACK buffer, then swaps that
// buffer to the CRTC on the next vblank.
void GsDisplay::blit(const u16* raster, int w, int h, texbuffer_t* tb, texrect_t* rect,
                     bool overlay) {
    // Cache coherency before the DMA reads the EE RAM.
    const u32 bytes = (u32)w * (u32)h * sizeof(u16);
    SyncDCache((void*)raster, (u8*)raster + bytes);

    // Upload the raster -> VRAM texture.
    qword_t* q = xfer_->data;
    q = draw_texture_transfer(q, (void*)raster, w, h, GS_PSM_16, tb->address, tb->width);
    q = draw_texture_flush(q);
    dma_channel_send_chain(DMA_CHANNEL_GIF, xfer_->data, q - xfer_->data, 0, 0);
    dma_wait_fast();

    // Retarget drawing at the off-screen back buffer, clear it, draw the sprite.
    q = draw_->data;
    q = draw_setup_environment(q, 0, &frame_[drawIdx_], &z_);
    q = draw_clear(q, 0, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, 0x00, 0x00, 0x20);
    q = draw_texture_sampling(q, 0, &lod_);
    q = draw_texturebuffer(q, 0, tb, &clut_);
    q = draw_rect_textured(q, 0, rect);
    // FPS overlay: drawn after the game sprite so it sits on top of the border, outside the
    // canvas (top-right of the screen). Only the game present path passes overlay=true.
    if (overlay && fpsShow_) {
        q = drawFpsRects(q, 0, fpsValue_, SCR_W, SCR_H);
    }
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, draw_->data, q - draw_->data, 0, 0);
    dma_wait_fast();
    draw_wait_finish();

    // Flip WITHOUT blocking on vblank. draw_wait_finish() above already guarantees the
    // frame is fully rendered into frame_[drawIdx_]; graph_set_framebuffer_filtered writes
    // DISPFB, which the CRTC latches on its own at the next hardware vblank. We deliberately
    // do NOT wait for that latch: it used to cost up to a full 60 Hz frame (~11 ms measured)
    // of pure stall. Skipping it is tear-free HERE because this double-buffered surface is
    // only reused two frames later, and the game paces itself well above the 16.7 ms NTSC
    // vblank (a fixed ~21 ms Thread.sleep/frame), so a vblank always lands in between. If a
    // future game ran faster than ~60 FPS this could tear; re-add a vblank-count guard then.
    graph_set_framebuffer_filtered(frame_[drawIdx_].address, SCR_W, GS_PSM_32, 0, 0);
    drawIdx_ ^= 1;
}

void GsDisplay::present(const u16* rgba5551, int w, int h) {
    if (!ready_ || w <= 0 || h <= 0) {
        return;
    }
    if (!gameTexReady_) {
        initTexbuf(&texbuf_, GAME_TEX_W, GAME_TEX_H);
        gameTexReady_ = true;
    }
    // Contain-fit: scale by the limiting dimension so the whole screen is visible,
    // centred and letter/pillar-boxed. Recomputed every frame so a per-game resolution
    // change (portrait <-> landscape, different aspect) is applied without stale geometry.
    const float sx = (float)SCR_W / (float)w;
    const float sy = (float)SCR_H / (float)h;
    const float s  = sx < sy ? sx : sy;
    const float dw = (float)w * s;
    const float dh = (float)h * s;
    const float dx = ((float)SCR_W - dw) * 0.5f;
    const float dy = ((float)SCR_H - dh) * 0.5f;
    setRect(&rect_, dx, dy, dx + dw, dy + dh, (float)w, (float)h);
    blit(rgba5551, w, h, &texbuf_, &rect_, true);   // game path: allow the FPS overlay
}

void GsDisplay::presentFullscreen(const u16* rgba5551, int w, int h) {
    if (!ready_) {
        return;
    }
    if (!fsTexReady_) {
        initTexbuf(&fsTexbuf_, FS_TEX_W, FS_TEX_H);
        // Cover the entire TV: no pillarbox, source maps 1:1 to the native screen.
        setRect(&fsRect_, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, (float)w, (float)h);
        fsTexReady_ = true;
    }
    blit(rgba5551, w, h, &fsTexbuf_, &fsRect_, false);   // menu: never the FPS overlay
}

void GsDisplay::showSplash(const u16* rgba5551, int w, int h) {
    if (!ready_ || rgba5551 == 0 || w <= 0 || h <= 0) {
        return;
    }

    // Dedicated VRAM texture, sized to hold the fullscreen raster. Allocated here and
    // FREED before returning, so it occupies no VRAM once the menu (which allocates its
    // own textures lazily) starts. It is the most recent allocation when freed, so the
    // reclaim is clean regardless of the allocator's free policy.
    texbuffer_t tb;
    tb.width           = SPLASH_TEX_W;
    tb.psm             = GS_PSM_16;
    tb.address         = graph_vram_allocate(SPLASH_TEX_W, SPLASH_TEX_H, GS_PSM_16,
                                             GRAPH_ALIGN_BLOCK);
    tb.info.width      = draw_log2(SPLASH_TEX_W);
    tb.info.height     = draw_log2(SPLASH_TEX_H);
    tb.info.components = TEXTURE_COMPONENTS_RGBA;
    tb.info.function   = TEXTURE_FUNCTION_MODULATE;

    texrect_t rect;
    setRect(&rect, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, (float)w, (float)h);

    // Upload the raster to VRAM once -- only the modulation colour animates each frame,
    // so the (large) texture never has to be re-sent.
    const u32 bytes = (u32)w * (u32)h * sizeof(u16);
    SyncDCache((void*)rgba5551, (u8*)rgba5551 + bytes);
    qword_t* q = xfer_->data;
    q = draw_texture_transfer(q, (void*)rgba5551, w, h, GS_PSM_16, tb.address, tb.width);
    q = draw_texture_flush(q);
    dma_channel_send_chain(DMA_CHANNEL_GIF, xfer_->data, q - xfer_->data, 0, 0);
    dma_wait_fast();

    // Three-phase envelope (frames at ~60 Hz fields): ease-in-out fade in, hold, ease-in-
    // out fade out. MODULATE means out = texel * colour / 0x80, so a colour of 0..0x80
    // fades the image from black to full brightness and back.
    // The video signal only comes up HERE (ensureVideo on the splash), so on real hardware
    // the TV/monitor is often still syncing when the splash starts -- a short hold vanished
    // before any image appeared. Hold at full brightness long enough for the display to wake.
    const int FADE_IN = 24, HOLD = 180, FADE_OUT = 24;   // ~0.4s / ~3.0s / 0.4s
    const int TOTAL = FADE_IN + HOLD + FADE_OUT;
    for (int f = 0; f <= TOTAL; ++f) {
        float a;
        if (f < FADE_IN) {
            a = easeInOut((float)f / (float)FADE_IN);
        } else if (f < FADE_IN + HOLD) {
            a = 1.0f;
        } else {
            a = 1.0f - easeInOut((float)(f - FADE_IN - HOLD) / (float)FADE_OUT);
        }
        const u8 k = (u8)(a * 128.0f + 0.5f);
        rect.color.r = k; rect.color.g = k; rect.color.b = k;

        q = draw_->data;
        q = draw_setup_environment(q, 0, &frame_[drawIdx_], &z_);
        q = draw_clear(q, 0, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, 0x00, 0x00, 0x00);
        q = draw_texture_sampling(q, 0, &lod_);
        q = draw_texturebuffer(q, 0, &tb, &clut_);
        q = draw_rect_textured(q, 0, &rect);
        q = draw_finish(q);
        dma_channel_send_normal(DMA_CHANNEL_GIF, draw_->data, q - draw_->data, 0, 0);
        dma_wait_fast();
        draw_wait_finish();

        graph_set_framebuffer_filtered(frame_[drawIdx_].address, SCR_W, GS_PSM_32, 0, 0);
        graph_wait_vsync();
        drawIdx_ ^= 1;
    }

    // Reclaim the splash texture's VRAM before the menu allocates its own textures.
    graph_vram_free(tb.address);
}

} // namespace platform
} // namespace ps2
