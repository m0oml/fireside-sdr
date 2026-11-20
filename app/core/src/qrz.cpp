#include "fireside/qrz.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace fireside::qrz {
namespace {
std::string unescape(std::string s) {
    static const std::pair<const char*, const char*> ent[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}, {"&amp;", "&"}};
    for (auto& [e, r] : ent) for (size_t p = 0; (p = s.find(e, p)) != std::string::npos; p += std::char_traits<char>::length(r)) s.replace(p, std::char_traits<char>::length(e), r);
    return s;
}
std::string trim(std::string s) { while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; return s.substr(i); }
// the text of <tag>..</tag> inside `xml` (case-insensitive tag names: QRZ's are lower case inside <Callsign>, capitalised in <Session>)
std::string tag(const std::string& xml, const std::string& name) {
    std::string lx = xml; std::transform(lx.begin(), lx.end(), lx.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string ln = name; std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const std::string open = "<" + ln + ">", close = "</" + ln + ">";
    const size_t a = lx.find(open); if (a == std::string::npos) return "";
    const size_t b = lx.find(close, a + open.size()); if (b == std::string::npos) return "";
    return trim(unescape(xml.substr(a + open.size(), b - a - open.size())));
}
std::string section(const std::string& xml, const std::string& name) {
    const size_t a = xml.find("<" + name + ">"); if (a == std::string::npos) return "";
    const size_t b = xml.find("</" + name + ">", a); return b == std::string::npos ? "" : xml.substr(a, b - a);
}
std::string enc(const std::string& v) {
    std::string o; char b[4];
    for (unsigned char c : v) { if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c); else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; } }
    return o;
}
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
std::string upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
bool has_digit(const std::string& s) { return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }
}  // namespace

bool Reply::session_expired() const { return error.find("Session Timeout") != std::string::npos || error.find("Invalid session key") != std::string::npos; }
bool Reply::not_found() const { return error.rfind("Not found", 0) == 0; }

Reply parse_reply(const std::string& xml) {
    Reply r; const std::string s = section(xml, "Session");
    r.key = tag(s, "Key"); r.error = tag(s, "Error"); r.message = tag(s, "Message"); r.sub_exp = tag(s, "SubExp");
    if (const std::string c = tag(s, "Count"); !c.empty()) r.count = std::atoi(c.c_str());
    const std::string cs = section(xml, "Callsign");
    if (!cs.empty()) {
        Info i; i.call = upper(tag(cs, "call")); i.first_name = tag(cs, "fname"); i.name = tag(cs, "name"); i.town = tag(cs, "addr2"); i.state = tag(cs, "state");
        i.country = tag(cs, "country"); i.grid = tag(cs, "grid"); i.qsl_manager = tag(cs, "qslmgr");
        if (const std::string v = tag(cs, "lat"); !v.empty()) i.lat = std::atof(v.c_str());
        if (const std::string v = tag(cs, "lon"); !v.empty()) i.lon = std::atof(v.c_str());
        i.dxcc = std::atoi(tag(cs, "dxcc").c_str()); i.cq_zone = std::atoi(tag(cs, "cqzone").c_str()); i.itu_zone = std::atoi(tag(cs, "ituzone").c_str());
        i.lotw = tag(cs, "lotw") == "1"; i.eqsl = tag(cs, "eqsl") == "1"; i.paper_qsl = tag(cs, "mqsl") == "1";
        if (!i.call.empty()) r.info = i;
    }
    return r;
}

std::string login_query(const std::string& user, const std::string& password, const std::string& agent) { return "?username=" + enc(user) + ";password=" + enc(password) + ";agent=" + enc(agent); }
std::string lookup_query(const std::string& key, const std::string& callsign) { return "?s=" + enc(key) + ";callsign=" + enc(upper(callsign)); }

std::string base_call(const std::string& decoded) {
    std::string t = upper(trim(decoded)); if (t.size() >= 2 && t.front() == '<' && t.back() == '>') t = t.substr(1, t.size() - 2);
    if (t.empty() || t.find("...") != std::string::npos) return "";
    std::vector<std::string> parts; std::stringstream ss(t); std::string p; while (std::getline(ss, p, '/')) if (!p.empty()) parts.push_back(p);
    std::string best; for (auto& x : parts) if (has_digit(x) && std::isalpha(static_cast<unsigned char>(x.back())) && x.size() >= best.size()) best = x;   // the longest part that looks like a whole call (a tie: the later one, PREFIX/CALL)
    if (best.empty() || best.size() < 3 || best.size() > 10) return "";
    for (char c : best) if (!std::isalnum(static_cast<unsigned char>(c))) return "";
    return best;
}

