#include "fireside/iqsource.h"
#include <algorithm>
#include <deque>
#include <cmath>

namespace fireside {
using dsp::cf;
FileIqSource::FileIqSource(const std::string& path, int ci, int cq, bool loop) : path_(path), wav_(path, ci, cq), loop_(loop) {
    if (wav_.frames() == 0) throw std::runtime_error(path + ": no samples");
}
size_t FileIqSource::read(std::vector<cf>& out, size_t n) {
    size_t got = 0;
    while (got < n) {
        const size_t r = wav_.read(out, n - got);
        if (r == 0) { if (!loop_) break; wav_.rewind(); if (wav_.read(out, 0) == 0 && wav_.frames() == 0) break; continue; }
        got += r;
    }
    return got;
}
std::string FileIqSource::describe() const {
    const auto slash = path_.find_last_of('/');
    return (slash == std::string::npos ? path_ : path_.substr(slash + 1)) + " (" + std::to_string(int(wav_.sample_rate())) + " Hz, " + std::to_string(wav_.channels()) + " ch, " +
           std::to_string(size_t(double(wav_.frames()) / wav_.sample_rate())) + " s)";
}

ResamplingIqSource::ResamplingIqSource(std::unique_ptr<IqSource> inner, double out_rate) : inner_(std::move(inner)), out_rate_(out_rate), rs_(inner_->sample_rate(), out_rate) {}
size_t ResamplingIqSource::read(std::vector<cf>& out, size_t n) {
    while (pending_.size() < n && !eof_) {
        in_.clear(); const size_t want = std::max<size_t>(4096, size_t(double(n - pending_.size()) * inner_->sample_rate() / out_rate_) + 64);
        if (inner_->read(in_, want) == 0) { if (!inner_->live()) eof_ = true; break; }
        tmp_.clear(); rs_.process(in_.data(), in_.size(), tmp_); pending_.insert(pending_.end(), tmp_.begin(), tmp_.end());
    }
    const size_t take = std::min(n, pending_.size());
    out.insert(out.end(), pending_.begin(), pending_.begin() + long(take)); pending_.erase(pending_.begin(), pending_.begin() + long(take));
    return take;
}
std::string ResamplingIqSource::describe() const { return inner_->describe() + "  [resampled to " + std::to_string(int(out_rate_)) + " Hz]"; }

std::unique_ptr<IqSource> open_iq_file(const std::string& path, int ci, int cq, bool loop) {
    auto file = std::make_unique<FileIqSource>(path, ci, cq, loop); const double r = file->sample_rate();
    if (!dsp::needs_resampling(r)) return file;
    const double out = dsp::choose_output_rate(r);
    return std::make_unique<ResamplingIqSource>(std::move(file), out);
}

size_t DemoIqSource::read(std::vector<cf>& out, size_t n) {
    // fixed tones + AM carrier, phase-continuous across calls via the absolute sample index
    auto fixed = dsp::synth_tones(fs_, n, {{9000 + 1000, 0.3}, {9000 - 5000, 0.2}, {9000 - 5000 + 1000, 0.05}, {9000 - 5000 - 1000, 0.05}}, 0.001, uint32_t(1 + pos_ / (n ? n : 1)), double(pos_));
    const double period = 12.0 * fs_;
    for (size_t i = 0; i < n; ++i) {
        const double t = std::fmod(double(pos_ + i), period) / period;                    // 0..1
        const double f = 9000 + 20000.0 * (t < 0.5 ? t * 2 - 0.5 : 1.5 - t * 2) - 0.0;    // triangle sweep, 9 kHz +- 10 kHz
        sweep_phase_ += 2 * dsp::kPi * (f) / fs_; if (sweep_phase_ > dsp::kPi) sweep_phase_ -= 2 * dsp::kPi;
        fixed[i] += 0.05f * cf(float(std::cos(sweep_phase_)), float(std::sin(sweep_phase_)));
    }
    out.insert(out.end(), fixed.begin(), fixed.end()); pos_ += n;
    return n;
}
}  // namespace fireside
