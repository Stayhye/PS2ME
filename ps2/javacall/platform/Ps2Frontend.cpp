// PS2 JavaCall port — platform layer. Ps2Frontend implementation.
//
// Standalone native menu, no dependency on the Java VM. It renders a full native
// resolution (640x448) "console dashboard" UI -- a metallic-blue themed launcher with
// a header (logo + session clock), navigation tabs, an alphabet sidebar, a rounded
// game grid with a glowing selection, a right-hand details panel with a large preview,
// and a footer button legend -- into its own RGBA5551 raster with the embedded
// TrueType font, and presents it fullscreen through the shared GsDisplay (no
// pillarbox). It reads the shared pad backend and lists games from hal::GameStorage.
//
// Everything is drawn procedurally (gradients, bevelled rounded panels, a cyan
// selection glow, vector controller glyphs) except the two brand rasters: the Java
// flame logo (java_logo.png) and the PS2ME wordmark (ps2me_title.png). Until those
// assets are supplied the embedded PS2ME icon stands in for the logo and the wordmark
// is drawn in the UI font.
//
// Icons come from IconCache, a background worker that decodes only the on-screen icons
// off the render path, so navigation never stalls on JAR I/O. The render thread paces
// on vsync via an interrupt handler + semaphore (rather than a busy wait), which is
// also what lets the lower-priority icon worker run in the idle time between frames.
#include "Ps2Frontend.hpp"

#include "GsDisplay.hpp"
#include "MidletIcon.hpp"
#include "IconCache.hpp"
#include "Favorites.hpp"
#include "Recent.hpp"
#include "Settings.hpp"
#include "Ps2Storage.hpp"
#include "../hal/GameStorage.hpp"
#include "../hal/Keypad.hpp"
#include "../hal/IPad.hpp"
#include "../hal/SystemClock.hpp"   // session clock for the header

#include <tamtypes.h>   // u16
#include <malloc.h>     // memalign / malloc / free
#include <stdlib.h>
#include <string.h>     // memcpy (blit the baked background each frame)
#include <stdio.h>      // snprintf (clock / index labels)
#include <kernel.h>     // CreateSema / WaitSema / iSignalSema (vsync pacing)
#include <graph.h>      // graph_add_vsync_handler / graph_wait_vsync

// stb_truetype is a single-header library; define the implementation here (exactly
// one TU). The embedded font (g_ui_ttf/g_ui_ttf_len) is generated from vendors/ui.ttf.
// Both are found via -I<repo>/vendors added to this file's compile flags.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "ui_ttf.h"

// Embedded PS2ME title icon (assets/PS2ME_ICON.png -> byte array). Decoded once at
// startup via MidletIcon::decodePng and drawn beside the title. -I<repo>/assets.
#include "ps2me_icon.h"

namespace ps2 {
namespace platform {

namespace {

// Native NTSC TV resolution: the UI owns the whole screen (no pillarbox), so its
// raster is full-size and GsDisplay stretches it 1:1 onto the framebuffer.
const int SCREEN_W = 640;
const int SCREEN_H = 448;

// --- Dashboard layout (all in native pixels) --------------------------------------
const int MARGIN     = 10;
const int HEADER_H   = 34;              // top bar: logo + wordmark + clock
const int TITLE_ICON = 26;              // logo drawn left of the wordmark
const int TAB_Y      = 40;              // navigation tab row
const int TAB_H      = 26;
const int FOOTER_H   = 30;              // bottom button legend
const int SIDE_X     = 6;               // alphabet sidebar
const int SIDE_W     = 16;
const int DET_W      = 150;             // right-hand details panel

const int COLS       = 4;
const int ROWS       = 5;
const int GRID_GAP   = 6;
const int ICON       = 44;              // on-screen grid icon size (square)
const float NAME_PX  = 14.0f;

// Derived geometry (shared by render() and pick()).
const int CONTENT_Y      = TAB_Y + TAB_H + 8;        // top of the main area
const int FOOTER_Y       = SCREEN_H - FOOTER_H;
const int CONTENT_BOTTOM = FOOTER_Y - 4;
const int DET_X          = SCREEN_W - 8 - DET_W;
const int SCROLL_X       = DET_X - 12;               // vertical scrollbar
const int GRID_X0        = SIDE_X + SIDE_W + 8;
const int GRID_X1        = SCROLL_X - 8;
const int GRID_CELLW     = (GRID_X1 - GRID_X0 - (COLS - 1) * GRID_GAP) / COLS;
const int GRID_Y0        = CONTENT_Y + 2;
const int GRID_ROWSTRIDE = (CONTENT_BOTTOM - GRID_Y0) / ROWS;

// --- Font -------------------------------------------------------------------------
stbtt_fontinfo g_font;
bool           g_fontOk = false;

bool initFont() {
    if (g_fontOk) {
        return true;
    }
    const int off = stbtt_GetFontOffsetForIndex(g_ui_ttf, 0);
    if (off < 0 || !stbtt_InitFont(&g_font, g_ui_ttf, off)) {
        return false;
    }
    g_fontOk = true;
    return true;
}

// --- Our own software raster (GS-native RGBA5551 / CT16) --------------------------
// The GS 16-bit texture is R in the low bits, G, then B, alpha in the top bit (the
// exact layout Ps2Framebuffer converts RGB565 into before presenting).
u16* g_ras = 0;
u16* g_bg  = 0;             // baked background (gradient + vignette), blitted per frame
int  g_w = 0;
int  g_h = 0;

// The title logo (TITLE_ICON x TITLE_ICON RGBA8888), decoded once; null if it failed.
unsigned char* g_titleIcon = 0;

// Distinct game-name initials for the alphabet sidebar, built once from the list.
char g_initials[48];
int  g_initialCount = 0;
bool g_initialsBuilt = false;

// The current tab's "view": a list of game indices to display/navigate. ALL GAMES is
// the identity (all games); FAVORITES is the favourited subset. Rebuilt on tab change
// and whenever a favourite is toggled.
const int MAX_VIEW = 2048;      // matches Ps2HostStorage / IconCache caps
int g_view[MAX_VIEW];
int g_viewCount = 0;

// Sort order applied to the non-recent part of the view. 0=Name A-Z, 1=Name Z-A.
const int SORT_COUNT = 2;
int g_sortMode = 0;

// Number of leading g_view slots reserved for the "recent" row (0 = no recent row).
// When >0, g_view[0..COLS-1] hold the recent games padded with -1 to a full row, and
// the sorted rest starts at index COLS.
int g_recentBand = 0;

// Frame counter for the active item's name auto-shift (marquee); reset when the
// selection changes so each newly-active long name scrolls from the start.
unsigned g_marqueeTick = 0;

// Vsync pacing: an interrupt handler signals this semaphore each field, and the render
// loop blocks on it. Blocking (rather than busy-waiting graph_wait_vsync) is what frees
// the CPU for the lower-priority icon worker between frames.
int g_vsyncSema = -1;
int g_vsyncCb   = -1;

int onVsync(int /*cause*/) {
    iSignalSema(g_vsyncSema);
    return 0;
}

inline u16 rgba5551(int r, int g, int b) {
    return (u16)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) |
                 (((b >> 3) & 0x1F) << 10) | 0x8000);
}

// Blend an (sr,sg,sb) colour over an RGBA5551 pixel with coverage a (0..255).
inline u16 blend5551(u16 dst, int sr, int sg, int sb, int a) {
    const int dr = ( dst        & 0x1F) << 3;
    const int dg = ((dst >> 5)  & 0x1F) << 3;
    const int db = ((dst >> 10) & 0x1F) << 3;
    const int r = dr + ((sr - dr) * a) / 255;
    const int g = dg + ((sg - dg) * a) / 255;
    const int b = db + ((sb - db) * a) / 255;
    return rgba5551(r, g, b);
}

// Integer square root (rounded down) -- rounded-corner insets, no libm dependency.
inline int isqrt(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

inline void plotA(int x, int y, int r, int g, int b, int a) {
    if (a <= 0) return;
    if ((unsigned)x < (unsigned)g_w && (unsigned)y < (unsigned)g_h) {
        u16* p = &g_ras[y * g_w + x];
        *p = (a >= 255) ? rgba5551(r, g, b) : blend5551(*p, r, g, b, a);
    }
}

void fillRect(int x, int y, int w, int h, u16 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    for (int j = 0; j < h; ++j) {
        u16* row = g_ras + (y + j) * g_w + x;
        for (int i = 0; i < w; ++i) {
            row[i] = c;
        }
    }
}

// A plain (square) vertical-gradient rect -- header/footer bars.
void rectVGrad(int x, int y, int w, int h,
               int r0, int g0, int b0, int r1, int g1, int b1) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; ++j) {
        const int t = (h > 1) ? (j * 255 / (h - 1)) : 0;
        const u16 c = rgba5551(r0 + (r1 - r0) * t / 255,
                               g0 + (g1 - g0) * t / 255,
                               b0 + (b1 - b0) * t / 255);
        const int yy = y + j;
        if (yy < 0 || yy >= g_h) continue;
        int xa = x, xb = x + w;
        if (xa < 0) xa = 0;
        if (xb > g_w) xb = g_w;
        u16* row = g_ras + yy * g_w;
        for (int i = xa; i < xb; ++i) row[i] = c;
    }
}

