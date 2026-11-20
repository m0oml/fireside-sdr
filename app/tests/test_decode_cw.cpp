// Morse decoder against generated CW: clean, noisy, different speeds and pitches, drifting tone, speed change, and silence/noise only.
#include <cmath>
#include "fireside/decode.h"
#include "test_util.h"
using namespace fireside::decode;

namespace {
std::string decode_all(std::vector<float> x, CwOptions o = {}, size_t chunk = 4096) {
    CwDecoder d(o); std::string out; for (size_t i = 0; i < x.size(); i += chunk) d.process(x.data() + i, std::min(chunk, x.size() - i), out); return out;
}
std::string squash(const std::string& s) { std::string r; for (char c : s) if (c != ' ' || (!r.empty() && r.back() != ' ')) r.push_back(c); while (!r.empty() && r.back() == ' ') r.pop_back(); while (!r.empty() && r[0] == ' ') r.erase(0, 1); return r; }
// character errors by edit distance
size_t edit(const std::string& a, const std::string& b) {
    std::vector<size_t> p(b.size() + 1), c(b.size() + 1); for (size_t j = 0; j <= b.size(); ++j) p[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) { c[0] = i; for (size_t j = 1; j <= b.size(); ++j) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)}); std::swap(p, c); }
    return p[b.size()];
}
// A real signal appears mid-stream, so the first character is only partly heard and is dropped (like every CW decoder). Tests send a throw-away "VVV " first (as operators do) and compare what follows.
std::string after_first_word(const std::string& out) { const size_t sp = out.find(' '); return sp == std::string::npos ? std::string() : squash(out.substr(sp + 1)); }
std::vector<float> with_vvv(const std::string& text, double wpm, double pitch, double fs = 48000, double amp = 0.5, double edge = 5, double lead = 0.5, double farns = 0) { return synth_morse("VVV " + text, wpm, pitch, fs, amp, edge, lead, farns); }
}  // namespace

