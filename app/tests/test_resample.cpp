// Resampler, streaming WAV reader (incl. RF64 and unpatched sizes) and automatic resampling of recordings at odd sample rates.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "fireside/dsp.h"
#include "fireside/iqsource.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::dsp;

namespace {
double cplx_amp(const std::vector<cf>& x, size_t from, size_t n, double hz, double fs) {
    std::complex<double> s(0, 0); for (size_t i = 0; i < n; ++i) s += std::complex<double>(x[from + i]) * std::polar(1.0, -2 * kPi * hz * double(from + i) / fs); return std::abs(s) / double(n);
}
std::string tmpf(const char* n) { return (std::filesystem::temp_directory_path() / (std::string("firesiders_") + std::to_string(::getpid()) + "_" + n)).string(); }
std::vector<cf> resample_all(const std::vector<cf>& x, double in, double out, size_t chunk = 0) {
    RationalResampler r(in, out); std::vector<cf> y; if (!chunk) chunk = x.size();
    for (size_t o = 0; o < x.size(); o += chunk) r.process(x.data() + o, std::min(chunk, x.size() - o), y);
    return y;
}
}  // namespace

TEST(resample_rate_selection) {
    for (double r : {48000.0, 96000.0, 144000.0, 192000.0}) CHECK(!needs_resampling(r));
    for (double r : {44100.0, 62500.0, 250000.0, 1024000.0, 2400000.0, 384000.0, 8000.0}) CHECK(needs_resampling(r));
    CHECK_EQ(choose_output_rate(44100), 48000.0); CHECK_EQ(choose_output_rate(62500), 48000.0); CHECK_EQ(choose_output_rate(96000 + 1000), 96000.0);
    CHECK_EQ(choose_output_rate(250000), 192000.0); CHECK_EQ(choose_output_rate(2400000), 192000.0); CHECK_EQ(choose_output_rate(8000), 48000.0);
}
TEST(resample_keeps_tones_at_the_right_frequency_and_level) {
    struct Case { double in, out; }; 
    for (Case c : {Case{62500, 48000}, Case{44100, 48000}, Case{2400000, 192000}, Case{1024000, 192000}, Case{250000, 192000}, Case{384000, 192000}}) {
        const size_t n = size_t(c.in * 0.5); auto x = synth_tones(c.in, n, {{5000, 0.5}, {-7000, 0.25}});
        auto y = resample_all(x, c.in, c.out);
        CHECK(std::fabs(double(y.size()) - double(n) * c.out / c.in) < 3.0);                  // output length = input length x ratio
        const size_t from = y.size() / 2, len = 8192;
        CHECK_NEAR(cplx_amp(y, from, len, 5000, c.out), 0.5, 0.01); CHECK_NEAR(cplx_amp(y, from, len, -7000, c.out), 0.25, 0.01);
    }
}
TEST(resample_rejects_what_would_alias) {
    // 62.5 kHz -> 48 kHz: a tone at +30 kHz is above the new Nyquist (24 kHz) and would fold to -18 kHz. It must be gone (> 60 dB down).
    auto x = synth_tones(62500, 31250, {{30000, 0.5}}); auto y = resample_all(x, 62500, 48000);
    CHECK(cplx_amp(y, y.size() / 2, 8192, -18000, 48000) < 0.0005); CHECK(cplx_amp(y, y.size() / 2, 8192, 30000 - 48000, 48000) < 0.0005);
    // 2.4 MHz -> 192 kHz: a tone at 150 kHz folds to -42 kHz otherwise
    auto z = synth_tones(2400000, 480000, {{150000, 0.5}}); auto w = resample_all(z, 2400000, 192000);
    CHECK(cplx_amp(w, w.size() / 2, 8192, 150000 - 192000, 192000) < 0.0005);
}
TEST(resample_streaming_equals_one_shot_for_any_chunking) {
    auto x = synth_tones(62500, 40000, {{3000, 0.4}, {-9000, 0.2}}, 0.01, 3); auto whole = resample_all(x, 62500, 48000);
    for (size_t chunk : {size_t(1), size_t(7), size_t(1000), size_t(4097)}) {
        auto parts = resample_all(x, 62500, 48000, chunk); CHECK_EQ(parts.size(), whole.size());
        for (size_t i = 0; i < whole.size(); ++i) CHECK(std::abs(whole[i] - parts[i]) < 1e-5f);
    }
}
TEST(resample_rejects_bad_rates) {
    CHECK_THROWS(RationalResampler(0, 48000), std::invalid_argument); CHECK_THROWS(RationalResampler(48000, -1), std::invalid_argument);
    CHECK_THROWS(RationalResampler(44100.5, 48000), std::invalid_argument); CHECK_THROWS(RationalResampler(48000, 1e10), std::invalid_argument);
    CHECK_THROWS(RationalResampler(44101, 48000), std::invalid_argument);                    // reduces to 48000/44101: far too many phases
    RationalResampler same(48000, 48000); std::vector<cf> y; auto x = synth_tones(48000, 1000, {{1000, 0.5}}); same.process(x.data(), x.size(), y); CHECK_EQ(y.size(), size_t(1000));
}
TEST(resample_is_fast_enough_for_big_recordings) {
    auto x = synth_tones(2400000, 2400000, {{5000, 0.5}}); const auto t0 = std::chrono::steady_clock::now(); auto y = resample_all(x, 2400000, 192000);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s < 2.0);                                                                          // 1 s of 2.4 Msps in under 2 s even on a slow box (about 0.05 s here)
    CHECK(y.size() > 191000);
}

