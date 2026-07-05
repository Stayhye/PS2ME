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

extern "C" {
// MIDP / javacall entry points, all defined in libjvm.a (the merged MIDP archive).
int  javacall_events_init(void);
int  javacall_create_event_queue_lock(void);
void javanotify_start_java_with_arbitrary_args(int argc, char* argv[]);
void JavaTask(void);

// The game the native front-end picked, handed to GameLoader.chosenGame() (KNI in
// GameLoaderKni.cpp). Defined there so the KNI TU owns the storage.
extern int ps2_chosen_game;
}

int main(int argc, char** argv) {
    // Straight through our javacall layer, before anything else: proves libjavacall
    // and the print path linked and run on real hardware.
    javacall_print("j2me-ps2: booting phoneME Feature MIDP\n");

    // Bring up USB mass storage (ps2_drivers) so the program can run from a USB stick,
    // and anchor games + the icon cache to the ELF's own launch directory (argv[0]).
    // Safe with no USB / unknown path (cache off, games fall back to host:). Done once,
    // before the menu lists games.
    const char* bootPath = (argc > 0 && argv != 0) ? argv[0] : 0;
    ps2::platform::Ps2Storage::instance().mount(bootPath);

    for (;;) {
        // 1) Standalone native front-end. No VM is running here: it owns the screen,
        //    the pad and the games list on its own. Returns the chosen game or -1.
        const int idx = ps2::platform::Ps2Frontend::instance().pick();
        if (idx < 0) {
            break;   // user quit the launcher
        }
        ps2_chosen_game = idx;

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
