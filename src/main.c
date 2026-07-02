/*
 * j2me-ps2  -  Marco 0: prova do caminho de video do HAL
 *
 * Reescrito para usar libdraw/libgraph (o caminho da referencia do ps2sdk),
 * modelado nos samples ee/font/samples/font.c e ee/draw/samples/texture.
 * A versao anterior com gsKit dava tela preta; libdraw e o metodo canonico.
 *
 *   [framebuffer de software RGBA5551 em RAM da EE]   <- "a tela do celular"
 *          |  (rasterizacao por CPU, estilo gxj/putpixel do phoneME)
 *          v
 *   [SyncDCache -> draw_texture_transfer (DMA) p/ VRAM]
 *          |
 *          v
 *   [draw_rect_textured: sprite 2D escalonada, aspecto preservado]
 */

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <tamtypes.h>
#include <kernel.h>

#include <dma.h>
#include <packet.h>
#include <graph.h>
#include <gs_psm.h>
#include <draw.h>
#include <draw2d.h>

#include "j2me_input.h"

/* --- tela de exibicao (framebuffer da TV) --- */
#define SCR_W 640
#define SCR_H 448          /* NTSC; PAL usaria 512 */

/* --- "tela virtual" do celular (portrait tipico de MIDlet) --- */
#define LCD_W 240
#define LCD_H 320
/* dimensoes potencia-de-2 usadas p/ registrar a textura no GS */
#define TEX_W 256          /* TBW e log2 usam 256 (>= LCD_W, mult. de 64) */
#define TEX_H 512          /* log2 usa 512 (>= LCD_H) */

