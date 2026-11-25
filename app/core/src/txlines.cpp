#include "fireside/txlines.h"
#include "fireside/rxant.h"
#include "fireside/startup.h"

namespace fireside {
const char* line_role_id(LineRole r) {
    switch (r) { case LineRole::Band: return "band"; case LineRole::Amplifier: return "amp"; case LineRole::Transverter: return "xvtr"; default: return "unused"; }
}
LineRole line_role_from_id(const std::string& id) {
    if (id == "band") return LineRole::Band; if (id == "amp") return LineRole::Amplifier; if (id == "xvtr") return LineRole::Transverter; return LineRole::Unused;
}

TxLines::TxLines(Flex& fx, atu::Clock clock) : fx_(fx), clk_(std::move(clock)) { require_firewire(fx_); }

std::array<bool, kTxLines> TxLines::wanted(const LineConfig& c, double hz) {
    std::array<bool, kTxLines> w{}; const int b = rx_band_index(hz);
    for (int i = 0; i < kTxLines; ++i) w[size_t(i)] = c.role[size_t(i)] == LineRole::Band && b >= 0 && b < 32 && ((c.band_mask[size_t(i)] >> b) & 1u);
    return w;
}
bool TxLines::any_used(const LineConfig& c) { for (auto r : c.role) if (r == LineRole::Band) return true; return false; }

void TxLines::configure(const LineConfig& c) {
    LineConfig v = c; if (!(v.settle_s >= 0.01)) v.settle_s = 0.01; if (v.settle_s > 1.0) v.settle_s = 1.0;
    std::lock_guard<std::mutex> l(m_); cfg_ = v;
}
LineConfig TxLines::config() const { std::lock_guard<std::mutex> l(m_); return cfg_; }
void TxLines::arm() {
    if (!fx_.safety().allow_write) throw SafetyError("TxLines::arm: the Flex client has writes disabled");
    std::lock_guard<std::mutex> l(m_); armed_ = true; cur_ = {{-1, -1, -1}}; amp_off_ = {}; err_.clear();   // a new start-up: nothing known about the lines
}
void TxLines::disarm() { std::lock_guard<std::mutex> l(m_); armed_ = false; }
bool TxLines::armed() const { std::lock_guard<std::mutex> l(m_); return armed_; }
void TxLines::set_busy(std::function<bool()> b) { std::lock_guard<std::mutex> l(m_); busy_ = std::move(b); }
std::array<int, kTxLines> TxLines::state() const { std::lock_guard<std::mutex> l(m_); return cur_; }
std::string TxLines::last_error() const { std::lock_guard<std::mutex> l(m_); return err_; }

namespace {
const char* const kRca[kTxLines] = {"RDAL_OP_SET_RCA_TX1", "RDAL_OP_SET_RCA_TX2", "RDAL_OP_SET_RCA_TX3"};
const char* const kAmp[kTxLines] = {"RDAL_OP_SET_AMP_TX1", "RDAL_OP_SET_AMP_TX2", "RDAL_OP_SET_AMP_TX3"};
}

TxLines::Result TxLines::set_locked(const std::array<bool, kTxLines>& want) {
    bool changed = false;
    try {
        for (int i = 0; i < kTxLines; ++i) {
            const size_t k = size_t(i); const bool band = cfg_.role[k] == LineRole::Band;
            if (!band && !(ever_used_[k] && cur_[k] != 0)) continue;     // never written unless it is (or was, and is still on) a band line
            const int need = band && want[k] ? 1 : 0;
            if (!amp_off_[k]) {                                          // "do not follow transmit": the only value the gate lets through
                Safety& s = fx_.safety(); const bool had = s.permitted(kAmp[k]); s.permit(kAmp[k]);
                try { fx_.set(kAmp[k], 0, 0); } catch (...) { if (!had) s.unpermit(kAmp[k]); throw; }
                if (!had) s.unpermit(kAmp[k]);
                amp_off_[k] = true;
            }
            if (cur_[k] != need) { fx_.set(kRca[k], double(need), 0); cur_[k] = need; ever_used_[k] = true; changed = true; }
        }
        fx_.sync();
    } catch (const std::exception& e) { armed_ = false; err_ = e.what(); return Result::Fault; }
    if (changed) { last_change_ = clk_.now(); return Result::Sent; }
    return Result::Unchanged;
}

TxLines::Result TxLines::apply(double hz) {
    std::lock_guard<std::mutex> l(m_);
    if (!armed_) return Result::NotArmed;
    if (busy_ && busy_()) return Result::Busy;                          // never during an over (the dial is locked then anyway)
    return set_locked(wanted(cfg_, hz));
}

std::string TxLines::for_over(double hz) {
    double wait = 0;
    {
        std::lock_guard<std::mutex> l(m_);
        bool needed = any_used(cfg_); for (int i = 0; i < kTxLines; ++i) needed = needed || (ever_used_[size_t(i)] && cur_[size_t(i)] != 0);
        if (!needed) return "";                                          // no line in use: transmit is not affected at all
        if (!armed_) return "the rear TX lines are not ready" + (err_.empty() ? std::string() : ": " + err_);
        if (set_locked(wanted(cfg_, hz)) == Result::Fault) return "a rear TX line could not be set: " + err_;
        wait = cfg_.settle_s - (clk_.now() - last_change_);
    }
    if (wait > 1e-6) clk_.sleep(wait);                                     // the relay settles before any RF (no hot switching)
    return "";
}

void TxLines::release() {
    std::lock_guard<std::mutex> l(m_);
    if (armed_)
        for (int i = 0; i < kTxLines; ++i)
            if (cur_[size_t(i)] == 1) { try { fx_.set(kRca[i], 0, 0); fx_.sync(); cur_[size_t(i)] = 0; } catch (const std::exception& e) { err_ = e.what(); } }
    armed_ = false;
}
}  // namespace fireside
