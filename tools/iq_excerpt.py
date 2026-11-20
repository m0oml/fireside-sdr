#!/usr/bin/env python3
"""Cut an excerpt out of a direct-stream capture (tools/record_band.sh) in the form `fireside --decode-file` takes: 2-channel float32 I/Q at the capture's rate,
un-mirrored, with the radio's 9 kHz DDS offset removed (0 Hz = the dial). Then tune inside it with the tool's offset argument:
  tools/iq_excerpt.py CAPTURE.wav excerpt.wav [--start 30] [--seconds 70]
  build/gui/fireside --decode-file excerpt.wav cw|psk31|rtty x TONE_HZ OFFSET_HZ     (OFFSET_HZ = signal frequency - dial, Hz; negative below the dial)
ft8/ft4 take no tone or offset (see tools/compare_ft8.py for whole captures)."""
import argparse, struct, numpy as np
ap = argparse.ArgumentParser(); ap.add_argument("src"); ap.add_argument("dst"); ap.add_argument("--start", type=float, default=0); ap.add_argument("--seconds", type=float, default=70); a = ap.parse_args()
with open(a.src, "rb") as f:
    h = f.read(44); fs = struct.unpack("<I", h[24:28])[0]
raw = np.memmap(a.src, dtype="<i4", mode="r", offset=44).reshape(-1, 2); i0 = int(a.start * fs); n = min(int(a.seconds * fs), len(raw) - i0)
z = (raw[i0:i0 + n, 0].astype(np.float64) + 1j * raw[i0:i0 + n, 1]) / 2**31
z = (np.conj(z) * np.exp(-2j * np.pi * 9000.0 * np.arange(n) / fs)).astype(np.complex64)
d = np.empty(2 * n, "<f4"); d[0::2] = z.real; d[1::2] = z.imag
open(a.dst, "wb").write(b"RIFF" + struct.pack("<I", 36 + d.nbytes) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, fs, fs * 8, 8, 32) + b"data" + struct.pack("<I", d.nbytes) + d.tobytes())
print(f"{a.dst}: {n / fs:.1f} s at {fs} Hz")
