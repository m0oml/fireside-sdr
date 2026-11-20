// Mirror of tools/tests/test_startup.py (the C++ port of the reviewed start-up/shut-down sequence).
#include <chrono>
#include <cstring>
#include <thread>
#include <tuple>
#include "fake_dice.h"
#include "fake_radio.h"
#include "fireside/client.h"
#include "fireside/identify.h"
#include "fireside/startup.h"
#include "fireside/tuning.h"
using namespace fireside;
using namespace fakedice;

namespace {
IdentifyOptions quick() { IdentifyOptions o; o.boot_wait_s = 1; o.sleep = [](double) {}; return o; }
struct Rig {
    FakeDiceBus bus; PalMailbox mb; Safety s; Flex fx;
    explicit Rig(bool allow_write = true, double timeout = 1.0) : bus(), mb(bus, ByteOrder::Big, timeout, 0), s(), fx(mb, [&] { Safety x; x.allow_write = allow_write; return x; }()) { flex_defaults(bus); if (!mb.verify()) throw TestFailure("fake bus did not verify"); }
};
std::vector<uint32_t> ops(const std::vector<Wr>& w) { std::vector<uint32_t> o; for (auto& x : w) o.push_back(x.op); return o; }
}  // namespace

TEST(startup_tuning_word_matches_powersdr_formula) {
    for (double f : {0.1, 1.8, 7.1, 14.2345, 29.7, 50.313, 54.0}) CHECK_EQ(freq_to_tw(f), uint32_t(4294967295.0 * f / 500.0));
    CHECK_EQ(freq_to_tw(7.1), 0x03A29C77u);
    for (double bad : {0.0, -1.0, 65.01, 500.0}) CHECK_THROWS(freq_to_tw(bad), std::invalid_argument);
}
TEST(startup_step_order_and_contents) {
    auto st = startup_steps(14.2, "ANT2");
    std::vector<std::string> names; for (auto& s : st) names.push_back(s.op);
    CHECK(names == (std::vector<std::string>{"INITIALIZE", "SET_TRX_PREAMP", "SET_STANDBY", "SET_RX1_ANT", "SET_RX1_FREQ_TW"}));
    CHECK_EQ(st[2].d1, 0.0); CHECK_EQ(st[3].d1, 2.0);
    CHECK_EQ(uint32_t(st[4].d1), freq_to_tw(14.2)); CHECK_NEAR(st[4].d2, 14.2, 1e-9);
    CHECK_THROWS(startup_steps(7.1, "ANT9"), std::invalid_argument);
    CHECK_THROWS(startup_steps(65.5), std::invalid_argument);
}
TEST(startup_no_tx_or_calibration_opcodes_in_startup) {
    for (auto& s : startup_steps(7.1)) {
        if (s.op == "SET_TRX_PREAMP") continue;
        for (const char* b : {"MOX", "PA_BIAS", "EEPROM", "QSD", "QSE", "SET_TR", "SIG", "GEN", "TEST", "POT", "REG"}) CHECK(s.op.find(b) == std::string::npos);
    }
}
TEST(startup_every_step_resolves_to_a_known_write_opcode) {
    auto all = startup_steps(7.1); for (auto& s : shutdown_steps()) all.push_back(s);
    for (auto& s : all) { const OpInfo* o = find_opcode(s.op); CHECK(o != nullptr); CHECK(!is_read(*o)); }
}

TEST(startup_identify_over_mailbox) {
    Rig r; auto id = identify(r.fx, quick());
    CHECK_EQ(id.model_name(), std::string("FLEX-5000A")); CHECK_EQ(id.serial, 12345u); CHECK_EQ(id.firmware, 0x02010408u);
    CHECK(id.has_atu()); CHECK(!id.has_rx2());
    CHECK(write_ops(r.bus).empty());
}
TEST(startup_atu_drain_loops_until_empty) {
    Rig r; int left = 3;
    r.bus.dynamic[opv("RDAL_OP_ATU_GET_RESULT")] = [&] { uint32_t v = uint32_t(left) << 24; left = left > 0 ? left - 1 : 0; return v; };
    CHECK_EQ(flush_atu(r.fx), 4);                                // reads 3,2,1,0
}

