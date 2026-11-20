// The ONLY file that opens an ALSA PLAYBACK stream. Audio out to the radio's jacks; no mixer or control API, no radio opcodes.
#include <cstring>
#include <stdexcept>
#include "fireside/playback.h"
#ifdef FIRESIDE_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace fireside {
#ifdef FIRESIDE_HAVE_ALSA
namespace {
struct HwParams { snd_pcm_hw_params_t* p = nullptr; HwParams() { snd_pcm_hw_params_malloc(&p); } ~HwParams() { snd_pcm_hw_params_free(p); } };
std::string err(const std::string& dev, const char* what, int e) { return dev + ": " + what + ": " + snd_strerror(e); }

class AlsaPcmOut : public IPcmOut {
public:
    AlsaPcmOut(const std::string& dev, double rate, int ch, size_t period) : dev_(dev) {
        int e = snd_pcm_open(&h_, dev.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (e < 0) throw IoError(err(dev, "cannot open playback", e));
        try {
            HwParams hw;
            if ((e = snd_pcm_hw_params_any(h_, hw.p)) < 0) throw IoError(err(dev, "hw_params_any", e));
            if ((e = snd_pcm_hw_params_set_access(h_, hw.p, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) throw IoError(err(dev, "interleaved access", e));
            if ((e = snd_pcm_hw_params_set_format(h_, hw.p, SND_PCM_FORMAT_S32_LE)) < 0) throw IoError(err(dev, "S32_LE format", e));
            if ((e = snd_pcm_hw_params_set_channels(h_, hw.p, unsigned(ch))) < 0) { unsigned lo = 0, hi = 0; snd_pcm_hw_params_get_channels_min(hw.p, &lo); snd_pcm_hw_params_get_channels_max(hw.p, &hi);
                throw IoError(err(dev, ("channels=" + std::to_string(ch) + " (device offers " + std::to_string(lo) + ".." + std::to_string(hi) + ")").c_str(), e)); }
            unsigned r = unsigned(rate); if ((e = snd_pcm_hw_params_set_rate(h_, hw.p, r, 0)) < 0) throw IoError(err(dev, ("rate " + std::to_string(r)).c_str(), e));
            snd_pcm_uframes_t per = period; int dir = 0; snd_pcm_hw_params_set_period_size_near(h_, hw.p, &per, &dir); snd_pcm_uframes_t buf = per * 4; snd_pcm_hw_params_set_buffer_size_near(h_, hw.p, &buf);
            if ((e = snd_pcm_hw_params(h_, hw.p)) < 0) throw IoError(err(dev, "hw_params", e));
            ch_ = ch; rate_ = rate; period_ = size_t(per);
            if ((e = snd_pcm_prepare(h_)) < 0) throw IoError(err(dev, "prepare", e));
        } catch (...) { snd_pcm_close(h_); h_ = nullptr; throw; }
    }
    ~AlsaPcmOut() override { if (h_) { snd_pcm_drop(h_); snd_pcm_close(h_); } }
    int channels() const override { return ch_; }
    double rate() const override { return rate_; }
    uint64_t underruns() const override { return underruns_; }
    std::string describe() const override { return dev_ + " (playback " + std::to_string(ch_) + " ch S32_LE " + std::to_string(int(rate_)) + " Hz)"; }
    size_t write(const int32_t* d, size_t frames) override {
        const snd_pcm_sframes_t n = snd_pcm_writei(h_, d, frames);
        if (n == -EAGAIN) return 0;
        if (n < 0) { if (n == -EPIPE) ++underruns_; const int r = snd_pcm_recover(h_, int(n), 1); if (r < 0) throw IoError(err(dev_, "playback failed", r)); return 0; }
        return size_t(n);
    }
private:
    snd_pcm_t* h_ = nullptr; std::string dev_; int ch_ = 0; double rate_ = 0; size_t period_ = 0; uint64_t underruns_ = 0;
};
}  // namespace
std::unique_ptr<IPcmOut> open_alsa_play(const std::string& d, double rate, int ch, size_t period) { return std::make_unique<AlsaPcmOut>(d, rate, ch, period); }
#else
std::unique_ptr<IPcmOut> open_alsa_play(const std::string& d, double, int, size_t) { throw IoError(d + ": built without ALSA (libasound development files were not found)"); }
#endif
}  // namespace fireside
