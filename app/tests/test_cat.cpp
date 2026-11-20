// CAT (Kenwood TS-2000 subset) protocol and TCP tests. Transmit must always be refused.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "fireside/cat.h"
#include "test_util.h"
using namespace fireside;

TEST(cat_identity_and_defaults) {
    CatSession s;
    CHECK_EQ(s.feed("ID;"), std::string("ID019;"));
    CHECK_EQ(s.feed("FA;"), std::string("FA00007100000;"));
    CHECK_EQ(s.feed("MD;"), std::string("MD2;"));
    CHECK_EQ(s.feed("PS;"), std::string("PS1;"));
    CHECK_EQ(s.feed("FR;FT;"), std::string("FR0;FT0;"));
    CHECK_EQ(s.feed("AI;"), std::string("AI0;"));
}
TEST(cat_set_frequency_and_query_and_callbacks) {
    CatSession s; std::vector<uint64_t> tuned; s.on_rx_freq = [&](uint64_t hz) { tuned.push_back(hz); };
    CHECK_EQ(s.feed("FA00014074000;"), std::string(""));                              // set: no reply
    CHECK_EQ(s.feed("FA;"), std::string("FA00014074000;"));
    CHECK(tuned == (std::vector<uint64_t>{14074000}));
    s.feed("FA00014074000;"); CHECK_EQ(tuned.size(), size_t(1));                      // unchanged: no retune
    s.feed("FB00007074000;"); CHECK_EQ(tuned.size(), size_t(1));                      // VFO B is not the receive VFO
    s.feed("FR1;"); CHECK(tuned == (std::vector<uint64_t>{14074000, 7074000}));       // switching RX to B retunes
    CHECK_EQ(s.state().freq_b, uint64_t(7074000));
}
TEST(cat_rejects_bad_frequencies) {
    CatSession s;
    for (const char* bad : {"FA123;", "FA0000710000;", "FA000071000000;", "FA0000710000x;", "FA00000000000;", "FA00065000001;", "FA99999999999;"})
        CHECK_EQ(s.feed(bad), std::string("?;"));
    CHECK_EQ(s.state().freq_a, uint64_t(7100000));
    CHECK_EQ(s.feed("FA00065000000;"), std::string(""));                              // 65 MHz exactly is the top of the range
}
TEST(cat_mode_set_get_and_validation) {
    CatSession s; std::vector<int> modes; s.on_mode = [&](int m) { modes.push_back(m); };
    s.feed("MD1;"); CHECK_EQ(s.feed("MD;"), std::string("MD1;")); s.feed("MD1;");
    CHECK(modes == (std::vector<int>{1}));
    for (const char* bad : {"MD0;", "MD8;", "MDA;", "MD12;"}) CHECK_EQ(s.feed(bad), std::string("?;"));
    CHECK_EQ(s.feed("MD9;"), std::string("")); CHECK_EQ(s.state().mode, 9);
}
TEST(cat_if_answer_is_37_chars_and_carries_freq_mode_and_split) {
    CatSession s; s.feed("FA00014250000;"); s.feed("MD3;");
    auto r = s.feed("IF;");
    CHECK_EQ(r.size(), size_t(37)); CHECK_EQ(r.substr(0, 13), std::string("IF00014250000")); CHECK_EQ(r[28], '3'); CHECK_EQ(r.back(), ';');
    CHECK_EQ(r[27], '0'); CHECK_EQ(r[29], '0'); CHECK_EQ(r[31], '0');                 // RX, VFO A, not split
    s.feed("FT1;"); r = s.feed("IF;"); CHECK_EQ(r[31], '1');                          // TX on B while RX on A = split
    CHECK_EQ(s.feed("IF0;"), std::string("?;"));
}
TEST(cat_transmit_is_always_refused_and_counted) {
    CatSession s;
    for (const char* tx : {"TX;", "TX0;", "TX1;", "TX2;", "MX1;", "KYhello;"}) CHECK_EQ(s.feed(tx), std::string("?;"));
    CHECK_EQ(s.refused_tx(), uint64_t(6));
    CHECK_EQ(s.feed("RX;"), std::string(""));                                         // returning to receive is harmless
}
TEST(cat_power_off_is_refused_but_power_on_is_accepted) {
    CatSession s;
    CHECK_EQ(s.feed("PS0;"), std::string("?;")); CHECK(s.state().power_on);
    CHECK_EQ(s.feed("PS1;"), std::string(""));
}
TEST(cat_af_gain_and_smeter) {
    CatSession s; CHECK_EQ(s.feed("AG0;"), std::string("AG0128;"));
    s.feed("AG0255;"); CHECK_EQ(s.feed("AG0;"), std::string("AG0255;"));
    CHECK_EQ(s.feed("AG0256;"), std::string("?;")); CHECK_EQ(s.feed("AG1;"), std::string("?;"));
    s.set_smeter(12); CHECK_EQ(s.feed("SM0;"), std::string("SM00012;"));
    s.set_smeter(99); CHECK_EQ(s.feed("SM0;"), std::string("SM00030;"));
}
TEST(cat_partial_commands_and_garbage_are_handled) {
    CatSession s;
    CHECK_EQ(s.feed("FA000"), std::string("")); CHECK_EQ(s.feed("14074"), std::string("")); CHECK_EQ(s.feed("000;FA;"), std::string("FA00014074000;"));
    CHECK_EQ(s.feed("\r\nID;\r\n"), std::string("ID019;"));
    CHECK_EQ(s.feed("ZZ;"), std::string("?;")); CHECK_EQ(s.feed(";"), std::string("?;")); CHECK_EQ(s.feed("A;"), std::string("?;"));
    CHECK_EQ(s.feed(std::string(100, 'x')), std::string("?;"));                       // runaway line: one error, then skipped up to its terminator
    CHECK_EQ(s.feed("xx;"), std::string(""));
    CHECK_EQ(s.feed("ID;"), std::string("ID019;"));                                   // and the session recovers
    CHECK_EQ(s.feed("ID;FA;MD;"), std::string("ID019;FA00014074000;MD2;"));          // several commands in one read
}

