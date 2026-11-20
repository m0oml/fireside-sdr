#include "fireside/audio_fifo.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace fireside {
DriftFifo::DriftFifo(double rate, double target_s, double band_s, double max_s)
    : buf_(size_t(rate * max_s)), target_(size_t(rate * target_s)), band_(size_t(rate * band_s)) {}
void DriftFifo::push(const float* x, size_t n) {
    std::lock_guard<std::mutex> l(m_); const size_t cap = buf_.size();
    for (size_t i = 0; i < n; ++i) {
        const float v = std::isfinite(x[i]) ? x[i] : 0.0f; peak_ = std::max(peak_, std::fabs(v));
        if (count_ == cap) { head_ = (head_ + 1) % cap; --count_; ++dropped_; }
        buf_[(head_ + count_) % cap] = v; ++count_;
    }
    cv_.notify_all();
}
size_t DriftFifo::read(float* x, size_t n, double timeout_s) {
    std::unique_lock<std::mutex> l(m_);
    if (!cv_.wait_for(l, std::chrono::duration<double>(std::max(0.0, timeout_s)), [&] { return primed_ ? count_ > 0 : count_ >= target_; })) return 0;
    primed_ = true; const size_t cap = buf_.size(); size_t k = 0;
    while (k < n && count_ > 0) {
        const bool high = count_ > target_ + band_, low = count_ + band_ < target_;
        ++n_out_;
        if (high && n_out_ % 1000 == 0) { head_ = (head_ + 1) % cap; --count_; ++dropped_; if (!count_) break; }   // drop one
        if (low && n_out_ % 1000 == 0) { x[k++] = last_; ++repeated_; continue; }                                  // repeat one
        last_ = buf_[head_]; x[k++] = last_; head_ = (head_ + 1) % cap; --count_;
    }
    if (count_ == 0) primed_ = false;                          // ran dry: build the target fill up again before reading on
    return k;
}
void DriftFifo::clear() { std::lock_guard<std::mutex> l(m_); head_ = count_ = 0; primed_ = false; }
size_t DriftFifo::fill() const { std::lock_guard<std::mutex> l(m_); return count_; }
float DriftFifo::peak_and_reset() { std::lock_guard<std::mutex> l(m_); const float p = peak_; peak_ = 0; return p; }
}  // namespace fireside
