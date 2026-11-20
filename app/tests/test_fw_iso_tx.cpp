// The transmit direction of the direct FireWire stream (analysis/duplex-iso-tx.md): AM824 packets for the radio's RX[0] stream and
// media-clock replay of the radio's own packet sequence.
#include "fireside/fw_iso.h"
#include "test_util.h"
using namespace fireside;
namespace { uint32_t be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; } }

TEST(fw_iso_tx_encoder_48k_header_labels_and_dbc) {
    AmdtpEncoder e(8, false, 0x02, 0xC3); uint8_t h[8]; std::vector<uint8_t> pl;
    std::vector<int32_t> f(8 * 8); for (int b = 0; b < 8; ++b) for (int c = 0; c < 8; ++c) f[size_t(b * 8 + c)] = int32_t(uint32_t((b << 4 | c) + 1) << 8);
    f[0] = int32_t(0x80000000u);                                                    // the most negative sample keeps its sign bits in 24 bits
    e.packet(8, 0x1234, f.data(), h, pl);
    CHECK_EQ(be(h), uint32_t(0x03 << 24 | 9 << 16 | 0));                            // SID (node & 0x3f), DBS 9, DBC 0
    CHECK_EQ(be(h + 4), uint32_t(0x80000000u | 0x10u << 24 | 0x02u << 16 | 0x1234)); // EOH, FMT AM824, FDF 48 kHz, SYT
    CHECK_EQ(pl.size(), size_t(8 * 9 * 4));
    CHECK_EQ(be(pl.data()), 0x40800000u);                                           // label 0x40 + 24-bit sample
    CHECK_EQ(be(pl.data() + 4 * 1), 0x40000002u); CHECK_EQ(be(pl.data() + 4 * 8), 0x80000000u);   // channel 1; the MIDI slot: no data
    CHECK_EQ(be(pl.data() + 9 * 4 + 4 * 7), 0x40000018u);                           // block 1, channel 7
    e.packet(0, 0x5678, nullptr, h, pl);                                            // NO-DATA: CIP only, FDF/SYT no-info, DBC = the next block
    CHECK(pl.empty()); CHECK_EQ(be(h) & 0xFF, 8u); CHECK_EQ(be(h + 4) & 0x00FFFFFF, 0xFFFFFFu);
    e.packet(8, 0x0100, nullptr, h, pl); CHECK_EQ(be(h) & 0xFF, 8u); CHECK_EQ(be(pl.data()), 0x40000000u);   // silence
    for (int i = 0; i < 40; ++i) e.packet(8, 0, nullptr, h, pl);
    CHECK_EQ(int(e.dbc()), (8 * 42) % 256);                                          // wraps at 256
}
TEST(fw_iso_tx_encoder_192k_dual_wire_round_trips_through_the_decoder) {
    AmdtpEncoder e(8, true, 0x04, 1); uint8_t h[8]; std::vector<uint8_t> pl;
    std::vector<int32_t> f(32 * 8); for (size_t i = 0; i < f.size(); ++i) f[i] = int32_t(uint32_t(i * 977 + 13) << 8);
    e.packet(16, 0x2000, f.data(), h, pl);
    CHECK_EQ((be(h) >> 16) & 0xFF, 17u); CHECK_EQ((be(h + 4) >> 16) & 0xFF, 0x04u);
    std::vector<uint8_t> pkt(h, h + 8); pkt.insert(pkt.end(), pl.begin(), pl.end());
    pkt[0] &= 0x3F;                                                                 // the decoder wants quadlet 0's top bits 00 (they are, SID < 64)
    AmdtpDecoder d(8, true); std::vector<int32_t> out; d.packet(pkt.data(), pkt.size(), 5, out);
    CHECK_EQ(out.size(), f.size()); CHECK(out == f);                                // same frames, same order: dual-wire slots agree both ways
    CHECK_EQ(d.stats().rejected, 0u);
}
TEST(fw_iso_tx_syt_shift_and_cycle_arithmetic) {
    CHECK_EQ(syt_shift(0x1ABC, 3), uint16_t(0x4ABC)); CHECK_EQ(syt_shift(0xEABC, 3), uint16_t(0x1ABC)); CHECK_EQ(syt_shift(kSytNoInfo, 5), kSytNoInfo);
    CHECK_EQ(cycle_add(7999, 2), 1); CHECK_EQ(cycle_diff(1, 7999), 2); CHECK_EQ(cycle_diff(7999, 1), -2);
}
TEST(fw_iso_tx_replay_one_out_per_cycle_with_gap_fill_and_drops) {
    ReplayScheduler r(96); std::vector<OutPacket> out;
    r.received(7990, 8, 0x1100, out); CHECK_EQ(out.size(), size_t(1)); CHECK_EQ(out[0].cycle, (7990 + 96) % 8000); CHECK_EQ(out[0].blocks, 8); CHECK_EQ(out[0].syt, syt_shift(0x1100, 96));
    out.clear(); r.received(7991, 0, kSytNoInfo, out); CHECK_EQ(out.size(), size_t(1)); CHECK_EQ(out[0].blocks, 0);
    out.clear(); r.received(7994, 8, 0x2000, out);                                  // two receive cycles missing: NO-DATA for them, then this one
    CHECK_EQ(out.size(), size_t(3)); CHECK_EQ(out[0].blocks, 0); CHECK_EQ(out[1].blocks, 0); CHECK_EQ(out[2].blocks, 8);
    for (size_t i = 0; i < out.size(); ++i) CHECK_EQ(out[i].cycle, cycle_add(7992 + 96, int(i)));   // consecutive cycles
    CHECK_EQ(r.filled(), 2u);
    out.clear(); r.received(7994, 8, 0x2000, out); CHECK(out.empty()); CHECK_EQ(r.dropped(), 1u);   // a repeat: dropped
    out.clear(); r.received(cycle_add(7994, 900), 8, 0x2000, out); CHECK(out.empty()); CHECK(r.broken());   // > 100 ms missing
}
