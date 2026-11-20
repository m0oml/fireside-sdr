// BP + OSD fallback for the (174,91) code (core/src/ft8_osd.h): codewords are valid, BP decodes clean ones, OSD recovers words BP cannot and never
// returns a wrong codeword as a success, and pure noise does not decode.
#include <cmath>
#include <random>
#include "../core/src/ft8_osd.h"
#include "fireside/decode.h"
#include "test_util.h"
extern "C" {
#include <ft8/constants.h>
#include <ft8/crc.h>
#include <ft8/message.h>
}
using namespace fireside::decode;

namespace {
// A full codeword for a message: 77 bits + CRC14 (ft8_lib), parity from ft8_lib's generator table.
std::vector<uint8_t> codeword(const char* text) {
    ftx_message_t m; if (ftx_message_encode(&m, nullptr, text) != FTX_MESSAGE_RC_OK) return {};
    uint8_t a91[FTX_LDPC_K_BYTES] = {0}; ftx_add_crc(m.payload, a91);
    std::vector<uint8_t> cw(174, 0); for (int i = 0; i < 91; ++i) cw[size_t(i)] = (a91[i / 8] >> (7 - i % 8)) & 1;
    for (int j = 0; j < 83; ++j) { int s = 0; for (int i = 0; i < 91; ++i) s ^= ((kFTX_LDPC_generator[j][i / 8] >> (7 - i % 8)) & 1) & cw[size_t(i)]; cw[size_t(91 + j)] = uint8_t(s); }
    return cw;
}
int unsatisfied(const std::vector<uint8_t>& cw) {
    int n = 0; for (int j = 0; j < 83; ++j) { int s = 0; for (int i = 0; i < kFTX_LDPC_Num_rows[j]; ++i) s ^= cw[size_t(kFTX_LDPC_Nm[j][i] - 1)]; n += s; } return n;
}
std::vector<float> channel(const std::vector<uint8_t>& cw, double amp, double sigma, std::mt19937& rng) {
    std::normal_distribution<float> g(0.0f, float(sigma)); std::vector<float> l(174); for (size_t i = 0; i < 174; ++i) l[i] = float((cw[i] ? amp : -amp)) + g(rng); return l;
}
}  // namespace

TEST(osd_codewords_satisfy_every_parity_check_and_the_crc) {
    for (const char* t : {"CQ M0OML IO92", "G4ABC DL1XYZ -15", "K1ABC W9XYZ RR73", "CQ DX K1ABC FN42"}) {
        const auto cw = codeword(t); CHECK_EQ(cw.size(), size_t(174)); CHECK_EQ(unsatisfied(cw), 0); CHECK(osd::crc_ok(cw.data()));
        auto bad = cw; bad[10] ^= 1; CHECK(!osd::crc_ok(bad.data()));
    }
}
TEST(osd_bp_decodes_a_clean_word_without_osd) {
    const auto cw = codeword("CQ M0OML IO92"); std::vector<float> l(174); for (size_t i = 0; i < 174; ++i) l[i] = cw[i] ? 4.0f : -4.0f;
    const auto r = osd::decode(l.data()); CHECK(r.ok); CHECK(!r.by_osd); CHECK_EQ(r.hard_errors, 0); for (size_t i = 0; i < 174; ++i) CHECK_EQ(int(r.bits[i]), int(cw[i]));
}
TEST(osd_recovers_words_bp_cannot_and_never_returns_a_wrong_one) {
    // At a noise level where BP alone mostly fails, BP+OSD must decode clearly more, and every success must be the codeword that was sent.
    std::mt19937 rng(7); const char* msgs[] = {"CQ M0OML IO92", "G4ABC DL1XYZ -15", "K1ABC W9XYZ RR73"};
    int bp = 0, both = 0, wrong = 0; const int trials = 150;
    for (int t = 0; t < trials; ++t) {
        const auto cw = codeword(msgs[t % 3]); const auto l = channel(cw, 1.0, 0.95, rng);
        const auto r0 = osd::decode(l.data(), 0), r2 = osd::decode(l.data(), 2);
        if (r0.ok) ++bp;
        if (r2.ok) { ++both; for (size_t i = 0; i < 174; ++i) if (r2.bits[i] != cw[i]) { ++wrong; break; } }
    }
    CHECK(both >= bp + trials / 10); CHECK_EQ(wrong, 0);
}
TEST(osd_pure_noise_soft_bits_do_not_decode) {
    std::mt19937 rng(11); std::normal_distribution<float> g(0.0f, 1.0f); int ok = 0;
    for (int t = 0; t < 400; ++t) { std::vector<float> l(174); for (auto& v : l) v = 3.0f * g(rng); const auto r = osd::decode(l.data()); if (r.ok && r.hard_errors <= 36) ++ok; }
    CHECK(ok <= 1);                                                                       // the CRC lets about 1 in 8000 OSD attempts through; the 36-error rule removes those
}
TEST(osd_decoder_finds_no_messages_in_pure_noise_slots) {
    std::mt19937 rng(3); std::normal_distribution<float> g(0.0f, 0.1f); int falses = 0;
    for (int k = 0; k < 40; ++k) { std::vector<float> a(180000); for (auto& v : a) v = g(rng); falses += int(Ft8Decoder::decode_slot(a.data(), a.size(), "").size()); }
    CHECK_EQ(falses, 0);
}

