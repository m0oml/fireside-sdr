#include "fireside/tx_control.h"
#include "fireside/siggen.h"
#include "fireside/dsp.h"
#include <atomic>
#include <thread>
#include "fireside/decode.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include "fireside/tuning.h"
#include "fireside/tx_out.h"
#include "fireside/tx_render.h"

namespace fireside::txc {
constexpr double kOverGraceS = 0.15;           // FT8/FT4, voice and other programs: how long a reading may sit above a limit (two or three readings) before unkeying
// The forward-power limits of an over scale with the set power, but never above fixed ceilings tied to the PA's rating (kMaxWatts): at 100 W the
// sustained limit is 110 W (not 150 W) and the instant one 130 W (not 300 W).
double fwd_limit_w(double power_w) { return std::min(std::max(2.0, 1.5 * power_w), kFwdCeilingW); }
double hard_fwd_limit_w(double power_w) { return std::min(std::max(6.0, 3.0 * power_w), kHardFwdCeilingW); }
bool TxControl::is_safety_stop(const std::string& r) {
    for (const char* k : {" above ", "time limit", "FAILED", " failed", "under 5 mW", "refused the key"}) if (r.find(k) != std::string::npos) return true;
    return false;                                     // "ramp finished", "over finished", "tune finished", "PTT released", "aborted", ...: normal ends
}
namespace { void say(const LogFn& l, const std::string& s) { if (l) l(s); } }

TxControl::TxControl(Flex& fx, PcmFactory pcm, atu::Clock clock) : fx_(fx), pcm_(std::move(pcm)), clk_(clock), atu_(fx, clock) {
    utc_ = [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); };
}
TxControl::~TxControl() { disarm(); }

void TxControl::arm(const std::vector<uint8_t>& now, const std::vector<uint8_t>& backup, const LogFn& log) {
    std::lock_guard<std::mutex> l(op_);
    armed_ = false;
    if (retired_) throw std::runtime_error("the radio session has ended: start the radio again");
    if (backup.empty()) throw std::runtime_error("no EEPROM backup to compare with");
    if (now != backup) {
        size_t n = 0, first = 0; const size_t m = std::min(now.size(), backup.size()); for (size_t i = 0; i < m; ++i) if (now[i] != backup[i]) { if (!n) first = i; ++n; }
        if (now.size() != backup.size()) { if (!n) first = m; n += std::max(now.size(), backup.size()) - m; }   // the bytes one image has and the other lacks
        char b[400]; std::snprintf(b, sizeof b, "the radio's EEPROM differs from the saved backup (%zu byte%s differ%s, first at 0x%zX; sizes %zu / %zu): transmit refused, because the PA bias comes from it. "
            "Dump it (firesidectl eeprom-dump) and compare; if the change is expected, remove the old backup and the app will offer to save a new one", n, n == 1 ? "" : "s", n == 1 ? "s" : "", first, now.size(), backup.size());
        throw std::runtime_error(b);
    }
    auto c = txs::from_image(now); if (!c.valid) throw std::runtime_error("transmit calibration: " + c.problem);
    txs::write_bias(fx_, c, log);
    cal_ = std::move(c); armed_ = true;
    say(log, "transmit armed: EEPROM identical to the backup, PA bias set from it");
}
void TxControl::arm_from_radio(const std::vector<uint8_t>& backup, const LogFn& log) {
    if (retired_) throw std::runtime_error("the radio session has ended: start the radio again");
    say(log, "reading the EEPROM (read-only) to compare with the backup ...");
    std::vector<uint8_t> now; { std::lock_guard<std::mutex> l(op_); now = read_image(fx_, "TRX", backup.empty() ? kTrxLen : backup.size()); }
    arm(now, backup, log);
}
TxControl::BackupSaved TxControl::backup_eeprom(const std::string& dir, const LogFn& log) {
    if (retired_) throw std::runtime_error("the radio session has ended: start the radio again");
    say(log, "reading the EEPROM twice (read-only) to save a reference backup ...");
    BackupSaved r;
    { std::lock_guard<std::mutex> l(op_); r.image = read_verified(fx_, "TRX", kTrxLen, 2); }                 // throws unless both reads are identical
    const auto c = txs::from_image(r.image);
    if (!c.valid) throw std::runtime_error("this EEPROM is not a usable transmit calibration (" + c.problem + "): nothing was saved");
    r.paths = save_backup(dir, {{"TRX", r.image}});
    say(log, "EEPROM backup saved: " + (r.paths.empty() ? std::string("(no file)") : r.paths.front()));
    return r;
}
// armed_ goes false BEFORE waiting: an operation queued behind the running one clears abort_ when it starts (Busy), but then finds itself disarmed and refuses.
void TxControl::disarm() { armed_ = false; abort_ = true; std::lock_guard<std::mutex> l(op_); armed_ = false; }   // waits for a running operation (it sees abort_ and unkeys)
void TxControl::retire() { retired_ = true; disarm(); }

double TxControl::k_for(int band) const { auto it = k_.find(band); return it != k_.end() ? it->second : 0.0; }
std::map<int, double> parse_drive_text(const std::string& text) {
    std::map<int, double> m; std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line); std::string kv; int band = -1; double k = 0;
        while (ls >> kv) { const auto eq = kv.find('='); if (eq == std::string::npos) continue;
            const std::string key = kv.substr(0, eq), v = kv.substr(eq + 1);
            try { if (key == "band") band = txm::band_from_name(v); else if (key == "k") k = std::stod(v); } catch (...) {} }
        if (band >= 0 && k > 1000 && k < 1e7) m[band] = k;
    }
    return m;
}
std::string drive_text(const std::map<int, double>& k) {
    std::ostringstream o; for (auto& [b, v] : k) o << "band=" << txm::band_name(b) << " k=" << long(std::lround(v)) << "\n"; return o.str();
}
void TxControl::set_k(int band, double k) { if (band >= 0 && band < txm::kBands && k > 1000) k_[band] = k; }

void TxControl::prepare(double mhz, uint32_t mode, int ant) {
    if (ant < 1 || ant > 3) throw std::invalid_argument("transmit antenna must be ANT1..ANT3");
    fx_.set("RDAL_OP_SET_TX_ANT", uint32_t(ant), 0);
    fx_.set("RDAL_OP_SET_TX_DSP_MODE", mode, 0);
    fx_.set("RDAL_OP_SET_TX_FREQ_TW", double(freq_to_tw(mhz)), mhz);
    fx_.set("RDAL_OP_SET_TX_DSP_FILTER", 200, 3100);              // as PowerSDR does
    fx_.set("RDAL_OP_SET_TX_OFFSET", 0, 0);
    txs::write_carrier_null(fx_, cal_, mhz); fx_.sync();
}

