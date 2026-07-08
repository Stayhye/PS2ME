/*
 * Ps2MidpMain.cpp - PlayStation 2 entrypoint for the phoneME Feature MIDP stack.
 *
 * Front-end-first boot. The user-facing launcher menu is a STANDALONE native (C)
 * front-end (ps2::platform::Ps2Frontend) with no ties to the Java VM: it runs here,
 * from main(), BEFORE any MIDP/javacall initialization, drawing straight to the GS
 * and reading the pad and the games directory directly. Only once the user has
 * chosen a game do we bring up the VM to run that one game.
 *
 * Flow:
 *   loop:
 *     1. Ps2Frontend::pick()  - native menu; lists host:games/, returns the chosen
 *                               game index (or -1 to quit). No VM involved.
 *     2. bring up MIDP (once) - events + event-queue lock + static properties.
 *     3. javanotify_start_java_with_arbitrary_args - queue the AMS start event for
 *                               our thin loader (com.j2meps2.loader.GameLoader),
 *                               which installs + runs the chosen game.
 *     4. JavaTask             - drain the queue; returns when the game (and the
 *                               loader) finish, dropping us back to the menu.
 *
 * The chosen index is handed to the loader through the plain C global
 * ps2_chosen_game, read back by the GameLoader.chosenGame() KNI (GameLoaderKni.cpp).
 * The VM never draws or drives the menu; the menu never calls into MIDP.
 */

extern "C" {
#include <javacall_logging.h>    // javacall_print (boot sign-of-life)
#include <javacall_properties.h> // javacall_initialize_configurations
#include <kernel.h>              // ps2sdk EE: SleepThread
}

#include "../javacall/platform/Ps2Frontend.hpp"
#include "../javacall/platform/Ps2Storage.hpp"
#include "../javacall/platform/Ps2MemCard.hpp"
#include "../javacall/platform/Ps2SaveIcon.hpp"
#include "../javacall/platform/MidletIcon.hpp"
#include "../javacall/platform/Ps2Audio.hpp"
#include "../javacall/platform/Settings.hpp"
#include "../javacall/platform/Resolutions.hpp"
#include "../javacall/platform/SifLock.hpp"
#include "../javacall/hal/LcdDevice.hpp"
#include "../javacall/hal/GameStorage.hpp"
#include "../version.h"   // PS2ME_VERSION_STRING (boot banner)

#include <stdio.h>    // snprintf (per-game save icon path building)
#include <string.h>   // strlen / memcpy (save title)

extern "C" {
// MIDP / javacall entry points, all defined in libjvm.a (the merged MIDP archive).
int  javacall_events_init(void);
int  javacall_create_event_queue_lock(void);
void javanotify_start_java_with_arbitrary_args(int argc, char* argv[]);
void JavaTask(void);

// MIDP screen-buffer bridge (jcapp_export.c): re-reads javacall_lcd_get_screen into
// gxj_system_screen_buffer. We call it to re-sync the MIDP rasterizer to a new canvas
// size when a later game changes resolution (the VM's LCD is init'd only once).
int  jcapp_get_screen_buffer(int hardwareId);

// The game the native front-end picked, handed to GameLoader.chosenGame() (KNI in
// GameLoaderKni.cpp). Defined there so the KNI TU owns the storage.
extern int ps2_chosen_game;

// Per-game save directory "/PS2ME_<game>" on the card (Ps2GameSaveBridge.cpp); the game's
// rms.dat lives inside it, and we install its browser icon there (installGameSaveIcon).
int ps2gs_save_dir(int index, char* out, int cap);
}