TEST(cat_connections_have_independent_input_buffers) {
    CatSession s; CatConn a, b;
    CHECK_EQ(s.feed("FA000", a), std::string(""));                                   // client A stalls mid-command
    CHECK_EQ(s.feed("ID;", b), std::string("ID019;"));                               // client B is unaffected
    CHECK_EQ(s.feed("14074000;", a), std::string(""));                               // A completes later
    CHECK_EQ(s.state().freq_a, uint64_t(14074000));
}

namespace {
struct Client {
    int fd;
    explicit Client(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) throw TestFailure("connect failed");
    }
    ~Client() { ::close(fd); }
    std::string ask(const std::string& s, size_t want) {
        (void)!::write(fd, s.data(), s.size()); std::string r;
        while (r.size() < want) { pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 2000) <= 0) break; char b[256]; ssize_t n = ::read(fd, b, sizeof b); if (n <= 0) break; r.append(b, size_t(n)); }
        return r;
    }
};
}  // namespace

TEST(cat_tcp_server_round_trip_two_clients_and_loopback_only) {
    CatSession s; CatTcpServer srv(s); const uint16_t port = srv.start(0); CHECK(port != 0);
    Client a(port), b(port);
    CHECK_EQ(a.ask("ID;", 6), std::string("ID019;"));
    a.ask("FA00018100000;", 0); CHECK_EQ(b.ask("FA;", 14), std::string("FA00018100000;"));   // shared state
    CHECK_EQ(b.ask("TX1;", 2), std::string("?;")); CHECK_EQ(s.refused_tx(), uint64_t(1));
    CHECK_THROWS(srv.start(0), std::runtime_error);                                  // already running
    srv.stop(); srv.stop();                                                          // idempotent
}
TEST(cat_tcp_server_can_restart_and_survives_a_dropped_client) {
    CatSession s; CatTcpServer srv(s);
    uint16_t port = srv.start(0); { Client gone(port); gone.ask("FA000", 0); }       // disconnect mid-command
    Client again(port); CHECK_EQ(again.ask("ID;", 6), std::string("ID019;"));
    srv.stop(); port = srv.start(0); Client c(port); CHECK_EQ(c.ask("MD;", 4), std::string("MD2;"));
}
