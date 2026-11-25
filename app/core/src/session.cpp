#include "fireside/session.h"
#include "fireside/codec.h"
#include "fireside/tx_control.h"
#include <cstdio>
#include <sstream>

namespace fireside {
RadioSession::RadioSession(Opener open, std::function<void(double)> sleep) : open_(std::move(open)), sleep_(sleep ? std::move(sleep) : std::function<void(double)>([](double) {})) {}

IdentifyOptions RadioSession::ident_opts() const { IdentifyOptions io; io.boot_wait_s = opt_.boot_wait_s; io.sleep = sleep_; return io; }

void RadioSession::connect(const SessionOptions& opt) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Disconnected) throw std::logic_error("RadioSession::connect: already connected");
    if (opt.byte_order != "auto" && opt.byte_order != "big" && opt.byte_order != "little") throw std::invalid_argument("bad byte order");
    if (opt.path.empty()) throw std::invalid_argument("no device path");
    if (!open_) throw std::runtime_error("no bus available");
    auto handle = open_(opt.path);
    if (handle->is_local()) throw std::runtime_error(opt.path + " is the local controller; pick the radio's node (see `list`)");
    ByteOrder order = ByteOrder::Big;
    if (opt.byte_order == "big") order = ByteOrder::Big; else if (opt.byte_order == "little") order = ByteOrder::Little;
    else {
        const auto pr = probe_order(handle->bus());
        if (!pr.order) {
            std::ostringstream m; m << "could not determine the mailbox byte order:"; for (const auto& n : pr.notes) m << " " << n << ";"; m << "\nnothing was written";
            throw StartupError(m.str());
        }
        order = *pr.order;
    }
    auto mb = std::make_unique<PalMailbox>(handle->bus(), order);
    if (!mb->verify(opt.boot_wait_s))                                          // F4: proven over THIS mailbox, whatever the order came from
        throw StartupError("the mailbox did not return a plausible firmware revision (wrong byte order, radio still booting, or firmware outside the supported range); nothing was written");
    Safety safety; safety.allow_write = opt.allow_write;
    handle_ = std::move(handle); mb_ = std::move(mb); fx_ = std::make_unique<Flex>(*mb_, safety); order_ = order; opt_ = opt; state_ = State::Connected;
}

Identity RadioSession::identify() {
    std::lock_guard<std::mutex> l(m_);
    if (state_ == State::Disconnected) throw std::logic_error("RadioSession::identify: not connected");
    return fireside::identify(*fx_, ident_opts());
}

Identity RadioSession::start(double freq_mhz, const std::string& rx_ant, const LogFn& log) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ == State::Disconnected) throw std::logic_error("RadioSession::start: not connected");
    if (state_ == State::Started) throw std::logic_error("RadioSession::start: already started");
    if (!opt_.allow_write) throw SafetyError("start writes to the radio but this session was opened without allow_write");
    const double off = opt_.if_offset_hz;                                      // validate everything before any traffic
    if (!(off >= 0.0 && off < 20000.0)) throw std::invalid_argument("IF offset must be 0..20000 Hz");
    if (!Tuner::in_range(freq_mhz * 1e6, off)) throw std::invalid_argument("dial frequency out of range (DDS = dial - IF offset must be inside 0..65 MHz)");
    const double dds_mhz = freq_mhz - off / 1e6;
    startup_steps(dds_mhz, rx_ant);
    Identity idn = run_startup(*fx_, dds_mhz, rx_ant, log, ident_opts());
    if (log && off != 0.0) { char b[128]; std::snprintf(b, sizeof b, "dial %.6f MHz: DDS %.6f MHz, %g kHz below it (IF offset, as PowerSDR)", freq_mhz, dds_mhz, off / 1e3); log(b); }
    sleep_(kSettleBeforeAudioS);
    TuneConfig tc; tc.if_offset_hz = off;
    auto tuner = std::make_shared<Tuner>(*fx_, tc); tuner->arm(freq_mhz * 1e6);   // last sent = this dial (its DDS went out in the start-up)
    auto preamp = std::make_shared<RxPreamp>(*fx_); preamp->arm();            // start-up has just forced the preamp off
    auto rxant = std::make_shared<RxAntenna>(*fx_); rxant->arm(rx_ant);          // the start-up has just selected rx_ant
    auto lines = std::make_shared<TxLines>(*fx_); lines->configure(line_cfg_); lines->arm();   // nothing is written until a line is used (apply / an over)
    tuner_ = std::move(tuner); preamp_ = std::move(preamp); rxant_ = std::move(rxant); lines_ = std::move(lines); state_ = State::Started;
    return idn;
}

