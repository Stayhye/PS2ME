/*
 * PS2 phoneME -- MMAPI Player implementation over the SPU2 mixer.
 *
 * A single class implements Player plus the two mandatory controls (VolumeControl and,
 * for tone sequences, ToneControl), so getControl() just returns `this` for the types it
 * supports -- the classic small-footprint MMAPI player shape. The format-specific work is
 * a native submit chosen by `kind` (WAV / tone-seq / device-tone), reusing the Ps2Audio
 * voice table that already backs the Nokia Sound API. See ps2/vm/MediaKni.cpp.
 *
 * Scope (first cut): audio/x-wav, audio/x-tone-seq and Manager.playTone. The mixer is
 * monophonic (last-wins), so a new sound replaces the previous one -- background music and
 * a simultaneous SFX do not yet mix. MIDI is not handled here (Manager rejects it).
 *
 * Known limits (as in Nokia Sound): natural end-of-media is NOT reported to listeners (that
 * would need a cross-thread callback from the mixer into the VM); setMediaTime is a no-op
 * (no seek). Self-initiated STARTED/STOPPED/CLOSED/VOLUME_CHANGED events ARE delivered.
 */
package javax.microedition.media;

import java.util.Vector;
import javax.microedition.media.control.ToneControl;
import javax.microedition.media.control.VolumeControl;

final class Ps2Player implements Player, ToneControl, VolumeControl {

    static final int KIND_WAV     = 0;   // audio/x-wav (PCM)
    static final int KIND_TONESEQ = 1;   // audio/x-tone-seq (ToneControl sequence)
    static final int KIND_MIDI    = 2;   // audio/midi (Standard MIDI File)

    private final int    kind;
    private final String contentType;
    private byte[]       data;           // WAV bytes, or the tone sequence (may be set later)

    private int state = UNREALIZED;
    private int loopCount = 1;           // MMAPI: 1 = once, -1 = indefinite
    private int voiceId = -1;            // handle from the native mixer, or -1

    private int     level = 100;         // VolumeControl level 0..100
    private boolean muted = false;

    private final Vector listeners = new Vector();

    Ps2Player(int kind, byte[] data, String contentType) {
        this.kind = kind;
        this.data = data;
        this.contentType = contentType;
    }

    // --- Player lifecycle -------------------------------------------------------------

    public synchronized void realize() throws MediaException {
        checkClosed();
        if (state < REALIZED) {
            state = REALIZED;
        }
    }

    public synchronized void prefetch() throws MediaException {
        realize();
        if (state < PREFETCHED) {
            state = PREFETCHED;
        }
    }

    public synchronized void start() throws MediaException {
        prefetch();
        if (state == STARTED) {
            return;
        }
        // 0 = loop forever in the native mixer; N = play N times.
        int loop = (loopCount < 0) ? 0 : loopCount;
        int vol = muted ? 0 : (level * 255 / 100);
        if (data != null) {
            if (kind == KIND_WAV) {
                voiceId = Ps2MediaNatives.nativePlayWav(data, 0, data.length, vol, loop);
            } else if (kind == KIND_MIDI) {
                voiceId = Ps2MediaNatives.nativePlayMidi(data, 0, data.length, vol, loop);
            } else {
                voiceId = Ps2MediaNatives.nativePlayToneSeq(data, 0, data.length, vol, loop);
            }
        }
        System.out.println("[mmapi] start kind=" + kind + " bytes="              // TEMP diag
                + (data == null ? -1 : data.length) + " loop=" + loop + " vol=" + vol
                + " -> voice=" + voiceId);
        state = STARTED;
        postEvent(PlayerListener.STARTED, new Long(0));
    }

    public synchronized void stop() throws MediaException {
        checkClosed();
        if (state == STARTED) {
            stopVoice();
            state = PREFETCHED;
            postEvent(PlayerListener.STOPPED, new Long(0));
        }
    }

