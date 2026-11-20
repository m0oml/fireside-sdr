// Per-candidate FT8 demodulator (internal to fireside_core), after WSJT-X 3.x lib/ft8/ft8_downsample.f90, sync8d.f90 and ft8b.f90 (GPL v3): reimplemented from that source.
// ft8_lib computes one soft-bit set from its coarse waterfall (magnitudes only). WSJT-X, for each candidate, mixes it to baseband at 200 Hz, refines time (to 5 ms) and
// frequency (to 0.5 Hz) on the Costas arrays, takes complex per-symbol spectra and builds five soft-bit sets: 1, 2 and 3 symbols combined coherently, a per-bit
// normalised one, and the strongest of the first three. Weak signals that fail on one set often decode on another.
#pragma once
#include <complex>
#include <vector>

namespace fireside::decode::demod {
// FFT of one slot at 12 kHz, computed once and shared by every candidate in a decoding pass: FT8 15 s zero-padded to 192000 points, FT4 the whole 7.5 s slot (90000 points).
class SlotSpectrum {
public:
    SlotSpectrum(const float* audio12k, size_t n, bool ft4 = false);
    const std::vector<std::complex<float>>& bins() const { return cx_; }
    int nfft() const { return nfft_; }
private:
    std::vector<std::complex<float>> cx_; int nfft_;
};
struct Soft {
    bool ok = false;            // the hard sync check passed (more than 6 of the 21 Costas symbols where expected)
    int nsync = 0;              // Costas symbols whose strongest tone is the expected one (max 21)
    double f0 = 0, t0 = 0;      // refined audio frequency of tone 0 (Hz) and start of symbol 0 (s from the slot start)
    float llr[5][174] = {};
    float sync_power = 0; int sync_bits = 0;  // FT4: sync4d power at the best time/frequency and sync bits right (of 32), for diagnostics     // the five soft-bit sets, WSJT-X order: nsym 1, 2, 3, bit-normalised, best of 1-3 (scaled as WSJT-X: x 2.83 after normalising)
};
// f0_guess / t0_guess from the candidate (Hz, s). FT8 only.
Soft demodulate(const SlotSpectrum& spec, double f0_guess, double t0_guess);
// FT4 (after WSJT-X lib/ft4/ft4_downsample.f90, sync4d.f90, get_ft4_bitmetrics.f90 and lib/ft4_decode.f90, GPL v3: reimplemented): 666.67 Hz baseband (32 samples per symbol), sync on the
// four 4x4 Costas arrays searched over the whole -0.52..+1.52 s range in WSJT-X's three segments (+-12 Hz, then +-4 Hz and +-5 samples), complex symbol spectra, and THREE soft-bit
// sets: 1, 2 and 4 symbols combined coherently (llr[0..2], x 2.83 after normalising). Hard checks as WSJT-X: sync power >= 1.2, >= 8 of the 16 Costas symbols, >= 20 of the 32 sync bits.
// One Soft per time segment that qualifies (up to 3); ok = passed the checks. t0 = start of symbol 0 (the ramp). The bits are still scrambled (rvec): pack_result undoes it.
std::vector<Soft> demodulate_ft4(const SlotSpectrum& spec, double f0_guess);
}  // namespace fireside::decode::demod
