// End-to-end tests of `firesidecli` (run_cli) against a fake DICE bus: byte-order proof (F4), write gating, exact sequences, serve with CAT + tuner + shutdown.
#include <arpa/inet.h>
#include <atomic>
#include <future>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include "fake_dice.h"
#include "fireside/cli.h"
#include "fireside/tuning.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
struct Cli {
    FakeDiceBus bus; bool local = false; int opened = 0; CliEnv env; std::ostringstream out, err;
    Cli() { flex_defaults(bus); env.open_bus = [this](const std::string&) { ++opened; return std::unique_ptr<BusHandle>(new FakeHandle(bus, local)); }; env.sleep = [](double) {}; }
    int run(std::vector<std::string> a) { return run_cli(a, out, err, env); }
};
std::vector<uint32_t> ops(const std::vector<Wr>& w) { std::vector<uint32_t> o; for (auto& x : w) o.push_back(x.op); return o; }
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
}  // namespace

TEST(cli_list_prints_nodes_and_handles_none) {
    Cli c; c.env.list_nodes = [] { return std::vector<FwNode>{{"/dev/fw0", 0x00AABBCCDDEEFF00ull, true, ""}, {"/dev/fw1", 0x0011223344556677ull, false, ""}, {"/dev/fw2", std::nullopt, false, "Permission denied"}}; };
    CHECK_EQ(c.run({"list"}), 0);
    CHECK(has(c.out.str(), "/dev/fw0   guid=00AABBCCDDEEFF00   local controller")); CHECK(has(c.out.str(), "remote node")); CHECK(has(c.out.str(), "error: Permission denied"));
    Cli d; d.env.list_nodes = [] { return std::vector<FwNode>{}; }; CHECK_EQ(d.run({"list"}), 0); CHECK(has(d.out.str(), "no /dev/fw* nodes")); CHECK_EQ(d.opened, 0);
}
TEST(cli_startup_dry_run_needs_no_radio_and_no_allow_write) {
    Cli c; CHECK_EQ(c.run({"startup", "--dry-run", "--freq", "14.2", "--ant", "ANT2"}), 0);
    CHECK(has(c.out.str(), "SET_RX1_FREQ_TW")); CHECK(has(c.out.str(), "tune RX1 to 14.191 MHz")); CHECK(has(c.out.str(), "INITIALIZE(0, 0)")); CHECK(has(c.out.str(), "RX1 antenna ANT2")); CHECK(!has(c.out.str(), "\\n"));
    CHECK_EQ(c.opened, 0); CHECK(c.bus.writes.empty());
    Cli bad; CHECK_EQ(bad.run({"startup", "--dry-run", "--freq", "99"}), 2); CHECK_EQ(bad.run({"startup", "--dry-run", "--ant", "ANT9"}), 2);
}
TEST(cli_identify_probes_the_order_then_reads_only) {
    Cli c; CHECK_EQ(c.run({"--fw", "/dev/fw1", "identify"}), 0);
    CHECK(has(c.out.str(), "FLEX-5000A")); CHECK(has(c.out.str(), "5748-0000") || has(c.out.str(), "serial"));
    CHECK(write_ops(c.bus).empty());                                                       // identify never writes
}
TEST(cli_writes_need_allow_write_and_never_open_the_bus_without_it) {
    for (auto cmd : {std::vector<std::string>{"--fw", "/dev/fw1", "startup"}, {"--fw", "/dev/fw1", "shutdown"}, {"--fw", "/dev/fw1", "serve"}}) {
        Cli c; CHECK_EQ(c.run(cmd), 3); CHECK(has(c.err.str(), "--allow-write")); CHECK_EQ(c.opened, 0); CHECK(c.bus.writes.empty());
    }
}
TEST(cli_startup_sends_the_exact_reviewed_sequence) {
    Cli c; CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "startup", "--freq", "7.1"}), 0);
    auto w = write_ops(c.bus); CHECK(ops(w) == (std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347}));
    CHECK_EQ(w[4].d1, freq_to_tw(7.091)); CHECK_NEAR(as_f32(w[4].d2), 7.091, 1e-5); CHECK(has(c.out.str(), "radio is up on 7.1 MHz")); CHECK(has(c.out.str(), "9 kHz below the dial"));
}
TEST(cli_shutdown_sequence_and_last_write_confirmed) {
    Cli c; CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "shutdown"}), 0);
    CHECK(write_ops(c.bus) == (std::vector<Wr>{{1292, 0, 0}, {1320, 1, 0}, {1285, 0, 0}}));
    CHECK(!(be32(c.bus.mem[kPalCmd]) & kPalBusy));
}
TEST(cli_wrong_explicit_byte_order_writes_nothing_real) {                                    // F4: even an explicit --byte-order must be proven
    Cli c; CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "--byte-order", "little", "startup"}), 5);
    CHECK(has(c.err.str(), "nothing was written"));
    for (auto code : c.bus.executed) { const OpInfo* o = find_opcode(code); CHECK(!o || is_read(*o)); }   // only the (inert) probe word reached the firmware
}
TEST(cli_unusable_radio_is_reported_not_written) {
    { Cli c; c.bus.answers[opv("RDAL_OP_GET_FIRMWARE_REV")] = 0x01030008; CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "startup"}), 5); CHECK(write_ops(c.bus).empty()); }   // firmware out of range
    { Cli c; c.bus.answers[opv("RDAL_OP_GET_MODEL")] = 3; CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "startup"}), 5); CHECK(has(c.err.str(), "FLEX-3000")); CHECK(write_ops(c.bus).empty()); }
    { Cli c; c.local = true; CHECK_EQ(c.run({"--fw", "/dev/fw0", "--allow-write", "startup"}), 2); CHECK(has(c.err.str(), "local controller")); CHECK(c.bus.writes.empty()); }
    { Cli c; c.bus.stuck = true; c.bus.mem[kPalCmd] = be({kPalBusy}); CHECK(c.run({"--fw", "/dev/fw1", "--allow-write", "startup", "--boot-wait", "0"}) == 5); CHECK(c.bus.writes.empty()); }
}
TEST(cli_argument_errors_exit_2_without_touching_anything) {
    for (auto a : {std::vector<std::string>{}, {"bogus"}, {"--fw"}, {"--byte-order", "middle", "list"}, {"--fw", "/dev/fw1", "startup", "--freq", "abc"}, {"--fw", "/dev/fw1", "--allow-write", "startup", "--freq", "70"},
                   {"--fw", "/dev/fw1", "--allow-write", "startup", "--ant", "ANT9"}, {"--fw", "/dev/fw1", "serve", "--port", "70000"}, {"--wat"}, {"identify"}, {"--fw", "/dev/fw1", "identify", "extra"}}) {
        Cli c; const int rc = c.run(a); CHECK_EQ(rc, 2); CHECK(c.bus.writes.empty());
    }
}

