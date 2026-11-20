// FT8/FT4 slot scheduler: pure functions of UTC. Boundaries, parity, lead times, late joins, and a cross-check against the real generators.
#include <cmath>
#include <ctime>
#include <random>
#include "fireside/decode.h"
#include "fireside/tx_dsp.h"
#include "test_util.h"
using namespace fireside; using namespace fireside::tx;

namespace {
double utc_of(int y, int mo, int d, int h, int mi, double s) { struct tm t{}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; return double(timegm(&t)) + s; }
double sec_in_minute(double utc) { return std::fmod(std::fmod(utc, 60.0) + 60.0, 60.0); }
}  // namespace

TEST(slots_basic_constants) {
    CHECK_EQ(slot_seconds(DigiMode::FT8), 15.0); CHECK_EQ(slot_seconds(DigiMode::FT4), 7.5);
    CHECK_NEAR(signal_seconds(DigiMode::FT8), 12.64, 1e-9); CHECK_NEAR(signal_seconds(DigiMode::FT4), 5.04, 1e-9);
    CHECK(signal_seconds(DigiMode::FT8) + default_start_offset_s(DigiMode::FT8) < slot_seconds(DigiMode::FT8));
    CHECK(signal_seconds(DigiMode::FT4) + default_start_offset_s(DigiMode::FT4) < slot_seconds(DigiMode::FT4));
}
TEST(ft8_slots_follow_utc_and_parity_is_minute_based) {
    const double t0 = utc_of(2026, 11, 15, 12, 34, 0);                                    // a real date, on the minute
    struct C { double off; double start; Parity par; }; 
    for (C c : {C{0, 0, Parity::Even}, C{14.999, 0, Parity::Even}, C{15, 15, Parity::Odd}, C{29.9, 15, Parity::Odd}, C{30, 30, Parity::Even}, C{45.5, 45, Parity::Odd}, C{59.999, 45, Parity::Odd}}) {
        const Slot s = slot_containing(t0 + c.off, DigiMode::FT8);
        CHECK_EQ(s.start_utc, t0 + c.start); CHECK(s.parity == c.par);
    }
    CHECK(slot_containing(t0 + 60, DigiMode::FT8).parity == Parity::Even);                // the next minute starts even again
    CHECK(slot_containing(t0 - 0.001, DigiMode::FT8).parity == Parity::Odd);              // just before: the :45 slot of the previous minute
    CHECK_EQ(slot_containing(t0 - 0.001, DigiMode::FT8).start_utc, t0 - 15);
}
TEST(ft4_slots_are_7_5_seconds_and_even_means_00_15_30_45) {
    const double t0 = utc_of(2026, 11, 15, 12, 34, 0);
    struct C { double off, start; Parity par; };
    for (C c : {C{0, 0, Parity::Even}, C{7.49, 0, Parity::Even}, C{7.5, 7.5, Parity::Odd}, C{14.99, 7.5, Parity::Odd}, C{15, 15, Parity::Even}, C{52.5, 52.5, Parity::Odd}, C{59.9, 52.5, Parity::Odd}}) {
        const Slot s = slot_containing(t0 + c.off, DigiMode::FT4); CHECK_EQ(s.start_utc, t0 + c.start); CHECK(s.parity == c.par);
    }
}
TEST(next_slot_is_strictly_after) {
    const double t0 = utc_of(2026, 11, 15, 12, 34, 0);
    CHECK_EQ(next_slot(t0 + 0.0, DigiMode::FT8).start_utc, t0 + 15);                      // exactly on a boundary: the NEXT slot, not this one
    CHECK_EQ(next_slot(t0 + 14.9, DigiMode::FT8).start_utc, t0 + 15); CHECK_EQ(next_slot(t0 + 59.9, DigiMode::FT8).start_utc, t0 + 60);
    CHECK_EQ(next_slot(t0 + 3.0, DigiMode::FT4).start_utc, t0 + 7.5);
}
TEST(next_slot_with_parity_and_lead) {
    const double t0 = utc_of(2026, 11, 15, 12, 34, 0);
    auto at = [&](double off, Parity p, double lead = 0) { return next_slot_with_parity(t0 + off, DigiMode::FT8, p, lead).start_utc - t0; };
    CHECK_EQ(at(14.9, Parity::Odd), 15.0);                                                // :15 is odd and starts in 0.1 s
    CHECK_EQ(at(14.9, Parity::Odd, 1.0), 45.0);                                           // needs 1 s of preparation: the next odd one is :45
    CHECK_EQ(at(14.9, Parity::Even), 30.0); CHECK_EQ(at(0.0, Parity::Even), 0.0);         // a slot starting exactly now qualifies with no lead
    CHECK_EQ(at(0.0, Parity::Even, 0.5), 30.0); CHECK_EQ(at(15.0, Parity::Odd, 0.0), 15.0);
    CHECK_EQ(at(15.0, Parity::Odd, 15.0), 30.0 + 15.0);                                   // exactly 15 s of lead from :15 -> :30 is even, so :45
    CHECK_EQ(at(15.0, Parity::Even, 15.0), 30.0);
    CHECK_THROWS(next_slot_with_parity(t0, DigiMode::FT8, Parity::Odd, -1.0), std::invalid_argument);
}
TEST(slot_search_agrees_with_brute_force_everywhere) {
    std::mt19937_64 g(7); std::uniform_real_distribution<double> d(1.7e9, 1.8e9), lead(0.0, 40.0);
    for (int i = 0; i < 20000; ++i) {
        const double utc = d(g); const DigiMode m = i % 2 ? DigiMode::FT8 : DigiMode::FT4; const Parity p = (i / 2) % 2 ? Parity::Odd : Parity::Even; const double L = lead(g), len = slot_seconds(m);
        const Slot c = slot_containing(utc, m); CHECK(c.start_utc <= utc && utc < c.start_utc + len);
        CHECK((sec_in_minute(c.start_utc) / len == std::floor(sec_in_minute(c.start_utc) / len)) );   // every slot starts on a multiple of its length within the minute
        const Slot n = next_slot_with_parity(utc, m, p, L);
        CHECK(n.parity == p); CHECK(n.start_utc >= utc + L);                               // far enough away, right parity
        CHECK(n.start_utc - 2 * len < utc + L);                                            // and the first such slot (the one two slots earlier would be too early)
        CHECK(n.index % 2 == (p == Parity::Even ? 0 : 1));
        // minute-relative parity (what operators mean) equals the index parity
        const long long within = (long long)std::llround(sec_in_minute(n.start_utc) / len); CHECK_EQ((within % 2 == 0) ? 0 : 1, int(n.parity == Parity::Odd));
    }
}
TEST(plan_tx_on_time_late_and_next) {
    const double t0 = utc_of(2026, 11, 15, 12, 34, 0); const double off = 0.5; const DigiMode m = DigiMode::FT8;
    auto early = plan_tx(t0 + 0.1, m, Parity::Even, off);                                   // in the right slot, audio due in 0.4 s
    CHECK_EQ(early.slot.start_utc, t0); CHECK_NEAR(early.audio_start_utc, t0 + 0.5, 1e-9); CHECK_EQ(early.skip_s, 0.0);
    auto wrong = plan_tx(t0 + 0.1, m, Parity::Odd, off);                                    // the other parity: wait for :15
    CHECK_EQ(wrong.slot.start_utc, t0 + 15); CHECK_NEAR(wrong.audio_start_utc, t0 + 15.5, 1e-9);
    auto missed = plan_tx(t0 + 1.0, m, Parity::Even, off);                                  // 0.5 s late, joining late not allowed: next even slot
    CHECK_EQ(missed.slot.start_utc, t0 + 30);
    auto late = plan_tx(t0 + 1.0, m, Parity::Even, off, 1.0);                               // 0.5 s late and allowed: join now, 0.5 s of audio skipped
    CHECK_EQ(late.slot.start_utc, t0); CHECK_NEAR(late.audio_start_utc, t0 + 1.0, 1e-9); CHECK_NEAR(late.skip_s, 0.5, 1e-9);
    auto too_late = plan_tx(t0 + 2.0, m, Parity::Even, off, 1.0); CHECK_EQ(too_late.slot.start_utc, t0 + 30); CHECK_EQ(too_late.skip_s, 0.0);
    auto no_prep = plan_tx(t0 + 0.1, m, Parity::Even, off, 0.0, 1.0);                       // 0.4 s to go but 1 s needed to prepare: skip to the next even slot
    CHECK_EQ(no_prep.slot.start_utc, t0 + 30);
    auto prep_ok = plan_tx(t0 + 0.1, m, Parity::Even, off, 0.0, 0.3); CHECK_EQ(prep_ok.slot.start_utc, t0);
    // the audio never starts before now + lead, never in the wrong parity, and a late join never skips more than allowed
    std::mt19937_64 g(3); std::uniform_real_distribution<double> d(1.7e9, 1.8e9), small(0.0, 3.0);
    for (int i = 0; i < 20000; ++i) {
        const double utc = d(g), ml = small(g), lead = small(g); const DigiMode mm = i % 2 ? DigiMode::FT8 : DigiMode::FT4; const Parity p = (i / 2) % 2 ? Parity::Odd : Parity::Even;
        const TxPlan q = plan_tx(utc, mm, p, 0.5, ml, lead);
        CHECK(q.slot.parity == p); CHECK(q.skip_s >= 0 && q.skip_s <= ml + 1e-9);
        CHECK(q.audio_start_utc >= utc + lead - 1e-6);                                    // never before the caller is ready, late join or not
        if (q.skip_s > 0) { CHECK_NEAR(q.audio_start_utc, utc + lead, 1e-6); CHECK_NEAR(q.audio_start_utc - q.skip_s, q.slot.start_utc + 0.5, 1e-6); CHECK(q.skip_s < signal_seconds(mm)); }
        CHECK(q.audio_start_utc - q.skip_s >= q.slot.start_utc + 0.5 - 1e-6 - 1e-9 && q.audio_start_utc <= q.slot.start_utc + slot_seconds(mm) + 1e-6);
    }
}
TEST(slots_reject_bad_input) {
    CHECK_THROWS(slot_containing(std::nan(""), DigiMode::FT8), std::invalid_argument); CHECK_THROWS(next_slot(INFINITY, DigiMode::FT4), std::invalid_argument);
    CHECK_THROWS(plan_tx(1.7e9, DigiMode::FT8, Parity::Even, -0.5), std::invalid_argument); CHECK_THROWS(plan_tx(1.7e9, DigiMode::FT8, Parity::Even, 0.5, -1), std::invalid_argument);
}
TEST(signal_length_and_start_offset_match_the_real_generators) {
    // the generators place the signal dt into the slot; its last non-zero sample must land at dt + signal_seconds, and fit the slot
    for (DigiMode m : {DigiMode::FT8, DigiMode::FT4}) {
        const double dt = default_start_offset_s(m);
        const auto a = m == DigiMode::FT8 ? decode::synth_ft8("CQ M0OML IO91", 1000, 48000, 0.3, dt) : decode::synth_ft4("CQ M0OML IO91", 1000, 48000, 0.3, dt);
        CHECK_EQ(a.size(), size_t(slot_seconds(m) * 48000));
        size_t first = 0, last = 0; for (size_t i = 0; i < a.size(); ++i) if (a[i] != 0.0f) { if (!first) first = i; last = i; }
        CHECK_NEAR(double(first) / 48000, dt, 0.01); CHECK_NEAR(double(last) / 48000, dt + signal_seconds(m), 0.01);
        CHECK(double(last) / 48000 < slot_seconds(m));
    }
}

