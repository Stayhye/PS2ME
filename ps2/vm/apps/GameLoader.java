/*
 * GameLoader - Milestone B4 bootstrap MIDlet for the PlayStation 2 port.
 *
 * Front-end foundation: enumerates the user's game archives (.jar) from a storage
 * device (host:games/ for now; USB mass:/memory card later) through native methods
 * bridged to our C game-storage HAL, seeds each into the rmfs and installs it, then
 * hands off to the AppManager -- where the installed suites now appear in the list
 * and launch through the standard path (no internal-suite patching needed).
 *
 * The rmfs is volatile, so this runs every boot: read from storage -> install ->
 * list. Installation runs on a worker thread (like CommandLineInstaller) and the
 * install prompts are all answered "yes" for a silent, headless install.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.j2meps2.loader;

import javax.microedition.midlet.MIDlet;
import java.io.IOException;
import java.io.OutputStream;

import com.sun.midp.main.MIDletSuiteUtils;
import com.sun.midp.midlet.MIDletSuite;
import com.sun.midp.installer.Installer;
import com.sun.midp.installer.FileInstaller;
import com.sun.midp.installer.InstallListener;
import com.sun.midp.installer.InstallState;
import com.sun.midp.configurator.Constants;
import com.sun.midp.io.j2me.storage.RandomAccessStream;
import com.sun.midp.io.j2me.storage.File;
import com.sun.midp.midletsuite.MIDletSuiteStorage;
import com.sun.midp.midletsuite.MIDletInfo;

public class GameLoader extends MIDlet implements InstallListener, Runnable {

    // Bridged to ps2/vm/GameLoaderKni.cpp -> ps2::hal::GameStorage.
    private static native int listGames();
    private static native int gameName(int index, byte[] buf);
    private static native int openGame(int index);
    private static native int readChunk(byte[] buf, int max);
    private static native void closeGame();

    public GameLoader() {
        // Install off the event thread (mirrors CommandLineInstaller).
        new Thread(this).start();
    }

    public void run() {
        int firstId = 0;
        String firstName = null;
        try {
            int n = listGames();
            System.out.println("[GameLoader] .jar files in host:games/ = " + n);
            System.out.println("[GameLoader]   rmfs free at start = " + freeBytes());
            for (int i = 0; i < n; i++) {
                byte[] nb = new byte[256];
                int nl = gameName(i, nb);
                String name = new String(nb, 0, nl);
                try {
                    int id = installGame(i, name);
                    if (firstId == 0 && id > 0) {
                        firstId = id;
                        firstName = name;
                    }
                } catch (Throwable t) {
                    System.out.println("[GameLoader]   " + name + " FAILED: " + t);
                }
            }
        } catch (Throwable t) {
            System.out.println("[GameLoader] ERROR: " + t);
        }

        // Diagnostic B4.1b: launch the first installed (stored) suite DIRECTLY,
        // bypassing the AppManager UI. The crash in _free_r happens right after the
        // Manager's list UI comes up with a stored suite; launching straight to the
        // game tells us whether the fault is in the Manager's list rendering (this
        // path avoids it) or in reading the stored suite itself (this path still hits
        // it). If it works, it's also the seed of our own front-end launcher.
        if (firstId > 0) {
            try {
                launchInstalled(firstId, firstName);
            } catch (Throwable t) {
                System.out.println("[GameLoader] launch FAILED: " + t);
            }
        } else {
            MIDletSuiteUtils.execute(MIDletSuite.INTERNAL_SUITE_ID,
                "com.sun.midp.appmanager.Manager", "AppManager");
        }
        notifyDestroyed();
    }

    /** Launch an installed (stored) suite by id, resolving its MIDlet-1 class. */
    private void launchInstalled(int suiteId, String name) throws Exception {
        MIDletSuiteStorage storage = MIDletSuiteStorage.getMIDletSuiteStorage();
        MIDletSuite suite = storage.getMIDletSuite(suiteId, false);
        String cls = new MIDletInfo(suite.getProperty("MIDlet-1")).classname;
        suite.close();
        System.out.println("[GameLoader] launching suite " + suiteId
            + " midlet " + cls);
        MIDletSuiteUtils.execute(suiteId, cls, name);
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
        System.out.println("[GameLoader]   seeded " + name + " (" + total
            + " bytes), rmfs free = " + freeBytes());

        int id = new FileInstaller().installJar(url, null,
            Constants.INTERNAL_STORAGE_ID, false, false, this);
        System.out.println("[GameLoader]   installed " + name + " id=" + id
            + ", rmfs free = " + freeBytes());

        // The installer copied the JAR into the suite storage; the seed is now a
        // duplicate. Delete it to release its rmfs blocks before the next game.
        try {
            new File().delete(storageName);
            System.out.println("[GameLoader]   freed seed " + storageName
                + ", rmfs free = " + freeBytes());
        } catch (Throwable t) {
            System.out.println("[GameLoader]   seed delete failed: " + t);
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

    protected void startApp() {
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
