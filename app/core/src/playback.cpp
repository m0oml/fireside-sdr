#include "fireside/playback.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fireside {
RadioAudioOut::RadioAudioOut(std::unique_ptr<IPcmOut> pcm, int pair, double input_rate) : pcm_(std::move(pcm)), pair_(pair) {
    if (!pcm_) throw std::invalid_argument("no playback stream");
    if (pair < 0 || 2 * pair + 1 >= pcm_->channels()) throw std::invalid_argument("channel pair " + std::to_string(pair) + " does not exist on a " + std::to_string(pcm_->channels()) + "-channel stream");
    if (std::fabs(pcm_->rate() - input_rate) > 0.5) rs_ = std::make_unique<dsp::RationalResampler>(input_rate, pcm_->rate());      // the stream may run at 96 or 192 kHz
}
void RadioAudioOut::flush() {
    const size_t ch = size_t(pcm_->channels());
    while (pending_pos_ < pending_frames_) {
        const size_t n = pcm_->write(pending_.data() + pending_pos_ * ch, pending_frames_ - pending_pos_);
        if (n == 0) break;
        pending_pos_ += n; written_ += n;
    }
    if (pending_pos_ == pending_frames_) { pending_pos_ = pending_frames_ = 0; }
}
void RadioAudioOut::push(const float* audio, size_t n) {
    if (n == 0) return;
    const size_t ch = size_t(pcm_->channels());
    const float* src = audio; size_t m = n;
    if (rs_) { in_.resize(n); for (size_t i = 0; i < n; ++i) in_[i] = dsp::cf(audio[i], 0.0f); out_.clear(); rs_->process(in_.data(), n, out_); m = out_.size(); }
    const size_t limit = size_t(pcm_->rate() * 0.5);                                              // never hold more than half a second: drop instead of adding delay
    if (pending_frames_ - pending_pos_ + m > limit) { dropped_ += m; flush(); return; }
    if (pending_pos_ > 0 && pending_pos_ == pending_frames_) pending_pos_ = pending_frames_ = 0;
    pending_.resize((pending_frames_ + m) * ch, 0);
    for (size_t i = 0; i < m; ++i) {
        const float v = std::clamp((rs_ ? out_[i].real() : src[i]) * volume_, -1.0f, 1.0f); const int32_t s = int32_t(std::lround(double(v) * 2147483647.0));
        int32_t* f = pending_.data() + (pending_frames_ + i) * ch; for (size_t c = 0; c < ch; ++c) f[c] = 0;
        f[size_t(2 * pair_)] = s; f[size_t(2 * pair_ + 1)] = s;                                    // mono audio on both channels of the pair; every other channel stays silent
    }
    pending_frames_ += m; flush();
}
}  // namespace fireside
