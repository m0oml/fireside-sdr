// ATU protocol, per-band ATU memory, PA power / SWR meter and the transmit gate, against a simulated tuner that behaves like the one on the
// radio: result word [queue][b3][b2][code], 0x00FFFFFF = nothing pending,
// a pass followed by the relay / power / SWR reports, a fail with its reason.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <deque>
#include "fake_dice.h"
#include "fireside/atu.h"
#include "fireside/atu_memory.h"
#include "fireside/sim_radio.h"
#include "fireside/tx_gate.h"
#include "fireside/tx_meter.h"
#include "fireside/tuning.h"
#include "fireside/rxant.h"
#include "fireside/txlines.h"
#include "test_util.h"
using namespace fireside; using namespace fakedice;

namespace {
uint32_t res(uint8_t code, uint8_t b2 = 0, uint8_t b3 = 0, uint8_t rem = 0) { return uint32_t(rem) << 24 | uint32_t(b3) << 16 | uint32_t(b2) << 8 | code; }

struct FakeClock { double t = 1000.0; atu::Clock clock() { return {[this] { return t; }, [this](double s) { t += s; }}; } };

// The tuner, PA detector and EEPROM behind the simulated mailbox: the library model (also used by fireside --simulate).
using AtuSim = fireside::sim::RadioModel;

struct Rig {
    FakeDiceBus bus; AtuSim sim; PalMailbox mb; Flex fx; FakeClock clk;
    Rig() : bus(), sim(bus), mb(bus, ByteOrder::Big, 1.0, 0), fx(mb, [] { Safety s; s.allow_write = true; return s; }()) { mb.verify(); }
};
std::vector<Wr> sent(Rig& r, const char* op) { std::vector<Wr> o; for (auto& w : write_ops(r.bus)) if (w.op == opv(op)) o.push_back(w); return o; }

// 20 m calibration from this radio's EEPROM backup (2026-11-17): PA bridge volts at 1/2/5/10/20/90 W, reflected factor 0.983.
txm::PaCal cal20() {
    txm::PaCal c; for (int b = 0; b < txm::kBands; ++b) { c.bridge[size_t(b)] = {0.148, 0.216, 0.365, 0.544, 0.806, 1.78}; c.rev_corr[size_t(b)] = 1.0; }
    c.bridge[5] = {0.14794, 0.21636, 0.36493, 0.54351, 0.80581, 1.77956}; c.rev_corr[5] = 0.983; c.valid = true; return c;
}
}  // namespace

// ---------------------------------------------------------------- protocol
TEST(atu_result_words_decode_as_in_the_capture) {
    CHECK(!atu::parse(0x00FFFFFFu));
    auto m = atu::parse(0x04240405u); CHECK(m); CHECK_EQ(int(m->code), 5); CHECK_EQ(int(m->b2), 0x04); CHECK_EQ(int(m->b3), 0x24); CHECK_EQ(int(m->remaining), 4);     // "4 more messages queued"
    atu::Status s; atu::apply(s, *m); CHECK(s.fwd_w); CHECK_NEAR(*s.fwd_w, 10.60, 1e-9);            // "FWDPWR = 0x0424 = 10.60 W"
    atu::apply(s, *atu::parse(0x020A)); CHECK(s.tune_pass && !*s.tune_pass); CHECK_EQ(int(s.fail_reason), 2);   // TUNEFAIL reason 2
    atu::apply(s, *atu::parse(0x09)); CHECK(*s.tune_pass);
    atu::apply(s, *atu::parse(res(atu::R_SWR, 0, 0))); CHECK_NEAR(*s.swr, 1.0, 1e-9);
    atu::apply(s, *atu::parse(res(atu::R_SWR, 0, 64))); CHECK_NEAR(*s.swr, 3.0, 1e-9);            // rho = sqrt(64/256) = 0.5
    atu::apply(s, *atu::parse(res(atu::R_SWRTHRESH, 2))); CHECK_NEAR(*s.threshold, 1.5, 1e-9);
    atu::apply(s, *atu::parse(0xF8)); CHECK(*s.tune_pass);                                         // the unknown 0xF8 seen once on the radio is ignored
    CHECK(atu::threshold_request(1.5) == std::optional<uint8_t>(atu::SWR15)); CHECK(!atu::threshold_request(1.4));
    CHECK_EQ(atu::send_d1(atu::SET_IND, 30), uint32_t(65 << 8 | 30));
}
TEST(atu_start_up_sends_what_powersdr_sends_and_no_rf) {
    Rig r; atu::Atu a(r.fx, r.clk.clock()); a.init();
    auto w = sent(r, "RDAL_OP_ATU_SEND_CMD"); CHECK_EQ(w.size(), size_t(3));
    CHECK_EQ(w[0].d1, atu::send_d1(atu::AUTO_OFF)); CHECK_EQ(w[1].d1, atu::send_d1(atu::STANDBY)); CHECK_EQ(w[2].d1, atu::send_d1(atu::LOZ));
    CHECK(sent(r, "RDAL_OP_SET_MOX").empty()); CHECK(a.status().active && !*a.status().active); CHECK(a.status().auto_on && !*a.status().auto_on);
    CHECK(!r.fx.safety().permitted("RDAL_OP_ATU_SEND_CMD"));                                         // the permit is only held around each send
}
TEST(atu_saved_relays_go_back_without_transmitting) {
    Rig r; atu::Atu a(r.fx, r.clk.clock());
    CHECK(a.set_relays(30, 7, true)); CHECK_EQ(r.sim.ind, 30); CHECK_EQ(r.sim.cap, 7); CHECK_EQ(r.sim.hiz, 1);
    CHECK_EQ(*a.status().ind, 30); CHECK_EQ(*a.status().cap, 7);
    CHECK(sent(r, "RDAL_OP_SET_MOX").empty());
    for (auto& w : sent(r, "RDAL_OP_ATU_SEND_CMD")) CHECK(w.d1 >> 8 != atu::FULL_TUNE && w.d1 >> 8 != atu::MEM_TUNE);
    CHECK_THROWS(a.set_relays(128, 0, false), std::invalid_argument);
    r.sim.on_send(0);                                                                                 // (no-op)
    AtuSim& s = r.sim; s.bus.dynamic[opv("RDAL_OP_ATU_SEND_CMD")] = [&s] { const uint32_t d1 = be32(s.bus.mem[kPalResult]); if ((d1 >> 8) != atu::SET_CAP) s.on_send(d1); return 0u; };
    CHECK(!a.set_relays(31, 9, false));                                                               // a tuner that ignores one setting is noticed
}
// Reports already queued before a restore (the old relays) must not make it "NOT confirmed": seen on the radio 2026-11-25 on 80, 30 and 20 m.
TEST(atu_restore_is_confirmed_past_old_queued_reports) {
    Rig r; atu::Atu a(r.fx, r.clk.clock());
    r.sim.ind = 50; r.sim.cap = 16; r.sim.hiz = 1;
    for (int k = 0; k < 4; ++k) { r.sim.q.push_back(res(atu::R_IND, 50)); r.sim.q.push_back(res(atu::R_CAP, 16)); r.sim.q.push_back(res(atu::R_HILOZ, 1)); r.sim.q.push_back(res(atu::R_FWDPWR, 0, 0)); }
    CHECK(a.set_relays(4, 39, true)); CHECK_EQ(*a.status().ind, 4); CHECK_EQ(*a.status().cap, 39); CHECK_EQ(r.sim.ind, 4);
}
TEST(safety_gate_allows_tuner_settings_but_never_a_tune_erase_or_key_without_the_gate) {
    Rig r; Safety& s = r.fx.safety(); s.permit("RDAL_OP_ATU_SEND_CMD"); s.permit("RDAL_OP_SET_MOX");
    for (uint8_t req : {uint8_t(atu::FULL_TUNE), uint8_t(atu::MEM_TUNE), uint8_t(42), uint8_t(43), uint8_t(57), uint8_t(11), uint8_t(68), uint8_t(67), uint8_t(200)})
        CHECK_THROWS(r.fx.set("RDAL_OP_ATU_SEND_CMD", atu::send_d1(req), 0), SafetyError);
    CHECK_THROWS(r.fx.set("RDAL_OP_ATU_SEND_CMD", atu::send_d1(atu::SWR15, 1), 0), SafetyError);    // extra bytes on a no-argument request
    CHECK_THROWS(r.fx.set("RDAL_OP_SET_MOX", 1, 0), SafetyError);                                     // keying needs the gate's grant
    s.set_tx_grant(true);
    r.fx.set("RDAL_OP_ATU_SEND_CMD", atu::send_d1(atu::FULL_TUNE), 0);                                // with the grant: a tune passes
    for (uint8_t req : {uint8_t(42), uint8_t(43), uint8_t(57), uint8_t(11)}) CHECK_THROWS(r.fx.set("RDAL_OP_ATU_SEND_CMD", atu::send_d1(req), 0), SafetyError);   // erase / reset / ant 2: never
    CHECK_THROWS(r.fx.set("RDAL_OP_SET_MOX", 2, 0), SafetyError); CHECK_THROWS(r.fx.set("RDAL_OP_SET_PA_BIAS", 1, 0), SafetyError);
    s.set_tx_grant(false); s.unpermit("RDAL_OP_ATU_SEND_CMD");
    CHECK_THROWS(r.fx.set("RDAL_OP_ATU_SEND_CMD", atu::send_d1(atu::SWR15), 0), SafetyError);        // nothing at all without the permit
    Safety copy = s; s.set_tx_grant(true); Safety copy2 = s; CHECK(!copy.tx_grant()); CHECK(!copy2.tx_grant());   // a copy never inherits the grant
}

// ---------------------------------------------------------------- transmit gate
TEST(tx_gate_refuses_until_armed_and_keys_only_through_set_mox) {
    Rig r; TxGate g(r.fx, {}, r.clk.clock()); std::string why;
    CHECK(!g.key("test", 1.0, &why)); CHECK(why.find("not armed") != std::string::npos); CHECK(write_ops(r.bus).empty());
    g.arm(); CHECK(g.key("test", 1.0)); CHECK(g.keyed()); CHECK(r.sim.keyed); CHECK(r.fx.safety().tx_grant());
    auto w = sent(r, "RDAL_OP_SET_MOX"); CHECK_EQ(w.size(), size_t(1)); CHECK_EQ(w[0].d1, 1u); CHECK_EQ(w[0].d2, 0u);
    CHECK(!g.key("again", 1.0, &why)); CHECK(why.find("already") != std::string::npos);
    g.unkey("done"); CHECK(!g.keyed()); CHECK(!r.sim.keyed); CHECK(!r.fx.safety().tx_grant()); CHECK(!r.fx.safety().permitted("RDAL_OP_SET_MOX"));
    w = sent(r, "RDAL_OP_SET_MOX"); CHECK_EQ(w.size(), size_t(2)); CHECK_EQ(w[1].d1, 0u);
    g.unkey("twice"); CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), size_t(2));                        // unkey when not keyed sends nothing
}
TEST(tx_gate_deadline_power_and_swr_limits_unkey) {
    Rig r; GateConfig c; c.max_key_s = 2.0; c.max_fwd_w = 20; c.max_swr = 3.0; TxGate g(r.fx, c, r.clk.clock()); g.arm();
    CHECK(g.key("t", 60.0)); r.clk.t += 1.9; g.tick(); CHECK(g.keyed()); r.clk.t += 0.2; g.tick(); CHECK(!g.keyed());   // never longer than max_key_s
    CHECK(g.last_unkey_reason().find("time limit") != std::string::npos);
    CHECK(g.key("t", 1.0)); txm::Reading rd; rd.fwd_w = 25; g.report(rd); CHECK(!g.keyed()); CHECK(g.last_unkey_reason().find("forward power") != std::string::npos);
    CHECK(g.key("t", 1.0)); rd = {}; rd.fwd_w = 1.5; rd.swr = 9.0; g.report(rd); CHECK(g.keyed());   // too little power to judge the SWR
    rd.fwd_w = 10; g.report(rd); CHECK(!g.keyed()); CHECK(g.last_unkey_reason().find("SWR") != std::string::npos);
    { GateConfig c2; c2.max_rev_w = 20; c2.max_swr = 1e9; TxGate g2(r.fx, c2, r.clk.clock()); g2.arm(); CHECK(g2.key("t", 1.0));
      rd = {}; rd.fwd_w = 16; rd.rev_w = 11; rd.swr = 11.2; g2.report(rd); CHECK(g2.keyed());          // a tune's extreme mismatch: below the reflected limit, keeps going
      rd.rev_w = 21; g2.report(rd); CHECK(!g2.keyed()); CHECK(g2.last_unkey_reason().find("reflected") != std::string::npos); }
    CHECK(g.key("t", 1.0)); g.disarm(); CHECK(!g.keyed()); CHECK(!g.armed()); CHECK(!g.key("t", 1.0));
    { Rig r2; TxGate g2(r2.fx, {}, r2.clk.clock()); g2.arm(); g2.key("t", 1.0); CHECK(r2.sim.keyed); }   // (the destructor unkeys: checked below)
    Rig r3; { TxGate g3(r3.fx, {}, r3.clk.clock()); g3.arm(); g3.key("t", 1.0); } CHECK(!r3.sim.keyed); CHECK(!r3.fx.safety().tx_grant());
}
TEST(tx_gate_unkeys_even_when_the_link_fails) {
    Rig r; TxGate g(r.fx, {}, r.clk.clock()); g.arm(); CHECK(g.key("t", 1.0));
    r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});                                          // the mailbox stops answering
    g.unkey("link test"); CHECK(!g.keyed()); CHECK(!r.fx.safety().tx_grant());
    CHECK(g.log().back().find("SET_MOX 0 FAILED") != std::string::npos);                              // and says so
}

