// PS2 JavaCall port — platform layer. Ps2Frontend implementation.
//
// Standalone native menu, no dependency on the Java VM. It renders a full native
// resolution (640x448) grid of games -- each an application icon with a name below,
// phone-launcher style -- into its own RGBA5551 raster with the embedded TrueType font,
// presents it fullscreen through the shared GsDisplay (no pillarbox), reads the shared
// pad backend, and lists games from hal::GameStorage.
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

#include <tamtypes.h>   // u16
#include <malloc.h>     // memalign / malloc / free
#include <stdlib.h>
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

// Phone-launcher grid: square icon + name label below, fixed 4 x 5 page.
const int   MARGIN   = 14;
const int   TITLE_H  = 46;
const int   COLS     = 4;
const int   ROWS     = 5;
const int   GAP      = 8;               // horizontal gap between cells
const int   VGAP     = 6;               // vertical gap between cells
const int   ICON     = 48;              // on-screen icon size (square)
const int   TITLE_ICON = 36;            // logo drawn left of the title text
const float TITLE_PX   = 30.0f;
const float NAME_PX    = 15.0f;

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
int  g_w = 0;
int  g_h = 0;

// The title logo (TITLE_ICON x TITLE_ICON RGBA8888), decoded once; null if it failed.
unsigned char* g_titleIcon = 0;

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

// Decode + downscale the embedded PS2ME title logo once into g_titleIcon.
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
    fillRect(x, y, ICON, ICON, rgba5551(70, 74, 96));
    const char* nm = hal::GameStorage::instance().nameAt(i);
    char c[2];
    c[0] = (nm != 0 && nm[0] != 0) ? nm[0] : '?';
    c[1] = '\0';
    drawText(x + ICON / 2 - textWidth(c, 40.0f) / 2, y + ICON / 2 - 22,
             c, 230, 230, 240, 40.0f);
}

void render(int count, int selected, int topRow, int cellW, int rowStride,
            int gridY, int visibleRows) {
    // Ice-white background + subtle header (light theme).
    fillRect(0, 0, g_w, g_h, rgba5551(236, 240, 245));
    fillRect(0, 0, g_w, TITLE_H, rgba5551(218, 224, 234));
    int titleX = MARGIN;
    if (g_titleIcon != 0) {
        blitRGBA(g_titleIcon, TITLE_ICON, TITLE_ICON, MARGIN, (TITLE_H - TITLE_ICON) / 2);
        titleX = MARGIN + TITLE_ICON + 12;
    }
    drawText(titleX, 10, "PS2ME", 40, 52, 84, TITLE_PX);

    if (count <= 0) {
        // No games: show the storage-resolution trace on screen (the EE console is
        // invisible on real hardware booted standalone from USB).
        drawText(MARGIN, gridY + 2, "Nenhum jogo encontrado. Diagnostico:",
                 150, 60, 60, NAME_PX + 2);
        const char* p = Ps2Storage::instance().diagText();
        int ly = gridY + 28;
        char line[160];
        int li = 0;
        for (;; ++p) {
            if (*p == '\n' || *p == '\0') {
                line[li] = '\0';
                if (li > 0) {
                    drawText(MARGIN, ly, line, 40, 46, 70, 15.0f);
                    ly += 19;
                }
                li = 0;
                if (*p == '\0') {
                    break;
                }
            } else if (li < (int)sizeof(line) - 1) {
                line[li++] = *p;
            }
        }
        return;
    }

    // Only the visible page is drawn (the grid can hold ~1200 games).
    const int firstIdx = topRow * COLS;
    const int lastIdx  = (topRow + visibleRows) * COLS;   // exclusive
    for (int i = firstIdx; i < count && i < lastIdx; ++i) {
        const int r = i / COLS;
        const int c = i % COLS;
        const int cellX = MARGIN + c * (cellW + GAP);
        const int cellY = gridY + (r - topRow) * rowStride;
        const int cellH = rowStride - VGAP;
        const bool sel = (i == selected);

        if (sel) {
            fillRect(cellX, cellY - 2, cellW, cellH, rgba5551(70, 130, 225));
        }

        drawIcon(cellX + (cellW - ICON) / 2, cellY, i);

        const char* nm = hal::GameStorage::instance().nameAt(i);
        if (nm == 0) {
            nm = "?";
        }
        const int nameY = cellY + ICON + 6;
        const int pad   = 4;
        const int clipL = cellX + pad;
        const int clipR = cellX + cellW - pad;
        const int avail = cellW - 2 * pad;
        // Light text over the highlight; dark text on the ice-white background.
        const int tr = sel ? 255 : 55;
        const int tg = sel ? 255 : 62;
        const int tb = sel ? 255 : 82;

        char base[128];
        stripJar(base, (int)sizeof(base), nm);
        const int fullW = textWidth(base, NAME_PX);

        if (!sel || fullW <= avail) {
            // Static: truncate ("..") when it overflows, otherwise centre.
            char label[48];
            makeLabel(label, (int)sizeof(label), nm, (float)avail, NAME_PX);
            const int tw = textWidth(label, NAME_PX);
            drawText(cellX + (cellW - tw) / 2, nameY, label, tr, tg, tb, NAME_PX);
        } else {
            // Active item whose name overflows: auto-shift (marquee) the full name,
            // clipped to the cell, with a seamless wrap and a brief start pause.
            const int period = fullW + 32;             // trailing gap before repeat
            int t = (int)g_marqueeTick - 45;           // hold at the start briefly
            if (t < 0) t = 0;
            const int off = (t / 2) % period;          // ~30 px/s at 60 fps
            const int x0  = clipL - off;
            drawTextClip(x0,          nameY, base, tr, tg, tb, NAME_PX, clipL, clipR);
            drawTextClip(x0 + period, nameY, base, tr, tg, tb, NAME_PX, clipL, clipR);
        }
    }
}

