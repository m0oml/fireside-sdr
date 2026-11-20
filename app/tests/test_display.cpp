// Display maths and I/Q sources (Qt-free parts of the receiver GUI).
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include "fireside/display.h"
#include "fireside/iqsource.h"
#include "fireside/tx_render.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::display;

TEST(display_colormap_endpoints_clamp_and_continuity) {
    CHECK_EQ(colormap(0.f), 0x000000u); CHECK_EQ(colormap(1.f), 0xFFFFFFu); CHECK_EQ(colormap(-5.f), 0x000000u); CHECK_EQ(colormap(7.f), 0xFFFFFFu);
    CHECK_EQ(colormap(0.2f), 0x00008Cu);                                         // dark blue stop
    uint32_t prev = colormap(0.f); int worst = 0;
    for (int i = 1; i <= 1000; ++i) {
        uint32_t c = colormap(float(i) / 1000.f);
        for (int sh : {16, 8, 0}) worst = std::max(worst, std::abs(int((c >> sh) & 255) - int((prev >> sh) & 255)));
        prev = c;
    }
    CHECK(worst <= 12);                                                          // no jumps: smooth ramp
}
TEST(display_auto_scale_follows_noise_floor_and_peak) {
    std::vector<float> db(1000, -100.f); db[500] = -30.f;
    auto s = auto_scale(db); CHECK_NEAR(s.lo_db, -106.0, 1e-4); CHECK_NEAR(s.hi_db, -24.0, 1e-4);   // peak + 6
    std::vector<float> quiet(1000, -100.f); auto q = auto_scale(quiet); CHECK_NEAR(q.hi_db - q.lo_db, 60.0, 1e-4);   // minimum span
    CHECK_NEAR(to_unit(-106.f, s), 0.0, 1e-6); CHECK_NEAR(to_unit(-24.f, s), 1.0, 1e-6); CHECK_NEAR(to_unit(-200.f, s), 0.0, 1e-6); CHECK_NEAR(to_unit(0.f, s), 1.0, 1e-6);
    CHECK_EQ(auto_scale({}).lo_db, -120.f);
    auto m = smooth({-100, -20}, {-60, -20}, 0.25f); CHECK_NEAR(m.lo_db, -90.0, 1e-4); CHECK_NEAR(m.hi_db, -20.0, 1e-6);
}
TEST(display_axis_ticks_and_pixel_mapping) {
    CHECK_NEAR(nice_step(48000, 8), 5000.0, 1e-9); CHECK_NEAR(nice_step(192000, 8), 20000.0, 1e-9); CHECK_NEAR(nice_step(9600, 8), 1000.0, 1e-9);
    CHECK_NEAR(nice_step(100, 10), 10.0, 1e-9); CHECK_NEAR(nice_step(0.7, 7), 0.1, 1e-12);
    CHECK_EQ(hz_to_x(0, 48000, 1001), 500); CHECK_EQ(hz_to_x(-24000, 48000, 1001), 0); CHECK_EQ(hz_to_x(24000, 48000, 1001), 1000);
    for (int x : {0, 1, 250, 777, 1000}) CHECK_EQ(hz_to_x(x_to_hz(x, 96000, 1001), 96000, 1001), x);
}
TEST(display_decimate_max_keeps_narrow_peaks) {
    std::vector<float> db(4096, -100.f); db[1234] = -20.f;
    auto d = decimate_max(db, 512); CHECK_EQ(d.size(), size_t(512));
    CHECK_EQ(*std::max_element(d.begin(), d.end()), -20.f); CHECK_EQ(size_t(std::max_element(d.begin(), d.end()) - d.begin()), size_t(1234 * 512 / 4096));
    CHECK(decimate_max(db, 8192) == db); CHECK(decimate_max(db, 0).empty());
    CHECK_EQ(format_mhz(7101000), std::string("7.101000 MHz"));
}

