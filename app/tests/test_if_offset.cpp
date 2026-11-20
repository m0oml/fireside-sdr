// The 9 kHz IF offset (PowerSDR style): the session tunes the radio's DDS 9 kHz below the dial and the receiver shifts the I/Q back.
// Why (measured on the real FLEX-5000A, 2026-11-17): its I/Q has a narrow hole at 0 Hz (DC removal), so an AM carrier tuned exactly onto
// the DDS is stripped and the audio is ruined; a few Hz off it is fine. These tests pin the DDS values, the mirror direction (a wrong sign
// would put the dial 18 kHz out) and the AM result through a model of that hole.
#include <cmath>
#include <complex>
#include <vector>
#include "fake_dice.h"
#include "fireside/dsp.h"
#include "fireside/session.h"
#include "fireside/tuning.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::dsp; using namespace fakedice;

namespace {
struct S {
    FakeDiceBus bus; RadioSession session;
    S() : bus(), session([this](const std::string&) { return std::unique_ptr<BusHandle>(new FakeHandle(bus, false)); }) { flex_defaults(bus); }
    SessionOptions opts(double if_hz = kRadioIfOffsetHz) { SessionOptions o; o.path = "/dev/fw1"; o.allow_write = true; o.boot_wait_s = 0; o.if_offset_hz = if_hz; return o; }
};
constexpr double kFs = 48000, kPi2 = 6.283185307179586;
size_t peak_bin_of(const std::vector<float>& db) { size_t k = 0; for (size_t i = 1; i < db.size(); ++i) if (db[i] > db[k]) k = i; return k; }

// What the radio delivers for an RF signal: complex baseband relative to the DDS, then the measured quirks: mirrored I/Q and a ~2 Hz DC hole.
std::vector<cf> radio_iq(double carrier_rel_dds_hz, double am_depth, double tone_hz, size_t n, bool hole) {
    std::vector<cf> x(n); double m = 0, mi = 0; const double a = kPi2 * 2.0 / kFs;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / kFs, env = 0.01 * (1.0 + am_depth * std::sin(kPi2 * tone_hz * t));
        cf s = std::polar(float(env), float(kPi2 * carrier_rel_dds_hz * t));
        s = std::conj(s);                                                                   // the radio's I/Q is mirrored (user-confirmed on air)
        if (hole) { m += a * (s.real() - m); mi += a * (s.imag() - mi); s = cf(float(s.real() - m), float(s.imag() - mi)); }   // one-pole DC removal
        x[i] = s;
    }
    return x;
}
double level_at(const std::vector<float>& y, size_t from, double hz) {                    // single-bin DFT magnitude
    double re = 0, im = 0; for (size_t i = from; i < y.size(); ++i) { const double p = kPi2 * hz * double(i) / kFs; re += y[i] * std::cos(p); im -= y[i] * std::sin(p); }
    return std::hypot(re, im);
}
double am_thd_percent(double if_hz, bool hole) {                                           // AM station tuned DEAD ON the dial, AGC Med, as in the app
    ReceiveConfig rc; rc.if_offset_hz = if_hz; rc.mirror = true; DemodConfig dc; dc.mode = Mode::AM; dc.agc = AgcMode::Med;
    ReceiveChain ch(rc, dc); std::vector<float> audio;
    auto x = radio_iq(if_hz, 0.8, 1000, size_t(3 * kFs), hole);                            // the carrier sits at +if_hz from the DDS = exactly on the dial
    ch.process(x.data(), x.size(), audio);
    const size_t from = size_t(kFs); const double f1 = level_at(audio, from, 1000);
    double h = 0; for (int k = 2; k <= 6; ++k) { const double v = level_at(audio, from, 1000.0 * k); h += v * v; }
    return 100.0 * std::sqrt(h) / f1;
}
}  // namespace

TEST(if_offset_constant_matches_the_receiver) { CHECK_EQ(kRadioIfOffsetHz, kIfOffsetHz); CHECK_EQ(SessionOptions{}.if_offset_hz, kIfOffsetHz); }

