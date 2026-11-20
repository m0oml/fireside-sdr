#pragma once
// NR2: spectral noise reduction for voice, from WDSP's EMNR (Warren Pratt NR0V, TAPR OpenHPSDR-wdsp 2.11, GPL v2 or later; vendored in
// third_party/wdsp-nr2, see its README). Estimates the noise spectrum and attenuates it frame by frame (4096-point FFT, 4x overlap, "trained"
// gain, artifact elimination). Measured against our adaptive line filter on speech in real band noise (HANDOFF 4e): +1.3 .. +2.1 dB output SNR
// where the line filter was 2 .. 3 dB WORSE than nothing, at a fifth of the CPU. The price is delay: about 90 ms.
// Qt-free, thread-safe in the sense that each instance is used from one thread (the engine's). Built only when the system has libfftw3
// (FIRESIDE_HAVE_NR2); without it spectral_nr_available() is false and the constructor throws.
#include <memory>
#include "fireside/dsp.h"          // AudioStage

namespace fireside::dsp {
bool spectral_nr_available();
void enable_spectral_nr();                               // registers NR2 with the demodulator (DemodConfig::spectral_nr); safe to call more than once; no-op if not built in
class SpectralNr : public AudioStage {
public:
    explicit SpectralNr(double sample_rate = 48000);     // throws std::runtime_error if not built in
    ~SpectralNr();
    SpectralNr(const SpectralNr&) = delete; SpectralNr& operator=(const SpectralNr&) = delete;
    float process(float x) override;                     // one audio sample in, one out (delayed by delay_s())
    double delay_s() const;
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
}  // namespace fireside::dsp
