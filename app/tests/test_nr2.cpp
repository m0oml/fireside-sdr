// NR2 (WDSP EMNR, fireside/nr2.h): reduces noise, keeps voice-like bursts, is safe to create / destroy from several threads, and the demodulator
// option leaves everything else alone. Skipped when NR2 is not built in (no libfftw3).
#include <thread>
#include "fireside/dsp.h"
#include "fireside/nr2.h"
#include "test_util.h"
using namespace fireside::dsp;

namespace {
struct Lcg { uint64_t s = 12345; double gauss() { double a = 0; for (int i = 0; i < 12; ++i) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; a += double(s >> 11) / 9007199254740992.0; } return a - 6.0; } };
// a voice-like signal: a 150 Hz harmonic series to 2.4 kHz with a falling spectrum, on for 0.6 s, off for 0.6 s
std::vector<float> noisy_bursts(double fs, double seconds, double noise_rms, std::vector<bool>* on) {
    std::vector<float> x(size_t(fs * seconds)); Lcg g; on->assign(x.size(), false);
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = double(i) / fs; const bool burst = std::fmod(t, 1.2) < 0.6; (*on)[i] = burst; double v = 0;
        if (burst) for (int h = 1; h <= 16; ++h) v += std::sin(2 * M_PI * 150.0 * h * t + h) / (1.0 + 0.4 * h);
        x[i] = float(0.12 * v + noise_rms * g.gauss());
    }
    return x;
}
double rms(const std::vector<float>& x, size_t a, size_t b) { double e = 0; for (size_t i = a; i < b; ++i) e += double(x[i]) * x[i]; return std::sqrt(e / double(std::max<size_t>(1, b - a))); }
}  // namespace

TEST(nr2_turns_noise_down_and_keeps_voice_like_bursts) {
    if (!spectral_nr_available()) return;
    const double fs = 48000; std::vector<bool> on; auto x = noisy_bursts(fs, 24.0, 0.05, &on);
    SpectralNr nr(fs); std::vector<float> y(x.size()); for (size_t i = 0; i < x.size(); ++i) y[i] = nr.process(x[i]);
    const double d = nr.delay_s(); CHECK(d > 0.03 && d < 0.2);
    double in_noise = 0, out_noise = 0, in_sig = 0, out_sig = 0; int nn = 0, ns = 0;
    for (double t0 = 12.0; t0 + 1.2 <= 23.0; t0 += 1.2) {                      // after the noise estimate has settled; 0.6 s on, 0.6 s off, the output delayed by d
        const size_t a = size_t((t0 + 0.12) * fs), b = size_t((t0 + 0.55) * fs), c = size_t((t0 + 0.72) * fs), e = size_t((t0 + 1.15) * fs), sh = size_t(d * fs);
        in_sig += rms(x, a, b); out_sig += rms(y, a + sh, b + sh); ++ns; in_noise += rms(x, c, e); out_noise += rms(y, c + sh, e + sh); ++nn;
    }
    in_sig /= ns; out_sig /= ns; in_noise /= nn; out_noise /= nn;
    const double noise_db = 20 * std::log10(out_noise / in_noise), sig_db = 20 * std::log10(out_sig / in_sig);
    CHECK(noise_db < -6.0);                                                   // the pauses get at least 6 dB quieter
    CHECK(sig_db > -4.0);                                                     // the speech-like bursts keep (nearly) their level
    CHECK(sig_db - noise_db > 6.0);
}
TEST(nr2_silence_in_silence_out_and_never_nan) {
    if (!spectral_nr_available()) return;
    SpectralNr nr(48000); for (int i = 0; i < 48000; ++i) CHECK(nr.process(0.0f) == 0.0f);
    Lcg g; for (int i = 0; i < 96000; ++i) { const float y = nr.process(float(0.3 * g.gauss())); CHECK(std::isfinite(y)); CHECK(std::fabs(y) < 4.0f); }
    for (int i = 0; i < 20000; ++i) { const float y = nr.process(i % 2 ? 1.0f : -1.0f); CHECK(std::isfinite(y)); }   // full-scale Nyquist: must not blow up
}
TEST(nr2_instances_come_and_go_from_several_threads) {
    if (!spectral_nr_available()) return;
    std::vector<std::thread> th; std::vector<int> ok(4, 0);
    for (int t = 0; t < 4; ++t) th.emplace_back([&ok, t] { for (int k = 0; k < 8; ++k) { SpectralNr nr(48000); Lcg g; g.s += uint64_t(t * 7 + k); float last = 0; for (int i = 0; i < 6000; ++i) last = nr.process(float(0.1 * g.gauss())); if (std::isfinite(last)) ++ok[size_t(t)]; } });
    for (auto& x : th) x.join();
    for (int t = 0; t < 4; ++t) CHECK_EQ(ok[size_t(t)], 8);
}
TEST(nr2_in_the_demodulator_is_an_option_that_changes_only_the_audio) {
    enable_spectral_nr();
    const double fs = 48000; auto iq = synth_tones(fs, size_t(fs * 3), {{1000, 0.2}}, 0.05, 3);
    DemodConfig a; a.mode = Mode::USB; a.input_rate = fs; a.audio_rate = fs; DemodConfig b = a; b.spectral_nr = true;
    std::vector<float> ya, yb, yz; { Demodulator d(a); d.process(iq.data(), iq.size(), ya); }
    { Demodulator d(b); d.process(iq.data(), iq.size(), yb); }
    CHECK_EQ(ya.size(), yb.size());
    if (spectral_nr_available()) { double diff = 0; for (size_t i = 0; i < ya.size(); ++i) diff += std::fabs(double(ya[i]) - yb[i]); CHECK(diff > 1.0); }
    else CHECK(ya == yb);
    DemodConfig c = b; c.mode = Mode::CW; DemodConfig cz = a; cz.mode = Mode::CW; std::vector<float> yc, ycz; { Demodulator d(c); d.process(iq.data(), iq.size(), yc); } { Demodulator d(cz); d.process(iq.data(), iq.size(), ycz); }
    CHECK(yc == ycz);                                                          // CW: ignored (it would smear the tone)
}
