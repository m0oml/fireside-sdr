// The transmit I/Q output: the ONLY code that puts samples on output ch0 / ch1 (QSE I / Q, analysis/transmit-stage2.md section 1).
//   * ch0/1 are exactly zero unless `keyed` is true (set from the TxGate's state); every other channel is always zero
//   * a single tone (offset Hz from the TX DDS; 0 = a carrier on the DDS frequency), or a rendered buffer (FT8 ...), at amplitude `target`, slewed over
//     ~5 ms so keying never clicks
//   * the radio's TX image correction (EEPROM, per band) so the unwanted sideband is suppressed
//   * a HARD clamp on |I| and |Q| at `cap` (the band's drive for the gate's power limit) and never above txs::kAbsoluteMaxDrive (100 W even at a low measured drive levels), applied
//     after everything else: no setting, bug or race can send more
// Used by TxControl (firesidekey and the GUI transmit card).
#pragma once
#include <atomic>
#include <limits>
#include <complex>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include "fireside/playback.h"

namespace fireside::txo {

// A copy of the transmitted I/Q for the display (the transmit spectrum / waterfall): what the over sends, before the mirror, the image correction
// and the clamp (USB above the TX DDS), at the stream's rate, in drive units (power out = the band's k x |x|^2). The stream thread NEVER waits for it:
// write() only try-locks, and drops what does not fit. Display only: nothing read from it goes back to the radio.
class IqTap {
public:
    explicit IqTap(size_t capacity = size_t(1) << 18) : ring_(capacity) {}
    void write(const std::complex<float>* x, size_t n, double rate) noexcept;    // the stream thread
    size_t read(std::vector<std::complex<float>>& out, double* rate = nullptr);   // appends everything queued; returns how many
    uint64_t dropped() const { return dropped_.load(); }
private:
    std::mutex m_; std::vector<std::complex<float>> ring_; size_t head_ = 0, count_ = 0; double rate_ = 0; std::atomic<uint64_t> dropped_{0};
};
class ToneSource {
public:
    explicit ToneSource(double rate = 48000, int channels = 8) : rate_(rate), ch_(channels) {}
    void set_keyed(bool k) { keyed_.store(k); }
    void set_target(double amplitude) { target_.store(amplitude < 0 ? 0 : amplitude); }
    void set_cap(double cap);                                     // clamped to kAbsoluteMaxDrive
    void set_tone_hz(double hz) { tone_.store(hz); }
    void set_mirror(bool m) { mirror_.store(m); }
    void set_tap(std::shared_ptr<IqTap> t) { tap_ = std::move(t); }   // set before the Stream starts: the keyed I/Q is copied to it (the display)                 // Q sign (the TX sign convention is to verify on the radio)
    // TX image correction from the EEPROM tables (PowerSDR units, DttSP correctIQ.c for TX): Q += 0.001 * phase * I, then I *= 1 + 0.001 * gain, on the
    // physical I/Q as sent to the radio (after the mirror), before the hard clamp.
    void set_correction(double gain_units, double phase_units) { corr_gain_.store(1.0 + 0.001 * gain_units); corr_phase_.store(0.001 * phase_units); }
    // Buffer mode (FT8 / FT4 ...): play `iq` (at the stream's rate, envelope normalised to 1) instead of the tone, scaled by the same slewed level, through the
    // same mirror, correction and clamp. Set before the Stream starts. seek() chooses the next sample to play (any thread); after the end: zeros, done() true.
    void set_buffer(std::vector<std::complex<float>> iq) { buf_ = std::move(iq); have_buf_ = true; pos_ = 0; done_ = false; }
    void seek(long long sample) { seek_.store(sample); }            // may be negative: zeros until the buffer's sample 0 is due
    bool done() const { return done_.load(); }
    // Live mode (voice): play I/Q pushed by another thread (envelope <= 1, at the stream's rate) through the same level, mirror, correction
    // and clamp; an empty ring plays silence (counted). Set before the Stream starts.
    void set_live(size_t ring_frames);
    size_t push_live(const std::complex<float>* iq, size_t n);   // any thread; returns how many fitted
    size_t live_queued() const; uint64_t live_underruns() const { return live_under_.load(); }
    long long position() const { return pos_pub_.load(); }
    double cap() const { return cap_.load(); }
    double level() const { return level_pub_.load(); }            // the current (slewed) amplitude (any thread)
    // Interleaved S32_LE frames.
    void fill(int32_t* out, size_t frames);
private:
    double rate_; int ch_; std::atomic<bool> keyed_{false}, mirror_{false}; std::atomic<double> target_{0}, cap_{0}, tone_{0}, corr_gain_{1.0}, corr_phase_{0.0};
    double level_ = 0, phase_ = 0; std::atomic<double> level_pub_{0};   // level_ belongs to the stream thread; level_pub_ is what others read
    bool live_ = false; mutable std::mutex live_m_; std::vector<std::complex<float>> live_ring_; size_t live_head_ = 0, live_count_ = 0; std::atomic<uint64_t> live_under_{0};
    std::vector<std::complex<float>> buf_; bool have_buf_ = false; long long pos_ = 0; std::atomic<long long> seek_{kNoSeek}, pos_pub_{0}; static constexpr long long kNoSeek = std::numeric_limits<long long>::min(); std::atomic<bool> done_{false};
    std::shared_ptr<IqTap> tap_; std::vector<std::complex<float>> tap_buf_;
};

// Runs a ToneSource into an 8-channel playback PCM on its own thread (period-sized blocks; zeros keep the stream alive while unkeyed).
class Stream {
public:
    Stream(std::unique_ptr<IPcmOut> pcm, ToneSource& src);
    ~Stream();
    uint64_t frames() const { return frames_.load(); }
    uint64_t underruns() const { return pcm_->underruns(); }
    bool failed() const { return failed_.load(); }
    double latency_s() const { return pcm_->latency_s(); }
private:
    std::unique_ptr<IPcmOut> pcm_; ToneSource& src_; std::atomic<bool> stop_{false}, failed_{false}; std::atomic<uint64_t> frames_{0}; std::thread th_;
    void run();
};
}  // namespace fireside::txo
