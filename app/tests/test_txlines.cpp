// The rear TX lines as band switches (TxLines): only SET_RCA_TXn 0/1 and SET_AMP_TXn (0, 0); nothing for unused lines; idempotent; never during an over;
// the transmit path sets them and waits out the settle time; a fault disarms and blocks transmit; release switches used lines off; FireWire only.
#include <vector>
#include "fake_dice.h"
#include "fake_radio.h"
#include "fireside/session.h"
#include "fireside/txlines.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
struct FakeClock { double t = 1000; std::vector<double> sleeps; atu::Clock clock() { return {[this] { return t; }, [this](double s) { sleeps.push_back(s); t += s; }}; } };
struct LRig {
    FakeDiceBus bus; PalMailbox mb; Flex fx; bool ready; FakeClock clk; TxLines lines;
    explicit LRig(double mailbox_timeout = 1.0)
        : bus(), mb(bus, ByteOrder::Big, mailbox_timeout, 0), fx(mb, [] { Safety s; s.allow_write = true; return s; }()),
          ready([this] { flex_defaults(bus); return mb.verify(); }()), clk(), lines(fx, clk.clock()) {}
};
const uint32_t kRca1 = 1269, kRca2 = 1270, kRca3 = 1271, kAmp1 = 1298, kAmp2 = 1299;
constexpr double k80 = 3.573e6, k40 = 7.074e6, k20 = 14.074e6;
uint32_t bit(const char* id) { size_t n = 0; auto* b = rx_bands(n); for (size_t i = 0; i < n; ++i) if (std::string(b[i].id) == id) return 1u << i; return 0; }
LineConfig band80_on_tx1() { LineConfig c; c.role[0] = LineRole::Band; c.band_mask[0] = bit("80m"); return c; }
}  // namespace

