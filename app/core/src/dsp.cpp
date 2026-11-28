#include "fireside/dsp.h"
#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace fireside::dsp {
// ---------------------------------------------------------------- FFT
Fft::Fft(size_t n) : n_(n), tw_(n / 2), rev_(n) {
    if (n < 2 || (n & (n - 1))) throw std::invalid_argument("FFT size must be a power of two >= 2");
    for (size_t k = 0; k < n / 2; ++k) tw_[k] = cf(float(std::cos(2 * kPi * double(k) / double(n))), float(-std::sin(2 * kPi * double(k) / double(n))));
    size_t bits = 0; while ((size_t(1) << bits) < n) ++bits;
    for (size_t i = 0; i < n; ++i) { size_t r = 0; for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b); rev_[i] = uint32_t(r); }
}
void Fft::forward(cf* a) const {
    for (size_t i = 0; i < n_; ++i) if (rev_[i] > i) std::swap(a[i], a[rev_[i]]);
    for (size_t len = 2; len <= n_; len <<= 1) {
        const size_t half = len / 2, stride = n_ / len;
        for (size_t i = 0; i < n_; i += len)
            for (size_t k = 0; k < half; ++k) { cf t = a[i + k + half] * tw_[k * stride]; a[i + k + half] = a[i + k] - t; a[i + k] += t; }
    }
}
std::vector<float> blackman_harris(size_t n) {
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i) {
        const double x = 2 * kPi * double(i) / double(n);
        w[i] = float(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x));
    }
    return w;
}

// ---------------------------------------------------------------- IF shift
IfShifter::IfShifter(const ReceiveConfig& c) : step_(2 * kPi * c.if_offset_hz / c.sample_rate), mirror_(c.mirror) {
    if (!(c.sample_rate > 0)) throw std::invalid_argument("sample rate must be positive");
}
void IfShifter::process(cf* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const cf s = mirror_ ? std::conj(x[i]) : x[i];
        x[i] = s * cf(float(std::cos(phase_)), float(-std::sin(phase_)));
        phase_ += step_; if (phase_ > kPi) phase_ -= 2 * kPi;
    }
}

// ---------------------------------------------------------------- spectrum / waterfall
SpectrumAnalyzer::SpectrumAnalyzer(double fs, size_t n, float averaging)
    : fs_(fs), n_(n), alpha_(averaging), fft_(n), win_(blackman_harris(n)), win_sum_(0), ring_(n), work_(n), db_(n, -200.f), pw_(n, 0.f) {
    for (float w : win_) win_sum_ += w;
    if (!(averaging > 0.f && averaging <= 1.f)) throw std::invalid_argument("averaging must be in (0, 1]");
}
size_t SpectrumAnalyzer::bin_for_hz(double hz) const {
    const double k = std::round(hz * double(n_) / fs_ + double(n_) / 2.0);
    return size_t(std::min(std::max(k, 0.0), double(n_ - 1)));
}
size_t SpectrumAnalyzer::push(const cf* x, size_t n) {
    size_t produced = 0;
    while (n > 0) {
        const size_t take = std::min(n, n_ - fill_);
        std::copy(x, x + take, ring_.begin() + long(fill_)); fill_ += take; x += take; n -= take;
        if (fill_ == n_) { compute(); ++produced; std::copy(ring_.begin() + long(n_ / 2), ring_.end(), ring_.begin()); fill_ = n_ / 2; }   // 50 % overlap
    }
    return produced;
}
void SpectrumAnalyzer::compute() {
    for (size_t i = 0; i < n_; ++i) work_[i] = ring_[i] * win_[i];
    fft_.forward(work_.data());
    const float norm = 1.0f / (win_sum_ * win_sum_);
    for (size_t k = 0; k < n_; ++k) {
        const float p = std::norm(work_[(k + n_ / 2) % n_]) * norm;                   // fft-shift
        pw_[k] = frames_ == 0 ? p : (1.0f - alpha_) * pw_[k] + alpha_ * p;
        db_[k] = 10.0f * std::log10(pw_[k] + 1e-20f);
    }
    ++frames_;
    if (on_frame) on_frame(db_);
}
void Waterfall::push(const std::vector<float>& r) {
    if (r.size() != width_) throw std::invalid_argument("waterfall row width mismatch");
    rows_.push_front(r); while (rows_.size() > max_rows_) rows_.pop_back();
}
std::vector<uint8_t> Waterfall::to_gray(float lo, float hi) const {
    std::vector<uint8_t> out; out.reserve(rows_.size() * width_);
    for (const auto& r : rows_) for (float v : r) out.push_back(uint8_t(std::lround(255.0f * std::min(std::max((v - lo) / (hi - lo), 0.0f), 1.0f))));
    return out;
}

