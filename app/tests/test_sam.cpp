// Synchronous AM (Demodulator with AmDetector::Sync): the loop locks to a mistuned carrier and reports it, the audio is clean, it does not distort when the carrier
// fades more than the sidebands (selective fading, where the envelope detector does), it is never noisier (a little better near threshold), and one-sideband detection drops interference on the
// other side. Measured against the envelope detector on the same signals.
#include <cmath>
#include <complex>
#include <functional>
#include <random>
#include <vector>
#include "fireside/dsp.h"
#include "test_util.h"
using namespace fireside::dsp;

namespace {
constexpr double kFs = 48000, kPi2 = 6.283185307179586;
// AM at 48 kHz complex baseband: (carrier(t) + depth * cos(2 pi 1 kHz t)) at df Hz from the dial; extra(t) adds anything else (interference, noise).
std::vector<cf> am(double seconds, double df, double depth, const std::function<double(double)>& carrier, const std::function<cf(double)>& extra = nullptr, double phase0 = 0.7) {
    std::vector<cf> x(size_t(seconds * kFs));
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = double(i) / kFs, env = 0.1 * (carrier(t) + depth * std::cos(kPi2 * 1000 * t));
        x[i] = std::polar(float(env), float(kPi2 * df * t + phase0)); if (extra) x[i] += extra(t);
    }
    return x;
}
std::vector<float> demod(const std::vector<cf>& x, AmDetector det, AmSideband sb = AmSideband::Both, Demodulator** keep = nullptr) {
    DemodConfig c; c.mode = Mode::AM; c.input_rate = kFs; c.audio_rate = kFs; c.am_detector = det; c.am_sideband = sb; c.agc = AgcMode::Off;
    static std::unique_ptr<Demodulator> last; last = std::make_unique<Demodulator>(c); std::vector<float> a; last->process(x.data(), x.size(), a);
    if (keep) *keep = last.get();
    return a;
}
double level(const std::vector<float>& y, size_t from, double hz) {               // single-bin DFT magnitude
    double re = 0, im = 0; for (size_t i = from; i < y.size(); ++i) { const double p = kPi2 * hz * double(i) / kFs; re += y[i] * std::cos(p); im -= y[i] * std::sin(p); }
    return std::hypot(re, im) * 2.0 / double(y.size() - from);
}
double thd(const std::vector<float>& y, size_t from) { double h = 0; for (int k = 2; k <= 6; ++k) { const double v = level(y, from, 1000.0 * k); h += v * v; } return 100.0 * std::sqrt(h) / level(y, from, 1000); }
}  // namespace

TEST(sam_locks_to_a_mistuned_carrier_and_reports_where_it_is) {
    for (double df : {60.0, -150.0, 0.0, 190.0}) {
        Demodulator* d = nullptr; demod(am(1.0, df, 0.5, [](double) { return 1.0; }), AmDetector::Sync, AmSideband::Both, &d);
        CHECK(d->am_locked()); CHECK_NEAR(d->am_carrier_hz(), df, 1.0);
    }
}
TEST(sam_clean_audio_matches_the_envelope_detector) {
    const auto x = am(2.0, 35.0, 0.8, [](double) { return 1.0; });
    const auto s = demod(x, AmDetector::Sync), e = demod(x, AmDetector::Envelope); const size_t from = size_t(kFs);
    CHECK(thd(s, from) < 1.5); CHECK(thd(e, from) < 1.5);
    CHECK_NEAR(20 * std::log10(level(s, from, 1000) / level(e, from, 1000)), 0.0, 1.0);                    // same loudness
}
TEST(sam_does_not_distort_in_selective_fading_where_the_envelope_detector_does) {
    // The carrier fades to 10 % twice a second while the sidebands stay (the classic medium-wave night fade): the envelope "overmodulates", synchronous AM does not.
    const auto x = am(4.0, 20.0, 0.5, [](double t) { return 0.55 + 0.45 * std::cos(kPi2 * 0.5 * t); });
    const auto s = demod(x, AmDetector::Sync), e = demod(x, AmDetector::Envelope); const size_t from = size_t(kFs);
    const double ts = thd(s, from), te = thd(e, from);
    CHECK(ts < 3.0); CHECK(te > 3.0 * ts);
}
TEST(sam_is_never_noisier_than_the_envelope_detector_and_better_near_its_threshold) {
    // Measured: above the envelope detector's threshold both give the same audio SNR (theory says so too); near a 0 dB carrier-to-noise ratio (8 kHz channel) the
    // envelope detector's threshold effect starts and synchronous AM is ~1.5 dB better. Its real gains are fading and one-sideband reception (tests above/below).
    auto snr = [](const std::vector<float>& y) {
        const size_t from = size_t(kFs); const double sig = level(y, from, 1000); double noise = 0; int n = 0;
        for (double f = 300; f <= 3000; f += 37) if (std::fabs(f - 1000) > 60 && std::fabs(f - 2000) > 60 && std::fabs(f - 3000) > 60) { const double v = level(y, from, f); noise += v * v; ++n; }
        return 20 * std::log10(sig / std::sqrt(noise / n));
    };
    for (float sd : {0.04f, 0.2f}) {                                                    // CNR about +13 dB and -1 dB
        std::mt19937 rng(3); std::normal_distribution<float> g(0.0f, sd);
        const auto x = am(4.0, 40.0, 0.5, [](double) { return 1.0; }, [&](double) { return cf(g(rng), g(rng)); });
        const double s = snr(demod(x, AmDetector::Sync)), e = snr(demod(x, AmDetector::Envelope));
        CHECK(s >= e - 0.3); if (sd > 0.1f) CHECK(s >= e + 1.0);
    }
}
TEST(sam_one_sideband_drops_interference_on_the_other_side) {
    // A whistle 2.5 kHz above the carrier (an adjacent station): lower-sideband detection must remove it and keep the programme; upper keeps it.
    const auto x = am(2.0, 15.0, 0.5, [](double) { return 1.0; }, [](double t) { return std::polar(0.02f, float(kPi2 * (15.0 + 2500.0) * t)); });
    const size_t from = size_t(kFs);
    const auto both = demod(x, AmDetector::Sync, AmSideband::Both), lower = demod(x, AmDetector::Sync, AmSideband::Lower), upper = demod(x, AmDetector::Sync, AmSideband::Upper);
    const double w_both = level(both, from, 2500), w_low = level(lower, from, 2500), w_up = level(upper, from, 2500);
    CHECK(20 * std::log10(w_both / w_low) >= 30.0);                                    // whistle gone from the lower sideband
    CHECK(20 * std::log10(w_up / w_both) >= 3.0);                                      // and all of it in the upper (twice the amplitude of the both-sidebands mix)
    CHECK_NEAR(20 * std::log10(level(lower, from, 1000) / level(both, from, 1000)), 0.0, 1.0);   // the programme stays at the same level
    CHECK(thd(lower, from) < 2.0);
}
TEST(sam_without_a_carrier_stays_quiet_and_unlocked) {
    std::mt19937 rng(9); std::normal_distribution<float> g(0.0f, 0.05f); std::vector<cf> x(static_cast<size_t>(kFs)); for (auto& v : x) v = cf(g(rng), g(rng));
    Demodulator* d = nullptr; const auto a = demod(x, AmDetector::Sync, AmSideband::Both, &d);
    CHECK(!d->am_locked()); double peak = 0; for (float v : a) peak = std::max(peak, double(std::fabs(v))); CHECK(peak < 1.0); CHECK(std::isfinite(d->am_carrier_hz()));
}
