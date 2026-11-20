// Offline renderers and the firesidetx tool: output ceiling, lengths, sidebands, WAV round trip, the tool itself, and error handling.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include "fireside/decode.h"
#include "fireside/dsp.h"
#include "fireside/tx_render.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::tx; using fireside::dsp::kPi;

namespace {
std::string tmpf(const char* n) { return (std::filesystem::temp_directory_path() / (std::string("firesidetx_") + std::to_string(::getpid()) + "_" + n)).string(); }
double power_in(const std::vector<cf>& x, double lo, double hi) {                      // power between lo and hi Hz (DFT on 2^16 samples from the middle)
    const size_t n = 65536; if (x.size() < n) throw std::runtime_error("test signal too short"); const size_t from = x.size() / 2 - n / 2; dsp::Fft f(n); std::vector<cf> a(x.begin() + long(from), x.begin() + long(from + n)); auto w = dsp::blackman_harris(n);
    for (size_t i = 0; i < n; ++i) a[i] *= w[i];
    f.forward(a.data()); double p = 0;
    for (size_t k = 0; k < n; ++k) { const double hz = (k < n / 2 ? double(k) : double(k) - double(n)) * 48000.0 / double(n); if (hz >= lo && hz <= hi) p += std::norm(a[k]); } return p;
}
double db(double r) { return 10 * std::log10(r + 1e-30); }
}  // namespace

