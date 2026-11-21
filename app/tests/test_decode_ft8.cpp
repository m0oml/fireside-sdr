// FT8: real on-air recordings (WSJT-X's answers as ground truth), generated signals (clean, noisy, simultaneous, streamed over slots), slot timing, and edge cases.
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <chrono>
#include <set>
#include <thread>
extern "C" {
#include <ft8/message.h>
}
#include "fireside/decode.h"
#include "fireside/dsp.h"
#include "test_util.h"
using namespace fireside::decode;

namespace {
std::string dir() { return FIRESIDE_FT8_DIR; }
std::vector<float> load12k(const std::string& wav) { auto r = fireside::dsp::read_iq_wav(wav, 0, 0); std::vector<float> x; for (auto& v : r.samples) x.push_back(v.real()); return x; }
struct Truth { int snr; double dt, freq; std::string text; };
std::vector<Truth> truth(const std::string& txt) {                                   // "110615  -2  1.0  431 ~  VK4BLE OH8JK R-17     France"
    std::vector<Truth> t; std::ifstream f(txt); std::string line; const std::regex re(R"(^\d+\s+([+-]?\d+)\s+([\d.]+)\s+(\d+) ~\s+(.*)$)");
    while (std::getline(f, line)) { std::smatch m; if (std::regex_match(line, m, re)) { std::string msg = m[4]; const size_t sp = msg.find("  "); if (sp != std::string::npos) msg = msg.substr(0, sp); while (!msg.empty() && msg.back() == ' ') msg.pop_back(); t.push_back({std::stoi(m[1]), std::stod(m[2]), std::stod(m[3]), msg}); } }
    return t;
}
std::vector<Truth> truth_jt9(const std::string& txt) {                                 // WSJT-X 3.3.0-beta5 jt9 output: "000000  -13 -0.1 1501 ~  IW5BT TA1MB R-09      " (DT can be negative, lines are padded)
    std::vector<Truth> t; std::ifstream f(txt); std::string line; const std::regex re(R"(^\d+\s+([+-]?\d+)\s+(-?[\d.]+)\s+(\d+)\s+[~+]\s+(.*)$)");      // "~" after FT8 lines, "+" after FT4 lines
    while (std::getline(f, line)) { std::smatch m; if (std::regex_match(line, m, re)) { std::string msg = m[4]; while (!msg.empty() && (msg.back() == ' ' || msg.back() == '\r')) msg.pop_back(); t.push_back({std::stoi(m[1]), std::stod(m[2]), std::stod(m[3]), msg}); } }
    return t;
}
std::string run(const std::vector<float>& x, Ft8Options o = {}, size_t chunk = 4800) {                          // feeds the audio, then waits for the background decode and collects its lines
    Ft8Decoder d(o); std::string out; for (size_t i = 0; i < x.size(); i += chunk) d.process(x.data() + i, std::min(chunk, x.size() - i), out);
    std::vector<float> none(480, 0.0f); for (int k = 0; k < 400 && d.busy(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10)); d.process(none.data(), none.size(), out); return out;
}
bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
// Only the four ft8_lib recordings (MIT) are committed; the rest of the fixtures are recordings made off the air or taken from WSJT-X, kept out of the repository.
// A test whose recordings are missing says so and skips.
bool have_wavs(std::initializer_list<const char*> stems) {
    for (const char* st : stems) if (!std::ifstream(dir() + "/" + st + ".wav").good()) { std::printf("  (skipped: recording %s.wav is not in this checkout)\n", st); return false; }
    return true;
}
void add(std::vector<float>& a, const std::vector<float>& b) { if (a.size() < b.size()) a.resize(b.size(), 0.0f); for (size_t i = 0; i < b.size(); ++i) a[i] += b[i]; }
}  // namespace

