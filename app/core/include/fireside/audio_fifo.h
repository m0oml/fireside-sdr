// A FIFO between a capture thread on one clock (the PC's sound card) and a reader on another (the radio's stream): mono float, 48 kHz.
// Drift correction: the fill is held near `target_s`; when it is more than `band_s` above, one sample in a thousand is dropped, more than
// `band_s` below, one in a thousand is repeated (0.1 %: inaudible on speech, covers sound-card clocks that are off by 1000 ppm).
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace fireside {
class DriftFifo {
public:
    explicit DriftFifo(double rate = 48000, double target_s = 0.04, double band_s = 0.02, double max_s = 0.5);
    void push(const float* x, size_t n);                      // capture side; the oldest is dropped when full
    size_t read(float* x, size_t n, double timeout_s);        // reader side: waits for the target fill first, then corrects the drift
    void clear();
    size_t fill() const;
    uint64_t dropped() const { std::lock_guard<std::mutex> l(m_); return dropped_; }
    uint64_t repeated() const { std::lock_guard<std::mutex> l(m_); return repeated_; }
    float peak_and_reset();                                    // |x| peak since the last call (level meters)
private:
    mutable std::mutex m_; std::condition_variable cv_; std::vector<float> buf_; size_t head_ = 0, count_ = 0, target_, band_; bool primed_ = false;
    uint64_t dropped_ = 0, repeated_ = 0, n_out_ = 0; float peak_ = 0, last_ = 0;
};
}  // namespace fireside
