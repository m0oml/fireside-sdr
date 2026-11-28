// Receive DSP on baseband I/Q: 9 kHz IF shift, spectrum + waterfall, simple SSB/AM/CW demodulator, dead-stream watchdog.
// Hardware-free: everything runs on recorded or synthetic I/Q. This is NOT WDSP (no dev package on the build host); it sits behind small
// interfaces so WDSP can replace the demodulator later. Conventions (analysis/audio-stream-layout.md): I = in ch0, Q = in ch1, sample = I + jQ.
// OPEN until a known carrier is seen on hardware: whether I/Q is mirrored. ReceiveConfig::mirror conjugates the input to compensate.
#pragma once
#include <complex>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fireside::dsp {
using cf = std::complex<float>;
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kIfOffsetHz = 9000.0;               // PowerSDR: if_shift on, if_freq 0.009 MHz (DDS = dial - 9 kHz)

// ---- FFT ----
class Fft {                                                  // radix-2, in-place, forward, unscaled
public:
    explicit Fft(size_t n);                                  // n must be a power of two >= 2
    size_t size() const { return n_; }
    void forward(cf* a) const;
private:
    size_t n_; std::vector<cf> tw_; std::vector<uint32_t> rev_;
};
std::vector<float> blackman_harris(size_t n);                // 4-term, periodic form (for spectral analysis)

// ---- IF shift ----
// The radio's DDS sits at dial - if_offset, so the dial frequency arrives at +if_offset in the I/Q. This moves it to 0 Hz.
struct ReceiveConfig { double sample_rate = 48000; double if_offset_hz = kIfOffsetHz; bool mirror = false; };
class IfShifter {
public:
    explicit IfShifter(const ReceiveConfig& c);
    void process(cf* x, size_t n);                           // in place; phase-continuous across calls
private:
    double phase_ = 0, step_; bool mirror_;
};
// DDS frequency (MHz) that puts `dial_mhz` at +if_offset in the I/Q. (The reviewed start-up still tunes the DDS exactly to the dial; see startup-review.md.)
inline double dds_for_dial_mhz(double dial_mhz, double if_offset_hz = kIfOffsetHz) { return dial_mhz - if_offset_hz / 1e6; }

// ---- spectrum / waterfall ----
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(double sample_rate, size_t fft_size = 4096, float averaging = 1.0f /*1 = none, <1 = exponential*/);
    // Feeds samples; every fft_size/2 new samples produces a frame (50 % overlap). Returns the number of frames produced.
    size_t push(const cf* x, size_t n);
    const std::vector<float>& db() const { return db_; }     // fft-shifted: index 0 = -fs/2, index N/2 = centre. dBFS (full-scale complex tone = 0 dB)
    size_t size() const { return n_; }
    double bin_hz(size_t k) const { return (double(k) - double(n_) / 2.0) * fs_ / double(n_); }   // offset from the centre frequency
    size_t bin_for_hz(double hz) const;                      // nearest bin
    size_t frames() const { return frames_; }
    std::function<void(const std::vector<float>&)> on_frame;
private:
    double fs_; size_t n_; float alpha_; Fft fft_; std::vector<float> win_; float win_sum_;
    std::vector<cf> ring_, work_; size_t fill_ = 0, frames_ = 0; std::vector<float> db_, pw_;
    void compute();
};
class Waterfall {
public:
    Waterfall(size_t width, size_t max_rows) : width_(width), max_rows_(max_rows) {}
    void push(const std::vector<float>& db_row);             // newest first; rows beyond max_rows are dropped
    size_t rows() const { return rows_.size(); }
    size_t width() const { return width_; }
    const std::vector<float>& row(size_t i) const { return rows_[i]; }   // 0 = newest
    std::vector<uint8_t> to_gray(float lo_db, float hi_db) const;        // rows() x width() bytes, newest row first
private:
    size_t width_, max_rows_; std::deque<std::vector<float>> rows_;
};

