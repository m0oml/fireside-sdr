// The FLEX-5000's internal ATU (an AT200PC-type tuner behind the radio's ATU_SEND_CMD / ATU_GET_RESULT mailbox opcodes).
// Protocol from PowerSDR's FWC/fwc_atu.cs (GPL), checked on the radio:
//   send:  ATU_SEND_CMD(d1 = request << 8 | b2, d2 = b3)
//   reply: ATU_GET_RESULT(d1 = wait ms) -> [queue remaining][b3][b2][response code] (byte 3 .. byte 0); 0x00FFFFFF = nothing pending.
// What needs no RF (relays, thresholds, reports) goes through the safety gate's reviewed ATU values; a tune needs a carrier, so it only runs
// while the TxGate holds the key (tx_gate.h). Qt-free; time and sleep are injectable for the tests.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "fireside/client.h"

namespace fireside::atu {
enum Req : uint8_t {
    NOOP = 0, IND_UP = 1, IND_DN = 2, CAP_UP = 3, CAP_DN = 4, MEM_TUNE = 5, FULL_TUNE = 6, HIZ = 8, LOZ = 9, ANT1 = 10,
    ALL_UPDATE = 40, VERSION = 41, STANDBY = 44, ACTIVE = 45, MANUAL_STORE = 46,
    SWR11 = 50, SWR13 = 51, SWR15 = 52, SWR17 = 53, SWR20 = 54, SWR25 = 55, SWR30 = 56,
    AUTO_ON = 58, AUTO_OFF = 59, FWD_PWR = 60, REV_PWR = 61, SWR = 62, UPDATE_ON = 63, UPDATE_OFF = 64, SET_IND = 65, SET_CAP = 66,
};
enum Resp : uint8_t {
    R_NOOP = 0, R_IND = 1, R_CAP = 2, R_HILOZ = 3, R_ANTENNA = 4, R_FWDPWR = 5, R_SWR = 6, R_TXFREQ = 7, R_TUNEPASS = 9, R_TUNEFAIL = 10,
    R_VERSION = 11, R_CLEAR_DONE = 12, R_INSTANDBY = 13, R_ACTIVE = 14, R_STORE_OK = 15, R_SWRTHRESH = 16, R_AUTO_STATUS = 17, R_REVPWR = 18, R_UPDATE_STATUS = 19,
};
enum class Fail : uint8_t { NoRf = 0, CarrierLost = 1, SwrTooHigh = 2 };
const char* fail_text(uint8_t reason);

inline constexpr uint32_t send_d1(uint8_t req, uint8_t b2 = 0) { return uint32_t(req) << 8 | b2; }
struct Msg { uint8_t code = 0, b2 = 0, b3 = 0, remaining = 0; };
// nullopt when nothing is pending (PowerSDR's FlushBuffer test: bytes 0..2 all 0xFF).
std::optional<Msg> parse(uint32_t raw);

// The thresholds the tuner knows (requests 50..56 <-> report values 0..6).
inline constexpr double kThresholds[] = {1.1, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0};
std::optional<uint8_t> threshold_request(double swr);          // 1.5 -> SWR15; nullopt if not one of kThresholds

struct Status {
    std::optional<int> ind, cap, hiz, antenna;                   // relay settings as reported
    std::optional<double> fwd_w, rev_w, swr, threshold;          // the tuner's own meter (watts, SWR) and its active good-tune threshold
    std::optional<double> tx_freq_mhz; std::optional<bool> active, auto_on, updates_on;
    std::optional<bool> tune_pass; uint8_t fail_reason = 0;
    std::optional<double> version;
};
void apply(Status& s, const Msg& m);                             // port of fwc_atu.cs ParseResult

struct Clock { std::function<double()> now; std::function<void(double)> sleep; };
Clock real_clock();

class Atu {
public:
    // `fx` must allow writes; the ATU_SEND_CMD permit is taken around each send (only reviewed values pass the gate).
    explicit Atu(Flex& fx, Clock clock = real_clock()) : fx_(fx), clk_(std::move(clock)) {}
    const Status& status() const { return st_; }
    // One request. Refused by the safety gate unless it is a reviewed non-RF request, or a tune while the TxGate holds the key.
    void send(uint8_t req, uint8_t b2 = 0, uint8_t b3 = 0);
    // Reads one message (waits up to wait_ms in the radio); applies it. nullopt if nothing was pending.
    std::optional<Msg> poll(uint32_t wait_ms = 200);
    int drain(int max_msgs = 64);                                // read until the queue is empty; returns messages read
    // PowerSDR's start-up sequence (as PowerSDR does): auto tune off, standby, Lo-Z. No RF.
    void init();
    void set_threshold(double swr);                              // throws std::invalid_argument if not one of kThresholds
    // Put saved relay settings back without transmitting: inductor and capacitor values directly, then Hi/Lo-Z (SET_IND also sets the Hi/Lo-Z relay, so
    // it must come first: seen on the radio 2026-11-19), then ask for a report. Returns false if the report does not show the requested values.
    bool set_relays(int ind, int cap, bool hiz);
    void clear_meter() { st_.fwd_w.reset(); st_.rev_w.reset(); st_.swr.reset(); }   // before asking for fresh FWD_PWR / REV_PWR / SWR reports
    void request_update();                                       // ALL_UPDATE: the tuner reports its relays (no RF)

    enum class TuneResult { Pass, Fail, Timeout };
    struct TuneOutcome { TuneResult result = TuneResult::Timeout; uint8_t fail_reason = 0; double seconds = 0; Status status; std::vector<Msg> msgs; };
    // Runs a tune that has ALREADY been keyed by the caller (the TxGate): sends the threshold first (PowerSDR did not re-send it, so a changed
    // setting never reached the tuner), then the tune request, then polls every ~35 ms until pass / fail (and drains the queue) or `timeout_s`.
    // `keyed` is checked on every poll: if the key goes away the tune is abandoned (Timeout). Never keys or unkeys anything itself.
    TuneOutcome tune(bool full, double threshold, const std::function<bool()>& keyed, double timeout_s = 20.0);
private:
    Flex& fx_; Clock clk_; Status st_;
};
}  // namespace fireside::atu