TEST(txlines_opcode_numbers_and_roles) {
    CHECK_EQ(opv("RDAL_OP_SET_RCA_TX1"), kRca1); CHECK_EQ(opv("RDAL_OP_SET_RCA_TX2"), kRca2); CHECK_EQ(opv("RDAL_OP_SET_RCA_TX3"), kRca3);
    CHECK_EQ(opv("RDAL_OP_SET_AMP_TX1"), kAmp1); CHECK_EQ(opv("RDAL_OP_SET_AMP_TX2"), kAmp2);
    CHECK(line_role_from_id("band") == LineRole::Band); CHECK(line_role_from_id("amp") == LineRole::Amplifier); CHECK(line_role_from_id("xvtr") == LineRole::Transverter);
    CHECK(line_role_from_id("?") == LineRole::Unused); CHECK_EQ(std::string(line_role_id(LineRole::Band)), std::string("band"));
    auto c = band80_on_tx1(); auto w = TxLines::wanted(c, k80); CHECK(w[0] && !w[1] && !w[2]); CHECK(!TxLines::wanted(c, k40)[0]); CHECK(!TxLines::wanted(c, 4.5e6)[0]);   // outside every band: off
    c.role[0] = LineRole::Amplifier; CHECK(!TxLines::wanted(c, k80)[0]); CHECK(!TxLines::any_used(c));                                   // placeholders drive nothing
}
TEST(txlines_with_no_line_in_use_send_nothing_and_never_block_transmit) {
    LRig r; LineConfig c; c.role[1] = LineRole::Amplifier; c.role[2] = LineRole::Transverter; c.band_mask = {{~0u, ~0u, ~0u}}; r.lines.configure(c);
    CHECK(r.lines.for_over(k80).empty());                                              // not even armed: transmit is not affected
    r.lines.arm(); CHECK(r.lines.apply(k80) == TxLines::Result::Unchanged); CHECK(r.lines.for_over(k20).empty()); r.lines.release();
    CHECK(write_ops(r.bus).empty()); CHECK(r.clk.sleeps.empty());
}
TEST(txlines_band_line_follows_the_dial_with_only_the_reviewed_writes) {
    LRig r; r.lines.configure(band80_on_tx1()); r.lines.arm();
    CHECK(r.lines.apply(k80) == TxLines::Result::Sent);
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(2));
    CHECK(w[0] == (Wr{kAmp1, 0, 0})); CHECK(w[1] == (Wr{kRca1, 1, 0}));               // first "do not follow transmit", then the line on
    CHECK(r.lines.apply(k80 + 50e3) == TxLines::Result::Unchanged); CHECK_EQ(write_ops(r.bus).size(), size_t(2));   // same band: nothing
    CHECK(r.lines.apply(k40) == TxLines::Result::Sent); w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(3)); CHECK(w[2] == (Wr{kRca1, 0, 0}));
    CHECK(r.lines.apply(k20) == TxLines::Result::Unchanged);
    for (auto& x : write_ops(r.bus)) { CHECK(x.op == kRca1 || x.op == kAmp1); CHECK(x.d1 <= 1); CHECK_EQ(x.d2, 0u); if (x.op == kAmp1) CHECK_EQ(x.d1, 0u); }
    CHECK(!r.fx.safety().permitted("RDAL_OP_SET_AMP_TX1"));                           // the permit is only held around the write
    CHECK(r.lines.state()[0] == 0 && r.lines.state()[1] == -1 && r.lines.state()[2] == -1);   // unused lines never written
}
TEST(txlines_first_write_after_arm_states_the_line_even_when_off) {
    LRig r; r.lines.configure(band80_on_tx1()); r.lines.arm();
    CHECK(r.lines.apply(k40) == TxLines::Result::Sent);                               // unknown power-on state: explicitly off
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(2)); CHECK(w[0] == (Wr{kAmp1, 0, 0})); CHECK(w[1] == (Wr{kRca1, 0, 0}));
    r.lines.arm(); CHECK(r.lines.apply(k40) == TxLines::Result::Sent); CHECK_EQ(write_ops(r.bus).size(), size_t(4));   // a new start-up: stated again
}
TEST(txlines_no_change_while_an_over_runs_but_the_over_sets_and_settles) {
    LRig r; r.lines.configure(band80_on_tx1()); r.lines.arm(); bool busy = true; r.lines.set_busy([&] { return busy; });
    CHECK(r.lines.apply(k80) == TxLines::Result::Busy); CHECK(write_ops(r.bus).empty());
    CHECK(r.lines.for_over(k80).empty());                                              // the over itself (busy): set, then wait the settle time
    CHECK_EQ(write_ops(r.bus).size(), size_t(2)); CHECK_EQ(r.clk.sleeps.size(), size_t(1)); CHECK_NEAR(r.clk.sleeps[0], 0.05, 1e-9);
    CHECK(r.lines.for_over(k80).empty()); CHECK_EQ(r.clk.sleeps.size(), size_t(1));   // settled long ago (the fake clock advanced): no wait
    busy = false; CHECK(r.lines.apply(k40) == TxLines::Result::Sent); r.clk.t += 0.02;
    CHECK(r.lines.for_over(k40).empty()); CHECK_EQ(r.clk.sleeps.size(), size_t(2)); CHECK_NEAR(r.clk.sleeps[1], 0.03, 1e-9);   // only what is left of the 50 ms
    auto c = band80_on_tx1(); c.settle_s = 5; r.lines.configure(c); CHECK_NEAR(r.lines.config().settle_s, 1.0, 1e-12);   // clamped to 1 s
}
TEST(txlines_refuse_transmit_when_not_armed_or_after_a_fault) {
    LRig r(0.02); r.lines.configure(band80_on_tx1());
    CHECK(!r.lines.for_over(k80).empty()); CHECK(r.lines.apply(k80) == TxLines::Result::NotArmed); CHECK(write_ops(r.bus).empty());
    r.lines.arm(); r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});
    CHECK(r.lines.apply(k80) == TxLines::Result::Fault); CHECK(!r.lines.armed()); CHECK(!r.lines.last_error().empty());
    const auto why = r.lines.for_over(k80); CHECK(!why.empty()); CHECK(why.find("not ready") != std::string::npos);
    r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0}); CHECK(r.lines.apply(k80) == TxLines::Result::NotArmed);   // stays disarmed until the next start-up
}
TEST(txlines_release_switches_used_lines_off_and_a_dropped_role_goes_off) {
    LRig r; LineConfig c; c.role[0] = LineRole::Band; c.role[2] = LineRole::Band; c.band_mask[0] = bit("80m"); c.band_mask[2] = bit("80m") | bit("40m");
    r.lines.configure(c); r.lines.arm(); CHECK(r.lines.apply(k80) == TxLines::Result::Sent);
    c.role[2] = LineRole::Unused; r.lines.configure(c); CHECK(r.lines.apply(k80) == TxLines::Result::Sent);   // TX3 left the band role while on: switched off
    CHECK(write_ops(r.bus).back() == (Wr{kRca3, 0, 0})); const size_t n = write_ops(r.bus).size();
    CHECK(r.lines.apply(k40) == TxLines::Result::Sent); CHECK_EQ(write_ops(r.bus).size(), n + 1);                // TX3 is no longer driven at all
    CHECK(r.lines.apply(k80) == TxLines::Result::Sent); r.lines.release();
    CHECK(write_ops(r.bus).back() == (Wr{kRca1, 0, 0})); CHECK(!r.lines.armed()); CHECK(r.lines.state()[0] == 0);
}
TEST(txlines_amp_tx_only_ever_passes_the_gate_as_zero) {
    LRig r; Safety& s = r.fx.safety();
    CHECK_THROWS(r.fx.set("RDAL_OP_SET_AMP_TX1", 0, 0), SafetyError);                   // dangerous without the permit
    s.permit("RDAL_OP_SET_AMP_TX1"); r.fx.set("RDAL_OP_SET_AMP_TX1", 0, 0);
    CHECK_THROWS(r.fx.set("RDAL_OP_SET_AMP_TX1", 1, 0), SafetyError);                   // "follow transmit" never passes, even with the permit
    CHECK_THROWS(r.fx.set("RDAL_OP_SET_AMP_TX1", 0, 1), SafetyError);
    CHECK(Safety::value_restricted("RDAL_OP_SET_AMP_TX3")); CHECK(!Safety::reviewed_value("RDAL_OP_SET_AMP_TX2", 1, 0));
}
TEST(txlines_need_firewire_and_writes) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Safety s; s.allow_write = true; Flex midi_fx(link, s);
    CHECK_THROWS(TxLines(midi_fx), StartupError);
    LRig ro; ro.fx.safety().allow_write = false; CHECK_THROWS(ro.lines.arm(), SafetyError);
}
TEST(session_tx_lines_follow_the_table_and_go_off_at_shutdown) {
    FakeDiceBus bus; flex_defaults(bus); RadioSession session([&](const std::string&) { return std::unique_ptr<BusHandle>(new FakeHandle(bus, false)); });
    SessionOptions o; o.path = "/dev/fw1"; o.allow_write = true; o.boot_wait_s = 0;
    session.configure_tx_lines(band80_on_tx1());
    CHECK(session.apply_tx_lines(k80) == TxLines::Result::NotArmed); session.connect(o); session.start(3.573, "ANT1");
    auto rca = [&] { std::vector<Wr> v; for (auto& w : write_ops(bus)) if (w.op >= kRca1 && w.op <= kRca3) v.push_back(w); return v; };
    CHECK(rca().empty());                                                              // the start-up itself never touches the lines
    CHECK(session.apply_tx_lines(k80) == TxLines::Result::Sent); CHECK(rca().back() == (Wr{kRca1, 1, 0})); CHECK_EQ(session.tx_lines_state()[0], 1);
    session.shutdown(); CHECK(rca().back() == (Wr{kRca1, 0, 0}));                       // off at stop
    CHECK(session.apply_tx_lines(k80) == TxLines::Result::NotArmed);
    session.start(3.573, "ANT1"); CHECK(session.apply_tx_lines(k80) == TxLines::Result::Sent);   // the table survived the restart
    const size_t n = write_ops(bus).size(); session.disconnect(); CHECK_EQ(write_ops(bus).size(), n);   // disconnect writes nothing
}