TEST(plan_tx_late_join_honours_the_preparation_time) {
    // review fix: a late join used to start "now" even when the caller needs time to prepare
    const double t0 = 1.7e9 - std::fmod(1.7e9, 30.0);                                       // a 30 s boundary: an even FT8 slot
    CHECK(slot_containing(t0, DigiMode::FT8).parity == Parity::Even);
    const auto q = plan_tx(t0 + 0.7, DigiMode::FT8, Parity::Even, 0.5, 2.0, 0.5);         // 0.2 s late, needs 0.5 s: start at +1.2, skipping 0.7 s
    CHECK_EQ(q.slot.start_utc, t0); CHECK_NEAR(q.audio_start_utc, t0 + 1.2, 1e-6); CHECK_NEAR(q.skip_s, 0.7, 1e-6);
    const auto r = plan_tx(t0 + 0.1, DigiMode::FT8, Parity::Even, 0.5, 0.0, 0.5);         // ready at +0.6, due at +0.5, late joins off: next even slot
    CHECK_EQ(r.slot.start_utc, t0 + 30); CHECK_EQ(r.skip_s, 0.0); CHECK_NEAR(r.audio_start_utc, t0 + 30.5, 1e-6);
    const auto s2 = plan_tx(t0 + 13.5, DigiMode::FT8, Parity::Even, 0.5, 20.0);           // 13 s late: the whole 12.64 s signal is past, so refused even though 20 s is allowed
    CHECK_EQ(s2.slot.start_utc, t0 + 30);
}
TEST(slots_reject_out_of_range_times_and_offsets) {
    CHECK_THROWS(slot_containing(-1.0, DigiMode::FT8), std::invalid_argument); CHECK_THROWS(slot_containing(1e300, DigiMode::FT8), std::invalid_argument);
    CHECK_THROWS(plan_tx(1.7e9, DigiMode::FT4, Parity::Even, 3.0), std::invalid_argument);   // 3 s + 5.04 s does not fit 7.5 s
    CHECK_THROWS(plan_tx(1.7e9, DigiMode::FT8, Parity::Even, 0.5, 0, -1), std::invalid_argument);
}
