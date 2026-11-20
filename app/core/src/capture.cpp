#include "fireside/capture.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sys/stat.h>

namespace fireside {
std::vector<CaptureCard> list_capture_cards(const std::string& dir) {
    std::vector<CaptureCard> out;
    std::ifstream in(dir + "/cards"); std::string line; const std::regex re(R"(^\s*(\d+)\s+\[(\S+)\s*\]:\s*(.*))");
    while (std::getline(in, line)) {
        std::smatch m; if (!std::regex_match(line, m, re)) continue;
        CaptureCard c; c.card = std::stoi(m[1]); c.id = m[2].str(); c.name = m[3].str();
        struct stat st; if (::stat((dir + "/card" + std::to_string(c.card) + "/pcm0c").c_str(), &st) != 0) continue;        // no capture PCM
        std::string low = c.id + " " + c.name; std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        c.flex_like = low.find("flex") != std::string::npos || low.find("dice") != std::string::npos;
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const CaptureCard& a, const CaptureCard& b) { return a.flex_like && !b.flex_like; });
    return out;
}

PcmIqSource::PcmIqSource(std::unique_ptr<IPcm> pcm, int ci, int cq, size_t block)
    : pcm_(std::move(pcm)), ci_(ci), cq_(cq), block_(block) {
    if (!pcm_) throw std::invalid_argument("no PCM");
    if (ci < 0 || cq < 0 || ci >= pcm_->channels() || cq >= pcm_->channels()) throw std::invalid_argument("I/Q channel index outside the stream's channel count");
    if (block == 0) throw std::invalid_argument("block must be > 0");
    stats_.sum_sq.assign(size_t(pcm_->channels()), 0.0); stats_.peak.assign(size_t(pcm_->channels()), 0.0);
}
void PcmIqSource::account(const int32_t* d, size_t frames) {
    const size_t ch = size_t(pcm_->channels());
    for (size_t f = 0; f < frames; ++f) for (size_t c = 0; c < ch; ++c) {
        const double v = double(s32_to_float(d[f * ch + c])); stats_.sum_sq[c] += v * v; stats_.peak[c] = std::max(stats_.peak[c], std::fabs(v));
    }
    stats_.frames += frames; ++stats_.reads; stats_.xruns = pcm_->xruns();
    std::vector<dsp::cf> iq(frames); for (size_t f = 0; f < frames; ++f) iq[f] = dsp::cf(s32_to_float(d[f * ch + size_t(ci_)]), s32_to_float(d[f * ch + size_t(cq_)]));
    bool zero = true; for (const auto& v : iq) if (v.real() != 0.0f || v.imag() != 0.0f) { zero = false; break; }
    zero_run_ = zero ? zero_run_ + frames : 0;                                      // time-based: independent of how the PCM chunks its reads
    dead_ = double(zero_run_) > pcm_->rate();
}
size_t PcmIqSource::read_raw(std::vector<int32_t>& out, size_t frames, double timeout_s) {
    const size_t ch = size_t(pcm_->channels()); buf_.resize(frames * ch);
    const size_t got = pcm_->read(buf_.data(), frames, timeout_s);
    if (got == 0) { ++stats_.empty_reads; stats_.xruns = pcm_->xruns(); return 0; }
    account(buf_.data(), got); out.insert(out.end(), buf_.begin(), buf_.begin() + long(got * ch)); return got;
}
size_t PcmIqSource::read(std::vector<dsp::cf>& out, size_t n) {
    size_t got = 0; std::vector<int32_t> raw; const size_t ch = size_t(pcm_->channels());
    while (got < n) {
        raw.clear(); const size_t r = read_raw(raw, std::min(n - got, block_), got == 0 ? 0.005 : 0.0);
        if (r == 0) break;
        for (size_t f = 0; f < r; ++f) out.emplace_back(s32_to_float(raw[f * ch + size_t(ci_)]), s32_to_float(raw[f * ch + size_t(cq_)]));
        got += r;
    }
    return got;
}

IqAnalysis analyze_iq(const std::vector<dsp::cf>& x, double fs) {
    IqAnalysis a; const size_t N = 8192;
    if (x.size() < N) return a;
    dsp::SpectrumAnalyzer sp(fs, N, 0.25f); sp.push(x.data(), x.size());
    const auto& db = sp.db(); const size_t k = size_t(std::max_element(db.begin(), db.end()) - db.begin());
    a.peak_hz = sp.bin_hz(k); a.peak_dbfs = double(db[k]);
    const long off = long(k) - long(N / 2);
    if (std::labs(off) >= 8) {                                          // a line at DC has no distinct mirror image
        const size_t m = size_t(long(N / 2) - off); float best = -300.f;
        for (long d = -2; d <= 2; ++d) { const long i = long(m) + d; if (i >= 0 && i < long(N)) best = std::max(best, db[size_t(i)]); }
        a.image_dbfs = double(best); a.image_rejection_db = a.peak_dbfs - a.image_dbfs; a.image_valid = true;
    }
    return a;
}
}  // namespace fireside
