// CW transmit: shaped keying (envelope, timing, spectrum vs hard keying), round trip through the CW demodulator and decoder, iambic keyer timing and modes.
#include <cmath>
#include "fireside/decode.h"
#include "fireside/dsp.h"
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::tx; using fireside::dsp::kPi;

namespace {
constexpr double kFs = 48000;
// max-hold spectrum relative to its peak: the worst level found at or beyond `off` Hz from the carrier
double worst_beyond(const std::vector<cf>& iq, double off) {
    dsp::SpectrumAnalyzer sa(kFs, 8192); std::vector<float> mx(8192, -300.0f);
    sa.on_frame = [&](const std::vector<float>& d) { for (size_t i = 0; i < d.size(); ++i) mx[i] = std::max(mx[i], d[i]); };
    sa.push(iq.data(), iq.size()); float pk = -300; for (float v : mx) pk = std::max(pk, v);
    float m = -300; for (size_t i = 0; i < mx.size(); ++i) if (std::fabs(sa.bin_hz(i)) >= off) m = std::max(m, mx[i]); return double(m - pk);
}
std::vector<cf> shape(const std::vector<uint8_t>& k, CwShapeConfig c = {}) { CwShaper s(c); std::vector<cf> y; s.process(k.data(), k.size(), y); return y; }
// runs of key-down / key-up as (state, length) pairs
std::vector<std::pair<bool, size_t>> runs(const std::vector<uint8_t>& k) { std::vector<std::pair<bool, size_t>> r; for (uint8_t v : k) { if (r.empty() || r.back().first != bool(v)) r.push_back({bool(v), 0}); ++r.back().second; } return r; }
std::vector<uint8_t> keyer(IambicKeyer& kr, const std::vector<uint8_t>& paddles) { std::vector<uint8_t> k; kr.process(paddles.data(), paddles.size(), k); return k; }
std::vector<uint8_t> hold(uint8_t p, size_t n) { return std::vector<uint8_t>(n, p); }
std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
constexpr size_t kUnit20 = 2880;   // 1.2 / 20 wpm * 48 kHz
}  // namespace