TEST(render_ft8_and_ft4_fill_whole_slots_under_the_ceiling_on_the_right_sideband) {
    for (bool ft4 : {false, true}) for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
        const auto y = render_ft("CQ M0OML IO91", 1200, ft4, sb, -6);
        CHECK_EQ(y.size(), size_t((ft4 ? 7.5 : 15.0) * 48000)); CHECK(peak_magnitude(y) <= kRenderCeiling);
        CHECK(peak_magnitude(y) > 0.45);                                               // -6 dBFS = 0.5, not muted
        const double wanted = sb == Sideband::USB ? power_in(y, 800, 1700) : power_in(y, -1700, -800), other = sb == Sideband::USB ? power_in(y, -1700, -800) : power_in(y, 800, 1700);
        CHECK(db(wanted / other) > 45);
    }
}
TEST(a_usb_receiver_hears_an_lsb_render_at_least_45_db_down) {
    // The synthetic files are noiseless, so even this leakage still decodes in the app (FT8 does, at a lower SNR): measure it instead of guessing.
    const auto y = render_ft("CQ M0OML IO91", 1200, false, Sideband::LSB, -6);
    dsp::DemodConfig d; d.input_rate = 48000; d.audio_rate = 48000; d.mode = dsp::Mode::USB; d.agc = dsp::AgcMode::Off; dsp::Demodulator dm(d);
    std::vector<float> a; dm.process(y.data(), y.size(), a); std::vector<cf> ac(a.size()); for (size_t i = 0; i < a.size(); ++i) ac[i] = cf(a[i], 0);
    dsp::DemodConfig dl = d; dl.mode = dsp::Mode::LSB; dsp::Demodulator dml(dl); std::vector<float> b; dml.process(y.data(), y.size(), b);
    double pa = 0, pb = 0; for (size_t i = 24000; i < a.size(); ++i) { pa += double(a[i]) * a[i]; pb += double(b[i]) * b[i]; }
    CHECK(db(pb / pa) > 45);
}
TEST(render_cw_has_the_requested_level_and_ends_silent) {
    const auto y = render_cw("CQ", 20, 5, -10); CHECK_NEAR(peak_magnitude(y), std::pow(10.0, -10.0 / 20.0), 0.005);
    CHECK(std::abs(y.front()) == 0.0f); CHECK(std::abs(y.back()) == 0.0f); CHECK(y.size() > size_t(48000 * 1.5));
    CHECK_THROWS(render_cw("CQ", 100), std::invalid_argument); CHECK_THROWS(render_cw("CQ", 20, 5, 0), std::invalid_argument);
}
TEST(render_psk31_and_rtty_decode_back_and_stay_limited) {
    auto audio = [](const std::vector<cf>& y) { std::vector<float> a(y.size()); for (size_t i = 0; i < y.size(); ++i) a[i] = y[i].real(); return a; };   // USB: real part = audio
    const auto p = render_psk31("CQ DE M0OML", 1000, Sideband::USB); CHECK(peak_magnitude(p) <= kRenderCeiling);
    { auto a = audio(p); fireside::decode::Psk31Decoder d(fireside::decode::PskOptions{48000, 1000}); std::string out; d.process(a.data(), a.size(), out); CHECK(out.find("CQ DE M0OML") != std::string::npos); }
    const auto r = render_rtty("CQ TEST DE M0OML", 2125, Sideband::USB); CHECK(peak_magnitude(r) <= kRenderCeiling);
    { auto a = audio(r); fireside::decode::RttyDecoder d; std::string out; d.process(a.data(), a.size(), out); CHECK(out.find("CQ TEST DE M0OML") != std::string::npos); }
    CHECK_THROWS(render_psk31("", 1000, Sideband::USB), std::invalid_argument); CHECK_THROWS(render_psk31("A", 50, Sideband::USB), std::invalid_argument);
    CHECK_THROWS(render_rtty("A", 2900, Sideband::USB), std::invalid_argument); CHECK_THROWS(render_rtty("A", 2125, Sideband::USB, 0), std::invalid_argument);
}
TEST(render_two_tone_is_clean_and_limited) {
    for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
        const auto y = render_two_tone(2, -6, sb); CHECK_EQ(y.size(), size_t(96000)); CHECK(peak_magnitude(y) <= kRenderCeiling);
        const auto r = measure_imd(y, sb); CHECK(r.im3_dbc() < -100);
    }
    CHECK_THROWS(render_two_tone(0, -6, Sideband::USB), std::invalid_argument); CHECK_THROWS(render_two_tone(2, 0, Sideband::USB), std::invalid_argument);
}
TEST(render_ssb_never_exceeds_the_ceiling_even_on_hostile_audio) {
    std::vector<float> a(96000); for (size_t i = 0; i < a.size(); ++i) a[i] = (i / 30) % 2 ? 40.0f : -40.0f;
    a[1000] = std::nanf(""); a[2000] = INFINITY;
    for (bool mic : {true, false}) for (Sideband sb : {Sideband::USB, Sideband::LSB}) {
        const auto y = render_ssb(a, sb, mic, 30); CHECK_EQ(y.size(), a.size()); CHECK(peak_magnitude(y) <= kRenderCeiling);
        for (const cf& v : y) CHECK(std::isfinite(v.real()) && std::isfinite(v.imag()));
    }
    CHECK_THROWS(render_ssb(a, Sideband::USB, true, 80), std::invalid_argument);
}
namespace {
std::vector<std::pair<double, double>> runs(const std::vector<cf>& y) {   // on-runs of the envelope: (start s, length s)
    double pk = peak_magnitude(y); std::vector<std::pair<double, double>> r; bool on = false; size_t st = 0;
    for (size_t i = 0; i < y.size(); ++i) { const bool o = std::abs(y[i]) > 0.5 * pk; if (o && !on) { on = true; st = i; } else if (!o && on) { on = false; r.push_back({st / kRenderRate, (i - st) / kRenderRate}); } }
    return r;
}
}  // namespace
TEST(render_paddles_keys_dits_dahs_and_squeezes) {
    const double u = 0.06;                                                  // one unit at 20 wpm
    auto a = runs(render_paddles({{0.0, true, false}, {4.5 * u, false, false}}, 20));            // dit paddle held for 4.5 units: dit gap dit gap dit
    CHECK_EQ(a.size(), size_t(3)); for (auto& r : a) CHECK_NEAR(r.second, u, 0.004);
    a = runs(render_paddles({{0.1, false, true}, {0.15, false, false}}, 20));                    // a tap on the dah paddle: exactly one dah
    CHECK_EQ(a.size(), size_t(1)); CHECK_NEAR(a[0].second, 3 * u, 0.004);
    const std::vector<PaddleEvent> sq = {{0.0, true, true}, {3.0 * u, false, false}};            // squeeze, released during the dah
    CHECK_EQ(runs(render_paddles(sq, 20, IambicMode::A)).size(), size_t(2));                      // mode A: dit dah
    CHECK_EQ(runs(render_paddles(sq, 20, IambicMode::B)).size(), size_t(3));                      // mode B: one more element of the opposite kind
    a = runs(render_paddles({{0.0, true, false}, {0.001, false, false}}, 20, IambicMode::A, /*reverse=*/true));   // reversed: the dit paddle gives a dah
    CHECK_EQ(a.size(), size_t(1)); CHECK_NEAR(a[0].second, 3 * u, 0.004);
    const auto y = render_paddles({{0.0, true, true}, {600.0, false, false}}, 60); CHECK(peak_magnitude(y) <= kRenderCeiling);
    CHECK_THROWS(render_paddles({}, 20), std::invalid_argument); CHECK_THROWS(render_paddles({{1.0, true, false}, {0.5, false, false}}, 20), std::invalid_argument);
    CHECK_THROWS(render_paddles({{601.0, true, false}}, 20), std::invalid_argument); CHECK_THROWS(render_paddles({{std::nan(""), true, false}}, 20), std::invalid_argument);
    CHECK_THROWS(render_paddles({{0.0, true, false}}, 100), std::invalid_argument);
}
TEST(render_error_handling) {
    CHECK_THROWS(render_ft("THIS TEXT IS WAY TOO LONG TO ENCODE AT ALL", 1000, false, Sideband::USB), std::runtime_error);
    CHECK_THROWS(render_ft("CQ M0OML IO91", 100, false, Sideband::USB), std::invalid_argument); CHECK_THROWS(render_ft("CQ M0OML IO91", 1000, false, Sideband::USB, 0), std::invalid_argument);
    CHECK_THROWS(read_audio_wav("/nonexistent/x.wav"), std::runtime_error);
}
TEST(iq_wav_round_trips_and_audio_wav_is_resampled) {
    const auto y = render_cw("E", 25, 5, -6, 300); const std::string p = tmpf("rt.wav"); write_iq_wav(p, y);
    const auto r = dsp::read_iq_wav(p, 0, 1); CHECK_EQ(r.sample_rate, 48000.0); CHECK_EQ(r.samples.size(), y.size()); for (size_t i = 0; i < y.size(); i += 97) CHECK(std::abs(r.samples[i] - y[i]) < 1e-6);
    std::vector<float> t(44100); for (size_t i = 0; i < t.size(); ++i) t[i] = float(0.5 * std::sin(2 * kPi * 1000 * double(i) / 44100)); const std::string q = tmpf("a44.wav"); dsp::write_wav_pcm16_mono(q, 44100, t);
    const auto a = read_audio_wav(q); CHECK(std::fabs(double(a.size()) - 48000.0) < 5.0);                  // 1 s at 44.1 kHz -> 1 s at 48 kHz
    std::complex<double> s(0, 0); for (size_t i = 12000; i < 36000; ++i) s += double(a[i]) * std::polar(1.0, -2 * kPi * 1000 * double(i) / 48000); CHECK_NEAR(std::abs(s) / 24000.0 * 2, 0.5, 0.02);
    std::filesystem::remove(p); std::filesystem::remove(q);
}
TEST(firesidetx_tool_writes_wavs_and_refuses_bad_requests) {
    const std::string tool = FIRESIDETX_PATH, f = tmpf("tool.wav"), in = tmpf("in.wav"), out = tmpf("ssb.wav");
    auto run = [&](const std::string& args) { return std::system((tool + " " + args + " >/dev/null 2>&1").c_str()); };
    CHECK_EQ(run("ft8 \"CQ M0OML IO91\" " + f + " --freq 1500"), 0);
    const auto r = dsp::read_iq_wav(f, 0, 1); CHECK_EQ(r.sample_rate, 48000.0); CHECK_EQ(r.samples.size(), size_t(15 * 48000)); CHECK(peak_magnitude(r.samples) <= kRenderCeiling);
    std::vector<float> speech(96000);
    for (size_t i = 0; i < speech.size(); ++i) speech[i] = float(0.3 * std::sin(2 * kPi * 1000 * double(i) / 48000));
    dsp::write_wav_pcm16_mono(in, 48000, speech);
    CHECK_EQ(run("ssb " + in + " " + out + " --lsb"), 0); const auto s = dsp::read_iq_wav(out, 0, 1); CHECK_EQ(s.samples.size(), speech.size()); CHECK(db(power_in(s.samples, -1100, -900) / power_in(s.samples, 900, 1100)) > 45);
    CHECK_EQ(run("cw \"CQ\" " + f + " --wpm 25 --edge 6"), 0); CHECK_EQ(run("twotone " + f + " --seconds 1 --peak -9"), 0);
    CHECK(run("ft8 \"THIS TEXT IS WAY TOO LONG TO ENCODE AT ALL\" " + f) != 0);          // cannot encode
    CHECK(run("ft8 \"CQ M0OML IO91\" " + f + " --level 3") != 0);                         // louder than -1 dBFS
    CHECK(run("bogus x y") != 0); CHECK(run("ft8 \"CQ\"") != 0); CHECK(run("ft8 \"CQ M0OML IO91\" " + f + " --nonsense") != 0); CHECK(run("") != 0);
    CHECK(run("ssb /nonexistent.wav " + out) != 0);
    for (auto& p : {f, in, out}) std::filesystem::remove(p);
}
