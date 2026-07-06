/*
 * Nokia UI API -- com.nokia.mid.sound.Sound (PS2 phoneME compatibility shim).
 *
 * The legacy Nokia sound class: plays tones and sampled data (WAV/OTA). Our phoneME
 * build has no audio backend (MMAPI is disabled), so this is a FUNCTIONAL STUB: it
 * tracks the sound state and drives the SoundListener contract faithfully (play ->
 * SOUND_PLAYING, stop/release -> SOUND_STOPPED/UNINITIALIZED) so games that chain the
 * next sound from the callback keep advancing, but no audio is produced. Provided so
 * games that use com.nokia.mid.sound.* resolve and run silently instead of crashing
 * with NoClassDefFoundError.
 *
 * If/when a PS2 audio backend is added, the play/stop bodies are the single place to
 * wire it in.
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

    public Sound(int freq, long duration) {
        init(freq, duration);
    }

    public Sound(byte[] data, int type) {
        init(data, type);
    }

    public static int getConcurrentSoundCount(int type) {
        return 1;
    }

    public static int[] getSupportedFormats() {
        return new int[] { FORMAT_TONE, FORMAT_WAV };
    }

    /** (Re)initialise as a tone. Leaves the sound ready (stopped) to play. */
    public void init(int freq, long duration) {
        setState(SOUND_STOPPED);
    }

    /** (Re)initialise from sampled data. Leaves the sound ready (stopped) to play. */
    public void init(byte[] data, int type) {
        setState(SOUND_STOPPED);
    }

    /**
     * "Play" the sound loop times (0 = loop forever). No audio backend, so this only
     * moves the state to SOUND_PLAYING and notifies the listener; the game continues.
     */
    public void play(int loop) {
        setState(SOUND_PLAYING);
    }

    public void stop() {
        if (state == SOUND_PLAYING) {
            setState(SOUND_STOPPED);
        }
    }

    public void resume() {
        if (state == SOUND_STOPPED) {
            setState(SOUND_PLAYING);
        }
    }

    public void release() {
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

    private void setState(int newState) {
        state = newState;
        if (listener != null) {
            listener.soundStateChanged(this, newState);
        }
    }
}
