#include "fireside/tune.h"
#include <chrono>
#include <cmath>
#include "fireside/startup.h"
#include "fireside/tuning.h"

namespace fireside {
namespace {
double steady_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

Tuner::Tuner(Flex& fx, TuneConfig cfg, std::function<double()> now) : fx_(fx), cfg_(cfg), now_(now ? std::move(now) : std::function<double()>(steady_now)) {
    require_firewire(fx_);
    if (!(cfg_.min_interval_s >= 0) || !(cfg_.clock_mhz > 0) || !std::isfinite(cfg_.if_offset_hz) || cfg_.if_offset_hz < 0) throw std::invalid_argument("bad TuneConfig");
}
bool Tuner::in_range(double dial_hz, double if_offset_hz) {
    if (!std::isfinite(dial_hz) || !std::isfinite(if_offset_hz)) return false;
    const double dds_mhz = (dial_hz - if_offset_hz) / 1e6, dial_mhz = dial_hz / 1e6;
    return dds_mhz > 0.0 && dds_mhz <= kMaxRxMHz && dial_mhz > 0.0 && dial_mhz <= kMaxRxMHz;
}
void Tuner::arm(std::optional<double> current) {
    if (!fx_.safety().allow_write) throw SafetyError("Tuner::arm: the Flex client has writes disabled");
    std::lock_guard<std::mutex> l(m_);
    armed_ = true; err_.clear(); pending_.reset(); last_sent_ = current;
}
void Tuner::disarm() { std::lock_guard<std::mutex> l(m_); armed_ = false; pending_.reset(); }
bool Tuner::armed() const { std::lock_guard<std::mutex> l(m_); return armed_; }

TuneResult Tuner::send_locked(double dial_hz) {
    const double dds_mhz = (dial_hz - cfg_.if_offset_hz) / 1e6;
    try {
        // The only write in this file: d1 = tuning word, d2 = DDS frequency in MHz as float32 (same encoding as the reviewed start-up step).
        fx_.set("SET_RX1_FREQ_TW", double(freq_to_tw(dds_mhz, cfg_.clock_mhz)), dds_mhz);
    } catch (const std::exception& e) {                          // mailbox fault, timeout, safety refusal: stop writing until the operator re-arms
        armed_ = false; pending_.reset(); err_ = e.what(); ++c_.faults; return TuneResult::Fault;
    }
    last_sent_ = dial_hz; last_write_t_ = now_(); pending_.reset(); ++c_.sent;
    return TuneResult::Sent;
}

TuneResult Tuner::set_if_offset(double hz) {
    std::lock_guard<std::mutex> l(m_);
    if (!armed_) return TuneResult::NotArmed;
    if (!std::isfinite(hz) || hz < 0 || hz > 20000) return TuneResult::Rejected;
    if (hz == cfg_.if_offset_hz) return TuneResult::Unchanged;
    cfg_.if_offset_hz = hz;
    const std::optional<double> dial = pending_ ? pending_ : last_sent_;
    if (!dial) return TuneResult::Unchanged;                                   // nothing tuned yet: the next request uses the new offset
    return send_locked(*dial);
}
double Tuner::if_offset() const { std::lock_guard<std::mutex> l(m_); return cfg_.if_offset_hz; }
TuneResult Tuner::request(double dial_hz) {
    std::lock_guard<std::mutex> l(m_);
    if (!armed_) return TuneResult::NotArmed;
    if (!in_range(dial_hz, cfg_.if_offset_hz)) { ++c_.rejected; return TuneResult::Rejected; }
    if (last_sent_ && *last_sent_ == dial_hz && !pending_) { ++c_.unchanged; return TuneResult::Unchanged; }
    if (now_() - last_write_t_ >= cfg_.min_interval_s) return send_locked(dial_hz);
    if (last_sent_ && *last_sent_ == dial_hz) { pending_.reset(); ++c_.unchanged; return TuneResult::Unchanged; }   // spun away and back inside the interval
    pending_ = dial_hz; ++c_.coalesced; return TuneResult::Coalesced;
}
bool Tuner::pump() {
    std::lock_guard<std::mutex> l(m_);
    if (!armed_ || !pending_ || now_() - last_write_t_ < cfg_.min_interval_s) return false;
    return send_locked(*pending_) == TuneResult::Sent;
}
std::optional<double> Tuner::last_sent_hz() const { std::lock_guard<std::mutex> l(m_); return last_sent_; }
std::optional<double> Tuner::pending_hz() const { std::lock_guard<std::mutex> l(m_); return pending_; }
Tuner::Counters Tuner::counters() const { std::lock_guard<std::mutex> l(m_); return c_; }
std::string Tuner::last_error() const { std::lock_guard<std::mutex> l(m_); return err_; }
}  // namespace fireside
