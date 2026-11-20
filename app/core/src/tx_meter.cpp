#include "fireside/tx_meter.h"
#include <cmath>

namespace fireside::txm {
namespace {
struct Edge { const char* name; double lo, hi; };
constexpr Edge kEdges[kBands] = {{"160m", 1.8e6, 2.0e6}, {"80m", 3.5e6, 4.0e6}, {"60m", 5.1e6, 5.5e6}, {"40m", 7.0e6, 7.3e6}, {"30m", 10.1e6, 10.15e6},
                                 {"20m", 14.0e6, 14.35e6}, {"17m", 18.068e6, 18.168e6}, {"15m", 21.0e6, 21.45e6}, {"12m", 24.89e6, 24.99e6},
                                 {"10m", 28.0e6, 29.7e6}, {"6m", 50.0e6, 54.0e6}};
const TableResult* find(const Decoded& d, const char* name) { for (auto& t : d.tables) if (t.name == name) return &t; return nullptr; }
}  // namespace

const char* band_name(int b) { return b >= 0 && b < kBands ? kEdges[b].name : ""; }
int band_of(double f) { for (int b = 0; b < kBands; ++b) if (f >= kEdges[b].lo && f <= kEdges[b].hi) return b; return -1; }
int band_from_name(const std::string& n) { for (int b = 0; b < kBands; ++b) if (n == kEdges[b].name) return b; return -1; }

PaCal PaCal::from_eeprom(const Decoded& d) {
    PaCal c;
    const TableResult* br = find(d, "PA bridge"); const TableResult* sw = find(d, "PA SWR");
    if (!br || !sw) { c.problem = "the EEPROM decode has no PA bridge / PA SWR table"; return c; }
    if (!br->pointer_ok || !br->checksum_ok || br->has_nan || br->values.size() != size_t(kBands)) { c.problem = "PA bridge table unusable"; return c; }
    if (!sw->pointer_ok || !sw->checksum_ok || sw->has_nan || sw->values.size() != size_t(kBands)) { c.problem = "PA SWR table unusable"; return c; }
    for (int b = 0; b < kBands; ++b) {
        if (br->values[size_t(b)].size() != 6 || sw->values[size_t(b)].empty()) { c.problem = "table row size"; return c; }
        for (int j = 0; j < 6; ++j) c.bridge[size_t(b)][size_t(j)] = br->values[size_t(b)][size_t(j)];
        for (int j = 1; j < 6; ++j) if (!(c.bridge[size_t(b)][size_t(j)] > c.bridge[size_t(b)][size_t(j - 1)])) { c.problem = std::string("PA bridge not increasing on ") + band_name(b); return c; }
        c.rev_corr[size_t(b)] = sw->values[size_t(b)][0];
        if (!(c.rev_corr[size_t(b)] > 0.1 && c.rev_corr[size_t(b)] < 10)) { c.problem = std::string("PA SWR factor implausible on ") + band_name(b); return c; }
    }
    c.valid = true; return c;
}

double watts(const PaCal& c, int band, int adc) {
    if (!c.valid || band < 0 || band >= kBands || adc < 2) return 0.0;
    static const double kP[6] = {1.0, 2.0, 5.0, 10.0, 20.0, 90.0};
    const auto& t = c.bridge[size_t(band)];
    const double v = adc / 4096.0 * 2.5, v2 = v * v;
    int hi = 6; for (int i = 0; i < 6; ++i) if (v < t[size_t(i)]) { hi = i; break; }
    if (hi != 6) {
        const double vl = hi ? t[size_t(hi - 1)] : 0.0, pl = hi ? kP[hi - 1] : 0.0, vh = t[size_t(hi)], ph = kP[hi];
        return pl + (ph - pl) * ((v2 - vl * vl) / (vh * vh - vl * vl));
    }
    const double vl2 = t[4] * t[4], vh2 = t[5] * t[5];            // above the 90 W point: PowerSDR's extrapolation
    if (vl2 == vh2) return 0.0;
    const double a = 70.0 / (vh2 - vl2), b = 90.0 / (a * vh2);
    return a * v2 + b;
}

Reading read(const PaCal& c, int band, int adc_fwd, int adc_rev) {
    Reading r; if (!c.valid || band < 0 || band >= kBands) return r;
    r.fwd_w = watts(c, band, adc_fwd); r.rev_w = watts(c, band, adc_rev) * c.rev_corr[size_t(band)];
    if ((adc_fwd == 0 && adc_rev == 0) || (r.fwd_w <= 1.0 && r.rev_w <= 1.0)) return r;
    if (r.rev_w >= r.fwd_w) { r.swr = 25.0; return r; }
    const double q = std::sqrt(r.rev_w / r.fwd_w);
    r.swr = std::min(25.0, (1 + q) / (1 - q));
    return r;
}
}  // namespace fireside::txm
