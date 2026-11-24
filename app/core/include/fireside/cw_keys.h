// The CW key input: the radio's KEY jack (dot = tip, dash = ring) read from the PAL EVENT register (read-only) every poll_s on a thread of its own.
// Presses are latched: take() returns the current contacts OR any contact closed since the last take(), so a tap shorter than the polling interval
// still reaches the keyer. on_press() is called (on the watcher thread) when a contact closes while nothing has taken the input for a while:
// that is how a CW over starts (semi break-in). Qt-free; nothing here writes to the radio or keys it.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include "fireside/tx_control.h"

namespace fireside::cwk {
class KeyWatcher : public txc::ICwKeys {
public:
    using ReadFn = std::function<std::optional<uint32_t>()>;     // the EVENT bitmap (bit 0 dot, bit 1 dash); nullopt = could not read
    KeyWatcher(ReadFn read, double poll_s = 0.003);
    ~KeyWatcher() override;
    void start(); void stop(); bool running() const { return th_.joinable(); }
    void set_on_press(std::function<void()> f) { std::lock_guard<std::mutex> l(m_); on_press_ = std::move(f); }
    uint8_t paddles() override { return take(); }
    uint8_t take();                                               // current | latched since the last take; clears the latch
    uint8_t current() const { return cur_.load(); }               // for the key lights
    uint64_t reads() const { return reads_.load(); } uint64_t failures() const { return fails_.load(); }
    double mean_read_ms() const { const uint64_t n = reads_.load(); return n ? 1000.0 * read_s_.load() / double(n) : 0; }
private:
    ReadFn read_; double poll_s_; std::thread th_; std::atomic<bool> stop_{false}; std::atomic<uint8_t> cur_{0}, latch_{0};
    std::atomic<uint64_t> reads_{0}, fails_{0}; std::atomic<double> read_s_{0}; std::atomic<double> last_take_{0};
    std::mutex m_; std::function<void()> on_press_;
    void run();
};
}  // namespace fireside::cwk
