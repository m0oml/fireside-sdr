// Direct FireWire stream (fw_iso.h): the three-register writer, the AM824 decoder (48 kHz and 192 kHz dual-wire, as measured on the radio)
// and the threaded IPcm, all against fakes. The real cdev receiver is exercised on the hardware (analysis/audio-stream-layout.md).
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include "fireside/fw_iso.h"
#include "fireside/pal.h"
#include "test_util.h"
using namespace fireside;

namespace {
constexpr uint64_t G = kDiceBase + 0x28, T = kDiceBase + 0x1A4;            // the radio's section offsets (10 and 105 quadlets)
constexpr uint64_t kClock = G + kDiceGlobalClockSelect, kEnable = G + kDiceGlobalEnable, kTxIso = T + kDiceTxStream0 + kDiceTxIso;

// A DICE register file like the radio's: writing CLOCK_SELECT re-locks STATUS at the new rate (unless told not to).
struct RegBus : IBus {
    std::map<uint64_t, uint32_t> r; std::vector<std::pair<uint64_t, uint32_t>> writes; bool lock = true; uint64_t fail_write_at = 0;
    RegBus() {
        r[kDiceBase] = 0x28 / 4; r[kDiceBase + 4] = 0x17C / 4; r[kDiceBase + 8] = 0x1A4 / 4;
        r[kClock] = 0x020C; r[kEnable] = 0; r[G + kDiceGlobalStatus] = 0x201; r[G + kDiceGlobalSampleRate] = 48000; r[G + kDiceGlobalClockCaps] = 0x10000054;
        r[kTxIso] = kDiceNoChannel; r[T + kDiceTxStream0 + kDiceTxAudio] = 8; r[T + kDiceTxStream0 + kDiceTxMidi] = 1;
    }
    std::vector<uint8_t> read(uint64_t a, size_t) override {
        auto it = r.find(a); if (it == r.end()) throw FwError("fake DICE: address error");
        const uint32_t v = it->second; return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    }
    void write(uint64_t a, const std::vector<uint8_t>& d) override {
        CHECK_EQ(d.size(), size_t(4));
        if (a == fail_write_at) throw FwError("fake DICE: write failed");
        const uint32_t v = uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
        writes.emplace_back(a, v); r[a] = v;
        if (a == kClock && lock) r[G + kDiceGlobalStatus] = (v & 0xF00) | 1;
        if (a == kClock && !lock) r[G + kDiceGlobalStatus] = 0x200;          // unlocked
    }
};
using W = std::vector<std::pair<uint64_t, uint32_t>>;
void nosleep(double) {}

// One CIP/AM824 packet as the radio sends it. samples[block][slot] are 24-bit values; the MIDI slot (last) is label 0x80.
std::vector<uint8_t> pkt(uint32_t dbc, uint32_t dbs, uint32_t fdf, const std::vector<std::vector<int32_t>>& blocks, uint32_t fmt = 0x10) {
    std::vector<uint8_t> o; auto put = [&](uint32_t w) { for (int i = 3; i >= 0; --i) o.push_back(uint8_t(w >> (8 * i))); };
    put((1u << 24) | (dbs << 16) | (dbc & 0xFF)); put((2u << 30) | (fmt << 24) | (fdf << 16) | 0xFFFF);
    for (auto& b : blocks) { for (size_t s = 0; s + 1 < dbs; ++s) put(0x40000000u | (uint32_t(s < b.size() ? b[s] : 0) & 0xFFFFFF)); put(0x80000000u); }
    return o;
}
std::vector<std::vector<int32_t>> ramp_blocks(int nb, int dbs, int start) {
    std::vector<std::vector<int32_t>> b(size_t(nb), std::vector<int32_t>(size_t(dbs - 1)));
    for (int i = 0; i < nb; ++i) for (int s = 0; s < dbs - 1; ++s) b[size_t(i)][size_t(s)] = (start + i) * 100 + s;
    return b;
}
}  // namespace

TEST(fw_iso_rate_helpers) {
    CHECK_EQ(dice_rate_index(48000), 2); CHECK_EQ(dice_rate_index(96000), 4); CHECK_EQ(dice_rate_index(192000), 6); CHECK_EQ(dice_rate_index(44100), -1);
    CHECK(dice_supported_rates(0x10000054) == (std::vector<unsigned>{48000, 96000, 192000}));
    CHECK(dice_supported_rates(0x04) == (std::vector<unsigned>{48000}));
    CHECK(!dice_dual_wire(96000) && dice_dual_wire(192000));
    CHECK_EQ(amdtp_max_payload(48000, 8), size_t(296)); CHECK_EQ(amdtp_max_payload(192000, 8), size_t(1096)); CHECK_EQ(amdtp_max_payload(96000, 8), size_t(584));
    CHECK_EQ(iso_bandwidth_units(1096), 1108u);
}

