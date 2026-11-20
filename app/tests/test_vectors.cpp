// The numbers in protocol/test-vectors.json (documented in protocol/*.md), checked against the C++ code.
// tools/tests/test_vectors.py checks the same file against an independent implementation of the documented rules.
#include <cstring>
#include "fireside/atu.h"
#include "fireside/fw_iso.h"
#include "fireside/pal.h"
#include "fireside/protocol.h"
#include "fireside/tuning.h"
#include "fireside/tx_meter.h"
#include "fake_dice.h"
using namespace fireside;
using namespace fakedice;

namespace {
std::string hex(const uint8_t* p, size_t n) { static const char* d = "0123456789ABCDEF"; std::string s; for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
std::string hex(const std::vector<uint8_t>& v) { return hex(v.data(), v.size()); }
}  // namespace

TEST(vectors_mailbox_command_blocks) {
    FakeDiceBus bus; PalMailbox mb(bus, ByteOrder::Big, 1.0, 0);
    (void)mb.request(1200, 0, 0, true, 1.0);
    CHECK_EQ(bus.writes[0].first, uint64_t(0xFFFFE0001000ULL)); CHECK_EQ(hex(bus.writes[0].second), std::string("800004B0" "00000000" "00000000"));
    mb.request(1320, 1, 0, false, 1.0);
    CHECK_EQ(hex(bus.writes[1].second), std::string("80000528" "00000001" "00000000"));
}
TEST(vectors_sysex_frames) {
    CHECK_EQ(hex(encode_request(1, 1200, 0, 0)), std::string("F00000413031303030303030423030343030303030303030303030303030303030303030F7"));
    CHECK_EQ(encode_request(0x1234, 1320, 1, 0).size(), size_t(37));
}
TEST(vectors_tuning_word) {
    CHECK_EQ(freq_to_tw(14.074), 0x0734B513u);
    float f = 14.074f; uint32_t b; std::memcpy(&b, &f, 4); CHECK_EQ(b, 0x41612F1Bu);
}
TEST(vectors_cip_packets_and_audio_quadlets) {
    AmdtpEncoder e(8, false, 0x02, 1); uint8_t h[8]; std::vector<uint8_t> pl;
    std::vector<int32_t> f(8 * 8, 0); f[0] = int32_t(0x12345678);
    e.packet(8, 0x1234, f.data(), h, pl);
    CHECK_EQ(hex(h, 8), std::string("0109000090021234")); CHECK_EQ(hex(pl.data(), 4), std::string("40123456")); CHECK_EQ(hex(pl.data() + 32, 4), std::string("80000000"));
    e.packet(8, 0x1234, nullptr, h, pl); CHECK_EQ(hex(h, 8), std::string("0109000890021234"));
    e.packet(0, 0x1234, nullptr, h, pl); CHECK_EQ(hex(h, 8), std::string("0109001090FFFFFF"));
    std::vector<int32_t> g(8 * 8, 0); g[0] = int32_t(0xFFFFFF00u); g[8] = int32_t(0x80000000u);
    e.packet(8, 0, g.data(), h, pl); CHECK_EQ(hex(pl.data(), 4), std::string("40FFFFFF")); CHECK_EQ(hex(pl.data() + 36, 4), std::string("40800000"));
}
TEST(vectors_dual_wire_block) {
    AmdtpDecoder d(8, true); std::vector<uint8_t> pkt = {0x01, 0x11, 0x00, 0x00, 0x90, 0x04, 0x20, 0x00};   // CIP: SID 1, DBS 17, DBC 0, FDF 0x04
    auto put = [&](uint32_t q) { for (int i = 3; i >= 0; --i) pkt.push_back(uint8_t(q >> (8 * i))); };
    put(0x40000001); put(0x40000002); put(0x40000003); put(0x40000004); for (int i = 0; i < 12; ++i) put(0x40000000); put(0x80000000);
    std::vector<int32_t> out; d.packet(pkt.data(), pkt.size(), 0, out);
    CHECK_EQ(out.size(), size_t(16));                                  // two frames of eight channels
    CHECK_EQ(out[0], int32_t(1 << 8)); CHECK_EQ(out[1], int32_t(3 << 8));        // frame t: channel 0 = slot 0, channel 1 = slot 2
    CHECK_EQ(out[8], int32_t(2 << 8)); CHECK_EQ(out[9], int32_t(4 << 8));        // frame t+1: slots 1 and 3
}
TEST(vectors_atu_results) {
    CHECK(!atu::parse(0x00FFFFFF).has_value());
    auto m = atu::parse(0x00001D01); CHECK(m.has_value()); CHECK_EQ(int(m->code), 1); CHECK_EQ(int(m->b2), 29); CHECK_EQ(int(m->remaining), 0);
    atu::Status s; atu::apply(s, *atu::parse(0x00001D01)); CHECK_EQ(*s.ind, 29);
    atu::apply(s, *atu::parse(0x00A60205)); CHECK_NEAR(*s.fwd_w, 6.78, 1e-9);
}
TEST(vectors_pa_meter) {
    txm::PaCal c; c.valid = true; c.bridge[5] = {0.2, 0.3, 0.45, 0.65, 0.9, 1.9}; c.rev_corr[5] = 0.5;
    const auto r = txm::read(c, 5, 1000, 300);
    CHECK_NEAR(r.fwd_w, 8.8643, 1e-3); CHECK_NEAR(r.rev_w, 0.4191, 1e-3); CHECK(r.swr.has_value()); CHECK_NEAR(*r.swr, 1.5557, 1e-3);
}
