// Live-capture logic over a fake PCM (conversion, channel pick, stats, watchdog, errors), card listing, I/Q analysis, and the real ALSA path against ALSA's "null" PCM.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "fireside/capture.h"
#include "test_util.h"
using namespace fireside; using dsp::cf;

namespace {
// 8-channel fake: ch0/1 = a complex tone (I/Q), ch2 = constant, the rest silent. Can serve short reads, time out, xrun or fail.
struct FakePcm : IPcm {
    int ch = 8; double fs = 48000; double tone_hz = 1000, amp = 0.5; uint64_t pos = 0, xr = 0;
    size_t max_per_read = 1u << 30; int empty_reads_before_data = 0; bool fail = false, silent = false; int32_t ch2_value = 0;
    int channels() const override { return ch; } double rate() const override { return fs; } uint64_t xruns() const override { return xr; }
    std::string describe() const override { return "fake pcm"; }
    size_t read(int32_t* out, size_t frames, double) override {
        if (fail) throw IoError("fake: device gone");
        if (empty_reads_before_data > 0) { --empty_reads_before_data; return 0; }
        const size_t n = std::min(frames, max_per_read);
        for (size_t f = 0; f < n; ++f) {
            const double a = 2 * dsp::kPi * tone_hz * double(pos + f) / fs;
            for (int c = 0; c < ch; ++c) out[f * size_t(ch) + size_t(c)] = 0;
            if (!silent) { out[f * size_t(ch)] = int32_t(std::lround(amp * std::cos(a) * 2147483647.0)); out[f * size_t(ch) + 1] = int32_t(std::lround(amp * std::sin(a) * 2147483647.0)); }
            if (ch > 2) out[f * size_t(ch) + 2] = ch2_value;
        }
        pos += n; return n;
    }
};
std::unique_ptr<PcmIqSource> src_of(FakePcm*& raw, int ci = 0, int cq = 1) { auto p = std::make_unique<FakePcm>(); raw = p.get(); return std::make_unique<PcmIqSource>(std::move(p), ci, cq, 512); }
}  // namespace

