// WSPR: the encoder, the signal, wsprd's output, the wsprnet request bodies, the slot timing and the decoder's slot handling.
// No radio. The round trip through the real wsprd runs only when WSJT-X is installed (skipped otherwise).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <unistd.h>
#include "fireside/tx_dsp.h"
#include "fireside/wspr.h"
#include "fireside/wspr_decode.h"
#include "test_util.h"

using namespace fireside;

namespace {
std::string temp_dir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / (std::string(name) + "_" + std::to_string(::getpid())); std::filesystem::create_directories(d); return d.string();
}
// Several messages at -20 dB SNR in 2500 Hz (12 kHz, 114 s), as wsprd sees a slot
std::vector<float> slot_audio(const std::vector<std::tuple<std::string, std::string, int, double>>& msgs, double snr_db, unsigned seed) {
    std::vector<float> a(size_t(114 * 12000), 0.0f);
    for (const auto& [c, g, d, f] : msgs) { const auto x = wspr::synth_audio(wspr::encode(c, g, d), f, 12000, 1.0, 1.0, 114); for (size_t i = 0; i < a.size(); ++i) a[i] += x[i]; }
    std::mt19937 rng(seed); std::normal_distribution<float> n(0, 1);
    const float sd = float(std::sqrt(0.5 / std::pow(10.0, snr_db / 10) * 6000 / 2500));   // signal power 0.5; noise in 2500 Hz snr_db below it
    float pk = 0; for (auto& v : a) { v += sd * n(rng); pk = std::max(pk, std::fabs(v)); }
    for (auto& v : a) v *= 0.5f / pk;
    return a;
}
}  // namespace

TEST(wspr_symbols_are_162_four_level_with_the_sync_vector_in_their_low_bit) {
    const auto s = wspr::encode("M0OML", "IO91", 37);
    for (int i = 0; i < wspr::kSymbols; ++i) { CHECK(s[size_t(i)] <= 3); CHECK_EQ(int(s[size_t(i)] & 1), int(wspr::kSync[i])); }
    CHECK(wspr::encode("m0oml", "io91", 37) == s);                                                       // case does not matter
    CHECK(wspr::encode("M0OML", "IO91", 30) != s); CHECK(wspr::encode("M0OML", "IO92", 37) != s); CHECK(wspr::encode("M0OMK", "IO91", 37) != s);
    int ones = 0; for (int i = 0; i < wspr::kSymbols; ++i) ones += wspr::kSync[i]; CHECK(ones > 60 && ones < 100);
    CHECK_NEAR(wspr::kSignalS, 110.592, 1e-3); CHECK_NEAR(wspr::kToneSpacingHz, 1.46484375, 1e-9);
}

TEST(wspr_refuses_what_a_type_1_message_cannot_carry) {
    for (const char* c : {"M0OML/P", "EA8/M0OML", "", "TOOLONG1", "ABCDEF", "M0-ML"}) CHECK_THROWS(wspr::encode(c, "IO91", 37), std::invalid_argument);
    for (const char* g : {"IO9", "IO91AB", "ZZ91", "1O91", ""}) CHECK_THROWS(wspr::encode("M0OML", g, 37), std::invalid_argument);
    for (int d : {-3, 1, 5, 36, 38, 61, 63}) CHECK_THROWS(wspr::encode("M0OML", "IO91", d), std::invalid_argument);
    for (const char* c : {"M0OML", "K1ABC", "VK2XYZ", "2E0ABC", "G4A", "K1A", "W1AW"}) CHECK(wspr::plain_call(c));
    CHECK(!wspr::plain_call("M0OML/P")); CHECK(!wspr::plain_call("ABCDEF"));
}

TEST(wspr_power_is_the_nearest_legal_dbm) {
    CHECK_EQ(wspr::dbm_for_watts(5), 37); CHECK_EQ(wspr::dbm_for_watts(1), 30); CHECK_EQ(wspr::dbm_for_watts(20), 43); CHECK_EQ(wspr::dbm_for_watts(10), 40);
    CHECK_EQ(wspr::dbm_for_watts(2), 33); CHECK_EQ(wspr::dbm_for_watts(0.2), 23);
    CHECK_NEAR(wspr::watts_for_dbm(37), 5.01, 0.01); CHECK_THROWS(wspr::dbm_for_watts(0), std::invalid_argument);
}

