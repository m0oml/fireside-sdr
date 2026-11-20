// Mirror of tools/tests/test_pal.py: PAL mailbox over a fake DICE register file, plus cdev packing over a fake syscall layer.
#include <cstring>
#include <deque>
#include <memory>
#include <linux/firewire-cdev.h>
#include "fireside/client.h"
#include "fireside/firewire.h"
#include "fireside/identify.h"
#include "fireside/pal.h"
#include "fireside/protocol.h"
#include "fake_dice.h"
using namespace fireside;
using namespace fakedice;

namespace {
}  // namespace

TEST(pal_read_op_wire_format_and_result) {
    FakeDiceBus bus; PalMailbox mb(bus, ByteOrder::Big, 1.0, 0);
    CHECK_EQ(mb.request(1221, 7, 9, true, 1.0), 0u);
    CHECK_EQ(bus.writes[0].first, uint64_t(0xFFFFE0001000ULL));
    CHECK(bus.writes[0].second == be({0x80000000u | 1221, 7, 9}));
}
TEST(pal_waits_for_idle_before_writing) {
    FakeDiceBus bus(2, 3); PalMailbox mb(bus, ByteOrder::Big, 1.0, 0);
    mb.request(1220, 0, 0, true, 1.0);
    CHECK(bus.reads >= 3 + 1);
    CHECK_EQ(bus.writes.size(), size_t(1));
}
TEST(pal_stuck_busy_times_out_instead_of_hanging) {
    FakeDiceBus bus(2, 1, true); PalMailbox mb(bus, ByteOrder::Big, 0.05, 0);
    CHECK_THROWS(mb.request(1221, 0, 0, true, 1.0), TimeoutError);
    CHECK(bus.writes.empty());                                  // never wrote over a busy mailbox
}
TEST(pal_set_does_not_wait_but_next_command_does) {
    FakeDiceBus bus(3); PalMailbox mb(bus, ByteOrder::Big, 1.0, 0);
    CHECK_EQ(mb.request(1277, 2, 0, false, 0), 0u);             // SET_ANT
    CHECK_EQ(mb.request(1220, 0, 0, true, 1.0), 0xA1B2C3D4u);
    CHECK(bus.executed == (std::vector<uint32_t>{1277, 1220}));
}
TEST(pal_little_endian_switch) {
    struct LeBus : IBus {
        std::vector<std::vector<uint8_t>> writes;
        std::vector<uint8_t> read(uint64_t, size_t) override { return {0, 0, 0, 0}; }
        void write(uint64_t, const std::vector<uint8_t>& d) override { writes.push_back(d); }
    } bus;
    PalMailbox mb(bus, ByteOrder::Little, 1.0, 0);
    mb.request(1221, 1, 0, false, 0);
    CHECK(bus.writes[0] == le({0x80000000u | 1221, 1, 0}));
}
TEST(pal_event_decode) {
    FakeDiceBus bus; bus.mem[kPalEvent] = be({0x01 | 0x08});
    auto e = PalMailbox(bus).read_event();
    CHECK_EQ(e.raw, 9u); CHECK(e.bits["dot"] && e.bits["mic_ptt"]); CHECK(!e.bits["dash"]);
    CHECK_EQ(e.bits.size(), size_t(7));
    CHECK(bus.writes.empty());                                  // reading events never writes
}
TEST(pal_flex_client_over_mailbox) {
    FakeDiceBus bus; PalMailbox mb(bus, ByteOrder::Big, 1.0, 0);
    Safety s; s.allow_write = true; Flex fx(mb, s);
    CHECK_EQ(fx.get("GET_MODEL"), 0u);
    fx.set("SET_RX1_FREQ", 7.0745);
    const auto& blk = bus.writes.back().second;
    CHECK_EQ(be32(blk, 0), 0x80000000u | 1244);
    CHECK_NEAR(as_f32(be32(blk, 4)), 7.0745, 1e-4);
    CHECK_THROWS(fx.set("SET_MOX", 1), SafetyError);
}
TEST(pal_swapped_word_never_aliases_a_real_opcode) {
    const uint32_t fw_op = opv("RDAL_OP_GET_FIRMWARE_REV");
    const uint32_t expect = be32(le({fw_op | kPalBusy})) & 0x7FFFFFFFu;     // independent derivation: the other-order reading of our CMD word
    CHECK_EQ(swapped_opcode_word(fw_op), expect);
    CHECK_EQ(swapped_opcode_word(fw_op), 0x30040080u);                       // value quoted in the Python doc/tests
    CHECK(find_opcode(swapped_opcode_word(fw_op)) == nullptr);
}
TEST(pal_swapped_word_never_aliases_any_real_opcode) {
    for (const OpInfo& op : kOps)       // true for EVERY opcode, so a wrong-order write is inert too
        CHECK(find_opcode(swapped_opcode_word(op.value)) == nullptr);
}

