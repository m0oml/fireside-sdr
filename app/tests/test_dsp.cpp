// Receive-chain tests on synthetic I/Q: FFT, spectrum levels, 9 kHz IF shift, mirror, waterfall, demodulators, streaming, WAV, watchdog.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include "fireside/cat.h"
#include "fireside/dsp.h"
#include "test_util.h"
using namespace fireside::dsp;

namespace {
double tone_amp(const std::vector<float>& a, size_t from, size_t n, double hz, double fs) {      // single-bin DFT amplitude of a real signal
    std::complex<double> s(0, 0);
    for (size_t i = 0; i < n; ++i) s += double(a[from + i]) * std::polar(1.0, -2 * kPi * hz * double(from + i) / fs);
    return 2.0 * std::abs(s) / double(n);
}
size_t peak_bin(const std::vector<float>& db) { return size_t(std::max_element(db.begin(), db.end()) - db.begin()); }
std::string tmp(const char* n) { return (std::filesystem::temp_directory_path() / (std::string("firesidedsp_") + std::to_string(::getpid()) + "_" + n)).string(); }

// Runs `iq` (raw radio I/Q) through a ReceiveChain and returns the audio.
std::vector<float> run(const std::vector<cf>& iq, ReceiveConfig rc, Mode m, size_t chunk = 0, double audio_rate = 48000) {
    DemodConfig dc; dc.mode = m; dc.audio_rate = audio_rate;
    ReceiveChain ch(rc, dc); std::vector<float> a;
    if (!chunk) chunk = iq.size();
    for (size_t o = 0; o < iq.size(); o += chunk) ch.process(iq.data() + o, std::min(chunk, iq.size() - o), a);
    return a;
}
// Same, with a full DemodConfig (input_rate is taken from rc).
std::vector<float> run_cfg(const std::vector<cf>& iq, ReceiveConfig rc, DemodConfig dc, size_t chunk = 0) {
    ReceiveChain ch(rc, dc); std::vector<float> a;
    if (!chunk) chunk = iq.size();
    for (size_t o = 0; o < iq.size(); o += chunk) ch.process(iq.data() + o, std::min(chunk, iq.size() - o), a);
    return a;
}
const ReceiveConfig kCentred{48000, 0.0, false};      // for tests that feed an already dial-centred signal
DemodConfig cfg(Mode m, AgcMode agc = AgcMode::Off) { DemodConfig d; d.mode = m; d.agc = agc; return d; }
// A tone `hz` above the dial (already centred, i.e. after the IF shift) whose amplitude steps from a0 to a1 at sample `at`.
std::vector<cf> stepped(size_t n, size_t at, double a0, double a1, double hz = 1000) {
    std::vector<cf> x(n); for (size_t i = 0; i < n; ++i) x[i] = float(i < at ? a0 : a1) * cf(float(std::cos(2 * kPi * hz * double(i) / 48000)), float(std::sin(2 * kPi * hz * double(i) / 48000)));
    return x;
}
}  // namespace

TEST(dsp_fft_matches_naive_dft) {
    const size_t n = 64; Fft f(n); auto x = synth_tones(1000, n, {{70, 0.7}, {-210, 0.3}}, 0.2, 5);
    auto y = x; f.forward(y.data());
    for (size_t k = 0; k < n; ++k) {
        std::complex<double> s(0, 0);
        for (size_t i = 0; i < n; ++i) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * kPi * double(k * i) / double(n));
        CHECK_NEAR(y[k].real(), s.real(), 1e-3); CHECK_NEAR(y[k].imag(), s.imag(), 1e-3);
    }
    CHECK_THROWS(Fft(100), std::invalid_argument);
}
TEST(dsp_full_scale_tone_reads_0_dbfs_and_has_deep_sidelobes) {
    const double fs = 48000; const size_t N = 4096; SpectrumAnalyzer sp(fs, N);
    const double hz = 100.0 * fs / double(N);                                       // exactly on a bin
    auto x = synth_tones(fs, N, {{hz, 1.0}}); CHECK_EQ(sp.push(x.data(), x.size()), size_t(1));
    const size_t k = sp.bin_for_hz(hz); CHECK_EQ(peak_bin(sp.db()), k);
    CHECK_NEAR(sp.db()[k], 0.0, 0.05); CHECK_NEAR(sp.bin_hz(k), hz, 1e-6);
    CHECK(sp.db()[k + 200] < -90.0f);
    SpectrumAnalyzer half(fs, N); auto y = synth_tones(fs, N, {{hz, 0.5}}); half.push(y.data(), y.size());
    CHECK_NEAR(half.db()[k], -6.02, 0.05);
}
TEST(dsp_spectrum_frames_overlap_by_half_and_callback_fires) {
    SpectrumAnalyzer sp(48000, 4096); size_t calls = 0; sp.on_frame = [&](const std::vector<float>& r) { ++calls; CHECK_EQ(r.size(), size_t(4096)); };
    auto x = synth_tones(48000, 4096 + 3 * 2048, {{1000, 1}});
    size_t made = 0; for (size_t o = 0; o < x.size(); o += 1000) made += sp.push(x.data() + o, std::min<size_t>(1000, x.size() - o));
    CHECK_EQ(made, size_t(4)); CHECK_EQ(calls, size_t(4)); CHECK_EQ(sp.frames(), size_t(4));
}
TEST(dsp_averaging_smooths_noise) {
    SpectrumAnalyzer raw(48000, 1024, 1.0f), avg(48000, 1024, 0.1f);
    auto x = synth_tones(48000, 1024 * 40, {}, 0.1, 9);
    raw.push(x.data(), x.size()); avg.push(x.data(), x.size());
    auto spread = [](const std::vector<float>& d) { double m = 0, v = 0; for (float f : d) m += f; m /= double(d.size()); for (float f : d) v += (f - m) * (f - m); return std::sqrt(v / double(d.size())); };
    CHECK(spread(avg.db()) < 0.6 * spread(raw.db()));
    CHECK_THROWS(SpectrumAnalyzer(48000, 1024, 0.0f), std::invalid_argument);
}