// Left/right inset of a rounded rect on row j (of height h, corner radius rad).
inline int cornerInset(int j, int h, int rad) {
    const int dy = (j < h - 1 - j) ? j : (h - 1 - j);
    if (dy >= rad) return 0;
    const int k = rad - 1 - dy;
    int v = rad * rad - k * k;
    if (v < 0) v = 0;
    return rad - isqrt(v);
}

// Rounded rect filled with a vertical gradient (opaque) -- panels, tiles, tabs.
void roundRectVGrad(int x, int y, int w, int h, int rad,
                    int r0, int g0, int b0, int r1, int g1, int b1) {
    if (w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    for (int j = 0; j < h; ++j) {
        const int ins = cornerInset(j, h, rad);
        const int t = (h > 1) ? (j * 255 / (h - 1)) : 0;
        const u16 c = rgba5551(r0 + (r1 - r0) * t / 255,
                               g0 + (g1 - g0) * t / 255,
                               b0 + (b1 - b0) * t / 255);
        const int yy = y + j;
        if (yy < 0 || yy >= g_h) continue;
        u16* row = g_ras + yy * g_w;
        for (int i = ins; i < w - ins; ++i) {
            const int xx = x + i;
            if (xx >= 0 && xx < g_w) row[xx] = c;
        }
    }
}

// Rounded rect filled with a flat colour at coverage a -- selection glow layers.
void roundRectFillA(int x, int y, int w, int h, int rad, int r, int g, int b, int a) {
    if (w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    for (int j = 0; j < h; ++j) {
        const int ins = cornerInset(j, h, rad);
        const int yy = y + j;
        for (int i = ins; i < w - ins; ++i) plotA(x + i, yy, r, g, b, a);
    }
}

// A bevelled metallic panel: bright 1px edge + inner gradient fill.
void panel(int x, int y, int w, int h, int rad) {
    roundRectFillA(x, y, w, h, rad, 120, 150, 205, 255);            // outer bevel edge
    const int r2 = rad - 1 > 0 ? rad - 1 : 0;
    roundRectVGrad(x + 1, y + 1, w - 2, h - 2, r2, 56, 86, 140, 26, 44, 84);
}

// The glowing cyan frame + recessed fill for the selected grid tile.
void drawSelection(int x, int y, int w, int h, int rad) {
    roundRectFillA(x - 5, y - 5, w + 10, h + 10, rad + 4,  70, 200, 255,  40);  // bloom
    roundRectFillA(x - 3, y - 3, w + 6,  h + 6,  rad + 3,  90, 215, 255,  95);
    roundRectFillA(x - 2, y - 2, w + 4,  h + 4,  rad + 2, 130, 235, 255, 255);  // frame
    roundRectVGrad(x, y, w, h, rad, 40, 74, 132, 24, 46, 90);                   // recess
}

// Bresenham line (for the triangle glyph).
void line(int x0, int y0, int x1, int y1, int r, int g, int b) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        plotA(x0, y0, r, g, b, 255);
        plotA(x0 + 1, y0, r, g, b, 255);   // 2px thick
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// PS2 face-button glyphs, drawn as vector outlines (footer legend).
void glyphCross(int cx, int cy, int R, int r, int g, int b) {
    for (int t = -R; t <= R; ++t) {
        plotA(cx + t, cy + t,     r, g, b, 255);
        plotA(cx + t, cy + t + 1, r, g, b, 255);
        plotA(cx + t, cy - t,     r, g, b, 255);
        plotA(cx + t, cy - t + 1, r, g, b, 255);
    }
}
void glyphCircle(int cx, int cy, int R, int r, int g, int b) {
    const int ro = R * R, ri = (R - 2) * (R - 2);
    for (int j = -R; j <= R; ++j)
        for (int i = -R; i <= R; ++i) {
            const int d = i * i + j * j;
            if (d <= ro && d >= ri) plotA(cx + i, cy + j, r, g, b, 255);
        }
}
void glyphTriangle(int cx, int cy, int R, int r, int g, int b) {
    line(cx,     cy - R,     cx - R, cy + R - 1, r, g, b);
    line(cx - R, cy + R - 1, cx + R, cy + R - 1, r, g, b);
    line(cx + R, cy + R - 1, cx,     cy - R,     r, g, b);
}
void glyphSquare(int cx, int cy, int R, int r, int g, int b) {
    for (int t = -R; t <= R; ++t) {
        plotA(cx + t, cy - R,     r, g, b, 255);
        plotA(cx + t, cy - R + 1, r, g, b, 255);
        plotA(cx + t, cy + R,     r, g, b, 255);
        plotA(cx + t, cy + R - 1, r, g, b, 255);
        plotA(cx - R,     cy + t, r, g, b, 255);
        plotA(cx - R + 1, cy + t, r, g, b, 255);
        plotA(cx + R,     cy + t, r, g, b, 255);
        plotA(cx + R - 1, cy + t, r, g, b, 255);
    }
}

inline int edgeFn(int ax, int ay, int bx, int by, int px, int py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Solid triangle fill (bounding-box + edge-sign test; handles either winding).
void fillTriangle(int ax, int ay, int bx, int by, int cx, int cy, int r, int g, int b) {
    int minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= g_w) maxx = g_w - 1;
    if (maxy >= g_h) maxy = g_h - 1;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            const int w0 = edgeFn(bx, by, cx, cy, x, y);
            const int w1 = edgeFn(cx, cy, ax, ay, x, y);
            const int w2 = edgeFn(ax, ay, bx, by, x, y);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                plotA(x, y, r, g, b, 255);
            }
        }
    }
}

// A solid 5-point star (favourite marker), as a triangle fan from the centre. Unit
// vertices alternate outer/inner around an up-pointing star (screen coords, y down).
void drawStar(int cx, int cy, int outR, int r, int g, int b) {
    static const float ux[10] = { 0.000f,  0.588f,  0.951f,  0.951f,  0.588f,
                                  0.000f, -0.588f, -0.951f, -0.951f, -0.588f };
    static const float uy[10] = { -1.000f, -0.809f, -0.309f,  0.309f,  0.809f,
                                   1.000f,  0.809f,  0.309f, -0.309f, -0.809f };
    const float innR = outR * 0.45f;
    int px[10], py[10];
    for (int i = 0; i < 10; ++i) {
        const float R = (i % 2 == 0) ? (float)outR : innR;
        px[i] = cx + (int)(ux[i] * R + (ux[i] < 0 ? -0.5f : 0.5f));
        py[i] = cy + (int)(uy[i] * R + (uy[i] < 0 ? -0.5f : 0.5f));
    }
    for (int i = 0; i < 10; ++i) {
        const int j = (i + 1) % 10;
        fillTriangle(cx, cy, px[i], py[i], px[j], py[j], r, g, b);
    }
}

// Pixel width of an ASCII string at pixel-height @p pxh (for centring).
int textWidth(const char* s, float pxh) {
    const float scale = stbtt_ScaleForPixelHeight(&g_font, pxh);
    float w = 0.0f;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, *p, &advance, &lsb);
        w += advance * scale;
    }
    return (int)w;
}

