// Transmit round trips: digital-mode audio -> SSB modulator (I/Q) -> receive Demodulator -> our own decoders. Offline, no radio code.
#include <chrono>
#include <thread>
#include "fireside/decode.h"
#include "fireside/dsp.h"
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::decode;

namespace {
std::vector<float> through_ssb(const std::vector<float>& audio, tx::Sideband sb) {
    tx::SsbConfig c; c.sideband = sb; c.lo_hz = 100;                       // a little below 150 Hz so RTTY/PSK low tones sit well inside
    const auto iq = tx::ssb_modulate(audio, c);
    dsp::DemodConfig d; d.input_rate = 48000; d.audio_rate = 48000; d.agc = dsp::AgcMode::Off;
    d.mode = sb == tx::Sideband::USB ? dsp::Mode::USB : dsp::Mode::LSB; d.lo_hz = sb == tx::Sideband::USB ? 100 : -2900; d.hi_hz = sb == tx::Sideband::USB ? 2900 : -100;
    dsp::Demodulator dm(d); std::vector<float> out; dm.process(iq.data(), iq.size(), out); return out;
}
// LSB: the radio mirrors the audio, so the receiver in LSB returns the original audio frequencies too.
std::string run_ft(const std::vector<float>& x, bool ft4) {
    Ft8Options o; o.ft4 = ft4; Ft8Decoder d(o); std::string out;
    for (size_t i = 0; i < x.size(); i += 4800) d.process(x.data() + i, std::min<size_t>(4800, x.size() - i), out);
    std::vector<float> none(480, 0.0f); for (int k = 0; k < 600 && d.busy(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10)); d.process(none.data(), none.size(), out); return out;
}
std::string trim(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back(); size_t i = 0; while (i < s.size() && s[i] == ' ') ++i; return s.substr(i); }
}  // namespace

TEST(tx_roundtrip_ft8) {
    for (auto sb : {tx::Sideband::USB, tx::Sideband::LSB})
        for (double f : {500.0, 1500.0, 2500.0}) { auto a = synth_ft8("CQ M0OML IO91", f, 48000, 0.3, 0.5); a.resize(15 * 48000, 0.0f); CHECK(run_ft(through_ssb(a, sb), false).find("CQ M0OML IO91") != std::string::npos); }
}
TEST(tx_roundtrip_ft4) {
    auto a = synth_ft4("CQ M0OML IO91", 1200, 48000, 0.3, 0.3); a.resize(size_t(7.5 * 48000), 0.0f);
    CHECK(run_ft(through_ssb(a, tx::Sideband::USB), true).find("CQ M0OML IO91") != std::string::npos);
}
TEST(tx_roundtrip_psk31) {
    const std::string t = "CQ CQ DE M0OML K";
    for (auto sb : {tx::Sideband::USB, tx::Sideband::LSB}) {
        auto y = through_ssb(synth_psk31(t + " ", 1000, 48000, 0.5, 2.0), sb); Psk31Decoder d(PskOptions{48000, 1000}); std::string out;
        for (size_t i = 0; i < y.size(); i += 4096) d.process(y.data() + i, std::min<size_t>(4096, y.size() - i), out);
        CHECK_EQ(trim(out), t);
    }
}
TEST(tx_roundtrip_rtty) {
    const std::string t = "CQ TEST DE M0OML";
    auto y = through_ssb(synth_rtty(t + "\r\n", 2125, 170, 45.45, 48000, 0.5, 0.5), tx::Sideband::USB); RttyDecoder d; std::string out;
    for (size_t i = 0; i < y.size(); i += 4096) d.process(y.data() + i, std::min<size_t>(4096, y.size() - i), out);
    CHECK(out.find(t) != std::string::npos);
}
TEST(tx_roundtrip_cw) {
    const std::string t = "CQ DE M0OML";
    auto y = through_ssb(synth_morse("VVV " + t + " ", 20, 700, 48000, 0.5, 5, 2.0), tx::Sideband::USB); CwDecoder d;   // VVV and a 2 s lead let the tracker lock, as in the CW decoder tests
    std::string out;
    for (size_t i = 0; i < y.size(); i += 4096) d.process(y.data() + i, std::min<size_t>(4096, y.size() - i), out);
    CHECK(out.find(t) != std::string::npos);
}