TuneResult RadioSession::tune(double dial_hz) {
    std::shared_ptr<Tuner> t; { std::lock_guard<std::mutex> l(m_); if (state_ != State::Started) return TuneResult::NotArmed; t = tuner_; }
    return t->request(dial_hz);                                                // outside the session lock so a slow write never blocks status queries
}
bool RadioSession::set_mic_gain_db(int db, std::string* err) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Started || !fx_ || !opt_.allow_write) { if (err) *err = "the radio is not started"; return false; }
    try { codec::set_mic_gain_db(*fx_, db); return true; } catch (const std::exception& e) { if (err) *err = e.what(); return false; }
}
std::optional<uint32_t> RadioSession::read_ptt() {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Started || !fx_) return std::nullopt;
    try { return uint32_t(fx_->get("RDAL_OP_READ_PTT", 0)) & 0xF; } catch (...) { return std::nullopt; }
}
std::optional<uint32_t> RadioSession::read_keys(bool event) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Started || !fx_ || !mb_) return std::nullopt;
    try { return (event ? mb_->read_event().raw : uint32_t(fx_->get("RDAL_OP_READ_PTT", 0))) & 0xF; } catch (...) { return std::nullopt; }
}
std::optional<uint32_t> RadioSession::read_pa_adc(int ch) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Started || !fx_ || ch < 0 || ch > 7) return std::nullopt;
    try { return uint32_t(fx_->get("RDAL_OP_READ_PA_ADC", uint32_t(ch))); } catch (...) { return std::nullopt; }
}
bool RadioSession::pump() {
    std::shared_ptr<Tuner> t; { std::lock_guard<std::mutex> l(m_); if (state_ != State::Started) return false; t = tuner_; }
    return t->pump();
}

PreampResult RadioSession::set_preamp(bool on) {
    std::shared_ptr<RxPreamp> p; { std::lock_guard<std::mutex> l(m_); if (state_ != State::Started) return PreampResult::NotArmed; p = preamp_; }
    return p->request(on);
}
bool RadioSession::preamp_on() const { std::lock_guard<std::mutex> l(m_); return preamp_ && preamp_->on(); }
AntResult RadioSession::set_rx_antenna(RxAnt a) {
    std::shared_ptr<RxAntenna> r; { std::lock_guard<std::mutex> l(m_); if (state_ != State::Started) return AntResult::NotArmed; r = rxant_; }
    return r->request(a);
}
std::optional<RxAnt> RadioSession::rx_antenna() const { std::lock_guard<std::mutex> l(m_); return rxant_ ? rxant_->current() : std::nullopt; }
void RadioSession::configure_tx_lines(const LineConfig& c) {
    std::shared_ptr<TxLines> t; { std::lock_guard<std::mutex> l(m_); line_cfg_ = c; t = lines_; }
    if (t) t->configure(c);
}
TxLines::Result RadioSession::apply_tx_lines(double dial_hz) {
    std::shared_ptr<TxLines> t; { std::lock_guard<std::mutex> l(m_); if (state_ != State::Started) return TxLines::Result::NotArmed; t = lines_; }
    return t ? t->apply(dial_hz) : TxLines::Result::NotArmed;
}
std::array<int, kTxLines> RadioSession::tx_lines_state() const { std::lock_guard<std::mutex> l(m_); return lines_ ? lines_->state() : std::array<int, kTxLines>{{-1, -1, -1}}; }

std::shared_ptr<txc::TxControl> RadioSession::transmit(txc::PcmFactory pcm) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Started) return nullptr;
    if (!tx_ || tx_->retired()) {
        tx_ = std::make_shared<txc::TxControl>(*fx_, std::move(pcm));
        std::weak_ptr<Tuner> wt = tuner_; const double rx_off = opt_.if_offset_hz;
        tx_->set_lines(lines_);
        std::weak_ptr<txc::TxControl> wtx = tx_; if (lines_) lines_->set_busy([wtx] { auto t = wtx.lock(); return t && t->busy(); });   // no line changes during an over
        tx_->set_key_hook([wt, rx_off](bool keyed, double below) { if (auto t = wt.lock()) t->set_if_offset(keyed ? below : rx_off); });   // the shared RX1/TX DDS: on the dial for the over (a CW over: 1 kHz below it), back to the IF shift after
    }
    return tx_;
}

void RadioSession::shutdown(const LogFn& log) {
    std::shared_ptr<txc::TxControl> tx; { std::lock_guard<std::mutex> l(m_); tx = tx_; }
    if (tx) tx->retire();                                                      // outside the session lock: a running tune unkeys and finishes first
    std::lock_guard<std::mutex> l(m_);
    if (state_ == State::Disconnected) throw std::logic_error("RadioSession::shutdown: not connected");
    if (!opt_.allow_write) throw SafetyError("shutdown writes to the radio but this session was opened without allow_write");
    if (tuner_) tuner_->disarm();                                              // no retunes while shutting down
    if (preamp_) { preamp_->request(false); preamp_->disarm(); }               // preamp back off (as at start-up); a fault here is ignored, the shutdown steps follow
    if (rxant_) rxant_->disarm();                                              // no antenna switching while shutting down
    if (lines_) lines_->release();                                             // every used rear TX line off (off = the external gear's safe position)
    run_steps(*fx_, shutdown_steps(), log);                                    // throws on failure: state kept so the caller can retry
    tx_.reset(); lines_.reset(); state_ = State::Connected;
}

void RadioSession::disconnect() {
    std::shared_ptr<txc::TxControl> tx; { std::lock_guard<std::mutex> l(m_); tx = tx_; }
    if (tx) tx->retire();                                                      // a running tune unkeys and finishes before the Flex goes away
    std::lock_guard<std::mutex> l(m_);
    if (tuner_) tuner_->disarm();                                              // a Tuner still held elsewhere can no longer write
    if (preamp_) preamp_->disarm();
    if (rxant_) rxant_->disarm();
    if (lines_) lines_->disarm();                                              // no writes here (the Flex is going away)
    tuner_.reset(); preamp_.reset(); rxant_.reset(); lines_.reset(); tx_.reset(); fx_.reset(); mb_.reset(); handle_.reset(); state_ = State::Disconnected;
}
}  // namespace fireside