TEST(fw_iso_dice_start_writes_exactly_three_registers_and_stop_restores_them) {
    RegBus b; DiceStreamControl d(b, nosleep);
    const auto s = d.read_state(); CHECK_EQ(s.global, G); CHECK_EQ(s.tx, T); CHECK_EQ(s.tx_audio, 8u); CHECK(s.locked()); CHECK_EQ(s.status_rate_index(), 2);
    d.start(192000, 5); CHECK(d.active());
    CHECK(b.writes == (W{{kClock, 0x060C}, {kTxIso, 5}, {kEnable, 1}}));       // rate 192k keeping the internal source, our channel, enable
    d.stop(); CHECK(!d.active()); CHECK(d.last_stop_note().empty());
    CHECK(b.writes == (W{{kClock, 0x060C}, {kTxIso, 5}, {kEnable, 1}, {kEnable, 0}, {kTxIso, kDiceNoChannel}, {kClock, 0x020C}}));
    d.stop(); CHECK_EQ(b.writes.size(), size_t(6));                             // idempotent
    for (auto& w : b.writes) CHECK(w.first == kClock || w.first == kTxIso || w.first == kEnable);
}
TEST(fw_iso_dice_refuses_before_any_write) {
    { RegBus b; DiceStreamControl d(b, nosleep); CHECK_THROWS(d.start(44100, 5), std::invalid_argument); CHECK_THROWS(d.start(192000, 63), std::invalid_argument); CHECK_THROWS(d.start(48000, -1), std::invalid_argument); CHECK(b.writes.empty()); }
    { RegBus b; b.r[G + kDiceGlobalClockCaps] = 0x10000004; DiceStreamControl d(b, nosleep); CHECK_THROWS(d.start(192000, 5), std::invalid_argument); CHECK(b.writes.empty()); }
    { RegBus b; b.r[kEnable] = 1; DiceStreamControl d(b, nosleep); CHECK_THROWS(d.start(192000, 5), IoError); CHECK(b.writes.empty()); }   // snd-dice is streaming
    { RegBus b; b.r[T + kDiceTxStream0 + kDiceTxAudio] = 0; DiceStreamControl d(b, nosleep); CHECK_THROWS(d.start(48000, 5), IoError); CHECK(b.writes.empty()); }
}
TEST(fw_iso_dice_no_lock_restores_the_clock_and_never_enables) {
    RegBus b; b.lock = false; int sleeps = 0; DiceStreamControl d(b, [&](double) { ++sleeps; });
    CHECK_THROWS(d.start(192000, 5), IoError);
    CHECK(b.writes == (W{{kClock, 0x060C}, {kClock, 0x020C}})); CHECK_EQ(sleeps, 40); CHECK(!d.active());   // 1 s waiting to lock, 1 s waiting to re-lock
    CHECK(d.last_stop_note().find("re-locked") != std::string::npos);
}
TEST(fw_iso_dice_failed_enable_write_still_restores) {
    RegBus b; b.fail_write_at = kEnable; DiceStreamControl d(b, nosleep);
    CHECK_THROWS(d.start(192000, 5), FwError);
    CHECK_EQ(b.r[kTxIso], kDiceNoChannel); CHECK_EQ(b.r[kClock], 0x020Cu);
}
TEST(fw_iso_dice_leaves_a_stream_another_program_took_over_alone) {
    RegBus b; { DiceStreamControl d(b, nosleep); d.start(192000, 5); b.r[kTxIso] = 2; const size_t n = b.writes.size(); d.stop(); CHECK_EQ(b.writes.size(), n); CHECK(d.last_stop_note().find("another program") != std::string::npos); }
}
TEST(fw_iso_dice_destructor_restores) {
    RegBus b; { DiceStreamControl d(b, nosleep); d.start(96000, 3); }
    CHECK_EQ(b.r[kEnable], 0u); CHECK_EQ(b.r[kTxIso], kDiceNoChannel); CHECK_EQ(b.r[kClock], 0x020Cu);
}

