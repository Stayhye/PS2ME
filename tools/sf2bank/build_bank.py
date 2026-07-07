#!/usr/bin/env python3
# PS2ME audio -- Phase 4, Stage 0: offline SoundFont -> compact wavetable bank.
#
# Reads a General MIDI SF2, resolves the two-level (preset+instrument) generator
# stacks, and emits a compact little-endian .bin: per GM program (0..127) plus a
# drum kit, one or more key-split zones each carrying a mono PCM16 sample
# (resampled to a single bank rate), root key, loop points, a volume-envelope
# ADSR, pan and attenuation. Velocity layers are collapsed to the loudest so the
# bank stays small; key splits are kept so tuning/timbre don't stretch.
#
# The runtime phase-increment then reduces to 2^((key-root)/12) because every
# sample is pre-resampled to the bank rate -- independent of each SF2 sample's
# own rate. This mirrors the Q16 accumulator already validated on the PS2.
#
# Usage:
#   python build_bank.py SOUNDFONT.sf2 OUT.bin [--rate 22050] [--max-seconds 4.0]
#                        [--programs 0,1,24,...] [--drum-bank 128] [--verbose]
#
# Bank format (all little-endian):
#   Header:
#     char magic[4]      = "PS2B"
#     u16  version       = 1
#     u16  flags         = 0
#     u32  sampleRate    (all PCM is at this rate)
#     u16  numPrograms   = 128
#     u16  numDrumNotes  = 128
#     u32  numZones
#     u32  pcmSamples    (total mono PCM16 samples in the pool)
#   ProgramDir[128] : { u16 zoneStart; u8 zoneCount; u8 pad }
#   DrumDir[128]    : { u16 zoneStart; u8 zoneCount; u8 pad }   # index = GM drum key
#   Zone[numZones]  : (see ZONE_FMT below)
#   int16 pcm[pcmSamples]
import struct, sys, os
import numpy as np

# ---- SF2 generator operators we care about -------------------------------------
GEN_STARTADDR      = 0
GEN_ENDADDR        = 1
GEN_STARTLOOP      = 2
GEN_ENDLOOP        = 3
GEN_STARTADDR_C    = 4
GEN_ENDADDR_C      = 12
GEN_STARTLOOP_C    = 45
GEN_ENDLOOP_C      = 50
GEN_PAN            = 17
GEN_DELAY_VOLENV   = 33
GEN_ATTACK_VOLENV  = 34
GEN_HOLD_VOLENV    = 35
GEN_DECAY_VOLENV   = 36
GEN_SUSTAIN_VOLENV = 37
GEN_RELEASE_VOLENV = 38
GEN_KEYRANGE       = 43
GEN_VELRANGE       = 44
GEN_INITATTEN      = 48
GEN_COARSETUNE     = 51
GEN_FINETUNE       = 52
GEN_SAMPLEID       = 53
GEN_SAMPLEMODES    = 54
GEN_ROOTKEY        = 58
GEN_INSTRUMENT     = 41

RANGE_GENS = {GEN_KEYRANGE, GEN_VELRANGE}
TERMINAL_GENS = {GEN_SAMPLEID, GEN_INSTRUMENT}

# Zone record: see comments. All little-endian.
#   u32 sampleOff, u32 sampleLen, u32 loopStart, u32 loopEnd,
#   u8 rootKey, s8 fineTune(cents,-128..127 clamp), u8 keyLo, u8 keyHi,
#   s16 pan(-500..500), u16 attenCb(*10... stored as raw cB 0..960),
#   u16 attackMs, u16 holdMs, u16 decayMs, u16 releaseMs, u16 sustainPermille,
#   u8 sampleMode, u8 pad
ZONE_FMT = "<IIII bB BB h H HHHH H B H B"
ZONE_SIZE = struct.calcsize(ZONE_FMT)
GEN_INITFILTERFC   = 8    # initial low-pass cutoff (absolute cents)
GEN_INITFILTERQ    = 9    # low-pass resonance (centibels)


def _walk_riff(data):
    """Return dict of top-level chunks. sdta/pdta expand into their subchunks."""
    assert data[:4] == b"RIFF" and data[8:12] == b"sfbk", "not an SF2 (RIFF/sfbk)"
    out = {}
    def walk(buf, base, end):
        p = base
        while p + 8 <= end:
            cid = buf[p:p+4]
            sz = struct.unpack_from("<I", buf, p+4)[0]
            body = p + 8
            if cid == b"LIST":
                ltype = buf[body:body+4]
                walk(buf, body+4, body+sz)
            else:
                out[cid.decode("latin1")] = (body, sz)
            p = body + sz + (sz & 1)
    walk(data, 12, 8 + struct.unpack_from("<I", data, 4)[0])
    return out


