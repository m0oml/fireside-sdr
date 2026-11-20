// RTTY decoder (ITA2 / Baudot, asynchronous 1 start + 5 data + 1.5 stop): two narrow tone detectors, a mark/space decision, mid-bit sampling.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <complex>
#include "fireside/decode.h"

namespace fireside::decode {
namespace {
constexpr double kPi = 3.14159265358979323846;
// ITA2, code = 5 bits as received (first data bit = least significant). Index 0..31. '\0' = prints nothing; '~' marks the shift codes (handled separately).
const char kLetters[32] = {0, 'T', '\r', 'O', ' ', 'H', 'N', 'M', '\n', 'L', 'R', 'G', 'I', 'P', 'C', 'V', 'E', 'Z', 'D', 'B', 'S', 'Y', 'F', 'X', 'A', 'W', 'J', '~', 'U', 'Q', 'K', '~'};
const char kFigures[32] = {0, '5', '\r', '9', ' ', '#', ',', '.', '\n', ')', '4', '&', '8', '0', ':', ';', '3', '"', '$', '?', '\'', '6', '!', '/', '-', '2', 7, '~', '7', '1', '(', '~'};   // US TTY figures; 7 = BEL (J)
}  // namespace
char ita2_char(int code, bool figs) { if (code < 0 || code > 31) return 0; const char c = figs ? kFigures[code] : kLetters[code]; return c == '~' ? 0 : c; }
int ita2_code(char ch, bool* figs) {
    ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (ch == ' ') { if (figs) *figs = false; return 4; } if (ch == '\n') { if (figs) *figs = false; return 8; } if (ch == '\r') { if (figs) *figs = false; return 2; }
    for (int i = 0; i < 32; ++i) if (kLetters[i] == ch && ch != 0 && ch != '~') { if (figs) *figs = false; return i; }
    for (int i = 0; i < 32; ++i) if (kFigures[i] == ch && ch != 0 && ch != '~') { if (figs) *figs = true; return i; }
    return -1;
}

struct RttyDecoder::Impl {
    RttyOptions o; int D = 24; double rate, T;                  // decision rate 2 kHz; T = samples per bit
    double phase[2] = {0, 0}, step[2]; std::complex<double> lp[2][2]; double alpha;
    int count = 0; double env_m = 0, env_s = 0, d = 0, prev_d = 0, hi = 0, lo = 0;
    enum { IDLE, FRAME } state = IDLE; long t = 0, frame_start = 0; int bits[5]; bool figures = false; size_t errors = 0; bool present = false; double avg_hi = 0, avg_lo = 0;
    explicit Impl(RttyOptions op) : o(op) {
        D = std::max(1, int(op.sample_rate / 2000)); rate = op.sample_rate / D; T = rate / op.baud;
        const double mk = op.mark_hz, sp = op.reverse ? op.mark_hz + op.shift_hz : op.mark_hz - op.shift_hz; step[0] = 2 * kPi * mk / op.sample_rate; step[1] = 2 * kPi * sp / op.sample_rate;
        alpha = 1.0 - std::exp(-2 * kPi * (0.75 * op.baud) / op.sample_rate); reset();
    }
    void reset() { for (auto& a : lp) a[0] = a[1] = {0, 0}; phase[0] = phase[1] = 0; count = 0; env_m = env_s = d = prev_d = 0; state = IDLE; t = 0; figures = false; errors = 0; present = false; avg_hi = avg_lo = 0; }
    void emit(int code, std::string& out) {
        if (code == 31) { figures = false; return; } if (code == 27) { figures = true; return; }
        const char c = ita2_char(code, figures);
        if (code == 4 && o.usos) figures = false;                     // unshift on space
        if (c == '\n') out.push_back('\n'); else if (c == '\r' || c == 0 || c == 7) return; else out.push_back(c);
    }
    void decision(std::string& out) {                            // called at the decision rate
        const double sum = env_m + env_s; hi = std::max(env_m, env_s); lo = std::min(env_m, env_s);
        avg_hi = 0.995 * avg_hi + 0.005 * hi; avg_lo = 0.995 * avg_lo + 0.005 * lo;
        present = avg_hi > 1e-6 && avg_hi / std::max(avg_lo, 1e-3 * avg_hi) > o.min_ratio;
        prev_d = d; d = sum > 1e-12 ? (env_m - env_s) / sum : 0.0;                  // +1 = mark, -1 = space
        if (!present) { state = IDLE; return; }
        if (state == IDLE) { if (prev_d > 0.0 && d <= 0.0) { state = FRAME; t = 0; } return; }   // a mark -> space edge is the start of a character
        ++t;
        auto at = [&](double bits_from_start) { return t == long(std::lround(bits_from_start * T)); };
        if (at(0.5)) { if (d > 0) { state = IDLE; ++errors; } return; }                                  // the start bit must still be space half a bit later
        for (int k = 0; k < 5; ++k) if (at(1.5 + k)) bits[k] = d > 0 ? 1 : 0;
        if (at(6.5)) {
            if (d > 0) { int code = 0; for (int k = 0; k < 5; ++k) code |= bits[k] << k; emit(code, out); } else ++errors;     // the stop bit must be mark
        }
        if (t >= long(std::lround(7.0 * T))) state = IDLE;
    }
    void audio(const float* x, size_t n, std::string& out) {
        for (size_t i = 0; i < n; ++i) {
            for (int r = 0; r < 2; ++r) {
                const std::complex<double> m = double(x[i]) * std::polar(1.0, -phase[r]); phase[r] += step[r]; if (phase[r] > 2 * kPi) phase[r] -= 2 * kPi;
                lp[r][0] += alpha * (m - lp[r][0]); lp[r][1] += alpha * (lp[r][0] - lp[r][1]);
            }
            if (++count == D) { count = 0; env_m = std::abs(lp[0][1]); env_s = std::abs(lp[1][1]); decision(out); }
        }
    }
};
RttyDecoder::RttyDecoder(RttyOptions o) : p_(std::make_unique<Impl>(o)) {}
RttyDecoder::~RttyDecoder() = default;
void RttyDecoder::process(const float* a, size_t n, std::string& out) { p_->audio(a, n, out); }
void RttyDecoder::reset() { p_->reset(); }
bool RttyDecoder::signal_present() const { return p_->present; }
size_t RttyDecoder::framing_errors() const { return p_->errors; }
std::string RttyDecoder::status() const { char b[96]; if (!p_->present) std::snprintf(b, sizeof b, "RTTY: no signal"); else std::snprintf(b, sizeof b, "RTTY %.2f baud, %.0f Hz shift: %zu framing errors", p_->o.baud, p_->o.shift_hz, p_->errors); return b; }

std::vector<float> synth_rtty(const std::string& text, double mark, double shift, double baud, double fs, double amp, double lead_s, bool reverse) {
    const double space = reverse ? mark + shift : mark - shift; std::vector<int> bitstream;                 // 1 = mark, 0 = space (one entry per bit; stop bit = 1.5 entries handled by timing)
    struct Sym { int bit; double len; }; std::vector<Sym> syms; syms.push_back({1, lead_s * baud}); bool figs = false, first = true;
    auto send = [&](int code) { syms.push_back({0, 1.0}); for (int k = 0; k < 5; ++k) syms.push_back({(code >> k) & 1, 1.0}); syms.push_back({1, 1.5}); };
    for (char c : text) {
        bool need_figs = false; const int code = ita2_code(c, &need_figs); if (code < 0) continue;
        if (c != ' ' && c != '\n' && c != '\r') { if (first || need_figs != figs) { send(need_figs ? 27 : 31); figs = need_figs; } }
        else if (c == ' ' || c == '\n') { /* letters/figures independent */ }
        send(code); first = false;
    }
    syms.push_back({1, 2.0});
    std::vector<float> out; double ph = 0; double acc_t = 0;
    for (const auto& s : syms) {
        const size_t n = size_t(s.len * fs / baud); const double w = 2 * kPi * (s.bit ? mark : space) / fs;
        for (size_t i = 0; i < n; ++i) { out.push_back(float(amp * std::sin(ph))); ph += w; } (void)acc_t;
    }
    return out;
}
}  // namespace fireside::decode