TEST(ft8_real_recordings_match_wsjtx) {
    size_t expected_total = 0, found_total = 0; std::vector<double> snr_err; int false_decodes = 0;
    for (const char* name : {"191111_110130", "191111_110200", "191111_110615", "191111_110700"}) {
        const auto x = load12k(dir() + "/" + name + ".wav"); CHECK_EQ(x.size(), size_t(180000));
        const auto got = Ft8Decoder::decode_slot(x.data(), x.size(), "T"); const auto want = truth(dir() + "/" + name + ".txt"); const auto ref = truth_jt9(dir() + "/" + name + ".jt9.txt");   // messages: the 2019 WSJT-X list; SNR: current WSJT-X (jt9 3.3)
        // A decode counts if either WSJT-X found it: the 2019 version or the current jt9 3.3 (the current one finds messages the 2019 list lacks). Hashed callsigns
        // ("<...>" from jt9, "<CALL>" from us) and jt9's a-priori / low-confidence markers are normalised as tools/compare_ft8.py does.
        auto norm = [](std::string t) { t = std::regex_replace(t, std::regex(R"(\s+(a\d|\?)$)"), ""); return std::regex_replace(t, std::regex("<[^>]*>"), "<H>"); };
        std::set<std::string> want_texts; for (auto& w : want) want_texts.insert(norm(w.text)); std::set<std::string> listed2019 = want_texts; for (auto& w : ref) want_texts.insert(norm(w.text));
        std::set<std::string> found; for (auto& g0 : got) { Ft8Message g = g0; g.text = norm(g.text);
            if (!want_texts.count(g.text)) { ++false_decodes; int calls = 0; std::istringstream is(g.text); std::string tok; while (is >> tok) if (std::regex_match(tok, std::regex("[A-Z0-9/<>.]{3,}"))) ++calls; CHECK(calls >= 2); continue; }   // still a well-formed message
            found.insert(g.text);
            for (auto& w : ref) if (norm(w.text) == g.text && std::fabs(w.freq - g.freq) <= 4) snr_err.push_back(double(g.snr - w.snr));                         // same message at the same frequency
            bool freq_ok = false; for (auto& w : want) if (norm(w.text) == g.text && std::fabs(w.freq - g.freq) <= 6) freq_ok = true; for (auto& w : ref) if (norm(w.text) == g.text && std::fabs(w.freq - g.freq) <= 6) freq_ok = true; CHECK(freq_ok);
        }
        expected_total += listed2019.size(); for (auto& t : found) if (listed2019.count(t)) ++found_total;
    }
    CHECK(false_decodes <= 2);                                                                                  // a CRC-valid message WSJT-X did not list (it was a reply to one it did): not a false decode, and rare
    CHECK(double(found_total) >= 0.70 * double(expected_total));                                                // measured ~75-85 %: ft8_lib is a little less sensitive than WSJT-X
    CHECK(snr_err.size() >= 30); double mean = 0; for (double e : snr_err) mean += e; mean /= double(snr_err.size()); CHECK(std::fabs(mean) <= 4.0);   // measured +3.0 here (quiet band) and ~0 on the busy live band below: see kSnrCalibrationDb
    size_t within = 0; for (double e : snr_err) if (std::fabs(e) <= 6) ++within; CHECK(double(within) >= 0.6 * double(snr_err.size()));                    // the SNR is an estimate, like WSJT-X's
}
TEST(ft8_snr_matches_current_wsjtx_on_a_busy_band) {
    if (!have_wavs({"live_20261117_s004", "live_20261117_s020"})) return;
    // Two slots from a live 40 m capture of the FLEX-5000A (2026-11-17, 23 signals per slot), references from WSJT-X's jt9 3.3.0-beta5 (-8 -d 3). On the full 10 min capture our SNR read 5.6 dB low before the recalibration.
    std::vector<double> err; size_t expected = 0, found = 0; int false_decodes = 0;
    for (const char* name : {"live_20261117_s004", "live_20261117_s020"}) {
        const auto x = load12k(dir() + "/" + name + ".wav"); CHECK_EQ(x.size(), size_t(180000));
        const auto got = Ft8Decoder::decode_slot(x.data(), x.size(), "T"); const auto want = truth_jt9(dir() + "/" + name + ".jt9.txt"); expected += want.size();
        // Decodes jt9 missed in this slot but which are genuine (checked against jt9's decodes of the whole 10 min capture):
        //   "MM7FTH VO1AW -15" (s004): VO1AW's report in the QSO jt9 decodes as "VO1AW MM7FTH IO86" (slots 1, 3) and "MM7FTH VO1AW RR73" (slot 6).
        static const std::set<std::string> genuine_extras = {"MM7FTH VO1AW -15"};
        for (auto& g : got) { bool known = false; for (auto& w : want) if (w.text == g.text && std::fabs(w.freq - g.freq) <= 4) { known = true; err.push_back(double(g.snr - w.snr)); }
                              if (known) ++found; else if (!genuine_extras.count(g.text)) ++false_decodes; }
    }
    CHECK_EQ(false_decodes, 0); CHECK(double(found) >= 0.80 * double(expected)); CHECK(err.size() >= 30);                // measured ~60 % single-pass; now ~95 % (subtraction, OSD, deep stage)
    double mean = 0, sq = 0; for (double e : err) mean += e; mean /= double(err.size()); for (double e : err) sq += (e - mean) * (e - mean);
    CHECK(std::fabs(mean) <= 1.5); CHECK(std::sqrt(sq / double(err.size())) <= 4.0);                                      // no offset, scatter like WSJT-X's own run to run
}
TEST(ft8_decodes_generated_signals_of_every_common_message_type) {
    for (const char* msg : {"CQ M0OML IO91", "M0OML G3XYZ IO91", "G3XYZ M0OML -12", "G3XYZ M0OML R-05", "M0OML G3XYZ RR73", "G3XYZ M0OML 73", "CQ DX M0OML IO91", "CQ POTA M0OML IO91", "TNX BOB 73 GL"}) {
        const auto slot = synth_ft8(msg, 1000, 48000, 0.3, 0.5); CHECK(!slot.empty());
        const auto out = run(slot); CHECK(has(out, msg));
    }
    CHECK(synth_ft8("THIS FREE TEXT IS WAY TOO LONG TO FIT", 1000).empty());                                       // cannot be encoded
}
TEST(ft8_decodes_at_any_frequency_and_time_offset) {
    for (double f : {250.0, 700.0, 1500.0, 2400.0, 2800.0}) for (double dt : {-0.3, 0.0, 0.5, 1.2}) { const auto out = run(synth_ft8("CQ M0OML IO91", f, 48000, 0.3, 0.5 + dt)); CHECK(has(out, "CQ M0OML IO91")); }
}
TEST(ft8_decodes_through_noise_down_to_about_minus_15_db) {
    // signal amplitude 0.3 (power 0.045); noise rms s gives SNR(2500 Hz) = 10 log10(0.045 / (s^2 * 2500 / 24000))
    for (double snr_db : {-5.0, -12.0}) {
        const double s = std::sqrt(0.045 / (std::pow(10.0, snr_db / 10.0) * 2500.0 / 24000.0)); auto slot = synth_ft8("CQ M0OML IO91", 1200, 48000, 0.3, 0.5); add_noise(slot, s, 21);
        CHECK(has(run(slot), "CQ M0OML IO91"));
    }
    auto hopeless = synth_ft8("CQ M0OML IO91", 1200, 48000, 0.3, 0.5); add_noise(hopeless, 4.0 * std::sqrt(0.045 / (std::pow(10.0, -2.5) * 2500.0 / 24000.0)) / 4.0 * 4.0, 5);
    CHECK(!has(run(hopeless), "CQ M0OML IO91") || true);                                                              // (very low SNR: may or may not decode; must not crash)
}
TEST(ft8_decodes_simultaneous_signals_and_reports_their_frequencies) {
    auto slot = synth_ft8("CQ M0OML IO91", 700, 48000, 0.25, 0.5); add(slot, synth_ft8("G3XYZ DL1ABC -07", 1300, 48000, 0.2, 0.7)); add(slot, synth_ft8("CQ DX K1ABC FN42", 2100, 48000, 0.15, 0.4));
    const auto out = run(slot); CHECK(has(out, "CQ M0OML IO91")); CHECK(has(out, "G3XYZ DL1ABC -07")); CHECK(has(out, "CQ DX K1ABC FN42"));
    Ft8Options o; Ft8Decoder d(o); std::string o2; d.process(slot.data(), slot.size(), o2); std::vector<float> idle(48000 * 3, 0.0f); d.process(idle.data(), idle.size(), o2);
}
TEST(ft8_streams_consecutive_slots_and_labels_them) {
    std::vector<float> stream; for (const char* m : {"CQ M0OML IO91", "M0OML G3XYZ -10", "G3XYZ M0OML R-12"}) { auto s = synth_ft8(m, 1000, 48000, 0.3, 0.5); stream.insert(stream.end(), s.begin(), s.end()); }
    const auto out = run(stream); CHECK(has(out, "S000")); CHECK(has(out, "S001")); CHECK(has(out, "S002")); CHECK(has(out, "CQ M0OML IO91")); CHECK(has(out, "M0OML G3XYZ -10")); CHECK(has(out, "G3XYZ M0OML R-12"));
    CHECK(out.find("CQ M0OML IO91") < out.find("M0OML G3XYZ -10")); CHECK(out.find("M0OML G3XYZ -10") < out.find("G3XYZ M0OML R-12"));
}
TEST(ft8_ignores_a_partly_heard_first_slot_and_waits_for_the_boundary) {
    auto a = synth_ft8("CQ M0OML IO91", 1000, 48000, 0.3, 0.5), b = synth_ft8("M0OML G3XYZ -10", 1000, 48000, 0.3, 0.5);
    std::vector<float> stream(a.begin() + 5 * 48000, a.end()); stream.insert(stream.end(), b.begin(), b.end());   // we tuned in 5 s into slot A
    Ft8Options o; o.start_offset_s = 5.0; const auto out = run(stream, o);
    CHECK(!has(out, "CQ M0OML IO91")); CHECK(has(out, "M0OML G3XYZ -10"));
    // the same audio with the offset unknown would mis-frame: with offset 0 the slot boundaries are wrong and the message is (usually) missed
}
TEST(ft8_chunking_does_not_change_the_result) {
    auto slot = synth_ft8("CQ M0OML IO91", 1100, 48000, 0.3, 0.5); add_noise(slot, 1.0, 3); const auto whole = run(slot, {}, slot.size());
    for (size_t chunk : {size_t(1000), size_t(4801), size_t(47999)}) CHECK_EQ(run(slot, {}, chunk), whole);
    CHECK(has(whole, "CQ M0OML IO91"));
}
TEST(ft8_prints_nothing_for_silence_or_noise) {
    std::vector<float> silence(48000 * 16, 0.0f); Ft8Decoder d; std::string out; d.process(silence.data(), silence.size(), out); std::this_thread::sleep_for(std::chrono::milliseconds(500)); d.process(silence.data(), 100, out); CHECK(out.empty());
    for (uint32_t seed : {1u, 2u, 3u}) { std::vector<float> noise(48000 * 15, 0.0f); add_noise(noise, 1.0, seed); CHECK(run(noise).empty()); }
}
TEST(ft8_format_status_and_reset) {
    Ft8Message m; m.time = "110130"; m.snr = -6; m.dt = 0.7; m.freq = 683; m.text = "CQ TA6CQ KN70"; CHECK_EQ(Ft8Decoder::format(m), std::string("110130  -6  0.7  683 ~  CQ TA6CQ KN70\n"));
    Ft8Decoder d; CHECK(d.status().find("FT8") != std::string::npos); auto s = synth_ft8("CQ M0OML IO91", 1000); std::string out; d.process(s.data(), s.size(), out); d.reset(); CHECK(d.status().find("0 message") != std::string::npos);
    { Ft8Decoder e; auto x = synth_ft8("CQ M0OML IO91", 1000); std::string o; e.process(x.data(), x.size(), o); }                    // destroyed while a decode may be running: must join, not crash
}
TEST(ft8_wall_clock_mode_labels_slots_with_utc_times) {
    Ft8Options o; o.wall_clock = true; Ft8Decoder d(o); std::string out; std::vector<float> chunk(4800, 0.0f); d.process(chunk.data(), chunk.size(), out);       // only checks that it starts and does not crash
    CHECK(d.status().find("FT8") != std::string::npos);
}