namespace {
// A radio whose mailbox is little-endian (mirror of LittleEndianBus in tools/tests/test_hardware_day.py).
struct LittleEndianBus : IBus {
    bool wedge; std::map<uint64_t, std::vector<uint8_t>> mem; std::vector<uint32_t> seen;
    explicit LittleEndianBus(bool wedge_) : wedge(wedge_) { mem[kPalCmd] = le({0}); mem[kPalResult] = le({0}); mem[kPalEvent] = le({0}); }
    std::vector<uint8_t> read(uint64_t off, size_t) override { return mem.at(off); }
    void write(uint64_t, const std::vector<uint8_t>& d) override {
        const uint32_t w = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24, code = w & 0x7FFFFFFFu;
        seen.push_back(code);
        if (code == opv("RDAL_OP_GET_FIRMWARE_REV")) { mem[kPalResult] = le({0x02010408}); mem[kPalCmd] = le({code}); }
        else if (wedge) mem[kPalCmd] = le({w});                  // unknown opcode: busy bit stays set
        else mem[kPalCmd] = le({code});                          // unknown opcode: completes with nothing done
    }
};
}  // namespace

TEST(pal_probe_big_endian_radio_only_reads) {
    FakeDiceBus bus(1); bus.answers[opv("RDAL_OP_GET_FIRMWARE_REV")] = 0x02010408;
    auto r = probe_order(bus, 0.2);
    CHECK(r.order && *r.order == ByteOrder::Big);
    CHECK_EQ(bus.executed.size(), size_t(1));
    for (auto code : bus.executed) CHECK(find_opcode(code) && is_read(*find_opcode(code)));
}
TEST(pal_probe_little_endian_radio_and_wrong_first_guess_is_harmless) {
    LittleEndianBus bus(false);
    auto r = probe_order(bus, 0.2);
    CHECK(r.order && *r.order == ByteOrder::Little);
    CHECK_EQ(bus.seen.size(), size_t(2));
    CHECK_EQ(bus.seen[0], 0x30040080u);                          // the wrong guess decodes as a non-opcode
    CHECK(find_opcode(bus.seen[0]) == nullptr);
    CHECK_EQ(bus.seen[1], opv("RDAL_OP_GET_FIRMWARE_REV"));
}
TEST(pal_probe_wedging_firmware_fails_cleanly_without_writing_into_a_busy_mailbox) {
    LittleEndianBus bus(true);
    auto r = probe_order(bus, 0.05);
    CHECK(!r.order);
    CHECK(bus.seen == (std::vector<uint32_t>{0x30040080u}));     // second attempt must not write while the mailbox is busy
    bool hint = false; for (auto& n : r.notes) if (n.find("power-cycle") != std::string::npos) hint = true;
    CHECK(hint);
}
TEST(pal_probe_busy_mailbox_aborts_before_trying_the_other_order) {
    FakeDiceBus bus(1, 1, true); bus.mem[kPalCmd] = be({kPalBusy});                         // busy before we send anything
    auto r = probe_order(bus, 0.05);
    CHECK(!r.order); CHECK(bus.writes.empty());                                              // the other order (which would read it as idle) was never tried
    size_t timeouts = 0; bool hint = false;
    for (auto& n : r.notes) { if (n.find("TimeoutError") != std::string::npos) ++timeouts; if (n.find("busy before any command") != std::string::npos) hint = true; }
    CHECK_EQ(timeouts, size_t(1)); CHECK(hint);
}
TEST(pal_probe_neither_order_works) {
    FakeDiceBus bus(1);                                          // firmware answers 0: implausible in both orders
    auto r = probe_order(bus, 0.2);
    CHECK(!r.order); CHECK_EQ(r.notes.size(), size_t(2));
}