TEST(startup_writes_over_midi_are_refused_before_any_traffic) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t);
    Safety s; s.allow_write = true; Flex fx(link, s);
    CHECK_THROWS(run_startup(fx, 7.1, "ANT1", {}, quick()), StartupError);
    CHECK_THROWS(run_steps(fx, shutdown_steps()), StartupError);
    try { run_steps(fx, shutdown_steps()); } catch (const StartupError& e) { CHECK(std::string(e.what()).find("FireWire mailbox") != std::string::npos); }
    fx.get("GET_MODEL");                                         // round trip: earlier sends have been processed
    CHECK(radio.writes().empty());
    CHECK_EQ(radio.gets(), size_t(1));                           // only the GET_MODEL above: refusal happened before any traffic
}
TEST(startup_waits_for_booting_radio) {
    Rig r; int n = 0; std::vector<double> naps;
    r.bus.dynamic[opv("RDAL_OP_GET_FIRMWARE_REV")] = [&] { return ++n < 4 ? 0u : 0x02010408u; };
    IdentifyOptions o; o.boot_wait_s = 60; o.poll_s = 0.5; o.sleep = [&](double s) { naps.push_back(s); };
    auto id = identify(r.fx, o);
    CHECK_EQ(id.firmware, 0x02010408u); CHECK(std::count(naps.begin(), naps.end(), 0.5) >= 3);
}
TEST(startup_gives_up_if_radio_never_answers) {
    Rig r; r.bus.answers[opv("RDAL_OP_GET_FIRMWARE_REV")] = 0;
    double t = 0; IdentifyOptions o; o.boot_wait_s = 5; o.poll_s = 1; o.sleep = [&](double s) { t += s; }; o.clock = [&] { return t; };
    try { identify(r.fx, o); FAIL_AT("no exception"); } catch (const StartupError& e) { CHECK(std::string(e.what()).find("too soon after power-on") != std::string::npos); }
}

