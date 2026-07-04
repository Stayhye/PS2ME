// PS2 JavaCall port — platform layer. GsDisplay implementation.
//
// Ported from the Milestone-0 video path (src/main.c). The virtual phone screen is
// a 16-bit texture uploaded per flush and drawn as a scaled, pillarboxed sprite.
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
// Power-of-two VRAM texture holding the virtual screen. Must be >= the LCD size;
// TBW/log2 want 256 wide (multiple of 64) and 512 tall.
const int TEX_W = 256;
const int TEX_H = 512;
} // namespace

GsDisplay::~GsDisplay() {
    if (xfer_ != 0) packet_free(xfer_);
    if (draw_ != 0) packet_free(draw_);
}

bool GsDisplay::init(int lcdW, int lcdH) {
    if (ready_) {
        return true;
    }
    lcdW_ = lcdW;
    lcdH_ = lcdH;

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // --- GS framebuffer + 2D draw environment ---
    // Must hold draw_setup_environment (GIFTAG + 15 GS regs = 16 qwords) followed
    // by draw_finish (GIFTAG + FINISH = 2 qwords) = 18 qwords. A 16-qword packet
    // overran env->data by 32 bytes, smashing the next heap chunk header -> the
    // packet_free(env) below (or a later free) crashed in newlib _free_r. The bug
    // was heap-layout dependent, so it only bit once real suites populated the heap.
    // 20 matches the ps2sdk draw samples' environment packet and leaves margin.
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

    // --- VRAM texture for the virtual screen (16-bit) ---
    texbuf_.width           = TEX_W;
    texbuf_.psm             = GS_PSM_16;
    texbuf_.address         = graph_vram_allocate(TEX_W, TEX_H, GS_PSM_16, GRAPH_ALIGN_BLOCK);
    texbuf_.info.width      = draw_log2(TEX_W);
    texbuf_.info.height     = draw_log2(TEX_H);
    texbuf_.info.components  = TEXTURE_COMPONENTS_RGBA;
    texbuf_.info.function    = TEXTURE_FUNCTION_MODULATE;

    // NEAREST sampling (sharp pixels), no CLUT (direct 16-bit texture).
    lod_.calculation = LOD_USE_K;
    lod_.max_level   = 0;
    lod_.mag_filter  = LOD_MAG_NEAREST;
    lod_.min_filter  = LOD_MIN_NEAREST;
    lod_.l           = 0;
    lod_.k           = 0;

    clut_.storage_mode = CLUT_STORAGE_MODE1;
    clut_.start        = 0;
    clut_.psm          = 0;
    clut_.load_method  = CLUT_NO_LOAD;
    clut_.address      = 0;

    // Destination rect: fit the portrait screen into the TV preserving aspect
    // (portrait -> pillarboxed). Height = screen height, width scaled from it.
    {
        const float dh = (float)SCR_H;
        const float dw = dh * (float)lcdW_ / (float)lcdH_;
        const float dx = ((float)SCR_W - dw) * 0.5f;
        rect_.v0.x = dx;              rect_.v0.y = 0.0f;   rect_.v0.z = 0;
        rect_.t0.u = 0.0f;           rect_.t0.v = 0.0f;
        rect_.v1.x = dx + dw;         rect_.v1.y = dh;     rect_.v1.z = 0;
        rect_.t1.u = (float)lcdW_;    rect_.t1.v = (float)lcdH_;
        rect_.color.r = 0x80; rect_.color.g = 0x80; rect_.color.b = 0x80;
        rect_.color.a = 0x80; rect_.color.q = 1.0f;
    }

    xfer_ = packet_init(128, PACKET_NORMAL);
    draw_ = packet_init(128, PACKET_NORMAL);
    if (xfer_ == 0 || draw_ == 0) {
        return false;
    }

    ready_ = true;
    return true;
}

void GsDisplay::present(const u16* rgba5551) {
    if (!ready_) {
        return;
    }

    // Cache coherency before the DMA reads the EE RAM.
    const u32 bytes = (u32)lcdW_ * (u32)lcdH_ * sizeof(u16);
    SyncDCache((void*)rgba5551, (u8*)rgba5551 + bytes);

    // Upload the virtual screen -> VRAM texture.
    qword_t* q = xfer_->data;
    q = draw_texture_transfer(q, (void*)rgba5551, lcdW_, lcdH_, GS_PSM_16,
                              texbuf_.address, texbuf_.width);
    q = draw_texture_flush(q);
    dma_channel_send_chain(DMA_CHANNEL_GIF, xfer_->data, q - xfer_->data, 0, 0);
    dma_wait_fast();

    // Clear the TV and draw the scaled textured sprite.
    q = draw_->data;
    q = draw_clear(q, 0, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, 0x00, 0x00, 0x20);
    q = draw_texture_sampling(q, 0, &lod_);
    q = draw_texturebuffer(q, 0, &texbuf_, &clut_);
    q = draw_rect_textured(q, 0, &rect_);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, draw_->data, q - draw_->data, 0, 0);
    dma_wait_fast();

    draw_wait_finish();
    graph_wait_vsync();
}

} // namespace platform
} // namespace ps2
