// The receive-antenna path (RxAntenna): only SET_RX1_ANT, only 1..4, armed-only, idempotent, fault-disarming, FireWire only, plus the band table that keys the Antennas tab.
#include "fake_dice.h"
#include "fake_radio.h"
#include "fireside/rxant.h"
#include "fireside/session.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
struct ARig {
    FakeDiceBus bus; PalMailbox mb; Flex fx; bool ready; RxAntenna ant;
    explicit ARig(double mailbox_timeout = 1.0)
        : bus(), mb(bus, ByteOrder::Big, mailbox_timeout, 0), fx(mb, [] { Safety s; s.allow_write = true; return s; }()),
          ready([this] { flex_defaults(bus); return mb.verify(); }()), ant(fx) {}
};
const uint32_t kAnt = 1278;                                                             // RDAL_OP_SET_RX1_ANT
}  // namespace

TEST(rxant_names_and_wire_values) {
    CHECK(opv("RDAL_OP_SET_RX1_ANT") == kAnt);
    CHECK(rx_ant_from_name("ANT1") == RxAnt::Ant1); CHECK(rx_ant_from_name("ant3") == RxAnt::Ant3); CHECK(rx_ant_from_name("RX1 IN") == RxAnt::Rx1In); CHECK(rx_ant_from_name("RX1IN") == RxAnt::Rx1In);
    CHECK(!rx_ant_from_name("NC").has_value()); CHECK(!rx_ant_from_name("").has_value()); CHECK(!rx_ant_from_name("ANT4").has_value()); CHECK(!rx_ant_from_name("SIG_GEN").has_value());
    CHECK_EQ(int(RxAnt::Ant1), 1); CHECK_EQ(int(RxAnt::Ant2), 2); CHECK_EQ(int(RxAnt::Ant3), 3); CHECK_EQ(int(RxAnt::Rx1In), 4);                        // PowerSDR FWCAnt
    CHECK_EQ(std::string(rx_ant_name(RxAnt::Rx1In)), std::string("RX1IN"));
}
TEST(rxant_sends_only_set_rx1_ant_with_one_to_four) {
    ARig r; r.ant.arm("ANT1");
    CHECK(r.ant.request(RxAnt::Ant1) == AntResult::Unchanged); CHECK(write_ops(r.bus).empty());           // the start-up already selected it
    CHECK(r.ant.request(RxAnt::Ant2) == AntResult::Sent); CHECK(r.ant.request(RxAnt::Rx1In) == AntResult::Sent); CHECK(r.ant.request(RxAnt::Ant3) == AntResult::Sent);
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(3));
    CHECK(w[0] == (Wr{kAnt, 2, 0})); CHECK(w[1] == (Wr{kAnt, 4, 0})); CHECK(w[2] == (Wr{kAnt, 3, 0}));
    CHECK(r.ant.current() == RxAnt::Ant3);
}
TEST(rxant_asking_for_the_current_antenna_sends_nothing_and_nc_start_is_unknown) {
    ARig r; r.ant.arm("NC"); CHECK(!r.ant.current().has_value());
    CHECK(r.ant.request(RxAnt::Ant1) == AntResult::Sent); CHECK(r.ant.request(RxAnt::Ant1) == AntResult::Unchanged); CHECK_EQ(write_ops(r.bus).size(), size_t(1));
    CHECK(r.ant.request(RxAnt::Ant1, true) == AntResult::Sent); CHECK_EQ(write_ops(r.bus).size(), size_t(2)); CHECK(write_ops(r.bus).back() == (Wr{kAnt, 1, 0}));   // forced: written again
}
TEST(rxant_refuses_until_armed_and_needs_firewire_and_allow_write) {
    ARig r; CHECK(r.ant.request(RxAnt::Ant2) == AntResult::NotArmed); CHECK(write_ops(r.bus).empty());
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Safety s; s.allow_write = true; Flex midi_fx(link, s);
    CHECK_THROWS(RxAntenna(midi_fx), StartupError);                                     // MIDI is never a route for this
    ARig ro; ro.fx.safety().allow_write = false; CHECK_THROWS(ro.ant.arm("ANT1"), SafetyError);
    r.ant.arm("ANT1"); r.ant.disarm(); CHECK(r.ant.request(RxAnt::Ant2) == AntResult::NotArmed); CHECK(write_ops(r.bus).empty());
    CHECK(r.ant.request(static_cast<RxAnt>(0)) == AntResult::NotArmed); CHECK(r.ant.request(static_cast<RxAnt>(9)) == AntResult::NotArmed);   // a bad cast never reaches the wire
}
TEST(rxant_mailbox_fault_disarms_until_rearmed) {
    ARig r(0.02); const size_t baseline = r.bus.writes.size(); r.ant.arm("ANT1"); r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});
    CHECK(r.ant.request(RxAnt::Ant2) == AntResult::Fault); CHECK(!r.ant.armed()); CHECK(!r.ant.last_error().empty()); CHECK(r.ant.current() == RxAnt::Ant1);   // still believes ANT1
    CHECK_EQ(r.bus.writes.size(), baseline);                                           // never wrote over a busy mailbox
    r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0}); CHECK(r.ant.request(RxAnt::Ant2) == AntResult::NotArmed);
}
TEST(session_rx_antenna_only_while_started_and_follows_the_startup_choice) {
    FakeDiceBus bus; flex_defaults(bus); RadioSession session([&](const std::string&) { return std::unique_ptr<BusHandle>(new FakeHandle(bus, false)); });
    SessionOptions o; o.path = "/dev/fw1"; o.allow_write = true; o.boot_wait_s = 0;
    CHECK(session.set_rx_antenna(RxAnt::Ant2) == AntResult::NotArmed); session.connect(o); CHECK(session.set_rx_antenna(RxAnt::Ant2) == AntResult::NotArmed); CHECK(write_ops(bus).empty());
    session.start(7.1, "ANT2"); const size_t after_start = write_ops(bus).size(); CHECK(session.rx_antenna() == RxAnt::Ant2);
    CHECK(session.set_rx_antenna(RxAnt::Ant2) == AntResult::Unchanged); CHECK_EQ(write_ops(bus).size(), after_start);
    CHECK(session.set_rx_antenna(RxAnt::Ant3) == AntResult::Sent); auto w = write_ops(bus); CHECK_EQ(w.size(), after_start + 1); CHECK(w.back() == (Wr{kAnt, 3, 0}));
    CHECK(session.reassert_rx_antenna() == AntResult::Sent); w = write_ops(bus); CHECK_EQ(w.size(), after_start + 2); CHECK(w.back() == (Wr{kAnt, 3, 0}));   // after power-up: the same antenna again
    session.shutdown(); CHECK(session.set_rx_antenna(RxAnt::Ant1) == AntResult::NotArmed); CHECK(session.reassert_rx_antenna() == AntResult::NotArmed);
    CHECK_EQ(write_ops(bus).size(), after_start + 2 + 3);                                // only the shutdown's three writes followed
    session.start(7.1, "ANT1"); CHECK(session.rx_antenna() == RxAnt::Ant1); session.disconnect(); CHECK(!session.rx_antenna().has_value());
}
TEST(rx_bands_cover_the_uk_amateur_bands_and_everything_else_is_other) {
    size_t n = 0; const RxBand* b = rx_bands(n); CHECK_EQ(n, size_t(12)); CHECK_EQ(std::string(b[0].id), std::string("lfmw"));
    CHECK_EQ(rx_band_index(693e3), 0); CHECK_EQ(std::string(b[rx_band_index(7.074e6)].id), std::string("40m")); CHECK_EQ(std::string(b[rx_band_index(14.074e6)].id), std::string("20m"));
    CHECK_EQ(std::string(b[rx_band_index(5.3585e6)].id), std::string("60m")); CHECK_EQ(std::string(b[rx_band_index(50.313e6)].id), std::string("6m")); CHECK_EQ(std::string(b[rx_band_index(29.6e6)].id), std::string("10m"));
    CHECK_EQ(rx_band_index(5.0e6), -1); CHECK_EQ(rx_band_index(9.6e6), -1); CHECK_EQ(rx_band_index(7.3e6), -1); CHECK_EQ(rx_band_index(1.805e6), -1); CHECK_EQ(rx_band_index(0), -1); CHECK_EQ(rx_band_index(65e6), -1);
    for (size_t i = 0; i + 1 < n; ++i) CHECK(b[i].hi_hz < b[i + 1].lo_hz);                // sorted, disjoint
}