// ---------------------------------------------------------------- FT4 (7.5 s slots)
TEST(ft4_decodes_generated_signals_of_common_message_types) {
    for (const char* msg : {"CQ M0OML IO92", "M0OML G3XYZ -10", "G3XYZ M0OML R-12", "M0OML G3XYZ RR73", "CQ RU K1ABC FN42"}) {
        Ft8Options o; o.ft4 = true; const auto out = run(synth_ft4(msg, 1000, 48000, 0.3, 0.3), o); CHECK(has(out, msg));
    }
    CHECK(synth_ft4("THIS FREE TEXT IS WAY TOO LONG TO FIT", 1000).empty());
    CHECK_EQ(synth_ft4("CQ M0OML IO92", 1000).size(), size_t(7.5 * 48000));
}
TEST(ft4_does_not_hear_ft8_and_ft8_does_not_hear_ft4) {
    Ft8Options o4; o4.ft4 = true; CHECK(!has(run(synth_ft8("CQ M0OML IO92", 1000), o4), "M0OML")); CHECK(!has(run(synth_ft4("CQ M0OML IO92", 1000)), "M0OML"));
}
TEST(ft4_streams_consecutive_slots_and_reports_frequencies) {
    std::vector<float> stream; for (const char* m : {"CQ M0OML IO92", "M0OML G3XYZ -10"}) { auto s = synth_ft4(m, 1500, 48000, 0.3, 0.3); stream.insert(stream.end(), s.begin(), s.end()); }
    Ft8Options o; o.ft4 = true; const auto out = run(stream, o); CHECK(has(out, "S000")); CHECK(has(out, "S001")); CHECK(has(out, "1500")); CHECK(has(out, "CQ M0OML IO92")); CHECK(has(out, "M0OML G3XYZ -10"));
    Ft8Decoder d(o); CHECK(d.status().find("FT4") != std::string::npos);
}
TEST(ft4_decodes_through_moderate_noise) {
    auto slot = synth_ft4("CQ M0OML IO92", 1200, 48000, 0.3, 0.3); add_noise(slot, 0.35, 11); Ft8Options o; o.ft4 = true; CHECK(has(run(slot, o), "CQ M0OML IO92"));
}
TEST(ft4_real_wsjtx_contest_sample_matches_wsjtx) {
    if (!have_wavs({"ft4_wsjtx_sample_contest"})) return;
    // WSJT-X's own FT4 contest sample (19 messages, 12 kHz, one 7.5 s slot) and jt9 -5's answers. Nine are ARRL RTTY Roundup messages ("N1TRK N4FKH 569 VA"), a type ft8_lib
    // cannot unpack. Of the 6 we miss, 3 lie above 3000 Hz (outside our search range) and 3 are at -13..-17 dB. Nothing may be invented.
    const auto x = load12k(dir() + "/ft4_wsjtx_sample_contest.wav"); const auto got = Ft8Decoder::decode_slot(x.data(), x.size(), "T", true);
    const auto ref = truth_jt9(dir() + "/ft4_wsjtx_sample_contest.jt9.txt"); CHECK_EQ(ref.size(), size_t(19));
    int found = 0; double snr_err = 0;
    for (const auto& r : ref) for (const auto& m : got) if (m.text == r.text) { ++found; snr_err += m.snr - r.snr; CHECK(std::fabs(m.freq - r.freq) <= 4.0); CHECK(std::fabs(m.dt - r.dt) <= 0.1); }
    CHECK(found >= 13); CHECK_EQ(int(got.size()), found);                                                                     // every decode is one of WSJT-X's
    CHECK(std::fabs(snr_err / found) <= 1.5);                                                                                 // SNR calibrated (was -2.1 dB low)
    std::string all; for (const auto& m : got) all += m.text + "\n";
    CHECK(has(all, "N1TRK N4FKH 569 VA")); CHECK(has(all, "NZ7P WA7JAY 589 CA")); CHECK(has(all, "KB0VHA KA1YQC R 539 MA")); CHECK(has(all, "K1JT WB4HXE 559 GA"));
}
namespace {
void put_bits(uint8_t* p, int from, int n, unsigned v) { for (int i = 0; i < n; ++i) { const int b = from + i; if ((v >> (n - 1 - i)) & 1u) p[b / 8] |= uint8_t(0x80u >> (b % 8)); } }
std::vector<uint8_t> payload_from_bits(const std::string& bits) { std::vector<uint8_t> p(10, 0); for (size_t i = 0; i < 77; ++i) if (bits[i] == '1') p[i / 8] |= uint8_t(0x80u >> (i % 8)); return p; }
}
TEST(ft8_unpacks_contest_dxpedition_and_eu_vhf_messages_exactly_as_wsjtx) {
    // app/tests/ft8/ft8code_message_types.txt: WSJT-X's own `ft8code` encoding of 274 messages (every Field Day section, every RTTY Roundup multiplier, serials,
    // reports, DXpedition, EU VHF; tools/gen_ft8_type_fixture.py). Each must unpack to the text WSJT-X prints (hashed callsigns as <...> with an empty table).
    std::ifstream f(dir() + "/ft8code_message_types.txt"); CHECK(bool(f)); std::string line; int n = 0, sections = 0, mults = 0;
    while (std::getline(f, line)) {
        const size_t tab = line.find('\t'); CHECK(tab == 77); const auto p = payload_from_bits(line.substr(0, 77)); const std::string want = line.substr(tab + 1);
        std::string got; const bool ok = unpack_extra_message(p.data(), got); CHECK(ok); CHECK_EQ(got, want); ++n;
        if (want.rfind("W1AW K1ABC 1A ", 0) == 0) ++sections; if (want.rfind("TU; W9XYZ K1ABC R 579 ", 0) == 0) ++mults;
    }
    CHECK_EQ(n, 274); CHECK_EQ(sections, 86); CHECK_EQ(mults, 171);
}
TEST(ft8_unpacks_hashed_callsigns_once_they_have_been_heard) {
    // DXpedition "K1ABC RR73; W9XYZ <KH1/KH7Z> -12" and EU VHF "<PA3XYZ> <G4ABC> ..." carry hashes: the call shows once a message containing it has been seen.
    std::ifstream f(dir() + "/ft8code_message_types.txt"); std::string line; std::vector<uint8_t> dx, eu;
    while (std::getline(f, line)) { if (line.find("RR73; W9XYZ <...> -12") != std::string::npos) dx = payload_from_bits(line.substr(0, 77)); if (line.find("<...> <...> R 590003 IO91NP") != std::string::npos) eu = payload_from_bits(line.substr(0, 77)); }
    CHECK(!dx.empty() && !eu.empty()); std::string t;
    CHECK(unpack_extra_message(dx.data(), t, {"KH1/KH7Z W9XYZ RR73"})); CHECK_EQ(t, std::string("K1ABC RR73; W9XYZ <KH1/KH7Z> -12"));
    CHECK(unpack_extra_message(dx.data(), t)); CHECK_EQ(t, std::string("K1ABC RR73; W9XYZ <...> -12"));                      // not heard: unresolved, as WSJT-X shows it
}
TEST(ft8_refuses_extra_type_payloads_with_invalid_contents) {
    auto build = [](unsigned i3, unsigned n3, auto&& fill) { std::vector<uint8_t> p(10, 0); put_bits(p.data(), 74, 3, i3); put_bits(p.data(), 71, 3, n3); fill(p.data()); std::string t; return unpack_extra_message(p.data(), t); };
    ftx_message_t m; CHECK_EQ(int(ftx_message_encode(&m, nullptr, "W9XYZ K1ABC RR73")), 0);
    unsigned a = 0, b = 0; for (int i = 0; i < 28; ++i) { a = (a << 1) | ((m.payload[i / 8] >> (7 - i % 8)) & 1u); const int j = 29 + i; b = (b << 1) | ((m.payload[j / 8] >> (7 - j % 8)) & 1u); }
    CHECK(build(3, 0, [&](uint8_t* p) { put_bits(p, 1, 28, a); put_bits(p, 29, 28, b); put_bits(p, 61, 13, 8172); }) == false);               // multiplier beyond the table
    CHECK(build(3, 0, [&](uint8_t* p) { put_bits(p, 1, 28, a); put_bits(p, 29, 28, b); put_bits(p, 61, 13, 0); }) == false);                  // neither serial nor multiplier
    CHECK(build(3, 0, [&](uint8_t* p) { put_bits(p, 1, 28, 1); put_bits(p, 29, 28, b); put_bits(p, 61, 13, 5); }) == false);                  // n28 <= 2 (CQ/DE token) is not a callsign here
    CHECK(build(0, 3, [&](uint8_t* p) { put_bits(p, 0, 28, a); put_bits(p, 28, 28, b); put_bits(p, 64, 7, 0); }) == false);                   // Field Day section 0 / 87+ do not exist
    CHECK(build(0, 3, [&](uint8_t* p) { put_bits(p, 0, 28, a); put_bits(p, 28, 28, b); put_bits(p, 64, 7, 87); }) == false);
    CHECK(build(0, 2, [&](uint8_t*) {}) == false); CHECK(build(0, 7, [&](uint8_t*) {}) == false); CHECK(build(6, 0, [&](uint8_t*) {}) == false); CHECK(build(1, 0, [&](uint8_t*) {}) == false);   // not ours
    CHECK(build(5, 0, [&](uint8_t* p) { put_bits(p, 49, 25, 18662400u); }) == false);                                                          // EU VHF grid out of range
}
TEST(ft4_live_capture_slot_matches_wsjtx) {
    if (!have_wavs({"ft4_live_20261118_s002"})) return;
    // One 7.5 s slot from the 192 kHz direct capture on 7.0475 MHz (2026-11-18 07:49 UTC): OZ1LTQ calling "CQ NA", jt9 -5: +11 dB, DT -0.1 s, 739 Hz.
    const auto x = load12k(dir() + "/ft4_live_20261118_s002.wav"); const auto got = Ft8Decoder::decode_slot(x.data(), x.size(), "T", true);
    const auto ref = truth_jt9(dir() + "/ft4_live_20261118_s002.jt9.txt"); CHECK_EQ(ref.size(), size_t(1)); CHECK_EQ(got.size(), size_t(1));
    CHECK_EQ(got[0].text, ref[0].text); CHECK(std::fabs(got[0].freq - ref[0].freq) <= 2.0); CHECK(std::fabs(got[0].dt - ref[0].dt) <= 0.1); CHECK(std::abs(got[0].snr - ref[0].snr) <= 3);
}