// Draw an ASCII string with its top-left near (x,y), clipping horizontally to
// [clipL, clipR). Returns the end x pen position.
int drawTextClip(int x, int y, const char* s, int sr, int sg, int sb, float pxh,
                 int clipL, int clipR) {
    const float scale = stbtt_ScaleForPixelHeight(&g_font, pxh);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &lineGap);
    const int baseline = y + (int)(ascent * scale);

    float xpos = (float)x;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, *p, &advance, &lsb);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, *p, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0;
        const int gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 96 && gh <= 96) {
            static unsigned char gbuf[96 * 96];
            stbtt_MakeCodepointBitmap(&g_font, gbuf, gw, gh, gw, scale, scale, *p);
            const int gx = (int)(xpos + 0.5f) + x0;
            const int gy = baseline + y0;
            for (int j = 0; j < gh; ++j) {
                const int py = gy + j;
                if (py < 0 || py >= g_h) continue;
                u16* row = g_ras + py * g_w;
                const unsigned char* gr = gbuf + j * gw;
                for (int i = 0; i < gw; ++i) {
                    const int px = gx + i;
                    if (px < clipL || px >= clipR || px < 0 || px >= g_w) continue;
                    const int a = gr[i];
                    if (a) {
                        row[px] = blend5551(row[px], sr, sg, sb, a);
                    }
                }
            }
        }
        xpos += advance * scale;
    }
    return (int)xpos;
}

// Draw an ASCII string clipped only to the raster.
int drawText(int x, int y, const char* s, int sr, int sg, int sb, float pxh) {
    return drawTextClip(x, y, s, sr, sg, sb, pxh, 0, g_w);
}

// Draw an ASCII string with its cap-height box vertically centred on @p cy (using the
// real font metrics), so labels sit true-centre in bars/pills regardless of pixel size.
int drawTextVC(int x, int cy, const char* s, int sr, int sg, int sb, float pxh) {
    const float scale = stbtt_ScaleForPixelHeight(&g_font, pxh);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &lineGap);
    const int baseline = cy + (int)(0.36f * pxh);      // centre the ~0.72em cap box
    const int top = baseline - (int)(ascent * scale);
    return drawText(x, top, s, sr, sg, sb, pxh);
}

// Copy game name @p src into @p out, dropping a trailing ".jar".
void stripJar(char* out, int outCap, const char* src) {
    int n = 0;
    for (const char* p = src; *p != 0 && n < outCap - 1; ++p) {
        out[n++] = *p;
    }
    out[n] = '\0';
    if (n >= 4) {
        char* e = out + n - 4;
        if (e[0] == '.' &&
            (e[1] == 'j' || e[1] == 'J') &&
            (e[2] == 'a' || e[2] == 'A') &&
            (e[3] == 'r' || e[3] == 'R')) {
            out[n - 4] = '\0';
        }
    }
}

// Build a display label for game name @p src: drop a trailing ".jar" and truncate with
// ".." so it fits @p maxW pixels at @p pxh.
void makeLabel(char* out, int outCap, const char* src, float maxW, float pxh) {
    char base[128];
    stripJar(base, (int)sizeof(base), src);

    const float scale = stbtt_ScaleForPixelHeight(&g_font, pxh);
    float w = 0.0f;
    int o = 0;
    for (const unsigned char* p = (const unsigned char*)base; *p != 0 && o < outCap - 3; ++p) {
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, *p, &advance, &lsb);
        const float cw = advance * scale;
        if (w + cw > maxW) {
            out[o++] = '.';
            out[o++] = '.';
            out[o] = '\0';
            return;
        }
        out[o++] = (char)*p;
        w += cw;
    }
    out[o] = '\0';
}

// Nearest-neighbour downscale of an @p w x @p h RGBA source into a fresh @p dst x @p dst
// RGBA tile (caller frees). Returns null on OOM. Used for the (synchronous) title logo.
unsigned char* downscaleSquare(const unsigned char* src, int w, int h, int dst) {
    unsigned char* tile = (unsigned char*)malloc(dst * dst * 4);
    if (tile == 0) {
        return 0;
    }
    for (int dy = 0; dy < dst; ++dy) {
        const int sy = (h > 0) ? dy * h / dst : 0;
        for (int dx = 0; dx < dst; ++dx) {
            const int sx = (w > 0) ? dx * w / dst : 0;
            const unsigned char* s = src + (sy * w + sx) * 4;
            unsigned char* d = tile + (dy * dst + dx) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return tile;
}

// Decode + downscale the embedded PS2ME title logo once into g_titleIcon. (Mock for
// the java_logo.png brand asset until it is supplied.)
void loadTitleIcon() {
    if (g_titleIcon != 0) {
        return;
    }
    int w = 0, h = 0;
    unsigned char* src = MidletIcon::decodePng(g_ps2me_icon, (int)g_ps2me_icon_len, &w, &h);
    if (src == 0 || w <= 0 || h <= 0) {
        return;
    }
    g_titleIcon = downscaleSquare(src, w, h, TITLE_ICON);
    MidletIcon::release(src);
}

// Alpha-blend a @p tw x @p th RGBA8888 tile over the raster at (x,y).
void blitRGBA(const unsigned char* t, int tw, int th, int x, int y) {
    for (int dy = 0; dy < th; ++dy) {
        const int py = y + dy;
        if (py < 0 || py >= g_h) continue;
        u16* row = g_ras + py * g_w;
        const unsigned char* s = t + dy * tw * 4;
        for (int dx = 0; dx < tw; ++dx) {
            const int px = x + dx;
            if (px < 0 || px >= g_w) continue;
            const int a = s[dx * 4 + 3];
            if (a) {
                row[px] = blend5551(row[px], s[dx * 4 + 0], s[dx * 4 + 1], s[dx * 4 + 2], a);
            }
        }
    }
}

// Draw game i's icon at (x,y): the cached tile if the worker has decoded it, otherwise
// a lettered placeholder (and the request is queued by IconCache::draw).
void drawIcon(int x, int y, int i) {
    if (IconCache::instance().draw(i, g_ras, g_w, g_h, x, y)) {
        return;
    }
    fillRect(x, y, ICON, ICON, rgba5551(60, 78, 120));
    const char* nm = hal::GameStorage::instance().nameAt(i);
    char c[2];
    c[0] = (nm != 0 && nm[0] != 0) ? nm[0] : '?';
    c[1] = '\0';
    drawText(x + ICON / 2 - textWidth(c, 40.0f) / 2, y + ICON / 2 - 22,
             c, 220, 230, 245, 40.0f);
}

// One-time distinct-initials scan for the alphabet sidebar (names arrive sorted).
void buildInitials(int count) {
    if (g_initialsBuilt) {
        return;
    }
    g_initialCount = 0;
    for (int i = 0; i < count; ++i) {
        const char* nm = hal::GameStorage::instance().nameAt(i);
        char c = (nm != 0 && nm[0] != 0) ? nm[0] : '#';
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '#';
        bool found = false;
        for (int k = 0; k < g_initialCount; ++k) {
            if (g_initials[k] == c) { found = true; break; }
        }
        if (!found && g_initialCount < (int)sizeof(g_initials) - 1) {
            g_initials[g_initialCount++] = c;
        }
    }
    // Sort ascending so the sidebar reads in order regardless of storage order.
    for (int a = 1; a < g_initialCount; ++a) {
        const char key = g_initials[a];
        int j = a - 1;
        while (j >= 0 && g_initials[j] > key) {
            g_initials[j + 1] = g_initials[j];
            --j;
        }
        g_initials[j + 1] = key;
    }
    g_initialsBuilt = true;
}

// Uppercased first letter of game @p i ('#' if none), for the sidebar highlight.
char initialOf(int i) {
    const char* nm = hal::GameStorage::instance().nameAt(i);
    char c = (nm != 0 && nm[0] != 0) ? nm[0] : '#';
    if (c >= 'a' && c <= 'z') c -= 32;
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '#';
    return c;
}

// Case-insensitive ASCII compare (game-name ordering).
int ciStrcmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (ca == 0) return 0;
    }
}

// qsort comparator over game indices, by name, honouring g_sortMode (A-Z / Z-A).
int viewCmp(const void* pa, const void* pb) {
    const int ga = *(const int*)pa, gb = *(const int*)pb;
    const char* na = hal::GameStorage::instance().nameAt(ga);
    const char* nb = hal::GameStorage::instance().nameAt(gb);
    int c = ciStrcmp(na != 0 ? na : "", nb != 0 ? nb : "");
    return (g_sortMode == 1) ? -c : c;   // Z-A negates
}

const char* sortModeName(int mode) {
    return mode == 1 ? "Z-A" : "A-Z";
}