// ---------------------------------------------------------------- filters
std::vector<float> ComplexFir::lowpass(double cutoff, double transition, double fs) {
    if (!(cutoff > 0 && transition > 0 && cutoff < fs / 2)) throw std::invalid_argument("bad lowpass design");
    size_t m = size_t(std::ceil(5.5 * fs / transition)) | 1;
    const double fc = cutoff / fs, mid = double(m - 1) / 2.0;
    std::vector<float> h(m); double sum = 0;
    for (size_t i = 0; i < m; ++i) {
        const double t = double(i) - mid, x = 2 * kPi * double(i) / double(m - 1);
        const double sinc = t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x);
        h[i] = float(sinc * w); sum += h[i];
    }
    for (auto& v : h) v = float(v / sum);
    return h;
}
ComplexFir::ComplexFir(std::vector<cf> taps, int decim) : h_(std::move(taps)), decim_(decim), buf_(h_.size() - 1, cf(0, 0)) {
    if (h_.empty() || decim < 1) throw std::invalid_argument("bad FIR");
}
void ComplexFir::process(const cf* x, size_t n, std::vector<cf>& out) {
    buf_.insert(buf_.end(), x, x + n);
    const size_t m = h_.size();
    size_t start = skip_;
    while (start + m <= buf_.size()) {
        cf acc(0, 0);
        const cf* p = buf_.data() + start + m - 1;
        for (size_t k = 0; k < m; ++k) acc += h_[k] * p[-long(k)];
        out.push_back(acc); start += size_t(decim_);
    }
    const size_t drop = std::min(start, buf_.size() - (m - 1));                       // keep the last m-1 samples of history
    buf_.erase(buf_.begin(), buf_.begin() + long(drop)); skip_ = start - drop;
}

std::pair<double, double> default_passband(Mode m) {
    switch (m) {
        case Mode::USB: return {200, 2800};
        case Mode::LSB: return {-2800, -200};
        case Mode::AM: return {-4000, 4000};
        case Mode::CW: return {-250, 250};
        case Mode::FM: return {-7500, 7500};
    }
    return {0, 0};
}

std::pair<double, double> passband_for(Mode m, double w, double low) {
    if (!(w > 0)) throw std::invalid_argument("filter width must be positive");
    switch (m) {
        case Mode::USB: return {low, low + w};
        case Mode::LSB: return {-(low + w), -low};
        case Mode::AM: case Mode::FM: case Mode::CW: return {-w / 2, w / 2};
    }
    return {0, 0};
}
std::vector<double> filter_widths(Mode m) {
    switch (m) {
        case Mode::USB: case Mode::LSB: return {1800, 2400, 2700, 3000, 3600};
        case Mode::CW: return {250, 500, 1000};
        case Mode::AM: return {4000, 6000, 8000, 10000};
        case Mode::FM: return {8000, 12000, 15000};
    }
    return {};
}
double default_filter_width(Mode m) {
    switch (m) { case Mode::USB: case Mode::LSB: return 2700; case Mode::CW: return 500; case Mode::AM: return 8000; case Mode::FM: return 15000; }
    return 2700;
}

