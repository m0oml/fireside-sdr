// Voice transmit audio stages added for SSB (PowerSDR-style): 10-band graphic EQ, downward expander, leveler, in the mic chain.
#include <cmath>
#include <complex>
#include <cstdio>
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

// ---------------------------------------------------------------- FM transmit (FmModulator): round trip through our own FM receiver
namespace tx = fireside::tx;
namespace {
using fireside::dsp::Demodulator; using fireside::dsp::DemodConfig;
// Shifts the modulator's output (carrier at +offset) back to 0 Hz and demodulates it with the receiver's FM detector.
std::vector<float> fm_receive(const std::vector<tx::cf>& iq, double offset_hz, double dev, bool deemph = true, double ctcss = 0, double* heard = nullptr) {
    std::vector<fireside::dsp::cf> x(iq.size());
    for (size_t i = 0; i < iq.size(); ++i) x[i] = iq[i] * std::polar(1.0f, float(-2 * M_PI * offset_hz * double(i) / 48000));
    DemodConfig c; c.mode = fireside::dsp::Mode::FM; c.fm_deviation_hz = dev; c.lo_hz = -10000; c.hi_hz = 10000; c.fm_deemphasis = deemph; c.ctcss_hz = ctcss;
    Demodulator d(c); std::vector<float> a; d.process(x.data(), x.size(), a); if (heard) *heard = d.ctcss_heard_hz(); return a;
}
double amp_at(const std::vector<float>& a, size_t from, size_t n, double hz) {
    std::complex<double> s(0, 0); for (size_t i = 0; i < n; ++i) s += double(a[from + i]) * std::polar(1.0, -2 * M_PI * hz * double(from + i) / 48000); return 2 * std::abs(s) / double(n);
}
std::vector<double> inst_freq(const std::vector<tx::cf>& iq) {     // the discriminator, Hz
    std::vector<double> f(iq.size(), 0.0); for (size_t i = 1; i < iq.size(); ++i) f[i] = std::arg(std::complex<double>(iq[i]) * std::conj(std::complex<double>(iq[i - 1]))) * 48000 / (2 * M_PI); return f;
}
std::vector<float> sine(double hz, double amp, size_t n) { std::vector<float> x(n); for (size_t i = 0; i < n; ++i) x[i] = float(amp * std::sin(2 * M_PI * hz * double(i) / 48000)); return x; }
}  // namespace
TEST(fm_tx_round_trip_through_our_receiver_is_flat_400_to_2500_hz) {
    for (double dev : {2500.0, 5000.0}) {
        auto gain = [&](double hz) { tx::FmTxConfig c; c.deviation_hz = dev; tx::FmModulator m(c); auto x = sine(hz, 0.4, 48000); std::vector<tx::cf> iq; m.process(x.data(), x.size(), iq);
                                     auto a = fm_receive(iq, c.offset_hz, dev); return amp_at(a, 24000, 24000, hz) / 0.4; };
        const double g1k = gain(1000); CHECK_NEAR(g1k, 1.0, 0.06);               // +-1 audio = full deviation, back to +-1
        for (double hz : {400.0, 600.0, 1500.0, 2000.0, 2500.0}) { const double db = 20 * std::log10(gain(hz) / g1k); if (std::fabs(db) > 1.0) std::printf("  dev %.0f: %.0f Hz %.2f dB\n", dev, hz, db); CHECK(std::fabs(db) < 1.0); }
    }
}
TEST(fm_tx_peak_deviation_never_exceeds_the_setting_even_20_db_too_hot) {
    for (double dev : {2500.0, 5000.0}) for (double tone : {0.0, 88.5}) {
        tx::FmTxConfig c; c.deviation_hz = dev; c.ctcss_hz = tone; tx::FmModulator m(c);
        std::vector<float> x(96000); for (size_t i = 0; i < x.size(); ++i) { const double t = double(i) / 48000; x[i] = float(10.0 * (std::sin(2 * M_PI * 700 * t) + std::sin(2 * M_PI * 2900 * t) + 0.5 * std::sin(2 * M_PI * 4100 * t))); }
        std::vector<tx::cf> iq; m.process(x.data(), x.size(), iq);
        CHECK(m.peak_deviation_hz() <= dev * 1.0000001);
        double mx = 0; auto f = inst_freq(iq); for (size_t i = 1; i < f.size(); ++i) mx = std::max(mx, std::fabs(f[i] - c.offset_hz)); CHECK(mx <= dev * 1.01);
        float lo = 1, hi = 0; for (auto& z : iq) { lo = std::min(lo, std::abs(z)); hi = std::max(hi, std::abs(z)); } CHECK(hi - lo < 1e-4f);   // constant envelope
    }
}
TEST(fm_tx_hot_speech_stays_inside_the_carson_bandwidth) {
    for (double dev : {2500.0, 5000.0}) {
        tx::FmTxConfig c; c.deviation_hz = dev; c.ctcss_hz = 77.0; tx::FmModulator m(c);
        std::vector<float> x(65536); uint32_t r = 3; for (size_t i = 0; i < x.size(); ++i) { const double t = double(i) / 48000; r = r * 1664525u + 1013904223u;
            x[i] = float(4.0 * (std::sin(2 * M_PI * 450 * t) + std::sin(2 * M_PI * 1300 * t) * std::sin(2 * M_PI * 3 * t)) + 0.5 * (double(r >> 8) / double(1u << 24) - 0.5)); }   // hot, clipping, with noise
        std::vector<tx::cf> iq; m.process(x.data(), x.size(), iq);
        const size_t N = 32768; fireside::dsp::Fft fft(N); std::vector<fireside::dsp::cf> b(N); double in = 0, out = 0;
        for (size_t i = 0; i < N; ++i) b[i] = iq[N + i] * float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(N)));
        fft.forward(b.data());
        const double half = tx::carson_bandwidth_hz(dev) / 2 + 500;
        for (size_t k = 0; k < N; ++k) { double f = double(k) * 48000 / double(N); if (f > 24000) f -= 48000; const double p = std::norm(b[k]); if (std::fabs(f - c.offset_hz) <= half) in += p; else out += p; }
        const double db = 10 * std::log10(out / (in + out)); std::printf("  dev %.0f: %.1f dB outside +-%.0f Hz\n", dev, db, half); CHECK(db < -20);
    }
}
TEST(fm_tx_ctcss_is_decoded_by_our_receiver_and_the_burst_is_1750_hz) {
    for (double tone : {67.0, 88.5, 118.8, 203.5}) {
        tx::FmTxConfig c; c.deviation_hz = 2500; c.ctcss_hz = tone; tx::FmModulator m(c); auto x = sine(1000, 0.3, 72000); std::vector<tx::cf> iq; m.process(x.data(), x.size(), iq);
        double heard = 0; auto a = fm_receive(iq, c.offset_hz, 2500, true, tone, &heard); CHECK_NEAR(heard, tone, 0.05);
        CHECK(amp_at(a, 48000, 24000, 1000) > 0.2);                               // the tone squelch is open: the voice comes through
        std::printf("  tone %.1f Hz: %.4f in the speaker\n", tone, amp_at(a, 48000, 24000, tone)); CHECK(amp_at(a, 48000, 24000, tone) < (tone <= 151.4 ? 0.01 : 0.05));   // the 300 Hz high-pass keeps the tone out of the speaker (all the UK tones); the top tones, near the voice band, at least 18 dB down from the ~0.4 they would be
    }
    tx::FmTxConfig c; c.deviation_hz = 5000; tx::FmModulator m(c); std::vector<float> x(48000, 0.0f); std::vector<tx::cf> iq; m.process(x.data(), x.size(), iq, true);
    auto f = inst_freq(iq); std::vector<float> d(f.size()); for (size_t i = 0; i < f.size(); ++i) d[i] = float((f[i] - c.offset_hz) / 5000);
    CHECK_NEAR(amp_at(d, 4800, 38400, 1750), 0.6, 0.02);                           // 60 % of the deviation, at 1750 Hz
}
TEST(fm_tx_rejects_bad_settings) {
    auto bad = [](auto f) { tx::FmTxConfig c; f(c); try { tx::FmModulator m(c); return false; } catch (const std::invalid_argument&) { return true; } };
    CHECK(bad([](tx::FmTxConfig& c) { c.deviation_hz = 7000; })); CHECK(bad([](tx::FmTxConfig& c) { c.deviation_hz = 500; }));
    CHECK(bad([](tx::FmTxConfig& c) { c.ctcss_hz = 30; })); CHECK(bad([](tx::FmTxConfig& c) { c.offset_hz = 20000; })); CHECK(bad([](tx::FmTxConfig& c) { c.amplitude = 1.5; }));
    CHECK(!bad([](tx::FmTxConfig&) {}));
}
