/*
 * Nokia UI API -- com.nokia.mid.ui.DeviceControl (PS2 phoneME compatibility shim).
 *
 * Backlight and vibra control. The PS2 console has neither a backlight nor a vibra
 * motor (controller rumble is out of scope), so every method is a safe no-op: games
 * that call these to keep the screen lit or buzz simply continue running. Provided so
 * the class resolves and the calls link.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.ui;

public class DeviceControl {

    private DeviceControl() {}

    /** Set light group brightness (0..100). No-op on PS2. */
    public static void setLights(int num, int level) {}

    /** Flash the lights for the given duration in ms. No-op on PS2. */
    public static void flashLights(long duration) {}

    /** Start the vibra at the given frequency for duration ms. No-op on PS2. */
    public static void startVibra(int freq, long duration) {}

    /** Stop the vibra. No-op on PS2. */
    public static void stopVibra() {}
}