TEST(fw_iso_decoder_48k_frames_dbc_and_s32_scaling) {
    AmdtpDecoder dec(8, false); std::vector<int32_t> out;
    CHECK_EQ(dec.expected_dbs(), 9u);
    auto e = pkt(0, 9, 0x02, {}); dec.packet(e.data(), e.size(), 100, out);                  // empty packet first (DBC of the next data packet)
    auto a = pkt(0, 9, 0x02, ramp_blocks(8, 9, 0)); dec.packet(a.data(), a.size(), 101, out);
    auto b = pkt(8, 9, 0x02, ramp_blocks(8, 9, 8)); dec.packet(b.data(), b.size(), 102, out);
    CHECK_EQ(out.size(), size_t(16 * 8)); CHECK_EQ(dec.stats().frames, 16u); CHECK_EQ(dec.stats().empty, 1u); CHECK_EQ(dec.stats().dbc_breaks, 0u); CHECK_EQ(dec.stats().cycle_gaps, 0u);
    CHECK_EQ(out[0], 0 << 8); CHECK_EQ(out[7], 7 << 8); CHECK_EQ(out[8 * 9 + 3], (9 * 100 + 3) << 8);   // frame 9, channel 3; MIDI slot not delivered
    std::vector<int32_t> neg; AmdtpDecoder d2(8, false); auto n = pkt(0, 9, 0x02, {{-1, -8388608, 8388607}}); d2.packet(n.data(), n.size(), -1, neg);
    CHECK_EQ(neg[0], -256); CHECK_EQ(neg[1], INT32_MIN); CHECK_EQ(neg[2], 8388607 * 256);             // 24-bit sign carried into S32
    CHECK_NEAR(s32_to_float(neg[1]), -1.0, 1e-9);
}
TEST(fw_iso_decoder_192k_dual_wire_slot_order) {
    // A block carries two sample times: slot 2c = channel c at t, slot 2c+1 = channel c at t+1 (verified on air: I/Q image 43 dB down).
    AmdtpDecoder dec(8, true); CHECK_EQ(dec.expected_dbs(), 17u);
    std::vector<std::vector<int32_t>> blocks(16, std::vector<int32_t>(16));
    for (int bk = 0; bk < 16; ++bk) for (int c = 0; c < 8; ++c) for (int t = 0; t < 2; ++t) blocks[size_t(bk)][size_t(2 * c + t)] = (2 * bk + t) * 10 + c;
    std::vector<int32_t> out; auto p = pkt(0, 17, 0x04, blocks); CHECK_EQ(p.size(), size_t(1096));
    dec.packet(p.data(), p.size(), 0, out);
    CHECK_EQ(dec.stats().frames, 32u); CHECK_EQ(out.size(), size_t(32 * 8));
    for (int f = 0; f < 32; ++f) for (int c = 0; c < 8; ++c) CHECK_EQ(out[size_t(f * 8 + c)], (f * 10 + c) << 8);
}
TEST(fw_iso_decoder_counts_dbc_breaks_and_lost_cycles) {
    AmdtpDecoder dec(8, false); std::vector<int32_t> out;
    auto a = pkt(0, 9, 2, ramp_blocks(8, 9, 0)), b = pkt(24, 9, 2, ramp_blocks(8, 9, 0)), c = pkt(32, 9, 2, ramp_blocks(8, 9, 0));
    dec.packet(a.data(), a.size(), 7998, out); dec.packet(b.data(), b.size(), 1, out); dec.packet(c.data(), c.size(), 2, out);   // 7999 and 0 missing
    CHECK_EQ(dec.stats().dbc_breaks, 1u); CHECK_EQ(dec.stats().cycle_gaps, 1u); CHECK_EQ(dec.stats().lost_cycles, 2u);
    AmdtpDecoder w(8, false); w.packet(a.data(), a.size(), 7999, out); auto a2 = pkt(8, 9, 2, ramp_blocks(8, 9, 0)); w.packet(a2.data(), a2.size(), 0, out);
    CHECK_EQ(w.stats().cycle_gaps, 0u);                                                          // 7999 -> 0 is the normal wrap
}
TEST(fw_iso_decoder_rejects_what_it_cannot_trust) {
    AmdtpDecoder dec(8, false); std::vector<int32_t> out;
    auto wrong_dbs = pkt(0, 17, 4, ramp_blocks(2, 17, 0)); dec.packet(wrong_dbs.data(), wrong_dbs.size(), -1, out);   // a 192k packet at 48k
    auto mute = pkt(0, 9, 2, ramp_blocks(2, 9, 0), 0x00); dec.packet(mute.data(), mute.size(), -1, out);              // not AM824
    uint8_t shortp[4] = {0}; dec.packet(shortp, 4, -1, out);
    auto partial = pkt(0, 9, 2, ramp_blocks(2, 9, 0)); partial.resize(partial.size() - 4); dec.packet(partial.data(), partial.size(), -1, out);
    CHECK_EQ(dec.stats().rejected, 4u); CHECK(out.empty());
    auto nodata = pkt(0, 9, 0xFF, {}); dec.packet(nodata.data(), nodata.size(), -1, out); CHECK_EQ(dec.stats().empty, 1u);
}

namespace {
// Fake receiver: emits radio-like 192k dual-wire packets (3 data + 1 empty per 4 cycles), I = ramp, Q = -ramp.
struct FakeRx : IIsoReceiver {
    std::vector<std::string>* log; std::atomic<bool> started{false}; std::atomic<int> stall{0}; std::atomic<bool> fail{false}; int cyc = 0, dbc = 0, t = 0; bool dual;
    explicit FakeRx(std::vector<std::string>* l, bool dual_ = true) : log(l), dual(dual_) {}
    int channel() const override { return 7; }
    void start() override { log->push_back("rx start"); started = true; }
    size_t poll(double, const IsoPacketFn& fn) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (fail) throw FwError("FireWire bus reset: the direct stream stopped (start it again)");
        if (stall) return 0;
        for (int i = 0; i < 32; ++i, ++cyc) {
            const int dbs = dual ? 17 : 9, nb = dual ? 16 : 8, per = dual ? 2 : 1;
            std::vector<uint8_t> p;
            if (cyc % 4 == 3) p = pkt(uint32_t(dbc), uint32_t(dbs), dual ? 4 : 2, {});
            else {
                std::vector<std::vector<int32_t>> b(size_t(nb), std::vector<int32_t>(size_t(dbs - 1)));
                for (int k = 0; k < nb; ++k) for (int s = 0; s < per; ++s) { const int v = (t++ % 1000); b[size_t(k)][size_t(dual ? 0 + s : 0)] = v; b[size_t(k)][size_t(dual ? 2 + s : 1)] = -v; }
                p = pkt(uint32_t(dbc), uint32_t(dbs), dual ? 4 : 2, b); dbc = (dbc + nb) & 0xFF;
            }
            fn(p.data(), p.size(), uint32_t(cyc % 8000));
        }
        return 32;
    }
    void stop() noexcept override { log->push_back("rx stop"); }
    std::string describe() const override { return "fake iso"; }
};
struct LoggingBus : RegBus {
    std::vector<std::string>* log; explicit LoggingBus(std::vector<std::string>* l) : log(l) {}
    void write(uint64_t a, const std::vector<uint8_t>& d) override { RegBus::write(a, d); log->push_back(a == kEnable ? (d[3] ? "enable" : "disable") : a == kClock ? "clock" : "iso"); }
};
}  // namespace

