#include "fireside/eeprom.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <unistd.h>
#include "fireside/identify.h"

namespace fireside {
namespace {
constexpr uint8_t kChecksumPresent = 0xC5;
constexpr uint32_t kEndOfTable = 0xDEADBEEF;
constexpr unsigned kTxCarrierPtr = 0x62, kTxCarrierAddr = 0x760;

struct TableSpec { const char* name; unsigned ptr, addr, rows, per, csum; bool has_range; double lo, hi; bool trx, rx2; };
// name, pointer addr, table addr, rows, floats per row, checksum addr, plausible range, in TRX, in RX2   (transcribed from PowerSDR fwc_eeprom.cs)
constexpr TableSpec kTables[] = {
    {"RX level",       0x48, 0x100, 11, 3,  0x18F, false, 0, 0,     true, true},
    {"RX image gain",  0x4A, 0x190, 11, 1,  0x1BF, true, -500, 500, true, true},
    {"RX image phase", 0x4C, 0x1C0, 11, 1,  0x1EF, true, -500, 500, true, true},
    {"TX image gain",  0x4E, 0x1F0, 11, 1,  0x21F, true, -500, 500, true, false},
    {"TX image phase", 0x50, 0x220, 11, 1,  0x24F, true, -500, 500, true, false},
    {"PA bridge",      0x56, 0x2E0, 11, 6,  0x3EF, true, 0.0, 2.5,  true, false},
    {"PA power",       0x58, 0x3F0, 11, 13, 0x62F, true, 0.0, 1.0,  true, false},
    {"PA SWR",         0x5A, 0x630, 11, 1,  0x65F, true, 0.2, 5.0,  true, false},   // PA SWR: per-band reflected-power correction (PowerSDR swr_table), not an SWR: 0.46 on 6 m is normal
    {"ATU SWR",        0x5C, 0x660, 11, 1,  0x68F, true, 0.5, 5.0,  true, false},
};
constexpr unsigned kUnverifiedCount = 3;                          // PA bias, VU level, VU power: copied, checksum rule not plain XOR

uint16_t u16(const std::vector<uint8_t>& d, size_t o) { return uint16_t(d[o] | d[o + 1] << 8); }
uint32_t u32(const std::vector<uint8_t>& d, size_t o) { return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24; }
float f32(const std::vector<uint8_t>& d, size_t o) { uint32_t u = u32(d, o); float f; std::memcpy(&f, &u, 4); return f; }
double round6(double x) { return std::isfinite(x) ? std::round(x * 1e6) / 1e6 : x; }

// FILETIME (100 ns ticks since 1601-01-01 UTC) -> ISO 8601 with +00:00, as Python's datetime.isoformat().
std::optional<std::string> filetime_iso(uint64_t ft) {
    if (ft == 0 || ft == UINT64_MAX) return std::nullopt;
    const uint64_t us_total = (ft + 5) / 10;                      // microseconds since 1601
    const uint64_t days = us_total / 86400000000ull, rem_us = us_total % 86400000000ull;
    long long z = (long long)days - 134774 + 719468;              // days from 1601-01-01 -> civil date (Hinnant's algorithm)
    const long long era = (z >= 0 ? z : z - 146096) / 146097; const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400; const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    if (y > 9999) return std::nullopt;                            // Python raises OverflowError -> None
    const unsigned secs = unsigned(rem_us / 1000000), us = unsigned(rem_us % 1000000);
    char b[64];
    int n = std::snprintf(b, sizeof b, "%04lld-%02u-%02uT%02u:%02u:%02u", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
    if (us) n += std::snprintf(b + n, sizeof b - n, ".%06u", us);
    std::snprintf(b + n, sizeof b - n, "+00:00");
    return std::string(b);
}
uint8_t xor_bytes(const uint8_t* p, size_t n) { uint8_t x = 0; for (size_t i = 0; i < n; ++i) x ^= p[i]; return x; }
}  // namespace

Decoded decode_eeprom(const std::vector<uint8_t>& data, const std::string& which) {
    Decoded r; r.which = which; r.length = data.size();
    if (data.size() < 0x200) { r.problems.push_back("image too short (" + std::to_string(data.size()) + " bytes) to decode"); return r; }
    r.header_valid = true; Header& h = r.header;
    h.serial_raw = u32(data, 0x18); h.serial = serial_str(h.serial_raw);
    h.checksum_flag = data[0x3F]; h.checksums_present = data[0x3F] == kChecksumPresent;
    h.last_cal = filetime_iso((uint64_t(u32(data, 0x40)) << 32) | u32(data, 0x44));
    h.image_version = data[0x1BE];
    if (std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 0xFF; })) {
        r.problems.push_back("EEPROM reads all 0xFF: blank, absent, or the bus is not working"); return r;
    }
    if (!h.checksums_present) {
        char b[160]; std::snprintf(b, sizeof b, "checksum flag @0x3F is 0x%02X, not 0xC5: radio was never calibrated at the factory?", h.checksum_flag);
        r.problems.push_back(b);
    }
    for (const TableSpec& s : kTables) {
        if (!(which == "TRX" ? s.trx : s.rx2) || s.addr + s.rows * s.per * 4 > data.size()) continue;
        TableResult t; t.name = s.name; t.pointer_addr = s.ptr; t.pointer = u16(data, s.ptr); t.address = s.addr; t.rows = s.rows; t.per_row = s.per; t.checksum_addr = s.csum;
        t.pointer_ok = t.pointer == s.addr;
        t.checksum_stored = data[s.csum]; t.checksum_calc = xor_bytes(&data[s.addr], size_t(s.rows) * s.per * 4);
        t.checksum_ok = t.checksum_stored == t.checksum_calc;
        bool any_out = false;
        for (unsigned i = 0; i < s.rows; ++i) {
            std::vector<double> row;
            for (unsigned j = 0; j < s.per; ++j) {
                double v = round6(f32(data, s.addr + (i * s.per + j) * 4));
                if (std::isnan(v)) t.has_nan = true; else if (s.has_range && (v < s.lo || v > s.hi)) any_out = true;
                row.push_back(v);
            }
            t.values.push_back(row);
        }
        if (s.has_range) t.in_range = !any_out;
        char b[320];
        if (!t.pointer_ok) { std::snprintf(b, sizeof b, "%s: pointer @0x%02X is 0x%04X, expected 0x%04X", s.name, s.ptr, t.pointer, s.addr); r.problems.push_back(b); }
        if (!t.checksum_ok) { std::snprintf(b, sizeof b, "%s: checksum stored 0x%02X != calculated 0x%02X", s.name, t.checksum_stored, t.checksum_calc); r.problems.push_back(b); }
        if (t.has_nan) r.problems.push_back(std::string(s.name) + ": contains NaN");
        if (t.in_range && !*t.in_range) r.problems.push_back(std::string(s.name) + ": value outside plausible range");
        r.tables.push_back(std::move(t));
    }
    if (which == "TRX") {
        r.unverified = kUnverifiedCount;
        if (data.size() >= kTxCarrierAddr + 12) {
            std::vector<CarrierEntry> e; size_t off = kTxCarrierAddr;
            while (off + 12 <= data.size() && e.size() < 100) {
                const uint32_t hi = u32(data, off);
                if (hi == kEndOfTable) break;
                const uint32_t lo = u32(data, off + 4), val = u32(data, off + 8);
                e.push_back({round6(double((uint64_t(hi) << 32) + lo) * std::pow(2.0, -20) * 1e-6), val});
                off += 12;
            }
            r.tx_terminated = off + 12 <= data.size() && u32(data, off) == kEndOfTable;
            r.tx_carrier = std::move(e);
        }
    }
    return r;
}

