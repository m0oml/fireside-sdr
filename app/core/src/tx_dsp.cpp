#include "fireside/tx_dsp.h"
#include "fireside/decode.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fireside::tx {
using dsp::kPi;

dsp::ComplexFir SsbModulator::make(const SsbConfig& c) {
    if (!(c.sample_rate >= 8000 && c.sample_rate <= 384000)) throw std::invalid_argument("SSB sample rate must be 8..384 kHz");
    if (!(c.edge_hz >= 20 && c.edge_hz <= 1000)) throw std::invalid_argument("SSB edge width must be 20..1000 Hz");   // also bounds the filter length
    if (!(c.lo_hz >= c.edge_hz)) throw std::invalid_argument("SSB low edge must be at least the edge width above 0 Hz (keeps the carrier in the stopband)");
    if (!(c.hi_hz > c.lo_hz) || !(c.hi_hz < c.sample_rate / 2)) throw std::invalid_argument("bad SSB passband");
    double lo = c.lo_hz, hi = c.hi_hz;
    if (c.sideband == Sideband::LSB) { const double l = lo; lo = -hi; hi = -l; }
    const double bw = hi - lo, fc = (hi + lo) / 2;
    auto lp = dsp::ComplexFir::lowpass(bw / 2, c.edge_hz, c.sample_rate);
    std::vector<cf> h(lp.size()); const double mid = double(lp.size() - 1) / 2.0;
    for (size_t i = 0; i < lp.size(); ++i) {
        const double a = 2 * kPi * fc * (double(i) - mid) / c.sample_rate;
        h[i] = 2.0f * lp[i] * cf(float(std::cos(a)), float(std::sin(a)));
    }
    return dsp::ComplexFir(std::move(h), 1);
}
SsbModulator::SsbModulator(const SsbConfig& c) : c_(c), fir_(make(c)) {}
void SsbModulator::process(const float* audio, size_t n, std::vector<cf>& iq) {
    tmp_.resize(n);
    for (size_t i = 0; i < n; ++i) tmp_[i] = cf(audio[i], 0.0f);
    fir_.process(tmp_.data(), n, iq);
}

std::vector<cf> ssb_modulate(const std::vector<float>& audio, const SsbConfig& c) {
    SsbModulator m(c);
    std::vector<float> in = audio; in.resize(audio.size() + m.latency_samples(), 0.0f);   // flush the tail
    std::vector<cf> out; out.reserve(in.size());
    m.process(in.data(), in.size(), out);
    out.erase(out.begin(), out.begin() + long(m.latency_samples()));
    return out;
}

// ---- Biquad ----
Biquad::Biquad(Type t, double fs, double hz, double q, double gain_db) : fs_(fs) {
    if (!(fs > 0 && hz > 0 && hz < fs / 2 && q > 0) || !std::isfinite(gain_db)) throw std::invalid_argument("bad biquad design");
    const double w = 2 * kPi * hz / fs, cw = std::cos(w), sw = std::sin(w), alpha = sw / (2 * q), A = std::pow(10.0, gain_db / 40.0);
    double b0, b1, b2, a0, a1, a2;
    switch (t) {
        case Type::HighPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::LowPass: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::Peak: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
        case Type::LowShelf: { const double r = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + r); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - r);
            a0 = (A + 1) + (A - 1) * cw + r; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - r; break; }
        case Type::HighShelf: { const double r = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + r); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - r);
            a0 = (A + 1) - (A - 1) * cw + r; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - r; break; }
        default: throw std::invalid_argument("bad biquad type");
    }
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
}
float Biquad::process(float x) {
    const double y = b0_ * x + s1_;
    s1_ = b1_ * x - a1_ * y + s2_; s2_ = b2_ * x - a2_ * y;
    if (!std::isfinite(y) || !std::isfinite(s1_) || !std::isfinite(s2_)) { s1_ = s2_ = 0; return 0.0f; }   // never let one bad sample poison the state for good
    return float(y);
}
double Biquad::response_db(double hz) const {
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * hz / fs_), z2 = z1 * z1;
    return 20 * std::log10(std::abs((b0_ + b1_ * z1 + b2_ * z2) / (1.0 + a1_ * z1 + a2_ * z2)));
}

namespace {
double coef(double ms, double fs) { return ms > 0 ? 1.0 - std::exp(-1.0 / (ms * 1e-3 * fs)) : 1.0; }   // one-pole smoothing per sample
double db2lin(double d) { return std::pow(10.0, d / 20.0); }
float finite_or_zero(float x) { return std::isfinite(x) ? x : 0.0f; }
}