TEST(capture_s32_conversion_is_exact_at_the_edges) {
    CHECK_EQ(s32_to_float(0), 0.0f); CHECK_EQ(s32_to_float(INT32_MIN), -1.0f); CHECK_EQ(s32_to_float(0x40000000), 0.5f); CHECK_EQ(s32_to_float(-0x40000000), -0.5f);
    CHECK(s32_to_float(INT32_MAX) < 1.0001f && s32_to_float(INT32_MAX) >= 1.0f);
}
TEST(capture_picks_rx1_pair_and_honours_other_channels) {
    FakePcm* raw; auto s = src_of(raw); std::vector<cf> x; CHECK_EQ(s->read(x, 1000), size_t(1000)); CHECK(s->live());
    CHECK_NEAR(x[0].real(), 0.5, 1e-6); CHECK_NEAR(x[0].imag(), 0.0, 1e-6);
    const double a = 2 * dsp::kPi * 1000.0 * 10 / 48000; CHECK_NEAR(x[10].real(), 0.5 * std::cos(a), 1e-6); CHECK_NEAR(x[10].imag(), 0.5 * std::sin(a), 1e-6);
    FakePcm* raw2; auto s2 = src_of(raw2, 2, 0); raw2->ch2_value = 0x20000000; std::vector<cf> y; s2->read(y, 4);
    CHECK_NEAR(y[0].real(), 0.25, 1e-6); CHECK_NEAR(y[0].imag(), 0.5, 1e-6);                    // I = ch2, Q = ch0
}
TEST(capture_never_returns_more_than_asked_and_handles_short_and_empty_reads) {
    FakePcm* raw; auto s = src_of(raw); raw->max_per_read = 100;
    std::vector<cf> x; CHECK_EQ(s->read(x, 1000), size_t(1000)); CHECK_EQ(x.size(), size_t(1000)); CHECK_EQ(s->stats().frames, uint64_t(1000));
    raw->empty_reads_before_data = 1; CHECK_EQ(s->read(x, 50), size_t(0)); CHECK_EQ(s->stats().empty_reads, uint64_t(1));      // timeout: 0 samples, not an error
    CHECK_EQ(s->read(x, 50), size_t(50));
    CHECK_EQ(s->read(x, 0), size_t(0));
}
TEST(capture_stats_give_per_channel_rms_and_peak) {
    FakePcm* raw; auto s = src_of(raw); raw->ch2_value = 0x10000000; std::vector<cf> x; s->read(x, 4800);
    const auto& st = s->stats();
    CHECK_NEAR(st.rms(0), 0.5 / std::sqrt(2.0), 2e-3); CHECK_NEAR(st.rms(1), 0.5 / std::sqrt(2.0), 2e-3);
    CHECK_NEAR(st.rms(2), 0.125, 1e-6); CHECK_EQ(st.rms(5), 0.0); CHECK_NEAR(st.peak[0], 0.5, 1e-3); CHECK_EQ(st.peak[7], 0.0);
}
TEST(capture_dead_stream_flag_follows_the_one_second_rule) {
    FakePcm* raw; auto s = src_of(raw); raw->silent = true; std::vector<cf> x;
    s->read(x, 40000); CHECK(!s->stream_dead());                                             // 0.83 s of zeros
    s->read(x, 10000); CHECK(s->stream_dead());                                              // > 1 s
    raw->silent = false; s->read(x, 512); CHECK(!s->stream_dead());                          // recovers as soon as signal returns
}
TEST(capture_dead_flag_is_independent_of_read_size) {
    for (size_t chunk : {size_t(1), size_t(4096), size_t(20000)}) {
        FakePcm* raw; auto s = src_of(raw); raw->silent = true; raw->max_per_read = chunk; std::vector<cf> x;
        for (size_t got = 0; got < 47000; ) got += s->read(x, 47000 - got);
        CHECK(!s->stream_dead());                                                    // 0.98 s
        for (size_t got = 0; got < 2000; ) got += s->read(x, 2000 - got);
        CHECK(s->stream_dead());                                                     // > 1 s
        if (chunk == 1) break;                                                       // (1-frame reads are slow; once is enough)
    }
}
TEST(capture_reports_xruns_and_propagates_device_errors) {
    FakePcm* raw; auto s = src_of(raw); std::vector<cf> x; raw->xr = 3; s->read(x, 512); CHECK_EQ(s->stats().xruns, uint64_t(3));
    raw->fail = true; CHECK_THROWS(s->read(x, 512), IoError);
}
TEST(capture_rejects_bad_channel_indices_and_null_pcm) {
    auto mk = [&](int ci, int cq) { return PcmIqSource(std::make_unique<FakePcm>(), ci, cq); };
    CHECK_THROWS(mk(0, 8), std::invalid_argument); CHECK_THROWS(mk(-1, 1), std::invalid_argument); CHECK_THROWS(PcmIqSource(nullptr), std::invalid_argument);
    CHECK_THROWS(PcmIqSource(std::make_unique<FakePcm>(), 0, 1, 0), std::invalid_argument);
}
TEST(capture_read_raw_returns_all_channels_for_recording) {
    FakePcm* raw; auto s = src_of(raw); raw->ch2_value = 123456; std::vector<int32_t> r; CHECK_EQ(s->read_raw(r, 10, 0.1), size_t(10));
    CHECK_EQ(r.size(), size_t(80)); CHECK_EQ(r[2], 123456); CHECK_EQ(r[3], 0); CHECK_EQ(s->channels(), 8);
}
TEST(capture_feeds_the_receive_chain_end_to_end) {
    FakePcm* raw; auto s = src_of(raw); raw->tone_hz = 9000 + 1500;                            // dial + 1.5 kHz with the DDS 9 kHz below the dial
    dsp::ReceiveConfig rc; dsp::DemodConfig dc; dsp::ReceiveChain ch(rc, dc); std::vector<cf> x; std::vector<float> audio;
    s->read(x, 16384); ch.process(x.data(), x.size(), audio);
    const auto& sp = ch.spectrum(); const size_t k = size_t(std::max_element(sp.db().begin(), sp.db().end()) - sp.db().begin());
    CHECK(std::fabs(sp.bin_hz(k) - 1500.0) < 15.0);
}

