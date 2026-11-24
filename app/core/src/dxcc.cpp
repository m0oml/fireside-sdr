#include "fireside/dxcc.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace fireside::decode {
namespace {
const DxccEntry kTable[] = {
#include "dxcc_table.inc"
};
struct ExactEntry { const char* call; const char* cont; int cq, itu; const char* dxcc; };
const ExactEntry kExact[] = {
#include "dxcc_exact.inc"
};
const std::unordered_map<std::string, const ExactEntry*>& exact_index() {
    static const auto m = [] { std::unordered_map<std::string, const ExactEntry*> r; for (const auto& e : kExact) r.emplace(e.call, &e); return r; }();
    return m;
}
const std::unordered_map<std::string, const DxccEntry*>& index() {
    static const auto m = [] { std::unordered_map<std::string, const DxccEntry*> r; for (const auto& e : kTable) r.emplace(e.prefix, &e); return r; }();
    return m;
}
}  // namespace

const DxccEntry* dxcc_lookup(const std::string& call) {
    for (size_t n = std::min<size_t>(call.size(), 7); n >= 1; --n) { const auto it = index().find(call.substr(0, n)); if (it != index().end()) return it->second; }
    return nullptr;
}
Entity dxcc_entity(const std::string& in) {
    std::string call; for (char ch : in) call += char(std::toupper(static_cast<unsigned char>(ch)));
    Entity out; if (call.empty() || call[0] == '<') return out;                                       // a hashed call: unknown
    auto from_exact = [&](const std::string& c) { const auto it = exact_index().find(c); if (it == exact_index().end()) return false;
        out = {true, it->second->dxcc, it->second->cont, it->second->cq, it->second->itu}; return true; };
    if (from_exact(call)) return out;
    std::vector<std::string> parts; { size_t a = 0; for (;;) { const size_t b = call.find('/', a); parts.push_back(call.substr(a, b - a)); if (b == std::string::npos) break; a = b + 1; } }
    std::vector<std::string> keep;
    for (const auto& p : parts) {
        if (p == "MM" || p == "AM") return out;                                                        // maritime / aeronautical mobile: no entity
        if (p.empty() || p == "P" || p == "M" || p == "QRP" || p == "A" || p == "B" || p == "LH" || (p.size() == 1 && std::isdigit(static_cast<unsigned char>(p[0])))) continue;
        keep.push_back(p);
    }
    if (keep.empty() || keep.size() > 2) return out;
    std::string pre = keep[0];
    if (keep.size() == 2) pre = keep[1].size() < keep[0].size() ? keep[1] : keep[0];               // two parts: the shorter is the prefix (EA8/G4ABC, G4ABC/EA8)
    else if (from_exact(pre)) return out;                                                              // "G4ABC/P": the base call may be an exact entry
    const DxccEntry* e = dxcc_lookup(pre); if (!e) return out;
    out = {true, e->dxcc, e->cont, e->cq, e->itu}; return out;
}
namespace {
bool callsign_shaped(const std::string& w) {                       // WSJT-X's callok: the call-area digit 2nd or 3rd, a letter before it, letters after it
    const int n = int(w.size()); if (n < 3) return false;
    int i0 = -1; for (int i = n - 1; i >= 0; --i) if (std::isdigit(static_cast<unsigned char>(w[size_t(i)]))) { i0 = i; break; }
    if (i0 != 1 && i0 != 2) return false;
    bool letter = false; for (int i = 0; i < i0; ++i) if (std::isalpha(static_cast<unsigned char>(w[size_t(i)]))) letter = true;
    if (!letter) return false;
    for (int i = i0 + 1; i < n; ++i) if (!std::isalpha(static_cast<unsigned char>(w[size_t(i)]))) return false;
    return true;
}
}  // namespace
bool callsign_prefix_known(const std::string& in) {
    if (in.empty() || in[0] == '<') return true;                                                      // a hashed call: nothing to check
    std::string call; for (char ch : in) call += char(std::toupper(static_cast<unsigned char>(ch)));
    if (exact_index().count(call)) return true;
    size_t a = 0;
    for (;;) {
        const size_t b = call.find('/', a); const std::string p = call.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (callsign_shaped(p) && !exact_index().count(p) && !dxcc_lookup(p)) return false;
        if (b == std::string::npos) break; a = b + 1;
    }
    return true;
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
