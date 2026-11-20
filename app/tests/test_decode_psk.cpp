// PSK31 decoder against generated BPSK31 (Varicode): clean, noisy, off-centre, other frequencies, chunking, and noise only.
#include <cmath>
#include "fireside/decode.h"
#include "test_util.h"
using namespace fireside::decode;

namespace {
std::string run(const std::vector<float>& x, PskOptions o = {}, size_t chunk = 4096) { Psk31Decoder d(o); std::string out; for (size_t i = 0; i < x.size(); i += chunk) d.process(x.data() + i, std::min(chunk, x.size() - i), out); return out; }
size_t edit(const std::string& a, const std::string& b) {
    std::vector<size_t> p(b.size() + 1), c(b.size() + 1); for (size_t j = 0; j <= b.size(); ++j) p[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) { c[0] = i; for (size_t j = 1; j <= b.size(); ++j) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)}); std::swap(p, c); }
    return p[b.size()];
}
std::string trim(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back(); size_t i = 0; while (i < s.size() && (s[i] == ' ')) ++i; return s.substr(i); }
}  // namespace

TEST(varicode_table_is_the_standard_one) {
    CHECK_EQ(std::string(varicode_for(' ')), std::string("1")); CHECK_EQ(std::string(varicode_for('e')), std::string("11")); CHECK_EQ(std::string(varicode_for('t')), std::string("101"));
    CHECK_EQ(std::string(varicode_for('o')), std::string("111")); CHECK_EQ(std::string(varicode_for('a')), std::string("1011")); CHECK_EQ(std::string(varicode_for('\n')), std::string("11101"));
    CHECK(varicode_for(200) == nullptr);
    for (int i = 0; i < 128; ++i) { const std::string c = varicode_for(static_cast<unsigned char>(i)); CHECK(c.find("00") == std::string::npos); CHECK(c.front() == '1' && c.back() == '1'); for (int j = i + 1; j < 128; ++j) CHECK(c != varicode_for(static_cast<unsigned char>(j))); }
}
TEST(psk31_decodes_clean_text) {
    const std::string text = "CQ CQ CQ DE M0OML M0OML PSE K";
    CHECK_EQ(trim(run(synth_psk31(text + " ", 1000, 48000, 0.5, 2.0))), text);
}
TEST(psk31_decodes_digits_punctuation_lower_case_and_newlines) {
    const std::string text = "Hello, World! 73 es GL de m0oml.\nRST 599/599 QTH: London";
    CHECK_EQ(trim(run(synth_psk31(text + " ", 1500, 48000, 0.5, 2.0), PskOptions{48000, 1500})), text);
}
TEST(psk31_works_across_the_audio_band) { for (double f : {400.0, 800.0, 1200.0, 2000.0, 2800.0}) CHECK_EQ(trim(run(synth_psk31("TEST DE M0OML ", f, 48000, 0.5, 2.0), PskOptions{48000, f})), std::string("TEST DE M0OML")); }
TEST(psk31_afc_follows_a_signal_that_is_a_few_hertz_off) {
    for (double off : {-15.0, -9.0, -3.0, 4.0, 9.0, 14.0}) {                                                   // beyond +-7 Hz plain differential detection fails: the coarse spectral search finds the signal first
        PskOptions o; o.center_hz = 1000; const std::string text = "CQ CQ DE M0OML M0OML PSE K TEST TEST DE M0OML K";
        const auto out = trim(run(synth_psk31(text + " ", 1000 + off, 48000, 0.5, 4.0), o));
        CHECK(out.size() >= 24 && text.find(out.substr(out.size() - 20)) != std::string::npos);                                                          // once locked the rest is clean (the first words may be lost while it finds the signal)
    }
}
TEST(psk31_survives_noise) {
    const std::string text = "CQ CQ DE M0OML K";
    auto x = synth_psk31(text + " ", 1000, 48000, 0.5, 2.0);
    for (double rms : {1.0, 3.0}) { auto y = x; add_noise(y, rms, 5); CHECK(edit(trim(run(y)), text) <= (rms < 2 ? 0 : 2)); }                       // noise rms 3 = about +12 dB in the 31 Hz bandwidth
}
TEST(psk31_is_independent_of_chunking) {
    auto x = synth_psk31("CQ DE M0OML K ", 1000); const auto whole = run(x, {}, x.size());
    for (size_t chunk : {size_t(1), size_t(997), size_t(4800)}) CHECK_EQ(run(x, {}, chunk), whole);
}
TEST(psk31_prints_nothing_for_silence_or_noise) {
    std::vector<float> silence(48000 * 6, 0.0f); CHECK_EQ(run(silence), std::string());
    std::vector<float> noise(48000 * 20, 0.0f); add_noise(noise, 1.0, 11); CHECK(run(noise).size() <= 12);                                       // squelch: not a stream of random characters
    Psk31Decoder d; std::string out; auto x = synth_psk31("", 1000, 48000, 0.5, 3.0); d.process(x.data(), x.size(), out); CHECK(d.locked()); CHECK(d.status().find("locked") != std::string::npos);
}
TEST(psk31_reset_clears_the_lock) { Psk31Decoder d; std::string out; auto x = synth_psk31("CQ ", 1000, 48000, 0.5, 3.0); d.process(x.data(), x.size(), out); d.reset(); CHECK(!d.locked()); CHECK_EQ(d.center(), 1000.0); }

