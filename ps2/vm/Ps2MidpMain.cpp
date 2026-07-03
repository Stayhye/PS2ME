/*
 * Ps2MidpMain.cpp - PlayStation 2 entrypoint for the phoneME Feature MIDP stack.
 *
 * Milestone B1 boot glue. The desktop javacall ports provide main() in
 * javacall/implementation/<port>/midp/main.c; the PS2 has no command line and no
 * windowing, so this file supplies a minimal main() that brings up the platform
 * services and hands control to the MIDP AMS event loop.
 *
 * Boot sequence (single OS thread, EVENTS=master_mode):
 *   1. javacall_events_init            - ready the platform event pipe.
 *   2. javacall_create_event_queue_lock- create the MIDP event-queue lock.
 *   3. javacall_initialize_configurations - load the static property table.
 *   4. javanotify_start_java_with_arbitrary_args - queue the AMS start event.
 *   5. JavaTask                        - drain the queue; the start event drives
 *                                        runMidlet -> midpInitialize, which brings
 *                                        up LCDUI (jcapp_init calls javacall_lcd_init
 *                                        for us) and the rest of MIDP.
 *
 * Unlike Milestone A's Ps2Main.cpp, the JVMSPI_* callbacks are provided by the
 * MIDP library, so this file defines only main(). Kept in ps2/vm/ as VM-integration
 * glue, separate from the javacall HAL contract.
 */

extern "C" {
#include <javacall_logging.h>    // javacall_print (boot sign-of-life)
#include <javacall_properties.h> // javacall_initialize_configurations
#include <kernel.h>              // ps2sdk EE: SleepThread
}

extern "C" {
// MIDP / javacall entry points, all defined in libjvm.a (the merged MIDP archive).
int  javacall_events_init(void);
int  javacall_create_event_queue_lock(void);
void javanotify_start_java_with_arbitrary_args(int argc, char* argv[]);
void JavaTask(void);
}

int main(int /*argc*/, char** /*argv*/) {
    // Straight through our javacall layer, before MIDP starts: proves libjavacall
    // and the print path linked and run on real hardware.
    javacall_print("j2me-ps2: booting phoneME Feature MIDP\n");

    javacall_events_init();
    javacall_create_event_queue_lock();
    javacall_initialize_configurations();  // load the static property table (MIDP reads it)

    // Queue the standard AMS start event. runMidlet treats argv[0] as the program
    // name; "internal" (== INTERNAL_SUITE_ID) selects the romized internal suite
    // without touching the suite storage (runMidlet.c); the class is the MIDlet to
    // run from it.
    //
    // Milestone B3.1 -- first light: launch our romized Canvas MIDlet directly,
    // bypassing the AppManager. This is the smallest surface that exercises the
    // full loop (paint() -> gxj -> framebuffer, keyPressed() <- pad, animation
    // thread). B3.2 will switch this back to com.sun.midp.appmanager.Manager and
    // make the MIDlet appear in the AppManager list instead.
    static char a0[] = "runMidlet";
    static char a1[] = "internal";
    static char a2[] = "com.j2meps2.demo.HelloCanvas";
    char* jargv[] = { a0, a1, a2 };
    javanotify_start_java_with_arbitrary_args(3, jargv);

    // Enter the AMS event loop. Returns once the MIDlet/AMS run completes.
    JavaTask();

    // Returning from main() resets the console and scrolls the EE Console away.
    // Halt instead so the boot log stays on screen for inspection.
    javacall_print("j2me-ps2: JavaTask returned; halting (no reboot).\n");
    for (;;) {
        SleepThread();
    }
    return 0; // not reached
}
