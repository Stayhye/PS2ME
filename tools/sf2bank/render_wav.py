#!/usr/bin/env python3
# PS2ME audio -- Phase 4, Stage 0: render a MIDI through a wavetable bank to WAV.
#
# This mirrors the future PS2 sample-synth: it reads the compact .bin bank built
# by build_bank.py, parses a Standard MIDI File (the same event model as the
# validated decodeMidi -- note on/off, tempo, plus program-change/CC7/CC10/CC11/
# pitch-bend that Stage 4 will need), and synthesizes each note by resampling its
# bank sample (Q16-style linear interpolation, with loop) under an ADSR envelope,
# pan and velocity. If the WAV sounds right vs KEmulator, the PS2 will too.
#
# Usage:
#   python render_wav.py BANK.bin SONG.mid OUT.wav [--rate 22050] [--mono]
#                        [--poly 0] [--bend-range 2] [--gain auto|1.0] [--seconds S]
import struct, sys, wave
import numpy as np

ZONE_FMT = "<IIII bB BB h H HHHH H BB"
ZONE_SIZE = struct.calcsize(ZONE_FMT)


# ----------------------------------------------------------------------------- bank
class Bank:
    def __init__(self, path):
        b = open(path, "rb").read()
        magic, ver, flags, rate, nprog, ndrum, nzones, npcm = \
            struct.unpack_from("<4sHHIHHII", b, 0)
        assert magic == b"PS2B", "not a PS2ME bank"
        self.rate = rate
        p = struct.calcsize("<4sHHIHHII")
        self.prog_dir = [struct.unpack_from("<HBB", b, p + i*4)[:2] for i in range(128)]
        p += 128 * 4
        self.drum_dir = [struct.unpack_from("<HBB", b, p + i*4)[:2] for i in range(128)]
        p += 128 * 4
        self.zones = []
        for i in range(nzones):
            z = struct.unpack_from(ZONE_FMT, b, p + i*ZONE_SIZE)
            self.zones.append(dict(off=z[0], length=z[1], ls=z[2], le=z[3],
                                   cents=z[4], root=z[5], klo=z[6], khi=z[7],
                                   pan=z[8], atten=z[9], a=z[10], h=z[11],
                                   d=z[12], r=z[13], susp=z[14], mode=z[15]))
        p += nzones * ZONE_SIZE
        self.pcm = np.frombuffer(b, dtype="<i2", count=npcm, offset=p).astype(np.float32)
        print(f"bank: rate={rate}Hz zones={nzones} pcm={npcm} "
              f"({npcm*2/1e6:.2f} MB)  programs*={sum(1 for c in self.prog_dir if c[1])} "
              f"drums={sum(1 for c in self.drum_dir if c[1])}")

    def melodic_zone(self, prog, note):
        start, cnt = self.prog_dir[prog]
        best = None
        for i in range(start, start + cnt):
            z = self.zones[i]
            if z["klo"] <= note <= z["khi"]:
                return z
            best = z
        return best   # fall back to last zone if no key match

    def drum_zone(self, note):
        start, cnt = self.drum_dir[note]
        return self.zones[start] if cnt else None


# ------------------------------------------------------------------------- MIDI parse
def _varlen(data, p):
    val = 0
    while True:
        b = data[p]; p += 1
        val = (val << 7) | (b & 0x7f)
        if not (b & 0x80):
            return val, p