// Rebuild g_view for the active tab. SETTINGS: empty. FAVORITES: the favourited subset
// (sorted). ALL GAMES: a recent row (up to COLS most-recent games, padded to a full row
// with -1) followed by the sorted rest, excluding the recent ones (no duplicates).
void rebuildView(int activeTab, int count) {
    g_recentBand = 0;
    if (activeTab == 2) {
        g_viewCount = 0;
        return;
    }
    if (activeTab == 1) {
        g_viewCount = Favorites::instance().list(g_view, MAX_VIEW);
        if (g_viewCount > 1) qsort(g_view, g_viewCount, sizeof(int), viewCmp);
        return;
    }

    // ALL GAMES.
    int recent[COLS];
    const int rc = Settings::instance().showRecent()
                       ? Recent::instance().list(recent, COLS) : 0;
    g_recentBand = rc;

    int w = 0;
    if (rc > 0) {
        for (int i = 0; i < rc; ++i)   g_view[w++] = recent[i];
        for (int i = rc; i < COLS; ++i) g_view[w++] = -1;   // pad to a full row
    }

    static int rest[MAX_VIEW];
    int rn = 0;
    const int total = count < MAX_VIEW ? count : MAX_VIEW;
    for (int g = 0; g < total; ++g) {
        bool isRecent = false;
        for (int k = 0; k < rc; ++k) {
            if (recent[k] == g) { isRecent = true; break; }
        }
        if (!isRecent) rest[rn++] = g;
    }
    if (rn > 1) qsort(rest, rn, sizeof(int), viewCmp);
    for (int i = 0; i < rn && w < MAX_VIEW; ++i) g_view[w++] = rest[i];
    g_viewCount = w;
}

// The view index where the sorted (alphabetical) section starts, past the recent row.
int firstSorted() { return g_recentBand > 0 ? COLS : 0; }

// Selectable-slot helpers (the recent row may have empty -1 pad cells).
bool slotOk(int i)    { return i >= 0 && i < g_viewCount && g_view[i] >= 0; }
int  nextValid(int f) { for (int i = f; i < g_viewCount; ++i) if (g_view[i] >= 0) return i; return -1; }
int  prevValid(int f) { for (int i = f; i >= 0; --i)         if (g_view[i] >= 0) return i; return -1; }

// Index of game @p game's initial within the sorted sidebar list (0 if absent).
int initialIndexOf(int game) {
    const char c = initialOf(game);
    for (int k = 0; k < g_initialCount; ++k) {
        if (g_initials[k] == c) return k;
    }
    return 0;
}

// First view index (past the recent row) whose game starts with @p c, or -1 if none.
int jumpToInitial(char c) {
    for (int i = firstSorted(); i < g_viewCount; ++i) {
        if (g_view[i] >= 0 && initialOf(g_view[i]) == c) return i;
    }
    return -1;
}

// The session clock shown in the header ("HH:MM:SS", counting up from boot).
void formatClock(char* out, int cap) {
    javacall_int64 ms = hal::SystemClock::instance().elapsedMillis();
    if (ms < 0) ms = 0;
    const javacall_int64 s = ms / 1000;
    const int sec = (int)(s % 60);
    const int mn  = (int)((s / 60) % 60);
    const int hr  = (int)((s / 3600) % 100);
    snprintf(out, cap, "%02d:%02d:%02d", hr, mn, sec);
}

// --- Region renderers -------------------------------------------------------------

void drawHeader() {
    rectVGrad(0, 0, g_w, HEADER_H, 40, 66, 120, 22, 38, 74);
    for (int x = 0; x < g_w; ++x) plotA(x, 0, 96, 128, 186, 150);            // top sheen
    for (int x = 0; x < g_w; ++x) plotA(x, HEADER_H - 1, 12, 20, 40, 255);   // hairline

    int tx = MARGIN;
    if (g_titleIcon != 0) {
        blitRGBA(g_titleIcon, TITLE_ICON, TITLE_ICON, MARGIN, (HEADER_H - TITLE_ICON) / 2);
        tx = MARGIN + TITLE_ICON + 10;
    }
    drawTextVC(tx, HEADER_H / 2, "PS2ME", 240, 246, 255, 26.0f);

    // Right-aligned two-line clock ("SYSTEM" over the running MM:SS:mmm), as in the kit.
    char clk[16];
    formatClock(clk, (int)sizeof(clk));
    const int lw = textWidth("SYSTEM", 11.0f);
    drawText(g_w - MARGIN - lw, 3, "SYSTEM", 150, 185, 220, 11.0f);
    const int cw = textWidth(clk, 16.0f);
    drawText(g_w - MARGIN - cw, 15, clk, 210, 226, 246, 16.0f);
}

// A small solid horizontal arrowhead pointing left/right (tab movement legend).
void chevronH(int tipX, int cy, int size, bool left, int r, int g, int b) {
    for (int i = 0; i <= size; ++i) {
        const int x = left ? tipX + i : tipX - i;
        for (int j = -i; j <= i; ++j) plotA(x, cy + j, r, g, b, 255);
    }
}

// An "L1"/"R1" shoulder-button badge flanking the tab row, with an outward arrow.
void drawTabHint(int edgeX, int cy, const char* label, bool leftSide) {
    const float px = 13.0f;
    const int tw = textWidth(label, px);
    const int bw = tw + 12, bh = 20, arrow = 5, gap = 4;
    const int bx = leftSide ? edgeX - bw : edgeX;
    roundRectVGrad(bx, cy - bh / 2, bw, bh, 6, 44, 64, 104, 26, 40, 76);
    drawTextVC(bx + (bw - tw) / 2, cy, label, 185, 208, 232, px);
    if (leftSide) {
        chevronH(bx - gap - arrow, cy, arrow, true,  130, 180, 225);   // points left
    } else {
        chevronH(bx + bw + gap + arrow, cy, arrow, false, 130, 180, 225); // points right
    }
}

void drawTabs(int activeTab) {
    static const char* labels[3] = { "ALL GAMES", "FAVORITES", "SETTINGS" };
    const int areaX = GRID_X0 - 4;
    const int areaW = SCROLL_X - areaX;
    const int tabW  = 118, gap = 10;
    const int total = 3 * tabW + 2 * gap;
    const int x0    = areaX + (areaW - total) / 2;

    for (int i = 0; i < 3; ++i) {
        const int tx = x0 + i * (tabW + gap);
        const bool act = (i == activeTab);
        if (act) {
            roundRectFillA(tx - 3, TAB_Y - 3, tabW + 6, TAB_H + 6, 11, 70, 200, 255, 55);
            roundRectFillA(tx - 1, TAB_Y - 1, tabW + 2, TAB_H + 2, 10, 120, 230, 255, 255);
            roundRectVGrad(tx, TAB_Y, tabW, TAB_H, 9, 46, 96, 150, 30, 64, 110);
        } else {
            roundRectVGrad(tx, TAB_Y, tabW, TAB_H, 9, 44, 64, 104, 26, 40, 76);
        }
        char lbl[32];
        snprintf(lbl, (int)sizeof(lbl), "[ %s ]", labels[i]);
        const int lw = textWidth(lbl, 15.0f);
        const int tr = act ? 245 : 150;
        const int tg = act ? 250 : 178;
        const int tb = act ? 255 : 208;
        drawTextVC(tx + (tabW - lw) / 2, TAB_Y + TAB_H / 2, lbl, tr, tg, tb, 15.0f);
    }

    // Movement legend: shoulder-button hints flanking the tab row.
    const int hy = TAB_Y + TAB_H / 2;
    drawTabHint(x0 - 10, hy, "L1", true);
    drawTabHint(x0 + total + 10, hy, "R1", false);

    // Current sort mode, right-aligned on the tab row.
    char sortLbl[24];
    snprintf(sortLbl, (int)sizeof(sortLbl), "SORT: %s", sortModeName(g_sortMode));
    const int sw = textWidth(sortLbl, 13.0f);
    drawTextVC(g_w - MARGIN - sw, hy, sortLbl, 150, 185, 220, 13.0f);
}

