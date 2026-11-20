// Per-candidate FT8 demodulator: see ft8_demod.h. Reimplemented from WSJT-X ft8_downsample.f90, sync8d.f90 and ft8b.f90 (GPL v3). FFTs: kiss_fft (in ft8_lib).
#include "ft8_demod.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

extern "C" {
#include <fft/kiss_fft.h>
#include <fft/kiss_fftr.h>
#include <ft8/constants.h>
}

namespace fireside::decode::demod {
namespace {
constexpr int kNfft1 = 192000, kNfft2 = 3200, kNp2 = 2812, kNsps = 1920, kNn = 79;
constexpr int kNfft1Ft4 = 90000;                                          // FT4: the whole 7.5 s slot at 12 kHz
constexpr double kFs2 = 200.0, kBaud = 12000.0 / kNsps, kTwoPi = 6.283185307179586;
using cf = std::complex<float>;
struct KissDeleter { void operator()(void* p) const { std::free(p); } };

// 200 Hz complex baseband with tone 0 at 0 Hz (ft8_downsample): band f0 - 1.5 baud .. f0 + 8.5 baud, cosine-tapered at both ends.
std::vector<cf> downsample(const std::vector<cf>& cx, double f0) {
    static thread_local std::unique_ptr<void, KissDeleter> inv(kiss_fft_alloc(kNfft2, 1, nullptr, nullptr));
    static const std::vector<float> taper = [] { std::vector<float> t(101); for (int i = 0; i <= 100; ++i) t[size_t(i)] = float(0.5 * (1.0 + std::cos(i * M_PI / 100))); return t; }();
    const double df = 12000.0 / kNfft1;
    const int i0 = int(std::lround(f0 / df)), it = std::min(int(std::lround((f0 + 8.5 * kBaud) / df)), kNfft1 / 2), ib = std::max(1, int(std::lround((f0 - 1.5 * kBaud) / df)));
    std::vector<cf> c1(kNfft2, cf(0, 0)); int k = 0;
    for (int i = ib; i <= it && k < kNfft2; ++i) c1[size_t(k++)] = cx[size_t(i)];
    for (int i = 0; i <= 100 && i < k; ++i) c1[size_t(i)] *= taper[size_t(100 - i)];
    for (int i = 0; i <= 100 && k - 1 - 100 + i >= 0; ++i) c1[size_t(k - 1 - 100 + i)] *= taper[size_t(i)];
    std::vector<cf> sh(kNfft2); const int s = ((i0 - ib) % kNfft2 + kNfft2) % kNfft2;                  // cshift: bin i0 (tone 0) moves to 0 Hz
    for (int j = 0; j < kNfft2; ++j) sh[size_t(j)] = c1[size_t((j + s) % kNfft2)];
    std::vector<cf> out(kNfft2);
    kiss_fft(static_cast<kiss_fft_cfg>(inv.get()), reinterpret_cast<const kiss_fft_cpx*>(sh.data()), reinterpret_cast<kiss_fft_cpx*>(out.data()));
    const float fac = float(1.0 / std::sqrt(double(kNfft1) * kNfft2)); for (auto& v : out) v *= fac;
    return out;
}
// Power of the three 7x7 Costas arrays with the waveform starting at sample i0 (sync8d); delf tweaks the reference frequency (Hz).
float sync8d(const std::vector<cf>& cd, int i0, double delf) {
    float sync = 0;
    for (int i = 0; i < 7; ++i) {
        const double dphi = kTwoPi * (kFT8_Costas_pattern[i] / 32.0 + delf / kFs2);
        for (int g = 0; g < 3; ++g) {
            const int i1 = i0 + (i + 36 * g) * 32; if (i1 < 0 || i1 + 31 > kNp2 - 1) continue;
            cf z(0, 0); double phi = 0;
            for (int j = 0; j < 32; ++j) { z += cd[size_t(i1 + j)] * std::conj(cf(float(std::cos(phi)), float(std::sin(phi)))); phi += dphi; }
            sync += std::norm(z);
        }
    }
    return sync;
}
void normalize(float* b, int n = 174) {                                 // normalizebmet: divide by the standard deviation
    double s = 0, s2 = 0; for (int i = 0; i < n; ++i) { s += b[i]; s2 += double(b[i]) * b[i]; }
    const double av = s / n, av2 = s2 / n, var = av2 - av * av; const double sig = var > 0 ? std::sqrt(var) : std::sqrt(av2);
    if (sig > 0) for (int i = 0; i < n; ++i) b[i] = float(b[i] / sig);
}
}  // namespace

SlotSpectrum::SlotSpectrum(const float* x, size_t n, bool ft4) : cx_(size_t((ft4 ? kNfft1Ft4 : kNfft1) / 2 + 1)), nfft_(ft4 ? kNfft1Ft4 : kNfft1) {
    static thread_local std::unique_ptr<void, KissDeleter> fwd8(kiss_fftr_alloc(kNfft1, 0, nullptr, nullptr)), fwd4(kiss_fftr_alloc(kNfft1Ft4, 0, nullptr, nullptr));
    std::vector<float> in(size_t(nfft_), 0.0f); std::copy_n(x, std::min<size_t>(n, ft4 ? size_t(kNfft1Ft4) : 180000), in.begin());
    kiss_fftr(static_cast<kiss_fftr_cfg>((ft4 ? fwd4 : fwd8).get()), in.data(), reinterpret_cast<kiss_fft_cpx*>(cx_.data()));
}

Soft demodulate(const SlotSpectrum& spec, double f1, double t0_guess) {
    Soft r;
    std::vector<cf> cd = downsample(spec.bins(), f1);
    // time: +-0.2 s around the candidate (WSJT-X searches +-50 ms around its own, finer candidate), then frequency +-2.5 Hz in 0.5 Hz, then time again +-4 samples
    const int i0 = int(std::lround(t0_guess * kFs2)); int ibest = i0; float smax = -1;
    for (int idt = i0 - 40; idt <= i0 + 40; ++idt) { const float s = sync8d(cd, idt, 0.0); if (s > smax) { smax = s; ibest = idt; } }
    double delfbest = 0; smax = -1;
    for (int ifr = -5; ifr <= 5; ++ifr) { const float s = sync8d(cd, ibest, ifr * 0.5); if (s > smax) { smax = s; delfbest = ifr * 0.5; } }
    f1 += delfbest; cd = downsample(spec.bins(), f1);
    int ib2 = ibest; smax = -1;
    for (int idt = -4; idt <= 4; ++idt) { const float s = sync8d(cd, ibest + idt, 0.0); if (s > smax) { smax = s; ib2 = ibest + idt; } }
    ibest = ib2; r.f0 = f1; r.t0 = ibest / kFs2;
    // complex spectra of the 79 symbols: bins 0..7 of a 32-point DFT (6.25 Hz per bin = the tone spacing)
    cf cs[8][kNn]; float s8[8][kNn];
    for (int k = 0; k < kNn; ++k) {
        const int i1 = ibest + k * 32; const bool in = i1 >= 0 && i1 + 31 <= kNp2 - 1;
        for (int t = 0; t < 8; ++t) {
            cf z(0, 0); if (in) for (int n = 0; n < 32; ++n) { const double ph = -kTwoPi * t * n / 32.0; z += cd[size_t(i1 + n)] * cf(float(std::cos(ph)), float(std::sin(ph))); }
            cs[t][k] = z / 1e3f; s8[t][k] = std::abs(z);
        }
    }
    for (int k = 0; k < 7; ++k) for (int g : {0, 36, 72}) { int ip = 0; for (int t = 1; t < 8; ++t) if (s8[t][k + g] > s8[ip][k + g]) ip = t; if (ip == kFT8_Costas_pattern[k]) ++r.nsync; }
    if (r.nsync <= 6) return r;                                           // WSJT-X bails out (syncmin 6 at its deepest setting)
    // bit metrics: for 1, 2 and 3 symbols at a time, the best tone combination with the bit set minus the best with it clear
    float bmeta[174] = {}, bmetb[174] = {}, bmetc[174] = {}, bmetd[174] = {}, bmete[174] = {}; float s2[512];
    for (int nsym = 1; nsym <= 3; ++nsym) {
        const int nt = 1 << (3 * nsym), ibmax = nsym == 1 ? 2 : nsym == 2 ? 5 : 8;
        for (int ihalf = 1; ihalf <= 2; ++ihalf) for (int k = 1; k <= 29; k += nsym) {
            const int ks = (ihalf == 1 ? k + 7 : k + 43) - 1;                 // 0-based symbol index
            for (int i = 0; i < nt; ++i) {
                const int i1 = i / 64, i2 = (i & 63) / 8, i3 = i & 7;
                auto C = [&](int t, int sym) { return sym < kNn ? cs[kFT8_Gray_map[t]][sym] : cf(0, 0); };
                s2[i] = nsym == 1 ? std::abs(C(i3, ks)) : nsym == 2 ? std::abs(C(i2, ks) + C(i3, ks + 1)) : std::abs(C(i1, ks) + C(i2, ks + 1) + C(i3, ks + 2));
            }
            const int i32 = (k - 1) * 3 + (ihalf - 1) * 87;                   // 0-based bit index
            for (int ib = 0; ib <= ibmax; ++ib) {
                if (i32 + ib > 173) continue;
                const int bit = ibmax - ib; float m1 = -1e30f, m0 = -1e30f;
                for (int i = 0; i < nt; ++i) { if ((i >> bit) & 1) m1 = std::max(m1, s2[i]); else m0 = std::max(m0, s2[i]); }
                const float bm = m1 - m0;
                if (nsym == 1) { bmeta[i32 + ib] = bm; const float den = std::max(m1, m0); bmetd[i32 + ib] = den > 0 ? bm / den : 0.0f; }
                else if (nsym == 2) bmetb[i32 + ib] = bm; else bmetc[i32 + ib] = bm;
            }
        }
    }
    for (int i = 0; i < 174; ++i) { float b = bmeta[i]; if (std::fabs(bmetb[i]) > std::fabs(b)) b = bmetb[i]; if (std::fabs(bmetc[i]) > std::fabs(b)) b = bmetc[i]; bmete[i] = b; }
    float* sets[5] = {bmeta, bmetb, bmetc, bmetd, bmete};
    for (int s = 0; s < 5; ++s) { normalize(sets[s]); for (int i = 0; i < 174; ++i) r.llr[s][i] = 2.83f * sets[s][i]; }
    r.ok = true; return r;
}

// ---------------------------------------------------------------- FT4 (see ft8_demod.h)
namespace {
constexpr int kNdown4 = 18, kNss4 = 32, kNn4 = 103, kNfft2Ft4 = kNfft1Ft4 / kNdown4;          // 666.67 Hz, 32 samples per symbol, 103 sync + data symbols, 5000 baseband samples
constexpr double kFs4 = 12000.0 / kNdown4, kBaud4 = 12000.0 / 576;
// ft4_downsample: the band f0 - 1 baud .. f0 + 4 baud (flat from -0.5 to +3.5 baud: tones 0..3), tone 0 at 0 Hz, 666.67 Hz complex baseband, mean power 1.
std::vector<cf> downsample4(const std::vector<cf>& cx, double f0) {
    static thread_local std::unique_ptr<void, KissDeleter> inv(kiss_fft_alloc(kNfft2Ft4, 1, nullptr, nullptr));
    static const std::vector<float> window = [] {
        const double df = 12000.0 / kNfft1Ft4; const int iwt = int(0.5 * kBaud4 / df), iwf = int(4 * kBaud4 / df), iws = int(kBaud4 / df);
        std::vector<float> w(kNfft2Ft4, 0.0f);
        for (int j = 0; j < iwt; ++j) w[size_t(j)] = float(0.5 * (1 + std::cos(M_PI * (iwt - 1 - j) / iwt)));
        for (int j = iwt; j < iwt + iwf; ++j) w[size_t(j)] = 1.0f;
        for (int j = 0; j < iwt; ++j) w[size_t(iwt + iwf + j)] = float(0.5 * (1 + std::cos(M_PI * j / iwt)));
        std::vector<float> sh(kNfft2Ft4); for (int j = 0; j < kNfft2Ft4; ++j) sh[size_t(j)] = w[size_t((j + iws) % kNfft2Ft4)];   // cshift(window, iws)
        return sh; }();
    const double df = 12000.0 / kNfft1Ft4; const int i0 = int(std::lround(f0 / df)), nh = kNfft1Ft4 / 2;
    std::vector<cf> c1(kNfft2Ft4, cf(0, 0));
    if (i0 >= 0 && i0 <= nh) c1[0] = cx[size_t(i0)];
    for (int i = 1; i <= kNfft2Ft4 / 2; ++i) { if (i0 + i >= 0 && i0 + i <= nh) c1[size_t(i)] = cx[size_t(i0 + i)]; if (i0 - i >= 0 && i0 - i <= nh && i < kNfft2Ft4 - i) c1[size_t(kNfft2Ft4 - i)] = cx[size_t(i0 - i)]; }
    for (int i = 0; i < kNfft2Ft4; ++i) c1[size_t(i)] *= window[size_t(i)];
    std::vector<cf> out(kNfft2Ft4);
    kiss_fft(static_cast<kiss_fft_cfg>(inv.get()), reinterpret_cast<const kiss_fft_cpx*>(c1.data()), reinterpret_cast<kiss_fft_cpx*>(out.data()));
    double p = 0; for (const cf& v : out) p += std::norm(v); p /= kNfft2Ft4; if (p > 0) { const float g = float(1.0 / std::sqrt(p)); for (auto& v : out) v *= g; }
    return out;
}
// sync4d: magnitude of the correlation with each 4x4 Costas array (every 2nd sample, so 16 per symbol), the reference shifted by idf Hz (-16..16); summed over the four arrays.
// A partly outside array counts if more than 16 of its samples are inside (as WSJT-X does for the first and last). References are precomputed (continuous phase across the 4 symbols).
struct SyncRefs { cf r[33][4][64]; SyncRefs() {
    for (int idf = -16; idf <= 16; ++idf) for (int g = 0; g < 4; ++g) { double phi = 0; int n = 0;
        for (int k = 0; k < 4; ++k) { const double dphi = kTwoPi * (2.0 * kFT4_Costas_pattern[g][k] / kNss4 + 2.0 * idf / kFs4); for (int j = 0; j < kNss4 / 2; ++j) { r[idf + 16][g][n++] = std::conj(cf(float(std::cos(phi)), float(std::sin(phi)))); phi += dphi; } } } } };
const SyncRefs& refs() { static const SyncRefs r; return r; }
float sync4d(const std::vector<cf>& cd, int i0, int idf) {
    const int np = int(cd.size()); float sync = 0; const auto& R = refs().r[idf + 16];
    for (int g = 0; g < 4; ++g) {
        const int i1 = i0 + 33 * g * kNss4; cf z(0, 0); int used = 0;
        for (int n = 0; n < 64; ++n) { const int idx = i1 + 2 * n; if (idx < 0 || idx >= np) continue; z += cd[size_t(idx)] * R[g][n]; ++used; }
        if (used > 16) sync += std::abs(z) / float(2 * kNss4);
    }
    return sync;
}
}  // namespace

namespace {
Soft metrics_ft4(const SlotSpectrum& spec, double f1, int ibest, float smax);
}
std::vector<Soft> demodulate_ft4(const SlotSpectrum& spec, double f0) {
    std::vector<Soft> out; if (spec.nfft() != kNfft1Ft4) return out;
    const std::vector<cf> cd2 = downsample4(spec.bins(), f0);
    // WSJT-X: the start of the first Costas array is searched over -0.52 .. +1.52 s in three segments (0.16..0.84 s first, then 0.84..1.52, then -0.52..0.16), coarse (4 samples,
    // 3 Hz in -12..12) then fine (1 sample, 1 Hz, +-5 / +-4). A later segment is tried only if its sync beats the first one's; each qualifying segment is demodulated.
    static const int kSeg[3][2] = {{108, 560}, {560, 1012}, {-344, 108}}; float smax1 = 0;
    for (int seg = 0; seg < 3; ++seg) {
        int ibest = -1, idfbest = 0; float smax = -99;
        for (int idf = -12; idf <= 12; idf += 3) for (int ib = kSeg[seg][0]; ib <= kSeg[seg][1]; ib += 4) { const float s = sync4d(cd2, ib, idf); if (s > smax) { smax = s; ibest = ib; idfbest = idf; } }
        { const int ib0 = ibest, idf0 = idfbest; smax = -99;
          for (int idf = idf0 - 4; idf <= idf0 + 4; ++idf) for (int ib = ib0 - 5; ib <= ib0 + 5; ++ib) { const float s = sync4d(cd2, ib, idf); if (s > smax) { smax = s; ibest = ib; idfbest = idf; } } }
        if (seg == 0) smax1 = smax;
        if (smax < 1.2f) continue;                                                                    // too little sync power
        if (seg > 0 && smax < smax1) continue;
        const double f1 = f0 + idfbest; if (f1 <= 10.0 || f1 >= 4990.0) continue;
        out.push_back(metrics_ft4(spec, f1, ibest, smax));
    }
    return out;
}
namespace {
Soft metrics_ft4(const SlotSpectrum& spec, double f1, int ibest, float smax) {
    Soft r; r.sync_power = smax;
    std::vector<cf> cb = downsample4(spec.bins(), f1);
    { double p = 0; for (const cf& v : cb) p += std::norm(v); p /= double(kNss4 * kNn4); if (p > 0) { const float g = float(1.0 / std::sqrt(p)); for (auto& v : cb) v *= g; } }
    r.f0 = f1; r.t0 = (ibest - kNss4) / kFs4;                                                         // start of the ramp symbol, as the candidate's time
    // complex spectra of the 103 symbols: bins 0..3 of a 32-point DFT (20.83 Hz = the tone spacing)
    static thread_local float s4[4][kNn4]; static thread_local cf csl[4][kNn4];
    for (int k = 0; k < kNn4; ++k) {
        const int i1 = ibest + k * kNss4;
        for (int t = 0; t < 4; ++t) {
            cf z(0, 0);
            for (int n = 0; n < kNss4; ++n) { const int idx = i1 + n; if (idx < 0 || idx >= int(cb.size())) continue; const double ph = -kTwoPi * t * n / kNss4; z += cb[size_t(idx)] * cf(float(std::cos(ph)), float(std::sin(ph))); }
            csl[t][k] = z; s4[t][k] = std::abs(z);
        }
    }
    for (int g = 0; g < 4; ++g) for (int k = 0; k < 4; ++k) { const int sym = k + 33 * g; int ip = 0; for (int t = 1; t < 4; ++t) if (s4[t][sym] > s4[ip][sym]) ip = t; if (ip == kFT4_Costas_pattern[g][k]) ++r.nsync; }
    if (r.nsync < 8) return r;                                                                        // badsync
    // bit metrics for 1, 2 and 4 symbols at a time: the best tone sequence with the bit set minus the best with it clear (206 bits = 103 symbols x 2)
    static thread_local float bm[3][2 * kNn4]; for (auto& a : bm) std::fill(a, a + 2 * kNn4, 0.0f);
    float s2[256];
    for (int nseq = 0; nseq < 3; ++nseq) {
        const int nsym = nseq == 0 ? 1 : nseq == 1 ? 2 : 4, nt = 1 << (2 * nsym), ibmax = 2 * nsym - 1;
        for (int ks = 0; ks + nsym <= kNn4; ks += nsym) {
            for (int i = 0; i < nt; ++i) {
                const int i1 = i / 64, i2 = (i & 63) / 16, i3 = (i & 15) / 4, i4 = i & 3;
                auto C = [&](int t, int sym) { return csl[kFT4_Gray_map[t]][sym]; };
                s2[i] = nsym == 1 ? std::abs(C(i4, ks)) : nsym == 2 ? std::abs(C(i3, ks) + C(i4, ks + 1)) : std::abs(C(i1, ks) + C(i2, ks + 1) + C(i3, ks + 2) + C(i4, ks + 3));
            }
            const int ipt = 2 * ks;                                                                   // 0-based bit index
            for (int ib = 0; ib <= ibmax; ++ib) {
                if (ipt + ib >= 2 * kNn4) continue;
                const int bit = ibmax - ib; float m1 = -1e30f, m0 = -1e30f;
                for (int i = 0; i < nt; ++i) { if ((i >> bit) & 1) m1 = std::max(m1, s2[i]); else m0 = std::max(m0, s2[i]); }
                bm[nseq][ipt + ib] = m1 - m0;
            }
        }
    }
    bm[1][204] = bm[0][204]; bm[1][205] = bm[0][205];                                                 // the tail no 2- or 4-symbol group covers (WSJT-X copies the shorter metric)
    for (int i = 200; i < 204; ++i) bm[2][i] = bm[1][i]; bm[2][204] = bm[0][204]; bm[2][205] = bm[0][205];
    for (auto& a : bm) normalize(a, 2 * kNn4);
    // sync check on the hard bits of the four Costas arrays (8 bits each): at least 20 of 32 right
    static const int kSyncBits[4][8] = {{0,0,0,1,1,0,1,1}, {0,1,0,0,1,1,1,0}, {1,1,1,0,0,1,0,0}, {1,0,1,1,0,0,0,1}};
    int q = 0; for (int g = 0; g < 4; ++g) for (int b = 0; b < 8; ++b) if (int(bm[0][66 * g + b] >= 0) == kSyncBits[g][b]) ++q;
    r.sync_bits = q; if (q < 20) return r;
    for (int set = 0; set < 3; ++set) for (int i = 0; i < 58; ++i) {                                  // data bits: symbols 4..32, 37..65, 70..98 (0-based) = bits 8..65, 74..131, 140..197
        r.llr[set][i] = 2.83f * bm[set][8 + i]; r.llr[set][58 + i] = 2.83f * bm[set][74 + i]; r.llr[set][116 + i] = 2.83f * bm[set][140 + i];
    }
    r.ok = true; return r;
}
}  // namespace
}  // namespace fireside::decode::demod