// ---- Gate ----
NoiseGate::NoiseGate(double fs, const GateConfig& c)
    : c_(c), env_rel_(coef(20, fs)), att_(coef(c.attack_ms, fs)), rel_(coef(c.release_ms, fs)), floor_(float(db2lin(std::min(0.0, c.range_db)))),
      gain_(floor_), hold_n_(size_t(std::max(0.0, c.hold_ms) * 1e-3 * fs)) {}
float NoiseGate::process(float x) {
    x = finite_or_zero(x);
    const double a = std::fabs(double(x));
    env_ = a > env_ ? a : env_ + env_rel_ * (a - env_);              // instant peak, 20 ms decay
    const double lvl = 20 * std::log10(env_ + 1e-12);
    if (lvl >= c_.threshold_dbfs) { open_ = true; hold_ = hold_n_; }
    else if (open_ && lvl < c_.threshold_dbfs - c_.hysteresis_db) { if (hold_ > 0) --hold_; else open_ = false; }
    const float target = open_ ? 1.0f : floor_;
    gain_ += float((target > gain_ ? att_ : rel_) * (target - gain_));
    return x * gain_;
}

// ---- Compressor ----
Compressor::Compressor(double fs, const CompressorConfig& c) : c_(c), att_(coef(c.attack_ms, fs)), rel_(coef(c.release_ms, fs)), makeup_(db2lin(c.makeup_db)) {
    if (!(c.ratio >= 1) || !(c.makeup_db >= -40 && c.makeup_db <= 40) || !std::isfinite(c.threshold_dbfs)) throw std::invalid_argument("bad compressor settings");
}
float Compressor::process(float x) {
    x = finite_or_zero(x);
    const double a = std::fabs(double(x));
    env_ += (a > env_ ? att_ : rel_) * (a - env_);
    const double lvl = 20 * std::log10(env_ + 1e-12);
    gr_db_ = lvl > c_.threshold_dbfs ? (c_.threshold_dbfs - lvl) * (1.0 - 1.0 / c_.ratio) : 0.0;   // <= 0
    return float(x * db2lin(gr_db_) * makeup_);
}

// ---- Limiter ----
PeakLimiter::PeakLimiter(double fs, const LimiterConfig& c) : ceil_(float(c.ceiling)), la_(size_t(std::lround(std::max(0.0, c.lookahead_ms) * 1e-3 * fs))), rel_(coef(c.release_ms, fs)) {
    if (!(c.ceiling > 0 && c.ceiling <= 1.0)) throw std::invalid_argument("limiter ceiling must be in (0, 1]");
    if (!(c.lookahead_ms >= 0 && c.lookahead_ms <= 50) || !(c.release_ms >= 0 && c.release_ms <= 10000) || !(fs >= 8000 && fs <= 384000)) throw std::invalid_argument("bad limiter timing or sample rate");
    if (double(ceil_) > c.ceiling) ceil_ = std::nextafter(ceil_, 0.0f);   // largest float not above the configured ceiling (float(0.8) is 0.800000012)
    if (la_ < 1) la_ = 1;
    delay_.assign(la_, cf(0, 0)); box_.assign(la_, 1.0); bsum_ = double(la_);
}
cf PeakLimiter::step(cf x) {
    if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) x = cf(0, 0);
    const double m = std::abs(std::complex<double>(x));
    const double need = m > ceil_ ? double(ceil_) / m : 1.0;          // gain this sample needs
    while (!minq_.empty() && minq_.back().second >= need) minq_.pop_back();
    minq_.emplace_back(n_, need);
    while (minq_.front().first + la_ < n_) minq_.pop_front();         // window [n - la, n]
    const double wmin = minq_.front().second;
    held_ = std::min(wmin, held_ + rel_ * (wmin - held_ > 0 ? (wmin - held_) : 0.0));   // drop at once, recover exponentially; always <= wmin
    bsum_ += held_ - box_[bpos_]; box_[bpos_] = held_; bpos_ = (bpos_ + 1) % la_;
    if (bpos_ == 0) { bsum_ = 0; for (double v : box_) bsum_ += v; }  // re-sum once per cycle so rounding cannot accumulate
    gain_out_ = std::min(1.0, bsum_ / double(la_));                    // average of la values, each <= the need of the sample leaving the delay
    const cf out = delay_[dpos_]; delay_[dpos_] = x; dpos_ = (dpos_ + 1) % la_; ++n_;
    cf y = cf(float(out.real() * gain_out_), float(out.imag() * gain_out_));
    const double ym = std::abs(std::complex<double>(y));
    if (ym > ceil_) { const float k = float(double(ceil_) / ym * (1.0 - 1e-6)); y *= k; }   // backstop against rounding
    return y;
}
void PeakLimiter::process(const cf* x, size_t n, std::vector<cf>& out) { for (size_t i = 0; i < n; ++i) out.push_back(step(x[i])); }
void PeakLimiter::process(const float* x, size_t n, std::vector<float>& out) { for (size_t i = 0; i < n; ++i) out.push_back(step(cf(x[i], 0)).real()); }

