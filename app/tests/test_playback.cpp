// RadioAudioOut: only the chosen channel pair carries audio, level and resampling, backpressure, bad pairs, and the real ALSA playback path against the "null" device.
#include <cmath>
#include "fireside/playback.h"
#include "test_util.h"
using namespace fireside;

namespace {
struct FakeOut : IPcmOut {
    int ch = 8; double fs = 48000; size_t capacity = 1u << 30; std::vector<int32_t> got;      // `capacity` frames may still be accepted
    int channels() const override { return ch; } double rate() const override { return fs; } std::string describe() const override { return "fake out"; }
    size_t write(const int32_t* d, size_t frames) override { const size_t n = std::min(frames, capacity); got.insert(got.end(), d, d + n * size_t(ch)); capacity -= n; return n; }
};
std::vector<float> tone(size_t n, double hz = 1000, double amp = 0.5, double fs = 48000) { std::vector<float> x(n); for (size_t i = 0; i < n; ++i) x[i] = float(amp * std::sin(2 * M_PI * hz * double(i) / fs)); return x; }
}  // namespace

TEST(radio_out_puts_audio_on_the_chosen_pair_only) {
    for (int pair = 0; pair < 4; ++pair) {
        auto f = std::make_unique<FakeOut>(); FakeOut* raw = f.get(); RadioAudioOut out(std::move(f), pair); out.set_volume(1.0f); auto x = tone(4800); out.push(x.data(), x.size());
        CHECK_EQ(raw->got.size(), size_t(4800 * 8)); double energy[8] = {0};
        for (size_t i = 0; i < 4800; ++i) for (int c = 0; c < 8; ++c) energy[c] += std::fabs(double(raw->got[i * 8 + size_t(c)]));
        for (int c = 0; c < 8; ++c) { if (c == 2 * pair || c == 2 * pair + 1) CHECK(energy[c] > 1e9); else CHECK_EQ(energy[c], 0.0); }
        for (size_t i = 0; i < 4800; ++i) CHECK_EQ(raw->got[i * 8 + size_t(2 * pair)], raw->got[i * 8 + size_t(2 * pair + 1)]);                  // mono on both channels of the pair
    }
}
TEST(radio_out_scales_clips_and_converts_to_s32) {
    auto f = std::make_unique<FakeOut>(); FakeOut* raw = f.get(); RadioAudioOut out(std::move(f), 1);
    out.set_volume(0.5f); std::vector<float> x = {0.5f, -0.5f, 1.0f, 0.0f}; out.push(x.data(), x.size());
    CHECK_EQ(raw->got[0 * 8 + 2], int32_t(std::lround(0.25 * 2147483647.0))); CHECK_EQ(raw->got[1 * 8 + 2], int32_t(std::lround(-0.25 * 2147483647.0))); CHECK_EQ(raw->got[2 * 8 + 2], int32_t(std::lround(0.5 * 2147483647.0))); CHECK_EQ(raw->got[3 * 8 + 2], 0);
    out.set_volume(5.0f); std::vector<float> loud = {2.0f}; out.push(loud.data(), 1); CHECK_EQ(raw->got[4 * 8 + 2], int32_t(2147483647));                                           // volume and samples are clamped
    out.set_volume(-1.0f); out.push(loud.data(), 1); CHECK_EQ(raw->got[5 * 8 + 2], 0);
}
TEST(radio_out_resamples_to_the_stream_rate) {
    for (double fs : {96000.0, 192000.0}) {
        auto f = std::make_unique<FakeOut>(); f->fs = fs; FakeOut* raw = f.get(); RadioAudioOut out(std::move(f), 0); out.set_volume(1.0f); auto x = tone(48000, 1000, 0.5);
        for (size_t o = 0; o < x.size(); o += 4800) out.push(x.data() + o, 4800);
        const size_t frames = raw->got.size() / 8; CHECK(std::fabs(double(frames) - fs) < 100);                                    // 1 s of audio = fs frames
        std::complex<double> s(0, 0); const size_t from = frames / 2, len = 8192; for (size_t i = 0; i < len; ++i) s += double(raw->got[(from + i) * 8]) / 2147483647.0 * std::polar(1.0, -2 * M_PI * 1000.0 * double(from + i) / fs);
        CHECK_NEAR(2.0 * std::abs(s) / double(len), 0.5, 0.02);                                                                   // the tone keeps its frequency and level
    }
}
TEST(radio_out_never_blocks_and_drops_instead_of_building_delay) {
    auto f = std::make_unique<FakeOut>(); FakeOut* raw = f.get(); raw->capacity = 0; RadioAudioOut out(std::move(f), 0); auto x = tone(4800);
    for (int i = 0; i < 20; ++i) out.push(x.data(), x.size());                                                                    // the device takes nothing: 96000 frames offered
    CHECK(out.frames_dropped() > 60000); CHECK_EQ(out.frames_written(), uint64_t(0));
    raw->capacity = 1u << 30; out.push(x.data(), x.size()); CHECK(out.frames_written() > 0 && out.frames_written() <= 24000 + 4800);   // when it drains, at most half a second is delivered (no growing backlog)
}
TEST(radio_out_keeps_partial_writes_and_order) {
    auto f = std::make_unique<FakeOut>(); FakeOut* raw = f.get(); raw->capacity = 1000; RadioAudioOut out(std::move(f), 0); out.set_volume(1.0f);
    std::vector<float> ramp(3000); for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = float(i) / 4000.0f; out.push(ramp.data(), ramp.size()); CHECK_EQ(raw->got.size(), size_t(1000 * 8));
    raw->capacity = 5000; std::vector<float> more(10, 0.9f); out.push(more.data(), more.size());                                  // the rest of the first push goes out first, then the new audio
    CHECK_EQ(raw->got.size(), size_t(3010 * 8)); for (size_t i = 0; i < 3000; i += 499) CHECK(std::llabs(raw->got[i * 8] - int32_t(std::lround(double(ramp[i]) * 2147483647.0))) < 2);
}
TEST(radio_out_rejects_a_pair_the_stream_does_not_have) {
    CHECK_THROWS(RadioAudioOut(std::make_unique<FakeOut>(), 4), std::invalid_argument); CHECK_THROWS(RadioAudioOut(std::make_unique<FakeOut>(), -1), std::invalid_argument); CHECK_THROWS(RadioAudioOut(nullptr, 0), std::invalid_argument);
    auto two = std::make_unique<FakeOut>(); two->ch = 2; CHECK_THROWS(RadioAudioOut(std::move(two), 1), std::invalid_argument); auto ok = std::make_unique<FakeOut>(); ok->ch = 2; RadioAudioOut fine(std::move(ok), 0); CHECK(fine.describe().find("channels 0/1") != std::string::npos);
}
TEST(alsa_playback_real_path_against_the_null_device) {
    std::unique_ptr<IPcmOut> pcm; try { pcm = open_alsa_play("null", 48000, 8, 1024); } catch (const IoError& e) { std::cout << "       (skipped: " << e.what() << ")\n"; return; }
    CHECK_EQ(pcm->channels(), 8); CHECK_EQ(pcm->rate(), 48000.0); RadioAudioOut out(std::move(pcm), 2); auto x = tone(4800); for (int i = 0; i < 10; ++i) out.push(x.data(), x.size()); CHECK(out.frames_written() > 0);
    CHECK_THROWS(open_alsa_play("hw:CARD=fireside_no_such_card,DEV=0", 48000, 8, 1024), IoError);
}
