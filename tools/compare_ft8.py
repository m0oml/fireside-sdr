#!/usr/bin/env python3
"""Compare our FT8 decoder with WSJT-X's jt9 on a live capture from the radio.

  tools/compare_ft8.py CAPTURE.wav [--mode ft4] [--trim S] [--work DIR]

CAPTURE.wav: a 32-bit PCM capture of the FLEX-5000A, I/Q in channels 0/1, of any length (at least one slot). Any rate: 48/96/192 kHz. The radio's I/Q is mirrored and its DDS sits 9 kHz below the dial; this script
un-mirrors it, removes the offset and cuts it into slots, so the FIRST SLOT MUST START ON A UTC BOUNDARY (FT8 :00 :15 :30 :45, FT4 every 7.5 s). Where that boundary is comes from, in this order:
  --trim S            seconds to skip from the start of the file (you know it);
  the notes beside the recording (<name>.txt written by the app's Grab / Rec): `start_utc` to the millisecond gives the trim exactly; an older note with only the end time to the second
                      makes the script try trims within +-1 s of the estimate and keep the one where jt9 decodes most;
  neither: 0.40 s (a capture started just before a boundary, e.g. arecord; firesidecapture --align-ft8 needs --trim 0).
  work/ours/cNN.wav   2-ch float32 I/Q centred on the dial at the capture's own rate, up to 4 slots (FT4: 8) per file, for `fireside --decode-file`
  work/jt9/sNNN.wav   12 kHz mono 16-bit USB audio, one slot per file, for jt9 (WSJT-X: apt install wsjtx)
then runs both decoders and prints: decodes found by both / only one, SNR, DT and frequency differences, and our hit rate by WSJT-X SNR.
Read-only for the radio: it only reads the capture file. Needs numpy, jt9, build/gui/fireside."""
import argparse, collections, contextlib, io, os, re, shutil, statistics as st, struct, subprocess, sys
import numpy as np

MODE = {'ft8': dict(name='ft8', slot=15.0, per=4, flag='-8'), 'ft4': dict(name='ft4', slot=7.5, per=8, flag='-5')}   # per: slots per file for our decoder (60 s)
PER = 4

def wav_layout(path):
    """(rate, channels, data offset) of a 32-bit PCM WAV; walks the chunks so arecord's 44-byte and other headers both work."""
    with open(path, 'rb') as f:
        h = f.read(12)
        if h[:4] != b'RIFF' or h[8:12] != b'WAVE': sys.exit(f"{path}: not a WAV file")
        rate = ch = bits = None
        while True:
            c = f.read(8)
            if len(c) < 8: sys.exit(f"{path}: no data chunk")
            cid, n = c[:4], struct.unpack('<I', c[4:])[0]
            if cid == b'fmt ': fmt, ch, rate, _, _, bits = struct.unpack('<HHIIHH', f.read(16)); f.seek(n - 16, 1)
            elif cid == b'data':
                if bits != 32: sys.exit(f"{path}: expected 32-bit PCM, got {bits}-bit")
                return rate, ch, f.tell()
            else: f.seek(n + (n & 1), 1)

def norm(m):
    # jt9 decodes each 15 s file alone, so a hashed callsign it has not seen in that file is "<...>"; ours carries the hash table across
    # slots and prints "<CALL>". Same message: compare with any <...> collapsed.
    # jt9 also appends a-priori decoding markers ("a1".."a7") and "?" for low-confidence decodes: not part of the message.
    m = re.sub(r"\s+(a\d|\?)$", "", " ".join(m.split()))
    return re.sub(r"<[^>]*>", "<H>", m)

def wav_f32(path, a, FS):
    d = np.empty(2 * len(a), dtype='<f4'); d[0::2] = a.real; d[1::2] = a.imag
    hdr = b'RIFF' + struct.pack('<I', 36 + d.nbytes) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 3, 2, FS, FS * 8, 8, 32) + b'data' + struct.pack('<I', d.nbytes)
    open(path, 'wb').write(hdr + d.tobytes())

M = MODE['ft8']
GROUPS = []                                                                                           # slots in each of our files (the last may be short)