double TxControl::test_cap(int band) const {     // tunes, drive ramps and test tones (15 W at most): the hard clamp stays at 30 W, as during bring-up
    const double k = k_for(band);
    return std::min(drive_cap(band), k > 1000 ? std::sqrt(30.0 / k) : txs::drive_for_watts(cal_, band, 30.0));
}
double TxControl::drive_cap(int band) const {
    // The drive ceiling for any over on this band: kMaxWatts + 10 % (room for the closed loop) by the band's MEASURED drive level when there is one,
    // else the EEPROM table's 100 W drive (it reads ~5-10 dB low on this radio: that gives far less than 100 W). Never above kAbsoluteMaxDrive,
    // which ToneSource enforces again whatever is asked.
    const double k = k_for(band);
    const double d = k > 1000 ? std::sqrt(1.1 * txs::kMaxWatts / k) : txs::drive_for_watts(cal_, band, 100.0);
    return std::min(d, txs::kAbsoluteMaxDrive);
}
RampResult TxControl::ramp(double mhz, double target_w, const Options& o, const ProgressFn& p, const LogFn& log, double hold_s) {
    std::lock_guard<std::mutex> l(op_); RampResult r; Busy b(*this);
    if (!armed_) { r.stop = "transmit is not armed"; return r; }
    const int band = txm::band_of(mhz * 1e6);
    if (band < 0) { r.stop = "outside the amateur bands"; return r; }
    if (!(target_w > 0) || target_w > 15) { r.stop = "ramp target must be 0..15 W"; return r; }   // 15 W (user 2026-11-19: test tones)
    if (!(hold_s > 0) || hold_s > 10) { r.stop = "hold must be 0..10 s"; return r; }
    const double d1w = txs::drive_for_watts(cal_, band, 1.0), dcap = test_cap(band);
    if (o.tx_ant < 1 || o.tx_ant > 3) { r.stop = "transmit antenna must be ANT1..ANT3"; return r; }
    prepare(mhz, 1, o.tx_ant);
    txo::ToneSource src(o.rate, 8); src.set_cap(dcap); src.set_target(0); src.set_tone_hz(o.tone_hz); src.set_mirror(o.mirror_q);
    src.set_correction(cal_.image_gain[size_t(band)], cal_.image_phase[size_t(band)]);
    txo::Stream st(pcm_(o.rate), src); clk_.sleep(0.3);
    if (st.failed()) { r.stop = "the I/Q stream failed to start"; return r; }
    GateConfig gc; gc.max_key_s = (target_w > 5 ? 10.0 : 6.0) + hold_s; gc.max_fwd_w = std::min(22.5, std::max(1.0, 2 * target_w)); gc.max_swr = 3.0; gc.swr_min_fwd_w = 0.5;
    KeyHook kh(key_hook_);
    TxGate gate(fx_, gc, clk_); gate.arm(); std::string why;
    if (!gate.key("drive ramp", gc.max_key_s, &why)) { r.stop = why; r.unkey_failed = gate.unkey_failed(); note_stop(why); return r; }
    keyed_ = true; src.set_keyed(true);
    const double step = std::pow(10.0, 0.1), t0 = clk_.now(); double drive = d1w / 10, hold = -1, next = 0.2, last_step = 0, last_trim = 0, kmax = 0, cmp_t = -1;
    MeterCompare cmp_; bool cmp_pending = false;
    auto now = [&] { return clk_.now() - t0; };
    src.set_target(drive);
    while (gate.keyed()) {
        if (abort_) { gate.unkey("aborted"); break; }
        if (st.failed()) { gate.unkey("the I/Q stream failed"); break; }
        txm::Reading rd; int af = 0, ar = 0;
        try { af = int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcFwd)); ar = int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcRev)); rd = txm::read(cal_.meter, band, af, ar); }
        catch (const std::exception& e) { gate.unkey(std::string("meter read failed: ") + e.what()); break; }
        r.max_fwd_w = std::max(r.max_fwd_w, rd.fwd_w); if (p) p({now(), src.level(), rd});
        if (hold >= 0) {                                                               // meter comparison (no RF of its own: report requests only)
            if (now() - cmp_t >= 0.5) {
                if (cmp_pending) { MeterCompare c = cmp_; const auto& a = atu_.status(); c.atu_fwd_w = a.fwd_w; c.atu_rev_w = a.rev_w; c.atu_swr = a.swr; r.compare.push_back(c); }
                cmp_ = MeterCompare{now(), af, ar, rd.fwd_w, rd.rev_w, rd.swr.value_or(0)}; cmp_t = now(); cmp_pending = true;
                try { atu_.drain(); atu_.clear_meter(); atu_.send(atu::FWD_PWR); atu_.send(atu::REV_PWR); atu_.send(atu::SWR); } catch (const std::exception&) { cmp_pending = false; }
            } else { try { atu_.poll(5); } catch (const std::exception&) {} }
        }
        gate.report(rd); gate.tick(); if (!gate.keyed()) break;
        if (rd.fwd_w > 1.5 * target_w) { gate.unkey("forward power above 1.5 x the target"); break; }
        if (drive >= d1w * 0.999 && r.max_fwd_w < 0.005) { gate.unkey("under 5 mW at the table's 1 W drive point: wrong channel, sign or path"); break; }
        if (rd.fwd_w >= 0.05 && src.level() > 0 && now() - last_step >= 0.15) kmax = std::max(kmax, rd.fwd_w / (src.level() * src.level()));
        // Near the target the meter lags (~one 0.2 s step on the radio): there wait long enough for it, then go to the target in proportion;
        // hold only within +-10 %, and trim towards the target while holding (the 15 W test tone held at 18-20 W before, 2026-11-19).
        const double settle = rd.fwd_w > 0.25 * target_w ? 0.45 : 0.2, since = now() - last_step;
        if (hold < 0 && since >= settle && rd.fwd_w >= 0.9 * target_w && rd.fwd_w <= 1.1 * target_w) { hold = now() + hold_s; last_trim = now(); }
        if (hold >= 0 && now() >= hold) { r.reached = true; gate.unkey("ramp finished at the target"); break; }
        if (hold < 0 && now() >= next && since >= settle) {
            const double ratio = rd.fwd_w > 0.3 ? target_w / rd.fwd_w : 1e9;      // power still to go (meaningless below 0.3 W)
            drive = std::min(ratio < step * step ? drive * std::sqrt(ratio) : drive * step, dcap);   // the last step lands on the target
            src.set_target(drive); last_step = now(); next = now() + settle;
        }
        if (hold >= 0 && now() - last_trim >= 0.4 && rd.fwd_w > 0.3 && std::fabs(rd.fwd_w / target_w - 1) > 0.05) {
            drive = std::min(drive * std::clamp(std::sqrt(target_w / rd.fwd_w), 0.9, 1.1), dcap); src.set_target(drive); last_trim = now();
        }
        clk_.sleep(0.05);
    }
    src.set_keyed(false); gate.ensure_unkeyed(2.0); keyed_ = false;   // a failed unkey: keep trying for 2 s before reporting it
    r.stop = gate.last_unkey_reason(); r.seconds = now(); r.k = kmax; r.unkey_failed = gate.unkey_failed(); note_stop(r.stop);
    if (r.reached && kmax > 1000) set_k(band, kmax);
    char m[160]; std::snprintf(m, sizeof m, "ramp: %s, max %.2f W, %.0f W per drive^2", r.stop.c_str(), r.max_fwd_w, kmax); say(log, m);
    for (const auto& c : r.compare) {
        char x[240]; auto f = [](const std::optional<double>& v, const char* fmt) { char b[24]; if (!v) return std::string("-"); std::snprintf(b, sizeof b, fmt, *v); return std::string(b); };
        std::snprintf(x, sizeof x, "meter %.1f s: ours adc fwd %d rev %d = %.2f W / %.2f W, SWR %.2f | tuner %s W / %s W, SWR %s", c.t, c.adc_fwd, c.adc_rev, c.fwd_w, c.rev_w, c.swr,
                      f(c.atu_fwd_w, "%.2f").c_str(), f(c.atu_rev_w, "%.2f").c_str(), f(c.atu_swr, "%.2f").c_str());
        say(log, x);
    }
    return r;
}

