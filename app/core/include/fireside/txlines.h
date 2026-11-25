// The rear RCA TX1..TX3 lines (red, white, yellow) used as BAND switches: a line is on while the dial is in a band ticked for it, on receive and
// transmit alike (an external relay, e.g. an 80 m T-mode relay at the balun). The ONE way the app drives these lines. Rules:
//   1. only SET_RCA_TXn with d1 = 0 / 1, d2 = 0 (PowerSDR FWC.SetRCATX1..3), and SET_AMP_TXn with (0, 0) only: "do not follow transmit". PowerSDR's
//      default is to make every line follow T/R (FWCAmpTX1..3 = true); a band relay must never click with each over, so the first write to a used line
//      after arm() turns that off. SET_AMP_TXn 1 is refused by the safety gate (value-restricted to 0, 0).
//   2. a line whose role is not Band is never written: with no line in use the radio sees no new traffic at all.
//   3. a line never changes while an over runs: apply() is refused while busy(); the transmit path calls for_over() (unkeyed, before every key-down),
//      which sets the lines for that frequency and waits until settle_s has passed since the last change (no hot switching of the relay).
//   4. release() (radio stop / app close): every used line off, then disarmed. Build external gear so that OFF is the safe position.
//   5. FireWire mailbox only; armed only after a completed start-up; any mailbox fault disarms until re-armed (the transmit path then refuses).
// Amplifier keying and transverter roles are placeholders (not built): treated like Unused, nothing is sent. Thread-safe.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "fireside/atu.h"
#include "fireside/client.h"

namespace fireside {
enum class LineRole { Unused = 0, Band = 1, Amplifier = 2, Transverter = 3 };
inline constexpr int kTxLines = 3;
struct LineConfig {
    std::array<LineRole, kTxLines> role{};                        // TX1, TX2, TX3
    std::array<uint32_t, kTxLines> band_mask{};                   // bit i: on in rx_bands()[i] (rxant.h); outside every band: off
    double settle_s = 0.05;                                       // a relay's operate + bounce time: no key-down sooner after a change (0.01..1 s)
};
const char* line_role_id(LineRole r);                             // "unused" "band" "amp" "xvtr"
LineRole line_role_from_id(const std::string& id);                // anything unknown: Unused

class TxLines {
public:
    enum class Result { Sent, Unchanged, NotArmed, Busy, Fault };
    explicit TxLines(Flex& fx, atu::Clock clock = atu::real_clock());   // throws StartupError unless fx's link is the proven FireWire mailbox
    static std::array<bool, kTxLines> wanted(const LineConfig& c, double dial_hz);   // which lines should be on at this dial (Band lines only)
    static bool any_used(const LineConfig& c);
    void configure(const LineConfig& c);                          // stores the table (writes nothing: the next apply / for_over does); a line leaving the Band role is switched off by the next apply
    LineConfig config() const;
    void arm();                                                   // after a completed start-up; throws SafetyError if writes are disabled. Line states are unknown until first written
    void disarm();
    bool armed() const;
    void set_busy(std::function<bool()> busy);                    // true while an over runs (TxControl::busy): apply() is refused then
    Result apply(double dial_hz);                                 // receive: the lines for this dial. Unchanged writes nothing
    // The transmit path, unkeyed, before every key-down: sets the lines for hz and waits out the settle time. "" when ready, else why not (nothing may key).
    std::string for_over(double hz);
    void release();                                               // every used line off (best effort), then disarmed
    std::array<int, kTxLines> state() const;                      // -1 unknown, 0 off, 1 on
    std::string last_error() const;
private:
    Flex& fx_; atu::Clock clk_; mutable std::mutex m_; LineConfig cfg_; bool armed_ = false; std::function<bool()> busy_;
    std::array<int, kTxLines> cur_{{-1, -1, -1}}; std::array<bool, kTxLines> amp_off_{}; std::array<bool, kTxLines> ever_used_{}; double last_change_ = -1e9; std::string err_;
    Result set_locked(const std::array<bool, kTxLines>& want);    // the only writes; caller holds m_
};
}  // namespace fireside