TEST(dsp_if_offset_puts_the_dial_at_zero) {
    // radio DDS is 9 kHz below the dial: a signal 2 kHz above the dial arrives at +11 kHz, the dial itself at +9 kHz
    ReceiveConfig rc;
    for (auto [raw_hz, want_hz] : {std::pair{11000.0, 2000.0}, {9000.0, 0.0}, {5000.0, -4000.0}}) {
        DemodConfig dc; ReceiveChain ch(rc, dc); std::vector<float> a;
        auto x = synth_tones(rc.sample_rate, 8192, {{raw_hz, 0.5}}); ch.process(x.data(), x.size(), a);
        auto& sp = ch.spectrum(); CHECK(std::fabs(sp.bin_hz(peak_bin(sp.db())) - want_hz) <= sp.bin_hz(1) - sp.bin_hz(0));
    }
    CHECK_NEAR(dds_for_dial_mhz(7.1), 7.091, 1e-12);
}
TEST(dsp_mirror_flag_compensates_swapped_iq) {
    ReceiveConfig plain; ReceiveConfig mir; mir.mirror = true;
    auto raw = synth_tones(48000, 8192, {{-11000, 0.5}});                           // mirrored radio: +2 kHz above dial shows at -11 kHz
    { DemodConfig dc; ReceiveChain c(mir, dc); std::vector<float> a; c.process(raw.data(), raw.size(), a);
      CHECK_NEAR(c.spectrum().bin_hz(peak_bin(c.spectrum().db())), 2000.0, 12.0); }
    { DemodConfig dc; ReceiveChain c(plain, dc); std::vector<float> a; c.process(raw.data(), raw.size(), a);
      CHECK_NEAR(c.spectrum().bin_hz(peak_bin(c.spectrum().db())), -20000.0, 12.0); }   // wrong guess puts it elsewhere
}
TEST(dsp_if_shift_is_phase_continuous_across_chunks) {
    ReceiveConfig rc; auto x = synth_tones(48000, 5000, {{12345, 0.3}}, 0.01, 4);
    auto a = x, b = x; IfShifter s1(rc), s2(rc);
    s1.process(a.data(), a.size()); size_t o = 0; for (size_t n : {1u, 999u, 1500u, 2500u}) { s2.process(b.data() + o, n); o += n; }
    for (size_t i = 0; i < x.size(); ++i) CHECK(std::abs(a[i] - b[i]) < 1e-5f);
}

TEST(dsp_waterfall_keeps_newest_rows_and_scales) {
    Waterfall wf(4, 3);
    for (int i = 0; i < 5; ++i) wf.push({float(-100 + i), -100, -50, 0});
    CHECK_EQ(wf.rows(), size_t(3)); CHECK_EQ(wf.row(0)[0], -96.0f); CHECK_EQ(wf.row(2)[0], -98.0f);
    auto g = wf.to_gray(-100, 0); CHECK_EQ(g.size(), size_t(12));
    CHECK_EQ(g[1], 0); CHECK_EQ(g[2], 128); CHECK_EQ(g[3], 255);                    // -100 -> 0, -50 -> 128 (rounded), 0 -> 255
    CHECK_THROWS(wf.push({1, 2}), std::invalid_argument);
}