TEST(wspr_signal_is_constant_envelope_one_tone_per_symbol_at_the_right_place) {
    const auto s = wspr::encode("M0OML", "IO91", 37);
    const auto iq = wspr::synth_iq(s, 1500, 12000, 1.0, 120);
    CHECK_EQ(iq.size(), size_t(120 * 12000));
    CHECK_EQ(std::abs(iq[11999]), 0.0f); CHECK(std::abs(iq[12000 + 2000]) > 0.999f);                     // silent for the first second, then envelope 1
    const size_t end = 12000 + size_t(std::llround(wspr::kSignalS * 12000)); CHECK_EQ(std::abs(iq[end + 10]), 0.0f);
    for (int k : {0, 1, 50, 161}) {                                                                       // the instantaneous frequency mid-symbol is tone k
        const size_t mid = 12000 + size_t((k + 0.5) * 8192); const double dphi = std::arg(iq[mid + 1] * std::conj(iq[mid]));
        CHECK_NEAR(dphi * 12000 / (2 * M_PI), 1500 + wspr::kToneSpacingHz * s[size_t(k)], 0.01);
    }
}

TEST(wspr_wav_header_is_16_bit_mono_at_12_khz) {
    const auto b = wspr::wav16({0.0f, 0.5f, -1.0f, 2.0f});
    CHECK_EQ(b.size(), size_t(44 + 8)); CHECK(std::string(b.begin(), b.begin() + 4) == "RIFF"); CHECK(std::string(b.begin() + 8, b.begin() + 12) == "WAVE");
    CHECK_EQ(int(b[22]), 1); CHECK_EQ(int(b[24] | (b[25] << 8)), 12000); CHECK_EQ(int(b[34]), 16);
    CHECK_EQ(int16_t(b[46] | (b[47] << 8)), int16_t(16384)); CHECK_EQ(int16_t(b[48] | (b[49] << 8)), int16_t(-32767)); CHECK_EQ(int16_t(b[50] | (b[51] << 8)), int16_t(32767));   // clipped
}

TEST(wspr_reads_wsprd_lines_of_all_three_message_types) {
    auto a = wspr::parse_wsprd_line("1200 -20  0.1  14.097102  0  M0OML IO91 37 ");
    CHECK(a); CHECK_EQ(a->hhmm, std::string("1200")); CHECK_EQ(a->snr, -20); CHECK_NEAR(a->dt, 0.1, 1e-9); CHECK_NEAR(a->mhz, 14.097102, 1e-9); CHECK_EQ(a->drift, 0);
    CHECK_EQ(a->call, std::string("M0OML")); CHECK_EQ(a->grid, std::string("IO91")); CHECK_EQ(a->dbm, 37);
    auto b = wspr::parse_wsprd_line("0002 -27 -1.3   7.040123 -1  EA8/M0OML 23"); CHECK(b); CHECK_EQ(b->call, std::string("EA8/M0OML")); CHECK(b->grid.empty()); CHECK_EQ(b->dbm, 23); CHECK_EQ(b->drift, -1);
    auto c = wspr::parse_wsprd_line("2258 -21  0.3  14.097069  0  <K1ABC> FN42KM 37"); CHECK(c); CHECK_EQ(c->call, std::string("<K1ABC>")); CHECK_EQ(c->grid, std::string("FN42KM"));
    for (const char* bad : {"<DecodeFinished>", "", "1200 -20 0.1", "12x0 -20 0.1 14.0971 0 M0OML IO91 37", "1200 -20 0.1 14.0971 0 M0OML IO91 x37"}) CHECK(!wspr::parse_wsprd_line(bad));
    CHECK_EQ(wspr::format(*a), std::string("1200  -20  0.1  14.097102   0  M0OML IO91 37"));
}