TEST(fw_iso_pcm_delivers_ordered_frames_and_restores_the_radio) {
    std::vector<std::string> log; LoggingBus b(&log);
    {
        FwIsoPcm pcm(b, std::make_unique<FakeRx>(&log), 192000, {}, nosleep);
        CHECK_EQ(pcm.channels(), 8); CHECK_EQ(pcm.rate(), 192000.0);
        CHECK(log == (std::vector<std::string>{"rx start", "clock", "iso", "enable"}));      // listening before the radio is told to send
        CHECK_EQ(b.r[kTxIso], 7u);
        std::vector<int32_t> buf(8 * 5000); size_t got = 0;
        for (int i = 0; i < 200 && got < 5000; ++i) got += pcm.read(buf.data() + got * 8, 5000 - got, 0.1);
        CHECK_EQ(got, size_t(5000));
        for (size_t f = 0; f < got; ++f) { CHECK_EQ(buf[f * 8], int32_t(f % 1000) << 8); CHECK_EQ(buf[f * 8 + 1], -(int32_t(f % 1000) << 8)); }
        CHECK_EQ(pcm.stats().dbc_breaks, 0u); CHECK_EQ(pcm.stats().rejected, 0u);
        CHECK(pcm.describe().find("192000") != std::string::npos);
    }
    CHECK_EQ(log.back(), std::string("clock")); CHECK_EQ(b.r[kEnable], 0u); CHECK_EQ(b.r[kTxIso], kDiceNoChannel); CHECK_EQ(b.r[kClock], 0x020Cu);
    const auto stop = std::find(log.begin(), log.end(), "rx stop"), dis = std::find(log.begin(), log.end(), "disable");
    CHECK(stop != log.end() && dis != log.end() && stop < dis);                                 // stop listening, then stop the radio
}
TEST(fw_iso_pcm_through_pcm_iq_source_gives_i_and_q) {
    std::vector<std::string> log; RegBus b;
    PcmIqSource src(std::make_unique<FwIsoPcm>(b, std::make_unique<FakeRx>(&log, false), 48000, FwIsoOptions{}, nosleep), 0, 1, 1024);
    CHECK_EQ(src.sample_rate(), 48000.0); CHECK(src.live());
    std::vector<dsp::cf> x; for (int i = 0; i < 100 && x.size() < 3000; ++i) src.read(x, 3000 - x.size());
    CHECK(x.size() >= 3000);
    for (size_t k = 1; k < 3000; ++k) { CHECK_NEAR(x[k].real(), double(int32_t(k % 1000) << 8) / 2147483648.0, 1e-9); CHECK_NEAR(x[k].imag(), -x[k].real(), 1e-9); }
}
TEST(fw_iso_pcm_reports_a_bus_reset_and_a_stall) {
    std::vector<std::string> log; RegBus b;
    { auto rx = std::make_unique<FakeRx>(&log); FakeRx* r = rx.get(); FwIsoPcm pcm(b, std::move(rx), 192000, {}, nosleep);
      std::vector<int32_t> buf(8 * 100000); r->fail = true; bool threw = false;
      for (int i = 0; i < 100 && !threw; ++i) { try { pcm.read(buf.data(), 100000, 0.05); } catch (const IoError& e) { threw = std::string(e.what()).find("bus reset") != std::string::npos; } }
      CHECK(threw); }
    CHECK_EQ(b.r[kEnable], 0u);                                                                 // still restored
    { FwIsoOptions o; o.stall_seconds = 0.2; auto rx = std::make_unique<FakeRx>(&log); rx->stall = 1; FwIsoPcm pcm(b, std::move(rx), 48000, o, nosleep);
      std::vector<int32_t> buf(8 * 16); bool threw = false; const auto t0 = std::chrono::steady_clock::now();
      while (!threw && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) { try { pcm.read(buf.data(), 16, 0.05); } catch (const IoError& e) { threw = std::string(e.what()).find("no data") != std::string::npos; } }
      CHECK(threw); }
}
TEST(fw_iso_pcm_refuses_when_the_radio_is_busy_and_writes_nothing) {
    std::vector<std::string> log; RegBus b; b.r[kEnable] = 1;
    CHECK_THROWS(FwIsoPcm(b, std::make_unique<FakeRx>(&log), 192000, {}, nosleep), IoError);
    CHECK(b.writes.empty()); CHECK(log == (std::vector<std::string>{"rx start", "rx stop"}));
    CHECK_THROWS(FwIsoPcm(b, std::make_unique<FakeRx>(&log), 44100, {}, nosleep), std::invalid_argument);
}
TEST(fw_iso_pcm_counts_overflow_when_the_reader_falls_behind) {
    std::vector<std::string> log; RegBus b; FwIsoOptions o; o.ring_seconds = 0.05;
    FwIsoPcm pcm(b, std::make_unique<FakeRx>(&log), 192000, o, nosleep);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));                                // the fake produces far faster than real time
    CHECK(pcm.xruns() > 0);
    std::vector<int32_t> buf(8 * 64); CHECK(pcm.read(buf.data(), 64, 0.1) > 0);
}

