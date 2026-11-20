#include "fireside/protocol.h"
#include <cmath>
#include <cstring>

namespace fireside {
namespace {
constexpr uint8_t kStart = 0xF0, kEnd = 0xF7;
constexpr uint8_t kMfr[3] = {0x00, 0x00, 0x41};

Frame wrap(const uint8_t* payload, size_t n) {
    static const char* hex = "0123456789ABCDEF";
    Frame f; f.reserve(n * 2 + 5);
    f.push_back(kStart); f.insert(f.end(), kMfr, kMfr + 3);
    for (size_t i = 0; i < n; ++i) { f.push_back(hex[payload[i] >> 4]); f.push_back(hex[payload[i] & 15]); }
    f.push_back(kEnd);
    return f;
}
int nib(uint8_t c) {                       // uppercase hex only, as the Python reference
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::optional<std::vector<uint8_t>> unwrap(const Frame& f) {
    if (f.size() < 6 || f.front() != kStart || f.back() != kEnd) return std::nullopt;
    if (std::memcmp(&f[1], kMfr, 3) != 0) return std::nullopt;
    size_t n = f.size() - 5;
    if (n % 2) return std::nullopt;
    std::vector<uint8_t> out(n / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nib(f[4 + 2 * i]), lo = nib(f[5 + 2 * i]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = uint8_t(hi << 4 | lo);
    }
    return out;
}
void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

Frame encode_request(uint16_t id, uint32_t opcode, uint32_t d1, uint32_t d2) {
    uint8_t p[16] = {};
    put16(p, id); put16(p + 2, 0); put32(p + 4, opcode); put32(p + 8, d1); put32(p + 12, d2);
    return wrap(p, 16);
}
std::optional<Request> decode_request(const Frame& f) {
    auto raw = unwrap(f);
    if (!raw || raw->size() != 16) return std::nullopt;
    const uint8_t* p = raw->data();
    return Request{get16(p), get16(p + 2), get32(p + 4), get32(p + 8), get32(p + 12)};
}
Frame encode_reply(uint16_t id, uint32_t value) {
    uint8_t p[6]; put16(p, id); put32(p + 2, value);
    return wrap(p, 6);
}
std::optional<Reply> decode_reply(const Frame& f) {
    auto raw = unwrap(f);
    if (!raw || raw->size() < 6) return std::nullopt;
    return Reply{get16(raw->data()), get32(raw->data() + 2)};
}

uint32_t pack_u32(long long v) {
    if (v < 0 || v > 0xFFFFFFFFll) throw std::out_of_range("u32 out of range: " + std::to_string(v));
    return uint32_t(v);
}
uint32_t pack_i32(long long v) {
    if (v < -0x80000000ll || v > 0x7FFFFFFFll) throw std::out_of_range("i32 out of range: " + std::to_string(v));
    return uint32_t(int32_t(v));
}
uint32_t pack_f32(double v) { float f = float(v); uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint32_t pack_arg(double v, ArgType t) {
    switch (t) {
        case ArgType::U32: return pack_u32(llround(v));
        case ArgType::I32: return pack_i32(llround(v));
        case ArgType::F32: return pack_f32(v);
        case ArgType::None: return pack_u32(llround(v));
    }
    throw std::invalid_argument("bad ArgType");
}
float as_f32(uint32_t raw) { float f; std::memcpy(&f, &raw, 4); return f; }
int32_t as_i32(uint32_t raw) { return int32_t(raw); }
}  // namespace fireside
