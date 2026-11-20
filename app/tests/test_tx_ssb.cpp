// SSB modulator: sideband placement, suppression, passband edges, round trip through the receive Demodulator.
#include <cmath>
#include "fireside/dsp.h"
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::tx; using fireside::dsp::kPi;

namespace {
constexpr double kFs = 48000;
std::vector<float> tone(double hz, size_t n, double amp = 0.5) { std::vector<float> a(n); for (size_t i = 0; i < n; ++i) a[i] = float(amp * std::sin(2 * kPi * hz * double(i) / kFs)); return a; }
double amp_at(const std::vector<cf>& x, size_t from, size_t n, double hz) {
    std::complex<double> s(0, 0); for (size_t i = 0; i < n; ++i) s += std::complex<double>(x[from + i]) * std::polar(1.0, -2 * kPi * hz * double(from + i) / kFs); return std::abs(s) / double(n);
}
double db(double r) { return 20 * std::log10(r + 1e-12); }
SsbConfig cfg(Sideband sb) { SsbConfig c; c.sideband = sb; return c; }
}  // namespace

TEST(ssb_usb_tone_lands_above_the_dial_with_unity_gain) {
    auto y = ssb_modulate(tone(1000, 48000), cfg(Sideband::USB));
    CHECK_EQ(y.size(), size_t(48000));
    CHECK_NEAR(amp_at(y, 8000, 32768, 1000), 0.5, 0.005);
    CHECK(db(amp_at(y, 8000, 32768, -1000) / 0.5) < -50);            // opposite sideband
}
TEST(ssb_lsb_tone_lands_below_the_dial) {
    auto y = ssb_modulate(tone(1000, 48000), cfg(Sideband::LSB));
    CHECK_NEAR(amp_at(y, 8000, 32768, -1000), 0.5, 0.005);
    CHECK(db(amp_at(y, 8000, 32768, 1000) / 0.5) < -50);
}
TEST(ssb_has_no_carrier_and_rejects_out_of_band) {
    for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
        const double s = sb == Sideband::USB ? 1.0 : -1.0;
        auto lo = ssb_modulate(tone(40, 48000), cfg(sb));              // below the passband, near the carrier
        CHECK(db(amp_at(lo, 8000, 32768, s * 40) / 0.5) < -50);
        CHECK(db(amp_at(lo, 8000, 32768, 0) / 0.5) < -50);
        auto hi = ssb_modulate(tone(4500, 48000), cfg(sb));            // above the passband
        CHECK(db(amp_at(hi, 8000, 32768, s * 4500) / 0.5) < -50);
    }
}
TEST(ssb_passband_is_flat_across_the_voice_band) {
    for (double f : {300.0, 700.0, 1500.0, 2400.0, 2700.0}) {
        auto y = ssb_modulate(tone(f, 48000), cfg(Sideband::USB));
        CHECK_NEAR(amp_at(y, 8000, 32768, f), 0.5, 0.01);
    }
}
TEST(ssb_streaming_matches_one_shot_and_reports_latency) {
    auto a = tone(1500, 20000); SsbModulator m(cfg(Sideband::USB)); std::vector<cf> y;
    for (size_t o = 0; o < a.size(); o += 777) m.process(a.data() + o, std::min<size_t>(777, a.size() - o), y);
    SsbModulator m2(cfg(Sideband::USB)); std::vector<cf> z; m2.process(a.data(), a.size(), z);
    CHECK_EQ(y.size(), z.size()); for (size_t i = 0; i < y.size(); ++i) CHECK(std::abs(y[i] - z[i]) < 1e-5);
    CHECK(m.latency_samples() > 100);
}
TEST(ssb_rejects_bad_configuration) {
    SsbConfig c; c.lo_hz = 3000; c.hi_hz = 2000; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);
    c = SsbConfig{}; c.hi_hz = 30000; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);
    c = SsbConfig{}; c.edge_hz = 0; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);
}
TEST(ssb_round_trips_through_the_receive_demodulator) {
    for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
        auto a = tone(1200, 48000, 0.4);
        auto iq = ssb_modulate(a, cfg(sb));
        dsp::DemodConfig d; d.input_rate = kFs; d.audio_rate = kFs; d.mode = sb == Sideband::USB ? dsp::Mode::USB : dsp::Mode::LSB; d.agc = dsp::AgcMode::Off;
        dsp::Demodulator dm(d); std::vector<float> out; dm.process(iq.data(), iq.size(), out);
        // audio comes back at 1200 Hz with the original amplitude (the demodulator's passband is flat to within its own tolerance)
        std::vector<cf> o(out.size()); for (size_t i = 0; i < out.size(); ++i) o[i] = cf(out[i], 0);
        CHECK_NEAR(amp_at(o, 12000, 24000, 1200) * 2, 0.4, 0.02);
    }
}

TEST(ssb_review_carrier_is_always_in_the_stopband) {
    SsbConfig c; c.lo_hz = 0; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);                 // review fix: lo below the edge width is refused
    c = SsbConfig{}; c.lo_hz = 50; c.edge_hz = 100; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);
    c = SsbConfig{}; c.edge_hz = 1; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);                // would build a filter of millions of taps
    c = SsbConfig{}; c.sample_rate = 0; CHECK_THROWS(SsbModulator{c}, std::invalid_argument);
    for (Sideband sb : {Sideband::USB, Sideband::LSB}) {                                                // a DC offset on the audio does not become a carrier
        std::vector<float> dc(48000, 0.5f); auto y = ssb_modulate(dc, cfg(sb));
        double pk = 0; for (size_t i = 12000; i < 36000; ++i) pk = std::max(pk, double(std::abs(y[i]))); CHECK(db(pk / 0.5) < -60);
    }
}