TEST(tx_gate_keeps_retrying_a_failed_unkey_until_it_gets_through) {
    Rig r; TxGate g(r.fx, {}, r.clk.clock()); g.arm(); CHECK(g.key("t", 5.0)); CHECK(r.sim.keyed);
    r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});                                          // a link glitch eats the three tries
    g.unkey("glitch"); CHECK(g.unkey_failed()); CHECK(!g.keyed()); CHECK(r.sim.keyed);               // the radio is still keyed
    CHECK(r.fx.safety().permitted("RDAL_OP_SET_MOX"));                                                // the permit is kept for the retries
    std::string why; CHECK(!g.key("again", 1.0, &why)); CHECK(why.find("may still be keyed") != std::string::npos);   // never keys on top of it
    g.tick(); CHECK(g.unkey_failed());                                                                // still stuck: still failed
    r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0});                                                // the link recovers
    g.tick(); CHECK(!g.unkey_failed()); CHECK(!r.sim.keyed);                                          // the next tick gets SET_MOX 0 through
    CHECK(g.last_unkey_reason().find("got through on a later try") != std::string::npos);
    CHECK(!r.fx.safety().permitted("RDAL_OP_SET_MOX")); CHECK(!r.fx.safety().tx_grant());
    CHECK(g.key("after", 1.0)); g.unkey("done"); CHECK(!r.sim.keyed);                                 // and it can key normally again
}
TEST(tx_gate_destructor_and_ensure_unkeyed_retry_a_failed_unkey) {
    Rig r;
    {   TxGate g(r.fx, {}, r.clk.clock()); g.arm(); CHECK(g.key("t", 5.0));
        r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy}); g.unkey("glitch"); CHECK(g.unkey_failed());
        CHECK(!g.ensure_unkeyed(0.3)); CHECK(g.unkey_failed());                                       // still stuck after 0.3 s: reported, not hidden
        r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0}); CHECK(g.ensure_unkeyed(0.3)); CHECK(!r.sim.keyed); }
    {   TxGate g(r.fx, {}, r.clk.clock()); g.arm(); CHECK(g.key("t", 5.0));
        r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy}); g.unkey("glitch"); r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0}); }
    CHECK(!r.sim.keyed); CHECK(!r.fx.safety().permitted("RDAL_OP_SET_MOX"));                         // the destructor's retries unkeyed it
}
TEST(tx_gate_refused_key_reports_a_failed_unkey_after_it) {
    Rig r; TxGate g(r.fx, {}, r.clk.clock()); g.arm();
    r.bus.stuck = true; r.bus.mem[kPalCmd] = be({kPalBusy});                                          // nothing gets through: the key and every unkey fail
    std::string why; CHECK(!g.key("t", 1.0, &why)); CHECK(g.unkey_failed());
    CHECK(why.find("refused the key") != std::string::npos); CHECK(why.find("may still be keyed") != std::string::npos);
    r.bus.stuck = false; r.bus.mem[kPalCmd] = be({0});
}

// ---------------------------------------------------------------- a whole tune
TEST(atu_tune_passes_like_the_third_tune_of_the_capture) {
    Rig r; atu::Atu a(r.fx, r.clk.clock()); TxGate g(r.fx, {}, r.clk.clock()); g.arm(); TuneMeter m;
    auto o = run_atu_tune(g, a, cal20(), 5, 1.5, true, 1, [] { return true; }, &m);
    CHECK(o); CHECK(o->result == atu::Atu::TuneResult::Pass);
    CHECK_EQ(*o->status.ind, 30); CHECK_EQ(*o->status.cap, 7); CHECK_EQ(*o->status.hiz, 1); CHECK_NEAR(*o->status.fwd_w, 10.6, 1e-9); CHECK_NEAR(*o->status.threshold, 1.5, 1e-9);
    CHECK(!g.keyed()); CHECK(!r.sim.keyed);
    // order: TX mode CWU, key, threshold, full tune, ..., unkey, mode back
    std::vector<uint32_t> ops; for (auto& w : write_ops(r.bus)) ops.push_back(w.op);
    auto at = [&](uint32_t op, size_t from = 0) { for (size_t i = from; i < ops.size(); ++i) if (ops[i] == op) return i; return ops.size(); };
    const size_t mode = at(opv("RDAL_OP_SET_TX_DSP_MODE")), key = at(opv("RDAL_OP_SET_MOX")), send = at(opv("RDAL_OP_ATU_SEND_CMD")), unkey = at(opv("RDAL_OP_SET_MOX"), key + 1), mode2 = at(opv("RDAL_OP_SET_TX_DSP_MODE"), mode + 1);
    CHECK(mode < key && key < send && send < unkey && unkey < mode2 && mode2 < ops.size());
    auto modes = sent(r, "RDAL_OP_SET_TX_DSP_MODE"); CHECK_EQ(modes[0].d1, 4u); CHECK_EQ(modes[1].d1, 1u);
    auto sends = sent(r, "RDAL_OP_ATU_SEND_CMD"); CHECK_EQ(sends[0].d1, atu::send_d1(atu::SWR15)); CHECK_EQ(sends[1].d1, atu::send_d1(atu::FULL_TUNE));   // threshold sent right before the tune
    CHECK_NEAR(m.max_fwd_w, 11.4, 0.3);                                                               // the PA meter on the readings of a passing tune (tuner: 10.6 W)
    CHECK(o->seconds < g.config().max_key_s);
    // the carrier hook: on only while keyed
    Rig r2; atu::Atu a2(r2.fx, r2.clk.clock()); TxGate g2(r2.fx, {}, r2.clk.clock()); g2.arm(); std::vector<std::pair<bool, bool>> ev;   // (carrier on?, keyed then?)
    run_atu_tune(g2, a2, cal20(), 5, 1.5, true, 1, [] { return true; }, nullptr, nullptr, [&](bool on) { ev.push_back({on, r2.sim.keyed}); });
    CHECK_EQ(ev.size(), size_t(2)); CHECK(ev[0].first && ev[0].second); CHECK(!ev[1].first && ev[1].second); CHECK(!r2.sim.keyed);
}
TEST(atu_tune_fail_no_carrier_and_no_answer_all_end_unkeyed) {
    { Rig r; r.sim.best_swr = 1.8; atu::Atu a(r.fx, r.clk.clock()); TxGate g(r.fx, {}, r.clk.clock()); g.arm();
      auto o = run_atu_tune(g, a, cal20(), 5, 1.1, true, 1, [] { return true; });
      CHECK(o && o->result == atu::Atu::TuneResult::Fail); CHECK_EQ(int(o->fail_reason), 2); CHECK(!r.sim.keyed); }
    { Rig r; atu::Atu a(r.fx, r.clk.clock()); TxGate g(r.fx, {}, r.clk.clock()); g.arm(); std::string why;
      CHECK(!run_atu_tune(g, a, cal20(), 5, 1.5, true, 1, [] { return false; }, nullptr, &why)); CHECK(why.find("carrier") != std::string::npos);
      CHECK(write_ops(r.bus).empty()); }                                                              // nothing at all is sent without a tune carrier
    { Rig r; atu::Atu a(r.fx, r.clk.clock()); TxGate g(r.fx, {}, r.clk.clock()); std::string why;
      CHECK(!run_atu_tune(g, a, cal20(), 5, 1.5, true, 1, [] { return true; }, nullptr, &why)); CHECK(write_ops(r.bus).empty()); }   // not armed
    { Rig r; r.sim.silent = true; atu::Atu a(r.fx, r.clk.clock()); GateConfig c; c.max_key_s = 3.0; TxGate g(r.fx, c, r.clk.clock()); g.arm();
      auto o = run_atu_tune(g, a, cal20(), 5, 1.5, true, 1, [] { return true; });
      CHECK(o && o->result == atu::Atu::TuneResult::Timeout); CHECK(!r.sim.keyed); CHECK(g.last_unkey_reason().find("time limit") != std::string::npos);
      CHECK(o->seconds <= 3.5); }
    { Rig r; r.sim.adc_fwd = 2300; atu::Atu a(r.fx, r.clk.clock()); TxGate g(r.fx, {}, r.clk.clock()); g.arm();   // the drive runs away: the power cap stops it
      auto o = run_atu_tune(g, a, cal20(), 5, 1.5, true, 1, [] { return true; });
      CHECK(o && o->result != atu::Atu::TuneResult::Pass); CHECK(!r.sim.keyed); CHECK(g.last_unkey_reason().find("forward power") != std::string::npos); }
}

// ---------------------------------------------------------------- meters
TEST(pa_meter_hits_the_calibration_points_and_reads_swr) {
    const txm::PaCal c = cal20();
    for (int j = 0; j < 6; ++j) { const double p[6] = {1, 2, 5, 10, 20, 90}; const int adc = int(std::lround(c.bridge[5][size_t(j)] / 2.5 * 4096)); CHECK_NEAR(txm::watts(c, 5, adc), p[j], p[j] * 0.01 + 0.02); }
    double last = 0; for (int adc = 2; adc < 4096; adc += 37) { const double w = txm::watts(c, 5, adc); CHECK(w >= last); last = w; }   // monotonic, incl. the >90 W extrapolation
    CHECK_EQ(txm::watts(c, 5, 1), 0.0); CHECK_EQ(txm::watts(c, -1, 900), 0.0); CHECK_EQ(txm::watts(c, 11, 900), 0.0);
    auto r = txm::read(c, 5, 966, 238); CHECK_NEAR(r.fwd_w, 11.45, 0.1); CHECK_NEAR(r.rev_w, 0.96 * 0.983, 0.05); CHECK(r.swr); CHECK(*r.swr > 1.5 && *r.swr < 2.0);
    CHECK(!txm::read(c, 5, 200, 100).swr);                                                            // under 1 W: no SWR
    CHECK_NEAR(*txm::read(c, 5, 966, 966).swr, 25.0, 1e-9);                                           // reflected >= forward: shown as 25
    txm::PaCal bad; CHECK_EQ(txm::watts(bad, 5, 900), 0.0); CHECK(!txm::read(bad, 5, 900, 100).swr);  // no calibration: no meter
}
TEST(supply_and_pa_temperature_readings_use_powersdrs_scaling) {
    CHECK_NEAR(txm::supply_volts(2055), 13.80, 0.01); CHECK_NEAR(txm::supply_volts(4095), 27.49, 0.01); CHECK_EQ(txm::supply_volts(0), 0.0);
    CHECK_NEAR(txm::pa_temp_c(959), 35.0, 0.2); CHECK_NEAR(txm::pa_temp_c(360), 201.1, 0.2);     // 0.585 V = 35 C; 0.22 V = 201 C (PowerSDR: 301 - mV / 2.2)
    sim::DiceBus bus; sim::RadioModel r(bus);
    CHECK_NEAR(txm::supply_volts(int(r.pa_adc(txm::kAdcSupply))), 13.8, 0.01); CHECK_NEAR(txm::pa_temp_c(int(r.pa_adc(txm::kAdcTemp))), 35.0, 0.2);   // the simulated radio, on receive
}
TEST(pa_meter_calibration_comes_from_the_eeprom_or_not_at_all) {
    Decoded d; TableResult b; b.name = "PA bridge"; b.pointer_ok = b.checksum_ok = true; TableResult s; s.name = "PA SWR"; s.pointer_ok = s.checksum_ok = true;
    for (int i = 0; i < 11; ++i) { b.values.push_back({0.15, 0.22, 0.37, 0.54, 0.81, 1.78}); s.values.push_back({1.0 + 0.05 * i}); }
    d.tables = {b, s}; auto c = txm::PaCal::from_eeprom(d); CHECK(c.valid); CHECK_NEAR(c.rev_corr[10], 1.5, 1e-9);
    d.tables[0].values[3][2] = 0.1; CHECK(!txm::PaCal::from_eeprom(d).valid);                          // not increasing
    d.tables[0].values[3][2] = 0.37; d.tables[1].checksum_ok = false; CHECK(!txm::PaCal::from_eeprom(d).valid);
    d.tables.pop_back(); CHECK(!txm::PaCal::from_eeprom(d).valid);
    const auto img = read_bin("trx.bin"); auto real = txm::PaCal::from_eeprom(decode_eeprom(img)); (void)real;   // the test image decodes without throwing
}
TEST(tx_bands_match_the_eeprom_rows) {
    CHECK_EQ(txm::band_of(1.85e6), 0); CHECK_EQ(txm::band_of(3.575e6), 1); CHECK_EQ(txm::band_of(7.074e6), 3); CHECK_EQ(txm::band_of(14.3354e6), 5);
    CHECK_EQ(txm::band_of(50.313e6), 10); CHECK_EQ(txm::band_of(9.0e6), -1); CHECK_EQ(txm::band_of(144.0e6), -1);
    for (int b = 0; b < txm::kBands; ++b) CHECK_EQ(txm::band_from_name(txm::band_name(b)), b);
    CHECK_EQ(txm::band_from_name("2m"), -1);
}

// ---------------------------------------------------------------- per-band memory
TEST(atu_memory_round_trips_and_skips_damaged_lines) {
    atu::Memory m; atu::Setting s; s.ind = 30; s.cap = 7; s.hiz = true; s.swr = 1.0; s.fwd_w = 10.6; s.freq_hz = 14335400; s.utc = "2026-11-18T23:15:23Z";
    m.store(5, s); atu::Setting t = s; t.ind = 12; t.hiz = false; t.utc = ""; m.store(3, t); m.store(11, s); m.store(-1, s);
    CHECK_EQ(m.all().size(), size_t(2));
    const std::string txt = m.to_text(); CHECK(txt.find("band=20m ind=30 cap=7 hiz=1") != std::string::npos);
    int bad = -1; auto back = atu::Memory::from_text(txt + "garbage\nband=17m ind=200 cap=1 hiz=0\nband=99m ind=1 cap=1 hiz=0\nband=15m ind=x cap=1 hiz=0\n# comment\n\n", &bad);
    CHECK_EQ(bad, 4); CHECK_EQ(back.all().size(), size_t(2));
    auto r = back.recall(5); CHECK(r); CHECK_EQ(r->ind, 30); CHECK_EQ(r->cap, 7); CHECK(r->hiz); CHECK_NEAR(r->fwd_w, 10.6, 1e-9); CHECK_EQ(r->utc, std::string("2026-11-18T23:15:23Z"));
    CHECK(!back.recall(3)->hiz); CHECK(back.recall(3)->utc.empty()); CHECK(!back.recall(0));
    back.forget(5); CHECK(!back.recall(5));
}

