"""Optional regression test on REAL public recordings (not in git; see NOTES.md for the links). Skips cleanly if the files, the firesideiq binary, numpy or scipy are missing.
Validates the demodulators against the reference audio shipped with the HamSCI baseband_iq_examples repository, and runs every effect over every recording."""
import os, shutil, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, "..", "..")
REC = os.path.join(ROOT, "recordings")
FIRESIDEIQ = next((p for p in (os.environ.get("FIRESIDEIQ"), os.path.join(ROOT, "build", "firesideiq")) if p and os.path.exists(p)), None)
try:
    import numpy as np
    from scipy.io import wavfile
    from scipy.signal import butter, correlate, resample_poly, sosfiltfilt
except ImportError:
    np = None


def have(*names): return all(os.path.exists(os.path.join(REC, n)) for n in names)


def run(wav, *args):
    out = tempfile.mktemp(suffix=".wav")
    r = subprocess.run([FIRESIDEIQ, os.path.join(REC, wav), *args, "--audio", out], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    rate, x = wavfile.read(out); os.unlink(out); return rate, x.astype(float) / 32768.0


def corr(ours, ours_rate, ref, ref_rate):
    from math import gcd
    g = gcd(ours_rate, ref_rate); ours = resample_poly(ours, ref_rate // g, ours_rate // g)
    sos = butter(4, (150, 3500), btype="band", fs=ref_rate, output="sos"); a, b = sosfiltfilt(sos, ours), sosfiltfilt(sos, ref)
    seg = slice(ref_rate * 2, ref_rate * 12); a = a[seg] - a[seg].mean(); b = b[seg] - b[seg].mean()
    c = correlate(b, a, mode="full", method="fft") / (np.linalg.norm(a) * np.linalg.norm(b)); return float(np.max(np.abs(c)))


@unittest.skipUnless(np is not None and FIRESIDEIQ, "needs numpy, scipy and build/firesideiq")
class RealRecordings(unittest.TestCase):
    @unittest.skipUnless(have("HamSCI_AM_modulated_WWV_iq.wav", "HamSCI_AM_demodulated_WWV.wav"), "HamSCI WWV recordings not downloaded")
    def test_am_demodulator_matches_the_reference_audio(self):
        rr, ref = wavfile.read(os.path.join(REC, "HamSCI_AM_demodulated_WWV.wav")); ref = ref.astype(float) if ref.ndim == 1 else ref[:, 0].astype(float)
        rate, ours = run("HamSCI_AM_modulated_WWV_iq.wav", "--mode", "am", "--am", "env", "--if-offset", "0")                  # the reference is an envelope detector's output
        self.assertGreater(corr(ours, rate, ref, rr), 0.99)       # measured 0.9936
        rate, ours = run("HamSCI_AM_modulated_WWV_iq.wav", "--mode", "am", "--if-offset", "0")                                  # the default is the synchronous detector: a different, better detector, still close
        self.assertGreater(corr(ours, rate, ref, rr), 0.98)       # measured 0.9869

    @unittest.skipUnless(have("HamSCI_nbfm_modulate_iq.wav", "HamSCI_nbfm_demod.wav"), "HamSCI NBFM recordings not downloaded")
    def test_fm_demodulator_matches_the_textbook_discriminator_and_reference(self):
        rate, ours = run("HamSCI_nbfm_modulate_iq.wav", "--mode", "fm", "--width", "6000", "--if-offset", "0")
        rr, ref = wavfile.read(os.path.join(REC, "HamSCI_nbfm_demod.wav")); ref = ref.astype(float) if ref.ndim == 1 else ref[:, 0].astype(float)
        self.assertGreater(corr(ours, rate, ref, rr), 0.90)                                  # measured 0.908; an ideal numpy discriminator scores 0.877 on this pair
        ri, iq = wavfile.read(os.path.join(REC, "HamSCI_nbfm_modulate_iq.wav")); z = iq[:, 0].astype(float) + 1j * iq[:, 1].astype(float)
        ideal = resample_poly(np.concatenate([[0], np.angle(z[1:] * np.conj(z[:-1]))]), 24, 125)
        self.assertGreater(corr(ours, rate, ideal, 48000), 0.97)                              # ours is the same discriminator, just filtered

    def test_every_effect_on_every_recording_stays_sane(self):
        files = [f for f in ("SDRuno_DEMO_3957kHz.wav", "HamSCI_AM_modulated_WWV_iq.wav", "HamSCI_nbfm_modulate_iq.wav") if have(f)]
        if not files: self.skipTest("no recordings downloaded")
        for f in files:
            for args in (["--mode", "am", "--agc", "med"], ["--mode", "usb", "--agc", "fast", "--nb"], ["--mode", "fm", "--agc", "slow"], ["--mode", "lsb", "--agc", "off"]):
                rate, x = run(f, *args, "--if-offset", "0")
                self.assertTrue(np.all(np.isfinite(x)), (f, args)); self.assertGreater(len(x), rate * 5, (f, args))
                self.assertLess(np.max(np.abs(x)), 1.0, (f, args))                          # no clipping or blow-ups (AGC target is 0.25)


if __name__ == "__main__":
    unittest.main()