namespace {
struct Tcp {
    int fd; explicit Tcp(uint16_t port) { fd = ::socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) throw TestFailure("connect failed"); }
    ~Tcp() { ::close(fd); }
    std::string ask(const std::string& s, size_t want) { (void)!::write(fd, s.data(), s.size()); std::string r;
        while (r.size() < want) { pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 2000) <= 0) break; char b[128]; ssize_t n = ::read(fd, b, sizeof b); if (n <= 0) break; r.append(b, size_t(n)); } return r; }
};
}  // namespace

TEST(cli_serve_runs_startup_then_cat_retunes_then_shuts_down_cleanly) {
    Cli c; c.env.sleep = [](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(std::min(s, 0.005))); };
    std::atomic<bool> stop{false}; std::promise<uint16_t> port; c.env.should_stop = [&] { return stop.load(); }; c.env.on_listening = [&](uint16_t p) { port.set_value(p); };
    int rc = -1; std::thread cli([&] { rc = c.run({"--fw", "/dev/fw1", "--allow-write", "serve", "--freq", "7.1", "--port", "0"}); });
    auto fut = port.get_future(); CHECK(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    { Tcp t(fut.get());
      CHECK_EQ(t.ask("ID;", 6), std::string("ID019;")); CHECK_EQ(t.ask("FA;", 14), std::string("FA00007100000;"));       // CAT starts on the start-up frequency
      t.ask("FA00014074000;", 0); CHECK_EQ(t.ask("FA;", 14), std::string("FA00014074000;"));                              // retune request
      CHECK_EQ(t.ask("TX1;", 2), std::string("?;")); CHECK_EQ(t.ask("PS0;", 2), std::string("?;")); CHECK_EQ(t.ask("FA00099999999;", 2), std::string("?;")); }
    std::this_thread::sleep_for(std::chrono::milliseconds(120)); stop = true; cli.join();
    CHECK_EQ(rc, 0);
    auto w = write_ops(c.bus);
    CHECK(ops(w) == (std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347, 1347, 1292, 1320, 1285}));                       // startup, ONE retune, shutdown
    CHECK_EQ(w[4].d1, freq_to_tw(7.091)); CHECK_EQ(w[5].d1, freq_to_tw(14.065));
    CHECK(w[6] == (Wr{1292, 0, 0}) && w[7] == (Wr{1320, 1, 0}) && w[8] == (Wr{1285, 0, 0}));
    CHECK(has(c.out.str(), "listening on 127.0.0.1:")); CHECK(has(c.out.str(), "shutting down")); CHECK(has(c.out.str(), "1 write(s)"));
}
TEST(cli_serve_stops_without_cat_traffic_and_still_shuts_down) {
    Cli c; std::atomic<int> polls{0}; c.env.should_stop = [&] { return ++polls > 3; };
    CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "serve", "--port", "0"}), 0);
    CHECK(ops(write_ops(c.bus)) == (std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347, 1292, 1320, 1285}));              // no retune: nothing asked for one
}
TEST(cli_serve_startup_failure_does_not_run_shutdown_or_listen) {
    Cli c; c.bus.answers[opv("RDAL_OP_GET_MODEL")] = 3; CHECK_EQ(c.run({"--fw", "/dev/fw1", "--allow-write", "serve"}), 5);
    CHECK(write_ops(c.bus).empty()); CHECK(!has(c.out.str(), "listening"));
}

TEST(cli_serve_shuts_down_even_if_cat_cannot_start) {                                       // Opus G10: port already taken after a successful start
    int blocker = ::socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(blocker, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(blocker, 1) == 0);
    socklen_t len = sizeof a; ::getsockname(blocker, reinterpret_cast<sockaddr*>(&a), &len); const int port = ntohs(a.sin_port);
    Cli c; const int rc = c.run({"--fw", "/dev/fw1", "--allow-write", "serve", "--port", std::to_string(port)}); ::close(blocker);
    CHECK_EQ(rc, 2); CHECK(has(c.err.str(), "serve stopped")); CHECK(has(c.out.str(), "shutting down"));
    CHECK(ops(write_ops(c.bus)) == (std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347, 1292, 1320, 1285}));            // started, then put back in standby
}