TEST(dsp_usb_passes_upper_sideband_and_rejects_lower) {
    ReceiveConfig rc; const size_t n = 48000;
    auto up = run(synth_tones(48000, n, {{9000 + 1000, 1.0}}), rc, Mode::USB);
    auto dn = run(synth_tones(48000, n, {{9000 - 1000, 1.0}}), rc, Mode::USB);
    CHECK_NEAR(tone_amp(up, 20000, 8192, 1000, 48000), 1.0, 0.02);
    CHECK(tone_amp(dn, 20000, 8192, 1000, 48000) < 0.01);                           // > 40 dB opposite-sideband rejection
}
TEST(dsp_lsb_passes_lower_sideband_and_rejects_upper) {
    ReceiveConfig rc; const size_t n = 48000;
    auto up = run(synth_tones(48000, n, {{9000 + 1000, 1.0}}), rc, Mode::LSB);
    auto dn = run(synth_tones(48000, n, {{9000 - 1000, 1.0}}), rc, Mode::LSB);
    CHECK_NEAR(tone_amp(dn, 20000, 8192, 1000, 48000), 1.0, 0.02);
    CHECK(tone_amp(up, 20000, 8192, 1000, 48000) < 0.01);
}
TEST(dsp_usb_band_edges) {
    ReceiveConfig rc;
    auto low = run(synth_tones(48000, 48000, {{9000 + 50, 1.0}}), rc, Mode::USB);        // below 200 Hz: rejected
    auto high = run(synth_tones(48000, 48000, {{9000 + 4000, 1.0}}), rc, Mode::USB);     // above 2800 Hz: rejected
    CHECK(tone_amp(low, 20000, 8192, 50, 48000) < 0.01); CHECK(tone_amp(high, 20000, 8192, 4000, 48000) < 0.01);
}
TEST(dsp_am_recovers_the_modulation) {
    ReceiveConfig rc;                                                                // carrier 0.5, 50 % modulated at 1 kHz: sidebands 0.125 each
    auto a = run(synth_tones(48000, 96000, {{9000, 0.5}, {10000, 0.125}, {8000, 0.125}}), rc, Mode::AM);
    CHECK_NEAR(tone_amp(a, 60000, 8192, 1000, 48000), 0.25, 0.02);
    double dc = 0; for (size_t i = 60000; i < 68192; ++i) dc += a[i]; CHECK(std::fabs(dc / 8192) < 0.01);   // carrier DC removed
}
TEST(dsp_cw_tone_lands_on_the_pitch) {
    ReceiveConfig rc;
    auto a = run(synth_tones(48000, 48000, {{9000, 1.0}}), rc, Mode::CW);                // zero beat: audio at the 700 Hz pitch
    CHECK_NEAR(tone_amp(a, 20000, 8192, 700, 48000), 1.0, 0.02);
    auto b = run(synth_tones(48000, 48000, {{9000 + 100, 1.0}}), rc, Mode::CW);          // 100 Hz high: 800 Hz
    CHECK_NEAR(tone_amp(b, 20000, 8192, 800, 48000), 1.0, 0.03);
    auto c = run(synth_tones(48000, 48000, {{9000 + 1000, 1.0}}), rc, Mode::CW);         // outside the 500 Hz filter
    CHECK(tone_amp(c, 20000, 8192, 1700, 48000) < 0.01);
}
TEST(dsp_192k_stream_decimates_to_48k_audio) {
    ReceiveConfig rc; rc.sample_rate = 192000; const size_t n = 192000;
    auto a = run(synth_tones(192000, n, {{9000 + 1500, 1.0}}), rc, Mode::USB);
    CHECK_EQ(a.size(), n / 4);
    CHECK_NEAR(tone_amp(a, 20000, 8192, 1500, 48000), 1.0, 0.03);
    auto alias = run(synth_tones(192000, n, {{9000 + 30000, 1.0}}), rc, Mode::USB);          // would alias into the passband at 48 kHz without the anti-alias filter
    double worst = 0; for (double f : {1000.0, 2000.0, 1800.0, 2500.0}) worst = std::max(worst, tone_amp(alias, 20000, 8192, f, 48000));
    CHECK(worst < 0.01);
}
TEST(dsp_chunked_processing_equals_one_shot) {
    for (double fs : {48000.0, 192000.0}) {
        ReceiveConfig rc; rc.sample_rate = fs; auto x = synth_tones(fs, size_t(fs / 2), {{9000 + 1200, 0.6}, {9000 + 300, 0.3}}, 0.01, 3);
        auto whole = run(x, rc, Mode::USB), parts = run(x, rc, Mode::USB, 1001);
        CHECK_EQ(whole.size(), parts.size());
        for (size_t i = 0; i < whole.size(); ++i) CHECK(std::fabs(whole[i] - parts[i]) < 1e-4f);
    }
}
TEST(dsp_bad_configuration_is_rejected) {
    DemodConfig d; d.input_rate = 44100; d.audio_rate = 48000; CHECK_THROWS(Demodulator(d), std::invalid_argument);
    d.input_rate = 96000; d.audio_rate = 44100; CHECK_THROWS(Demodulator(d), std::invalid_argument);
    d = DemodConfig{}; d.lo_hz = 500; d.hi_hz = 400; CHECK_THROWS(Demodulator(d), std::invalid_argument);
    ReceiveConfig rc; rc.sample_rate = 0; CHECK_THROWS(IfShifter(rc), std::invalid_argument);
}
TEST(dsp_noise_generator_is_deterministic_and_calibrated) {
    auto a = synth_tones(48000, 100000, {}, 0.25, 7), b = synth_tones(48000, 100000, {}, 0.25, 7), c = synth_tones(48000, 100000, {}, 0.25, 8);
    CHECK(a == b); CHECK(!(a == c));
    double p = 0; for (auto& v : a) p += std::norm(v); CHECK_NEAR(std::sqrt(p / double(a.size())), 0.25, 0.005);
}

TEST(dsp_watchdog_trips_only_after_more_than_one_second_of_zero_buffers) {
    DeadStreamWatchdog w(48000, 1024); std::vector<cf> z(1024, cf(0, 0)), live(1024, cf(1e-6f, 0));
    for (int i = 0; i < 46; ++i) CHECK(!w.feed(z.data(), z.size()));
    CHECK(!w.feed(z.data(), z.size()));                                              // 47 buffers = 1.003 s: still not "more than"
    CHECK(w.feed(z.data(), z.size()));
    CHECK(!w.feed(live.data(), live.size()));                                        // any signal resets
    for (int i = 0; i < 47; ++i) CHECK(!w.feed(z.data(), z.size()));
    z[5] = cf(0, 1e-9f); CHECK(!w.feed(z.data(), z.size()));                         // one non-zero sample is a live stream
}

