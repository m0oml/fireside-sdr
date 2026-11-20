// Audio OUT to the radio's own jacks (headphones / powered speakers / line out) through the 8-channel ALSA playback stream of the FireWire card.
// The PC makes the audio; the radio's DACs play it. WHICH of the 8 output channels reach which jack is the open question in analysis/audio-stream-layout.md section 4
// (FlexRadio's two tables disagree), so nothing here has a default: a channel PAIR must be chosen explicitly, after the output-channel test (tools/firesideplay).
// This is audio only: no opcodes, no keying. Using the wrong pair cannot transmit (MOX stays off), but it could put receive audio where it does not belong.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "fireside/dsp.h"
#include "fireside/rawmidi.h"          // IoError

namespace fireside {
class IPcmOut {
public:
    virtual ~IPcmOut() = default;
    virtual int channels() const = 0;
    virtual double rate() const = 0;
    // Writes up to `frames` interleaved S32_LE frames; returns how many were accepted (0 = the device is full, try later). Recovers from underruns itself. Throws IoError.
    virtual size_t write(const int32_t* interleaved, size_t frames) = 0;
    virtual uint64_t underruns() const { return 0; }
    virtual std::string describe() const = 0;
    virtual double latency_s() const { return 2.0 * 1024 / rate(); }   // a written sample reaches the radio this much later (ALSA: two periods)
};
// Opens a playback-only hw PCM (e.g. "hw:CARD=FLEX5000,DEV=0", or ALSA's "null" for tests). Throws IoError (also when built without ALSA).
std::unique_ptr<IPcmOut> open_alsa_play(const std::string& device, double rate, int channels = 8, size_t period_frames = 1024);

// Mono audio (48 kHz) -> the chosen channel pair of an N-channel stream, all other channels silent. Resamples to the stream rate; never blocks; drops what the device cannot take.
class RadioAudioOut {
public:
    // pair: 0 = channels 0/1, 1 = 2/3, 2 = 4/5, 3 = 6/7. Throws std::invalid_argument for a pair the stream does not have.
    RadioAudioOut(std::unique_ptr<IPcmOut> pcm, int pair, double input_rate = 48000);
    void set_volume(float v) { volume_ = v < 0 ? 0 : v > 1 ? 1 : v; }
    void push(const float* audio, size_t n);
    int pair() const { return pair_; }
    uint64_t frames_written() const { return written_; }
    uint64_t frames_dropped() const { return dropped_; }
    uint64_t underruns() const { return pcm_->underruns(); }
    std::string describe() const { return pcm_->describe() + ", channels " + std::to_string(2 * pair_) + "/" + std::to_string(2 * pair_ + 1); }
private:
    std::unique_ptr<IPcmOut> pcm_; int pair_; float volume_ = 0.5f; std::unique_ptr<dsp::RationalResampler> rs_; std::vector<int32_t> pending_; size_t pending_pos_ = 0, pending_frames_ = 0; uint64_t written_ = 0, dropped_ = 0;
    std::vector<dsp::cf> in_, out_;
    void flush();
};
}  // namespace fireside
