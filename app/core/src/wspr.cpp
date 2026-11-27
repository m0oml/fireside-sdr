#include "fireside/wspr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace fireside::wspr {
const uint8_t kSync[kSymbols] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,0,0};

namespace {
std::string upper(std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
int code_of(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'A' && c <= 'Z') return c - 'A' + 10; if (c == ' ') return 36; return -1; }
bool legal_dbm(int d) { const int r = d % 10; return d >= 0 && d <= 60 && (r == 0 || r == 3 || r == 7); }
// The callsign as six characters with the call-area digit third: a digit in 2nd place gets a leading space ("M0OML" -> " M0OML",
// "K1ABC" -> " K1ABC"); "VK2XYZ" and "2E0ABC" already have it third. Padded with spaces to six.
std::string six(const std::string& call) {
    std::string c = call; if (c.size() >= 2 && std::isdigit(static_cast<unsigned char>(c[1])) && !(c.size() >= 3 && std::isdigit(static_cast<unsigned char>(c[2])))) c = " " + c;
    while (c.size() < 6) c += ' ';
    return c;
}
int parity(uint32_t v) { return __builtin_parity(v); }
}  // namespace

bool plain_call(const std::string& call0) {
    const std::string c = upper(call0); if (c.empty() || c.size() > 6) return false;
    for (char ch : c) if (!std::isalnum(static_cast<unsigned char>(ch))) return false;
    const std::string s = six(c); if (s.size() != 6) return false;
    if (!(code_of(s[0]) >= 0 && code_of(s[0]) <= 36)) return false;                       // space, digit or letter
    if (!(code_of(s[1]) >= 0 && code_of(s[1]) <= 35)) return false;                       // digit or letter
    if (!std::isdigit(static_cast<unsigned char>(s[2]))) return false;                    // the call area digit, third
    for (int i = 3; i < 6; ++i) if (!(s[size_t(i)] == ' ' || (s[size_t(i)] >= 'A' && s[size_t(i)] <= 'Z'))) return false;   // the suffix: letters (or spaces)
    for (int i = 3; i < 5; ++i) if (s[size_t(i)] == ' ' && s[size_t(i + 1)] != ' ') return false;                            // no gap inside the suffix
    return true;
}

Symbols encode(const std::string& call0, const std::string& grid0, int dbm) {
    const std::string call = upper(call0), grid = upper(grid0);
    if (!plain_call(call)) throw std::invalid_argument("WSPR: \"" + call0 + "\" is not a plain callsign (a compound call with / needs a type 2/3 message)");
    if (grid.size() != 4 || grid[0] < 'A' || grid[0] > 'R' || grid[1] < 'A' || grid[1] > 'R' || !std::isdigit(static_cast<unsigned char>(grid[2])) || !std::isdigit(static_cast<unsigned char>(grid[3])))
        throw std::invalid_argument("WSPR: \"" + grid0 + "\" is not a 4-character grid (e.g. IO91)");
    if (!legal_dbm(dbm)) throw std::invalid_argument("WSPR: " + std::to_string(dbm) + " dBm is not a WSPR power (0..60 ending in 0, 3 or 7)");
    const std::string s = six(call);
    uint32_t n = uint32_t(code_of(s[0]));
    n = n * 36 + uint32_t(code_of(s[1]));
    n = n * 10 + uint32_t(code_of(s[2]));
    for (int i = 3; i < 6; ++i) n = n * 27 + uint32_t(code_of(s[size_t(i)]) - 10);       // A..Z -> 0..25, space (36) -> 26
    uint32_t m = uint32_t((179 - 10 * (grid[0] - 'A') - (grid[2] - '0')) * 180 + 10 * (grid[1] - 'A') + (grid[3] - '0'));
    m = m * 128 + uint32_t(dbm + 64);
    uint8_t c[11] = {};
    c[0] = uint8_t(n >> 20); c[1] = uint8_t(n >> 12); c[2] = uint8_t(n >> 4); c[3] = uint8_t(((n & 0x0f) << 4) | ((m >> 18) & 0x0f));
    c[4] = uint8_t(m >> 10); c[5] = uint8_t(m >> 2); c[6] = uint8_t((m & 0x03) << 6);
    uint8_t coded[kSymbols]; size_t k = 0; uint32_t reg = 0;
    for (int i = 0; i < 81; ++i) {                                                            // 50 message bits + 31 zero tail bits
        const int bit = (c[i / 8] >> (7 - i % 8)) & 1; reg = (reg << 1) | uint32_t(bit);
        coded[k++] = uint8_t(parity(reg & 0xF2D05351u)); coded[k++] = uint8_t(parity(reg & 0xE4613C47u));
    }
    uint8_t inter[kSymbols] = {}; size_t p = 0;
    for (int i = 0; i < 256 && p < size_t(kSymbols); ++i) {
        int j = 0; for (int b = 0; b < 8; ++b) if (i & (1 << b)) j |= 1 << (7 - b);         // bit-reversed index
        if (j < kSymbols) inter[j] = coded[p++];
    }
    Symbols out{}; for (int i = 0; i < kSymbols; ++i) out[size_t(i)] = uint8_t(kSync[i] + 2 * inter[i]);
    return out;
}

