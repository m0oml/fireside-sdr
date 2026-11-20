// The ONE way the app switches the receive preamp. Modelled on Tuner (same reviewed conditions):
//   1. only SET_TRX_PREAMP is ever sent, and only with the wire values 0 (off) or 1 (on): nothing else is reachable from here
//      (tools/tests/test_tune_guard.py checks the source)
//   2. FireWire mailbox only; armed only after a completed start-up (arm()); any mailbox fault disarms until the operator re-arms
//   3. idempotent: asking for the state the radio is already in sends nothing (start-up has just forced the preamp OFF, so "last" starts at off)
// Receive front-end gain only. Nothing here keys the transmitter, touches PA bias, the EEPROM or the dangerous-write gate. Thread-safe.
#pragma once
#include <mutex>
#include <string>
#include "fireside/client.h"

namespace fireside {
enum class PreampResult { Sent, Unchanged, NotArmed, Fault };

class RxPreamp {
public:
    explicit RxPreamp(Flex& fx);                      // throws StartupError unless fx's link is the proven FireWire mailbox
    void arm();                                       // call only after run_startup() succeeded (it sent SetTRXPreamp(false)); throws SafetyError if writes are disabled
    void disarm();
    bool armed() const;
    PreampResult request(bool on);                    // NotArmed until arm(); Fault disarms
    bool on() const;                                  // what we last sent (false after arm())
    std::string last_error() const;
private:
    Flex& fx_; mutable std::mutex m_; bool armed_ = false, on_ = false; std::string err_;
};
}  // namespace fireside
