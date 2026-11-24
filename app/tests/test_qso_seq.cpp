// FT8/FT4 QSO sequencing: both standard exchanges, repeats, give-up, other callers, late double-clicks, logging once.
#include <thread>
#include <cstring>
#include "fireside/qso_seq.h"
#include "fireside/tx_control.h"
#include "test_util.h"
using namespace fireside;
namespace { seq::Heard H(const char* t, int snr = -10) { return seq::parse(t, snr); } }

TEST(seq_parses_the_message_kinds) {
    auto a = H("CQ DX DK0ULR JN58"); CHECK(a.ok && a.cq && a.from == "DK0ULR" && a.grid == "JN58");
    auto b = H("M0OML DK0ULR R-12"); CHECK(b.ok && b.to == "M0OML" && b.r_report && b.report == "-12");
    auto c = H("M0OML DK0ULR +05"); CHECK(c.report == "+05" && !c.r_report);
    CHECK(H("M0OML DK0ULR RR73").rr73); CHECK(H("M0OML DK0ULR RRR").rrr); CHECK(H("M0OML DK0ULR 73").seventy3); CHECK(H("<M0OML> DK0ULR JN58").to == "M0OML");
    CHECK(!H("M0OML <...> -10").ok); CHECK(!H("TNX").ok);
    CHECK_EQ(seq::report_text(-8), std::string("-08")); CHECK_EQ(seq::report_text(5), std::string("+05")); CHECK_EQ(seq::report_text(-40), std::string("-30"));
}
TEST(seq_answers_a_cq_through_to_73_and_logs_once) {
    seq::Sequencer s("M0OML", "IO92ar");
    CHECK(s.reply_to(H("CQ DK0ULR JN58", -12))); CHECK_EQ(s.next_message(), std::string("DK0ULR M0OML IO92"));
    CHECK(!s.take_log());
    CHECK(!s.heard(H("G4ABC DK0ULR -03")));                                          // they answer someone else: no change
    CHECK_EQ(s.next_message(), std::string("DK0ULR M0OML IO92")); CHECK_EQ(s.tries(), 2);
    CHECK(s.heard(H("M0OML DK0ULR -15", -8))); CHECK_EQ(s.next_message(), std::string("DK0ULR M0OML R-08"));
    CHECK(!s.heard(H("M0OML DK0ULR -15", -9))); CHECK_EQ(s.next_message(), std::string("DK0ULR M0OML R-08"));   // their report again: ours again
    CHECK(s.heard(H("M0OML DK0ULR RR73"))); CHECK_EQ(s.next_message(), std::string("DK0ULR M0OML 73"));
    auto l = s.take_log(); CHECK(l); CHECK_EQ(l->call, std::string("DK0ULR")); CHECK_EQ(l->rst_sent, std::string("-08")); CHECK_EQ(l->rst_rcvd, std::string("-15")); CHECK_EQ(l->grid, std::string("JN58"));
    CHECK(!s.take_log());                                                               // once
    CHECK_EQ(s.next_message(), std::string("")); CHECK(s.stage() == seq::Stage::Done);  // 73 goes out once
}
TEST(seq_calls_cq_and_works_the_first_caller) {
    seq::Sequencer s("M0OML", "IO92");
    s.call_cq(); CHECK_EQ(s.next_message(), std::string("CQ M0OML IO92"));
    CHECK(!s.heard(H("M0OML 0J0BQR JA35", -17))); CHECK(s.stage() == seq::Stage::Cq);                       // a callsign no country uses (a false decode): not answered
    CHECK(s.heard(H("M0OML SP9ABC KN09", -14))); CHECK_EQ(s.dx(), std::string("SP9ABC")); CHECK_EQ(s.next_message(), std::string("SP9ABC M0OML -14"));
    CHECK(!s.heard(H("M0OML OH2XYZ KP20")));                                         // a second caller waits
    CHECK(s.heard(H("M0OML SP9ABC R-09"))); CHECK_EQ(s.next_message(), std::string("SP9ABC M0OML RR73"));
    auto l = s.take_log(); CHECK(l && l->rst_sent == "-14" && l->rst_rcvd == "-09" && l->grid == "KN09");   // logged when RR73 goes out
    CHECK(s.heard(H("M0OML SP9ABC R-09"))); CHECK_EQ(s.next_message(), std::string("SP9ABC M0OML RR73"));    // they missed it: again
    CHECK_EQ(s.next_message(), std::string()); CHECK(s.stage() == seq::Stage::Done);                                // nothing back: RR73 is not repeated blindly (WSJT-X)
    CHECK(s.heard(H("M0OML SP9ABC R-09"))); CHECK_EQ(s.next_message(), std::string("SP9ABC M0OML RR73"));    // their repeat decoded late: once more
    CHECK_EQ(s.next_message(), std::string()); CHECK(s.heard(H("M0OML SP9ABC R-09"))); CHECK_EQ(s.next_message(), std::string("SP9ABC M0OML RR73"));
    CHECK_EQ(s.next_message(), std::string()); CHECK(!s.heard(H("M0OML SP9ABC R-09")));                       // twice at most after stopping
    CHECK(!s.take_log());                                                                                      // logged once only
    s.heard(H("M0OML SP9ABC 73")); CHECK(s.stage() == seq::Stage::Done); CHECK_EQ(s.next_message(), std::string("")); CHECK(!s.take_log());   // their 73 after the end: nothing more
}
TEST(seq_gives_up_and_handles_late_double_clicks) {
    seq::Sequencer s("M0OML", "IO92", 3);
    s.reply_to(H("CQ EA7R IM66")); for (int i = 0; i < 3; ++i) CHECK(!s.next_message().empty());
    CHECK(s.next_message().empty()); CHECK(s.stage() == seq::Stage::GaveUp); CHECK(!s.active());
    seq::Sequencer t("M0OML", "IO92");
    CHECK(t.reply_to(H("M0OML DL2AL JO31", -7))); CHECK_EQ(t.next_message(), std::string("DL2AL M0OML -07"));   // they called me
    seq::Sequencer u("M0OML", "IO92");
    CHECK(u.reply_to(H("M0OML DL2AL -11", -6))); CHECK_EQ(u.next_message(), std::string("DL2AL M0OML R-06"));   // their report: our R-report
    seq::Sequencer v("M0OML", "IO92");
    CHECK(v.reply_to(H("M0OML DL2AL R-11", -6))); CHECK_EQ(v.next_message(), std::string("DL2AL M0OML RR73")); CHECK(v.take_log());
    seq::Sequencer w("M0OML", "IO92"); CHECK(!w.reply_to(H("CQ M0OML IO92"))); CHECK(!w.reply_to(H("M0OML DL2AL 73")));   // myself; nothing left to answer
    seq::Sequencer x("M0OML", "IO92"); CHECK(x.reply_to(H("G4ABC DL2AL -05"))); CHECK_EQ(x.next_message(), std::string("DL2AL M0OML IO92"));   // working someone else: call them
    seq::Sequencer y("M0OML/P", "IO92"); y.reply_to(H("CQ DL2AL JO31")); CHECK(y.heard(H("M0OML/P DL2AL -10"))); CHECK_EQ(y.next_message(), std::string("DL2AL M0OML/P R-10"));
}
#include "fireside/decode.h"
TEST(ft8_slot_cut_short_by_a_transmission_still_decodes) {   // receive pauses ~0.8 s before our slot: the caller's slot so far is decoded, not lost
    auto slot = decode::synth_ft8("M0OML DK0ULR -12", 1500, 48000, 0.05, 0.5);
    decode::Ft8Options o; decode::Ft8Decoder d(o); std::string out;
    d.process(slot.data(), size_t(14.2 * 48000), out); CHECK(out.empty());            // not a whole slot: no decode yet
    d.finish(out); CHECK(out.find("M0OML DK0ULR -12") != std::string::npos);
    decode::Ft8Decoder e(o); std::string o2; e.process(slot.data(), size_t(10.0 * 48000), o2); e.finish(o2); CHECK(o2.empty());   // signals not all in yet: nothing
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
TEST(ft8_quick_pass_timing_on_a_real_recording) {   // FIRESIDE_FT8_12K=file.f32 (12 kHz float audio, slot-aligned): quick pass vs full decode per slot
    const char* path = std::getenv("FIRESIDE_FT8_12K"); if (!path) return;
    std::ifstream in(path, std::ios::binary); std::vector<char> a((std::istreambuf_iterator<char>(in)), {}); std::vector<float> x(a.size() / 4); std::memcpy(x.data(), a.data(), x.size() * 4);
    for (size_t s = 0; (s + 1) * 180000 <= x.size(); ++s) {
        const float* slot = x.data() + s * 180000;
        auto t0 = std::chrono::steady_clock::now(); auto full = decode::Ft8Decoder::decode_slot(slot, 180000, "", false, "M0OML"); const double tf = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        decode::Ft8Options o; o.sample_rate = 12000; o.my_call = "M0OML"; decode::Ft8Decoder d(o); std::string out;
        d.process(slot, size_t(14.2 * 12000), out); t0 = std::chrono::steady_clock::now(); d.finish(out); const double tq = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t nq = 0; for (char c : out) nq += c == '\n';
        std::printf("  slot %zu: full %zu in %.2f s | cut at 14.2 s, quick pass %zu in %.2f s\n", s, full.size(), tf, nq, tq);
    }
}
TEST(ft8_stream_restarting_just_after_a_boundary_still_decodes_that_slot) {   // receive comes back after our over: up to 0.4 s late loses nothing
    auto slot = decode::synth_ft8("M0OML DK0ULR R-12", 1500, 48000, 0.05, 0.5); slot.resize(slot.size() + 48000 * 15, 0.0f);   // a slot, then a silent one
    for (double late : {0.3, 0.6}) {
        decode::Ft8Options o; o.start_offset_s = late; decode::Ft8Decoder d(o); std::string out;
        const size_t from = size_t(late * 48000);
        for (size_t i = from; i + 4800 <= slot.size(); i += 4800) { d.process(slot.data() + i, 4800, out); }
        for (int k = 0; k < 300 && d.busy(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        d.process(nullptr, 0, out);
        if (late < 0.4) CHECK(out.find("M0OML DK0ULR R-12") != std::string::npos); else CHECK(out.find("M0OML DK0ULR R-12") == std::string::npos);
    }
}
TEST(ft8_peek_gives_the_answer_early_and_the_full_decode_does_not_repeat_it) {   // duplex: receive never pauses; the answer is read at 14.2 s
    auto a = decode::synth_ft8("M0OML DK0ULR -12", 1500, 48000, 0.05, 0.5), b = decode::synth_ft8("CQ EA7R IM66", 800, 48000, 0.001, 0.5);
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] += b[i];
    a.resize(a.size() + 48000 * 15, 0.0f);
    decode::Ft8Options o; decode::Ft8Decoder d(o); std::string out;
    d.process(a.data(), size_t(14.2 * 48000), out); CHECK(out.empty());
    std::string early; d.peek(early); CHECK(early.find("M0OML DK0ULR -12") != std::string::npos);
    std::string again; d.peek(again); CHECK(again.empty());                            // once per slot
    std::string rest; d.process(a.data() + size_t(14.2 * 48000), a.size() - size_t(14.2 * 48000), rest);
    for (int k = 0; k < 300 && d.busy(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    d.process(nullptr, 0, rest);
    CHECK(rest.find("M0OML DK0ULR -12") == std::string::npos);                         // not twice
    const bool weak_early = early.find("CQ EA7R") != std::string::npos, weak_late = rest.find("CQ EA7R") != std::string::npos;
    CHECK(weak_early != weak_late || (!weak_early && !weak_late));                     // the weak one at most once
}
TEST(zz_prepare_ft_timing) {
    if (!std::getenv("FIRESIDE_TIMING")) return;
    for (double rate : {48000.0, 96000.0, 192000.0}) for (bool ft4 : {false, true}) {
        const auto t0 = std::chrono::steady_clock::now(); auto f = txc::TxControl::prepare_ft("DA0M M0OML R-07", 1500, ft4, rate);
        std::printf("  %s %.0f kHz: %.3f s (%zu samples)\n", ft4 ? "FT4" : "FT8", rate / 1000, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), f.buf.size());
    }
}
TEST(ft_transmit_signal_is_ready_well_inside_a_slot) {   // it is rendered when our slot is due: ~3 s (filtered audio) made every over a cycle late
    const auto t0 = std::chrono::steady_clock::now(); auto f = txc::TxControl::prepare_ft("DA0M M0OML R-07", 1500, false, 192000);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!f.buf.empty()); CHECK(s < 0.5);
}

TEST(seq_a_limited_call_gives_up_after_two_tries_and_an_answer_restores_the_normal_limit) {
    seq::Sequencer s("M0OML", "IO92");
    CHECK(s.reply_to(H("M0OML OK1XYZ JO70", -12))); s.limit_tries(2);
    CHECK_EQ(s.next_message(), std::string("OK1XYZ M0OML -12")); CHECK_EQ(s.next_message(), std::string("OK1XYZ M0OML -12"));
    CHECK_EQ(s.next_message(), std::string()); CHECK(s.stage() == seq::Stage::GaveUp);                     // two tries, then move on
    seq::Sequencer v("M0OML", "IO92");
    CHECK(v.reply_to(H("M0OML OK1XYZ JO70", -12))); v.limit_tries(2); CHECK(!v.next_message().empty());
    CHECK(v.heard(H("M0OML OK1XYZ R-10")));                                                                   // they answered: the next step
    for (int i = 0; i < 4; ++i) { CHECK_EQ(v.next_message(), std::string("OK1XYZ M0OML RR73")); (void)v.heard(H("M0OML OK1XYZ R-10")); }   // more than 2: the limit was for the call only
}
TEST(seq_a_late_rr73_while_calling_cq_is_not_a_new_qso) {   // we sent 73 and went back to CQ; they repeat RR73 (missed our 73): nothing is sent or logged
    seq::Sequencer s("M0OML", "IO92");
    s.call_cq(); CHECK_EQ(s.next_message(), std::string("CQ M0OML IO92"));
    CHECK(!s.heard(H("M0OML DK0ULR RR73"))); CHECK(!s.heard(H("M0OML DK0ULR RRR"))); CHECK(!s.heard(H("M0OML DK0ULR 73")));
    CHECK(s.stage() == seq::Stage::Cq); CHECK(s.dx().empty()); CHECK(!s.take_log()); CHECK_EQ(s.next_message(), std::string("CQ M0OML IO92"));
    CHECK(s.heard(H("M0OML DK0ULR JN58", -11))); CHECK_EQ(s.next_message(), std::string("DK0ULR M0OML -11"));   // a real answer still works
}
TEST(seq_a_refused_reply_leaves_the_qso_alone) {   // double-clicking a late "ME OTHER 73" must not wipe a finished QSO's pending log
    seq::Sequencer s("M0OML", "IO92");
    CHECK(s.reply_to(H("M0OML DL2AL R-11", -6))); CHECK_EQ(s.next_message(), std::string("DL2AL M0OML RR73"));
    CHECK(!s.reply_to(H("M0OML G4ABC 73")));
    CHECK_EQ(s.dx(), std::string("DL2AL")); auto l = s.take_log(); CHECK(l && l->call == "DL2AL" && l->rst_rcvd == "-11");
}
TEST(ft8_callsign_hash_table_never_hangs_or_loses_calls) { CHECK_EQ(decode::hash_table_self_check(), std::string()); }
TEST(seq_cq_picks_a_clear_frequency) {
    using seq::HeardSignal;
    CHECK_NEAR(seq::clear_tx_hz({}, 0, 1200, false), 1200.0, 1e-9);                                  // nothing heard: stay
    CHECK_NEAR(seq::clear_tx_hz({{600, 0}, {1800, 1}}, 0, 1200, false), 1200.0, 1e-9);                // already clear: stay (no hopping)
    const double f = seq::clear_tx_hz({{1190, 0}, {1000, 1}, {1500, 0}}, 0, 1200, false);           // someone on top of us: move
    CHECK(std::fabs(f - 1200) > 1);
    for (double h : {1190.0, 1000.0, 1500.0}) CHECK(f + 50 + 15 <= h || f >= h + 50 + 15);           // clear of everyone, with the guard
    CHECK(f >= 300 && f + 50 <= 2700); CHECK_NEAR(std::fmod(f, 10.0), 0.0, 1e-9);
    CHECK(f > 1500 + 65);                                                                             // the widest gap (1565..2650), not a narrow one
    std::vector<HeardSignal> full; for (double h = 300; h < 2700; h += 75) full.push_back({h, (int(h) / 75) % 2});   // a packed band: clear only in our own slots
    const double g = seq::clear_tx_hz(full, 0, 1210, false);
    for (const auto& h : full) if (h.parity == 0) CHECK(g + 65 <= h.hz || g >= h.hz + 65);           // our own slots clear at least
    std::vector<HeardSignal> solid; for (double h = 250; h < 2750; h += 30) { solid.push_back({h, 0}); solid.push_back({h, 1}); }
    CHECK_NEAR(seq::clear_tx_hz(solid, 0, 1200, false), 1200.0, 1e-9);                               // nowhere clear: stay
    const double f4 = seq::clear_tx_hz({{1190, 0}}, 0, 1200, true); CHECK(f4 + 83.3 + 15 <= 1190 || f4 >= 1190 + 83.3 + 15);   // FT4 is wider
    CHECK_EQ(seq::line_parity("093015 -10  0.1 1200 ~  CQ X", false), 1); CHECK_EQ(seq::line_parity("093000 -10", false), 0);
    CHECK_EQ(seq::line_parity("093007 -10", true), 1); CHECK_EQ(seq::line_parity("S003 x", false), 1);
}
