// Mic chain: biquads, gate, compressor, look-ahead limiter (hard ceiling, even on hostile input), and the whole voice TX path's I/Q ceiling.
#include <cmath>
#include <limits>
#include <random>
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::tx; using fireside::dsp::kPi;

namespace {
constexpr double kFs = 48000;
std::vector<float> tone(double hz, size_t n, double amp) { std::vector<float> a(n); for (size_t i = 0; i < n; ++i) a[i] = float(amp * std::sin(2 * kPi * hz * double(i) / kFs)); return a; }
double peak(const std::vector<float>& x, size_t from = 0) { double p = 0; for (size_t i = from; i < x.size(); ++i) p = std::max(p, double(std::fabs(x[i]))); return p; }
double peak(const std::vector<cf>& x, size_t from = 0) { double p = 0; for (size_t i = from; i < x.size(); ++i) p = std::max(p, double(std::abs(x[i]))); return p; }
double rms_db(const std::vector<float>& x, size_t from) { double s = 0; for (size_t i = from; i < x.size(); ++i) s += double(x[i]) * x[i]; return 10 * std::log10(s / double(x.size() - from) + 1e-20); }
template <class F> std::vector<float> run(F& f, const std::vector<float>& x) { std::vector<float> y; for (float v : x) y.push_back(f.process(v)); return y; }
// The kind of input a broken mic, a bad driver or a mistake can deliver: loud noise, impulses, full-scale square, steps, huge values, NaN, inf.
std::vector<float> hostile(size_t n, uint32_t seed) {
    std::mt19937 g(seed); std::normal_distribution<float> nd(0, 3.0f); std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t seg = (i / 4800) % 6;
        x[i] = seg == 0 ? nd(g) : seg == 1 ? ((i % 4800) == 100 ? 50.0f : 0.0f) : seg == 2 ? ((i / 40) % 2 ? 1.0f : -1.0f) : seg == 3 ? (i % 4800 < 2400 ? 0.01f : 2.0f) : seg == 4 ? float(1e6 * std::sin(double(i))) : 0.5f;
    }
    x[777] = std::numeric_limits<float>::quiet_NaN(); x[778] = std::numeric_limits<float>::infinity(); x[9000] = -std::numeric_limits<float>::infinity();
    return x;
}
}  // namespace