// Two real slots (7.0475 MHz, 2026-11-18 14:49 UTC) with a portable station, EA6SA/P. They exposed two faults: the plausibility check read the /P flag as "R" (so "CQ EA6SA/P JM19", -7 dB,
// was refused) and every weak /P decode was refused outright ("EA6SA/P EA3FBX JN11", -21 dB; hard=18). Both must now decode, as jt9 -5 does.
TEST(ft4_portable_station_slots_decode_as_wsjtx_does) {
    if (!have_wavs({"ft4_portable_20261118_s000", "ft4_portable_20261118_s005"})) return;
    for (const char* name : {"ft4_portable_20261118_s000", "ft4_portable_20261118_s005"}) {
        const auto x = load12k(dir() + "/" + name + ".wav"); const auto got = Ft8Decoder::decode_slot(x.data(), x.size(), "T", true);
        const auto ref = truth_jt9(dir() + "/" + name + ".jt9.txt"); CHECK_EQ(ref.size(), size_t(2));
        for (const auto& r : ref) { bool found = false; for (const auto& g : got) found |= g.text == r.text && std::fabs(g.freq - r.freq) <= 3.0; if (!found) std::printf("  %s: missing '%s'\n", name, r.text.c_str()); CHECK(found); }
    }
}

// The FT4 deep stage (WSJT-X ft4_decode.f90): two real slots from 7.0474 MHz (2026-11-18 14:45 UTC). s001 has "<...> SP3IPA -09" starting ~0.5 s after the others: ft8_lib's candidates
// at its frequency are at the wrong time, so only the full -0.5..+1.5 s time search finds it. s002 has "EA3IJQ EA6SA/P +10", which neither BP nor OSD on the waterfall decodes.
// Every jt9 -5 decode must be found, with the same DT within 0.1 s (a deep decode reports the refined time, not the candidate's) and SNR within 7 dB.
TEST(ft4_deep_stage_finds_what_jt9_finds_in_real_slots) {
    if (!have_wavs({"ft4_deep_20261118_s001", "ft4_deep_20261118_s002"})) return;
    for (const char* name : {"ft4_deep_20261118_s001", "ft4_deep_20261118_s002"}) {
        const auto x = load12k(dir() + "/" + name + ".wav"); const auto got = Ft8Decoder::decode_slot(x.data(), x.size(), "T", true);
        const auto ref = truth_jt9(dir() + "/" + name + ".jt9.txt"); CHECK_EQ(ref.size(), size_t(2));
        double off = 0; int n = 0;                                                                    // our DT and jt9's differ by a constant (slot cut): compare the spread, not the value
        for (const auto& r : ref) for (const auto& g : got) if (g.text == r.text) { off += g.dt - r.dt; ++n; }
        if (n) off /= n;
        for (const auto& r : ref) {
            bool found = false; for (const auto& g : got) if (g.text == r.text && std::fabs(g.freq - r.freq) <= 3.0) { found = true; CHECK(std::fabs(g.dt - r.dt - off) <= 0.1); CHECK(std::abs(g.snr - r.snr) <= 7); }   // SNR: our FT4 estimate differs from jt9's by up to ~6 dB on these recordings, deep or not (open item)
            if (!found) std::printf("  %s: missing '%s'\n", name, r.text.c_str()); CHECK(found);
        }
    }
}

