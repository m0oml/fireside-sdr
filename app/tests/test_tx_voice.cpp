// Voice transmit audio stages added for SSB (PowerSDR-style): 10-band graphic EQ, downward expander, leveler, in the mic chain.
#include <cmath>
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside::tx;
namespace {
std::vector<float> tone(double hz, double amp, double s, double fs = 48000) { std::vector<float> v(size_t(s * fs)); for (size_t i = 0; i < v.size(); ++i) v[i] = float(amp * std::sin(2 * M_PI * hz * double(i) / fs)); return v; }
double rms_db(const std::vector<float>& v, size_t from, size_t to) { double s = 0; for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i]; return 10 * std::log10(s / double(to - from) + 1e-30); }
double through(const std::vector<EqBand>& eq, double hz) { MicChainConfig c; c.highpass_hz = 0; c.lowpass_hz = 0; c.compressor = false; c.eq = eq; c.limiter.ceiling = 1.0;
    MicChain m(c); auto x = tone(hz, 0.1, 1.0); std::vector<float> y; m.process(x.data(), x.size(), y); return rms_db(y, 24000, 48000) - rms_db(x, 24000, 48000); }
}  // namespace

TEST(tx_voice_graphic_eq10_boosts_and_cuts_its_bands) {
    double g[10] = {0, 0, 0, -10, 0, 0, +8, 0, 0, 0};
    const auto eq = graphic_eq10(g); CHECK_EQ(eq.size(), size_t(2));                  // flat bands are skipped
    CHECK_NEAR(through(eq, 250), -10, 1.0); CHECK_NEAR(through(eq, 2000), 8, 1.0); CHECK_NEAR(through(eq, 1000), 0, 2.5);   // the neighbours barely move
    double flat[10] = {}; CHECK(graphic_eq10(flat).empty());
    double bad[10] = {0, 0, 0, 0, 0, 20, 0, 0, 0, 0}; CHECK_THROWS(graphic_eq10(bad), std::invalid_argument);
}
TEST(tx_voice_downward_expander_drops_quiet_noise_and_leaves_speech_alone) {
    ExpanderConfig c; c.threshold_dbfs = -40; c.ratio = 3; c.range_db = -30; DownwardExpander e(48000, c);
    auto loud = tone(1000, 0.1, 1.0), quiet = tone(1000, 0.001, 2.0);                  // -23 dBFS speech, -63 dBFS room noise
    std::vector<float> y; for (float v : loud) y.push_back(e.process(v)); CHECK_NEAR(rms_db(y, 24000, 48000), rms_db(loud, 24000, 48000), 0.3);
    y.clear(); for (float v : quiet) y.push_back(e.process(v));
    CHECK(rms_db(y, 48000, 96000) < rms_db(quiet, 48000, 96000) - 25);                // ~23 dB under threshold x (3 - 1) = -46 dB, limited to the 30 dB range
    CHECK(e.gain_db() >= -30.5);
    CHECK_THROWS(DownwardExpander(48000, ExpanderConfig{-40, 0.5}), std::invalid_argument);
}
TEST(tx_voice_leveler_brings_soft_and_loud_voices_to_the_target) {
    for (double amp : {0.02, 0.3}) {                                                    // -37 and -13 dBFS in, target -16 (max +15 dB of gain)
        LevelerConfig c; c.target_dbfs = -16; c.max_gain_db = 15; Leveler l(48000, c);
        auto x = tone(500, amp, 6.0); std::vector<float> y; for (float v : x) y.push_back(l.process(v));
        const double out = rms_db(y, 5 * 48000, 6 * 48000) + 3.0;                     // rms + 3 dB = the sine's peak level
        if (amp > 0.1) CHECK_NEAR(out, -16, 1.5); else CHECK_NEAR(out, 20 * std::log10(amp) + 15, 1.5);   // the soft one: limited to +15 dB
    }
    Leveler l(48000, LevelerConfig{}); for (int i = 0; i < 48000; ++i) l.process(0.0f); CHECK_NEAR(l.gain_db(), 0, 1e-9);   // silence: gain held
}
TEST(tx_voice_full_chain_with_every_stage_stays_under_the_ceiling) {
    TxChainConfig c; double g[10] = {0, -6, -3, 0, 2, 3, 4, 2, 0, 0}; c.mic.eq = graphic_eq10(g);
    c.mic.expander = true; c.mic.leveler = true; c.mic.compressor = true; c.mic.clip_level = 0.5; c.iq_limiter.ceiling = 0.7;
    TxChain t(c); std::vector<float> a; for (double amp : {0.001, 0.05, 0.9}) { auto x = tone(800, amp, 1.0); a.insert(a.end(), x.begin(), x.end()); }
    std::vector<cf> iq; t.process(a.data(), a.size(), iq);
    double pk = 0; for (auto& z : iq) pk = std::max(pk, double(std::abs(z))); CHECK(pk <= 0.7 + 1e-6); CHECK(pk > 0.3);
}
#include <thread>
#include "fireside/audio_fifo.h"
TEST(tx_voice_drift_fifo_holds_its_fill_against_a_fast_and_a_slow_clock) {   // the PC's sound card vs the radio's clock
    for (double ppm : {+800.0, -800.0}) {
        fireside::DriftFifo f(48000, 0.04, 0.02, 0.5);
        const double in_rate = 48000 * (1 + ppm * 1e-6); double acc = 0; std::vector<float> blk(480), out(480); float v = 0;
        for (int b = 0; b < 3000; ++b) {                                              // 30 s of 10 ms blocks, simulated
            acc += in_rate * 0.01; const size_t n = size_t(acc); acc -= double(n);
            std::vector<float> in(n); for (auto& s : in) { s = v; v += 1e-5f; } f.push(in.data(), n);
            f.read(out.data(), 480, 0);
        }
        CHECK(f.fill() > 48000 * 0.015 && f.fill() < 48000 * 0.065);                // held near 40 ms
        if (ppm > 0) CHECK(f.dropped() > 0); else CHECK(f.repeated() > 0);
    }
    fireside::DriftFifo g; std::vector<float> o(100); CHECK_EQ(g.read(o.data(), 100, 0.01), size_t(0));   // nothing yet: waits for the target
}
