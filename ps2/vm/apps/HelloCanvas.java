/*
 * HelloCanvas - Milestone B3 bring-up MIDlet for the PlayStation 2 port.
 *
 * A minimal Canvas MIDlet that proves the whole runtime loop end to end:
 *   - it is romized into the MIDP ROM as an internal suite and launched by
 *     class name (runMidlet "internal" com.j2meps2.demo.HelloCanvas), so it
 *     needs no JAR/JAD install and no rmfs contents;
 *   - paint() renders static shapes + text through phoneME's gxj software
 *     rasterizer into our framebuffer (platform::Ps2Framebuffer / GsDisplay);
 *   - keyPressed() consumes the DualShock D-pad via javanotify_key_event
 *     (platform::Ps2Pad / hal::Keypad) to move a cursor;
 *   - a background thread drives a simple animation (a frame counter) to
 *     exercise timing and repaint under load.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4 (no generics/enhanced-for).
 */
package com.j2meps2.demo;

import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Graphics;

public class HelloCanvas extends MIDlet {

    private final DemoCanvas canvas = new DemoCanvas();

    protected void startApp() {
        Display.getDisplay(this).setCurrent(canvas);
        canvas.start();
    }

    protected void pauseApp() {
        canvas.stop();
    }

    protected void destroyApp(boolean unconditional) {
        canvas.stop();
    }

    /**
     * The drawing surface: static shapes + text, a key-driven cursor, and a
     * ticking animation frame counter.
     */
    private static final class DemoCanvas extends Canvas implements Runnable {

        private int cursorX = 0;
        private int cursorY = 0;
        private int frame = 0;
        private volatile boolean running = false;
        private Thread animator;

        void start() {
            if (animator == null) {
                running = true;
                animator = new Thread(this);
                animator.start();
            }
        }

        void stop() {
            running = false;
            animator = null;
        }

        /** Animation loop: bump the frame counter ~10x/second and repaint. */
        public void run() {
            while (running) {
                frame++;
                repaint();
                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    // ignore; the running flag handles shutdown
                }
            }
        }

        protected void paint(Graphics g) {
            int w = getWidth();
            int h = getHeight();

            // Background
            g.setColor(0x000030);
            g.fillRect(0, 0, w, h);

            // A row of colored bars (static shapes)
            int[] colors = { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff };
            int barW = w / colors.length;
            for (int i = 0; i < colors.length; i++) {
                g.setColor(colors[i]);
                g.fillRect(i * barW, 10, barW - 2, 24);
            }

            // Text
            g.setColor(0xffffff);
            g.drawString("J2ME on PS2 - B3", 4, 42, Graphics.TOP | Graphics.LEFT);
            g.drawString("Frame: " + frame, 4, 62, Graphics.TOP | Graphics.LEFT);
            g.drawString("Move with the D-pad", 4, 82, Graphics.TOP | Graphics.LEFT);

            // An outlined box the cursor lives in
            int boxX = 10, boxY = 110, boxW = w - 20, boxH = h - 130;
            g.setColor(0x808080);
            g.drawRect(boxX, boxY, boxW, boxH);

            // The key-driven cursor (a small filled square), clamped to the box
            int cs = 12;
            g.setColor(0x00ffcc);
            g.fillRect(boxX + 4 + cursorX, boxY + 4 + cursorY, cs, cs);
        }

        protected void keyPressed(int keyCode) {
            int action = getGameAction(keyCode);
            int step = 8;
            if (action == LEFT) {
                cursorX -= step;
            } else if (action == RIGHT) {
                cursorX += step;
            } else if (action == UP) {
                cursorY -= step;
            } else if (action == DOWN) {
                cursorY += step;
            }

            // Clamp to the box interior (box is w-20 by h-130, cursor is 12 px,
            // inset 4 px on each side).
            int maxX = (getWidth() - 20) - 8 - 12;
            int maxY = (getHeight() - 130) - 8 - 12;
            if (cursorX < 0) { cursorX = 0; }
            if (cursorY < 0) { cursorY = 0; }
            if (cursorX > maxX) { cursorX = maxX; }
            if (cursorY > maxY) { cursorY = maxY; }

            repaint();
        }
    }
}
