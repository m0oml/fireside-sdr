// The only file that talks to libasound. CAPTURE ONLY: SND_PCM_STREAM_CAPTURE is the single stream direction used; no playback, mixer or control API is called.
#include <cstring>
#include <stdexcept>
#include "fireside/capture.h"
#ifdef FIRESIDE_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace fireside {
#ifdef FIRESIDE_HAVE_ALSA
namespace {
struct HwParams { snd_pcm_hw_params_t* p = nullptr; HwParams() { snd_pcm_hw_params_malloc(&p); } ~HwParams() { snd_pcm_hw_params_free(p); } };
std::string err(const std::string& dev, const char* what, int e) { return dev + ": " + what + ": " + snd_strerror(e); }

class AlsaPcm : public IPcm {
public:
    AlsaPcm(const std::string& dev, double rate, int ch, size_t period) : dev_(dev) {
        int e = snd_pcm_open(&h_, dev.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (e < 0) throw IoError(err(dev, "cannot open capture", e));
        try {
            HwParams hw;
            if ((e = snd_pcm_hw_params_any(h_, hw.p)) < 0) throw IoError(err(dev, "hw_params_any", e));
            if ((e = snd_pcm_hw_params_set_access(h_, hw.p, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) throw IoError(err(dev, "interleaved access", e));
            if ((e = snd_pcm_hw_params_set_format(h_, hw.p, SND_PCM_FORMAT_S32_LE)) < 0) throw IoError(err(dev, "S32_LE format", e));
            if ((e = snd_pcm_hw_params_set_channels(h_, hw.p, unsigned(ch))) < 0) {
                unsigned lo = 0, hi = 0; snd_pcm_hw_params_get_channels_min(hw.p, &lo); snd_pcm_hw_params_get_channels_max(hw.p, &hi);
                throw IoError(err(dev, ("channels=" + std::to_string(ch) + " (device offers " + std::to_string(lo) + ".." + std::to_string(hi) + ")").c_str(), e));
            }
            unsigned r = unsigned(rate);
            if ((e = snd_pcm_hw_params_set_rate(h_, hw.p, r, 0)) < 0) throw IoError(err(dev, ("rate " + std::to_string(r)).c_str(), e));
            snd_pcm_uframes_t per = period; int dir = 0; snd_pcm_hw_params_set_period_size_near(h_, hw.p, &per, &dir);
            snd_pcm_uframes_t buf = per * 4; snd_pcm_hw_params_set_buffer_size_near(h_, hw.p, &buf);
            if ((e = snd_pcm_hw_params(h_, hw.p)) < 0) throw IoError(err(dev, "hw_params", e));
            ch_ = ch; rate_ = rate; period_ = size_t(per);
            if ((e = snd_pcm_prepare(h_)) < 0) throw IoError(err(dev, "prepare", e));
            if ((e = snd_pcm_start(h_)) < 0) throw IoError(err(dev, "start", e));
        } catch (...) { snd_pcm_close(h_); h_ = nullptr; throw; }
    }
    ~AlsaPcm() override { if (h_) snd_pcm_close(h_); }
    int channels() const override { return ch_; }
    double rate() const override { return rate_; }
    uint64_t xruns() const override { return xruns_; }
    std::string describe() const override { return dev_ + " (" + std::to_string(ch_) + " ch S32_LE " + std::to_string(int(rate_)) + " Hz, period " + std::to_string(period_) + ")"; }
    size_t read(int32_t* out, size_t frames, double timeout_s) override {
        const int w = snd_pcm_wait(h_, int(timeout_s * 1000));
        if (w < 0) { recover(w); return 0; }
        if (w == 0) return 0;
        const snd_pcm_sframes_t n = snd_pcm_readi(h_, out, frames);
        if (n == -EAGAIN) return 0;
        if (n < 0) { if (n == -EPIPE) ++xruns_; recover(int(n)); return 0; }
        return size_t(n);
    }
private:
    snd_pcm_t* h_ = nullptr; std::string dev_; int ch_ = 0; double rate_ = 0; size_t period_ = 0; uint64_t xruns_ = 0;
    void recover(int e) {
        if (e == -EPIPE) ++xruns_;
        const int r = snd_pcm_recover(h_, e, 1);
        if (r < 0) throw IoError(err(dev_, "capture failed", r));
        if (snd_pcm_state(h_) != SND_PCM_STATE_RUNNING) snd_pcm_start(h_);
    }
};
}  // namespace

std::unique_ptr<IPcm> open_alsa_pcm(const std::string& d, double rate, int ch, size_t period) { return std::make_unique<AlsaPcm>(d, rate, ch, period); }
PcmCaps probe_alsa_pcm(const std::string& dev) {
    PcmCaps c; snd_pcm_t* h = nullptr; int e = snd_pcm_open(&h, dev.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (e < 0) { c.error = snd_strerror(e); return c; }
    HwParams hw; snd_pcm_hw_params_any(h, hw.p);
    unsigned a = 0, b = 0; int dir = 0;
    snd_pcm_hw_params_get_channels_min(hw.p, &a); snd_pcm_hw_params_get_channels_max(hw.p, &b); c.ch_min = int(a); c.ch_max = int(b);
    snd_pcm_hw_params_get_rate_min(hw.p, &c.rate_min, &dir); snd_pcm_hw_params_get_rate_max(hw.p, &c.rate_max, &dir);
    c.s32le = snd_pcm_hw_params_test_format(h, hw.p, SND_PCM_FORMAT_S32_LE) == 0;
    for (unsigned r : {48000u, 96000u, 192000u}) {              // a rate counts only if it works with all 8 channels the receiver needs (snd-dice drops channels at 96/192 kHz)
        HwParams t; snd_pcm_hw_params_copy(t.p, hw.p);
        if (snd_pcm_hw_params_set_channels(h, t.p, 8) == 0 && snd_pcm_hw_params_test_rate(h, t.p, r, 0) == 0) c.rates_ok.push_back(r);
    }
    snd_pcm_close(h); c.ok = true; return c;
}
#else
std::unique_ptr<IPcm> open_alsa_pcm(const std::string& d, double, int, size_t) { throw IoError(d + ": built without ALSA (libasound development files were not found)"); }
PcmCaps probe_alsa_pcm(const std::string&) { PcmCaps c; c.error = "built without ALSA"; return c; }
#endif
}  // namespace fireside