// ---------------------------------------------------------------- stage 3 pieces: bias / carrier null from the EEPROM, the I/Q output
#include "fireside/tx_out.h"
#include "fireside/tx_setup.h"
namespace {
// The author's own radio's EEPROM image (never committed: backups/ is git-ignored). Make backups/reference_trx.bin a copy or symlink of your radio's F5K_<serial>_<time>_trx.bin; without it these tests skip.
// Several checks compare against what PowerSDR wrote to the author's radio, so another radio's image will not match them.
const char* kBackup = FIRESIDE_SOURCE_DIR "/../backups/reference_trx.bin";
std::vector<uint8_t> backup_image() { std::ifstream in(kBackup, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
}  // namespace
TEST(tx_setup_from_this_radios_eeprom_matches_the_powersdr_capture) {
    const auto img = backup_image(); if (img.empty()) { std::printf("  (skipped: no EEPROM backup in this checkout)\n"); return; }
    const auto c = txs::from_image(img); CHECK(c.valid);
    const uint8_t want[8] = {0xEB, 0x85, 0xE8, 0x87, 0xEB, 0x80, 0xE5, 0x83};                         // what PowerSDR wrote (capture, 35388 ms)
    for (int i = 0; i < 8; ++i) CHECK_EQ(int(c.bias_pots[size_t(i)]), int(want[i]));
    CHECK_NEAR(c.power[5][0], 0.0116, 1e-4); CHECK_NEAR(c.power[5][12], 0.0725, 1e-4);
    CHECK_NEAR(txs::drive_for_watts(c, 5, 1.0), 0.0116 / 2.23, 1e-4); CHECK_NEAR(txs::drive_for_watts(c, 5, 100), 0.0725 / 2.23, 1e-4);
    double last = 0; for (double w = 0.1; w <= 100; w *= 1.1) { const double d = txs::drive_for_watts(c, 5, w); CHECK(d >= last); last = d; }
    CHECK_EQ(txs::drive_for_watts(c, 5, 1000), txs::drive_for_watts(c, 5, 100)); CHECK_EQ(txs::drive_for_watts(c, 5, 0), 0.0); CHECK_EQ(txs::drive_for_watts(c, -1, 5), 0.0);
    CHECK(!c.carrier.empty()); (void)txs::carrier_word(c, 14.3354);
    auto blank = img; for (int i = 0; i < 8; ++i) blank[0x2C0 + size_t(i)] = 0xFF; CHECK(!txs::from_image(blank).valid);   // never bias from a blank table
    auto noflag = img; noflag[0x3F] = 0xFF; CHECK(!txs::from_image(noflag).valid);
}
TEST(safety_gate_allows_bias_and_carrier_pots_only_with_the_eeprom_values) {
    Rig r; Safety& s = r.fx.safety(); s.permit("RDAL_OP_PA_POT_SET_RDAC"); s.permit("RDAL_OP_SET_TRX_POT");
    CHECK_THROWS(r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 0, 0xEB), SafetyError);                       // not calibrated yet: refused
    s.set_tx_calibration({0xEB, 0x85, 0xE8, 0x87, 0xEB, 0x80, 0xE5, 0x83}, {0x807B867Eu});
    r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 0, 0xEB); r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 7, 0x83); r.fx.set("RDAL_OP_SET_TRX_POT", double(0x807B867Eu), 0);
    CHECK_THROWS(r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 0, 0xEC), SafetyError); CHECK_THROWS(r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 8, 0xEB), SafetyError);
    CHECK_THROWS(r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 1, 0x7A), SafetyError);                       // the raw byte on an odd pot is NOT the reviewed value
    CHECK_THROWS(r.fx.set("RDAL_OP_SET_TRX_POT", double(0x807B867Fu), 0), SafetyError);
    s.unpermit("RDAL_OP_PA_POT_SET_RDAC"); CHECK_THROWS(r.fx.set("RDAL_OP_PA_POT_SET_RDAC", 0, 0xEB), SafetyError);
}
TEST(tx_setup_writes_the_eight_pots_in_order_and_nothing_else) {
    const auto img = backup_image(); if (img.empty()) return;
    Rig r; const auto c = txs::from_image(img); txs::write_bias(r.fx, c);
    auto w = write_ops(r.bus); CHECK_EQ(w.size(), size_t(8));
    for (size_t i = 0; i < 8; ++i) { CHECK_EQ(w[i].op, opv("RDAL_OP_PA_POT_SET_RDAC")); CHECK_EQ(w[i].d1, uint32_t(i)); CHECK_EQ(w[i].d2, uint32_t(c.bias_pots[i])); }
    CHECK(!r.fx.safety().permitted("RDAL_OP_PA_POT_SET_RDAC"));
    txs::write_carrier_null(r.fx, c, 14.3354); CHECK_EQ(write_ops(r.bus).back().op, opv("RDAL_OP_SET_TRX_POT"));
    txs::TxCal bad; CHECK_THROWS(txs::write_bias(r.fx, bad), std::invalid_argument);
}
TEST(tx_output_is_zero_unless_keyed_and_never_above_the_cap) {
    txo::ToneSource src(48000, 8); std::vector<int32_t> b(4800 * 8);
    auto peak = [&](int ch) { int64_t m = 0; for (size_t f = 0; f < 4800; ++f) m = std::max<int64_t>(m, std::llabs(int64_t(b[f * 8 + size_t(ch)]))); return double(m) / 2147483647.0; };
    src.set_cap(0.01); src.set_target(0.5); src.set_tone_hz(1000); src.fill(b.data(), 4800);
    for (auto v : b) CHECK_EQ(v, 0);                                                                  // unkeyed: everything zero, whatever the target
    src.set_keyed(true); src.fill(b.data(), 4800);
    CHECK(peak(0) <= 0.01 + 1e-9 && peak(1) <= 0.01 + 1e-9); CHECK(peak(0) > 0.0099);               // clamped to the cap
    for (int ch = 2; ch < 8; ++ch) CHECK_EQ(peak(ch), 0.0);                                         // only ch0/1 ever carry anything
    src.set_cap(1.0); CHECK_NEAR(src.cap(), txs::kAbsoluteMaxDrive, 1e-12);                          // the absolute ceiling
    txo::ToneSource s2(48000, 8); s2.set_cap(0.04); s2.set_target(0.02); s2.set_keyed(true); s2.fill(b.data(), 48);   // 1 ms: still ramping (5 ms slew)
    CHECK(s2.level() < 0.02 * 0.5); s2.fill(b.data(), 480); CHECK_NEAR(s2.level(), 0.02, 1e-12);
    s2.set_keyed(false); s2.fill(b.data(), 1); CHECK_EQ(b[0], 0); CHECK_EQ(s2.level(), 0.0);
    txo::ToneSource dc(48000, 8); dc.set_cap(0.04); dc.set_target(0.01); dc.set_tone_hz(0); dc.set_keyed(true); dc.fill(b.data(), 4800);   // 0 Hz: a carrier on the DDS
    CHECK_NEAR(double(b[4799 * 8]) / 2147483647.0, 0.01, 1e-6); CHECK_EQ(b[4799 * 8 + 1], 0);
}

// ---------------------------------------------------------------- TxControl (GUI + firesidekey) on the simulated radio
#include <mutex>
#include <thread>
#include "fireside/session.h"
#include "fireside/tx_control.h"
namespace {
// A playback device that keeps the peak of ch0 of what was last written: the simulated PA turns it into forward power (k W per drive^2).
struct SimPcm : IPcmOut {
    std::atomic<double>* level; explicit SimPcm(std::atomic<double>* l) : level(l) {}
    int channels() const override { return 8; } double rate() const override { return 48000; } std::string describe() const override { return "sim"; }
    size_t write(const int32_t* x, size_t n) override {
        double pk = 0; for (size_t f = 0; f < n; ++f) { pk = std::max(pk, std::fabs(double(x[f * 8]) / 2147483647.0)); for (int c = 2; c < 8; ++c) if (x[f * 8 + size_t(c)] != 0) throw std::logic_error("a channel other than ch0/1 carried samples"); }
        level->store(pk); std::this_thread::sleep_for(std::chrono::microseconds(int64_t(1e6 * double(n) / rate()))); return n;   // real time, like a sound card
    }
};
struct TxRig : Rig {
    txs::TxCal cal; std::unique_ptr<txc::TxControl> tx;
    TxRig() {
        cal = txs::from_image(backup_image()); sim.pa_from_level = true; sim.meter = cal.meter; sim.rev_frac = 0.02;
        tx = std::make_unique<txc::TxControl>(fx, [this](double) { return std::unique_ptr<IPcmOut>(new SimPcm(&sim.level)); }, atu::real_clock());   // real time: the sample stream runs on its own thread
        tx->set_k(5, 11150);                                       // this radio's measured 20 m drive (there is no built-in default)
    }
};
}  // namespace
TEST(tx_control_arms_only_on_an_eeprom_identical_to_the_backup) {
    const auto img = backup_image(); if (img.empty()) return;
    TxRig r; auto other = img; other[0x200] ^= 1;
    CHECK_THROWS(r.tx->arm(other, img), std::runtime_error); CHECK(!r.tx->armed()); CHECK(write_ops(r.bus).empty());
    CHECK_THROWS(r.tx->arm(img, {}), std::runtime_error);
    r.tx->arm(img, img); CHECK(r.tx->armed()); CHECK_EQ(write_ops(r.bus).size(), size_t(8));       // the 8 bias pots and nothing else
    auto rr = r.tx->ramp(14.336, 2.0, {}); (void)rr;
    r.tx->retire(); CHECK(!r.tx->armed()); CHECK_THROWS(r.tx->arm(img, img), std::runtime_error);
    const size_t n = write_ops(r.bus).size(); auto t = r.tx->tune(14.336, 3, 1.5, {}); CHECK(!t.outcome); CHECK(!t.why_not.empty()); CHECK_EQ(write_ops(r.bus).size(), n);
}
TEST(tx_control_ramp_reaches_the_target_and_measures_the_drive) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image());
    std::vector<double> fwd; auto res = r.tx->ramp(14.336, 2.0, {}, [&](const txc::Progress& p) { fwd.push_back(p.rd.fwd_w); });
    CHECK(res.reached); CHECK(res.max_fwd_w < 3.0 && res.max_fwd_w >= 1.6); CHECK(!r.sim.keyed); CHECK(!r.tx->keyed());
    CHECK_NEAR(res.k, 11150, 1500); CHECK_NEAR(r.tx->k_for(5), res.k, 1e-9);
    CHECK(!fwd.empty());
    TxRig d; d.sim.dead_pa = true; d.tx->arm(backup_image(), backup_image());                           // nothing comes out: stops at the 1 W drive point
    auto dr = d.tx->ramp(14.336, 2.0, {}); CHECK(!dr.reached); CHECK(dr.stop.find("5 mW") != std::string::npos); CHECK(!d.sim.keyed);
    CHECK(!r.tx->ramp(9.0, 2.0, {}).reached); CHECK(!r.tx->ramp(14.336, 16.0, {}).reached);            // outside a band / target above 5 W: refused
    {   // the sample stream dies while keyed (the playback device fails after ~1 s): unkey at once
        struct DyingPcm : SimPcm { int left = 100; using SimPcm::SimPcm; size_t write(const int32_t* x, size_t n) override { if (--left < 0) throw IoError("device gone"); return SimPcm::write(x, n); } };
        TxRig s2; s2.tx = std::make_unique<txc::TxControl>(s2.fx, [&s2](double) { return std::unique_ptr<IPcmOut>(new DyingPcm(&s2.sim.level)); }, atu::real_clock());
        s2.tx->set_k(5, 11150); s2.tx->arm(backup_image(), backup_image());
        auto sr = s2.tx->ramp(14.336, 2.0, {}); CHECK(!sr.reached); CHECK(sr.stop.find("stream failed") != std::string::npos); CHECK(!s2.sim.keyed);
    }
}
TEST(tx_control_tune_on_an_unmeasured_band_borrows_the_next_bands_measured_drive) {
    if (backup_image().empty()) return;
    {   TxRig r; r.tx->arm(backup_image(), backup_image());                                            // nothing measured: the EEPROM table (errs low)
        int from = 7; CHECK_EQ(r.tx->k_estimate(8, &from), 0.0); CHECK_EQ(from, -1);
        auto t = r.tx->tune(24.915, 3.0, 1.5, {}); CHECK(t.outcome && t.estimated_drive); CHECK(t.max_fwd_w < 2.0); CHECK(!r.sim.keyed); }
    TxRig r; r.tx->set_k(7, 11150); r.tx->set_k(9, 9000); r.tx->arm(backup_image(), backup_image());   // 15 m and 10 m measured, 12 m not
    int from = -1; CHECK_EQ(r.tx->k_estimate(8, &from), 11150.0); CHECK_EQ(from, 7);               // the larger k: the lower drive
    CHECK_EQ(r.tx->k_estimate(7, &from), 11150.0); CHECK_EQ(from, 7);                              // a measured band keeps its own
    CHECK_EQ(r.tx->k_estimate(3, &from), 0.0); CHECK_EQ(from, -1);                                 // no measured neighbour
    std::vector<std::string> said; auto t = r.tx->tune(24.915, 3.0, 1.5, {}, {}, [&](const std::string& m) { said.push_back(m); });
    CHECK(t.outcome && t.estimated_drive); CHECK(t.max_fwd_w > 2.0 && t.max_fwd_w < 5.0); CHECK(!r.sim.keyed);
    bool named = false; for (auto& m : said) named = named || m.find("using 15m's measured level") != std::string::npos; CHECK(named);
    CHECK_EQ(r.tx->k_for(8), 0.0);                                                                  // nothing stored for 12 m: Measure drive does that
    CHECK(!r.tx->tune(24.915, 16, 1.5, {}).outcome);                                               // the 15 W tune limit still holds
}
TEST(tx_control_tune_passes_saves_the_band_and_aborts_on_request) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); r.sim.tune_time_polls = 30;
    auto t = r.tx->tune(14.336, 3.0, 1.5, {});
    CHECK(t.outcome && t.outcome->result == atu::Atu::TuneResult::Pass); CHECK(t.saved); CHECK_EQ(t.saved->ind, 30); CHECK(!r.sim.keyed);
    CHECK(r.tx->memory().recall(5)); CHECK(t.max_fwd_w > 2.0 && t.max_fwd_w < 5.0);
    r.sim.ind = 0; r.sim.cap = 0; CHECK(r.tx->restore_relays(5)); CHECK_EQ(r.sim.ind, 30); CHECK_EQ(r.sim.cap, 7);   // no RF: relays back from memory
    CHECK(!r.tx->tune(14.336, 16, 1.5, {}).outcome);                                                   // > 15 W: refused
    auto t40 = r.tx->tune(7.1, 3.0, 1.5, {});                                                          // 40 m, never measured: the table's drive (errs low)
    CHECK(t40.outcome && t40.estimated_drive); CHECK(t40.max_fwd_w < 3.0); CHECK(!r.sim.keyed);
    TxRig f; f.tx->arm(backup_image(), backup_image()); r.bus.stuck = false;                          // the unkey that never gets through is reported, not hidden
    { GateConfig gc; TxGate g(f.fx, gc, atu::real_clock()); g.arm(); CHECK(g.key("t", 1.0)); f.bus.stuck = true; f.bus.mem[kPalCmd] = be({kPalBusy});
      g.unkey("test"); CHECK(g.unkey_failed()); CHECK(g.last_unkey_reason().find("may still be keyed") != std::string::npos); f.bus.stuck = false; f.bus.mem[kPalCmd] = be({0}); }
    TxRig a; a.tx->arm(backup_image(), backup_image()); a.sim.silent = true; std::atomic<int> polls{0};   // abort from the GUI thread while keyed
    auto ab = a.tx->tune(14.336, 3.0, 1.5, {}, [&](const txc::Progress&) { if (++polls == 5) a.tx->abort(); });
    CHECK(ab.outcome && ab.outcome->result != atu::Atu::TuneResult::Pass); CHECK(ab.stop.find("abort") != std::string::npos); CHECK(!a.sim.keyed);
}
TEST(drive_calibration_has_no_default_and_round_trips_as_text) {
    FakeDiceBus bus; flex_defaults(bus); PalMailbox mb(bus, ByteOrder::Big, 1.0, 0); Flex fx(mb); txc::TxControl t(fx, [](double) { return std::unique_ptr<IPcmOut>(); });
    for (int b = 0; b < txm::kBands; ++b) CHECK_EQ(t.k_for(b), 0.0);                                        // nothing assumed for any band
    auto m = txc::parse_drive_text("band=20m k=11150\nband=40m k=9800\nband=99m k=5\njunk\nband=17m k=12\n");
    CHECK_EQ(m.size(), size_t(2)); CHECK_EQ(m[5], 11150.0); CHECK_EQ(m[3], 9800.0);
    CHECK(txc::parse_drive_text(txc::drive_text(m)) == m);
    t.set_all_k(m); CHECK_EQ(t.k_for(5), 11150.0);
}
TEST(session_owns_the_transmit_controller_and_retires_it_on_shutdown) {
    FakeDiceBus bus; flex_defaults(bus);
    RadioSession s([&](const std::string&) { return std::unique_ptr<BusHandle>(new FakeHandle(bus)); }, [](double) {});
    SessionOptions o; o.path = "/dev/fw1"; o.allow_write = true; s.connect(o);
    auto pcm = [](double) -> std::unique_ptr<IPcmOut> { return nullptr; };
    CHECK(!s.transmit(pcm)); s.start(14.336, "ANT1"); auto tx = s.transmit(pcm); CHECK(tx != nullptr); CHECK(s.transmit(pcm) == tx);
    s.shutdown(); CHECK(tx->retired()); CHECK(!s.transmit(pcm)); s.disconnect();
}
TEST(tx_image_correction_suppresses_the_unwanted_sideband_of_a_modelled_exciter) {
    // An exciter whose imbalance is exactly what the EEPROM's 20 m correction undoes (the inverse of DttSP's TX correctIQ): I_hw = I / g, Q_hw = Q - ph * I_hw.
    const double gu = -33.0, pu = 0.12, g = 1 + 0.001 * gu, ph = 0.001 * pu;
    auto image_db = [&](bool corrected) {
        txo::ToneSource s(48000, 8); s.set_cap(0.04); s.set_target(0.01); s.set_tone_hz(1500); s.set_keyed(true); if (corrected) s.set_correction(gu, pu);
        std::vector<int32_t> b(4800 * 8); s.fill(b.data(), 4800); s.fill(b.data(), 4800);     // second block: past the 5 ms ramp
        std::complex<double> up = 0, dn = 0;
        for (size_t n = 0; n < 4800; ++n) {
            const double i = b[n * 8] / 2147483647.0, q = b[n * 8 + 1] / 2147483647.0, ih = i / g, qh = q - ph * ih;
            const std::complex<double> x(ih, qh), e = std::polar(1.0, 2 * M_PI * 1500.0 * double(n) / 48000.0);
            up += x * std::conj(e); dn += x * e;                                                    // +1500 Hz (wanted) and -1500 Hz (image)
        }
        return 20 * std::log10(std::abs(dn) / std::abs(up));
    };
    const double raw = image_db(false), fixed = image_db(true);
    CHECK(raw > -45 && raw < -30);                                  // this radio's 20 m imbalance uncorrected: image ~36 dB down
    CHECK(fixed < -80);                                             // corrected: gone
    const auto img = backup_image(); if (img.empty()) return;
    const auto c = txs::from_image(img); CHECK_NEAR(c.image_gain[5], -33.0, 1e-6); CHECK_NEAR(c.image_phase[5], 0.125, 1e-6); CHECK_NEAR(c.image_gain[10], 35.0, 1e-6);
}

