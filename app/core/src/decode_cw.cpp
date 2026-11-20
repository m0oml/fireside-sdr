#include "fireside/decode.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>

namespace fireside::decode {
namespace {
constexpr double kPi = 3.14159265358979323846;
const std::map<std::string, char>& morse_map() {
    static const std::map<std::string, char> m = {
        {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'}, {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'}, {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'},
        {"-.", 'N'}, {"---", 'O'}, {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'}, {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'}, {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
        {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'}, {"-...-", '='}, {".-.-.", '+'}, {"-....-", '-'}, {"-.--.", '('}, {"-.--.-", ')'}, {"---...", ':'},
        {".----.", '\''}, {".-..-.", '"'}, {".--.-.", '@'}, {"-.-.-.", ';'}, {"-.-.--", '!'}};
    return m;
}
}  // namespace
const char* morse_for(char c) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& kv : morse_map()) if (kv.second == c) return kv.first.c_str();
    return nullptr;
}
char char_for_morse(const std::string& s) { auto it = morse_map().find(s); return it == morse_map().end() ? 0 : it->second; }

struct CwDecoder::Impl {
    CwOptions o; double cur_bw; int D;                        // cur_bw: the detection bandwidth now in use; D: samples per envelope step (500 Hz envelope)
    double env_rate;
    // mixer + four cascaded one-pole low-pass filters: -3 dB at +-bandwidth/2, about -17 dB at 1.5x the bandwidth, -27 dB at 2.5x; envelope sampled every D samples
    double phase = 0, step; std::complex<double> lp[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}; double alpha; int count = 0;
    // envelope history for thresholds
    std::deque<float> hist; size_t hist_max; int since_thr = 0; double floor = 0, peak = 0, peak_local = 0, sep = 0; bool present = false; int miss = 0, since_on = 0;
    // keyer state
    bool key = false; int run = 0; bool pending = false; bool pending_state = false; int pending_run = 0;
    int on_len = 0, off_len = 0; std::deque<double> on_ms; double dit_ms = 60; bool dit_known = false; int pulses = 0;
    std::deque<double> intra, gaps; double letter_gap = 0;      // intra-character gaps (for the speed readout) and the longer gaps (for letter/word spacing, incl. Farnsworth)
    std::string chr; bool word_pending = false, emitted_since_space = false, space_done = true, synced = false;   // synced: a character boundary has been seen since the signal appeared
    // pitch tracking
    std::deque<float> raw; size_t raw_max; int track_counter = 0; double pitch;
    explicit Impl(CwOptions op) : o(op), cur_bw(op.bandwidth_hz), D(std::max(8, int(op.sample_rate / 500.0))), env_rate(op.sample_rate / D), step(2 * kPi * op.pitch_hz / op.sample_rate),
        alpha(1.0 - std::exp(-2 * kPi * (op.bandwidth_hz / 2 / 0.435) / op.sample_rate)), hist_max(size_t(env_rate * 3)), raw_max(size_t(op.sample_rate * 0.3)), pitch(op.pitch_hz) {}
    void reset() { *this = Impl(o); }
    double ms(int steps) const { return 1000.0 * steps / env_rate; }
    static double percentile(std::vector<float>& v, double q) { size_t k = size_t(q * double(v.size() - 1)); std::nth_element(v.begin(), v.begin() + long(k), v.end()); return v[k]; }

