#include "fireside/award.h"
#include <cctype>
#include "fireside/dxcc.h"

namespace fireside::award {
const char* tag_text(int t) {
    switch (t) {
        case 0: return "NEW"; case 1: return "new band/mode"; case 2: return "worked";
        case kNewDxcc: return "NEW DXCC"; case kNewDxccBand: return "DXCC new band"; case kNewCont: return "new continent"; case kNewCq: return "new CQ zone";
        case kNewItu: return "new ITU zone"; case kNewGrid: return "new grid"; case kNewGridBand: return "grid new band";
        default: return "";
    }
}
std::string grid4(const std::string& g) {
    if (g.size() < 4) return "";
    const char a = char(std::toupper(static_cast<unsigned char>(g[0]))), b = char(std::toupper(static_cast<unsigned char>(g[1])));
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || !std::isdigit(static_cast<unsigned char>(g[2])) || !std::isdigit(static_cast<unsigned char>(g[3]))) return "";
    if (a == 'R' && b == 'R' && g[2] == '7' && g[3] == '3') return "";                                   // RR73 is a sign-off, not the square in the Arctic Ocean
    return std::string{a, b, g[2], g[3]};
}
std::string norm_band(const std::string& band) { std::string o; for (char c : band) if (!std::isspace(static_cast<unsigned char>(c))) o += char(std::tolower(static_cast<unsigned char>(c))); return o; }

void WorkedIndex::clear() { *this = WorkedIndex(); }
void WorkedIndex::add(const std::string& call, const std::string& band_in, const std::string& grid, bool count) {
    if (count) ++qsos_; const std::string band = norm_band(band_in);
    const decode::Entity e = decode::dxcc_entity(call);
    if (e.ok) { dxcc_.insert(e.dxcc); dxcc_band_.insert({e.dxcc, band}); cont_.insert(e.cont); cq_.insert(e.cq); itu_.insert(e.itu); }
    const std::string g = grid4(grid); if (!g.empty()) { grid_.insert(g); grid_band_.insert({g, band}); }
}
int WorkedIndex::tag(const std::string& call, const std::string& band_in, const std::string& grid, const Options& o) const {
    const std::string band = norm_band(band_in);
    const decode::Entity e = decode::dxcc_entity(call); const std::string g = grid4(grid);
    if (e.ok) {
        if (o.dxcc && !dxcc_.count(e.dxcc)) return kNewDxcc;
        if (o.dxcc_band && !dxcc_band_.count({e.dxcc, band})) return kNewDxccBand;
        if (o.cont && !cont_.count(e.cont)) return kNewCont;
        if (o.cq && !cq_.count(e.cq)) return kNewCq;
        if (o.itu && !itu_.count(e.itu)) return kNewItu;
    }
    if (!g.empty()) {
        if (o.grid && !grid_.count(g)) return kNewGrid;
        if (o.grid_band && !grid_band_.count({g, band})) return kNewGridBand;
    }
    return kNone;
}
}  // namespace fireside::award
