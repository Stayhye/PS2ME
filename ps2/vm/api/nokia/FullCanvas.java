/*
 * Nokia UI API -- com.nokia.mid.ui.FullCanvas (PS2 phoneME compatibility shim).
 *
 * Many commercial J2ME games (especially Nokia S40 titles) extend FullCanvas instead
 * of the standard javax.microedition.lcdui.Canvas. FullCanvas predates MIDP 2.0's
 * setFullScreenMode(); it is simply a Canvas that owns the whole screen (no title,
 * no soft-key labels, no command area) and forbids commands. phoneME Feature does not
 * ship the Nokia UI API, so those games fail at class-init with
 * NoClassDefFoundError: com/nokia/mid/ui/FullCanvas. This romized system class
 * provides the API on top of MIDP 2.0.
 *
 * Romized into the MIDP ROM image as a bootstrap/system class (see romize-midlet.sh
 * and APP_JAVA in build-midp-ps2.sh), so game MIDlets resolve it from the system
 * classpath exactly as they would on a real Nokia handset.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.ui;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.CommandListener;

public abstract class FullCanvas extends Canvas {

    // Nokia game-key codes. Games switch on these in keyPressed(); they match the
    // negative codes MIDP uses for the corresponding game actions / soft keys.
    public static final int KEY_UP_ARROW    = -1;
    public static final int KEY_DOWN_ARROW  = -2;
    public static final int KEY_LEFT_ARROW  = -3;
    public static final int KEY_RIGHT_ARROW = -4;
    public static final int KEY_SOFTKEY1    = -6;
    public static final int KEY_SOFTKEY2    = -7;
    public static final int KEY_SOFTKEY3    = -5;
    public static final int KEY_SEND        = -10;
    public static final int KEY_END         = -11;

    protected FullCanvas() {
        super();
        // The whole point of FullCanvas: reserve the entire display.
        setFullScreenMode(true);
    }

    // FullCanvas owns the full screen, so it exposes no command area. The Nokia
    // contract is to reject commands with IllegalStateException.
    public void addCommand(Command command) {
        throw new IllegalStateException("FullCanvas does not allow commands");
    }

    public void setCommandListener(CommandListener listener) {
        throw new IllegalStateException("FullCanvas does not allow a command listener");
    }
}
