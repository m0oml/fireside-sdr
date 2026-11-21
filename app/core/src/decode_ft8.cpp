#include <complex>
#include <chrono>
// FT8 decoding on top of ft8_lib (MIT, Karlis Goba): sync search, LDPC(174,91), CRC and message unpacking come from the library; this file does slot timing,
// 48 -> 12 kHz resampling, the callsign hash table, background decoding and test-signal synthesis (the GFSK waveform follows the library's gen_ft8 demo).
#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>
#include "fireside/decode.h"
#include "fireside/dsp.h"
#include "fireside/dxcc.h"
#include "ft8_demod.h"
#include "ft8_osd.h"

extern "C" {
#include <common/monitor.h>
#include <ft8/constants.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <ft8/message.h>
#include <ft8/crc.h>
#include <ft8/ldpc.h>
}

namespace fireside::decode {
namespace {
// Tuned 2026-11-18 with tools/ft8bench.py against jt9 on two live 10 min 40 m captures (1832 jt9 decodes): time oversampling 4 (was 2) +2.3 %,
// 50 LDPC iterations (was 25) +0.4 %, envelope filter 3000 samples +1.3 %; frequency oversampling 4 LOST 20 % (kept at 2); more candidates or a
// lower sync threshold gave < 0.3 % for 13 % more time; a 4th pass found nothing the 3rd did not.
constexpr int kRate = 12000, kMaxMsgs = 120, kMaxCand = 140, kMinScore = 10, kLdpcIter = 50, kTimeOsr = 4, kFreqOsr = 2;
// OSD fallback (WSJT-X deep decoding): BP sums after this many iterations are each tried (0 = off), at this depth.
constexpr bool kDeep = true;                // WSJT-X's per-candidate deep stage (fine sync, 5 soft-bit sets, a-priori passes) after the fallbacks above
constexpr int kOsdSums = 2, kOsdDepth = 2;   // on: +6.9 % of jt9's decodes on two live captures, 0 false in 1000 noise slots (analysis/ft8-vs-wsjtx-2026-11-18.md)

// ft8_lib wants callsign hashing through plain function pointers: a thread-local table pointer carries the context
struct HashTable {
    struct Entry { char call[12] = {0}; uint32_t hash = 0; };
    Entry e[256]; int size = 0;
    void add(const char* c, uint32_t hash) {
        int i = int(((hash >> 12) & 0x3FFu) * 23u) % 256;
        while (e[i].call[0]) { if ((e[i].hash & 0x3FFFFFu) == hash && !std::strcmp(e[i].call, c)) { e[i].hash &= 0x3FFFFFu; return; } i = (i + 1) % 256; }
        ++size; std::strncpy(e[i].call, c, 11); e[i].call[11] = 0; e[i].hash = hash;
    }
    bool lookup(ftx_callsign_hash_type_t t, uint32_t hash, char* out) const {
        const unsigned shift = t == FTX_CALLSIGN_HASH_10_BITS ? 12u : (t == FTX_CALLSIGN_HASH_12_BITS ? 10u : 0u);
        int i = int(((hash >> (12 - shift)) & 0x3FFu) * 23u) % 256;
        while (e[i].call[0]) { if (((e[i].hash & 0x3FFFFFu) >> shift) == hash) { std::strcpy(out, e[i].call); return true; } i = (i + 1) % 256; }
        out[0] = 0; return false;
    }
    void age(uint8_t max_age) {
        for (auto& x : e) if (x.call[0]) { const uint8_t a = uint8_t(x.hash >> 24); if (a > max_age) { x.call[0] = 0; x.hash = 0; --size; } else x.hash = ((uint32_t(a) + 1u) << 24) | (x.hash & 0x3FFFFFu); }
    }
};
thread_local HashTable* g_tab = nullptr;
bool hash_lookup(ftx_callsign_hash_type_t t, uint32_t h, char* out) { return g_tab ? g_tab->lookup(t, h, out) : (out[0] = 0, false); }
void hash_save(const char* c, uint32_t h) { if (g_tab) g_tab->add(c, h); }

// SNR in WSJT-X's convention (signal power / noise power in 2500 Hz). The decoded message gives the 79 tones that were sent, so the signal power is the mean of the
// magnitudes at exactly those bins; the noise power comes from empty bins 3..10 bins (19..62 Hz) either side. Calibrated against WSJT-X's reports on real recordings.
// Recalibrated 2026-11-17 against WSJT-X 3.3.0-beta5 (jt9) on a live 10 min capture (497 matched messages): our figure was -5.6 dB low (flat from -14 to +8 dB, slope 0.95, sd 2.8).
// The old value (-4.0) was tuned to 2019 WSJT-X reports on 4 quiet-band recordings; jt9 3.3 reads +2.7 dB above those on the same files, and our noise estimate (neighbouring bins)
// is also pushed up on a crowded band, so the two sets disagree by about 3 dB. The constant follows the busy band, which is the real-world case. tools/compare_ft8.py repeats the measurement.
constexpr double kSnrCalibrationDb = 1.7;
// FT4, 2026-11-18, against jt9 (WSJT-X 3.3.0-beta5 -5): ours read -2.1 dB low on WSJT-X's own contest sample (13 matched messages) and -2.8 dB on a live 3 min 40 m
// capture (10 messages, quiet band); pooled -2.4. Thin data (23 messages): repeat with a busy FT4 capture.
constexpr double kFt4SnrCalibrationDb = 2.4;
int estimate_snr(const ftx_waterfall_t* wf, const ftx_candidate_t* c, const uint8_t* tones, bool ft4) {
    const int nn = ft4 ? FT4_NN : FT8_NN; const double bin_hz = ft4 ? 1.0 / FT4_SYMBOL_PERIOD : 1.0 / FT8_SYMBOL_PERIOD;   
    static const int kFt8Noise[] = {-10, -9, -8, -7, -6, -5, -4, -3, 11, 12, 13, 14, 15, 16, 17, 18}, kFt4Noise[] = {-8, -7, -6, -5, -4, -3, 7, 8, 9, 10, 11, 12};
    double sig = 0, noise_sum = 0; int ns = 0; std::vector<double> nz;
    for (int s = 0; s < nn; ++s) {
        const int block = c->time_offset + s; if (block < 0 || block >= wf->num_blocks) continue;
        const WF_ELEM_T* p = wf->mag + (((size_t(block) * wf->time_osr + c->time_sub) * wf->freq_osr + c->freq_sub) * wf->num_bins + size_t(c->freq_offset));
        sig += std::pow(10.0, WF_ELEM_MAG(p[tones[s]]) / 10.0); ++ns;
        for (int k : ft4 ? std::vector<int>(std::begin(kFt4Noise), std::end(kFt4Noise)) : std::vector<int>(std::begin(kFt8Noise), std::end(kFt8Noise))) { const int bin = c->freq_offset + k; if (bin >= 0 && bin < wf->num_bins) nz.push_back(std::pow(10.0, WF_ELEM_MAG(p[k]) / 10.0)); }
    }
    if (!ns || nz.empty()) return -24;
    std::nth_element(nz.begin(), nz.begin() + long(nz.size() / 2), nz.end()); noise_sum = nz[nz.size() / 2] / 0.693;   // median of an exponential distribution = ln2 x mean
    const double s_over_n = std::max(sig / ns - noise_sum, 1e-3 * noise_sum) / noise_sum;                              // signal-only power over noise power in one 6.25 Hz bin
    const double snr = 10.0 * std::log10(s_over_n) - 10.0 * std::log10(2500.0 / bin_hz) + (ft4 ? kFt4SnrCalibrationDb : kSnrCalibrationDb);
    return int(std::lround(std::clamp(snr, -24.0, 30.0)));
}

// ---------------------------------------------------------------- soft bits for the OSD fallback
// ft8_lib keeps its likelihood extraction private; these few lines are copied from its decode.c (MIT, Karlis Goba) so the vendored library stays unchanged.
const WF_ELEM_T* cand_mag(const ftx_waterfall_t* wf, const ftx_candidate_t* c) {
    int o = c->time_offset; o = o * wf->time_osr + c->time_sub; o = o * wf->freq_osr + c->freq_sub; o = o * wf->num_bins + c->freq_offset; return wf->mag + o;
}
float mx2(float a, float b) { return a >= b ? a : b; }
float mx4(float a, float b, float c, float d) { return mx2(mx2(a, b), mx2(c, d)); }
void candidate_llr(const ftx_waterfall_t* wf, const ftx_candidate_t* c, bool ft4, float* llr) {
    const WF_ELEM_T* mag = cand_mag(wf, c);
    if (ft4) for (int k = 0; k < FT4_ND; ++k) {
        const int sym = k + (k < 29 ? 5 : k < 58 ? 9 : 13), b = 2 * k, block = c->time_offset + sym;
        if (block < 0 || block >= wf->num_blocks) { llr[b] = llr[b + 1] = 0; continue; }
        const WF_ELEM_T* w = mag + sym * wf->block_stride; float s2[4]; for (int j = 0; j < 4; ++j) s2[j] = WF_ELEM_MAG(w[kFT4_Gray_map[j]]);
        llr[b] = mx2(s2[2], s2[3]) - mx2(s2[0], s2[1]); llr[b + 1] = mx2(s2[1], s2[3]) - mx2(s2[0], s2[2]);
    } else for (int k = 0; k < FT8_ND; ++k) {
        const int sym = k + (k < 29 ? 7 : 14), b = 3 * k, block = c->time_offset + sym;
        if (block < 0 || block >= wf->num_blocks) { llr[b] = llr[b + 1] = llr[b + 2] = 0; continue; }
        const WF_ELEM_T* w = mag + sym * wf->block_stride; float s2[8]; for (int j = 0; j < 8; ++j) s2[j] = WF_ELEM_MAG(w[kFT8_Gray_map[j]]);
        llr[b] = mx4(s2[4], s2[5], s2[6], s2[7]) - mx4(s2[0], s2[1], s2[2], s2[3]); llr[b + 1] = mx4(s2[2], s2[3], s2[6], s2[7]) - mx4(s2[0], s2[1], s2[4], s2[5]);
        llr[b + 2] = mx4(s2[1], s2[3], s2[5], s2[7]) - mx4(s2[0], s2[2], s2[4], s2[6]);
    }
    float sum = 0, sum2 = 0; for (int i = 0; i < FTX_LDPC_N; ++i) { sum += llr[i]; sum2 += llr[i] * llr[i]; }                  // ft8_lib's normalisation (variance 24)
    const float var = (sum2 - sum * sum / FTX_LDPC_N) / FTX_LDPC_N; if (var <= 0) return; const float nf = std::sqrt(24.0f / var); for (int i = 0; i < FTX_LDPC_N; ++i) llr[i] *= nf;
}
// A BP/OSD result -> ft8_lib message (payload, hash); false for the all-zero word or an undefined message type (WSJT-X rules).
bool pack_result(const osd::Result& r, bool ft4, ftx_message_t* msg) {
    if (std::all_of(r.bits, r.bits + FTX_LDPC_N, [](uint8_t b) { return b == 0; })) return false;
    uint8_t a91[FTX_LDPC_K_BYTES] = {0}; for (int i = 0; i < FTX_LDPC_K; ++i) if (r.bits[i]) a91[i / 8] |= uint8_t(0x80u >> (i % 8));
    a91[9] &= 0xF8; a91[10] = 0; msg->hash = ftx_compute_crc(a91, 96 - 14);
    for (int i = 0; i < 10; ++i) msg->payload[i] = ft4 ? uint8_t(a91[i] ^ kFT4_XOR_sequence[i]) : a91[i];
    const unsigned i3 = msg->payload[9] >> 3 & 7u, n3 = (msg->payload[8] & 1u) << 2 | msg->payload[9] >> 6;   // bits 74..76 and 71..73
    return !(i3 > 5 || (i3 == 0 && (n3 > 6 || n3 == 2)));
}
thread_local int g_fb_hard = 0; thread_local float g_fb_dmin = 0; thread_local bool g_fb_osd = false; thread_local int g_fb_stage = 0; thread_local int g_fb_nsync = 0;   // for FT8_DEBUG_OSD
// When ft8_lib's belief propagation fails: WSJT-X's BP (keeping its sums) and then OSD on ft8_lib's soft bits. Rules (ft8b.f90): at most 36 hard errors, not the
// all-zero word, a defined message type; and (checked after unpacking) no "/R" or "TU;" outside contest use.
bool decode_fallback(const ftx_waterfall_t* wf, const ftx_candidate_t* c, bool ft4, ftx_message_t* msg) {
    float llr[FTX_LDPC_N]; candidate_llr(wf, c, ft4, llr);
    const osd::Result r = osd::decode(llr, kOsdSums, kOsdDepth);
    g_fb_hard = r.hard_errors; g_fb_dmin = r.dmin; g_fb_osd = r.by_osd; g_fb_stage = 1;
    if (!r.ok || r.hard_errors > 36) return false;
    // Our extra rule (measured 2026-11-18 on 2 live captures, 165 fallback decodes): genuine ones with >= 26 hard errors all had dmin <= 52.7, genuine ones with dmin >= 60 all had
    // <= 20 hard errors; the one false decode had both (29, 66.1). A decode needing many flips AND fitting the channel badly is refused. Revisit with more data.
    if (r.hard_errors >= 26 && r.dmin >= 60.0f) return false;
    return pack_result(r, ft4, msg);
}
// A-priori bits (WSJT-X "a1" CQ, "a2" your call): the first 29 message bits (call 1 + its /R bit) and i3 = 1 (standard message).
struct ApBits { bool cq = true; bool mine = false; int8_t mycall[29] = {}; };
ApBits make_ap(const std::string& my_call) {
    ApBits a; if (my_call.empty()) return a;
    ftx_message_t m; if (ftx_message_encode(&m, nullptr, (my_call + " K1ABC RR73").c_str()) != FTX_MESSAGE_RC_OK || ftx_message_get_type(&m) != FTX_MESSAGE_TYPE_STANDARD) return a;   // non-standard calls: no a2 (as WSJT-X)
    for (int i = 0; i < 29; ++i) a.mycall[i] = ((m.payload[i / 8] >> (7 - i % 8)) & 1) ? 1 : -1;
    a.mine = true; return a;
}
// WSJT-X's deep stage for FT8 (ft8b.f90): refine the candidate at 200 Hz, five soft-bit sets with BP+OSD, then the a-priori passes (CQ, your call) on sets 1 and 3.
bool decode_deep_one(const demod::Soft& s, const ApBits& ap, ftx_message_t* msg, int* ap_type, bool ft4);
// WSJT-X's deep stage. FT8 (ft8b.f90): one refined demodulation around the candidate. FT4 (ft4_decode.f90): up to three, one per time segment of the full DT search.
bool decode_deep(const demod::SlotSpectrum& spec, double f0, double t_s, const ApBits& ap, ftx_message_t* msg, int* ap_type, bool ft4, double* f_ref, double* t_ref) {
    static const bool dbg = std::getenv("FT8_DEBUG_DEEP") != nullptr;                         // FT8_DEBUG_DEEP=1: every candidate the deep stage looks at
    std::vector<demod::Soft> v; if (ft4) v = demod::demodulate_ft4(spec, f0); else v.push_back(demod::demodulate(spec, f0, t_s));
    for (const auto& s : v) {
        if (dbg) std::fprintf(stderr, "DEEP f=%.1f t=%.3f -> ok=%d f=%.1f t=%.3f nsync=%d sync=%.2f syncbits=%d\n", f0, t_s, int(s.ok), s.f0, s.t0, s.nsync, s.sync_power, s.sync_bits);
        if (s.ok && decode_deep_one(s, ap, msg, ap_type, ft4)) { *f_ref = s.f0; *t_ref = s.t0; return true; }   // the refined time and frequency: for DT and subtraction
    }
    return false;
}
bool decode_deep_one(const demod::Soft& s, const ApBits& ap, ftx_message_t* msg, int* ap_type, bool ft4) {
    g_fb_nsync = s.nsync;
    auto accept = [&](const osd::Result& r, int stage) {
        g_fb_hard = r.hard_errors; g_fb_dmin = r.dmin; g_fb_osd = r.by_osd; g_fb_stage = stage;
        return r.ok && r.hard_errors <= 36 && pack_result(r, ft4, msg);
    };
    for (int set = 0; set < (ft4 ? 3 : 5); ++set) { if (accept(osd::decode(s.llr[set], 2, 2), 10 + set)) { *ap_type = 0; return true; } }
    static const int8_t kCq[29] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,1,-1,-1};   // n28 = 2 ("CQ"), /R bit 0
    for (int type = 1; type <= 2; ++type) {
        if ((type == 1 && !ap.cq) || (type == 2 && !ap.mine)) continue;
        const int8_t* bits = type == 1 ? kCq : ap.mycall;
        for (int set : {0, 2}) {
            if (ft4 && set != 2) continue;                                                       // FT4: the 4-symbol set only
            float l[174]; std::copy_n(s.llr[set], 174, l); uint8_t mask[174] = {0};
            float apmag = 0; for (float v : l) apmag = std::max(apmag, std::fabs(v)); if (ft4) { apmag = 0; for (int i = 0; i < 174; ++i) apmag = std::max(apmag, std::fabs(s.llr[0][i])); } apmag *= 1.1f;   // FT4: from the 1-symbol set (WSJT-X: maxval(abs(llra)))
            for (int i = 0; i < 29; ++i) { mask[i] = 1; const bool flip = ft4 && ((kFT4_XOR_sequence[i / 8] >> (7 - i % 8)) & 1); l[i] = apmag * (flip ? -bits[i] : bits[i]); }
            if (!ft4) { mask[74] = mask[75] = mask[76] = 1; l[74] = -apmag; l[75] = -apmag; l[76] = apmag; }    // FT8: i3 = 1 as well (WSJT-X FT4 fixes only the 29 bits)
            if (accept(osd::decode(l, 2, 2, mask), 20 + type * 2 + (set == 2))) { *ap_type = type; return true; }
        }
    }
    return false;
}

// ---------------------------------------------------------------- message types ft8_lib does not unpack (written from WSJT-X 3.x lib/77bit/packjt77.f90)
// ft8_lib unpacks only standard, non-standard-call, free-text and telemetry messages; the rest passed the CRC and were silently dropped. Added here, bit-exact with WSJT-X
// (every section and multiplier is checked against `ft8code` in the tests):
//   i3 = 3        ARRL RTTY Roundup (FT4 and FT8 contests): TU(1) n28a(28) n28b(28) R(1) report(3) exchange(13). Report 529 + 10 x value; exchange 8001..8171 = multiplier
//                 (50 US states, Canadian provinces, DC, Dutch provinces, X01..X99 DX codes), 1..7999 = serial number.
//   i3 = 0, n3 = 3 / 4   ARRL Field Day: n28a(28) n28b(28) R(1) transmitters-1(4) class(3) section(7); n3 = 4 adds 16 transmitters. 86 sections.
//   i3 = 0, n3 = 1       DXpedition: "K1ABC RR73; W9XYZ <KH1/KH7Z> -12": n28a n28b hash10(10) report(5); the third call is a 10-bit hash ("<...>" until its call has been heard).
//   i3 = 5        EU VHF contest: "<PA3XYZ> <G4ABC/P> R 590003 IO91NP": hash12(12) hash22(22) R(1) report(3) serial(11) grid6(25); both calls are hashes.
// The two full callsigns of the first three types are packed exactly as in a standard message, so a standard payload is built from the same bits and ft8_lib decodes them
// (and keeps its hash table).
const char* const kSections[86] = {
    "AB","AK","AL","AR","AZ","BC","CO","CT","DE","EB","EMA","ENY","EPA","EWA","GA","GH","IA","ID","IL","IN","KS","KY","LA","LAX","NS","MB","MDC","ME",
    "MI","MN","MO","MS","MT","NC","ND","NE","NFL","NH","NL","NLI","NM","NNJ","NNY","TER","NTX","NV","OH","OK","ONE","ONN","ONS","OR","ORG","PAC","PR",
    "QC","RI","SB","SC","SCV","SD","SDG","SF","SFL","SJV","SK","SNJ","STX","SV","TN","UT","VA","VI","VT","WCF","WI","WMA","WNY","WPA","WTX","WV",
    "WWA","WY","DX","PE","NB"};
const char* const kMultipliers[171] = {
    "AL","AK","AZ","AR","CA","CO","CT","DE","FL","GA","HI","ID","IL","IN","IA","KS","KY","LA","ME","MD","MA","MI","MN","MS","MO","MT","NE","NV","NH",
    "NJ","NM","NY","NC","ND","OH","OK","OR","PA","RI","SC","SD","TN","TX","UT","VT","VA","WA","WV","WI","WY","NB","NS","QC","ON","MB","SK","AB","BC",
    "NWT","NF","LB","NU","YT","PEI","DC","DR","FR","GD","GR","OV","ZH","ZL","X01","X02","X03","X04","X05","X06","X07","X08","X09","X10","X11","X12",
    "X13","X14","X15","X16","X17","X18","X19","X20","X21","X22","X23","X24","X25","X26","X27","X28","X29","X30","X31","X32","X33","X34","X35","X36",
    "X37","X38","X39","X40","X41","X42","X43","X44","X45","X46","X47","X48","X49","X50","X51","X52","X53","X54","X55","X56","X57","X58","X59","X60",
    "X61","X62","X63","X64","X65","X66","X67","X68","X69","X70","X71","X72","X73","X74","X75","X76","X77","X78","X79","X80","X81","X82","X83","X84",
    "X85","X86","X87","X88","X89","X90","X91","X92","X93","X94","X95","X96","X97","X98","X99"};
unsigned get_bits(const uint8_t* p, int from, int n) { unsigned v = 0; for (int i = 0; i < n; ++i) v = (v << 1) | ((p[(from + i) / 8] >> (7 - (from + i) % 8)) & 1u); return v; }
void put_bits(uint8_t* p, int from, int n, unsigned v) { for (int i = 0; i < n; ++i) { const int b = from + i; const unsigned bit = (v >> (n - 1 - i)) & 1u; p[b / 8] = uint8_t((p[b / 8] & ~(0x80u >> (b % 8))) | (bit << (7 - b % 8))); } }
// The two callsigns packed as n28a (bits a..a+27) and n28b (bits b..b+27): both must be real callsigns (n28 > 2), as in WSJT-X.
bool two_calls(const uint8_t* payload, int a, int b, ftx_callsign_hash_interface_t* hif, std::string& to, std::string& de) {
    const unsigned n28a = get_bits(payload, a, 28), n28b = get_bits(payload, b, 28); if (n28a <= 2 || n28b <= 2) return false;
    ftx_message_t m; std::memset(&m, 0, sizeof m); put_bits(m.payload, 0, 28, n28a); put_bits(m.payload, 29, 28, n28b); put_bits(m.payload, 74, 3, 1);   // ip bits 0, i3 = 1 (standard)
    char t[14] = {0}, d[14] = {0}, x[14] = {0}; ftx_field_t types[FTX_MAX_MESSAGE_FIELDS];
    if (ftx_message_decode_std(&m, hif, t, d, x, types) != FTX_MESSAGE_RC_OK) return false;
    to = t; de = d; return true;
}
std::string hashed(ftx_callsign_hash_interface_t* hif, ftx_callsign_hash_type_t type, unsigned h) { char c[16] = {0}; return hif->lookup_hash(type, h, c) ? std::string("<") + c + ">" : std::string("<...>"); }
bool decode_extra_type(const uint8_t* payload, ftx_callsign_hash_interface_t* hif, std::string& text) {
    const unsigned i3 = get_bits(payload, 74, 3), n3 = get_bits(payload, 71, 3); std::string c1, c2; char b[24];
    if (i3 == 3) {
        const unsigned tu = get_bits(payload, 0, 1), ir = get_bits(payload, 57, 1), irpt = get_bits(payload, 58, 3), nexch = get_bits(payload, 61, 13); std::string exch;
        if (nexch >= 8001 && nexch <= 8171) exch = kMultipliers[nexch - 8001];
        else if (nexch >= 1 && nexch <= 7999) { std::snprintf(b, sizeof b, "%04u", nexch); exch = b; }
        else return false;
        if (!two_calls(payload, 1, 29, hif, c1, c2)) return false;
        std::snprintf(b, sizeof b, "%u", 529u + 10u * irpt);
        text = std::string(tu ? "TU; " : "") + c1 + " " + c2 + " " + (ir ? "R " : "") + b + " " + exch; return true;
    }
    if (i3 == 0 && (n3 == 3 || n3 == 4)) {
        const unsigned ir = get_bits(payload, 56, 1), intx = get_bits(payload, 57, 4), nclass = get_bits(payload, 61, 3), isec = get_bits(payload, 64, 7);
        if (isec < 1 || isec > 86 || !two_calls(payload, 0, 28, hif, c1, c2)) return false;
        std::snprintf(b, sizeof b, "%u%c", intx + 1 + (n3 == 4 ? 16u : 0u), char('A' + nclass));
        text = c1 + " " + c2 + (ir ? " R " : " ") + b + " " + kSections[isec - 1]; return true;
    }
    if (i3 == 0 && n3 == 1) {
        const unsigned n10 = get_bits(payload, 56, 10), n5 = get_bits(payload, 66, 5); if (!two_calls(payload, 0, 28, hif, c1, c2)) return false;
        std::snprintf(b, sizeof b, "%+03d", 2 * int(n5) - 30);
        text = c1 + " RR73; " + c2 + " " + hashed(hif, FTX_CALLSIGN_HASH_10_BITS, n10) + " " + b; return true;
    }
    if (i3 == 5) {
        const unsigned n12 = get_bits(payload, 0, 12), n22 = get_bits(payload, 12, 22), ir = get_bits(payload, 34, 1), irpt = get_bits(payload, 35, 3), serial = get_bits(payload, 38, 11), g = get_bits(payload, 49, 25);
        if (g > 18662399u) return false;
        char grid[7]; unsigned n = g; const unsigned j1 = n / (18 * 10 * 10 * 24 * 24); n -= j1 * 18 * 10 * 10 * 24 * 24; const unsigned j2 = n / (10 * 10 * 24 * 24); n -= j2 * 10 * 10 * 24 * 24;
        const unsigned j3 = n / (10 * 24 * 24); n -= j3 * 10 * 24 * 24; const unsigned j4 = n / (24 * 24); n -= j4 * 24 * 24; const unsigned j5 = n / 24, j6 = n - j5 * 24;
        if (j1 > 17 || j2 > 17 || j3 > 9 || j4 > 9) return false;
        std::snprintf(grid, sizeof grid, "%c%c%u%u%c%c", char('A' + j1), char('A' + j2), j3, j4, char('A' + j5), char('A' + j6));
        std::snprintf(b, sizeof b, "%u%04u", 52u + irpt, serial);
        text = hashed(hif, FTX_CALLSIGN_HASH_12_BITS, n12) + " " + hashed(hif, FTX_CALLSIGN_HASH_22_BITS, n22) + (ir ? " R " : " ") + b + " " + grid; return true;
    }
    return false;
}

// ---------------------------------------------------------------- plausibility (WSJT-X unpack77 / unpack28 / callok, packjt77.f90)
// ft8_lib unpacks some payloads WSJT-X refuses. A CRC-valid word from a weak or deep decode is checked as WSJT-X does before it is shown.
bool callok(const std::string& w) {                                       // WSJT-X callok: a plausible standard callsign
    const int n = int(w.size()); if (n < 3 || w[0] == 'Q') return false;
    int i0 = -1; for (int i = n - 1; i >= 0; --i) if (std::isdigit(static_cast<unsigned char>(w[size_t(i)]))) { i0 = i; break; }
    if (i0 != 1 && i0 != 2) return false;                                 // the call-area digit is the 2nd or 3rd character
    bool letter = false; for (int i = 0; i < i0; ++i) if (std::isalpha(static_cast<unsigned char>(w[size_t(i)]))) letter = true;
    if (!letter) return false;                                            // the prefix has a letter
    for (int i = i0 + 1; i < n; ++i) if (!std::isalpha(static_cast<unsigned char>(w[size_t(i)]))) return false;   // the suffix is all letters
    return true;
}
constexpr unsigned kNtokens = 2063592, kMax22 = 4194304, kMaxGrid4 = 32400;
bool n28_ok(unsigned n28) {                                               // a callsign field: tokens and hashes pass; a full callsign must pass callok
    if (n28 < kNtokens + kMax22) return true;
    unsigned n = n28 - kNtokens - kMax22; static const char c1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", c2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", c3[] = "0123456789", c4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const unsigned i1 = n / (36 * 10 * 27 * 27 * 27); n -= 36 * 10 * 27 * 27 * 27 * i1; const unsigned i2 = n / (10 * 27 * 27 * 27); n -= 10 * 27 * 27 * 27 * i2;
    const unsigned i3 = n / (27 * 27 * 27); n -= 27 * 27 * 27 * i3; const unsigned i4 = n / (27 * 27); n -= 27 * 27 * i4; const unsigned i5 = n / 27, i6 = n - 27 * i5;
    if (i1 > 36 || i2 > 35 || i3 > 9 || i4 > 26 || i5 > 26 || i6 > 26) return false;
    std::string c; for (char ch : {c1[i1], c2[i2], c3[i3], c4[i4], c4[i5], c4[i6]}) c += ch;
    const size_t a = c.find_first_not_of(' '); if (a == std::string::npos) return false; c = c.substr(a); while (!c.empty() && c.back() == ' ') c.pop_back();
    if (c.find(' ') != std::string::npos) return false;                   // a space inside the call
    return callok(c);
}
bool is_cq_token(unsigned n28) { return n28 >= 2 && n28 <= 532443; }      // CQ, CQ nnn, CQ xxxx
// The payload rules WSJT-X applies while unpacking: callsigns plausible, no "CQ ... R grid", no "CQ ... RRR/RR73/73/report", no "CQ <hash>".
bool payload_plausible(const uint8_t* p) {
    const unsigned i3 = get_bits(p, 74, 3), n3 = get_bits(p, 71, 3);
    if (i3 == 1 || i3 == 2) {
        const unsigned a = get_bits(p, 0, 28), b = get_bits(p, 29, 28), ir = get_bits(p, 58, 1), g = get_bits(p, 59, 15);   // bit 28 / 57 are the /R or /P flags of the two calls; ir = bit 58, the grid or report = bits 59..73 (as ft8_lib's decode_std)
        if (!n28_ok(a) || !n28_ok(b)) return false;
        if (is_cq_token(a)) { if (g <= kMaxGrid4 ? ir == 1 : g - kMaxGrid4 >= 2) return false; if (b >= kNtokens && b < kNtokens + kMax22) return false; }
        return true;
    }
    if (i3 == 3) return n28_ok(get_bits(p, 1, 28)) && n28_ok(get_bits(p, 29, 28));
    if (i3 == 0 && (n3 == 1 || n3 == 3 || n3 == 4)) return n28_ok(get_bits(p, 0, 28)) && n28_ok(get_bits(p, 28, 28));
    return true;
}

// ---------------------------------------------------------------- location sanity (weak decodes only)
// A false decode has a random locator, so it is usually impossible for its callsign's country (a Lithuanian call in the Sahara). The sender is the last callsign before the
// locator ("CQ LY4PRC KO24", "G4ABC LY4PRC R KO24"). "RR73" looks like a locator but is a word. Checked against cty.dat (dxcc.h). Real decodes the BP stage finds are not
// filtered: a ham can operate anywhere. Measured on 1190 WSJT-X decodes with a callsign and locator: 0 genuine ones flagged.
bool location_plausible(const char* text) {
    std::vector<std::string> t; { std::string cur; for (const char* c = text;; ++c) { if (*c == ' ' || *c == 0) { if (!cur.empty() && cur != "R") t.push_back(cur); cur.clear(); if (!*c) break; } else cur += *c; } }
    if (t.size() < 3 || t.back() == "RR73" || t.back().size() != 4) return true;
    static const char* const kWords[] = {"CQ", "DX", "NA", "SA", "EU", "AS", "AF", "OC", "QRP", "TEST", "RU", "FD", "DE", "QRZ"};
    const std::string& call = t[t.size() - 2]; for (const char* w : kWords) if (call == w) return true;
    return callsign_grid_plausible(call, t.back());
}

// ---------------------------------------------------------------- multi-pass decoding with signal subtraction (as WSJT-X does)
// A decoded message gives the exact tones sent, so its waveform can be rebuilt, fitted to the audio and subtracted; signals underneath it
// (overlapping in time and frequency on a busy band) then decode in the next pass.
constexpr int kPasses = 3;
constexpr double kFocusHz = 20.0;            // quick pass: the deep stage (with a-priori decoding of messages to us) within this of a station calling us
// Weak (OSD / deep) decodes of message types other than standard and non-standard calls (contest forms, DXpedition, telemetry, free text) were ALL false in 12000 noise slots
// (18 of 18, 18..31 hard errors) and none of 230 genuine weak decodes on the air was one: they need hard errors <= 17. A weak "/P" message (type 2: 3 false in noise with 24..26 hard
// errors; the genuine one seen had 18; before this the whole type was refused, which also threw away genuine portable stations) needs <= 21.
constexpr int kUnusualMaxHard = 17, kSlashPMaxHard = 21;
// FT4 deep-stage decodes (stage >= 10) need sync and a code distance like the genuine ones: 6 of 6 seen on the air had Costas sync 11..15 of 16 and dmin 4..24; the 6 false ones it
// added in 6000 noise slots had sync 8..11 and dmin 23..35 (each fails at least one of these).
constexpr int kFt4DeepMinSync = 10; constexpr double kFt4DeepMaxDmin = 25.0;
// ft8_lib's waterfall time (a candidate's time_offset + time_sub) is 1.24 symbols AFTER the signal's true start: measured by fitting decoded signals, FT8 0.198 s (n = 231, sd 0.013 s),
// FT4 0.059 s (n = 21). The deep stages report true time; a deep decode's place is mapped back with this for the waterfall SNR. (Centring FT8's deep search on the true start
// instead of the candidate time was tried: 881 vs 882 of jt9's 939 on the live capture, no gain, so FT8's search is unchanged.)
constexpr double kWaterfallLagSymbols = 1.24;
// GFSK phase (radians, relative to t = 0 of the waveform) of a tone sequence at f0 Hz: the same pulse shaping as the generator below and WSJT-X.
std::vector<double> gfsk_phase(const uint8_t* tones, int nsym, int nsp, double bt, double f0, double fs) {
    const int nwave = nsym * nsp; const double K = 5.336446, dphi_peak = 2 * M_PI / nsp;
    std::vector<double> pulse(size_t(3 * nsp)); for (int i = 0; i < 3 * nsp; ++i) { const double t = i / double(nsp) - 1.5; pulse[size_t(i)] = (std::erf(K * bt * (t + 0.5)) - std::erf(K * bt * (t - 0.5))) / 2; }
    std::vector<double> dphi(size_t(nwave + 2 * nsp), 2 * M_PI * f0 / fs);
    for (int i = 0; i < nsym; ++i) for (int j = 0; j < 3 * nsp; ++j) dphi[size_t(i * nsp + j)] += dphi_peak * tones[i] * pulse[size_t(j)];
    for (int j = 0; j < 2 * nsp; ++j) { dphi[size_t(j)] += dphi_peak * pulse[size_t(j + nsp)] * tones[0]; dphi[size_t(j + nsym * nsp)] += dphi_peak * pulse[size_t(j)] * tones[nsym - 1]; }
    std::vector<double> ph(static_cast<size_t>(nwave)); double phi = 0; for (int k = 0; k < nwave; ++k) { ph[size_t(k)] = phi; phi += dphi[size_t(k + nsp)]; }
    return ph;
}
struct Waveform { std::vector<float> c, s; int nsp, nsym; };                   // cos / sin of the phase, for correlation and subtraction
Waveform make_waveform(const uint8_t* tones, bool ft4, double f0) {
    Waveform w; w.nsym = ft4 ? FT4_NN : FT8_NN; w.nsp = int(0.5 + kRate * (ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD));
    const auto ph = gfsk_phase(tones, w.nsym, w.nsp, ft4 ? 1.0 : 2.0, f0, kRate);
    w.c.resize(ph.size()); w.s.resize(ph.size()); for (size_t k = 0; k < ph.size(); ++k) { w.c[k] = float(std::cos(ph[k])); w.s[k] = float(std::sin(ph[k])); }
    return w;
}
// Non-coherent fit: sum over symbols of |sum of x * conj(ref)|^2 with the reference started at sample `at` and shifted by df Hz. Per-symbol
// magnitudes make it tolerant of fading and of a little frequency error; the peak is sharp enough in time to place the waveform to a few samples.
double fit_score(const float* x, long n, const Waveform& w, long at, double df) {
    double total = 0; const double wr = std::cos(-2 * M_PI * df / kRate), wi = std::sin(-2 * M_PI * df / kRate); double rr = 1, ri = 0;
    for (int sy = 0; sy < w.nsym; ++sy) {
        double ar = 0, ai = 0;
        for (int j = 0; j < w.nsp; ++j) {
            const long k = long(sy) * w.nsp + j, i = at + k;
            if (i >= 0 && i < n) { const double cr = w.c[size_t(k)] * rr + w.s[size_t(k)] * ri, ci = w.s[size_t(k)] * rr - w.c[size_t(k)] * ri;   // ref * e^{-j 2 pi df t}
                                   ar += x[i] * cr; ai -= x[i] * ci; }
            const double t = rr * wr - ri * wi; ri = rr * wi + ri * wr; rr = t;
        }
        total += ar * ar + ai * ai;
    }
    return total;
}
struct Fit { long at = 0; double df = 0; };
Fit refine(const float* x, long n, const Waveform& w, double t_guess_s) {
    Fit best; double bs = -1; const long g = long(std::lround(t_guess_s * kRate));
    auto tryp = [&](long at, double df) { const double v = fit_score(x, n, w, at, df); if (v > bs) { bs = v; best = {at, df}; } };
    for (long d = -4200; d <= 4200; d += 240) tryp(g + d, 0.0);                                       // +-0.35 s in 20 ms steps
    const long a1 = best.at; for (double df = -2.0; df <= 2.0001; df += 0.25) tryp(a1, df);
    const double d1 = best.df; for (long d = -240; d <= 240; d += 24) tryp(a1 + d, d1);               // +-20 ms in 2 ms
    const long a2 = best.at; for (double df = d1 - 0.25; df <= d1 + 0.2501; df += 0.05) tryp(a2, df);
    const double d2 = best.df; for (long d = -24; d <= 24; d += 4) tryp(a2 + d, d2);
    return best;
}
// Subtracts the fitted signal: complex envelope = low-pass(x * conj(ref)) (two cascaded boxcars, edge-normalised), then x -= 2 Re(env * ref).
void subtract(float* x, long n, const Waveform& w, const Fit& f) {
    // two cascaded 3000-sample (0.25 s) boxcars: the envelope may follow fading over about half a second
    const long nw = long(w.c.size()), L = 3000; const size_t nws = static_cast<size_t>(nw); std::vector<double> zr(nws), zi(nws), cr(nws), ci(nws), one(nws);
    const double wr = std::cos(2 * M_PI * f.df / kRate), wi = std::sin(2 * M_PI * f.df / kRate); double rr = 1, ri = 0;
    for (long k = 0; k < nw; ++k) {
        cr[size_t(k)] = w.c[size_t(k)] * rr - w.s[size_t(k)] * ri; ci[size_t(k)] = w.s[size_t(k)] * rr + w.c[size_t(k)] * ri;    // ref * e^{+j 2 pi df t}
        const long i = f.at + k; const double v = (i >= 0 && i < n) ? x[i] : 0.0; one[size_t(k)] = (i >= 0 && i < n) ? 1.0 : 0.0;
        zr[size_t(k)] = v * cr[size_t(k)]; zi[size_t(k)] = -v * ci[size_t(k)];
        const double t = rr * wr - ri * wi; ri = rr * wi + ri * wr; rr = t;
    }
    auto box = [&](std::vector<double>& a) { std::vector<double> c(a.size() + 1, 0.0); for (size_t k = 0; k < a.size(); ++k) c[k + 1] = c[k] + a[k];
                                             for (long k = 0; k < nw; ++k) { const long lo = std::max(0L, k - L / 2), hi = std::min(nw, k + L / 2 + 1); a[size_t(k)] = c[size_t(hi)] - c[size_t(lo)]; } };
    for (int pass = 0; pass < 2; ++pass) { box(zr); box(zi); box(one); }
    for (long k = 0; k < nw; ++k) {
        const long i = f.at + k; if (i < 0 || i >= n || one[size_t(k)] <= 0) continue;
        const double er = zr[size_t(k)] / one[size_t(k)], ei = zi[size_t(k)] / one[size_t(k)];
        x[i] -= float(2.0 * (er * cr[size_t(k)] - ei * ci[size_t(k)]));
    }
}

std::vector<Ft8Message> decode_slot_with(const float* audio, size_t n, HashTable& tab, const std::string& label, bool ft4, const std::string& my_call, bool quick = false,
                                         const std::vector<double>& focus = {}) {   // focus: in a quick pass, the deep stage still runs within kFocusHz of these audio frequencies (stations calling us)   // quick: one pass, no deep stage (the strong messages, fast)
    const ApBits ap = make_ap(my_call); static const bool dbg_drop = std::getenv("FT8_DEBUG_DROP") != nullptr, no_deep4 = std::getenv("FT8_NO_DEEP4") != nullptr;   // FT8_NO_DEEP4=1: FT4 without the deep stage (to compare)      // FT8_DEBUG_DROP=1: say why a decoded message was thrown away
    std::vector<Ft8Message> out; const size_t kSlot = size_t((ft4 ? FT4_SLOT_TIME : FT8_SLOT_TIME) * kRate);
    std::vector<float> resid(audio, audio + n); if (resid.size() < kSlot) resid.resize(kSlot, 0.0f);     // decoded signals are subtracted from this copy
    std::vector<ftx_message_t> decoded;                                                                    // every message so far, to skip it in later passes
    ftx_callsign_hash_interface_t hif; hif.lookup_hash = hash_lookup; hif.save_hash = hash_save; HashTable* prev = g_tab; g_tab = &tab;
    for (int pass = 0; pass < (quick ? 1 : kPasses); ++pass) {
        monitor_t mon; monitor_config_t cfg = {200.0f, 3000.0f, kRate, kTimeOsr, kFreqOsr, ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8}; monitor_init(&mon, &cfg);
        for (size_t pos = 0; pos + size_t(mon.block_size) <= resid.size() && mon.wf.num_blocks < mon.wf.max_blocks; pos += size_t(mon.block_size)) monitor_process(&mon, resid.data() + pos);
        ftx_candidate_t cand[kMaxCand]; const int nc = ftx_find_candidates(&mon.wf, kMaxCand, cand, kMinScore);
        struct New { uint8_t tones[FT4_NN]; double f0, t_s; bool deep = false; };
        std::unique_ptr<demod::SlotSpectrum> spec;                                                // the slot's FFT for the deep stage, made when first needed                 // FT4_NN (105) > FT8_NN (79): room for either
        std::vector<New> fresh; std::vector<double> deep_f;                                      // frequencies the FT4 deep stage has searched this pass
        for (int i = 0; i < nc && int(decoded.size()) < kMaxMsgs; ++i) {
            const ftx_candidate_t* c = &cand[i]; ftx_message_t msg; ftx_decode_status_t st; bool fallback = false; int ap_type = 0; double deep_f0 = -1, deep_t = 0;
            if (!ftx_decode_candidate(&mon.wf, c, kLdpcIter, &msg, &st)) {
                bool got = kOsdSums != 0 && decode_fallback(&mon.wf, c, ft4, &msg);
                const double fc = (mon.min_bin + c->freq_offset + double(c->freq_sub) / mon.wf.freq_osr) / mon.symbol_period;
                const bool tried = ft4 && std::any_of(deep_f.begin(), deep_f.end(), [&](double f) { return std::fabs(f - fc) < 2.0; });   // FT4 searches all times: once per frequency is enough
                const bool near_focus = std::any_of(focus.begin(), focus.end(), [&](double f) { return std::fabs(f - fc) < kFocusHz; });
                if (!got && kDeep && (!quick || near_focus) && (!ft4 || !no_deep4) && !tried) {                             // WSJT-X's deep stage (FT8: ft8b.f90; FT4: ft4_decode.f90)
                    if (ft4) deep_f.push_back(fc);
                    if (!spec) spec = std::make_unique<demod::SlotSpectrum>(resid.data(), resid.size(), ft4);
                    const double f0 = (mon.min_bin + c->freq_offset + double(c->freq_sub) / mon.wf.freq_osr) / mon.symbol_period;
                    const double ts = (c->time_offset + double(c->time_sub) / mon.wf.time_osr) * mon.symbol_period;
                    got = decode_deep(*spec, f0, ts, ap, &msg, &ap_type, ft4, &deep_f0, &deep_t);
                }
                if (!got) continue; fallback = true;
            }
            if (!payload_plausible(msg.payload)) { if (dbg_drop) std::fprintf(stderr, "DROP %s payload_implausible f=%.0f\n", label.c_str(), (mon.min_bin + c->freq_offset + double(c->freq_sub) / mon.wf.freq_osr) / mon.symbol_period); continue; }   // WSJT-X would not unpack it
            if (std::any_of(decoded.begin(), decoded.end(), [&](const ftx_message_t& d) { return d.hash == msg.hash && !std::memcmp(d.payload, msg.payload, sizeof msg.payload); })) continue;
            decoded.push_back(msg);
            char text[FTX_MAX_MESSAGE_LENGTH]; ftx_message_offsets_t offs;
            if (ftx_message_decode(&msg, &hif, text, &offs) != FTX_MESSAGE_RC_OK) {
                std::string extra;                                                              // a type ft8_lib cannot unpack: contest / DXpedition / EU VHF (decode_extra_type)
                if (!decode_extra_type(msg.payload, &hif, extra)) continue;
                std::snprintf(text, sizeof text, "%s", extra.c_str());
            }
            if (fallback && !location_plausible(text)) { if (dbg_drop) std::fprintf(stderr, "DROP %s location '%s'\n", label.c_str(), text); continue; }                                    // a weak decode with an impossible locator: false (kept in `decoded`: not retried)
            if (fallback && !ft4 && (std::strstr(text, "/R") || std::strncmp(text, "TU; ", 4) == 0)) { if (dbg_drop) std::fprintf(stderr, "DROP %s contest-form '%s'\n", label.c_str(), text); decoded.pop_back(); continue; }   // WSJT-X: implausible outside contests
            if (fallback) {                                                                          // weak decodes: how much evidence a message type needs (measured on 12000 noise slots and 250 real weak decodes)
                const unsigned i3 = get_bits(msg.payload, 74, 3);
                const bool usual = i3 == 1 || i3 == 2 || i3 == 4;                                    // standard (incl. /P) and non-standard calls
                if ((!usual && g_fb_hard > kUnusualMaxHard) || (i3 == 2 && g_fb_hard > kSlashPMaxHard)) {
                    if (dbg_drop) std::fprintf(stderr, "DROP %s weak-%s '%s' hard=%d dmin=%.1f score=%d\n", label.c_str(), i3 == 2 ? "slash-P" : "unusual-type", text, g_fb_hard, g_fb_dmin, c->score);
                    decoded.pop_back(); continue;
                }
                if (ft4 && g_fb_stage >= 10 && (g_fb_nsync < kFt4DeepMinSync || g_fb_dmin >= kFt4DeepMaxDmin)) {
                    if (dbg_drop) std::fprintf(stderr, "DROP %s ft4-deep '%s' nsync=%d dmin=%.1f\n", label.c_str(), text, g_fb_nsync, g_fb_dmin);
                    decoded.pop_back(); continue;
                }
            }
            New nw{}; if (ft4) ft4_encode(msg.payload, nw.tones); else ft8_encode(msg.payload, nw.tones);
            if (fallback && std::getenv("FT8_DEBUG_OSD")) std::fprintf(stderr, "OSDDBG %s stage=%d osd=%d hard=%d dmin=%.1f nsync=%d score=%d pass=%d ap=%d snr=%d i3=%u n3=%u | %s\n", label.c_str(), g_fb_stage, int(g_fb_osd), g_fb_hard, g_fb_dmin, g_fb_stage >= 10 ? g_fb_nsync : -1, c->score, pass, ap_type, estimate_snr(&mon.wf, c, nw.tones, ft4), get_bits(msg.payload, 74, 3), get_bits(msg.payload, 71, 3), text);
            nw.f0 = (mon.min_bin + c->freq_offset + double(c->freq_sub) / mon.wf.freq_osr) / mon.symbol_period;
            nw.t_s = (c->time_offset + double(c->time_sub) / mon.wf.time_osr) * mon.symbol_period;
            if (deep_f0 > 0) { nw.f0 = deep_f0; nw.t_s = deep_t; nw.deep = true; }                                   // a deep decode knows the signal's time and frequency better than the candidate
            ftx_candidate_t at = *c;                                                                 // where the signal is on the waterfall: the deep stage's refined place if it found one
            if (deep_f0 > 0) {
                const double tb = nw.t_s / mon.symbol_period + kWaterfallLagSymbols, fb = nw.f0 * mon.symbol_period - mon.min_bin;
                int to = int(std::floor(tb)), ts_ = int(std::lround((tb - to) * mon.wf.time_osr)), fo = int(std::floor(fb)), fs_ = int(std::lround((fb - fo) * mon.wf.freq_osr));
                if (ts_ >= mon.wf.time_osr) { ++to; ts_ = 0; } if (fs_ >= mon.wf.freq_osr) { ++fo; fs_ = 0; }
                at.time_offset = int16_t(to); at.time_sub = uint8_t(ts_); at.freq_offset = int16_t(fo); at.freq_sub = uint8_t(fs_);
            }
            Ft8Message m; m.time = label; m.snr = estimate_snr(&mon.wf, &at, nw.tones, ft4); m.freq = nw.f0; m.text = text; m.dt = nw.t_s; m.ap = ap_type;
            out.push_back(std::move(m)); fresh.push_back(nw);
        }
        monitor_free(&mon);
        const size_t first = out.size() - fresh.size();
        for (size_t k = 0; k < fresh.size(); ++k) {                                                        // fit each new signal, report its true start, subtract it
            const Waveform w = make_waveform(fresh[k].tones, ft4, fresh[k].f0);
            const Fit f = refine(resid.data(), long(resid.size()), w, fresh[k].t_s);
            if (dbg_drop && std::getenv("FT8_DEBUG_FIT")) std::fprintf(stderr, "FIT %s from=%s t_in=%.3f fitted=%.3f diff=%+.3f f=%.1f\n", label.c_str(), fresh[k].deep ? "deep" : "cand", fresh[k].t_s, double(f.at) / kRate, double(f.at) / kRate - fresh[k].t_s, fresh[k].f0);
            out[first + k].dt = double(f.at) / kRate - 0.5;                                                // WSJT-X convention: seconds after slot start + 0.5 s
            out[first + k].freq = fresh[k].f0 + f.df;
            subtract(resid.data(), long(resid.size()), w, f);
        }
        if (fresh.empty()) break;
    }
    g_tab = prev; tab.age(10);
    std::sort(out.begin(), out.end(), [](const Ft8Message& a, const Ft8Message& b) { return a.freq < b.freq; });
    return out;
}
}  // namespace