namespace {

// SIF-locked random-access reader over the currently open JAR (mirrors IconCache's), so the
// save-icon decode's host: reads serialize against the audio mixer's SIF traffic.
struct SaveIconJarReader : ps2::platform::MidletIcon::IJarReader {
    virtual int readAt(unsigned int off, void* buf, int len) {
        ps2::platform::SifLock::acquire();
        const int r = ps2::hal::GameStorage::instance().readAt(off, buf, len);
        ps2::platform::SifLock::release();
        return r;
    }
};

// Icon-format version for per-game saves; bump to force a one-time rewrite on next exit.
const int kGameIconVersion = 3;   // bumped with Ps2SaveIcon's 1-bit alpha (transparent icons)

// After a game exits, if it saved progress this run, give its memory-card directory a real
// "save" appearance in the console's OSD browser: an icon.sys + 3D icon textured with the
// GAME's own icon and titled with its name. The rms.dat itself is written VM-side during the
// snapshot (javaTask.c); only the icon is installed here, on the native side, where decoding
// the JAR (host: I/O + the native stack) is safe -- doing it mid-VM-cycle is not. The
// presence of "/PS2ME_<game>/rms.dat" is the "has save data" signal, so a game that never
// saved gets no folder. Written once per game (version-gated); later exits just refresh
// rms.dat. No-op without a card.
void installGameSaveIcon(int idx) {
    using namespace ps2::platform;
    ps2::hal::GameStorage& st = ps2::hal::GameStorage::instance();

    if (!Ps2MemCard::instance().available()) {
        return;
    }
    char dir[64], path[80];
    if (ps2gs_save_dir(idx, dir, (int)sizeof(dir)) <= 0) {
        return;
    }

    // Only games that actually saved this run get a browser save (the snapshot writes
    // rms.dat lazily -- only with data -- so its presence is the "has data" signal).
    unsigned char probe[8];
    snprintf(path, sizeof(path), "%s/rms.dat", dir);
    if (Ps2MemCard::instance().readFile(path, probe, (int)sizeof(probe)) <= 0) {
        return;
    }

    // Write the icon once per version: skip the ~33 KB rewrite once installed.
    int ver = 0;
    snprintf(path, sizeof(path), "%s/iconver", dir);
    if (Ps2MemCard::instance().readFile(path, &ver, (int)sizeof(ver)) == (int)sizeof(ver) &&
        ver == kGameIconVersion) {
        return;
    }

    // Decode the game's own icon straight from its JAR (streaming: only the ZIP tail + the
    // icon entry); Ps2SaveIcon resamples it onto the 128x128 card-icon texture.
    SifLock::acquire();
    const int size = st.openAt(idx);
    SifLock::release();
    unsigned char* rgba = 0;
    int w = 0, h = 0;
    if (size > 0) {
        SaveIconJarReader reader;
        rgba = MidletIcon::loadStreaming(reader, size, &w, &h);
    }
    SifLock::acquire();
    st.close();
    SifLock::release();

    // Title the save with the game's name (drop a trailing ".jar" for a tidy label).
    const char* nm = st.nameAt(idx);
    char title[40];
    title[0] = '\0';
    if (nm != 0) {
        int n = (int)strlen(nm);
        if (n >= 4 && nm[n - 4] == '.' &&
            (nm[n - 3] | 0x20) == 'j' && (nm[n - 2] | 0x20) == 'a' &&
            (nm[n - 1] | 0x20) == 'r') {
            n -= 4;
        }
        if (n > (int)sizeof(title) - 1) {
            n = (int)sizeof(title) - 1;
        }
        memcpy(title, nm, (size_t)n);
        title[n] = '\0';
    }

    if (Ps2SaveIcon::install(dir, (title[0] != '\0') ? title : "Game", rgba, w, h)) {
        int v = kGameIconVersion;
        snprintf(path, sizeof(path), "%s/iconver", dir);
        Ps2MemCard::instance().writeFile(path, &v, (int)sizeof(v));
    }
    if (rgba != 0) {
        MidletIcon::release(rgba);
    }
}

} // namespace