// ---------------------------------------------------------------- FM: CTCSS tones, tone squelch, sub-audio high-pass and de-emphasis
const std::vector<double>& ctcss_tones() {
    static const std::vector<double> t = {67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
                                          131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6,
                                          199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1};
    return t;
}
char ctcss_uk_letter(double hz) {
    static const std::pair<double, char> uk[] = {{67.0, 'A'}, {71.9, 'B'}, {77.0, 'C'}, {82.5, 'D'}, {88.5, 'E'}, {94.8, 'F'}, {103.5, 'G'}, {110.9, 'H'}, {118.8, 'J'}};
    for (const auto& [f, c] : uk) if (std::fabs(f - hz) < 0.05) return c;
    return 0;
}
Biquad Biquad::highpass(double fs, double f0, double q) {
    const double w = 2 * kPi * f0 / fs, al = std::sin(w) / (2 * q), cw = std::cos(w), a0 = 1 + al; Biquad b;
    b.b0 = (1 + cw) / 2 / a0; b.b1 = -(1 + cw) / a0; b.b2 = b.b0; b.a1 = -2 * cw / a0; b.a2 = (1 - al) / a0; return b;
}
Biquad Biquad::lowpass(double fs, double f0, double q) {
    const double w = 2 * kPi * f0 / fs, al = std::sin(w) / (2 * q), cw = std::cos(w), a0 = 1 + al; Biquad b;
    b.b0 = (1 - cw) / 2 / a0; b.b1 = (1 - cw) / a0; b.b2 = b.b0; b.a1 = -2 * cw / a0; b.a2 = (1 - al) / a0; return b;
}
namespace { constexpr double kCtcssWindowS = 0.4, kCtcssEvalS = 0.05, kCtcssHoldS = 0.25, kCtcssRatio = 15.85; constexpr int kCtcssHits = 3; }   // 12 dB over the median, three evaluations running
CtcssDetector::CtcssDetector(double audio_rate, double want_hz)
    : want_(want_hz), decim_(std::max(1, int(std::lround(audio_rate / 1000.0)))), lp1_(Biquad::lowpass(audio_rate, 320, 0.5412)), lp2_(Biquad::lowpass(audio_rate, 320, 1.3066)) {
    rate_ = audio_rate / decim_;
    const size_t n = size_t(std::lround(kCtcssWindowS * rate_)); ring_.assign(n, 0.0f); win_.resize(n);
    for (size_t i = 0; i < n; ++i) win_[i] = float(0.5 - 0.5 * std::cos(2 * kPi * (double(i) + 0.5) / double(n)));
}
bool CtcssDetector::process(float a) {
    acc_ += lp2_.run(lp1_.run(a));                          // low-pass (4th order, 320 Hz), then a box average over the decimation (nulls at 1 kHz multiples)
    if (++phase_ < decim_) return open_;
    phase_ = 0; ring_[pos_] = float(acc_ / decim_); acc_ = 0; pos_ = (pos_ + 1) % ring_.size(); if (filled_ < ring_.size()) ++filled_;
    t_s_ += 1.0 / rate_;
    if (++since_eval_ >= int(std::lround(kCtcssEvalS * rate_)) && filled_ == ring_.size()) { since_eval_ = 0; evaluate(); }
    return open_;
}
void CtcssDetector::evaluate() {
    const auto& tones = ctcss_tones(); const size_t n = ring_.size(); std::vector<double> pw(tones.size());
    double total = 0; for (size_t i = 0; i < n; ++i) total += double(ring_[i]) * ring_[i];
    for (size_t k = 0; k < tones.size(); ++k) {            // Goertzel on the windowed block, oldest sample first
        const double c = 2 * std::cos(2 * kPi * tones[k] / rate_); double s1 = 0, s2 = 0;
        for (size_t i = 0; i < n; ++i) { const double s0 = double(ring_[(pos_ + i) % n]) * win_[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        pw[k] = s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    size_t best = 0; for (size_t k = 1; k < pw.size(); ++k) if (pw[k] > pw[best]) best = k;
    std::vector<double> sorted = pw; std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
    const double median = sorted[sorted.size() / 2];
    const bool hit = pw[best] > kCtcssRatio * median && pw[best] > 1e-6 * double(n) * double(n) && pw[best] > 1e-4 * total * double(n);   // a real tone, not silence; and a fair share of the block's energy
    const double f = hit ? tones[best] : 0.0;
    if (hit && f == cand_) ++hits_; else { hits_ = hit ? 1 : 0; cand_ = f; }
    if (hits_ >= kCtcssHits) { heard_hz_ = f; last_hit_s_ = t_s_; }
    else if (t_s_ - last_hit_s_ > kCtcssHoldS) heard_hz_ = 0;
    open_ = heard_hz_ > 0 && (want_ <= 0 || std::fabs(heard_hz_ - want_) < 0.05);
}
FmAudio::FmAudio(double fs, bool hpf, bool de)
    : hpf_(hpf), de_(de), h1_(Biquad::highpass(fs, 300, 0.5176)), h2_(Biquad::highpass(fs, 300, 0.7071)), h3_(Biquad::highpass(fs, 300, 1.9319)) {
    const double K = 2 * fs, wz = K * std::tan(kPi * 3000 / fs), wp = K * std::tan(kPi * 300 / fs), d = 1 + K / wp;   // H(s) = (1 + s/wz) / (1 + s/wp), bilinear
    de_b0_ = (1 + K / wz) / d; de_b1_ = (1 - K / wz) / d; de_a1_ = (1 - K / wp) / d;
    const std::complex<double> z = std::polar(1.0, -2 * kPi * 1000 / fs), h = (de_b0_ + de_b1_ * z) / (1.0 + de_a1_ * z);
    de_b0_ /= std::abs(h); de_b1_ /= std::abs(h);                                 // unity at 1 kHz
}
float FmAudio::process(float x) {
    if (hpf_) x = h3_.run(h2_.run(h1_.run(x)));
    if (de_) { const double y = de_b0_ * x + de_b1_ * de_x1_ - de_a1_ * de_y1_; de_x1_ = x; de_y1_ = y; x = float(y); }
    return x;
}

namespace { std::mutex g_stage_m; AudioStageFactory g_spectral_nr; }
void set_spectral_nr_factory(AudioStageFactory f) { std::lock_guard<std::mutex> l(g_stage_m); g_spectral_nr = std::move(f); }
Demodulator::Demodulator(const DemodConfig& c)
    : c_(c), decim_(int(std::lround(c.input_rate / c.audio_rate))),
      pre_([&] {
          const double ratio = c.input_rate / c.audio_rate;
          if (ratio < 1 || std::fabs(ratio - std::round(ratio)) > 1e-6) throw std::invalid_argument("input rate must be an integer multiple of the audio rate");
          if (std::lround(ratio) == 1) return ComplexFir({cf(1, 0)}, 1);
          auto lp = ComplexFir::lowpass(0.4 * c.audio_rate, 0.2 * c.audio_rate, c.input_rate);
          return ComplexFir(std::vector<cf>(lp.begin(), lp.end()), int(std::lround(ratio)));
      }()),
      band_([&] {
          double lo = c.lo_hz, hi = c.hi_hz;
          if (lo == 0 && hi == 0) std::tie(lo, hi) = default_passband(c.mode);
          if (!(hi > lo)) throw std::invalid_argument("passband must have hi > lo");
          const double bw = hi - lo, fc = (hi + lo) / 2;
          auto lp = ComplexFir::lowpass(bw / 2, std::max(c.edge_floor_hz, c.edge_fraction * bw), c.audio_rate);
          std::vector<cf> h(lp.size()); const double mid = double(lp.size() - 1) / 2.0;
          for (size_t i = 0; i < lp.size(); ++i) { const double a = 2 * kPi * fc * (double(i) - mid) / c.audio_rate; h[i] = lp[i] * cf(float(std::cos(a)), float(std::sin(a))); }
          return ComplexFir(std::move(h), 1);
      }()),
      agc_(c.audio_rate, c.agc, c.agc_target, c.agc_max_gain_db), nb_(c.nb_threshold) {
    if (c.notch_hz != 0) notch_ = std::make_unique<Notch>(c.audio_rate, c.notch_hz, c.notch_q);
    for (double f : c.extra_notches_hz) if (f > 0 && f < c.audio_rate / 2) extra_notches_.push_back(std::make_unique<Notch>(c.audio_rate, f, c.notch_q));
    if (c.auto_notch) anf_ = std::make_unique<AdaptiveLineFilter>(AdaptiveLineFilter::Output::Residual);
    if (c.noise_reduction) nr_ = std::make_unique<AdaptiveLineFilter>(AdaptiveLineFilter::Output::Prediction, 128, 16, 0.005f);   // measured: +6 dB SNR on a tone in noise, adapts in well under a second
    if (c.spectral_nr && c.mode != Mode::CW) { AudioStageFactory f; { std::lock_guard<std::mutex> l(g_stage_m); f = g_spectral_nr; } if (f) nr2_ = f(c.audio_rate); }   // voice modes only: it would smear a CW tone
    if (c.mode == Mode::AM && c.am_detector == AmDetector::Sync) sam_reset();
    if (c.mode == Mode::FM) { ctcss_ = std::make_unique<CtcssDetector>(c.audio_rate, c.ctcss_hz); fm_audio_ = std::make_unique<FmAudio>(c.audio_rate, c.fm_subaudio_hpf, c.fm_deemphasis); }
}
void Demodulator::sam_reset() {
    const double wn = 2 * kPi * std::max(5.0, c_.sam_loop_hz) / c_.audio_rate, zeta = 0.707;   // second-order loop: proportional + integral path
    sam_kp_ = 2 * zeta * wn; sam_ki_ = wn * wn; sam_ca_ = 1.0 - std::exp(-2 * kPi * 200.0 / c_.audio_rate);   // carrier estimate: one-pole low-pass, 200 Hz
    sam_phase_ = sam_freq_ = 0; sam_c_ = cf(0, 0); sam_err_avg_ = 1; sam_cmag_avg_ = 0; sam_locked_ = false;
    const int taps = 129, mid = taps / 2; sam_hilbert_.assign(taps, 0.0f);                  // windowed Hilbert transformer: 2 / (pi n) on odd taps
    for (int n = -mid; n <= mid; ++n) if (n % 2 != 0) { const double w = 0.42 + 0.5 * std::cos(kPi * n / mid) + 0.08 * std::cos(2 * kPi * n / mid); sam_hilbert_[size_t(n + mid)] = float(2.0 / (kPi * n) * w); }
    sam_q_.assign(taps, 0.0f); sam_i_.assign(taps, 0.0f); sam_pos_ = 0;
}
float Demodulator::sam_sample(cf s) {
    const cf z = s * cf(float(std::cos(sam_phase_)), float(-std::sin(sam_phase_)));          // the signal with the loop's carrier removed: carrier at 0 Hz, in phase
    sam_c_ += float(sam_ca_) * (z - sam_c_);                                                 // carrier estimate (the audio modulation averages out)
    const double cmag = std::abs(sam_c_); sam_cmag_avg_ += 0.0002 * (cmag - sam_cmag_avg_);
    const bool carrier = cmag > 0.15 * sam_cmag_avg_ && cmag > 1e-9;                         // in a deep carrier fade the loop coasts on its frequency instead of chasing noise
    double err = 0;
    if (carrier) {
        err = std::atan2(double(sam_c_.imag()), double(sam_c_.real()));
        sam_freq_ += sam_ki_ * err;
        const double lim = 2 * kPi * 300.0 / c_.audio_rate; sam_freq_ = std::clamp(sam_freq_, -lim, lim);
    }
    sam_phase_ += sam_freq_ + sam_kp_ * err; if (sam_phase_ > kPi) sam_phase_ -= 2 * kPi; else if (sam_phase_ < -kPi) sam_phase_ += 2 * kPi;
    sam_err_avg_ += 0.001 * (std::fabs(err) - sam_err_avg_); sam_locked_ = carrier && sam_err_avg_ < 0.3;
    float a = z.real();                                                                      // both sidebands: the in-phase part
    if (c_.am_sideband != AmSideband::Both) {                                                // one sideband: I -+ H{Q} (I delayed by the Hilbert filter's group delay)
        const size_t taps = sam_hilbert_.size(), mid = taps / 2;
        sam_q_[sam_pos_] = z.imag(); sam_i_[sam_pos_] = z.real();
        double hq = 0; for (size_t k = 0; k < taps; ++k) hq += sam_hilbert_[k] * sam_q_[(sam_pos_ + taps - k) % taps];
        const float id = sam_i_[(sam_pos_ + taps - mid) % taps]; sam_pos_ = (sam_pos_ + 1) % taps;
        a = c_.am_sideband == AmSideband::Upper ? float(id - hq) : float(id + hq);
    }
    am_dc_ += 0.0005 * (a - am_dc_); return float(a - am_dc_);                                // remove the carrier (DC), as the envelope detector does
}
double Demodulator::signal_dbfs() const { return metered_ ? 10.0 * std::log10(power_ + 1e-20) : -200.0; }
void Demodulator::process(const cf* x, size_t n, std::vector<float>& audio) {
    tmp0_.assign(x, x + n); tmp1_.clear(); tmp2_.clear();
    if (c_.noise_blanker) nb_.process(tmp0_.data(), tmp0_.size());
    pre_.process(tmp0_.data(), tmp0_.size(), tmp1_);
    band_.process(tmp1_.data(), tmp1_.size(), tmp2_);
    const bool manual = c_.agc == AgcMode::Off && c_.manual_gain_db != 0.0; const float mg = manual ? float(std::pow(10.0, c_.manual_gain_db / 20.0)) : 1.0f;
    const double step = 2 * kPi * c_.cw_pitch_hz / c_.audio_rate, meter_a = 1.0 - std::exp(-1.0 / (0.1 * c_.audio_rate));
    const double fm_scale = c_.audio_rate / (2 * kPi * c_.fm_deviation_hz), sq_a = 1.0 - std::exp(-1.0 / (0.01 * c_.audio_rate));
    for (cf& s : tmp2_) {
        const double p = double(std::norm(s));                      // power in the passband, measured before the AGC touches it
        power_ = metered_ ? power_ + meter_a * (p - power_) : p; metered_ = true;
        agc_.process(&s, 1);
        float a;
        switch (c_.mode) {
            case Mode::USB: case Mode::LSB: a = s.real(); break;
            case Mode::AM: { if (c_.am_detector == AmDetector::Sync) { a = sam_sample(s); break; } const double m = std::abs(s); am_dc_ += 0.0005 * (m - am_dc_); a = float(m - am_dc_); break; }
            case Mode::CW: { a = float(s.real() * std::cos(cw_phase_) - s.imag() * std::sin(cw_phase_)); cw_phase_ += step; if (cw_phase_ > kPi) cw_phase_ -= 2 * kPi; break; }
            case Mode::FM: { a = float(std::arg(s * std::conj(prev_)) * fm_scale); prev_ = s; if (ctcss_) ctcss_->process(a); if (fm_audio_) a = fm_audio_->process(a); break; }
            default: a = 0;
        }
        if (notch_) a = notch_->process(a);
        for (auto& n : extra_notches_) a = n->process(a);
        if (anf_) a = anf_->process(a);
        if (nr_) a = nr_->process(a);
        if (nr2_) a = nr2_->process(a);
        const bool level_sq = c_.squelch_dbfs > -199.0, tone_sq = ctcss_ && c_.ctcss_hz > 0;
        if (level_sq) {                                                      // gate on the pre-AGC level, with hysteresis
            sq_power_ += sq_a * (p - sq_power_);                             // fast detector (10 ms): the 100 ms S-meter smoothing would hold the gate open for ~0.5 s
            const double db = 10.0 * std::log10(sq_power_ + 1e-20);
            if (squelch_open_ && db < c_.squelch_dbfs - 2.0) squelch_open_ = false; else if (!squelch_open_ && db >= c_.squelch_dbfs) squelch_open_ = true;
        }
        if (level_sq || tone_sq) {                                           // both must be open (tone squelch: the wanted CTCSS tone is heard); click-free ramp
            const bool open = (!level_sq || squelch_open_) && (!tone_sq || ctcss_->open());
            squelch_gain_ += 0.01f * ((open ? 1.0f : 0.0f) - squelch_gain_); a *= squelch_gain_;
        }
        audio.push_back(manual ? std::tanh(a * mg * c_.gain) : a * c_.gain);              // manual gain (AGC off) is soft-limited: tanh is linear for small signals, never exceeds 1
    }
}

ReceiveChain::ReceiveChain(const ReceiveConfig& rc, const DemodConfig& dc, size_t fft_size)
    : rc_(rc), shift_(rc), spec_(rc.sample_rate, fft_size), demod_([&] { DemodConfig d = dc; d.input_rate = rc.sample_rate; return d; }()) {}
void ReceiveChain::process(const cf* iq, size_t n, std::vector<float>& audio) {
    buf_.assign(iq, iq + n);
    shift_.process(buf_.data(), n);
    spec_.push(buf_.data(), n);
    demod_.process(buf_.data(), n, audio);
}

// ---------------------------------------------------------------- AGC, noise blanker, S-meter
AdaptiveLineFilter::AdaptiveLineFilter(Output out, size_t taps, size_t delay, float mu)
    : out_(out), n_(taps), d_(delay), mu_(mu), w_(taps, 0.0f), hist_(taps + delay, 0.0f) {
    if (taps == 0 || !(mu > 0.0f && mu < 2.0f)) throw std::invalid_argument("bad adaptive filter parameters");
}
float AdaptiveLineFilter::process(float x) {
    // reference vector = x[n-d-k], k = 0..n-1 (the delay decorrelates broadband noise between the reference and the sample being predicted)
    const size_t len = hist_.size(); float y = 0.0f;
    for (size_t k = 0; k < n_; ++k) y += w_[k] * hist_[(pos_ + len - (d_ + k) % len) % len];
    const float e = x - y;
    const float g = mu_ * e / float(power_ + 1e-6);
    for (size_t k = 0; k < n_; ++k) w_[k] += g * hist_[(pos_ + len - (d_ + k) % len) % len];
    // advance the ring: x becomes the newest sample; keep the energy of the reference window incrementally
    pos_ = (pos_ + 1) % len; hist_[pos_] = x;
    double p = 0; for (size_t k = 0; k < n_; ++k) { const float r = hist_[(pos_ + len - (d_ + k) % len) % len]; p += double(r) * double(r); }
    power_ = p;
    return out_ == Output::Prediction ? y : e;
}
Notch::Notch(double fs, double hz, double q) {
    if (!(fs > 0) || !(hz > 0 && hz < fs / 2) || !(q > 0.1)) throw std::invalid_argument("bad notch parameters");
    const double w0 = 2 * kPi * hz / fs, alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
    b0_ = 1 / a0; b1_ = -2 * std::cos(w0) / a0; b2_ = 1 / a0; a1_ = -2 * std::cos(w0) / a0; a2_ = (1 - alpha) / a0;
}
float Notch::process(float x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_; x1_ = x; y2_ = y1_; y1_ = y; return float(y);
}
AgcParams agc_preset(AgcMode m) {
    switch (m) { case AgcMode::Fast: return {50, 0}; case AgcMode::Med: return {250, 50}; case AgcMode::Slow: return {1000, 500}; default: return {250, 50}; }
}
Agc::Agc(double fs, AgcMode mode, float target, double max_gain_db)
    : fs_(fs), mode_(mode), target_(target), floor_(target / float(std::pow(10.0, max_gain_db / 20.0))), level_(floor_), max_gain_db_(max_gain_db) {
    if (!(target > 0) || !(max_gain_db >= 0)) throw std::invalid_argument("bad AGC parameters");
    set_mode(mode);
}
void Agc::set_mode(AgcMode m) {
    mode_ = m; const AgcParams p = agc_preset(m);
    decay_ = float(1.0 - std::exp(-1.0 / (p.decay_ms * 1e-3 * fs_)));
    hang_samples_ = size_t(p.hang_ms * 1e-3 * fs_); hang_ = 0;
}
void Agc::process(cf* x, size_t n) {
    if (mode_ == AgcMode::Off) { gain_ = 1.0f; return; }
    for (size_t i = 0; i < n; ++i) {
        const float env = std::abs(x[i]);
        if (env > level_) level_ = env;
        if (env > 0.9f * level_) hang_ = hang_samples_;                  // signal still at (or near) the held peak: keep re-arming the hang
        else if (hang_ > 0) --hang_;
        else level_ += decay_ * (env - level_);
        if (level_ < floor_) level_ = floor_;
        gain_ = target_ / level_;
        x[i] *= gain_;
    }
}
void NoiseBlanker::process(cf* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const float m = std::abs(x[i]);
        if (!primed_) { avg_ = m; primed_ = true; }
        if (m > thr_ * avg_ && avg_ > 0) { left_ = hold_ + 1; }
        if (left_ > 0) { x[i] = cf(0, 0); --left_; ++blanked_total_; }
        else avg_ += (m - avg_) * (1.0f / 512.0f);                       // learn the level only from un-blanked samples
    }
}
SReading s_reading(double dbm) {
    const double s = 9.0 + (dbm + 73.0) / 6.0;
    if (s >= 9.0) return {9, int(std::lround(dbm + 73.0))};
    return {int(std::max(0.0, std::floor(s + 1e-9))), 0};
}
int cat_smeter_value(double dbm) {
    if (dbm <= -73.0) return int(std::lround(std::max(0.0, (9.0 + (dbm + 73.0) / 6.0)) * 15.0 / 9.0));
    return int(std::min(30L, 15L + std::lround((dbm + 73.0) / 4.0)));
}

bool DeadStreamWatchdog::feed(const cf* x, size_t n) {
    bool all_zero = n > 0;
    for (size_t i = 0; i < n && all_zero; ++i) if (x[i].real() != 0.0f || x[i].imag() != 0.0f) all_zero = false;
    zeros_ = all_zero ? zeros_ + 1 : 0;
    return zeros_ > limit_;
}

// ---------------------------------------------------------------- synthetic and recorded I/Q
std::vector<cf> synth_tones(double fs, size_t n, const std::vector<Tone>& tones, double noise_rms, uint32_t seed, double start) {
    std::vector<cf> out(n);
    uint64_t s = 0x9E3779B97F4A7C15ull ^ seed;
    auto rnd = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double(s >> 11) + 0.5) / 9007199254740992.0; };
    for (size_t i = 0; i < n; ++i) {
        std::complex<double> v(0, 0);
        for (const auto& t : tones) { const double a = 2 * kPi * t.hz * (start + double(i)) / fs; v += t.amplitude * std::complex<double>(std::cos(a), std::sin(a)); }
        if (noise_rms > 0) {
            const double r = std::sqrt(-2 * std::log(rnd())) * noise_rms / std::sqrt(2.0), th = 2 * kPi * rnd();
            v += std::complex<double>(r * std::cos(th), r * std::sin(th));
        }
        out[i] = cf(float(v.real()), float(v.imag()));
    }
    return out;
}