int dbm_for_watts(double w) {
    if (!(w > 0)) throw std::invalid_argument("WSPR: power must be above 0 W");
    const double d = 10 * std::log10(w * 1000); int best = 0; double err = 1e9;
    for (int v = 0; v <= 60; ++v) if (legal_dbm(v) && std::fabs(v - d) < err) { err = std::fabs(v - d); best = v; }
    return best;
}
double watts_for_dbm(int dbm) { return std::pow(10.0, dbm / 10.0) / 1000.0; }

std::vector<std::complex<float>> synth_iq(const Symbols& s, double audio_hz, double fs, double lead_s, double total_s) {
    const double sym_n = kSymbolS * fs; const size_t nsig = size_t(std::llround(kSymbols * sym_n)), at = size_t(std::llround(std::max(0.0, lead_s) * fs));
    const size_t total = std::max(at + nsig, total_s > 0 ? size_t(std::llround(total_s * fs)) : size_t(0));
    std::vector<std::complex<float>> out(total, std::complex<float>(0, 0));
    const size_t ramp = size_t(fs * 0.02); double phi = 0;                                   // 20 ms raised-cosine ends: no key clicks
    for (size_t k = 0; k < nsig; ++k) {
        const size_t sym = std::min(size_t(kSymbols - 1), size_t(double(k) / sym_n));
        double e = 1; if (k < ramp) e = (1 - std::cos(M_PI * double(k) / double(ramp))) / 2; else if (k + ramp >= nsig) e = (1 - std::cos(M_PI * double(nsig - 1 - k) / double(ramp))) / 2;
        out[at + k] = std::complex<float>(float(e * std::cos(phi)), float(e * std::sin(phi)));
        phi = std::fmod(phi + 2 * M_PI * (audio_hz + kToneSpacingHz * s[sym]) / fs, 2 * M_PI);
    }
    return out;
}
std::vector<float> synth_audio(const Symbols& s, double audio_hz, double fs, double amplitude, double lead_s, double total_s) {
    const auto iq = synth_iq(s, audio_hz, fs, lead_s, total_s); std::vector<float> a(iq.size());
    for (size_t i = 0; i < iq.size(); ++i) a[i] = float(amplitude) * iq[i].real();
    return a;
}

std::vector<uint8_t> wav16(const std::vector<float>& x, int rate) {
    std::vector<uint8_t> b; auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }; auto u16 = [&](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    const uint32_t data = uint32_t(x.size() * 2);
    b.insert(b.end(), {'R', 'I', 'F', 'F'}); u32(36 + data); b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); u32(16); u16(1); u16(1); u32(uint32_t(rate)); u32(uint32_t(rate) * 2); u16(2); u16(16);
    b.insert(b.end(), {'d', 'a', 't', 'a'}); u32(data);
    for (float v : x) { const int s = int(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); u16(uint16_t(int16_t(s))); }
    return b;
}

