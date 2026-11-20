#include "fireside/pskr.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "fireside/dxcc.h"
#include "fireside/logbook.h"
#include "fireside/qrz.h"
#include "fireside/qso_seq.h"
#include "fireside/tx_meter.h"
#include <sstream>

namespace fireside::pskr {
namespace {
std::string upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void put_str(std::vector<uint8_t>& v, const std::string& s) { put16(v, uint16_t(s.size())); v.insert(v.end(), s.begin(), s.end()); }
std::vector<uint8_t> frame(uint8_t first, const std::vector<uint8_t>& body) {      // fixed header: type/flags, remaining length (variable-length int)
    std::vector<uint8_t> out{first}; size_t n = body.size();
    do { uint8_t b = uint8_t(n % 128); n /= 128; if (n) b |= 0x80; out.push_back(b); } while (n);
    out.insert(out.end(), body.begin(), body.end()); return out;
}
}  // namespace

std::string sender_topic(const std::string& call) {
    const std::string c = upper(call);
    if (c.empty() || c.size() > 20) return "";
    for (char ch : c) if (!(std::isalnum(static_cast<unsigned char>(ch)))) return "";
    return "pskr/filter/v2/+/+/" + c + "/#";
}

namespace mqtt {
std::vector<uint8_t> connect(const std::string& id, uint16_t keepalive) {
    std::vector<uint8_t> b; put_str(b, "MQTT"); b.push_back(4);          // protocol level 4 = 3.1.1
    b.push_back(0x02);                                                     // clean session
    put16(b, keepalive); put_str(b, id); return frame(0x10, b);
}
std::vector<uint8_t> subscribe(uint16_t pid, const std::string& topic) {
    std::vector<uint8_t> b; put16(b, pid); put_str(b, topic); b.push_back(0); return frame(0x82, b);   // SUBSCRIBE has flags 0010
}
std::vector<uint8_t> pingreq() { return {0xC0, 0x00}; }
std::vector<uint8_t> disconnect() { return {0xE0, 0x00}; }

void Decoder::feed(const uint8_t* p, size_t n) { if (!broken_) buf_.insert(buf_.end(), p, p + n); }
std::optional<Packet> Decoder::next() {
    if (broken_ || buf_.size() < 2) return std::nullopt;
    size_t len = 0, mul = 1, i = 1;
    for (;; ++i) {
        if (i >= buf_.size()) return std::nullopt;                         // length not complete yet
        if (i > 4) { broken_ = true; return std::nullopt; }                // at most 4 length bytes
        len += size_t(buf_[i] & 0x7F) * mul; mul *= 128;
        if (!(buf_[i] & 0x80)) break;
    }
    const size_t hdr = i + 1; if (len > (1u << 20)) { broken_ = true; return std::nullopt; }   // a report is a few hundred bytes
    if (buf_.size() < hdr + len) return std::nullopt;
    Packet pk; const uint8_t t = buf_[0] >> 4; pk.flags = buf_[0] & 0x0F;
    const uint8_t* b = buf_.data() + hdr;
    switch (t) {
        case 2: pk.type = Type::Connack; if (len >= 2) pk.connack_code = b[1]; break;
        case 9: pk.type = Type::Suback; if (len >= 3) pk.suback_code = b[2]; break;
        case 13: pk.type = Type::Pingresp; break;
        case 3: {
            pk.type = Type::Publish; if (len < 2) { broken_ = true; return std::nullopt; }
            const size_t tl = size_t(b[0]) << 8 | b[1]; size_t off = 2 + tl; if (off > len) { broken_ = true; return std::nullopt; }
            pk.topic.assign(reinterpret_cast<const char*>(b + 2), tl);
            if ((pk.flags >> 1) & 3) off += 2;                                 // QoS 1/2 carry a packet id (we subscribe at QoS 0, but be safe)
            if (off > len) { broken_ = true; return std::nullopt; }
            pk.payload.assign(reinterpret_cast<const char*>(b + off), len - off); break;
        }
        default: pk.type = Type::Other; break;
    }
    buf_.erase(buf_.begin(), buf_.begin() + long(hdr + len));
    return pk;
}
}  // namespace mqtt

// A flat JSON object: string and number values only (what the feed sends); nested values are skipped.
std::optional<Spot> parse_spot(const std::string& j) {
    size_t i = 0; auto ws = [&] { while (i < j.size() && std::isspace(static_cast<unsigned char>(j[i]))) ++i; };
    auto str = [&](std::string& out) -> bool {
        if (i >= j.size() || j[i] != '"') return false; ++i; out.clear();
        while (i < j.size() && j[i] != '"') { if (j[i] == '\\' && i + 1 < j.size()) { ++i; out += j[i] == 'n' ? '\n' : j[i]; } else out += j[i]; ++i; }
        if (i >= j.size()) return false; ++i; return true;
    };
    ws(); if (i >= j.size() || j[i] != '{') return std::nullopt; ++i;
    std::map<std::string, std::string> sv; std::map<std::string, double> nv;
    for (;;) {
        ws(); if (i < j.size() && j[i] == '}') break;
        std::string key; if (!str(key)) return std::nullopt; ws(); if (i >= j.size() || j[i] != ':') return std::nullopt; ++i; ws();
        if (i < j.size() && j[i] == '"') { std::string v; if (!str(v)) return std::nullopt; sv[key] = v; }
        else if (i < j.size() && (j[i] == '{' || j[i] == '[')) { int depth = 0; do { if (j[i] == '{' || j[i] == '[') ++depth; else if (j[i] == '}' || j[i] == ']') --depth; ++i; } while (i < j.size() && depth > 0); }
        else { const size_t s0 = i; while (i < j.size() && j[i] != ',' && j[i] != '}') ++i; const std::string v = j.substr(s0, i - s0);
               char* end = nullptr; const double d = std::strtod(v.c_str(), &end); if (end != v.c_str()) nv[key] = d; }
        ws(); if (i < j.size() && j[i] == ',') { ++i; continue; } if (i < j.size() && j[i] == '}') break; return std::nullopt;
    }
    Spot s; auto S = [&](const char* k) { auto it = sv.find(k); return it == sv.end() ? std::string() : it->second; };
    auto N = [&](const char* k, double d) { auto it = nv.find(k); return it == nv.end() ? d : it->second; };
    s.sender = upper(S("sc")); s.receiver = upper(S("rc")); if (s.sender.empty() || s.receiver.empty()) return std::nullopt;
    s.sender_grid = S("sl"); s.receiver_grid = S("rl"); s.mode = S("md"); s.band = S("b");
    s.seq = uint64_t(std::max(0.0, N("sq", 0))); s.freq_hz = N("f", 0); s.t = int64_t(N("t", 0));
    if (nv.count("rp")) { s.snr = int(std::lround(nv["rp"])); s.has_snr = true; }
    s.sender_dxcc = int(N("sa", -1)); s.receiver_dxcc = int(N("ra", -1));
    return s;
}

Heard::Heard(std::string c, std::string g) { set_station(std::move(c), std::move(g)); }
void Heard::set_station(std::string c, std::string g) {
    call_ = upper(std::move(c)); if (g == grid_) return; grid_ = std::move(g);
    double la1, lo1; const bool mine = qrz::grid_latlon(grid_, la1, lo1);                  // my locator became known / changed: every row's distance again
    for (auto& [k, h] : rows_) { double la2, lo2; if (mine && qrz::grid_latlon(h.grid, la2, lo2)) qrz::distance_bearing(la1, lo1, la2, lo2, h.km, h.bearing); else h.km = h.bearing = -1; }
}
bool Heard::add(const Spot& s) {
    if (call_.empty() || upper(s.sender) != call_) return false;
    if (s.seq) { if (seen_.count(s.seq)) return false; seen_[s.seq] = s.t; }
    HeardBy& h = rows_[s.receiver + "|" + s.band];
    if (!h.count) { h.receiver = s.receiver; h.band = s.band; h.first_t = s.t; if (const auto* e = decode::dxcc_lookup(s.receiver)) h.country = e->name; }
    ++h.count; h.mode = s.mode; if (!s.receiver_grid.empty()) h.grid = s.receiver_grid;
    if (s.t >= h.last_t) { h.last_t = s.t; if (s.has_snr) h.last_snr = s.snr; }
    if (s.has_snr) h.best_snr = std::max(h.best_snr, s.snr);
    double la1, lo1, la2, lo2;
    if (qrz::grid_latlon(grid_, la1, lo1) && qrz::grid_latlon(h.grid, la2, lo2)) qrz::distance_bearing(la1, lo1, la2, lo2, h.km, h.bearing);
    return true;
}
std::vector<HeardBy> Heard::since(int64_t t0, const std::string& band) const {
    std::vector<HeardBy> v; for (const auto& [k, h] : rows_) if (h.last_t >= t0 && (band.empty() || h.band == band)) v.push_back(h);
    std::sort(v.begin(), v.end(), [](const HeardBy& a, const HeardBy& b) { return a.last_t != b.last_t ? a.last_t > b.last_t : a.receiver < b.receiver; });
    return v;
}
Heard::Summary Heard::summary(int64_t t0, const std::string& band) const {
    Summary s; std::map<std::string, bool> calls, countries;
    for (const auto& h : since(t0, band)) {
        calls[h.receiver] = true; if (!h.country.empty()) countries[h.country] = true; s.spots += h.count; ++s.by_band[h.band];
        if (h.km > s.max_km) { s.max_km = h.km; s.furthest = h.receiver; }
        if (h.best_snr > s.best_snr) { s.best_snr = h.best_snr; s.best = h.receiver; }
    }
    s.stations = int(calls.size()); s.countries = int(countries.size()); return s;
}
void Heard::prune(int64_t before) {
    for (auto it = rows_.begin(); it != rows_.end();) it = it->second.last_t < before ? rows_.erase(it) : std::next(it);
    for (auto it = seen_.begin(); it != seen_.end();) it = it->second < before ? seen_.erase(it) : std::next(it);
}

std::optional<Report> report_from_line(const std::string& line, double dial_hz, const std::string& mode, int64_t t, const std::string& my_call) {
    int snr = 0; const std::string text = logbook::ftx_line_text(line, &snr); if (text.empty()) return std::nullopt;
    std::istringstream is(line); std::string hhmmss, s_snr, s_dt, s_hz; if (!(is >> hhmmss >> s_snr >> s_dt >> s_hz)) return std::nullopt;
    char* end = nullptr; const double hz = std::strtod(s_hz.c_str(), &end); if (end == s_hz.c_str() || hz < 0 || hz > 5000) return std::nullopt;
    const seq::Heard h = seq::parse(text, snr); if (!h.ok || h.from.empty() || h.from.find('<') != std::string::npos || !logbook::valid_call(h.from)) return std::nullopt;
    if (!my_call.empty() && qrz::base_call(upper(h.from)) == qrz::base_call(upper(my_call))) return std::nullopt;
    Report r; r.call = upper(h.from); r.grid = h.grid; r.mode = mode; r.snr = snr; r.t = t;
    const double f = dial_hz + hz; if (!(f > 0 && f < 4.0e9)) return std::nullopt; r.freq_hz = uint32_t(std::lround(f)); return r;
}

namespace {
void p8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void p16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void p32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }
void pstr(std::vector<uint8_t>& v, const std::string& s) { const size_t n = std::min<size_t>(s.size(), 254); v.push_back(uint8_t(n)); v.insert(v.end(), s.begin(), s.begin() + long(n)); }
void field(std::vector<uint8_t>& v, uint16_t ie, uint16_t len) { p16(v, uint16_t(0x8000 | ie)); p16(v, len); p32(v, 30351); }   // enterprise-specific
void finish_set(std::vector<uint8_t>& v, size_t start) { while ((v.size() - start) % 4) v.push_back(0); const size_t n = v.size() - start; v[start + 2] = uint8_t(n >> 8); v[start + 3] = uint8_t(n); }
void templates(std::vector<uint8_t>& v) {
    size_t s = v.size(); p16(v, 3); p16(v, 0); p16(v, kReceiverTemplate); p16(v, 4); p16(v, 1);          // options template: 4 fields, 1 scope field
    field(v, 2, 0xFFFF); field(v, 4, 0xFFFF); field(v, 8, 0xFFFF); field(v, 9, 0xFFFF); finish_set(v, s);
    s = v.size(); p16(v, 2); p16(v, 0); p16(v, kSenderTemplate); p16(v, 7);
    field(v, 1, 0xFFFF); field(v, 5, 4); field(v, 6, 1); field(v, 10, 0xFFFF); field(v, 3, 0xFFFF); field(v, 11, 1); p16(v, 150); p16(v, 4);   // ..., flowStartSeconds
    finish_set(v, s);
}
size_t sender_record_size(const Report& r) { return 1 + std::min<size_t>(r.call.size(), 254) + 4 + 1 + 1 + std::min<size_t>(r.mode.size(), 254) + 1 + std::min<size_t>(r.grid.size(), 254) + 1 + 4; }
constexpr size_t kMaxDatagram = 1400;                    // under a typical MTU
}  // namespace