// @p focus is true when the d-pad has moved into the alphabet column; @p focusSel is
// then the highlighted letter index. Otherwise the current game's initial is shown.
void drawSidebar(int curGame, bool focus, int focusSel) {
    const int y0 = CONTENT_Y;
    const int h  = CONTENT_BOTTOM - CONTENT_Y;
    panel(SIDE_X, y0, SIDE_W, h, 6);
    if (g_initialCount <= 0) {
        return;
    }

    // Up/down chevrons: the column scrolls when focused (d-pad up/down).
    const int cxm = SIDE_X + SIDE_W / 2;
    fillTriangle(cxm, y0 + 4, cxm - 4, y0 + 9, cxm + 4, y0 + 9, 110, 170, 215);
    fillTriangle(cxm - 4, y0 + h - 9, cxm + 4, y0 + h - 9, cxm, y0 + h - 4, 110, 170, 215);

    const int listTop = y0 + 13;
    const int listH   = h - 26;
    const int n = g_initialCount;

    // Highlighted entry: the focused letter, or the current game's initial.
    int hi = -1;
    if (focus) {
        hi = focusSel < 0 ? 0 : (focusSel >= n ? n - 1 : focusSel);
    } else if (curGame >= 0) {
        hi = initialIndexOf(curGame);
    }

    const int rowH = 15;
    int maxShow = listH / rowH;
    if (maxShow < 1) maxShow = 1;
    const int show = n < maxShow ? n : maxShow;

    int start = (hi >= 0 ? hi : 0) - show / 2;
    if (start < 0) start = 0;
    if (start + show > n) start = n - show;

    int y = listTop + (listH - show * rowH) / 2;
    for (int k = start; k < start + show; ++k) {
        const bool act = (k == hi);
        char s[2] = { g_initials[k], 0 };
        const int lw = textWidth(s, 13.0f);
        const int lx = SIDE_X + (SIDE_W - lw) / 2;
        if (act && focus) {
            roundRectFillA(SIDE_X - 2, y - 3, SIDE_W + 4, rowH + 4, 5, 70, 200, 255, 75); // glow
            roundRectFillA(SIDE_X + 1, y - 1, SIDE_W - 2, rowH, 4, 120, 235, 255, 255);
            drawText(lx, y, s, 16, 34, 62, 13.0f);
        } else if (act) {
            roundRectFillA(SIDE_X + 1, y - 1, SIDE_W - 2, rowH, 4, 90, 215, 255, 255);
            drawText(lx, y, s, 18, 38, 68, 13.0f);
        } else {
            drawText(lx, y, s, 150, 180, 215, 13.0f);
        }
        y += rowH;
    }
}

void drawScrollbar(int viewCount, int topRow) {
    const int trackY = CONTENT_Y;
    const int trackH = CONTENT_BOTTOM - CONTENT_Y;
    roundRectVGrad(SCROLL_X, trackY, 6, trackH, 3, 20, 32, 60, 14, 22, 44);
    const int totalRows = (viewCount + COLS - 1) / COLS;
    if (totalRows > ROWS) {
        int thumbH = trackH * ROWS / totalRows;
        if (thumbH < 16) thumbH = 16;
        const int maxTop = totalRows - ROWS;
        const int thumbY = trackY + (trackH - thumbH) * topRow / (maxTop > 0 ? maxTop : 1);
        roundRectVGrad(SCROLL_X, thumbY, 6, thumbH, 3, 100, 190, 240, 50, 110, 180);
    }
}

void drawGrid(int viewCount, int selected, int topRow, const int* view, bool gridFocused) {
    // Recent row: an amber "shelf" behind the first row, with a RECENT tag in any
    // unused cells. Drawn before the tiles so it shows through the gaps and pad cells.
    if (g_recentBand > 0 && topRow == 0) {
        const int rowW = COLS * GRID_CELLW + (COLS - 1) * GRID_GAP;
        roundRectVGrad(GRID_X0 - 2, GRID_Y0 - 2, rowW + 4, GRID_ROWSTRIDE - 2, 10,
                       98, 80, 36, 60, 48, 20);
        if (g_recentBand < COLS) {
            const int ex0 = GRID_X0 + g_recentBand * (GRID_CELLW + GRID_GAP);
            const int ex1 = GRID_X0 + rowW;
            const int tw  = textWidth("RECENT", 16.0f);
            drawTextVC(ex0 + (ex1 - ex0 - tw) / 2, GRID_Y0 - 2 + (GRID_ROWSTRIDE - 2) / 2,
                       "RECENT", 214, 182, 112, 16.0f);
        }
    }

    const int firstIdx = topRow * COLS;
    const int lastIdx  = (topRow + ROWS) * COLS;   // exclusive
    for (int i = firstIdx; i < viewCount && i < lastIdx; ++i) {
        const int game  = view[i];
        if (game < 0) {
            continue;   // empty recent-row pad cell (the amber shelf shows through)
        }
        const int r = i / COLS;
        const int c = i % COLS;
        const int cellX = GRID_X0 + c * (GRID_CELLW + GRID_GAP);
        const int cellY = GRID_Y0 + (r - topRow) * GRID_ROWSTRIDE;
        const int cellH = GRID_ROWSTRIDE - 4;
        const bool sel  = (i == selected);

        if (sel && gridFocused) {
            drawSelection(cellX + 2, cellY, GRID_CELLW - 4, cellH, 8);
        } else if (sel) {
            // Focus is on the sidebar: show a muted (grey) frame where we'll return.
            roundRectFillA(cellX, cellY - 2, GRID_CELLW, cellH + 4, 10, 120, 140, 170, 255);
            roundRectVGrad(cellX + 2, cellY, GRID_CELLW - 4, cellH, 8, 40, 60, 104, 24, 40, 76);
        } else {
            roundRectVGrad(cellX + 2, cellY, GRID_CELLW - 4, cellH, 8, 40, 60, 104, 24, 40, 76);
        }

        const int iconX = cellX + (GRID_CELLW - ICON) / 2;
        const int iconY = cellY + 3;
        drawIcon(iconX, iconY, game);

        // Favourite marker: a gold star (dark halo) at the tile's top-right.
        if (Favorites::instance().isFavorite(game)) {
            const int sx = cellX + GRID_CELLW - 12, sy = cellY + 11;
            drawStar(sx, sy, 7, 40, 34, 10);
            drawStar(sx, sy, 6, 255, 205, 60);
        }

        const char* nm = hal::GameStorage::instance().nameAt(game);
        if (nm == 0) {
            nm = "?";
        }
        const int nameY = iconY + ICON + 2;   // sits inside the tile (no overflow)
        const int pad   = 4;
        const int clipL = cellX + pad;
        const int clipR = cellX + GRID_CELLW - pad;
        const int avail = GRID_CELLW - 2 * pad;
        const int tr = sel ? 255 : 210;
        const int tg = sel ? 255 : 220;
        const int tb = sel ? 255 : 235;

        char base[128];
        stripJar(base, (int)sizeof(base), nm);
        const int fullW = textWidth(base, NAME_PX);

        if (!sel || fullW <= avail) {
            char label[48];
            makeLabel(label, (int)sizeof(label), nm, (float)avail, NAME_PX);
            const int tw = textWidth(label, NAME_PX);
            drawText(cellX + (GRID_CELLW - tw) / 2, nameY, label, tr, tg, tb, NAME_PX);
        } else {
            // Active item whose name overflows: auto-shift (marquee) the full name.
            const int period = fullW + 32;
            int t = (int)g_marqueeTick - 45;
            if (t < 0) t = 0;
            const int off = (t / 2) % period;
            const int x0  = clipL - off;
            drawTextClip(x0,          nameY, base, tr, tg, tb, NAME_PX, clipL, clipR);
            drawTextClip(x0 + period, nameY, base, tr, tg, tb, NAME_PX, clipL, clipR);
        }
    }
}