def parse_midi(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MThd", "not a MIDI (MThd)"
    fmt, ntrk, div = struct.unpack_from(">HHH", d, 8)
    events = []           # (tick, seq, kind, ch, a, b)
    seq = 0
    p = 14
    for _ in range(ntrk):
        if d[p:p+4] != b"MTrk":
            break
        tlen = struct.unpack_from(">I", d, p+4)[0]
        p += 8
        end = p + tlen
        tick = 0
        status = 0
        while p < end:
            dt, p = _varlen(d, p)
            tick += dt
            b0 = d[p]
            if b0 & 0x80:
                status = b0; p += 1
            # running status: reuse status, b0 is data
            hi = status & 0xf0
            ch = status & 0x0f
            if hi in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                a = d[p]; b = d[p+1]; p += 2
                if hi == 0x90:
                    events.append((tick, seq, "off" if b == 0 else "on", ch, a, b))
                elif hi == 0x80:
                    events.append((tick, seq, "off", ch, a, b))
                elif hi == 0xB0:
                    events.append((tick, seq, "cc", ch, a, b))
                elif hi == 0xE0:
                    events.append((tick, seq, "bend", ch, a, b))
                # 0xA0 aftertouch ignored
            elif hi in (0xC0, 0xD0):
                a = d[p]; p += 1
                if hi == 0xC0:
                    events.append((tick, seq, "prog", ch, a, 0))
            elif b0 == 0xFF:
                mtype = d[p]; p += 1
                mlen, p = _varlen(d, p)
                if mtype == 0x51:
                    us = (d[p] << 16) | (d[p+1] << 8) | d[p+2]
                    events.append((tick, seq, "tempo", 0, us, 0))
                p += mlen
            elif b0 in (0xF0, 0xF7):
                mlen, p = _varlen(d, p)
                p += mlen
            else:
                p += 1
            seq += 1
    events.sort(key=lambda e: (e[0], e[1]))
    return fmt, div, events


def tempo_map_to_samples(events, div, rate):
    """Attach an absolute sample time to every event."""
    out = []
    if div & 0x8000:                       # SMPTE
        fps = -( (div >> 8) - 256 if (div >> 8) >= 128 else (div >> 8) )
        tpf = div & 0xff
        spt = rate / float(fps * tpf)
        for (tick, seq, kind, ch, a, b) in events:
            out.append((tick * spt, kind, ch, a, b))
        return out
    ppq = div if div else 480
    us_per_q = 500000.0
    anchor_tick = 0
    anchor_samp = 0.0
    for (tick, seq, kind, ch, a, b) in events:
        samp = anchor_samp + (tick - anchor_tick) * us_per_q * rate / (ppq * 1e6)
        out.append((samp, kind, ch, a, b))
        if kind == "tempo":
            anchor_samp = samp
            anchor_tick = tick
            us_per_q = float(a)
    return out


# --------------------------------------------------------------------------- envelope
def make_env(rate, held, aMs, hMs, dMs, sus, rMs, oneshot, note_len):
    a = int(aMs * rate / 1000); h = int(hMs * rate / 1000)
    dd = int(dMs * rate / 1000); r = max(1, int(rMs * rate / 1000))
    if oneshot:
        # A one-shot (drum / unlooped sample) plays out and decays IN the sample
        # itself. The SF2 release is often far longer than the sample (a 2.4 s
        # release on a 0.1 s kick) -- applying it would ramp the whole thing to
        # zero. So just declick: a ~2 ms fade-in and a short fade-out at the end.
        n = note_len
        env = np.ones(n, dtype=np.float32)
        aa = min(max(1, int(0.002 * rate)), n)
        env[:aa] = np.linspace(0, 1, aa, dtype=np.float32)
        rr = min(max(1, int(0.006 * rate)), n // 4)
        if rr > 1:
            env[n-rr:] *= np.linspace(1, 0, rr, dtype=np.float32)
        return env
    held = max(0, held)
    pre = np.empty(held, dtype=np.float32)
    if held:
        t = np.arange(held)
        pre[:] = sus
        m = t < a
        pre[m] = (t[m] / max(1, a))
        m = (t >= a) & (t < a + h)
        pre[m] = 1.0
        m = (t >= a + h) & (t < a + h + dd)
        pre[m] = 1.0 + (t[m] - (a + h)) * (sus - 1.0) / max(1, dd)
    rel_start = pre[-1] if held else 0.0
    rel = np.linspace(rel_start, 0.0, r, dtype=np.float32)
    return np.concatenate([pre, rel])


# --------------------------------------------------------------------------- synth
def render(bank, events_s, rate, mono, bend_range, poly, seconds,
           force_prog=-1, no_drums=False, debug=False, drums_only=False,
           only_chan=-1):
    # channel state
    prog = [0] * 16
    vol = [100] * 16          # CC7
    expr = [127] * 16         # CC11
    pan = [64] * 16           # CC10 (0..127, 64 center)
    bend = [0.0] * 16         # semitones
    # active melodic notes: (ch,note) -> voice dict (for note-off)
    active = {}
    voices = []               # finished/started voices to render
    dbg = dict(on=0, drum=0, no_zone=0, stuck=0, off_matched=0, off_orphan=0,
               prog_used=set())

    song_end = 0.0
    for (samp, kind, ch, a, b) in events_s:
        song_end = max(song_end, samp)
        if kind == "prog":
            prog[ch] = a
        elif kind == "cc":
            if a == 7: vol[ch] = b
            elif a == 11: expr[ch] = b
            elif a == 10: pan[ch] = b
        elif kind == "bend":
            val = (a | (b << 7)) - 8192
            bend[ch] = val / 8192.0 * bend_range
        elif kind == "on":
            note, vel = a, b
            if only_chan >= 0 and ch != only_chan:
                continue
            dbg["on"] += 1
            if ch == 9:
                if no_drums:
                    continue
                dbg["drum"] += 1
                z = bank.drum_zone(note)
                if z is None:
                    dbg["no_zone"] += 1; continue
                v = _mk_voice(bank, z, note, vel, ch, pan[ch], vol[ch], expr[ch],
                              0.0, rate, oneshot=True)
                if v: v["on"] = samp; voices.append(v)
            else:
                if drums_only:
                    continue
                p = force_prog if force_prog >= 0 else prog[ch]
                dbg["prog_used"].add(p)
                z = bank.melodic_zone(p, note)
                if z is None:
                    dbg["no_zone"] += 1; continue
                # re-trigger of a still-held note: release the old voice first,
                # else it drones until song end (mush). This matters a LOT for
                # dense monophonic lines that repeat the same pitch.
                old = active.get((ch, note))
                if old is not None and old["off"] is None:
                    old["off"] = samp; dbg["stuck"] += 1
                v = _mk_voice(bank, z, note, vel, ch, pan[ch], vol[ch], expr[ch],
                              bend[ch], rate, oneshot=(z["mode"] == 0))
                if v:
                    v["on"] = samp
                    active[(ch, note)] = v
                    voices.append(v)
        elif kind == "off":
            v = active.pop((ch, a), None)
            if v is not None and v["off"] is None:
                v["off"] = samp; dbg["off_matched"] += 1
            else:
                dbg["off_orphan"] += 1
    if debug:
        print(f"  [dbg] noteOn={dbg['on']} drums={dbg['drum']} noZone={dbg['no_zone']} "
              f"retrigStuck={dbg['stuck']} offMatched={dbg['off_matched']} "
              f"offOrphan={dbg['off_orphan']} progs={sorted(dbg['prog_used'])}")

    tail = int(1.0 * rate)
    total = int(song_end) + tail
    if seconds > 0:
        total = min(total, int(seconds * rate))
    L = np.zeros(total, dtype=np.float32)
    R = np.zeros(total, dtype=np.float32)

    # optional voice cap by note-on time (mirror PS2 stealing coarsely)
    if poly > 0:
        voices = _cap_polyphony(voices, poly)

    max_conc = _max_concurrency(voices, rate)
    for v in voices:
        _render_voice(bank, v, L, R, rate)

    if mono:
        out = (L + R) * 0.5
        return out, None, max_conc, total
    return L, R, max_conc, total


def _mk_voice(bank, z, note, vel, ch, pan127, cvol, cexpr, bendsemi, rate, oneshot):
    root = z["root"]
    step = 2.0 ** ((note - root + z["cents"] / 100.0 + bendsemi) / 12.0)
    if z["length"] < 2:
        return None
    # initialAttenuation is in centibels per spec, but SF2 banks were authored
    # for EMU hardware that scaled it by 0.4 -- FluidSynth (our reference) does the
    # same. Applying the raw cB made high-attenuation voices (e.g. the square lead
    # at 255 cB = 25.5 dB) nearly silent, dropping whole melodic tracks.
    atten_gain = 10.0 ** (-z["atten"] * 0.4 / 200.0)
    gain = (vel / 127.0) * (cvol / 127.0) * (cexpr / 127.0) * atten_gain
    # equal-power pan: combine zone pan (-500..500) and channel pan (0..127)
    pz = z["pan"] / 500.0
    pc = (pan127 - 64) / 64.0
    ppos = max(-1.0, min(1.0, pz + pc))
    theta = (ppos + 1.0) * 0.25 * np.pi
    panL, panR = np.cos(theta), np.sin(theta)
    return dict(z=z, note=note, step=step, gain=gain, panL=panL, panR=panR,
                oneshot=oneshot, on=0.0, off=None)


def _render_voice(bank, v, L, R, rate):
    z = v["z"]
    step = v["step"]
    on = int(v["on"])
    length = z["length"]
    off_abs = v["off"]
    if v["oneshot"]:
        note_len = int(length / step)
        note_len = max(2, note_len)
        env = make_env(rate, 0, z["a"], z["h"], z["d"], z["susp"]/1000.0,
                       z["r"], True, note_len)
        n = len(env)
    else:
        held = int((off_abs - v["on"]) if off_abs is not None else length / step)
        held = max(0, held)
        env = make_env(rate, held, z["a"], z["h"], z["d"], z["susp"]/1000.0,
                       z["r"], False, 0)
        n = len(env)
    if n <= 0:
        return
    pos = step * np.arange(n, dtype=np.float64)
    base = z["off"]
    ls, le = z["ls"], z["le"]
    looping = (z["mode"] in (1, 3)) and (le - ls) > 1 and not v["oneshot"]
    if looping:
        loop_len = le - ls
        m = pos >= le
        pos = pos.copy()
        pos[m] = ls + np.mod(pos[m] - le, loop_len)
    else:
        # one-shot / no loop: stop at sample end
        valid = pos < (length - 1)
        if not valid.all():
            cut = int(np.argmax(~valid)) if (~valid).any() else n
            n = cut
            pos = pos[:n]; env = env[:n]
            if n <= 0:
                return
    i0 = np.floor(pos).astype(np.int64)
    i0 = np.clip(i0, 0, length - 2)
    frac = (pos - i0).astype(np.float32)
    s0 = bank.pcm[base + i0]
    s1 = bank.pcm[base + i0 + 1]
    sig = (s0 * (1.0 - frac) + s1 * frac) * (env * v["gain"] / 32768.0)
    end = min(on + n, len(L))
    m = end - on
    if m <= 0:
        return
    L[on:end] += sig[:m] * v["panL"]
    R[on:end] += sig[:m] * v["panR"]


def _max_concurrency(voices, rate):
    pts = []
    for v in voices:
        on = v["on"]
        dur = (v["off"] - v["on"]) if v["off"] else v["z"]["length"] / max(1e-6, v["step"])
        pts.append((on, 1)); pts.append((on + max(dur, 1), -1))
    pts.sort()
    cur = mx = 0
    for _, d in pts:
        cur += d; mx = max(mx, cur)
    return mx


def _cap_polyphony(voices, poly):
    # coarse: keep at most `poly` voices sounding at once, drop the quietest new ones
    voices = sorted(voices, key=lambda v: v["on"])
    active = []   # (endSample, v)
    kept = []
    for v in voices:
        on = v["on"]
        dur = (v["off"] - v["on"]) if v["off"] else v["z"]["length"] / max(1e-6, v["step"])
        active = [(e, vv) for (e, vv) in active if e > on]
        if len(active) < poly:
            active.append((on + dur, v)); kept.append(v)
        else:
            # steal quietest active if new is louder
            active.sort(key=lambda ev: ev[1]["gain"])
            if v["gain"] > active[0][1]["gain"]:
                active[0] = (on + dur, v); kept.append(v)
    return kept


def write_wav(path, L, R, rate):
    if R is None:
        data = L
        peak = np.max(np.abs(data)) or 1.0
        data = np.clip(data / peak * 0.95, -1, 1)
        pcm = (data * 32767).astype("<i2")
        with wave.open(path, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
            w.writeframes(pcm.tobytes())
    else:
        peak = max(np.max(np.abs(L)), np.max(np.abs(R))) or 1.0
        L = np.clip(L / peak * 0.95, -1, 1); R = np.clip(R / peak * 0.95, -1, 1)
        inter = np.empty(len(L) * 2, dtype="<i2")
        inter[0::2] = (L * 32767).astype("<i2")
        inter[1::2] = (R * 32767).astype("<i2")
        with wave.open(path, "wb") as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
            w.writeframes(inter.tobytes())
    return peak


def main(argv):
    if len(argv) < 4:
        print(__doc__); return 1
    bank_path, mid_path, out_path = argv[1], argv[2], argv[3]
    rate, mono, poly, bend_range, seconds = None, False, 0, 2.0, 0.0
    force_prog, no_drums, debug, drums_only, only_chan = -1, False, False, False, -1
    i = 4
    while i < len(argv):
        a = argv[i]
        if a == "--rate": rate = int(argv[i+1]); i += 2
        elif a == "--mono": mono = True; i += 1
        elif a == "--poly": poly = int(argv[i+1]); i += 2
        elif a == "--bend-range": bend_range = float(argv[i+1]); i += 2
        elif a == "--seconds": seconds = float(argv[i+1]); i += 2
        elif a == "--force-prog": force_prog = int(argv[i+1]); i += 2
        elif a == "--no-drums": no_drums = True; i += 1
        elif a == "--drums-only": drums_only = True; i += 1
        elif a == "--only-chan": only_chan = int(argv[i+1]); i += 2
        elif a == "--debug": debug = True; i += 1
        elif a == "--gain": i += 2   # reserved
        else:
            print("unknown arg", a); return 1
    bank = Bank(bank_path)
    if rate is None:
        rate = bank.rate
    fmt, div, events = parse_midi(mid_path)
    n_on = sum(1 for e in events if e[2] == "on")
    progs = sorted(set(e[3] for e in events if e[2] == "prog"))
    chans = sorted(set(e[3] for e in events if e[2] in ("on",)))
    print(f"midi: fmt={fmt} div={div} events={len(events)} notes={n_on} "
          f"channels={chans}")
    events_s = tempo_map_to_samples(events, div, rate)
    L, R, max_conc, total = render(bank, events_s, rate, mono, bend_range, poly,
                                   seconds, force_prog, no_drums, debug, drums_only,
                                   only_chan)
    peak = write_wav(out_path, L, R, rate)
    print(f"wav: {out_path}  {total/rate:.1f}s @ {rate}Hz {'mono' if mono else 'stereo'}"
          f"  maxPolyphony={max_conc}  preNormPeak={peak:.2f}")
    if max_conc > 24:
        print(f"  note: peak polyphony {max_conc} > PS2 target 24 "
              f"(use --poly 24 to preview stealing)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