TEST(wspr_dial_frequencies_sit_in_their_bands_and_60_m_signals_in_the_uk_segment) {
    CHECK_NEAR(wspr::dial_hz_for(14.2e6), 14.0956e6, 1); CHECK_NEAR(wspr::dial_hz_for(7.1e6), 7.0386e6, 1); CHECK_NEAR(wspr::dial_hz_for(3.7e6), 3.5686e6, 1);
    CHECK_NEAR(wspr::dial_hz_for(28.5e6), 28.1246e6, 1); CHECK_NEAR(wspr::dial_hz_for(50.1e6), 50.293e6, 1); CHECK_EQ(wspr::dial_hz_for(15e6), 0.0);
    const double d60 = wspr::dial_hz_for(5.3e6); CHECK(d60 + 1400 >= 5.2885e6 && d60 + 1600 <= 5.2920e6);   // UK 60 m segment 5288.5-5292 kHz
}

TEST(wsprnet_bodies_carry_what_wsjtx_sends) {
    wspr::Spot s; s.hhmm = "1200"; s.snr = -20; s.dt = 0.1; s.mhz = 14.097102; s.drift = 0; s.call = "EA8/M0OML"; s.grid = ""; s.dbm = 37;
    const auto b = wspr::wsprnet_spot_body(s, "261126", "m0oml", "IO91", 14.0956, "Fireside 1.0");
    CHECK_EQ(b, std::string("function=wspr&rcall=M0OML&rgrid=IO91&rqrg=14.095600&date=261126&time=1200&sig=-20&dt=0.1&drift=0&tqrg=14.097102&tcall=EA8%2FM0OML&tgrid=&dbm=37&version=Fireside+1.0&mode=2"));
    CHECK_EQ(wspr::wsprnet_stat_body("M0OML", "IO91", 14.0956, 20, 14.0971, 37, "Fireside 1.0"),
             std::string("function=wsprstat&rcall=M0OML&rgrid=IO91&rqrg=14.095600&tpct=20&tqrg=14.097100&dbm=37&version=Fireside+1.0&mode=2"));
}

TEST(wspr_slots_are_two_minutes_on_even_minutes_never_joined_late) {
    using namespace fireside::tx;
    const double t0 = 1.7e9 - std::fmod(1.7e9, 120.0);                                                   // an even UTC minute
    CHECK_EQ(slot_seconds(DigiMode::WSPR), 120.0); CHECK_NEAR(signal_seconds(DigiMode::WSPR), 110.592, 1e-9); CHECK_EQ(default_start_offset_s(DigiMode::WSPR), 1.0);
    CHECK_EQ(std::fmod(slot_containing(t0 + 50, DigiMode::WSPR).start_utc, 120.0), 0.0);
    const Parity p = slot_containing(t0, DigiMode::WSPR).parity;
    auto a = plan_tx(t0 - 5, DigiMode::WSPR, p, 1.0, 0, 1.5); CHECK_EQ(a.slot.start_utc, t0); CHECK_EQ(a.audio_start_utc, t0 + 1); CHECK_EQ(a.skip_s, 0.0);
    auto b = plan_tx(t0 + 0.2, DigiMode::WSPR, p, 1.0, 0, 1.5); CHECK_EQ(b.slot.start_utc, t0 + 240); CHECK_EQ(b.skip_s, 0.0);   // too late for this one: the next of that parity
}

TEST(wspr_decoder_runs_the_decoder_on_each_114_s_and_reports_every_slot) {
    std::atomic<int> runs{0}; std::mutex m; std::vector<std::string> files; std::vector<decode::WsprSlot> got;
    decode::WsprOptions o; o.sample_rate = 12000; o.work_dir = temp_dir("fireside_wspr_dec"); o.dial_mhz = 7.0386;
    o.runner = [&](const std::string& wav, double mhz, const std::string&, std::string& out, std::string&) {
        { std::lock_guard<std::mutex> l(m); files.push_back(wav); } CHECK(std::filesystem::file_size(wav) == 44 + 114 * 12000 * 2); CHECK_NEAR(mhz, 7.0386, 1e-9);
        out = "0000 -15  0.2   7.040086  0  K1ABC FN42 37 \n<DecodeFinished>\n"; ++runs; return true; };
    o.on_slot = [&](const decode::WsprSlot& s) { std::lock_guard<std::mutex> l(m); got.push_back(s); };
    decode::WsprDecoder d(o); std::string text; std::vector<float> block(12000, 0.001f);
    for (int sec = 0; sec < 240; ++sec) d.process(block.data(), block.size(), text);
    for (int i = 0; i < 200 && d.busy(); ++i) usleep(10000);
    d.process(block.data(), 0, text);
    CHECK_EQ(runs.load(), 2); CHECK_EQ(got.size(), size_t(2));
    CHECK(text.find("K1ABC FN42 37") != std::string::npos); CHECK(text.find("0002") != std::string::npos);   // the second slot is labelled two minutes on
    { std::lock_guard<std::mutex> l(m); for (auto& f : files) CHECK(!std::filesystem::exists(f)); CHECK_EQ(got[0].spots.size(), size_t(1)); CHECK(got[0].ok); }
    std::filesystem::remove_all(o.work_dir);
}

