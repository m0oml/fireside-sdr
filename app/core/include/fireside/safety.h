// Safety policy: identical rules to tools/firesidectl/client.py. A donated radio's factory calibration lives in EEPROM.
//   reads: always allowed | writes: need allow_write | EEPROM/register/I2C/bias/reset/power writes, anything that can key the
//   transmitter, and opcodes PowerSDR never calls: also need allow_dangerous (or an explicit, reviewed permit()).
// A permit() for a value-restricted opcode (INITIALIZE, SET_MOX, SET_PA_BIAS) covers ONLY its reviewed wire values (0, 0); for ATU_SEND_CMD only the
// non-RF, non-erasing tuner requests. Keying (SET_MOX 1) and ATU tunes additionally need the transmit grant (set only by the TxGate). The values are checked here,
// inside the gate, on the packed words, so no other caller or thread can use an open permit to send anything else (Opus diff-check F1/F2).
// Thread-safe: permit()/unpermit()/check*() may race.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include "fireside/opcodes.h"

namespace fireside {
struct SafetyError : std::runtime_error { using std::runtime_error::runtime_error; };

class Safety {
public:
    bool allow_write = false, allow_dangerous = false;
    Safety() = default;
    Safety(const Safety& o) : allow_write(o.allow_write), allow_dangerous(o.allow_dangerous) { std::lock_guard<std::mutex> l(o.m_); permitted_ = o.permitted_; }
    Safety& operator=(const Safety& o) {
        if (this != &o) { std::set<std::string> p; { std::lock_guard<std::mutex> l(o.m_); p = o.permitted_; }
                          std::lock_guard<std::mutex> l(m_); permitted_ = std::move(p); allow_write = o.allow_write; allow_dangerous = o.allow_dangerous; }
        return *this;
    }
    void check(const OpInfo& op, bool is_get) const;                       // writes checked without values: a permit never covers a value-restricted opcode
    void check_write(const OpInfo& op, uint32_t d1, uint32_t d2) const;    // writes with their packed wire values
    void permit(const std::string& full_name) { std::lock_guard<std::mutex> l(m_); permitted_.insert(full_name); }
    void unpermit(const std::string& full_name) { std::lock_guard<std::mutex> l(m_); permitted_.erase(full_name); }
    bool permitted(const std::string& full_name) const { std::lock_guard<std::mutex> l(m_); return permitted_.count(full_name) != 0; }
    static bool matches_dangerous(const std::string& full_name);
    static bool value_restricted(const std::string& full_name);            // INITIALIZE, SET_MOX, SET_PA_BIAS, ATU_SEND_CMD, PA pots, TRX pot, WRITE_CODEC_REG (mic gain 0x16 only), SET_FULL_DUPLEX (0 / 1)
    // true if not restricted; (0, 0) for INITIALIZE / SET_MOX / SET_PA_BIAS; for ATU_SEND_CMD the requests that cannot make RF or erase anything (atu.h)
    static bool reviewed_value(const std::string& full_name, uint32_t d1, uint32_t d2);
    // Transmit grant: only the TxGate (tx_gate.h) sets it, while it holds the key. With it (and a permit), SET_MOX(1, 0) and the ATU tune requests
    // (memory tune / full tune) pass; without it they are refused like any unreviewed value. Never covers anything else.
    void set_tx_grant(bool on) { std::lock_guard<std::mutex> l(m_); tx_grant_ = on; }
    bool tx_grant() const { std::lock_guard<std::mutex> l(m_); return tx_grant_; }
    static bool tx_reviewed_value(const std::string& full_name, uint32_t d1, uint32_t d2);
    // Transmit calibration from the radio's own verified EEPROM (tx_setup.h is the only caller): PA_POT_SET_RDAC may then write exactly pot i = pots[i]
    // (i 0..7), and SET_TRX_POT exactly one of the TX carrier-null words. Until it is set, both are refused like any unreviewed value.
    void set_tx_calibration(const std::array<uint8_t, 8>& pa_pots, const std::vector<uint32_t>& carrier_null_words) {
        std::lock_guard<std::mutex> l(m_); pa_pots_ = pa_pots; carrier_words_ = std::set<uint32_t>(carrier_null_words.begin(), carrier_null_words.end()); }
    bool calibrated_value(const std::string& full_name, uint32_t d1, uint32_t d2) const;
private:
    mutable std::mutex m_; std::set<std::string> permitted_; bool tx_grant_ = false;
    std::optional<std::array<uint8_t, 8>> pa_pots_; std::set<uint32_t> carrier_words_;
    void check_impl(const OpInfo& op, bool is_get, const uint32_t* d1, const uint32_t* d2) const;
};
}  // namespace fireside
