// The ONE way the app retunes the radio. CAT frequency changes (and later click-to-tune) go through Tuner and nowhere else.
// Conditions from the Opus review (analysis/startup-review.md, "Held-back changes"):
//   1. only SET_RX1_FREQ_TW is ever sent (no other opcode is reachable from here; tools/tests/test_tune_guard.py checks the source)
//   2. frequency is range-checked (0 < f <= 65 MHz, for the DDS frequency AND the dial)
//   3. rate-limited with coalescing: a spinning knob or CAT polling storm sends at most one write per min_interval, always the latest frequency
//   4. FireWire mailbox only; armed only after a completed start-up (arm()); any mailbox fault disarms until the operator re-arms
// Thread-safe. Nothing calls this on real hardware yet.
#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include "fireside/cat.h"
#include "fireside/client.h"

namespace fireside {
// The radio's DDS sits this far BELOW the dial (PowerSDR: if_shift on, if_freq 0.009 MHz); the receiver shifts the I/Q back up by the same amount.
// Measured 2026-11-17: the radio's I/Q has a narrow hole at 0 Hz (DC removal, ~2 Hz wide, centre bin ~21 dB down), so a carrier tuned exactly onto the
// DDS is stripped and AM sounds dreadful; with the offset nothing on the dial lands there. Must equal dsp::kIfOffsetHz (checked by a test).
inline constexpr double kRadioIfOffsetHz = 9000.0;
struct TuneConfig {
    double if_offset_hz = 0.0;          // DDS = dial - if_offset (0 = DDS on the dial, as the reviewed start-up does today)
    double min_interval_s = 0.05;       // <= 20 writes/s
    double clock_mhz = 500.0;
};
enum class TuneResult { Sent, Coalesced, Unchanged, Rejected, NotArmed, Fault };

class Tuner {
public:
    // Throws StartupError unless fx's link is the FireWire mailbox. `now` returns seconds (default: steady clock); injectable for tests.
    explicit Tuner(Flex& fx, TuneConfig cfg = {}, std::function<double()> now = {});
    // Call only after run_startup() succeeded. Throws SafetyError if fx has writes disabled. `current_dial_hz` (if known) is what the radio is already tuned to.
    void arm(std::optional<double> current_dial_hz = std::nullopt);
    void disarm();
    bool armed() const;
    // Ask for a dial frequency in Hz. Sends immediately if the rate limit allows, otherwise remembers the latest request (see pump()).
    TuneResult request(double dial_hz);
    // Sends the pending (coalesced) request if the interval has elapsed. Call from a timer. Returns true if a write was sent.
    bool pump();
    std::optional<double> last_sent_hz() const;
    // The DDS offset below the dial (the 9 kHz IF shift on receive). Transmitting needs 0, as PowerSDR does (if_shift = false before
    // MOX): on the FLEX-5000 RX1 and the transmitter share one DDS. Re-sends the current dial at once with the new offset (no rate limit).
    // Returns Sent, Unchanged (already so), NotArmed or Fault.
    TuneResult set_if_offset(double hz);
    double if_offset() const;
    std::optional<double> pending_hz() const;
    struct Counters { unsigned long sent = 0, coalesced = 0, rejected = 0, unchanged = 0, faults = 0; };
    Counters counters() const;
    std::string last_error() const;
    static bool in_range(double dial_hz, double if_offset_hz);          // the range check, exposed for the GUI/CAT to pre-validate
private:
    Flex& fx_; TuneConfig cfg_; std::function<double()> now_;
    mutable std::mutex m_; bool armed_ = false; std::optional<double> last_sent_, pending_; double last_write_t_ = -1e9; Counters c_; std::string err_;
    TuneResult send_locked(double dial_hz);
};

// CAT -> Tuner: when the receive frequency changes, ask the tuner. CAT's own validation (<= 65 MHz, 11 digits) runs first; the tuner re-checks.
inline void connect_cat_to_tuner(CatSession& cat, Tuner& tuner) { cat.on_rx_freq = [&tuner](uint64_t hz) { tuner.request(double(hz)); }; }
}  // namespace fireside