TEST(tx_biquad_responses) {
    Biquad hp(Biquad::Type::HighPass, kFs, 100); CHECK(hp.response_db(50) < -11); CHECK_NEAR(hp.response_db(1000), 0, 0.1); CHECK_NEAR(hp.response_db(100), -3.01, 0.05);
    Biquad lp(Biquad::Type::LowPass, kFs, 3000); CHECK(lp.response_db(6000) < -11); CHECK_NEAR(lp.response_db(500), 0, 0.1);
    Biquad pk(Biquad::Type::Peak, kFs, 1500, 1.0, 6); CHECK_NEAR(pk.response_db(1500), 6, 0.05); CHECK_NEAR(pk.response_db(100), 0, 0.3);
    Biquad ls(Biquad::Type::LowShelf, kFs, 300, 0.7071, -6); CHECK_NEAR(ls.response_db(30), -6, 0.3); CHECK_NEAR(ls.response_db(5000), 0, 0.3);
    Biquad hs(Biquad::Type::HighShelf, kFs, 2000, 0.7071, 4); CHECK_NEAR(hs.response_db(10000), 4, 0.3); CHECK_NEAR(hs.response_db(100), 0, 0.3);
    // the time-domain filter agrees with the formula
    auto y = run(pk, tone(1500, 48000, 0.1)); CHECK_NEAR(20 * std::log10(peak(y, 24000) / 0.1), 6, 0.1);
    CHECK_THROWS(Biquad(Biquad::Type::LowPass, kFs, 30000), std::invalid_argument); CHECK_THROWS(Biquad(Biquad::Type::Peak, kFs, 1000, 0), std::invalid_argument);
}
TEST(tx_gate_closes_on_quiet_opens_on_speech_without_clicks) {
    NoiseGate g(kFs, GateConfig{});                                        // -50 dBFS threshold, -40 dB range
    auto quiet = tone(1000, 24000, 0.001);                                 // -60 dBFS
    auto y = run(g, quiet); CHECK(!g.open()); CHECK_NEAR(20 * std::log10(peak(y, 12000) / 0.001), -40, 0.5);
    auto loud = tone(1000, 24000, 0.1); float prev = g.gain(); double max_step = 0;
    for (float v : loud) { g.process(v); max_step = std::max(max_step, double(std::fabs(g.gain() - prev))); prev = g.gain(); }
    CHECK(g.open()); CHECK_NEAR(g.gain(), 1.0, 1e-3);
    CHECK(max_step < 0.03);                                                // 1 ms attack: spread over ~50 samples, not a step
    for (int i = 0; i < 4800; ++i) g.process(0.002f * float(std::sin(i * 0.13)));   // -54 dBFS: inside the hysteresis, stays open
    CHECK(g.open());
    for (int i = 0; i < 24000; ++i) g.process(0.0f);                       // silence: closes after the hold, ramps down
    CHECK(!g.open()); CHECK(g.gain() < 0.02);
}
TEST(tx_compressor_static_curve) {
    CompressorConfig c; c.threshold_dbfs = -20; c.ratio = 4;
    for (double in_db : {-30.0, -10.0, 0.0}) {
        Compressor k(kFs, c); const double a = std::pow(10.0, in_db / 20.0) * std::sqrt(2.0);   // sine with this peak-envelope... level is measured on the peak
        auto y = run(k, tone(1000, 48000, a));
        const double in_pk = 20 * std::log10(a), out_pk = 20 * std::log10(peak(y, 24000));
        const double want = in_pk > -20 ? -20 + (in_pk + 20) / 4 : in_pk;
        CHECK_NEAR(out_pk, want, 0.6);                                     // peak detector ripples a little on a sine
    }
    CompressorConfig m = c; m.makeup_db = 6; Compressor k(kFs, m); auto y = run(k, tone(1000, 48000, 0.01)); CHECK_NEAR(20 * std::log10(peak(y, 24000) / 0.01), 6, 0.2);
    CompressorConfig bad; bad.ratio = 0.5; CHECK_THROWS(Compressor(kFs, bad), std::invalid_argument);
}
TEST(tx_limiter_ceiling_holds_on_hostile_input) {
    for (double ceiling : {1.0, 0.891, 0.5}) for (double la : {0.5, 5.0}) {
        LimiterConfig c; c.ceiling = ceiling; c.lookahead_ms = la; PeakLimiter l(kFs, c);
        for (uint32_t seed : {1u, 2u}) {
            auto x = hostile(48000 * 2, seed); std::vector<float> y;
            for (size_t o = 0; o < x.size(); o += 333) l.process(x.data() + o, std::min<size_t>(333, x.size() - o), y);
            CHECK_EQ(y.size(), x.size()); CHECK(peak(y) <= ceiling);
            for (float v : y) CHECK(std::isfinite(v));
        }
        std::vector<cf> xc, yc; std::mt19937 g(9); std::normal_distribution<float> nd(0, 5);
        for (int i = 0; i < 48000; ++i) xc.emplace_back(nd(g), nd(g));
        xc[100] = cf(std::numeric_limits<float>::quiet_NaN(), 0);
        PeakLimiter lc(kFs, c); lc.process(xc.data(), xc.size(), yc); CHECK(peak(yc) <= ceiling);   // complex: the magnitude is bounded, so I and Q are too
    }
}
TEST(tx_limiter_leaves_quiet_audio_alone_and_recovers) {
    LimiterConfig c; PeakLimiter l(kFs, c); auto x = tone(1000, 48000, 0.5); std::vector<float> y; l.process(x.data(), x.size(), y);
    const size_t d = l.latency_samples(); CHECK_EQ(d, size_t(240));
    for (size_t i = d; i < y.size(); ++i) CHECK(std::fabs(y[i] - x[i - d]) < 1e-6);              // below the ceiling: a pure delay
    std::vector<float> burst = tone(1000, 4800, 2.0), after = tone(1000, 48000, 0.5); std::vector<float> z;
    l.process(burst.data(), burst.size(), z); CHECK(peak(z, d) <= c.ceiling); l.process(after.data(), after.size(), z);
    CHECK(peak(z, z.size() - 4800) > 0.49);                                                         // gain recovered after the burst (100 ms release)
    CHECK(peak(z, 4800 + d + 200) < 0.5 + 1e-6);                                                    // and never over-recovers
    LimiterConfig bad; bad.ceiling = 1.01; CHECK_THROWS(PeakLimiter(kFs, bad), std::invalid_argument); bad.ceiling = 0; CHECK_THROWS(PeakLimiter(kFs, bad), std::invalid_argument);
}
TEST(tx_limiter_limits_a_step_without_overshoot) {
    LimiterConfig c; c.ceiling = 0.8; PeakLimiter l(kFs, c); std::vector<float> x(4800, 0.1f); x.resize(48000, 1.5f); std::vector<float> y; l.process(x.data(), x.size(), y);
    CHECK(peak(y) <= 0.8); CHECK_NEAR(y.back(), 0.8, 1e-4);                                         // settles exactly at the ceiling
    for (size_t i = 0; i < 4800; ++i) CHECK(y[i] <= 0.1f + 1e-6);                                    // gain comes down before the step arrives, not after
}
TEST(tx_mic_chain_bounds_and_levels) {
    MicChainConfig c; c.mic_gain_db = 20; c.gate = true; c.clip_level = 0.7; c.eq = {{Biquad::Type::Peak, 1800, 1.0, 4}};
    MicChain m(c); auto x = hostile(48000 * 2, 5); std::vector<float> y; m.process(x.data(), x.size(), y);
    CHECK_EQ(y.size(), x.size()); CHECK(peak(y) <= c.limiter.ceiling); for (float v : y) CHECK(std::isfinite(v));
    // compression evens out levels above the threshold (-20 dBFS, 3:1): 18 dB in comes out as about 6 dB
    MicChainConfig q; MicChain a(q), b(q); auto lo = tone(1000, 48000, 0.1), hi = tone(1000, 48000, 0.8); std::vector<float> ya, yb;
    a.process(lo.data(), lo.size(), ya); b.process(hi.data(), hi.size(), yb);
    const double spread = rms_db(yb, 24000) - rms_db(ya, 24000); CHECK(spread > 4.5 && spread < 7.5);
    MicChainConfig bad; bad.clip_level = 1.5; CHECK_THROWS(MicChain{bad}, std::invalid_argument); bad = MicChainConfig{}; bad.mic_gain_db = 60; CHECK_THROWS(MicChain{bad}, std::invalid_argument);
}
TEST(tx_ssb_filter_overshoots_so_the_iq_limiter_is_needed) {
    // A clipped (square-ish) audio at 0.9 gives an I/Q envelope above 0.9 after the SSB filter: the audio limit alone is not enough.
    std::vector<float> sq(48000); for (size_t i = 0; i < sq.size(); ++i) sq[i] = std::sin(2 * kPi * 400 * double(i) / kFs) > 0 ? 0.9f : -0.9f;
    auto iq = ssb_modulate(sq); CHECK(peak(iq, 4800) > 0.95);
    TxChainConfig t; t.mic.mic_gain_db = 30; t.mic.clip_level = 1.0; t.mic.limiter.ceiling = 1.0; t.iq_limiter.ceiling = 0.891;
    TxChain ch(t); auto x = hostile(48000 * 3, 8); std::vector<cf> y;
    for (size_t o = 0; o < x.size(); o += 1000) ch.process(x.data() + o, std::min<size_t>(1000, x.size() - o), y);
    CHECK_EQ(y.size(), x.size()); CHECK(peak(y) <= 0.891);
    for (auto& v : y) CHECK(std::isfinite(v.real()) && std::isfinite(v.imag()));
    std::vector<cf> y2; TxChain ch2(t); ch2.process(sq.data(), sq.size(), y2); CHECK(peak(y2) <= 0.891); CHECK(peak(y2, 9600) > 0.85);   // still loud: limited, not muted
}