TEST(iqsource_file_reads_loops_and_describes) {
    const std::string p = (std::filesystem::temp_directory_path() / ("firesideiqs_" + std::to_string(::getpid()) + ".wav")).string();
    std::vector<float> il; for (int i = 0; i < 10; ++i) { il.push_back(float(i)); il.push_back(-float(i)); }
    dsp::write_wav_f32(p, 96000, 2, il);
    { FileIqSource s(p); std::vector<dsp::cf> out; CHECK_EQ(s.read(out, 4), size_t(4)); CHECK_EQ(s.read(out, 100), size_t(6)); CHECK_EQ(s.read(out, 5), size_t(0));
      CHECK_EQ(out[9].real(), 9.0f); CHECK_EQ(out[9].imag(), -9.0f); CHECK_EQ(s.sample_rate(), 96000.0); s.rewind(); CHECK_EQ(s.read(out, 1), size_t(1));
      CHECK(s.describe().find("96000 Hz") != std::string::npos); }
    { FileIqSource s(p, 0, 1, true); std::vector<dsp::cf> out; CHECK_EQ(s.read(out, 25), size_t(25)); CHECK_EQ(out[10].real(), 0.0f); CHECK_EQ(out[24].real(), 4.0f); }   // loops
    std::filesystem::remove(p);
    CHECK_THROWS(FileIqSource("/nonexistent/x.wav"), std::runtime_error);
}
TEST(iqsource_demo_is_phase_continuous_across_reads_and_has_its_peaks_where_documented) {
    DemoIqSource a, b; std::vector<dsp::cf> whole, parts;
    a.read(whole, 9000); for (size_t n : {1u, 4000u, 4999u}) b.read(parts, n);
    CHECK_EQ(parts.size(), size_t(9000));
    float worst = 0; for (size_t i = 0; i < whole.size(); ++i) worst = std::max(worst, std::abs(whole[i] - parts[i]));
    CHECK(worst < 0.02f);                                                              // tones and sweep are phase-continuous; only the -60 dBFS noise differs (reseeded per call)
    dsp::ReceiveConfig rc; dsp::SpectrumAnalyzer sp(48000, 4096); std::vector<dsp::cf> big; DemoIqSource d; d.read(big, 4096);
    dsp::IfShifter sh(rc); sh.process(big.data(), big.size()); sp.push(big.data(), big.size());
    size_t pk = size_t(std::max_element(sp.db().begin(), sp.db().end()) - sp.db().begin());
    CHECK(std::fabs(sp.bin_hz(pk) - 1000.0) < 25.0);                                  // the 0.3 carrier, 1 kHz above the dial after the IF shift
}

TEST(display_palettes_are_smooth_ordered_and_named) {
    CHECK_EQ(std::string(palette_name(Palette::Classic)), std::string("Classic")); CHECK_EQ(std::string(palette_name(Palette::Gray)), std::string("Gray")); CHECK_EQ(kPaletteCount, 4);
    auto lum = [](uint32_t c) { return 0.299 * double((c >> 16) & 255) + 0.587 * double((c >> 8) & 255) + 0.114 * double(c & 255); };
    for (Palette p : {Palette::Classic, Palette::Inferno, Palette::Viridis, Palette::Gray}) {
        CHECK_EQ(colormap(-1.f, p), colormap(0.f, p)); CHECK_EQ(colormap(9.f, p), colormap(1.f, p));
        uint32_t prev = colormap(0.f, p); int worst = 0;
        for (int i = 1; i <= 1000; ++i) { uint32_t c = colormap(float(i) / 1000.f, p); for (int sh : {16, 8, 0}) worst = std::max(worst, std::abs(int((c >> sh) & 255) - int((prev >> sh) & 255))); prev = c; }
        CHECK(worst <= 14);                                                                                  // no jumps
    }
    for (Palette p : {Palette::Inferno, Palette::Viridis, Palette::Gray}) { double prev = -1; for (int i = 0; i <= 100; ++i) { const double l = lum(colormap(float(i) / 100.f, p)); CHECK(l >= prev - 1.0); prev = l; } }   // brightness only goes up: strong signals look brighter
    CHECK_EQ(colormap(0.f, Palette::Gray), 0x000000u); CHECK_EQ(colormap(1.f, Palette::Gray), 0xFFFFFFu); CHECK_EQ(colormap(0.5f, Palette::Classic), colormap(0.5f));
}

