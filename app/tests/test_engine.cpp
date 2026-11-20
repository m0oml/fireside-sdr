// RxEngine: manual stepping, bounded queues, reconfigure, end of file, real-time pacing on the worker thread, live sources, errors, concurrent consumers.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include "fireside/capture.h"
#include "fireside/rx_engine.h"
#include "test_util.h"
using namespace fireside; using dsp::cf;
using namespace std::chrono_literals;

namespace {
RxOptions opts(bool threaded, dsp::Mode m = dsp::Mode::USB) { RxOptions o; o.threaded = threaded; o.dc.mode = m; o.rc.if_offset_hz = 9000; return o; }
std::unique_ptr<RxEngine> demo_engine(RxOptions o) { auto e = std::make_unique<RxEngine>(); e->set_source(std::make_unique<DemoIqSource>(48000)); e->configure(o); return e; }
struct SilentPcm : IPcm {
    int ch = 2; bool fail_after = false; int calls = 0; bool tone = false;
    int channels() const override { return ch; } double rate() const override { return 48000; } std::string describe() const override { return "silent pcm"; }
    size_t read(int32_t* out, size_t frames, double) override {
        if (fail_after && ++calls > 5) throw IoError("pcm: device gone");
        const size_t n = std::min<size_t>(frames, 480);                                       // hands out 10 ms at a time, instantly
        for (size_t i = 0; i < n * size_t(ch); ++i) out[i] = tone && i % 2 == 0 ? 0x10000000 : 0;
        std::this_thread::sleep_for(1ms); return n;
    }
};
}  // namespace

