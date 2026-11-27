#pragma once
// The CW sidetone on the PC's default audio output: a sine at the receiver's CW pitch that follows the over's key state (the generator thread
// feeds it, or the practice keyer; a 10 ms timer hands it to the sound card, with a cushion of silence if it nearly ran dry). Shaped like the transmitted element (5 ms edges), so it never clicks.
#include <QObject>
#include <QString>
#include <QTimer>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>
#include "AudioOut.h"

class CwSidetone : public QObject {
public:
    explicit CwSidetone(bool enable, QObject* parent = nullptr) : QObject(parent), out_(new AudioOut(enable, this)), t_(new QTimer(this)) {   // enable=false: no device opened (tests)
        out_->setIdleFillMs(kIdleMs);                                                // opened (and kept awake) before the first element: a short delay, no cold start
        t_->setInterval(10); connect(t_, &QTimer::timeout, this, [this] { drain(); });
        t_->start();
    }
    bool available() const { return out_->available(); }
    void setPitch(double hz) { std::lock_guard<std::mutex> l(m_); pitch_ = hz; }
    void setVolume(float v) { vol_ = v; }
    void feed(const uint8_t* key, size_t n, double rate) {          // any thread
        std::lock_guard<std::mutex> l(m_);
        const double step = rate / 48000.0, de = 1.0 / (0.005 * 48000.0), dph = 2 * M_PI * pitch_ / 48000.0;
        while (pos_ < double(n)) {
            const bool k = key[size_t(pos_)] != 0; env_ = k ? std::min(1.0, env_ + de) : std::max(0.0, env_ - de);
            pending_.push_back(float(0.5 * (1 - std::cos(M_PI * env_)) * std::sin(ph_))); ph_ += dph; if (ph_ > 2 * M_PI) ph_ -= 2 * M_PI;
            pos_ += step;
        }
        pos_ -= double(n);
        if (pending_.size() > 48000 / 5) { pending_.erase(pending_.begin(), pending_.end() - 48000 / 10); ++trimmed_; }   // never let the delay grow (the sound card stalled)
    }
    // Since the last call: times the sound card had (nearly) run dry and got a cushion of silence, blocks dropped (sink full), backlog trims.
    QString takeStats() { long t; { std::lock_guard<std::mutex> l(m_); t = trimmed_; trimmed_ = 0; } const long g = gaps_, d = out_->dropped() - dropped0_; gaps_ = 0; dropped0_ = out_->dropped();
                          return g || d || t ? QString("sidetone: %1 gap(s) filled, %2 block(s) dropped, %3 backlog trim(s)").arg(g).arg(d).arg(t) : QString(); }
private:
    static constexpr int kIdleMs = 30, kLowMs = 8, kCushionMs = 25;
    void drain() {                                                                    // GUI thread: hand what the generator made to the sound card in one piece
        std::vector<float> v; { std::lock_guard<std::mutex> l(m_); v.swap(pending_); }
        if (v.empty()) return;
        if (out_->available() && out_->queuedMs() < kLowMs) {                        // (nearly) ran dry, e.g. the window was busy: a short silence first, not a click
            v.insert(v.begin(), size_t(48 * (kCushionMs - out_->queuedMs())), 0.0f); ++gaps_; }
        out_->push(v, vol_);
    }
    AudioOut* out_; QTimer* t_; std::mutex m_; std::vector<float> pending_; double pos_ = 0, env_ = 0, ph_ = 0, pitch_ = 700; float vol_ = 0.3f;
    long gaps_ = 0, dropped0_ = 0, trimmed_ = 0;
};