TEST(wav_roundtrip_8_channel_float_picks_rx1_pair) {
    const std::string p = tmp("8ch.wav"); const size_t n = 100; std::vector<float> il(n * 8, 0.0f);
    for (size_t i = 0; i < n; ++i) { il[i * 8] = 0.001f * float(i); il[i * 8 + 1] = -0.5f + 0.01f * float(i); il[i * 8 + 2] = 0.9f; }
    write_wav_f32(p, 96000, 8, il);
    auto r = read_iq_wav(p); CHECK_EQ(r.sample_rate, 96000.0); CHECK_EQ(r.channels, 8); CHECK_EQ(r.samples.size(), n);
    CHECK_NEAR(r.samples[10].real(), 0.01, 1e-6); CHECK_NEAR(r.samples[10].imag(), -0.4, 1e-6);
    auto r2 = read_iq_wav(p, 2, 0); CHECK_NEAR(r2.samples[10].real(), 0.9, 1e-6); CHECK_NEAR(r2.samples[10].imag(), 0.01, 1e-6);
    CHECK_THROWS(read_iq_wav(p, 0, 8), std::runtime_error);
    std::filesystem::remove(p);
}
TEST(wav_reads_s32_24_and_16_bit_pcm_as_arecord_writes_them) {
    auto build = [&](uint16_t bits, std::vector<int32_t> frames, int ch) {           // hand-built header independent of our writer
        std::vector<uint8_t> v; auto p32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }; auto p16 = [&](uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
        const int bps = bits / 8; for (char c : std::string("RIFF")) v.push_back(uint8_t(c)); p32(uint32_t(36 + frames.size() * bps)); for (char c : std::string("WAVEfmt ")) v.push_back(uint8_t(c));
        p32(16); p16(1); p16(uint16_t(ch)); p32(48000); p32(uint32_t(48000 * ch * bps)); p16(uint16_t(ch * bps)); p16(bits); for (char c : std::string("data")) v.push_back(uint8_t(c)); p32(uint32_t(frames.size() * bps));
        for (int32_t s : frames) {
            for (int b = 0; b < bps; ++b) v.push_back(uint8_t(uint32_t(s) >> (8 * (4 - bps + b))));
        }
        return v; };
    for (uint16_t bits : {16, 24, 32}) {
        const std::string p = tmp("pcm.wav"); auto v = build(bits, {int32_t(0x40000000), int32_t(0xC0000000), 0, 0}, 2);   // +0.5, -0.5 as the top bits of an S32 value
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(v.data()), long(v.size()));
        auto r = read_iq_wav(p); CHECK_EQ(r.samples.size(), size_t(2)); CHECK_NEAR(r.samples[0].real(), 0.5, 1e-4); CHECK_NEAR(r.samples[0].imag(), -0.5, 1e-4);
        std::filesystem::remove(p);
    }
}
TEST(wav_rejects_garbage_and_mono_pcm16_writer_works) {
    const std::string p = tmp("junk.wav"); std::ofstream(p) << "not a wav file at all";
    CHECK_THROWS(read_iq_wav(p), std::runtime_error); CHECK_THROWS(read_iq_wav(tmp("missing.wav")), std::runtime_error);
    write_wav_pcm16_mono(p, 48000, {0.5f, -0.5f, 2.0f});
    auto r = read_iq_wav(p, 0, 0); CHECK_EQ(r.samples.size(), size_t(3)); CHECK_NEAR(r.samples[0].real(), 0.5, 1e-3); CHECK_NEAR(r.samples[2].real(), 1.0, 1e-3);   // clipped
    std::filesystem::remove(p);
}