TEST(display_scale_grows_fast_and_shrinks_slowly) {
    auto run = [](Scale c, Scale t, double seconds, float dt) { for (double x = 0; x < seconds; x += dt) c = smooth_asymmetric(c, t, dt); return c; };
    const Scale base{-120, -20};
    auto grown = run({-100, -60}, {-120, -20}, 2.0, 0.02f);                              // a strong signal appears: more range within 2 s (95 % of the way)
    CHECK(grown.hi_db > -60 + 0.9 * 40 && grown.lo_db < -100 - 0.9 * 20);
    auto shrunk = run(base, {-100, -60}, 2.0, 0.02f);                                    // it disappears: after the same 2 s the axis has barely moved
    CHECK(shrunk.hi_db > -20 - 0.1 * 40 && shrunk.lo_db < -120 + 0.1 * 20);
    auto later = run(base, {-100, -60}, 60.0, 0.02f);                                    // and gets there in the end (about 3 time constants)
    CHECK(later.hi_db < -20 - 0.9 * 40 && later.lo_db > -120 + 0.9 * 20);
}
TEST(display_scale_smoothing_ignores_the_frame_rate_and_never_overshoots) {
    const Scale c{-110, -30}, t{-90, -50};
    Scale one = smooth_asymmetric(c, t, 1.0f), many = c; for (int i = 0; i < 100; ++i) many = smooth_asymmetric(many, t, 0.01f);
    CHECK_NEAR(one.lo_db, many.lo_db, 1e-3); CHECK_NEAR(one.hi_db, many.hi_db, 1e-3);   // one 1 s step equals a hundred 10 ms steps (192 kHz vs 48 kHz sources look the same)
    Scale x = c; for (int i = 0; i < 1000; ++i) { x = smooth_asymmetric(x, t, 0.5f); CHECK(x.lo_db >= c.lo_db && x.lo_db <= t.lo_db && x.hi_db <= c.hi_db && x.hi_db >= t.hi_db); }
    auto same = smooth_asymmetric(c, t, 0.0f); CHECK_EQ(same.lo_db, c.lo_db); CHECK_EQ(same.hi_db, c.hi_db);   // no time passed: no movement
    auto jump = smooth_asymmetric(c, t, 0.1f, 0.0f, 0.0f); CHECK_EQ(jump.lo_db, t.lo_db); CHECK_EQ(jump.hi_db, t.hi_db);   // zero time constants: straight to the target
    auto nan = smooth_asymmetric(c, t, std::nanf("")); CHECK_EQ(nan.lo_db, c.lo_db);                  // a bad time step changes nothing
}

