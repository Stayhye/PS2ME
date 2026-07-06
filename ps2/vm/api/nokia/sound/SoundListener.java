/*
 * Nokia UI API -- com.nokia.mid.sound.SoundListener (PS2 phoneME compatibility shim).
 *
 * Callback interface for Sound state changes. Games register one via
 * Sound.setSoundListener() to be told when a sound starts (SOUND_PLAYING) or finishes
 * (SOUND_STOPPED), often to chain the next sound. Provided so games that reference it
 * (e.g. AgeOfEmpires2) resolve the class.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.sound;

public interface SoundListener {
    void soundStateChanged(Sound sound, int event);
}
