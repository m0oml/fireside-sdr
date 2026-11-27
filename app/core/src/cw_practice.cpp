#include "fireside/cw_practice.h"
#include <chrono>
#include <vector>

namespace fireside::cwp {
Practice::Practice(Paddles paddles, Out out, const tx::KeyerConfig& k, bool straight) : paddles_(std::move(paddles)), out_(std::move(out)), cfg_(k), straight_(straight) {
    cfg_.sample_rate = kRate; keyer_ = std::make_unique<tx::IambicKeyer>(cfg_);
}
Practice::~Practice() { stop(); }
void Practice::start() { if (th_.joinable()) return; stop_ = false; th_ = std::thread([this] { run(); }); }
void Practice::stop() { stop_ = true; if (th_.joinable()) th_.join(); }
void Practice::run() {
    using Clock = std::chrono::steady_clock;
    const size_t blk = size_t(kRate / 1000); std::vector<uint8_t> k(blk);
    const auto t0 = Clock::now(); uint64_t made = 0;
    while (!stop_) {
        const uint64_t due = uint64_t(std::chrono::duration<double>(Clock::now() - t0).count() * kRate);
        if (due > made + uint64_t(0.1 * kRate)) made = due - blk;                  // stalled (suspend, load): skip ahead rather than burst
        while (made + blk <= due && !stop_) {
            uint8_t pad = 0; try { pad = paddles_ ? uint8_t(paddles_() & 3) : 0; } catch (...) {}
            uint64_t d = 0;
            for (size_t i = 0; i < blk; ++i) { const bool kd = straight_ ? pad != 0 : keyer_->step(pad & 1, pad & 2); k[i] = kd; d += kd; }
            if (out_) { try { out_(k.data(), blk, kRate); } catch (...) {} }
            made += blk; samples_ += blk; down_ += d;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}  // namespace fireside::cwp
