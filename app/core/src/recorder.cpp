#include "fireside/recorder.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace fireside {
namespace {
inline int32_t to_i32(float v) { const double x = double(v) * 2147483648.0; return x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : int32_t(std::lround(x)); }
void put16(std::FILE* f, uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); }
void put32(std::FILE* f, uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); }
}  // namespace

Recorder::Recorder(Format f) : fmt_(f) {
    if (fmt_.channels < 1 || fmt_.channels > 8 || (fmt_.bits != 16 && fmt_.bits != 32)) throw std::invalid_argument("Recorder: channels 1..8, bits 16 or 32");
    th_ = std::thread([this] { writer(); });
}
Recorder::~Recorder() { stop(); { std::lock_guard<std::mutex> l(m_); quit_ = true; } cv_.notify_all(); if (th_.joinable()) th_.join(); }

void Recorder::configure(double rate_hz, double replay_seconds) {
    stop();                                                                  // a file's rate is fixed: finish the recording first
    std::lock_guard<std::mutex> l(m_);
    rate_ = rate_hz; ring_frames_ = replay_seconds > 0 && rate_hz > 0 ? size_t(std::lround(replay_seconds * rate_hz)) : 0;
    ring_.assign(ring_frames_ * size_t(fmt_.channels), 0); head_ = 0; count_ = 0; ++gen_;
}
double Recorder::rate() const { std::lock_guard<std::mutex> l(m_); return rate_; }
double Recorder::buffered_seconds() const { std::lock_guard<std::mutex> l(m_); return rate_ > 0 ? double(count_) / rate_ : 0.0; }

void Recorder::push(const float* x, size_t frames) {
    if (frames == 0) return;
    const size_t ch = size_t(fmt_.channels);
    std::vector<int32_t> conv(frames * ch); for (size_t i = 0; i < conv.size(); ++i) conv[i] = to_i32(x[i]);          // outside the lock
    std::unique_lock<std::mutex> l(m_);
    if (ring_frames_) {
        size_t from = 0, n = frames; if (n > ring_frames_) { from = n - ring_frames_; n = ring_frames_; }
        for (size_t i = 0; i < n;) {                                                                                  // copy in at most two pieces around the wrap
            const size_t chunk = std::min(n - i, ring_frames_ - head_);
            std::memcpy(ring_.data() + head_ * ch, conv.data() + (from + i) * ch, chunk * ch * sizeof(int32_t)); head_ = (head_ + chunk) % ring_frames_; i += chunk;
        }
        count_ = std::min(ring_frames_, count_ + frames);
    }
    if (recording_) {
        const uint64_t bytes = uint64_t(frames) * ch * (fmt_.bits / 8), queued_file = queued_bytes_ / (32 / fmt_.bits);
        if (rec_bytes_ + queued_file + bytes > max_file_) { recording_ = false; finishing_ = true; if (error_.empty()) error_ = "size limit reached: recording stopped"; }
        else if (queued_bytes_ + conv.size() * 4 > max_queue_) { dropped_ += frames; if (error_.empty()) error_ = "the disk is too slow: samples were dropped"; }
        else { queued_bytes_ += conv.size() * 4; queue_.push_back(std::move(conv)); l.unlock(); cv_.notify_one(); }
    }
}

bool Recorder::grab(double seconds, const std::string& path) {
    std::lock_guard<std::mutex> l(m_);
    if (count_ == 0 || rate_ <= 0 || saving_) return false;
    const size_t want = std::min(count_, size_t(std::max(0.0, std::lround(seconds * rate_) + 0.0))); if (want == 0) return false;
    const size_t ch = size_t(fmt_.channels); Job j; j.path = path; j.data.resize(want * ch);
    const size_t start = (head_ + ring_frames_ - want) % ring_frames_;                                              // the oldest wanted frame
    const size_t first = std::min(want, ring_frames_ - start);
    std::memcpy(j.data.data(), ring_.data() + start * ch, first * ch * sizeof(int32_t));
    if (first < want) std::memcpy(j.data.data() + first * ch, ring_.data(), (want - first) * ch * sizeof(int32_t));
    saving_ = true; grabs_.push_back(std::move(j)); cv_.notify_one(); return true;
}

