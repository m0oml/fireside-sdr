// Hybrid BP + OSD decoder for the (174,91) LDPC code: see ft8_osd.h. Reimplemented from WSJT-X lib/ft8/decode174_91.f90 and osd174_91.f90 (GPL v3).
// The code tables (generator, parity checks) are ft8_lib's (MIT), which match WSJT-X's.
#include "ft8_osd.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstring>
#include <numeric>

extern "C" {
#include <ft8/constants.h>
#include <ft8/crc.h>
}

namespace fireside::decode::osd {
namespace {
constexpr int M = N - K;                      // 83 parity checks
using Bits = std::bitset<N>;

// Systematic generator of the LDPC code alone (no CRC cascade; WSJT-X calls it with Keff = 91): row i = codeword of the unit message e_i.
const std::array<Bits, K>& generator() {
    static const std::array<Bits, K> g = [] {
        std::array<Bits, K> rows{};
        for (int i = 0; i < K; ++i) {
            rows[size_t(i)].set(size_t(i));
            for (int j = 0; j < M; ++j) if ((kFTX_LDPC_generator[j][i / 8] >> (7 - i % 8)) & 1) rows[size_t(i)].set(size_t(K + j));
        }
        return rows;
    }();
    return g;
}
float platanh(float x) { const float a = std::fabs(x); return a > 0.9999999f ? std::copysign(7.0f, x) : std::atanh(x); }   // WSJT-X clips at +-7

// Order-statistics decoding of `llr` (positive = 1). Returns the best codeword in natural order and its hard-error count; false if its CRC is bad.
bool osd(const float* llr, int ndeep, const uint8_t* apmask, uint8_t* cw_out, int& nhardmin) {
    const auto& gen = generator();
    // most reliable bits first
    std::array<int, N> idx; std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return std::fabs(llr[a]) > std::fabs(llr[b]); });
    std::array<Bits, K> g;                                                    // generator with columns in that order
    for (int r = 0; r < K; ++r) for (int c = 0; c < N; ++c) g[size_t(r)][size_t(c)] = gen[size_t(r)][size_t(idx[size_t(c)])];
    // Gaussian elimination: the most reliable independent bits become the information set (pivot search limited to k+20 columns, as WSJT-X)
    for (int id = 0; id < K; ++id) {
        for (int icol = id; icol < std::min(N, K + 20); ++icol) {
            if (!g[size_t(id)][size_t(icol)]) continue;
            if (icol != id) { for (auto& row : g) { const bool a = row[size_t(id)], b = row[size_t(icol)]; row[size_t(id)] = b; row[size_t(icol)] = a; } std::swap(idx[size_t(id)], idx[size_t(icol)]); }
            for (int ii = 0; ii < K; ++ii) if (ii != id && g[size_t(ii)][size_t(id)]) g[size_t(ii)] ^= g[size_t(id)];
            break;
        }
    }
    Bits hdec; std::array<float, N> absr{}; std::array<bool, N> apr{};
    for (int c = 0; c < N; ++c) { hdec[size_t(c)] = llr[idx[size_t(c)]] >= 0; absr[size_t(c)] = std::fabs(llr[idx[size_t(c)]]); apr[size_t(c)] = apmask && apmask[idx[size_t(c)]]; }
    Bits c0; for (int i = 0; i < K; ++i) if (hdec[size_t(i)]) c0 ^= g[size_t(i)];                         // order 0: hard decisions on the information set
    auto weighted = [&](const Bits& x, int from) { float s = 0; for (int c = from; c < N; ++c) if (x[size_t(c)]) s += absr[size_t(c)]; return s; };
    Bits best = c0; Bits nx = c0 ^ hdec; float dmin = weighted(nx, 0); nhardmin = int(nx.count());
    if (ndeep >= 1) {
        const int nt = 40, ntheta = ndeep == 1 ? 12 : 10; const bool npre1 = ndeep >= 2;
        Bits parity_mask, nt_mask; for (int c = K; c < N; ++c) parity_mask.set(size_t(c)); for (int c = K; c < K + nt; ++c) nt_mask.set(size_t(c));
        for (int iflag = K - 1; iflag >= 0; --iflag) {                             // order 1 (one information bit flipped), and with npre1 a second one, pre-screened
            if (apr[size_t(iflag)]) continue;                                   // a-priori bits are never flipped
            const Bits ce1 = c0 ^ g[size_t(iflag)];
            const Bits e2sub = (ce1 ^ hdec) & parity_mask;
            const float d1 = absr[size_t(iflag)];                                 // m0 = hard decisions, so only the flipped bit counts in the information part
            const int iend = npre1 ? 0 : iflag;
            for (int n1 = iflag; n1 >= iend; --n1) {
                if (n1 != iflag && apr[size_t(n1)]) continue;
                Bits e2 = e2sub; int nd1kpt;
                if (n1 == iflag) nd1kpt = int((e2sub & nt_mask).count()) + 1;
                else { e2 ^= g[size_t(n1)] & parity_mask; nd1kpt = int((e2 & nt_mask).count()) + 2; }
                if (nd1kpt > ntheta) continue;                                     // pre-processing rule: too many disagreements in the first 40 parity bits
                const float dd = d1 + (n1 == iflag ? 0.0f : absr[size_t(n1)]) + weighted(e2, K);
                if (dd < dmin) { dmin = dd; best = n1 == iflag ? ce1 : (ce1 ^ g[size_t(n1)]); nhardmin = int((best ^ hdec).count()); }
            }
        }
    }
    for (int c = 0; c < N; ++c) cw_out[idx[size_t(c)]] = best[size_t(c)] ? 1 : 0;   // back to natural order
    return crc_ok(cw_out);
}
}  // namespace

