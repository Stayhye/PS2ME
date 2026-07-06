/*
 * Nokia UI API -- com.nokia.mid.ui.DirectUtils (PS2 phoneME compatibility shim).
 *
 * Factory helpers: wrap a MIDP Graphics as a DirectGraphics, and create images. On a
 * real Nokia handset the Graphics object itself implements DirectGraphics and this is
 * just a cast; here we return a wrapper (DirectGraphicsImp) that holds the Graphics,
 * so games MUST obtain DirectGraphics through DirectUtils.getDirectGraphics(g) rather
 * than casting (DirectGraphics)g -- the standard, documented usage.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.ui;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

public class DirectUtils {

    private DirectUtils() {}

    /** Wrap the given Graphics for pixel-level (DirectGraphics) drawing. */
    public static DirectGraphics getDirectGraphics(Graphics g) {
        return new DirectGraphicsImp(g);
    }

    /** Decode an image from a sub-range of a byte[] (delegates to MIDP). */
    public static Image createImage(byte[] imageData, int imageOffset, int imageLength) {
        return Image.createImage(imageData, imageOffset, imageLength);
    }

    /**
     * Create a mutable image pre-filled with an ARGB colour. MIDP mutable images are
     * opaque, so the alpha channel cannot be preserved; we fill with the RGB part.
     * (Fully transparent fills are therefore approximated as opaque -- documented
     * limitation of the port.)
     */
    public static Image createImage(int width, int height, int argb) {
        Image img = Image.createImage(width, height);
        Graphics g = img.getGraphics();
        g.setColor(argb & 0x00ffffff);
        g.fillRect(0, 0, width, height);
        return img;
    }
}
