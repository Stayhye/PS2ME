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
#include "../javacall/platform/Ps2Audio.hpp"
#include "../javacall/platform/Settings.hpp"
#include "../javacall/platform/Resolutions.hpp"
#include "../javacall/platform/SifLock.hpp"
#include "../javacall/hal/LcdDevice.hpp"

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
}

int main(int argc, char** argv) {
    // Arm the shared SIF lock before anything touches SIF or spawns a thread: from here
    // on every SIF user (controller, host:/mass: I/O, EE-console prints, audio, icon
    // worker) serializes through it, so the audio mixer thread can't collide with the
    // controller reads / file I/O on the non-reentrant SIF bus.
    ps2::platform::SifLock::init();

    // Straight through our javacall layer, before anything else: proves libjavacall
    // and the print path linked and run on real hardware.
    javacall_print("j2me-ps2: booting phoneME Feature MIDP\n");

    // Bring up USB mass storage (ps2_drivers) so the program can run from a USB stick,
    // and anchor games + the icon cache to the ELF's own launch directory (argv[0]).
    // Safe with no USB / unknown path (cache off, games fall back to host:). Done once,
    // before the menu lists games.
    const char* bootPath = (argc > 0 && argv != 0) ? argv[0] : 0;
    ps2::platform::Ps2Storage::instance().mount(bootPath);

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
    }

    // Returning from main() resets the console and scrolls the EE Console away.
    // Halt instead so the log stays on screen for inspection.
    javacall_print("j2me-ps2: launcher exited; halting (no reboot).\n");
    for (;;) {
        SleepThread();
    }
    return 0; // not reached
}