bool crc_ok(const uint8_t* b) {
    uint8_t a91[12] = {0}; for (int i = 0; i < K; ++i) if (b[i]) a91[i / 8] |= uint8_t(0x80u >> (i % 8));
    const uint16_t got = ftx_extract_crc(a91); a91[9] &= 0xF8; a91[10] = 0; a91[11] = 0;
    return ftx_compute_crc(a91, 96 - 14) == got;
}

Result decode(const float* llr, int max_osd, int norder, const uint8_t* apmask) {
    Result r; max_osd = std::clamp(max_osd, 0, 3);
    static thread_local float tov[N][3], toc[M][7], tanhtoc[M][7]; float zn[N], zsum[N] = {0}, zsave[3][N];
    std::memset(tov, 0, sizeof tov);
    for (int j = 0; j < M; ++j) for (int i = 0; i < kFTX_LDPC_Num_rows[j]; ++i) toc[j][i] = llr[kFTX_LDPC_Nm[j][i] - 1];
    int ncnt = 0, nclast = 0;
    for (int iter = 0; iter <= 30; ++iter) {
        for (int i = 0; i < N; ++i) { zn[i] = llr[i] + ((apmask && apmask[i]) ? 0.0f : tov[i][0] + tov[i][1] + tov[i][2]); zsum[i] += zn[i]; }
        if (iter > 0 && iter <= max_osd) std::memcpy(zsave[iter - 1], zsum, sizeof zsum);
        uint8_t cw[N]; for (int i = 0; i < N; ++i) cw[i] = zn[i] > 0 ? 1 : 0;
        int ncheck = 0;
        for (int j = 0; j < M; ++j) { int s = 0; for (int i = 0; i < kFTX_LDPC_Num_rows[j]; ++i) s += cw[kFTX_LDPC_Nm[j][i] - 1]; ncheck += s & 1; }
        if (ncheck == 0 && crc_ok(cw)) {
            r.ok = true; std::memcpy(r.bits, cw, N);
            r.hard_errors = 0; for (int i = 0; i < N; ++i) { if ((cw[i] ? 1.0f : -1.0f) * llr[i] < 0) { ++r.hard_errors; r.dmin += std::fabs(llr[i]); } }
            return r;
        }
        if (iter > 0) { if (ncheck - nclast < 0) ncnt = 0; else ++ncnt; if (ncnt >= 5 && iter >= 10 && ncheck > 15) break; }   // WSJT-X's early stop
        nclast = ncheck;
        for (int j = 0; j < M; ++j) for (int i = 0; i < kFTX_LDPC_Num_rows[j]; ++i) {               // bits -> checks (leaving out what the check sent)
            const int b = kFTX_LDPC_Nm[j][i] - 1; float v = zn[b];
            for (int kk = 0; kk < 3; ++kk) if (kFTX_LDPC_Mn[b][kk] - 1 == j) v -= tov[b][kk];
            toc[j][i] = v; tanhtoc[j][i] = std::tanh(-v / 2);
        }
        for (int b = 0; b < N; ++b) for (int i = 0; i < 3; ++i) {                                     // checks -> bits
            const int ch = kFTX_LDPC_Mn[b][i] - 1; float t = 1;
            for (int i2 = 0; i2 < kFTX_LDPC_Num_rows[ch]; ++i2) if (kFTX_LDPC_Nm[ch][i2] - 1 != b) t *= tanhtoc[ch][i2];
            tov[b][i] = 2 * platanh(-t);
        }
    }
    for (int s = 0; s < max_osd; ++s) {                                                                   // BP failed: OSD on BP's accumulated beliefs
        uint8_t cw[N]; int nhard = 0;
        if (osd(zsave[s], norder, apmask, cw, nhard) && nhard > 0) {
            r.ok = true; r.by_osd = true; r.hard_errors = nhard; std::memcpy(r.bits, cw, N);
            for (int i = 0; i < N; ++i) if ((cw[i] ? 1.0f : -1.0f) * llr[i] < 0) r.dmin += std::fabs(llr[i]);
            return r;
        }
    }
    return r;
}
}  // namespace fireside::decode::osd