TEST(if_offset_session_tunes_the_dds_9khz_below_the_dial) {
    S s; s.session.connect(s.opts()); std::vector<std::string> log;
    s.session.start(7.1, "ANT1", [&](const std::string& m) { log.push_back(m); });
    auto w = write_ops(s.bus); CHECK_EQ(w.size(), size_t(5)); CHECK_EQ(w[4].op, opv("RDAL_OP_SET_RX1_FREQ_TW"));
    CHECK_EQ(w[4].d1, freq_to_tw(7.091)); CHECK_NEAR(as_f32(w[4].d2), 7.091, 1e-5);       // both words carry the DDS, not the dial
    bool said = false; for (auto& m : log) if (m.find("9 kHz below") != std::string::npos) said = true; CHECK(said);
    CHECK(s.session.tune(7.1e6) == TuneResult::Unchanged); CHECK_EQ(write_ops(s.bus).size(), size_t(5));   // the tuner knows the radio is already there
    CHECK(s.session.tune(14.074e6) == TuneResult::Sent); w = write_ops(s.bus); CHECK_EQ(w.back().d1, freq_to_tw(14.065)); CHECK_NEAR(as_f32(w.back().d2), 14.065, 1e-5);
    CHECK_EQ(s.session.if_offset_hz(), 9000.0);
}
TEST(if_offset_refuses_a_dial_whose_dds_would_be_out_of_range_before_any_traffic) {
    S s; s.session.connect(s.opts()); const size_t w0 = s.bus.writes.size();
    CHECK_THROWS(s.session.start(0.005, "ANT1"), std::invalid_argument);                  // 5 kHz: the DDS would be below 0
    CHECK_THROWS(s.session.start(65.5, "ANT1"), std::invalid_argument);
    CHECK_EQ(s.bus.writes.size(), w0);
    S b; b.session.connect(b.opts(-1)); CHECK_THROWS(b.session.start(7.1, "ANT1"), std::invalid_argument);
    S c; c.session.connect(c.opts(30000)); CHECK_THROWS(c.session.start(7.1, "ANT1"), std::invalid_argument); CHECK(write_ops(c.bus).empty());
}
TEST(if_offset_zero_still_puts_the_dds_on_the_dial) {
    S s; s.session.connect(s.opts(0)); s.session.start(7.1, "ANT1");
    CHECK_EQ(write_ops(s.bus)[4].d1, 0x03A29C77u);                                         // the old behaviour, kept as an option
}
TEST(if_offset_mirror_direction_puts_a_known_station_on_its_frequency) {
    // dial 7.100 MHz, DDS 7.091 MHz; a carrier 1 kHz above the dial is 10 kHz above the DDS. The radio mirrors it (raw at -10 kHz).
    ReceiveConfig rc; rc.mirror = true; DemodConfig dc; ReceiveChain ch(rc, dc); std::vector<float> a;
    auto x = radio_iq(10000, 0.0, 1000, 16384, false); ch.process(x.data(), x.size(), a);
    auto& sp = ch.spectrum(); CHECK_NEAR(sp.bin_hz(peak_bin_of(sp.db())), 1000.0, 2 * (sp.bin_hz(1) - sp.bin_hz(0)));
    ReceiveChain hole_ch(rc, dc); std::vector<float> b; std::vector<cf> dc_only(16384, cf(0.01f, 0.0f));   // the radio's DC (its hole) lands 9 kHz BELOW the dial
    hole_ch.process(dc_only.data(), dc_only.size(), b);
    CHECK_NEAR(hole_ch.spectrum().bin_hz(peak_bin_of(hole_ch.spectrum().db())), -9000.0, 2 * (sp.bin_hz(1) - sp.bin_hz(0)));
}
TEST(if_offset_am_dead_on_tune_survives_the_radios_dc_hole) {
    const double with_offset = am_thd_percent(9000, true), without = am_thd_percent(0, true), clean = am_thd_percent(9000, false);
    CHECK(clean < 2.0);
    CHECK(with_offset < 2.0);                                                              // the fix: carrier 9 kHz away from the hole
    CHECK(without > 20.0);                                                                 // the bug as heard on 693 kHz: the hole strips the carrier
}
