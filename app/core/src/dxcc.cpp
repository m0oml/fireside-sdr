#include "fireside/dxcc.h"
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace fireside::decode {
namespace {
const DxccEntry kTable[] = {
#include "dxcc_table.inc"
};
const std::unordered_map<std::string, const DxccEntry*>& index() {
    static const auto m = [] { std::unordered_map<std::string, const DxccEntry*> r; for (const auto& e : kTable) r.emplace(e.prefix, &e); return r; }();
    return m;
}
}  // namespace

const DxccEntry* dxcc_lookup(const std::string& call) {
    for (size_t n = std::min<size_t>(call.size(), 7); n >= 1; --n) { const auto it = index().find(call.substr(0, n)); if (it != index().end()) return it->second; }
    return nullptr;
}
bool grid4_center(const std::string& g, double& lat, double& lon) {
    if (g.size() != 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return false;
    lon = (g[0] - 'A') * 20.0 - 180.0 + (g[2] - '0') * 2.0 + 1.0; lat = (g[1] - 'A') * 10.0 - 90.0 + (g[3] - '0') + 0.5; return true;
}
double great_circle_km(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180.0, dl = (lo2 - lo1) * r, a = std::sin((la2 - la1) * r / 2), b = std::sin(dl / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b; return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
}
bool callsign_grid_plausible(const std::string& call, const std::string& grid) {
    if (call.empty() || call.find('/') != std::string::npos || call[0] == '<') return true;
    double la, lo; if (!grid4_center(grid, la, lo)) return true;
    const DxccEntry* e = dxcc_lookup(call); if (!e) return true;
    return great_circle_km(e->lat, e->lon, la, lo) <= e->radius_km;
}
}  // namespace fireside::decode