TuneResult TxControl::tune(double mhz, double power_w, double threshold, const Options& o, const ProgressFn& p, const LogFn& log) {
    std::lock_guard<std::mutex> l(op_); TuneResult r; Busy b(*this);
    if (!armed_) { r.why_not = "transmit is not armed"; return r; }
    const int band = txm::band_of(mhz * 1e6);
    if (band < 0) { r.why_not = "outside the amateur bands"; return r; }
    if (!(power_w >= 1) || power_w > 15) { r.why_not = "tune power must be 1..15 W"; return r; }
    if (o.tx_ant != 1) { r.why_not = "the ATU is on ANT1 only: this band transmits on ANT" + std::to_string(o.tx_ant); return r; }
    if (!atu::threshold_request(threshold)) { r.why_not = "the tuner only knows thresholds 1.1, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0"; return r; }
    // The drive: this band's measured level if there is one; otherwise the EEPROM power table, which on 20 m gave ~5 dB LESS than the power it names
    // (so a first tune on a new band errs low; the tuner says "no RF" if it is too little). A measurement needs a match, so the first tune comes first.
    const double k = k_for(band), dcap = test_cap(band);
    const double drive = k > 1000 ? std::sqrt(power_w / k) : txs::drive_for_watts(cal_, band, power_w);
    r.estimated_drive = !(k > 1000);
    if (!(drive > 0)) { r.why_not = "no drive for this band"; return r; }
    if (r.estimated_drive) say(log, std::string("no measured drive on ") + txm::band_name(band) + ": using the EEPROM power table (errs low); press Measure drive after a good tune");
    if (drive > dcap) { r.why_not = "the drive for this power is above the tune ceiling (30 W)"; return r; }
    prepare(mhz, 1, 1);                                           // tune: ANT1 (the ATU's)
    atu_.drain(); atu_.init(); clk_.sleep(1.5); atu_.drain();                  // relays settle (two runs keyed straight after a reset saw an open-like load)
    auto rep = atu_report_locked();
    if (!rep || !rep->ind || !rep->cap || !rep->hiz) { r.why_not = "the tuner did not report its relays: not keying"; return r; }
    txo::ToneSource src(o.rate, 8); src.set_cap(std::min(dcap, drive * 1.25)); src.set_target(drive); src.set_tone_hz(o.tone_hz); src.set_mirror(o.mirror_q);
    src.set_correction(cal_.image_gain[size_t(band)], cal_.image_phase[size_t(band)]);
    txo::Stream st(pcm_(o.rate), src); clk_.sleep(0.3);
    if (st.failed()) { r.why_not = "the I/Q stream failed to start"; return r; }
    GateConfig gc; gc.max_key_s = 15.0; gc.max_fwd_w = 30.0; gc.max_rev_w = 20.0; gc.max_swr = 1e9;   // fixed limits while the tuner searches (see firesidekey / analysis)
    KeyHook kh(key_hook_);
    TxGate gate(fx_, gc, clk_); gate.arm(); TuneMeter meter;
    const double t0 = clk_.now();
    if (abort_) { r.why_not = "stopped before keying"; return r; }
    auto ready = [&] { return !st.failed() && !abort_; };
    auto carrier = [&](bool on) { src.set_keyed(on); keyed_ = on; };
    auto poll = [&](const txm::Reading& rd) { if (p) p({clk_.now() - t0, src.level(), rd}); if (abort_) gate.unkey("aborted"); else if (st.failed()) gate.unkey("the I/Q stream failed"); };
    r.outcome = run_atu_tune(gate, atu_, cal_.meter, band, threshold, true, 1, ready, &meter, &r.why_not, carrier, poll);
    if (!r.outcome && abort_) r.why_not = "stopped before keying";
    for (auto& x : meter.readings) { if (x.fwd_w > 0.5) { r.last_fwd_w = x.fwd_w; r.last_swr = x.swr; } }
    gate.ensure_unkeyed(2.0);
    r.max_fwd_w = meter.max_fwd_w; r.stop = gate.last_unkey_reason(); r.unkey_failed = gate.unkey_failed(); keyed_ = false; note_stop(r.stop);
    // A pass only counts when the tuner finished by itself ("tune finished") and reports a usable SWR. A "pass" read after our own limit cut the
    // carrier (the key-down time limit, a power limit, Stop) is not a match: seen 2026-11-21 on 12 m (15 s limit, SWR 1022 stored).
    if (r.outcome && r.outcome->result == atu::Atu::TuneResult::Pass) {
        const auto& s = r.outcome->status;
        if (r.stop != "tune finished") r.pass_rejected = "the tuner reported a pass only after the carrier was cut (" + r.stop + "): not a match, nothing saved";
        else if (s.swr && *s.swr > 3.0) { char b[120]; std::snprintf(b, sizeof b, "the tuner reported a pass at SWR %.1f: not a match, nothing saved", *s.swr); r.pass_rejected = b; }
        if (!r.pass_rejected.empty()) { r.outcome->result = atu::Atu::TuneResult::Timeout; say(log, "tune: " + r.pass_rejected); }
    }
    if (r.outcome && r.outcome->result == atu::Atu::TuneResult::Pass && r.outcome->status.ind && r.outcome->status.cap && r.outcome->status.hiz) {
        const auto& s = r.outcome->status; atu::Setting set; set.ind = *s.ind; set.cap = *s.cap; set.hiz = *s.hiz != 0;
        set.swr = s.swr ? *s.swr : 0; set.fwd_w = s.fwd_w ? *s.fwd_w : 0; set.freq_hz = mhz * 1e6;
        char ts[32]; std::time_t t = std::time(nullptr); std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t)); set.utc = ts;
        mem_.store(band, set); r.saved = set;
    }
    char m[200]; std::snprintf(m, sizeof m, "tune: %s (%s), max %.1f W", !r.outcome ? "refused" : r.outcome->result == atu::Atu::TuneResult::Pass ? "PASS" :
                               r.outcome->result == atu::Atu::TuneResult::Fail ? "FAIL" : "stopped", r.stop.c_str(), r.max_fwd_w); say(log, m);
    return r;
}