// ---------------------------------------------------------------- FT8 / FT4 transmit: what goes out decodes, on time, at the set power
#include "fireside/decode.h"
#include "fireside/dsp.h"
namespace {
double wall_utc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
struct CapPcm : SimPcm {                                    // also keeps every frame (ch0/ch1) with the (shifted) UTC of each write
    double r; std::vector<std::complex<double>>* iq; std::vector<std::pair<double, size_t>>* marks; std::function<double()> utc;
    CapPcm(std::atomic<double>* l, double rate, std::vector<std::complex<double>>* x, std::vector<std::pair<double, size_t>>* m, std::function<double()> u) : SimPcm(l), r(rate), iq(x), marks(m), utc(std::move(u)) {}
    double rate() const override { return r; }
    size_t write(const int32_t* x, size_t n) override {
        marks->push_back({utc(), iq->size()});
        for (size_t f = 0; f < n; ++f) iq->push_back({x[f * 8] / 2147483647.0, x[f * 8 + 1] / 2147483647.0});
        return SimPcm::write(x, n);
    }
};
}  // namespace
TEST(ft4_transmission_decodes_on_time_at_the_set_power) {
    if (backup_image().empty()) return;
    for (double rate : {48000.0, 96000.0}) {
        TxRig r; std::vector<std::complex<double>> iq; std::vector<std::pair<double, size_t>> marks;
        const double now = wall_utc(), slot = 7.5, next = std::ceil(now / slot) * slot; const double shift = (next - 3.5) - now;   // a slot starts ~3.5 s from "now" (rendering + 1.5 s lead)
        auto utc = [shift] { return wall_utc() + shift; };
        r.tx = std::make_unique<txc::TxControl>(r.fx, [&](double) { return std::unique_ptr<IPcmOut>(new CapPcm(&r.sim.level, rate, &iq, &marks, utc)); }, atu::real_clock());
        r.tx->set_utc_source(utc); r.tx->set_k(5, 8000);                                     // drive level 40 % wrong: the closed loop must fix the power
        r.tx->arm(backup_image(), backup_image());
        const tx::Parity par = (long long)(std::llround(next / slot)) % 2 ? tx::Parity::Odd : tx::Parity::Even;
        txc::Options o; o.rate = rate;
        auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, true, par, 5.0, o);
        CHECK(res.sent); CHECK(!r.sim.keyed); CHECK_NEAR(res.slot_utc, next, 1e-3);
        CHECK_NEAR(res.mean_fwd_w, 5.0, 0.6);                                                // the loop brought it to the set power
        CHECK(r.tx->k_for(5) > 8000 && r.tx->k_for(5) < 11150 * 1.05);                        // and the band's drive level learned towards the truth (11 150)
        // decode what went out: undo the mirror, slot start from the write times, USB audio = Re{z}, 12 kHz
        size_t s0 = 0; for (auto& m : marks) if (m.first <= res.slot_utc) s0 = m.second + size_t(std::lround((res.slot_utc - m.first) * rate));
        std::vector<dsp::cf> z; for (size_t i = s0; i < iq.size() && i < s0 + size_t(7.5 * rate); ++i) z.push_back(dsp::cf(float(iq[i].real()), float(-iq[i].imag())));
        dsp::RationalResampler rs(rate, 12000); std::vector<dsp::cf> y; z.resize(z.size() + 2000); rs.process(z.data(), z.size(), y);
        std::vector<float> audio; for (auto& v : y) audio.push_back(v.real() * 50);
        auto msgs = decode::Ft8Decoder::decode_slot(audio.data(), std::min(audio.size(), size_t(90000)), "", true);
        bool found = false; for (auto& m : msgs) if (m.text == "CQ M0OML IO91") { found = true; CHECK_NEAR(m.freq, 1500, 10); CHECK(std::fabs(m.dt) < 0.1); }   // dt: WSJT-X convention (0 = the nominal start)
        CHECK(found);
    }
}
TEST(ft_send_refuses_what_it_cannot_send_and_stops_on_abort) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); txc::Options o;
    CHECK(!r.tx->send_ft(14.080, "THIS IS NOT FT8 !!!", 1500, false, tx::Parity::Even, 5, o).sent);
    CHECK(!r.tx->send_ft(14.349, "CQ M0OML IO91", 2000, false, tx::Parity::Even, 5, o).sent);         // would end above the band edge
    CHECK(!r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, false, tx::Parity::Even, 101, o).sent);       // > 100 W (the radio's rating)
    CHECK(write_ops(r.bus).size() == 8);                                                               // nothing but the arm's bias pots went out
    const double now = wall_utc(), next = std::ceil(now / 15.0) * 15.0, shift = (next - 3.0) - now;
    r.tx->set_utc_source([shift] { return wall_utc() + shift; });
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(4500)); r.tx->abort(); });
    const tx::Parity par = (long long)(std::llround(next / 15.0)) % 2 ? tx::Parity::Odd : tx::Parity::Even;
    auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, false, par, 5, o); t.join();
    CHECK(!res.sent); CHECK(res.stop.find("abort") != std::string::npos); CHECK(!r.sim.keyed);
}

// ---------------------------------------------------------------- FT8 / FT4 transmit, more: on fast simulated time (x5) so whole FT8 overs stay quick
namespace {
constexpr double kSpeed = 5.0;
// One time base for everything: the TxControl clock (gate timers, sleeps), its UTC source and the playback pacing all run kSpeed x real time.
struct FastTime {
    double w0 = wall_utc(), base, speed;                // simulated UTC at w0; simulated seconds per real second
    explicit FastTime(double utc_start, double sp = kSpeed) : base(utc_start), speed(sp) {}
    double now() const { return base + (wall_utc() - w0) * speed; }
    atu::Clock clock() const { return {[this] { return now(); }, [this](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0, s) / speed)); }}; }
};
// Playback: ch0/ch1 kept with the simulated UTC of each write, the PA level for the simulated radio, nothing allowed on ch2..7; can fail after `fail_after` writes.
struct FastPcm : IPcmOut {
    double r; const FastTime& t; std::atomic<double>* level; std::vector<std::complex<double>>& iq; std::vector<std::pair<double, size_t>>& marks; std::atomic<double>& peak; long fail_after; long writes = 0;
    FastPcm(double rate, const FastTime& ft, std::atomic<double>* l, std::vector<std::complex<double>>& x, std::vector<std::pair<double, size_t>>& m, std::atomic<double>& pk, long fail = -1)
        : r(rate), t(ft), level(l), iq(x), marks(m), peak(pk), fail_after(fail) {}
    int channels() const override { return 8; } double rate() const override { return r; } std::string describe() const override { return "fast sim"; }
    size_t write(const int32_t* x, size_t n) override {
        if (fail_after >= 0 && ++writes > fail_after) throw std::runtime_error("device gone");
        marks.push_back({t.now(), iq.size()}); double pk = 0;
        for (size_t f = 0; f < n; ++f) {
            for (int c = 2; c < 8; ++c) if (x[f * 8 + size_t(c)] != 0) throw std::logic_error("a channel other than ch0/1 carried samples");
            const double i = x[f * 8] / 2147483647.0, q = x[f * 8 + 1] / 2147483647.0; iq.push_back({i, q}); pk = std::max({pk, std::fabs(i), std::fabs(q)});
        }
        level->store(pk); peak.store(std::max(peak.load(), pk));
        std::this_thread::sleep_for(std::chrono::duration<double>(double(n) / r / t.speed)); return n;
    }
};
struct FtRig : Rig {
    std::unique_ptr<FastTime> ft; std::unique_ptr<txc::TxControl> tx; std::vector<std::complex<double>> iq; std::vector<std::pair<double, size_t>> marks; std::atomic<double> peak{0};
    double rate; long fail_after = -1;
    // `utc_start`: the simulated UTC now (pick it relative to a slot boundary)
    FtRig(double utc_start, double r = 48000, double speed = kSpeed) : rate(r) {
        ft = std::make_unique<FastTime>(utc_start, speed);
        sim.pa_from_level = true; sim.meter = txs::from_image(backup_image()).meter; sim.rev_frac = 0.02;
        tx = std::make_unique<txc::TxControl>(fx, [this](double rr) { return std::unique_ptr<IPcmOut>(new FastPcm(rr, *ft, &sim.level, iq, marks, peak, fail_after)); }, ft->clock());
        tx->set_utc_source([this] { return ft->now(); }); tx->set_k(5, 11150); tx->arm(backup_image(), backup_image());
    }
    size_t mox_writes() { return sent(*this, "RDAL_OP_SET_MOX").size(); }
    // What went out from the slot start, as USB audio at 12 kHz (mirror undone when `unmirror`).
    std::vector<float> audio(double slot_utc, double seconds, bool unmirror = true) {
        size_t s0 = 0; for (auto& m : marks) if (m.first <= slot_utc) s0 = m.second + size_t(std::max(0L, std::lround((slot_utc - m.first) * rate)));
        std::vector<dsp::cf> z;
        if (!marks.empty() && marks.front().first > slot_utc) z.resize(size_t(std::lround((marks.front().first - slot_utc) * rate)) - marks.front().second);   // the stream began after the slot did
        for (size_t i = s0; i < iq.size() && i < s0 + size_t(seconds * rate); ++i) z.push_back(dsp::cf(float(iq[i].real()), float(unmirror ? -iq[i].imag() : iq[i].imag())));
        z.resize(size_t(seconds * rate) + 2000); dsp::RationalResampler rs(rate, 12000); std::vector<dsp::cf> y; rs.process(z.data(), z.size(), y);
        std::vector<float> a; for (auto& v : y) a.push_back(v.real() * 50); return a;
    }
};
// A UTC `before` seconds ahead of a boundary of a slot of `parity`
double utc_before_slot(double slot_len, tx::Parity parity, double before, double* slot_start = nullptr) {
    double s = std::ceil(wall_utc() / slot_len) * slot_len + 2 * slot_len;
    if ((long long)std::llround(s / slot_len) % 2 != (parity == tx::Parity::Odd ? 1 : 0)) s += slot_len;
    if (slot_start) *slot_start = s; return s - before;
}
bool decodes(const std::vector<float>& a, bool ft4, const std::string& text, double hz, double* dt = nullptr) {
    for (auto& m : decode::Ft8Decoder::decode_slot(a.data(), std::min(a.size(), size_t(ft4 ? 90000 : 180000)), "", ft4))
        if (m.text == text && std::fabs(m.freq - hz) < 10) { if (dt) *dt = m.dt; return true; }
    return false;
}
// Energy of z in the +f..+f+bw band against -f..-f-bw (which sideband the signal is on), 6.25 Hz steps over the first second of signal
double sideband_db(const std::vector<std::complex<float>>& z, double rate, double f, double bw, size_t from) {
    double up = 0, dn = 0;
    for (double hz = f - 10; hz <= f + bw + 10; hz += 6.25) {
        std::complex<double> u, d;
        for (size_t n = from; n < from + size_t(rate) && n < z.size(); ++n) { const auto e = std::polar(1.0, 2 * M_PI * hz * double(n) / rate); const std::complex<double> x(z[n]); u += x * std::conj(e); d += x * e; }
        up += std::norm(u); dn += std::norm(d);
    }
    return 10 * std::log10(up / std::max(dn, 1e-30));
}
}  // namespace