TEST(wspr_decoder_joining_late_waits_for_the_next_slot_and_a_failing_decoder_is_reported) {
    std::atomic<int> runs{0}; std::vector<decode::WsprSlot> got; std::mutex m;
    decode::WsprOptions o; o.sample_rate = 12000; o.work_dir = temp_dir("fireside_wspr_late"); o.start_offset_s = 30;   // 30 s into a slot: that one is lost
    o.runner = [&](const std::string&, double, const std::string&, std::string&, std::string& err) { ++runs; err = "boom"; return false; };
    o.on_slot = [&](const decode::WsprSlot& s) { std::lock_guard<std::mutex> l(m); got.push_back(s); };
    decode::WsprDecoder d(o); std::string text; std::vector<float> block(12000, 0.001f);
    for (int sec = 0; sec < 89 + 113; ++sec) d.process(block.data(), block.size(), text);
    CHECK_EQ(runs.load(), 0);                                                                             // 90 s to the boundary, then 114 s are needed
    for (int sec = 0; sec < 3; ++sec) d.process(block.data(), block.size(), text);
    for (int i = 0; i < 200 && d.busy(); ++i) usleep(10000);
    CHECK_EQ(runs.load(), 1); { std::lock_guard<std::mutex> l(m); CHECK_EQ(got.size(), size_t(1)); CHECK(!got[0].ok); CHECK_EQ(got[0].error, std::string("boom")); }
    d.process(block.data(), 0, text); CHECK(d.status().find("boom") != std::string::npos); CHECK(text.empty());
    std::filesystem::remove_all(o.work_dir);
}