bool unpack_extra_message(const uint8_t payload[10], std::string& text, const std::vector<std::string>& heard_messages) {
    ftx_callsign_hash_interface_t hif; hif.lookup_hash = hash_lookup; hif.save_hash = hash_save; HashTable tab; HashTable* prev = g_tab; g_tab = &tab;
    for (const auto& h : heard_messages) { ftx_message_t m; ftx_message_encode(&m, &hif, h.c_str()); }          // encoding a message hashes its callsigns, as having decoded it does
    const bool ok = decode_extra_type(payload, &hif, text); g_tab = prev; return ok;
}
std::string Ft8Decoder::format(const Ft8Message& m) { char b[200]; std::snprintf(b, sizeof b, "%s %+3d %4.1f %4.0f ~  %s%s\n", m.time.c_str(), m.snr, m.dt, m.freq, m.text.c_str(), m.ap == 1 ? " a1" : m.ap == 2 ? " a2" : ""); return b; }
std::vector<Ft8Message> Ft8Decoder::decode_slot(const float* a, size_t n, const std::string& label, bool ft4, const std::string& my_call) { HashTable tab; return decode_slot_with(a, n, tab, label, ft4, my_call); }

constexpr double kLateJoinS = 0.4;           // a stream (re)starting this far into a slot still decodes it: FT8 / FT4 signals start 0.5 s in
struct Ft8Decoder::Impl {
    Ft8Options o; std::unique_ptr<fireside::dsp::RationalResampler> rs;
    std::vector<float> slot; size_t fill = 0; long skip = -1; bool started = false; long slot_index = 0; double t0_utc = 0;   // skip: 12 kHz samples to discard until the first slot boundary
    std::vector<std::string> peeked; long peeked_slot = -1; HashTable tab; std::thread job; std::atomic<bool> running{false}; std::atomic<double> last_decode_s{0}, last_quick_s{0}; std::vector<double> focus; std::mutex fm; std::vector<double> get_focus() { std::lock_guard<std::mutex> l(fm); return focus; } std::mutex m; std::vector<Ft8Message> results; std::string last; size_t decoded_total = 0; std::vector<fireside::dsp::cf> tmp_in, tmp_out;
    double slot_s() const { return o.ft4 ? double(FT4_SLOT_TIME) : double(FT8_SLOT_TIME); }
    size_t slot_len() const { return size_t(slot_s() * kRate); }
    explicit Impl(Ft8Options op) : o(op) { reset_state(); }
    void reset_state() { join(); rs = std::make_unique<fireside::dsp::RationalResampler>(o.sample_rate, double(kRate)); slot.assign(slot_len(), 0.0f); fill = 0; skip = -1; started = false; slot_index = 0; { std::lock_guard<std::mutex> l(m); results.clear(); } last.clear(); decoded_total = 0; }
    void join() { if (job.joinable()) job.join(); }
    std::string label_for(long idx) const {
        char b[24];
        if (o.wall_clock) { const time_t t = time_t(t0_utc + double(idx) * slot_s()); struct tm g; gmtime_r(&t, &g); std::snprintf(b, sizeof b, "%02d%02d%02d", g.tm_hour, g.tm_min, g.tm_sec); }
        else std::snprintf(b, sizeof b, "S%03ld", idx);
        return b;
    }
    void launch() {
        join(); const std::string label = label_for(slot_index++); std::vector<float> copy = slot; fill = 0;
        running = true;
        std::vector<std::string> skip; if (peeked_slot == slot_index - 1) skip.swap(peeked); peeked.clear(); peeked_slot = -1;   // lines a peek already gave for this slot
        job = std::thread([this, copy = std::move(copy), label, skip = std::move(skip)] { const auto t0 = std::chrono::steady_clock::now(); auto r = decode_slot_with(copy.data(), copy.size(), tab, label, o.ft4, o.my_call); last_decode_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            { std::lock_guard<std::mutex> l(m); for (auto& x : r) if (std::find(skip.begin(), skip.end(), x.text) == skip.end()) results.push_back(std::move(x)); } running = false; });
    }
    void audio(const float* x, size_t n, std::string& out) {
        if (!started) {                                       // work out how far into a slot the first sample is
            started = true; double off = o.start_offset_s;
            if (o.wall_clock) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); const double now = double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9; off = std::fmod(now, slot_s()); t0_utc = now - off; }
            if (off <= kLateJoinS) { skip = 0; std::fill(slot.begin(), slot.end(), 0.0f); fill = size_t(std::lround(off * kRate)); }   // up to 0.4 s in: nothing lost (signals start at 0.5 s), silence before
            else skip = long(std::lround((slot_s() - off) * kRate));                        // later: that slot cannot be decoded, wait for the next boundary
            if (skip > 0 && o.wall_clock) slot_index = 1;
        }
        tmp_in.resize(n); for (size_t i = 0; i < n; ++i) tmp_in[i] = fireside::dsp::cf(x[i], 0.0f);
        tmp_out.clear(); rs->process(tmp_in.data(), n, tmp_out);
        for (const auto& v : tmp_out) {
            if (skip > 0) { --skip; continue; }
            slot[fill++] = v.real(); if (fill == slot.size()) launch();
        }
        std::lock_guard<std::mutex> l(m);
        for (auto& r : results) { const std::string line = Ft8Decoder::format(r); out += line; last = line; ++decoded_total; }
        results.clear();
    }
};
Ft8Decoder::Ft8Decoder(Ft8Options o) : p_(std::make_unique<Impl>(o)) {}
Ft8Decoder::~Ft8Decoder() { p_->join(); }
void Ft8Decoder::process(const float* a, size_t n, std::string& out) { p_->audio(a, n, out); }
void Ft8Decoder::set_focus(const std::vector<double>& hz) { std::lock_guard<std::mutex> l(p_->fm); p_->focus = hz; }
void Ft8Decoder::reset() { p_->reset_state(); }
void Ft8Decoder::peek(std::string& out) {
    auto& p = *p_; p.join();
    { std::lock_guard<std::mutex> l(p.m); for (auto& r : p.results) { const std::string line = Ft8Decoder::format(r); out += line; p.last = line; ++p.decoded_total; } p.results.clear(); }
    const double sig_end = 0.5 + (p.o.ft4 ? 5.04 : 12.64) + 0.9;
    if (!(p.started && p.skip <= 0 && double(p.fill) / kRate >= sig_end) || p.peeked_slot == p.slot_index) return;   // once per slot
    std::vector<float> copy = p.slot; std::fill(copy.begin() + long(p.fill), copy.end(), 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    auto quick = decode_slot_with(copy.data(), copy.size(), p.tab, p.label_for(p.slot_index), p.o.ft4, p.o.my_call, true, p.get_focus());
    p.last_quick_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    p.peeked.clear(); p.peeked_slot = p.slot_index;
    for (auto& r : quick) { const std::string line = Ft8Decoder::format(r); out += line; p.last = line; ++p.decoded_total; p.peeked.push_back(r.text); }
}
void Ft8Decoder::finish(std::string& out) {
    auto& p = *p_; p.join();
    { std::lock_guard<std::mutex> l(p.m); for (auto& r : p.results) { const std::string line = Ft8Decoder::format(r); out += line; p.last = line; ++p.decoded_total; } p.results.clear(); }
    const double sig_end = 0.5 + (p.o.ft4 ? 5.04 : 12.64) + 0.9;                       // nominal start + signal + margin (decoder DT spread)
    if (!(p.started && p.skip <= 0 && double(p.fill) / kRate >= sig_end)) return;
    std::fill(p.slot.begin() + long(p.fill), p.slot.end(), 0.0f); p.fill = 0;
    std::vector<float> copy = p.slot; const std::string label = p.label_for(p.slot_index++);
    // quick first (one pass, no deep stage: ~0.3 s): an answer to us must be known before our slot starts; the full decode follows in the background
    const auto t0 = std::chrono::steady_clock::now();
    auto quick = decode_slot_with(copy.data(), copy.size(), p.tab, label, p.o.ft4, p.o.my_call, true, p.get_focus());
    p.last_quick_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<std::string> seen; for (auto& r : quick) { const std::string line = Ft8Decoder::format(r); out += line; p.last = line; ++p.decoded_total; seen.push_back(r.text); }
    if (p.o.late_lines) {                                                               // the weak ones: later, through the callback (this decoder may be gone by then)
        HashTable tab = p.tab; auto sink = p.o.late_lines; const bool ft4 = p.o.ft4; const std::string me = p.o.my_call;
        std::thread([copy = std::move(copy), tab, sink, ft4, me, label, seen]() mutable {
            std::string lines; for (auto& r : decode_slot_with(copy.data(), copy.size(), tab, label, ft4, me))
                if (std::find(seen.begin(), seen.end(), r.text) == seen.end()) lines += Ft8Decoder::format(r);
            if (!lines.empty()) sink(lines);
        }).detach();
    }
}
bool Ft8Decoder::busy() const { return p_->running.load(); }
std::string Ft8Decoder::status() const {
    char b[128]; const double secs = double(p_->fill) / kRate;
    std::snprintf(b, sizeof b, p_->o.ft4 ? "FT4: slot %.1f/7.5 s, %zu message(s) decoded, last decode %.2f s" : "FT8: slot %.0f/15 s, %zu message(s) decoded, last decode %.2f s", secs, p_->decoded_total, p_->last_decode_s.load());
    if (p_->last_quick_s.load() > 0) { const size_t k = std::strlen(b); std::snprintf(b + k, sizeof b - k, " (before TX %.2f s)", p_->last_quick_s.load()); }
    return b;
}

// ---------------------------------------------------------------- generator (GFSK, as in ft8_lib's gen_ft8 demo)
namespace {
// 8-GFSK (FT8) / 4-GFSK (FT4) waveform from a tone sequence, placed `dt_s` into a slot of `slot_s` seconds (follows the library's gen_ft8 demo)
std::vector<float> gfsk_slot(const uint8_t* tones, int nsym, double symbol_period, double bt, double f0, double fs, double amp, double dt_s, double slot_s) {
    const int nsp = int(0.5 + fs * symbol_period), nwave = nsym * nsp; const double K = 5.336446, dphi_peak = 2 * M_PI / nsp;
    std::vector<double> pulse(size_t(3 * nsp)); for (int i = 0; i < 3 * nsp; ++i) { const double t = i / double(nsp) - 1.5; pulse[size_t(i)] = (std::erf(K * bt * (t + 0.5)) - std::erf(K * bt * (t - 0.5))) / 2; }
    std::vector<double> dphi(size_t(nwave + 2 * nsp), 2 * M_PI * f0 / fs);
    for (int i = 0; i < nsym; ++i) for (int j = 0; j < 3 * nsp; ++j) dphi[size_t(i * nsp + j)] += dphi_peak * tones[i] * pulse[size_t(j)];
    for (int j = 0; j < 2 * nsp; ++j) { dphi[size_t(j)] += dphi_peak * pulse[size_t(j + nsp)] * tones[0]; dphi[size_t(j + nsym * nsp)] += dphi_peak * pulse[size_t(j)] * tones[nsym - 1]; }
    std::vector<float> w(static_cast<size_t>(nwave), 0.0f); double phi = 0; for (int k = 0; k < nwave; ++k) { w[size_t(k)] = float(std::sin(phi)); phi = std::fmod(phi + dphi[size_t(k + nsp)], 2 * M_PI); }
    const int ramp = nsp / 8; for (int i = 0; i < ramp; ++i) { const double e = (1 - std::cos(2 * M_PI * i / (2.0 * ramp))) / 2; w[size_t(i)] *= float(e); w[size_t(nwave - 1 - i)] *= float(e); }
    std::vector<float> slot(size_t(slot_s * fs), 0.0f); const size_t at = size_t(std::max(0.0, dt_s) * fs);
    for (size_t i = 0; i < w.size() && at + i < slot.size(); ++i) slot[at + i] = float(amp) * w[i];
    return slot;
}
}  // namespace
// The same GFSK phase as gfsk_slot, as the ANALYTIC signal exp(j phi): a single sideband at +f0 by construction (no SSB filter), at any
// rate, envelope 1 (with the same 1/8-symbol ramps). For transmit: computed in milliseconds where filtering real audio took ~3 s.
std::vector<std::complex<float>> synth_ft_iq(const std::string& message, double f0, double fs, bool ft4, double dt_s) {
    ftx_message_t msg; if (ftx_message_encode(&msg, nullptr, message.c_str()) != FTX_MESSAGE_RC_OK) return {};
    uint8_t tones[FT4_NN]; int nsym; double sp, bt, slot_s;
    if (ft4) { ft4_encode(msg.payload, tones); nsym = FT4_NN; sp = FT4_SYMBOL_PERIOD; bt = 1.0; slot_s = FT4_SLOT_TIME; }
    else { ft8_encode(msg.payload, tones); nsym = FT8_NN; sp = FT8_SYMBOL_PERIOD; bt = 2.0; slot_s = FT8_SLOT_TIME; }
    const int nsp = int(0.5 + fs * sp), nwave = nsym * nsp; const double K = 5.336446, dphi_peak = 2 * M_PI / nsp;
    std::vector<double> pulse(size_t(3 * nsp)); for (int i = 0; i < 3 * nsp; ++i) { const double t = i / double(nsp) - 1.5; pulse[size_t(i)] = (std::erf(K * bt * (t + 0.5)) - std::erf(K * bt * (t - 0.5))) / 2; }
    std::vector<double> dphi(size_t(nwave + 2 * nsp), 2 * M_PI * f0 / fs);
    for (int i = 0; i < nsym; ++i) for (int j = 0; j < 3 * nsp; ++j) dphi[size_t(i * nsp + j)] += dphi_peak * tones[i] * pulse[size_t(j)];
    for (int j = 0; j < 2 * nsp; ++j) { dphi[size_t(j)] += dphi_peak * pulse[size_t(j + nsp)] * tones[0]; dphi[size_t(j + nsym * nsp)] += dphi_peak * pulse[size_t(j)] * tones[nsym - 1]; }
    std::vector<std::complex<float>> out(size_t(slot_s * fs), std::complex<float>(0, 0)); const size_t at = size_t(std::max(0.0, dt_s) * fs);
    const int ramp = nsp / 8; double phi = 0;
    for (int k = 0; k < nwave && at + size_t(k) < out.size(); ++k) {
        double e = 1; if (k < ramp) e = (1 - std::cos(2 * M_PI * k / (2.0 * ramp))) / 2; else if (k >= nwave - ramp) e = (1 - std::cos(2 * M_PI * (nwave - 1 - k) / (2.0 * ramp))) / 2;
        out[at + size_t(k)] = std::complex<float>(float(e * std::cos(phi)), float(e * std::sin(phi)));
        phi = std::fmod(phi + dphi[size_t(k + nsp)], 2 * M_PI);
    }
    return out;
}
std::vector<float> synth_ft8(const std::string& message, double f0, double fs, double amp, double dt_s) {
    ftx_message_t msg; if (ftx_message_encode(&msg, nullptr, message.c_str()) != FTX_MESSAGE_RC_OK) return {};
    uint8_t tones[FT8_NN]; ft8_encode(msg.payload, tones); return gfsk_slot(tones, FT8_NN, FT8_SYMBOL_PERIOD, 2.0, f0, fs, amp, dt_s, FT8_SLOT_TIME);
}
std::vector<float> synth_ft4(const std::string& message, double f0, double fs, double amp, double dt_s) {
    ftx_message_t msg; if (ftx_message_encode(&msg, nullptr, message.c_str()) != FTX_MESSAGE_RC_OK) return {};
    uint8_t tones[FT4_NN]; ft4_encode(msg.payload, tones); return gfsk_slot(tones, FT4_NN, FT4_SYMBOL_PERIOD, 1.0, f0, fs, amp, dt_s, FT4_SLOT_TIME);
}
}  // namespace fireside::decode