void drawDetails(int viewCount, int selected, const int* view) {
    const int px = DET_X, py = CONTENT_Y, pw = DET_W, ph = CONTENT_BOTTOM - CONTENT_Y;
    panel(px, py, pw, ph, 10);
    drawText(px + 10, py + 8, "GAME DETAILS", 150, 185, 220, 12.0f);
    if (viewCount <= 0) {
        return;
    }
    const int game = view[selected];

    // Vertically centre the preview + name + counter block in the panel body (below
    // the "GAME DETAILS" caption), so the panel reads balanced rather than top-heavy.
    const int prev = pw - 26;
    const int gap1 = 12, nameH = 16, gap2 = 8, infoH = 14;
    const int blockH = prev + gap1 + nameH + gap2 + infoH;
    const int regTop = py + 26, regBot = py + ph - 8;
    int top = regTop + ((regBot - regTop) - blockH) / 2;
    if (top < regTop) top = regTop;

    const int prevX = px + (pw - prev) / 2;
    const int prevY = top;

    // Recessed frame behind the preview.
    roundRectFillA(prevX - 3, prevY - 3, prev + 6, prev + 6, 8, 120, 150, 205, 255);
    roundRectVGrad(prevX - 2, prevY - 2, prev + 4, prev + 4, 7, 28, 46, 86, 18, 32, 64);

    if (!IconCache::instance().drawScaled(game, g_ras, g_w, g_h, prevX, prevY, prev)) {
        char c[2] = { initialOf(game), 0 };
        drawText(prevX + prev / 2 - textWidth(c, 64.0f) / 2, prevY + prev / 2 - 36,
                 c, 210, 224, 244, 64.0f);
    }

    const char* nm = hal::GameStorage::instance().nameAt(game);
    char lbl[40];
    makeLabel(lbl, (int)sizeof(lbl), nm != 0 ? nm : "?", (float)(pw - 16), 16.0f);
    const int lw = textWidth(lbl, 16.0f);
    drawText(px + (pw - lw) / 2, prevY + prev + gap1, lbl, 235, 242, 252, 16.0f);

    int pos = 0, total = 0;
    for (int i = 0; i < viewCount; ++i) {
        if (view[i] >= 0) { ++total; if (i <= selected) ++pos; }
    }
    char info[32];
    snprintf(info, (int)sizeof(info), "%d / %d", pos, total);
    const int iw = textWidth(info, 13.0f);
    drawText(px + (pw - iw) / 2, prevY + prev + gap1 + nameH + gap2, info, 150, 180, 215, 13.0f);
}

// Context-aware button legend: only the actions available in the current context are
// shown (e.g. no BACK at the root, no FAVORITE/SORT on SETTINGS or while the alphabet
// column is focused). Glyph kind: 0=cross, 1=circle, 2=triangle, 3=square.
void drawFooter(int activeTab, bool sidebarFocus) {
    rectVGrad(0, FOOTER_Y, g_w, FOOTER_H, 26, 42, 78, 14, 24, 50);
    for (int x = 0; x < g_w; ++x) plotA(x, FOOTER_Y, 60, 90, 140, 255);   // top hairline
    const int cy = FOOTER_Y + FOOTER_H / 2;

    const bool canBack = sidebarFocus || activeTab != 0;   // somewhere to go back to
    int kinds[4];
    const char* labels[4];
    int n = 0;
    if (activeTab == 2) {                       // SETTINGS
        kinds[n] = 0; labels[n] = "SELECT"; ++n;
        if (canBack) { kinds[n] = 1; labels[n] = "BACK"; ++n; }
    } else if (sidebarFocus) {                  // alphabet column focused
        kinds[n] = 0; labels[n] = "JUMP"; ++n;
        kinds[n] = 1; labels[n] = "BACK"; ++n;
    } else {                                     // ALL GAMES / FAVORITES grid
        kinds[n] = 0; labels[n] = "LAUNCH"; ++n;
        if (canBack) { kinds[n] = 1; labels[n] = "BACK"; ++n; }
        kinds[n] = 2; labels[n] = "FAVORITE"; ++n;
        kinds[n] = 3; labels[n] = "SORT BY..."; ++n;
    }

    const int glyphW = 16;   // glyph column (centre at +7) before the label
    const int itemGap = 26;

    int total = 0;
    for (int i = 0; i < n; ++i) {
        total += glyphW + textWidth(labels[i], 15.0f);
        if (i < n - 1) total += itemGap;
    }
    int x = (g_w - total) / 2;
    if (x < MARGIN) x = MARGIN;

    for (int i = 0; i < n; ++i) {
        const int gcx = x + 7;
        switch (kinds[i]) {
            case 0: glyphCross(gcx,    cy, 7, 100, 150, 235); break;
            case 1: glyphCircle(gcx,   cy, 7, 235,  92,  92); break;
            case 2: glyphTriangle(gcx, cy, 7,  92, 216, 150); break;
            case 3: glyphSquare(gcx,   cy, 7, 230, 112, 190); break;
        }
        drawTextVC(x + glyphW, cy, labels[i], 232, 240, 250, 15.0f);
        x += glyphW + textWidth(labels[i], 15.0f) + itemGap;
    }
}

// A single centred line across the main content area (empty tab placeholders).
void drawContentMessage(const char* msg) {
    const int left = GRID_X0, right = DET_X + DET_W;
    const int cy = (CONTENT_Y + CONTENT_BOTTOM) / 2;
    const int w = textWidth(msg, 18.0f);
    drawTextVC(left + (right - left - w) / 2, cy, msg, 150, 185, 220, 18.0f);
}

const int SETTINGS_COUNT = 4;

// The SETTINGS tab: a vertical list of options (two actions + two toggles). @p sel is
// the highlighted row.
void drawSettings(int sel) {
    const int left  = GRID_X0;
    const int right = DET_X + DET_W;
    const int rowH = 42, gap = 12;
    const int totalH = SETTINGS_COUNT * rowH + (SETTINGS_COUNT - 1) * gap;
    int y = CONTENT_Y + ((CONTENT_BOTTOM - CONTENT_Y) - totalH) / 2;

    for (int i = 0; i < SETTINGS_COUNT; ++i) {
        const bool s = (i == sel);
        if (s) {
            roundRectFillA(left - 2, y - 2, (right - left) + 4, rowH + 4, 10, 90, 215, 255, 255);
            roundRectVGrad(left, y, right - left, rowH, 9, 46, 88, 148, 30, 54, 100);
        } else {
            roundRectVGrad(left, y, right - left, rowH, 9, 40, 60, 104, 26, 42, 78);
        }

        const char* label = "";
        char value[24];
        value[0] = '\0';
        switch (i) {
            case 0: label = "Clear recent games";
                    snprintf(value, sizeof(value), "%d saved", Recent::instance().count());
                    break;
            case 1: label = "Clear favorites";
                    snprintf(value, sizeof(value), "%d saved", Favorites::instance().count());
                    break;
            case 2: label = "Show recent row";
                    snprintf(value, sizeof(value), "%s", Settings::instance().showRecent() ? "On" : "Off");
                    break;
            case 3: label = "Default sort";
                    snprintf(value, sizeof(value), "%s", Settings::instance().defaultSort() == 1 ? "Z-A" : "A-Z");
                    break;
        }
        const int cy = y + rowH / 2;
        drawTextVC(left + 14, cy, label, s ? 255 : 220, s ? 255 : 228, 255, 17.0f);
        const int vw = textWidth(value, 15.0f);
        drawTextVC(right - 14 - vw, cy, value, s ? 235 : 150, s ? 245 : 185, 255, 15.0f);
        y += rowH + gap;
    }

    const char* hint = "X: select / toggle";
    const int hw = textWidth(hint, 13.0f);
    drawText(left + (right - left - hw) / 2, y + 2, hint, 150, 180, 215, 13.0f);
}

void render(int viewCount, int selected, int topRow, int activeTab, const int* view,
            bool sidebarFocus, int sidebarSel, int settingsSel) {
    memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));   // baked background
    drawHeader();
    drawTabs(activeTab);
    const int curGame = viewCount > 0 ? view[selected] : -1;
    drawSidebar(curGame, sidebarFocus, sidebarSel);
    drawFooter(activeTab, sidebarFocus);

    if (activeTab == 2) {                       // SETTINGS
        drawSettings(settingsSel);
        return;
    }
    if (activeTab == 1 && viewCount <= 0) {     // FAVORITES: empty
        drawContentMessage("No favorites yet -- mark games with TRIANGLE");
        return;
    }
    if (activeTab == 0 && viewCount <= 0) {
        // No games: show the storage-resolution trace in the grid area (the EE console
        // is invisible on real hardware booted standalone from USB).
        drawText(GRID_X0, GRID_Y0 + 2, "No games found. Diagnostics:", 235, 120, 120, NAME_PX + 2);
        const char* p = Ps2Storage::instance().diagText();
        int ly = GRID_Y0 + 28;
        char lineBuf[160];
        int li = 0;
        for (;; ++p) {
            if (*p == '\n' || *p == '\0') {
                lineBuf[li] = '\0';
                if (li > 0) {
                    drawText(GRID_X0, ly, lineBuf, 200, 214, 235, 15.0f);
                    ly += 19;
                }
                li = 0;
                if (*p == '\0') break;
            } else if (li < (int)sizeof(lineBuf) - 1) {
                lineBuf[li++] = *p;
            }
        }
        return;
    }

    drawScrollbar(viewCount, topRow);
    drawDetails(viewCount, selected, view);
    drawGrid(viewCount, selected, topRow, view, !sidebarFocus);
}