// ---- MicChain ----
std::vector<EqBand> graphic_eq10(const double g[10]) {
    std::vector<EqBand> v;
    for (int i = 0; i < 10; ++i) {
        if (!(std::fabs(g[i]) <= 15)) throw std::invalid_argument("graphic EQ gains must be within +-15 dB");
        if (g[i] != 0) v.push_back({Biquad::Type::Peak, kGraphicEqHz[i], 1.41, g[i]});   // Q 1.41: one octave between the -3 dB points
    }
    return v;
}
DownwardExpander::DownwardExpander(double fs, const ExpanderConfig& c)
    : c_(c), env_rel_(coef(30, fs)), att_(coef(c.attack_ms, fs)), rel_(coef(c.release_ms, fs)), floor_(db2lin(std::min(0.0, c.range_db))), hold_n_(size_t(std::max(0.0, c.hold_ms) * 1e-3 * fs)) {
    if (!(c.ratio >= 1 && c.ratio <= 20) || c.range_db > 0 || !std::isfinite(c.threshold_dbfs)) throw std::invalid_argument("bad expander settings");
}
float DownwardExpander::process(float x) {
    x = finite_or_zero(x);
    const double a = std::fabs(double(x));
    env_ = a > env_ ? a : env_ + env_rel_ * (a - env_);             // instant peak, 30 ms decay
    const double lvl = 20 * std::log10(env_ + 1e-12);
    double target = 1;
    if (lvl >= c_.threshold_dbfs) hold_ = hold_n_;                   // speech: unity (and hold it a little after the last syllable)
    else if (hold_ > 0) --hold_;
    else target = std::max(floor_, db2lin((lvl - c_.threshold_dbfs) * (c_.ratio - 1)));
    gain_ += (target > gain_ ? att_ : rel_) * (target - gain_);
    return float(x * gain_);
}
Leveler::Leveler(double fs, const LevelerConfig& c) : c_(c), env_att_(coef(5, fs)), env_rel_(coef(300, fs)), g_att_(coef(c.attack_ms, fs)), g_rel_(coef(c.release_ms, fs)) {
    if (!(c.max_gain_db >= 0 && c.max_gain_db <= 30) || !(c.target_dbfs < 0 && c.target_dbfs > -40)) throw std::invalid_argument("bad leveler settings");
}
float Leveler::process(float x) {
    x = finite_or_zero(x);
    const double a = std::fabs(double(x));
    env_ += (a > env_ ? env_att_ : env_rel_) * (a - env_);           // speech envelope: fast up, slow down
    const double lvl = 20 * std::log10(env_ + 1e-12);
    if (lvl > -60) {                                                   // silence: hold the gain (no pumping up of the noise)
        const double want = std::clamp(c_.target_dbfs - lvl, -30.0, c_.max_gain_db);
        gain_db_ += (want < gain_db_ ? g_att_ : g_rel_) * (want - gain_db_);
    }
    return float(x * db2lin(gain_db_));
}