bool Recorder::start(const std::string& path, double preroll) {
    std::unique_lock<std::mutex> l(m_);
    if (recording_ || finishing_ || rate_ <= 0) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false; std::fclose(f);                           // fail early if the folder is not writable
    const size_t ch = size_t(fmt_.channels); size_t pre = std::min(count_, size_t(std::max(0.0, std::lround(preroll * rate_) + 0.0)));
    queue_.clear(); queued_bytes_ = 0; rec_bytes_ = 0; rec_frames_ = 0; dropped_ = 0; error_.clear(); path_ = path;
    if (pre) {
        std::vector<int32_t> p(pre * ch); const size_t start_i = (head_ + ring_frames_ - pre) % ring_frames_, first = std::min(pre, ring_frames_ - start_i);
        std::memcpy(p.data(), ring_.data() + start_i * ch, first * ch * sizeof(int32_t)); if (first < pre) std::memcpy(p.data() + first * ch, ring_.data(), (pre - first) * ch * sizeof(int32_t));
        queued_bytes_ += p.size() * 4; queue_.push_back(std::move(p));
    }
    stream_path_ = path; stream_pending_ = true; recording_ = true; finishing_ = false; l.unlock(); cv_.notify_one(); return true;
}
void Recorder::stop() {
    std::unique_lock<std::mutex> l(m_);
    if (recording_) { recording_ = false; finishing_ = true; }
    cv_.notify_all();
    cv_.wait(l, [&] { return !finishing_; });                                                                       // the writer clears it after the header is final
}
Recorder::Status Recorder::status() const {
    std::lock_guard<std::mutex> l(m_); Status s; s.recording = recording_; s.saving = saving_; s.rec_bytes = rec_bytes_ + queued_bytes_ / (32 / fmt_.bits);
    s.rec_seconds = rate_ > 0 ? double(rec_frames_) / rate_ : 0.0; s.dropped_frames = dropped_; s.path = recording_ ? path_ : std::string(); s.last_path = last_path_; s.error = error_; return s;
}
void Recorder::set_limits(uint64_t max_file, uint64_t max_queue) { std::lock_guard<std::mutex> l(m_); max_file_ = max_file; max_queue_ = max_queue; }

void Recorder::write_header(std::FILE* f, uint64_t data) const {
    const uint32_t ch = uint32_t(fmt_.channels), bytes = uint32_t(fmt_.bits / 8), rate = uint32_t(std::lround(rate_)), d = uint32_t(std::min<uint64_t>(data, 0xFFFFFFFFull - 40));
    std::fseek(f, 0, SEEK_SET); std::fwrite("RIFF", 1, 4, f); put32(f, 36 + d); std::fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, uint16_t(ch)); put32(f, rate);
    put32(f, rate * ch * bytes); put16(f, uint16_t(ch * bytes)); put16(f, uint16_t(fmt_.bits)); std::fwrite("data", 1, 4, f); put32(f, d);
}

void Recorder::write_samples(std::FILE* f, const std::vector<int32_t>& v, std::vector<uint8_t>& out) const {
    if (fmt_.bits == 32) { std::fwrite(v.data(), 4, v.size(), f); return; }
    out.resize(v.size() * 2);
    for (size_t i = 0; i < v.size(); ++i) { int32_t s = int32_t((int64_t(v[i]) + 0x8000) >> 16); s = std::clamp(s, -32768, 32767); out[2 * i] = uint8_t(s); out[2 * i + 1] = uint8_t(s >> 8); }
    std::fwrite(out.data(), 1, out.size(), f);
}

void Recorder::writer() {
    std::FILE* f = nullptr; std::string cur; std::vector<uint8_t> scratch;
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
        cv_.wait(l, [&] { return quit_ || !grabs_.empty() || stream_pending_ || !queue_.empty() || finishing_; });
        if (stream_pending_ && !f) {                                                                                // a streaming recording begins
            cur = stream_path_; stream_pending_ = false; f = std::fopen(cur.c_str(), "wb");
            if (!f) { recording_ = false; finishing_ = false; error_ = "cannot write " + cur; queue_.clear(); queued_bytes_ = 0; cv_.notify_all(); }
            else { l.unlock(); write_header(f, 0); l.lock(); }
        }
        while (!grabs_.empty()) {                                                                                   // grabs are independent of a running recording
            Job j = std::move(grabs_.front()); grabs_.pop_front(); l.unlock();
            std::FILE* g = std::fopen(j.path.c_str(), "wb"); bool ok = g != nullptr;
            if (g) { write_header(g, uint64_t(j.data.size()) * (fmt_.bits / 8)); write_samples(g, j.data, scratch); ok = std::ferror(g) == 0 && std::fclose(g) == 0; }
            l.lock(); saving_ = false; if (ok) last_path_ = j.path; else error_ = "cannot write " + j.path;
        }
        while (f && !queue_.empty()) {
            std::vector<int32_t> blk = std::move(queue_.front()); queue_.pop_front(); l.unlock();                       // still counted in queued_bytes_ while it is being written: never in neither count
            write_samples(f, blk, scratch); const uint64_t b = uint64_t(blk.size()) * (fmt_.bits / 8); l.lock(); rec_bytes_ += b; queued_bytes_ -= blk.size() * 4; rec_frames_ += blk.size() / size_t(fmt_.channels);
        }
        if (f && finishing_ && queue_.empty()) {                                                                    // no more data coming: finalise
            const uint64_t data = rec_bytes_; l.unlock(); write_header(f, data); const bool ok = std::ferror(f) == 0 && std::fclose(f) == 0; l.lock(); f = nullptr;
            if (ok) last_path_ = cur; else if (error_.empty()) error_ = "write error on " + cur; finishing_ = false; cv_.notify_all();
        }
        if (quit_ && grabs_.empty() && queue_.empty() && !f && !stream_pending_) return;
    }
}
}  // namespace fireside
