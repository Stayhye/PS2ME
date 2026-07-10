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
#include "Resolutions.hpp"
#include "ControlLayouts.hpp"
#include "Ps2Storage.hpp"
#include "Ps2MemCard.hpp"          // register the "saving" banner as its I/O-activity listener
#include "Ps2Audio.hpp"            // menu background music (start on the menu, pause in-game)
#include "../hal/GameStorage.hpp"
#include "../hal/Keypad.hpp"
#include "../hal/IPad.hpp"
#include "../hal/SystemClock.hpp"   // session clock for the header
#include "../../version.h"          // PS2ME_VERSION_STRING (shown in the header)

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

// Embedded boot splash (assets/SPLASH.png -> byte array). Decoded once by splash() and
// shown full-screen with a fade before the menu. -I<repo>/assets.
#include "splash_png.h"

// Embedded on-screen keyboard (assets/keyboard.png -> byte array). Decoded by
// controlsGuide() to render the DualShock 2 mapping + live button tester. -I<repo>/assets.
#include "keyboard_png.h"

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

// The game index we last launched (-1 = none). Persists across pick() calls so that
// returning from a game restores the cursor onto that game rather than resetting to the
// first cell. Set when a game is launched, consumed on the next pick() entry.
int g_restoreGame = -1;

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