std::optional<atu::Status> TxControl::atu_report_locked() { if (retired_ || !armed_) return std::nullopt; try { atu_.request_update(); return atu_.status(); } catch (...) { return std::nullopt; } }
std::optional<atu::Status> TxControl::atu_report() {              // never alongside an over or a tune (they use the same tuner link): busy reads as no report
    std::unique_lock<std::mutex> l(op_, std::try_to_lock);
    if (!l.owns_lock()) return std::nullopt;
    return atu_report_locked();
}

bool TxControl::restore_relays(int band, const LogFn& log) {
    std::lock_guard<std::mutex> l(op_);
    if (!armed_) { say(log, "transmit is not armed"); return false; }
    auto s = mem_.recall(band); if (!s) { say(log, std::string("nothing stored for ") + txm::band_name(band)); return false; }
    const bool ok = atu_.set_relays(s->ind, s->cap, s->hiz);
    char m[120]; std::snprintf(m, sizeof m, "ATU %s: ind %d cap %d %s %s", txm::band_name(band), s->ind, s->cap, s->hiz ? "Hi-Z" : "Lo-Z", ok ? "restored (no RF)" : "NOT confirmed by the tuner"); say(log, m);
    return ok;
}
}  // namespace fireside::txc

namespace fireside::txc {
namespace {
// A rendered slot (48 kHz, sample 0 = slot start) at the stream rate, envelope normalised to 1. The resampler's filter delay is measured and removed so
// sample n is still slot start + n / rate (the FT8 timing). A proper filter, not interpolation: images would be transmitted ~46 kHz from the carrier.
}  // namespace

TxControl::FtPrepared TxControl::prepare_ft(const std::string& text, double audio_hz, bool ft4, double rate) {
    if (!(audio_hz >= 200 && audio_hz <= 3000)) throw std::invalid_argument("audio frequency must be 200..3000 Hz");
    if (text.find_first_not_of(' ') == std::string::npos) throw std::invalid_argument("no message to send");
    if (!(rate == 48000 || rate == 96000 || rate == 192000)) throw std::invalid_argument("stream rate must be 48, 96 or 192 kHz");
    const tx::DigiMode mode = ft4 ? tx::DigiMode::FT4 : tx::DigiMode::FT8;
    FtPrepared f; f.rate = rate; f.ft4 = ft4; f.text = text; f.audio_hz = audio_hz;
    f.buf = decode::synth_ft_iq(text, audio_hz, rate, ft4, tx::default_start_offset_s(mode));   // the analytic GFSK directly at the stream rate (ms, not ~3 s)
    if (f.buf.empty()) throw std::runtime_error("cannot encode this message: \"" + text + "\"");
    return f;
}
SendResult TxControl::send_ft(double mhz, const std::string& text, double audio_hz, bool ft4, tx::Parity parity, double power_w, const Options& o,
                               const ProgressFn& p, const LogFn& log) {
    FtPrepared f;
    try { f = prepare_ft(text, audio_hz, ft4, o.rate); } catch (const std::exception& e) { SendResult r; r.why_not = e.what(); if (log) log(std::string("send refused: ") + e.what()); return r; }
    return send_prepared(mhz, f, parity, power_w, o, p, log, 1.5);
}
SendResult TxControl::send_prepared(double mhz, const FtPrepared& f, tx::Parity parity, double power_w, const Options& o,
                                     const ProgressFn& p, const LogFn& log, double min_lead_s) {
    std::lock_guard<std::mutex> l(op_); SendResult r; Busy b(*this);
    auto no = [&](const std::string& w) { r.why_not = w; say(log, "send refused: " + w); return r; };
    const std::string& text = f.text; const double audio_hz = f.audio_hz; const bool ft4 = f.ft4;
    if (!armed_) return no("transmit is not armed");
    if (f.buf.empty() || f.rate != o.rate) return no("the prepared signal does not match the stream rate");
    const int band = txm::band_of((mhz * 1e6) + audio_hz + 50);
    if (band < 0 || txm::band_of(mhz * 1e6) != band) return no("outside the amateur bands");
    if (!(power_w >= 1) || power_w > txs::kMaxWatts) return no("power must be 1..100 W");
    const tx::DigiMode mode = ft4 ? tx::DigiMode::FT4 : tx::DigiMode::FT8;
    const double off = tx::default_start_offset_s(mode), sig = tx::signal_seconds(mode), slot_len = tx::slot_seconds(mode);
    auto buf = f.buf;

    // drive: measured level, else the EEPROM table (errs low); the closed loop below trims it either way
    const double k = k_for(band), dcap = drive_cap(band);
    double drive = k > 1000 ? std::sqrt(power_w / k) : txs::drive_for_watts(cal_, band, power_w); r.estimated_drive = !(k > 1000);
    if (!(drive > 0)) return no("no usable drive for this power on this band");
    if (drive > dcap) {                                              // more than the band's drive ceiling allows: send at the ceiling, never above it
        const double was = power_w; drive = dcap; if (k > 1000) power_w = std::max(1.0, dcap * dcap * k);
        r.limited_to_w = power_w; char m[160]; std::snprintf(m, sizeof m, "%.0f W is above this band's limit: sending at %.1f W (the drive ceiling)", was, power_w); say(log, m);
    }
    const double drive_max = std::min(dcap, drive * 2.0);          // the loop may raise the drive by at most 6 dB
    tx::TxPlan plan;
    try { plan = tx::plan_tx(utc_(), mode, parity, off, 2.0, min_lead_s); } catch (const std::exception& e) { return no(e.what()); }
    r.slot_utc = plan.slot.start_utc; r.late_s = plan.skip_s;
    if (o.tx_ant < 1 || o.tx_ant > 3) return no("transmit antenna must be ANT1..ANT3");
    prepare(mhz, 1, o.tx_ant);                                    // USB, TX DDS on the dial
    drive /= std::sqrt(2.0);                                       // soft start: 3 dB low, the loop below brings it up to the set power within about 0.7 s
    txo::ToneSource src(o.rate, 8); src.set_cap(std::min(dcap, drive_max * 1.1)); src.set_target(drive); src.set_mirror(o.mirror_q);
    src.set_correction(cal_.image_gain[size_t(band)], cal_.image_phase[size_t(band)]); src.set_buffer(std::move(buf));
    txo::Stream st(pcm_(o.rate), src);
    const double latency_s = st.latency_s();                      // what the playback device holds before a sample is sent
    auto wait_until = [&](double utc) { while (!abort_ && utc_() < utc) clk_.sleep(std::min(0.05, std::max(0.001, utc - utc_()))); };
    wait_until(plan.skip_s > 0 ? plan.audio_start_utc - 0.15 : plan.slot.start_utc - 0.15);   // ~0.15 s before the slot (joining late: as planned)
    if (abort_) { r.stop = "aborted before keying"; return r; }
    if (st.failed()) return no("the I/Q stream failed to start");
    GateConfig gc; gc.max_key_s = slot_len; gc.max_fwd_w = fwd_limit_w(power_w); gc.max_rev_w = std::max(2.0, 0.5 * power_w); gc.max_swr = 3.0;
    gc.grace_s = kOverGraceS; gc.hard_fwd_w = hard_fwd_limit_w(power_w);              // a key-down overshoot is ridden out for 0.15 s; the hard limit never
    KeyHook kh(key_hook_);
    TxGate gate(fx_, gc, clk_); gate.arm(); std::string why;
    if (!gate.key(std::string(ft4 ? "FT4 " : "FT8 ") + text, gc.max_key_s, &why)) { r.unkey_failed = gate.unkey_failed(); note_stop(why); return no(why); }
    keyed_ = true;
    src.seek((long long)std::llround((utc_() + latency_s - plan.slot.start_utc) * o.rate)); src.set_keyed(true);
    const double sig_start = plan.slot.start_utc + off, sig_end = sig_start + sig, t0 = clk_.now();
    double next_trim = sig_start + 0.4, sum_fwd = 0; int n_fwd = 0;
    while (gate.keyed()) {
        if (abort_) { gate.unkey("aborted"); break; }
        if (st.failed()) { gate.unkey("the I/Q stream failed"); break; }
        const double now = utc_();
        if (src.done() || now > sig_end + 0.15) { gate.unkey("over finished"); r.sent = true; break; }
        txm::Reading rd;
        try { rd = txm::read(cal_.meter, band, int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcFwd)), int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcRev))); }
        catch (const std::exception& e) { gate.unkey(std::string("meter read failed: ") + e.what()); break; }
        r.max_fwd_w = std::max(r.max_fwd_w, rd.fwd_w); if (p) p({clk_.now() - t0, src.level(), rd});
        gate.report(rd); gate.tick(); if (!gate.keyed()) break;
        if (now > sig_start + 0.3 && now < sig_end - 0.2 && rd.fwd_w > 0.05) {   // a low floor: an EEPROM-estimated drive reads low at a few W; drive_max bounds the rise
            if (now >= next_trim && now < sig_start + 3.0) {        // closed loop: trim towards the set power in the first 3 s (constant envelope)
                drive = std::clamp(drive * std::clamp(std::sqrt(power_w / rd.fwd_w), 0.8, 1.25), 0.0, drive_max); src.set_target(drive); next_trim = now + 0.25;
            } else if (now >= sig_start + 3.3) { sum_fwd += rd.fwd_w; ++n_fwd; r.swr = rd.swr; }
        }
        clk_.sleep(0.05);
    }
    src.set_keyed(false); gate.ensure_unkeyed(2.0); keyed_ = false; r.unkeyed_utc = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    r.stop = gate.last_unkey_reason(); r.unkey_failed = gate.unkey_failed(); r.drive = drive; note_stop(r.stop);
    if (n_fwd > 3) {
        r.mean_fwd_w = sum_fwd / n_fwd; r.k = r.mean_fwd_w / (drive * drive);
        if (r.sent && (!r.swr || *r.swr < 2.5)) set_k(band, k > 1000 ? 0.7 * k + 0.3 * r.k : r.k);   // the band's drive level learns from every clean over
    }
    char m[200]; std::snprintf(m, sizeof m, "%s %s: %s, %.1f W (set %.1f W)%s", ft4 ? "FT4" : "FT8", text.c_str(), r.stop.c_str(), r.mean_fwd_w, power_w, r.late_s > 0 ? " (joined late)" : ""); say(log, m);
    return r;
}
}  // namespace fireside::txc