int main(int argc, char** argv) {
    // Arm the shared SIF lock before anything touches SIF or spawns a thread: from here
    // on every SIF user (controller, host:/mass: I/O, EE-console prints, audio, icon
    // worker) serializes through it, so the audio mixer thread can't collide with the
    // controller reads / file I/O on the non-reentrant SIF bus.
    ps2::platform::SifLock::init();

    // Straight through our javacall layer, before anything else: proves libjavacall
    // and the print path linked and run on real hardware.
    javacall_print("PS2ME " PS2ME_VERSION_STRING ": booting phoneME Feature MIDP\n");

    // Bring up USB mass storage (ps2_drivers) so the program can run from a USB stick,
    // and anchor games + the icon cache to the ELF's own launch directory (argv[0]).
    // Safe with no USB / unknown path (cache off, games fall back to host:). Done once,
    // before the menu lists games.
    const char* bootPath = (argc > 0 && argv != 0) ? argv[0] : 0;
    ps2::platform::Ps2Storage::instance().mount(bootPath);

    // Bring up the memory card (ps2_drivers mcman/mcserv + libmc) right after storage, so
    // the launcher's own state -- favourites, recents, settings, per-game resolutions --
    // persists on the card rather than the USB stick. This is what lets the program boot
    // from a read-only CDVD disc and still remember the user's choices. Must follow mount()
    // (SIF up, IOP reset done on USB boot) and stay single-threaded (before the icon
    // worker). With no card present the config classes fall back to the boot directory.
    ps2::platform::Ps2MemCard::instance().init();

    // Give the launcher's /PS2ME directory a proper PS2 "save" appearance -- an icon.sys
    // plus a 3D icon textured with the PS2ME logo -- so the console's OSD browser lists it
    // with a real icon and title instead of flagging it as "Corrupted Data". No-op when no
    // card is present; writes once per icon version (skips the rewrite once installed).
    ps2::platform::Ps2SaveIcon::installAppIcon("PS2ME");

    // Bring up audio (ps2_drivers libsd + audsrv) AFTER storage: the audio IOP modules
    // must load after Ps2Storage's USB-boot IOP reset, and this is still single-threaded
    // (before the menu / icon worker), so no SIF lock is needed. A short startup chime
    // both confirms the SPU2/audsrv chain is live and gives the launcher a bit of life.
    if (ps2::platform::Ps2Audio::instance().init()) {
        // Load the wavetable bank (beside games/) into RAM once, while still single-
        // threaded and before the Java pool exists -- plenty of free RAM here. Optional:
        // if it's missing the synth just stays on the square-wave path.
        char bankPath[224];
        if (ps2::platform::Ps2Storage::instance().bankPath(bankPath, sizeof(bankPath))) {
            ps2::platform::Ps2Audio::instance().loadBank(bankPath);
        }
        ps2::platform::Ps2Audio::instance().startMixer();     // SIF-locked ring feeder
    }

    for (;;) {
        // 1) Standalone native front-end. No VM is running here: it owns the screen,
        //    the pad and the games list on its own. Returns the chosen game or -1.
        const int idx = ps2::platform::Ps2Frontend::instance().pick();
        if (idx < 0) {
            break;   // user quit the launcher
        }
        ps2_chosen_game = idx;

        // 1b) Apply this game's canvas resolution/orientation BEFORE the VM sizes it, so
        //     getWidth/getHeight and the RGB565 raster match what the game expects (many
        //     landscape titles are clipped by the default portrait canvas otherwise).
        {
            int rw = 0, rh = 0;
            ps2::platform::Resolutions::instance().get(idx, &rw, &rh);
            ps2::hal::LcdDevice::instance().setResolution(rw, rh);
            // The VM's LCD is brought up only once (the VM persists across games). For the
            // first game jcapp_init reads the size itself; for later games force the MIDP
            // screen buffer to re-read our new geometry so its Canvas is sized correctly.
            static bool lcdUp = false;
            if (lcdUp) {
                jcapp_get_screen_buffer(ps2::hal::LcdDevice::PRIMARY_ID);
            }
            lcdUp = true;
        }

        // 2) Bring up MIDP once, lazily, only after a game has been chosen.
        static bool vmInitialized = false;
        if (!vmInitialized) {
            javacall_events_init();
            javacall_create_event_queue_lock();
            javacall_initialize_configurations();  // load the static property table
            vmInitialized = true;
        }

        // 3) Queue the AMS start event for our thin loader. "internal"
        //    (== INTERNAL_SUITE_ID) selects the romized internal suite without
        //    touching suite storage (runMidlet.c); GameLoader reads ps2_chosen_game,
        //    installs that one game and executes it. The SVM restart loop (native,
        //    inside midp_run_midlet_with_args_cp) runs the game and, with nothing else
        //    queued afterwards, returns -- so JavaTask() returns to us and we redraw
        //    the menu on the next iteration.
        static char a0[] = "runMidlet";
        static char a1[] = "internal";
        static char a2[] = "com.j2meps2.loader.GameLoader";
        char* jargv[] = { a0, a1, a2 };

        // Show launch progress while the VM installs and starts the game. By default
        // this is the friendly loading screen (game icon + name + staged progress bar);
        // with "Debug mode" on it is instead the raw VM stdout trace (the [Launcher]
        // milestones + any crash), which on real hardware is the only console left after
        // the USB IOP reset. Both overlays turn themselves off the moment the game draws
        // its first frame (Ps2Framebuffer::present).
        if (ps2::platform::Settings::instance().debugMode()) {
            ps2::platform::Ps2Frontend::instance().logEnable(true);
        } else {
            ps2::platform::Ps2Frontend::instance().loadingBegin(idx);
        }
        javacall_print("=== launch trace (install + start) ===\n");

        javanotify_start_java_with_arbitrary_args(3, jargv);

        // 4) Run the VM until the chosen game (and the loader) finish.
        JavaTask();

        // 4b) If the game just saved progress this run, give its memory-card save a real
        //     OSD-browser entry -- an icon.sys + a 3D icon textured with the game's own
        //     icon and titled with its name. Done here on the native side (JAR decode is
        //     safe now that the VM cycle is over), version-gated so it runs once per game.
        installGameSaveIcon(idx);
    }

    // Returning from main() resets the console and scrolls the EE Console away.
    // Halt instead so the log stays on screen for inspection.
    javacall_print("j2me-ps2: launcher exited; halting (no reboot).\n");
    for (;;) {
        SleepThread();
    }
    return 0; // not reached
}