static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

WavStream::WavStream(const std::string& path, int ci, int cq) : f_(path, std::ios::binary), path_(path), ci_(ci), cq_(cq) {
    if (!f_) throw std::runtime_error("cannot open " + path);
    f_.seekg(0, std::ios::end); const uint64_t file_size = uint64_t(f_.tellg()); f_.seekg(0);
    uint8_t hdr[12];
    if (!f_.read(reinterpret_cast<char*>(hdr), 12) || (std::memcmp(hdr, "RIFF", 4) && std::memcmp(hdr, "RF64", 4)) || std::memcmp(hdr + 8, "WAVE", 4)) throw std::runtime_error(path + ": not a RIFF/WAVE file");
    uint64_t data_len = 0, rf64_data = 0; bool have_fmt = false, have_data = false; unsigned tag = 0; uint16_t ch = 0, bits = 0; uint32_t rate = 0;
    while (!have_data) {
        uint8_t ch_hdr[8]; if (!f_.read(reinterpret_cast<char*>(ch_hdr), 8)) break;
        const uint32_t len = rd32(ch_hdr + 4); const uint64_t body = uint64_t(f_.tellg());
        if (!std::memcmp(ch_hdr, "ds64", 4) && len >= 16) { uint8_t d[16]; f_.read(reinterpret_cast<char*>(d), 16); rf64_data = rd64(d + 8); }
        else if (!std::memcmp(ch_hdr, "fmt ", 4) && len >= 16) {
            std::vector<uint8_t> b(std::min<uint32_t>(len, 64)); f_.read(reinterpret_cast<char*>(b.data()), long(b.size()));
            tag = rd16(b.data()); ch = rd16(&b[2]); rate = rd32(&b[4]); bits = rd16(&b[14]); if (tag == 0xFFFE && b.size() >= 26) tag = rd16(&b[24]); have_fmt = true;
        } else if (!std::memcmp(ch_hdr, "data", 4)) {
            have_data = true; data_off_ = body; data_len = len;
            if (len == 0xFFFFFFFFu) data_len = rf64_data ? rf64_data : file_size - body;        // RF64, or a streaming writer that never patched the size
            if (body + data_len > file_size) data_len = file_size - body;                       // truncated file: use what is there
            break;
        }
        f_.clear(); f_.seekg(std::streamoff(body + len + (len & 1)));
    }
    if (!have_fmt || !have_data || !ch) throw std::runtime_error(path + ": missing fmt or data chunk");
    if (ci < 0 || cq < 0 || ci >= ch || cq >= ch) throw std::runtime_error(path + ": has " + std::to_string(ch) + " channels, cannot pick " + std::to_string(ci) + "/" + std::to_string(cq));
    if (!((tag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32))) throw std::runtime_error(path + ": unsupported sample format");
    rate_ = rate; channels_ = ch; tag_ = tag; bits_ = bits; frames_ = data_len / (uint64_t(ch) * (bits / 8)); f_.clear(); f_.seekg(std::streamoff(data_off_));
}
void WavStream::rewind() { f_.clear(); f_.seekg(std::streamoff(data_off_)); pos_ = 0; }
size_t WavStream::read(std::vector<cf>& out, size_t n) {
    const size_t n_frames = size_t(std::min<uint64_t>(n, frames_ - pos_)); if (!n_frames) return 0;
    const size_t bps = bits_ / 8, frame = bps * size_t(channels_); raw_.resize(n_frames * frame);
    f_.read(reinterpret_cast<char*>(raw_.data()), long(raw_.size())); const size_t got = size_t(f_.gcount()) / frame;
    auto sample = [&](const uint8_t* p) -> float {
        if (tag_ == 3) { float v; std::memcpy(&v, p, 4); return v; }
        if (bits_ == 16) return float(int16_t(rd16(p))) / 32768.0f;
        if (bits_ == 24) return float(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24)) / 2147483648.0f;
        return float(int32_t(rd32(p))) / 2147483648.0f;
    };
    for (size_t i = 0; i < got; ++i) out.emplace_back(sample(&raw_[i * frame + size_t(ci_) * bps]), sample(&raw_[i * frame + size_t(cq_) * bps]));
    pos_ += got; if (got < n_frames) pos_ = frames_;                                             // short read: treat as the end
    return got;
}
IqRecording read_iq_wav(const std::string& path, int ch_i, int ch_q) {
    WavStream w(path, ch_i, ch_q); IqRecording r; r.sample_rate = w.sample_rate(); r.channels = w.channels(); r.samples.reserve(size_t(w.frames()));
    while (w.read(r.samples, 1 << 16) > 0) {}
    return r;
}
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
static void write_wav(const std::string& path, uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> h; h.insert(h.end(), {'R', 'I', 'F', 'F'}); put32(h, uint32_t(36 + payload.size())); h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(h, 16); put16(h, tag); put16(h, ch); put32(h, rate); put32(h, rate * ch * bits / 8); put16(h, uint16_t(ch * bits / 8)); put16(h, bits);
    h.insert(h.end(), {'d', 'a', 't', 'a'}); put32(h, uint32_t(payload.size()));
    std::ofstream o(path, std::ios::binary); if (!o) throw std::runtime_error("cannot write " + path);
    o.write(reinterpret_cast<const char*>(h.data()), long(h.size())); o.write(reinterpret_cast<const char*>(payload.data()), long(payload.size()));
}
void write_wav_f32(const std::string& path, double fs, int channels, const std::vector<float>& d) {
    std::vector<uint8_t> p(d.size() * 4); std::memcpy(p.data(), d.data(), p.size());
    write_wav(path, 3, uint16_t(channels), uint32_t(fs), 32, p);
}
void write_wav_pcm16_mono(const std::string& path, double fs, const std::vector<float>& s) {
    std::vector<uint8_t> p; for (float v : s) put16(p, uint16_t(int16_t(std::lround(std::min(std::max(v, -1.0f), 1.0f) * 32767.0f))));
    write_wav(path, 1, 1, uint32_t(fs), 16, p);
}

