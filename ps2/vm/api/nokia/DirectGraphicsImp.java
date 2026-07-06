/*
 * Nokia UI API -- com.nokia.mid.ui.DirectGraphicsImp (PS2 phoneME compatibility shim).
 *
 * DirectGraphics implementation backed by a MIDP 2.0 Graphics. Everything maps onto
 * standard primitives:
 *   - drawImage(manipulation)  -> Graphics.drawRegion() with the matching transform
 *   - drawPixels(...)          -> unpack the packed format to ARGB, then drawRGB()
 *                                 (or createRGBImage()+drawRegion() when rotated)
 *   - draw/fillTriangle        -> Graphics.drawLine() / Graphics.fillTriangle()
 *   - draw/fillPolygon         -> line loop / triangle fan (convex approximation)
 *
 * Limitations (documented): getPixels() cannot read back from a MIDP Graphics, so it
 * is a no-op; sub-byte gray formats are best-effort. These are uncommon paths.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.ui;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

class DirectGraphicsImp implements DirectGraphics {

    private final Graphics g;
    private int alpha = 0xff;   // alpha byte of the last setARGBColor()

    DirectGraphicsImp(Graphics graphics) {
        this.g = graphics;
    }

    public void setARGBColor(int argbColor) {
        alpha = (argbColor >>> 24) & 0xff;
        g.setColor(argbColor & 0x00ffffff);
    }

    public int getAlphaComponent() {
        return alpha;
    }

    public int getNativePixelFormat() {
        // Our LCD framebuffer is RGB565 (see javacall_lcd), so advertise that.
        return TYPE_USHORT_565_RGB;
    }

    // --- image blit with rotation/flip -----------------------------------------

    public void drawImage(Image img, int x, int y, int anchor, int manipulation) {
        if (img == null) {
            return;
        }
        g.drawRegion(img, 0, 0, img.getWidth(), img.getHeight(),
                     toTransform(manipulation), x, y, anchor);
    }

    // Map a Nokia manipulation (rotation is counter-clockwise, optionally OR-ed with a
    // flip) to a MIDP transform code (javax.microedition.lcdui.game.Sprite): NONE=0,
    // MIRROR_ROT180=1, MIRROR=2, ROT180=3, MIRROR_ROT270=4, ROT90=5, MIRROR_ROT90=6,
    // ROT270=7. Single rotations/flips (the common cases) are exact; unusual combos
    // fall back to NONE.
    private static int toTransform(int manip) {
        switch (manip) {
            case 0:                              return 0;   // TRANS_NONE
            case FLIP_HORIZONTAL:                return 2;   // TRANS_MIRROR
            case FLIP_VERTICAL:                  return 1;   // TRANS_MIRROR_ROT180
            case ROTATE_90:                      return 7;   // Nokia CCW90 == MIDP CW270
            case ROTATE_180:                     return 3;   // TRANS_ROT180
            case ROTATE_270:                     return 5;   // Nokia CCW270 == MIDP CW90
            case ROTATE_90 | FLIP_HORIZONTAL:    return 6;   // TRANS_MIRROR_ROT90
            case ROTATE_90 | FLIP_VERTICAL:      return 4;   // TRANS_MIRROR_ROT270
            default:                             return 0;
        }
    }

    // --- drawPixels: unpack to ARGB, then blit ---------------------------------

    public void drawPixels(int[] pixels, boolean transparency, int offset, int scanlength,
                           int x, int y, int width, int height, int manipulation, int format) {
        if (pixels == null || width <= 0 || height <= 0) {
            return;
        }
        final boolean opaque = (format == TYPE_INT_888_RGB) || !transparency;
        int[] argb = new int[width * height];
        for (int row = 0; row < height; row++) {
            int src = offset + row * scanlength;
            int dst = row * width;
            for (int col = 0; col < width; col++) {
                int p = pixels[src + col];
                argb[dst + col] = opaque ? (0xff000000 | (p & 0x00ffffff)) : p;
            }
        }
        blit(argb, !opaque, x, y, width, height, manipulation);
    }

    public void drawPixels(short[] pixels, boolean transparency, int offset, int scanlength,
                           int x, int y, int width, int height, int manipulation, int format) {
        if (pixels == null || width <= 0 || height <= 0) {
            return;
        }
        int[] argb = new int[width * height];
        boolean hasAlpha = false;
        for (int row = 0; row < height; row++) {
            int src = offset + row * scanlength;
            int dst = row * width;
            for (int col = 0; col < width; col++) {
                int v = pixels[src + col] & 0xffff;
                int a = 0xff, r, gg, b;
                switch (format) {
                    case TYPE_USHORT_4444_ARGB:
                        a = (v >>> 12) & 0xf; r = (v >>> 8) & 0xf; gg = (v >>> 4) & 0xf; b = v & 0xf;
                        a = (a << 4) | a; r = (r << 4) | r; gg = (gg << 4) | gg; b = (b << 4) | b;
                        break;
                    case TYPE_USHORT_1555_ARGB:
                        a = ((v >>> 15) & 1) != 0 ? 0xff : 0;
                        r = (v >>> 10) & 0x1f; gg = (v >>> 5) & 0x1f; b = v & 0x1f;
                        r = (r << 3) | (r >>> 2); gg = (gg << 3) | (gg >>> 2); b = (b << 3) | (b >>> 2);
                        break;
                    case TYPE_USHORT_555_RGB:
                        r = (v >>> 10) & 0x1f; gg = (v >>> 5) & 0x1f; b = v & 0x1f;
                        r = (r << 3) | (r >>> 2); gg = (gg << 3) | (gg >>> 2); b = (b << 3) | (b >>> 2);
                        break;
                    case TYPE_USHORT_444_RGB:
                        r = (v >>> 8) & 0xf; gg = (v >>> 4) & 0xf; b = v & 0xf;
                        r = (r << 4) | r; gg = (gg << 4) | gg; b = (b << 4) | b;
                        break;
                    case TYPE_USHORT_565_RGB:
                    default:
                        r = (v >>> 11) & 0x1f; gg = (v >>> 5) & 0x3f; b = v & 0x1f;
                        r = (r << 3) | (r >>> 2); gg = (gg << 2) | (gg >>> 4); b = (b << 3) | (b >>> 2);
                        break;
                }
                if (!transparency) {
                    a = 0xff;
                }
                if (a != 0xff) {
                    hasAlpha = true;
                }
                argb[dst + col] = (a << 24) | (r << 16) | (gg << 8) | b;
            }
        }
        blit(argb, hasAlpha, x, y, width, height, manipulation);
    }

    public void drawPixels(byte[] pixels, byte[] transparencyMask, int offset, int scanlength,
                           int x, int y, int width, int height, int manipulation, int format) {
        if (pixels == null || width <= 0 || height <= 0) {
            return;
        }
        // Best-effort: TYPE_BYTE_332_RGB and TYPE_BYTE_8_GRAY are one byte per pixel;
        // other gray depths are approximated as 8-bit gray. A 1-bit-per-pixel MSB-first
        // transparency mask (if present) sets alpha.
        int[] argb = new int[width * height];
        boolean hasAlpha = false;
        for (int row = 0; row < height; row++) {
            int src = offset + row * scanlength;
            int dst = row * width;
            for (int col = 0; col < width; col++) {
                int v = pixels[src + col] & 0xff;
                int r, gg, b;
                if (format == TYPE_BYTE_332_RGB) {
                    r = ((v >>> 5) & 0x7) * 255 / 7;
                    gg = ((v >>> 2) & 0x7) * 255 / 7;
                    b = (v & 0x3) * 255 / 3;
                } else {
                    r = gg = b = v;   // 8-bit gray (and fallback for other gray depths)
                }
                int a = 0xff;
                if (transparencyMask != null) {
                    int bitIndex = row * (((width + 7) >>> 3) << 3) + col;
                    int mi = offset + (bitIndex >>> 3);
                    if (mi < transparencyMask.length) {
                        int bit = (transparencyMask[mi] >>> (7 - (bitIndex & 7))) & 1;
                        if (bit == 0) {
                            a = 0;
                            hasAlpha = true;
                        }
                    }
                }
                argb[dst + col] = (a << 24) | (r << 16) | (gg << 8) | b;
            }
        }
        blit(argb, hasAlpha, x, y, width, height, manipulation);
    }

    // Blit a tightly-packed (scanlength == width) ARGB buffer, applying any rotation.
    private void blit(int[] argb, boolean processAlpha,
                      int x, int y, int width, int height, int manipulation) {
        if (manipulation == 0) {
            g.drawRGB(argb, 0, width, x, y, width, height, processAlpha);
        } else {
            Image img = Image.createRGBImage(argb, width, height, processAlpha);
            g.drawRegion(img, 0, 0, width, height,
                         toTransform(manipulation), x, y, Graphics.TOP | Graphics.LEFT);
        }
    }

    // --- getPixels: cannot read back from a MIDP Graphics (documented no-op) ----

    public void getPixels(byte[] pixels, byte[] transparencyMask, int offset, int scanlength,
                          int x, int y, int width, int height, int format) {
    }

    public void getPixels(int[] pixels, int offset, int scanlength,
                          int x, int y, int width, int height, int format) {
    }

    public void getPixels(short[] pixels, int offset, int scanlength,
                          int x, int y, int width, int height, int format) {
    }

    // --- polygons / triangles --------------------------------------------------

    public void drawTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor) {
        g.setColor(argbColor & 0x00ffffff);
        g.drawLine(x1, y1, x2, y2);
        g.drawLine(x2, y2, x3, y3);
        g.drawLine(x3, y3, x1, y1);
    }

    public void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor) {
        g.setColor(argbColor & 0x00ffffff);
        g.fillTriangle(x1, y1, x2, y2, x3, y3);
    }

    public void drawPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset,
                            int nPoints, int argbColor) {
        if (nPoints < 2) {
            return;
        }
        g.setColor(argbColor & 0x00ffffff);
        for (int i = 0; i < nPoints - 1; i++) {
            g.drawLine(xPoints[xOffset + i], yPoints[yOffset + i],
                       xPoints[xOffset + i + 1], yPoints[yOffset + i + 1]);
        }
        g.drawLine(xPoints[xOffset + nPoints - 1], yPoints[yOffset + nPoints - 1],
                   xPoints[xOffset], yPoints[yOffset]);   // close the loop
    }

    public void fillPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset,
                            int nPoints, int argbColor) {
        if (nPoints < 3) {
            return;
        }
        g.setColor(argbColor & 0x00ffffff);
        // Triangle fan from vertex 0 -- exact for convex polygons, approximate otherwise.
        int x0 = xPoints[xOffset], y0 = yPoints[yOffset];
        for (int i = 1; i < nPoints - 1; i++) {
            g.fillTriangle(x0, y0,
                           xPoints[xOffset + i], yPoints[yOffset + i],
                           xPoints[xOffset + i + 1], yPoints[yOffset + i + 1]);
        }
    }
}
