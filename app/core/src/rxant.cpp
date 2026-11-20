#include "fireside/rxant.h"
#include <algorithm>
#include "fireside/startup.h"

namespace fireside {
std::optional<RxAnt> rx_ant_from_name(const std::string& n) {
    std::string u; for (char c : n) if (c != ' ' && c != '_') u += char(std::toupper(static_cast<unsigned char>(c)));
    if (u == "ANT1") return RxAnt::Ant1; if (u == "ANT2") return RxAnt::Ant2; if (u == "ANT3") return RxAnt::Ant3; if (u == "RX1IN") return RxAnt::Rx1In;
    return std::nullopt;
}
const char* rx_ant_name(RxAnt a) { switch (a) { case RxAnt::Ant1: return "ANT1"; case RxAnt::Ant2: return "ANT2"; case RxAnt::Ant3: return "ANT3"; case RxAnt::Rx1In: return "RX1IN"; } return "?"; }

RxAntenna::RxAntenna(Flex& fx) : fx_(fx) { require_firewire(fx_); }
void RxAntenna::arm(const std::string& started_with) {
    if (!fx_.safety().allow_write) throw SafetyError("RxAntenna::arm: the Flex client has writes disabled");
    const auto a = rx_ant_from_name(started_with);
    std::lock_guard<std::mutex> l(m_); armed_ = true; cur_ = a ? int(*a) : 0; err_.clear();
}
void RxAntenna::disarm() { std::lock_guard<std::mutex> l(m_); armed_ = false; }
bool RxAntenna::armed() const { std::lock_guard<std::mutex> l(m_); return armed_; }
std::optional<RxAnt> RxAntenna::current() const { std::lock_guard<std::mutex> l(m_); if (cur_ == 0) return std::nullopt; return RxAnt(cur_); }
std::string RxAntenna::last_error() const { std::lock_guard<std::mutex> l(m_); return err_; }
AntResult RxAntenna::request(RxAnt a) {
    const int v = int(a); if (v < 1 || v > 4) return AntResult::NotArmed;      // unreachable through the enum; kept so a bad cast can never reach the wire
    std::lock_guard<std::mutex> l(m_);
    if (!armed_) return AntResult::NotArmed;
    if (v == cur_) return AntResult::Unchanged;
    try { fx_.set("SET_RX1_ANT", double(v), 0.0); }                            // the only write in this file: d1 = 1..4, d2 = 0 (as PowerSDR's SetRX1Ant)
    catch (const std::exception& e) { armed_ = false; err_ = e.what(); return AntResult::Fault; }
    cur_ = v; return AntResult::Sent;
}

namespace {
constexpr RxBand kBands[] = {
    {"lfmw", "Long / medium wave", 100e3, 1800e3},
    {"160m", "160 m", 1810e3, 2000e3}, {"80m", "80 m", 3500e3, 3800e3}, {"60m", "60 m", 5258.5e3, 5406.5e3}, {"40m", "40 m", 7000e3, 7200e3}, {"30m", "30 m", 10100e3, 10150e3},
    {"20m", "20 m", 14000e3, 14350e3}, {"17m", "17 m", 18068e3, 18168e3}, {"15m", "15 m", 21000e3, 21450e3}, {"12m", "12 m", 24890e3, 24990e3}, {"10m", "10 m", 28000e3, 29700e3},
    {"6m", "6 m", 50000e3, 52000e3},
};
}
const RxBand* rx_bands(size_t& n) { n = std::size(kBands); return kBands; }
int rx_band_index(double hz) { for (size_t i = 0; i < std::size(kBands); ++i) if (hz >= kBands[i].lo_hz && hz <= kBands[i].hi_hz) return int(i); return -1; }
}  // namespace fireside