Uploader::Uploader(std::string c, std::string g, std::string sw, uint32_t obs, int64_t start, uint32_t seed) : software_(std::move(sw)), obs_(obs), rng_(seed ? seed : 1) {
    set_station(std::move(c), std::move(g)); next_ = start + 300 + jitter(60);
}
uint32_t Uploader::jitter(uint32_t max_s) { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_ % (max_s + 1); }
void Uploader::set_station(std::string c, std::string g) { call_ = upper(std::move(c)); grid_ = std::move(g); }
bool Uploader::add(const Report& r) {
    const int band = txm::band_of(double(r.freq_hz));
    const std::string key = r.call + "|" + (band >= 0 ? std::to_string(band) : std::to_string(r.freq_hz / 1000000));
    auto it = last_reported_.find(key); if (it != last_reported_.end() && r.t - it->second < 3600) return false;
    last_reported_[key] = r.t; pending_.push_back(r); return true;
}
bool Uploader::due(int64_t now) const {
    if (pending_.empty() || call_.empty() || grid_.empty()) return false;
    size_t bytes = 16 + 200; for (const auto& r : pending_) bytes += sender_record_size(r);
    return now >= next_ || bytes > kMaxDatagram;
}
std::vector<uint8_t> Uploader::build(int64_t now) {
    std::vector<uint8_t> v; if (pending_.empty() || call_.empty() || grid_.empty()) return v;
    p16(v, 10); p16(v, 0); p32(v, uint32_t(now)); p32(v, seq_); p32(v, obs_);
    if (datagrams_ < 3 || now - last_templates_ >= 3600) { templates(v); last_templates_ = now; }
    size_t s = v.size(); p16(v, kReceiverTemplate); p16(v, 0); pstr(v, call_); pstr(v, grid_); pstr(v, software_); pstr(v, antenna_); finish_set(v, s);
    s = v.size(); p16(v, kSenderTemplate); p16(v, 0); size_t n = 0;
    for (; n < pending_.size(); ++n) {
        const Report& r = pending_[n]; if (n && v.size() + sender_record_size(r) + 3 > kMaxDatagram) break;
        pstr(v, r.call); p32(v, r.freq_hz); p8(v, uint8_t(int8_t(std::clamp(r.snr, -128, 127)))); pstr(v, r.mode); pstr(v, r.grid); p8(v, 1); p32(v, uint32_t(r.t));
    }
    finish_set(v, s);
    v[2] = uint8_t(v.size() >> 8); v[3] = uint8_t(v.size());
    pending_.erase(pending_.begin(), pending_.begin() + long(n)); seq_ += uint32_t(n); ++datagrams_;
    next_ = now + 300 + jitter(60);
    for (auto it = last_reported_.begin(); it != last_reported_.end();) it = now - it->second > 7200 ? last_reported_.erase(it) : std::next(it);
    return v;
}

std::string csv_header() { return "utc_epoch,band,mode,freq_hz,snr,receiver,receiver_grid,km,bearing,antenna"; }
std::string csv_line(const Spot& s, double km, double bearing, const std::string& antenna) {
    std::string a; for (char c : antenna) if (c != ',' && c != '\n' && c != '\r') a += c;      // a note, not a field separator
    char b[256]; std::snprintf(b, sizeof b, "%lld,%s,%s,%.0f,%s,%s,%s,%s,%s,", static_cast<long long>(s.t), s.band.c_str(), s.mode.c_str(), s.freq_hz,
                               s.has_snr ? std::to_string(s.snr).c_str() : "", s.receiver.c_str(), s.receiver_grid.c_str(),
                               km >= 0 ? std::to_string(int(std::lround(km))).c_str() : "", bearing >= 0 ? std::to_string(int(std::lround(bearing))).c_str() : "");
    return std::string(b) + a;
}
}  // namespace fireside::pskr