namespace fireside::txc {
VoiceResult TxControl::voice(double mhz, IAudioIn& in, const std::function<bool()>& ptt, const VoiceOptions& v, const ProgressFn& p, const LogFn& log) {
    std::lock_guard<std::mutex> l(op_); VoiceResult r; Busy b(*this);
    auto no = [&](const std::string& w) { r.why_not = w; say(log, "voice refused: " + w); return r; };
    if (!armed_) return no("transmit is not armed");
    const int band = txm::band_of(mhz * 1e6);
    if (band < 0 || txm::band_of(mhz * 1e6 + (v.lsb ? -3000.0 : 3000.0)) != band) return no("outside the amateur bands");
    if (!(v.peak_w >= 1 && v.peak_w <= txs::kMaxWatts)) return no("peak power must be 1..100 W");
    if (!(v.max_key_s > 0 && v.max_key_s <= 600)) return no("time-out must be 1..600 s");
    const double k = k_for(band); if (!(k > 1000)) return no("no measured drive level on this band: press Measure drive first");
    const double ceiling = v.chain.iq_limiter.ceiling, dcap = drive_cap(band);
    double peak_w = v.peak_w;
    double drive = std::sqrt(peak_w / k) / ceiling;                            // full-scale processed audio (= the ceiling) -> peak_w
    if (!(drive > 0)) return no("no usable drive for this power on this band");
    if (drive * ceiling > dcap) {                                              // above the band's drive ceiling: the ceiling, never above it
        drive = dcap / ceiling; peak_w = std::max(1.0, dcap * dcap * k); r.limited_to_w = peak_w;
        char m[160]; std::snprintf(m, sizeof m, "%.0f W PEP is above this band's limit: %.1f W PEP (the drive ceiling)", v.peak_w, peak_w); say(log, m);
    }
    tx::TxChainConfig cc = v.chain; cc.mic.sample_rate = 48000; cc.ssb.sideband = v.lsb ? tx::Sideband::LSB : tx::Sideband::USB;
    tx::TxChain chain(cc);
    if (v.o.tx_ant < 1 || v.o.tx_ant > 3) return no("transmit antenna must be ANT1..ANT3");
    prepare(mhz, v.lsb ? 0 : 1, v.o.tx_ant);
    txo::ToneSource src(v.o.rate, 8); src.set_cap(std::min(dcap, drive * ceiling * 1.05)); src.set_target(drive); src.set_mirror(v.o.mirror_q);
    src.set_correction(cal_.image_gain[size_t(band)], cal_.image_phase[size_t(band)]); src.set_live(size_t(v.o.rate * 0.06));
    // the producer: audio -> chain -> I/Q 48 kHz -> stream rate -> the live ring (keeps ~40 ms queued)
    std::atomic<bool> stop{false}; std::atomic<bool> failed{false}; std::string prod_err;
    std::thread prod([&] {
        try {
            std::unique_ptr<dsp::RationalResampler> rs; if (v.o.rate != 48000) rs = std::make_unique<dsp::RationalResampler>(48000.0, v.o.rate);
            std::vector<float> a(480); std::vector<tx::cf> iq; std::vector<dsp::cf> up; std::vector<std::complex<float>> out;
            while (!stop) {
                const size_t n = in.read(a.data(), a.size(), 0.05); if (n == 0) continue;
                iq.clear(); chain.process(a.data(), n, iq);
                const std::complex<float>* src_p; size_t m;
                if (rs) { up.clear(); rs->process(iq.data(), iq.size(), up); src_p = up.data(); m = up.size(); } else { out.assign(iq.begin(), iq.end()); src_p = out.data(); m = out.size(); }
                size_t done = 0; while (done < m && !stop) { const size_t w = src.push_live(src_p + done, m - done); done += w; if (done < m) std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            }
        } catch (const std::exception& e) { prod_err = e.what(); failed = true; }
    });
    struct Join { std::atomic<bool>& s; std::thread& t; ~Join() { s = true; if (t.joinable()) t.join(); } } join{stop, prod};
    txo::Stream st(pcm_(v.o.rate), src);
    for (int i = 0; i < 40 && src.live_queued() < size_t(v.o.rate * 0.03) && !failed; ++i) clk_.sleep(0.005);   // ~30 ms of audio queued first
    if (failed) return no("audio input: " + prod_err);
    if (st.failed()) return no("the I/Q stream failed to start");
    if (!ptt()) { r.stop = "PTT released before keying"; return r; }
    GateConfig gc; gc.max_key_s = v.max_key_s; gc.max_fwd_w = fwd_limit_w(peak_w); gc.max_rev_w = std::max(2.0, 0.5 * peak_w); gc.max_swr = 3.0;
    gc.grace_s = kOverGraceS; gc.hard_fwd_w = hard_fwd_limit_w(peak_w);               // as FT8: a key-down overshoot (a data program's tone starts hard) is ridden out, the hard limit never
    KeyHook kh(key_hook_);
    TxGate gate(fx_, gc, clk_); gate.arm(); std::string why;
    if (!gate.key(v.lsb ? "voice LSB" : "voice USB", gc.max_key_s, &why)) { r.unkey_failed = gate.unkey_failed(); note_stop(why); return no(why); }
    keyed_ = true; r.keyed = true; src.set_keyed(true); const double t0 = clk_.now(); double last_trim = t0;
    while (gate.keyed()) {
        if (abort_) { gate.unkey("aborted"); break; }
        if (!ptt()) { gate.unkey("PTT released"); break; }
        if (failed) { gate.unkey("audio input failed: " + prod_err); break; }
        if (st.failed()) { gate.unkey("the I/Q stream failed"); break; }
        txm::Reading rd;
        try { rd = txm::read(cal_.meter, band, int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcFwd)), int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcRev))); }
        catch (const std::exception& e) { gate.unkey(std::string("meter read failed: ") + e.what()); break; }
        r.max_fwd_w = std::max(r.max_fwd_w, rd.fwd_w); if (p) p({clk_.now() - t0, src.level(), rd});
        gate.report(rd); gate.tick(); if (!gate.keyed()) break;
        if (rd.fwd_w > 1.1 * peak_w && clk_.now() - last_trim >= 0.2) { drive *= 0.9; src.set_target(drive); last_trim = clk_.now(); }   // ALC: only ever down
        clk_.sleep(0.02);
    }
    src.set_keyed(false); gate.ensure_unkeyed(2.0); keyed_ = false;
    r.stop = gate.last_unkey_reason(); r.unkey_failed = gate.unkey_failed(); r.seconds = clk_.now() - t0; r.drive = drive; r.underruns = src.live_underruns(); note_stop(r.stop);
    char m[200]; std::snprintf(m, sizeof m, "voice %s: %s, %.1f s, max %.1f W (peak set %.0f W)", v.lsb ? "LSB" : "USB", r.stop.c_str(), r.seconds, r.max_fwd_w, peak_w); say(log, m);
    return r;
}
}  // namespace fireside::txc

