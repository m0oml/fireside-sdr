#pragma once
// The CW sidetone on the PC's audio output: a sine at the receiver's CW pitch that follows the key state (fed by the over's generator thread or the
// practice keyer). Shaped like the transmitted element (5 ms edges), so it never clicks. It plays from a thread of its own, so a busy window (the
// transmit picture switching on at the start of an over) can never starve it; with the PC output set to the radio's headphones it goes the
// AudioOut way instead (GUI thread).
#include <QObject>
#include <QString>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

class AudioOut; class QThread; class QTimer;
class CwSidetone : public QObject {
public:
    explicit CwSidetone(bool enable, QObject* parent = nullptr);   // enable=false: no device opened (tests)
    ~CwSidetone() override;
    bool available() const { return avail_.load(); }
    void setPitch(double hz) { std::lock_guard<std::mutex> l(m_); pitch_ = hz; }
    void setVolume(float v) { vol_ = v; }
    void feed(const uint8_t* key, size_t n, double rate);            // any thread
    // Since the last call: blocks dropped (the sound card full), backlog trims (nothing played it for 200 ms), times the card ran dry mid-tone. Empty if none.
    QString takeStats();
    // The player's side (its thread, or the GUI timer on the fallback): takes what was made, never more than `max` samples.
    std::vector<float> take(size_t max);
    float volume() const { return vol_.load(); }
    std::atomic<long> dry_{0}, dropped_{0};
    std::atomic<bool> avail_{false};
private:
    bool enable_; std::mutex m_; std::vector<float> pending_; double pos_ = 0, env_ = 0, ph_ = 0, pitch_ = 700; std::atomic<float> vol_{0.3f}; long trimmed_ = 0;
    QThread* thr_ = nullptr; QObject* worker_ = nullptr;             // the own-thread player
    AudioOut* out_ = nullptr; QTimer* t_ = nullptr;                  // the fallback (radio headphones)
    QTimer* watch_ = nullptr; QString dev_;                          // follows a change of the PC output
    void start(); void stop();
};