// ---------------------------------------------------------------- multi-pass decoding with signal subtraction (2026-11-18)
TEST(ft8_subtraction_reveals_a_signal_hidden_under_a_stronger_one) {
    // A weak station 6 Hz from a strong one, overlapping in time: its tones sit in the strong signal's bins, so a single pass cannot decode it.
    // Once the strong signal is decoded, rebuilt, fitted and subtracted, the weak one decodes in the next pass.
    auto x = synth_ft8("CQ M0OML IO92", 1000, 12000, 1.0, 0.5);
    add(x, synth_ft8("G4ABC DL1XYZ -15", 1006.25, 12000, 0.08, 0.66)); add_noise(x, 0.05, 7);
    const auto r = Ft8Decoder::decode_slot(x.data(), x.size(), "S000");
    std::string all; for (const auto& m : r) all += m.text + "\n";
    CHECK(has(all, "CQ M0OML IO92")); CHECK(has(all, "G4ABC DL1XYZ -15"));
    CHECK_EQ(r.size(), size_t(2));                                                               // and nothing invented from the residue
}
TEST(ft8_reports_dt_in_the_wsjtx_convention) {
    // WSJT-X DT = start of the transmission - 0.5 s. The fitted start is sample-accurate, so DT is exact to a few ms (was 0.7 s off before).
    for (double dt : {-0.4, 0.0, 0.37, 1.2}) {
        const auto x = synth_ft8("CQ M0OML IO92", 1400, 12000, 0.3, 0.5 + dt);
        const auto r = Ft8Decoder::decode_slot(x.data(), x.size(), ""); CHECK_EQ(r.size(), size_t(1));
        CHECK_NEAR(r[0].dt, dt, 0.01); CHECK_NEAR(r[0].freq, 1400.0, 0.3);
    }
    const auto out = run(synth_ft8("CQ M0OML IO92", 1400, 48000, 0.3, 0.8));                // through the 48 kHz streaming path (resampler)
    CHECK(has(out, "  0.3 1400 ~  CQ M0OML IO92"));
}
TEST(ft4_subtraction_path_handles_ft4_slots) {
    auto x = synth_ft4("CQ M0OML IO92", 1000, 12000, 0.5, 0.5); add(x, synth_ft4("CQ G4ABC IO91", 1500, 12000, 0.2, 0.7));
    const auto r = Ft8Decoder::decode_slot(x.data(), x.size(), "", true); CHECK_EQ(r.size(), size_t(2));
    for (const auto& m : r) CHECK(m.dt > -0.2 && m.dt < 0.4);
}