TEST(tx_review_one_huge_sample_does_not_wedge_the_mic_chain) {
    // review fix: 3e38 x mic gain overflowed to inf, poisoned the biquad state with NaN, and the chain stayed silent for good
    MicChainConfig c; c.mic_gain_db = 20; MicChain m(c);
    std::vector<float> x(4800, 0.0f); x[100] = 3e38f; x[200] = -3e38f; auto t = tone(1000, 96000, 0.05); x.insert(x.end(), t.begin(), t.end());
    std::vector<float> y; m.process(x.data(), x.size(), y);
    CHECK(peak(y) <= c.limiter.ceiling); CHECK(peak(y, y.size() - 24000) > 0.05);                     // the tone comes back afterwards
    Biquad b(Biquad::Type::LowPass, kFs, 1000); b.process(3e38f); b.process(3e38f); CHECK(std::isfinite(b.process(0.1f)));
}
TEST(tx_review_settings_are_bounded) {
    MicChainConfig e; e.eq = {{Biquad::Type::Peak, 1000, 1.0, 40}}; CHECK_THROWS(MicChain{e}, std::invalid_argument);
    CompressorConfig k; k.makeup_db = 60; CHECK_THROWS(Compressor(kFs, k), std::invalid_argument);
    LimiterConfig l; l.lookahead_ms = 1e9; CHECK_THROWS(PeakLimiter(kFs, l), std::invalid_argument);
    l = {}; l.release_ms = std::nan(""); CHECK_THROWS(PeakLimiter(kFs, l), std::invalid_argument);
    l = {}; CHECK_THROWS(PeakLimiter(0, l), std::invalid_argument);
}
TEST(tx_review_iq_limiter_splatter_is_negligible_when_it_acts) {
    // Speech-like audio (band-limited noise, syllable envelope) driven +30 dB into the chain. The SSB filter overshoots, the I/Q limiter acts,
    // and its gain changes spread a little energy outside the passband. Measured -92 dBc opposite sideband, -97 above 3.1 kHz, -88 near the carrier.
    std::mt19937 g(3); std::normal_distribution<float> nd(0, 1); std::vector<float> a(48000 * 8);
    Biquad lp(Biquad::Type::LowPass, kFs, 2500), hp(Biquad::Type::HighPass, kFs, 300), pk(Biquad::Type::Peak, kFs, 800, 1.5, 10);
    for (size_t i = 0; i < a.size(); ++i) { const double env = std::pow(std::fabs(std::sin(2 * kPi * 3.3 * double(i) / kFs)), 2.0); a[i] = float(env * pk.process(lp.process(hp.process(nd(g))))) * 0.3f; }
    auto run_chain = [&](double iq_ceiling) { TxChainConfig t; t.mic.mic_gain_db = 30; t.iq_limiter.ceiling = iq_ceiling; TxChain ch(t); std::vector<cf> y; ch.process(a.data(), a.size(), y); return y; };
    const auto unlimited = run_chain(1.0); CHECK(peak(unlimited, 48000) > 0.95);                      // without the I/Q limiter the overshoot reaches full scale
    const auto y = run_chain(0.891); CHECK(peak(y) <= 0.891); CHECK(peak(y, 48000) > 0.89);           // with it: held at the ceiling, and it really acted
    dsp::SpectrumAnalyzer sa(kFs, 8192); std::vector<double> acc(8192, 0.0); size_t fr = 0;
    sa.on_frame = [&](const std::vector<float>& d) { for (size_t i = 0; i < d.size(); ++i) acc[i] += std::pow(10.0, d[i] / 10.0); ++fr; };
    sa.push(y.data() + 48000, y.size() - 48000);
    double inb = 0, opp = 0, above = 0, carrier = 0;
    for (size_t i = 0; i < acc.size(); ++i) { const double hz = sa.bin_hz(i), p = acc[i] / double(fr);
        if (hz >= 150 && hz <= 2800) inb += p; else if (hz <= -150 && hz >= -2800) opp += p; else if (hz > 3100 && hz < 6000) above += p; else if (hz > -100 && hz < 100) carrier += p; }
    CHECK(10 * std::log10(opp / inb) < -70); CHECK(10 * std::log10(above / inb) < -70); CHECK(10 * std::log10(carrier / inb) < -70);
}
TEST(tx_hum_filter_removes_mains_harmonics_and_spares_the_voice_between_them) {
    auto through = [](double hum, double hz) {
        MicChainConfig c; c.highpass_hz = 0; c.lowpass_hz = 0; c.compressor = false; c.limiter.ceiling = 1.0; c.hum_hz = hum;
        MicChain m(c); auto x = tone(hz, 96000, 0.1); std::vector<float> y; m.process(x.data(), x.size(), y);
        return rms_db(y, 48000) - rms_db(x, 48000);
    };
    for (double h : {150.0, 250.0, 350.0, 550.0, 750.0, 1150.0}) CHECK(through(50, h) < -30);            // the buzz lines measured on the air mic (2026-11-26)
    CHECK(through(50, 250.1) < -15); CHECK(through(50, 1150.5) < -10);                                  // the mains drifts ~0.1 Hz: still deep at n x the drift
    for (double h : {275.0, 525.0, 1025.0}) CHECK(std::fabs(through(50, h)) < 1.0);                     // the voice between the lines: untouched
    CHECK(std::fabs(through(50, 2000.0)) < 0.2); CHECK(std::fabs(through(0, 250.0)) < 0.1);             // above the last notch, and off: no effect
    CHECK(through(60, 300) < -30 && std::fabs(through(60, 330)) < 1.0);                                 // 60 Hz mains
    bool threw = false; try { MicChainConfig c; c.hum_hz = 55; MicChain m(c); } catch (const std::invalid_argument&) { threw = true; } CHECK(threw);
}
