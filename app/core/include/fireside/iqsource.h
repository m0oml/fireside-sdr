// Where I/Q comes from. Today: recorded files and a synthetic demo. Live ALSA capture will be a third implementation of the same interface.
#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "fireside/dsp.h"

namespace fireside {
class IqSource {
public:
    virtual ~IqSource() = default;
    virtual double sample_rate() const = 0;
    virtual size_t read(std::vector<dsp::cf>& out, size_t n) = 0;   // appends up to n samples, returns how many (0 = ended; live sources never end)
    virtual std::string describe() const = 0;
    virtual bool live() const { return false; }                      // live sources can return 0 samples without having ended
    virtual void set_loop(bool) {}                                   // file sources: restart at the end instead of stopping
    virtual void rewind() {}                                         // file sources: back to the first sample (FT8/FT4 slots are counted from there)
};

class FileIqSource : public IqSource {                               // WAV, streamed from disk: RX1 I/Q = channels 0/1 by default
public:
    explicit FileIqSource(const std::string& path, int ch_i = 0, int ch_q = 1, bool loop = false);   // throws std::runtime_error
    double sample_rate() const override { return wav_.sample_rate(); }
    size_t read(std::vector<dsp::cf>& out, size_t n) override;
    std::string describe() const override;
    void set_loop(bool l) override { loop_ = l; }
    void rewind() override { wav_.rewind(); }
    size_t size() const { return size_t(wav_.frames()); }
private:
    std::string path_; dsp::WavStream wav_; bool loop_;
};

// Wraps any source and delivers it at another sample rate (polyphase resampler). The recording's centre frequency stays the centre; its edges beyond the new Nyquist are filtered out.
class ResamplingIqSource : public IqSource {
public:
    ResamplingIqSource(std::unique_ptr<IqSource> inner, double out_rate);       // throws std::invalid_argument for awkward ratios
    double sample_rate() const override { return out_rate_; }
    size_t read(std::vector<dsp::cf>& out, size_t n) override;
    std::string describe() const override;
    bool live() const override { return inner_->live(); }
    void set_loop(bool l) override { inner_->set_loop(l); }
    void rewind() override { inner_->rewind(); pending_.clear(); eof_ = false; }
private:
    std::unique_ptr<IqSource> inner_; double out_rate_; dsp::RationalResampler rs_; std::deque<dsp::cf> pending_; std::vector<dsp::cf> in_, tmp_; bool eof_ = false;
};
// Opens a WAV I/Q file at whatever rate it has: files the chain can take directly are used as they are, others are resampled (see dsp::needs_resampling).
std::unique_ptr<IqSource> open_iq_file(const std::string& path, int ch_i = 0, int ch_q = 1, bool loop = false);

// Deterministic demo as a radio with its DDS 9 kHz below the dial would deliver it: a steady carrier 1 kHz above the dial, an AM-like carrier with
// 1 kHz sidebands 5 kHz below, a tone sweeping +-10 kHz over 12 s (makes the waterfall move), and noise at about -60 dBFS.
class DemoIqSource : public IqSource {
public:
    explicit DemoIqSource(double sample_rate = 48000) : fs_(sample_rate) {}
    double sample_rate() const override { return fs_; }
    size_t read(std::vector<dsp::cf>& out, size_t n) override;
    std::string describe() const override { return "synthetic demo signal"; }
private:
    double fs_; uint64_t pos_ = 0; double sweep_phase_ = 0;
};
}  // namespace fireside
