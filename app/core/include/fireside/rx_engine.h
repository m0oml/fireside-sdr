// Runs source -> ReceiveChain on a worker thread so a GUI (or anything) only draws and plays what the engine hands over. Qt-free.
//   * non-live sources (files, demo) are paced in real time; live sources are consumed as the device delivers
//   * spectrum frames and audio go into BOUNDED queues (oldest dropped, counted), so a slow consumer can never grow memory
//   * threaded=false gives deterministic manual stepping (advance(n)) for tests and smoke runs
// Thread-safe: take_*(), status() and configure() may be called from any thread while the worker runs.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "fireside/decode.h"
#include "fireside/dsp.h"
#include "fireside/iqsource.h"

namespace fireside {
struct RxStatus {
    bool running = false, ended = false, live = false, stream_dead = false;
    std::string error, source, decoder;                   // decoder: the active decoder's one-line status (empty if none)
    double sample_rate = 0, peak_hz = 0, peak_db = -200, signal_dbfs = -200, agc_gain_db = 0;
    bool am_locked = false; double am_carrier_hz = 0;      // synchronous AM: the loop holds the carrier, and where it is (Hz from the dial)
    size_t fft_size = 0;
    uint64_t samples = 0, audio_samples = 0, frames = 0, frames_dropped = 0, audio_dropped = 0, xruns = 0;
};
struct RxOptions { dsp::ReceiveConfig rc; dsp::DemodConfig dc; size_t fft = 4096; size_t max_frames = 256, max_audio = 48000 * 2; bool threaded = true; };

class RxEngine {
public:
    RxEngine() = default;
    ~RxEngine() { stop(); }
    RxEngine(const RxEngine&) = delete; RxEngine& operator=(const RxEngine&) = delete;

    void set_source(std::unique_ptr<IqSource> src);       // stops processing, replaces the source, resets counters (rc.sample_rate follows the source on configure())
    void configure(RxOptions opt);                        // (re)builds the chain; keeps the source. Throws std::invalid_argument for bad settings (engine state unchanged)
    // Taps for recording (live sources only). Called on the worker thread with the engine's lock held: be quick, never call back into the engine.
    // iq: the block exactly as the source delivered it (the radio's raw I/Q), rate = the source's sample rate; audio: the demodulated audio, rate = the audio rate.
    using IqTap = std::function<void(const dsp::cf* x, size_t n, double rate_hz)>; using AudioTap = std::function<void(const float* x, size_t n, double rate_hz)>;
    void set_taps(IqTap iq, AudioTap audio);
    void start();                                         // begin/resume (spawns the worker if threaded). No-op without source/config
    void pause();
    void stop();                                          // joins the worker
    bool running() const { return running_; }
    size_t advance(size_t samples);                       // manual mode: process this many source samples now; returns how many were read
    std::vector<std::vector<float>> take_frames();        // spectrum frames produced since the last call (fft-shifted dBFS, centred on the dial)
    size_t take_audio(std::vector<float>& out);           // appends audio produced since the last call; returns the count
    // Runs `fn` with the source (nullptr if none) under the engine lock, e.g. to toggle looping on a FileIqSource. Keep it short: the worker waits.
    // Optional text decoder fed with the demodulated audio on the worker thread (no audio is lost if the display lags). nullptr = off. Decoders expect 48 kHz audio (the default).
    void set_decoder(std::unique_ptr<decode::TextDecoder> d);
    size_t take_text(std::string& out);
    void finish_decoder();
    void peek_decoder();                                  // duplex: the decoder looks at the slot so far without stopping (FT8/FT4 quick pass); then take_text()                                // receive is about to stop: the decoder decodes what it can now (FT8/FT4: the slot so far); then take_text()                   // appends the decoded text since the last call; returns the character count
    void with_source(const std::function<void(IqSource*)>& fn);
    RxStatus status() const;
    double bin_hz(size_t k) const;                        // offset of spectrum bin k from the dial
private:
    mutable std::mutex m_, d_;                            // m_: source+chain (worker/configure), d_: queues+status
    std::unique_ptr<IqSource> src_; std::unique_ptr<dsp::ReceiveChain> chain_; RxOptions opt_; bool have_opt_ = false;
    IqTap iq_tap_; AudioTap audio_tap_; std::thread th_; std::atomic<bool> running_{false}, quit_{false}; std::atomic<uint64_t> epoch_{0};
    std::deque<std::vector<float>> frames_; std::deque<float> audio_; std::string text_; std::unique_ptr<decode::TextDecoder> dec_; RxStatus st_;
    double consumed_ = 0; std::chrono::steady_clock::time_point origin_;
    void loop();
    size_t process_locked(size_t n);                      // requires m_
    void on_frame(const std::vector<float>& db);
};
}  // namespace fireside