TEST(psk31_first_character_survives_any_number_of_idle_bits) {                                                      // odd and even idle counts: a lone leftover zero must not corrupt the next code
    for (double lead : {2.0, 2.03, 2.06, 2.1, 2.13, 3.0, 3.5}) CHECK_EQ(trim(run(synth_psk31("CQ DE M0OML K ", 1000, 48000, 0.5, lead))), std::string("CQ DE M0OML K"));
}
TEST(psk31_prints_nothing_on_keyed_cw_or_ft8_tones_but_still_copies_psk) {
    // On a real band (40 m during the CWT, 2026-11-18) the SNR test alone let keyed CW and FT8 tones through and printed garbage. They must now stay silent: their phase does not
    // step by 0 / 180 degrees and their symbol timing does not matter. A PSK31 signal in the same noise still copies.
    PskOptions off; off.min_phase_quality = 0; off.min_timing_contrast = 0;
    size_t old_chars = 0, new_chars = 0;
    for (double df : {0.0, 7.0, -12.0}) {
        auto cw = synth_morse("CQ CWT DE G4ABC G4ABC TEST 5NN TU ", 28, 1000 + df, 48000, 0.3, 5, 0.5);                    // keyed CW near the PSK channel
        add_noise(cw, 0.05, 7); old_chars += run(cw, off).size(); new_chars += run(cw).size();
        std::vector<float> ft8(48000 * 13, 0.0f); uint32_t r = 99u + uint32_t(df + 20); double ph = 0;                       // 8-FSK at 6.25 baud, 6.25 Hz spacing (FT8-like)
        for (size_t i = 0; i < ft8.size(); ++i) { if (i % 7680 == 0) r = r * 1664525u + 1013904223u; ph += 2 * M_PI * (1000 + df - 20 + 6.25 * double((r >> 20) & 7)) / 48000.0; ft8[i] = float(0.3 * std::sin(ph)); }
        add_noise(ft8, 0.05, 8); old_chars += run(ft8, off).size(); new_chars += run(ft8).size();
    }
    CHECK(new_chars <= 2);
    CHECK(old_chars > 20);                                                                                                       // meaningful: without the shape tests these print
    auto psk = synth_psk31("cq cq de m0oml m0oml pse k ", 1000, 48000, 0.3); add_noise(psk, 0.05, 9);
    CHECK(edit(trim(run(psk)), "cq cq de m0oml m0oml pse k") <= 1);
}
