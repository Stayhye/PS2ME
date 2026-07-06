/*
 * Nokia UI API -- OTA melody source (PS2 phoneME). A Nokia OTA ringing-tone (Smart
 * Messaging) byte array, decoded into a note sequence and played by the SPU2 mixer.
 * This is what Sound(byte[], FORMAT_TONE) uses. See AudioClip / Sound.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.sound;

final class OtaClip implements AudioClip {
    private final byte[] data;

    OtaClip(byte[] data) {
        this.data = data;
    }

    public int play(int gain, int loop) {
        if (data == null) {
            return -1;
        }
        return Sound.nativePlayOta(data, 0, data.length, gain, loop);
    }

    public void stop(int voiceId) {
        Sound.nativeStop(voiceId);
    }
}
