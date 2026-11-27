// When live audio was heard, for the slot decoders (FT8 / FT4 / WSPR). Audio reaches a decoder in blocks, some time after it was heard, and a stream
// that starts hands over a backlog first (about 0.7 s on the radio, 2026-11-26: every WSPR DT read +0.7 s against WSJT-X), so "now" at the first block
// is not the time of its first sample. Each block: observe(now, samples so far, this block included). The newest sample cannot have been heard after
// now, so the UTC of sample 0 is at most now - n / rate; the estimate is the smallest of those bounds over the last window_s (the blocks that came
// with the least delay). A sliding window, not the smallest ever: it also follows a sample clock running fast or slow against the PC's.
// Header only (no object of its own): the decoders on the transmit-isolation allowlist may include it.
#pragma once
#include <cstdint>
#include <deque>
#include <utility>

namespace fireside::decode {
class SlotClock {
public:
    explicit SlotClock(double rate = 48000, double window_s = 20) : rate_(rate), win_(window_s) {}
    void observe(double now, uint64_t samples) {
        const double b = now - double(samples) / rate_;
        while (!q_.empty() && q_.back().second >= b) q_.pop_back();                   // a monotonic queue: its front is the window's minimum
        q_.push_back({now, b});
        while (q_.size() > 1 && q_.front().first < now - win_) q_.pop_front();
    }
    bool valid() const { return !q_.empty(); }
    double t0() const { return q_.empty() ? 0.0 : q_.front().second; }              // the UTC sample 0 was heard at
    double utc_of(double sample) const { return t0() + sample / rate_; }
    double rate() const { return rate_; }
    void reset() { q_.clear(); }
private:
    double rate_, win_; std::deque<std::pair<double, double>> q_;                     // (when observed, bound for t0)
};
}  // namespace fireside::decode