TEST(engine_manual_stepping_is_deterministic) {
    auto e = demo_engine(opts(false));
    CHECK_EQ(e->advance(48000), size_t(48000));
    auto st = e->status();
    CHECK_EQ(st.samples, uint64_t(48000)); CHECK_EQ(st.audio_samples, uint64_t(48000)); CHECK_EQ(st.fft_size, size_t(4096)); CHECK(st.sample_rate == 48000.0);
    CHECK(st.frames >= 20 && st.frames <= 24);
    CHECK(std::fabs(st.peak_hz - 1000.0) < 25.0 || std::fabs(st.peak_hz + 5000.0) < 25.0);   // the demo's two strong carriers
    CHECK(st.signal_dbfs > -40.0 && st.signal_dbfs < 0.0);
    auto frames = e->take_frames(); CHECK_EQ(frames.size(), size_t(st.frames)); CHECK_EQ(frames[0].size(), size_t(4096)); CHECK(e->take_frames().empty());
    std::vector<float> audio; CHECK_EQ(e->take_audio(audio), size_t(48000)); CHECK_EQ(audio.size(), size_t(48000)); CHECK_EQ(e->take_audio(audio), size_t(0));
    CHECK_NEAR(e->bin_hz(2048), 0.0, 1e-9); CHECK_NEAR(e->bin_hz(2049), 48000.0 / 4096.0, 1e-9);
}
TEST(engine_queues_are_bounded_and_count_what_they_drop) {
    RxOptions o = opts(false); o.max_frames = 5; o.max_audio = 1000;
    auto e = demo_engine(o); e->advance(48000);
    auto st = e->status(); CHECK(e->take_frames().size() == 5); CHECK(st.frames_dropped == st.frames - 5);
    std::vector<float> a; CHECK_EQ(e->take_audio(a), size_t(1000)); CHECK_EQ(st.audio_dropped, uint64_t(47000));
}
TEST(engine_bad_reconfigure_throws_and_keeps_the_old_chain) {
    auto e = demo_engine(opts(false)); e->advance(4800);
    RxOptions bad = opts(false); bad.dc.audio_rate = 44100;                                    // 48 kHz is not a multiple of 44.1 kHz
    CHECK_THROWS(e->configure(bad), std::invalid_argument);
    CHECK_EQ(e->advance(4800), size_t(4800)); CHECK_EQ(e->status().samples, uint64_t(9600));
    RxOptions lsb = opts(false, dsp::Mode::LSB); e->configure(lsb); CHECK_EQ(e->advance(4800), size_t(4800));   // a good change keeps the source running
    CHECK_EQ(e->status().samples, uint64_t(14400));
}
TEST(engine_file_source_ends_and_stays_ended) {
    const std::string p = (std::filesystem::temp_directory_path() / ("firesideeng_" + std::to_string(::getpid()) + ".wav")).string();
    std::vector<float> il; for (auto& v : dsp::synth_tones(48000, 48000, {{10000, 0.3}})) { il.push_back(v.real()); il.push_back(v.imag()); }
    dsp::write_wav_f32(p, 48000, 2, il);
    RxEngine e; e.set_source(std::make_unique<FileIqSource>(p)); e.configure(opts(false));
    CHECK_EQ(e.advance(60000), size_t(48000)); CHECK(!e.status().ended); CHECK_EQ(e.advance(100), size_t(0));
    CHECK(e.status().ended); e.start(); CHECK(!e.running());                                    // no restart of a finished file until a new source is set
    e.set_source(std::make_unique<FileIqSource>(p)); e.configure(opts(false)); CHECK(!e.status().ended); CHECK_EQ(e.advance(1000), size_t(1000));
    std::filesystem::remove(p);
}
TEST(engine_threaded_paces_in_real_time_and_stops_cleanly) {
    auto e = demo_engine(opts(true)); e->start(); CHECK(e->running());
    std::this_thread::sleep_for(700ms);
    auto st = e->status(); const double s = double(st.samples) / 48000.0;
    CHECK(s > 0.35 && s < 0.95);                                                               // real time within scheduling jitter
    CHECK(st.frames > 3); CHECK(st.running);
    std::vector<float> a; e->take_audio(a); CHECK(double(a.size()) > 0.3 * 48000);
    e->stop(); CHECK(!e->running()); const auto n1 = e->status().samples; std::this_thread::sleep_for(100ms); CHECK_EQ(e->status().samples, n1);   // really stopped
    e->start(); std::this_thread::sleep_for(300ms); CHECK(e->status().samples > n1); e->stop();                                                      // and restartable
}
TEST(engine_concurrent_consumers_and_reconfigure_while_running) {
    auto e = demo_engine(opts(true)); e->start();
    std::atomic<bool> go{true}; size_t frames = 0, audio = 0;
    std::thread consumer([&] { std::vector<float> a; while (go) { frames += e->take_frames().size(); audio += e->take_audio(a); a.clear(); (void)e->status(); std::this_thread::sleep_for(2ms); } });
    for (auto m : {dsp::Mode::LSB, dsp::Mode::AM, dsp::Mode::CW, dsp::Mode::FM, dsp::Mode::USB}) { std::this_thread::sleep_for(60ms); e->configure(opts(true, m)); }
    std::this_thread::sleep_for(100ms); go = false; consumer.join(); e->stop();
    CHECK(frames > 0); CHECK(audio > 10000); CHECK(e->status().error.empty());
}
TEST(engine_set_source_while_running_switches_cleanly) {
    auto e = demo_engine(opts(true)); e->start(); std::this_thread::sleep_for(100ms);
    e->set_source(std::make_unique<DemoIqSource>(96000)); CHECK(!e->running()); CHECK(e->status().samples == 0 && e->status().sample_rate == 96000.0);
    e->configure(opts(true)); e->start(); std::this_thread::sleep_for(300ms); CHECK(e->status().samples > 5000); e->stop();
}
TEST(engine_live_source_reports_dead_stream_and_pcm_errors) {
    {   // silent live PCM: consumed as delivered, flagged dead after > 1 s of zeros
        RxEngine e; auto pcm = std::make_unique<SilentPcm>(); e.set_source(std::make_unique<PcmIqSource>(std::move(pcm), 0, 1, 480)); e.configure(opts(true));
        e.start(); const auto t0 = std::chrono::steady_clock::now(); bool dead = false;
        while (std::chrono::steady_clock::now() - t0 < 6s && !dead) { std::this_thread::sleep_for(50ms); dead = e.status().stream_dead; }
        e.stop(); CHECK(dead); CHECK(e.status().live);
    }
    {   // device failure: error is recorded, engine stops, stop() is safe
        RxEngine e; auto pcm = std::make_unique<SilentPcm>(); pcm->fail_after = true; pcm->tone = true;
        e.set_source(std::make_unique<PcmIqSource>(std::move(pcm), 0, 1, 480)); e.configure(opts(true)); e.start();
        for (int i = 0; i < 100 && e.status().error.empty(); ++i) std::this_thread::sleep_for(20ms);
        CHECK(e.status().error.find("device gone") != std::string::npos); CHECK(!e.running()); e.stop();
    }
}
TEST(engine_without_source_or_config_is_inert) {
    RxEngine e; e.start(); CHECK(!e.running()); CHECK_EQ(e.advance(100), size_t(0)); CHECK(e.take_frames().empty());
    e.set_source(std::make_unique<DemoIqSource>()); e.start(); CHECK(!e.running());          // no config yet
    e.stop(); e.stop();
}

