/*
 * Nokia UI API -- com.nokia.mid.sound.Sound (PS2 phoneME).
 *
 * Plays tones, OTA ringing-tone melodies (Sound(byte[], FORMAT_TONE)) and WAV
 * (Sound(byte[], FORMAT_WAV)) through the PS2 SPU2 mixer. The format-specific work lives
 * behind an AudioClip chosen once at construction (ToneClip / OtaClip / WavClip), so
 * play()/stop() never branch on format -- adding a format is one new AudioClip. The
 * SoundListener contract is driven here on the Java side (play -> SOUND_PLAYING,
 * stop/release -> SOUND_STOPPED/UNINITIALIZED).
 *
 * Known MVP limits: natural end-of-sound is NOT reported to the listener (that would need
 * a cross-thread callback from the mixer into the VM); resume() replays from the start.
 * If no audio backend is present, the natives return -1 and the class runs silently.
 *
 * CLDC 1.1 / MIDP 2.0, compiled -source/-target 1.4.
 */
package com.nokia.mid.sound;

public class Sound {

    public static final int FORMAT_TONE = 1;
    public static final int FORMAT_WAV  = 5;

    public static final int SOUND_PLAYING       = 0;
    public static final int SOUND_STOPPED       = 1;
    public static final int SOUND_UNINITIALIZED = 3;

    private int state = SOUND_UNINITIALIZED;
    private int gain  = 255;                 // 0..255, full volume by default
    private SoundListener listener;

    private AudioClip clip;                   // format-specific source, chosen at construction
    private int       voiceId = -1;           // handle from the native mixer, or -1

    // Implemented in ps2/vm/NokiaSoundKni.cpp over the Ps2Audio SPU2 mixer. Package-private
    // so the AudioClip implementations in this package can call them.
    static native int  nativePlayTone(int freq, int durMs, int vol, int loop);
    static native int  nativePlayOta(byte[] data, int off, int len, int vol, int loop);
    static native int  nativePlayWav(byte[] data, int off, int len, int vol, int loop);
    static native void nativeStop(int id);
    static native int  nativeVoiceCount();

    public Sound(int freq, long duration) {
        init(freq, duration);
    }

    public Sound(byte[] data, int type) {
        init(data, type);
    }

    public static int getConcurrentSoundCount(int type) {
        int n = nativeVoiceCount();
        return n > 0 ? n : 1;
    }

    public static int[] getSupportedFormats() {
        return new int[] { FORMAT_TONE, FORMAT_WAV };
    }

    /** (Re)initialise as a tone. Leaves the sound ready (stopped) to play. */
    public void init(int freq, long duration) {
        stopVoice();
        clip = new ToneClip(freq, (int) duration);
        setState(SOUND_STOPPED);
    }

    /** (Re)initialise from sampled data (FORMAT_WAV) or an OTA melody (FORMAT_TONE). */
    public void init(byte[] data, int type) {
        stopVoice();
        clip = (type == FORMAT_WAV) ? (AudioClip) new WavClip(data)
                                    : (AudioClip) new OtaClip(data);
        setState(SOUND_STOPPED);
    }

    /**
     * Play the sound loop times (0 = loop forever). Submits it to the SPU2 mixer via the
     * chosen clip and moves to SOUND_PLAYING (notifying the listener). Keeps advancing
     * even if the native backend is absent or rejects the sound (voiceId stays -1).
     */
    public void play(int loop) {
        if (loop < 0) {
            loop = 0;
        }
        if (clip != null) {
            voiceId = clip.play(gain, loop);
        }
        setState(SOUND_PLAYING);
    }

    public void stop() {
        if (state == SOUND_PLAYING) {
            stopVoice();
            setState(SOUND_STOPPED);
        }
    }

    public void resume() {
        if (state == SOUND_STOPPED) {
            play(1);                 // voice is gone; replay once from the start
        }
    }

    public void release() {
        stopVoice();
        clip = null;
        setState(SOUND_UNINITIALIZED);
    }

    public int getState() {
        return state;
    }

    public void setGain(int gain) {
        this.gain = gain;
    }

    public int getGain() {
        return gain;
    }

    public void setSoundListener(SoundListener listener) {
        this.listener = listener;
    }

    private void stopVoice() {
        if (voiceId > 0 && clip != null) {
            clip.stop(voiceId);
            voiceId = -1;
        }
    }

    private void setState(int newState) {
        state = newState;
        if (listener != null) {
            listener.soundStateChanged(this, newState);
        }
    }
}