class SF2:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        c = _walk_riff(self.data)
        self.smpl = self._chunk(c, "smpl")          # raw PCM16 pool
        self.pcm = np.frombuffer(self.smpl, dtype="<i2")
        self.phdr = self._records(c, "phdr", 38)
        self.pbag = self._records(c, "pbag", 4)
        self.pgen = self._records(c, "pgen", 4)
        self.inst = self._records(c, "inst", 22)
        self.ibag = self._records(c, "ibag", 4)
        self.igen = self._records(c, "igen", 4)
        self.shdr = self._records(c, "shdr", 46)

    def _chunk(self, c, name):
        if name not in c:
            return b""
        off, sz = c[name]
        return self.data[off:off+sz]

    def _records(self, c, name, size):
        off, sz = c[name]
        n = sz // size
        return [self.data[off+i*size: off+(i+1)*size] for i in range(n)]

    # -- accessors ---------------------------------------------------------------
    def phdr_get(self, i):
        r = self.phdr[i]
        name = r[:20].split(b"\0")[0].decode("latin1", "replace")
        preset, bank, bagndx = struct.unpack_from("<HHH", r, 20)
        return name, preset, bank, bagndx

    def inst_get(self, i):
        r = self.inst[i]
        name = r[:20].split(b"\0")[0].decode("latin1", "replace")
        bagndx = struct.unpack_from("<H", r, 20)[0]
        return name, bagndx

    def shdr_get(self, i):
        r = self.shdr[i]
        (start, end, sloop, eloop, srate) = struct.unpack_from("<IIIII", r, 20)
        opitch, pcorr, link, stype = struct.unpack_from("<BbHH", r, 40)
        return dict(start=start, end=end, sloop=sloop, eloop=eloop, srate=srate,
                    root=opitch, pcorr=pcorr, link=link, stype=stype)

    @staticmethod
    def _bag(rec):
        return struct.unpack_from("<HH", rec, 0)   # genNdx, modNdx

    @staticmethod
    def _gen(rec):
        op, amt = struct.unpack_from("<HH", rec, 0)
        return op, amt

    def _zone_gens(self, gen_arr, g0, g1):
        """Parse a run of generators into {op: value}. Ranges -> (lo,hi);
        terminals -> unsigned; everything else -> signed short."""
        d = {}
        for g in range(g0, g1):
            op, amt = self._gen(gen_arr[g])
            if op in RANGE_GENS:
                d[op] = (amt & 0xff, (amt >> 8) & 0xff)
            elif op in TERMINAL_GENS:
                d[op] = amt
            else:
                d[op] = amt - 0x10000 if amt >= 0x8000 else amt
        return d

    def resolve_instrument(self, inst_id):
        """Return list of {op:val} zones (global merged), each with a sampleID."""
        _, b0 = self.inst_get(inst_id)
        _, b1 = self.inst_get(inst_id + 1)
        zones, glob = [], {}
        for iz in range(b0, b1):
            g0 = self._bag(self.ibag[iz])[0]
            g1 = self._bag(self.ibag[iz + 1])[0]
            gens = self._zone_gens(self.igen, g0, g1)
            if GEN_SAMPLEID not in gens:
                if iz == b0:
                    glob = gens                 # instrument global zone
                continue
            merged = dict(glob); merged.update(gens)
            zones.append(merged)
        return zones

    def resolve_preset(self, preset_idx):
        """Return list of fully-merged zones (preset gens added onto instrument
        gens) each carrying a sampleID."""
        _, _, _, b0 = self.phdr_get(preset_idx)
        _, _, _, b1 = self.phdr_get(preset_idx + 1)
        out, pglob = [], {}
        for pz in range(b0, b1):
            g0 = self._bag(self.pbag[pz])[0]
            g1 = self._bag(self.pbag[pz + 1])[0]
            pgens = self._zone_gens(self.pgen, g0, g1)
            if GEN_INSTRUMENT not in pgens:
                if pz == b0:
                    pglob = pgens               # preset global zone
                continue
            eff = dict(pglob); eff.update(pgens)
            inst_id = eff[GEN_INSTRUMENT]
            for iz in self.resolve_instrument(inst_id):
                out.append(self._combine(eff, iz))
        return out

    @staticmethod
    def _combine(pgens, izgens):
        """Preset gens are additive over the instrument zone (SF2 spec), except
        ranges (intersect) and terminals (ignored)."""
        res = dict(izgens)
        for op, amt in pgens.items():
            if op in TERMINAL_GENS:
                continue
            if op in RANGE_GENS:
                lo, hi = amt
                if op in res:
                    rlo, rhi = res[op]
                    res[op] = (max(lo, rlo), min(hi, rhi))
                else:
                    res[op] = amt
            else:
                res[op] = res.get(op, 0) + amt
        return res