// The SELECT button, as a small horizontal rounded-rectangle outline (footer legend).
void glyphSelect(int cx, int cy, int r, int g, int b) {
    const int hw = 8, hh = 4;   // half width / height of the badge
    for (int i = -hw + 1; i <= hw - 1; ++i) {
        plotA(cx + i, cy - hh, r, g, b, 255);
        plotA(cx + i, cy + hh, r, g, b, 255);
    }
    for (int j = -hh + 1; j <= hh - 1; ++j) {
        plotA(cx - hw, cy + j, r, g, b, 255);
        plotA(cx + hw, cy + j, r, g, b, 255);
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

// Launcher version, shown as a small tag in the bottom-right of the footer. Comes from
// the single source of truth in ps2/version.h (bump via version.sh, never here).
const char* const kAppVersion = PS2ME_VERSION_STRING;

// Author credit, shown small and subtle beside the PS2ME wordmark in the header.
const char* const kAppAuthor = "by Wellinator";

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
    // Author credit, small and subtle, just right of the wordmark on its baseline.
    drawTextVC(tx + textWidth("PS2ME", 26.0f) + 8, HEADER_H / 2 + 6,
               kAppAuthor, 104, 132, 172, 12.0f);

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

// One focusable field in the details panel: a rounded pill with a left caption and a
// right-aligned value, highlighted (with up/down chevrons at its right edge) when it
// currently holds the focus.
void drawDetailPill(int x, int y, int w, int h, const char* caption, const char* value,
                    bool focused) {
    if (focused) {
        roundRectFillA(x - 2, y - 2, w + 4, h + 4, 7, 90, 215, 255, 255);
        roundRectVGrad(x, y, w, h, 6, 46, 88, 148, 30, 54, 100);
    } else {
        roundRectVGrad(x, y, w, h, 6, 34, 52, 92, 22, 36, 68);
    }
    const int my = y + h / 2;
    drawTextVC(x + 8, my, caption, focused ? 235 : 150, focused ? 245 : 185, 255, 12.0f);
    int valRight = x + w - 8;
    if (focused) {
        const int ax = x + w - 10, ay = my;
        fillTriangle(ax, ay - 7, ax - 5, ay - 1, ax + 5, ay - 1, 210, 235, 255);
        fillTriangle(ax - 5, ay + 1, ax + 5, ay + 1, ax, ay + 7, 210, 235, 255);
        valRight = ax - 12;
    }
    const int vw = textWidth(value, 13.0f);
    drawTextVC(valRight - vw, my, value, focused ? 255 : 210, focused ? 255 : 220, 255, 13.0f);
}

// detailsFocus: 0 = panel not focused, 1 = SCREEN pill focused, 2 = CONTROL pill focused.
void drawDetails(int viewCount, int selected, const int* view, int detailsFocus) {
    const int px = DET_X, py = CONTENT_Y, pw = DET_W, ph = CONTENT_BOTTOM - CONTENT_Y;
    panel(px, py, pw, ph, 10);
    drawText(px + 10, py + 8, "GAME DETAILS", 150, 185, 220, 12.0f);
    if (viewCount <= 0) {
        return;
    }
    const int game = view[selected];

    // Vertically centre the preview + name + counter + screen-size + control-layout block
    // in the panel body (below the "GAME DETAILS" caption), so it reads balanced rather
    // than top-heavy.
    const int prev = pw - 26;
    const int gap1 = 12, nameH = 16, gap2 = 8, infoH = 14, gap3 = 12, pillH = 24, gap4 = 8;
    const int blockH = prev + gap1 + nameH + gap2 + infoH + gap3 + pillH + gap4 + pillH;
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

    const int pillX = px + 12, pillW = pw - 24;

    // Per-game screen size (up/down cycle presets when this pill is focused).
    int rw = 0, rh = 0;
    Resolutions::instance().get(game, &rw, &rh);
    char res[16];
    snprintf(res, (int)sizeof(res), "%dx%d", rw, rh);
    const int scy = prevY + prev + gap1 + nameH + gap2 + infoH + gap3;
    drawDetailPill(pillX, scy, pillW, pillH, "SCREEN", res, detailsFocus == 1);

    // Per-game control layout (Global / Simple / Complete).
    const char* clv = ControlLayouts::preset(ControlLayouts::instance().indexFor(game));
    const int cly = scy + pillH + gap4;
    drawDetailPill(pillX, cly, pillW, pillH, "PAD", clv, detailsFocus == 2);
}

// Context-aware button legend: only the actions available in the current context are
// shown (e.g. no BACK at the root, no FAVORITE/SORT on SETTINGS or while the alphabet
// column is focused). Glyph kind: 0=cross, 1=circle, 2=triangle, 3=square.
void drawFooter(int activeTab, bool sidebarFocus, int detailsFocus) {
    rectVGrad(0, FOOTER_Y, g_w, FOOTER_H, 26, 42, 78, 14, 24, 50);
    for (int x = 0; x < g_w; ++x) plotA(x, FOOTER_Y, 60, 90, 140, 255);   // top hairline
    const int cy = FOOTER_Y + FOOTER_H / 2;

    // Version tag, tucked into the bottom-right corner (the button legend is centred, so
    // it never reaches this far right).
    drawTextVC(g_w - MARGIN - textWidth(kAppVersion, 13.0f), cy,
               kAppVersion, 110, 138, 178, 13.0f);

    const bool canBack = sidebarFocus || detailsFocus != 0 || activeTab != 0;
    int kinds[6];   // glyph kind: 0=cross 1=circle 2=triangle 3=square 4=up/down 5=select 6=left/right
    const char* labels[6];
    int n = 0;
    if (activeTab == 2) {                       // SETTINGS
        kinds[n] = 0; labels[n] = "SELECT"; ++n;
        if (canBack) { kinds[n] = 1; labels[n] = "BACK"; ++n; }
    } else if (sidebarFocus) {                  // alphabet column focused
        kinds[n] = 0; labels[n] = "JUMP"; ++n;
        kinds[n] = 1; labels[n] = "BACK"; ++n;
    } else if (detailsFocus != 0) {             // details panel focused (screen / pad)
        kinds[n] = 4; labels[n] = "FIELD"; ++n;    // up/down: pick the field
        kinds[n] = 6; labels[n] = "CHANGE"; ++n;   // left/right: change its value
        kinds[n] = 0; labels[n] = "LAUNCH"; ++n;
        kinds[n] = 1; labels[n] = "BACK"; ++n;
    } else {                                     // ALL GAMES / FAVORITES grid
        kinds[n] = 0; labels[n] = "LAUNCH"; ++n;
        if (canBack) { kinds[n] = 1; labels[n] = "BACK"; ++n; }
        kinds[n] = 2; labels[n] = "FAVORITE"; ++n;
        kinds[n] = 3; labels[n] = "SORT"; ++n;
        kinds[n] = 5; labels[n] = "OPTIONS"; ++n;
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
            case 4: fillTriangle(gcx, cy - 7, gcx - 5, cy - 1, gcx + 5, cy - 1, 150, 210, 245);
                    fillTriangle(gcx - 5, cy + 1, gcx + 5, cy + 1, gcx, cy + 7, 150, 210, 245);
                    break;
            case 5: glyphSelect(gcx, cy, 150, 210, 245); break;
            case 6: fillTriangle(gcx - 7, cy, gcx - 1, cy - 5, gcx - 1, cy + 5, 150, 210, 245);
                    fillTriangle(gcx + 1, cy - 5, gcx + 7, cy, gcx + 1, cy + 5, 150, 210, 245);
                    break;
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

const int SETTINGS_COUNT = 9;

// The SETTINGS tab: a vertical list of options (actions + toggles). @p sel is the
// highlighted row. The list scrolls: only the rows that fit at a comfortable size are
// drawn, and the visible window follows the selection so the list can keep growing.
void drawSettings(int sel) {
    // The alphabet quick-nav rail is hidden on this tab (see render), so center the option
    // rows in the full content width: mirror the right content margin on the left instead of
    // starting at the old grid edge, which left the reclaimed rail strip empty on the left.
    const int right = DET_X + DET_W;      // rightmost content edge (SCREEN_W - 8)
    const int left  = SCREEN_W - right;   // mirror it -> rows centered on screen
    const int rowH = 42, gap = 12;
    const int availH = CONTENT_BOTTOM - CONTENT_Y;

    // Rows that fit at this size; scroll when there are more than the window holds.
    int visible = (availH + gap) / (rowH + gap);
    if (visible < 1)              visible = 1;
    if (visible > SETTINGS_COUNT) visible = SETTINGS_COUNT;

    // Window top that keeps the selection in view (stateless: derived from sel each frame).
    int top = 0;
    if (sel >= visible)                  top = sel - visible + 1;
    if (top > SETTINGS_COUNT - visible)  top = SETTINGS_COUNT - visible;
    if (top < 0)                         top = 0;

    const int totalH = visible * rowH + (visible - 1) * gap;
    int y = CONTENT_Y + (availH - totalH) / 2;

    for (int k = 0; k < visible; ++k) {
        const int i = top + k;
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
            case 4: label = "Debug mode";
                    snprintf(value, sizeof(value), "%s", Settings::instance().debugMode() ? "On" : "Off");
                    break;
            case 5: label = "FPS counter";
                    snprintf(value, sizeof(value), "%s", Settings::instance().fpsCounter() ? "On" : "Off");
                    break;
            case 6: label = "Menu music";
                    snprintf(value, sizeof(value), "%s", Settings::instance().bgmEnabled() ? "On" : "Off");
                    break;
            case 7: label = "Control layout";
                    snprintf(value, sizeof(value), "%s",
                             Settings::instance().controlLayout() == 1 ? "Complete" : "Simple");
                    break;
            case 8: label = "Controls guide";
                    snprintf(value, sizeof(value), "%s", "View");
                    break;
        }
        const int cy = y + rowH / 2;
        drawTextVC(left + 14, cy, label, s ? 255 : 220, s ? 255 : 228, 255, 17.0f);
        const int vw = textWidth(value, 15.0f);
        drawTextVC(right - 14 - vw, cy, value, s ? 235 : 150, s ? 245 : 185, 255, 15.0f);
        y += rowH + gap;
    }

    // Scroll indicators: an up chevron when rows are hidden above, a down chevron below.
    const int acx = (left + right) / 2;
    if (top > 0) {
        const int ay = CONTENT_Y + 1;
        fillTriangle(acx, ay, acx - 7, ay + 9, acx + 7, ay + 9, 150, 195, 240);
    }
    if (top + visible < SETTINGS_COUNT) {
        const int by = CONTENT_BOTTOM - 1;
        fillTriangle(acx, by, acx - 7, by - 9, acx + 7, by - 9, 150, 195, 240);
    }
}

void render(int viewCount, int selected, int topRow, int activeTab, const int* view,
            bool sidebarFocus, int sidebarSel, int settingsSel, int detailsFocus) {
    memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));   // baked background
    drawHeader();
    drawTabs(activeTab);
    const int curGame = viewCount > 0 ? view[selected] : -1;
    if (activeTab != 2) {                       // no alphabet quick-nav rail on the SETTINGS tab
        drawSidebar(curGame, sidebarFocus, sidebarSel);
    }
    drawFooter(activeTab, sidebarFocus, detailsFocus);

    if (activeTab == 2) {                       // SETTINGS
        drawSettings(settingsSel);
        return;
    }
    if (activeTab == 1 && viewCount <= 0) {     // FAVORITES: empty
        drawContentMessage("No favorites yet -- mark games with TRIANGLE");
        return;
    }
    if (activeTab == 0 && viewCount <= 0) {
        if (Settings::instance().debugMode()) {
            // Debug mode: dump the raw storage-probe trace in the grid area (the EE console
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
        // Friendly empty state (default): tell the user where to drop their games. The raw
        // storage diagnostics stay one toggle away under Settings > Debug mode.
        const int left = GRID_X0, right = DET_X + DET_W;
        const int cx = (left + right) / 2;
        const int cy = (CONTENT_Y + CONTENT_BOTTOM) / 2;
        const char* title = "No games found";
        const char* l1    = "Add your .jar games to this folder:";
        const char* dir   = Ps2Storage::instance().gamesDir();
        const char* l3    = "then relaunch PS2ME.";
        const char* hint  = "Enable Debug mode in Settings for storage details.";
        int w;
        w = textWidth(title, 26.0f); drawTextVC(cx - w / 2, cy - 44, title, 232, 240, 252, 26.0f);
        w = textWidth(l1,    16.0f); drawTextVC(cx - w / 2, cy -  6, l1,    165, 192, 224, 16.0f);
        w = textWidth(dir,   15.0f); drawTextVC(cx - w / 2, cy + 18, dir,   120, 210, 245, 15.0f);
        w = textWidth(l3,    16.0f); drawTextVC(cx - w / 2, cy + 44, l3,    165, 192, 224, 16.0f);
        w = textWidth(hint,  12.0f); drawTextVC(cx - w / 2, cy + 74, hint,  110, 132, 165, 12.0f);
        return;
    }

    drawScrollbar(viewCount, topRow);
    drawDetails(viewCount, selected, view, detailsFocus);
    drawGrid(viewCount, selected, topRow, view, !sidebarFocus && detailsFocus == 0);
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

// --- On-screen application log (real-HW diagnostics) ------------------------------
// The VM's stdout (System.out + VM diagnostics via pcsl_print -> javacall_print ->
// StdoutSink, and our own javacall_print calls) is teed here so it renders on the native
// GS -- the only console left once the USB IOP reset kills the SIF tty. Bytes are
// buffered into lines; the debug split view (debugPresent / debugSplitRender) shows the
// tail of this rolling buffer beside the running game. Bounded, no heap.
const int LOG_MAX_LINES = 40;    // rolling scrollback; the view shows the tail that fits
const int LOG_LINE_CAP  = 96;    // long lines just clip to the panel width

char g_logLines[LOG_MAX_LINES][LOG_LINE_CAP];
int  g_logCount = 0;             // completed lines stored
char g_logBuild[LOG_LINE_CAP];   // line currently being assembled
int  g_logCur   = 0;             // chars in g_logBuild

// Active launch overlay: 0 = none, 1 = raw debug trace, 2 = friendly loading screen.
int  g_launchMode = 0;

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

// --- Friendly loading screen (default, non-debug launch view) ---------------------
// A centred card with the chosen game's icon + name and a staged progress bar. It is
// painted on loadingBegin() and repainted each time a [Launcher] milestone (seen in the
// teed VM stdout, loadingFeed()) advances the stage, so the user sees clear motion
// while the VM installs and starts the game instead of the raw developer log.
const char* const kLoadStages[4] = {
    "Preparing...", "Copying game...", "Installing...", "Starting..."
};
const int kLoadPct[4] = { 15, 45, 72, 92 };   // progress-bar fill per stage

char g_loadName[64];             // chosen game's display name (".jar" stripped)
int  g_loadGame  = -1;           // its storage index (for the cached icon)
int  g_loadStage = 0;            // 0..3, see kLoadStages
bool g_loadFail  = false;        // a "[Launcher] launch FAILED" line was seen

void loadingRender() {
    if (g_ras == 0 || !g_fontOk) {
        return;
    }
    memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));   // baked dashboard background
    drawHeader();                                           // same PS2ME header as the menu

    const int cardW = 360, cardH = 236;
    const int cardX = (g_w - cardW) / 2;
    const int cardY = (g_h - cardH) / 2 + 6;
    panel(cardX, cardY, cardW, cardH, 12);

    // Game icon (already decoded + cached while it sat in the menu) in a recessed frame,
    // or a lettered fallback if it is not cached.
    const int icon  = 92;
    const int iconX = cardX + (cardW - icon) / 2;
    const int iconY = cardY + 20;
    roundRectFillA(iconX - 3, iconY - 3, icon + 6, icon + 6, 8, 120, 150, 205, 255);
    roundRectVGrad(iconX - 2, iconY - 2, icon + 4, icon + 4, 7, 28, 46, 86, 18, 32, 64);
    if (g_loadGame < 0 ||
        !IconCache::instance().drawScaled(g_loadGame, g_ras, g_w, g_h, iconX, iconY, icon)) {
        char c[2] = { (char)(g_loadName[0] ? g_loadName[0] : '?'), 0 };
        drawText(iconX + icon / 2 - textWidth(c, 60.0f) / 2, iconY + icon / 2 - 34,
                 c, 210, 224, 244, 60.0f);
    }

    // Game name, centred under the icon.
    char lbl[48];
    makeLabel(lbl, (int)sizeof(lbl), g_loadName, (float)(cardW - 40), 20.0f);
    const int lw = textWidth(lbl, 20.0f);
    drawText(cardX + (cardW - lw) / 2, iconY + icon + 12, lbl, 235, 242, 252, 20.0f);

    // Stage label above the bar.
    const int barX = cardX + 24, barW = cardW - 48, barH = 12;
    const int barY = cardY + cardH - 30;
    const char* stage = g_loadFail ? "Failed to start" : kLoadStages[g_loadStage];
    const int sr = g_loadFail ? 235 : 150, sg = g_loadFail ? 110 : 185, sb = g_loadFail ? 110 : 220;
    const int sw = textWidth(stage, 15.0f);
    drawText(cardX + (cardW - sw) / 2, barY - 22, stage, sr, sg, sb, 15.0f);

    // Progress bar: recessed track + filled portion.
    roundRectVGrad(barX, barY, barW, barH, 6, 20, 32, 60, 14, 22, 44);
    const int pct = g_loadFail ? 100 : kLoadPct[g_loadStage];
    int fillW = barW * pct / 100;
    if (fillW < barH) fillW = barH;
    if (g_loadFail) {
        roundRectVGrad(barX, barY, fillW, barH, 6, 210, 90, 90, 150, 50, 50);
    } else {
        roundRectVGrad(barX, barY, fillW, barH, 6, 100, 210, 255, 40, 110, 190);
    }

    GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
}

// Advance the loading stage from a completed [Launcher] milestone line, repainting only
// when the stage (or the failure flag) actually changes. Stages never move backwards.
void loadingStageFromLine(const char* line) {
    int  st   = g_loadStage;
    bool fail = g_loadFail;
    if (strstr(line, "FAILED") != 0) {
        fail = true;
    } else if (strstr(line, "launching ") != 0) {
        st = 3;
    } else if (strstr(line, "installed ") != 0) {
        st = 2;
    } else if (strstr(line, "seeded ") != 0) {
        st = 1;
    }
    if (st < g_loadStage) {
        st = g_loadStage;
    }
    if (st != g_loadStage || fail != g_loadFail) {
        g_loadStage = st;
        g_loadFail  = fail;
        loadingRender();
    }
}

// Buffer teed VM stdout into lines and drive the stage from each completed one. Reuses
// g_logBuild (the raw-trace line buffer) since only one launch overlay is ever active.
void loadingFeed(const char* s, int len) {
    for (int i = 0; i < len; ++i) {
        const char c = s[i];
        if (c == '\n') {
            g_logBuild[g_logCur] = '\0';
            loadingStageFromLine(g_logBuild);
            g_logCur = 0;
        } else if (c != '\r' && g_logCur < LOG_LINE_CAP - 1) {
            g_logBuild[g_logCur++] = c;
        }
    }
}

// --- Debug split view (game left, full app log right) -----------------------------
// With "Debug mode" on, each game frame is composited here instead of shown fullscreen:
// the running game (aspect-preserved) fills the left half and the rolling application
// log (the same g_logLines buffer, TTY-style) fills the right half. Lets a developer
// watch the game and its stdout side by side on real hardware, where the SIF tty is gone.
const int SPLIT_X = 320;   // divider: left half = game, right half = log (screen / 2)

// The most recent game frame, cached so a log line arriving between game frames can
// re-render the split with the last picture (null until the game draws its first frame).
const unsigned short* g_dbgRaster = 0;
int g_dbgW = 0, g_dbgH = 0;

// Nearest-neighbour scale the game's RGB565 raster into a g_ras rect, converting to the
// GS-native RGBA5551 (R low, G, B, alpha set) -- same channel layout Ps2Framebuffer uses.
void blitScaled565(const unsigned short* src, int sw, int sh,
                   int dx, int dy, int dw, int dh) {
    if (src == 0 || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    for (int y = 0; y < dh; ++y) {
        const int py = dy + y;
        if (py < 0 || py >= g_h) continue;
        const int sy = y * sh / dh;
        const unsigned short* srow = src + sy * sw;
        u16* drow = g_ras + py * g_w + dx;
        for (int x = 0; x < dw; ++x) {
            const int px = dx + x;
            if (px < 0 || px >= g_w) continue;
            const unsigned p = srow[x * sw / dw];
            const unsigned r = (p >> 11) & 0x1F;   // RGB565 -> 5551 (green drops its LSB)
            const unsigned g = (p >> 6)  & 0x1F;
            const unsigned b =  p        & 0x1F;
            drow[x] = (u16)(r | (g << 5) | (b << 10) | 0x8000);
        }
    }
}

// A wrapped display row: a slice [p, p+n) of a stored log line. The log buffers stay
// valid for the whole render, so rows point into them instead of copying.
struct LogRow { const char* p; int n; };

// Append a row, dropping the oldest when full so the array always holds the newest rows.
void logRowAppend(LogRow* rows, int cap, int* count, const char* p, int n) {
    if (*count < cap) {
        rows[*count].p = p; rows[*count].n = n; ++(*count);
    } else {
        for (int k = 1; k < cap; ++k) rows[k - 1] = rows[k];
        rows[cap - 1].p = p; rows[cap - 1].n = n;
    }
}

// Word-wrap a null-terminated log line to @p maxW pixels at @p pxh, appending each visual
// row. Breaks at the last space that fits (hard-breaks when a word is too long), preserves
// blank lines, and always emits at least one character per row (never loops forever).
void logWrap(const char* s, float pxh, int maxW, LogRow* rows, int cap, int* count) {
    const float scale = stbtt_ScaleForPixelHeight(&g_font, pxh);
    int len = 0;
    while (s[len] != '\0') ++len;
    if (len == 0) {
        logRowAppend(rows, cap, count, s, 0);      // keep blank lines
        return;
    }
    int start = 0;
    while (start < len) {
        float wpx = 0.0f;
        int i = start, lastSpace = -1;
        while (i < len) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&g_font, (unsigned char)s[i], &adv, &lsb);
            const float cw = adv * scale;
            if (wpx + cw > (float)maxW && i > start) break;
            if (s[i] == ' ') lastSpace = i;
            wpx += cw;
            ++i;
        }
        const int end = (i < len && lastSpace > start) ? lastSpace : i;
        logRowAppend(rows, cap, count, s + start, end - start);
        start = end;
        while (start < len && s[start] == ' ') ++start;   // swallow the break's spaces
    }
}

