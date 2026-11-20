// The single tuning path: only SET_RX1_FREQ_TW, range-checked, rate-limited, armed-only, fault-disarming, thread-safe; plus CAT wiring.
#include <cmath>
#include <limits>
#include <thread>
#include "fake_dice.h"
#include "fake_radio.h"
#include "fireside/startup.h"
#include "fireside/tune.h"
#include "fireside/tuning.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
struct TRig {
    FakeDiceBus bus; PalMailbox mb; Flex fx; double t = 100.0; bool ready; Tuner tuner;       // `ready` must come before `tuner`: the Tuner needs a verified mailbox
    explicit TRig(TuneConfig cfg = {}, double mailbox_timeout = 1.0)
        : bus(), mb(bus, ByteOrder::Big, mailbox_timeout, 0), fx(mb, [] { Safety s; s.allow_write = true; return s; }()),
          ready([this] { flex_defaults(bus); return mb.verify(); }()), tuner(fx, cfg, [this] { return t; }) {}
};
const uint32_t kOp = 1347;                                                           // RDAL_OP_SET_RX1_FREQ_TW
}  // namespace

TEST(tune_sends_exactly_the_rx1_tuning_word_and_nothing_else) {
    TRig r; r.tuner.arm();
    CHECK(r.tuner.request(7100000.0) == TuneResult::Sent);
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(1)); CHECK_EQ(w[0].op, kOp);
    CHECK_EQ(w[0].d1, freq_to_tw(7.1)); CHECK_EQ(w[0].d1, 0x03A29C77u); CHECK_NEAR(as_f32(w[0].d2), 7.1, 1e-5);
    CHECK(r.tuner.last_sent_hz() && *r.tuner.last_sent_hz() == 7100000.0);
    CHECK(opv("RDAL_OP_SET_RX1_FREQ_TW") == kOp);
}
TEST(tune_if_offset_moves_the_dds_below_the_dial) {
    TuneConfig c; c.if_offset_hz = 9000; TRig r(c); r.tuner.arm();
    r.tuner.request(7100000.0); auto w = write_ops(r.bus);
    CHECK_EQ(w[0].d1, freq_to_tw(7.091)); CHECK_NEAR(as_f32(w[0].d2), 7.091, 1e-5);
    CHECK(!Tuner::in_range(5000.0, 9000.0));                                           // DDS would be below 0
    CHECK(Tuner::in_range(9001.0, 9000.0));
}
TEST(tune_refuses_until_armed_and_requires_firewire_and_allow_write) {
    TRig r; CHECK(r.tuner.request(7100000.0) == TuneResult::NotArmed); CHECK(write_ops(r.bus).empty());
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Safety s; s.allow_write = true; Flex midi_fx(link, s);
    CHECK_THROWS(Tuner(midi_fx), StartupError);                                         // MIDI is never a tuning route
    TRig ro; ro.fx.safety().allow_write = false; CHECK_THROWS(ro.tuner.arm(), SafetyError);
    r.tuner.arm(); r.tuner.disarm(); CHECK(r.tuner.request(7100000.0) == TuneResult::NotArmed);
}
TEST(tune_rejects_out_of_range_and_non_finite_frequencies_without_writing) {
    TRig r; r.tuner.arm();
    for (double bad : {0.0, -7e6, 65.000001e6, 500e6, std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), 1e300}) CHECK(r.tuner.request(bad) == TuneResult::Rejected);
    CHECK(write_ops(r.bus).empty()); CHECK_EQ(r.tuner.counters().rejected, 8ul);
    CHECK(r.tuner.request(65e6) == TuneResult::Sent);                                  // the top of the range is allowed
}
TEST(tune_rate_limits_and_coalesces_to_the_latest_request) {
    TuneConfig c; c.min_interval_s = 0.05; TRig r(c); r.tuner.arm();
    CHECK(r.tuner.request(7000000.0) == TuneResult::Sent);
    for (int i = 1; i <= 100; ++i) { r.t += 0.0001; CHECK(r.tuner.request(7000000.0 + 100.0 * i) == TuneResult::Coalesced); }   // a knob spinning for 10 ms
    CHECK_EQ(write_ops(r.bus).size(), size_t(1));
    CHECK(!r.tuner.pump());                                                              // too early
    r.t += 0.05; CHECK(r.tuner.pump());
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(2)); CHECK_EQ(w[1].d1, freq_to_tw(7.01));     // only the LATEST frequency was sent
    CHECK(!r.tuner.pending_hz()); CHECK(!r.tuner.pump());
}
TEST(tune_sustained_spinning_never_exceeds_20_writes_per_second) {
    TRig r; r.tuner.arm(); unsigned long writes = 0;
    for (int i = 0; i < 2000; ++i) { r.t += 0.001; r.tuner.request(7000000.0 + 10.0 * i); r.tuner.pump(); }      // 2 s of 1 kHz requests
    writes = write_ops(r.bus).size();
    CHECK(writes <= 41); CHECK(writes >= 30);
    CHECK_EQ(r.tuner.counters().sent, writes);
}
TEST(tune_unchanged_and_spin_away_and_back_do_not_write) {
    TRig r; r.tuner.arm(); r.tuner.request(7100000.0);
    CHECK(r.tuner.request(7100000.0) == TuneResult::Unchanged); CHECK_EQ(write_ops(r.bus).size(), size_t(1));
    r.t += 0.001; CHECK(r.tuner.request(7100500.0) == TuneResult::Coalesced);
    r.t += 0.001; CHECK(r.tuner.request(7100000.0) == TuneResult::Unchanged); CHECK(!r.tuner.pending_hz());       // back where the radio already is
    r.t += 1; CHECK(!r.tuner.pump()); CHECK_EQ(write_ops(r.bus).size(), size_t(1));
}
TEST(tune_mailbox_fault_disarms_until_rearmed) {
    TRig r({}, 0.02); const size_t baseline = r.bus.writes.size(); r.tuner.arm(); r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});         // firmware wedged
    CHECK(r.tuner.request(7100000.0) == TuneResult::Fault); CHECK(!r.tuner.armed()); CHECK(!r.tuner.last_error().empty()); CHECK_EQ(r.tuner.counters().faults, 1ul);
    CHECK_EQ(r.bus.writes.size(), baseline);                                             // never wrote over a busy mailbox
    r.t += 10; CHECK(r.tuner.request(7200000.0) == TuneResult::NotArmed);                // no retry storm against a dead radio
    r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0}); r.tuner.arm(); r.t += 10; CHECK(r.tuner.request(7200000.0) == TuneResult::Sent);
}
TEST(tune_is_thread_safe_and_every_write_is_a_valid_tuning_word) {
    TuneConfig c; c.min_interval_s = 0.0; TRig r(c); r.tuner.arm();
    std::vector<std::thread> th; for (int k = 0; k < 4; ++k) th.emplace_back([&, k] { for (int i = 0; i < 300; ++i) r.tuner.request(1e6 + 1000.0 * ((k * 300 + i) % 5000)); });
    for (auto& t : th) t.join();
    auto w = write_ops(r.bus); CHECK(w.size() >= 1);
    for (auto& x : w) { CHECK_EQ(x.op, kOp); CHECK(x.d1 > 0u); const double f = as_f32(x.d2); CHECK(f >= 1.0f && f <= 6.0f); }
    CHECK_EQ(r.tuner.counters().sent, w.size());
}
TEST(tune_cat_frequency_changes_retune_through_the_tuner_only) {
    TRig r; r.tuner.arm(); CatSession cat; connect_cat_to_tuner(cat, r.tuner);
    cat.feed("FA00014074000;"); r.t += 1; cat.feed("FA00014074000;");                    // same again: no second write
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(1)); CHECK_EQ(w[0].op, kOp); CHECK_EQ(w[0].d1, freq_to_tw(14.074));
    r.t += 1; cat.feed("FB00007074000;"); CHECK_EQ(write_ops(r.bus).size(), size_t(1));  // VFO B is not the receive VFO
    r.t += 1; cat.feed("FR1;"); w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(2)); CHECK_EQ(w[1].d1, freq_to_tw(7.074));
    r.t += 1; cat.feed("MD3;"); cat.feed("AG0200;"); CHECK_EQ(write_ops(r.bus).size(), size_t(2));      // other commands never write
    for (const char* bad : {"FA00065000001;", "FA123;", "TX1;", "PS0;", "MX1;"}) { r.t += 1; cat.feed(bad); }
    CHECK_EQ(write_ops(r.bus).size(), size_t(2)); CHECK_EQ(cat.refused_tx(), uint64_t(2));
    for (auto& x : write_ops(r.bus)) CHECK_EQ(x.op, kOp);
}