// ---- cdev packing over a fake syscall layer ("firewire.py can't run here: check its ioctl packing against the C layout") ----
namespace {
struct FakeSys : FwSys {
    using Responder = std::function<std::vector<uint8_t>(const fw_cdev_send_request&)>;
    std::vector<std::pair<unsigned long, std::vector<uint8_t>>> sent;
    std::deque<Responder> events;
    std::deque<std::vector<uint8_t>> ready;
    int ioctl(int, unsigned long req, void* arg) override {
        if (req == FW_CDEV_IOC_SEND_REQUEST) {
            auto* rq = static_cast<fw_cdev_send_request*>(arg);
            sent.emplace_back(req, std::vector<uint8_t>(static_cast<uint8_t*>(arg), static_cast<uint8_t*>(arg) + sizeof *rq));
            ready.push_back(events.front()(*rq)); events.pop_front();
        }
        return 0;
    }
    ssize_t read(int, void* buf, size_t) override { auto e = ready.front(); ready.pop_front(); std::memcpy(buf, e.data(), e.size()); return ssize_t(e.size()); }
    bool wait_readable(int, double) override { return !ready.empty(); }
};
}  // namespace

static std::vector<uint8_t> response_event(const fw_cdev_send_request& rq, uint32_t rcode, std::vector<uint8_t> data = {}) {
    std::vector<uint8_t> ev(20); const uint32_t type = FW_CDEV_EVENT_RESPONSE, len = uint32_t(data.size());
    std::memcpy(&ev[0], &rq.closure, 8); std::memcpy(&ev[8], &type, 4); std::memcpy(&ev[12], &rcode, 4); std::memcpy(&ev[16], &len, 4);
    ev.insert(ev.end(), data.begin(), data.end()); return ev;
}
static std::unique_ptr<FwDevice> make_dev(FakeSys& sys) {
    auto d = std::make_unique<FwDevice>(sys, 99, "/dev/fwX", false);
    d->set_state_for_test(7); return d;
}