def prepare(src, work, trim_s, ours=True, max_slots=None):
    for d in ("ours", "jt9"): os.makedirs(f"{work}/{d}", exist_ok=True)
    FS, nch, off = wav_layout(src); SLOT = int(round(M['slot'] * FS)); N12 = int(round(M['slot'] * 12000))
    raw = np.memmap(src, dtype='<i4', mode='r', offset=off); raw = raw[:len(raw) // nch * nch].reshape(-1, nch)
    print(f"{src}: {FS} Hz, {nch} channels, {len(raw) / FS:.1f} s")
    trim = int(round(trim_s * FS))
    z = (raw[trim:, 0].astype(np.float64) + 1j * raw[trim:, 1].astype(np.float64)) / 2**31
    z = (np.conj(z) * np.exp(-2j * np.pi * 9000.0 * np.arange(len(z)) / FS)).astype(np.complex64)   # un-mirror, then shift the DDS offset out
    ns = len(z) // SLOT
    if max_slots: ns = min(ns, max_slots)
    if ns < 1: sys.exit(f"{src}: after trimming {trim_s:.2f} s only {len(z) / FS:.1f} s are left: shorter than one {M['slot']} s slot")
    GROUPS.clear()
    if ours:
        for c in range((ns + PER - 1) // PER):                                                          # every slot is compared: the last file may hold fewer than PER
            k0, k1 = c * PER, min(ns, (c + 1) * PER); wav_f32(f"{work}/ours/c{c:02d}.wav", z[k0 * SLOT:k1 * SLOT], FS); GROUPS.append(k1 - k0)
    auds = []
    for k in range(ns):
        X = np.fft.fft(z[k * SLOT:(k + 1) * SLOT].astype(np.complex128)); f = np.fft.fftfreq(SLOT, 1 / FS)
        idx = np.where((f >= 100) & (f <= 3500))[0]; Y = np.zeros(N12, dtype=np.complex128)
        Y[np.round(f[idx] * N12 / 12000).astype(int)] = X[idx] * (N12 / SLOT); auds.append(np.fft.ifft(Y).real * 2)
    gain = 0.1 / np.sqrt(np.mean(np.concatenate(auds) ** 2))                                          # one static gain for the whole capture
    for k, a in enumerate(auds):
        s16 = np.clip(a * gain * 32768, -32768, 32767).astype('<i2')
        hdr = b'RIFF' + struct.pack('<I', 36 + s16.nbytes) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 12000, 24000, 2, 16) + b'data' + struct.pack('<I', s16.nbytes)
        open(f"{work}/jt9/s{k:03d}.wav", 'wb').write(hdr + s16.tobytes())
    return ns

def run_jt9(work, ns):
    J = {}
    for k in range(ns):
        p = f"{work}/jt9/s{k:03d}.wav"; t = f"{work}/jt9/t{k:03d}.wav"; shutil.copy(p, t)
        out = subprocess.run(["jt9", M['flag'], "-d", "3", "-p", str(int(M['slot'])), t], capture_output=True, text=True, timeout=120, cwd=work).stdout; os.remove(t)
        for l in out.splitlines():
            m = re.match(r"\d{6}\s+(-?\d+)\s+(-?[\d.]+)\s+(\d+)\s+[~+]\s+(.*)$", l.strip())
            if m: J.setdefault((k, norm(m[4])), (int(m[1]), float(m[2]), int(m[3])))
    return J

def run_ours(work, app):
    O = {}; env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    for c in range(len(GROUPS)):
        out = subprocess.run([app, "--decode-file", f"{work}/ours/c{c:02d}.wav", M['name']], capture_output=True, text=True, timeout=300, env=env).stdout
        for l in out.splitlines():
            m = re.match(r"S(\d+)\s+([+-]?\d+)\s+(-?[\d.]+)\s+(\d+)\s+~\s+(.*)$", l.strip())
            if m and int(m[1]) < GROUPS[c]: O.setdefault((c * PER + int(m[1]), norm(m[5])), (int(m[2]), float(m[3]), int(m[4])))   # the app pads a short file to 60 s by repeating it: ignore slots past the file's end
    return O

def report(J, O, nslots_ours):
    J = {k: v for k, v in J.items() if k[0] < nslots_ours}
    both = J.keys() & O.keys(); only_j = J.keys() - O.keys(); only_o = O.keys() - J.keys()
    print(f"slots 0..{nslots_ours - 1}:  jt9 {len(J)}   ours {len(O)}   both {len(both)}   only jt9 {len(only_j)}   only ours {len(only_o)}")
    if not both: return
    print(f"ours finds {100 * len(both) / len(J):.1f}% of jt9's decodes; {100 * len(only_o) / max(1, len(O)):.1f}% of ours are not in jt9")
    d = lambda i: [O[k][i] - J[k][i] for k in both]
    print(f"SNR  ours-jt9: mean {st.mean(d(0)):+.1f} dB  median {st.median(d(0)):+.1f}  sd {st.pstdev(d(0)):.1f}")
    print(f"DT   ours-jt9: mean {st.mean(d(1)):+.2f} s  median {st.median(d(1)):+.2f}")
    print(f"freq ours-jt9: mean {st.mean(d(2)):+.1f} Hz  max |d| {max(abs(x) for x in d(2))}")
    print("jt9 decodes by jt9 SNR (ours found / total):")
    for name, f in (("> -10", lambda s: s > -10), ("-10..-15", lambda s: -15 < s <= -10), ("-15..-20", lambda s: -20 < s <= -15), ("<= -20", lambda s: s <= -20)):
        tot = [k for k in J if f(J[k][0])]; print(f"  {name:>9}: {sum(k in O for k in tot):4d} / {len(tot):4d}")

def read_notes(wav):
    """key=value notes written next to a recording by the app (Grab / Rec: <name>.txt) or by firesidecapture; {} if there are none."""
    p = os.path.splitext(wav)[0] + ".txt"; d = {}
    if os.path.isfile(p):
        for tok in open(p, errors="replace").read().split():
            if "=" in tok: k, v = tok.split("=", 1); d[k] = v
    return d

def iso_epoch(t):
    from datetime import datetime, timezone
    for f in ("%Y-%m-%dT%H:%M:%S.%fZ", "%Y-%m-%dT%H:%M:%SZ"):
        try: return datetime.strptime(t, f).replace(tzinfo=timezone.utc).timestamp(), "." in t
        except ValueError: pass
    return None, False

def start_from_notes(notes):
    """(epoch seconds of the first sample, known to the millisecond?) from the notes, or (None, False)."""
    if "start_utc" in notes: return iso_epoch(notes["start_utc"])
    if "utc" in notes and "seconds" in notes:                                  # an older note: the end time to the second, and the length
        e, _ = iso_epoch(notes["utc"]); return (e - float(notes["seconds"]), False) if e is not None else (None, False)
    return None, False

def slot_trim(start_epoch):
    """Seconds to skip from the first sample to reach the next slot boundary (FT8 15 s, FT4 7.5 s: multiples of the slot length since the epoch, which are :00 :15 :30 :45 / :00 :07.5 ...)."""
    return (-start_epoch) % M['slot']

def sweep(src, work, centre, span=1.0, step=0.25, nslots=6):
    """When the start is only known to the second: try trims around the estimate and keep the one where jt9 decodes the most in the first few slots."""
    results = []; seen = []
    for k in range(-int(round(span / step)), int(round(span / step)) + 1):
        t = (centre + k * step) % M['slot']
        if any(abs(t - u) < 1e-9 for u in seen): continue
        seen.append(t)
        for f in glob_files(work + "/jt9"): os.remove(f)
        with contextlib.redirect_stdout(io.StringIO()): got = prepare(src, work, t, ours=False, max_slots=nslots)      # a short recording has fewer slots than asked for
        n = len(run_jt9(work, got)); print(f"  trim {t:5.2f} s: jt9 decodes {n} in {got} slots", flush=True)
        results.append((n, t))
    top = max(n for n, _ in results); plateau = sorted(t for n, t in results if n == top)
    return plateau[len(plateau) // 2]                                           # the middle of the trims that decode the most, not the first

def glob_files(d): return [os.path.join(d, f) for f in os.listdir(d)] if os.path.isdir(d) else []

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0]); ap.add_argument("capture")
    ap.add_argument("--trim", type=float, default=None, help="seconds to skip so the first slot starts on a boundary; default: worked out from the recording's notes (start_utc), else 0.40")
    ap.add_argument("--work", default=None); ap.add_argument("--mode", choices=list(MODE), default="ft8"); ap.add_argument("--app", default=os.path.join(os.path.dirname(__file__), "..", "build", "gui", "fireside"))
    a = ap.parse_args(); global M, PER; M = MODE[a.mode]; PER = M['per']; work = os.path.abspath(a.work or os.path.splitext(a.capture)[0] + "_work")   # absolute: jt9 runs with cwd=work
    if not shutil.which("jt9"): sys.exit("jt9 not found (apt install wsjtx)")
    trim = a.trim
    if trim is None:
        start, precise = start_from_notes(read_notes(a.capture))
        if start is None: trim = 0.40; print("no notes next to the recording: trim 0.40 s (use --trim to set it)")
        elif precise: trim = slot_trim(start); print(f"trim {trim:.3f} s from the recording's start time (to the millisecond)")
        else:
            c = slot_trim(start); print(f"the notes give the start only to the second: trying trims around {c:.2f} s")
            for d in ("ours", "jt9"): os.makedirs(f"{work}/{d}", exist_ok=True)
            trim = sweep(a.capture, work, c); print(f"using trim {trim:.2f} s")
        for f in glob_files(work + "/jt9"): os.remove(f)
    ns = prepare(a.capture, work, trim); print(f"{ns} slots ({ns * M['slot']:.1f} s) prepared in {work}")
    J = run_jt9(work, ns); O = run_ours(work, a.app); report(J, O, ns)

if __name__ == "__main__": main()