TEST(ft_rendered_messages_decode_for_every_qso_step_both_modes_both_rates) {   // the renderer alone, no radio: what prepare_ft builds is the message
    const std::vector<std::string> qso = {"CQ M0OML IO91", "DK0ULR M0OML IO91", "M0OML DK0ULR -06", "DK0ULR M0OML R-08", "M0OML DK0ULR RR73", "DK0ULR M0OML 73", "CQ DX M0OML IO91"};
    int n = 0;
    for (bool ft4 : {false, true}) for (double rate : {48000.0, 96000.0}) for (size_t i = 0; i < qso.size(); ++i) {
        const double hz = std::vector<double>{300, 1500, 2700}[(i + (ft4 ? 1 : 0)) % 3];
        if (rate == 96000 && i % 3) continue;                                                 // 96 kHz: a sample of them (the path is the same)
        const auto f = txc::TxControl::prepare_ft(qso[i], hz, ft4, rate);
        CHECK_EQ(f.text, qso[i]); CHECK_NEAR(f.rate, rate, 0); CHECK_EQ(f.ft4, ft4);
        const double end = 0.5 + (ft4 ? 5.04 : 12.64) + 0.05;                               // silent after the signal: nothing past the slot's signal time
        for (size_t k = size_t(end * rate); k < f.buf.size(); ++k) if (std::abs(f.buf[k]) > 1e-3) { CHECK(false); break; }
        double pk = 0; for (auto& v : f.buf) pk = std::max(pk, double(std::abs(v))); CHECK(pk <= 1.05 && pk > 0.95);   // envelope normalised to 1 (FT4: up to +2.5 % resampler overshoot; the hard clamp still applies)
        const size_t s0 = size_t((0.5 + 1.0) * rate);                                         // a second into the signal
        CHECK_NEAR(std::abs(f.buf[s0]), 1.0, 0.02);                                           // constant envelope (closed-loop power relies on it)
        CHECK(sideband_db(f.buf, rate, hz, ft4 ? 85 : 50, s0) > 40);                          // above the carrier: USB, nothing on the image side
        std::vector<dsp::cf> z(f.buf.begin(), f.buf.end()); z.resize(z.size() + 2000);
        dsp::RationalResampler rs(rate, 12000); std::vector<dsp::cf> y; rs.process(z.data(), z.size(), y);
        std::vector<float> a; for (auto& v : y) a.push_back(v.real() * 0.1f);   // within full scale (the decoder takes audio in +-1)
        double dt = 9; const bool ok = decodes(a, ft4, qso[i], hz, &dt); if (!ok) std::printf("  not decoded: %s %s at %.0f Hz, %.0f kHz\n", ft4 ? "FT4" : "FT8", qso[i].c_str(), hz, rate / 1000); CHECK(ok); CHECK(std::fabs(dt) < 0.1); ++n;
    }
    CHECK(n >= 20);
    CHECK_THROWS(txc::TxControl::prepare_ft("CQ M0OML IO91", 150, false, 48000), std::exception);    // audio outside 200..3000
    CHECK_THROWS(txc::TxControl::prepare_ft("CQ M0OML IO91", 3100, false, 48000), std::exception);
    CHECK_THROWS(txc::TxControl::prepare_ft("", 1500, false, 48000), std::exception);         // nothing to send (an empty box in the GUI)
    CHECK_THROWS(txc::TxControl::prepare_ft("   ", 1500, true, 48000), std::exception);
}

TEST(ft8_over_decodes_on_time_mirrored_on_the_upper_sideband_within_the_gate) {
    if (backup_image().empty()) return;
    const auto f = txc::TxControl::prepare_ft("DK0ULR M0OML IO91", 2500, false, 96000);    // rendered first (as the GUI does): on 5 x time it would eat the lead
    double slot = 0; FtRig r(utc_before_slot(15, tx::Parity::Odd, 8.0, &slot), 96000);
    txc::Options o; o.rate = 96000;
    auto res = r.tx->send_prepared(14.074, f, tx::Parity::Odd, 10.0, o);
    CHECK(res.sent); CHECK(!res.unkey_failed); CHECK(!r.sim.keyed); CHECK_NEAR(res.slot_utc, slot, 1e-6); CHECK_EQ(res.late_s, 0.0);
    CHECK_EQ(r.mox_writes(), size_t(2));                                                      // keyed once, unkeyed once
    CHECK(res.max_fwd_w <= 15.0); CHECK_NEAR(res.mean_fwd_w, 10.0, 1.0);                       // inside the gate's 1.5 x; at the set power
    CHECK(r.peak.load() <= txs::drive_for_watts(r.tx->cal(), 5, 100.0) + 1e-9);               // never above the hard clamp
    // the signal starts after the slot start and ends inside the slot
    double first = 0, last = 0;
    for (size_t k = 0; k < r.marks.size(); ++k) { const size_t e = k + 1 < r.marks.size() ? r.marks[k + 1].second : r.iq.size(); bool any = false;
        for (size_t i = r.marks[k].second; i < e; ++i) if (std::abs(r.iq[i]) > 0) { any = true; break; }
        if (any) { if (!first) first = r.marks[k].first; last = r.marks[k].first; } }
    CHECK(first > slot && first < slot + 0.6); CHECK(last < slot + 13.4);                    // 0.5 s + 12.64 s of signal (+ block granularity)
    double dt = 9; CHECK(decodes(r.audio(slot, 15), false, "DK0ULR M0OML IO91", 2500, &dt)); CHECK(std::fabs(dt) < 0.1);
    // mirrored as the radio needs: on the wire the signal sits BELOW the carrier, un-mirrored above it
    size_t s0 = 0; for (auto& m : r.marks) if (m.first <= slot + 2.0) s0 = m.second;
    std::vector<std::complex<float>> wire, plain;
    for (size_t i = s0; i < s0 + 96000 && i < r.iq.size(); ++i) { wire.push_back(std::complex<float>(r.iq[i])); plain.push_back(std::conj(std::complex<float>(r.iq[i]))); }
    CHECK(sideband_db(wire, 96000, 2500, 50, 0) < -25); CHECK(sideband_db(plain, 96000, 2500, 50, 0) > 25);
}

TEST(ft_without_the_mirror_puts_the_signal_above_the_carrier_on_the_wire) {
    if (backup_image().empty()) return;
    double slot = 0; FtRig r(utc_before_slot(7.5, tx::Parity::Even, 8.0, &slot));
    txc::Options o; o.mirror_q = false;
    auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1000, true, tx::Parity::Even, 5.0, o);
    CHECK(res.sent); CHECK(decodes(r.audio(slot, 7.5, false), true, "CQ M0OML IO91", 1000));
}

TEST(ft_closed_loop_raises_a_low_drive_and_the_gate_stops_a_dangerous_one) {
    if (backup_image().empty()) return;
    {   // the band's level reads 80 % too high: the drive starts at 2.8 W, the loop brings it up to 5 W, and learns
        double slot = 0; FtRig r(utc_before_slot(7.5, tx::Parity::Odd, 8.0, &slot)); r.tx->set_k(5, 20000); txc::Options o;
        auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, true, tx::Parity::Odd, 5.0, o);
        CHECK(res.sent); CHECK_NEAR(res.mean_fwd_w, 5.0, 0.6); CHECK(r.tx->k_for(5) < 20000 && r.tx->k_for(5) > 11150 * 0.95);
    }
    {   // the level reads 1.7 x too low: full drive would be ~8.6 W for a 5 W over (limit 7.5 W); the soft start (3 dB low) begins at ~4.3 W and the loop trims to 5 W
        double slot = 0; FtRig r(utc_before_slot(7.5, tx::Parity::Odd, 8.0, &slot)); r.tx->set_k(5, 6500); txc::Options o;
        auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, true, tx::Parity::Odd, 5.0, o);
        CHECK(res.sent); CHECK_NEAR(res.mean_fwd_w, 5.0, 0.6); CHECK(res.max_fwd_w < 7.5);
    }
    {   // the level reads 5.6 x too low: even 3 dB low the over starts at ~14 W (limit 7.5 W): sustained, so the gate unkeys after its 0.15 s grace, nothing learned
        double slot = 0; FtRig r(utc_before_slot(7.5, tx::Parity::Odd, 8.0, &slot)); r.tx->set_k(5, 2000); txc::Options o;
        auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, true, tx::Parity::Odd, 5.0, o);
        CHECK(!res.sent); CHECK(!r.sim.keyed); CHECK_EQ(r.mox_writes(), size_t(2)); CHECK(res.stop.find("forward") != std::string::npos);
        CHECK_NEAR(r.tx->k_for(5), 2000, 1e-9);
        // and it was short: keyed for well under a second of the over
        double keyed_s = 0; for (auto& m : r.marks) if (m.first > res.slot_utc && m.second < r.iq.size() && std::abs(r.iq[m.second]) > 0) keyed_s = m.first - res.slot_utc;
        if (!(keyed_s < 1.5)) std::printf("  keyed %.2f s, stop %s, max %.1f W\n", keyed_s, res.stop.c_str(), res.max_fwd_w); CHECK(keyed_s < 1.5);
    }
}

TEST(ft_high_swr_during_the_over_unkeys_and_teaches_nothing) {
    if (backup_image().empty()) return;
    double slot = 0; FtRig r(utc_before_slot(7.5, tx::Parity::Even, 8.0, &slot)); txc::Options o;
    auto res = r.tx->send_ft(14.080, "CQ M0OML IO91", 1500, true, tx::Parity::Even, 5.0, o,
                             [&](const txc::Progress& p) { if (p.t > 2.5) r.sim.rev_frac = 0.6; });    // the antenna goes bad 2.5 s in (SWR ~ 8.8)
    CHECK(!res.sent); CHECK(!r.sim.keyed); CHECK_EQ(r.mox_writes(), size_t(2));
    CHECK(res.stop.find("SWR") != std::string::npos || res.stop.find("reflected") != std::string::npos);
    CHECK_NEAR(r.tx->k_for(5), 11150, 1e-9);
}

TEST(ft_joins_the_current_slot_up_to_2_s_late_still_on_time) {
    if (backup_image().empty()) return;
    double slot = 0; utc_before_slot(15, tx::Parity::Even, 0, &slot);
    const auto f = txc::TxControl::prepare_ft("CQ M0OML IO91", 1200, false, 48000);
    FtRig r(slot + 1.2); txc::Options o;                                                      // pressed 1.2 s into an even slot: 0.7 s of the signal already gone
    auto res = r.tx->send_prepared(14.074, f, tx::Parity::Even, 5.0, o, {}, {}, 0.0);
    CHECK(res.sent); CHECK_NEAR(res.slot_utc, slot, 1e-6); CHECK(res.late_s > 0.5 && res.late_s < 1.5);
    double dt = 9; CHECK(decodes(r.audio(slot, 15), false, "CQ M0OML IO91", 1200, &dt)); CHECK(std::fabs(dt) < 0.15);   // what is sent is where it belongs in the slot
}

TEST(ft_send_refusals_and_failures_leave_the_radio_unkeyed) {
    if (backup_image().empty()) return;
    double slot = 0; FtRig r(utc_before_slot(7.5, tx::Parity::Even, 8.0, &slot)); txc::Options o;
    const auto f48 = txc::TxControl::prepare_ft("CQ M0OML IO91", 1500, true, 48000);
    o.rate = 96000; CHECK(!r.tx->send_prepared(14.080, f48, tx::Parity::Even, 5, o).sent); o.rate = 48000;   // rendered for another rate
    CHECK(!r.tx->send_prepared(7.040, txc::TxControl::prepare_ft("CQ M0OML IO91", 1500, true, 48000), tx::Parity::Even, 0.5, o).sent);   // < 1 W
    CHECK(!r.tx->send_prepared(13.990, f48, tx::Parity::Even, 5, o).sent);                    // dial below 20 m
    CHECK_EQ(r.mox_writes(), size_t(0));
    r.tx->disarm(); CHECK(!r.tx->send_prepared(14.080, f48, tx::Parity::Even, 5, o).sent); CHECK_EQ(r.mox_writes(), size_t(0));
    r.tx->arm(backup_image(), backup_image());
    r.fail_after = 850;                                                                       // the playback device dies mid-over
    auto res = r.tx->send_prepared(14.080, f48, tx::Parity::Even, 5, o);
    CHECK(!res.sent); CHECK(!r.sim.keyed); CHECK(r.mox_writes() <= size_t(2)); CHECK(!r.tx->keyed());
}