TxChainConfig voice_chain_defaults() {
    TxChainConfig c; auto& m = c.mic;
    m.highpass_hz = 100; m.lowpass_hz = 3000;
    m.expander = true; m.exp_cfg = ExpanderConfig{};
    m.leveler = true; m.lev_cfg.target_dbfs = -6; m.lev_cfg.max_gain_db = 20;
    m.compressor = true; m.comp_cfg.threshold_dbfs = -18; m.comp_cfg.ratio = 3; m.comp_cfg.makeup_db = 12;
    m.limiter.ceiling = 0.95; c.ssb.lo_hz = 150; c.ssb.hi_hz = 2800; c.iq_limiter.ceiling = 0.9;
    return c;
}
MicChain::MicChain(const MicChainConfig& c) : c_(c), pre_(float(db2lin(c.mic_gain_db))), lim_(c.sample_rate, c.limiter) {
    if (!std::isfinite(c.mic_gain_db) || c.mic_gain_db > 40) throw std::invalid_argument("mic gain out of range");
    if (c.clip_level < 0 || c.clip_level > 1) throw std::invalid_argument("clip level must be 0 (off) or in (0, 1]");
    for (const auto& b : c.eq) if (!(std::fabs(b.gain_db) <= 24)) throw std::invalid_argument("EQ band gain must be within +-24 dB");
    if (c.highpass_hz > 0) filters_.emplace_back(Biquad::Type::HighPass, c.sample_rate, c.highpass_hz);
    for (const auto& b : c.eq) filters_.emplace_back(b.type, c.sample_rate, b.hz, b.q, b.gain_db);
    if (c.lowpass_hz > 0) filters_.emplace_back(Biquad::Type::LowPass, c.sample_rate, c.lowpass_hz);
    if (c.gate) gate_ = std::make_unique<NoiseGate>(c.sample_rate, c.gate_cfg);
    if (c.expander) exp_ = std::make_unique<DownwardExpander>(c.sample_rate, c.exp_cfg);
    if (c.leveler) lev_ = std::make_unique<Leveler>(c.sample_rate, c.lev_cfg);
    if (c.compressor) comp_ = std::make_unique<Compressor>(c.sample_rate, c.comp_cfg);
}
void MicChain::process(const float* x, size_t n, std::vector<float>& out) {
    tmp_.resize(n);
    const float cl = float(c_.clip_level);
    for (size_t i = 0; i < n; ++i) {
        // Internal levels are bounded (1e3 = +60 dBFS) so a huge-but-finite input times the mic gain cannot overflow to inf and wedge a filter.
        float v = float(std::clamp(double(finite_or_zero(x[i])) * double(pre_), -1e3, 1e3));
        for (auto& f : filters_) v = f.process(v);
        v = std::clamp(finite_or_zero(v), -1e4f, 1e4f);
        if (gate_) v = gate_->process(v);
        if (exp_) v = exp_->process(v);
        if (lev_) v = lev_->process(v);
        if (comp_) v = comp_->process(v);
        if (cl > 0) v = std::clamp(v, -cl, cl);
        tmp_[i] = finite_or_zero(v);
    }
    lim_.process(tmp_.data(), n, out);
}

// ---- TxChain ----
TxChain::TxChain(const TxChainConfig& c) : mic_(c.mic), ssb_(c.ssb), lim_(c.ssb.sample_rate, c.iq_limiter) {
    if (c.mic.sample_rate != c.ssb.sample_rate) throw std::invalid_argument("mic and SSB sample rates differ");
}
void TxChain::process(const float* audio, size_t n, std::vector<cf>& iq) {
    a_.clear(); b_.clear();
    mic_.process(audio, n, a_); ssb_.process(a_.data(), a_.size(), b_); lim_.process(b_.data(), b_.size(), iq);
}

// ---- CW ----
CwShaper::CwShaper(const CwShapeConfig& c) : c_(c) {
    if (!(c.edge_ms >= 1 && c.edge_ms <= 20)) throw std::invalid_argument("CW edge must be 1..20 ms");
    if (!(c.amplitude > 0 && c.amplitude <= 1)) throw std::invalid_argument("CW amplitude must be in (0, 1]");
    if (!(std::fabs(c.offset_hz) <= 3000) || !(c.sample_rate >= 8000 && c.sample_rate <= 384000)) throw std::invalid_argument("bad CW offset or sample rate");
    step_ = 1.0 / (c.edge_ms * 1e-3 * c.sample_rate); dphase_ = 2 * kPi * c.offset_hz / c.sample_rate;
}
float CwShaper::envelope() const { return float(c_.amplitude * (0.5 - 0.5 * std::cos(kPi * pos_))); }
void CwShaper::process(const uint8_t* key, size_t n, std::vector<cf>& iq) {
    for (size_t i = 0; i < n; ++i) {
        pos_ = key[i] ? std::min(1.0, pos_ + step_) : std::max(0.0, pos_ - step_);
        const double e = envelope();
        iq.emplace_back(float(e * std::cos(phase_)), float(e * std::sin(phase_)));
        phase_ += dphase_; if (phase_ > kPi) phase_ -= 2 * kPi; else if (phase_ < -kPi) phase_ += 2 * kPi;
    }
}
std::vector<uint8_t> text_keying(const std::string& text, double wpm, double fs, double lead_s, double farnsworth_wpm) {
    if (!(wpm >= 5 && wpm <= 60)) throw std::invalid_argument("wpm must be 5..60");
    if (!(fs >= 8000 && fs <= 384000) || !(lead_s >= 0 && lead_s <= 60)) throw std::invalid_argument("bad sample rate or lead time");
    const auto env = decode::morse_envelope(text, wpm, fs, 0.0, lead_s, farnsworth_wpm);   // edge 0 = hard 0/1 keying
    std::vector<uint8_t> k(env.size()); for (size_t i = 0; i < env.size(); ++i) k[i] = env[i] > 0.5f;
    return k;
}

