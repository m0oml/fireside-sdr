#include "fireside/tx_gate.h"
#include <algorithm>
#include <cstdio>

namespace fireside {
namespace { const char* kMox = "RDAL_OP_SET_MOX"; }

TxGate::~TxGate() { try { unkey("gate destroyed"); } catch (...) {} }
void TxGate::note(const std::string& s) { char b[32]; std::snprintf(b, sizeof b, "%.3f ", clk_.now()); log_.push_back(b + s); }

void TxGate::disarm(const std::string& why) { unkey(why); armed_ = false; }

bool TxGate::key(const std::string& purpose, double max_s, std::string* why_not) {
    auto refuse = [&](const std::string& w) { note("refused: " + w); if (why_not) *why_not = w; return false; };
    if (!armed_) return refuse("transmit is not armed");
    if (keyed_) return refuse("already keyed");
    if (!(max_s > 0)) return refuse("no key-down time given");
    Safety& s = fx_.safety();
    had_permit_ = s.permitted(kMox); s.permit(kMox); s.set_tx_grant(true);
    keyed_ = true; deadline_ = clk_.now() + std::min(max_s, cfg_.max_key_s); fwd_since_ = rev_since_ = swr_since_ = -1;   // marked keyed BEFORE sending: if the send half-works, unkey still runs
    try { fx_.set(kMox, 1, 0); fx_.sync(); }                      // wait until the firmware has taken it (a write is otherwise unacknowledged)
    catch (const std::exception& e) { unkey(std::string("key failed: ") + e.what()); return refuse(std::string("the radio refused the key: ") + e.what()); }
    note("KEY " + purpose); return true;
}

void TxGate::unkey(const std::string& why) {
    if (!keyed_) return;
    keyed_ = false; last_reason_ = why;
    Safety& s = fx_.safety(); s.set_tx_grant(false);              // first: nothing can start a tune or re-key from here on
    std::string err;
    for (int attempt = 0; attempt < 3; ++attempt) {               // the unkey must get through: three tries, 50 ms apart
        try { fx_.set(kMox, 0, 0); fx_.sync(); err.clear(); break; } catch (const std::exception& e) { err = e.what(); clk_.sleep(0.05); }
    }
    if (!err.empty()) { unkey_failed_ = true; last_reason_ = why + " (SET_MOX 0 FAILED 3 times: " + err + ": the radio may still be keyed, power it off)"; }
    if (!had_permit_) s.unpermit(kMox);
    note("UNKEY " + why + (err.empty() ? "" : " (SET_MOX 0 FAILED: " + err + ")"));
}

void TxGate::tick() { if (keyed_ && clk_.now() >= deadline_) unkey("key-down time limit"); }

void TxGate::report(const txm::Reading& r) {
    if (!keyed_) return;
    if (r.fwd_w > cfg_.hard_fwd_w) { char b[112]; std::snprintf(b, sizeof b, "forward power %.1f W above the %.1f W limit", r.fwd_w, cfg_.hard_fwd_w); unkey(b); return; }   // never ridden out
    const double now = clk_.now();
    auto over = [&](bool cond, double& since) { if (!cond) { since = -1; return false; } if (since < 0) since = now; return now - since >= cfg_.grace_s; };   // over the limit for grace_s (0: at once)
    if (over(r.fwd_w > cfg_.max_fwd_w, fwd_since_)) { char b[96]; std::snprintf(b, sizeof b, "forward power %.1f W above the %.1f W limit", r.fwd_w, cfg_.max_fwd_w); unkey(b); return; }
    if (over(r.rev_w > cfg_.max_rev_w, rev_since_)) { char b[96]; std::snprintf(b, sizeof b, "reflected power %.1f W above the %.1f W limit", r.rev_w, cfg_.max_rev_w); unkey(b); return; }
    if (over(r.swr && r.fwd_w >= cfg_.swr_min_fwd_w && *r.swr > cfg_.max_swr, swr_since_)) { char b[96]; std::snprintf(b, sizeof b, "SWR %.1f above the %.1f limit", *r.swr, cfg_.max_swr); unkey(b); }
}

std::optional<atu::Atu::TuneOutcome> run_atu_tune(TxGate& gate, atu::Atu& atu, const txm::PaCal& cal, int band, double threshold, bool full,
                                                   uint32_t restore_dsp_mode, const std::function<bool()>& carrier_ready, TuneMeter* meter, std::string* why_not,
                                                   const std::function<void(bool)>& carrier, const std::function<void(const txm::Reading&)>& on_poll) {
    if (!atu::threshold_request(threshold)) { if (why_not) *why_not = "the tuner only knows thresholds 1.1, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0"; return std::nullopt; }
    if (!carrier_ready || !carrier_ready()) { if (why_not) *why_not = "no tune carrier on the I/Q stream"; return std::nullopt; }
    if (!gate.armed()) { if (why_not) *why_not = "transmit is not armed"; return std::nullopt; }
    Flex& fx = gate.flex();
    fx.set("RDAL_OP_SET_TX_DSP_MODE", 4, 0);                      // CWU, as PowerSDR does for the tune carrier
    struct Restore { Flex& fx; TxGate& g; uint32_t mode; const std::function<void(bool)>& carrier;   // whatever happens: carrier off, unkey, then the operating mode back
        ~Restore() { try { if (carrier) carrier(false); } catch (...) {} try { g.unkey("tune finished"); } catch (...) {} try { fx.set("RDAL_OP_SET_TX_DSP_MODE", mode, 0); } catch (...) {} } }
        restore{fx, gate, restore_dsp_mode, carrier};
    if (!gate.key("ATU tune", gate.config().max_key_s, why_not)) return std::nullopt;
    if (carrier) carrier(true);
    bool fwd_next = true; int adc_fwd = 0, adc_rev = 0;
    auto keyed = [&] {
        gate.tick();
        if (!gate.keyed()) return false;
        try {                                                     // one PA detector channel per poll, alternating (as PowerSDR)
            if (fwd_next) adc_fwd = int(fx.get("RDAL_OP_READ_PA_ADC", txm::kAdcFwd)); else adc_rev = int(fx.get("RDAL_OP_READ_PA_ADC", txm::kAdcRev));
        } catch (const std::exception& e) { gate.unkey(std::string("PA meter read failed: ") + e.what()); return false; }
        fwd_next = !fwd_next;
        const txm::Reading rd = txm::read(cal, band, adc_fwd, adc_rev);
        if (meter) { meter->readings.push_back(rd); meter->max_fwd_w = std::max(meter->max_fwd_w, rd.fwd_w); }
        gate.report(rd);
        if (on_poll && gate.keyed()) on_poll(rd);
        return gate.keyed();
    };
    auto out = atu.tune(full, threshold, keyed, gate.config().max_key_s + 1.0);   // the gate's own limit ends the key-down first
    return out;
}
}  // namespace fireside
