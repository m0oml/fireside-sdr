// Hybrid BP + OSD decoder for the FT8/FT4 (174,91) LDPC code (internal to fireside_core).
// After WSJT-X 3.x lib/ft8/decode174_91.f90 and osd174_91.f90 (GPL v3, Steve Franke K9AN / Joe Taylor K1JT): reimplemented in C++ from that source.
// ft8_lib (which we use for sync, LLRs and BP) stops at belief propagation; WSJT-X then runs an ordered-statistics decoder on BP's accumulated beliefs,
// which is where most of its decodes below about -18 dB come from.
#pragma once
#include <cstdint>

namespace fireside::decode::osd {
constexpr int N = 174, K = 91;
struct Result {
    bool ok = false;            // a codeword with a good CRC
    bool by_osd = false;        // found by OSD (BP alone failed): the caller applies WSJT-X's extra plausibility checks
    int hard_errors = -1;       // bits where the codeword disagrees with the hard decisions of the decoder's input (WSJT-X nharderrors)
    float dmin = 0;             // weighted distance to the channel LLRs
    uint8_t bits[N] = {0};      // the codeword, message (77) + CRC (14) + parity (83), 0/1
};
// llr: 174 channel log-likelihoods, positive = bit 1 (ft8_lib's convention, normalised as ft8_lib does). max_osd: 0 = BP only, 1..3 = OSD on the BP sums
// after that many iterations (WSJT-X uses 2). norder: OSD depth (WSJT-X FT8 uses 2).
// apmask (optional, 174 entries): 1 = a-priori bit (its llr is pinned by the caller): BP does not update it and OSD never flips it.
Result decode(const float* llr, int max_osd = 2, int norder = 2, const uint8_t* apmask = nullptr);
bool crc_ok(const uint8_t* bits91);                         // CRC14 over the 77 message bits (zero-extended to 82), as ft8_lib / WSJT-X
}  // namespace fireside::decode::osd