TEST(morse_table_round_trips_and_is_unique) {
    std::string all = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?/=+-():'\"@;!";
    for (char c : all) { const char* m = morse_for(c); CHECK(m != nullptr); CHECK_EQ(char_for_morse(m), c); }
    CHECK_EQ(std::string(morse_for('a')), std::string(".-")); CHECK(morse_for('~') == nullptr); CHECK_EQ(char_for_morse("........"), char(0));
    CHECK_EQ(std::string(morse_for('S')), std::string("...")); CHECK_EQ(std::string(morse_for('O')), std::string("---")); CHECK_EQ(std::string(morse_for('5')), std::string("....."));
}
TEST(synth_morse_has_the_right_timing) {
    auto x = synth_morse("PARIS", 20, 700, 48000, 0.5, 5, 0, 0);                                         // one PARIS at 20 WPM = 50 units of 60 ms = 3 s (+1 s tail)
    CHECK(std::fabs(double(x.size()) / 48000.0 - (50 * 0.06 - 4 * 0.06 + 1.0 - 0.0)) < 0.45);           // 50 units minus the trailing word gap not included, plus 1 s tail: ~3.76 s
    float peak = 0; for (float v : x) peak = std::max(peak, std::fabs(v)); CHECK_NEAR(peak, 0.5, 0.01);
}
TEST(cw_decodes_clean_text_at_several_speeds) {
    const std::string text = "CQ CQ DE M0OML M0OML K";
    for (double wpm : {10.0, 15.0, 20.0, 25.0, 30.0, 35.0}) {
        auto out = after_first_word(decode_all(with_vvv(text + " ", wpm, 700)));
        CHECK_EQ(out, text);
    }
}
TEST(cw_decodes_digits_and_punctuation) {
    const std::string text = "RST 599 QTH LONDON 73 ES TNX FER QSO?";
    CHECK_EQ(after_first_word(decode_all(with_vvv(text + " ", 22, 700))), text);
}
TEST(cw_reports_speed_and_pitch) {
    CwDecoder d; std::string out; auto x = synth_morse("PARIS PARIS PARIS PARIS ", 20, 700); d.process(x.data(), x.size(), out);
    CHECK(d.wpm() > 18.0 && d.wpm() < 22.0); CHECK(std::fabs(d.pitch() - 700.0) <= 10.0); CHECK(d.status().find("WPM") != std::string::npos);
}
TEST(cw_works_at_other_pitches_with_tracking) {
    const std::string text = "TEST DE M0OML";
    for (double pitch : {550.0, 620.0, 760.0, 820.0}) {
        auto out = after_first_word(decode_all(with_vvv(text + " ", 18, pitch, 48000, 0.5, 5, 2.0)));          // 2 s lead-in lets the tracker lock first
        CHECK(edit(out, text) <= 2);
    }
}
TEST(cw_rejects_a_strong_neighbouring_signal) {
    // wanted: weak CW at 700 Hz; interferer: a stronger, differently keyed CW 300 Hz away. The decoder must copy the wanted one only.
    CwOptions fixed; fixed.track_pitch = false;
    auto wanted = with_vvv("CQ DE M0OML K ", 20, 700, 48000, 0.2, 5, 1.0), other = synth_morse("TEST TEST DE G3XYZ G3XYZ QRZ ", 17, 1000, 48000, 0.6, 5, 1.0);
    other.resize(std::max(other.size(), wanted.size()), 0.0f); for (size_t i = 0; i < wanted.size(); ++i) other[i] += wanted[i];
    const auto got = after_first_word(decode_all(other, fixed));
    CHECK(got.rfind("CQ DE M0OML K", 0) == 0);                                                                  // the wanted message comes out exactly (what follows is the neighbour leaking at -17 dB once the wanted one stops)
    CwOptions at_other = fixed; at_other.pitch_hz = 1000;                                                       // and tuned to the other one it copies THAT
    CHECK(decode_all(other, at_other).find("G3XYZ") != std::string::npos);
}
TEST(cw_survives_noise) {
    const std::string text = "CQ CQ DE M0OML K";
    auto x = with_vvv(text + " ", 20, 700, 48000, 0.3, 5, 1.0);                                                // tone 0.3 amplitude
    for (double rms : {0.05, 0.15}) {                                                                           // ~ +14 and +6 dB SNR in the 170 Hz detector's noise bandwidth
        auto y = x; add_noise(y, rms, 7); auto out = after_first_word(decode_all(y));
        CHECK(edit(out, text) <= (rms < 0.1 ? 1 : 3));
    }
}
TEST(cw_copes_with_soft_keying_and_slow_fades) {
    for (double depth : {0.2, 0.3}) {                                                                            // amplitude swings between (1-2*depth) and 1: up to 8 dB
        auto x = with_vvv("CQ DE M0OML K ", 25, 700, 48000, 0.4, 10.0);                                          // soft 10 ms edges
        for (size_t i = 0; i < x.size(); ++i) x[i] *= float((1.0 - depth) + depth * std::sin(2 * M_PI * 0.25 * double(i) / 48000.0));
        CHECK(edit(after_first_word(decode_all(x)), "CQ DE M0OML K") <= 1);
    }
}
TEST(cw_recovers_after_a_speed_change_and_handles_farnsworth_spacing) {
    auto a = with_vvv("CQ CQ DE M0OML ", 12, 700, 48000, 0.5, 5, 0.5), b = synth_morse("TEST TEST TEST K ", 28, 700, 48000, 0.5, 5, 0.2);
    a.insert(a.end(), b.begin(), b.end()); const auto out = squash(decode_all(a));
    CHECK(out.find("CQ CQ DE M0OML") != std::string::npos);                                                      // the slow part is exact
    CHECK(out.size() >= 6 && out.substr(out.size() - 6) == "TEST K");                                            // the fast part is right again within a word of the change
    auto fo = decode_all(with_vvv("CQ DE M0OML K ", 20, 700, 48000, 0.5, 5, 0.5, 10)); const size_t at = fo.find("CQ");                       // 20 WPM characters, 10 WPM spacing (Farnsworth)
    CHECK(at != std::string::npos); CHECK_EQ(squash(fo.substr(at)), std::string("CQ DE M0OML K"));
}
TEST(cw_is_independent_of_chunking) {
    auto x = synth_morse("CQ DE M0OML K ", 20, 700, 48000, 0.5, 5, 0.5); const auto whole = decode_all(x, {}, x.size());
    for (size_t chunk : {size_t(1), size_t(100), size_t(4801)}) CHECK_EQ(decode_all(x, {}, chunk), whole);
}
TEST(cw_prints_nothing_for_silence_or_pure_noise) {
    std::vector<float> silence(48000 * 8, 0.0f); CHECK_EQ(decode_all(silence), std::string());
    std::vector<float> noise(48000 * 20, 0.0f); add_noise(noise, 0.2, 3); CHECK(decode_all(noise).size() <= 2);                                  // at most a stray character, not a stream of garbage
    std::vector<float> steady = synth_morse("", 20, 700); steady.assign(48000 * 5, 0.0f); for (size_t i = 0; i < steady.size(); ++i) steady[i] = float(0.5 * std::sin(2 * M_PI * 700.0 * double(i) / 48000.0));
    CHECK(decode_all(steady).size() <= 2);                                                                                                         // a carrier that never keys is not text
}
TEST(cw_reset_forgets_the_speed) {
    CwDecoder d; std::string out; auto x = synth_morse("PARIS PARIS PARIS PARIS ", 30, 700); d.process(x.data(), x.size(), out); CHECK(d.wpm() > 25);
    d.reset(); CHECK_EQ(d.wpm(), 0.0); CHECK(!d.signal_present());
}
TEST(cw_band_noise_crashes_and_qrm_print_nothing_then_a_station_still_copies) {
    // A real band between overs (40 m, 2026-11-18): noise whose level wanders, short crashes of QRM, and a drifting switch-mode supply harmonic sweeping through the passband.
    // The level test alone let this print streams of E / T / I; the keying (two-level) test must keep it silent and still copy the station that follows.
    CwOptions off; off.min_separation = off.hold_separation = 0; off.min_separation_short = 1e9;
    size_t old_chars = 0;
    for (uint32_t seed : {11u, 12u, 13u}) for (double sweep : {0.0, 0.12}) {
        std::vector<float> x(48000 * 40, 0.0f); add_noise(x, 0.25, seed);
        double ph = 0; uint32_t r = seed * 2654435761u;
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = double(i) / 48000.0, f = 700.0 + 260.0 * std::sin(2 * M_PI * t / (13.0 + seed));                  // the harmonic drifts across the CW filter and back
            ph += 2 * M_PI * f / 48000.0; if (i % 4800 == 0) r = r * 1664525u + 1013904223u;
            x[i] = float(x[i] * (0.5 + 0.5 * double(r >> 8) / 16777216.0) + sweep * std::sin(ph));                              // the noise level changes every 0.1 s
        }
        for (size_t i = 0; i < x.size();) {                                                                                     // crashes: 10-80 ms, 0.1-2 s apart, 5x the noise
            r = r * 1664525u + 1013904223u; i += 4800 + (r >> 8) % 96000; r = r * 1664525u + 1013904223u; const size_t len = 480 + (r >> 8) % 3400;
            for (size_t j = i; j < std::min(x.size(), i + len); ++j) x[j] *= 5.0f;
        }
        size_t n = 0; for (char c : decode_all(x)) n += c != ' '; CHECK(n <= 1);
        for (char c : decode_all(x, off)) old_chars += c != ' ';
        auto both = x; auto sig = with_vvv("CQ CWT DE M0OML M0OML K ", 26, 700, 48000, 0.5); both.insert(both.end(), sig.begin(), sig.end());
        const std::string all = decode_all(both); const size_t at = all.find("CQ"); CHECK(at != std::string::npos); if (at != std::string::npos) CHECK(edit(squash(all.substr(at)), "CQ CWT DE M0OML M0OML K") <= 1);
    }
    CHECK(old_chars > 50);                                                                                                       // the test is meaningful: without the keying test this noise prints
}