// ---- filters / demodulator ----
class ComplexFir {                                           // streaming, optional decimation
public:
    ComplexFir(std::vector<cf> taps, int decim = 1);
    void process(const cf* x, size_t n, std::vector<cf>& out);   // appends
    size_t taps() const { return h_.size(); }
    static std::vector<float> lowpass(double cutoff_hz, double transition_hz, double fs);   // Blackman windowed sinc, odd length
private:
    std::vector<cf> h_; int decim_; std::vector<cf> buf_; size_t skip_ = 0;
};
enum class Mode { USB, LSB, AM, CW, FM };
enum class AgcMode { Off, Fast, Med, Slow };
struct AgcParams { double decay_ms, hang_ms; };
AgcParams agc_preset(AgcMode m);                             // decay/hang in ms: Fast 50/0, Med 250/50, Slow 1000/500. Attack is instantaneous (peak-tracking), so output never exceeds the target

// AGC on the complex band-limited signal: level follows the envelope up instantly, holds for `hang`, then decays exponentially; output envelope = target at most.
class Agc {
public:
    Agc(double sample_rate, AgcMode mode, float target = 0.25f, double max_gain_db = 80.0);
    void process(cf* x, size_t n);
    double gain_db() const { return 20.0 * std::log10(double(gain_)); }
    void set_mode(AgcMode m);
private:
    double fs_; AgcMode mode_; float target_, floor_, level_, gain_ = 1.0f; float decay_ = 0; size_t hang_samples_ = 0, hang_ = 0; double max_gain_db_;
};