// ------------------------------------------------------------------ AGC, S-meter, FM, noise blanker
TEST(agc_brings_weak_and_strong_signals_to_the_same_level) {
    for (auto mode : {AgcMode::Fast, AgcMode::Med, AgcMode::Slow}) {
        double lvl[2];
        for (int k = 0; k < 2; ++k) {
            auto a = run_cfg(stepped(72000, 0, k ? 0.1 : 0.001, k ? 0.1 : 0.001), kCentred, cfg(Mode::USB, mode));
            lvl[k] = tone_amp(a, 50000, 8192, 1000, 48000);
        }
        CHECK_NEAR(lvl[0], 0.25, 0.03); CHECK_NEAR(lvl[1], 0.25, 0.03);
    }
    auto off0 = run_cfg(stepped(72000, 0, 0.001, 0.001), kCentred, cfg(Mode::USB)), off1 = run_cfg(stepped(72000, 0, 0.1, 0.1), kCentred, cfg(Mode::USB));
    CHECK(tone_amp(off1, 50000, 8192, 1000, 48000) > 50 * tone_amp(off0, 50000, 8192, 1000, 48000));   // without AGC the 40 dB difference remains
}
TEST(agc_never_overshoots_the_target_on_a_sudden_strong_signal) {
    auto a = run_cfg(stepped(48000, 24000, 0.001, 0.5), kCentred, cfg(Mode::USB, AgcMode::Fast));
    float peak = 0; for (size_t i = 0; i < a.size(); ++i) peak = std::max(peak, std::fabs(a[i]));
    CHECK(peak <= 0.2501f);                                                         // instant attack: |output| <= target
}
TEST(agc_hang_and_decay_follow_the_presets) {
    auto gain_after = [&](AgcMode m, double seconds_after_drop) {
        DemodConfig dc = cfg(Mode::USB, m); Demodulator d(dc); std::vector<float> a;
        auto loud = stepped(24000, 0, 0.5, 0.5); d.process(loud.data(), loud.size(), a);
        const double g0 = d.agc_gain_db();
        auto quiet = stepped(size_t(48000 * seconds_after_drop), 0, 0.001, 0.001); d.process(quiet.data(), quiet.size(), a);
        return d.agc_gain_db() - g0;
    };
    CHECK(gain_after(AgcMode::Slow, 0.4) < 1.0);                                    // inside the 500 ms hang: gain holds
    CHECK(gain_after(AgcMode::Slow, 4.0) > 25.0);                                   // and recovers afterwards
    CHECK(gain_after(AgcMode::Fast, 0.3) > 30.0);                                   // fast: no hang, 50 ms decay
    CHECK(gain_after(AgcMode::Fast, 0.3) > gain_after(AgcMode::Med, 0.3));
    CHECK(gain_after(AgcMode::Med, 0.3) > gain_after(AgcMode::Slow, 0.3));
}
TEST(agc_off_is_unity_and_bad_parameters_throw) {
    Demodulator d(cfg(Mode::USB)); std::vector<float> a; auto x = stepped(4800, 0, 0.1, 0.1); d.process(x.data(), x.size(), a);
    CHECK_NEAR(d.agc_gain_db(), 0.0, 1e-9);
    DemodConfig bad = cfg(Mode::USB, AgcMode::Fast); bad.agc_target = 0; CHECK_THROWS(Demodulator(bad), std::invalid_argument);
    bad = cfg(Mode::USB, AgcMode::Fast); bad.agc_max_gain_db = -1; CHECK_THROWS(Demodulator(bad), std::invalid_argument);
    d.set_agc(AgcMode::Fast); d.process(x.data(), x.size(), a); CHECK(d.agc_gain_db() > 5.0);   // 0.1 -> 0.25 target: +8 dB; switching on at run time works
}
TEST(smeter_reads_in_band_power_before_the_agc) {
    ReceiveConfig rc;
    for (double amp : {1.0, 0.1, 0.01}) {
        for (auto agc : {AgcMode::Off, AgcMode::Fast}) {
            DemodConfig dc = cfg(Mode::USB, agc); ReceiveChain ch(rc, dc); std::vector<float> a;
            auto x = synth_tones(48000, 48000, {{9000 + 1000, amp}}); ch.process(x.data(), x.size(), a);
            CHECK_NEAR(ch.signal_dbfs(), 20.0 * std::log10(amp), 0.3);
        }
    }
    DemodConfig dc = cfg(Mode::USB); ReceiveChain empty(rc, dc); CHECK_EQ(empty.signal_dbfs(), -200.0);
    auto x = synth_tones(48000, 48000, {{9000 + 6000, 0.5}});                         // outside the USB passband: not counted
    ReceiveChain ch(rc, dc); std::vector<float> a; ch.process(x.data(), x.size(), a); CHECK(ch.signal_dbfs() < -60.0);
}
TEST(smeter_s_units_and_cat_scale) {
    auto r = s_reading(-73); CHECK_EQ(r.s_units, 9); CHECK_EQ(r.over_s9_db, 0);
    r = s_reading(-53); CHECK_EQ(r.s_units, 9); CHECK_EQ(r.over_s9_db, 20);
    CHECK_EQ(s_reading(-79).s_units, 8); CHECK_EQ(s_reading(-121).s_units, 1); CHECK_EQ(s_reading(-127).s_units, 0); CHECK_EQ(s_reading(-140).s_units, 0);
    CHECK_EQ(cat_smeter_value(-140), 0); CHECK_EQ(cat_smeter_value(-73), 15); CHECK_EQ(cat_smeter_value(-79), 13);
    CHECK_EQ(cat_smeter_value(-53), 20); CHECK_EQ(cat_smeter_value(-13), 30); CHECK_EQ(cat_smeter_value(10), 30);
    int prev = -1; for (double d = -140; d <= 0; d += 1) { int v = cat_smeter_value(d); CHECK(v >= prev); prev = v; }   // monotonic
}
TEST(smeter_feeds_the_cat_sm_command) {
    ReceiveConfig rc; DemodConfig dc = cfg(Mode::USB); ReceiveChain ch(rc, dc); std::vector<float> a;
    auto x = synth_tones(48000, 48000, {{9000 + 1000, 0.1}}); ch.process(x.data(), x.size(), a);   // -20 dBFS
    const double full_scale_dbm = -53.0;                                              // pretend calibration: 0 dBFS = -53 dBm  => -73 dBm = S9
    fireside::CatSession cat; cat.set_smeter(cat_smeter_value(ch.signal_dbfs() + full_scale_dbm));
    CHECK_EQ(cat.feed("SM0;"), std::string("SM00015;"));
}
TEST(fm_demodulator_recovers_deviation_independent_of_amplitude) {
    for (double amp : {0.5, 0.05}) {
        std::vector<cf> x(96000); const double dev = 3000, fm = 1000;
        for (size_t i = 0; i < x.size(); ++i) { const double t = double(i) / 48000, ph = 2 * kPi * 0.0 * t + (dev / fm) * std::sin(2 * kPi * fm * t); x[i] = float(amp) * cf(float(std::cos(ph)), float(std::sin(ph))); }
        auto a = run_cfg(x, kCentred, cfg(Mode::FM));   // dial-centred already: no IF offset
        CHECK_NEAR(tone_amp(a, 40000, 8192, 1000, 48000), dev / 5000.0, 0.03);
    }
}
TEST(noise_blanker_removes_impulses_but_keeps_the_signal) {
    auto make = [&] { auto x = stepped(48000, 0, 0.1, 0.1); for (size_t i = 5000; i < x.size(); i += 4999) x[i] += cf(20, 0); return x; };
    auto peak_after = [&](const std::vector<float>& a) { float p = 0; for (size_t i = 6000; i < a.size(); ++i) p = std::max(p, std::fabs(a[i])); return p; };
    DemodConfig off = cfg(Mode::USB), on = cfg(Mode::USB); on.noise_blanker = true;
    auto a0 = run_cfg(make(), kCentred, off), a1 = run_cfg(make(), kCentred, on);
    CHECK(peak_after(a0) > 0.5f); CHECK(peak_after(a1) < 0.13f);
    CHECK_NEAR(tone_amp(a1, 30000, 8192, 1000, 48000), 0.1, 0.01);
    Demodulator d(on); std::vector<float> a; auto x = make(); d.process(x.data(), x.size(), a); CHECK(d.blanked() >= 8 * 3 && d.blanked() < 60);   // 9 impulses x 3 samples
}
TEST(noise_blanker_leaves_a_clean_signal_alone) {
    DemodConfig on = cfg(Mode::USB); on.noise_blanker = true; Demodulator d(on); std::vector<float> a;
    auto x = synth_tones(48000, 48000, {{1000, 0.1}, {1500, 0.1}}, 0.01, 2); d.process(x.data(), x.size(), a);
    CHECK_EQ(d.blanked(), size_t(0));
}
TEST(agc_nb_fm_chunked_equals_one_shot) {
    for (double fs : {48000.0, 192000.0}) for (Mode m : {Mode::USB, Mode::FM}) {
        ReceiveConfig rc; rc.sample_rate = fs; DemodConfig dc = cfg(m, AgcMode::Med); dc.noise_blanker = true;
        auto x = synth_tones(fs, size_t(fs / 2), {{9000 + 1200, 0.2}}, 0.02, 6); x[3000] += cf(5, 0);
        auto whole = run_cfg(x, rc, dc), parts = run_cfg(x, rc, dc, 1001);
        CHECK_EQ(whole.size(), parts.size()); for (size_t i = 0; i < whole.size(); ++i) CHECK(std::fabs(whole[i] - parts[i]) < 1e-3f);
    }
}

