// Live I/Q capture from the radio's audio interface (snd-dice presents an 8-channel S32_LE ALSA capture PCM; analysis/audio-stream-layout.md).
// CAPTURE ONLY: nothing here opens a playback stream, writes mixer/control elements or touches MIDI. The output-channel experiment is deliberately not in this file.
// PcmIqSource is generic over IPcm so everything except the device itself is tested with a fake; AlsaPcm (alsa_pcm.cpp) is the real one.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "fireside/dsp.h"
#include "fireside/iqsource.h"
#include "fireside/rawmidi.h"           // IoError

namespace fireside {
struct CaptureCard { int card = 0; std::string id, name; bool flex_like = false; std::string device() const { return "hw:CARD=" + id + ",DEV=0"; } };
// Cards with a capture PCM (cardN/pcm0c), from /proc/asound; FLEX/DICE-looking cards first.
std::vector<CaptureCard> list_capture_cards(const std::string& asound_dir = "/proc/asound");

class IPcm {
public:
    virtual ~IPcm() = default;
    virtual int channels() const = 0;
    virtual double rate() const = 0;
    // Reads up to `frames` interleaved S32_LE frames; returns frames read, 0 = nothing within timeout_s. Recovers from xruns internally. Throws IoError.
    virtual size_t read(int32_t* interleaved, size_t frames, double timeout_s) = 0;
    virtual uint64_t xruns() const { return 0; }
    virtual std::string describe() const = 0;
};
// Opens a capture-only hw PCM (e.g. "hw:CARD=FLEX5000,DEV=0", or ALSA's "null" for tests). Throws IoError (also when built without ALSA).
std::unique_ptr<IPcm> open_alsa_pcm(const std::string& device, double rate, int channels = 8, size_t period_frames = 1024);
struct PcmCaps { bool ok = false; std::string error; int ch_min = 0, ch_max = 0; unsigned rate_min = 0, rate_max = 0; bool s32le = false; std::vector<unsigned> rates_ok; };
PcmCaps probe_alsa_pcm(const std::string& device);          // opens capture, queries hw params (48/96/192 kHz), closes; starts nothing

inline float s32_to_float(int32_t v) { return float(double(v) / 2147483648.0); }

struct CaptureStats {
    uint64_t frames = 0, reads = 0, empty_reads = 0, xruns = 0;
    std::vector<double> sum_sq, peak;                       // per channel, full-scale = 1.0
    double rms(size_t ch) const { return frames ? std::sqrt(sum_sq[ch] / double(frames)) : 0.0; }
};

class PcmIqSource : public IqSource {
public:
    // ch_i/ch_q: RX1 I/Q = 0/1 on the 5000 (audio-stream-layout.md section 2). Throws std::invalid_argument if they exceed the channel count.
    explicit PcmIqSource(std::unique_ptr<IPcm> pcm, int ch_i = 0, int ch_q = 1, size_t block = 1024);
    double sample_rate() const override { return pcm_->rate(); }
    size_t read(std::vector<dsp::cf>& out, size_t n) override;          // appends <= n samples; waits at most ~5 ms for the first; may return 0
    std::string describe() const override { return pcm_->describe(); }
    bool live() const override { return true; }
    // All channels, raw (for recording); updates the same stats. Appends up to `frames` frames of channels() int32 each.
    size_t read_raw(std::vector<int32_t>& interleaved, size_t frames, double timeout_s);
    int channels() const { return pcm_->channels(); }
    const CaptureStats& stats() const { return stats_; }
    bool stream_dead() const { return dead_; }                          // RX1 I/Q exactly zero for more than 1 s (PowerSDR restarts audio then; we only report)
private:
    std::unique_ptr<IPcm> pcm_; int ci_, cq_; size_t block_; CaptureStats stats_; uint64_t zero_run_ = 0; bool dead_ = false; std::vector<int32_t> buf_;
    void account(const int32_t* d, size_t frames);
};

struct IqAnalysis { double peak_hz = 0, peak_dbfs = -200, image_dbfs = -200, image_rejection_db = 0; bool image_valid = false; };
// Strongest line of an I/Q block and the level at its mirror frequency: a true quadrature pair has a deep image null, I-only / duplicated channels do not.
IqAnalysis analyze_iq(const std::vector<dsp::cf>& x, double fs);
}  // namespace fireside