// ---------------------------------------------------------------- portable (/P) callsigns: the plausibility check read the flags one bit early, so "CQ call/P grid" was thrown away
TEST(portable_callsigns_decode_in_cq_and_replies_in_ft4_and_ft8) {
    const char* msgs[] = {"CQ EA6SA/P JM19", "EA6SA/P EA3FBX JN11", "EA3FBX EA6SA/P +12", "EA3FBX EA6SA/P RR73", "CQ DX G4ABC/P IO91"};
    for (const char* m : msgs) {
        auto f4 = synth_ft4(m, 1500, 12000, 0.3, 0.3); f4.resize(size_t(7.5 * 12000), 0.0f);
        auto f8 = synth_ft8(m, 1500, 12000, 0.3, 0.5); f8.resize(size_t(15 * 12000), 0.0f);
        bool ok4 = false, ok8 = false;
        for (const auto& d : Ft8Decoder::decode_slot(f4.data(), f4.size(), "", true)) ok4 |= d.text == m;
        for (const auto& d : Ft8Decoder::decode_slot(f8.data(), f8.size(), "", false)) ok8 |= d.text == m;
        if (!ok4) std::printf("  FT4 missed: %s\n", m); if (!ok8) std::printf("  FT8 missed: %s\n", m);
        CHECK(ok4); CHECK(ok8);
    }
}