// ------------------------------------------------------------------ adaptive NR / auto-notch / manual notch / squelch
namespace {
double snr_db(const std::vector<float>& a, size_t from, size_t n, double hz, double fs) {      // tone-to-everything-else power ratio of a real signal
    const double amp = tone_amp(a, from, n, hz, fs); double tot = 0; for (size_t i = 0; i < n; ++i) tot += double(a[from + i]) * double(a[from + i]);
    const double sig = amp * amp / 2, noise = std::max(tot / double(n) - sig, 1e-12); return 10.0 * std::log10(sig / noise);
}
double rms(const std::vector<float>& a, size_t from, size_t n) { double t = 0; for (size_t i = 0; i < n; ++i) t += double(a[from + i]) * double(a[from + i]); return std::sqrt(t / double(n)); }
}  // namespace

TEST(nr_improves_the_snr_of_a_tone_in_noise) {
    auto x = synth_tones(48000, 48000 * 6, {{1000, 0.1}}, 0.1, 5);
    DemodConfig off = cfg(Mode::USB), on = cfg(Mode::USB); on.noise_reduction = true;
    auto a0 = run_cfg(x, kCentred, off), a1 = run_cfg(x, kCentred, on);
    const double s0 = snr_db(a0, 200000, 65536, 1000, 48000), s1 = snr_db(a1, 200000, 65536, 1000, 48000);
    CHECK(s1 > s0 + 5.0);                                                              // measured +6.5 dB; keep a margin
    CHECK(tone_amp(a1, 200000, 65536, 1000, 48000) > 0.095);                           // the signal itself is not eaten
}
TEST(auto_notch_removes_a_steady_tone_but_keeps_the_noise) {
    auto x = synth_tones(48000, 48000 * 4, {{1000, 0.3}}, 0.05, 6), noise_only = synth_tones(48000, 48000 * 4, {}, 0.05, 6);
    DemodConfig off = cfg(Mode::USB), on = cfg(Mode::USB); on.auto_notch = true;
    auto a0 = run_cfg(x, kCentred, off), a1 = run_cfg(x, kCentred, on), n0 = run_cfg(noise_only, kCentred, off);
    CHECK(tone_amp(a1, 150000, 32768, 1000, 48000) < 0.1 * tone_amp(a0, 150000, 32768, 1000, 48000));      // >= 20 dB down
    const double noise_ref = rms(n0, 150000, 32768), left = rms(a1, 150000, 32768);
    CHECK(left > 0.7 * noise_ref && left < 1.3 * noise_ref);                           // what remains is the noise floor, neither silence nor residue
}
TEST(manual_notch_kills_its_frequency_and_leaves_the_rest) {
    auto x = synth_tones(48000, 96000, {{1000, 0.3}, {1500, 0.3}});
    DemodConfig on = cfg(Mode::USB); on.notch_hz = 1000; on.notch_q = 10;
    auto a = run_cfg(x, kCentred, on), ref = run_cfg(x, kCentred, cfg(Mode::USB));
    CHECK(tone_amp(a, 60000, 8192, 1000, 48000) < 0.01 * 0.3 + 0.003);                // > 30 dB down
    CHECK(tone_amp(a, 60000, 8192, 1500, 48000) > 0.95 * tone_amp(ref, 60000, 8192, 1500, 48000));
    DemodConfig bad = cfg(Mode::USB); bad.notch_hz = 30000; CHECK_THROWS(Demodulator(bad), std::invalid_argument);   // above Nyquist
    bad.notch_hz = 1000; bad.notch_q = 0.0; CHECK_THROWS(Demodulator(bad), std::invalid_argument);
    CHECK_THROWS(AdaptiveLineFilter(AdaptiveLineFilter::Output::Residual, 0), std::invalid_argument);
    CHECK_THROWS(AdaptiveLineFilter(AdaptiveLineFilter::Output::Residual, 64, 16, 0.0f), std::invalid_argument);
}
TEST(squelch_gates_on_signal_level_quickly) {
    DemodConfig dc = cfg(Mode::USB); dc.squelch_dbfs = -40;
    auto loud = stepped(24000, 0, 0.1, 0.1), quiet = stepped(24000, 0, 0.001, 0.001);             // -20 dBFS and -60 dBFS
    std::vector<cf> x = loud; x.insert(x.end(), quiet.begin(), quiet.end()); x.insert(x.end(), loud.begin(), loud.end());
    auto a = run_cfg(x, kCentred, dc);
    CHECK(tone_amp(a, 10000, 8192, 1000, 48000) > 0.09);                                         // open on the loud signal
    CHECK(tone_amp(a, 24000 + 8000, 8192, 1000, 48000) < 0.0005);                                // closed within ~150 ms of the drop
    CHECK(tone_amp(a, 48000 + 8000, 8192, 1000, 48000) > 0.09);                                  // opens again
    DemodConfig open = cfg(Mode::USB); auto b = run_cfg(quiet, kCentred, open); CHECK(tone_amp(b, 12000, 8192, 1000, 48000) > 0.0009);   // without squelch the weak signal passes
}
TEST(squelch_hysteresis_prevents_chatter_inside_the_band) {
    // level wobbles between -41.9 and -40.1 dBFS: inside [threshold - 2 dB, threshold]. Once open it must stay open; once closed it must stay closed.
    auto wobble = [&](double start_db, size_t n) {
        std::vector<cf> x(n); for (size_t i = 0; i < n; ++i) { const double p = 2 * kPi * 1000.0 * double(i) / 48000, db = start_db + 0.9 * std::sin(2 * kPi * 5.0 * double(i) / 48000); x[i] = float(std::pow(10.0, db / 20.0)) * cf(float(std::cos(p)), float(std::sin(p))); } return x; };
    DemodConfig dc = cfg(Mode::USB); dc.squelch_dbfs = -40;
    auto open_first = stepped(24000, 0, 0.03, 0.03);                                              // -30 dBFS: opens
    auto x = open_first; auto w = wobble(-41.0, 72000); x.insert(x.end(), w.begin(), w.end());
    auto a = run_cfg(x, kCentred, dc); double lo = 1; for (size_t i = 30000; i + 480 < a.size(); i += 480) lo = std::min(lo, rms(a, i, 480));
    CHECK(lo > 0.004);                                                                             // never muted (signal is ~0.009 amplitude)
    auto closed = wobble(-41.0, 72000); auto b = run_cfg(closed, kCentred, dc); double hi = 0; for (size_t i = 24000; i + 480 < b.size(); i += 480) hi = std::max(hi, rms(b, i, 480));
    CHECK(hi < 0.001);                                                                             // started closed (initial state is open, closes within ms) and never reopens
}
TEST(nr_notch_squelch_chunked_equals_one_shot) {
    for (double fs : {48000.0, 192000.0}) {
        ReceiveConfig rc; rc.sample_rate = fs; DemodConfig dc = cfg(Mode::USB, AgcMode::Fast); dc.noise_reduction = true; dc.auto_notch = true; dc.notch_hz = 1800; dc.squelch_dbfs = -90;
        auto x = synth_tones(fs, size_t(fs / 2), {{9000 + 1200, 0.2}}, 0.02, 7);
        auto whole = run_cfg(x, rc, dc), parts = run_cfg(x, rc, dc, 1001);
        CHECK_EQ(whole.size(), parts.size()); for (size_t i = 0; i < whole.size(); ++i) CHECK(std::fabs(whole[i] - parts[i]) < 1e-3f);
    }
}