TEST(capture_card_listing_from_a_fake_proc_asound) {
    const auto dir = std::filesystem::temp_directory_path() / ("firesidecards_" + std::to_string(::getpid())); std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "card0" / "pcm0c"); std::filesystem::create_directories(dir / "card1" / "pcm0c"); std::filesystem::create_directories(dir / "card2" / "pcm0p");
    std::ofstream(dir / "cards") << " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n                      HDA Intel PCH at 0xfb830000 irq 75\n"
                                    " 1 [FLEX5000       ]: DICE - FlexRadio FLEX-5000\n                      FlexRadio FLEX-5000 at fw0\n"
                                    " 2 [HDMI           ]: HDA-Intel - HDA NVidia\n                      HDA NVidia at 0xfb080000 irq 74\n";
    auto c = list_capture_cards(dir.string());
    CHECK_EQ(c.size(), size_t(2));                                                           // card2 has no capture PCM
    CHECK_EQ(c[0].id, std::string("FLEX5000")); CHECK(c[0].flex_like); CHECK(!c[1].flex_like); CHECK_EQ(c[0].device(), std::string("hw:CARD=FLEX5000,DEV=0"));
    CHECK(list_capture_cards("/nonexistent").empty());
    std::filesystem::remove_all(dir);
}
TEST(capture_analysis_tells_quadrature_from_single_channel) {
    auto quad = dsp::synth_tones(48000, 16384, {{3000, 0.5}}, 0.0005);
    auto a = analyze_iq(quad, 48000); CHECK(a.image_valid); CHECK_NEAR(a.peak_hz, 3000, 8); CHECK(a.image_rejection_db > 50.0);
    std::vector<cf> only_i = quad; for (auto& v : only_i) v = cf(v.real(), 0);                 // Q missing: tone and mirror image equal
    auto b = analyze_iq(only_i, 48000); CHECK(b.image_rejection_db < 6.0);
    std::vector<cf> dup = quad; for (auto& v : dup) v = cf(v.real(), v.real());                // Q = I (duplicated channel)
    CHECK(analyze_iq(dup, 48000).image_rejection_db < 10.0);
    auto dc = dsp::synth_tones(48000, 16384, {{0, 0.5}}); CHECK(!analyze_iq(dc, 48000).image_valid);
    CHECK(!analyze_iq(std::vector<cf>(100), 48000).image_valid);
}

TEST(alsa_real_backend_reads_zeros_from_the_null_pcm_and_reports_a_dead_stream) {
    std::unique_ptr<IPcm> pcm;
    try { pcm = open_alsa_pcm("null", 48000, 8, 1024); } catch (const IoError& e) { std::cout << "       (skipped: " << e.what() << ")\n"; return; }
    CHECK_EQ(pcm->channels(), 8); CHECK_EQ(pcm->rate(), 48000.0);
    PcmIqSource src(std::move(pcm)); std::vector<cf> x; size_t got = 0; for (int i = 0; i < 400 && got < 60000; ++i) got += src.read(x, 4096);
    CHECK(got >= 48000); for (size_t i = 0; i < 1000; ++i) CHECK(x[i] == cf(0, 0));
    CHECK(src.stream_dead());                                                                // the null device is silent: exactly what the watchdog exists for
}
TEST(alsa_real_backend_reports_errors_for_missing_devices) {
    try { open_alsa_pcm("hw:CARD=fireside_no_such_card,DEV=0", 48000, 8, 1024); FAIL_AT("no exception"); }
    catch (const IoError& e) { CHECK(std::string(e.what()).find("fireside_no_such_card") != std::string::npos); }
    auto caps = probe_alsa_pcm("hw:CARD=fireside_no_such_card,DEV=0"); CHECK(!caps.ok); CHECK(!caps.error.empty());
}