TEST(startup_full_startup_sends_exact_sequence) {
    Rig r; run_startup(r.fx, 7.1, "ANT1", {}, quick());
    auto w = write_ops(r.bus);
    CHECK(ops(w) == (std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347}));      // INITIALIZE, TRX_PREAMP, STANDBY, RX1_ANT, RX1_FREQ_TW
    CHECK(w[0].d1 == 0 && w[0].d2 == 0); CHECK(w[1].d1 == 0 && w[1].d2 == 0); CHECK(w[2].d1 == 0 && w[2].d2 == 0);
    CHECK(w[3].d1 == 1 && w[3].d2 == 0);
    CHECK_EQ(w[4].d1, 0x03A29C77u); CHECK_NEAR(as_f32(w[4].d2), 7.1, 1e-5);
}
TEST(startup_log_shows_each_step) {
    Rig r; std::vector<std::string> lines; run_startup(r.fx, 7.1, "ANT1", [&](const std::string& s) { lines.push_back(s); }, quick());
    CHECK(lines.size() >= 6);                                    // identity + ATU drain + 5 steps
    CHECK(lines.back().find("SET_RX1_FREQ_TW") != std::string::npos);
}
TEST(startup_shutdown_unkeys_standby_then_pa_bias_off) {
    Rig r; run_steps(r.fx, shutdown_steps());
    auto w = write_ops(r.bus);
    CHECK(w == (std::vector<Wr>{{1292, 0, 0}, {1320, 1, 0}, {1285, 0, 0}}));   // SET_MOX 0, SET_STANDBY 1, SET_PA_BIAS 0
}
TEST(startup_exemptions_are_value_specific) {
    Rig r;
    for (const Step& bad : {Step{"SET_MOX", 1, 0, "key"}, Step{"SET_PA_BIAS", 1, 0, "bias on"}, Step{"INITIALIZE", 1, 0, "odd"}}) {
        try { run_steps(r.fx, {bad}); FAIL_AT("no exception"); }
        catch (const SafetyError& e) { CHECK(std::string(e.what()).find("not a reviewed value") != std::string::npos); }
    }
    CHECK(write_ops(r.bus).empty());
}
TEST(startup_bad_step_later_in_list_sends_nothing_at_all) {
    Rig r; auto steps = startup_steps(7.1); steps.push_back({"SET_MOX", 1, 0, "key"});
    CHECK_THROWS(run_steps(r.fx, steps), SafetyError);
    CHECK(write_ops(r.bus).empty());
}
TEST(startup_refuses_without_allow_write) {
    Rig r(false);
    CHECK_THROWS(run_startup(r.fx, 7.1, "ANT1", {}, quick()), SafetyError);
    CHECK(write_ops(r.bus).empty());
}
TEST(startup_permit_is_scoped) {
    Rig r; run_startup(r.fx, 7.1, "ANT1", {}, quick());
    for (const char* op : {"INITIALIZE", "SET_MOX", "SET_PA_BIAS"}) CHECK_THROWS(r.fx.set(op, 0, 0), SafetyError);   // still refused outside the sequences
}
TEST(startup_permit_released_even_if_the_write_throws) {
    Rig r(true, 0.02); r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});    // mailbox wedged: the write times out
    CHECK_THROWS(run_steps(r.fx, {{"SET_MOX", 0, 0, "unkey"}}), TimeoutError);
    CHECK_THROWS(r.fx.set("SET_MOX", 0, 0), SafetyError);
}
TEST(startup_bad_firmware_or_model_stops_before_any_write) {
    Rig r; const uint32_t fwop = opv("RDAL_OP_GET_FIRMWARE_REV"), mop = opv("RDAL_OP_GET_MODEL");
    for (uint32_t fw : {0x01030008u, 0x02010407u, 0x02020000u}) {
        r.bus.answers[fwop] = fw;
        try { run_startup(r.fx, 7.1, "ANT1", {}, quick()); FAIL_AT("no exception"); }
        catch (const StartupError& e) { CHECK(std::string(e.what()).find("outside PowerSDR's supported range") != std::string::npos); }
    }
    r.bus.answers[fwop] = 0x02010408; r.bus.answers[mop] = 3;
    try { run_startup(r.fx, 7.1, "ANT1", {}, quick()); FAIL_AT("no exception"); }
    catch (const StartupError& e) { CHECK(std::string(e.what()).find("FLEX-3000") != std::string::npos); }
    CHECK(write_ops(r.bus).empty());
}
TEST(startup_bad_arguments_stop_before_any_traffic) {
    Rig r; const size_t baseline = r.bus.writes.size();                                     // (the rig's own verify() used the bus once)
    CHECK_THROWS(run_startup(r.fx, 99.0, "ANT1", {}, quick()), std::invalid_argument);
    CHECK_THROWS(run_startup(r.fx, 7.1, "ANT9", {}, quick()), std::invalid_argument);
    CHECK_EQ(r.bus.writes.size(), baseline);
}

