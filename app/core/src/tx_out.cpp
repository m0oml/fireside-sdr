#include "fireside/tx_out.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
#include "fireside/tx_setup.h"

namespace fireside::txo {
void ToneSource::set_cap(double c) { cap_.store(std::clamp(c, 0.0, txs::kAbsoluteMaxDrive)); }
void ToneSource::set_live(size_t n) { std::lock_guard<std::mutex> l(live_m_); live_ring_.assign(std::max<size_t>(n, 256), {0, 0}); live_head_ = live_count_ = 0; live_ = true; }
size_t ToneSource::push_live(const std::complex<float>* x, size_t n) {
    std::lock_guard<std::mutex> l(live_m_); if (!live_) return 0;
    const size_t cap = live_ring_.size(), k = std::min(n, cap - live_count_);
    for (size_t i = 0; i < k; ++i) live_ring_[(live_head_ + live_count_ + i) % cap] = x[i];
    live_count_ += k; return k;
}
size_t ToneSource::live_queued() const { std::lock_guard<std::mutex> l(live_m_); return live_count_; }

void IqTap::write(const std::complex<float>* x, size_t n, double rate) noexcept {
    std::unique_lock<std::mutex> l(m_, std::try_to_lock); if (!l.owns_lock()) { dropped_ += n; return; }
    rate_ = rate; const size_t cap = ring_.size();
    for (size_t i = 0; i < n; ++i) { if (count_ == cap) { ++dropped_; continue; } ring_[(head_ + count_) % cap] = x[i]; ++count_; }
}
size_t IqTap::read(std::vector<std::complex<float>>& out, double* rate) {
    std::lock_guard<std::mutex> l(m_); const size_t n = count_, cap = ring_.size();
    for (size_t i = 0; i < n; ++i) out.push_back(ring_[(head_ + i) % cap]);
    head_ = (head_ + n) % cap; count_ = 0; if (rate) *rate = rate_; return n;
}
void ToneSource::fill(int32_t* out, size_t frames) {
    const bool keyed = keyed_.load(); const double target = keyed ? target_.load() : 0.0, cap = cap_.load(), w = 2 * M_PI * tone_.load() / rate_;
    const double g = corr_gain_.load(), ph = corr_phase_.load();
    const double qs = mirror_.load() ? -1.0 : 1.0, step = 1.0 / (0.005 * rate_) * std::max(target, level_);   // full swing in ~5 ms
    if (const long long sk = seek_.exchange(kNoSeek); sk != kNoSeek) pos_ = sk;
    const bool tap = tap_ && keyed; if (tap && tap_buf_.size() < frames) tap_buf_.resize(frames);
    for (size_t f = 0; f < frames; ++f) {
        if (level_ < target) level_ = std::min(target, level_ + step); else if (level_ > target) level_ = std::max(target, level_ - step);
        if (!keyed) level_ = 0;                                   // unkeyed: exactly zero at once (the gate has already dropped MOX)
        double i, q;
        if (live_) {                                              // live mode (voice): the pushed I/Q, silence when it runs dry
            std::complex<float> x(0, 0);
            { std::lock_guard<std::mutex> l(live_m_); if (live_count_) { x = live_ring_[live_head_]; live_head_ = (live_head_ + 1) % live_ring_.size(); --live_count_; } else if (keyed) ++live_under_; }
            i = level_ * double(x.real()); q = qs * level_ * double(x.imag());
        } else if (have_buf_) {                                   // buffer mode: the rendered signal, then zeros
            std::complex<float> x(0, 0);
            if (pos_ >= 0 && pos_ < (long long)buf_.size()) x = buf_[size_t(pos_)]; else if (pos_ >= (long long)buf_.size()) done_ = true;
            if (keyed) ++pos_;
            i = level_ * double(x.real()); q = qs * level_ * double(x.imag());
        } else { i = level_ * std::cos(phase_); q = qs * level_ * std::sin(phase_); }
        if (tap) tap_buf_[f] = std::complex<float>(float(i), float(qs * q));   // the display's copy: before the mirror, correction and clamp
        q += ph * i; i *= g;                                      // TX image correction (DttSP correctIQ, isTX)
        phase_ += w; if (phase_ > 2 * M_PI) phase_ -= 2 * M_PI;
        i = std::clamp(i, -cap, cap); q = std::clamp(q, -cap, cap); // the hard clamp, last
        int32_t* o = out + f * size_t(ch_);
        std::fill(o, o + ch_, 0);
        o[0] = int32_t(std::lround(i * 2147483647.0)); o[1] = int32_t(std::lround(q * 2147483647.0));
    }
    if (tap) tap_->write(tap_buf_.data(), frames, rate_);
    level_pub_.store(level_); pos_pub_.store(pos_);
}

Stream::Stream(std::unique_ptr<IPcmOut> pcm, ToneSource& src) : pcm_(std::move(pcm)), src_(src) { th_ = std::thread([this] { run(); }); }
Stream::~Stream() { stop_ = true; if (th_.joinable()) th_.join(); }
void Stream::run() {
    const size_t n = 256; const int ch = pcm_->channels(); std::vector<int32_t> buf(n * size_t(ch));
    try {
        while (!stop_) {
            src_.fill(buf.data(), n);
            size_t done = 0;
            while (done < n && !stop_) {
                const size_t w = pcm_->write(buf.data() + done * size_t(ch), n - done);
                if (w == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); else { done += w; frames_ += w; }
            }
        }
        src_.set_keyed(false); src_.fill(buf.data(), n); pcm_->write(buf.data(), n);   // leave zeros behind
    } catch (...) { failed_ = true; }
}
}  // namespace fireside::txo