// ---------------------------------------------------------------- stress (run under ASan / TSan: see HANDOFF)
TEST(fw_iso_pcm_open_close_many_times_with_a_slow_and_a_fast_reader) {
    std::vector<std::string> log; RegBus b;
    for (int round = 0; round < 12; ++round) {
        FwIsoOptions o; o.ring_seconds = (round % 2) ? 0.05 : 1.0;                         // a tiny ring overflows constantly, a big one does not
        FwIsoPcm pcm(b, std::make_unique<FakeRx>(&log, round % 3 != 0), (round % 3 != 0) ? 192000u : 48000u, o, nosleep);
        std::vector<int32_t> buf(8 * 4096); size_t got = 0;
        for (int i = 0; i < 20; ++i) {
            got += pcm.read(buf.data(), (round % 4 == 0) ? 7 : 4096, 0.02);                // odd sizes, tiny reads, timeouts
            if (round % 5 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(15)); // a slow reader
        }
        CHECK(got > 0); if (round % 2 == 0) pcm.close();                                  // explicit close on some, the destructor on the rest
    }
    CHECK_EQ(b.r[kEnable], 0u); CHECK_EQ(b.r[kTxIso], kDiceNoChannel); CHECK_EQ(b.r[kClock], 0x020Cu);   // every round restored the radio
}
TEST(fw_iso_pcm_reading_from_two_threads_while_it_is_closed_is_safe) {
    std::vector<std::string> log; RegBus b; auto pcm = std::make_unique<FwIsoPcm>(b, std::make_unique<FakeRx>(&log), 192000u, FwIsoOptions{}, nosleep);
    std::atomic<bool> stop{false}; std::atomic<size_t> total{0};
    auto reader = [&] { std::vector<int32_t> buf(8 * 512); while (!stop) { try { total += pcm->read(buf.data(), 512, 0.005); } catch (const IoError&) { break; } } };
    std::thread t1(reader), t2(reader); std::this_thread::sleep_for(std::chrono::milliseconds(150)); pcm->close(); stop = true; t1.join(); t2.join();
    CHECK(total > 0); CHECK_EQ(b.r[kEnable], 0u); pcm.reset();
}