namespace fireside::txc {
TxControl::GenCheck TxControl::sig_gen_check(double dial_mhz, double tone_hz, const std::function<std::pair<double, double>()>& read,
                                             const std::function<bool(bool)>& preamp, bool preamp_was_on, const LogFn& log) {
    std::lock_guard<std::mutex> l(op_); GenCheck r; Busy b(*this);
    auto say = [&](const std::string& m) { if (log) log(m); };
    if (!read) { r.why = "no receiver reading"; return r; }
    if (!(std::fabs(tone_hz) >= 200 && std::fabs(tone_hz) <= 3000)) { r.why = "the generator offset must be 200..3000 Hz from the dial"; return r; }
    auto wait = [&](double s) { const double t0 = clk_.now(); while (!abort_ && clk_.now() - t0 < s) clk_.sleep(0.05); };
    auto average = [&](double s, double* peak) {               // power average of the receiver's in-passband level, sampled every 0.1 s
        double sum = 0, pk = 0; int n = 0; const double t0 = clk_.now();
        while (!abort_ && clk_.now() - t0 < s) { const auto [d, p] = read(); if (d > -199) { sum += std::pow(10.0, d / 10.0); ++n; pk = p; } clk_.sleep(0.1); }
        if (peak) *peak = pk; return n ? 10.0 * std::log10(sum / n) : -200.0;
    };
    auto put_preamp_back = [&] { if (preamp) preamp(preamp_was_on); };
    if (preamp && preamp_was_on) preamp(false);                 // PowerSDR measures with the preamp off first
    wait(0.5); r.off_dbfs = average(1.5, nullptr);
    if (abort_) { put_preamp_back(); r.why = "stopped"; return r; }
    try { siggen::on(fx_, dial_mhz + tone_hz / 1e6); }
    catch (const std::exception& e) { r.why = std::string("could not switch the generator on: ") + e.what(); put_preamp_back(); return r; }
    say("signal generator on (receive only): receiver input = the radio's internal generator at " + std::to_string(dial_mhz + tone_hz / 1e6) + " MHz");
    wait(1.5); r.on_dbfs = average(2.0, &r.peak_hz);
    if (preamp && !abort_) {
        if (preamp(true)) { wait(0.5); r.preamp_dbfs = average(2.0, nullptr); r.preamp_measured = !abort_; }
        preamp(false);
    }
    r.restore_error = siggen::off(fx_);                         // always, whatever happened above
    put_preamp_back();
    say(r.restore_error.empty() ? "signal generator off: receiver input back to the antenna" : "SIGNAL GENERATOR OFF FAILED: " + r.restore_error);
    if (!r.restore_error.empty()) { r.why = "the generator could not be switched off (" + r.restore_error + "): power-cycle the radio"; return r; }
    if (abort_) { r.why = "stopped"; return r; }
    if (r.on_dbfs - r.off_dbfs < 30) { r.why = "the generator was not seen (only " + std::to_string(int(std::lround(r.on_dbfs - r.off_dbfs))) + " dB above the antenna's signal)"; return r; }
    if (std::fabs(r.peak_hz - tone_hz) > 150) { r.why = "the strongest signal was at " + std::to_string(int(std::lround(r.peak_hz))) + " Hz, not at the generator's " + std::to_string(int(tone_hz)) + " Hz"; return r; }
    if (r.on_dbfs > -3) { r.why = "the generator overloads the receiver"; return r; }
    r.ok = true; return r;
}
}  // namespace fireside::txc