// Bake the static background (vertical gradient + radial vignette) into g_bg once, so
// each frame just memcpy's it rather than recomputing the per-pixel vignette.
void buildBackground() {
    if (g_bg == 0) {
        return;
    }
    const int cx = g_w / 2, cy = g_h / 2;
    const int dxs = cx * cx / 45 + 1;
    const int dys = cy * cy / 45 + 1;
    for (int y = 0; y < g_h; ++y) {
        const int t = (g_h > 1) ? (y * 255 / (g_h - 1)) : 0;
        const int br = 18 + (28 - 18) * t / 255;
        const int bg = 28 + (46 - 28) * t / 255;
        const int bb = 54 + (88 - 54) * t / 255;
        const int dy = y - cy;
        const int vy = (dy * dy) / dys;
        u16* row = g_bg + y * g_w;
        for (int x = 0; x < g_w; ++x) {
            const int dx = x - cx;
            int f = 100 - ((dx * dx) / dxs + vy);
            if (f < 58) f = 58;
            if (f > 100) f = 100;
            row[x] = rgba5551(br * f / 100, bg * f / 100, bb * f / 100);
        }
    }
}

// Allocate the DMA-aligned native-resolution raster (+ baked background) and bring up
// the shared GS.
bool ensureVideo() {
    if (g_ras == 0) {
        const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(u16);
        g_ras = (u16*)memalign(128, bytes);
        g_bg  = (u16*)memalign(128, bytes);
        if (g_ras == 0 || g_bg == 0) {
            return false;
        }
        g_w = SCREEN_W;
        g_h = SCREEN_H;
        buildBackground();
    }
    return GsDisplay::instance().init();
}

// --- On-screen launch log (real-HW diagnostics) -----------------------------------
// The VM's stdout (System.out via javacall_print -> StdoutSink) is teed here during
// the launch window so the [Launcher] milestones + any exception traces render on the
// native GS -- the only console left once the USB IOP reset kills the SIF tty. Bytes
// are buffered into lines; each newline pushes a line and repaints. Bounded, no heap.
const int LOG_TITLE_H  = 46;
const int LOG_MAX_LINES = 22;    // fits 640x448 below the title at 17px stride
const int LOG_LINE_CAP  = 96;    // long lines just clip to the raster width

char g_logLines[LOG_MAX_LINES][LOG_LINE_CAP];
int  g_logCount = 0;             // completed lines stored
char g_logBuild[LOG_LINE_CAP];   // line currently being assembled
int  g_logCur   = 0;             // chars in g_logBuild
bool g_logOn    = false;

void logRender() {
    if (g_ras == 0 || !g_fontOk) {
        return;
    }
    fillRect(0, 0, g_w, g_h, rgba5551(16, 18, 28));
    drawText(MARGIN, 8, "PS2ME - launching game", 210, 220, 235, 30.0f);
    int ly = LOG_TITLE_H + 8;
    for (int i = 0; i < g_logCount; ++i) {
        drawText(MARGIN, ly, g_logLines[i], 190, 205, 225, 15.0f);
        ly += 17;
    }
    if (g_logCur > 0 && g_logCount < LOG_MAX_LINES) {
        g_logBuild[g_logCur] = '\0';          // show the partial line in flight too
        drawText(MARGIN, ly, g_logBuild, 190, 205, 225, 15.0f);
    }
    GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
}

void logPushLine() {
    g_logBuild[g_logCur] = '\0';
    if (g_logCount >= LOG_MAX_LINES) {         // scroll: drop the oldest line
        for (int i = 1; i < LOG_MAX_LINES; ++i) {
            int k = 0;
            for (; g_logLines[i][k] != '\0' && k < LOG_LINE_CAP - 1; ++k) {
                g_logLines[i - 1][k] = g_logLines[i][k];
            }
            g_logLines[i - 1][k] = '\0';
        }
        g_logCount = LOG_MAX_LINES - 1;
    }
    int k = 0;
    for (; g_logBuild[k] != '\0' && k < LOG_LINE_CAP - 1; ++k) {
        g_logLines[g_logCount][k] = g_logBuild[k];
    }
    g_logLines[g_logCount][k] = '\0';
    g_logCount++;
    g_logCur = 0;
}

// Frame pacing: block until the next field. Uses the vsync-interrupt semaphore when
// it is installed (so the icon worker runs meanwhile); falls back to a busy vsync.
void waitFrame() {
    if (g_vsyncCb >= 0) {
        WaitSema(g_vsyncSema);
    } else {
        graph_wait_vsync();
    }
}

} // namespace

Ps2Frontend& Ps2Frontend::instance() {
    static Ps2Frontend inst;
    return inst;
}

void Ps2Frontend::logEnable(bool on) {
    g_logOn = on;
    if (on) {                     // start each launch window with a clean trace
        g_logCount = 0;
        g_logCur   = 0;
    }
}

void Ps2Frontend::logWrite(const char* s, int len) {
    if (!g_logOn || s == 0) {
        return;
    }
    bool sawNewline = false;
    for (int i = 0; i < len; ++i) {
        const char c = s[i];
        if (c == '\n') {
            logPushLine();
            sawNewline = true;
        } else if (c == '\r') {
            // ignore CR
        } else if (g_logCur < LOG_LINE_CAP - 1) {
            g_logBuild[g_logCur++] = (c >= 32 && c < 127) ? c : ' ';
        }
    }
    if (sawNewline) {
        logRender();              // repaint once per completed line (bounded)
    }
}

