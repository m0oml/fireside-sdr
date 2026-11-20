// The receive-preamp path: only SET_TRX_PREAMP, only 0/1, armed-only, idempotent, fault-disarming, FireWire only.
#include "fake_dice.h"
#include "fake_radio.h"
#include "fireside/preamp.h"
#include "fireside/session.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
struct PRig {
    FakeDiceBus bus; PalMailbox mb; Flex fx; bool ready; RxPreamp pre;
    explicit PRig(double mailbox_timeout = 1.0)
        : bus(), mb(bus, ByteOrder::Big, mailbox_timeout, 0), fx(mb, [] { Safety s; s.allow_write = true; return s; }()),
          ready([this] { flex_defaults(bus); return mb.verify(); }()), pre(fx) {}
};
const uint32_t kPre = 1247;                                                             // RDAL_OP_SET_TRX_PREAMP
}  // namespace

TEST(preamp_sends_only_set_trx_preamp_with_zero_or_one) {
    PRig r; r.pre.arm();
    CHECK(opv("RDAL_OP_SET_TRX_PREAMP") == kPre);
    CHECK(r.pre.request(true) == PreampResult::Sent); CHECK(r.pre.on());
    CHECK(r.pre.request(false) == PreampResult::Sent); CHECK(!r.pre.on());
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(2));
    CHECK(w[0] == (Wr{kPre, 1, 0})); CHECK(w[1] == (Wr{kPre, 0, 0}));
}
TEST(preamp_asking_for_the_current_state_sends_nothing) {
    PRig r; r.pre.arm();
    CHECK(r.pre.request(false) == PreampResult::Unchanged); CHECK(write_ops(r.bus).empty());          // start-up has just forced it off
    CHECK(r.pre.request(true) == PreampResult::Sent); CHECK(r.pre.request(true) == PreampResult::Unchanged); CHECK_EQ(write_ops(r.bus).size(), size_t(1));
}
TEST(preamp_refuses_until_armed_and_needs_firewire_and_allow_write) {
    PRig r; CHECK(r.pre.request(true) == PreampResult::NotArmed); CHECK(write_ops(r.bus).empty());
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Safety s; s.allow_write = true; Flex midi_fx(link, s);
    CHECK_THROWS(RxPreamp(midi_fx), StartupError);                                      // MIDI is never a route for this
    PRig ro; ro.fx.safety().allow_write = false; CHECK_THROWS(ro.pre.arm(), SafetyError);
    r.pre.arm(); r.pre.disarm(); CHECK(r.pre.request(true) == PreampResult::NotArmed); CHECK(write_ops(r.bus).empty());
}
TEST(preamp_rearming_assumes_off_again) {
    PRig r; r.pre.arm(); r.pre.request(true); r.pre.disarm(); r.pre.arm(); CHECK(!r.pre.on());      // the next start-up writes SetTRXPreamp(false) first
}
TEST(preamp_mailbox_fault_disarms_until_rearmed) {
    PRig r(0.02); const size_t baseline = r.bus.writes.size(); r.pre.arm(); r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});
    CHECK(r.pre.request(true) == PreampResult::Fault); CHECK(!r.pre.armed()); CHECK(!r.pre.last_error().empty()); CHECK(!r.pre.on());
    CHECK_EQ(r.bus.writes.size(), baseline);                                           // never wrote over a busy mailbox
    r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0}); CHECK(r.pre.request(true) == PreampResult::NotArmed);   // stays off until the operator re-arms
}
TEST(session_preamp_only_while_started_and_goes_off_at_shutdown) {
    FakeDiceBus bus; flex_defaults(bus); RadioSession session([&](const std::string&) { return std::unique_ptr<BusHandle>(new FakeHandle(bus, false)); });
    SessionOptions o; o.path = "/dev/fw1"; o.allow_write = true; o.boot_wait_s = 0;
    CHECK(session.set_preamp(true) == PreampResult::NotArmed);
    session.connect(o); CHECK(session.set_preamp(true) == PreampResult::NotArmed); CHECK(write_ops(bus).empty());
    session.start(7.1, "ANT1"); const size_t after_start = write_ops(bus).size();
    CHECK(session.set_preamp(true) == PreampResult::Sent); CHECK(session.preamp_on());
    auto w = write_ops(bus); CHECK_EQ(w.size(), after_start + 1); CHECK(w.back() == (Wr{kPre, 1, 0}));
    session.shutdown(); CHECK(!session.preamp_on());
    w = write_ops(bus); CHECK(w[after_start + 1] == (Wr{kPre, 0, 0}));                  // preamp off is the first write of the shutdown, then MOX/standby/PA bias
    CHECK(session.set_preamp(true) == PreampResult::NotArmed);
    session.start(7.1, "ANT1"); CHECK(!session.preamp_on()); session.disconnect(); CHECK(session.set_preamp(true) == PreampResult::NotArmed);
}