// ---- Opus diff-check findings F1-F3 (mirrors the Python tests of the same names)
TEST(startup_permit_covers_only_the_reviewed_value_even_inside_the_window) {
    Rig r; const std::string mox = "RDAL_OP_SET_MOX";
    r.fx.safety().permit(mox);
    CHECK_THROWS(r.fx.set("SET_MOX", 1), SafetyError);                              // same caller, inside the window
    bool other_refused = false;
    std::thread t([&] { try { r.fx.set("SET_MOX", 1); } catch (const SafetyError&) { other_refused = true; } }); t.join();
    CHECK(other_refused);                                                            // another thread, inside the window
    r.fx.set("SET_MOX", 0);                                                          // the reviewed value still passes
    r.fx.safety().unpermit(mox);
    for (auto [op, d1, d2] : {std::tuple{"SET_PA_BIAS", 1.0, 0.0}, std::tuple{"INITIALIZE", 0.0, 1.0}}) {
        const std::string full = find_opcode(std::string(op))->name; r.fx.safety().permit(full);
        CHECK_THROWS(r.fx.set(op, d1, d2), SafetyError); r.fx.safety().unpermit(full);
    }
    CHECK(write_ops(r.bus) == (std::vector<Wr>{{1292, 0, 0}}));
}
TEST(startup_second_argument_must_be_zero_too) {
    Rig r;
    for (const Step& bad : {Step{"SET_MOX", 0, 5, "odd d2"}, Step{"SET_PA_BIAS", 0, 1, "odd d2"}}) {
        try { run_steps(r.fx, {bad}); FAIL_AT("no exception"); }
        catch (const SafetyError& e) { CHECK(std::string(e.what()).find("not a reviewed value") != std::string::npos); }
    }
    CHECK(!exemption_value_ok("SET_MOX", -1, 0)); CHECK(!exemption_value_ok("NO_SUCH_OP", 0, 0)); CHECK(exemption_value_ok("SET_MOX", 0, 0));
    CHECK(write_ops(r.bus).empty());
}
TEST(startup_allow_dangerous_still_overrides_explicitly) {
    Rig r; r.fx.safety().allow_dangerous = true; r.fx.set("SET_PA_BIAS", 0, 0);
    CHECK(write_ops(r.bus) == (std::vector<Wr>{{1285, 0, 0}}));
}
TEST(startup_last_write_is_confirmed_consumed) {
    Rig r; run_steps(r.fx, shutdown_steps());
    CHECK(r.bus.executed.size() >= 3);
    CHECK(std::vector<uint32_t>(r.bus.executed.end() - 3, r.bus.executed.end()) == (std::vector<uint32_t>{1292, 1320, 1285}));   // PA bias off really ran
    CHECK(!(be32(r.bus.mem[kPalCmd]) & kPalBusy));
}
TEST(startup_mailbox_that_never_takes_the_last_write_is_reported) {
    Rig r(true, 0.05); r.bus.busy_polls = 1000000000;                                 // firmware stops consuming commands
    CHECK_THROWS(run_steps(r.fx, {{"SET_STANDBY", 1, 0, "standby"}}), TimeoutError);
}
TEST(startup_midi_link_sync_is_a_no_op) {
    FakeRadio radio; RawMidi t(radio.path()); MidiLink link(t); Flex fx(link); fx.sync(0.01);   // nothing to wait for, nothing sent
    CHECK(radio.writes().empty());
}

TEST(startup_unverified_mailbox_is_refused_before_any_traffic) {                        // F4
    FakeDiceBus bus; flex_defaults(bus); PalMailbox mb(bus, ByteOrder::Big, 1.0, 0); Safety s; s.allow_write = true; Flex fx(mb, s);
    CHECK(!mb.verified());
    for (int i = 0; i < 2; ++i) {
        try { if (i == 0) run_startup(fx, 7.1, "ANT1", {}, quick()); else run_steps(fx, shutdown_steps()); FAIL_AT("no exception"); }
        catch (const StartupError& e) { CHECK(std::string(e.what()).find("not been verified") != std::string::npos); }
    }
    CHECK(bus.writes.empty()); CHECK_EQ(bus.reads, 0);
}
TEST(startup_verify_proves_the_order_and_rejects_the_wrong_one) {
    FakeDiceBus bus; flex_defaults(bus);
    PalMailbox good(bus, ByteOrder::Big, 1.0, 0); CHECK(good.verify()); CHECK(good.verified());
    PalMailbox bad(bus, ByteOrder::Little, 0.05, 0); CHECK(!bad.verify(0.0, 0.0)); CHECK(!bad.verified());
    bus.answers[opv("RDAL_OP_GET_FIRMWARE_REV")] = 0x01000000;                            // plausible-looking, out of range
    PalMailbox old(bus, ByteOrder::Big, 1.0, 0); CHECK(!old.verify(0.0, 0.0));
}
TEST(startup_verify_waits_for_a_booting_radio) {
    FakeDiceBus bus; flex_defaults(bus); int n = 0; bus.dynamic[opv("RDAL_OP_GET_FIRMWARE_REV")] = [&] { return ++n < 3 ? 0u : 0x02010408u; };
    PalMailbox mb(bus, ByteOrder::Big, 1.0, 0); CHECK(mb.verify(5.0, 0.001)); CHECK_EQ(n, 3);
}

TEST(startup_verify_fails_fast_on_a_definite_wrong_answer) {                               // Opus G12
    FakeDiceBus bus; flex_defaults(bus); bus.answers[opv("RDAL_OP_GET_FIRMWARE_REV")] = 0x01030008;
    PalMailbox mb(bus, ByteOrder::Big, 1.0, 0); const auto t0 = std::chrono::steady_clock::now();
    CHECK(!mb.verify(10.0, 0.5));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)); CHECK_EQ(bus.writes.size(), size_t(1));   // one probe, no 10 s of retries
}
