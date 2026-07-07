# Phase-4 pitch diagnostic: measure per-note tuning of MY synth vs FluidSynth
# on the SAME soundfont, isolating whether a detuning is my renderer or the bank.
#
# For each tested GM program, emit a MIDI holding a few sustained notes, render it
# through (a) my wavetable synth (render_wav --force-prog) and (b) FluidSynth, then
# measure each note's fundamental via FFT + parabolic peak interpolation and report
# the error in cents against equal-temperament. A big MINE error with a small FLUID
# error on the same note => my renderer/bank build is off for that instrument.
#
#   python pitch_diag.py BANK.bin SOUNDFONT.sf2
import struct, sys, os, subprocess, wave
import numpy as np

RATE = 22050
FS = os.path.abspath("tools/bin/fluidsynth/bin/fluidsynth.exe")
TMP = "build/hl4mgm_test/_pitch"
NOTES = [48, 55, 60, 64, 67, 72, 79, 84]           # C3 G3 C4 E4 G4 C5 G5 C6
PROGS = [0, 24, 40, 48, 56, 73, 80, 81]            # piano gtr violin strings tpt flute squareLd sawLd

def midi_note_hz(n): return 440.0 * (2.0 ** ((n - 69) / 12.0))

def write_midi(path, prog, notes, dur_ticks=480, div=480):
    """One track: program change then each note held dur_ticks, sequential."""
    trk = bytearray()
    def vlq(v):
        b = [v & 0x7f]; v >>= 7
        while v: b.append((v & 0x7f) | 0x80); v >>= 7
        return bytes(reversed(b))
    trk += vlq(0) + bytes([0xC0, prog & 0x7f])
    for nt in notes:
        trk += vlq(0) + bytes([0x90, nt, 100])
        trk += vlq(dur_ticks) + bytes([0x80, nt, 0])
    trk += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, div)
    mtrk = b"MTrk" + struct.pack(">I", len(trk)) + bytes(trk)
    open(path, "wb").write(hdr + mtrk)

def read_wav_mono(path):
    w = wave.open(path, "rb")
    ch, sw, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    d = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64)
    w.close()
    if ch == 2: d = d.reshape(-1, 2).mean(axis=1)
    return d, fr

def measure_hz(sig, fr, f_ref):
    """Fundamental via autocorrelation (robust to which partial is loudest).
    Search the lag around the expected period f_ref, so we lock the fundamental
    even for bright/hollow timbres where FFT+HPS grabs an overtone."""
    if len(sig) < 2048 or np.max(np.abs(sig)) < 200.0:
        return 0.0
    x = sig - np.mean(sig)
    n = len(x)
    nfft = 1 << (int(np.log2(2 * n)) + 1)
    X = np.fft.rfft(x, nfft)
    ac = np.fft.irfft(X * np.conj(X), nfft)[:n]
    if ac[0] <= 0: return 0.0
    ac /= ac[0]
    # search lag within +-6 semitones of the expected period
    p0 = fr / f_ref
    lo = max(2, int(p0 / (2.0 ** (6 / 12.0))))
    hi = min(n - 2, int(p0 * (2.0 ** (6 / 12.0))))
    if hi <= lo + 1: return 0.0
    k = lo + int(np.argmax(ac[lo:hi]))
    a0, b0, c0 = ac[k-1], ac[k], ac[k+1]
    denom = (a0 - 2*b0 + c0)
    delta = 0.5 * (a0 - c0) / denom if denom != 0 else 0.0
    lag = k + delta
    return fr / lag if lag > 0 else 0.0

def cents(f_meas, f_ref):
    if f_meas <= 0 or f_ref <= 0: return None
    return 1200.0 * np.log2(f_meas / f_ref)

def note_slices(sig, fr, n_notes, dur_s):
    """Split concatenated per-note render into n_notes windows; return the stable
    portion of each (skip the 0.12 s attack, take the next ~0.5 s)."""
    per = int(dur_s * fr)
    a = int(0.12 * fr); b = a + int(0.55 * fr)
    out = []
    for i in range(n_notes):
        s = i * per
        out.append(sig[s + a: s + b])
    return out

def main(argv):
    bank, sf2 = argv[1], argv[2]
    os.makedirs(TMP, exist_ok=True)
    dur_ticks, div = 960, 480          # 2 quarters = 1.0 s/note at 120bpm default
    dur_s = dur_ticks / div * 0.5
    print(f"{'prog':>4} {'note':>4} {'refHz':>8} {'MINE c':>8} {'FLUID c':>8} {'delta':>7}")
    worst = []
    for prog in PROGS:
        mid = f"{TMP}/p{prog}.mid"
        write_midi(mid, prog, NOTES, dur_ticks=dur_ticks, div=div)
        mine = f"{TMP}/p{prog}_mine.wav"
        flud = f"{TMP}/p{prog}_fluid.wav"
        subprocess.run([sys.executable, "tools/sf2bank/render_wav.py", bank, mid, mine,
                        "--force-prog", str(prog), "--mono"],
                       capture_output=True)
        subprocess.run([FS, "-ni", "-R", "0", "-C", "0", "-g", "1.0", "-r", str(RATE),
                        "-F", flud, sf2, mid], capture_output=True)
        try:
            ms, mfr = read_wav_mono(mine); fs, ffr = read_wav_mono(flud)
        except Exception as e:
            print(f"{prog:>4}  render/read failed: {e}"); continue
        m_sl = note_slices(ms, mfr, len(NOTES), dur_s)
        f_sl = note_slices(fs, ffr, len(NOTES), dur_s)
        for i, nt in enumerate(NOTES):
            ref = midi_note_hz(nt)
            mc = cents(measure_hz(m_sl[i], mfr, ref), ref) if i < len(m_sl) else None
            fc = cents(measure_hz(f_sl[i], ffr, ref), ref) if i < len(f_sl) else None
            ds = (mc - fc) if (mc is not None and fc is not None) else None
            sd = f"{ds:+7.1f}" if ds is not None else "   n/a "
            sm = f"{mc:+8.1f}" if mc is not None else "   n/a  "
            sf = f"{fc:+8.1f}" if fc is not None else "   n/a  "
            print(f"{prog:>4} {nt:>4} {ref:>8.1f} {sm} {sf} {sd}")
            if ds is not None: worst.append((abs(ds), prog, nt, mc, fc, ds))
    worst.sort(reverse=True)
    print("\n== worst MINE-vs-FLUID divergences (my renderer's fault) ==")
    for a, prog, nt, mc, fc, ds in worst[:12]:
        print(f"  prog {prog:3d} note {nt:3d}: MINE {mc:+7.1f}c  FLUID {fc:+7.1f}c  delta {ds:+7.1f}c")

if __name__ == "__main__":
    sys.exit(main(sys.argv))