// ---------------------------------------------------------------- WSPR over (the same send path as FT8: TxGate, limits, closed loop)
#include "fireside/wspr.h"
#include "fireside/wspr_decode.h"
TEST(wspr_over_starts_1_s_after_the_even_minute_holds_5_w_ends_in_the_slot_and_wsprd_decodes_it) {
    if (backup_image().empty()) return;
    double slot = 0; FtRig r(utc_before_slot(120, tx::Parity::Even, 30.0, &slot), 48000, 20.0); txc::Options o;   // 30 s ahead (rendering the over takes real time); 20 x time: the 110.6 s over takes ~6 s
    auto res = r.tx->send_wspr(14.0956, "M0OML", "IO91", 1500, tx::Parity::Even, 5.0, o);
    CHECK(res.sent); CHECK(!res.unkey_failed); CHECK(!r.sim.keyed); CHECK_NEAR(res.slot_utc, slot, 1e-6); CHECK_EQ(res.late_s, 0.0);
    CHECK_EQ(r.mox_writes(), size_t(2));
    CHECK(res.max_fwd_w <= 7.5); CHECK_NEAR(res.mean_fwd_w, 5.0, 0.6);
    double first = 0, last = 0;
    for (size_t k = 0; k < r.marks.size(); ++k) { const size_t e = k + 1 < r.marks.size() ? r.marks[k + 1].second : r.iq.size(); bool any = false;
        for (size_t i = r.marks[k].second; i < e; ++i) if (std::abs(r.iq[i]) > 0) { any = true; break; }
        if (any) { if (!first) first = r.marks[k].first; last = r.marks[k].first; } }
    CHECK(first > slot + 0.6 && first < slot + 1.3); CHECK(last > slot + 110 && last < slot + 112.5);   // 1 s + 110.6 s of signal (block granularity at 20 x)
    if (decode::find_wsprd().empty()) { std::printf("  (wsprd not installed: decode check skipped)\n"); return; }
    const auto dir = std::filesystem::temp_directory_path() / ("fireside_wspr_tx_" + std::to_string(::getpid())); std::filesystem::create_directories(dir);
    auto a = r.audio(slot, 114); float pk = 0; for (float v : a) pk = std::max(pk, std::fabs(v)); for (auto& v : a) v *= 0.5f / std::max(pk, 1e-9f);
    const auto wav = (dir / "000101_0000.wav").string(); { const auto b = wspr::wav16(a); std::ofstream f(wav, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); }
    std::string out, err; CHECK(decode::wsprd_runner()(wav, 14.0956, dir.string(), out, err));
    bool found = false;
    std::istringstream is(out); std::string ln;
    while (std::getline(is, ln)) if (auto sp = wspr::parse_wsprd_line(ln)) if (sp->call == "M0OML" && sp->grid == "IO91" && sp->dbm == 37 && std::fabs(sp->mhz - (14.0956 + 1502.2e-6)) < 3e-6) found = true;
    CHECK(found); if (!found) std::printf("  wsprd said: %s %s\n", out.c_str(), err.c_str());
    std::filesystem::remove_all(dir);
}
TEST(wspr_send_refusals_and_a_swr_trip_leave_the_radio_unkeyed) {
    if (backup_image().empty()) return;
    double slot = 0; FtRig r(utc_before_slot(120, tx::Parity::Even, 3.0, &slot), 48000, 20.0); txc::Options o;
    CHECK(!r.tx->send_wspr(14.0956, "M0OML", "IO91", 1500, tx::Parity::Even, 25.0, o).sent);     // above 20 W
    CHECK(!r.tx->send_wspr(14.0956, "M0OML", "IO91", 1500, tx::Parity::Even, 0.5, o).sent);      // below 1 W
    CHECK(!r.tx->send_wspr(14.0956, "M0OML/P", "IO91", 1500, tx::Parity::Even, 5.0, o).sent);    // compound call: not a type-1 message
    CHECK(!r.tx->send_wspr(14.0956, "M0OML", "IO9", 1500, tx::Parity::Even, 5.0, o).sent);       // bad grid
    CHECK(!r.tx->send_wspr(14.0956, "M0OML", "IO91", 1700, tx::Parity::Even, 5.0, o).sent);      // outside the WSPR sub-band
    CHECK(!r.tx->send_wspr(13.9956, "M0OML", "IO91", 1500, tx::Parity::Even, 5.0, o).sent);      // below 20 m
    CHECK_EQ(r.mox_writes(), size_t(0));
    r.sim.rev_frac = 0.6;                                                                     // a bad antenna: SWR far above 3
    auto res = r.tx->send_wspr(14.0956, "M0OML", "IO91", 1500, tx::Parity::Even, 5.0, o);
    CHECK(!res.sent); CHECK(!r.sim.keyed); CHECK(!res.unkey_failed); CHECK(txc::TxControl::is_safety_stop(res.stop));
}
TEST(tx_key_hook_runs_right_before_mox_on_and_right_after_mox_off) {   // the session moves the shared RX1/TX DDS onto the dial for the over (as PowerSDR)
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image());
    std::vector<std::pair<bool, size_t>> ev;                                        // (keyed?, SET_MOX writes so far)
    r.tx->set_key_hook([&](bool k, double) { ev.push_back({k, sent(r, "RDAL_OP_SET_MOX").size()}); });
    txc::Options o; auto res = r.tx->ramp(14.10, 2.0, o);
    CHECK(res.reached); CHECK_EQ(ev.size(), size_t(2));
    CHECK(ev[0].first && ev[0].second == 0);                                        // before the key: no MOX yet
    CHECK(!ev[1].first); CHECK(ev[1].second >= 2); CHECK(!r.sim.keyed);             // after: MOX on and off already sent
}
TEST(tx_control_ramp_to_15_w_and_never_past_its_ceiling) {   // the test tone at up to 15 W (user 2026-11-19); the ramp's own trips scale with it
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); txc::Options o;
    auto res = r.tx->ramp(14.10, 15.0, o, {}, {}, 1.0);
    if (!res.reached) std::printf("  ramp stop: %s, max %.2f W, %.1f s\n", res.stop.c_str(), res.max_fwd_w, res.seconds);
    CHECK(res.reached); CHECK(res.max_fwd_w > 12.0 && res.max_fwd_w < 1.5 * 15.0); CHECK(!r.sim.keyed);
    CHECK(!r.tx->ramp(14.10, 16.0, o).reached);                                       // above 15 W: refused before keying
    CHECK_NEAR(r.tx->drive_cap(5), std::min(std::sqrt(110.0 / r.tx->k_for(5)), txs::kAbsoluteMaxDrive), 1e-12);  // the ceiling: 100 W + 10 % by the measured level, absolute clamp
    CHECK_NEAR(r.tx->test_cap(5), std::sqrt(30.0 / r.tx->k_for(5)), 1e-12);                                      // tunes and test tones: still 30 W
    CHECK(r.tx->drive_cap(5) <= txs::kAbsoluteMaxDrive);
}
TEST(tx_control_ramp_with_a_lagging_meter_settles_on_the_target_without_overshoot) {   // the 15 W test tone peaked at 18-20 W on the air
    if (backup_image().empty()) return;
    for (double target : {2.0, 5.0, 15.0}) {
        TxRig r; r.sim.meter_lag_s = 0.25; r.tx->arm(backup_image(), backup_image()); txc::Options o;
        std::vector<double> held; auto res = r.tx->ramp(14.10, target, o, [&](const txc::Progress& p) { held.push_back(p.rd.fwd_w); }, {}, 3.0);
        double tail = 0; size_t n = 0; for (size_t i = held.size() * 3 / 4; i < held.size(); ++i) { tail += held[i]; ++n; }
        tail = n ? tail / double(n) : 0;
        std::printf("  target %.0f W: reached %d, max %.2f W, last quarter %.2f W (%s)\n", target, int(res.reached), res.max_fwd_w, tail, res.stop.c_str());
        CHECK(res.reached); CHECK(res.max_fwd_w <= 1.2 * target); CHECK_NEAR(tail, target, 0.1 * target);
    }
}

// ---------------------------------------------------------------- voice (SSB) transmit on the simulated radio
namespace {
struct FakeMic : txc::IAudioIn {                                                    // syllable-like bursts of 700 + 1900 Hz, paced in real time
    size_t pos = 0; std::atomic<bool> fail{false}; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    size_t read(float* x, size_t n, double) override {                              // paced by absolute time, like a sound card clock (no drift)
        if (fail) throw IoError("mic unplugged");
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(1e6 * double(pos + n) / 48000)));
        for (size_t i = 0; i < n; ++i, ++pos) { const double t = double(pos) / 48000, env = 0.5 * (1 - std::cos(2 * M_PI * 4 * t));
            x[i] = float(0.25 * env * (std::sin(2 * M_PI * 700 * t) + std::sin(2 * M_PI * 1900 * t))); }
        return n;
    }
    std::string describe() const override { return "fake mic"; }
};
double sideband_ratio_db(const std::vector<std::complex<double>>& iq, double rate, bool unmirror) {   // energy 300..2800 Hz above / below the carrier
    double up = 0, dn = 0; const size_t n = std::min<size_t>(iq.size(), size_t(rate));
    for (double f = 300; f <= 2800; f += 50) { std::complex<double> u, d;
        for (size_t i = iq.size() - n; i < iq.size(); ++i) { std::complex<double> z = iq[i]; if (unmirror) z = std::conj(z); const auto e = std::polar(1.0, 2 * M_PI * f * double(i) / rate); u += z * std::conj(e); d += z * e; }
        up += std::norm(u); dn += std::norm(d); }
    return 10 * std::log10((up + 1e-30) / (dn + 1e-30));
}
}  // namespace
TEST(voice_usb_and_lsb_on_their_sideband_at_the_set_peak_and_ptt_unkeys) {
    if (backup_image().empty()) return;
    for (bool lsb : {false, true}) {
        TxRig r; std::vector<std::complex<double>> iq; std::vector<std::pair<double, size_t>> marks;
        r.tx = std::make_unique<txc::TxControl>(r.fx, [&](double) { return std::unique_ptr<IPcmOut>(new CapPcm(&r.sim.level, 48000, &iq, &marks, [] { return wall_utc(); })); }, atu::real_clock());
        r.tx->set_k(5, 11150); r.tx->arm(backup_image(), backup_image());
        FakeMic mic; const auto t0 = std::chrono::steady_clock::now();
        txc::VoiceOptions v; v.lsb = lsb; v.peak_w = 10;                               // the voice defaults: speech peaks reach the ceiling
        auto tap = std::make_shared<txo::IqTap>(); r.tx->set_iq_tap(tap);
        auto res = r.tx->voice(14.20, mic, [&] { return std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2500); }, v);
        CHECK(res.keyed); CHECK(res.stop.find("PTT released") != std::string::npos); CHECK(!r.sim.keyed); CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), size_t(2));
        CHECK(res.max_fwd_w > 7.0 && res.max_fwd_w <= 1.15 * 10.0);                     // peaks at the set power (the ALC only trims down)
        CHECK(res.iq_peak_db > -3.0 && res.iq_peak_db <= 0.5); CHECK(res.drive_start > 0); CHECK(res.trims >= 0 && res.drive <= res.drive_start);   // the diagnostics in the over's log line
        CHECK(res.mean_fwd_w > 0 && res.mean_fwd_w <= res.talk_fwd_w + 1e-9 && res.talk_fwd_w <= res.max_fwd_w + 1e-9);   // average power: whole over <= while talking <= peak
        const double sb = sideband_ratio_db(iq, 48000, true);                          // undo the mirror: USB above the carrier
        // > 20 dB: the right sideband. (On the simulated, perfect radio the EEPROM's TX image correction for this radio itself makes an image
        // ~36 dB down; on the real radio it cancels the radio's own imbalance. ~27 dB seen here.)
        if (lsb) CHECK(sb < -20); else CHECK(sb > 20);
        CHECK(res.underruns < 48000 / 10);                                              // the live ring kept up (silence only at the start / end)
        {   // the display's copy of the over: the I/Q sent, in drive units (k x |x|^2 = watts), at the stream's rate
            std::vector<std::complex<float>> got; double rate = 0; tap->read(got, &rate);
            float pk = 0; for (auto& z : got) pk = std::max(pk, std::abs(z));
            CHECK(got.size() > 48000 && rate == 48000); CHECK(11150.0 * pk * pk > 5.0 && 11150.0 * pk * pk < 1.2 * 10.0);
        }
    }
}
TEST(iq_tap_drops_rather_than_grows_and_reads_everything) {
    txo::IqTap t(100); std::vector<std::complex<float>> x(150, {0.5f, 0.f}), out; double rate = 0;
    t.write(x.data(), x.size(), 96000); CHECK_EQ(t.dropped(), uint64_t(50));
    CHECK_EQ(t.read(out, &rate), size_t(100)); CHECK(rate == 96000); CHECK_EQ(t.read(out), size_t(0));
}
TEST(voice_refusals_time_out_and_a_failing_mic_leave_the_radio_unkeyed) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); FakeMic mic; txc::VoiceOptions v;
    v.peak_w = 101; CHECK(!r.tx->voice(14.2, mic, [] { return true; }, v).keyed); v.peak_w = 10;
    CHECK(!r.tx->voice(7.0, mic, [] { return true; }, v).keyed);                        // no measured drive level on 40 m in this rig
    CHECK(!r.tx->voice(14.2, mic, [] { return false; }, v).keyed);                      // PTT already released
    CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), size_t(0));
    v.max_key_s = 0.6; auto t = r.tx->voice(14.2, mic, [] { return true; }, v);
    CHECK(t.keyed); CHECK(t.stop.find("time limit") != std::string::npos); CHECK(!r.sim.keyed);
    std::thread k([&] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); mic.fail = true; });
    v.max_key_s = 10; auto f = r.tx->voice(14.2, mic, [] { return true; }, v); k.join();
    CHECK(f.keyed); CHECK(f.stop.find("audio input") != std::string::npos); CHECK(!r.sim.keyed);
}
namespace {
struct FakeTone : txc::IAudioIn {                                                   // another program's FT8-like tone: 1500 Hz, constant envelope, real time
    double amp = 1.0; size_t pos = 0; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    size_t read(float* x, size_t n, double) override {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(1e6 * double(pos + n) / 48000)));
        for (size_t i = 0; i < n; ++i, ++pos) x[i] = float(amp * std::sin(2 * M_PI * 1500 * double(pos) / 48000));
        return n;
    }
    std::string describe() const override { return "fake program"; }
};
}  // namespace
// Keying by another program (CAT PTT) is a voice over with the data chain: a full-scale tone gives the set power, a quieter one less (never more),
// on the right sideband, and the radio unkeys when the program releases PTT.
TEST(data_over_from_another_program_full_scale_gives_the_set_power_and_ptt_unkeys) {
    if (backup_image().empty()) return;
    for (double amp : {1.0, 0.5}) {
        TxRig r; std::vector<std::complex<double>> iq; std::vector<std::pair<double, size_t>> marks;
        r.tx = std::make_unique<txc::TxControl>(r.fx, [&](double) { return std::unique_ptr<IPcmOut>(new CapPcm(&r.sim.level, 48000, &iq, &marks, [] { return wall_utc(); })); }, atu::real_clock());
        r.tx->set_k(5, 11150); r.tx->arm(backup_image(), backup_image());
        FakeTone tone; tone.amp = amp; const auto t0 = std::chrono::steady_clock::now();
        txc::VoiceOptions v; v.chain = tx::data_chain_defaults(); v.peak_w = 5; v.max_key_s = 120;
        auto res = r.tx->voice(14.074, tone, [&] { return std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2000); }, v);
        CHECK(res.keyed); CHECK(res.stop.find("PTT released") != std::string::npos); CHECK(!r.sim.keyed); CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), size_t(2));
        if (amp == 1.0) CHECK(res.max_fwd_w > 4.0 && res.max_fwd_w <= 1.15 * 5.0);     // full scale: the set power (the ALC only trims down)
        else CHECK(res.max_fwd_w > 0.8 && res.max_fwd_w < 2.0);                         // -6 dB audio: about a quarter of the power
        CHECK(sideband_ratio_db(iq, 48000, true) > 20);                                 // USB
    }
}
TEST(safety_stops_are_told_to_the_trip_hook_and_normal_ends_are_not) {
    for (const char* r : {"SWR 3.4 above the 3.0 limit", "forward power 11.5 W above the 4.0 W limit", "reflected power 9.0 W above the 5.0 W limit", "key-down time limit",
                          "meter read failed: x", "PA meter read failed: x", "the I/Q stream failed", "audio input failed: x", "under 5 mW at the table's 1 W drive point: wrong channel, sign or path",
                          "off (SET_MOX 0 FAILED 3 times: x: the radio may still be keyed, power it off)", "the radio refused the key: x"})
        CHECK(txc::TxControl::is_safety_stop(r));
    for (const char* r : {"ramp finished at the target", "over finished", "tune finished", "PTT released", "aborted", "PTT released before keying", "disarmed", "gate destroyed", ""})
        CHECK(!txc::TxControl::is_safety_stop(r));
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); std::vector<std::string> trips; r.tx->set_trip_hook([&](const std::string& w) { trips.push_back(w); });
    txc::Options o; auto ok = r.tx->ramp(14.10, 2.0, o); CHECK(ok.reached); CHECK(trips.empty());                   // a normal ramp tells nobody
    r.sim.pa_from_level = false; r.sim.adc_fwd = 966; r.sim.adc_rev = 238;                                         // the fixed reading is far above a 2 W target
    auto bad = r.tx->ramp(14.10, 2.0, o); CHECK(!bad.reached); CHECK_EQ(trips.size(), size_t(1)); CHECK(trips[0].find("above") != std::string::npos); CHECK(!r.sim.keyed);
}
TEST(tx_antenna_per_over_and_the_atu_only_on_ant1) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); txc::Options o; o.tx_ant = 2;
    auto res = r.tx->ramp(14.10, 2.0, o); CHECK(res.reached);
    const auto ant = sent(r, "RDAL_OP_SET_TX_ANT"); CHECK(!ant.empty()); CHECK_EQ(ant.back().d1, 2u);   // the over went out on ANT2
    CHECK(r.tx->tune(14.10, 3.0, 1.5, o).why_not.find("ATU is on ANT1") != std::string::npos);          // the ATU cannot tune ANT2
    o.tx_ant = 4; CHECK(!r.tx->ramp(14.10, 2.0, o).reached); o.tx_ant = 0; CHECK(!r.tx->ramp(14.10, 2.0, o).reached);   // RX1 IN / nothing: refused
}
TEST(band_lines_are_set_and_settled_before_every_key_down_and_a_fault_refuses_the_over) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); txc::Options o;
    auto lines = std::make_shared<TxLines>(r.fx); LineConfig c; c.role[1] = LineRole::Band; size_t nb = 0; auto* bands = rx_bands(nb);
    for (size_t i = 0; i < nb; ++i) if (std::string(bands[i].id) == "20m") c.band_mask[1] = 1u << i;
    lines->configure(c); lines->arm(); r.tx->set_lines(lines);
    auto res = r.tx->ramp(14.10, 2.0, o); CHECK(res.reached);
    const auto w = write_ops(r.bus); size_t rca = w.size(), mox = w.size();
    for (size_t i = 0; i < w.size(); ++i) { if (w[i].op == opv("RDAL_OP_SET_RCA_TX2") && rca == w.size()) rca = i; if (w[i].op == opv("RDAL_OP_SET_MOX") && w[i].d1 == 1 && mox == w.size()) mox = i; }
    CHECK(rca < w.size()); CHECK(mox < w.size()); CHECK(rca < mox); CHECK_EQ(w[rca].d1, 1u);                // TX2 on (20 m) before the key went down
    CHECK(sent(r, "RDAL_OP_SET_RCA_TX1").empty() && sent(r, "RDAL_OP_SET_RCA_TX3").empty());               // unused lines untouched
    CHECK_EQ(sent(r, "RDAL_OP_SET_RCA_TX2").size(), size_t(1));                                            // and nothing switched during or after the over
    lines->disarm(); const size_t moxes = sent(r, "RDAL_OP_SET_MOX").size();
    auto no = r.tx->ramp(14.10, 2.0, o); CHECK(!no.reached); CHECK(no.stop.find("rear TX lines") != std::string::npos);
    auto t = r.tx->tune(14.10, 3.0, 1.5, o); CHECK(t.why_not.find("rear TX lines") != std::string::npos);
    CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), moxes);                                                    // nothing keyed
    r.tx->set_lines(nullptr); CHECK(r.tx->ramp(14.10, 2.0, o).reached);                                    // no lines in use: as before
}
TEST(test_tone_hold_compares_our_meter_with_the_tuner) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); txc::Options o;
    std::vector<std::string> lines; auto res = r.tx->ramp(14.10, 3.0, o, {}, [&](const std::string& m) { lines.push_back(m); }, 2.0);
    CHECK(res.reached); CHECK(res.compare.size() >= 2);
    for (const auto& c : res.compare) { CHECK(c.adc_fwd > 0); CHECK(c.atu_fwd_w.has_value()); CHECK(std::fabs(*c.atu_fwd_w - 3.0) < 0.01); CHECK(c.atu_swr.has_value()); }
    bool logged = false; for (auto& l : lines) if (l.find("meter ") == 0 && l.find("| tuner 3.00 W") != std::string::npos) logged = true; CHECK(logged);
    for (auto& w : write_ops(r.bus)) if (w.op == 1294u) { const uint32_t req = w.d1 >> 8; CHECK(req != 5u && req != 6u); }   // no tune started by the comparison
}