TEST(wavstream_matches_the_whole_file_reader_and_rewinds) {
    const std::string p = tmpf("s.wav"); std::vector<float> il; for (int i = 0; i < 1000; ++i) { il.push_back(float(i) / 1000); il.push_back(-float(i) / 1000); }
    write_wav_f32(p, 62500, 2, il); WavStream w(p); CHECK_EQ(w.sample_rate(), 62500.0); CHECK_EQ(w.frames(), uint64_t(1000));
    std::vector<cf> a; CHECK_EQ(w.read(a, 300), size_t(300)); CHECK_EQ(w.read(a, 5000), size_t(700)); CHECK_EQ(w.read(a, 10), size_t(0));
    auto whole = read_iq_wav(p); CHECK(whole.samples == a); w.rewind(); std::vector<cf> b; w.read(b, 1000); CHECK(b == a);
    std::filesystem::remove(p);
}
namespace {
std::vector<uint8_t> wav_bytes(const char* magic, bool rf64, bool unpatched, int frames) {      // hand-built 16-bit stereo 44.1 kHz file
    std::vector<uint8_t> v; auto p32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }; auto p16 = [&](uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(t[i])); };
    tag(magic); p32(rf64 ? 0xFFFFFFFFu : 0); tag("WAVE");
    if (rf64) { tag("ds64"); p32(28); p32(0); p32(0); p32(uint32_t(frames * 4)); p32(0); p32(uint32_t(frames)); p32(0); p32(0); }
    tag("fmt "); p32(16); p16(1); p16(2); p32(44100); p32(44100 * 4); p16(4); p16(16);
    tag("data"); p32(rf64 || unpatched ? 0xFFFFFFFFu : uint32_t(frames * 4)); for (int i = 0; i < frames; ++i) { p16(uint16_t(int16_t(i))); p16(uint16_t(int16_t(-i))); }
    return v;
}
}  // namespace
TEST(wavstream_reads_rf64_and_unpatched_streaming_headers_and_truncated_files) {
    for (auto [magic, rf, un] : {std::tuple{"RIFF", false, true}, std::tuple{"RF64", true, false}}) {
        const std::string p = tmpf("h.wav"); auto v = wav_bytes(magic, rf, un, 500); std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(v.data()), long(v.size()));
        WavStream w(p); CHECK_EQ(w.frames(), uint64_t(500)); std::vector<cf> a; w.read(a, 1000); CHECK_EQ(a.size(), size_t(500)); CHECK_NEAR(a[499].real(), 499.0 / 32768.0, 1e-6); std::filesystem::remove(p);
    }
    const std::string p = tmpf("t.wav"); auto v = wav_bytes("RIFF", false, false, 500); v.resize(v.size() - 402);          // cut mid-frame
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(v.data()), long(v.size())); WavStream w(p); std::vector<cf> a; w.read(a, 1000); CHECK_EQ(a.size(), size_t(399)); std::filesystem::remove(p);
}
TEST(open_iq_file_resamples_odd_rates_and_leaves_good_ones_alone) {
    const std::string p = tmpf("o.wav"), q = tmpf("q.wav"); std::vector<float> il;
    for (auto& v : synth_tones(62500, 62500, {{4000, 0.5}})) { il.push_back(v.real()); il.push_back(v.imag()); } write_wav_f32(p, 62500, 2, il);
    auto src = open_iq_file(p); CHECK_EQ(src->sample_rate(), 48000.0); CHECK(src->describe().find("resampled to 48000 Hz") != std::string::npos);
    std::vector<cf> y; size_t got = 0; for (int i = 0; i < 100 && got < 60000; ++i) got += src->read(y, 4096);
    CHECK(got > 47000 && got < 48100);                                                                                     // 1 s of input = 48000 outputs, then it ends
    CHECK_NEAR(cplx_amp(y, 20000, 8192, 4000, 48000), 0.5, 0.01); CHECK_EQ(src->read(y, 100), size_t(0));
    il.clear(); for (auto& v : synth_tones(96000, 1000, {{4000, 0.5}})) { il.push_back(v.real()); il.push_back(v.imag()); } write_wav_f32(q, 96000, 2, il);
    auto ok = open_iq_file(q); CHECK_EQ(ok->sample_rate(), 96000.0); CHECK(ok->describe().find("resampled") == std::string::npos);
    std::filesystem::remove(p); std::filesystem::remove(q);
}
TEST(resampled_recording_loops_and_feeds_the_receive_chain) {
    const std::string p = tmpf("l.wav"); std::vector<float> il;
    for (auto& v : synth_tones(250000, 25000, {{9000 + 1500, 0.3}})) { il.push_back(v.real()); il.push_back(v.imag()); } write_wav_f32(p, 250000, 2, il);   // 0.1 s at 250 ksps, DDS 9 kHz below the dial
    auto src = open_iq_file(p, 0, 1, true); CHECK_EQ(src->sample_rate(), 192000.0);
    std::vector<cf> x; size_t got = 0; for (int i = 0; i < 400 && got < 100000; ++i) got += src->read(x, 4096); CHECK(got >= 100000);                   // 0.1 s of file looped to > 0.5 s of output
    ReceiveConfig rc; rc.sample_rate = 192000; DemodConfig dc; dc.input_rate = 192000; ReceiveChain ch(rc, dc); std::vector<float> a; ch.process(x.data(), x.size(), a);
    const auto& sp = ch.spectrum(); const size_t k = size_t(std::max_element(sp.db().begin(), sp.db().end()) - sp.db().begin()); CHECK(std::fabs(sp.bin_hz(k) - 1500.0) < 60.0);
    std::filesystem::remove(p);
}
