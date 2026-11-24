#pragma once
// The CW sidetone on the PC's default audio output: a sine at the receiver's CW pitch that follows the over's key state (the generator thread
// feeds it; a 5 ms timer hands it to the sound card). Shaped like the transmitted element (5 ms edges), so it never clicks.
#include <QObject>
#include <QTimer>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>
#include "AudioOut.h"

class CwSidetone : public QObject {
public:
    explicit CwSidetone(QObject* parent = nullptr) : QObject(parent), out_(new AudioOut(true, this)), t_(new QTimer(this)) {
        t_->setInterval(5); connect(t_, &QTimer::timeout, this, [this] { std::vector<float> v; { std::lock_guard<std::mutex> l(m_); v.swap(pending_); } if (!v.empty()) out_->push(v, vol_); });
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
        if (pending_.size() > 48000 / 5) pending_.erase(pending_.begin(), pending_.end() - 48000 / 10);   // never let the delay grow (the sound card stalled)
    }
private:
    AudioOut* out_; QTimer* t_; std::mutex m_; std::vector<float> pending_; double pos_ = 0, env_ = 0, ph_ = 0, pitch_ = 700; float vol_ = 0.3f;
};