// ---- decoders on the worker thread: the whole receive path (I/Q -> demodulator -> audio -> text)
namespace {
struct VecSource : IqSource {                                  // plays a fixed I/Q vector once at 48 kHz
    std::vector<cf> x; size_t pos = 0; explicit VecSource(std::vector<cf> v) : x(std::move(v)) {}
    double sample_rate() const override { return 48000; } std::string describe() const override { return "test"; }
    size_t read(std::vector<cf>& out, size_t n) override { const size_t k = std::min(n, x.size() - pos); out.insert(out.end(), x.begin() + long(pos), x.begin() + long(pos + k)); pos += k; return k; }
};
std::string decode_through_engine(std::vector<cf> iq, dsp::Mode mode, std::unique_ptr<decode::TextDecoder> dec) {
    RxEngine e; e.set_source(std::make_unique<VecSource>(std::move(iq))); RxOptions o = opts(false, mode); o.rc.if_offset_hz = 0; e.configure(o); e.set_decoder(std::move(dec));
    while (e.advance(4800) > 0) {}
    std::string t; e.take_text(t); return t;
}
std::vector<cf> real_to_iq(const std::vector<float>& x) { std::vector<cf> z(x.size()); for (size_t i = 0; i < x.size(); ++i) z[i] = cf(x[i], 0.0f); return z; }   // a real signal in I only: the USB filter keeps its +f half
}  // namespace

