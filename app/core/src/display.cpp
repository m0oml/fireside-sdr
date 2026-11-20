#include "fireside/display.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fireside::display {
namespace { constexpr float kSilentDb = -199.5f; }                                 // the analyzer reports exact zeros as -200 dB
bool is_silent(const std::vector<float>& db) {
    if (db.empty()) return true;
    size_t live = 0; for (float v : db) if (v > kSilentDb) ++live;
    return live * 10 < db.size();                                                  // fewer than 10 % of the bins carry anything
}
Scale auto_scale(const std::vector<float>& db, float min_span, float max_span) {
    std::vector<float> v; v.reserve(db.size()); float peak = -1e9f; for (float x : db) if (x > kSilentDb) { v.push_back(x); peak = std::max(peak, x); }
    if (v.empty()) return {};
    auto mid = v.begin() + long(v.size() / 2); std::nth_element(v.begin(), mid, v.end());
    Scale s; s.lo_db = *mid - 6.f; s.hi_db = std::max(peak + 6.f, s.lo_db + min_span);
    if (max_span > min_span && s.hi_db - s.lo_db > max_span) s.lo_db = s.hi_db - max_span;       // a very clean signal: keep the strong part readable instead of showing 190 dB of nothing
    return s;
}
Scale smooth(Scale c, Scale t, float a) { return {c.lo_db + a * (t.lo_db - c.lo_db), c.hi_db + a * (t.hi_db - c.hi_db)}; }
Scale smooth_asymmetric(Scale c, Scale t, float dt, float grow_tau, float shrink_tau) {
    auto alpha = [&](float tau) { return !(dt > 0.f) ? 0.f : !(tau > 0.f) ? 1.f : 1.f - std::exp(-dt / tau); };
    const float a_lo = t.lo_db < c.lo_db ? alpha(grow_tau) : alpha(shrink_tau);     // lo falling: more range
    const float a_hi = t.hi_db > c.hi_db ? alpha(grow_tau) : alpha(shrink_tau);     // hi rising: more range
    return {c.lo_db + a_lo * (t.lo_db - c.lo_db), c.hi_db + a_hi * (t.hi_db - c.hi_db)};
}
Scale ScaleTracker::update(const std::vector<float>& frame, float dt) {
    if (is_silent(frame)) return cur_;
    const Scale target = auto_scale(frame, min_span_, max_span_);
    cur_ = have_ ? smooth_asymmetric(cur_, target, dt, grow_, shrink_) : target; have_ = true;
    if (max_span_ > min_span_ && cur_.hi_db - cur_.lo_db > max_span_) cur_.lo_db = cur_.hi_db - max_span_;   // the smoothed span, too, is capped: the quick growth must not chase every low-level frame downwards
    return cur_;
}
float to_unit(float db, Scale s) { return s.hi_db > s.lo_db ? std::min(std::max((db - s.lo_db) / (s.hi_db - s.lo_db), 0.f), 1.f) : 0.f; }
const char* palette_name(Palette p) { switch (p) { case Palette::Classic: return "Classic"; case Palette::Inferno: return "Inferno"; case Palette::Viridis: return "Viridis"; case Palette::Gray: return "Gray"; } return "?"; }
namespace {
struct Stop { float p; int r, g, b; };
uint32_t lerp_stops(const Stop* st, int n, float u) {
    u = std::min(std::max(u, 0.f), 1.f);
    for (int i = 0; i + 1 < n; ++i) if (u <= st[i + 1].p) {
        const float t = (u - st[i].p) / (st[i + 1].p - st[i].p);
        auto mix = [&](int a, int b) { return uint32_t(std::lround(float(a) + t * float(b - a))); };
        return mix(st[i].r, st[i + 1].r) << 16 | mix(st[i].g, st[i + 1].g) << 8 | mix(st[i].b, st[i + 1].b);
    }
    return uint32_t(st[n - 1].r) << 16 | uint32_t(st[n - 1].g) << 8 | uint32_t(st[n - 1].b);
}
}  // namespace
uint32_t colormap(float u, Palette pal) {
    static const Stop classic[] = {{0.00f, 0, 0, 0}, {0.20f, 0, 0, 140}, {0.40f, 0, 170, 230}, {0.60f, 240, 230, 0}, {0.80f, 230, 30, 0}, {1.00f, 255, 255, 255}};
    static const Stop inferno[] = {{0.00f, 0, 0, 4}, {0.14f, 40, 11, 84}, {0.29f, 101, 21, 110}, {0.43f, 159, 42, 99}, {0.57f, 212, 72, 66}, {0.71f, 245, 125, 21}, {0.86f, 250, 193, 39}, {1.00f, 252, 255, 164}};
    static const Stop viridis[] = {{0.00f, 68, 1, 84}, {0.11f, 72, 40, 120}, {0.22f, 62, 74, 137}, {0.33f, 49, 104, 142}, {0.44f, 38, 130, 142}, {0.56f, 31, 158, 137}, {0.67f, 53, 183, 121}, {0.78f, 109, 205, 89}, {0.89f, 180, 222, 44}, {1.00f, 253, 231, 37}};
    static const Stop gray[] = {{0.00f, 0, 0, 0}, {1.00f, 255, 255, 255}};
    switch (pal) {
        case Palette::Inferno: return lerp_stops(inferno, 8, u);
        case Palette::Viridis: return lerp_stops(viridis, 10, u);
        case Palette::Gray: return lerp_stops(gray, 2, u);
        default: return lerp_stops(classic, 6, u);
    }
}
double nice_step(double span, int ticks) {
    const double raw = span / double(ticks > 0 ? ticks : 1), mag = std::pow(10.0, std::floor(std::log10(raw))), n = raw / mag;
    return (n < 1.5 ? 1.0 : n < 3.5 ? 2.0 : n < 7.5 ? 5.0 : 10.0) * mag;
}
int hz_to_x(double hz, double span, int w) { return int(std::lround((hz / span + 0.5) * double(w - 1))); }
double x_to_hz(int x, double span, int w) { return (double(x) / double(w - 1) - 0.5) * span; }
std::vector<float> decimate_max(const std::vector<float>& db, size_t out) {
    if (out == 0 || db.empty()) return {};
    if (out >= db.size()) return db;
    std::vector<float> r(out, -1e9f);
    for (size_t i = 0; i < db.size(); ++i) { const size_t o = i * out / db.size(); r[o] = std::max(r[o], db[i]); }
    return r;
}
std::string format_mhz(double hz) { char b[48]; std::snprintf(b, sizeof b, "%.6f MHz", hz / 1e6); return b; }
}  // namespace fireside::display
