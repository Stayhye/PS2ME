/*
 * Nokia UI API -- WAV source (PS2 phoneME). A PCM WAV byte array, decoded/resampled to
 * 22050/16/mono and played by the SPU2 mixer. This is what Sound(byte[], FORMAT_WAV)
 * uses. See AudioClip / Sound.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.sound;

final class WavClip implements AudioClip {
    private final byte[] data;

    WavClip(byte[] data) {
        this.data = data;
    }

    public int play(int gain, int loop) {
        if (data == null) {
            return -1;
        }
        return Sound.nativePlayWav(data, 0, data.length, gain, loop);
    }

    public void stop(int voiceId) {
        Sound.nativeStop(voiceId);
    }
}
