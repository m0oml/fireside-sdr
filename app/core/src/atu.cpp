#include "fireside/atu.h"
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace fireside::atu {
const char* fail_text(uint8_t r) {
    switch (r) { case 0: return "no RF detected"; case 1: return "carrier lost before the tune finished"; case 2: return "could not bring the SWR below the threshold"; default: return "unknown reason"; }
}
std::optional<Msg> parse(uint32_t raw) {
    Msg m; m.code = uint8_t(raw); m.b2 = uint8_t(raw >> 8); m.b3 = uint8_t(raw >> 16); m.remaining = uint8_t(raw >> 24);
    if (m.code == 0xFF && m.b2 == 0xFF && m.b3 == 0xFF) return std::nullopt;
    return m;
}
std::optional<uint8_t> threshold_request(double swr) {
    for (int i = 0; i < 7; ++i) if (std::fabs(swr - kThresholds[i]) < 1e-6) return uint8_t(SWR11 + i);
    return std::nullopt;
}
void apply(Status& s, const Msg& m) {
    switch (m.code) {
        case R_IND: s.ind = m.b2; break;
        case R_CAP: s.cap = m.b2; break;
        case R_HILOZ: s.hiz = m.b2; break;
        case R_ANTENNA: s.antenna = m.b2; break;
        case R_FWDPWR: s.fwd_w = ((m.b2 << 8) + m.b3) * 0.01; break;
        case R_REVPWR: s.rev_w = ((m.b2 << 8) + m.b3) * 0.01; break;
        case R_SWR: { const double ro = std::sqrt(double(m.b3) / 256.0); s.swr = (1 + ro) / (1 - ro); break; }
        case R_TXFREQ: { const int p = (m.b2 << 8) + m.b3; if (p > 0) s.tx_freq_mhz = 32768.0 / (p * 1.6); break; }
        case R_TUNEPASS: s.tune_pass = true; break;
        case R_TUNEFAIL: s.tune_pass = false; s.fail_reason = m.b2; break;
        case R_VERSION: s.version = (m.b3 >> 4) + (m.b3 & 0xF) * 0.1; break;
        case R_INSTANDBY: s.active = false; break;
        case R_ACTIVE: s.active = true; break;
        case R_SWRTHRESH: if (m.b2 < 7) s.threshold = kThresholds[m.b2]; break;
        case R_AUTO_STATUS: s.auto_on = m.b2 != 0; break;
        case R_UPDATE_STATUS: s.updates_on = m.b2 != 0; break;
        default: break;                                           // NOOP, CLEAR_DONE, STORE_OK, unknown (0xF8 was seen once in the capture)
    }
}
Clock real_clock() {
    return {[] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); },
            [](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }};
}

void Atu::send(uint8_t req, uint8_t b2, uint8_t b3) {
    Safety& s = fx_.safety(); const std::string n = "RDAL_OP_ATU_SEND_CMD";
    const bool had = s.permitted(n); s.permit(n);
    try { fx_.set(n, double(send_d1(req, b2)), double(b3)); } catch (...) { if (!had) s.unpermit(n); throw; }
    if (!had) s.unpermit(n);
}
std::optional<Msg> Atu::poll(uint32_t wait_ms) {
    auto m = parse(fx_.get("RDAL_OP_ATU_GET_RESULT", wait_ms, 0, 1.0 + wait_ms / 1000.0));
    if (m) apply(st_, *m);
    return m;
}
int Atu::drain(int max_msgs) {
    int n = 0;
    while (n < max_msgs) { auto m = poll(0); if (!m) break; ++n; if (m->remaining == 0) break; }
    return n;
}
void Atu::init() { send(AUTO_OFF); drain(); send(STANDBY); drain(); send(LOZ); drain(); }
void Atu::set_threshold(double swr) {
    auto r = threshold_request(swr); if (!r) throw std::invalid_argument("the tuner only knows SWR thresholds 1.1, 1.3, 1.5, 1.7, 2.0, 2.5 and 3.0");
    send(*r); drain();
}
void Atu::request_update() {                                      // the relays may still be switching: wait for the three reports (up to ~2 s)
    st_.ind.reset(); st_.cap.reset(); st_.hiz.reset();
    send(ALL_UPDATE);
    for (int i = 0; i < 10; ++i) {
        auto m = poll(200);
        if (st_.ind && st_.cap && st_.hiz && (!m || m->remaining == 0)) { drain(); return; }
        if (!m) clk_.sleep(0.02);
    }
}
bool Atu::set_relays(int ind, int cap, bool hiz) {
    if (ind < 0 || ind > 127 || cap < 0 || cap > 127) throw std::invalid_argument("inductor and capacitor values are 0..127");
    send(SET_IND, uint8_t(ind | (hiz ? 0x80 : 0))); drain();     // "set inductor (and HiLoZ)": bit 7 = Hi-Z (on the radio 2026-11-19: SET_IND 30 cleared Hi-Z,
    send(SET_CAP, uint8_t(cap)); drain();                         //  and a separate HIZ request after it did not set it again)
    request_update();
    return st_.ind && *st_.ind == ind && st_.cap && *st_.cap == cap && st_.hiz && (*st_.hiz != 0) == hiz;
}
Atu::TuneOutcome Atu::tune(bool full, double threshold, const std::function<bool()>& keyed, double timeout_s) {
    TuneOutcome o; const double t0 = clk_.now();
    set_threshold(threshold);
    drain();
    st_.tune_pass.reset(); st_.fail_reason = 0;
    send(full ? FULL_TUNE : MEM_TUNE);
    bool done = false;
    while (clk_.now() - t0 < timeout_s) {
        if (!keyed()) break;                                      // the gate unkeyed (timeout, error, power cap): abandon
        auto m = poll(200);
        if (m) {
            o.msgs.push_back(*m);
            if (m->code == R_TUNEPASS || m->code == R_TUNEFAIL) done = true;
            if (done && m->remaining == 0) break;                 // the reports that follow a pass (relays, power, SWR) have been read
        } else if (done) break;
        clk_.sleep(0.035);
    }
    o.seconds = clk_.now() - t0; o.status = st_;
    if (done && st_.tune_pass) o.result = *st_.tune_pass ? TuneResult::Pass : TuneResult::Fail;
    o.fail_reason = st_.fail_reason;
    return o;
}
}  // namespace fireside::atu
