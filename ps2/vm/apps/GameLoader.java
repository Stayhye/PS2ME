/*
 * GameLoader - launcher glue for the PlayStation 2 port.
 *
 * This is the *invisible* Java shim behind our native (C) front-end. It is NOT a UI:
 * the user-facing menu is drawn entirely in C (ps2/javacall/.../Ps2Frontend), which
 * runs from main() BEFORE the VM starts and has no ties to MIDP. By the time this
 * shim runs, the game has already been chosen; the shim only does the things that
 * inherently belong to the VM -- install the chosen JAR and tell the AMS to run it.
 *
 * Per run (one VM boot per game, driven by the C main() loop):
 *   1. chosenGame() -> the index the native menu already picked (never a UI call).
 *   2. install that one game (uninstalling any leftover so the volatile ~3 MB rmfs
 *      only ever holds one suite -- an install balloons to ~1.2 MB).
 *   3. execute(game): the native SVM restart loop (midp_run_midlet_with_args_cp) runs
 *      the game; when it exits with nothing else queued, the loop ends and JavaTask()
 *      returns to C main(), which redraws the native menu.
 *
 * The class keeps its name + native methods (bridged in GameLoaderKni.cpp) so the
 * KNI table and the Ps2MidpMain entry point stay unchanged.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.j2meps2.loader;

import javax.microedition.midlet.MIDlet;
import java.io.IOException;
import java.io.OutputStream;

import com.sun.midp.main.MIDletSuiteUtils;
import com.sun.midp.midlet.MIDletSuite;
import com.sun.midp.installer.FileInstaller;
import com.sun.midp.installer.InstallListener;
import com.sun.midp.installer.InstallState;
import com.sun.midp.configurator.Constants;
import com.sun.midp.io.j2me.storage.RandomAccessStream;
import com.sun.midp.io.j2me.storage.File;
import com.sun.midp.midletsuite.MIDletSuiteStorage;
import com.sun.midp.midletsuite.MIDletInfo;

public class GameLoader extends MIDlet implements InstallListener, Runnable {

    // Bridged to ps2/vm/GameLoaderKni.cpp -> ps2::hal::GameStorage + the chosen index.
    private static native int listGames();
    private static native int gameName(int index, byte[] buf);
    private static native int openGame(int index);
    private static native int readChunk(byte[] buf, int max);
    private static native void closeGame();
    /** The game index the native C front-end already picked (or -1). No UI here. */
    private static native int chosenGame();
    /** Drop a stale native suite storage lock so remove() can free the suite. */
    private static native void clearSuiteLock(int id);
    /** Restore the chosen game's RecordStores from the memory card into the rmfs, before
     *  it runs (no-op when there is no card or no saved data). */
    private static native void restoreGameSave();

    protected void startApp() {
        // Install can be slow; run it off the event thread (mirrors
        // CommandLineInstaller). No game is running while we are the active MIDlet, so
        // blocking the single green-thread OS thread is fine.
        new Thread(this).start();
    }

    public void run() {
        // The native C menu already ran (before the VM booted) and stored the pick.
        // We do not draw or block on any UI here; we just read the chosen index.
        final int idx = chosenGame();
        if (idx < 0) {
            notifyDestroyed();         // nothing to run -> end this VM run
            return;
        }

        try {
            byte[] nb = new byte[256];
            int nl = gameName(idx, nb);
            String name = new String(nb, 0, nl);

            // Keep the rmfs to one installed game at a time.
            uninstallAll();
            int suiteId = installGame(idx, name);
            if (suiteId <= 0) {
                throw new IOException("install returned " + suiteId);
            }

            // Repopulate the game's RecordStores from the memory card before it runs, so
            // its previous progress is there when it opens them (no-op if nothing saved).
            restoreGameSave();

            MIDletSuiteStorage storage = MIDletSuiteStorage.getMIDletSuiteStorage();
            MIDletSuite suite = storage.getMIDletSuite(suiteId, false);
            String cls = new MIDletInfo(suite.getProperty("MIDlet-1")).classname;
            suite.close();

            System.out.println("[Launcher] launching " + name + " (suite " + suiteId
                + ", midlet " + cls + ")");

            MIDletSuiteUtils.execute(suiteId, cls, name);
        } catch (Throwable t) {
            System.out.println("[Launcher] launch FAILED: " + t);
            // Nothing is queued, so this VM run just ends and JavaTask() returns to C
            // main(), which redraws the native menu -- a failed launch drops the user
            // back to the menu rather than halting the launcher.
        }
        notifyDestroyed();             // end this run so the SVM loop starts the game
    }

    /** Remove every installed (stored) suite, freeing the rmfs for the next game. */
    private void uninstallAll() {
        try {
            MIDletSuiteStorage storage = MIDletSuiteStorage.getMIDletSuiteStorage();
            int[] ids = storage.getListOfSuites();
            // TEMP diagnostic: the "storage full" on the 2nd/3rd launch means the rmfs
            // is not being reclaimed. Log the suite list and the free bytes after each
            // remove so we can see whether remove() actually frees (or throws
            // MIDletSuiteLockedException for the suite that just ran). REMOVE later.
            System.out.println("[Launcher] uninstallAll: " + ids.length
                + " suite(s) installed, rmfs free = " + freeBytes());
            for (int i = 0; i < ids.length; i++) {
                // The suite's run-lock from its last launch can outlive that VM cycle
                // (finalizer-based unlock isn't guaranteed on teardown), which makes
                // remove() throw MIDletSuiteLockedException. No stored suite is running
                // at the menu, so any lock here is stale -> drop it before removing.
                clearSuiteLock(ids[i]);
                try {
                    storage.remove(ids[i]);
                    System.out.println("[Launcher]   removed suite " + ids[i]
                        + ", rmfs free = " + freeBytes());
                } catch (Throwable t) {
                    System.out.println("[Launcher]   remove " + ids[i] + " failed: " + t);
                }
            }
        } catch (Throwable t) {
            System.out.println("[Launcher] uninstallAll: " + t);
        }
    }

    /**
     * Stream game[index] from storage into the rmfs, then install it. The rmfs
     * filename is a generic "app<i>.jar" (the suite's real name comes from the JAR
     * manifest), which sidesteps spaces/encoding in the original filename.
     */
    private int installGame(int index, String name) throws Exception {
        final String storageName = "app" + index + ".jar";
        final String url = "file:///" + storageName;

        int size = openGame(index);
        if (size < 0) {
            throw new IOException("cannot open " + name);
        }

        RandomAccessStream out = new RandomAccessStream();
        out.connect(storageName, RandomAccessStream.READ_WRITE_TRUNCATE);
        OutputStream os = out.openOutputStream();
        byte[] chunk = new byte[8192];
        int total = 0, r;
        while ((r = readChunk(chunk, chunk.length)) > 0) {
            os.write(chunk, 0, r);
            total += r;
        }
        os.close();
        out.disconnect();
        closeGame();
        System.out.println("[Launcher]   seeded " + name + " (" + total
            + " bytes), rmfs free = " + freeBytes());

        int id = new FileInstaller().installJar(url, null,
            Constants.INTERNAL_STORAGE_ID, false, false, this);
        System.out.println("[Launcher]   installed " + name + " id=" + id
            + ", rmfs free = " + freeBytes());

        try {
            new File().delete(storageName);
        } catch (Throwable t) {
            System.out.println("[Launcher]   seed delete failed: " + t);
        }
        return id;
    }

    /** rmfs free space, for diagnosing "storage full". */
    private static long freeBytes() {
        try {
            return new File().getBytesAvailableForFiles(Constants.INTERNAL_STORAGE_ID);
        } catch (Throwable t) {
            return -1;
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    // InstallListener: proceed silently -- answer "yes" to every prompt.
    public boolean warnUser(InstallState state) { return true; }
    public boolean confirmJarDownload(InstallState state) { return true; }
    public void updateStatus(int status, InstallState state) { }
    public boolean keepRMS(InstallState state) { return true; }
    public boolean confirmAuthPath(InstallState state) { return true; }
    public boolean confirmRedirect(InstallState state, String newLocation) { return true; }
}