namespace fireside::txc {
CwResult TxControl::cw(double mhz, ICwKeys& keys, const CwOptions& c, const ProgressFn& p, const LogFn& log) {
    std::lock_guard<std::mutex> l(op_); CwResult r; Busy b(*this);
    auto no = [&](const std::string& w) { r.why_not = w; say(log, "CW refused: " + w); return r; };
    auto steady = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };   // thread-safe (the generator uses it)
    const double t_call = steady();
    if (!armed_) return no("transmit is not armed");
    const int band = txm::band_of(mhz * 1e6);
    if (band < 0 || txm::band_of(mhz * 1e6 - 200) != band || txm::band_of(mhz * 1e6 + 200) != band) return no("outside the amateur bands");
    if (!(c.power_w >= 1 && c.power_w <= txs::kMaxWatts)) return no("power must be 1..100 W");
    if (!(c.hang_s >= 0.05 && c.hang_s <= 3)) return no("the break-in delay must be 0.05..3 s");
    if (!(c.max_over_s > 0 && c.max_over_s <= 1800) || !(c.max_keydown_s >= 1 && c.max_keydown_s <= 60)) return no("bad time limits");
    if (!(c.o.rate >= 8000 && c.o.rate <= 384000)) return no("bad stream rate");
    for (char ch : c.text) if (!(std::isalnum(static_cast<unsigned char>(ch)) || std::strchr(" .,?/=+-", ch))) return no(std::string("cannot send '") + ch + "' in Morse");
    const double k = k_for(band); if (!(k > 1000)) return no("no measured drive level on this band: press Measure drive first");
    double power_w = c.power_w, drive = std::sqrt(power_w / k); const double dcap = drive_cap(band);
    if (!(drive > 0)) return no("no usable drive for this power on this band");
    if (drive > dcap) {
        drive = dcap; power_w = std::max(1.0, dcap * dcap * k); r.limited_to_w = power_w;
        char m[160]; std::snprintf(m, sizeof m, "%.0f W is above this band's limit: CW at %.1f W (the drive ceiling)", c.power_w, power_w); say(log, m);
    }
    tx::KeyerConfig kc = c.keyer; kc.sample_rate = c.o.rate;
    std::unique_ptr<tx::IambicKeyer> keyer; std::unique_ptr<tx::CwShaper> shaper; std::vector<uint8_t> text_key;
    try {
        keyer = std::make_unique<tx::IambicKeyer>(kc);
        shaper = std::make_unique<tx::CwShaper>(tx::CwShapeConfig{c.o.rate, c.edge_ms, kCwOffsetHz, 1.0});
        if (!c.text.empty()) text_key = tx::text_keying(c.text, kc.wpm, c.o.rate, 0.0);
    } catch (const std::exception& e) { return no(e.what()); }
    if (!c.text.empty() && std::find(text_key.begin(), text_key.end(), uint8_t(1)) == text_key.end()) return no("nothing to send in that text");
    if (c.o.tx_ant < 1 || c.o.tx_ant > 3) return no("transmit antenna must be ANT1..ANT3");
    prepare(mhz - kCwOffsetHz / 1e6, 4, c.o.tx_ant);                                   // CWU; the TX DDS 1 kHz below the dial, the tone 1 kHz above it: the carrier on the dial
    txo::ToneSource src(c.o.rate, 8); src.set_cap(std::min(dcap, drive * 1.05)); src.set_target(drive); src.set_mirror(c.o.mirror_q);
    src.set_correction(cal_.image_gain[size_t(band)], cal_.image_phase[size_t(band)]); src.set_live(size_t(c.o.rate * 0.05));
    // The generator: ~1 ms blocks, keeping only kLead of them queued ahead of the stream (the shorter, the sooner a paddle press is heard on the air).
    const size_t blk = std::max<size_t>(8, size_t(std::lround(c.o.rate / 1000))), lead = blk * 6;
    std::atomic<bool> stop{false}, go{false}, done{false}, stuck{false}; std::atomic<bool> text_done{c.text.empty()}, text_stopped{false};
    std::atomic<double> first_key_t{-1};
    std::thread gen([&] {
        std::vector<uint8_t> key(blk); std::vector<tx::cf> iq; std::vector<std::complex<float>> out; size_t text_pos = 0;
        const size_t hang = size_t(c.hang_s * c.o.rate), max_down = size_t(c.max_keydown_s * c.o.rate);
        size_t since_down = 0, down_run = 0; bool any = false;
        while (!stop) {
            if (src.live_queued() >= lead) { std::this_thread::sleep_for(std::chrono::microseconds(250)); continue; }
            if (!go) { std::fill(key.begin(), key.end(), 0); }                          // MOX not on yet: silence (the paddle presses wait in `keys`)
            else {
                uint8_t pad = keys.paddles() & 3;
                bool paddle_mode = text_done.load();
                if (!paddle_mode && pad) { text_done = true; text_stopped = true; paddle_mode = true; }   // touching the paddle stops the text
                for (size_t i = 0; i < blk; ++i) {
                    bool kd;
                    if (!paddle_mode) { kd = text_pos < text_key.size() && text_key[text_pos]; if (++text_pos >= text_key.size()) { text_done = true; paddle_mode = true; } }
                    else if (c.straight) kd = pad != 0;
                    else kd = keyer->step(pad & 1, pad & 2);
                    key[i] = kd;
                    if (kd) { since_down = 0; ++down_run; if (!any) { any = true; first_key_t = steady(); } } else { ++since_down; down_run = 0; }
                    if (down_run > max_down) stuck = true;
                }
                if (stuck) std::fill(key.begin(), key.end(), 0);
                if (text_done && (c.straight || keyer->idle()) && since_down >= hang && !pad) done = true;   // the break-in delay has passed with the key up
            }
            iq.clear(); shaper->process(key.data(), key.size(), iq);
            out.assign(iq.begin(), iq.end());
            size_t w = 0; while (w < out.size() && !stop) { w += src.push_live(out.data() + w, out.size() - w); if (w < out.size()) std::this_thread::sleep_for(std::chrono::microseconds(250)); }
            if (c.sidetone && go) { try { c.sidetone(key.data(), key.size(), c.o.rate); } catch (...) {} }
            if (done || stuck) break;
        }
    });
    struct Join { std::atomic<bool>& s; std::thread& t; ~Join() { s = true; if (t.joinable()) t.join(); } } join{stop, gen};
    txo::Stream st(pcm_(c.o.rate), src);
    for (int i = 0; i < 40 && src.live_queued() < lead / 2; ++i) clk_.sleep(0.002);
    if (st.failed()) return no("the I/Q stream failed to start");
    GateConfig gc; gc.max_key_s = c.max_over_s; gc.max_fwd_w = fwd_limit_w(power_w); gc.max_rev_w = std::max(2.0, 0.5 * power_w); gc.max_swr = 3.0;
    gc.grace_s = kOverGraceS; gc.hard_fwd_w = hard_fwd_limit_w(power_w);
    KeyHook kh(key_hook_, kCwOffsetHz);                                                     // the DDS 1 kHz below the dial for the over
    TxGate gate(fx_, gc, clk_); gate.arm(); std::string why;
    if (!gate.key("CW", gc.max_key_s, &why)) { r.unkey_failed = gate.unkey_failed(); note_stop(why); return no(why); }
    keyed_ = true; r.keyed = true; src.set_keyed(true);
    clk_.sleep(0.008);                                                                       // the T/R relays settle before the first element (no hot switching)
    go = true; const double t0 = clk_.now(); double last_trim = t0;
    while (gate.keyed()) {
        if (abort_) { gate.unkey("aborted"); break; }
        if (stuck) { char m[120]; std::snprintf(m, sizeof m, "key held down for %.0f s (stuck key?): time limit", c.max_keydown_s); gate.unkey(m); break; }
        if (done && src.live_queued() == 0) { clk_.sleep(std::min(0.2, st.latency_s() + 0.01)); gate.unkey("CW over finished"); break; }   // the last samples played out first
        if (st.failed()) { gate.unkey("the I/Q stream failed"); break; }
        txm::Reading rd;
        try { rd = txm::read(cal_.meter, band, int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcFwd)), int(fx_.get("RDAL_OP_READ_PA_ADC", txm::kAdcRev))); }
        catch (const std::exception& e) { gate.unkey(std::string("meter read failed: ") + e.what()); break; }
        r.max_fwd_w = std::max(r.max_fwd_w, rd.fwd_w); if (p) p({clk_.now() - t0, src.level(), rd});
        gate.report(rd); gate.tick(); if (!gate.keyed()) break;
        if (rd.fwd_w > 1.1 * power_w && clk_.now() - last_trim >= 0.2) { drive *= 0.9; src.set_target(drive); last_trim = clk_.now(); }   // ALC: only ever down
        clk_.sleep(0.02);
    }
    src.set_keyed(false); gate.ensure_unkeyed(2.0); keyed_ = false;
    r.stop = gate.last_unkey_reason(); r.unkey_failed = gate.unkey_failed(); r.seconds = clk_.now() - t0; r.drive = drive; r.underruns = src.live_underruns();
    r.text_done = text_done && !text_stopped; r.text_stopped = text_stopped; r.key_delay_s = first_key_t >= 0 ? first_key_t - t_call : 0; r.queue_ms = 1000.0 * double(lead) / c.o.rate;
    note_stop(r.stop);
    char m[220]; std::snprintf(m, sizeof m, "CW: %s, %.1f s, max %.1f W (set %.0f W), first element %.0f ms after the key", r.stop.c_str(), r.seconds, r.max_fwd_w, power_w, 1000 * r.key_delay_s); say(log, m);
    return r;
}
}  // namespace fireside::txc
