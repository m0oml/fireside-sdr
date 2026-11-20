// RTTY decoder against generated 45.45 baud / 170 Hz shift signals and variants.
#include <cmath>
#include "fireside/decode.h"
#include "test_util.h"
using namespace fireside::decode;

namespace {
std::string run(const std::vector<float>& x, RttyOptions o = {}, size_t chunk = 4096) { RttyDecoder d(o); std::string out; for (size_t i = 0; i < x.size(); i += chunk) d.process(x.data() + i, std::min(chunk, x.size() - i), out); return out; }
size_t edit(const std::string& a, const std::string& b) {
    std::vector<size_t> p(b.size() + 1), c(b.size() + 1); for (size_t j = 0; j <= b.size(); ++j) p[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) { c[0] = i; for (size_t j = 1; j <= b.size(); ++j) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)}); std::swap(p, c); }
    return p[b.size()];
}
std::string trim(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back(); size_t i = 0; while (i < s.size() && s[i] == ' ') ++i; return s.substr(i); }
}  // namespace

TEST(ita2_tables_round_trip) {
    for (char c : std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789-?:$!&#()'\",./;")) { bool f = false; const int code = ita2_code(c, &f); CHECK(code >= 0); CHECK_EQ(ita2_char(code, f), c); }
    bool f = true; CHECK_EQ(ita2_code('E', &f), 16); CHECK(!f); CHECK_EQ(ita2_code('3', &f), 16); CHECK(f); CHECK_EQ(ita2_code('t', &f), 1); CHECK_EQ(ita2_code(' ', &f), 4); CHECK_EQ(ita2_code('\n', &f), 8);
    CHECK_EQ(ita2_code('~', &f), -1); CHECK_EQ(ita2_char(31, false), char(0)); CHECK_EQ(ita2_char(27, true), char(0)); CHECK_EQ(ita2_char(99, false), char(0));
}
TEST(rtty_decodes_clean_text_with_figures) {
    const std::string text = "CQ CQ CQ DE M0OML M0OML K RST 599 QTH LONDON 73";
    CHECK_EQ(trim(run(synth_rtty(text + " ", 2125, 170, 45.45))), text);
}
TEST(rtty_handles_figure_shifts_unshift_on_space_and_newlines) {
    const std::string text = "NAME IS JOHN 1234567890 AGE 42\nQTH: LONDON, UK (G3XYZ)";
    CHECK_EQ(trim(run(synth_rtty(text + " ", 2125, 170, 45.45))), text);
}
TEST(rtty_other_frequencies_shifts_and_bauds) {
    const std::string text = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789";
    struct Case { double mark, shift, baud; }; for (Case c : {Case{1275, 170, 45.45}, Case{2125, 85, 45.45}, Case{2125, 170, 50.0}, Case{1000, 425, 75.0}, Case{2125, 170, 45.45}}) {
        RttyOptions o; o.mark_hz = c.mark; o.shift_hz = c.shift; o.baud = c.baud;
        CHECK(edit(trim(run(synth_rtty(text + " ", c.mark, c.shift, c.baud), o)), text) <= 1);
    }
}
TEST(rtty_reverse_polarity) {
    const std::string text = "RYRYRY DE M0OML";
    RttyOptions o; o.reverse = true; CHECK_EQ(trim(run(synth_rtty(text + " ", 2125, 170, 45.45, 48000, 0.5, 0.5, true), o)), text);
    CHECK(edit(trim(run(synth_rtty(text + " ", 2125, 170, 45.45, 48000, 0.5, 0.5, true))), text) > 3);                     // without reverse set it is garbage, not silently right
}
TEST(rtty_survives_noise) {
    const std::string text = "CQ CQ DE M0OML K";
    auto x = synth_rtty(text + " ", 2125, 170, 45.45, 48000, 0.5, 1.0);
    for (double rms : {0.3, 0.8}) { auto y = x; add_noise(y, rms, 9); CHECK(edit(trim(run(y)), text) <= (rms < 0.5 ? 0 : 3)); }
}
TEST(rtty_tolerates_a_few_hertz_of_mistuning) {
    const std::string text = "CQ DE M0OML K";
    for (double off : {-10.0, -5.0, 5.0, 10.0}) CHECK(edit(trim(run(synth_rtty(text + " ", 2125 + off, 170, 45.45))), text) <= 1);
}
TEST(rtty_is_independent_of_chunking) {
    auto x = synth_rtty("CQ DE M0OML K 599 ", 2125, 170, 45.45); const auto whole = run(x, {}, x.size());
    for (size_t chunk : {size_t(1), size_t(777), size_t(4800)}) CHECK_EQ(run(x, {}, chunk), whole);
}
TEST(rtty_prints_nothing_for_silence_or_noise_and_reports_status) {
    std::vector<float> silence(48000 * 6, 0.0f); CHECK_EQ(run(silence), std::string());
    std::vector<float> noise(48000 * 20, 0.0f); add_noise(noise, 0.5, 4); CHECK(run(noise).size() <= 3);
    RttyDecoder d; std::string out; auto x = synth_rtty("RYRYRYRYRYRY ", 2125, 170, 45.45, 48000, 0.5, 2.0); d.process(x.data(), x.size(), out); CHECK(d.signal_present()); CHECK_EQ(d.framing_errors(), size_t(0)); CHECK(d.status().find("45.45 baud") != std::string::npos);
    d.reset(); CHECK(!d.signal_present());
}
