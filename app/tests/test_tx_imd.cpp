// Two-tone generator and IMD measurement: the measurement against closed-form distortion, then the stated figures for the transmit chain.
#include <cmath>
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::tx; using fireside::dsp::kPi;

namespace {
constexpr size_t kN = 96000;   // 2 s
std::vector<cf> through(TxChainConfig t, const std::vector<float>& a) { TxChain ch(t); std::vector<cf> y; for (size_t o = 0; o < a.size(); o += 480) ch.process(a.data() + o, std::min<size_t>(480, a.size() - o), y); return y; }
}  // namespace

TEST(two_tone_generator_levels_and_frequencies) {
    auto a = two_tone_audio(kN, 0.8); CHECK_EQ(a.size(), kN);
    double pk = 0; for (float v : a) pk = std::max(pk, double(std::fabs(v))); CHECK_NEAR(pk, 0.8, 0.002);   // the SUM peaks at `peak` when the tones align
    std::vector<cf> x(a.size()); for (size_t i = 0; i < a.size(); ++i) x[i] = cf(a[i], 0);
    auto amp = [&](double hz) { std::complex<double> s(0, 0); for (size_t i = 0; i < x.size(); ++i) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * kPi * hz * double(i) / 48000); return std::abs(s) / double(x.size()) * 2; };
    CHECK_NEAR(amp(600), 0.4, 1e-3); CHECK_NEAR(amp(1500), 0.4, 1e-3); CHECK(amp(900) < 1e-3);
    CHECK_THROWS(two_tone_audio(100, 1.5), std::invalid_argument); CHECK_THROWS(two_tone_audio(100, 0.5, 1900, 700), std::invalid_argument); CHECK_THROWS(two_tone_audio(100, 0.5, 700, 30000), std::invalid_argument);
}
TEST(imd_measurement_matches_a_known_cubic_distortion) {
    // y = x + a x^3 on two tones of amplitude A: IM3 = (3/4) a A^3 / (A + (9/4) a A^3)
    for (double a : {0.05, 0.2, 0.5}) {
        const double A = 0.25; auto x = two_tone_audio(kN, 2 * A); for (auto& v : x) v += float(a) * v * v * v;
        const double want = 20 * std::log10(0.75 * a * A * A * A / (A + 2.25 * a * A * A * A));
        for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
            SsbConfig c; c.sideband = sb; const auto r = measure_imd(ssb_modulate(x, c), sb);
            CHECK_NEAR(r.im3_low_dbc, want, 0.3); CHECK_NEAR(r.im3_high_dbc, want, 0.3); CHECK_NEAR(r.im3_dbc(), want, 0.3);
            CHECK_NEAR(r.tone1_db, r.tone2_db, 0.05);
        }
    }
}
TEST(imd_products_outside_the_passband_are_not_counted) {
    auto r = measure_imd(ssb_modulate(two_tone_audio(kN, 0.5)), Sideband::USB);
    CHECK(!std::isnan(r.im3_low_dbc) && !std::isnan(r.im3_high_dbc) && !std::isnan(r.im5_low_dbc));     // 300, 2400 and 1200 Hz are transmitted
    CHECK(std::isnan(r.im5_high_dbc));                                                                  // 3300 Hz is not
    auto q = measure_imd(ssb_modulate(two_tone_audio(kN, 0.5, 700, 1900)), Sideband::USB, 700, 1900);               // 2f2-f1 = 3100 Hz
    CHECK(std::isnan(q.im3_high_dbc)); CHECK(!std::isnan(q.im3_dbc()));                                  // im3_dbc() ignores it
    CHECK_THROWS(measure_imd(std::vector<cf>(5000), Sideband::USB), std::invalid_argument);
}
TEST(the_linear_chain_adds_no_measurable_intermodulation) {
    TxChainConfig t; t.mic.compressor = false;                                                           // filters + SSB + (idle) limiters
    for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
        t.ssb.sideband = sb;
        for (double peak : {0.1, 0.5, 0.88}) { const auto r = measure_imd(through(t, two_tone_audio(kN, peak)), sb); CHECK(r.im3_dbc() < -100); CHECK(r.im5_dbc() < -100); }   // measured about -135 dBc or better
    }
}
TEST(the_default_chain_with_its_compressor_stays_below_minus_55_dbc) {
    TxChainConfig t;                                                                                     // compressor on: its slow gain ripple is the only distortion source
    for (Sideband sb : {Sideband::USB, Sideband::LSB}) for (double peak : {0.3, 0.9}) {
        t.ssb.sideband = sb; const auto r = measure_imd(through(t, two_tone_audio(kN, peak)), sb);
        CHECK(r.im3_dbc() < -55); CHECK(r.im5_dbc() < -65);                                              // measured -63 and -72 dBc
    }
}
TEST(the_limiter_adds_no_intermodulation_on_a_steady_two_tone_even_when_overdriven) {
    // The look-ahead window (5 ms) spans many beat periods (1.1 ms), so the gain is constant for this signal. Speech is different: this
    // is a statement about the test signal, not a promise about voice. The ceiling is what protects the radio, and test_tx_mic checks it.
    TxChainConfig t; t.mic.compressor = false; t.mic.limiter.ceiling = 1.0; t.iq_limiter.ceiling = 0.891; t.mic.mic_gain_db = 9.5;
    const auto y = through(t, two_tone_audio(kN, 0.9)); double pk = 0; for (auto& v : y) pk = std::max(pk, double(std::abs(v)));
    CHECK(pk <= 0.891); const auto r = measure_imd(y, Sideband::USB); CHECK(r.im3_dbc() < -90);
}
TEST(the_measurement_does_see_a_clipper) {
    TxChainConfig t; t.mic.compressor = false; t.mic.clip_level = 0.5;                                  // clips the 0.9-peak two-tone hard
    const auto r = measure_imd(through(t, two_tone_audio(kN, 0.9)), Sideband::USB);
    CHECK(r.im3_dbc() > -30); CHECK(r.im3_dbc() < 0);                                                    // measured -17 dBc
}
