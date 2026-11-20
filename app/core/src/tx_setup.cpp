#include "fireside/tx_setup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace fireside::txs {
namespace {
const TableResult* find(const Decoded& d, const char* name) { for (auto& t : d.tables) if (t.name == name) return &t; return nullptr; }
}  // namespace

TxCal from_image(const std::vector<uint8_t>& img) {
    TxCal c; auto bad = [&](const std::string& w) { c.problem = w; return c; };
    if (img.size() < kTrxLen) return bad("EEPROM image too short");
    const Decoded d = decode_eeprom(img, "TRX");
    if (!d.header_valid || !d.header.checksums_present) return bad("EEPROM header invalid or its checksum flag (0x3F) is not set");
    const unsigned ptr = unsigned(img[0x54]) | unsigned(img[0x55]) << 8;
    if (ptr < 0x2C0 || ptr + 8 > 0x800) return bad("PA bias pointer implausible");
    for (int i = 0; i < 8; ++i) c.eeprom_bias[size_t(i)] = img[ptr + size_t(i)];
    for (uint8_t b : c.eeprom_bias) if (b == 0x00 || b == 0xFF) return bad("PA bias row 0 is blank: the PA was never biased (refusing)");
    for (int i = 0; i < 8; ++i) c.bias_pots[size_t(i)] = (i % 2) ? uint8_t(255 - c.eeprom_bias[size_t(i)]) : c.eeprom_bias[size_t(i)];
    const TableResult* pw = find(d, "PA power");
    if (!pw || !pw->pointer_ok || !pw->checksum_ok || pw->has_nan || pw->values.size() != size_t(txm::kBands)) return bad("PA power table unusable");
    for (int b = 0; b < txm::kBands; ++b) {
        if (pw->values[size_t(b)].size() != 13) return bad("PA power row size");
        for (int j = 0; j < 13; ++j) {
            c.power[size_t(b)][size_t(j)] = pw->values[size_t(b)][size_t(j)];
            if (!(c.power[size_t(b)][size_t(j)] > 0) || (j && c.power[size_t(b)][size_t(j)] < c.power[size_t(b)][size_t(j - 1)])) {
                char m[80]; std::snprintf(m, sizeof m, "PA power table not increasing on %s", txm::band_name(b)); return bad(m); }
        }
    }
    c.meter = txm::PaCal::from_eeprom(d); if (!c.meter.valid) return bad("PA meter: " + c.meter.problem);
    for (const char* nm : {"TX image gain", "TX image phase"}) {          // optional: a bad table only means no correction (logged by the EEPROM check)
        const TableResult* t = find(d, nm); const bool gain = std::string(nm) == "TX image gain";
        if (!t || !t->pointer_ok || !t->checksum_ok || t->has_nan || t->values.size() != size_t(txm::kBands)) continue;
        for (int b = 0; b < txm::kBands; ++b) { const double v = t->values[size_t(b)].empty() ? 0.0 : t->values[size_t(b)][0]; if (std::fabs(v) <= 500) (gain ? c.image_gain : c.image_phase)[size_t(b)] = v; }
    }
    if (!d.tx_carrier || d.tx_carrier->empty()) return bad("no TX carrier-null table");
    c.carrier = *d.tx_carrier;
    c.valid = true; return c;
}

uint32_t carrier_word(const TxCal& c, double mhz) {
    if (c.carrier.empty()) throw std::logic_error("no TX carrier-null table");
    const CarrierEntry* best = &c.carrier[0];
    for (auto& e : c.carrier) if (std::fabs(e.freq_mhz - mhz) < std::fabs(best->freq_mhz - mhz)) best = &e;
    return best->value;
}

double drive_for_watts(const TxCal& c, int band, double w) {
    if (!c.valid || band < 0 || band >= txm::kBands || !(w > 0)) return 0.0;
    const auto& t = c.power[size_t(band)];
    double v;
    if (w >= 100) v = t[12];
    else if (w <= 1) v = t[0] * std::sqrt(w);                     // below the 1 W point: amplitude ~ sqrt(power)
    else {
        int hi = 1; while (kPowerPoints[hi] < w) ++hi;
        const int lo = hi - 1; const double f = (w - kPowerPoints[lo]) / (kPowerPoints[hi] - kPowerPoints[lo]);
        v = std::sqrt(t[size_t(lo)] * t[size_t(lo)] + f * (t[size_t(hi)] * t[size_t(hi)] - t[size_t(lo)] * t[size_t(lo)]));
    }
    return std::min(kAbsoluteMaxDrive, v / kAudioVolts);
}

void write_bias(Flex& fx, const TxCal& c, const LogFn& log) {
    if (!c.valid) throw std::invalid_argument("transmit calibration not valid: " + c.problem);
    std::vector<uint32_t> words; for (auto& e : c.carrier) words.push_back(e.value);
    Safety& s = fx.safety(); s.set_tx_calibration(c.bias_pots, words);
    const std::string n = "RDAL_OP_PA_POT_SET_RDAC"; const bool had = s.permitted(n); s.permit(n);
    try {
        for (int i = 0; i < 8; ++i) {
            fx.set(n, i, c.bias_pots[size_t(i)]); fx.sync();       // one at a time, each confirmed (unacknowledged bursts made PowerSDR's reads go stale)
            if (log) { char m[64]; std::snprintf(m, sizeof m, "PA bias pot %d = 0x%02X", i, c.bias_pots[size_t(i)]); log(m); }
        }
    } catch (...) { if (!had) s.unpermit(n); throw; }
    if (!had) s.unpermit(n);
}

void write_carrier_null(Flex& fx, const TxCal& c, double mhz) {
    const uint32_t w = carrier_word(c, mhz);
    Safety& s = fx.safety(); const std::string n = "RDAL_OP_SET_TRX_POT"; const bool had = s.permitted(n); s.permit(n);
    try { fx.set(n, double(w), 0); fx.sync(); } catch (...) { if (!had) s.unpermit(n); throw; }
    if (!had) s.unpermit(n);
}
}  // namespace fireside::txs