IambicKeyer::IambicKeyer(const KeyerConfig& c) : c_(c) {
    if (!(c.wpm >= 5 && c.wpm <= 60)) throw std::invalid_argument("keyer speed must be 5..60 wpm");
    if (!(c.weight >= 25 && c.weight <= 75)) throw std::invalid_argument("keyer weight must be 25..75");
    if (!(c.sample_rate >= 8000 && c.sample_rate <= 384000)) throw std::invalid_argument("keyer sample rate must be 8..384 kHz");
    unit_ = size_t(std::lround(1.2 / c.wpm * c.sample_rate));
    extra_ = std::lround((c.weight - 50) / 50.0 * double(unit_));          // |extra| <= unit/2, so every space stays >= half a unit
}
void IambicKeyer::start(El e) {
    cur_ = e; t_ = 0; mem_dit_ = mem_dah_ = squeeze_ = false;
    const size_t mark = (e == El::Dit ? 1 : 3) * unit_;
    on_ = size_t(long(mark) + extra_); total_ = mark + unit_;
}
bool IambicKeyer::step(bool dit, bool dah) {
    if (c_.reverse) std::swap(dit, dah);
    if (cur_ == El::None) {
        const El opp = last_ == El::Dit ? El::Dah : last_ == El::Dah ? El::Dit : El::None;
        if (opp == El::Dah && (mem_dah_ || dah)) start(El::Dah);
        else if (opp == El::Dit && (mem_dit_ || dit)) start(El::Dit);
        else if (last_ == El::Dit && dit) start(El::Dit);
        else if (last_ == El::Dah && dah) start(El::Dah);
        else if (dit) start(El::Dit);                                         // fresh start; dit wins a dead heat
        else if (dah) start(El::Dah);
        else { last_ = El::None; mem_dit_ = mem_dah_ = false; return false; }
    }
    if (cur_ == El::Dit && dah) mem_dah_ = true;
    if (cur_ == El::Dah && dit) mem_dit_ = true;
    if (dit && dah) squeeze_ = true;
    const bool k = t_ < on_;
    if (++t_ >= total_) {
        if (c_.mode == IambicMode::A && squeeze_ && !dit && !dah) mem_dit_ = mem_dah_ = false;   // squeeze released: Mode A stops here
        last_ = cur_; cur_ = El::None;
    }
    return k;
}
void IambicKeyer::process(const uint8_t* p, size_t n, std::vector<uint8_t>& key) { for (size_t i = 0; i < n; ++i) key.push_back(step(p[i] & 1, p[i] & 2)); }