void debugSplitRender(const unsigned short* raster, int w, int h) {
    if (g_ras == 0 || !g_fontOk) {
        return;
    }
    // Left half: black stage for the game. Right half: TTY-dark log panel + divider.
    fillRect(0, 0, SPLIT_X, g_h, rgba5551(0, 0, 0));
    fillRect(SPLIT_X, 0, g_w - SPLIT_X, g_h, rgba5551(14, 16, 24));
    fillRect(SPLIT_X - 1, 0, 2, g_h, rgba5551(60, 90, 140));

    // Game, aspect-preserved and centred in the left region (2px inset). Before the game
    // draws its first frame the left half shows a "Loading..." placeholder.
    if (raster != 0 && w > 0 && h > 0) {
        const int rw = SPLIT_X - 4, rh = g_h - 4;
        int dw = rw, dh = h * rw / w;
        if (dh > rh) { dh = rh; dw = w * rh / h; }
        blitScaled565(raster, w, h, 2 + (rw - dw) / 2, 2 + (rh - dh) / 2, dw, dh);
    } else {
        const int tw = textWidth("Loading...", 22.0f);
        drawText((SPLIT_X - tw) / 2, g_h / 2 - 14, "Loading...", 200, 214, 235, 22.0f);
    }

    // Right half: the rolling app log, TTY green-on-dark, word-wrapped to the panel width
    // and tail-windowed so the newest wrapped rows always fill the bottom.
    const int lx = SPLIT_X + 8;
    const int clipR = g_w - 4;
    drawText(lx, 6, "APP LOG", 130, 205, 150, 15.0f);
    const int top = 26, stride = 15;
    const int fit = (g_h - top) / stride;
    const float pxh = 13.0f;
    const int maxW = clipR - lx;

    static LogRow rows[96];               // render is single-threaded; keep this off the stack
    int rc = 0;
    for (int i = 0; i < g_logCount; ++i) {
        logWrap(g_logLines[i], pxh, maxW, rows, 96, &rc);
    }
    if (g_logCur > 0) {
        g_logBuild[g_logCur] = '\0';
        logWrap(g_logBuild, pxh, maxW, rows, 96, &rc);    // partial line in flight
    }

    int ly = top;
    for (int i = (rc > fit ? rc - fit : 0); i < rc; ++i) {
        char rb[LOG_LINE_CAP];
        int n = rows[i].n;
        if (n > LOG_LINE_CAP - 1) n = LOG_LINE_CAP - 1;
        memcpy(rb, rows[i].p, (size_t)n);
        rb[n] = '\0';
        drawTextClip(lx, ly, rb, 175, 232, 178, pxh, lx, clipR);
        ly += stride;
    }

    GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
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

// --- Memory-card "saving" spinner -------------------------------------------------
// A memory-card write (a favourite/setting/resolution change now; a game save later) is
// issued to the IOP and Ps2MemCard polls it to completion. Between polls it calls
// memCardIoActivity(kIoTick) so we can animate a small, discreet "Saving" spinner in the
// bottom-right corner -- the UI stays alive through the write instead of freezing. We keep
// a backup of just the spinner's box so each frame restores the menu underneath before
// redrawing the rotated spinner (no trails, no full re-render), and on kIoEnd we hold it a
// short minimum so an instant write (e.g. a virtual card on the emulator) is still visible.
const int   SPIN_W = 150;               // spinner badge box (bottom-right corner)
const int   SPIN_H = 30;
const int   SPIN_MIN_FRAMES = 14;       // ~0.23s floor so a fast write still shows

u16  g_spinBackup[SPIN_W * SPIN_H];     // menu pixels under the badge (render is single-threaded)
bool g_spinActive = false;              // a write is currently drawing the badge
int  g_spinTick   = 0;                  // rotation phase (advances each frame)
int  g_spinFrames = 0;                  // frames shown this write (for the minimum hold)

inline int spinX() { return g_w - 10 - SPIN_W; }
inline int spinY() { return FOOTER_Y - 8 - SPIN_H; }   // just above the footer legend

void spinCapture() {
    const int bx = spinX(), by = spinY();
    for (int j = 0; j < SPIN_H; ++j) {
        const int py = by + j;
        if (py < 0 || py >= g_h) continue;
        for (int i = 0; i < SPIN_W; ++i) {
            const int px = bx + i;
            g_spinBackup[j * SPIN_W + i] =
                ((unsigned)px < (unsigned)g_w) ? g_ras[py * g_w + px] : 0;
        }
    }
}

void spinRestore() {
    const int bx = spinX(), by = spinY();
    for (int j = 0; j < SPIN_H; ++j) {
        const int py = by + j;
        if (py < 0 || py >= g_h) continue;
        for (int i = 0; i < SPIN_W; ++i) {
            const int px = bx + i;
            if ((unsigned)px < (unsigned)g_w) {
                g_ras[py * g_w + px] = g_spinBackup[j * SPIN_W + i];
            }
        }
    }
}

// A small filled disc (spinner dot), alpha-blended so trailing dots fade out.
void spinDot(int cx, int cy, int rad, int r, int g, int b, int a) {
    for (int j = -rad; j <= rad; ++j) {
        for (int i = -rad; i <= rad; ++i) {
            if (i * i + j * j <= rad * rad) plotA(cx + i, cy + j, r, g, b, a);
        }
    }
}

// Draw the badge (rounded pill + rotating ring of dots + "Saving" label) at rotation
// phase @p tick, over the already-restored menu pixels.
void drawSpinBadge(int tick) {
    const int bx = spinX(), by = spinY();
    // Discreet dark translucent pill with a faint amber edge.
    roundRectFillA(bx - 1, by - 1, SPIN_W + 2, SPIN_H + 2, 9, 210, 170, 80, 150);
    roundRectFillA(bx, by, SPIN_W, SPIN_H, 8, 12, 18, 32, 235);

    // Ring of 8 dots around the badge's left; the "head" is brightest and it rotates.
    static const int SDX[8] = { 0, 6, 8, 6, 0, -6, -8, -6 };
    static const int SDY[8] = { -8, -6, 0, 6, 8, 6, 0, -6 };
    const int cx = bx + 18, cy = by + SPIN_H / 2;
    const int head = ((tick % 8) + 8) % 8;
    for (int k = 0; k < 8; ++k) {
        const int ph = ((head - k) % 8 + 8) % 8;   // 0 = head
        int a = 255 - ph * 26;
        if (a < 45) a = 45;
        spinDot(cx + SDX[k], cy + SDY[k], 2, 255, 205, 110, a);
    }

    drawTextVC(bx + 36, cy, "Saving", 236, 224, 190, 15.0f);
}

// Draw one spinner frame: restore the clean menu box, draw the badge at the current phase,
// present (which waits for the vblank flip so the frame is actually shown), and advance.
void spinFrame() {
    spinRestore();
    drawSpinBadge(g_spinTick);
    GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
    ++g_spinTick;
    ++g_spinFrames;
}

// Ps2MemCard activity hook. kIoBegin: capture the box + draw the first frame. kIoTick:
// advance a frame (called between completion polls while the write is in flight). kIoEnd:
// hold to the minimum so a fast write is still seen, then restore the clean menu box.
// Registered by pick() once video is up; left registered for a later off-menu game save.
void memCardIoActivity(int phase) {
    if (g_ras == 0 || !g_fontOk || !GsDisplay::instance().ready()) {
        return;   // no screen yet (e.g. boot-time icon writes before pick())
    }
    switch (phase) {
        case ps2::platform::Ps2MemCard::kIoBegin:
            spinCapture();
            g_spinTick = 0;
            g_spinFrames = 0;
            g_spinActive = true;
            spinFrame();
            break;
        case ps2::platform::Ps2MemCard::kIoTick:
            if (g_spinActive) spinFrame();
            break;
        case ps2::platform::Ps2MemCard::kIoEnd:
            if (g_spinActive) {
                while (g_spinFrames < SPIN_MIN_FRAMES) spinFrame();
                spinRestore();
                GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
                g_spinActive = false;
            }
            break;
    }
}

} // namespace

