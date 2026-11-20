#include "fireside/atu_memory.h"
#include <cstdio>
#include <sstream>
#include "fireside/tx_meter.h"

namespace fireside::atu {
void Memory::store(int band, const Setting& s) { if (band >= 0 && band < txm::kBands) m_[band] = s; }
std::optional<Setting> Memory::recall(int band) const { auto it = m_.find(band); if (it == m_.end()) return std::nullopt; return it->second; }

std::string Memory::to_text() const {
    std::ostringstream o;
    for (auto& [b, s] : m_) {
        char line[256];
        std::snprintf(line, sizeof line, "band=%s ind=%d cap=%d hiz=%d swr=%.2f fwd_w=%.2f freq_hz=%.0f utc=%s\n", txm::band_name(b), s.ind, s.cap, s.hiz ? 1 : 0,
                      s.swr, s.fwd_w, s.freq_hz, s.utc.empty() ? "-" : s.utc.c_str());
        o << line;
    }
    return o.str();
}

Memory Memory::from_text(const std::string& text, int* bad) {
    Memory m; int nbad = 0; std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos || line[0] == '#') continue;
        std::istringstream ls(line); std::string kv; Setting s; int band = -1; int have = 0; bool ok = true;
        while (ls >> kv) {
            const auto eq = kv.find('='); if (eq == std::string::npos) { ok = false; break; }
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            try {
                if (k == "band") { band = txm::band_from_name(v); have |= 1; }
                else if (k == "ind") { s.ind = std::stoi(v); have |= 2; }
                else if (k == "cap") { s.cap = std::stoi(v); have |= 4; }
                else if (k == "hiz") { s.hiz = std::stoi(v) != 0; have |= 8; }
                else if (k == "swr") s.swr = std::stod(v);
                else if (k == "fwd_w") s.fwd_w = std::stod(v);
                else if (k == "freq_hz") s.freq_hz = std::stod(v);
                else if (k == "utc") s.utc = v == "-" ? "" : v;
            } catch (...) { ok = false; break; }
        }
        if (!ok || have != 15 || band < 0 || s.ind < 0 || s.ind > 127 || s.cap < 0 || s.cap > 127) { ++nbad; continue; }
        m.store(band, s);
    }
    if (bad) *bad = nbad;
    return m;
}
}  // namespace fireside::atu