TEST(cdev_quadlet_read) {
    FakeSys sys; sys.events.push_back([](auto& rq) { return response_event(rq, 0, {0x80, 0x00, 0x04, 0xc5}); });
    auto dev = make_dev(sys);
    CHECK(dev->read(kPalCmd, 4) == (std::vector<uint8_t>{0x80, 0x00, 0x04, 0xc5}));
    CHECK_EQ(sys.sent[0].first, (unsigned long)FW_CDEV_IOC_SEND_REQUEST);
    CHECK_EQ(sys.sent[0].second.size(), size_t(40));
    fw_cdev_send_request rq; std::memcpy(&rq, sys.sent[0].second.data(), sizeof rq);
    CHECK_EQ(rq.tcode, kTcodeReadQuadlet); CHECK_EQ(rq.length, 4u); CHECK_EQ(rq.offset, uint64_t(0xFFFFE0001000ULL)); CHECK_EQ(rq.generation, 7u);
}
TEST(cdev_block_write_tcode) {
    FakeSys sys; sys.events.push_back([](auto& rq) { return response_event(rq, 0); });
    auto dev = make_dev(sys);
    dev->write(kPalCmd, std::vector<uint8_t>(12, 0));
    fw_cdev_send_request rq; std::memcpy(&rq, sys.sent[0].second.data(), sizeof rq);
    CHECK_EQ(rq.tcode, kTcodeWriteBlock); CHECK_EQ(rq.length, 12u);
}
TEST(cdev_rcode_error_raised) {
    FakeSys sys; sys.events.push_back([](auto& rq) { return response_event(rq, 7); });
    auto dev = make_dev(sys);
    try { dev->read(kPalCmd, 4); FAIL_AT("no exception"); }
    catch (const FwError& e) { CHECK(std::string(e.what()).find("address error") != std::string::npos); }
}
TEST(cdev_bus_reset_then_retry_on_new_generation) {
    FakeSys sys;
    sys.events.push_back([](auto&) {
        fw_cdev_event_bus_reset r{}; r.type = FW_CDEV_EVENT_BUS_RESET; r.node_id = 0xFFC1; r.local_node_id = 0xFFC0; r.generation = 8;
        std::vector<uint8_t> ev(sizeof r); std::memcpy(ev.data(), &r, sizeof r); return ev; });
    sys.events.push_back([](auto& rq) { return response_event(rq, 0, {0, 0, 0, 3}); });
    auto dev = make_dev(sys);
    CHECK(dev->read(kPalResult, 4) == (std::vector<uint8_t>{0, 0, 0, 3}));
    std::vector<uint32_t> gens;
    for (auto& s : sys.sent) { fw_cdev_send_request rq; std::memcpy(&rq, s.second.data(), sizeof rq); gens.push_back(rq.generation); }
    CHECK(gens == (std::vector<uint32_t>{7, 8}));
    CHECK_EQ(dev->node_id(), 0xFFC1u);
}
TEST(cdev_stale_generation_rcode_retries_and_gives_up) {
    FakeSys sys; for (int i = 0; i < 3; ++i) sys.events.push_back([](auto& rq) { return response_event(rq, 19); });
    auto dev = make_dev(sys);
    CHECK_THROWS(dev->read(kPalCmd, 4), FwError);
    CHECK_EQ(sys.sent.size(), size_t(3));
}
TEST(cdev_no_response_times_out) {
    struct Silent : FakeSys { int ioctl(int, unsigned long, void*) override { return 0; } bool wait_readable(int, double) override { return false; } } sys;
    auto dev = std::make_unique<FwDevice>(sys, 99, "/dev/fwX", false); dev->timeout_s = 0.05;
    CHECK_THROWS(dev->read(kPalCmd, 4), TimeoutError);
}
TEST(cdev_get_info_parses_rom_and_guid) {
    struct InfoSys : FakeSys {
        int ioctl(int, unsigned long req, void* arg) override {
            CHECK_EQ(req, (unsigned long)FW_CDEV_IOC_GET_INFO);
            auto* gi = static_cast<fw_cdev_get_info*>(arg);
            CHECK_EQ(gi->version, 4u);
            const uint32_t rom[6] = {0, 0, 0, 0x00112233u, 0x44556677u, 0};          // the kernel copies ROM quadlets in host byte order
            std::memcpy(reinterpret_cast<void*>(uintptr_t(gi->rom)), rom, sizeof rom); gi->rom_length = sizeof rom;
            auto* r = reinterpret_cast<fw_cdev_event_bus_reset*>(uintptr_t(gi->bus_reset));
            r->node_id = 0xFFC0; r->local_node_id = 0xFFC0; r->generation = 5;
            return 0;
        }
    } sys;
    FwDevice dev(sys, 99, "/dev/fwX", true);
    CHECK(dev.guid() && *dev.guid() == 0x0011223344556677ULL);
    CHECK(dev.is_local()); CHECK_EQ(dev.generation(), 5u);
}
TEST(cdev_list_devices_ignores_non_fw_names) {
    CHECK(list_fw_devices("/nonexistent-dir-for-fireside-test").empty());
}