// ---------------------------------------------------------------- a-priori decoding (WSJT-X "a2": your callsign in the first field) and the deep stage
TEST(osd_a_priori_bits_recover_more_words_and_never_a_wrong_one) {
    // Pinning the 29 bits of "M0OML" + i3 (as the a2 pass does) must recover clearly more noisy words than a plain decode, and only the word that was sent.
    std::mt19937 rng(21); const auto cw = codeword("M0OML G4ABC -12"); int plain = 0, ap = 0, wrong = 0; const int trials = 150;
    for (int t = 0; t < trials; ++t) {
        auto l = channel(cw, 1.0, 1.05, rng); const auto r0 = osd::decode(l.data(), 2, 2);
        float apmag = 0; for (float v : l) apmag = std::max(apmag, std::fabs(v)); apmag *= 1.1f; uint8_t mask[174] = {0};
        for (int i = 0; i < 29; ++i) { mask[i] = 1; l[size_t(i)] = cw[size_t(i)] ? apmag : -apmag; }
        mask[74] = mask[75] = mask[76] = 1; l[74] = -apmag; l[75] = -apmag; l[76] = apmag;
        const auto r1 = osd::decode(l.data(), 2, 2, mask);
        plain += r0.ok; if (r1.ok) { ++ap; for (size_t i = 0; i < 174; ++i) if (r1.bits[i] != cw[i]) { ++wrong; break; } }
    }
    CHECK(ap >= plain + trials / 10); CHECK_EQ(wrong, 0);
}
TEST(ft8_deep_stage_decodes_weak_signals_and_marks_a_priori_decodes) {
    int got = 0; const int n = 8;                                                          // -20 dB (2500 Hz): the deep stage (fine sync, 5 soft-bit sets) decodes almost all
    for (int seed = 0; seed < n; ++seed) {
        auto x = synth_ft8("CQ G4ABC IO91", 1200, 12000, 0.3, 0.5); const double s = std::sqrt(0.045 / (std::pow(10.0, -20.0 / 10.0) * 2500.0 / 6000.0));
        std::mt19937 rng(unsigned(seed) * 7 + 1); std::normal_distribution<float> g(0.0f, float(s)); for (auto& v : x) v += g(rng);
        for (auto& m : Ft8Decoder::decode_slot(x.data(), x.size(), "", false, "M0OML")) if (m.text == "CQ G4ABC IO91") ++got;
    }
    CHECK(got >= n - 1);
    Ft8Message m; m.time = "120000"; m.snr = -22; m.dt = 0.1; m.freq = 1200; m.text = "M0OML G4ABC -12"; m.ap = 2;
    CHECK_EQ(Ft8Decoder::format(m), std::string("120000 -22  0.1 1200 ~  M0OML G4ABC -12 a2\n"));
}
TEST(ft8_your_callsign_never_appears_in_noise) {
    std::mt19937 rng(5); std::normal_distribution<float> g(0.0f, 0.1f); int falses = 0;
    for (int k = 0; k < 40; ++k) { std::vector<float> a(180000); for (auto& v : a) v = g(rng); falses += int(Ft8Decoder::decode_slot(a.data(), a.size(), "", false, "M0OML").size()); }
    CHECK_EQ(falses, 0);
}
