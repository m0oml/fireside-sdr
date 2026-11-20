// RadioSession: connect (read-only, byte order proven) -> identify -> start -> tune -> shutdown, state machine and refusals, over the simulated bus.
#include <chrono>
#include <thread>
#include "fake_dice.h"
#include "fireside/session.h"
#include "fireside/tuning.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
struct S {
    FakeDiceBus bus; bool local = false; int opened = 0; RadioSession session;
    S() : bus(), session([this](const std::string&) { ++opened; return std::unique_ptr<BusHandle>(new FakeHandle(bus, local)); }) { flex_defaults(bus); }
    SessionOptions opts(bool w = true, const char* order = "auto") { SessionOptions o; o.path = "/dev/fw1"; o.allow_write = w; o.byte_order = order; o.boot_wait_s = 0; return o; }
};
std::vector<uint32_t> ops(const std::vector<Wr>& w) { std::vector<uint32_t> o; for (auto& x : w) o.push_back(x.op); return o; }
}  // namespace

TEST(session_connect_is_read_only_and_identify_works) {
    S s; CHECK(s.session.state() == RadioSession::State::Disconnected);
    s.session.connect(s.opts()); CHECK(s.session.state() == RadioSession::State::Connected); CHECK(s.session.byte_order() == ByteOrder::Big);
    auto id = s.session.identify(); CHECK_EQ(id.model_name(), std::string("FLEX-5000A")); CHECK(write_ops(s.bus).empty());
    CHECK_THROWS(s.session.connect(s.opts()), std::logic_error);                              // already connected
}
TEST(session_operations_before_connect_are_refused) {
    S s;
    CHECK_THROWS(s.session.identify(), std::logic_error); CHECK_THROWS(s.session.start(7.1, "ANT1"), std::logic_error); CHECK_THROWS(s.session.shutdown(), std::logic_error);
    CHECK(s.session.tune(7.1e6) == TuneResult::NotArmed); CHECK(!s.session.pump()); CHECK_EQ(s.opened, 0);
}
TEST(session_start_needs_allow_write_and_valid_arguments_before_any_traffic) {
    S s; s.session.connect(s.opts(false)); const size_t w0 = s.bus.writes.size();
    CHECK_THROWS(s.session.start(7.1, "ANT1"), SafetyError); CHECK_THROWS(s.session.shutdown(), SafetyError); CHECK_EQ(s.bus.writes.size(), w0);
    S t; t.session.connect(t.opts()); const size_t w1 = t.bus.writes.size();
    CHECK_THROWS(t.session.start(99.0, "ANT1"), std::invalid_argument); CHECK_THROWS(t.session.start(7.1, "ANT9"), std::invalid_argument); CHECK_EQ(t.bus.writes.size(), w1);
}
TEST(session_full_cycle_start_tune_shutdown) {
    S s; s.session.connect(s.opts()); std::vector<std::string> log;
    s.session.start(7.1, "ANT1", [&](const std::string& m) { log.push_back(m); });
    CHECK(s.session.state() == RadioSession::State::Started); CHECK(s.session.tuner() && s.session.tuner()->armed()); CHECK(log.size() >= 6);
    CHECK_THROWS(s.session.start(7.1, "ANT1"), std::logic_error);                             // not twice
    CHECK(s.session.tune(14.074e6) == TuneResult::Sent);
    s.session.shutdown();
    CHECK(s.session.state() == RadioSession::State::Connected); CHECK(!s.session.tuner()->armed()); CHECK(s.session.tune(7.2e6) == TuneResult::NotArmed);
    CHECK(ops(write_ops(s.bus)) == (std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347, 1347, 1292, 1320, 1285}));
    auto w = write_ops(s.bus); CHECK_EQ(w[4].d1, freq_to_tw(7.091)); CHECK_EQ(w[5].d1, freq_to_tw(14.065));      // the DDS sits 9 kHz below the dial
    s.session.start(7.2, "ANT2");                                                              // a stopped radio can be started again
    CHECK(s.session.state() == RadioSession::State::Started); CHECK_EQ(write_ops(s.bus).size(), size_t(9 + 5));
}
TEST(session_shutdown_is_valid_from_connected_without_a_start) {
    S s; s.session.connect(s.opts()); s.session.shutdown();
    CHECK(write_ops(s.bus) == (std::vector<Wr>{{1292, 0, 0}, {1320, 1, 0}, {1285, 0, 0}})); CHECK(s.session.state() == RadioSession::State::Connected);
}
TEST(session_pump_sends_the_coalesced_retune) {
    S s; s.session.connect(s.opts()); s.session.start(7.1, "ANT1");
    CHECK(s.session.tune(7.2e6) == TuneResult::Sent);
    CHECK(s.session.tune(7.3e6) == TuneResult::Coalesced); CHECK(!s.session.pump());                      // inside the 50 ms interval
    std::this_thread::sleep_for(std::chrono::milliseconds(70)); CHECK(s.session.pump());
    auto w = write_ops(s.bus); CHECK_EQ(w.back().d1, freq_to_tw(7.291));
}
TEST(session_failed_shutdown_keeps_the_state_so_it_can_be_retried) {
    S s; s.session.connect(s.opts()); s.session.start(7.1, "ANT1");
    s.bus.stuck = true; s.bus.mem[kPalCmd] = be({kPalBusy});                                               // firmware wedges
    CHECK_THROWS(s.session.shutdown(), TimeoutError);
    CHECK(s.session.state() == RadioSession::State::Started); CHECK(!s.session.tuner()->armed());          // retunes stay off
    s.bus.stuck = false; s.bus.mem[kPalCmd] = be({0}); s.session.shutdown(); CHECK(s.session.state() == RadioSession::State::Connected);
}
TEST(session_refuses_bad_connections_without_writing) {
    { S s; s.local = true; CHECK_THROWS(s.session.connect(s.opts()), std::runtime_error); CHECK(s.bus.writes.empty()); CHECK(s.session.state() == RadioSession::State::Disconnected); }
    { S s; CHECK_THROWS(s.session.connect(s.opts(true, "little")), StartupError); CHECK(s.session.state() == RadioSession::State::Disconnected);
      for (auto code : s.bus.executed) { const OpInfo* o = find_opcode(code); CHECK(!o || is_read(*o)); } }
    { S s; s.bus.answers[opv("RDAL_OP_GET_FIRMWARE_REV")] = 0x01030008; CHECK_THROWS(s.session.connect(s.opts()), StartupError); CHECK(write_ops(s.bus).empty()); }
    { S s; SessionOptions o = s.opts(); o.path = ""; CHECK_THROWS(s.session.connect(o), std::invalid_argument); o.path = "x"; o.byte_order = "middle"; CHECK_THROWS(s.session.connect(o), std::invalid_argument); CHECK_EQ(s.opened, 0); }
}
TEST(session_disconnect_resets_and_allows_a_new_connection) {
    S s; s.session.connect(s.opts()); s.session.start(7.1, "ANT1"); s.session.disconnect();
    CHECK(s.session.state() == RadioSession::State::Disconnected); CHECK(!s.session.tuner()); CHECK(s.session.tune(7.1e6) == TuneResult::NotArmed);
    s.session.connect(s.opts(false)); CHECK(!s.session.allow_write());
}

TEST(session_disconnect_disarms_a_tuner_handed_out_earlier) {                              // Opus G5
    S s; s.session.connect(s.opts()); s.session.start(7.1, "ANT1"); auto t = s.session.tuner(); const size_t n = write_ops(s.bus).size();
    s.session.disconnect();
    CHECK(t && !t->armed()); CHECK(t->request(7.2e6) == TuneResult::NotArmed); CHECK(!t->pump()); CHECK_EQ(write_ops(s.bus).size(), n);
}