    void update_thresholds() {
        if (hist.size() < size_t(env_rate * 0.8)) { present = false; return; }
        std::vector<float> v(hist.begin(), hist.end());
        floor = percentile(v, 0.20); peak = percentile(v, 0.97);
        const size_t tail = std::min(v.size(), size_t(env_rate)); std::vector<float> t(hist.end() - long(tail), hist.end()); peak_local = std::max(percentile(t, 0.97), 0.5 * peak);   // follows fades, never collapses into the noise
        const bool was = present;
        present = peak > 1e-6 && peak / std::max(floor, 1e-3 * peak) > o.min_snr_ratio;       // (a noise-free test signal has an exactly zero floor)
        if (present) {                                                                         // keyed: the envelope sits at two separate levels; noise (and QRM) spreads between them
            // Split the last 3 s at the midpoint and compare the two groups: (mean_on - mean_off) / (sd_on + sd_off). Measured on a real band (CWT, 2026-11-18): band noise
            // and switch-mode QRM 1.4-1.75, keyed stations 2.3-4.4 (median). Entering needs min_separation, staying needs hold_separation (keeps copy through a fade).
            // A station that has just started fills only part of the 3 s, so entry also looks at the last 1.2 s alone (its own midpoint; a stricter limit, as a short window is noisier).
            auto separation = [&](size_t from, double mid, double& fon) {
                double s[2] = {0, 0}, q[2] = {0, 0}; size_t n[2] = {0, 0};
                for (size_t i = from; i < hist.size(); ++i) { const double x = hist[i]; const int k = x > mid; s[k] += x; q[k] += x * x; ++n[k]; }
                fon = double(n[1]) / double(std::max<size_t>(1, n[0] + n[1]));
                if (n[0] < 5 || n[1] < 5) return 0.0;
                const double m0 = s[0] / n[0], m1 = s[1] / n[1], v0 = std::max(0.0, q[0] / n[0] - m0 * m0), v1 = std::max(0.0, q[1] / n[1] - m1 * m1);
                return (m1 - m0) / std::max(1e-12 * peak, std::sqrt(v0) + std::sqrt(v1));
            };
            double fon = 0; sep = separation(0, floor + 0.5 * (peak - floor), fon);
            present = sep >= (was ? o.hold_separation : o.min_separation) && fon > (was ? 0.04 : 0.12) && fon < 0.85;   // (key down for > 85 % of 3 s: a carrier, not Morse)
            if (!present && hist.size() > size_t(env_rate * 1.2)) {
                const size_t from = hist.size() - size_t(env_rate * 1.2); std::vector<float> r(hist.begin() + long(from), hist.end());
                const double rf = percentile(r, 0.20), rp = percentile(r, 0.97); double rfon = 0;
                const double rs = rp / std::max(rf, 1e-3 * rp) > o.min_snr_ratio ? separation(from, rf + 0.5 * (rp - rf), rfon) : 0.0;
                if (rs >= (was ? o.hold_separation : o.min_separation_short) && rfon > 0.15 && rfon < 0.85 && (!was || since_on < 30)) { present = true; sep = rs; }   // (and keeps it until the 3 s window has filled)
            }
        }
        if (present) miss = 0; else if (was && ++miss < 4) present = true;                     // a signal is dropped only after 0.4 s of failing (one marginal look must not restart the character timing)
        if (present) since_on = was ? since_on + 1 : 0;
        if (present && !was) { synced = false; chr.clear(); }                                 // a signal just appeared
    }
    void track() {                                            // Goertzel scan around the pitch every 0.5 s; move only if one tone clearly dominates
        if (raw.size() < raw_max) return;
        double best = 0, sum = 0, bf = pitch; int n = 0;
        for (double f = pitch - 150; f <= pitch + 150; f += 10) {
            if (f < 200 || f > 1800) continue;
            const double w = 2 * kPi * f / o.sample_rate, c = 2 * std::cos(w); double s1 = 0, s2 = 0;
            for (float x : raw) { const double s0 = double(x) + c * s1 - s2; s2 = s1; s1 = s0; }
            const double e = s1 * s1 + s2 * s2 - c * s1 * s2; sum += e; ++n; if (e > best) { best = e; bf = f; }
        }
        if (n && best > 8.0 * (sum - best) / std::max(1, n - 1) && std::fabs(bf - pitch) >= 10) { pitch = bf; step = 2 * kPi * pitch / o.sample_rate; }
    }
    void estimate_dit() {                                     // 2-means on the recent pulse lengths: dits vs dahs
        if (on_ms.size() < 3) return;                                                               // (first estimate after 3 pulses)
        std::vector<double> d(on_ms.begin(), on_ms.end()); std::sort(d.begin(), d.end());
        double lo = d.front(), hi = d.back();
        for (int it = 0; it < 8; ++it) { double sl = 0, sh = 0; int nl = 0, nh = 0; for (double x : d) { if (std::fabs(x - lo) <= std::fabs(x - hi)) { sl += x; ++nl; } else { sh += x; ++nh; } } if (nl) lo = sl / nl; if (nh) hi = sh / nh; }
        double est;
        if (hi / lo > 2.0 && hi / lo < 4.5) est = lo;         // two clear clusters ~1:3
        else if (hi / lo >= 4.5) est = lo;                    // outliers: trust the short cluster
        else {                                                // one cluster only (a run of all dits or all dahs): judge it against the speed we already know
            const double med = d[d.size() / 2];
            if (!dit_known) est = med < 150.0 ? med : med / 3.0;   // no prior: a dit unless too long to be one (> 150 ms = slower than 8 WPM)
            else { const double r = med / dit_ms; est = std::fabs(std::log(r)) <= std::fabs(std::log(r / 3.0)) ? med : med / 3.0; }   // closer to 1 dit or to 3 dits?
        }
        const bool jump = dit_known && (est < 0.7 * dit_ms || est > 1.4 * dit_ms);                   // the operator changed speed: do not average old and new
        dit_ms = dit_known && !jump ? 0.6 * dit_ms + 0.4 * est : est; dit_known = true;
        adapt_bandwidth();
    }
    void adapt_bandwidth() {                                  // once the speed is known, narrow the filter to what the keying needs (about 4 / dit: 100 Hz at 30 WPM, 33 Hz at 10 WPM): less noise, same pulses
        if (!o.adaptive_bandwidth) return;
        const double want = std::clamp(4000.0 / dit_ms, 60.0, o.bandwidth_hz * 1.5);
        if (std::fabs(want - cur_bw) / cur_bw < 0.15) return;
        cur_bw = want; alpha = 1.0 - std::exp(-2 * kPi * (cur_bw / 2 / 0.435) / o.sample_rate);
    }
    double unit_ms() const {                                  // the speed readout averages the dit with the intra-character gap: keying edges shorten ON and lengthen OFF by the same amount
        if (!dit_known || intra.empty()) return dit_ms;
        std::vector<double> v(intra.begin(), intra.end()); std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end()); return 0.5 * (dit_ms + v[v.size() / 2]);
    }
    double word_gap_ms() const { return letter_gap > 0 ? std::max(1.45 * letter_gap, 3.5 * dit_ms) : 5.0 * dit_ms; }   // between a 3-unit and a 7-unit gap
    void learn_gap(double len) {                              // gaps >= 2 dits: letter gaps (3u) and word gaps (7u); u can exceed the dit (Farnsworth)
        gaps.push_back(len); if (gaps.size() > 12) gaps.pop_front();
        std::vector<double> d(gaps.begin(), gaps.end()); std::sort(d.begin(), d.end()); double lo = d.front(), hi = d.back();
        for (int it = 0; it < 8; ++it) { double sl = 0, sh = 0; int nl = 0, nh = 0; for (double x : d) { if (std::fabs(x - lo) <= std::fabs(x - hi)) { sl += x; ++nl; } else { sh += x; ++nh; } } if (nl) lo = sl / nl; if (nh) hi = sh / nh; }
        if (hi / lo > 1.8 && hi / lo < 3.4) letter_gap = lo;                                       // two clusters ~3:7
        else { const double med = d[d.size() / 2]; letter_gap = med < 5.0 * dit_ms || med >= 8.0 * dit_ms ? med : 3.0 * dit_ms; }   // one cluster: letters (normal or stretched), or only word gaps
    }
    void emit_char(std::string& out) {
        if (chr.empty()) return;
        const char c = char_for_morse(chr); out.push_back(c ? c : '*'); chr.clear(); emitted_since_space = true; space_done = false;
    }
    void on_edge(bool now_on, int run_steps, std::string& out) {   // the state that just ended lasted run_steps
        if (!present) { chr.clear(); return; }
        const double len = ms(run_steps);
        if (!now_on && len < 15.0) return;                      // an ON sliver shorter than a 60 WPM dit (the cut-off start of a pulse, or a noise spike) is not Morse
        if (!synced) {                                         // the first character after the signal appears is only partly heard: drop it, start at the next boundary
            if (!now_on) { on_ms.push_back(len); if (on_ms.size() > 10) on_ms.pop_front(); estimate_dit(); }
            else if (dit_known && len >= 2.0 * dit_ms) { synced = true; chr.clear(); }
            return;
        }
        if (!now_on) {                                         // an ON pulse just ended
            on_ms.push_back(len); if (on_ms.size() > 10) on_ms.pop_front(); ++pulses; estimate_dit();
            chr.push_back(len < 2.0 * dit_ms ? '.' : '-'); if (chr.size() > 8) chr.clear();
        } else {                                               // an OFF gap just ended: classify it
            if (len < 2.0 * dit_ms) { if (len >= 15.0) { intra.push_back(len); if (intra.size() > 12) intra.pop_front(); } }
            else {
                if (!chr.empty()) emit_char(out);
                if (len >= word_gap_ms() && emitted_since_space && !space_done) { out.push_back(' '); emitted_since_space = false; space_done = true; }
                learn_gap(len);
            }
        }
    }
    void idle(std::string& out) {                              // while the key is up: flush the character and the word space on timeout, before the next pulse arrives
        const double off = ms(off_len);
        if (!present || key) return;
        if (!chr.empty() && off >= 2.5 * dit_ms) emit_char(out);
        if (off >= 1.1 * word_gap_ms() && emitted_since_space && !space_done) { out.push_back(' '); emitted_since_space = false; space_done = true; }
    }
    void envelope_step(double e, std::string& out) {
        hist.push_back(float(e)); if (hist.size() > hist_max) hist.pop_front();
        if (++since_thr >= int(env_rate * 0.1)) { since_thr = 0; update_thresholds(); }
        if (!present) { key = false; run = 0; on_len = off_len = 0; pending = false; return; }
        const double hi = floor + 0.50 * (peak_local - floor), lo = floor + 0.35 * (peak_local - floor);
        const bool want = key ? e > lo : e > hi;
        // debounce: a change must persist for 3 envelope steps (6 ms) before it counts
        if (want != key) { if (!pending || pending_state != want) { pending = true; pending_state = want; pending_run = 0; } if (++pending_run >= 3) {
                const int ended = run; const bool was = key; key = want; run = pending_run; pending = false; on_edge(key, ended - 0, out); (void)was; off_len = key ? 0 : 0; } }
        else { pending = false; ++run; }
        if (key) { ++on_len; off_len = 0; } else { ++off_len; on_len = 0; idle(out); }
    }
    void audio(const float* x, size_t n, std::string& out) {
        for (size_t i = 0; i < n; ++i) {
            raw.push_back(x[i]); if (raw.size() > raw_max) raw.pop_front();
            const std::complex<double> m = double(x[i]) * std::polar(1.0, -phase); phase += step; if (phase > 2 * kPi) phase -= 2 * kPi;
            lp[0] += alpha * (m - lp[0]); for (int k = 1; k < 4; ++k) lp[k] += alpha * (lp[k - 1] - lp[k]);
            if (++count == D) {
                count = 0; envelope_step(2.0 * std::abs(lp[3]), out);
                if (o.track_pitch && ++track_counter >= int(env_rate * 0.5)) { track_counter = 0; track(); }
            }
        }
    }
};

CwDecoder::CwDecoder(CwOptions o) : p_(std::make_unique<Impl>(o)) {}
CwDecoder::~CwDecoder() = default;
void CwDecoder::process(const float* a, size_t n, std::string& out) { p_->audio(a, n, out); }
void CwDecoder::reset() { p_->reset(); }
double CwDecoder::wpm() const { return p_->dit_known ? 1200.0 / p_->unit_ms() : 0.0; }
double CwDecoder::pitch() const { return p_->pitch; }
bool CwDecoder::signal_present() const { return p_->present; }
std::string CwDecoder::status() const { char b[96]; if (!p_->present) std::snprintf(b, sizeof b, "CW: no signal"); else std::snprintf(b, sizeof b, "CW: %.0f WPM, %.0f Hz", wpm(), pitch()); return b; }

// ---------------------------------------------------------------- generators
std::vector<float> morse_envelope(const std::string& text, double wpm, double fs, double edge_ms, double lead_s, double farns) {
    const double dit = 1.2 / wpm, gap_unit = farns > 0 && farns < wpm ? (60.0 / farns - 31.0 * dit) / 19.0 : dit;   // PARIS = 50 units: 31 element units + 19 gap units
    std::vector<std::pair<bool, double>> seg; seg.push_back({false, lead_s});
    for (char ch : text) {
        if (ch == ' ') { seg.push_back({false, 7 * gap_unit - 3 * gap_unit}); continue; }                       // 3 gap units were already added after the previous char
        const char* m = morse_for(ch); if (!m) continue;
        for (const char* p = m; *p; ++p) { seg.push_back({true, *p == '.' ? dit : 3 * dit}); if (p[1]) seg.push_back({false, dit}); }
        seg.push_back({false, 3 * gap_unit});
    }
    seg.push_back({false, 1.0});
    std::vector<float> out; const size_t edge = size_t(edge_ms * 1e-3 * fs);
    for (auto [on, dur] : seg) {
        const size_t n = size_t(dur * fs);
        for (size_t i = 0; i < n; ++i) {
            double g = on ? 1.0 : 0.0;
            if (on) { if (i < edge) g = 0.5 - 0.5 * std::cos(kPi * double(i) / double(edge)); else if (n - i <= edge) g = 0.5 - 0.5 * std::cos(kPi * double(n - i) / double(edge)); }
            out.push_back(float(g));
        }
    }
    return out;
}
std::vector<float> synth_morse(const std::string& text, double wpm, double pitch, double fs, double amp, double edge_ms, double lead_s, double farns) {
    auto env = morse_envelope(text, wpm, fs, edge_ms, lead_s, farns); double phase = 0; const double w = 2 * kPi * pitch / fs;
    for (float& g : env) { g = float(amp * double(g) * std::sin(phase)); phase += w; }
    return env;
}
void add_noise(std::vector<float>& x, double rms, uint32_t seed) {
    uint64_t s = 0x9E3779B97F4A7C15ull ^ seed; auto rnd = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double(s >> 11) + 0.5) / 9007199254740992.0; };
    for (float& v : x) v += float(std::sqrt(-2 * std::log(rnd())) * std::cos(2 * kPi * rnd()) * rms);
}
}  // namespace fireside::decode