Ps2Frontend& Ps2Frontend::instance() {
    static Ps2Frontend inst;
    return inst;
}

void Ps2Frontend::splash() {
    // Bring up the raster + shared GS (same path the menu uses). If video can't come up
    // there is nothing to show, so bail quietly.
    if (!ensureVideo()) {
        return;
    }
    int w = 0, h = 0;
    unsigned char* px = MidletIcon::decodePng(g_splash_png, (int)g_splash_png_len, &w, &h);
    if (px == 0 || w <= 0 || h <= 0) {
        return;
    }
    // Convert the decoded RGBA8888 into the native RGBA5551 raster, nearest-fit to the
    // full screen (the asset is authored at 640x448, so this is 1:1 in practice).
    for (int y = 0; y < g_h; ++y) {
        const int sy = (h == g_h) ? y : (y * h / g_h);
        u16* row = g_ras + (size_t)y * g_w;
        const unsigned char* srow = px + (size_t)sy * w * 4;
        for (int x = 0; x < g_w; ++x) {
            const int sx = (w == g_w) ? x : (x * w / g_w);
            const unsigned char* s = srow + (size_t)sx * 4;
            row[x] = rgba5551(s[0], s[1], s[2]);
        }
    }
    MidletIcon::release(px);
    GsDisplay::instance().showSplash(g_ras, g_w, g_h);
}