// ---------------------------------------------------------------- resampling
bool needs_resampling(double r) { const double k = r / 48000.0; return !(r >= 48000.0 && r <= 192000.0 && std::fabs(k - std::round(k)) < 1e-9); }
double choose_output_rate(double r) { if (!needs_resampling(r)) return r; return r >= 192000.0 ? 192000.0 : r >= 96000.0 ? 96000.0 : 48000.0; }

RationalResampler::RationalResampler(double in_rate, double out_rate) {
    auto whole = [](double r) { return r >= 1.0 && r <= 1e9 && std::fabs(r - std::round(r)) < 1e-6; };
    if (!whole(in_rate) || !whole(out_rate)) throw std::invalid_argument("resampler rates must be whole numbers of Hz");
    const uint64_t a = uint64_t(std::llround(in_rate)), b = uint64_t(std::llround(out_rate)), g = std::gcd(a, b);
    const uint64_t L = b / g, M = a / g;
    if (L > 2000 || M > 4000000) throw std::invalid_argument("resampling ratio " + std::to_string(a) + " -> " + std::to_string(b) + " is too awkward (reduces to " + std::to_string(L) + "/" + std::to_string(M) + ")");
    L_ = unsigned(L); M_ = unsigned(M);
    const double lower = std::min(in_rate, out_rate), fd = in_rate * double(L_);               // the prototype filter runs at L x the input rate
    const double fc = 0.46 * lower, tw = 0.12 * lower;                                         // cutoff 0.92 x the lower Nyquist, transition 12 % of the lower rate
    K_ = unsigned(std::ceil(5.5 * fd / tw / double(L_))) | 1u;                                 // taps per phase (odd); total taps = K_ * L_
    const size_t T = size_t(K_) * L_; const double mid = double(T - 1) / 2.0; std::vector<double> h(T); double sum = 0;
    for (size_t n = 0; n < T; ++n) {
        const double t = double(n) - mid, x = 2 * kPi * double(n) / double(T - 1), w = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x);
        const double arg = 2 * fc / fd * t, sinc = arg == 0 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
        h[n] = 2 * fc / fd * sinc * w; sum += h[n];
    }
    for (auto& v : h) v *= double(L_) / sum;                                                   // unity DC gain after interpolation by L
    sub_.assign(L_, std::vector<cf>(K_));
    for (unsigned r = 0; r < L_; ++r) for (unsigned j = 0; j < K_; ++j) sub_[r][K_ - 1 - j] = cf(float(h[size_t(r) + size_t(j) * L_]), 0.0f);    // reversed: taps in time order
    base_ = -(long long)(K_ - 1); buf_.assign(K_ - 1, cf(0, 0));
}
void RationalResampler::process(const cf* x, size_t n, std::vector<cf>& out) {
    buf_.insert(buf_.end(), x, x + n);
    for (;;) {
        const long long i0 = pos_ / L_;                                                        // newest input sample this output needs
        if (i0 >= base_ + (long long)buf_.size()) break;
        const auto& h = sub_[size_t(pos_ % L_)]; const cf* p = buf_.data() + (i0 - (long long)(K_ - 1) - base_);
        float re = 0, im = 0; for (unsigned t = 0; t < K_; ++t) { re += h[t].real() * p[t].real(); im += h[t].real() * p[t].imag(); }
        out.emplace_back(re, im); pos_ += M_;
    }
    const long long keep_from = pos_ / L_ - (long long)(K_ - 1);                               // oldest sample the next output needs
    if (keep_from > base_) { buf_.erase(buf_.begin(), buf_.begin() + long(keep_from - base_)); base_ = keep_from; }
}
}  // namespace fireside::dsp