// Adaptive line filter (NLMS on a delayed reference): predicts the correlated part of a signal (tones, voiced speech) from its own past.
// Prediction = noise reduction (broadband noise is not predictable and is suppressed); Residual = auto-notch (tones are predicted and removed).
class AdaptiveLineFilter {
public:
    enum class Output { Prediction, Residual };
    AdaptiveLineFilter(Output out, size_t taps = 64, size_t delay = 16, float mu = 0.02f);
    float process(float x);
private:
    Output out_; size_t n_, d_; float mu_; std::vector<float> w_, hist_; size_t pos_ = 0; double power_ = 0;   // hist_ is a ring of n_+d_ samples, newest at pos_
};
// RBJ biquad notch at `hz` (audio domain) with quality factor q (higher = narrower).
class Notch {
public:
    Notch(double sample_rate, double hz, double q = 10.0);
    float process(float x);
private:
    double b0_, b1_, b2_, a1_, a2_, x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

// Impulse noise blanker on the full-rate complex stream: samples whose magnitude exceeds threshold x (running mean of un-blanked magnitudes) are zeroed, plus `hold` after.
class NoiseBlanker {
public:
    explicit NoiseBlanker(float threshold = 6.0f, size_t hold = 2) : thr_(threshold), hold_(hold) {}
    void process(cf* x, size_t n);
    size_t blanked() const { return blanked_total_; }
private:
    float thr_; size_t hold_, left_ = 0, blanked_total_ = 0; float avg_ = 0; bool primed_ = false;
};

// Signal strength. Relative to full scale unless the radio is calibrated; `full_scale_dbm` is the input level that reads 0 dBFS (needs a signal generator measurement; unknown today).
struct SReading { int s_units; int over_s9_db; };            // S0..S9, then S9 + dB. HF convention: S9 = -73 dBm, 6 dB per S-unit
SReading s_reading(double dbm);
int cat_smeter_value(double dbm);                            // Kenwood TS-2000 SM scale 0..30: S0..S9 -> 0..15, then 4 dB per step up to S9+60 -> 30 (from memory of the reference)
// Default passband (Hz relative to the dial) the demodulator uses for a mode when DemodConfig lo/hi are 0.
std::pair<double, double> default_passband(Mode m);
// Passband (Hz relative to the dial) for a filter of `width_hz` in a mode: SSB keeps the usual 150 Hz low cut (mirrored for LSB), AM/FM/CW are centred on the dial.
std::pair<double, double> passband_for(Mode m, double width_hz, double low_cut_hz = 150);   // SSB: low_cut .. low_cut+width (LSB mirrored); AM/FM/CW: centred on the dial (low_cut unused)
// Filter widths offered in the UI for a mode (Hz), narrowest first, and the one used by default.
std::vector<double> filter_widths(Mode m);
double default_filter_width(Mode m);
// AM detection: the envelope (|signal|, simplest, distorts when the carrier fades more than the sidebands), or synchronous AM: a phase-locked loop on the carrier and
// coherent detection against it (no fading distortion, less noise), optionally from one sideband only (to drop interference on the other side).
enum class AmDetector { Envelope, Sync };
enum class AmSideband { Both, Upper, Lower };
// An optional extra audio stage for the receiver (NR2, fireside/nr2.h). Transmit code includes this header and the transmit isolation check
// (tests/tx_isolation.cmake) allows it no receive-only extras, so the stage is created through a factory that the receiver registers at start-up.
struct AudioStage { virtual ~AudioStage() = default; virtual float process(float x) = 0; };
using AudioStageFactory = std::function<std::unique_ptr<AudioStage>(double audio_rate)>;
void set_spectral_nr_factory(AudioStageFactory f);          // nr2.cpp's enable_spectral_nr() calls this; nothing registered = DemodConfig::spectral_nr does nothing
// FM / repeater tones. The 50 standard CTCSS tones (EIA / TIA-603), 67.0 .. 254.1 Hz, lowest first; transmit (later) and receive share the table.
const std::vector<double>& ctcss_tones();
// UK regional repeater tone letter (A..J) for a tone, or 0 if it is not one of them (A 67.0, B 71.9, C 77.0, D 82.5, E 88.5, F 94.8, G 103.5, H 110.9, J 118.8).
char ctcss_uk_letter(double hz);
// CTCSS (tone squelch) detector on demodulated FM audio. Low-passes and decimates to 1 kHz, then every 50 ms measures all 50 tones over the last
// 0.4 s (Hann window, Goertzel). A tone is "heard" when it is the strongest of the 50 and >= 10 dB above their median, twice running (100 ms);
// it is lost after 250 ms without that. process() returns whether `want_hz` (0 = any tone) is being heard.
struct Biquad {                                              // direct form II transposed; coefficients normalised (a0 = 1)
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad highpass(double fs, double f0, double q);    // RBJ cookbook
    static Biquad lowpass(double fs, double f0, double q);
    float run(float x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return float(y); }
};
class CtcssDetector {
public:
    explicit CtcssDetector(double audio_rate, double want_hz = 0);
    bool process(float a);
    bool open() const { return open_; }
    double heard_hz() const { return heard_hz_; }        // the tone being heard (0 = none)
private:
    void evaluate();
    double want_, rate_; int decim_, phase_ = 0; double acc_ = 0; Biquad lp1_, lp2_;
    std::vector<float> ring_, win_; size_t pos_ = 0, filled_ = 0; int since_eval_ = 0, hits_ = 0; double last_hit_s_ = -1e9, t_s_ = 0, cand_ = 0;
    bool open_ = false; double heard_hz_ = 0;
};
// FM audio stages: 6th-order Butterworth high-pass at 300 Hz (keeps a CTCSS tone out of the speaker) and de-emphasis, the inverse of the
// usual 300 Hz .. 3 kHz pre-emphasis (fireside/tx_dsp.h FmModulator): a pole at 300 Hz and a zero at 3 kHz, normalised to unity at 1 kHz.
class FmAudio {
public:
    FmAudio(double audio_rate, bool subaudio_hpf, bool deemphasis);
    float process(float x);
private:
    bool hpf_, de_; Biquad h1_, h2_, h3_; double de_b0_ = 1, de_b1_ = 0, de_a1_ = 0, de_x1_ = 0, de_y1_ = 0;
};
struct DemodConfig {
    Mode mode = Mode::USB; double input_rate = 48000; double audio_rate = 48000;   // input_rate must be an integer multiple of audio_rate
    double lo_hz = 0, hi_hz = 0;                              // passband relative to the dial; 0/0 = mode default (USB 200..2800, LSB -2800..-200, AM +-4000, CW +-250)
    double edge_fraction = 0.1, edge_floor_hz = 150;          // filter skirt: transition width = max(edge_floor_hz, edge_fraction x bandwidth). Smaller = sharper (more taps, more delay)
    double cw_pitch_hz = 700; float gain = 1.0f;
    double manual_gain_db = 0;                                 // fixed gain used only when the AGC is Off (the radio's I/Q is about -90 dBFS: ~60 dB is audible); the result is soft-limited (tanh) so a strong signal cannot blast. 0 = unity, as before
    AgcMode agc = AgcMode::Off; float agc_target = 0.25f; double agc_max_gain_db = 80.0;
    bool noise_blanker = false; float nb_threshold = 6.0f;
    double fm_deviation_hz = 5000;                            // audio = +-1 at this deviation
    bool fm_deemphasis = true, fm_subaudio_hpf = true;         // FM only: 6 dB/oct de-emphasis, and the 300 Hz high-pass that keeps CTCSS out of the speaker
    double ctcss_hz = 0;                                       // FM only: tone squelch, audio passes only while this CTCSS tone is heard (0 = off). With the level squelch on as well, both must be open
    AmDetector am_detector = AmDetector::Sync; AmSideband am_sideband = AmSideband::Both;   // AM only
    double sam_loop_hz = 30;                                  // synchronous AM: PLL natural frequency (Hz). Wider = faster lock, more phase noise; it pulls in +-200 Hz of mistuning
    bool noise_reduction = false, auto_notch = false;
    bool spectral_nr = false;                                  // NR2 (fireside/nr2.h, WDSP EMNR): spectral noise reduction for voice; ignored if not built in; ~90 ms delay          // adaptive line filter: NR keeps the predictable part, auto-notch removes it
    double notch_hz = 0, notch_q = 10;                         // manual audio notch (0 = off)
    std::vector<double> extra_notches_hz;                      // further manual notches (audio Hz, same Q), applied after notch_hz
    double squelch_dbfs = -200;                                // mute audio while the in-passband signal is below this level (-200 = off); 2 dB hysteresis
};
class Demodulator {
public:
    explicit Demodulator(const DemodConfig& c);               // throws std::invalid_argument
    void process(const cf* x, size_t n, std::vector<float>& audio);   // x is already centred on the dial (after IfShifter); appends audio
    int decimation() const { return decim_; }
    double signal_dbfs() const;                               // in-passband power before AGC, ~100 ms smoothing; -200 if nothing yet. A full-scale in-band tone = 0 dBFS
    double agc_gain_db() const { return c_.agc == AgcMode::Off ? c_.manual_gain_db : agc_.gain_db(); }   // with the AGC off: the manual gain
    void set_agc(AgcMode m) { c_.agc = m; agc_.set_mode(m); }
    size_t blanked() const { return nb_.blanked(); }
    double am_carrier_hz() const { return sam_freq_ * c_.audio_rate / (2 * 3.14159265358979323846); }   // synchronous AM: where the loop has the carrier, Hz from the dial
    bool am_locked() const { return sam_locked_; }            // synchronous AM: the loop holds the carrier (small phase error, carrier present)
    double ctcss_heard_hz() const { return ctcss_ ? ctcss_->heard_hz() : 0.0; }   // FM: the CTCSS tone being heard (0 = none)
    bool tone_open() const { return !ctcss_ || c_.ctcss_hz <= 0 || ctcss_->open(); }
private:
    void sam_reset();
    float sam_sample(cf s);                                   // one sample of synchronous AM detection
    DemodConfig c_; int decim_; ComplexFir pre_, band_; std::vector<cf> tmp0_, tmp1_, tmp2_; Agc agc_; NoiseBlanker nb_;
    std::unique_ptr<Notch> notch_; std::vector<std::unique_ptr<Notch>> extra_notches_; std::unique_ptr<AdaptiveLineFilter> anf_, nr_; std::unique_ptr<AudioStage> nr2_; bool squelch_open_ = true; float squelch_gain_ = 1.0f; double sq_power_ = 0;
    double cw_phase_ = 0, am_dc_ = 0, power_ = 0; cf prev_{0, 0}; bool metered_ = false;
    double sam_phase_ = 0, sam_freq_ = 0, sam_err_avg_ = 1, sam_cmag_avg_ = 0; cf sam_c_{0, 0}; bool sam_locked_ = false; double sam_kp_ = 0, sam_ki_ = 0, sam_ca_ = 0;
    std::unique_ptr<CtcssDetector> ctcss_; std::unique_ptr<FmAudio> fm_audio_;
    std::vector<float> sam_hilbert_, sam_q_, sam_i_; size_t sam_pos_ = 0;   // Hilbert FIR on Q, and I delayed to match it (one-sideband detection)
};

// The whole receive path for one stream: IfShifter -> SpectrumAnalyzer (+ waterfall callback) and -> Demodulator.
class ReceiveChain {
public:
    ReceiveChain(const ReceiveConfig& rc, const DemodConfig& dc, size_t fft_size = 4096);
    void process(const cf* iq, size_t n, std::vector<float>& audio);   // modifies a private copy; audio appended
    SpectrumAnalyzer& spectrum() { return spec_; }
    const ReceiveConfig& config() const { return rc_; }
    Demodulator& demod() { return demod_; }
    double signal_dbfs() const { return demod_.signal_dbfs(); }
private:
    ReceiveConfig rc_; IfShifter shift_; SpectrumAnalyzer spec_; Demodulator demod_; std::vector<cf> buf_;
};

// Stream-loss watchdog, as PowerSDR (console.cs:50160): RX1 input exactly 0 for more than one second of buffers => restart audio.
class DeadStreamWatchdog {
public:
    DeadStreamWatchdog(double sample_rate, size_t block_size) : limit_(size_t(sample_rate / double(block_size)) + 1) {}
    bool feed(const cf* x, size_t n);                         // true once the stream has been dead for longer than 1 s
    void reset() { zeros_ = 0; }
private:
    size_t limit_, zeros_ = 0;
};

// ---- synthetic and recorded I/Q ----
struct Tone { double hz; double amplitude; };                // hz relative to the DC of the I/Q, amplitude linear (1 = full scale)
std::vector<cf> synth_tones(double fs, size_t n, const std::vector<Tone>& tones, double noise_rms = 0.0, uint32_t seed = 1, double start_sample = 0);
// Streaming WAV reader (PCM 16/24/32, float32, plain/extensible/RF64): reads frames from disk on demand, so multi-hundred-MB recordings need no memory. Throws std::runtime_error.
class WavStream {
public:
    explicit WavStream(const std::string& path, int ch_i = 0, int ch_q = 1);
    double sample_rate() const { return rate_; }
    int channels() const { return channels_; }
    uint64_t frames() const { return frames_; }
    size_t read(std::vector<cf>& out, size_t n);      // appends up to n I/Q samples; 0 at the end
    void rewind();
private:
    mutable std::ifstream f_; std::string path_; double rate_ = 0; int channels_ = 0, ci_, cq_; unsigned tag_ = 0, bits_ = 0; uint64_t data_off_ = 0, frames_ = 0, pos_ = 0; std::vector<uint8_t> raw_;
};

// Arbitrary-ratio resampler for complex I/Q (polyphase windowed-sinc, streaming). Rates must be whole numbers of Hz; the reduced ratio L/M must be <= 2000/ any M.
// Passband to 0.92 x the lower Nyquist, > 70 dB stopband. Group delay is not compensated (constant).
class RationalResampler {
public:
    RationalResampler(double in_rate, double out_rate);      // throws std::invalid_argument
    void process(const cf* x, size_t n, std::vector<cf>& out);   // appends
    unsigned up() const { return L_; } unsigned down() const { return M_; }
private:
    unsigned L_, M_, K_; std::vector<std::vector<cf>> sub_;   // sub_[phase][tap], taps in time order
    std::vector<cf> buf_; long long base_, pos_ = 0;
};
// Recordings the chain cannot take as they are (not a whole multiple of 48 kHz up to 192 kHz) are resampled: to 48/96/192 kHz, the largest not above the input, or 48 kHz if lower.
bool needs_resampling(double in_rate);
double choose_output_rate(double in_rate);

struct IqRecording { double sample_rate = 0; std::vector<cf> samples; int channels = 0; };
// Reads PCM16/24/32 or float32 WAV (plain or WAVE_FORMAT_EXTENSIBLE); picks channels ch_i, ch_q (default 0 and 1, the RX1 pair of the 8-ch stream). Throws std::runtime_error.
IqRecording read_iq_wav(const std::string& path, int ch_i = 0, int ch_q = 1);
void write_wav_f32(const std::string& path, double fs, int channels, const std::vector<float>& interleaved);
void write_wav_pcm16_mono(const std::string& path, double fs, const std::vector<float>& samples);
}  // namespace fireside::dsp