namespace {

// Scale-and-blit the decoded RGBA keyboard into the raster at (dx,dy) sized dw x dh,
// nearest-sampled, honouring the source alpha (the asset's rounded corners are
// transparent, so the panelled background shows through).
void blitKeyboard(const unsigned char* rgba, int kw, int kh, int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        int sy = y * kh / dh; if (sy >= kh) sy = kh - 1;
        const int ty = dy + y;
        if ((unsigned)ty >= (unsigned)g_h) continue;
        const unsigned char* srow = rgba + (size_t)sy * kw * 4;
        for (int x = 0; x < dw; ++x) {
            int sx = x * kw / dw; if (sx >= kw) sx = kw - 1;
            const unsigned char* s = srow + (size_t)sx * 4;
            const int a = s[3];
            if (a > 4) plotA(dx + x, ty, s[0], s[1], s[2], a);
        }
    }
}

// A translucent cyan glow over one key cell -- the live "this button is pressed" cue.
void kbHilite(int cx, int cy, int hw, int hh) {
    roundRectFillA(cx - hw - 4, cy - hh - 4, 2 * hw + 8, 2 * hh + 8, 9,  90, 215, 255,  60);
    roundRectFillA(cx - hw - 1, cy - hh - 1, 2 * hw + 2, 2 * hh + 2, 8, 150, 235, 255, 130);
}

// Right-stick offset (screen +y down) -> phone numpad digit by compass sector, matching
// the Keypad's COMPLETE mapping (N=2 S=8 E=6 W=4, NW=1 NE=3 SW=7 SE=9). 0 if centred.
char kbStickDigit(int dx, int dy) {
    const int uy = -dy, ax = dx < 0 ? -dx : dx, ay = uy < 0 ? -uy : uy;
    const bool diag = (ax * 1000 > ay * 414) && (ay * 1000 > ax * 414);
    if (diag) return (uy > 0) ? (dx > 0 ? '3' : '1') : (dx > 0 ? '9' : '7');
    if (ax > ay) return dx > 0 ? '6' : '4';
    return uy > 0 ? '2' : '8';
}

// Resolve the control layout a game will actually run under (per-game override, else the
// global default), as 0 = SIMPLE / 1 = COMPLETE. game < 0 -> the global default.
int effectiveLayout(int game) {
    const int cl = (game >= 0) ? ControlLayouts::instance().indexFor(game)
                               : (int)ControlLayouts::GLOBAL;
    if (cl == ControlLayouts::SIMPLE)   return 0;
    if (cl == ControlLayouts::COMPLETE) return 1;
    return Settings::instance().controlLayout() == 1 ? 1 : 0;   // GLOBAL
}