TEST(filter_presets_are_consistent_with_the_default_passbands) {
    for (Mode m : {Mode::USB, Mode::LSB, Mode::AM, Mode::CW, Mode::FM}) {
        auto w = filter_widths(m); CHECK(!w.empty()); CHECK(std::is_sorted(w.begin(), w.end()));
        CHECK(std::find(w.begin(), w.end(), default_filter_width(m)) != w.end());                      // the default is one of the presets
        for (double x : w) { auto pb = passband_for(m, x); CHECK(pb.second > pb.first); CHECK_NEAR(pb.second - pb.first, x, 1e-9); }
    }
    auto u = passband_for(Mode::USB, 2700), l = passband_for(Mode::LSB, 2700); CHECK_EQ(u.first, 150.0); CHECK_EQ(u.second, 2850.0); CHECK_EQ(l.first, -2850.0); CHECK_EQ(l.second, -150.0);
    auto cw = passband_for(Mode::CW, 500); CHECK_EQ(cw.first, -250.0); CHECK_EQ(cw.second, 250.0);
    CHECK_THROWS(passband_for(Mode::USB, 0), std::invalid_argument);
    // a demodulator built from a preset passes its band and rejects outside it
    DemodConfig dc = cfg(Mode::USB); auto pb = passband_for(Mode::USB, 1800); dc.lo_hz = pb.first; dc.hi_hz = pb.second;     // 150..1950
    auto in = run_cfg(stepped(48000, 0, 0.5, 0.5, 1000), kCentred, dc), out = run_cfg(stepped(48000, 0, 0.5, 0.5, 2600), kCentred, dc);
    CHECK(tone_amp(in, 20000, 8192, 1000, 48000) > 0.45); CHECK(tone_amp(out, 20000, 8192, 2600, 48000) < 0.01);
}