TEST(display_auto_scale_ignores_silent_bins_and_caps_the_span) {
    std::vector<float> half(1000, -200.f); for (size_t i = 0; i < 500; ++i) half[i] = -100.f; half[7] = -40.f;                  // half the bins are digital silence
    auto s = auto_scale(half); CHECK_NEAR(s.lo_db, -106.0, 1e-4); CHECK_NEAR(s.hi_db, -34.0, 1e-4);                         // median of the live bins only
    std::vector<float> clean(1000, -185.f); clean[500] = -6.f;                                                               // a very clean signal: 180 dB between floor and peak
    auto c = auto_scale(clean); CHECK_NEAR(c.hi_db, 0.0, 1e-4); CHECK_NEAR(c.hi_db - c.lo_db, 140.0, 1e-4);                 // the bottom is raised, the top follows the peak
    CHECK(is_silent(std::vector<float>(100, -200.f))); CHECK(is_silent({})); CHECK(!is_silent(std::vector<float>(100, -120.f)));
    std::vector<float> mostly(1000, -200.f); for (size_t i = 0; i < 80; ++i) mostly[i] = -90.f; CHECK(is_silent(mostly));      // under 10 % live
}
TEST(display_scale_tracker_holds_through_silence) {
    ScaleTracker t; const std::vector<float> silent(2048, -200.f), live = [] { std::vector<float> v(2048, -110.f); v[100] = -30.f; return v; }();
    auto a = t.update(silent, 0.04f); CHECK_NEAR(a.lo_db, -120.0, 1e-4); CHECK_NEAR(a.hi_db, -20.0, 1e-4);                   // nothing seen yet: the default scale
    auto b = t.update(live, 0.04f); auto c = t.update(silent, 0.04f); CHECK_EQ(b.lo_db, c.lo_db); CHECK_EQ(b.hi_db, c.hi_db);   // a gap changes nothing
    for (int i = 0; i < 1000; ++i) c = t.update(silent, 0.04f); CHECK_EQ(b.hi_db, c.hi_db);                                   // however long it lasts
    t.reset(); auto d = t.update(live, 0.04f); CHECK_NEAR(d.hi_db, -24.0, 1e-4);                                              // reset starts afresh from the next live frame
}
TEST(display_scale_tracker_shrink_time_is_adjustable) {
    const std::vector<float> loud = [] { std::vector<float> v(2048, -110.f); v[100] = -10.f; return v; }(), quiet(2048, -110.f);
    float span[2]; const float taus[2] = {5.f, 60.f};
    for (int k = 0; k < 2; ++k) { ScaleTracker t; t.set_shrink_tau(taus[k]); t.update(loud, 0.04f); Scale s{}; for (int i = 0; i < 250; ++i) s = t.update(quiet, 0.04f); span[k] = s.hi_db - s.lo_db; }   // 10 s after the loud signal went
    CHECK(span[0] < span[1] - 10.f);                                                   // quick settling has narrowed much further than slow
    ScaleTracker t; CHECK_NEAR(t.shrink_tau(), 20.0, 1e-6); t.set_shrink_tau(0.f); t.set_shrink_tau(-3.f); t.set_shrink_tau(1e9f); t.set_shrink_tau(std::nanf("")); CHECK_NEAR(t.shrink_tau(), 20.0, 1e-6);   // bad values are ignored
    t.set_shrink_tau(60.f); CHECK_NEAR(t.shrink_tau(), 60.0, 1e-6);
}
TEST(display_scale_tracker_is_steady_on_a_keyed_noiseless_signal) {
    // What the transmit tab's CW preview looks like to the receiver: key-down at -6 dBFS, key-up gaps of exact digital silence, and fading tails.
    // Before: the span swung between 85 and 160 dB and the edges moved about 10 dB; the tracker must hold both edges within 3 dB and the span within 140 dB.
    const auto one = fireside::tx::render_cw("CQ DE M0OML", 20, 5, -6, 0); std::vector<fireside::dsp::cf> iq; for (int k = 0; k < 4; ++k) iq.insert(iq.end(), one.begin(), one.end());
    fireside::dsp::ReceiveConfig rc; fireside::dsp::DemodConfig dc; fireside::dsp::ReceiveChain ch(rc, dc, 4096); std::vector<std::vector<float>> frames;
    ch.spectrum().on_frame = [&](const std::vector<float>& d) { frames.push_back(d); }; std::vector<float> audio;
    for (size_t o = 0; o < iq.size(); o += 4800) ch.process(iq.data() + o, std::min<size_t>(4800, iq.size() - o), audio);
    CHECK(frames.size() > 600);
    ScaleTracker t; const float dt = 2048.f / 48000.f; float lo_min = 1e9f, lo_max = -1e9f, hi_min = 1e9f, hi_max = -1e9f; double time = 0;
    for (const auto& f : frames) { const Scale s = t.update(f, dt); time += dt; CHECK(s.hi_db - s.lo_db <= 140.001f);
        if (time > 8.0) { lo_min = std::min(lo_min, s.lo_db); lo_max = std::max(lo_max, s.lo_db); hi_min = std::min(hi_min, s.hi_db); hi_max = std::max(hi_max, s.hi_db); } }
    CHECK(lo_max - lo_min < 3.0f); CHECK(hi_max - hi_min < 3.0f); CHECK(hi_max < 3.0f && hi_min > -6.0f);                         // steady, and the top sits just above the -6 dBFS peak
}