TEST(cw_shaper_envelope_is_smooth_and_keeps_the_keyed_length) {
    std::vector<uint8_t> k(1000, 0); k.resize(1000 + 4800, 1); k.resize(1000 + 4800 + 2000, 0);   // 100 ms key-down
    CwShapeConfig c; c.amplitude = 0.8; auto y = shape(k, c);
    double max_step = 0; for (size_t i = 1; i < y.size(); ++i) max_step = std::max(max_step, double(std::fabs(std::abs(y[i]) - std::abs(y[i - 1]))));
    CHECK(max_step < 0.8 * kPi / 2 / 240 * 1.01);                                                       // raised cosine over 5 ms = 240 samples
    CHECK_NEAR(std::abs(y[1000 + 2400]), 0.8, 1e-5);                                                    // full amplitude in the middle
    size_t first = 0, last = 0; for (size_t i = 0; i < y.size(); ++i) if (std::abs(y[i]) >= 0.4) { if (!first) first = i; last = i; }
    CHECK(std::labs(long(last - first + 1) - 4800) <= 2);                                               // half-amplitude width = keyed length
    CHECK(std::labs(long(first) - (1000 + 120)) <= 2);                                                  // shifted by edge/2
    CHECK(std::abs(y.back()) == 0.0f && std::abs(y[999]) == 0.0f);                                      // silent when key up
}
TEST(cw_shaper_short_tap_never_steps) {
    std::vector<uint8_t> k(500, 0); k.resize(530, 1); k.resize(1500, 0);                                // 30 samples: shorter than the edge
    auto y = shape(k); double max_step = 0, pk = 0;
    for (size_t i = 1; i < y.size(); ++i) { max_step = std::max(max_step, double(std::fabs(std::abs(y[i]) - std::abs(y[i - 1])))); pk = std::max(pk, double(std::abs(y[i]))); }
    CHECK(max_step < kPi / 2 / 240 * 1.01); CHECK(pk < 0.05); CHECK(std::abs(y.back()) == 0.0f);
}
TEST(cw_shaper_carrier_offset_and_bad_settings) {
    std::vector<uint8_t> k(48000, 1); CwShapeConfig c; c.offset_hz = 600; auto y = shape(k, c);
    std::complex<double> s(0, 0); for (size_t i = 4800; i < y.size(); ++i) s += std::complex<double>(y[i]) * std::polar(1.0, -2 * kPi * 600 * double(i) / kFs);
    CHECK_NEAR(std::abs(s) / double(y.size() - 4800), 1.0, 1e-3);
    c = {}; c.edge_ms = 0.5; CHECK_THROWS(CwShaper{c}, std::invalid_argument); c = {}; c.amplitude = 1.2; CHECK_THROWS(CwShaper{c}, std::invalid_argument);
    c = {}; c.offset_hz = 5000; CHECK_THROWS(CwShaper{c}, std::invalid_argument); CHECK_THROWS(text_keying("E", 100), std::invalid_argument);
}
TEST(cw_shaped_keying_has_no_key_clicks) {
    const auto k = text_keying("EEEEE TTTTT PARIS PARIS", 30, kFs, 0.2);
    const auto shaped = shape(k);                                                                       // 5 ms, measured -62 dBc beyond 300 Hz, -88 beyond 1 kHz
    CHECK(worst_beyond(shaped, 300) < -55); CHECK(worst_beyond(shaped, 1000) < -80); CHECK(worst_beyond(shaped, 2000) < -95);
    std::vector<cf> hard; for (auto v : k) hard.emplace_back(v ? 1.0f : 0.0f, 0.0f);                    // the same measurement does see clicks on hard keying
    CHECK(worst_beyond(hard, 1000) > -55); CHECK(worst_beyond(hard, 2000) > -60);
}
TEST(cw_round_trips_through_the_receive_cw_mode) {
    const std::string t = "CQ DE M0OML";
    for (double wpm : {15.0, 25.0}) {
        const auto iq = shape(text_keying("VVV " + t + " ", wpm, kFs, 2.0));                            // carrier on the dial: the receiver makes the 700 Hz note
        dsp::DemodConfig d; d.input_rate = kFs; d.audio_rate = kFs; d.mode = dsp::Mode::CW; d.agc = dsp::AgcMode::Off; d.cw_pitch_hz = 700;
        dsp::Demodulator dm(d); std::vector<float> a; dm.process(iq.data(), iq.size(), a);
        decode::CwDecoder dec; std::string out; for (size_t i = 0; i < a.size(); i += 4096) dec.process(a.data() + i, std::min<size_t>(4096, a.size() - i), out);
        CHECK(out.find(t) != std::string::npos);
    }
}
TEST(keyer_holding_one_paddle_repeats_at_the_right_speed) {
    KeyerConfig c; IambicKeyer kr(c);
    auto r = runs(keyer(kr, hold(1, kUnit20 * 10)));                                                    // dit paddle held for 10 units: 5 dits
    CHECK_EQ(r.size(), size_t(10)); for (size_t i = 0; i < r.size(); ++i) { CHECK_EQ(r[i].first, i % 2 == 0); CHECK_EQ(r[i].second, kUnit20); }
    IambicKeyer k2(c); r = runs(keyer(k2, hold(2, kUnit20 * 8)));                                      // dah held for 8 units: 2 dahs
    CHECK_EQ(r.size(), size_t(4)); CHECK_EQ(r[0].second, 3 * kUnit20); CHECK_EQ(r[1].second, kUnit20); CHECK_EQ(r[2].second, 3 * kUnit20);
    KeyerConfig c40; c40.wpm = 40; IambicKeyer k3(c40); r = runs(keyer(k3, hold(1, 1440 * 4))); CHECK_EQ(r[0].second, size_t(1440));
}
TEST(keyer_completes_an_element_from_a_short_tap_and_starts_at_once) {
    IambicKeyer kr(KeyerConfig{}); auto k = keyer(kr, cat(hold(1, 10), hold(0, kUnit20 * 4)));
    CHECK(k[0]); auto r = runs(k); CHECK_EQ(r[0].second, kUnit20); CHECK_EQ(r.size(), size_t(2));        // one full dit, then nothing
    CHECK(kr.idle());
}
TEST(keyer_squeeze_alternates_dit_first) {
    IambicKeyer kr(KeyerConfig{}); auto r = runs(keyer(kr, hold(3, kUnit20 * 12)));                      // both from the start: dit dah dit dah ...
    CHECK_EQ(r[0].second, kUnit20); CHECK_EQ(r[2].second, 3 * kUnit20); CHECK_EQ(r[4].second, kUnit20); CHECK_EQ(r[6].second, 3 * kUnit20);
    IambicKeyer k2(KeyerConfig{}); r = runs(keyer(k2, cat(hold(2, 100), hold(3, kUnit20 * 12))));       // dah first when it is pressed first
    CHECK_EQ(r[0].second, 3 * kUnit20); CHECK_EQ(r[2].second, kUnit20);
}
TEST(keyer_mode_b_adds_an_element_after_a_squeeze_mode_a_does_not) {
    // squeeze dah-first, release both in the middle of the first dah
    const auto p = cat(cat(hold(2, 100), hold(3, kUnit20)), hold(0, kUnit20 * 10));
    KeyerConfig b; b.mode = IambicMode::B; IambicKeyer kb(b); auto rb = runs(keyer(kb, p));
    KeyerConfig a; a.mode = IambicMode::A; IambicKeyer ka(a); auto ra = runs(keyer(ka, p));
    CHECK_EQ(ra.size(), size_t(2)); CHECK_EQ(ra[0].second, 3 * kUnit20);                                 // A: dah, stop
    CHECK_EQ(rb.size(), size_t(4)); CHECK_EQ(rb[0].second, 3 * kUnit20); CHECK_EQ(rb[2].second, kUnit20); // B: dah, then a dit
}
TEST(keyer_remembers_a_tap_of_the_other_paddle_in_both_modes) {
    // dah paddle held briefly, dit tapped (alone, not a squeeze) during the dah, nothing at the end: the dit is still sent
    const auto p = cat(cat(cat(hold(2, 100), hold(0, kUnit20)), hold(1, 50)), hold(0, kUnit20 * 10));
    for (IambicMode m : {IambicMode::A, IambicMode::B}) {
        KeyerConfig c; c.mode = m; IambicKeyer kr(c); auto r = runs(keyer(kr, p));
        CHECK_EQ(r.size(), size_t(4)); CHECK_EQ(r[0].second, 3 * kUnit20); CHECK_EQ(r[1].second, kUnit20); CHECK_EQ(r[2].second, kUnit20);
    }
}
TEST(keyer_weight_and_reverse) {
    KeyerConfig c; c.weight = 60; IambicKeyer kr(c); auto r = runs(keyer(kr, hold(1, kUnit20 * 6)));
    const size_t extra = kUnit20 / 5;                                                                    // (60 - 50) / 50 of a unit
    CHECK_EQ(r[0].second, kUnit20 + extra); CHECK_EQ(r[1].second, kUnit20 - extra); CHECK_EQ(r[0].second + r[1].second, 2 * kUnit20);
    KeyerConfig rv; rv.reverse = true; IambicKeyer k2(rv); r = runs(keyer(k2, hold(1, kUnit20 * 4))); CHECK_EQ(r[0].second, 3 * kUnit20);
    KeyerConfig bad; bad.wpm = 70; CHECK_THROWS(IambicKeyer{bad}, std::invalid_argument); bad = {}; bad.weight = 90; CHECK_THROWS(IambicKeyer{bad}, std::invalid_argument);
}
TEST(keyer_output_decodes) {
    // "TEST" sent with paddles: each element's paddle is held for one sample at the moment the keyer can take it; letters are separated by idle time
    const char* morse[] = {"-", ".", "...", "-"};
    IambicKeyer kr(KeyerConfig{}); std::vector<uint8_t> key;
    auto send_letter = [&](const std::vector<uint8_t>& els) {
        for (uint8_t p : els) { key.push_back(kr.step(p & 1, p & 2)); while (!kr.idle()) key.push_back(kr.step(false, false)); }
        for (size_t i = 0; i < 2 * kUnit20; ++i) key.push_back(kr.step(false, false));                // + 1 unit already after the element = 3 units letter gap
    };
    for (size_t i = 0; i < size_t(2 * kFs); ++i) key.push_back(kr.step(false, false));                // 2 s lead, then VVV, so the decoder locks
    for (int rep = 0; rep < 3; ++rep) send_letter({1, 1, 1, 2});                                       // V V V
    for (size_t i = 0; i < 4 * kUnit20; ++i) key.push_back(kr.step(false, false));                    // word gap
    for (const char* m : morse) { std::vector<uint8_t> els; for (const char* q = m; *q; ++q) els.push_back(*q == '.' ? 1 : 2); send_letter(els); }
    key.resize(key.size() + kFs, 0);
    const auto iq = shape(key);
    dsp::DemodConfig d; d.input_rate = kFs; d.audio_rate = kFs; d.mode = dsp::Mode::CW; d.agc = dsp::AgcMode::Off;
    dsp::Demodulator dm(d); std::vector<float> a; dm.process(iq.data(), iq.size(), a);
    decode::CwDecoder dec; std::string out; for (size_t i = 0; i < a.size(); i += 4096) dec.process(a.data() + i, std::min<size_t>(4096, a.size() - i), out);
    CHECK(out.find("TEST") != std::string::npos);
}

TEST(cw_review_sample_rates_are_checked) {
    CwShapeConfig c; c.sample_rate = 0; CHECK_THROWS(CwShaper{c}, std::invalid_argument);
    KeyerConfig k; k.sample_rate = 0; CHECK_THROWS(IambicKeyer{k}, std::invalid_argument);
    CHECK_THROWS(text_keying("E", 20, 1e9), std::invalid_argument); CHECK_THROWS(text_keying("E", 20, 48000, -1), std::invalid_argument);
}
