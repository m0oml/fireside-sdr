// VOX: the speech detector, its always-listening input with pre-roll, the no-RF feedback test, and the voice over's monitor.
#include <chrono>
#include <cmath>
#include <thread>
#include "fireside/vox.h"
#include "test_util.h"
using namespace fireside;
namespace {
std::vector<float> tone(double amp, double s, double hz = 800) { std::vector<float> v(size_t(s * 48000)); for (size_t i = 0; i < v.size(); ++i) v[i] = float(amp * std::sin(2 * M_PI * hz * double(i) / 48000)); return v; }
struct ScriptIn : txc::IAudioIn {                                       // real time: amp(t) x 800 Hz
    std::function<double(double)> amp; size_t pos = 0; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(); std::atomic<bool> fail{false};
    explicit ScriptIn(std::function<double(double)> a) : amp(std::move(a)) {}
    size_t read(float* x, size_t n, double) override {
        if (fail) throw IoError("mic unplugged");
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(1e6 * double(pos + n) / 48000)));
        for (size_t i = 0; i < n; ++i, ++pos) { const double t = double(pos) / 48000; x[i] = float(amp(t) * std::sin(2 * M_PI * 800 * t)); } return n; }
    std::string describe() const override { return "script"; }
};
}  // namespace

TEST(vox_detector_keys_on_speech_within_the_attack_and_holds_for_the_hang) {
    vox::VoxConfig c; c.threshold_dbfs = -30; c.hang_s = 0.5; c.attack_ms = 10; vox::VoxDetector d(c);
    auto quiet = tone(0.01, 1.0), loud = tone(0.2, 0.3), none = tone(0.0, 1.0);    // -40 dBFS room noise, -14 dBFS speech
    CHECK(!d.process(quiet.data(), quiet.size()));
    CHECK(!d.process(loud.data(), 240)); CHECK(d.process(loud.data() + 240, 480));      // 5 ms: not yet; 15 ms: on
    d.process(loud.data(), loud.size());
    CHECK(d.process(none.data(), 48000 * 45 / 100)); CHECK(!d.process(none.data(), 48000 * 25 / 100));   // held 0.45 s; off 0.5 s after the level fell below the threshold (~0.1 s after the sound stopped)
    vox::VoxConfig a = c; a.anti_vox = true; a.anti_gain_db = 6; vox::VoxDetector e(a);
    CHECK(!e.process(loud.data(), loud.size(), -10));                                 // the speaker at -10 dBFS: -14 at the mic is the speaker
    CHECK(e.process(loud.data(), loud.size(), -30));                                  // a quiet speaker: speech keys
    CHECK_THROWS(vox::VoxDetector(vox::VoxConfig{48000, 5}), std::invalid_argument);
}
TEST(vox_input_gives_the_over_the_pre_roll_so_the_first_syllable_is_sent) {
    auto in = std::make_unique<ScriptIn>([](double t) { return t > 0.5 ? 0.2 : 0.0; });   // silence, then speech from 0.5 s
    vox::VoxConfig c; c.threshold_dbfs = -30; c.hang_s = 0.5; vox::VoxInput v(std::move(in), c, 0.0, 0.15);
    for (int i = 0; i < 100 && !v.on(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(v.on()); v.set_reading(true);
    std::vector<float> x(48000); size_t got = 0; while (got < 9600) got += v.read(x.data() + got, x.size() - got, 0.2);
    size_t first = got; for (size_t i = 0; i < got; ++i) if (std::fabs(x[i]) > 0.05) { first = i; break; }
    CHECK(first > 48000 * 8 / 100 && first < 48000 * 20 / 100);                       // the speech onset sits ~0.15 s into what the over reads (the pre-roll)
    v.set_reading(false);
    auto bad = std::make_unique<ScriptIn>([](double) { return 0.0; }); auto* bp = bad.get(); vox::VoxInput f(std::move(bad), c, 0.0);
    bp->fail = true; std::this_thread::sleep_for(std::chrono::milliseconds(80)); CHECK(f.failed()); CHECK(!f.on());
    CHECK_THROWS(f.read(x.data(), 10, 0.01), IoError);
    vox::VoxConfig shorth = c; shorth.hang_s = 0.2; CHECK_THROWS(vox::VoxInput(std::make_unique<ScriptIn>([](double) { return 0.0; }), shorth, 0.0, 0.15), std::invalid_argument);
}
TEST(vox_feedback_test_plays_the_processed_voice_and_touches_nothing_else) {
    std::mutex m; std::vector<float> heard;
    {
        vox::MicLoop loop(std::make_unique<ScriptIn>([](double) { return 0.1; }), tx::voice_chain_defaults(), [&](const float* a, size_t n) { std::lock_guard<std::mutex> l(m); heard.insert(heard.end(), a, a + n); });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::fprintf(stderr, "  feedback test: in %.1f dBFS, out %.1f dBFS\n", loop.in_dbfs(), loop.out_dbfs());
        CHECK_NEAR(loop.in_dbfs(), -20, 1.5); CHECK(loop.out_dbfs() > -12);               // the processing drives speech up towards the ceiling
    }
    std::lock_guard<std::mutex> l(m); CHECK(heard.size() > 48000 / 4);
    double pk = 0; for (size_t i = heard.size() / 2; i < heard.size(); ++i) pk = std::max(pk, double(std::fabs(heard[i]))); CHECK(pk > 0.2 && pk <= 1.0);
}