// Render the on-screen keyboard + one layout's DS2 overlay (static markers + legend) plus
// the live "button pressed" highlights into g_ras. The caller owns the header bar, the
// footer, the decoded keyboard and the frame loop -- so both the guide and the first-run
// picker reuse this exact body.
void drawControlsBody(int layout, const unsigned char* kb, int kw, int kh,
                      bool ok, const hal::PadButtons& b) {
    // Keyboard placement + per-key anchor table, in the asset's 256x256 space. Centres were
    // measured from keyboard.png (groove/centroid analysis), not eyeballed: columns at
    // x=50/127/203, rows at y=95/132/170/208, soft keys and D-pad located directly.
    const int kbW = 344, kbH = 344, kx = 26, ky = 52;
    struct Spot { int ix, iy; };
    // 0..11 = 1 2 3 / 4 5 6 / 7 8 9 / * 0 # ; 12 = left soft, 13 = right soft, 14 = D-pad.
    const Spot spot[15] = {
        { 50, 95}, {127, 95}, {203, 95},
        { 50,132}, {127,132}, {203,132},
        { 50,170}, {127,170}, {203,170},
        { 50,208}, {127,208}, {203,208},
        { 54, 24}, {197, 23}, {129, 38},
    };
    int cx[15], cy[15];
    for (int i = 0; i < 15; ++i) {
        cx[i] = kx + spot[i].ix * kbW / 256;
        cy[i] = ky + spot[i].iy * kbH / 256;
    }
    const int chw = 31 * kbW / 256;   // key cell half-extents on screen (~42 x 21)
    const int chh = 16 * kbH / 256;

    if (kb != 0) {
        blitKeyboard(kb, kw, kh, kx, ky, kbW, kbH);
    }

    // Shared directional read (D-pad OR left stick) and D-pad arm offset.
    const int DZ = 40;
    const bool up    = ok && (b.up    || b.ly < 128 - DZ);
    const bool down  = ok && (b.down  || b.ly > 128 + DZ);
    const bool left  = ok && (b.left  || b.lx < 128 - DZ);
    const bool right = ok && (b.right || b.lx > 128 + DZ);
    const int dOff = (24 * kbW / 256) * 6 / 10;   // ~19 px

    if (layout == 1) {
            // ===== COMPLETE: full phone keypad =====
            // Live highlights: light the key(s) each pressed button drives. The D-pad
            // lights only the pressed ARM, so the direction is unambiguous.
            if (ok) {
                bool on[12] = { false };
                on[0] = b.l1;  on[2] = b.r1;  on[6] = b.l2;  on[8] = b.r2;     // 1 3 7 9
                on[4] = b.cross || b.r3;                                       // 5
                on[9] = b.square; on[10] = b.triangle; on[11] = b.circle;      // * 0 #
                on[1] = up; on[7] = down; on[3] = left; on[5] = right;        // 2 8 4 6
                const int rdx = b.rx - 128, rdy = b.ry - 128;
                if (rdx * rdx + rdy * rdy > DZ * DZ) {
                    const char d = kbStickDigit(rdx, rdy);
                    if (d >= '1' && d <= '9') on[d - '1'] = true;
                }
                for (int i = 0; i < 12; ++i) {
                    if (on[i]) kbHilite(cx[i], cy[i], chw, chh);
                }
                if (b.select) kbHilite(cx[12], cy[12], chw - 2, 8);
                if (b.start)  kbHilite(cx[13], cy[13], chw - 2, 8);
                if (up)    kbHilite(cx[14], cy[14] - dOff, 6, 6);
                if (down)  kbHilite(cx[14], cy[14] + dOff, 6, 6);
                if (left)  kbHilite(cx[14] - dOff, cy[14], 6, 6);
                if (right) kbHilite(cx[14] + dOff, cy[14], 6, 6);
            }
            // Static DS2 markers, centred in each key's top strip (uniform anchor).
            for (int i = 0; i < 12; ++i) {
                const int ax = cx[i], ay = cy[i] - chh + 2;
                switch (i) {
                    case 4:  glyphCross   (ax, ay, 6, 120, 160, 235); break;   // 5 = Cross
                    case 9:  glyphSquare  (ax, ay, 6, 230, 112, 190); break;   // * = Square
                    case 10: glyphTriangle(ax, ay, 6,  92, 216, 150); break;   // 0 = Triangle
                    case 11: glyphCircle  (ax, ay, 6, 235,  92,  92); break;   // # = Circle
                    case 1:  fillTriangle(ax, ay - 5, ax - 4, ay + 3, ax + 4, ay + 3, 150, 210, 245); break; // 2 up
                    case 7:  fillTriangle(ax - 4, ay - 3, ax + 4, ay - 3, ax, ay + 5, 150, 210, 245); break; // 8 down
                    case 3:  fillTriangle(ax - 5, ay, ax + 3, ay - 4, ax + 3, ay + 4, 150, 210, 245); break; // 4 left
                    case 5:  fillTriangle(ax + 5, ay, ax - 3, ay - 4, ax - 3, ay + 4, 150, 210, 245); break; // 6 right
                    case 0:  drawTextVC(ax - textWidth("L1", 11.0f) / 2, ay, "L1", 200, 220, 245, 11.0f); break;
                    case 2:  drawTextVC(ax - textWidth("R1", 11.0f) / 2, ay, "R1", 200, 220, 245, 11.0f); break;
                    case 6:  drawTextVC(ax - textWidth("L2", 11.0f) / 2, ay, "L2", 200, 220, 245, 11.0f); break;
                    case 8:  drawTextVC(ax - textWidth("R2", 11.0f) / 2, ay, "R2", 200, 220, 245, 11.0f); break;
                }
            }
            drawTextVC(cx[12] - textWidth("SELECT", 11.0f) / 2, cy[12], "SELECT", 190, 214, 240, 11.0f);
            drawTextVC(cx[13] - textWidth("START",  11.0f) / 2, cy[13], "START",  190, 214, 240, 11.0f);

            // Legend.
            int lx = kx + kbW + 22, ly = ky + 6;
            drawText(lx, ly, "MAPPING", 150, 185, 220, 14.0f); ly += 26;
            const int gx = lx + 8, tx = lx + 22, vx = lx + 150;
            glyphCross(gx, ly + 6, 6, 120, 160, 235);
            drawText(tx, ly, "Cross", 220, 230, 245, 13.0f);
            drawText(vx, ly, "5 / Fire", 160, 195, 235, 13.0f); ly += 22;
            glyphSquare(gx, ly + 6, 6, 230, 112, 190);
            drawText(tx, ly, "Square", 220, 230, 245, 13.0f);
            drawText(vx, ly, "*", 160, 195, 235, 13.0f); ly += 22;
            glyphTriangle(gx, ly + 6, 6, 92, 216, 150);
            drawText(tx, ly, "Triangle", 220, 230, 245, 13.0f);
            drawText(vx, ly, "0", 160, 195, 235, 13.0f); ly += 22;
            glyphCircle(gx, ly + 6, 6, 235, 92, 92);
            drawText(tx, ly, "Circle", 220, 230, 245, 13.0f);
            drawText(vx, ly, "#", 160, 195, 235, 13.0f); ly += 24;
            drawText(lx, ly, "L1 R1 L2 R2", 220, 230, 245, 13.0f);
            drawText(vx, ly, "1 3 7 9", 160, 195, 235, 13.0f); ly += 22;
            drawText(lx, ly, "Select / Start", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Soft 1/2", 160, 195, 235, 13.0f); ly += 22;
            drawText(lx, ly, "D-pad / L-stick", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Arrows", 160, 195, 235, 13.0f); ly += 22;
            drawText(lx, ly, "R-stick", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Numpad aim", 160, 195, 235, 13.0f); ly += 22;
            drawText(lx, ly, "R3", 220, 230, 245, 13.0f);
            drawText(vx, ly, "5", 160, 195, 235, 13.0f);
        } else {
            // ===== SIMPLE: only the D-pad (arrows), Cross (fire) and L1/R1 (soft keys) =====
            const int dcx = cx[14], dcy = cy[14];
            // Arrow reach onto the D-pad arms. The raised face (measured) is x[103,150],
            // y[21,61] -> half ~32 px wide / ~27 px tall on screen; keep the arrows inside
            // it so they don't touch the rims. The live glows sit at the same spots.
            const int dax = 24, day = 19;
            // Dim the whole numpad + face keys to signal "unused in this layout".
            for (int i = 0; i < 12; ++i) {
                roundRectFillA(cx[i] - chw, cy[i] - chh, 2 * chw, 2 * chh, 8, 8, 12, 22, 150);
            }
            // Live highlights.
            if (ok) {
                if (up)    kbHilite(dcx, dcy - day, 6, 6);
                if (down)  kbHilite(dcx, dcy + day, 6, 6);
                if (left)  kbHilite(dcx - dax, dcy, 6, 6);
                if (right) kbHilite(dcx + dax, dcy, 6, 6);
                if (b.cross) kbHilite(dcx, dcy, 7, 7);                 // fire (D-pad centre)
                if (b.l1) kbHilite(cx[12], cy[12], chw - 2, 8);
                if (b.r1) kbHilite(cx[13], cy[13], chw - 2, 8);
            }
            // Static markers: arrows out on the four arms, Cross (fire) in the centre.
            fillTriangle(dcx, dcy - day - 4, dcx - 5, dcy - day + 3, dcx + 5, dcy - day + 3, 150, 210, 245);
            fillTriangle(dcx - 5, dcy + day - 3, dcx + 5, dcy + day - 3, dcx, dcy + day + 4, 150, 210, 245);
            fillTriangle(dcx - dax - 4, dcy, dcx - dax + 3, dcy - 5, dcx - dax + 3, dcy + 5, 150, 210, 245);
            fillTriangle(dcx + dax + 4, dcy, dcx + dax - 3, dcy - 5, dcx + dax - 3, dcy + 5, 150, 210, 245);
            glyphCross(dcx, dcy, 7, 120, 160, 235);                    // Cross = Fire
            // Soft keys: the trace IS the button; label it just above (the short "L1"/"R1"
            // centred ON the wider trace read as misaligned, so caption it instead).
            drawTextVC(cx[12] - textWidth("L1", 11.0f) / 2, cy[12] - 12, "L1", 200, 220, 245, 11.0f);
            drawTextVC(cx[13] - textWidth("R1", 11.0f) / 2, cy[13] - 12, "R1", 200, 220, 245, 11.0f);

            // Legend.
            int lx = kx + kbW + 22, ly = ky + 6;
            drawText(lx, ly, "MAPPING", 150, 185, 220, 14.0f); ly += 26;
            const int gx = lx + 8, tx = lx + 22, vx = lx + 150;
            drawText(lx, ly, "D-pad / L-stick", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Arrows", 160, 195, 235, 13.0f); ly += 22;
            glyphCross(gx, ly + 6, 6, 120, 160, 235);
            drawText(tx, ly, "Cross", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Fire", 160, 195, 235, 13.0f); ly += 22;
            drawText(lx, ly, "L1", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Soft 1", 160, 195, 235, 13.0f); ly += 22;
            drawText(lx, ly, "R1", 220, 230, 245, 13.0f);
            drawText(vx, ly, "Soft 2", 160, 195, 235, 13.0f); ly += 26;
            drawText(lx, ly, "Other buttons", 150, 175, 205, 12.0f);
            drawText(vx, ly, "unused", 150, 175, 205, 12.0f);
        }
}

} // namespace

void Ps2Frontend::controlsGuide(int layout) {
    if (!ensureVideo()) {
        return;
    }
    int kw = 0, kh = 0;
    unsigned char* kb = MidletIcon::decodePng(g_keyboard_png, (int)g_keyboard_png_len, &kw, &kh);

    hal::IPad* pad = hal::Keypad::instance().pad();
    // Exit on HOLD (not tap) so START itself stays testable: a quick press highlights it
    // like any other button; holding it for HOLD_MS leaves. Arm only after START has been
    // released once, so the press that opened the guide doesn't immediately count.
    const unsigned HOLD_MS = 2000;
    bool     armed = false;
    unsigned holdBegin = 0;

    for (;;) {
        hal::PadButtons b;
        const bool ok = (pad != 0 && pad->ensureReady() && pad->read(&b));
        const unsigned now = (unsigned)hal::SystemClock::instance().elapsedMillis();
        unsigned heldMs = 0;
        if (ok) {
            if (!b.start) {
                armed = true;
                holdBegin = 0;
            } else if (armed) {
                if (holdBegin == 0) holdBegin = now ? now : 1;
                heldMs = now - holdBegin;
            }
        }
        // When the hold completes, render THIS frame with the bar full (clamp), present it
        // and leave at once -- no waiting for release (the caller ignores the held START).
        const bool doExit = (heldMs >= HOLD_MS);
        if (doExit) heldMs = HOLD_MS;

        memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));
        rectVGrad(0, 0, g_w, 34, 26, 42, 78, 14, 24, 50);
        for (int x = 0; x < g_w; ++x) plotA(x, 34, 60, 90, 140, 255);
        drawTextVC(MARGIN, 17, layout == 1 ? "CONTROLS  -  COMPLETE"
                                           : "CONTROLS  -  SIMPLE", 210, 228, 248, 18.0f);

        drawControlsBody(layout, kb, kw, kh, ok, b);

        // Footer hint + hold-to-exit progress (fills the footer's top hairline as START
        // is held; START stays free to test with a quick press).
        rectVGrad(0, FOOTER_Y, g_w, FOOTER_H, 26, 42, 78, 14, 24, 50);
        for (int x = 0; x < g_w; ++x) plotA(x, FOOTER_Y, 60, 90, 140, 255);
        drawTextVC(MARGIN, FOOTER_Y + FOOTER_H / 2,
                   "Press any button to test the mapping", 190, 210, 235, 14.0f);
        {
            const char* back = "HOLD START: EXIT";
            drawTextVC(g_w - MARGIN - textWidth(back, 14.0f), FOOTER_Y + FOOTER_H / 2,
                       back, heldMs > 0 ? 130 : 190, heldMs > 0 ? 225 : 210, 255, 14.0f);
        }
        if (heldMs > 0) {
            int fw = (int)((unsigned long long)g_w * heldMs / HOLD_MS);
            if (fw > g_w) fw = g_w;
            for (int yy = FOOTER_Y; yy <= FOOTER_Y + 2; ++yy)
                for (int x = 0; x < fw; ++x) plotA(x, yy, 120, 225, 255, 255);
        }

        GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
        if (doExit) break;   // the full-bar frame is on screen now -- return immediately
        waitFrame();
    }

    if (kb != 0) {
        MidletIcon::release(kb);
    }
}

void Ps2Frontend::chooseLayout() {
    if (!ensureVideo()) {
        return;
    }
    int kw = 0, kh = 0;
    unsigned char* kb = MidletIcon::decodePng(g_keyboard_png, (int)g_keyboard_png_len, &kw, &kh);

    hal::IPad* pad = hal::Keypad::instance().pad();
    int layout = Settings::instance().controlLayout() == 1 ? 1 : 0;   // seed from current default
    hal::PadButtons prev;
    bool pnl = false, pnr = false;   // previous left/right nav state (D-pad OR left stick)
    bool confirmed = false;

    for (;;) {
        hal::PadButtons b;
        const bool ok = (pad != 0 && pad->ensureReady() && pad->read(&b));
        if (ok) {
            // Switch with LEFT/RIGHT from the D-pad OR the left analog stick (the picker's
            // guide is static, so the stick has no other job here).
            const bool nl = b.left  || b.lx < 128 - 40;
            const bool nr = b.right || b.lx > 128 + 40;
            if (nl && !pnl) layout = 0;   // SIMPLE
            if (nr && !pnr) layout = 1;   // COMPLETE
            pnl = nl; pnr = nr;
            if (b.cross && !prev.cross) confirmed = true;
            prev = b;
        }

        memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));

        // Header: prompt on the left + a SIMPLE | COMPLETE segmented toggle on the right.
        rectVGrad(0, 0, g_w, 40, 26, 42, 78, 14, 24, 50);
        for (int x = 0; x < g_w; ++x) plotA(x, 40, 60, 90, 140, 255);
        drawTextVC(MARGIN, 20, "CHOOSE YOUR CONTROL LAYOUT", 210, 228, 248, 15.0f);
        {
            const char* names[2] = { "SIMPLE", "COMPLETE" };
            const int pw = 104, ph = 24, gap = 8, py = 8;
            const int px0 = g_w - MARGIN - (pw * 2 + gap);
            for (int i = 0; i < 2; ++i) {
                const int px = px0 + i * (pw + gap);
                const bool sel = (layout == i);
                if (sel) {
                    roundRectFillA(px - 2, py - 2, pw + 4, ph + 4, 8, 90, 215, 255, 255);
                    roundRectVGrad(px, py, pw, ph, 7, 46, 88, 148, 30, 54, 100);
                } else {
                    roundRectVGrad(px, py, pw, ph, 7, 34, 52, 92, 22, 36, 68);
                }
                const int tw = textWidth(names[i], 14.0f);
                drawTextVC(px + (pw - tw) / 2, py + ph / 2, names[i],
                           sel ? 255 : 170, sel ? 255 : 195, 255, 14.0f);
            }
        }

        // Static preview only (ok = false): the picker's guide is purely visual -- it does
        // not react to button presses, only shows the selected layout's mapping.
        drawControlsBody(layout, kb, kw, kh, false, b);

        // Footer.
        rectVGrad(0, FOOTER_Y, g_w, FOOTER_H, 26, 42, 78, 14, 24, 50);
        for (int x = 0; x < g_w; ++x) plotA(x, FOOTER_Y, 60, 90, 140, 255);
        drawTextVC(MARGIN, FOOTER_Y + FOOTER_H / 2, "LEFT / RIGHT: SWITCH", 190, 210, 235, 14.0f);
        {
            const char* c = "CROSS: CONFIRM";
            drawTextVC(g_w - MARGIN - textWidth(c, 14.0f), FOOTER_Y + FOOTER_H / 2, c, 190, 210, 235, 14.0f);
        }

        GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
        if (confirmed) break;
        waitFrame();
    }

    // Persist the choice as the global default and mark the picker done (so it is a
    // one-time, first-run screen).
    Settings::instance().setControlLayout(layout);
    Settings::instance().setLayoutChosen(true);

    // Swallow the confirm press so the menu grid doesn't read it as a game launch.
    {
        hal::PadButtons r;
        while (pad != 0 && pad->ensureReady() && pad->read(&r) && r.cross) {
            waitFrame();
        }
    }

    if (kb != 0) {
        MidletIcon::release(kb);
    }
}

void Ps2Frontend::logEnable(bool on) {
    if (on) {
        // Begin the debug split view straight away (no fullscreen trace step): clear the
        // rolling log and the cached game frame, and paint the initial "loading" split
        // (black left with a placeholder, empty log right). The game fills the left half
        // once it draws (debugPresent); log lines fill the right half live meanwhile.
        g_launchMode = 3;
        g_logCount   = 0;
        g_logCur     = 0;
        g_dbgRaster  = 0;
        g_dbgW = 0; g_dbgH = 0;
        debugSplitRender(0, 0, 0);
    } else {
        g_launchMode = 0;         // tear down any overlay (game draws / back to menu)
        g_dbgRaster  = 0;
    }
}

void Ps2Frontend::loadingBegin(int gameIndex) {
    g_launchMode = 2;             // friendly loading screen
    g_loadGame   = gameIndex;
    g_loadStage  = 0;
    g_loadFail   = false;
    g_logCur     = 0;             // fresh line buffer for milestone parsing
    const char* nm = hal::GameStorage::instance().nameAt(gameIndex);
    stripJar(g_loadName, (int)sizeof(g_loadName), nm != 0 ? nm : "?");
    loadingRender();              // paint the initial "Preparing..." frame at once
}

void Ps2Frontend::logWrite(const char* s, int len) {
    if (g_launchMode == 0 || s == 0) {
        return;
    }
    if (g_launchMode == 2) {      // friendly loader: parse milestones, no raw text
        loadingFeed(s, len);
        return;
    }
    // Debug split view (mode 3): buffer the teed VM/native stdout into lines.
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
    // Re-render the split on each completed line, reusing the last game frame, so the log
    // updates live even before the game's first frame and between its frames.
    if (sawNewline) {
        debugSplitRender(g_dbgRaster, g_dbgW, g_dbgH);
    }
}

bool Ps2Frontend::debugPresent(const unsigned short* raster565, int w, int h) {
    if (!Settings::instance().debugMode()) {
        return false;             // not debugging -> caller presents the game fullscreen
    }
    g_launchMode = 3;
    g_dbgRaster  = raster565;     // cache this frame so between-frame log lines can reuse it
    g_dbgW = w; g_dbgH = h;
    debugSplitRender(raster565, w, h);
    return true;
}

int Ps2Frontend::pick() {
    logEnable(false);             // menu owns the screen again; stop the launch trace
    if (!initFont() || !ensureVideo()) {
        return -1;
    }
    loadTitleIcon();

    // Video is up: let memory-card writes animate the discreet "saving" spinner. Idempotent
    // across repeated pick() calls, and left registered afterwards so a later game save shows
    // it too.
    Ps2MemCard::instance().setIoActivity(memCardIoActivity);

    // Menu is up: pause the streaming mixer -- the menu's BGM plays on a hardware ADPCM voice,
    // so the mixer (which only pushes silence here) would just fight it for SPU2 RAM / IOP-SIF
    // bandwidth and stall the icon worker. Undone on the way out to a game (below). The BGM
    // itself is started below, once Settings is loaded (it's gated on the "Menu music" toggle).
    Ps2Audio::instance().pauseMixer();

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
    Resolutions::instance().load(count);    // per-game canvas resolution overrides
    ControlLayouts::instance().load(count); // per-game control layout overrides
    Settings::instance().load();            // launcher preferences
    g_sortMode = Settings::instance().defaultSort();   // startup sort order

    // Start (or resume) the menu background music now that the toggle is known. Disabled ->
    // stopBgm() keeps the voice muted (a safe no-op when it was never started). Persists the
    // track position across menu visits, so returning from a game resumes where it left off.
    if (Settings::instance().bgmEnabled()) {
        Ps2Audio::instance().startBgm();
    } else {
        Ps2Audio::instance().stopBgm();
    }

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

    // First run only (after the splash, before the menu is shown): let the user pick the
    // global control layout, live-previewing each guide. Persists the choice and a "done"
    // flag so it never shows again. Needs the pad + vsync pacing up (both are, above).
    if (!Settings::instance().layoutChosen()) {
        chooseLayout();
    }

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
    int  detailsFocus = 0;        // details panel focus: 0=off, 1=SCREEN field, 2=PAD field
    int  lastDetailsFocus = -1;
    int  settingsSel = 0;         // highlighted SETTINGS row
    int  lastSettingsSel = -1;
    bool firstFrame = true;
    bool startStuck = false;      // ignore a START still held after the controls guide exits
    hal::PadButtons prev;         // all-false initial snapshot
    rebuildView(activeTab, count);   // initial view (all games)

    // Coming back from a game: put the cursor back on the game we just launched (in the
    // ALL GAMES view we start on) instead of resetting to the first cell. The row-clamp in
    // the loop below scrolls it into view. One-shot: cleared once consumed.
    if (g_restoreGame >= 0) {
        for (int i = 0; i < g_viewCount; ++i) {
            if (g_view[i] == g_restoreGame) { selected = i; break; }
        }
        g_restoreGame = -1;
    }

    while (chosen < 0) {
        // Poll the pad every field so input stays responsive even when we skip the
        // (expensive) redraw below.
        bool favToggled = false;
        bool sortChanged = false;
        bool settingsChanged = false;
        bool resChanged = false;
        hal::PadButtons b;
        if (pad != 0 && pad->ensureReady() && pad->read(&b)) {
            // Tab switching (L1/R1): resets the view + selection + sidebar/details/settings focus.
            if (b.r1 && !prev.r1 && activeTab < 2) {
                activeTab++; rebuildView(activeTab, count);
                selected = 0; topRow = 0; sidebarFocus = false; detailsFocus = 0; settingsSel = 0;
            }
            if (b.l1 && !prev.l1 && activeTab > 0) {
                activeTab--; rebuildView(activeTab, count);
                selected = 0; topRow = 0; sidebarFocus = false; detailsFocus = 0; settingsSel = 0;
            }
            // Back (Circle): unfocus the sidebar or details panel, else return to ALL GAMES.
            // At the root (ALL GAMES with nothing focused) it does nothing.
            if (b.circle && !prev.circle) {
                if (sidebarFocus) {
                    sidebarFocus = false;
                } else if (detailsFocus != 0) {
                    detailsFocus = 0;
                } else if (activeTab != 0) {
                    activeTab = 0; rebuildView(activeTab, count);
                    selected = 0; topRow = 0; settingsSel = 0;
                }
            }
            // START (from anywhere): open the controls guide / live tester for the layout
            // in effect -- the selected game's (grid) or the global default -- then repaint
            // the menu over its last frame on return. The guide exits on a held START, so
            // ignore that still-down START here until it is released (startStuck).
            if (!b.start) startStuck = false;
            if (b.start && !prev.start && !startStuck) {
                const int g = ((activeTab == 0 || activeTab == 1) && g_viewCount > 0)
                                  ? g_view[selected] : -1;
                controlsGuide(effectiveLayout(g));
                firstFrame = true;
                startStuck = true;
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
                        case 4: Settings::instance().setDebugMode(
                                    !Settings::instance().debugMode()); break;
                        case 5: Settings::instance().setFpsCounter(
                                    !Settings::instance().fpsCounter()); break;
                        case 6: {
                            // Toggle the menu BGM and apply it immediately -- the user is
                            // sitting in the menu, so start/mute the hardware voice now.
                            const bool on = !Settings::instance().bgmEnabled();
                            Settings::instance().setBgmEnabled(on);
                            if (on) Ps2Audio::instance().startBgm();
                            else    Ps2Audio::instance().stopBgm();
                            break;
                        }
                        case 7: Settings::instance().setControlLayout(
                                    Settings::instance().controlLayout() ^ 1); break;
                        case 8: controlsGuide(effectiveLayout(-1));           // global layout
                                startStuck = true; break;   // swallow the held START on exit
                    }
                    settingsChanged = true;
                }
            } else if (g_viewCount > 0 && (activeTab == 0 || activeTab == 1)) {
                // Select toggles the details panel (per-game screen size). Dedicated button
                // so focusing it never moves the grid selection.
                if (b.select && !prev.select && !sidebarFocus) {
                    detailsFocus = detailsFocus ? 0 : 1;   // open on the SCREEN field
                }
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
                } else if (detailsFocus != 0) {
                    // Details panel focused: up/down pick the field (1=screen, 2=pad);
                    // left/right change the focused field's value (persisted); circle backs
                    // out (handled above); cross launches.
                    if (b.up   && !prev.up)   { detailsFocus = 1; }
                    if (b.down && !prev.down) { detailsFocus = 2; }
                    if (detailsFocus == 1) {
                        if (b.left  && !prev.left)  { Resolutions::instance().cycle(g_view[selected], -1); resChanged = true; }
                        if (b.right && !prev.right) { Resolutions::instance().cycle(g_view[selected], +1); resChanged = true; }
                    } else {
                        if (b.left  && !prev.left)  { ControlLayouts::instance().cycle(g_view[selected], -1); resChanged = true; }
                        if (b.right && !prev.right) { ControlLayouts::instance().cycle(g_view[selected], +1); resChanged = true; }
                    }
                    if (b.cross && !prev.cross) { chosen = g_view[selected]; }
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
        // redrawing every field to animate the marquee (grid tabs only). But hold the
        // animation while icons are still loading: a per-frame render keeps the main
        // thread off the vsync block, starving the low-priority icon worker (it would
        // never get the idle CPU to decode). The marquee resumes once the grid is filled.
        bool marquee = false;
        if (g_viewCount > 0 && !moved && !sidebarFocus && !detailsFocus &&
            (activeTab == 0 || activeTab == 1) && !IconCache::instance().hasWork()) {
            const char* nm = hal::GameStorage::instance().nameAt(g_view[selected]);
            char base[128];
            stripJar(base, (int)sizeof(base), nm != 0 ? nm : "?");
            marquee = textWidth(base, NAME_PX) > (GRID_CELLW - 8);
        }

        // Render on demand: only when something changed -- navigation, a tab switch, a
        // sidebar/details-focus change, a favourite toggle, a screen-size change, a
        // marquee, a decoded icon, or a new clock second.
        const bool tabChanged = (activeTab != lastTab);
        const bool sbChanged  = (sidebarFocus != lastSbFocus) || (sidebarSel != lastSbSel);
        const bool detChanged = (detailsFocus != lastDetailsFocus) || resChanged;
        const bool setChanged = (settingsSel != lastSettingsSel) || settingsChanged;
        const bool dirty = IconCache::instance().takeDirty();
        const int nowSec = (int)(hal::SystemClock::instance().elapsedMillis() / 1000);
        const bool clockTick = (nowSec != lastClockSec);
        if (firstFrame || moved || tabChanged || sbChanged || detChanged || setChanged ||
            favToggled || sortChanged || marquee || dirty || clockTick) {
            if (marquee) {
                g_marqueeTick++;
            }
            IconCache::instance().tick();
            render(g_viewCount, selected, topRow, activeTab, g_view,
                   sidebarFocus, sidebarSel, settingsSel, detailsFocus);
            GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
            lastSel          = selected;
            lastTop          = topRow;
            lastTab          = activeTab;
            lastSbFocus      = sidebarFocus;
            lastSbSel        = sidebarSel;
            lastDetailsFocus = detailsFocus;
            lastSettingsSel  = settingsSel;
            lastClockSec     = nowSec;
            firstFrame       = false;
        }
        waitFrame();
    }

    // Pause the background music before handing off to a game so the game's audio plays
    // alone (the SPU2 voice keeps looping silently; the next pick() unmutes it), and resume
    // the streaming mixer the game needs for its own audio.
    Ps2Audio::instance().stopBgm();
    Ps2Audio::instance().resumeMixer();

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
    // Record the launch so it heads the recent row next time (worker is idle now), and
    // remember it so the next pick() restores the cursor onto it instead of cell 0.
    if (chosen >= 0) {
        Recent::instance().push(chosen);
        g_restoreGame = chosen;
    }
    return chosen;
}

} // namespace platform
} // namespace ps2
