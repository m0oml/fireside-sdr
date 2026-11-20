// Minimal dependency-free test harness.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <vector>

struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
template <class T> void show(std::ostream& o, const T& v) {            // printable for enums, 8-bit ints and anything with operator<<
    if constexpr (std::is_enum_v<T>) o << static_cast<long long>(v);
    else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>) o << int(v);
    else o << v;
}
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()
#define FAIL_AT(msg) throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg))
#define CHECK(c) do { if (!(c)) FAIL_AT(std::string("CHECK failed: ") + #c); } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::ostringstream _o; _o << "CHECK_EQ failed: " #a " == " #b "  ("; show(_o, _a); _o << " vs "; show(_o, _b); _o << ")"; FAIL_AT(_o.str()); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (eps))) { std::ostringstream _o; _o << "CHECK_NEAR failed: " #a " ~ " #b "  (" << _a << " vs " << _b << ")"; FAIL_AT(_o.str()); } } while (0)
#define CHECK_THROWS(expr, ExType) do { bool _t = false; try { (void)(expr); } catch (const ExType&) { _t = true; } catch (...) { FAIL_AT("wrong exception type from " #expr); } if (!_t) FAIL_AT("no exception from " #expr); } while (0)

inline std::string vec_path(const std::string& f) { return std::string(FIRESIDE_VECTOR_DIR) + "/" + f; }
inline std::vector<std::vector<std::string>> read_table(const std::string& f) {     // whitespace-separated rows, '#' comments skipped
    std::ifstream in(vec_path(f)); if (!in) throw TestFailure("cannot open vector file " + f);
    std::vector<std::vector<std::string>> rows; std::string line;
    while (std::getline(in, line)) { if (line.empty() || line[0] == '#') continue; std::istringstream s(line); std::vector<std::string> r; std::string t; while (s >> t) r.push_back(t); rows.push_back(r); }
    return rows;
}
inline std::vector<uint8_t> read_bin(const std::string& f) {
    std::ifstream in(vec_path(f), std::ios::binary); if (!in) throw TestFailure("cannot open " + f);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
inline std::string read_text(const std::string& f) { auto b = read_bin(f); return std::string(b.begin(), b.end()); }
inline std::vector<uint8_t> from_hex(const std::string& h) {
    std::vector<uint8_t> o; for (size_t i = 0; i + 1 < h.size(); i += 2) o.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16))); return o;
}
