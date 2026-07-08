// PS2 JavaCall port — platform layer. Ps2SaveIcon implementation.
//
// Builds the two files the PS2 OSD browser needs to render a directory as a proper save:
//
//   icon.sys  - 964-byte metadata (the libmc `mcIcon` struct): "PS2D" magic, title in
//               Shift-JIS (ASCII is a subset, so we copy the bytes directly), background
//               gradient, three light sources, and the icon filenames (all -> "icon.icn").
//
//   icon.icn  - a 3D icon in the documented PS2 icon format (matching wLaunchELF's
//               makeicon.c, which is the battle-tested reference):
//                 header (20B): id=0x00010000, anim_shapes=1, tex_type=0x06 (uncompressed),
//                               0x3F800000, vertex_count
//                 vertices    : 24B each -> position(3xf16 + pad), normal(3xf16 + pad),
//                               uv(2xf16), colour(RGBA u8, 0x80 = full)
//                 animation   : header(20B) + one static frame(16B)
//                 texture     : 128x128, 16-bit A1B5G5R5, uncompressed (32768B)
//               f16 fixed-point = value * 4096. We draw a double-sided flat quad (12
//               vertices = 4 triangles) so it is visible regardless of the browser's
//               back-face culling, textured with the PS2ME logo.
//
// tex_type: wLaunchELF uses 0x0E for an RLE-compressed texture; the compression bit is
// 0x08, so 0x06 selects the same layout uncompressed and lets us skip the RLE codec.
#include "Ps2SaveIcon.hpp"

extern "C" {
#include <libmc.h>          // mcIcon / iconIVECTOR / iconFVECTOR (icon.sys layout)
}

#include "Ps2MemCard.hpp"
#include "MidletIcon.hpp"   // decodePng (the embedded PS2ME logo)
#include "ps2me_icon.h"     // g_ps2me_icon / g_ps2me_icon_len (xxd -i of PS2ME_ICON.png)

#include <stdlib.h>         // malloc / free
#include <stdio.h>          // snprintf
#include <string.h>         // memset / memcpy / strlen

