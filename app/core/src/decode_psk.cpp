// PSK31 decoder (differential BPSK, 31.25 baud) with 16 candidate symbol timings and Varicode.
// The Varicode table is the standard one from G3PLX's PSK31 specification (ASCII 0-127).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "fireside/decode.h"

namespace fireside::decode {
namespace {
constexpr double kPi = 3.14159265358979323846;
const char* const kVaricode[128] = {
    "1010101011", "1011011011", "1011101101", "1101110111", "1011101011", "1101011111", "1011101111", "1011111101",
    "1011111111", "11101111", "11101", "1101101111", "1011011101", "11111", "1101110101", "1110101011",
    "1011110111", "1011110101", "1110101101", "1110101111", "1101011011", "1101101011", "1101101101", "1101010111",
    "1101111011", "1101111101", "1110110111", "1101010101", "1101011101", "1110111011", "1011111011", "1101111111",
    "1", "111111111", "101011111", "111110101", "111011011", "1011010101", "1010111011", "101111111",
    "11111011", "11110111", "101101111", "111011111", "1110101", "110101", "1010111", "110101111",
    "10110111", "10111101", "11101101", "11111111", "101110111", "101011011", "101101011", "110101101",
    "110101011", "110110111", "11110101", "110111101", "111101101", "1010101", "111010111", "1010101111",
    "1010111101", "1111101", "11101011", "10101101", "10110101", "1110111", "11011011", "11111101",
    "101010101", "1111111", "111111101", "101111101", "11010111", "10111011", "11011101", "10101011",
    "11010101", "111011101", "10101111", "1101111", "1101101", "101010111", "110110101", "101011101",
    "101110101", "101111011", "1010101101", "111110111", "111101111", "111111011", "1010111111", "101101101",
    "1011011111", "1011", "1011111", "101111", "101101", "11", "111101", "1011011",
    "101011", "1101", "111101011", "10111111", "11011", "111011", "1111", "111",
    "111111", "110111111", "10101", "10111", "101", "110111", "1111011", "1101011",
    "11011111", "1011101", "111010101", "1010110111", "110111011", "1010110101", "1011010111", "1110110101",
};
const std::map<std::string, unsigned char>& rev_table() {
    static const auto m = [] { std::map<std::string, unsigned char> r; for (int i = 0; i < 128; ++i) r[kVaricode[i]] = static_cast<unsigned char>(i); return r; }();
    return m;
}
}  // namespace
const char* varicode_for(unsigned char c) { return c < 128 ? kVaricode[c] : nullptr; }

struct Psk31Decoder::Impl {
    PskOptions o; int N, P = 16, step; double phase = 0, nominal, center;
    std::vector<std::complex<double>> acc, prev; std::vector<int> pos; std::vector<double> energy; int best = 0; bool have_best = false;
    std::string cur; bool pending_zero = false; double quality = 0, coh = 0, contrast = 0; bool shaped = false; double afc_acc = 0; int afc_n = 0;
    std::vector<float> ring; size_t ring_pos = 0, ring_fill = 0; int cog_counter = 0;         // last 0.5 s of audio for the coarse centre search
    std::complex<double> nacc[2] = {{0, 0}, {0, 0}}; double nphase[2] = {0, 0}, noise = 0; int nsamples = 0;      // two noise-reference mixers at centre +-250 Hz
    explicit Impl(PskOptions op) : o(op), N(int(std::lround(op.sample_rate / 31.25))), step(N / 16), nominal(op.center_hz), center(op.center_hz) { reset(); }
    void reset() {
        acc.assign(size_t(P), {0, 0}); prev.assign(size_t(P), {0, 0}); energy.assign(size_t(P), 0.0); pos.resize(size_t(P));
        for (int p = 0; p < P; ++p) pos[size_t(p)] = -p * step;
        best = 0; have_best = false; cur.clear(); pending_zero = false; quality = 0; coh = 0; contrast = 0; shaped = false; center = nominal; afc_acc = 0; afc_n = 0; phase = 0; nacc[0] = nacc[1] = {0, 0}; nphase[0] = nphase[1] = 0; noise = 0; nsamples = 0; ring.assign(size_t(op_rate() * 0.5), 0.0f); ring_pos = ring_fill = 0; cog_counter = 0;
    }
    double op_rate() const { return o.sample_rate; }
    void coarse_search() {                                     // centre of gravity of the power within +-40 Hz of the current centre: PSK31 sidebands are symmetric about the carrier (even when the carrier itself is suppressed, as in idle)
        if (ring_fill < ring.size()) return;
        double sp = 0, sf = 0;
        for (double f = center - 40; f <= center + 40; f += 2.0) {
            const double wv = 2 * kPi * f / o.sample_rate, c = 2 * std::cos(wv); double s1 = 0, s2 = 0;
            for (size_t k = 0; k < ring.size(); ++k) { const double s0 = double(ring[(ring_pos + k) % ring.size()]) + c * s1 - s2; s2 = s1; s1 = s0; }
            const double pw = s1 * s1 + s2 * s2 - c * s1 * s2; sp += pw; sf += pw * f;
        }
        if (sp <= 0) return;
        const double cog = sf / sp;
        if (std::fabs(cog - center) >= 8.0) center = std::clamp(center + 0.7 * (cog - center), nominal - 30, nominal + 30);
    }
    void put_char(unsigned char c, std::string& out) { if (c == 10) out.push_back('\n'); else if (c >= 32 && c < 127) out.push_back(char(c)); else if (c == 9) out.push_back(' '); }
    void bit(int b, std::string& out) {                        // Varicode: characters are separated by two or more zeros, codes never contain two zeros in a row
        if (b) { if (pending_zero) { if (!cur.empty()) cur.push_back('0'); pending_zero = false; } cur.push_back('1'); }      // a code always starts with 1: a lone idle zero before it is not part of it
        else if (pending_zero) { if (!cur.empty()) { auto it = rev_table().find(cur); if (it != rev_table().end() && quality >= o.min_snr && shaped) put_char(it->second, out); cur.clear(); } pending_zero = false; }
        else pending_zero = true;
        if (cur.size() > 12) cur.clear();
    }
    void symbol(int p, std::complex<double> z, std::string& out) {
        const std::complex<double> d = z * std::conj(prev[size_t(p)]); prev[size_t(p)] = z;
        energy[size_t(p)] = 0.9 * energy[size_t(p)] + 0.1 * std::abs(z);
        int b = 0; for (int i = 1; i < P; ++i) if (energy[size_t(i)] > energy[size_t(b)]) b = i;
        if (!have_best || (b != best && energy[size_t(b)] > 1.1 * energy[size_t(best)])) { best = b; have_best = true; }
        quality = noise > 1e-12 ? energy[size_t(best)] / noise : (energy[size_t(best)] > 1e-9 ? 100.0 : 0.0);   // with no noise at all (a clean test signal) any signal is strong
        if (p != best) return;
        // PSK31 shape (keyed CW, FT8 tones and noise can pass the SNR gate, PSK31 must also pass these): the phase steps between symbols are 0 or 180 degrees (cos 2*step near 1),
        // and the symbol timing matters (a reversal cancels in an integration that straddles it: the best timing is much stronger than the opposite one; a steady tone is the same at all).
        if (std::abs(d) > 1e-18) coh = 0.95 * coh + 0.05 * std::cos(2 * std::arg(d));
        contrast = energy[size_t(best)] / std::max(1e-12, energy[size_t((best + P / 2) % P)]);
        shaped = coh >= o.min_phase_quality && contrast >= o.min_timing_contrast;
        const bool one = d.real() > 0;                          // same phase as the previous symbol = 1, reversal = 0
        if (o.afc && one && quality >= o.min_snr && std::abs(z) > 1e-9) { afc_acc += std::arg(d); if (++afc_n == 16) { const double df = afc_acc / afc_n / (2 * kPi / 31.25); center += std::clamp(0.5 * df, -2.0, 2.0); center = std::clamp(center, nominal - 25, nominal + 25); afc_acc = 0; afc_n = 0; } }
        bit(one ? 1 : 0, out);
    }
    void audio(const float* x, size_t n, std::string& out) {
        const double w = 2 * kPi * center / o.sample_rate;
        for (size_t i = 0; i < n; ++i) {
            if (!ring.empty()) { ring[ring_pos] = x[i]; ring_pos = (ring_pos + 1) % ring.size(); if (ring_fill < ring.size()) ++ring_fill; }
            if (o.afc && ++cog_counter >= int(o.sample_rate)) { cog_counter = 0; if (quality >= o.min_snr) coarse_search(); }                // once a second, only on a signal
            const std::complex<double> m = double(x[i]) * std::polar(1.0, -phase); phase += 2 * kPi * center / o.sample_rate; if (phase > 2 * kPi) phase -= 2 * kPi;
            for (int r = 0; r < 2; ++r) {                       // noise references: the same integrate-and-dump length, 250 Hz either side; the quieter one is used (a neighbouring signal may sit on the other side)
                nacc[r] += double(x[i]) * std::polar(1.0, -nphase[r]); nphase[r] += 2 * kPi * (center + (r ? 250.0 : -250.0)) / o.sample_rate; if (nphase[r] > 2 * kPi) nphase[r] -= 2 * kPi;
            }
            if (++nsamples == N) { nsamples = 0; const double nm = std::min(std::abs(nacc[0]), std::abs(nacc[1])) * 1.25; noise = noise == 0 ? nm : 0.9 * noise + 0.1 * nm; nacc[0] = nacc[1] = {0, 0}; }   // x1.25: the minimum of two noise magnitudes underestimates their mean
            for (int p = 0; p < P; ++p) {
                int& ps = pos[size_t(p)]; if (ps >= 0) acc[size_t(p)] += m;
                if (++ps == N) { ps = 0; const auto z = acc[size_t(p)]; acc[size_t(p)] = {0, 0}; symbol(p, z, out); }
            }
        }
        (void)w;
    }
};
Psk31Decoder::Psk31Decoder(PskOptions o) : p_(std::make_unique<Impl>(o)) {}
Psk31Decoder::~Psk31Decoder() = default;
void Psk31Decoder::process(const float* a, size_t n, std::string& out) { p_->audio(a, n, out); }
void Psk31Decoder::reset() { p_->reset(); }
double Psk31Decoder::center() const { return p_->center; }
bool Psk31Decoder::locked() const { return p_->quality >= p_->o.min_snr && p_->shaped; }
std::string Psk31Decoder::status() const { char b[96]; if (!locked()) std::snprintf(b, sizeof b, "PSK31 %.0f Hz: no signal", p_->center); else std::snprintf(b, sizeof b, "PSK31 %.0f Hz: locked (SNR %.1fx)", p_->center, std::min(p_->quality, 99.0)); return b; }

// ---------------------------------------------------------------- generator
std::vector<float> synth_psk31(const std::string& text, double center, double fs, double amp, double lead_s) {
    std::string bits(size_t(31.25 * lead_s), '0');
    for (char c : text) { const char* v = varicode_for(static_cast<unsigned char>(c)); if (!v) continue; bits += v; bits += "00"; }
    bits += std::string(32, '0');
    const double T = fs / 31.25; const size_t total = size_t(double(bits.size()) * T) + 1; std::vector<float> out(total);
    std::vector<int> v(bits.size() + 2); v[0] = 1; for (size_t k = 0; k < bits.size(); ++k) v[k + 1] = bits[k] == '1' ? v[k] : -v[k]; v[bits.size() + 1] = v[bits.size()];   // symbol k has value v[k+1]; a 0 reverses the phase
    for (size_t i = 0; i < total; ++i) {                        // raised-cosine transitions between symbol CENTRES: the amplitude dips to zero half-way between two centres when the phase reverses
        const double t = double(i) / T - 0.5; const long k = long(std::floor(t)); const double u = t - double(k);
        const double a = k < 0 ? double(v[0]) : double(v[size_t(std::min<long>(k + 1, long(v.size()) - 1))]), b = double(v[size_t(std::min<long>(k + 2, long(v.size()) - 1))]);
        const double env = a * 0.5 * (1 + std::cos(kPi * u)) + b * 0.5 * (1 - std::cos(kPi * u));
        out[i] = float(amp * env * std::sin(2 * kPi * center * double(i) / fs));
    }
    return out;
}
}  // namespace fireside::decode