// First-time set-up: read the EEPROM twice, save it as the reference, never write to the radio, and refuse an unusable image.
TEST(tx_backup_eeprom_reads_twice_saves_the_reference_and_refuses_a_bad_image) {
    auto img = backup_image(); if (img.empty() || !txs::from_image(img).valid) img = read_bin("trx.bin");
    if (!txs::from_image(img).valid) { std::printf("  (skipped: no usable EEPROM image in this checkout)\n"); return; }
    const std::string dir = std::filesystem::temp_directory_path().string() + "/firesidecore_bkup_" + std::to_string(::getpid()); std::filesystem::remove_all(dir);
    {
        Rig r; r.sim.eeprom.assign(img.begin(), img.end());
        txc::TxControl tx(r.fx, [](double) { return std::unique_ptr<IPcmOut>(); }, atu::real_clock());
        const auto b = tx.backup_eeprom(dir);
        CHECK(b.image == img); CHECK(!b.paths.empty()); CHECK(write_ops(r.bus).empty());                  // read-only
        std::ifstream in(b.paths.front(), std::ios::binary); std::vector<uint8_t> saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); CHECK(saved == img);
        tx.arm(b.image, saved); CHECK(tx.armed());                                                        // the saved copy is a valid reference for arming
        auto other = saved; other[0x200] ^= 1;                                                           // a changed EEPROM: refused, and the message says how many bytes
        try { tx.arm(other, saved); CHECK(false); } catch (const std::runtime_error& e) { CHECK(std::string(e.what()).find("1 byte differs") != std::string::npos); }
    }
    {
        Rig r; r.sim.eeprom.assign(kTrxLen, 0);                                                          // all zeros: not a calibration
        txc::TxControl tx(r.fx, [](double) { return std::unique_ptr<IPcmOut>(); }, atu::real_clock());
        const std::string dir2 = dir + "_bad"; std::filesystem::remove_all(dir2);
        CHECK_THROWS(tx.backup_eeprom(dir2), std::runtime_error); CHECK(!std::filesystem::exists(dir2)); CHECK(write_ops(r.bus).empty());
    }
    std::filesystem::remove_all(dir);
}
TEST(signal_generator_check_is_receive_only_and_always_switches_the_relays_back) {
    Rig r; txc::TxControl tx(r.fx, [](double) { return std::unique_ptr<IPcmOut>(); }, r.clk.clock());
    auto sig_on = [&] { auto s = sent(r, "RDAL_OP_SET_SIG"); return !s.empty() && s.back().d1 == 1; };
    bool pre = false;
    // the receiver: the generator appears 1 kHz above the dial, 42 dB above the antenna's signal, once SET_SIG 1 has gone out; the preamp adds 14 dB
    auto read = [&]() -> std::pair<double, double> { if (!sig_on()) return {-95.0, 3500.0}; return {pre ? -39.0 : -53.0, 1000.0}; };
    auto preamp = [&](bool on) { pre = on; return true; };
    auto g = tx.sig_gen_check(14.2, 1000, read, preamp, true);
    CHECK(g.ok); CHECK(g.restore_error.empty()); CHECK_NEAR(g.on_dbfs, -53.0, 1e-6); CHECK(g.preamp_measured); CHECK_NEAR(g.preamp_dbfs - g.on_dbfs, 14.0, 1e-6);
    CHECK(pre);                                                                                    // the preamp ends as it was (on)
    const std::vector<std::pair<const char*, uint32_t>> want = {
        {"RDAL_OP_SET_FULL_DUPLEX", 1}, {"RDAL_OP_SET_TX_FREQ_TW", freq_to_tw(14.201)}, {"RDAL_OP_SET_GEN", 1}, {"RDAL_OP_SET_TEST", 1}, {"RDAL_OP_SET_SIG", 1},
        {"RDAL_OP_SET_FULL_DUPLEX", 0}, {"RDAL_OP_SET_GEN", 0}, {"RDAL_OP_SET_TEST", 0}, {"RDAL_OP_SET_SIG", 0}};
    const auto w = write_ops(r.bus); CHECK_EQ(w.size(), want.size());                              // exactly these writes, nothing else (no MOX, T/R, QSE)
    for (size_t i = 0; i < std::min(w.size(), want.size()); ++i) { CHECK_EQ(w[i].op, opv(want[i].first)); CHECK_EQ(w[i].d1, want[i].second); }
    // not seen (no generator signal): refused, and still switched back
    auto none = [&]() -> std::pair<double, double> { return {-95.0, 3500.0}; };
    const size_t n0 = write_ops(r.bus).size(); auto g2 = tx.sig_gen_check(14.2, 1000, none, {}, false);
    CHECK(!g2.ok); CHECK(g2.why.find("not seen") != std::string::npos); CHECK_EQ(write_ops(r.bus).size(), n0 + 9);
    CHECK(!sig_on()); CHECK(sent(r, "RDAL_OP_SET_MOX").empty()); CHECK(!r.sim.keyed);
    // the generator at the wrong place (e.g. LSB with a +1 kHz offset): refused
    auto off_place = [&]() -> std::pair<double, double> { if (!sig_on()) return {-95.0, 3500.0}; return {-53.0, -1000.0}; };
    CHECK(tx.sig_gen_check(14.2, 1000, off_place, {}, false).why.find("strongest signal") != std::string::npos);
    CHECK(!tx.sig_gen_check(14.2, 50, read, {}, false).ok);                                          // offset outside the passband: refused before any write
    // full duplex is gated to 0 / 1 only
    Safety s; s.allow_write = true; s.permit("RDAL_OP_SET_FULL_DUPLEX"); const OpInfo* fd = find_opcode("RDAL_OP_SET_FULL_DUPLEX");
    s.check_write(*fd, 1, 0); s.check_write(*fd, 0, 0); CHECK_THROWS(s.check_write(*fd, 2, 0), SafetyError); CHECK_THROWS(s.check_write(*fd, 1, 1), SafetyError);
}
TEST(gate_rides_out_a_brief_overshoot_but_not_a_sustained_one_or_a_hard_excess) {
    auto rd = [](double f, double rv = 0.1) { txm::Reading x; x.fwd_w = f; x.rev_w = rv; x.swr = 1.2; return x; };
    GateConfig c; c.max_key_s = 15; c.max_fwd_w = 15; c.max_rev_w = 5; c.max_swr = 3; c.grace_s = 0.15; c.hard_fwd_w = 30;
    {   // a key-down overshoot of two readings: ridden out
        Rig r; TxGate g(r.fx, c, r.clk.clock()); g.arm(); CHECK(g.key("t", 15));
        g.report(rd(15.9)); r.clk.t += 0.05; g.report(rd(16.5)); r.clk.t += 0.05; g.report(rd(10.0)); r.clk.t += 0.05; g.report(rd(10.0));
        CHECK(g.keyed()); g.unkey("done");
    }
    {   // sustained: unkeys once it has lasted 0.15 s
        Rig r; TxGate g(r.fx, c, r.clk.clock()); g.arm(); CHECK(g.key("t", 15));
        g.report(rd(20)); CHECK(g.keyed()); r.clk.t += 0.1; g.report(rd(20)); CHECK(g.keyed()); r.clk.t += 0.06; g.report(rd(20));
        CHECK(!g.keyed()); CHECK(g.last_unkey_reason().find("forward power 20.0 W above the 15.0 W limit") != std::string::npos);
    }
    {   // above the hard limit: at once, no grace
        Rig r; TxGate g(r.fx, c, r.clk.clock()); g.arm(); CHECK(g.key("t", 15));
        g.report(rd(31)); CHECK(!g.keyed()); CHECK(g.last_unkey_reason().find("30.0 W limit") != std::string::npos);
    }
    {   // reflected power and SWR get the same short grace
        Rig r; TxGate g(r.fx, c, r.clk.clock()); g.arm(); CHECK(g.key("t", 15));
        g.report(rd(10, 6)); CHECK(g.keyed()); r.clk.t += 0.2; g.report(rd(10, 6)); CHECK(!g.keyed());
    }
    {   // grace 0 (tunes, drive ramps): the first reading over unkeys, as before
        GateConfig c0 = c; c0.grace_s = 0; Rig r; TxGate g(r.fx, c0, r.clk.clock()); g.arm(); CHECK(g.key("t", 15));
        g.report(rd(15.5)); CHECK(!g.keyed());
    }
}

// A tuner "pass" that is not a match is not saved: here the tuner reports PASS with an SWR byte of 255 (SWR ~1022), as seen on 12 m on 2026-11-21.
TEST(tx_control_tune_rejects_a_pass_without_a_match) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); r.sim.tune_time_polls = 30; r.sim.best_swr = 9; r.sim.pass_regardless = true; r.sim.pass_swr_b3 = 255;
    auto t = r.tx->tune(14.336, 3.0, 1.5, {});
    CHECK(t.outcome); CHECK(t.outcome->result != atu::Atu::TuneResult::Pass); CHECK(!t.saved); CHECK(!r.tx->memory().recall(5));
    CHECK(t.pass_rejected.find("SWR") != std::string::npos); CHECK(!r.sim.keyed);
    r.sim.best_swr = 1.0; r.sim.pass_regardless = false; r.sim.pass_swr_b3 = 0;                      // a real match still passes and is saved
    auto ok = r.tx->tune(14.336, 3.0, 1.5, {});
    CHECK(ok.outcome && ok.outcome->result == atu::Atu::TuneResult::Pass); CHECK(ok.saved); CHECK(ok.pass_rejected.empty());
}
// A pass whose SWR was read with no forward power (0.00 W) is not a match: seen on 60 m 2026-11-25, it overwrote a good setting.
TEST(tx_control_tune_rejects_a_pass_read_without_power) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image()); r.sim.tune_time_polls = 30; r.sim.pass_fwd_cw = 0; r.sim.adc_rev = r.sim.adc_fwd; r.sim.rev_frac = 0.5;   // our meter: a poor match
    auto t = r.tx->tune(14.336, 3.0, 1.5, {});
    CHECK(t.outcome); CHECK(t.outcome->result != atu::Atu::TuneResult::Pass); CHECK(!t.saved); CHECK(!r.tx->memory().recall(5));
    CHECK(t.pass_rejected.find("0.00 W forward") != std::string::npos); CHECK(!r.sim.keyed);
    r.sim.adc_rev = 0; r.sim.rev_frac = 0.01;                                                         // a quick real pass (2026-11-26, 15 m): the tuner reads 0 W, our meter a good SWR
    auto ok = r.tx->tune(14.336, 3.0, 1.5, {});
    CHECK(ok.outcome && ok.outcome->result == atu::Atu::TuneResult::Pass); CHECK(ok.saved); CHECK(ok.pass_rejected.empty()); CHECK(ok.last_swr && *ok.last_swr <= 2.0);
}

