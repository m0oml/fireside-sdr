#include "fireside/logbook.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

namespace fireside::logbook {
namespace {
struct Band { const char* name; double lo, hi; };
const Band kBands[] = {{"2190m", 135.7e3, 137.8e3}, {"630m", 472e3, 479e3}, {"160m", 1.8e6, 2.0e6}, {"80m", 3.5e6, 4.0e6}, {"60m", 5.06e6, 5.45e6}, {"40m", 7.0e6, 7.3e6}, {"30m", 10.1e6, 10.15e6},
                       {"20m", 14.0e6, 14.35e6}, {"17m", 18.068e6, 18.168e6}, {"15m", 21.0e6, 21.45e6}, {"12m", 24.89e6, 24.99e6}, {"10m", 28.0e6, 29.7e6}, {"6m", 50e6, 54e6}, {"4m", 70e6, 70.5e6},
                       {"2m", 144e6, 148e6}, {"70cm", 420e6, 450e6}};
std::string upper(std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
std::string utc_date(int64_t t) { std::time_t tt = time_t(t); std::tm g{}; gmtime_r(&tt, &g); char b[16]; std::strftime(b, sizeof b, "%Y%m%d", &g); return b; }
std::string utc_time(int64_t t) { std::time_t tt = time_t(t); std::tm g{}; gmtime_r(&tt, &g); char b[16]; std::strftime(b, sizeof b, "%H%M%S", &g); return b; }
}  // namespace

std::string band_for_hz(double hz) { for (const auto& b : kBands) if (hz >= b.lo && hz <= b.hi) return b.name; return ""; }

AdifMode adif_mode(const std::string& app_mode) {
    const std::string m = upper(app_mode);
    if (m == "USB" || m == "LSB" || m == "SSB") return {"SSB", ""};
    if (m == "PSK31" || m == "PSK") return {"PSK", "PSK31"};
    if (m == "CW" || m == "CWU" || m == "CWL") return {"CW", ""};
    if (m == "FT4") return {"MFSK", "FT4"};                           // ADIF: FT4 is a SUBMODE of MFSK (FT8 is a mode of its own); Wavelog stores and matches it so (2026-11-20)
    if (m == "AM" || m == "FM" || m == "RTTY" || m == "FT8") return {m, ""};
    return {m, ""};
}

bool valid_call(const std::string& c) {
    if (c.size() < 3 || c.size() > 14) return false;
    bool digit = false, letter = false;
    for (unsigned char ch : c) { if (std::isdigit(ch)) digit = true; else if (std::isalpha(ch)) letter = true; else if (ch != '/') return false; }
    return digit && letter;
}

namespace {
std::vector<std::string> words(const std::string& t) { std::vector<std::string> w; std::string cur; for (char c : t) { if (c == ' ' || c == '\t') { if (!cur.empty()) w.push_back(cur), cur.clear(); } else cur += c; } if (!cur.empty()) w.push_back(cur); return w; }
std::string plain_call(std::string w) { if (w.size() > 2 && w.front() == '<' && w.back() == '>') w = w.substr(1, w.size() - 2); return valid_call(w) ? w : ""; }   // "<DL2AL>" is a hashed call the decoder resolved; "<...>" is not usable
bool is_grid(const std::string& w) { return (w.size() == 4 || w.size() == 6) && w[0] >= 'A' && w[0] <= 'R' && w[1] >= 'A' && w[1] <= 'R' && std::isdigit(static_cast<unsigned char>(w[2])) && std::isdigit(static_cast<unsigned char>(w[3])) && w != "RR73"; }
bool is_report(const std::string& w, std::string* out) {
    size_t i = (!w.empty() && w[0] == 'R') ? 1 : 0; if (w.size() != i + 3 || (w[i] != '+' && w[i] != '-') || !std::isdigit(static_cast<unsigned char>(w[i + 1])) || !std::isdigit(static_cast<unsigned char>(w[i + 2]))) return false;
    if (out) *out = w.substr(i); return true;
}
}  // namespace

FtxMessage parse_ftx_message(const std::string& text) {
    FtxMessage m; auto w = words(text); if (w.size() < 2) return m;
    size_t i = 0;
    if (w[0] == "CQ") {
        m.cq = true; i = 1; while (i < w.size() && plain_call(w[i]).empty() && w[i].find_first_of("0123456789") == std::string::npos && w[i].size() <= 4) ++i;        // CQ DX / CQ RU / CQ POTA modifiers
        if (i >= w.size() || plain_call(w[i]).empty()) return m;
        m.from = plain_call(w[i++]); for (; i < w.size(); ++i) if (is_grid(w[i])) m.grid = w[i];
        m.ok = true; return m;
    }
    m.to = plain_call(w[0]); m.from = plain_call(w[1]); if (m.from.empty()) return FtxMessage{};
    for (size_t k = 2; k < w.size(); ++k) { if (is_grid(w[k])) m.grid = w[k]; else is_report(w[k], &m.report); }
    m.ok = true; return m;
}
std::string ftx_line_text(const std::string& line, int* snr) {
    const size_t tilde = line.find(" ~"); if (tilde == std::string::npos || tilde < 6) return "";
    std::string text = line.substr(tilde + 2);
    if (const size_t sep = text.find("  \xE2\x94\x82"); sep != std::string::npos) text.resize(sep);                 /* the GUI's appended callbook info ("  │ Klaus, Ulm, ...") */ while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin()); while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (text.size() > 3 && text[text.size() - 3] == ' ' && text[text.size() - 2] == 'a' && std::isdigit(static_cast<unsigned char>(text.back()))) text.resize(text.size() - 3);   // a-priori marker (" a1".. " a7", WSJT-X)
    else if (text.size() > 2 && text[text.size() - 2] == ' ' && text.back() == '?') text.resize(text.size() - 2);
    if (snr) { auto w = words(line.substr(0, tilde)); *snr = w.size() >= 4 ? std::atoi(w[1].c_str()) : 0; }
    return text;
}

std::string adif_field(const std::string& name, const std::string& value) { return value.empty() ? "" : "<" + name + ":" + std::to_string(value.size()) + ">" + value; }

std::string adif_record(const Qso& q) {
    if (!valid_call(q.call)) throw std::invalid_argument("QSO needs a callsign");
    if (q.time_on_utc <= 0) throw std::invalid_argument("QSO needs a time");
    const std::string band = band_for_hz(q.freq_hz);
    if (band.empty()) throw std::invalid_argument("QSO frequency is outside the amateur bands");
    if (q.mode.empty()) throw std::invalid_argument("QSO needs a mode");
    const AdifMode am = adif_mode(q.mode); const int64_t off = q.time_off_utc > 0 ? q.time_off_utc : q.time_on_utc;
    char f[32]; std::snprintf(f, sizeof f, "%.6f", q.freq_hz / 1e6);
    std::string r;
    r += adif_field("CALL", upper(q.call)) + adif_field("BAND", band) + adif_field("FREQ", f) + adif_field("MODE", am.mode) + adif_field("SUBMODE", am.submode);
    r += adif_field("QSO_DATE", utc_date(q.time_on_utc)) + adif_field("TIME_ON", utc_time(q.time_on_utc)) + adif_field("QSO_DATE_OFF", utc_date(off)) + adif_field("TIME_OFF", utc_time(off));
    r += adif_field("RST_SENT", q.rst_sent) + adif_field("RST_RCVD", q.rst_rcvd) + adif_field("NAME", q.name) + adif_field("GRIDSQUARE", upper(q.grid)) + adif_field("COMMENT", q.comment);
    r += adif_field("STATION_CALLSIGN", upper(q.station_callsign)) + adif_field("MY_GRIDSQUARE", upper(q.my_grid)) + adif_field("OPERATOR", upper(q.operator_call));
    if (q.tx_pwr_w > 0) { char pw[16]; std::snprintf(pw, sizeof pw, q.tx_pwr_w < 10 ? "%.1f" : "%.0f", q.tx_pwr_w); r += adif_field("TX_PWR", pw); }
    return r + "<EOR>";
}

std::string adif_header(const std::string& program, const std::string& version) {
    return "FlexRadio app ADIF log\n" + adif_field("ADIF_VER", "3.1.4") + adif_field("PROGRAMID", program) + adif_field("PROGRAMVERSION", version) + "<EOH>\n";
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += char(c);
        }
    }
    return o;
}
std::string url_encode(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char c : s) { if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c); else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; } }
    return o;
}
std::string wavelog_adif_import_body(int id, const std::string& adif, bool dryrun) {
    return "{\"import_type\":\"adif\",\"station_profile_id\":" + std::to_string(id) + ",\"adif\":\"" + json_escape(adif) + "\"" + (dryrun ? ",\"dryrun\":true" : "") + "}";
}
std::string wavelog_radio_body(const std::string& radio, double freq_hz, const std::string& mode) {
    char f[32]; std::snprintf(f, sizeof f, "%.0f", freq_hz);
    return "{\"radio\":\"" + json_escape(radio) + "\",\"frequency\":" + f + ",\"mode\":\"" + json_escape(adif_mode(mode).mode) + "\"}";
}
std::string wavelog_history_path(const std::string& call) { return "/index.php/api/v2/qso?callsign=" + url_encode(upper(call)) + "&limit=1"; }
std::string wavelog_recent_path(int limit) { return "/index.php/api/v2/qso?limit=" + std::to_string(limit < 1 ? 1 : limit > 50 ? 50 : limit); }
std::string wavelog_lookup_path(const std::string& call, const std::string& band, const std::string& mode, bool callbook) {
    std::string p = "/index.php/api/v2/lookup?callsign=" + url_encode(upper(call)) + "&detail=full";
    if (!band.empty()) p += "&band=" + url_encode(band);
    if (!mode.empty()) p += "&mode=" + url_encode(adif_mode(mode).mode);
    if (callbook) p += "&callbook=true";
    return p;
}
std::string wavelog_normalise_base_url(const std::string& url) {
    size_t a = url.find_first_not_of(" \t\r\n"), b = url.find_last_not_of(" \t\r\n/");
    if (a == std::string::npos || b == std::string::npos || b < a) return "";
    const std::string u = url.substr(a, b - a + 1);
    if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return "";
    return u.find_first_of(" \"<>") == std::string::npos && u.size() > 8 ? u : "";
}

void append_adif_file(const std::string& path, const Qso& q, const std::string& program, const std::string& version) {
    const std::string rec = adif_record(q);                                   // validate before touching the file
    struct stat st{}; const bool fresh = ::stat(path.c_str(), &st) != 0 || st.st_size == 0;
    std::ofstream f(path, std::ios::app | std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    if (fresh) f << adif_header(program, version);
    f << rec << "\n"; f.flush();
    if (!f) throw std::runtime_error("cannot write " + path);
}
}  // namespace fireside::logbook
