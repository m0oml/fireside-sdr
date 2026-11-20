#include "fireside/tx_render.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "fireside/decode.h"

namespace fireside::tx {
namespace {
// Final I/Q limiter: aligned with the input (the limiter's delay is flushed out), same length.
std::vector<cf> finish(const std::vector<cf>& in) {
    LimiterConfig lc; lc.ceiling = kRenderCeiling; PeakLimiter lim(kRenderRate, lc);
    std::vector<cf> padded = in; padded.resize(in.size() + lim.latency_samples(), cf(0, 0));
    std::vector<cf> out; out.reserve(padded.size()); lim.process(padded.data(), padded.size(), out);
    out.erase(out.begin(), out.begin() + long(lim.latency_samples()));
    return out;
}
double level_to_amp(double db) { if (!(db <= -1.0 && db >= -80.0)) throw std::invalid_argument("level must be between -80 and -1 dBFS"); return std::pow(10.0, db / 20.0); }
}  // namespace

std::vector<cf> render_ssb(const std::vector<float>& audio, Sideband sb, bool mic_chain, double gain_db) {
    if (!(gain_db >= -40 && gain_db <= 40)) throw std::invalid_argument("gain must be between -40 and 40 dB");
    if (!mic_chain) { SsbConfig c; c.sideband = sb; return finish(ssb_modulate(audio, c)); }
    TxChainConfig t; t.ssb.sideband = sb; t.mic.mic_gain_db = gain_db; TxChain ch(t);
    std::vector<float> in = audio; in.resize(audio.size() + ch.latency_samples(), 0.0f);
    std::vector<cf> out; ch.process(in.data(), in.size(), out);
    out.erase(out.begin(), out.begin() + long(ch.latency_samples()));
    return finish(out);
}
std::vector<cf> render_ft(const std::string& text, double freq_hz, bool ft4, Sideband sb, double level_db) {
    if (!(freq_hz >= 200 && freq_hz <= 3000)) throw std::invalid_argument("frequency must be 200..3000 Hz");
    const double a = level_to_amp(level_db);
    const double dt = default_start_offset_s(ft4 ? DigiMode::FT4 : DigiMode::FT8);
    auto audio = ft4 ? decode::synth_ft4(text, freq_hz, kRenderRate, a, dt) : decode::synth_ft8(text, freq_hz, kRenderRate, a, dt);
    if (audio.empty()) throw std::runtime_error("cannot encode this message: \"" + text + "\"");
    return render_ssb(audio, sb, /*mic_chain=*/false);                     // no mic chain: it would filter and compress a digital signal
}
bool ft_message_ok(const std::string& text) { return !text.empty() && !decode::synth_ft8(text, 1000, kRenderRate, 0.1, default_start_offset_s(DigiMode::FT8)).empty(); }
std::vector<cf> render_cw(const std::string& text, double wpm, double edge_ms, double level_db, double offset_hz) {
    CwShapeConfig c; c.edge_ms = edge_ms; c.amplitude = level_to_amp(level_db); c.offset_hz = offset_hz;
    CwShaper sh(c); const auto key = text_keying(text, wpm, kRenderRate, 0.5);
    std::vector<cf> iq; sh.process(key.data(), key.size(), iq); return finish(iq);
}
std::vector<cf> render_psk31(const std::string& text, double freq_hz, Sideband sb, double level_db) {
    if (!(freq_hz >= 200 && freq_hz <= 3000)) throw std::invalid_argument("frequency must be 200..3000 Hz");
    if (text.empty() || text.size() > 500) throw std::invalid_argument("text must be 1..500 characters");
    return render_ssb(decode::synth_psk31(text + " ", freq_hz, kRenderRate, level_to_amp(level_db), 2.0), sb, /*mic_chain=*/false);
}
std::vector<cf> render_rtty(const std::string& text, double mark_hz, Sideband sb, double level_db) {
    if (!(mark_hz >= 200 && mark_hz <= 2800)) throw std::invalid_argument("mark frequency must be 200..2800 Hz");
    if (text.empty() || text.size() > 500) throw std::invalid_argument("text must be 1..500 characters");
    return render_ssb(decode::synth_rtty(text + "\r\n", mark_hz, 170, 45.45, kRenderRate, level_to_amp(level_db), 0.5), sb, /*mic_chain=*/false);
}
std::vector<cf> render_paddles(const std::vector<PaddleEvent>& ev, double wpm, IambicMode mode, bool reverse, double weight, double edge_ms, double level_db, double offset_hz) {
    if (ev.empty() || ev.size() > 100000) throw std::invalid_argument("paddle recording must have 1..100000 events");
    double prev = 0; for (const auto& e : ev) { if (!(e.t_s >= prev && e.t_s <= 600.0)) throw std::invalid_argument("paddle times must be non-decreasing and within 0..600 s"); prev = e.t_s; }
    KeyerConfig kc; kc.sample_rate = kRenderRate; kc.wpm = wpm; kc.weight = weight; kc.mode = mode; kc.reverse = reverse; IambicKeyer keyer(kc);
    CwShapeConfig c; c.edge_ms = edge_ms; c.amplitude = level_to_amp(level_db); c.offset_hz = offset_hz; CwShaper sh(c);
    const size_t lead = size_t(0.5 * kRenderRate), n = size_t(ev.back().t_s * kRenderRate) + size_t(1.5 * kRenderRate);
    std::vector<uint8_t> paddles(n, 0); size_t at = 0;
    for (size_t i = 0; i < ev.size(); ++i) { const size_t end = i + 1 < ev.size() ? std::min(n, size_t(ev[i + 1].t_s * kRenderRate)) : n; const uint8_t v = uint8_t((ev[i].dit ? 1 : 0) | (ev[i].dah ? 2 : 0)); for (size_t k = std::max(at, size_t(ev[i].t_s * kRenderRate)); k < end; ++k) paddles[k] = v; at = std::max(at, end); }
    std::vector<uint8_t> key(lead, 0); keyer.process(paddles.data(), n, key);
    std::vector<cf> iq; sh.process(key.data(), key.size(), iq); return finish(iq);
}
std::vector<cf> render_two_tone(double seconds, double peak_db, Sideband sb) {
    if (!(seconds > 0 && seconds <= 600)) throw std::invalid_argument("duration must be 0..600 s");
    return render_ssb(two_tone_audio(size_t(seconds * kRenderRate), level_to_amp(peak_db)), sb, /*mic_chain=*/false);
}
std::vector<float> read_audio_wav(const std::string& path) {
    const auto rec = dsp::read_iq_wav(path, 0, 0);
    std::vector<cf> x = rec.samples;
    if (rec.sample_rate != kRenderRate) { dsp::RationalResampler rs(rec.sample_rate, kRenderRate); std::vector<cf> y; rs.process(x.data(), x.size(), y); x.swap(y); }
    std::vector<float> a(x.size()); for (size_t i = 0; i < x.size(); ++i) a[i] = x[i].real(); return a;
}
void write_iq_wav(const std::string& path, const std::vector<cf>& iq, double fs) {
    std::vector<float> v; v.reserve(iq.size() * 2); for (const cf& s : iq) { v.push_back(s.real()); v.push_back(s.imag()); }
    dsp::write_wav_f32(path, fs, 2, v);
}
double peak_magnitude(const std::vector<cf>& iq) { double p = 0; for (const cf& s : iq) p = std::max(p, double(std::abs(s))); return p; }
}  // namespace fireside::tx