int Ps2Frontend::pick() {
    logEnable(false);             // menu owns the screen again; stop the launch trace
    if (!initFont() || !ensureVideo()) {
        return -1;
    }
    loadTitleIcon();

    int count = hal::GameStorage::instance().list();
    if (count < 0) {
        count = 0;
    }

    // First fullscreen present also allocates the GS texture; do it before the worker
    // starts, and show a brief splash while the first icons decode.
    memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));
    drawText(MARGIN, g_h / 2 - 16, "Loading...", 210, 224, 244, 24.0f);
    GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);

    buildInitials(count);         // alphabet sidebar contents (once)
    Favorites::instance().load(count);   // persisted favourites -> game indices
    Recent::instance().load(count);      // recently-launched games -> game indices
    Settings::instance().load();         // launcher preferences
    g_sortMode = Settings::instance().defaultSort();   // startup sort order

    // The pad backend is shared with the (not-yet-running) VM's Keypad, so we open
    // the controller exactly once; reading it here drives no VM code path. Do the
    // (SIF-heavy) open() NOW, while we're still single-threaded -- once the icon
    // worker exists, its host: I/O would collide with the pad's SIF RPC.
    hal::IPad* pad = hal::Keypad::instance().pad();
    if (pad != 0) {
        pad->ensureReady();   // fires Ps2Pad::open() (SifInitRpc + load pad IRX)
    }

    // Install the vsync pacing handler + semaphore, then start the background icon
    // worker (which runs at a lower priority, in the idle time between frames).
    if (g_vsyncSema < 0) {
        ee_sema_t s;
        s.count = 0; s.max_count = 1; s.init_count = 0;
        s.wait_threads = 0; s.attr = 0; s.option = 0;
        g_vsyncSema = CreateSema(&s);
    }
    g_vsyncCb = graph_add_vsync_handler(onVsync);
    IconCache::instance().begin(count, ICON);

    int selected = 0;
    int topRow = 0;
    int lastSel = -1;
    int lastTop = -1;
    int lastTab = -1;
    int lastClockSec = -1;
    int chosen = -1;
    int activeTab = 0;            // 0=ALL GAMES, 1=FAVORITES, 2=SETTINGS (L1/R1 switch)
    bool sidebarFocus = false;    // d-pad moved into the alphabet column
    int  sidebarSel = 0;          // highlighted letter while focused
    bool lastSbFocus = false;
    int  lastSbSel = -1;
    int  settingsSel = 0;         // highlighted SETTINGS row
    int  lastSettingsSel = -1;
    bool firstFrame = true;
    hal::PadButtons prev;         // all-false initial snapshot
    rebuildView(activeTab, count);   // initial view (all games)

    while (chosen < 0) {
        // Poll the pad every field so input stays responsive even when we skip the
        // (expensive) redraw below.
        bool favToggled = false;
        bool sortChanged = false;
        bool settingsChanged = false;
        hal::PadButtons b;
        if (pad != 0 && pad->ensureReady() && pad->read(&b)) {
            // Tab switching (L1/R1): resets the view + selection + sidebar/settings focus.
            if (b.r1 && !prev.r1 && activeTab < 2) {
                activeTab++; rebuildView(activeTab, count);
                selected = 0; topRow = 0; sidebarFocus = false; settingsSel = 0;
            }
            if (b.l1 && !prev.l1 && activeTab > 0) {
                activeTab--; rebuildView(activeTab, count);
                selected = 0; topRow = 0; sidebarFocus = false; settingsSel = 0;
            }
            // Back (Circle): unfocus the sidebar, else return to ALL GAMES. At the root
            // (ALL GAMES with nothing focused) it does nothing.
            if (b.circle && !prev.circle) {
                if (sidebarFocus) {
                    sidebarFocus = false;
                } else if (activeTab != 0) {
                    activeTab = 0; rebuildView(activeTab, count);
                    selected = 0; topRow = 0; settingsSel = 0;
                }
            }
            if (activeTab == 2) {
                // Settings navigation: up/down move; cross activates the row.
                if (b.up   && !prev.up   && settingsSel > 0)                   { settingsSel--; }
                if (b.down && !prev.down && settingsSel < SETTINGS_COUNT - 1)  { settingsSel++; }
                if (b.cross && !prev.cross) {
                    switch (settingsSel) {
                        case 0: Recent::instance().clear(); break;
                        case 1: Favorites::instance().clear(); break;
                        case 2: Settings::instance().setShowRecent(
                                    !Settings::instance().showRecent()); break;
                        case 3: {
                            const int m = Settings::instance().defaultSort() ^ 1;
                            Settings::instance().setDefaultSort(m);
                            g_sortMode = m;
                            break;
                        }
                    }
                    settingsChanged = true;
                }
            } else if (g_viewCount > 0 && (activeTab == 0 || activeTab == 1)) {
                if (sidebarFocus) {
                    // Alphabet column focused: up/down pick a letter; cross/right jumps
                    // to it and returns to the grid; left cancels.
                    if (b.up   && !prev.up   && sidebarSel > 0)                  { sidebarSel--; }
                    if (b.down && !prev.down && sidebarSel < g_initialCount - 1) { sidebarSel++; }
                    if ((b.cross && !prev.cross) || (b.right && !prev.right)) {
                        const int j = jumpToInitial(g_initials[sidebarSel]);
                        if (j >= 0) selected = j;
                        sidebarFocus = false;
                    }
                    if (b.left && !prev.left) {
                        sidebarFocus = false;   // cancel back to the grid
                    }
                } else {
                    // Grid navigation (skips the recent row's empty pad cells).
                    if (b.right && !prev.right) {
                        const int t = nextValid(selected + 1);
                        if (t >= 0) selected = t;
                    }
                    if (b.left && !prev.left) {
                        if (selected % COLS == 0) {   // at the left edge -> focus the sidebar
                            sidebarFocus = true;
                            sidebarSel = initialIndexOf(g_view[selected]);
                        } else {
                            const int t = prevValid(selected - 1);
                            if (t >= 0) selected = t;
                        }
                    }
                    if (b.down && !prev.down) {
                        const int t = selected + COLS;
                        if (t < g_viewCount && g_view[t] >= 0) selected = t;
                    }
                    if (b.up && !prev.up) {
                        int t = selected - COLS;
                        if (t >= 0) {
                            if (g_view[t] < 0) t = prevValid(t);   // snap onto the recent row
                            if (t >= 0) selected = t;
                        }
                    }
                    if (b.cross && !prev.cross) { chosen = g_view[selected]; }
                    if (b.triangle && !prev.triangle) {
                        Favorites::instance().toggle(g_view[selected]);
                        favToggled = true;
                        if (activeTab == 1) {   // a toggled-off game leaves the favourites view
                            rebuildView(activeTab, count);
                            if (selected >= g_viewCount) {
                                selected = g_viewCount > 0 ? g_viewCount - 1 : 0;
                            }
                        }
                    }
                    if (b.square && !prev.square) {
                        // Cycle the sort order, keeping the same game selected.
                        const int curGame = g_view[selected];
                        g_sortMode = (g_sortMode + 1) % SORT_COUNT;
                        rebuildView(activeTab, count);
                        selected = 0;
                        for (int i = 0; i < g_viewCount; ++i) {
                            if (g_view[i] == curGame) { selected = i; break; }
                        }
                        topRow = 0;
                        sortChanged = true;
                    }
                    // Page up/down (L2/R2): move the selection by a whole page.
                    if (b.r2 && !prev.r2) {
                        int t = selected + COLS * ROWS;
                        if (t > g_viewCount - 1) t = g_viewCount - 1;
                        if (!slotOk(t)) t = prevValid(t);
                        if (t >= 0) selected = t;
                    }
                    if (b.l2 && !prev.l2) {
                        int t = selected - COLS * ROWS;
                        if (t < 0) t = 0;
                        if (!slotOk(t)) t = nextValid(t);
                        if (t >= 0) selected = t;
                    }
                }
            }
            prev = b;
        }

        // Keep the selected row within the visible window.
        const int selRow = (g_viewCount > 0) ? selected / COLS : 0;
        if (selRow < topRow) {
            topRow = selRow;
        }
        if (selRow >= topRow + ROWS) {
            topRow = selRow - ROWS + 1;
        }

        const bool moved = (selected != lastSel) || (topRow != lastTop);
        if (moved) {
            g_marqueeTick = 0;   // restart the marquee for the newly active item
            IconCache::instance().pause();   // back the worker off so nav stays smooth
        }

        // Does the active item's name overflow its cell? Only then must we keep
        // redrawing every field to animate the marquee (grid tabs only).
        bool marquee = false;
        if (g_viewCount > 0 && !moved && !sidebarFocus && (activeTab == 0 || activeTab == 1)) {
            const char* nm = hal::GameStorage::instance().nameAt(g_view[selected]);
            char base[128];
            stripJar(base, (int)sizeof(base), nm != 0 ? nm : "?");
            marquee = textWidth(base, NAME_PX) > (GRID_CELLW - 8);
        }

        // Render on demand: only when something changed -- navigation, a tab switch, a
        // sidebar-focus change, a favourite toggle, a marquee, a decoded icon, or a
        // new clock second.
        const bool tabChanged = (activeTab != lastTab);
        const bool sbChanged  = (sidebarFocus != lastSbFocus) || (sidebarSel != lastSbSel);
        const bool setChanged = (settingsSel != lastSettingsSel) || settingsChanged;
        const bool dirty = IconCache::instance().takeDirty();
        const int nowSec = (int)(hal::SystemClock::instance().elapsedMillis() / 1000);
        const bool clockTick = (nowSec != lastClockSec);
        if (firstFrame || moved || tabChanged || sbChanged || setChanged ||
            favToggled || sortChanged || marquee || dirty || clockTick) {
            if (marquee) {
                g_marqueeTick++;
            }
            IconCache::instance().tick();
            render(g_viewCount, selected, topRow, activeTab, g_view,
                   sidebarFocus, sidebarSel, settingsSel);
            GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
            lastSel         = selected;
            lastTop         = topRow;
            lastTab         = activeTab;
            lastSbFocus     = sidebarFocus;
            lastSbSel       = sidebarSel;
            lastSettingsSel = settingsSel;
            lastClockSec    = nowSec;
            firstFrame      = false;
        }
        waitFrame();
    }

    // Quiesce the icon worker before handing the CPU to the VM: drop queued work and
    // let any in-flight decode finish, so the worker is left blocked (idle).
    IconCache::instance().clearPending();
    while (IconCache::instance().busy()) {
        waitFrame();
    }
    if (g_vsyncCb >= 0) {
        graph_remove_vsync_handler(g_vsyncCb);
        g_vsyncCb = -1;
    }
    // Record the launch so it heads the recent row next time (worker is idle now).
    if (chosen >= 0) {
        Recent::instance().push(chosen);
    }
    return chosen;
}

} // namespace platform
} // namespace ps2