/* Empacota RGB 8-bit no formato 16-bit do GS (CT16 = RGBA5551, R nos bits baixos). */
static inline u16 rgb1555(u8 r, u8 g, u8 b)
{
    return (u16)(((r >> 3)) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
}

static u16 *g_lcd;         /* framebuffer da tela virtual (alinhado 128) */
static u32  g_lcd_bytes;

/* recursos de GS/draw */
static framebuffer_t g_frame;
static zbuffer_t     g_z;
static texbuffer_t   g_texbuf;

#define BOX_W 48
#define BOX_H 48

/* -------------------------------------------------------------------------
 * "Rasterizador de teste" - placeholder do rasterizador do phoneME.
 * Agora a caixa e posicionada pelo chamador (controlada pelo pad no M1).
 * ---------------------------------------------------------------------- */
static void lcd_render(int frame, int bx, int by, u16 box_col)
{
    int x, y;

    /* fundo: gradiente vertical azul->preto (confere canais de cor) */
    for (y = 0; y < LCD_H; y++) {
        u8 b = (u8)(y * 255 / LCD_H);
        u16 line = rgb1555(0, (u8)(b / 3), b);
        u16 *row = g_lcd + y * LCD_W;
        for (x = 0; x < LCD_W; x++)
            row[x] = line;
    }

    /* caixa controlada pelo jogador */
    for (y = 0; y < BOX_H; y++) {
        u16 *row = g_lcd + (by + y) * LCD_W + bx;
        for (x = 0; x < BOX_W; x++)
            row[x] = box_col;
    }

    /* linha de varredura branca subindo/descendo (indicador de vida/anim) */
    {
        int sy = (frame * 4) % (2 * LCD_H);
        if (sy >= LCD_H) sy = 2 * LCD_H - 1 - sy;
        u16 *row = g_lcd + sy * LCD_W;
        for (x = 0; x < LCD_W; x++)
            row[x] = rgb1555(255, 255, 255);
    }

    /* marcadores de canto: R=sup-esq, G=sup-dir, B=inf-esq, branco=inf-dir */
    {
        int m = 12, i, j;
        for (j = 0; j < m; j++) for (i = 0; i < m; i++) {
            g_lcd[(j) * LCD_W + (i)]                          = rgb1555(255, 0, 0);
            g_lcd[(j) * LCD_W + (LCD_W - 1 - i)]              = rgb1555(0, 255, 0);
            g_lcd[(LCD_H - 1 - j) * LCD_W + (i)]              = rgb1555(0, 0, 255);
            g_lcd[(LCD_H - 1 - j) * LCD_W + (LCD_W - 1 - i)]  = rgb1555(255, 255, 255);
        }
    }
}

/* Inicializa o GS, aloca framebuffer e monta o ambiente de desenho 2D. */
static void init_env(void)
{
    packet_t *packet = packet_init(16, PACKET_NORMAL);
    qword_t *q = packet->data;

    g_frame.width   = SCR_W;
    g_frame.height  = SCR_H;
    g_frame.psm     = GS_PSM_32;
    g_frame.mask    = 0;
    g_frame.address = graph_vram_allocate(SCR_W, SCR_H, GS_PSM_32, GRAPH_ALIGN_PAGE);

    /* sem zbuffer (2D) */
    g_z.enable  = 0;
    g_z.method  = ZTEST_METHOD_GREATER;
    g_z.address = 0;
    g_z.mask    = 1;
    g_z.zsm     = 0;

    /* liga o primeiro framebuffer aos circuitos de leitura da TV */
    graph_initialize(g_frame.address, SCR_W, SCR_H, GS_PSM_32, 0, 0);

    /* ambiente de desenho (define XYOFFSET p/ coords de tela 0..W) */
    q = draw_setup_environment(q, 0, &g_frame, &g_z);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    dma_wait_fast();
    packet_free(packet);

    /* VRAM p/ a textura da tela virtual (16-bit) */
    g_texbuf.width   = TEX_W;
    g_texbuf.psm     = GS_PSM_16;
    g_texbuf.address = graph_vram_allocate(TEX_W, TEX_H, GS_PSM_16, GRAPH_ALIGN_BLOCK);
    g_texbuf.info.width      = draw_log2(TEX_W);
    g_texbuf.info.height     = draw_log2(TEX_H);
    g_texbuf.info.components  = TEXTURE_COMPONENTS_RGBA;
    g_texbuf.info.function    = TEXTURE_FUNCTION_MODULATE;
}

int main(int argc, char *argv[])
{
    packet_t *xfer;   /* pacote de upload da textura */
    packet_t *draw;   /* pacote de desenho           */
    int frame = 0;

    lod_t       lod;
    clutbuffer_t clut;
    texrect_t   rect;

    /* framebuffer da tela virtual */
    g_lcd_bytes = LCD_W * LCD_H * sizeof(u16);
    g_lcd = (u16 *)memalign(128, g_lcd_bytes);
    memset(g_lcd, 0, g_lcd_bytes);

    /* DMA + GS */
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    init_env();

    /* entrada (controle -> keypad J2ME) */
    int have_pad = input_init();
    printf("j2me-ps2 M1: pad %s\n", have_pad ? "OK" : "NAO ENCONTRADO");

    /* amostragem: NEAREST (pixels nitidos); sem CLUT (textura direta 16-bit) */
    lod.calculation = LOD_USE_K;
    lod.max_level = 0;
    lod.mag_filter = LOD_MAG_NEAREST;
    lod.min_filter = LOD_MIN_NEAREST;
    lod.l = 0;
    lod.k = 0;

    clut.storage_mode = CLUT_STORAGE_MODE1;
    clut.start = 0;
    clut.psm = 0;
    clut.load_method = CLUT_NO_LOAD;
    clut.address = 0;

    /* retangulo destino: encaixa portrait na TV preservando aspecto (pillarbox) */
    {
        float dh = (float)SCR_H;
        float dw = dh * (float)LCD_W / (float)LCD_H;   /* 448 * 240/320 = 336 */
        float dx = ((float)SCR_W - dw) * 0.5f;         /* (640-336)/2 = 152   */
        rect.v0.x = dx;        rect.v0.y = 0.0f;    rect.v0.z = 0;
        rect.t0.u = 0.0f;      rect.t0.v = 0.0f;
        rect.v1.x = dx + dw;   rect.v1.y = dh;      rect.v1.z = 0;
        rect.t1.u = (float)LCD_W; rect.t1.v = (float)LCD_H;
        rect.color.r = 0x80; rect.color.g = 0x80; rect.color.b = 0x80;
        rect.color.a = 0x80; rect.color.q = 1.0f;
    }

    xfer = packet_init(128, PACKET_NORMAL);
    draw = packet_init(128, PACKET_NORMAL);

    printf("j2me-ps2 M0 (libdraw): tela %dx%d -> %.0fx%.0f em %dx%d\n",
           LCD_W, LCD_H, rect.v1.x - rect.v0.x, rect.v1.y - rect.v0.y, SCR_W, SCR_H);

    /* estado da "aplicacao": caixa controlavel, comeca centralizada */
    int bx = (LCD_W - BOX_W) / 2;
    int by = (LCD_H - BOX_H) / 2;
    const int speed = 3;

    for (;;) {
        qword_t *q;
        j2me_keys_t k;
        u16 box_col = rgb1555(255, 160, 0);   /* laranja (parado/solto) */

        /* 0) le o controle e move a caixa (clamp nas bordas da tela virtual) */
        if (have_pad && input_poll(&k)) {
            if (k.left)  bx -= speed;
            if (k.right) bx += speed;
            if (k.up)    by -= speed;
            if (k.down)  by += speed;
            if (bx < 0) bx = 0;
            if (bx > LCD_W - BOX_W) bx = LCD_W - BOX_W;
            if (by < 0) by = 0;
            if (by > LCD_H - BOX_H) by = LCD_H - BOX_H;
            if (k.fire) box_col = rgb1555(0, 220, 255);  /* ciano enquanto Cross */
        }

        /* 1) o "MIDlet" pinta sua tela */
        lcd_render(frame, bx, by, box_col);

        /* 2) coerencia de cache antes do DMA ler a RAM da EE */
        SyncDCache(g_lcd, (u8 *)g_lcd + g_lcd_bytes);

        /* 3) upload da tela virtual -> VRAM */
        q = xfer->data;
        q = draw_texture_transfer(q, g_lcd, LCD_W, LCD_H, GS_PSM_16,
                                  g_texbuf.address, g_texbuf.width);
        q = draw_texture_flush(q);
        dma_channel_send_chain(DMA_CHANNEL_GIF, xfer->data, q - xfer->data, 0, 0);
        dma_wait_fast();

        /* 4) desenha: limpa a tela e desenha o quad texturizado escalonado */
        q = draw->data;
        q = draw_clear(q, 0, 0.0f, 0.0f, (float)SCR_W, (float)SCR_H, 0x00, 0x00, 0x20);
        q = draw_texture_sampling(q, 0, &lod);
        q = draw_texturebuffer(q, 0, &g_texbuf, &clut);
        q = draw_rect_textured(q, 0, &rect);
        q = draw_finish(q);
        dma_channel_send_normal(DMA_CHANNEL_GIF, draw->data, q - draw->data, 0, 0);
        dma_wait_fast();

        draw_wait_finish();
        graph_wait_vsync();

        frame++;
    }

    return 0;
}
