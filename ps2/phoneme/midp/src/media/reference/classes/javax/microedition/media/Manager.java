/*
 * PS2 phoneME -- functional Manager for the MMAPI (javax.microedition.media) audio path.
 *
 * This replaces the reference stub (whose createPlayer/playTone are inert) so J2ME games
 * get real audio. It builds a Ps2Player over the PS2 SPU2 mixer for the formats we support:
 * audio/x-wav, audio/x-tone-seq (ToneControl) and the device://tone locator, plus the
 * simple Manager.playTone. See Ps2Player / Ps2MediaNatives / ps2/vm/MediaKni.cpp.
 *
 * First cut: no MIDI (createPlayer throws MediaException for it), no capture/streaming
 * protocols, monophonic playback (the mixer is last-wins). Everything lives in the `media`
 * subsystem so it compiles alongside this class in `make midp`.
 *
 * CLDC 1.1 / MIDP 2.0. Keeps the public API of the JSR-135 subset that MIDP mandates.
 */
package javax.microedition.media;

import java.io.InputStream;
import java.io.IOException;
import java.io.ByteArrayOutputStream;

public final class Manager {

    /** Locator for a tone Player played back via ToneControl. Value "device://tone". */
    public final static String TONE_DEVICE_LOCATOR = "device://tone";

    private final static String TONE_CT = "audio/x-tone-seq";
    private final static String WAV_CT  = "audio/x-wav";
    private final static String MIDI_CT = "audio/midi";

    /** Cap on how much of an InputStream we buffer (matches the native scratch). */
    private final static int MAX_MEDIA = 128 * 1024;

    // MIDI note (0..127) -> frequency (Hz): 440 * 2^((note-69)/12), rounded. Note 69 = A4.
    private final static int[] MIDI_HZ = {
            8,     9,     9,    10,    10,    11,    12,    12,    13,    14,    15,    15,
           16,    17,    18,    19,    21,    22,    23,    24,    26,    28,    29,    31,
           33,    35,    37,    39,    41,    44,    46,    49,    52,    55,    58,    62,
           65,    69,    73,    78,    82,    87,    92,    98,   104,   110,   117,   123,
          131,   139,   147,   156,   165,   175,   185,   196,   208,   220,   233,   247,
          262,   277,   294,   311,   330,   349,   370,   392,   415,   440,   466,   494,
          523,   554,   587,   622,   659,   698,   740,   784,   831,   880,   932,   988,
         1047,  1109,  1175,  1245,  1319,  1397,  1480,  1568,  1661,  1760,  1865,  1976,
         2093,  2217,  2349,  2489,  2637,  2794,  2960,  3136,  3322,  3520,  3729,  3951,
         4186,  4435,  4699,  4978,  5274,  5588,  5920,  6272,  6645,  7040,  7459,  7902,
         8372,  8870,  9397,  9956, 10548, 11175, 11840, 12544 };

    private Manager() { }

    public static String[] getSupportedContentTypes(String protocol) {
        return new String[] { TONE_CT, WAV_CT, MIDI_CT };
    }

    public static String[] getSupportedProtocols(String content_type) {
        return new String[] { "device" };
    }

    /**
     * Create a Player from a locator. Only the tone device locator is supported; other
     * protocols (http/file/capture/rtp) throw MediaException.
     */
    public static Player createPlayer(String locator)
            throws IOException, MediaException {
        if (locator == null) {
            throw new IllegalArgumentException();
        }
        System.out.println("[mmapi] createPlayer locator=" + locator);   // TEMP diag
        if (locator.startsWith(TONE_DEVICE_LOCATOR)) {
            // No sequence yet; set later via ToneControl.setSequence().
            return new Ps2Player(Ps2Player.KIND_TONESEQ, null, TONE_CT);
        }
        System.out.println("[mmapi] REJECT locator=" + locator);          // TEMP diag
        throw new MediaException("Cannot create a Player for: " + locator);
    }

    /**
     * Create a Player from an InputStream. The type is the content-type (may be null, in
     * which case we sniff the header). Supported: audio/x-wav and audio/x-tone-seq.
     */
    public static Player createPlayer(InputStream stream, String type)
            throws IOException, MediaException {
        if (stream == null) {
            throw new IllegalArgumentException();
        }
        byte[] data = readFully(stream);
        int kind = classify(type, data);
        int b0 = (data != null && data.length > 0) ? (data[0] & 0xFF) : -1;   // TEMP diag
        System.out.println("[mmapi] createPlayer type=" + type + " bytes="       // TEMP diag
                + (data == null ? -1 : data.length) + " b0=" + b0 + " kind=" + kind);
        if (kind == Ps2Player.KIND_WAV) {
            return new Ps2Player(Ps2Player.KIND_WAV, data, WAV_CT);
        }
        if (kind == Ps2Player.KIND_TONESEQ) {
            return new Ps2Player(Ps2Player.KIND_TONESEQ, data, TONE_CT);
        }
        if (kind == Ps2Player.KIND_MIDI) {
            return new Ps2Player(Ps2Player.KIND_MIDI, data, MIDI_CT);
        }
        System.out.println("[mmapi] REJECT type=" + type + " (unsupported)");   // TEMP diag
        throw new MediaException("Cannot create a Player for content type: " + type);
    }

    /**
     * Play back a single tone (note 0..127, duration ms, volume 0..100). Non-blocking.
     */
    public static void playTone(int note, int duration, int volume)
            throws MediaException {
        if (note < 0 || note > 127 || duration <= 0) {
            throw new IllegalArgumentException("bad param");
        }
        if (volume < 0) {
            volume = 0;
        } else if (volume > 100) {
            volume = 100;
        }
        System.out.println("[mmapi] playTone note=" + note + " dur=" + duration    // TEMP diag
                + " vol=" + volume);
        Ps2MediaNatives.nativePlayTone(MIDI_HZ[note], duration, volume * 255 / 100, 1);
    }

    // --- helpers ----------------------------------------------------------------------

    private static byte[] readFully(InputStream in) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] tmp = new byte[2048];
        int total = 0;
        int n;
        while (total < MAX_MEDIA && (n = in.read(tmp)) > 0) {
            bos.write(tmp, 0, n);
            total += n;
        }
        return bos.toByteArray();
    }

    // Decide the kind from the declared type, else sniff the header. -1 = unsupported.
    private static int classify(String type, byte[] data) {
        if (type != null) {
            String t = type.toLowerCase();
            if (t.indexOf("wav") >= 0) {
                return Ps2Player.KIND_WAV;
            }
            if (t.indexOf("tone") >= 0) {
                return Ps2Player.KIND_TONESEQ;
            }
            if (t.indexOf("midi") >= 0 || t.indexOf("mid") >= 0 || t.indexOf("spmidi") >= 0) {
                return Ps2Player.KIND_MIDI;
            }
            // fall through to sniffing for anything else
        }
        if (data != null && data.length >= 4
                && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
            return Ps2Player.KIND_WAV;
        }
        if (data != null && data.length >= 4
                && data[0] == 'M' && data[1] == 'T' && data[2] == 'h' && data[3] == 'd') {
            return Ps2Player.KIND_MIDI;
        }
        // MMAPI tone sequences begin with {VERSION (-2), 1}.
        if (data != null && data.length >= 2 && data[0] == -2) {
            return Ps2Player.KIND_TONESEQ;
        }
        return -1;
    }
}
