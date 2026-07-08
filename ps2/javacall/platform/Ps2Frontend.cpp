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
}

void drawSidebar(int count, int selected) {
    const int y0 = CONTENT_Y;
    const int h  = CONTENT_BOTTOM - CONTENT_Y;
    panel(SIDE_X, y0, SIDE_W, h, 6);
    if (count <= 0 || g_initialCount <= 0) {
        return;
    }

    const char cur = initialOf(selected);
    const int rowH = 15;
    int maxShow = (h - 8) / rowH;
    if (maxShow < 1) maxShow = 1;
    const int n = g_initialCount;
    const int show = n < maxShow ? n : maxShow;

    int curIdx = 0;
    for (int k = 0; k < n; ++k) {
        if (g_initials[k] == cur) { curIdx = k; break; }
    }
    int start = curIdx - show / 2;
    if (start < 0) start = 0;
    if (start + show > n) start = n - show;

    int y = y0 + 6;
    for (int k = start; k < start + show; ++k) {
        const bool act = (g_initials[k] == cur);
        char s[2] = { g_initials[k], 0 };
        const int lw = textWidth(s, 13.0f);
        const int lx = SIDE_X + (SIDE_W - lw) / 2;
        if (act) {
            roundRectFillA(SIDE_X + 1, y - 1, SIDE_W - 2, rowH, 4, 90, 215, 255, 255);
            drawText(lx, y, s, 18, 38, 68, 13.0f);
        } else {
            drawText(lx, y, s, 150, 180, 215, 13.0f);
        }
        y += rowH;
    }
}

void drawScrollbar(int count, int topRow) {
    const int trackY = CONTENT_Y;
    const int trackH = CONTENT_BOTTOM - CONTENT_Y;
    roundRectVGrad(SCROLL_X, trackY, 6, trackH, 3, 20, 32, 60, 14, 22, 44);
    const int totalRows = (count + COLS - 1) / COLS;
    if (totalRows > ROWS) {
        int thumbH = trackH * ROWS / totalRows;
        if (thumbH < 16) thumbH = 16;
        const int maxTop = totalRows - ROWS;
        const int thumbY = trackY + (trackH - thumbH) * topRow / (maxTop > 0 ? maxTop : 1);
        roundRectVGrad(SCROLL_X, thumbY, 6, thumbH, 3, 100, 190, 240, 50, 110, 180);
    }
}