TEST(wspr_round_trip_through_the_real_wsprd_decodes_every_message_exactly) {
    if (decode::find_wsprd().empty()) { std::printf("  (skipped: wsprd not installed)\n"); return; }
    const std::vector<std::tuple<std::string, std::string, int, double>> msgs = {{"M0OML", "IO91", 37, 1500}, {"K1ABC", "FN42", 30, 1440}, {"VK2XYZ", "QF56", 23, 1560}, {"2E0ABC", "JO01", 43, 1410}};
    const auto dir = temp_dir("fireside_wspr_rt"); const auto wav = dir + "/261126_1200.wav";
    { const auto b = wspr::wav16(slot_audio(msgs, -20, 7)); std::ofstream f(wav, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); }
    std::string out, err; CHECK(decode::wsprd_runner()(wav, 14.0956, dir, out, err));
    int found = 0;
    for (const auto& [c, g, d, f] : msgs) {
        std::istringstream is(out); std::string ln; bool ok = false;
        while (std::getline(is, ln)) if (auto s = wspr::parse_wsprd_line(ln)) if (s->call == c && s->grid == g && s->dbm == d && std::fabs(s->mhz - (14.0956 + (f + 1.5 * wspr::kToneSpacingHz) / 1e6)) < 1.5e-6) ok = true;
        CHECK(ok); found += ok;
    }
    if (found != int(msgs.size())) std::printf("  wsprd said:\n%s%s\n", out.c_str(), err.c_str());
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------- live timing: the SlotClock and the decoders starting on a backlog
#include "fireside/slot_clock.h"
TEST(slot_clock_sees_through_a_start_up_backlog_and_jitter) {
    decode::SlotClock c(48000);
    const double T0 = 1000, lat = 0.05; std::mt19937 rng(3); std::uniform_real_distribution<double> jit(0, 0.2);
    uint64_t n = uint64_t(0.7 * 48000); c.observe(T0 + 0.7 + lat + 0.3, n);                    // the first block: 0.7 s of old audio, handed over late
    for (int i = 0; i < 300; ++i) { n += 4800; c.observe(T0 + double(n) / 48000 + lat + jit(rng), n); }
    CHECK_NEAR(c.t0(), T0 + lat, 0.01);                                                            // only the least-delayed blocks count
    CHECK_NEAR(c.utc_of(48000.0 * 10), T0 + 10 + lat, 0.01);
}
TEST(slot_clock_follows_a_sample_clock_running_100_ppm_slow) {
    decode::SlotClock c(48000, 20); const double T0 = 5000, true_rate = 48000 * (1 - 100e-6); uint64_t n = 0;
    for (int i = 0; i < 20000; ++i) { n += 4800; c.observe(T0 + double(n) / true_rate + 0.03, n); }   // ~2000 s: the count runs 0.2 s behind real time by the end
    CHECK_NEAR(c.utc_of(double(n)), T0 + double(n) / true_rate + 0.03, 0.01);
}
namespace {
// A live stream as a decoder sees it: audio heard from t_start at `rate`, handed over in 0.1 s blocks 20 ms after it was heard, except the first
// block, which carries a 0.7 s backlog and arrives 0.75 s late (as the radio's stream does when receive starts).
template <class Dec> void feed_live(Dec& d, const std::vector<float>& a, double rate, double t_start, double& now, std::string& text) {
    size_t at = 0; const size_t first = size_t(0.7 * rate), blk = size_t(0.1 * rate);
    while (at < a.size()) {
        const size_t k = std::min(at == 0 ? first : blk, a.size() - at);
        now = t_start + double(at + k) / rate + (at == 0 ? 0.75 : 0.02);
        d.process(a.data() + at, k, text); at += k;
    }
}
}  // namespace
TEST(wspr_decoder_places_the_slot_by_when_audio_was_heard_not_when_it_arrived) {
    const double S = 1.8e9 - std::fmod(1.8e9, 120.0), t_start = S - 30;                           // the stream starts 30 s before an even minute
    std::vector<float> a(size_t((30 + 116) * 12000), 0.0f);
    const auto sig = wspr::synth_audio(wspr::encode("M0OML", "IO91", 37), 1500, 12000, 0.5, 0, 0);  // the signal alone, heard from S + 1 s
    for (size_t i = 0; i < sig.size(); ++i) a[size_t(31 * 12000) + i] = sig[i];
    double now = 0; std::vector<float> got; std::mutex m;
    decode::WsprOptions o; o.sample_rate = 12000; o.wall_clock = true; o.now = [&] { return now; }; o.work_dir = temp_dir("fireside_wspr_clock");
    o.runner = [&](const std::string& wav, double, const std::string&, std::string&, std::string&) {
        std::ifstream f(wav, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::lock_guard<std::mutex> l(m); for (size_t i = 44; i + 1 < b.size(); i += 2) got.push_back(float(int16_t(uint8_t(b[i]) | (uint8_t(b[i + 1]) << 8))) / 32768.0f); return true; };
    decode::WsprDecoder d(o); std::string text; feed_live(d, a, 12000, t_start, now, text);
    for (int i = 0; i < 300 && d.busy(); ++i) usleep(10000);
    std::lock_guard<std::mutex> l(m); CHECK(!got.empty());
    size_t on = 0; while (on < got.size() && std::fabs(got[on]) < 0.05f) ++on;
    CHECK_NEAR(double(on) / 12000, 1.02, 0.05);                                                     // 1 s into the slot (+ the 20 ms the audio is late), not 1.7 s
    std::filesystem::remove_all(o.work_dir);
}
TEST(ft8_decoder_live_dt_is_right_when_the_stream_starts_with_a_backlog) {
    const double S = 1.8e9 - std::fmod(1.8e9, 15.0) + 15, t_start = S - 5;                         // the stream starts 5 s before a slot
    std::vector<float> a(size_t(5 * 12000), 0.0f);
    const auto slot = decode::synth_ft8("CQ M0OML IO91", 1500, 12000, 0.3, 0.5); a.insert(a.end(), slot.begin(), slot.end()); a.resize(a.size() + 3 * 12000, 0.0f);
    double now = 0; decode::Ft8Options o; o.sample_rate = 12000; o.wall_clock = true; o.now = [&] { return now; };
    decode::Ft8Decoder d(o); std::string text; feed_live(d, a, 12000, t_start, now, text);
    for (int i = 0; i < 300 && d.busy(); ++i) usleep(10000);
    std::vector<float> none(1200, 0.0f); d.process(none.data(), 0, text);
    const auto at = text.find("CQ M0OML IO91"); CHECK(at != std::string::npos);
    if (at != std::string::npos) { std::istringstream is(text.substr(text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1)); std::string t; int snr; double dt; is >> t >> snr >> dt;
        CHECK_NEAR(dt, 0.02, 0.08); }                                                              // WSJT-X's DT (start - 0.5 s): ~0, not +0.7
}
TEST(wspr_false_decodes_are_dropped_real_spots_kept) {
    auto spot = [](const char* line) { return *wspr::parse_wsprd_line(line); };
    std::string why;
    CHECK(!decode::wspr_spot_plausible(spot("2242 -31 -0.6   3.570079  0  B4N/N85JIU 23"), &why)); CHECK(why.find("compound") != std::string::npos);   // 80 m, 2026-11-26: noise
    CHECK(!decode::wspr_spot_plausible(spot("1200 -10  0.1  14.097100  0  0J0ABC IO91 37"), &why)); CHECK(why.find("prefix") != std::string::npos);
    CHECK(!decode::wspr_spot_plausible(spot("1200 -27  0.1  14.097100  0  K1ABC JO01 37"), &why)); CHECK(why.find("locator") != std::string::npos);   // a US call in England, weak
    CHECK(decode::wspr_spot_plausible(spot("1200 -20  0.1  14.097100  0  K1ABC JO01 37")));          // strong: could be operated from anywhere
    CHECK(decode::wspr_spot_plausible(spot("1200 -20  0.1  14.097100  0  EA8/M0OML 37")));           // a compound call heard well
    CHECK(decode::wspr_spot_plausible(spot("1200 -27  0.1  14.097100  0  <...> IO91AB 37")));        // unresolved hash: nothing to check
    for (const char* real : {"2242 -24  0.1   3.570090  0  G4LEN IO81 23", "2242 -26  0.1   3.570101  0  EB1A IN73 23", "2206 -21  0.8   7.040086  0  IU0JJD JN61 23",
                             "2206 -27  0.9   7.040181  0  LB8WG JP53 20", "2208 -27  0.9   7.040190  0  <IU0JJD> JN61TP 23", "2228 -27  2.2   7.040150  0  E79Q JN84 23",
                             "2208 -26  2.7   7.040160  0  SQ9OZM KN09 23", "2216 -24  0.8   7.040140  0  SR9KRT KO00 23"})
        CHECK(decode::wspr_spot_plausible(spot(real)));                                              // every real spot heard today (40 / 80 m) is kept
}
TEST(wspr_our_own_beacon_is_never_listed_or_uploaded) {
    std::vector<decode::WsprSlot> got; std::mutex m;
    decode::WsprOptions o; o.sample_rate = 12000; o.work_dir = temp_dir("fireside_wspr_own"); o.my_call = "m0oml";
    o.runner = [](const std::string&, double, const std::string&, std::string& out, std::string&) {
        out = "0000  12  0.0   3.570100  0  M0OML IO91 37\n0000 -10  0.0   3.570150  0  EA8/M0OML 37\n0000 -20  0.1   3.570090  0  <M0OML> IO91AB 37\n0000 -24  0.1   3.570120  0  G4LEN IO81 37\n"; return true; };
    o.on_slot = [&](const decode::WsprSlot& s) { std::lock_guard<std::mutex> l(m); got.push_back(s); };
    decode::WsprDecoder d(o); std::string text; std::vector<float> block(12000, 0.001f);
    for (int sec = 0; sec < 115; ++sec) d.process(block.data(), block.size(), text);
    for (int i = 0; i < 200 && d.busy(); ++i) usleep(10000);
    d.process(block.data(), 0, text);
    CHECK(text.find("M0OML") == std::string::npos); CHECK(text.find("G4LEN") != std::string::npos);
    std::lock_guard<std::mutex> l(m); CHECK_EQ(got.size(), size_t(1)); CHECK_EQ(got[0].spots.size(), size_t(1)); CHECK(got[0].dropped.empty());   // not counted as a false decode either
    std::filesystem::remove_all(o.work_dir);
}