std::string csv_text(const std::vector<uint8_t>& data) {
    std::string out = ",";
    char b[32];
    for (int i = 0; i < 16; ++i) { std::snprintf(b, sizeof b, "%X,", i); out += b; }
    out += "\n";
    for (size_t row = 0; row < data.size() / 16; ++row) {
        std::snprintf(b, sizeof b, "%zX,", row); out += b;
        for (size_t i = 0; i < 16; ++i) { std::snprintf(b, sizeof b, "%X,", data[row * 16 + i]); out += b; }
        out += "\n";
    }
    return out;
}
std::vector<uint8_t> parse_csv(const std::string& text) {
    std::vector<uint8_t> out; std::istringstream in(text); std::string line;
    std::getline(in, line);                                        // header row
    while (std::getline(in, line)) {
        std::istringstream ls(line); std::string cell; bool first = true;
        while (std::getline(ls, cell, ',')) { if (first) { first = false; continue; } if (!cell.empty()) out.push_back(uint8_t(std::stoul(cell, nullptr, 16))); }
    }
    return out;
}

std::vector<uint8_t> read_image(Flex& fx, const std::string& which, size_t length, int retries, double timeout_s) {
    const std::string op = which == "TRX" ? "READ_TRX_EEPROM_UINT32" : "READ_RX2_EEPROM_UINT32";
    if (!length) length = which == "TRX" ? kTrxLen : kRx2Len;
    if (length % 4) throw std::invalid_argument("length must be a multiple of 4");
    std::vector<uint8_t> out; out.reserve(length);
    for (size_t addr = 0; addr < length; addr += 4) {
        uint32_t w = 0;
        for (int attempt = 0;; ++attempt) {
            try { w = fx.get(op, uint32_t(addr), 0, timeout_s); break; }
            catch (const TimeoutError&) {
                if (attempt == retries - 1) { char b[96]; std::snprintf(b, sizeof b, "%s EEPROM: no reply reading 0x%04zX after %d tries", which.c_str(), addr, retries); throw EepromReadError(b); }
            }
        }
        for (int k = 0; k < 4; ++k) out.push_back(uint8_t(w >> (8 * k)));       // low byte first, as PowerSDR's Dump()
    }
    return out;
}
std::vector<uint8_t> read_verified(Flex& fx, const std::string& which, size_t length, int passes) {
    auto first = read_image(fx, which, length);
    for (int n = 1; n < passes; ++n) {
        auto again = read_image(fx, which, length);
        if (again != first) {
            size_t i = 0, cnt = 0; bool found = false;
            for (size_t k = 0; k < first.size(); ++k) if (first[k] != again[k]) { if (!found) { i = k; found = true; } ++cnt; }
            char b[320]; std::snprintf(b, sizeof b, "%s EEPROM changed between pass 1 and pass %d (%zu bytes differ, first at 0x%04zX); not saving", which.c_str(), n + 1, cnt, i);
            throw EepromReadError(b);
        }
    }
    return first;
}

