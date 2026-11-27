// CW practice: the paddle drives the keyer and the sidetone only. Nothing here can reach the transmitter or the radio: the paddles come in
// through a function (the KeyWatcher's take()), and the key state goes out as blocks of one byte per sample at kRate (the sidetone's feed).
// Paced by the steady clock in 1 ms blocks on a thread of its own. Qt-free.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include "fireside/tx_dsp.h"

namespace fireside::cwp {
class Practice {
public:
    static constexpr double kRate = 48000;
    using Paddles = std::function<uint8_t()>;                                     // bit 0 dot, bit 1 dash
    using Out = std::function<void(const uint8_t* key, size_t n, double rate)>;
    Practice(Paddles paddles, Out out, const tx::KeyerConfig& k, bool straight);  // throws std::invalid_argument (the keyer's limits)
    ~Practice();
    void start(); void stop(); bool running() const { return th_.joinable(); }
    uint64_t samples() const { return samples_.load(); }                         // key-state samples produced (tests)
    uint64_t down_samples() const { return down_.load(); }                       // of which key down (tests)
private:
    Paddles paddles_; Out out_; tx::KeyerConfig cfg_; bool straight_; std::unique_ptr<tx::IambicKeyer> keyer_;
    std::thread th_; std::atomic<bool> stop_{false}; std::atomic<uint64_t> samples_{0}, down_{0};
    void run();
};
}  // namespace fireside::cwp