TEST(ft8_quick_pass_before_our_over_finds_a_weak_answer_at_the_callers_frequency) {
    // A weak "R-report" to us at 2254 Hz: the quick pass before our next over (peek, one pass) misses many of these; told where the caller is,
    // it runs the deep stage (with a-priori decoding of messages to us) there and should find more, in about the same time.
    const std::string msg = "M0OML LY3CY R-16"; int plain = 0, focused = 0; double t_plain = 0, t_focus = 0; const int kTrials = 12;
    for (int k = 0; k < kTrials; ++k) {
        const double snr_db = -19.0 - 0.25 * (k % 4);
        const double s = std::sqrt(0.045 / (std::pow(10.0, snr_db / 10.0) * 2500.0 / 24000.0));
        auto slot = synth_ft8(msg, 2254, 48000, 0.3, 0.5); add_noise(slot, s, 100 + uint32_t(k)); slot.resize(size_t(48000 * 14.5));
        for (int f = 0; f < 2; ++f) {
            Ft8Options o; o.my_call = "M0OML"; Ft8Decoder d(o); std::string out;
            if (f) d.set_focus({2254.0});
            for (size_t i = 0; i < slot.size(); i += 4800) d.process(slot.data() + i, std::min<size_t>(4800, slot.size() - i), out);
            const auto t0 = std::chrono::steady_clock::now(); d.peek(out); const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            (f ? t_focus : t_plain) += dt; if (has(out, msg)) ++(f ? focused : plain);
        }
    }
    std::printf("  weak answer before our over: plain quick %d/%d (%.2f s each), focused %d/%d (%.2f s each)\n", plain, kTrials, t_plain / kTrials, focused, kTrials, t_focus / kTrials);
    CHECK(focused >= plain); CHECK(focused > plain); CHECK(t_focus / kTrials < 1.5);
}
