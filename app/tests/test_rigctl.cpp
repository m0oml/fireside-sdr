// Hamlib NET rigctl protocol (fireside/rigctl.h). Reply formats as Hamlib 4.7.2's rigctld prints them. Transmit must always be refused.
#include "fireside/rigctl.h"
#include "test_util.h"
using namespace fireside::rigctl;

namespace {
struct FakeRig : Backend {
    double hz = 7074000, w = 3600; int mode = 0; bool dig = true, key = false, busy = false; int sets = 0, mode_sets = 0; double last_w = 0;
    double freq() const override { return hz; }
    int mode_index() const override { return mode; }
    double width() const override { return w; }
    bool digital() const override { return dig; }
    bool keyed() const override { return key; }
    bool tx_busy() const override { return busy || key; }
    void set_freq(double f) override { hz = f; ++sets; }
    void set_mode(int i, double width) override { mode = i; last_w = width; if (width > 0) w = width; ++mode_sets; }
};
}  // namespace

TEST(rigctl_get_freq_mode_vfo_short_and_long) {
    FakeRig r; Session s(r);
    CHECK_EQ(s.feed("f\n"), std::string("7074000\n"));
    CHECK_EQ(s.feed("\\get_freq\n"), std::string("7074000\n"));
    CHECK_EQ(s.feed("m\n"), std::string("PKTUSB\n3600\n"));                    // a data decoder is on: PKTUSB
    r.dig = false; CHECK_EQ(s.feed("m\n"), std::string("USB\n3600\n"));
    r.mode = 1; CHECK_EQ(s.feed("m\n"), std::string("LSB\n3600\n")); r.mode = 3; CHECK_EQ(s.feed("m\n"), std::string("CW\n3600\n"));
    CHECK_EQ(s.feed("v\n"), std::string("VFOA\n"));
    CHECK_EQ(s.feed("t\n"), std::string("0\n"));
    CHECK_EQ(s.feed("s\n"), std::string("0\nVFOA\n"));
    CHECK_EQ(s.feed("\\chk_vfo\n"), std::string("0\n"));
    CHECK_EQ(s.feed("\\get_powerstat\n"), std::string("1\n"));
    CHECK_EQ(s.feed("\\get_lock_mode\n"), std::string("0\nRPRT 0\n"));                 // as rigctld 4.7 prints it (Hamlib asks after set_freq)
    CHECK_EQ(s.feed("+\\get_lock_mode\n"), std::string("get_lock_mode:\nLocked: 0\nRPRT 0\n"));
    CHECK_EQ(s.feed("+\\chk_vfo\n"), std::string("ChkVFO: 0\n"));
    CHECK_EQ(s.feed("\\get_vfo_info VFOA\n"), std::string("7074000\nCW\n3600\n0\n0\n"));
}
TEST(rigctl_set_freq_and_limits) {
    FakeRig r; Session s(r);
    CHECK_EQ(s.feed("F 14074000\n"), std::string("RPRT 0\n")); CHECK_EQ(r.hz, 14074000.0);
    CHECK_EQ(s.feed("\\set_freq 21074000.000000\n"), std::string("RPRT 0\n")); CHECK_EQ(r.hz, 21074000.0);
    for (const char* bad : {"F 0\n", "F -5\n", "F 65000001\n", "F 1e12\n", "F abc\n", "F nan\n", "F\n"}) CHECK_EQ(s.feed(bad), std::string("RPRT -1\n"));
    CHECK_EQ(r.hz, 21074000.0); CHECK_EQ(r.sets, 2);
    CHECK_EQ(s.feed("F 65000000\n"), std::string("RPRT 0\n"));                 // the top of the range
}
TEST(rigctl_dial_refused_while_transmitting) {
    FakeRig r; Session s(r); r.busy = true;
    CHECK_EQ(s.feed("F 14074000\n"), std::string("RPRT -9\n")); CHECK_EQ(s.feed("M LSB 2400\n"), std::string("RPRT -9\n"));
    CHECK_EQ(r.sets, 0); CHECK_EQ(r.mode_sets, 0);
    r.busy = false; r.key = true; CHECK_EQ(s.feed("F 14074000\n"), std::string("RPRT -9\n")); CHECK_EQ(s.feed("t\n"), std::string("1\n"));   // get_ptt reports the real state
}
TEST(rigctl_refuses_ptt_without_a_ptt_request) {
    FakeRig r; Session s(r);
    for (const char* k : {"T 1\n", "T 2\n", "T 3\n", "\\set_ptt 1\n", "+T 1\n"}) { const std::string o = s.feed(k); CHECK(o.find("RPRT -9") != std::string::npos); }
    CHECK_EQ(s.refused_ptt(), uint64_t(5));
    CHECK_EQ(s.feed("T 0\n"), std::string("RPRT 0\n"));                        // unkeying is harmless
    CHECK_EQ(s.feed("b CQ\n"), std::string("RPRT -9\n"));                       // send_morse
    CHECK_EQ(s.feed("\\send_voice_mem 1\n"), std::string("RPRT -9\n"));
    CHECK_EQ(s.feed("\\set_powerstat 0\n"), std::string("RPRT -9\n"));          // power off
    CHECK_EQ(s.feed("\\set_powerstat 1\n"), std::string("RPRT 0\n"));
    CHECK(dump_state().find("ptt_type=0x0") != std::string::npos);
}
namespace {
struct FakePtt : PttRequest {
    bool allow = true; int on_calls = 0, off_calls = 0, beats = 0, last_client = -1; bool held = false;
    bool request_ptt(int client, bool on) override { last_client = client; if (!on) { ++off_calls; held = false; return true; } ++on_calls; if (allow) held = true; return allow; }
    void heartbeat(int client) override { last_client = client; ++beats; }
};
}  // namespace
TEST(rigctl_ptt_goes_only_through_the_ptt_request) {
    FakeRig r; FakePtt p; Session s(r, &p, 7);
    CHECK_EQ(s.feed("T 1\n"), std::string("RPRT 0\n")); CHECK(p.held); CHECK_EQ(p.on_calls, 1); CHECK_EQ(p.last_client, 7);
    CHECK_EQ(s.feed("T 0\n"), std::string("RPRT 0\n")); CHECK(!p.held); CHECK_EQ(p.off_calls, 1);
    CHECK_EQ(s.feed("+T 3\n"), std::string("set_ptt: 3\nRPRT 0\n")); CHECK_EQ(p.on_calls, 2);   // PTT data
    p.allow = false; CHECK_EQ(s.feed("T 1\n"), std::string("RPRT -9\n")); CHECK_EQ(s.refused_ptt(), uint64_t(1));
    CHECK_EQ(s.feed("T 0\n"), std::string("RPRT 0\n"));                     // releasing is never refused
    CHECK_EQ(s.feed("T 9\n"), std::string("RPRT -1\n")); CHECK_EQ(s.feed("T\n"), std::string("RPRT -1\n"));
    CHECK_EQ(p.on_calls, 3);                                                   // T 9 / T with no value never asked
    const int b0 = p.beats; s.feed("f\nm\n"); CHECK_EQ(p.beats, b0 + 2);    // every command is a heartbeat
    r.key = true; CHECK_EQ(s.feed("t\n"), std::string("1\n"));               // get_ptt still reports the real state
}
TEST(rigctl_set_mode_names_and_widths) {
    FakeRig r; Session s(r); r.dig = false;
    CHECK_EQ(s.feed("M LSB 2400\n"), std::string("RPRT 0\n")); CHECK_EQ(r.mode, 1); CHECK_EQ(r.w, 2400.0);
    CHECK_EQ(s.feed("M PKTUSB 0\n"), std::string("RPRT 0\n")); CHECK_EQ(r.mode, 0); CHECK_EQ(r.last_w, -1.0);   // 0 = leave the filter as the mode leaves it
    CHECK_EQ(s.feed("m\n"), std::string("PKTUSB\n2400\n"));                     // remembers the data request with no decoder on
    CHECK_EQ(s.feed("M USB -1\n"), std::string("RPRT 0\n")); CHECK_EQ(s.feed("m\n"), std::string("USB\n2400\n"));
    CHECK_EQ(s.feed("M AM 6000\n"), std::string("RPRT 0\n")); CHECK_EQ(r.mode, 2);
    CHECK_EQ(s.feed("M CWR 500\n"), std::string("RPRT 0\n")); CHECK_EQ(r.mode, 3);
    CHECK_EQ(s.feed("M FM\n"), std::string("RPRT 0\n")); CHECK_EQ(r.mode, 4);
    CHECK_EQ(s.feed("M RTTY 500\n"), std::string("RPRT -11\n")); CHECK_EQ(s.feed("M USB wide\n"), std::string("RPRT -1\n"));
    CHECK_EQ(r.mode, 4);
    FakeRig r2; Session s2(r2);                                                    // FT8 decoder on: PKTUSB until the client picks a mode itself
    CHECK_EQ(s2.feed("m\n"), std::string("PKTUSB\n3600\n")); s2.feed("M USB 0\n"); CHECK_EQ(s2.feed("m\n"), std::string("USB\n3600\n"));
}
TEST(rigctl_extended_responses) {
    FakeRig r; Session s(r);
    CHECK_EQ(s.feed("+f\n"), std::string("get_freq:\nFrequency: 7074000\nRPRT 0\n"));
    CHECK_EQ(s.feed("+m\n"), std::string("get_mode:\nMode: PKTUSB\nPassband: 3600\nRPRT 0\n"));
    CHECK_EQ(s.feed("+\\set_freq 7074500\n"), std::string("set_freq: 7074500\nRPRT 0\n"));
    CHECK_EQ(s.feed("+T 0\n"), std::string("set_ptt: 0\nRPRT 0\n"));
    CHECK_EQ(s.feed(";f\n"), std::string("get_freq:;Frequency: 7074500;RPRT 0\n"));
    CHECK_EQ(s.feed("|f\n"), std::string("get_freq:|Frequency: 7074500|RPRT 0\n"));
    CHECK_EQ(s.feed("+\\get_vfo_info VFOA\n"), std::string("get_vfo_info: VFOA\nFreq: 7074500\nMode: PKTUSB\nWidth: 3600\nSplit: 0\nSatMode: 0\nRPRT 0\n"));
}
TEST(rigctl_several_commands_partial_lines_unknown_and_quit) {
    FakeRig r; Session s(r);
    CHECK_EQ(s.feed("F 14074000 f m\n"), std::string("RPRT 0\n14074000\nPKTUSB\n3600\n"));
    CHECK_EQ(s.feed("f"), std::string("")); CHECK_EQ(s.feed("\r\n"), std::string("14074000\n"));    // split across reads, CRLF
    CHECK_EQ(s.feed("\\no_such_thing 1 2 f\n"), std::string("RPRT -4\n"));     // unknown: nothing done, rest of the line dropped
    CHECK_EQ(s.feed("l STRENGTH\n"), std::string("RPRT -11\n"));               // argument consumed: in step afterwards
    CHECK_EQ(s.feed("S 1 VFOB\n"), std::string("RPRT -11\n")); CHECK_EQ(s.feed("S 0 VFOA\n"), std::string("RPRT 0\n"));
    CHECK_EQ(s.feed("V VFOB\n"), std::string("RPRT -11\n")); CHECK_EQ(s.feed("V VFOA\n"), std::string("RPRT 0\n"));
    CHECK_EQ(s.feed(std::string(2000, 'x')), std::string("RPRT -1\n"));       // a runaway line
    CHECK_EQ(s.feed("\nq\nf\n"), std::string("RPRT -4\n")); CHECK(s.closed());   // the tail of the runaway, then quit: nothing after it
    CHECK_EQ(r.hz, 14074000.0);
}
TEST(rigctl_dump_state_shape) {
    const std::string d = dump_state();
    CHECK(d.rfind("1\n2\n0\n100000.000000 65000000.000000 0xc2f ", 0) == 0);
    CHECK(d.find("0 0 0 0 0 0 0\n0 0 0 0 0 0 0\n") != std::string::npos);       // no transmit ranges
    CHECK(d.size() > 10 && d.substr(d.size() - 5) == "done\n");
}
