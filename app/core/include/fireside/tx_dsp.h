// Transmit DSP. Produces sample buffers (audio or baseband I/Q); it is deliberately radio-free by construction:
// no link, session, safety, pal, startup, opcodes or MIDI. The radio only sees its output through TxControl and the TxGate. A CTest scans this module for forbidden includes.
// Conventions follow the receive side: 48 kHz complex I/Q, sample = I + jQ, dial frequency at 0 Hz, USB above it, LSB below it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <deque>
#include <optional>
#include <vector>
#include <memory>
#include "fireside/dsp.h"

namespace fireside::tx {
using fireside::dsp::cf;

enum class Sideband { USB, LSB };

struct SsbConfig {
    double sample_rate = 48000;
    Sideband sideband = Sideband::USB;
    double lo_hz = 150, hi_hz = 2800;     // audio passband; the sideband is mirrored about 0 Hz for LSB.
    double edge_hz = 100;                 // filter transition width, 20..1000 Hz (sets the opposite-sideband and carrier suppression)
    // No carrier: lo_hz must be >= edge_hz, so 0 Hz (where any DC offset in the audio lands) is always in the filter's stopband.
};

// Filter method: real audio -> one-sided complex band-pass FIR (the same design as Demodulator::band_) -> I/Q.
// A real tone of amplitude A comes out as a complex tone of magnitude A (unity gain; the factor 2 undoes the half lost by keeping one sideband).
// Streaming: the output is delayed by latency_samples() (group delay of the FIR).
class SsbModulator {
public:
    explicit SsbModulator(const SsbConfig& c);    // throws std::invalid_argument
    void process(const float* audio, size_t n, std::vector<cf>& iq);   // appends n samples
    size_t latency_samples() const { return (fir_.taps() - 1) / 2; }
    const SsbConfig& config() const { return c_; }
private:
    SsbConfig c_; dsp::ComplexFir fir_; std::vector<cf> tmp_;
    static dsp::ComplexFir make(const SsbConfig& c);
};

// One-shot helper: modulates `audio` and flushes the filter tail so the output is aligned with the input (same length, delay removed).
std::vector<cf> ssb_modulate(const std::vector<float>& audio, const SsbConfig& c = {});

// ---- Mic chain (real audio, 48 kHz) ---------------------------------------------------------------------------------------------
// RBJ cookbook biquad, transposed direct form II with double state.
class Biquad {
public:
    enum class Type { HighPass, LowPass, Peak, LowShelf, HighShelf, Notch };
    Biquad(Type t, double sample_rate, double hz, double q = 0.7071, double gain_db = 0);   // throws std::invalid_argument
    float process(float x);
    double response_db(double hz) const;    // magnitude response, for tests and UI
private:
    double b0_, b1_, b2_, a1_, a2_, s1_ = 0, s2_ = 0, fs_;
};
struct EqBand { Biquad::Type type; double hz, q, gain_db; };

// Noise gate: opens when the peak envelope rises above threshold, closes below threshold - hysteresis after `hold`.
// The gain moves smoothly (attack when opening, release when closing) between 1 and `range_db` (attenuation, not a hard mute).
struct GateConfig { double threshold_dbfs = -50, hysteresis_db = 6, range_db = -40, attack_ms = 1, hold_ms = 100, release_ms = 50; };
class NoiseGate {
public:
    NoiseGate(double sample_rate, const GateConfig& c);
    float process(float x);
    bool open() const { return open_; }
    float gain() const { return gain_; }
private:
    GateConfig c_; double env_ = 0, env_rel_, att_, rel_; float floor_, gain_; bool open_ = false; size_t hold_n_, hold_ = 0;
};

// Feed-forward compressor/leveler on a peak envelope (attack/release in ms). Above threshold the level rises by 1/ratio dB per dB.
struct CompressorConfig { double threshold_dbfs = -20, ratio = 3, attack_ms = 5, release_ms = 150, makeup_db = 0; };
class Compressor {
public:
    Compressor(double sample_rate, const CompressorConfig& c);   // throws std::invalid_argument (ratio < 1)
    float process(float x);
    double gain_reduction_db() const { return gr_db_; }
private:
    CompressorConfig c_; double env_ = 0, att_, rel_, gr_db_ = 0, makeup_;
};

// Look-ahead peak limiter with a hard ceiling. Gain = box-smoothed (over the look-ahead) min-hold of ceiling/|x| with an exponential
// release, which by construction never lets a sample exceed the ceiling; a final per-sample clamp backs that up against rounding.
// Non-finite input (NaN, inf) is replaced by silence. Works on real audio or on the complex I/Q envelope (phase is preserved).
// Output is delayed by latency_samples().
struct LimiterConfig { double ceiling = 0.891, lookahead_ms = 5, release_ms = 100; };   // 0.891 = -1 dBFS; ceiling in (0, 1], look-ahead 0..50 ms, release 0..10 s
class PeakLimiter {
public:
    PeakLimiter(double sample_rate, const LimiterConfig& c);    // throws std::invalid_argument
    void process(const float* x, size_t n, std::vector<float>& out);   // appends n
    void process(const cf* x, size_t n, std::vector<cf>& out);         // appends n
    size_t latency_samples() const { return la_; }
    double gain() const { return gain_out_; }
    float ceiling() const { return ceil_; }
private:
    cf step(cf x);
    float ceil_; size_t la_; double rel_;
    std::vector<cf> delay_; size_t dpos_ = 0;
    std::deque<std::pair<size_t, double>> minq_; size_t n_ = 0;   // monotonic queue for the sliding minimum of the required gain
    std::vector<double> box_; size_t bpos_ = 0; double bsum_ = 0, held_ = 1, gain_out_ = 1;
};

// 10-band graphic EQ (PowerSDR's TX EQ centres 32 Hz .. 16 kHz, one octave each): peaking bands, gains -15..+15 dB (0 = flat, skipped).
inline constexpr double kGraphicEqHz[10] = {32, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
std::vector<EqBand> graphic_eq10(const double gains_db[10]);   // throws std::invalid_argument outside +-15 dB

// Downward expander (PowerSDR's DEXP): below threshold the level falls by (ratio - 1) dB per dB under it, at most range_db; above it, unity.
// Quieter than a gate between words (no hard switching): room noise and fan hum drop away, speech is untouched.
struct ExpanderConfig { double threshold_dbfs = -45, ratio = 2.5, range_db = -30, attack_ms = 2, hold_ms = 150, release_ms = 150; };
class DownwardExpander {
public:
    DownwardExpander(double sample_rate, const ExpanderConfig& c);   // throws std::invalid_argument (ratio < 1, range > 0)
    float process(float x);
    double gain_db() const { return 20 * std::log10(gain_ + 1e-12); }
private:
    ExpanderConfig c_; double env_ = 0, env_rel_, att_, rel_, gain_ = 1, floor_; size_t hold_n_, hold_ = 0;
};
// Leveler: a slow AGC that brings the speech envelope to `target_dbfs` (never more than max_gain_db of gain), so a soft or a loud voice
// (or mic) reaches the compressor at the same level. Gain falls quickly (attack) and rises slowly (release); below -60 dBFS it holds.
struct LevelerConfig { double target_dbfs = -16, max_gain_db = 15, attack_ms = 30, release_ms = 800; };
class Leveler {
public:
    Leveler(double sample_rate, const LevelerConfig& c);             // throws std::invalid_argument
    float process(float x);
    double gain_db() const { return gain_db_; }
private:
    LevelerConfig c_; double env_ = 0, env_att_, env_rel_, gain_db_ = 0, g_att_, g_rel_;
};

// The whole microphone chain: mic gain -> high-pass -> EQ -> low-pass -> gate -> downward expander -> leveler -> compressor -> clipper -> limiter.
// Any stage can be switched off. The limiter's ceiling bounds the audio; the I/Q ceiling after SSB is TxChain's job (the SSB filter
// can raise peaks above the audio's, so an audio limit alone does not bound the transmit envelope).
struct MicChainConfig {
    double sample_rate = 48000, mic_gain_db = 0;
    double highpass_hz = 100, lowpass_hz = 3000;     // 0 = off
    std::vector<EqBand> eq;
    // Hum filter: narrow notches at every harmonic of the mains frequency (50 or 60 Hz; 0 = off) up to hum_max_hz, right after the input gain
    // and the high-pass, before the expander / leveler / compressor, so they never turn the buzz up. Each notch is about 6 Hz + 0.6 % wide
    // (the mains drifts ~0.1 Hz: n x that at the n-th harmonic).
    double hum_hz = 0, hum_max_hz = 1250;
    bool gate = false; GateConfig gate_cfg;
    bool expander = false; ExpanderConfig exp_cfg;
    bool leveler = false; LevelerConfig lev_cfg;
    bool compressor = true; CompressorConfig comp_cfg;
    double clip_level = 0;                           // 0 = off; otherwise hard clip at this level (adds harmonics: the SSB filter removes those out of band)
    LimiterConfig limiter;
};
class MicChain {
public:
    explicit MicChain(const MicChainConfig& c);      // throws std::invalid_argument
    void process(const float* x, size_t n, std::vector<float>& out);   // appends n (delayed by latency_samples())
    size_t latency_samples() const { return lim_.latency_samples(); }
    const NoiseGate* gate() const { return gate_.get(); }
    const Compressor* compressor() const { return comp_.get(); }
    const DownwardExpander* expander() const { return exp_.get(); }
    const Leveler* leveler() const { return lev_.get(); }
private:
    MicChainConfig c_; float pre_; std::vector<Biquad> filters_; std::unique_ptr<NoiseGate> gate_; std::unique_ptr<DownwardExpander> exp_; std::unique_ptr<Leveler> lev_; std::unique_ptr<Compressor> comp_; PeakLimiter lim_; std::vector<float> tmp_;
};

// Voice transmit path: MicChain -> SsbModulator -> PeakLimiter on the I/Q envelope. The output magnitude never exceeds iq_limiter.ceiling.
struct TxChainConfig { MicChainConfig mic; SsbConfig ssb; LimiterConfig iq_limiter; };
// Voice defaults (PowerSDR-like): high-pass 100 Hz, low-pass 3 kHz, expander on, leveler to -6 dBFS (max +20 dB), compressor 3:1 above
// -18 dBFS with +12 dB make-up (speech drives into the limiters), audio limiter 0.95, SSB 150..2800 Hz, I/Q ceiling 0.9: speech peaks reach the ceiling, so the power
// setting is the peak envelope power.
TxChainConfig voice_chain_defaults();
// Data from another program (WSJT-X, fldigi ...): no processing at all (no filters, EQ, expander, leveler, compressor or clipper), so a tone goes
// out as a clean tone; SSB 100..3000 Hz; the audio limiter (0.95) and the I/Q ceiling (0.9) stay as the hard ceilings. The power setting is
// reached when the program's audio is at full scale.
TxChainConfig data_chain_defaults();
class TxChain {
public:
    explicit TxChain(const TxChainConfig& c);
    void process(const float* audio, size_t n, std::vector<cf>& iq);   // appends n
    size_t latency_samples() const { return mic_.latency_samples() + ssb_.latency_samples() + lim_.latency_samples(); }
private:
    MicChain mic_; SsbModulator ssb_; PeakLimiter lim_; std::vector<float> a_; std::vector<cf> b_;
};

// ---- CW ---------------------------------------------------------------------------------------------------------------------------
// Keying is one byte per sample (non-zero = key down). The shaper turns it into I/Q: a carrier at offset_hz from the dial (0 = on the
// dial, which is where the receive CW mode listens: it makes the beat note itself) with raised-cosine edges of edge_ms.
// The rise starts at key-down and the fall at key-up, so the half-amplitude points sit edge/2 after each transition: the keyed
// length (weight) is preserved exactly. A key change mid-edge reverses from the current level; the envelope never steps.
struct CwShapeConfig { double sample_rate = 48000, edge_ms = 5, offset_hz = 0, amplitude = 1.0; };
class CwShaper {
public:
    explicit CwShaper(const CwShapeConfig& c);   // throws std::invalid_argument (edge 1..20 ms, amplitude in (0, 1], |offset| <= 3 kHz)
    void process(const uint8_t* key, size_t n, std::vector<cf>& iq);   // appends n
    float envelope() const;
private:
    CwShapeConfig c_; double pos_ = 0, step_, phase_ = 0, dphase_;
};
// Keying for text at a speed (PARIS timing, optional Farnsworth spacing), with `lead_s` of key-up first and 1 s after.
std::vector<uint8_t> text_keying(const std::string& text, double wpm, double fs = 48000, double lead_s = 0.5, double farnsworth_wpm = 0);

// Iambic keyer as pure logic: paddle states in, key out, one call per sample. Element memory in both modes (a tap of the opposite
// paddle during an element is sent next). Mode B also sends one more opposite element when a squeeze is released; Mode A does not.
// Weight 50 is standard 1:1 (dit) / 3:1 (dah); other values lengthen or shorten every mark by the same amount and shorten or
// lengthen the following space to keep the element period (and so the speed) unchanged.
enum class IambicMode { A, B };
struct KeyerConfig { double sample_rate = 48000, wpm = 20, weight = 50; IambicMode mode = IambicMode::B; bool reverse = false; };
class IambicKeyer {
public:
    explicit IambicKeyer(const KeyerConfig& c);   // throws std::invalid_argument (wpm 5..60, weight 25..75)
    bool step(bool dit, bool dah);               // one sample; returns key down
    void process(const uint8_t* paddles, size_t n, std::vector<uint8_t>& key);   // bit 0 = dit paddle, bit 1 = dah paddle; appends n
    bool idle() const { return cur_ == El::None; }
private:
    enum class El { None, Dit, Dah };
    void start(El e);
    KeyerConfig c_; size_t unit_; long extra_;
    El cur_ = El::None, last_ = El::None; size_t t_ = 0, on_ = 0, total_ = 0; bool mem_dit_ = false, mem_dah_ = false, squeeze_ = false;
};

// ---- Two-tone test and distortion measurement --------------------------------------------------------------------------------------
// Standard SSB linearity test: two equal audio tones (default 600 and 1500 Hz: both third-order products, 300 and 2400 Hz, fall inside the 150-2800 Hz passband). `peak` is the peak of the SUM as a fraction of full
// scale, so peak = 1 drives a stage exactly to its limit (each tone is peak/2; cosines, so they align at t=0 and the peak is exact). Output is real audio, `n` samples.
std::vector<float> two_tone_audio(size_t n, double peak = 0.5, double f1 = 600, double f2 = 1500, double fs = 48000);

struct ImdResult {
    double tone1_db, tone2_db;           // dBFS of each tone (a full-scale complex tone is 0 dBFS)
    double im3_low_dbc, im3_high_dbc;    // 2f1-f2 and 2f2-f1, relative to the weaker tone
    double im5_low_dbc, im5_high_dbc;    // 3f1-2f2 and 3f2-2f1
    // A product that folds outside [lo_hz, hi_hz] cannot be transmitted (the SSB filter removes it): it is reported as NaN and ignored below.
    double im3_dbc() const { return worst(im3_low_dbc, im3_high_dbc); }   // the worse (higher) of the two
    double im5_dbc() const { return worst(im5_low_dbc, im5_high_dbc); }
private:
    static double worst(double a, double b) { return a != a ? b : b != b ? a : (a > b ? a : b); }
};
// Measures the I/Q produced from a two-tone test. Tone frequencies are the AUDIO frequencies; for LSB the products are looked up mirrored
// about 0 Hz. A product at a negative audio frequency (2f1-f2 < 0) is real audio at |f|, and is looked up there. Uses a Blackman-Harris windowed DFT evaluated at the exact product frequencies (no bin-straddling error) over
// samples [skip, n): choose skip past the filter and limiter settling. Throws std::invalid_argument if the segment is under 4096 samples.
ImdResult measure_imd(const std::vector<cf>& iq, Sideband sb, double f1 = 600, double f2 = 1500, double fs = 48000, size_t skip = 9600, double lo_hz = 150, double hi_hz = 2800);

// ---- FT8 / FT4 slot timing (pure functions of UTC) -----------------------------------------------------------------------------------
// Slots are aligned to UTC: FT8 every 15 s (:00 :15 :30 :45), FT4 every 7.5 s (:00 :07.5 :15 ...). Even/odd is the parity of the slot
// index (floor(utc / length)); since 60 s is a whole number of slots the parity is the same as counted from the start of any minute, so
// FT8 even = :00 and :30, odd = :15 and :45. `utc` is seconds since the Unix epoch. Nothing here reads a clock: the caller supplies the time.
enum class DigiMode { FT8, FT4, WSPR };   // WSPR: 120 s slots, each starting on an even UTC minute (parity = alternate two-minute slots)
enum class Parity { Even, Odd };
struct Slot { long long index; double start_utc; Parity parity; };
struct TxPlan {
    Slot slot;
    double audio_start_utc;     // when sample 0 of the transmit audio must be sent (slot start + start_offset, or now if joining late)
    double skip_s;              // seconds of the audio that are already in the past (0 unless joining late)
};
double slot_seconds(DigiMode m);                // 15 / 7.5 / 120 (WSPR)
double signal_seconds(DigiMode m);              // 79 x 0.16 = 12.64 s / 105 x 0.048 = 5.04 s (symbol periods: QEX 2020 Table 4; FT4 = 87 + 16 sync + 2 ramp symbols. FT4 period 7.5 s: "twice as fast as FT8"; the 6 s / 4.48 s of the April 2019 FT4 draft are superseded)
// Nominal start of the transmission: 0.5 s after the beginning of the T/R interval. Source: Franke, Somerville, Taylor, "The FT4 and FT8 Communication
// Protocols", QEX July/August 2020 (the AP section, which covers both modes: "the nominal start time, 0.5 s after the beginning of a transmit interval").
// The paper gives no separate FT4 figure. It also says that a late WSJT-X "will skip the missed part of the transmission and start sending a correctly
// synchronized message for the remainder" (what plan_tx's skip_s does), and that without a-priori information a decoder loses callers who start more than
// 5 s late (8 s with AP "mycall"), so a useful max_late_s is a few seconds at most.
double default_start_offset_s(DigiMode m);     // 0.5 s (FT8 / FT4); WSPR 1 s (WSJT-X: WSPR transmissions start 1 s after the even minute)
Slot slot_containing(double utc, DigiMode m);                  // the slot with start <= utc < start + length
Slot next_slot(double utc, DigiMode m);                        // the first slot that starts strictly after utc
Slot next_slot_with_parity(double utc, DigiMode m, Parity p, double min_lead_s = 0);   // first slot of parity p starting at least min_lead_s after utc
// Plans one transmission in a slot of parity p. If the current slot has that parity and utc is within max_late_s of where the audio should
// have started, joins it late (skip_s > 0, never more than max_late_s); otherwise the next slot of that parity, needing at least min_lead_s
// to prepare. A late join also honours min_lead_s: the audio starts at utc + min_lead_s, with skip_s covering everything already past.
// max_late_s = 0 means never join late. Throws std::invalid_argument for non-finite or out-of-range utc (0..1e12), negative times, or an
// offset that leaves no room for the signal in the slot.
TxPlan plan_tx(double utc, DigiMode m, Parity p, double start_offset_s, double max_late_s = 0, double min_lead_s = 0);
}  // namespace fireside::tx
