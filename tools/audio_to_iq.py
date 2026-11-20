#!/usr/bin/env python3
"""Turn a real audio WAV (e.g. WSJT-X's FT8/FT4 samples, 12 kHz mono) into a 48 kHz complex I/Q WAV the receiver app can open.
The audio is treated as an upper-sideband signal: a tone at 1000 Hz appears 1 kHz above the dial. Open it in the app, pick USB and the FT8/FT4 decoder.
usage: audio_to_iq.py in.wav out.wav"""
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import hilbert, resample_poly

def main(src, dst):
    rate, x = wavfile.read(src)
    x = x.astype(np.float64)
    if x.ndim > 1: x = x[:, 0]
    x /= max(1.0, np.abs(x).max())
    if rate != 48000:
        from math import gcd
        g = gcd(48000, rate); x = resample_poly(x, 48000 // g, rate // g)
    z = hilbert(x)                                              # analytic signal: only positive frequencies, so USB
    out = np.stack([z.real, z.imag], axis=1).astype(np.float32) * 0.5
    wavfile.write(dst, 48000, out)
    print(f"{dst}: {len(x) / 48000:.1f} s, 48 kHz complex float32")

if __name__ == "__main__":
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(*sys.argv[1:])