namespace {
std::string write_new(const std::string& path, const void* data, size_t n) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0444);     // create-only: never overwrite an existing backup
    if (fd < 0) throw IoError("cannot create " + path + ": " + std::strerror(errno));
    const char* p = static_cast<const char*>(data);
    while (n) { ssize_t w = ::write(fd, p, n); if (w <= 0) { ::close(fd); throw IoError("write failed: " + path); } p += w; n -= size_t(w); }
    ::close(fd);
    return path;
}
}  // namespace
std::vector<std::string> save_backup(const std::string& dir, const std::map<std::string, std::vector<uint8_t>>& images) {
    std::filesystem::create_directories(dir);
    char stamp[32]; std::time_t t = std::time(nullptr); std::tm tmv; gmtime_r(&t, &tmv);
    std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tmv);
    std::string sn = "unknown";
    if (auto it = images.find("TRX"); it != images.end()) { Decoded d = decode_eeprom(it->second, "TRX"); if (d.header_valid) sn = d.header.serial; }
    const std::string prefix = dir + "/F5K_" + sn;
    std::vector<std::string> paths;
    for (auto& [which, img] : images) {
        std::string lw = which; std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
        paths.push_back(write_new(prefix + "_" + stamp + "_" + lw + ".bin", img.data(), img.size()));
    }
    if (auto it = images.find("TRX"); it != images.end()) {
        const std::string compat = prefix + " backup.csv";
        if (!std::filesystem::exists(compat)) { std::string c = csv_text(it->second); paths.push_back(write_new(compat, c.data(), c.size())); }
    }
    return paths;
}
RxLevelCal rx_level_cal(const Decoded& d) {
    RxLevelCal c; auto bad = [&](const std::string& w) { c.problem = w; return c; };
    const TableResult* t = nullptr; for (const auto& x : d.tables) if (x.name == "RX level") t = &x;
    if (!t) return bad("no RX level table");
    if (!t->pointer_ok || !t->checksum_ok) return bad("RX level table: pointer or checksum wrong");
    if (t->has_nan || t->values.size() != 11) return bad("RX level table: not 11 bands");
    for (size_t b = 0; b < 11; ++b) {
        const auto& r = t->values[b]; if (r.size() != 3) return bad("RX level table: row size");
        if (!(r[0] >= -90 && r[0] <= -30) || !(r[1] >= -30 && r[1] <= 5) || !(r[2] >= -70 && r[2] <= -10)) return bad("RX level table: a value out of range");
        c.display[b] = r[0]; c.preamp[b] = r[1]; c.meter[b] = r[2];
    }
    c.valid = true; return c;
}
}  // namespace fireside
