/*
 * GameLoader - Milestone B4 bootstrap MIDlet for the PlayStation 2 port.
 *
 * Front-end foundation: enumerates the user's game archives (.jar) from a storage
 * device (host:games/ for now; USB mass:/memory card later) through native methods
 * bridged to our C game-storage HAL, then (B4.1b) seeds each into the rmfs and
 * installs it so it shows up in the AppManager list and launches.
 *
 * B4.1a (this version) proves the new machinery only: the KNI natives resolve, and
 * host: enumeration + read work under PCSX2 HostFS. It logs what it finds to the EE
 * Console and hands off to the AppManager. Installation is added next.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.j2meps2.loader;

import javax.microedition.midlet.MIDlet;
import com.sun.midp.main.MIDletSuiteUtils;
import com.sun.midp.midlet.MIDletSuite;

public class GameLoader extends MIDlet {

    // Bridged to ps2/vm/GameLoaderKni.cpp -> ps2::hal::GameStorage.
    private static native int listGames();
    private static native int gameName(int index, byte[] buf);
    private static native int openGame(int index);
    private static native int readChunk(byte[] buf, int max);
    private static native void closeGame();

    protected void startApp() {
        try {
            int n = listGames();
            System.out.println("[GameLoader] .jar files in host:games/ = " + n);
            for (int i = 0; i < n; i++) {
                byte[] nb = new byte[256];
                int nl = gameName(i, nb);
                String name = new String(nb, 0, nl);

                int size = openGame(i);
                byte[] chunk = new byte[4096];
                int first = readChunk(chunk, chunk.length);
                closeGame();

                System.out.println("[GameLoader]   [" + i + "] " + name
                    + " size=" + size + " firstRead=" + first);
            }
        } catch (Throwable t) {
            System.out.println("[GameLoader] ERROR: " + t);
        }

        // Hand off to the AppManager so the screen comes up (the SVM restart loop
        // runs it next; see midp_run_midlet_with_args_cp).
        MIDletSuiteUtils.execute(MIDletSuite.INTERNAL_SUITE_ID,
            "com.sun.midp.appmanager.Manager", "AppManager");
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
