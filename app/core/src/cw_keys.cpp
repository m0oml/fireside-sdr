#include "fireside/cw_keys.h"
#include <chrono>

namespace fireside::cwk {
namespace { double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); } }
KeyWatcher::KeyWatcher(ReadFn read, double poll_s) : read_(std::move(read)), poll_s_(poll_s < 0.001 ? 0.001 : poll_s > 0.05 ? 0.05 : poll_s) {}
KeyWatcher::~KeyWatcher() { stop(); }
void KeyWatcher::start() { if (th_.joinable()) return; stop_ = false; cur_ = 0; latch_ = 0; th_ = std::thread([this] { run(); }); }
void KeyWatcher::stop() { stop_ = true; if (th_.joinable()) th_.join(); }
uint8_t KeyWatcher::take() { last_take_ = now_s(); return uint8_t(cur_.load() | latch_.exchange(0)); }
void KeyWatcher::run() {
    uint8_t prev = 0;
    while (!stop_) {
        const double t0 = now_s();
        std::optional<uint32_t> v; try { v = read_(); } catch (...) {}
        const double dt = now_s() - t0;
        if (!v) ++fails_;
        else {
            reads_ += 1; read_s_.store(read_s_.load() + dt);
            const uint8_t k = uint8_t(*v & 3); cur_ = k; latch_ |= k;
            if ((k & ~prev) && now_s() - last_take_.load() > 0.1) {   // a contact closed and no over is reading the input: start one
                std::function<void()> f; { std::lock_guard<std::mutex> l(m_); f = on_press_; } if (f) f();
            }
            prev = k;
        }
        const double left = poll_s_ - (now_s() - t0);
        if (left > 0) std::this_thread::sleep_for(std::chrono::duration<double>(left));
    }
}
}  // namespace fireside::cwk
