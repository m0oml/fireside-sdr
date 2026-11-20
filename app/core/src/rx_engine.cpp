#include "fireside/rx_engine.h"
#include <algorithm>
#include <chrono>
#include "fireside/capture.h"

namespace fireside {
using Clock = std::chrono::steady_clock;

void RxEngine::set_source(std::unique_ptr<IqSource> src) {
    stop();
    std::lock_guard<std::mutex> l(m_);
    src_ = std::move(src); consumed_ = 0;
    std::lock_guard<std::mutex> dl(d_);
    frames_.clear(); audio_.clear(); const bool keep_fft = st_.fft_size != 0; const size_t fft = st_.fft_size; st_ = RxStatus{}; if (keep_fft) st_.fft_size = fft;
    if (src_) { st_.sample_rate = src_->sample_rate(); st_.live = src_->live(); st_.source = src_->describe(); }
}
void RxEngine::configure(RxOptions opt) {
    std::lock_guard<std::mutex> l(m_);
    if (src_) opt.rc.sample_rate = src_->sample_rate();
    opt.dc.input_rate = opt.rc.sample_rate;
    auto chain = std::make_unique<dsp::ReceiveChain>(opt.rc, opt.dc, opt.fft);          // may throw: nothing changed yet
    chain->spectrum().on_frame = [this](const std::vector<float>& db) { on_frame(db); };
    chain_ = std::move(chain); opt_ = opt; have_opt_ = true;
    std::lock_guard<std::mutex> dl(d_); st_.fft_size = opt.fft; st_.error.clear();
}
void RxEngine::set_taps(IqTap iq, AudioTap audio) { std::lock_guard<std::mutex> l(m_); iq_tap_ = std::move(iq); audio_tap_ = std::move(audio); }
void RxEngine::start() {
    { std::lock_guard<std::mutex> l(m_); if (!src_ || !chain_) return; }
    { std::lock_guard<std::mutex> dl(d_); if (st_.ended) return; }
    origin_ = Clock::now(); consumed_ = 0;
    if (!have_opt_ || !opt_.threaded) { running_ = true; std::lock_guard<std::mutex> dl(d_); st_.running = true; return; }
    if (th_.joinable() && running_) return;
    if (th_.joinable()) { quit_ = true; th_.join(); }
    quit_ = false; running_ = true; { std::lock_guard<std::mutex> dl(d_); st_.running = true; }
    th_ = std::thread([this] { loop(); });
}
void RxEngine::pause() { stop(); }
void RxEngine::stop() {
    quit_ = true; if (th_.joinable()) th_.join();
    running_ = false; std::lock_guard<std::mutex> dl(d_); st_.running = false;
}

void RxEngine::on_frame(const std::vector<float>& db) {                                // called from inside chain->process, i.e. with m_ held
    const size_t pk = size_t(std::max_element(db.begin(), db.end()) - db.begin());
    std::lock_guard<std::mutex> dl(d_);
    st_.peak_db = double(db[pk]); st_.peak_hz = chain_->spectrum().bin_hz(pk); ++st_.frames;
    if (frames_.size() >= opt_.max_frames) { frames_.pop_front(); ++st_.frames_dropped; }
    frames_.push_back(db);
}
size_t RxEngine::process_locked(size_t n) {
    if (!src_ || !chain_ || n == 0) return 0;
    std::vector<dsp::cf> buf; std::vector<float> audio;
    const size_t got = src_->read(buf, n);
    if (got == 0) {
        if (!src_->live()) { std::lock_guard<std::mutex> dl(d_); st_.ended = true; st_.running = false; running_ = false; }
        return 0;
    }
    chain_->process(buf.data(), got, audio);
    if (src_->live()) { if (iq_tap_) iq_tap_(buf.data(), got, src_->sample_rate()); if (audio_tap_ && !audio.empty()) audio_tap_(audio.data(), audio.size(), opt_.dc.audio_rate); }
    std::string decoded, dstatus; if (dec_) { dec_->process(audio.data(), audio.size(), decoded); dstatus = dec_->status(); }
    std::lock_guard<std::mutex> dl(d_);
    if (dec_) { text_ += decoded; if (text_.size() > 200000) text_.erase(0, text_.size() - 100000); st_.decoder = dstatus; }
    st_.samples += got; st_.audio_samples += audio.size();
    st_.signal_dbfs = chain_->signal_dbfs(); st_.agc_gain_db = chain_->demod().agc_gain_db(); st_.am_locked = chain_->demod().am_locked(); st_.am_carrier_hz = chain_->demod().am_carrier_hz();
    for (float a : audio) { if (audio_.size() >= opt_.max_audio) { audio_.pop_front(); ++st_.audio_dropped; } audio_.push_back(a); }
    if (auto* live = dynamic_cast<PcmIqSource*>(src_.get())) { st_.xruns = live->stats().xruns; st_.stream_dead = live->stream_dead(); }
    return got;
}
size_t RxEngine::advance(size_t n) {
    std::lock_guard<std::mutex> l(m_);
    try { return process_locked(n); }
    catch (const std::exception& e) { std::lock_guard<std::mutex> dl(d_); st_.error = e.what(); throw; }
}
void RxEngine::loop() {
    while (!quit_) {
        size_t did = 0;
        try {
            std::unique_lock<std::mutex> l(m_);
            if (!src_ || !chain_) break;
            const double fs = src_->sample_rate();
            if (src_->live()) did = process_locked(size_t(fs * 0.05));
            else {
                const double due = std::chrono::duration<double>(Clock::now() - origin_).count() * fs - consumed_;
                const double capped = std::min(due, 0.2 * fs);                           // after a stall skip ahead rather than burst
                if (due > capped) consumed_ += due - capped;
                if (capped >= 64) { consumed_ += capped; did = process_locked(size_t(capped)); }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> dl(d_); st_.error = e.what(); st_.running = false; running_ = false; return;
        }
        if (!running_) return;                                                          // source ended
        if (did == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2)); else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<std::vector<float>> RxEngine::take_frames() {
    std::lock_guard<std::mutex> dl(d_);
    std::vector<std::vector<float>> out(std::make_move_iterator(frames_.begin()), std::make_move_iterator(frames_.end())); frames_.clear(); return out;
}
size_t RxEngine::take_audio(std::vector<float>& out) {
    std::lock_guard<std::mutex> dl(d_);
    const size_t n = audio_.size(); out.insert(out.end(), audio_.begin(), audio_.end()); audio_.clear(); return n;
}
void RxEngine::set_decoder(std::unique_ptr<decode::TextDecoder> d) {
    std::lock_guard<std::mutex> l(m_); dec_ = std::move(d);
    std::lock_guard<std::mutex> dl(d_); text_.clear(); st_.decoder.clear();
}
void RxEngine::finish_decoder() {
    std::string t; { std::lock_guard<std::mutex> l(m_); if (dec_) dec_->finish(t); }
    if (!t.empty()) { std::lock_guard<std::mutex> dl(d_); text_ += t; }
}
void RxEngine::peek_decoder() {
    std::string t; { std::lock_guard<std::mutex> l(m_); if (dec_) dec_->peek(t); }
    if (!t.empty()) { std::lock_guard<std::mutex> dl(d_); text_ += t; }
}
size_t RxEngine::take_text(std::string& out) { std::lock_guard<std::mutex> dl(d_); const size_t n = text_.size(); out += text_; text_.clear(); return n; }
void RxEngine::with_source(const std::function<void(IqSource*)>& fn) { std::lock_guard<std::mutex> l(m_); fn(src_.get()); }
RxStatus RxEngine::status() const { std::lock_guard<std::mutex> dl(d_); return st_; }
double RxEngine::bin_hz(size_t k) const {
    std::lock_guard<std::mutex> dl(d_);
    return st_.fft_size && st_.sample_rate > 0 ? (double(k) - double(st_.fft_size) / 2.0) * st_.sample_rate / double(st_.fft_size) : 0.0;
}
}  // namespace fireside
