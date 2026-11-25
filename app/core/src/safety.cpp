#include "fireside/safety.h"
#include <regex>

namespace fireside {
bool Safety::matches_dangerous(const std::string& n) {
    static const std::regex re(
        "EEPROM|REG_SET|WRITE_.*REG|_REG$|I2C_WRITE|FLEXWIRE_WRITE|PA_BIAS|POT|RDAC|"
        "RESET|PWRBT|POWER_OFF|POWER_DOWN|INITIALIZE|CLOCK|PLL|DDS_REG|GPIO|FAN|"
        "MOX|ATU_SEND|ENABLE_ATU|SET_TR$|AMP_TX|TX_DAC|FULL_DUPLEX|XVTR|XVTXEN|KEY_2M|PA_FILTER|TX_FILTER");
    return std::regex_search(n, re);
}
bool Safety::value_restricted(const std::string& n) {
    return n == "RDAL_OP_INITIALIZE" || n == "RDAL_OP_SET_MOX" || n == "RDAL_OP_SET_PA_BIAS" || n == "RDAL_OP_ATU_SEND_CMD" || n == "RDAL_OP_PA_POT_SET_RDAC" || n == "RDAL_OP_SET_TRX_POT"
        || n == "RDAL_OP_WRITE_CODEC_REG" || n == "RDAL_OP_SET_FULL_DUPLEX"
        || n == "RDAL_OP_SET_AMP_TX1" || n == "RDAL_OP_SET_AMP_TX2" || n == "RDAL_OP_SET_AMP_TX3";   // (0, 0) only: the rear TX line does NOT follow transmit (txlines.h)
}
bool Safety::calibrated_value(const std::string& n, uint32_t d1, uint32_t d2) const {
    std::lock_guard<std::mutex> l(m_);
    if (n == "RDAL_OP_PA_POT_SET_RDAC") return pa_pots_ && d1 < 8 && d2 == (*pa_pots_)[d1];
    if (n == "RDAL_OP_SET_TRX_POT") return d2 == 0 && carrier_words_.count(d1) != 0;
    return false;
}
namespace {
// ATU_SEND_CMD(d1 = request << 8 | b2, d2 = b3), PowerSDR FWC.ATUSendCmd. Requests that move relays, change settings or ask for reports: no RF, nothing erased.
bool atu_request_reviewed(uint32_t d1, uint32_t d2) {
    if (d1 > 0xFFFF || d2 > 0xFF) return false;
    const uint32_t req = d1 >> 8, b2 = d1 & 0xFF;
    switch (req) {
        case 0: case 1: case 2: case 3: case 4:                  // no-op, inductor up/down, capacitor up/down
        case 8: case 9: case 10:                                 // Hi-Z, Lo-Z, antenna 1 (the FLEX-5000 uses antenna 1 only: REQ_ANT2 = 11 is refused)
        case 40: case 41: case 44: case 45:                      // report all, version, standby, active
        case 59: case 60: case 61: case 62: case 64:             // auto tune OFF, report fwd / rev / SWR, live updates off
            // not allowed (unused, least privilege): 46 manual store (writes the tuner's memory), 58 auto tune ON (the tuner would retune by itself during
            // a later transmission), 63 live updates on (floods the result queue)
            return b2 == 0 && d2 == 0;
        case 50: case 51: case 52: case 53: case 54: case 55: case 56:   // SWR threshold 1.1 ... 3.0
            return b2 == 0 && d2 == 0;
        case 65: case 66: return d2 == 0;                        // set inductor (b2: value, with the Hi/Lo-Z bit) / capacitor (b2: value) directly
        default: return false;                                   // 5 memory tune / 6 full tune (need RF: transmit grant), 42/43 arm/clear memory, 57 reset, 67, 68, unknown
    }
}
}  // namespace
bool Safety::reviewed_value(const std::string& n, uint32_t d1, uint32_t d2) {
    if (!value_restricted(n)) return true;
    if (n == "RDAL_OP_ATU_SEND_CMD") return atu_request_reviewed(d1, d2);
    // Codec: ONLY the mic input gain, register 0x16 (PowerSDR fwcmixform.cs tbMic: 0 dB = 0x00, -128..-1 dB = 0x80..0xFF, 0x80 = muted).
    // An audio input level: it cannot key, route or switch anything else (analysis/voice-tx.md).
    if (n == "RDAL_OP_WRITE_CODEC_REG") return d1 == 0x16 && (d2 == 0x00 || (d2 >= 0x80 && d2 <= 0xFF));
    // Full duplex: only off / on (siggen.cpp, the built-in signal generator, receive only; PowerSDR FWC.SetFullDuplex).
    if (n == "RDAL_OP_SET_FULL_DUPLEX") return d1 <= 1 && d2 == 0;
    if (n == "RDAL_OP_PA_POT_SET_RDAC" || n == "RDAL_OP_SET_TRX_POT") return false;   // only calibrated_value() (the radio's own EEPROM values)
    return d1 == 0 && d2 == 0;
}
bool Safety::tx_reviewed_value(const std::string& n, uint32_t d1, uint32_t d2) {
    if (n == "RDAL_OP_SET_MOX") return d1 == 1 && d2 == 0;
    if (n == "RDAL_OP_ATU_SEND_CMD") return (d1 == (5u << 8) || d1 == (6u << 8)) && d2 == 0;   // memory tune, full tune
    return false;
}
void Safety::check(const OpInfo& op, bool is_get) const { check_impl(op, is_get, nullptr, nullptr); }
void Safety::check_write(const OpInfo& op, uint32_t d1, uint32_t d2) const { check_impl(op, false, &d1, &d2); }
void Safety::check_impl(const OpInfo& op, bool is_get, const uint32_t* d1, const uint32_t* d2) const {
    const std::string name = op.name;
    if (is_get) {
        const size_t pos = name.rfind("_OP_");
        const std::string tail = pos == std::string::npos ? name : name.substr(pos + 4);
        if (is_read(op) || tail.rfind("GET_", 0) == 0 || tail.rfind("READ_", 0) == 0) {
            if (name.find("EEPROM") != std::string::npos && name.find("WRITE") != std::string::npos)
                throw SafetyError(name + " is a write");
            return;
        }
        if (!allow_dangerous) throw SafetyError(name + ": not a known read opcode; needs allow_dangerous");
        return;
    }
    if (!allow_write) throw SafetyError(name + ": writes disabled (use allow_write)");
    if (permitted(name) && (!value_restricted(name) || (d1 && d2 && (reviewed_value(name, *d1, *d2) || calibrated_value(name, *d1, *d2) || (tx_grant() && tx_reviewed_value(name, *d1, *d2))))))
        return;                                                 // still requires allow_write (checked above)
    // a permitted opcode with an unreviewed (or unknown) value falls through to the normal dangerous check
    if ((matches_dangerous(name) || op.access == '?') && !allow_dangerous)
        throw SafetyError(name + ": dangerous/unknown write (needs allow_dangerous)");
}
}  // namespace fireside