void drawGrid(int count, int selected, int topRow) {
    const int firstIdx = topRow * COLS;
    const int lastIdx  = (topRow + ROWS) * COLS;   // exclusive
    for (int i = firstIdx; i < count && i < lastIdx; ++i) {
        const int r = i / COLS;
        const int c = i % COLS;
        const int cellX = GRID_X0 + c * (GRID_CELLW + GRID_GAP);
        const int cellY = GRID_Y0 + (r - topRow) * GRID_ROWSTRIDE;
        const int cellH = GRID_ROWSTRIDE - 4;
        const bool sel  = (i == selected);

        if (sel) {
            drawSelection(cellX + 2, cellY, GRID_CELLW - 4, cellH, 8);
        } else {
            roundRectVGrad(cellX + 2, cellY, GRID_CELLW - 4, cellH, 8, 40, 60, 104, 24, 40, 76);
        }

        const int iconX = cellX + (GRID_CELLW - ICON) / 2;
        const int iconY = cellY + 3;
        drawIcon(iconX, iconY, i);

        const char* nm = hal::GameStorage::instance().nameAt(i);
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

void drawDetails(int count, int selected) {
    const int px = DET_X, py = CONTENT_Y, pw = DET_W, ph = CONTENT_BOTTOM - CONTENT_Y;
    panel(px, py, pw, ph, 10);
    drawText(px + 10, py + 8, "GAME DETAILS", 150, 185, 220, 12.0f);
    if (count <= 0) {
        return;
    }

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

    if (!IconCache::instance().drawScaled(selected, g_ras, g_w, g_h, prevX, prevY, prev)) {
        char c[2] = { initialOf(selected), 0 };
        drawText(prevX + prev / 2 - textWidth(c, 64.0f) / 2, prevY + prev / 2 - 36,
                 c, 210, 224, 244, 64.0f);
    }

    const char* nm = hal::GameStorage::instance().nameAt(selected);
    char lbl[40];
    makeLabel(lbl, (int)sizeof(lbl), nm != 0 ? nm : "?", (float)(pw - 16), 16.0f);
    const int lw = textWidth(lbl, 16.0f);
    drawText(px + (pw - lw) / 2, prevY + prev + gap1, lbl, 235, 242, 252, 16.0f);

    char info[32];
    snprintf(info, (int)sizeof(info), "%d / %d", selected + 1, count);
    const int iw = textWidth(info, 13.0f);
    drawText(px + (pw - iw) / 2, prevY + prev + gap1 + nameH + gap2, info, 150, 180, 215, 13.0f);
}

void drawFooter() {
    rectVGrad(0, FOOTER_Y, g_w, FOOTER_H, 26, 42, 78, 14, 24, 50);
    for (int x = 0; x < g_w; ++x) plotA(x, FOOTER_Y, 60, 90, 140, 255);   // top hairline
    const int cy = FOOTER_Y + FOOTER_H / 2;

    // One legend entry: a face-button glyph (drawn by kind) + its label.
    static const char* labels[4] = { "LAUNCH", "BACK", "FAVORITE", "SORT BY..." };
    const int glyphW = 16;   // glyph column (centre at +7) before the label
    const int itemGap = 26;

    // Measure so the whole legend can be centred across the footer.
    int total = 0;
    for (int i = 0; i < 4; ++i) {
        total += glyphW + textWidth(labels[i], 15.0f);
        if (i < 3) total += itemGap;
    }
    int x = (g_w - total) / 2;
    if (x < MARGIN) x = MARGIN;

    for (int i = 0; i < 4; ++i) {
        const int gcx = x + 7;
        if      (i == 0) glyphCross(gcx,    cy, 7, 100, 150, 235);
        else if (i == 1) glyphCircle(gcx,   cy, 7, 235,  92,  92);
        else if (i == 2) glyphTriangle(gcx, cy, 7,  92, 216, 150);
        else             glyphSquare(gcx,   cy, 7, 230, 112, 190);
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

void render(int count, int selected, int topRow, int activeTab) {
    memcpy(g_ras, g_bg, (size_t)g_w * g_h * sizeof(u16));   // baked background
    drawHeader();
    drawTabs(activeTab);
    drawSidebar(count, selected);
    drawFooter();

    // FAVORITES / SETTINGS: content not implemented yet -- show a placeholder so the
    // tab switch is visibly working. (Filled in by later features.)
    if (activeTab != 0) {
        drawContentMessage(activeTab == 1
            ? "No favorites yet -- mark games with TRIANGLE"
            : "Settings -- coming soon");
        return;
    }

    // ALL GAMES.
    drawScrollbar(count, topRow);
    drawDetails(count, selected);

    if (count <= 0) {
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

    drawGrid(count, selected, topRow);
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
    bool firstFrame = true;
    hal::PadButtons prev;         // all-false initial snapshot

    while (chosen < 0) {
        // Poll the pad every field so input stays responsive even when we skip the
        // (expensive) redraw below.
        hal::PadButtons b;
        if (pad != 0 && pad->ensureReady() && pad->read(&b)) {
            // Tab switching (L1/R1) works on any tab.
            if (b.r1 && !prev.r1 && activeTab < 2) { activeTab++; }
            if (b.l1 && !prev.l1 && activeTab > 0) { activeTab--; }
            // Grid navigation + launch only on the ALL GAMES tab.
            if (count > 0 && activeTab == 0) {
                if (b.right && !prev.right && selected + 1 < count)    { selected++; }
                if (b.left  && !prev.left  && selected > 0)            { selected--; }
                if (b.down  && !prev.down  && selected + COLS < count) { selected += COLS; }
                if (b.up    && !prev.up    && selected - COLS >= 0)    { selected -= COLS; }
                if (b.cross && !prev.cross)                            { chosen = selected; }
            }
            prev = b;
        }

        // Keep the selected row within the visible window.
        const int selRow = (count > 0) ? selected / COLS : 0;
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
        // redrawing every field to animate the marquee (ALL GAMES tab only).
        bool marquee = false;
        if (count > 0 && !moved && activeTab == 0) {
            const char* nm = hal::GameStorage::instance().nameAt(selected);
            char base[128];
            stripJar(base, (int)sizeof(base), nm != 0 ? nm : "?");
            marquee = textWidth(base, NAME_PX) > (GRID_CELLW - 8);
        }

        // Render on demand: only when something changed -- navigation, a tab switch, a
        // marquee in flight, a freshly decoded icon, or the clock ticking a new second.
        const bool tabChanged = (activeTab != lastTab);
        const bool dirty = IconCache::instance().takeDirty();
        const int nowSec = (int)(hal::SystemClock::instance().elapsedMillis() / 1000);
        const bool clockTick = (nowSec != lastClockSec);
        if (firstFrame || moved || tabChanged || marquee || dirty || clockTick) {
            if (marquee) {
                g_marqueeTick++;
            }
            IconCache::instance().tick();
            render(count, selected, topRow, activeTab);
            GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
            lastSel      = selected;
            lastTop      = topRow;
            lastTab      = activeTab;
            lastClockSec = nowSec;
            firstFrame   = false;
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
    return chosen;
}

} // namespace platform
} // namespace ps2