std::string cache_line(const Info& i) {
    std::ostringstream o; o.precision(6); o << std::fixed;
    o << clean(i.call) << '\t' << i.fetched_utc << '\t' << (i.not_found ? 1 : 0) << '\t' << clean(i.first_name) << '\t' << clean(i.name) << '\t' << clean(i.town) << '\t' << clean(i.state) << '\t'
      << clean(i.country) << '\t' << clean(i.grid) << '\t';
    if (i.lat) o << *i.lat;
    o << '\t';
    if (i.lon) o << *i.lon;
    o << '\t' << i.dxcc << '\t' << i.cq_zone << '\t' << i.itu_zone << '\t' << (i.lotw ? 1 : 0) << '\t' << (i.eqsl ? 1 : 0) << '\t' << (i.paper_qsl ? 1 : 0) << '\t' << clean(i.qsl_manager);
    return o.str();
}
std::optional<Info> parse_cache_line(const std::string& line) {
    std::vector<std::string> f; size_t a = 0; while (true) { const size_t b = line.find('\t', a); f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    while (f.size() > 0 && f.size() < 18 && f.size() >= 9) f.push_back("");                // tolerate trailing empty fields lost by an editor
    if (f.size() != 18 || base_call(f[0]) != f[0]) return std::nullopt;
    Info i; i.call = f[0]; i.fetched_utc = std::atoll(f[1].c_str()); i.not_found = f[2] == "1"; i.first_name = f[3]; i.name = f[4]; i.town = f[5]; i.state = f[6]; i.country = f[7]; i.grid = f[8];
    if (!f[9].empty()) i.lat = std::atof(f[9].c_str());
    if (!f[10].empty()) i.lon = std::atof(f[10].c_str());
    i.dxcc = std::atoi(f[11].c_str()); i.cq_zone = std::atoi(f[12].c_str()); i.itu_zone = std::atoi(f[13].c_str()); i.lotw = f[14] == "1"; i.eqsl = f[15] == "1"; i.paper_qsl = f[16] == "1"; i.qsl_manager = f[17];
    if (i.fetched_utc <= 0) return std::nullopt;
    return i;
}
bool fresh(const Info& i, int64_t now) { const int64_t age = now - i.fetched_utc; return age >= 0 && age < (i.not_found ? 3 : 30) * 86400LL; }

bool grid_latlon(const std::string& g0, double& lat, double& lon) {
    std::string g = upper(trim(g0));
    if (g.size() == 8 || g.size() == 10) { if (!std::isdigit(static_cast<unsigned char>(g[6])) || !std::isdigit(static_cast<unsigned char>(g[7]))) return false; g.resize(6); }   // extended squares (JN97RI23): the 6-character square is plenty
    if (g.size() != 2 && g.size() != 4 && g.size() != 6) return false;
    if (g == "RR73") return false;                                   // a valid square in the Arctic, but in FT8 it is always the message
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return false;
    lon = (g[0] - 'A') * 20.0 - 180; lat = (g[1] - 'A') * 10.0 - 90; double wl = 20, wa = 10;
    if (g.size() >= 4) { if (!std::isdigit(static_cast<unsigned char>(g[2])) || !std::isdigit(static_cast<unsigned char>(g[3]))) return false; lon += (g[2] - '0') * 2.0; lat += (g[3] - '0') * 1.0; wl = 2; wa = 1; }
    if (g.size() == 6) { if (g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X') return false; lon += (g[4] - 'A') * (2.0 / 24); lat += (g[5] - 'A') * (1.0 / 24); wl = 2.0 / 24; wa = 1.0 / 24; }
    lon += wl / 2; lat += wa / 2; return true;
}
void distance_bearing(double lat1, double lon1, double lat2, double lon2, double& km, double& deg) {
    const double r = M_PI / 180, p1 = lat1 * r, p2 = lat2 * r, dl = (lon2 - lon1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1 - a)));
    deg = std::fmod(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r + 360.0, 360.0);
}
std::string short_text(const Info& i, const std::string& my_grid, const std::string& heard_grid) {
    std::vector<std::string> f;
    if (!i.not_found) {
        const std::string who = !i.first_name.empty() ? i.first_name : i.name; if (!who.empty()) f.push_back(who);
        if (!i.town.empty()) f.push_back(i.town.size() > 24 ? i.town.substr(0, 24) : i.town);
        if (!i.country.empty()) f.push_back(i.country);
    }
    double la1, lo1, la2 = 0, lo2 = 0; bool theirs = false, from_qrz = false;
    if (grid_latlon(heard_grid, la2, lo2) && heard_grid.size() >= 4) theirs = true;                     // what they sent: first choice
    else if (!i.not_found) {
        if (i.lat && i.lon && (*i.lat != 0 || *i.lon != 0)) { la2 = *i.lat; lo2 = *i.lon; theirs = true; } else theirs = grid_latlon(i.grid, la2, lo2);
        from_qrz = theirs;
    }
    if (theirs && grid_latlon(my_grid, la1, lo1)) { double km, deg; distance_bearing(la1, lo1, la2, lo2, km, deg); char b[40]; std::snprintf(b, sizeof b, from_qrz ? "%.0f km (QRZ)" : "%.0f km", km); f.push_back(b); }
    std::string o; for (size_t k = 0; k < f.size(); ++k) o += (k ? ", " : "") + f[k];
    return o;
}

std::map<std::string, std::string> parse_env(const std::string& text) {
    std::map<std::string, std::string> m; std::stringstream ss(text); std::string l;
    while (std::getline(ss, l)) { const size_t eq = l.find('='); if (eq == std::string::npos) continue; const std::string k = trim(l.substr(0, eq)); if (!k.empty() && k[0] != '#') m[k] = trim(l.substr(eq + 1)); }
    return m;
}
}  // namespace fireside::qrz