    public synchronized void deallocate() {
        if (state == STARTED) {
            stopVoice();
        }
        if (state > REALIZED) {
            state = REALIZED;
        }
    }

    public synchronized void close() {
        if (state == CLOSED) {
            return;
        }
        stopVoice();
        state = CLOSED;
        data = null;
        postEvent(PlayerListener.CLOSED, null);
    }

    public synchronized long setMediaTime(long now) throws MediaException {
        checkClosed();
        return 0;                    // no seek support; media time stays at 0
    }

    public synchronized long getMediaTime() {
        return TIME_UNKNOWN;
    }

    public synchronized int getState() {
        return state;
    }

    public long getDuration() {
        return TIME_UNKNOWN;
    }

    public String getContentType() {
        if (state == UNREALIZED || state == CLOSED) {
            throw new IllegalStateException();
        }
        return contentType;
    }

    public synchronized void setLoopCount(int count) {
        if (state == STARTED) {
            throw new IllegalStateException();
        }
        if (count == 0) {
            throw new IllegalArgumentException();
        }
        loopCount = count;
    }

    public void addPlayerListener(PlayerListener playerListener) {
        checkClosed();
        if (playerListener != null && !listeners.contains(playerListener)) {
            listeners.addElement(playerListener);
        }
    }

    public void removePlayerListener(PlayerListener playerListener) {
        checkClosed();
        if (playerListener != null) {
            listeners.removeElement(playerListener);
        }
    }

    // --- Controllable -----------------------------------------------------------------

    public Control[] getControls() {
        if (state == UNREALIZED || state == CLOSED) {
            throw new IllegalStateException();
        }
        // One object implements every control it offers, so it appears once.
        return new Control[] { this };
    }

    public Control getControl(String controlType) {
        if (controlType == null) {
            throw new IllegalArgumentException();
        }
        if (state == UNREALIZED || state == CLOSED) {
            throw new IllegalStateException();
        }
        String t = controlType;
        if (t.indexOf('.') < 0) {
            t = "javax.microedition.media.control." + t;
        }
        if (t.equals("javax.microedition.media.control.VolumeControl")) {
            return this;
        }
        if (t.equals("javax.microedition.media.control.ToneControl") && kind == KIND_TONESEQ) {
            return this;
        }
        return null;
    }

    // --- ToneControl ------------------------------------------------------------------

    public synchronized void setSequence(byte[] sequence) {
        if (sequence == null) {
            throw new IllegalArgumentException();
        }
        if (state == PREFETCHED || state == STARTED) {
            throw new IllegalStateException();
        }
        data = sequence;
    }

    // --- VolumeControl ----------------------------------------------------------------

    public synchronized int setLevel(int newLevel) {
        if (newLevel < 0) {
            newLevel = 0;
        } else if (newLevel > 100) {
            newLevel = 100;
        }
        level = newLevel;
        applyVolume();
        postEvent(PlayerListener.VOLUME_CHANGED, this);
        return level;
    }

    public int getLevel() {
        return level;
    }

    public synchronized void setMute(boolean mute) {
        if (mute == muted) {
            return;
        }
        muted = mute;
        applyVolume();
        postEvent(PlayerListener.VOLUME_CHANGED, this);
    }

    public boolean isMuted() {
        return muted;
    }

    // --- internals --------------------------------------------------------------------

    private void applyVolume() {
        if (voiceId > 0) {
            Ps2MediaNatives.nativeSetVolume(voiceId, muted ? 0 : (level * 255 / 100));
        }
    }

    private void stopVoice() {
        if (voiceId > 0) {
            Ps2MediaNatives.nativeStop(voiceId);
            voiceId = -1;
        }
    }

    private void checkClosed() {
        if (state == CLOSED) {
            throw new IllegalStateException();
        }
    }

    private void postEvent(String event, Object eventData) {
        for (int i = 0; i < listeners.size(); ++i) {
            ((PlayerListener) listeners.elementAt(i)).playerUpdate(this, event, eventData);
        }
    }
}
