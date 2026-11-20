// Display maths for the spectrum/waterfall widgets, kept Qt-free so it is unit-tested: colour map, auto-scaling, axis ticks, pixel mapping.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fireside::display {
struct Scale { float lo_db = -120.f, hi_db = -20.f; };
Scale auto_scale(const std::vector<float>& db, float min_span_db = 60.f, float max_span_db = 140.f);    // lo = median of the non-silent bins - 6 dB, hi = max(peak + 6, lo + min_span); the span never exceeds max_span (the top follows the peak, the bottom is raised)
bool is_silent(const std::vector<float>& db);                                  // (almost) every bin is at the analyzer floor (-200 dB): digital silence, which says nothing about the level to display
Scale smooth(Scale current, Scale target, float alpha);                       // one-pole move towards target (no flicker)
// Time-based, asymmetric move towards the target: the range GROWS (lo falls or hi rises) with time constant grow_tau_s, so a strong new signal is never clipped,
// and SHRINKS (lo rises or hi falls) with the much longer shrink_tau_s, so the axis does not breathe with every fade. dt_s is the time since the last update
// (frame hop = fft/2 / sample rate), so the speed does not depend on the sample rate. Each edge moves independently and never overshoots its target.
Scale smooth_asymmetric(Scale current, Scale target, float dt_s, float grow_tau_s = 0.6f, float shrink_tau_s = 20.f);

// Keeps the display scale of a spectrum/waterfall steady: auto_scale per frame, moved with smooth_asymmetric, and silent frames (gaps in a recording, a keyed
// signal's key-up) change nothing, so a gap neither stretches the axis down to -200 dB nor lets it collapse.
class ScaleTracker {
public:
    explicit ScaleTracker(float grow_tau_s = 0.6f, float shrink_tau_s = 20.f, float min_span_db = 60.f, float max_span_db = 140.f) : grow_(grow_tau_s), shrink_(shrink_tau_s), min_span_(min_span_db), max_span_(max_span_db) {}
    Scale update(const std::vector<float>& frame_db, float dt_s);   // returns the scale to draw this frame with
    Scale scale() const { return cur_; }
    void set_shrink_tau(float s) { if (s >= 1.f && s <= 300.f) shrink_ = s; }          // seconds to settle downwards; out-of-range or NaN values are ignored
    float shrink_tau() const { return shrink_; }
    void reset() { have_ = false; cur_ = Scale{}; }
private:
    float grow_, shrink_, min_span_, max_span_; Scale cur_{}; bool have_ = false;
};
float to_unit(float db, Scale s);                                             // clamped 0..1
enum class Palette { Classic, Inferno, Viridis, Gray };
inline constexpr int kPaletteCount = 4;
const char* palette_name(Palette p);
uint32_t colormap(float unit, Palette p = Palette::Classic);                  // 0xRRGGBB. Classic: black, blue, cyan, yellow, red, white
double nice_step(double span, int target_ticks);                              // 1/2/5 x 10^n closest to span/target_ticks
int hz_to_x(double offset_hz, double span_hz, int width);                     // offset from centre -> pixel (span centred)
double x_to_hz(int x, double span_hz, int width);
std::vector<float> decimate_max(const std::vector<float>& db, size_t out_width);   // max over each group: narrow signals stay visible on small widgets
std::string format_mhz(double hz);                                            // "7.101000 MHz"
}  // namespace fireside::display
