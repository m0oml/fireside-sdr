#include "fireside/vox.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include "fireside/rawmidi.h"

namespace fireside::vox {
VoxDetector::VoxDetector(const VoxConfig& c) : c_(c) {
    if (!(c.threshold_dbfs >= -80 && c.threshold_dbfs <= 0)) throw std::invalid_argument("VOX threshold must be -80..0 dBFS");
    if (!(c.hang_s >= 0.1 && c.hang_s <= 5)) throw std::invalid_argument("VOX hang must be 0.1..5 s");
    if (!(c.attack_ms >= 1 && c.attack_ms <= 200)) throw std::invalid_argument("VOX attack must be 1..200 ms");
    if (!(c.sample_rate >= 8000 && c.sample_rate <= 384000)) throw std::invalid_argument("bad VOX sample rate");
    rel_ = std::exp(-1.0 / (0.05 * c.sample_rate));                     // the envelope falls with a 50 ms time constant (rises at once)
    attack_ = size_t(c.attack_ms * 1e-3 * c.sample_rate); hang_ = size_t(c.hang_s * c.sample_rate);
}
bool VoxDetector::process(const float* x, size_t n, double spk) {
    const double thr = std::pow(10.0, c_.threshold_dbfs / 20), anti = c_.anti_vox ? std::pow(10.0, (spk + c_.anti_gain_db) / 20) : 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double a = std::fabs(double(x[i])); env_ = a > env_ ? a : env_ * rel_;
        const bool speech = env_ > thr && env_ > anti;
        if (speech) { below_ = 0; if (++above_ >= attack_) on_ = true; }
        else { above_ = 0; if (on_ && ++below_ >= hang_) on_ = false; }
    }
    return on_;
}

VoxInput::VoxInput(std::unique_ptr<txc::IAudioIn> in, const VoxConfig& c, double gain_db, double pre_roll_s, std::function<double()> spk)
    : in_(std::move(in)), det_(c), gain_(float(std::pow(10.0, gain_db / 20))), pre_(size_t(std::clamp(pre_roll_s, 0.0, 0.5) * c.sample_rate)), spk_(std::move(spk)) {
    if (!in_) throw std::invalid_argument("VOX: no input");
    if (c.hang_s < pre_roll_s + 0.1) throw std::invalid_argument("VOX hang must be at least 0.1 s longer than the pre-roll");
    th_ = std::thread([this] { run(); });
}
VoxInput::~VoxInput() { stop_ = true; cv_.notify_all(); if (th_.joinable()) th_.join(); }
std::string VoxInput::error() const { std::lock_guard<std::mutex> l(m_); return err_; }
std::string VoxInput::describe() const { return "VOX on " + in_->describe(); }
void VoxInput::set_reading(bool r) { std::lock_guard<std::mutex> l(m_); reading_ = r; if (!r) while (q_.size() > pre_) q_.pop_front(); }
void VoxInput::run() {
    std::vector<float> b(480), g(480);
    while (!stop_) {
        size_t n = 0;
        try { n = in_->read(b.data(), b.size(), 0.05); }
        catch (const std::exception& e) { { std::lock_guard<std::mutex> l(m_); err_ = e.what(); } failed_ = true; on_ = false; cv_.notify_all(); return; }
        if (!n) continue;
        for (size_t i = 0; i < n; ++i) g[i] = b[i] * gain_;
        double spk = -200; if (spk_) { try { spk = spk_(); } catch (...) {} }
        on_ = det_.process(g.data(), n, spk); level_ = det_.level_dbfs();
        std::lock_guard<std::mutex> l(m_);
        q_.insert(q_.end(), b.begin(), b.begin() + long(n));             // the over gets the raw input: the chain applies the gain itself
        if (!reading_) while (q_.size() > pre_) q_.pop_front();
        else while (q_.size() > 48000 * 2) q_.pop_front();              // a stalled reader never makes the delay grow past 2 s
        cv_.notify_all();
    }
}
size_t VoxInput::read(float* x, size_t n, double timeout_s) {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait_for(l, std::chrono::duration<double>(timeout_s), [&] { return !q_.empty() || failed_ || stop_; });
    if (failed_) throw IoError("VOX input: " + err_);
    const size_t k = std::min(n, q_.size()); for (size_t i = 0; i < k; ++i) { x[i] = q_.front(); q_.pop_front(); }
    return k;
}

void ssb_to_audio(const std::vector<tx::cf>& iq, std::vector<float>& out) { out.resize(iq.size()); for (size_t i = 0; i < iq.size(); ++i) out[i] = iq[i].real(); }

MicLoop::MicLoop(std::unique_ptr<txc::IAudioIn> in, const tx::TxChainConfig& chain, std::function<void(const float*, size_t)> out, std::shared_ptr<txo::IqTap> picture)
    : in_(std::move(in)), chain_([&] { tx::TxChainConfig c = chain; c.mic.sample_rate = 48000; c.ssb.sample_rate = 48000; return c; }()), out_(std::move(out)), pic_(std::move(picture)) {
    if (!in_) throw std::invalid_argument("feedback test: no input");
    th_ = std::thread([this, chain] {
        std::vector<float> a(480), o; std::vector<tx::cf> iq; double pin = 0, pout = 0; const double gain = std::pow(10.0, chain.mic.mic_gain_db / 20);
        while (!stop_) {
            size_t n = 0;
            try { n = in_->read(a.data(), a.size(), 0.05); }
            catch (const std::exception& e) { { std::lock_guard<std::mutex> l(m_); err_ = e.what(); } failed_ = true; return; }
            if (!n) continue;
            iq.clear(); chain_.process(a.data(), n, iq); ssb_to_audio(iq, o);
            if (pic_ && !iq.empty()) pic_->write(iq.data(), iq.size(), 48000);
            double pi = 0, po = 0; for (size_t i = 0; i < n; ++i) pi = std::max(pi, double(std::fabs(a[i])) * gain);   // as the chain sees it (after the input gain)
            for (float v : o) po = std::max(po, double(std::fabs(v)));
            pin = std::max(pi, pin * 0.9); pout = std::max(po, pout * 0.9);   // peak hold that falls ~1 dB per 10 ms block
            in_db_ = 20 * std::log10(pin + 1e-9); out_db_ = 20 * std::log10(pout + 1e-9);
            if (out_ && !o.empty()) { try { out_(o.data(), o.size()); } catch (...) {} }
        }
    });
}
MicLoop::~MicLoop() { stop_ = true; if (th_.joinable()) th_.join(); }
}  // namespace fireside::vox
