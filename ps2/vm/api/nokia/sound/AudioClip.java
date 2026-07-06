/*
 * Nokia UI API -- internal audio adapter (PS2 phoneME).
 *
 * A format-specific sound source. Sound picks the right implementation once, at
 * construction time (tone / OTA melody / WAV), so play()/stop() never branch on format.
 * Adding a new format = one new AudioClip, no change to Sound or the mixer.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.sound;

interface AudioClip {
    /** Submit this sound to the SPU2 mixer; returns a voice id (>0) or -1. */
    int play(int gain, int loop);

    /** Stop the given voice id (no-op if it already ended). */
    void stop(int voiceId);
}