TEST(engine_decodes_cw_psk31_and_rtty_through_the_real_receive_path) {
    {   // CW: a keyed carrier exactly on the dial (envelope + 0j); the CW demodulator turns it into a 700 Hz tone
        auto env = decode::morse_envelope("VVV CQ DE M0OML K ", 20); std::vector<cf> z(env.size()); for (size_t i = 0; i < env.size(); ++i) z[i] = cf(0.3f * env[i], 0.0f);
        const auto t = decode_through_engine(z, dsp::Mode::CW, std::make_unique<decode::CwDecoder>());
        CHECK(t.find("CQ DE M0OML K") != std::string::npos);
    }
    {   // PSK31 at 1000 Hz audio in USB
        const auto t = decode_through_engine(real_to_iq(decode::synth_psk31("CQ DE M0OML K ", 1000, 48000, 0.6, 3.0)), dsp::Mode::USB, std::make_unique<decode::Psk31Decoder>());
        CHECK(t.find("CQ DE M0OML K") != std::string::npos);
    }
    {   // RTTY at 2125/2295 Hz in USB (needs the wider voice filter: 2.7 kHz is enough for 2295 Hz)
        const auto t = decode_through_engine(real_to_iq(decode::synth_rtty("CQ DE M0OML K ", 2125, 170, 45.45, 48000, 0.6, 1.0)), dsp::Mode::USB, std::make_unique<decode::RttyDecoder>());
        CHECK(t.find("CQ DE M0OML K") != std::string::npos);
    }
}
TEST(engine_decoder_status_text_and_switching) {
    auto env = decode::morse_envelope("VVV CQ DE M0OML K ", 20); std::vector<cf> z(env.size()); for (size_t i = 0; i < env.size(); ++i) z[i] = cf(0.3f * env[i], 0.0f);
    RxEngine e; e.set_source(std::make_unique<VecSource>(z)); RxOptions o = opts(false, dsp::Mode::CW); o.rc.if_offset_hz = 0; e.configure(o);
    CHECK(e.status().decoder.empty()); e.set_decoder(std::make_unique<decode::CwDecoder>());
    for (int i = 0; i < 100; ++i) e.advance(4800);
    CHECK(e.status().decoder.find("WPM") != std::string::npos);
    std::string t; CHECK(e.take_text(t) > 5); std::string again; CHECK_EQ(e.take_text(again), size_t(0));                    // taking clears
    e.set_decoder(nullptr); CHECK(e.status().decoder.empty()); e.advance(4800); std::string none; CHECK_EQ(e.take_text(none), size_t(0));
}
TEST(engine_decoder_runs_on_the_worker_thread_with_a_live_consumer) {
    auto env = decode::morse_envelope("VVV CQ DE M0OML K ", 30); std::vector<cf> z; for (int rep = 0; rep < 2; ++rep) for (float g : env) z.push_back(cf(0.3f * g, 0.0f));         // ~4 s of real-time audio
    RxEngine e; e.set_source(std::make_unique<VecSource>(z)); RxOptions o = opts(true, dsp::Mode::CW); o.rc.if_offset_hz = 0; e.configure(o); e.set_decoder(std::make_unique<decode::CwDecoder>()); e.start();
    std::string text; for (int i = 0; i < 600 && !e.status().ended; ++i) { e.take_text(text); std::this_thread::sleep_for(20ms); } e.take_text(text); e.stop();
    CHECK(text.find("CQ DE M0OML K") != std::string::npos);
}

// ---------------------------------------------------------------- recording taps
namespace {
struct LiveVec : IqSource {                                    // a "live" source: 48 kHz tone blocks forever
    size_t sent = 0; double sample_rate() const override { return 48000; } std::string describe() const override { return "live test"; } bool live() const override { return true; }
    size_t read(std::vector<cf>& out, size_t n) override { for (size_t i = 0; i < n; ++i) out.push_back(std::polar(0.1f, float(0.3 * double(sent + i)))); sent += n; return n; }
};
}
TEST(engine_taps_get_the_live_blocks_and_the_audio_and_never_a_files_blocks) {
    for (bool live : {true, false}) {
        RxEngine e; if (live) e.set_source(std::make_unique<LiveVec>()); else e.set_source(std::make_unique<VecSource>(std::vector<cf>(48000, cf(0.1f, 0.0f))));
        e.configure(opts(false, dsp::Mode::USB)); size_t iq_n = 0, au_n = 0; double iq_rate = 0, au_rate = 0; float first_i = -9;
        e.set_taps([&](const cf* x, size_t n, double r) { if (!iq_n) first_i = x[0].real(); iq_n += n; iq_rate = r; }, [&](const float*, size_t n, double r) { au_n += n; au_rate = r; });
        e.start(); for (int i = 0; i < 20; ++i) e.advance(4800);
        if (live) { CHECK_EQ(iq_n, size_t(96000)); CHECK_NEAR(iq_rate, 48000.0, 1e-9); CHECK_NEAR(first_i, 0.1, 1e-6); CHECK(au_n > 90000); CHECK_NEAR(au_rate, 48000.0, 1e-9); }   // exactly the source's blocks, untouched by the chain
        else { CHECK_EQ(iq_n, size_t(0)); CHECK_EQ(au_n, size_t(0)); }                                      // a recording being played is not "live": nothing to record
        e.set_taps(nullptr, nullptr); e.advance(4800);
    }
}
