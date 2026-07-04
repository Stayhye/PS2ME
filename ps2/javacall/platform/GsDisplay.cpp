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
// Power-of-two VRAM texture for the pillarboxed game screen (<= 240x320 source).
const int GAME_TEX_W = 256;
const int GAME_TEX_H = 512;
// Power-of-two VRAM texture for the fullscreen UI (holds the 640x448 native raster).
const int FS_TEX_W = 1024;
const int FS_TEX_H = 512;

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

    // --- GS framebuffer + 2D draw environment ---
    // Must hold draw_setup_environment (GIFTAG + 15 GS regs = 16 qwords) followed by
    // draw_finish (GIFTAG + FINISH = 2 qwords) = 18 qwords. 20 matches the ps2sdk
    // draw samples' environment packet and leaves margin (a 16-qword packet overran
    // env->data and smashed the next heap chunk header -> a later free crashed).
    packet_t* env = packet_init(20, PACKET_NORMAL);
    if (env == 0) {
        return false;
    }
    qword_t* q = env->data;

    frame_.width   = SCR_W;
    frame_.height  = SCR_H;
    frame_.psm     = GS_PSM_32;
    frame_.mask    = 0;
    frame_.address = graph_vram_allocate(SCR_W, SCR_H, GS_PSM_32, GRAPH_ALIGN_PAGE);

    z_.enable  = 0;               // no z-buffer for 2D
    z_.method  = ZTEST_METHOD_GREATER;
    z_.address = 0;
    z_.mask    = 1;
    z_.zsm     = 0;

    // Tie the framebuffer to the TV read circuits.
    graph_initialize(frame_.address, SCR_W, SCR_H, GS_PSM_32, 0, 0);

    // Draw env: sets XYOFFSET so screen coords are plain 0..W / 0..H.
    q = draw_setup_environment(q, 0, &frame_, &z_);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, env->data, q - env->data, 0, 0);
    dma_wait_fast();
    packet_free(env);

    initSampling(&lod_, &clut_);

    xfer_ = packet_init(128, PACKET_NORMAL);
    draw_ = packet_init(128, PACKET_NORMAL);
    if (xfer_ == 0 || draw_ == 0) {
        return false;
    }

    ready_ = true;
    return true;
}

// Shared upload + textured-sprite draw. Uploads a w x h RGBA5551 raster into @p tb
// and draws @p rect, clearing the TV first.
namespace {
void blit(packet_t* xfer, packet_t* draw, const u16* raster, int w, int h,
          texbuffer_t* tb, clutbuffer_t* clut, lod_t* lod, texrect_t* rect,
          bool waitVsync) {
    // Cache coherency before the DMA reads the EE RAM.
    const u32 bytes = (u32)w * (u32)h * sizeof(u16);
    SyncDCache((void*)raster, (u8*)raster + bytes);

    // Upload the raster -> VRAM texture.
    qword_t* q = xfer->data;
    q = draw_texture_transfer(q, (void*)raster, w, h, GS_PSM_16, tb->address, tb->width);
    q = draw_texture_flush(q);
    dma_channel_send_chain(DMA_CHANNEL_GIF, xfer->data, q - xfer->data, 0, 0);
    dma_wait_fast();

    // Clear the TV and draw the textured sprite.
    q = draw->data;
    q = draw_clear(q, 0, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, 0x00, 0x00, 0x20);
    q = draw_texture_sampling(q, 0, lod);
    q = draw_texturebuffer(q, 0, tb, clut);
    q = draw_rect_textured(q, 0, rect);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, draw->data, q - draw->data, 0, 0);
    dma_wait_fast();

    draw_wait_finish();
    if (waitVsync) {
        graph_wait_vsync();   // the fullscreen (menu) path paces on vsync itself
    }
}
} // namespace

void GsDisplay::present(const u16* rgba5551, int w, int h) {
    if (!ready_) {
        return;
    }
    if (!gameTexReady_) {
        initTexbuf(&texbuf_, GAME_TEX_W, GAME_TEX_H);
        // Fit the portrait screen into the TV preserving aspect (pillarboxed).
        const float dh = (float)SCR_H;
        const float dw = dh * (float)w / (float)h;
        const float dx = ((float)SCR_W - dw) * 0.5f;
        setRect(&rect_, dx, 0.0f, dx + dw, dh, (float)w, (float)h);
        gameTexReady_ = true;
    }
    blit(xfer_, draw_, rgba5551, w, h, &texbuf_, &clut_, &lod_, &rect_, true);
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
    blit(xfer_, draw_, rgba5551, w, h, &fsTexbuf_, &clut_, &lod_, &fsRect_, false);
}

} // namespace platform
} // namespace ps2