TEST(tune_requires_a_verified_mailbox) {                                                   // F4
    FakeDiceBus bus; flex_defaults(bus); PalMailbox mb(bus, ByteOrder::Big, 1.0, 0); Safety s; s.allow_write = true; Flex fx(mb, s);
    CHECK_THROWS(Tuner(fx), StartupError); CHECK(bus.writes.empty());
    CHECK(mb.verify()); Tuner ok(fx); ok.arm(); CHECK(ok.request(7100000.0) == TuneResult::Sent);
}
TEST(tuner_if_offset_switch_resends_the_dial_at_once) {   // transmit: the DDS on the dial; afterwards back 9 kHz below it
    TuneConfig c; c.if_offset_hz = 9000; TRig r(c); r.tuner.arm();
    CHECK(r.tuner.request(14101000.0) == TuneResult::Sent);
    const size_t n = write_ops(r.bus).size();
    CHECK(r.tuner.set_if_offset(0) == TuneResult::Sent); CHECK_EQ(write_ops(r.bus).size(), n + 1);   // at once, within the rate limit
    CHECK_NEAR(as_f32(write_ops(r.bus).back().d2), 14.101, 1e-4);                                  // the DDS on the dial itself
    CHECK(r.tuner.set_if_offset(0) == TuneResult::Unchanged);
    CHECK(r.tuner.set_if_offset(9000) == TuneResult::Sent); CHECK_NEAR(as_f32(write_ops(r.bus).back().d2), 14.092, 1e-4);
    CHECK(r.tuner.set_if_offset(50000) == TuneResult::Rejected);
}