namespace ps2 {
namespace platform {

namespace {

const int   kTexDim   = 128;                       // PS2 icon texture is fixed 128x128
const int   kTexBytes = kTexDim * kTexDim * 2;     // 16-bit -> 32768
const int   kNumVerts = 12;                        // double-sided quad (4 triangles)
const float kHalf     = 2.5f;                      // quad half-extent (model units); sized
                                                   // to match a normal PS2 save's on-screen
                                                   // icon (4.0 rendered noticeably too big)

// Little-endian byte-cursor writers (icon.icn is a flat binary; avoid struct packing
// assumptions on the EE by emitting every field explicitly).
void put32(unsigned char*& p, unsigned int v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
    p += 4;
}
void put16(unsigned char*& p, int v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p += 2;
}
// Fixed-point f16: model-space float -> int16 scaled by 4096.
void putF16(unsigned char*& p, float f) {
    int v = (int)(f * 4096.0f);
    if (v > 32767)  v = 32767;
    if (v < -32768) v = -32768;
    put16(p, v);
}

// A1B5G5R5, red in the low bits, alpha bit set (opaque) -- the PS2 GS 16-bit / TIM format
// the browser expects (same convention as the launcher's rgba5551).
unsigned short toA1B5G5R5(int r, int g, int b) {
    return (unsigned short)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
                            (((b >> 3) & 0x1F) << 10) | 0x8000);
}

// ASCII -> full-width Shift-JIS, matching ps2sdk's strcpy_sjis. The PS2 OSD browser's save
// title is Shift-JIS and its font renders these 2-byte full-width codes, NOT raw half-width
// ASCII (a plain byte copy shows a blank title). Letters/digits map arithmetically; a small
// table covers space and punctuation. Values are the little-endian shorts as stored in the
// title buffer (e.g. space -> 0x4081 -> bytes 0x81,0x40 = full-width space).
struct SjisMap { unsigned char ascii; unsigned short sjis; };
const SjisMap kSjisSpecial[] = {
    { ' ', 0x4081 }, { '[', 0x6d81 }, { ']', 0x6e81 }, { '-', 0x7c81 }, { '.', 0x4481 },
    { '+', 0x7b81 }, { '*', 0x9681 }, { '/', 0x5e81 }, { '!', 0x4981 }, { '"', 0x6881 },
    { '#', 0x9481 }, { '$', 0x9081 }, { '%', 0x9381 }, { '&', 0x9581 }, { '\'', 0x6681 },
    { '(', 0x6981 }, { ')', 0x6a81 }, { '=', 0x8181 }, { '|', 0x6281 }, { '\\', 0x8f81 },
    { '?', 0x4881 }, { '_', 0x5181 }, { '{', 0x6f81 }, { '}', 0x7081 }, { '@', 0x9781 },
    { ';', 0x4781 }, { ':', 0x4681 }, { '<', 0x8381 }, { '>', 0x8481 }, { '`', 0x4d81 },
};
unsigned short asciiToSjis(unsigned char c) {
    for (unsigned i = 0; i < sizeof(kSjisSpecial) / sizeof(kSjisSpecial[0]); ++i) {
        if (kSjisSpecial[i].ascii == c) {
            return kSjisSpecial[i].sjis;
        }
    }
    int a = c;
    if (a > 96) ++a;                                   // lowercase adjust (per strcpy_sjis)
    return (unsigned short)(((a + 0x1f) << 8) | 0x82);
}

// One quad corner: model position (x,y up), texture uv, shared normal.
struct Corner { float x, y, u, v; };

// Emit one 24-byte icon vertex: position, normal (nz), uv, opaque grey colour (0x80).
void putVertex(unsigned char*& p, const Corner& c, float nz) {
    putF16(p, c.x); putF16(p, c.y); putF16(p, 0.0f); put16(p, 0);   // position + pad
    putF16(p, 0.0f); putF16(p, 0.0f); putF16(p, nz);  put16(p, 0);  // normal + pad
    putF16(p, c.u); putF16(p, c.v);                                 // uv
    p[0] = 0x80; p[1] = 0x80; p[2] = 0x80; p[3] = 0x80; p += 4;     // rgba (0x80 = full)
}

// Fill the 128x128 A1B5G5R5 texture from an RGBA8888 source image (@p rgba, @p w x @p h),
// nearest-resampled. The format carries a 1-bit alpha, so we honour the source alpha as an
// on/off mask: a source pixel that is (mostly) transparent becomes a fully transparent texel
// (0x0000) -- the icon.sys background gradient then shows through, giving the icon a shaped,
// "floating" look instead of sitting on a solid card -- while an opaque pixel keeps its
// colour with the alpha bit set. There is no partial alpha, so anti-aliased edges are
// thresholded (hard edges). Falls back to a plain opaque gradient when no source is given.
void buildTexture(unsigned char* tex, const unsigned char* rgba, int w, int h) {
    unsigned short* out = (unsigned short*)tex;
    const int bgR = 18, bgG = 26, bgB = 58;        // gradient fallback base (no source)
    for (int y = 0; y < kTexDim; ++y) {
        for (int x = 0; x < kTexDim; ++x) {
            if (rgba != 0 && w > 0 && h > 0) {
                const int sx = x * w / kTexDim;
                const int sy = y * h / kTexDim;
                const unsigned char* s = rgba + (sy * w + sx) * 4;
                // 1-bit alpha: below half -> transparent (show the gradient), else opaque.
                out[y * kTexDim + x] =
                    (s[3] < 128) ? 0 : toA1B5G5R5(s[0], s[1], s[2]);
            } else {
                const int t = y * 255 / (kTexDim - 1);        // opaque gradient fallback
                out[y * kTexDim + x] = toA1B5G5R5(bgR + t * 40 / 255,
                                                  bgG + t * 50 / 255, bgB + t * 70 / 255);
            }
        }
    }
}

// Build the whole icon.icn (textured with @p rgba, @p w x @p h) into a freshly malloc'd
// buffer; caller frees. Sets *outLen.
unsigned char* buildIcon(int* outLen, const unsigned char* rgba, int w, int h) {
    const int total = 20 + kNumVerts * 24 + 20 + 16 + kTexBytes;
    unsigned char* buf = (unsigned char*)malloc(total);
    if (buf == 0) {
        return 0;
    }
    unsigned char* p = buf;

    // Header.
    put32(p, 0x00010000);   // file id
    put32(p, 1);            // animation shapes
    put32(p, 0x06);         // texture type: uncompressed
    put32(p, 0x3F800000);   // fixed (float 1.0)
    put32(p, kNumVerts);    // vertex count

    // Quad corners (Y up). The browser's icon space has V increasing downward relative to
    // the texture, so the top corners map to v=1 and the bottom to v=0 -- this keeps the
    // logo upright (without the flip it renders upside down).
    const Corner TL = { -kHalf,  kHalf, 0.0f, 1.0f };
    const Corner TR = {  kHalf,  kHalf, 1.0f, 1.0f };
    const Corner BR = {  kHalf, -kHalf, 1.0f, 0.0f };
    const Corner BL = { -kHalf, -kHalf, 0.0f, 0.0f };

    // Front face (normal +Z): TL,TR,BR + TL,BR,BL.
    putVertex(p, TL,  1.0f); putVertex(p, TR,  1.0f); putVertex(p, BR,  1.0f);
    putVertex(p, TL,  1.0f); putVertex(p, BR,  1.0f); putVertex(p, BL,  1.0f);
    // Back face (normal -Z, reversed winding): TL,BR,TR + TL,BL,BR.
    putVertex(p, TL, -1.0f); putVertex(p, BR, -1.0f); putVertex(p, TR, -1.0f);
    putVertex(p, TL, -1.0f); putVertex(p, BL, -1.0f); putVertex(p, BR, -1.0f);

    // Animation: one static frame.
    put32(p, 1);            // anim header magic
    put32(p, 1);            // frame length
    put32(p, 0x3F800000);   // anim speed (1.0)
    put32(p, 0);            // play offset
    put32(p, 1);            // frame count
    put32(p, 0);            // frame: shape id
    put32(p, 1);            // frame: key count
    put32(p, 1);            // frame: time
    put32(p, 1);            // frame: value

    // Texture.
    buildTexture(p, rgba, w, h);
    p += kTexBytes;

    *outLen = total;
    return buf;
}

// Fill the 964-byte icon.sys (mcIcon) titled @p title (single line, ASCII/SJIS).
void buildIconSys(mcIcon* ic, const char* title) {
    memset(ic, 0, sizeof(mcIcon));
    memcpy(ic->head, "PS2D", 4);
    ic->type = 0;
    ic->trans = 0x60;                              // icon background opacity

    // Title: encode to full-width Shift-JIS (the browser won't render raw ASCII). Each
    // character becomes one 16-bit code; nlOffset is a character index, so pointing it at
    // the end keeps the whole title on a single line.
    int n = 0;
    for (; title[n] != '\0' && n < 33; ++n) {
        ic->title[n] = asciiToSjis((unsigned char)title[n]);
    }
    ic->title[n] = 0;
    ic->nlOffset = (unsigned short)n;              // single line (line 2 starts at the end)

    // Background gradient (RGB 0..0x80): a dark blue that suits the PS2ME logo.
    const int bg[4][3] = { { 30, 44, 96 }, { 30, 44, 96 }, { 10, 16, 44 }, { 10, 16, 44 } };
    for (int i = 0; i < 4; ++i) {
        ic->bgCol[i][0] = bg[i][0];
        ic->bgCol[i][1] = bg[i][1];
        ic->bgCol[i][2] = bg[i][2];
        ic->bgCol[i][3] = 0;
    }

    // Three light directions + colours + ambient (the standard values that light an icon
    // evenly; taken from the ps2sdk libmc example).
    const float ldir[3][3] = { { 0.5f, 0.5f, 0.5f }, { 0.0f, -0.4f, -0.1f }, { -0.5f, -0.5f, 0.5f } };
    const float lcol[3][3] = { { 0.3f, 0.3f, 0.3f }, { 0.4f, 0.4f, 0.4f }, { 0.5f, 0.5f, 0.5f } };
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            ic->lightDir[i][k] = ldir[i][k];
            ic->lightCol[i][k] = lcol[i][k];
        }
        ic->lightDir[i][3] = 0.0f;
        ic->lightCol[i][3] = 0.0f;
    }
    ic->lightAmbient[0] = 0.5f;
    ic->lightAmbient[1] = 0.5f;
    ic->lightAmbient[2] = 0.5f;
    ic->lightAmbient[3] = 0.0f;