// ---- Two-tone ----
std::vector<float> two_tone_audio(size_t n, double peak, double f1, double f2, double fs) {
    if (!(peak > 0 && peak <= 1) || !(f1 > 0 && f2 > f1 && f2 < fs / 2)) throw std::invalid_argument("bad two-tone settings");
    std::vector<float> a(n);
    for (size_t i = 0; i < n; ++i) { const double t = double(i) / fs; a[i] = float(0.5 * peak * (std::cos(2 * kPi * f1 * t) + std::cos(2 * kPi * f2 * t))); }
    return a;
}
ImdResult measure_imd(const std::vector<cf>& iq, Sideband sb, double f1, double f2, double fs, size_t skip, double lo_hz, double hi_hz) {
    if (iq.size() < skip + 4096) throw std::invalid_argument("two-tone segment too short");
    const size_t n = iq.size() - skip; const auto w = dsp::blackman_harris(n);
    double wsum = 0; for (double v : w) wsum += v;
    const double sgn = sb == Sideband::USB ? 1.0 : -1.0;
    auto level = [&](double hz) {                                         // amplitude (linear, full-scale tone = 1) at an audio frequency
        hz = std::fabs(hz);
        std::complex<double> acc(0, 0); const double step = -2 * kPi * sgn * hz / fs;
        for (size_t i = 0; i < n; ++i) acc += std::complex<double>(iq[skip + i]) * double(w[i]) * std::polar(1.0, step * double(skip + i));
        return std::abs(acc) / wsum;
    };
    auto db = [](double v) { return 20 * std::log10(v + 1e-12); };
    auto prod = [&](double hz, double ref) {                              // dBc, or NaN when the product is outside the transmitted band
        const double a = std::fabs(hz); return (a < lo_hz || a > hi_hz) ? std::nan("") : db(level(hz) / ref);
    };
    const double t1 = level(f1), t2 = level(f2), ref = std::min(t1, t2);
    ImdResult r;
    r.tone1_db = db(t1); r.tone2_db = db(t2);
    r.im3_low_dbc = prod(2 * f1 - f2, ref); r.im3_high_dbc = prod(2 * f2 - f1, ref);
    r.im5_low_dbc = prod(3 * f1 - 2 * f2, ref); r.im5_high_dbc = prod(3 * f2 - 2 * f1, ref);
    return r;
}

// ---- FT8 / FT4 slot timing ----
double slot_seconds(DigiMode m) { return m == DigiMode::FT8 ? 15.0 : 7.5; }
double signal_seconds(DigiMode m) { return m == DigiMode::FT8 ? 79 * 0.16 : 105 * 0.048; }
double default_start_offset_s(DigiMode) { return 0.5; }
namespace {
Slot slot_from_index(long long k, DigiMode m) { return Slot{k, double(k) * slot_seconds(m), (k % 2 == 0) ? Parity::Even : Parity::Odd}; }   // 15 and 7.5 are exact in binary: no rounding at the boundaries
void need_finite(double v, const char* what) { if (!std::isfinite(v)) throw std::invalid_argument(std::string(what) + " must be finite"); }
void need_utc(double utc) { need_finite(utc, "utc"); if (utc < 0 || utc > 1e12) throw std::invalid_argument("utc out of range"); }   // keeps the slot index far from overflow
}
Slot slot_containing(double utc, DigiMode m) { need_utc(utc); return slot_from_index((long long)std::floor(utc / slot_seconds(m)), m); }
Slot next_slot(double utc, DigiMode m) { return slot_from_index(slot_containing(utc, m).index + 1, m); }
Slot next_slot_with_parity(double utc, DigiMode m, Parity p, double min_lead_s) {
    need_utc(utc); need_finite(min_lead_s, "lead"); if (min_lead_s < 0) throw std::invalid_argument("lead must not be negative");
    long long k = slot_containing(utc + min_lead_s, m).index;
    if (slot_from_index(k, m).start_utc < utc + min_lead_s) ++k;                 // the slot containing the earliest allowed time has already started
    if (slot_from_index(k, m).parity != p) ++k;
    return slot_from_index(k, m);
}
TxPlan plan_tx(double utc, DigiMode m, Parity p, double start_offset_s, double max_late_s, double min_lead_s) {
    need_utc(utc); need_finite(start_offset_s, "start offset"); need_finite(max_late_s, "late limit"); need_finite(min_lead_s, "lead");
    if (start_offset_s < 0 || max_late_s < 0 || min_lead_s < 0) throw std::invalid_argument("negative time");
    if (start_offset_s + signal_seconds(m) > slot_seconds(m)) throw std::invalid_argument("start offset leaves no room for the signal in the slot");
    const Slot cur = slot_containing(utc, m);
    if (cur.parity == p) {
        const double due = cur.start_utc + start_offset_s, ready = utc + min_lead_s;   // when the audio is due / the earliest it can start
        if (ready <= due) return TxPlan{cur, due, 0.0};                                  // on time
        const double skip = ready - due;                                                 // joining late: start when ready, skipping what is already past
        if (skip <= max_late_s && skip < signal_seconds(m)) return TxPlan{cur, ready, skip};
    }
    const Slot n = next_slot_with_parity(utc, m, p, std::max(0.0, min_lead_s - start_offset_s));
    return TxPlan{n, n.start_utc + start_offset_s, 0.0};
}
}  // namespace fireside::tx
