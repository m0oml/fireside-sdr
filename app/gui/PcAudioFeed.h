#pragma once
// Audio produced on another thread (the feedback test, the transmit monitor) played on the PC's default output: pushed under a lock, handed to the
// sound card every 5 ms. The queue never grows past 0.2 s (a stalled sound card drops audio rather than adding delay).
#include <QObject>
#include <QTimer>
#include <mutex>
#include <vector>
#include "AudioOut.h"

class PcAudioFeed : public QObject {
public:
    explicit PcAudioFeed(bool enable, QObject* parent = nullptr) : QObject(parent), out_(new AudioOut(enable, this)), t_(new QTimer(this)) {   // enable=false: no device opened (tests)
        t_->setInterval(5); connect(t_, &QTimer::timeout, this, [this] { std::vector<float> v; { std::lock_guard<std::mutex> l(m_); v.swap(q_); } if (!v.empty()) out_->push(v, vol_); });
        t_->start();
    }
    bool available() const { return out_->available(); }
    void setVolume(float v) { vol_ = v; }
    void push(const float* x, size_t n) {                               // any thread
        std::lock_guard<std::mutex> l(m_); q_.insert(q_.end(), x, x + n);
        if (q_.size() > 48000 / 5) q_.erase(q_.begin(), q_.end() - 48000 / 10);
    }
private:
    AudioOut* out_; QTimer* t_; std::mutex m_; std::vector<float> q_; float vol_ = 0.5f;
};