def timecents_to_ms(tc, default_instant=True):
    if tc is None or tc <= -12000:
        return 0
    return int(round(1000.0 * (2.0 ** (tc / 1200.0))))


def sustain_permille(cb):
    # sustainVolEnv is attenuation in centibels; level = 10^(-cB/200).
    if cb is None:
        cb = 0
    cb = max(0, cb)
    frac = 10.0 ** (-cb / 200.0)
    return int(round(max(0.0, min(1.0, frac)) * 1000))


def resample_linear(x, ratio):
    """Resample mono float array by ratio (out_rate/in_rate) with linear interp."""
    if ratio == 1.0 or len(x) < 2:
        return x.astype(np.float32)
    n_out = max(1, int(round(len(x) * ratio)))
    idx = np.arange(n_out, dtype=np.float64) / ratio
    i0 = np.floor(idx).astype(np.int64)
    i0 = np.clip(i0, 0, len(x) - 2)
    frac = (idx - i0).astype(np.float32)
    return (x[i0] * (1.0 - frac) + x[i0 + 1] * frac).astype(np.float32)


def build(sf2_path, out_path, rate=22050, max_seconds=4.0,
          programs=None, drum_bank=128, verbose=False):
    sf = SF2(sf2_path)

    # index presets by (bank, preset)
    presets = {}
    for i in range(len(sf.phdr) - 1):        # last is terminal EOP
        name, preset, bank, _ = sf.phdr_get(i)
        presets[(bank, preset)] = i

    prog_list = programs if programs is not None else list(range(128))

    # A "job" is one program (or one drum key) -> list of extracted zones.
    pool = []            # list of np.int16 arrays (unique resampled samples)
    pool_len = 0         # in samples
    sample_cache = {}    # key -> (sampleOff, sampleLen, loopStart, loopEnd)

    zones = []           # emitted Zone dicts (in emission order)
    prog_dir = [(0, 0)] * 128
    drum_dir = [(0, 0)] * 128

    def add_sample(shdr, mode):
        """Resample the referenced SF2 sample to bank rate; cache & pool it.
        Returns (off, length, loopStart, loopEnd) in bank-rate samples."""
        key = (shdr["start"], shdr["end"], shdr["srate"])
        if key in sample_cache:
            return sample_cache[key]
        raw = sf.pcm[shdr["start"]:shdr["end"]].astype(np.float32)
        srate = shdr["srate"] or rate
        ratio = rate / float(srate)
        # trim absurdly long one-shots to keep the bank small
        max_in = int(max_seconds * srate)
        loop_end_rel = shdr["eloop"] - shdr["start"]
        if len(raw) > max_in and (mode == 0 or loop_end_rel <= 0):
            raw = raw[:max_in]
        res = resample_linear(raw, ratio)
        ls = int(round((shdr["sloop"] - shdr["start"]) * ratio))
        le = int(round((shdr["eloop"] - shdr["start"]) * ratio))
        ls = max(0, min(ls, len(res)))
        le = max(0, min(le, len(res)))
        pcm16 = np.clip(np.round(res), -32768, 32767).astype("<i2")
        nonlocal pool_len
        off = pool_len
        pool.append(pcm16)
        pool_len += len(pcm16)
        val = (off, len(pcm16), ls, le)
        sample_cache[key] = val
        return val

    def zone_from_gens(g, force_key=None):
        sid = g[GEN_SAMPLEID]
        shdr = sf.shdr_get(sid)
        mode = g.get(GEN_SAMPLEMODES, 0)
        off, length, ls, le = add_sample(shdr, mode)
        root = g.get(GEN_ROOTKEY, -1)
        if root is None or root < 0 or root > 127:
            root = shdr["root"]
        if force_key is not None:
            # drum voices are one-shots pitched at their own key
            root = force_key
        root_orig = root   # pitch reference before the coarse-tune fold below
        # SF2 pitch = (note - root) + coarseTune + (fine+pitchCorr)/100 semitones.
        # The bank's runtime step is 2^((note - root_stored + cents/100)/12), and
        # cents is only s8 (+-127). So FOLD the whole-semitone coarse tune into the
        # stored root (root_stored = root - coarse) and keep only the fine cents.
        # Clamping coarse into cents (the old bug) lost tricks like rootOverride=36
        # + coarseTune=-24 (net root 60), making those instruments play octaves high.
        coarse = g.get(GEN_COARSETUNE, 0)
        fine = g.get(GEN_FINETUNE, 0)
        root = max(0, min(127, root - coarse))
        # Wavetable loop tuning: when the sustain loop spans a whole number of
        # periods (a synthetic wavetable of N cycles), its INTEGER length -- not the
        # sample content -- sets the pitch. After resampling to the bank rate the
        # length no longer spans an exact N periods, so note==root plays sharp/flat
        # (e.g. a 1-cycle 15.8-sample loop stored as 15 = ~+90 cents). Correct cents
        # to the true period so note==root_orig sounds at exactly f(root_orig).
        #
        # The test is "loop length ~= an integer multiple of the period", NOT "loop
        # dominates the sample": HL4MGM's flute/guitar/violin are a recorded attack
        # plus a tiny 1-cycle sustain loop, so the old 0.5*length gate skipped them
        # and they played tens of cents off. Recorded multi-cycle loops (piano/
        # strings: hundreds of cycles, length not period-aligned) either land on
        # corr~=0 or fail the integer test, so their pitch stays in the content.
        corr = 0.0
        use_pcorr = True
        actual_loop = le - ls
        if mode in (1, 3) and actual_loop > 1:
            root_hz = 440.0 * (2.0 ** ((root_orig - 69) / 12.0))
            period = rate / root_hz
            if period > 0:
                cyc = actual_loop / period
                cycles = max(1, int(round(cyc)))
                if abs(cyc - cycles) <= 0.25:
                    # produced pitch = rate*cycles/actual_loop; corr < 0 when the loop
                    # is too short (note plays sharp). Adding corr lowers it in tune.
                    corr = 1200.0 * float(np.log2(actual_loop / (cycles * period)))
                    # This loop-length tuning is ABSOLUTE (it lands note==root on
                    # f(root) by construction). The shdr pitch-correction was the
                    # author's fix for the SAME loop at the SOURCE rate, so adding it
                    # would double-count -- drop it here (keep the deliberate fine).
                    use_pcorr = False
        pcorr = shdr["pcorr"] if use_pcorr else 0
        cents = max(-127, min(127, int(round(fine + pcorr + corr))))
        pan = max(-500, min(500, g.get(GEN_PAN, 0)))
        atten = max(0, min(960, g.get(GEN_INITATTEN, 0)))
        klo, khi = g.get(GEN_KEYRANGE, (0, 127))
        if force_key is not None:
            klo = khi = force_key
        a = timecents_to_ms(g.get(GEN_ATTACK_VOLENV))
        h = timecents_to_ms(g.get(GEN_HOLD_VOLENV))
        d = timecents_to_ms(g.get(GEN_DECAY_VOLENV))
        r = timecents_to_ms(g.get(GEN_RELEASE_VOLENV))
        susp = sustain_permille(g.get(GEN_SUSTAIN_VOLENV, 0))
        # if no loop, force a small release so one-shots ring out naturally
        if mode == 0 and r == 0:
            r = 40
        # SF2 per-voice low-pass filter: initialFilterFc is an absolute pitch in
        # cents (Hz = 8.176*2^(c/1200)); default 13500c ~= 20 kHz = wide open. Most
        # HL4MGM instruments set it far lower (1-5 kHz) and rely on it for timbre --
        # without it every voice is too bright/harsh. Store the cutoff in Hz (0 =
        # open) and the resonance in centibels (0 = none).
        fc_cents = g.get(GEN_INITFILTERFC, 13500)
        fc_hz = int(round(8.176 * (2.0 ** (fc_cents / 1200.0))))
        fc_hz = 0 if fc_hz >= 18000 else max(0, min(20000, fc_hz))
        fq = max(0, min(255, g.get(GEN_INITFILTERQ, 0)))
        return dict(off=off, length=length, ls=ls, le=le, root=root, cents=cents,
                    klo=klo, khi=khi, pan=pan, atten=atten,
                    a=min(a, 65000), h=min(h, 65000), d=min(d, 65000),
                    r=min(r, 65000), susp=susp, mode=mode, fc=fc_hz, fq=fq)

    def pick_loud_layer(zone_gens_list):
        """Collapse velocity layers: keep zones whose velRange covers 127."""
        loud = [g for g in zone_gens_list
                if g.get(GEN_VELRANGE, (0, 127))[1] >= 127]
        return loud if loud else zone_gens_list

    # ---- melodic programs ------------------------------------------------------
    for prog in prog_list:
        idx = presets.get((0, prog))
        if idx is None:
            continue
        gens_list = pick_loud_layer(sf.resolve_preset(idx))
        start = len(zones)
        for g in gens_list:
            if GEN_SAMPLEID not in g:
                continue
            zones.append(zone_from_gens(g))
        prog_dir[prog] = (start, len(zones) - start)
        if verbose:
            print(f"  prog {prog:3d}: {len(zones)-start} zones")

    # ---- drum kit --------------------------------------------------------------
    drum_idx = presets.get((drum_bank, 0))
    if drum_idx is None:
        # some fonts put the kit at (128, 0) only; try any preset in drum bank
        for (bank, pr), i in presets.items():
            if bank == drum_bank:
                drum_idx = i
                break
    if drum_idx is not None:
        drum_zones = sf.resolve_preset(drum_idx)
        # group drum zones by key; each key -> its (loudest) zone
        by_key = {}
        for g in drum_zones:
            if GEN_SAMPLEID not in g:
                continue
            klo, khi = g.get(GEN_KEYRANGE, (0, 127))
            if g.get(GEN_VELRANGE, (0, 127))[1] < 127:
                continue
            for k in range(klo, khi + 1):
                if k not in by_key:
                    by_key[k] = g
        for k in sorted(by_key):
            start = len(zones)
            zones.append(zone_from_gens(by_key[k], force_key=k))
            drum_dir[k] = (start, 1)
        if verbose:
            print(f"  drum kit: {len(by_key)} keys")
    else:
        print("  WARNING: no drum kit preset found (bank %d)" % drum_bank)

    # ---- emit ------------------------------------------------------------------
    pcm_all = np.concatenate(pool) if pool else np.zeros(0, dtype="<i2")
    header = struct.pack("<4sHHIHHII", b"PS2B", 1, 0, rate, 128, 128,
                         len(zones), len(pcm_all))
    parts = [header]
    for (s, c) in prog_dir:
        parts.append(struct.pack("<HBB", s & 0xffff, c & 0xff, 0))
    for (s, c) in drum_dir:
        parts.append(struct.pack("<HBB", s & 0xffff, c & 0xff, 0))
    for z in zones:
        parts.append(struct.pack(ZONE_FMT,
                                 z["off"], z["length"], z["ls"], z["le"],
                                 z["cents"], z["root"], z["klo"], z["khi"],
                                 z["pan"], z["atten"],
                                 z["a"], z["h"], z["d"], z["r"], z["susp"],
                                 z["mode"], z["fc"], z["fq"]))
    parts.append(pcm_all.tobytes())
    blob = b"".join(parts)
    open(out_path, "wb").write(blob)

    print(f"bank: {out_path}")
    print(f"  rate={rate}Hz  zones={len(zones)}  pcm={len(pcm_all)} samples "
          f"({len(pcm_all)*2/1e6:.2f} MB)")
    print(f"  total file: {len(blob)/1e6:.2f} MB")
    prog_used = sum(1 for c in prog_dir if c[1] > 0)
    drum_used = sum(1 for c in drum_dir if c[1] > 0)
    print(f"  programs with samples: {prog_used}/128   drum keys: {drum_used}")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    sf2_path, out_path = argv[1], argv[2]
    rate, max_seconds, programs, drum_bank, verbose = 22050, 4.0, None, 128, False
    i = 3
    while i < len(argv):
        a = argv[i]
        if a == "--rate": rate = int(argv[i+1]); i += 2
        elif a == "--max-seconds": max_seconds = float(argv[i+1]); i += 2
        elif a == "--programs": programs = [int(x) for x in argv[i+1].split(",")]; i += 2
        elif a == "--drum-bank": drum_bank = int(argv[i+1]); i += 2
        elif a == "--verbose": verbose = True; i += 1
        else:
            print("unknown arg", a); return 1
    build(sf2_path, out_path, rate, max_seconds, programs, drum_bank, verbose)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