    // The browser loads this icon for the list, copy and delete views.
    memcpy(ic->view, "icon.icn", 9);
    memcpy(ic->copy, "icon.icn", 9);
    memcpy(ic->del,  "icon.icn", 9);
}

} // namespace

// Bump when the generated icon.sys / icon.icn changes, to force a one-time rewrite on the
// next boot; otherwise an already-installed save at this version is left untouched.
const int kIconVersion = 3;   // bumped: 1-bit alpha (transparent texels show the gradient)

bool Ps2SaveIcon::install(const char* absDir, const char* title,
                          const unsigned char* rgba, int w, int h) {
    if (!Ps2MemCard::instance().available() || absDir == 0) {
        return false;
    }
    Ps2MemCard::instance().ensureDir(absDir);

    char path[80];
    mcIcon sys;
    buildIconSys(&sys, (title != 0) ? title : "PS2ME");
    snprintf(path, sizeof(path), "%s/icon.sys", absDir);
    const bool okSys = Ps2MemCard::instance().writeFile(path, &sys, (int)sizeof(mcIcon));

    int icnLen = 0;
    unsigned char* icn = buildIcon(&icnLen, rgba, w, h);
    bool okIcn = false;
    if (icn != 0) {
        snprintf(path, sizeof(path), "%s/icon.icn", absDir);
        okIcn = Ps2MemCard::instance().writeFile(path, icn, icnLen);
        free(icn);
    }
    return okSys && okIcn;
}

bool Ps2SaveIcon::installAppIcon(const char* title) {
    if (!Ps2MemCard::instance().available()) {
        return false;
    }

    // Write-once per version: skip the ~33 KB rewrite when the save is already current.
    int ver = 0;
    if (Ps2MemCard::instance().configRead("iconver", &ver, (int)sizeof(ver)) ==
            (int)sizeof(ver) && ver == kIconVersion) {
        return true;
    }

    // Texture the app save with the embedded PS2ME logo.
    int w = 0, h = 0;
    unsigned char* rgba = MidletIcon::decodePng(g_ps2me_icon, (int)g_ps2me_icon_len, &w, &h);
    const bool ok = install("/PS2ME", (title != 0) ? title : "PS2ME", rgba, w, h);
    if (rgba != 0) {
        MidletIcon::release(rgba);
    }

    // Record the version so subsequent boots skip the rewrite.
    if (ok) {
        int v = kIconVersion;
        Ps2MemCard::instance().configWrite("iconver", &v, (int)sizeof(v));
    }
    return ok;
}

} // namespace platform
} // namespace ps2