// ---------------------------------------------------------------- duplex: the transmit side (analysis/duplex-iso-tx.md)
namespace {
constexpr uint64_t R = kDiceBase + 0x3DC, kRxIso = R + kDiceRxStream0 + kDiceRxIso;   // the radio's RX section (read 2026-11-19)
struct DuplexBus : LoggingBus {
    explicit DuplexBus(std::vector<std::string>* l, uint32_t rx_audio = 8) : LoggingBus(l) {
        r[kDiceBase + 16] = 0x3DC / 4; r[R] = 1; r[R + 4] = 280 / 4; r[kRxIso] = kDiceNoChannel; r[R + kDiceRxStream0 + 4] = 0; r[R + kDiceRxStream0 + kDiceRxAudio] = rx_audio; r[R + kDiceRxStream0 + kDiceRxMidi] = 1;
    }
};
uint32_t be_q(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
struct TxLog { struct P { std::vector<uint8_t> h, pl; }; std::mutex m; std::vector<P> got; int start15 = -1; std::atomic<int> starts{0}; std::atomic<bool> stopped{false};
               size_t count() { std::lock_guard<std::mutex> l(m); return got.size(); } };
struct FakeTx : IIsoTransmitter {                                                   // records into a TxLog that outlives it (the stream owns the transmitter)
    TxLog* log; explicit FakeTx(TxLog* l) : log(l) {}
    int channel() const override { return 9; }
    bool queue(const uint8_t h[8], const uint8_t* pl, size_t n) override { std::lock_guard<std::mutex> l(log->m); log->got.push_back({std::vector<uint8_t>(h, h + 8), std::vector<uint8_t>(pl, pl + n)}); return true; }
    void start(int c) override { log->start15 = c; ++log->starts; }
    void reap() override {}
    size_t queued() const override { return 0; }
    void stop() noexcept override { log->stopped = true; }
    std::string describe() const override { return "fake tx"; }
};
}  // namespace
TEST(fw_iso_dice_duplex_points_rx_before_enable_and_restores_it) {
    std::vector<std::string> log; DuplexBus b(&log);
    { DiceStreamControl d(b, nosleep); d.start(48000, 7, 9);
      CHECK(log == (std::vector<std::string>{"clock", "iso", "iso", "enable"})); CHECK_EQ(b.r[kTxIso], 7u); CHECK_EQ(b.r[kRxIso], 9u); CHECK_EQ(d.rx_audio(), 8u); }
    CHECK_EQ(b.r[kEnable], 0u); CHECK_EQ(b.r[kRxIso], kDiceNoChannel); CHECK_EQ(b.r[kTxIso], kDiceNoChannel); CHECK_EQ(b.r[kClock], 0x020Cu);
    const auto dis = std::find(log.begin(), log.end(), "disable"); CHECK(dis != log.end() && dis == log.begin() + 4);   // disabled first, then the channels
    std::vector<std::string> l2; DuplexBus bad(&l2, 0); DiceStreamControl d2(bad, nosleep);
    CHECK_THROWS(d2.start(48000, 7, 9), IoError); CHECK(bad.writes.empty());          // refused before any write
    std::vector<std::string> l3; LoggingBus none(&l3); DiceStreamControl d3(none, nosleep);
    CHECK_THROWS(d3.start(48000, 7, 9), std::exception); CHECK(none.writes.empty());   // no RX section at all
}
TEST(fw_iso_duplex_sends_one_packet_per_received_one_with_the_written_samples) {
    std::vector<std::string> log; DuplexBus b(&log);
    TxLog txl; TxLog* t = &txl; auto tx = std::make_unique<FakeTx>(t);
    FwIsoOptions o; o.duplex = true; o.tx_lead_cycles = 128; o.out_ring_seconds = 1.0;
    {
        FwIsoPcm pcm(b, std::make_unique<FakeRx>(&log, false), 48000, o, nosleep, std::move(tx));
        CHECK(pcm.duplex()); CHECK_EQ(b.r[kRxIso], 9u); CHECK(fw_iso_duplex_running());
        auto out = open_fw_iso_out(); CHECK_EQ(out->channels(), 8); CHECK_EQ(out->rate(), 48000.0); CHECK(out->latency_s() > 0.016 && out->latency_s() < 1.1);
        std::vector<int32_t> ramp(8 * 20000, 0); for (int k = 0; k < 20000; ++k) { ramp[size_t(k) * 8] = int32_t(uint32_t(k + 1) << 8); ramp[size_t(k) * 8 + 1] = -int32_t(uint32_t(k + 1) << 8); }
        size_t w = 0; for (int i = 0; i < 2000 && w < 20000; ++i) { w += out->write(ramp.data() + w * 8, 20000 - w); if (w < 20000) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        CHECK_EQ(w, size_t(20000));
        for (int i = 0; i < 3000 && t->count() < 4000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pcm.close();
        CHECK_THROWS(out->write(ramp.data(), 1), IoError); CHECK(!fw_iso_duplex_running()); CHECK(t->stopped);
    }
    CHECK_EQ(b.r[kRxIso], kDiceNoChannel); CHECK_EQ(b.r[kEnable], 0u);
    CHECK_EQ(t->starts.load(), 1); CHECK_EQ(t->start15 & 0x1FFF, 128);                 // the first received cycle is 0: sent 128 cycles later
    std::lock_guard<std::mutex> l(t->m); CHECK(t->got.size() >= 4000);
    AmdtpDecoder dec(8, false); std::vector<int32_t> frames; int data = 0;
    for (size_t i = 0; i < t->got.size(); ++i) {
        const auto& p = t->got[i]; const uint32_t q0 = be_q(p.h.data()), q1 = be_q(p.h.data() + 4);
        CHECK_EQ((q0 >> 16) & 0xFF, 9u); CHECK_EQ((q1 >> 24) & 0x3F, 0x10u);
        const bool empty = p.pl.empty(); CHECK_EQ(empty, i % 4 == 3);                 // the radio's pattern replayed: 3 data packets, 1 empty
        if (!empty) { ++data; CHECK_EQ(p.pl.size(), size_t(8 * 9 * 4)); }
        std::vector<uint8_t> whole(p.h); whole.insert(whole.end(), p.pl.begin(), p.pl.end()); dec.packet(whole.data(), whole.size(), int((128 + i) % 8000), frames);
    }
    CHECK_EQ(dec.stats().dbc_breaks, 0u); CHECK_EQ(dec.stats().cycle_gaps, 0u); CHECK_EQ(dec.stats().rejected, 0u);   // a clean, continuous stream
    size_t at = 0; while (at < frames.size() / 8 && frames[at * 8] == 0) ++at;        // silence until our samples, then all of them in order
    CHECK(at < frames.size() / 8);
    size_t k = 0; for (; at + k < frames.size() / 8 && k < 20000; ++k) { if (frames[(at + k) * 8] != int32_t(uint32_t(k + 1) << 8) || frames[(at + k) * 8 + 1] != -int32_t(uint32_t(k + 1) << 8)) break; }
    CHECK(k == 20000 || at + k == frames.size() / 8);                                  // no sample lost, duplicated or reordered
    for (size_t f = 0; f < frames.size() / 8; ++f) for (int c = 2; c < 8; ++c) CHECK_EQ(frames[f * 8 + size_t(c)], 0);   // only ch0/1 carry anything
    CHECK(data > 0);
}
TEST(fw_iso_non_duplex_never_touches_the_rx_stream) {
    std::vector<std::string> log; DuplexBus b(&log);
    { FwIsoPcm pcm(b, std::make_unique<FakeRx>(&log, false), 48000, {}, nosleep); CHECK(!pcm.duplex()); CHECK(!fw_iso_duplex_running()); CHECK_THROWS(open_fw_iso_out(), IoError); }
    for (auto& w : b.writes) CHECK(w.first != kRxIso);
}
TEST(fw_iso_duplex_audio_never_reaches_the_transmit_channels_and_tx_never_the_audio_ones) {
    std::vector<std::string> log; DuplexBus b(&log); TxLog txl; FwIsoOptions o; o.duplex = true; o.out_ring_seconds = 1.0;
    {
        FwIsoPcm pcm(b, std::make_unique<FakeRx>(&log, false), 48000, o, nosleep, std::make_unique<FakeTx>(&txl));
        auto tx = open_fw_iso_out(FwIsoPcm::OutKind::Tx), au = open_fw_iso_out(FwIsoPcm::OutKind::Audio);
        // each writer fills ALL 8 channels: its own with (c+1) << 20, the others with a marker that must never come out
        std::vector<int32_t> txf(8 * 4000), auf(8 * 4000);
        for (size_t i = 0; i < txf.size(); ++i) { const int c = int(i % 8); txf[i] = c < 2 ? int32_t(uint32_t(c + 1) << 20) : int32_t(0x0A0000 << 8); auf[i] = c >= 2 ? int32_t(uint32_t(c + 1) << 20) : int32_t(0x0B0000 << 8); }
        size_t a = 0, t = 0; for (int i = 0; i < 2000 && (a < 4000 || t < 4000); ++i) { if (a < 4000) a += au->write(auf.data() + a * 8, 4000 - a); if (t < 4000) t += tx->write(txf.data() + t * 8, 4000 - t); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        for (int i = 0; i < 3000 && txl.count() < 3000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::lock_guard<std::mutex> l(txl.m); AmdtpDecoder dec(8, false); std::vector<int32_t> f;
    for (size_t i = 0; i < txl.got.size(); ++i) { std::vector<uint8_t> w(txl.got[i].h); w.insert(w.end(), txl.got[i].pl.begin(), txl.got[i].pl.end()); dec.packet(w.data(), w.size(), -1, f); }
    bool tx_seen = false, audio_seen = false;
    for (size_t k = 0; k < f.size() / 8; ++k) for (int c = 0; c < 8; ++c) { const int32_t v = f[k * 8 + size_t(c)]; if (!v) continue; CHECK_EQ(v, int32_t(uint32_t(c + 1) << 20)); if (c < 2) tx_seen = true; else audio_seen = true; }
    CHECK(tx_seen && audio_seen);                                                       // both arrive; no marker anywhere = no channel crossed over
}
TEST(fw_iso_duplex_input_tap_gives_an_input_channel_at_48k_and_fails_after_close) {
    std::vector<std::string> log; DuplexBus b(&log); TxLog txl; FwIsoOptions o; o.duplex = true;
    auto pcm = std::make_unique<FwIsoPcm>(b, std::make_unique<FakeRx>(&log, false), 48000, o, nosleep, std::make_unique<FakeTx>(&txl));
    auto tap = open_fw_iso_input(0); std::vector<float> x(4000); size_t got = 0;
    for (int i = 0; i < 200 && got < 4000; ++i) got += tap->read(x.data() + got, 4000 - got, 0.05);
    CHECK_EQ(got, size_t(4000));
    int ramp = 0; for (size_t i = 1; i < got; ++i) { const float d = x[i] - x[i - 1]; if (std::fabs(d - float(256.0 / 2147483648.0)) < 1e-9f) ++ramp; }
    CHECK(ramp > 3500);                                                                 // FakeRx puts a ramp on ch0: it arrives in order
    CHECK_THROWS(open_fw_iso_input(9), std::invalid_argument);
    pcm->close(); CHECK_THROWS(tap->read(x.data(), 10, 0.01), IoError);
}
TEST(fw_iso_dice_clears_a_stream_left_running_on_our_own_channel_only) {
    {   // left on channel 5 by a dead run; our receiver now holds channel 5: cleared, then started as usual
        RegBus b; b.r[kEnable] = 1; b.r[kTxIso] = 5; DiceStreamControl d(b, nosleep);
        d.start(192000, 5); CHECK(d.active()); CHECK(d.stale_note().find("channel 5") != std::string::npos);
        CHECK(b.writes.size() >= 2); CHECK(b.writes[0] == std::make_pair(kEnable, uint32_t(0))); CHECK(b.writes[1] == std::make_pair(kTxIso, kDiceNoChannel));
        d.stop(); CHECK_EQ(b.r[kEnable], 0u); CHECK_EQ(b.r[kTxIso], kDiceNoChannel);              // stop puts it back to idle, not to the stale state
    }
    {   // the radio sends on a channel we do not hold: someone else's stream, refused before any write
        RegBus b; b.r[kEnable] = 1; b.r[kTxIso] = 3; DiceStreamControl d(b, nosleep);
        CHECK_THROWS(d.start(192000, 5), IoError); CHECK(b.writes.empty()); CHECK(d.stale_note().empty());
    }
}
TEST(fw_iso_dice_duplex_clears_its_own_leftover_and_restores_to_idle) {
    {   // the 2026-11-19 16:06 state: ENABLE 1, TX iso 1, RX iso 2, nobody holding either channel; the new run gets 1 and 2 again
        std::vector<std::string> log; DuplexBus b(&log); b.r[kEnable] = 1; b.r[kTxIso] = 1; b.r[kRxIso] = 2;
        { DiceStreamControl d(b, nosleep); d.start(48000, 1, 2); CHECK(d.active()); CHECK(!d.stale_note().empty()); CHECK_EQ(b.r[kEnable], 1u); CHECK_EQ(b.r[kRxIso], 2u); }
        CHECK_EQ(b.r[kEnable], 0u); CHECK_EQ(b.r[kTxIso], kDiceNoChannel); CHECK_EQ(b.r[kRxIso], kDiceNoChannel);   // closed: idle, not the leftover
    }
    {   // receive-only start over a duplex leftover: it holds no transmit channel, so it cannot prove RX[0] is unused: refused, nothing written
        std::vector<std::string> log; DuplexBus b(&log); b.r[kEnable] = 1; b.r[kTxIso] = 1; b.r[kRxIso] = 2;
        DiceStreamControl d(b, nosleep); CHECK_THROWS(d.start(48000, 1), IoError); CHECK_EQ(b.r[kEnable], 1u); CHECK_EQ(b.r[kRxIso], 2u);
    }
    {   // duplex, but the radio's RX[0] is on a channel we do not hold: refused, nothing written
        std::vector<std::string> log; DuplexBus b(&log); b.r[kEnable] = 1; b.r[kTxIso] = 1; b.r[kRxIso] = 6;
        DiceStreamControl d(b, nosleep); CHECK_THROWS(d.start(48000, 1, 2), IoError); CHECK_EQ(b.r[kEnable], 1u); CHECK_EQ(b.r[kRxIso], 6u);
    }
}
TEST(fw_iso_duplex_close_wakes_a_waiting_tap_reader_and_waits_for_it) {   // the mic/TX thread reading while receive is stopped: no long wait, no freed stream
    std::vector<std::string> log; DuplexBus b(&log); TxLog txl; FwIsoOptions o; o.duplex = true;
    auto pcm = std::make_unique<FwIsoPcm>(b, std::make_unique<FakeRx>(&log, false), 48000, o, nosleep, std::make_unique<FakeTx>(&txl));
    auto tap = open_fw_iso_input(0); std::atomic<int> reads{0}; std::atomic<bool> threw{false};
    std::thread rd([&] { std::vector<float> x(64); try { for (;;) { tap->read(x.data(), x.size(), 5.0); ++reads; } } catch (const IoError&) { threw = true; } });
    for (int i = 0; i < 2000 && reads < 20; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(reads >= 20);
    const auto t0 = std::chrono::steady_clock::now(); pcm.reset(); rd.join();
    CHECK(threw); CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));         // woken by the close, not the 5 s timeout
}
TEST(fw_iso_duplex_a_stalled_stream_refuses_to_transmit) {   // the radio stops sending: an over must not key with nothing going out (it sent 0 W before)
    std::vector<std::string> log; DuplexBus b(&log); TxLog txl; FwIsoOptions o; o.duplex = true; o.stall_seconds = 0.2;
    auto rx = std::make_unique<FakeRx>(&log, false); FakeRx* r = rx.get();
    FwIsoPcm pcm(b, std::move(rx), 48000, o, nosleep, std::make_unique<FakeTx>(&txl));
    CHECK_EQ(o.tx_lead_cycles, 512); CHECK(FwIsoOptions{}.tx_lead_cycles == 512);
    auto out = open_fw_iso_out(); std::vector<int32_t> x(8 * 16, 0); out->write(x.data(), 16);   // fine while it runs
    r->stall = 1; for (int i = 0; i < 300 && !pcm.failed(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(pcm.failed());
    CHECK_THROWS(out->write(x.data(), 16), IoError);                                    // an over running now stops
    CHECK_THROWS(open_fw_iso_out(), IoError); CHECK_THROWS(open_fw_iso_input(0), IoError);   // and a new one is refused
    CHECK(fw_iso_duplex_running());                                                     // still published until closed: no fallback to the ALSA path
    pcm.close(); CHECK(!fw_iso_duplex_running());
}
