// Recorder: replay buffer + grab, streaming recording with pre-roll, header/size correctness, 16-bit audio, limits, and a concurrent producer. Files are read back with the
// app's own WAV reader (the same one "Open file..." uses), so a recording is known to replay.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include "fireside/dsp.h"
#include "fireside/iqsource.h"
#include "fireside/recorder.h"
#include "test_util.h"
using namespace fireside;

namespace {
std::string tmp(const char* name) { return "/tmp/firesiderec_" + std::to_string(::getpid()) + "_" + name + ".wav"; }
void wait_idle(Recorder& r) { for (int i = 0; i < 500 && (r.status().saving); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
// A ramp: frame k has I = k / 1e6, Q = -k / 1e6 (exactly representable to the 32-bit file's resolution at the tolerance used).
std::vector<float> ramp(size_t from, size_t n) { std::vector<float> v(2 * n); for (size_t i = 0; i < n; ++i) { v[2 * i] = float(double(from + i) / 1e6); v[2 * i + 1] = -float(double(from + i) / 1e6); } return v; }
uint32_t le32(const std::vector<uint8_t>& b, size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; }
std::vector<uint8_t> slurp(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); std::vector<uint8_t> b; if (!f) return b; uint8_t t[4096]; size_t n; while ((n = std::fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + n); std::fclose(f); return b; }
}  // namespace

TEST(recorder_grab_saves_exactly_the_last_seconds_across_the_ring_wrap) {
    Recorder r({2, 32}); r.configure(1000.0, 5.0);                                                     // 5 s ring at 1 kHz = 5000 frames
    CHECK(!r.grab(1, tmp("empty")));                                                                   // nothing buffered yet
    size_t pos = 0; for (int i = 0; i < 13; ++i) { auto v = ramp(pos, 700); r.push(v.data(), 700); pos += 700; }   // 9100 frames: wrapped almost twice
    CHECK_NEAR(r.buffered_seconds(), 5.0, 1e-9);
    const std::string p = tmp("grab"); CHECK(r.grab(2.0, p)); wait_idle(r); CHECK(r.status().last_path == p);
    FileIqSource src(p); CHECK_NEAR(src.sample_rate(), 1000.0, 1e-9); std::vector<dsp::cf> x; size_t got = 0; while (true) { const size_t n = src.read(x, 4096); if (!n) break; got += n; }
    CHECK_EQ(got, size_t(2000)); CHECK_NEAR(x.front().real(), double(pos - 2000) / 1e6, 2e-6); CHECK_NEAR(x.back().real(), double(pos - 1) / 1e6, 2e-6); CHECK_NEAR(x[5].imag(), -double(pos - 2000 + 5) / 1e6, 2e-6);
    for (size_t i = 1; i < x.size(); ++i) CHECK_NEAR(x[i].real() - x[i - 1].real(), 1e-6, 2e-6);       // contiguous: no gap or repeat at the wrap
    CHECK(r.grab(60.0, tmp("grab_all"))); wait_idle(r); FileIqSource all(tmp("grab_all")); std::vector<dsp::cf> y; while (all.read(y, 4096)) {} CHECK_EQ(y.size(), size_t(5000));   // clamped to what is buffered
    std::remove(p.c_str()); std::remove(tmp("grab_all").c_str());
}
TEST(recorder_streaming_recording_with_preroll_has_a_correct_header_and_contiguous_data) {
    Recorder r({2, 32}); r.configure(2000.0, 4.0); size_t pos = 0; for (int i = 0; i < 6; ++i) { auto v = ramp(pos, 1000); r.push(v.data(), 1000); pos += 1000; }   // 3 s buffered
    const std::string p = tmp("stream"); CHECK(r.start(p, 1.5)); CHECK(!r.start(p, 0));                // already recording
    for (int i = 0; i < 4; ++i) { auto v = ramp(pos, 1000); r.push(v.data(), 1000); pos += 1000; }
    CHECK(r.status().recording); r.stop(); r.stop(); CHECK(!r.status().recording); CHECK(r.status().error.empty());
    const auto b = slurp(p); CHECK(b.size() > 44); CHECK(std::string(b.begin(), b.begin() + 4) == "RIFF"); CHECK_EQ(le32(b, 4), uint32_t(b.size() - 8)); CHECK_EQ(le32(b, 40), uint32_t(b.size() - 44));
    CHECK_EQ(le32(b, 24), 2000u); CHECK_EQ(b[22], uint8_t(2)); CHECK_EQ(b[34], uint8_t(32));
    CHECK_EQ(size_t(le32(b, 40)), size_t(3000 + 4000) * 8);                                              // 1.5 s pre-roll (3000 frames) + 4000 frames pushed while recording
    FileIqSource src(p); std::vector<dsp::cf> x; while (src.read(x, 4096)) {} CHECK_EQ(x.size(), size_t(7000));
    CHECK_NEAR(x.front().real(), double(6000 - 3000) / 1e6, 2e-6);                                       // the pre-roll starts 1.5 s before the press
    for (size_t i = 1; i < x.size(); ++i) CHECK_NEAR(x[i].real() - x[i - 1].real(), 1e-6, 2e-6);        // pre-roll joins the live data without a gap
    std::remove(p.c_str());
}

TEST(recorder_a_grab_works_while_a_recording_is_running) {
    Recorder r({2, 32}); r.configure(1000.0, 3.0); auto v = ramp(0, 2000); r.push(v.data(), 2000);
    const std::string rec = tmp("during_rec"), g = tmp("during_grab"); CHECK(r.start(rec, 0)); v = ramp(2000, 500); r.push(v.data(), 500);
    CHECK(r.grab(1.0, g)); wait_idle(r); CHECK(r.status().last_path == g); CHECK(r.status().recording);       // the grab finished while the recording is still open
    r.stop(); FileIqSource a(g); std::vector<dsp::cf> x; while (a.read(x, 4096)) {} CHECK_EQ(x.size(), size_t(1000)); CHECK_NEAR(x.back().real(), 2499.0 / 1e6, 2e-6);
    FileIqSource b(rec); std::vector<dsp::cf> y; while (b.read(y, 4096)) {} CHECK_EQ(y.size(), size_t(500)); std::remove(rec.c_str()); std::remove(g.c_str());
}
TEST(recorder_audio_is_16_bit_mono_and_clips_at_full_scale) {
    Recorder r({1, 16}); r.configure(48000.0, 2.0); std::vector<float> a(4800); for (size_t i = 0; i < a.size(); ++i) a[i] = (i % 4 == 0) ? 0.5f : (i % 4 == 1) ? -0.5f : (i % 4 == 2) ? 2.0f : -3.0f;   // the last two exceed full scale
    r.push(a.data(), a.size()); const std::string p = tmp("audio"); CHECK(r.grab(1.0, p)); wait_idle(r);
    const auto b = slurp(p); CHECK_EQ(le32(b, 24), 48000u); CHECK_EQ(b[22], uint8_t(1)); CHECK_EQ(b[34], uint8_t(16)); CHECK_EQ(size_t(le32(b, 40)), size_t(4800) * 2);
    auto s16 = [&](size_t i) { return int16_t(b[44 + 2 * i] | b[45 + 2 * i] << 8); };
    CHECK_EQ(int(s16(0)), 16384); CHECK_EQ(int(s16(1)), -16384); CHECK_EQ(int(s16(2)), 32767); CHECK_EQ(int(s16(3)), -32768); std::remove(p.c_str());
}
TEST(recorder_size_limit_and_slow_disk_are_reported_not_silent) {
    { Recorder r({2, 32}); r.configure(1000.0, 1.0); r.set_limits(44 + 8 * 1500, 1u << 30); const std::string p = tmp("limit"); CHECK(r.start(p, 0));
      for (int i = 0; i < 5; ++i) { auto v = ramp(size_t(i) * 500, 500); r.push(v.data(), 500); }
      r.stop(); CHECK(r.status().error.find("size limit") != std::string::npos); const auto b = slurp(p); CHECK(b.size() <= 44 + 8 * 1500 + 8); CHECK_EQ(le32(b, 40), uint32_t(b.size() - 44)); std::remove(p.c_str()); }
    { Recorder r({2, 32}); r.configure(1000.0, 1.0); r.set_limits(1u << 30, 4000); const std::string p = tmp("queue"); CHECK(r.start(p, 0));
      auto v = ramp(0, 400); for (int i = 0; i < 50; ++i) r.push(v.data(), 400);                           // faster than the (tiny) queue can drain
      r.stop(); std::remove(p.c_str()); }                                                                  // may or may not drop depending on the writer's speed: must not crash or hang
}
TEST(recorder_unwritable_path_fails_cleanly_and_configure_finishes_a_recording) {
    Recorder r({2, 32}); r.configure(1000.0, 1.0); CHECK(!r.start("/nonexistent_dir_for_flex/x.wav", 0)); CHECK(!r.status().recording);
    auto v = ramp(0, 100); r.push(v.data(), 100); CHECK(r.grab(1.0, "/nonexistent_dir_for_flex/g.wav")); wait_idle(r); CHECK(r.status().error.find("cannot write") != std::string::npos);                // the failure is reported, not swallowed
    const std::string p = tmp("cfg"); CHECK(r.start(p, 0)); r.push(v.data(), 100); r.configure(2000.0, 1.0); CHECK(!r.status().recording); CHECK_EQ(r.buffered_seconds(), 0.0);   // new rate: the old file is finished, the buffer is empty
    const auto b = slurp(p); CHECK_EQ(le32(b, 24), 1000u); CHECK_EQ(size_t(le32(b, 40)), size_t(100) * 8); std::remove(p.c_str());
}
TEST(recorder_concurrent_producer_with_start_stop_and_grabs_is_safe) {
    Recorder r({2, 32}); r.configure(48000.0, 2.0); std::atomic<bool> stop{false}; std::thread prod([&] { size_t pos = 0; while (!stop) { auto v = ramp(pos, 480); r.push(v.data(), 480); pos += 480; std::this_thread::sleep_for(std::chrono::microseconds(500)); } });
    for (int i = 0; i < 6; ++i) { const std::string p = tmp("conc"); r.start(p, 0.2); std::this_thread::sleep_for(std::chrono::milliseconds(30)); r.grab(0.1, tmp("concg")); r.stop(); wait_idle(r); std::remove(p.c_str()); }
    stop = true; prod.join(); r.stop(); std::remove(tmp("concg").c_str());
}