TEST(filter_cuts_are_independent_and_the_skirt_is_adjustable) {
    auto a = passband_for(Mode::USB, 2400, 300), b = passband_for(Mode::LSB, 2400, 300); CHECK_EQ(a.first, 300.0); CHECK_EQ(a.second, 2700.0); CHECK_EQ(b.first, -2700.0); CHECK_EQ(b.second, -300.0);
    auto c = passband_for(Mode::CW, 400, 999); CHECK_EQ(c.first, -200.0); CHECK_EQ(c.second, 200.0);                          // low cut is ignored for centred modes
    // a raised low cut removes what the default (150 Hz) low cut lets through
    DemodConfig dc = cfg(Mode::USB); auto pb = passband_for(Mode::USB, 2400, 600); dc.lo_hz = pb.first; dc.hi_hz = pb.second;
    auto at300 = run_cfg(stepped(48000, 0, 0.5, 0.5, 300), kCentred, dc), at1500 = run_cfg(stepped(48000, 0, 0.5, 0.5, 1500), kCentred, dc);
    CHECK(tone_amp(at300, 20000, 8192, 300, 48000) < 0.02); CHECK(tone_amp(at1500, 20000, 8192, 1500, 48000) > 0.45);
    // the sharp setting rejects a tone just outside the top edge better than the soft one, and still passes the inside
    auto rej = [&](double fraction, double floor_hz) {
        DemodConfig d = cfg(Mode::USB); d.lo_hz = 300; d.hi_hz = 2700; d.edge_fraction = fraction; d.edge_floor_hz = floor_hz;
        auto o = run_cfg(stepped(48000, 0, 0.5, 0.5, 2900), kCentred, d); auto in = run_cfg(stepped(48000, 0, 0.5, 0.5, 1500), kCentred, d);
        CHECK(tone_amp(in, 20000, 8192, 1500, 48000) > 0.45); return tone_amp(o, 20000, 8192, 2900, 48000);
    };
    const double soft = rej(0.2, 250), normal = rej(0.1, 150), sharp = rej(0.04, 80);
    CHECK(sharp < normal && normal < soft); CHECK(sharp < 0.001);
}

TEST(several_manual_notches_each_remove_their_tone) {
    auto x = synth_tones(48000, 96000, {{700, 0.3}, {1000, 0.3}, {1500, 0.3}, {2000, 0.3}});
    DemodConfig on = cfg(Mode::USB); on.notch_hz = 700; on.extra_notches_hz = {1500, 2000}; on.notch_q = 10;
    auto a = run_cfg(x, kCentred, on), ref = run_cfg(x, kCentred, cfg(Mode::USB));
    for (double f : {700.0, 1500.0, 2000.0}) CHECK(tone_amp(a, 60000, 8192, f, 48000) < 0.01 * 0.3 + 0.003);
    CHECK(tone_amp(a, 60000, 8192, 1000, 48000) > 0.95 * tone_amp(ref, 60000, 8192, 1000, 48000));
}

// ---------------------------------------------------------------- manual gain with the AGC off (the radio's I/Q is ~ -90 dBFS: unity gain is silent)
TEST(manual_gain_with_agc_off_scales_the_audio_and_is_soft_limited) {
    auto at = [&](double db, double amp) { DemodConfig d = cfg(Mode::USB); d.manual_gain_db = db; return tone_amp(run_cfg(stepped(48000, 24000, amp, amp), kCentred, d), 24000, 8192, 1000, 48000); };
    const double base = at(0, 3e-5), g40 = at(40, 3e-5), g60 = at(60, 3e-5);
    CHECK(base > 0 && base < 1e-4);                                                                          // unity gain: inaudible, as heard
    CHECK_NEAR(g40 / base, 100.0, 4.0); CHECK_NEAR(g60 / base, 1000.0, 60.0);                                // +40 dB = x100, +60 dB = x1000 (linear while small)
    { DemodConfig d = cfg(Mode::USB); d.manual_gain_db = 60; auto loud = run_cfg(stepped(48000, 24000, 0.5, 0.5), kCentred, d); float pk = 0; for (float v : loud) pk = std::max(pk, std::fabs(v)); CHECK(pk <= 1.0f); CHECK(pk > 0.9f); }   // a strong signal at +60 dB saturates softly: never beyond full scale
    auto agc_on = [&](double db) { DemodConfig d = cfg(Mode::USB, AgcMode::Med); d.manual_gain_db = db; return tone_amp(run_cfg(stepped(48000, 24000, 3e-5, 3e-5), kCentred, d), 24000, 8192, 1000, 48000); };
    CHECK_NEAR(agc_on(60), agc_on(0), 1e-6); CHECK(agc_on(0) > 0.1);                                         // with an AGC on, the manual gain changes nothing and the AGC does its job
}
