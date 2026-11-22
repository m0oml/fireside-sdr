#include "fireside/sim_radio.h"
#include <chrono>
#include "fireside/atu.h"
#include "fireside/opcodes.h"

namespace fireside::sim {
namespace {
uint32_t res(uint8_t code, uint8_t b2 = 0, uint8_t b3 = 0, uint8_t rem = 0) { return uint32_t(rem) << 24 | uint32_t(b3) << 16 | uint32_t(b2) << 8 | code; }
uint32_t op(const char* n) { return find_opcode(std::string(n))->value; }
}  // namespace

RadioModel::RadioModel(DiceBus& b) : bus(b) {
    bus.set_flex_5000a();
    bus.dynamic[op("RDAL_OP_ATU_SEND_CMD")] = [this] { on_send(be_word(bus.mem[kPalResult])); return 0u; };
    bus.dynamic[op("RDAL_OP_ATU_GET_RESULT")] = [this] { return next(); };
    bus.dynamic[op("RDAL_OP_SET_MOX")] = [this] { keyed = be_word(bus.mem[kPalResult]) == 1; return 0u; };
    bus.dynamic[op("RDAL_OP_READ_PA_ADC")] = [this] { return pa_adc(be_word(bus.mem[kPalResult])); };
    bus.dynamic[op("RDAL_OP_READ_TRX_EEPROM_UINT32")] = [this] {
        const uint32_t a = be_word(bus.mem[kPalResult]);
        if (a + 4 > eeprom.size()) return 0xFFFFFFFFu;
        return uint32_t(eeprom[a]) | uint32_t(eeprom[a + 1]) << 8 | uint32_t(eeprom[a + 2]) << 16 | uint32_t(eeprom[a + 3]) << 24;
    };
}

void RadioModel::on_send(uint32_t d1) {
    const uint8_t req = uint8_t(d1 >> 8), b2 = uint8_t(d1); requests.push_back(req);
    switch (req) {
        case atu::AUTO_OFF: q.push_back(res(atu::R_AUTO_STATUS, 0)); break;
        case atu::STANDBY: q.push_back(res(atu::R_INSTANDBY)); break;
        case atu::HIZ: case atu::LOZ: break;                                  // ignored after a direct set, as on the radio
        case atu::SET_IND: ind = b2 & 0x7F; hiz = b2 >> 7; break;              // like the real tuner: Hi-Z in bit 7
        case atu::SET_CAP: cap = b2; break;
        case atu::ALL_UPDATE: report(false); break;
        case atu::FWD_PWR: q.push_back(res(atu::R_FWDPWR, 0x01, 0x2C)); break;             // the tuner's own meter: 3.00 W
        case atu::REV_PWR: q.push_back(res(atu::R_REVPWR, 0x00, 0x01)); break;             // 0.01 W
        case atu::SWR: q.push_back(res(atu::R_SWR, 0x00, 0x00)); break;
        case atu::FULL_TUNE: case atu::MEM_TUNE: tune_polls = 0; full = req == atu::FULL_TUNE; q.push_back(res(atu::R_ACTIVE)); break;
        default: if (req >= atu::SWR11 && req <= atu::SWR30) { thr = req - atu::SWR11; q.push_back(res(atu::R_SWRTHRESH, uint8_t(thr))); } break;
    }
}
void RadioModel::report(bool after_tune) {
    std::vector<uint32_t> r = {res(atu::R_IND, uint8_t(ind)), res(atu::R_CAP, uint8_t(cap)), res(atu::R_HILOZ, uint8_t(hiz)), res(atu::R_ANTENNA, 0)};
    if (after_tune) { r.push_back(res(atu::R_SWRTHRESH, uint8_t(thr))); r.push_back(res(atu::R_FWDPWR, 0x04, 0x24)); r.push_back(res(atu::R_REVPWR, 0, 0)); r.push_back(res(atu::R_SWR, 0, uint8_t(pass_swr_b3))); }
    for (auto w : r) q.push_back(w);
}
uint32_t RadioModel::next() {
    if (tune_polls >= 0 && !silent && ++tune_polls >= tune_time_polls) {
        tune_polls = -1;
        if (!keyed) q.push_back(res(atu::R_TUNEFAIL, 0));
        else if (best_swr > atu::kThresholds[thr] && !pass_regardless) q.push_back(res(atu::R_TUNEFAIL, 2));
        else { ind = 30; cap = 7; hiz = 1; q.push_back(res(atu::R_TUNEPASS)); report(true); }
    }
    if (q.empty()) return 0x00FFFFFFu;
    uint32_t w = q.front(); q.pop_front(); return (w & 0x00FFFFFFu) | uint32_t(q.size() > 255 ? 255 : q.size()) << 24;
}
uint32_t RadioModel::pa_adc(uint32_t ch) const {
    if (!keyed || dead_pa) return 0;
    if (!pa_from_level) return uint32_t(ch == 7 ? adc_fwd : ch == 6 ? adc_rev : 0);
    if (!meter.valid) return 0;
    const double lv = meter_lag_s > 0 ? lagged_level() : level.load(), w = k * lv * lv, want = ch == 7 ? w : w * rev_frac / meter.rev_corr[size_t(band)];
    int a = 0; while (a < 4095 && txm::watts(meter, band, a) < want) ++a;        // the detector reading that gives this power
    return uint32_t(a);
}
double RadioModel::lagged_level() const {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    lag_hist.push_back({now, level.load()});
    while (lag_hist.size() > 1 && lag_hist[1].first <= now - meter_lag_s) lag_hist.pop_front();
    return lag_hist.front().first <= now - meter_lag_s ? lag_hist.front().second : 0.0;
}
}  // namespace fireside::sim
