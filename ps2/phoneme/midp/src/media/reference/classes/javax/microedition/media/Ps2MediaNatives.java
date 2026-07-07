/*
 * PS2 phoneME -- native bridge for the MMAPI (javax.microedition.media) audio path.
 *
 * These map the media Player implementation (Ps2Player) onto the PS2 SPU2 mixer, exactly
 * like com.nokia.mid.sound.Sound's natives do -- both funnel into the same Ps2Audio voice
 * table. Implemented in ps2/vm/MediaKni.cpp; the MIDP NativesTableGen scans the romized
 * classes.zip, finds these `native` declarations and emits nativeFunctionTable.cpp entries
 * referencing the C symbols (Java_javax_microedition_media_Ps2MediaNatives_<method>).
 *
 * Package-private: only this package's Player uses them. Lives in the `media` subsystem
 * (compiled by `make midp`) rather than APP_JAVA, because Manager references Ps2Player at
 * compile time and Manager is compiled before the APP_JAVA romize pass.
 */
package javax.microedition.media;

final class Ps2MediaNatives {

    private Ps2MediaNatives() { }

    /** Play a decoded PCM WAV (any rate/bits/channels, resampled natively). id (>0) or -1. */
    static native int nativePlayWav(byte[] data, int off, int len, int vol, int loop);

    /** Play an MMAPI tone sequence (audio/x-tone-seq, ToneControl format). id (>0) or -1. */
    static native int nativePlayToneSeq(byte[] data, int off, int len, int vol, int loop);

    /** Play a Standard MIDI File (audio/midi) on the polyphonic synth. id (>0) or -1. */
    static native int nativePlayMidi(byte[] data, int off, int len, int vol, int loop);

    /** Play a single square-wave tone (freq Hz, duration ms). id (>0) or -1. */
    static native int nativePlayTone(int freq, int durMs, int vol, int loop);

    /** Stop the voice with this id (no-op if it already ended). */
    static native void nativeStop(int id);

    /** Set the volume (0..255) of a playing voice (VolumeControl). No-op if it ended. */
    static native void nativeSetVolume(int id, int vol);

    /** Number of concurrent voices the mixer supports. */
    static native int nativeVoiceCount();
}