std::optional<Spot> parse_wsprd_line(const std::string& line) {
    std::istringstream is(line); Spot s; std::string t;
    if (!(is >> s.hhmm) || s.hhmm.size() != 4 || !std::all_of(s.hhmm.begin(), s.hhmm.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return std::nullopt;
    if (!(is >> s.snr >> s.dt >> s.mhz >> s.drift)) return std::nullopt;
    std::vector<std::string> w; while (is >> t) w.push_back(t);
    if (w.size() < 2 || !(s.mhz > 0.1)) return std::nullopt;
    try { size_t used = 0; s.dbm = std::stoi(w.back(), &used); if (used != w.back().size()) return std::nullopt; } catch (...) { return std::nullopt; }
    s.call = w[0]; if (w.size() >= 3) s.grid = w[1];
    if (s.call.empty() || s.call.size() > 14) return std::nullopt;
    return s;
}
std::string format(const Spot& s) {
    char b[160]; std::snprintf(b, sizeof b, "%s %4d %4.1f %10.6f %3d  %s%s%s %d", s.hhmm.c_str(), s.snr, s.dt, s.mhz, s.drift, s.call.c_str(), s.grid.empty() ? "" : " ", s.grid.c_str(), s.dbm);
    return b;
}

double dial_hz_for(double hz) {
    struct B { double lo, hi, dial; };
    static const B k[] = {{1.81e6, 2.0e6, 1.8366e6}, {3.5e6, 3.8e6, 3.5686e6}, {5.2585e6, 5.4065e6, 5.2872e6}, {7.0e6, 7.2e6, 7.0386e6}, {10.1e6, 10.15e6, 10.1387e6},
                          {14.0e6, 14.35e6, 14.0956e6}, {18.068e6, 18.168e6, 18.1046e6}, {21.0e6, 21.45e6, 21.0946e6}, {24.89e6, 24.99e6, 24.9246e6},
                          {28.0e6, 29.7e6, 28.1246e6}, {50.0e6, 52.0e6, 50.293e6}};
    for (const auto& b : k) if (hz >= b.lo && hz <= b.hi) return b.dial;
    return 0;
}

std::string form_encode(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o += char(c);
        else if (c == ' ') o += '+';
        else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
namespace {
std::string mhz6(double v) { char b[32]; std::snprintf(b, sizeof b, "%.6f", v); return b; }
std::string body(const std::vector<std::pair<std::string, std::string>>& kv) { std::string o; for (const auto& [k, v] : kv) { if (!o.empty()) o += '&'; o += k + "=" + form_encode(v); } return o; }
}  // namespace
std::string wsprnet_spot_body(const Spot& s, const std::string& date, const std::string& rcall, const std::string& rgrid, double dial_mhz, const std::string& version) {
    char dt[16]; std::snprintf(dt, sizeof dt, "%.1f", s.dt);
    return body({{"function", "wspr"}, {"rcall", upper(rcall)}, {"rgrid", rgrid}, {"rqrg", mhz6(dial_mhz)}, {"date", date}, {"time", s.hhmm}, {"sig", std::to_string(s.snr)},
                 {"dt", dt}, {"drift", std::to_string(s.drift)}, {"tqrg", mhz6(s.mhz)}, {"tcall", s.call}, {"tgrid", s.grid}, {"dbm", std::to_string(s.dbm)},
                 {"version", version}, {"mode", "2"}});
}
std::string wsprnet_stat_body(const std::string& rcall, const std::string& rgrid, double dial_mhz, int tx_percent, double tx_mhz, int dbm, const std::string& version) {
    return body({{"function", "wsprstat"}, {"rcall", upper(rcall)}, {"rgrid", rgrid}, {"rqrg", mhz6(dial_mhz)}, {"tpct", std::to_string(tx_percent)}, {"tqrg", mhz6(tx_mhz)},
                 {"dbm", std::to_string(dbm)}, {"version", version}, {"mode", "2"}});
}
}  // namespace fireside::wspr