// Allocate the DMA-aligned native-resolution raster and bring up the shared GS.
bool ensureVideo() {
    if (g_ras == 0) {
        const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(u16);
        g_ras = (u16*)memalign(128, bytes);
        if (g_ras == 0) {
            return false;
        }
        g_w = SCREEN_W;
        g_h = SCREEN_H;
    }
    return GsDisplay::instance().init();
}

// --- On-screen launch log (real-HW diagnostics) -----------------------------------
// The VM's stdout (System.out via javacall_print -> StdoutSink) is teed here during
// the launch window so the [Launcher] milestones + any exception traces render on the
// native GS -- the only console left once the USB IOP reset kills the SIF tty. Bytes
// are buffered into lines; each newline pushes a line and repaints. Bounded, no heap.
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
    drawText(MARGIN, 8, "PS2ME - iniciando jogo", 210, 220, 235, TITLE_PX);
    int ly = TITLE_H + 8;
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
    fillRect(0, 0, g_w, g_h, rgba5551(236, 240, 245));
    drawText(MARGIN, g_h / 2 - 16, "Carregando...", 60, 70, 95, 24.0f);
    GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);

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

    // Fixed 4 x 5 grid page.
    const int gridY       = TITLE_H + 4;
    const int cellW       = (g_w - 2 * MARGIN - (COLS - 1) * GAP) / COLS;
    const int rowStride   = (g_h - gridY - 6) / ROWS;
    const int visibleRows = ROWS;

    int selected = 0;
    int topRow = 0;
    int lastSel = -1;
    int lastTop = -1;
    int chosen = -1;
    bool firstFrame = true;
    hal::PadButtons prev;   // all-false initial snapshot

    while (chosen < 0) {
        // Poll the pad every field so input stays responsive even when we skip the
        // (expensive) redraw below.
        hal::PadButtons b;
        if (pad != 0 && pad->ensureReady() && pad->read(&b)) {
            if (count > 0) {
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
        if (selRow >= topRow + visibleRows) {
            topRow = selRow - visibleRows + 1;
        }

        const bool moved = (selected != lastSel) || (topRow != lastTop);
        if (moved) {
            g_marqueeTick = 0;   // restart the marquee for the newly active item
            IconCache::instance().pause();   // back the worker off so nav stays smooth
        }

        // Does the active item's name overflow its cell? Only then must we keep
        // redrawing every field to animate the marquee.
        bool marquee = false;
        if (count > 0 && !moved) {
            const char* nm = hal::GameStorage::instance().nameAt(selected);
            char base[128];
            stripJar(base, (int)sizeof(base), nm != 0 ? nm : "?");
            marquee = textWidth(base, NAME_PX) > (cellW - 8);
        }

        // Render on demand: only when something changed -- navigation, a marquee in
        // flight, or a freshly decoded icon. Otherwise stay blocked on vsync so the
        // low-priority icon worker gets the whole field to decode in.
        const bool dirty = IconCache::instance().takeDirty();
        if (firstFrame || moved || marquee || dirty) {
            if (marquee) {
                g_marqueeTick++;
            }
            IconCache::instance().tick();
            render(count, selected, topRow, cellW, rowStride, gridY, visibleRows);
            GsDisplay::instance().presentFullscreen(g_ras, g_w, g_h);
            lastSel    = selected;
            lastTop    = topRow;
            firstFrame = false;
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
