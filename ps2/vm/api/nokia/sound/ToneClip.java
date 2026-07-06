/*
 * Nokia UI API -- tone source (PS2 phoneME). A single square-wave tone (freq/duration),
 * synthesized by the SPU2 mixer. See AudioClip / Sound.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.sound;

final class ToneClip implements AudioClip {
    private final int freq;
    private final int durMs;

    ToneClip(int freq, int durMs) {
        this.freq  = freq;
        this.durMs = durMs;
    }

    public int play(int gain, int loop) {
        return Sound.nativePlayTone(freq, durMs, gain, loop);
    }

    public void stop(int voiceId) {
        Sound.nativeStop(voiceId);
    }
}