// disarm() while an over runs and another is queued behind it: the queued one must not key (its start clears abort_, but it finds itself disarmed).
TEST(disarm_is_not_lost_when_another_over_is_queued) {
    if (backup_image().empty()) return;
    TxRig r; r.tx->arm(backup_image(), backup_image());
    txc::VoiceOptions v; v.chain = tx::data_chain_defaults(); v.peak_w = 5; v.max_key_s = 30;
    FakeTone a_in, b_in; txc::VoiceResult a, b;
    std::thread ta([&] { a = r.tx->voice(14.2, a_in, [] { return true; }, v); });
    for (int i = 0; i < 300 && !r.tx->keyed(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(r.tx->keyed());
    std::thread tb([&] { b = r.tx->voice(14.2, b_in, [] { return true; }, v); });                 // queued on the transmit lock
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = std::chrono::steady_clock::now();
    r.tx->disarm(); ta.join(); tb.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));                         // not the queued over's 30 s time-out
    CHECK(a.keyed); CHECK(a.stop.find("abort") != std::string::npos);
    CHECK(!b.keyed); CHECK(b.why_not.find("not armed") != std::string::npos); CHECK(!r.sim.keyed); CHECK(!r.tx->armed());
}

TEST(tx_over_limits_have_absolute_ceilings) {
    CHECK_NEAR(txc::fwd_limit_w(10), 15.0, 1e-9); CHECK_NEAR(txc::hard_fwd_limit_w(10), 30.0, 1e-9);   // low power: 1.5 x / 3 x as before
    CHECK_NEAR(txc::fwd_limit_w(1), 2.0, 1e-9); CHECK_NEAR(txc::hard_fwd_limit_w(1), 6.0, 1e-9);
    CHECK_NEAR(txc::fwd_limit_w(100), 110.0, 1e-9); CHECK_NEAR(txc::hard_fwd_limit_w(100), 130.0, 1e-9);   // 100 W: not 150 / 300
    CHECK_NEAR(txc::fwd_limit_w(50), 75.0, 1e-9); CHECK_NEAR(txc::hard_fwd_limit_w(50), 130.0, 1e-9);
    for (double w = 1; w <= 100; w += 1) CHECK(txc::hard_fwd_limit_w(w) > txc::fwd_limit_w(w));
}

// ---------------------------------------------------------------- CW (semi break-in)
namespace {
struct ScriptKeys : txc::ICwKeys {                                                    // the paddle as a function of time since the over first asked
    std::function<uint8_t(double)> f; std::chrono::steady_clock::time_point t0{}; bool started = false; int calls = 0;
    explicit ScriptKeys(std::function<uint8_t(double)> g) : f(std::move(g)) {}
    uint8_t paddles() override { if (!started) { started = true; t0 = std::chrono::steady_clock::now(); } ++calls;
        return f(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()); }
};
struct CwRig : TxRig {                                                                // keeps every transmitted frame
    std::vector<std::complex<double>> iq; std::vector<std::pair<double, size_t>> marks; std::vector<std::pair<bool, double>> hook;
    CwRig() {
        tx = std::make_unique<txc::TxControl>(fx, [this](double) { return std::unique_ptr<IPcmOut>(new CapPcm(&sim.level, 48000, &iq, &marks, [] { return wall_utc(); })); }, atu::real_clock());
        tx->set_k(5, 11150); tx->set_key_hook([this](bool k, double off) { hook.push_back({k, off}); });
    }
    std::vector<std::pair<size_t, size_t>> marks_on(double thr = 0.5) const {        // [start, end) of each keyed element, at half amplitude
        double pk = 0; for (auto& z : iq) pk = std::max(pk, std::abs(z));
        std::vector<std::pair<size_t, size_t>> m; bool on = false; size_t s = 0;
        for (size_t i = 0; i < iq.size(); ++i) { const bool o = std::abs(iq[i]) > thr * pk; if (o && !on) s = i; if (!o && on) m.push_back({s, i}); on = o; }
        if (on) m.push_back({s, iq.size()}); return m;
    }
};
}  // namespace
TEST(cw_paddle_sends_dits_on_the_dial_at_the_set_power_and_unkeys_after_the_hang) {
    if (backup_image().empty()) return;
    CwRig r; r.tx->arm(backup_image(), backup_image());
    ScriptKeys keys([](double t) { return uint8_t(t < 0.25 ? 1 : 0); });             // the dit paddle held for 250 ms
    txc::CwOptions c; c.power_w = 10; c.keyer.wpm = 20; c.hang_s = 0.3;
    const auto res = r.tx->cw(14.030, keys, c);
    CHECK(res.keyed); CHECK(res.stop.find("CW over finished") != std::string::npos); CHECK(!r.sim.keyed); CHECK(!r.tx->keyed());
    CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), size_t(2));                            // one over: key, unkey
    CHECK(res.max_fwd_w > 7.0 && res.max_fwd_w <= 1.15 * 10);
    auto modes = sent(r, "RDAL_OP_SET_TX_DSP_MODE"); CHECK(!modes.empty()); CHECK_EQ(modes.back().d1, 4u);   // CWU
    CHECK_EQ(r.hook.size(), size_t(2)); CHECK(r.hook[0].first); CHECK_NEAR(r.hook[0].second, txc::kCwOffsetHz, 1e-9);   // the DDS 1 kHz below the dial for the over
    const auto m = r.marks_on(); CHECK(m.size() >= 3 && m.size() <= 5);              // 250 ms of dits at 20 wpm (120 ms each, the last one completes)
    for (auto& e : m) CHECK_NEAR(double(e.second - e.first) / 48000, 0.060, 0.004);    // one unit = 60 ms at 20 wpm
    for (size_t i = 1; i < m.size(); ++i) CHECK_NEAR(double(m[i].first - m[i - 1].first) / 48000, 0.120, 0.004);
    {   // the carrier: 1 kHz above the DDS (= on the dial), nothing at the image
        std::complex<double> up, dn; const auto& e = m.front();
        for (size_t i = e.first + 240; i < e.second - 240; ++i) { const auto z = std::conj(r.iq[i]); const auto w = std::polar(1.0, 2 * M_PI * 1000 * double(i) / 48000); up += z * std::conj(w); dn += z * w; }
        CHECK(20 * std::log10(std::abs(up) / (std::abs(dn) + 1e-30)) > 20);
    }
    const size_t last = m.back().second; CHECK(double(r.iq.size() - last) / 48000 > 0.25);   // MOX held through the hang with the carrier at zero
    CHECK(double(m.front().first) / 48000 > 0.005);                                    // the first element starts after MOX (no hot switching)
}
TEST(cw_text_iambic_squeeze_straight_key_and_paddle_breaks_the_text) {
    if (backup_image().empty()) return;
    {   // text: "E" = one dit, then the over ends by itself
        CwRig r; r.tx->arm(backup_image(), backup_image()); ScriptKeys keys([](double) { return uint8_t(0); });
        txc::CwOptions c; c.text = "E"; c.hang_s = 0.1; const auto res = r.tx->cw(14.030, keys, c);
        CHECK(res.keyed && res.text_done && !res.text_stopped); CHECK_EQ(r.marks_on().size(), size_t(1));
    }
    {   // the paddle during a text stops the text and keys the paddle instead
        CwRig r; r.tx->arm(backup_image(), backup_image()); ScriptKeys keys([](double t) { return uint8_t(t > 0.3 && t < 0.35 ? 2 : 0); });
        txc::CwOptions c; c.text = "TEST TEST TEST"; c.hang_s = 0.2; const auto res = r.tx->cw(14.030, keys, c);
        CHECK(res.text_stopped && !res.text_done); CHECK(res.seconds < 2.0);
    }
    {   // a squeeze (both paddles) alternates dah dit; mode B adds one more element after release
        CwRig r; r.tx->arm(backup_image(), backup_image()); ScriptKeys keys([](double t) { return uint8_t(t < 0.05 ? 2 : t < 0.40 ? 3 : 0); });
        txc::CwOptions c; c.keyer.wpm = 20; c.hang_s = 0.2; r.tx->cw(14.030, keys, c);
        const auto m = r.marks_on(); CHECK(m.size() >= 3);
        CHECK_NEAR(double(m[0].second - m[0].first) / 48000, 0.180, 0.005); CHECK_NEAR(double(m[1].second - m[1].first) / 48000, 0.060, 0.005);   // dah, dit
    }
    {   // straight key: the contact itself, any length
        CwRig r; r.tx->arm(backup_image(), backup_image()); ScriptKeys keys([](double t) { return uint8_t(t < 0.5 ? 1 : 0); });
        txc::CwOptions c; c.straight = true; c.hang_s = 0.2; r.tx->cw(14.030, keys, c);
        const auto m = r.marks_on(); CHECK_EQ(m.size(), size_t(1)); if (!m.empty()) CHECK_NEAR(double(m[0].second - m[0].first) / 48000, 0.5, 0.01);
    }
}
TEST(cw_refusals_stuck_key_and_stop_leave_the_radio_unkeyed) {
    if (backup_image().empty()) return;
    CwRig r; r.tx->arm(backup_image(), backup_image()); ScriptKeys down([](double) { return uint8_t(1); });
    txc::CwOptions c; c.power_w = 101; CHECK(!r.tx->cw(14.03, down, c).keyed); c.power_w = 10;
    CHECK(!r.tx->cw(7.03, down, c).keyed);                                             // no measured drive level on 40 m in this rig
    CHECK(!r.tx->cw(14.5, down, c).keyed);                                             // outside the band
    c.text = "CQ {X}"; CHECK(!r.tx->cw(14.03, down, c).keyed); c.text.clear();
    CHECK_EQ(sent(r, "RDAL_OP_SET_MOX").size(), size_t(0));
    c.straight = true; c.max_keydown_s = 1; const auto s = r.tx->cw(14.03, down, c);   // a stuck key
    CHECK(s.keyed); CHECK(s.stop.find("stuck") != std::string::npos); CHECK(txc::TxControl::is_safety_stop(s.stop)); CHECK(!r.sim.keyed);
    c.straight = false; c.max_keydown_s = 15;
    std::thread k([&] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); r.tx->abort(); });
    const auto a = r.tx->cw(14.03, down, c); k.join();                                 // the paddle held: dits until Stop
    CHECK(a.keyed); CHECK(a.stop.find("aborted") != std::string::npos); CHECK(!r.sim.keyed);
}
#include "fireside/cw_keys.h"
TEST(cw_key_watcher_latches_short_taps_and_reports_a_press) {
    std::atomic<uint32_t> bits{0}; std::atomic<int> presses{0};
    cwk::KeyWatcher w([&]() -> std::optional<uint32_t> { return bits.load(); }, 0.002);
    w.set_on_press([&] { ++presses; }); w.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); CHECK_EQ(w.take(), uint8_t(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));                       // no over reading the input for > 100 ms: a press starts one
    bits = 1; std::this_thread::sleep_for(std::chrono::milliseconds(10)); bits = 0; std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(presses.load(), 1); CHECK_EQ(w.current(), uint8_t(0));
    CHECK_EQ(w.take(), uint8_t(1)); CHECK_EQ(w.take(), uint8_t(0));                     // the tap was kept until taken, once
    bits = 2; std::this_thread::sleep_for(std::chrono::milliseconds(10)); CHECK_EQ(presses.load(), 1);   // read recently: an over is running, no new start
    CHECK_EQ(w.take() & 2, 2); bits = 0;
    w.stop(); CHECK(w.reads() > 10); CHECK_EQ(w.failures(), uint64_t(0));
    cwk::KeyWatcher bad([]() -> std::optional<uint32_t> { return std::nullopt; }, 0.002); bad.start(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); bad.stop(); CHECK(bad.failures() > 0);
}
#include "fireside/cw_practice.h"
TEST(cw_practice_keys_only_the_sidetone_feed_at_the_set_speed) {   // the paddle -> the keyer -> a callback; decimal speeds work
    std::atomic<uint8_t> pad{0}; std::atomic<uint64_t> fed{0}, down{0};
    tx::KeyerConfig k; k.wpm = 12.5;
    cwp::Practice p([&] { return pad.load(); }, [&](const uint8_t* kd, size_t n, double r) { CHECK_EQ(r, 48000.0); fed += n; for (size_t i = 0; i < n; ++i) down += kd[i]; }, k, false);
    p.start(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); CHECK(fed.load() > 2400); CHECK_EQ(down.load(), uint64_t(0));   // paddle up: key up
    pad = 1; std::this_thread::sleep_for(std::chrono::milliseconds(800)); pad = 0; std::this_thread::sleep_for(std::chrono::milliseconds(300)); p.stop();
    const double down_s = double(down.load()) / 48000.0;                       // dots held ~0.8 s at 12.5 wpm: dot 96 ms on, 96 ms off, so about half
    CHECK(down_s > 0.3); CHECK(down_s < 0.6); CHECK_EQ(p.down_samples(), down.load());
    cwp::Practice st([&] { return uint8_t(2); }, nullptr, k, true); st.start(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); st.stop();
    CHECK(st.down_samples() > 0); CHECK_EQ(st.down_samples(), st.samples());     // straight key: down while closed
}
TEST(cw_key_watcher_slow_read_leaves_the_lock_free_for_others) {    // READ_PTT holds the session lock ~16 ms: the GUI must still get it
    std::mutex m;
    cwk::KeyWatcher w([&]() -> std::optional<uint32_t> { std::lock_guard<std::mutex> l(m); std::this_thread::sleep_for(std::chrono::milliseconds(16)); return 0u; }, 0.005);
    w.start(); std::this_thread::sleep_for(std::chrono::milliseconds(30));
    double worst = 0;
    for (int i = 0; i < 10; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        { std::lock_guard<std::mutex> l(m); }
        worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    w.stop();
    CHECK(worst < 0.1); CHECK(w.reads() > 5);
}
