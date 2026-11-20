// The transmit gate: the ONLY code that may key the transmitter (SET_MOX 1) or start an ATU tune.
//   * disarmed until arm(); disarm() unkeys
//   * every key-down has a deadline (never longer than cfg.max_key_s); tick() enforces it
//   * meter readings fed to report() unkey it above cfg.max_fwd_w, or above cfg.max_swr once there is enough power to judge
//   * unkey() always sends SET_MOX 0, also after an error, and the destructor unkeys
//   * while keyed it holds the safety gate's transmit grant (safety.h): without it SET_MOX 1 and ATU tune requests are refused everywhere
// Stage 1 (2026-11-18): tested on the simulated radio only. Not connected to the GUI or a real radio yet: that is a reviewed step of its own
// (the PC-to-radio I/Q stream has to exist first, and the first RF is a supervised low-power test).
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "fireside/atu.h"
#include "fireside/client.h"
#include "fireside/tx_meter.h"

namespace fireside {
struct GateConfig {
    double max_key_s = 15.0;                                      // hard limit for any single key-down (a full ATU tune took 8.9 .. 9.9 s in the capture)
    double max_fwd_w = 20.0;                                      // unkey above this forward power (PowerSDR tunes at 15 W)
    double max_swr = 5.0, swr_min_fwd_w = 2.0;                    // unkey above this SWR once forward power is at least swr_min_fwd_w
    double max_rev_w = 1e9;                                       // unkey above this reflected power (used during ATU tunes, where SWR is no guide)
};

class TxGate {
public:
    TxGate(Flex& fx, GateConfig cfg = {}, atu::Clock clock = atu::real_clock()) : fx_(fx), cfg_(cfg), clk_(std::move(clock)) {}
    ~TxGate();
    TxGate(const TxGate&) = delete; TxGate& operator=(const TxGate&) = delete;
    void arm() { armed_ = true; }
    void disarm(const std::string& why = "disarmed");
    bool armed() const { return armed_; }
    bool keyed() const { return keyed_; }
    // Keys for at most min(max_s, cfg.max_key_s). false (with the reason in *why_not) if disarmed, already keyed, or the radio refused.
    bool key(const std::string& purpose, double max_s, std::string* why_not = nullptr);
    void unkey(const std::string& why);                           // safe to call any time; sends SET_MOX 0 if keyed
    void tick();                                                  // unkeys when the deadline has passed
    void report(const txm::Reading& r);                           // meter limits
    const std::string& last_unkey_reason() const { return last_reason_; }
    bool unkey_failed() const { return unkey_failed_; }            // SET_MOX 0 never got through: the radio may still be keyed
    const std::vector<std::string>& log() const { return log_; }
    const GateConfig& config() const { return cfg_; }
    Flex& flex() { return fx_; }
private:
    Flex& fx_; GateConfig cfg_; atu::Clock clk_;
    bool armed_ = false, keyed_ = false, had_permit_ = false, unkey_failed_ = false; double deadline_ = 0; std::string last_reason_; std::vector<std::string> log_;
    void note(const std::string& s);
};

// One complete ATU tune the way PowerSDR does it on the FLEX-5000 (capture of 2026-11-18): TX DSP mode CWU, key, threshold + tune request,
// poll the tuner and the PA meter (~35 ms), unkey, restore the mode. The tune carrier itself must already be on the I/Q stream: `carrier_ready`
// is asked first and the tune is refused without it. Returns nullopt if the gate refused to key (reason in *why_not).
struct TuneMeter { std::vector<txm::Reading> readings; double max_fwd_w = 0; };
std::optional<atu::Atu::TuneOutcome> run_atu_tune(TxGate& gate, atu::Atu& atu, const txm::PaCal& cal, int band, double threshold, bool full,
                                                   uint32_t restore_dsp_mode, const std::function<bool()>& carrier_ready,
                                                   TuneMeter* meter = nullptr, std::string* why_not = nullptr,
                                                   const std::function<void(bool)>& carrier = {},    // called with true right after the key, false before the unkey
                                                   const std::function<void(const txm::Reading&)>& on_poll = {});   // after every meter reading (live display; may unkey)
}  // namespace fireside
