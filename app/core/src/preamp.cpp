#include "fireside/preamp.h"
#include "fireside/startup.h"

namespace fireside {
RxPreamp::RxPreamp(Flex& fx) : fx_(fx) { require_firewire(fx_); }
void RxPreamp::arm() {
    if (!fx_.safety().allow_write) throw SafetyError("RxPreamp::arm: the Flex client has writes disabled");
    std::lock_guard<std::mutex> l(m_); armed_ = true; on_ = false; err_.clear();
}
void RxPreamp::disarm() { std::lock_guard<std::mutex> l(m_); armed_ = false; }
bool RxPreamp::armed() const { std::lock_guard<std::mutex> l(m_); return armed_; }
bool RxPreamp::on() const { std::lock_guard<std::mutex> l(m_); return on_; }
std::string RxPreamp::last_error() const { std::lock_guard<std::mutex> l(m_); return err_; }
PreampResult RxPreamp::request(bool on) {
    std::lock_guard<std::mutex> l(m_);
    if (!armed_) return PreampResult::NotArmed;
    if (on == on_) return PreampResult::Unchanged;
    try { fx_.set("SET_TRX_PREAMP", on ? 1.0 : 0.0, 0.0); }                // the only write in this file: d1 = 0 or 1, d2 = 0 (as PowerSDR's SetTRXPreamp)
    catch (const std::exception& e) { armed_ = false; err_ = e.what(); return PreampResult::Fault; }
    on_ = on; return PreampResult::Sent;
}
}  // namespace fireside
