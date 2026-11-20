// Recording of what the receiver sees: a replay buffer that always holds the last N seconds (so "grab" saves what just happened) and a normal start/stop recording
// (with an optional pre-roll taken from the same buffer). One Recorder per stream: the I/Q of the band (2 channels, 32-bit PCM, the radio's raw I/Q: same files as
// firesidecapture --stream) and the demodulated audio (1 channel, 16-bit PCM, what you hear). WAV files, written on a background thread: push() never touches the disk.
// Qt-free. push() is called from the receive thread, everything else from any thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fireside {
class Recorder {
public:
    struct Format { int channels = 2; int bits = 32; };                       // bits: 16 or 32
    explicit Recorder(Format f);
    ~Recorder();
    Recorder(const Recorder&) = delete; Recorder& operator=(const Recorder&) = delete;

    // (Re)allocates the replay buffer for this sample rate and length; drops its contents and finishes any recording (the rate of a file is fixed). replay_seconds 0 = no buffer.
    void configure(double rate_hz, double replay_seconds);
    void push(const float* interleaved, size_t frames);                        // full scale = +-1.0; clipped
    double rate() const;
    double buffered_seconds() const;                                           // how much the replay buffer holds now (up to its length)
    // Saves the last `seconds` of the replay buffer (clamped to what it holds) to a WAV file on the writer thread. false if the buffer is empty or a grab is still being written.
    bool grab(double seconds, const std::string& path);
    // Starts a streaming recording, beginning with `preroll_seconds` from the replay buffer. false if already recording, no rate set, or the file cannot be created.
    bool start(const std::string& path, double preroll_seconds = 0);
    void stop();                                                               // finalises the header; waits for the data queued so far; idempotent
    struct Status { bool recording = false, saving = false; double rec_seconds = 0; uint64_t rec_bytes = 0; uint64_t dropped_frames = 0; std::string path, last_path, error; };
    Status status() const;
    // Limits (tests and safety): the largest file (a WAV header holds 4 GB) and how much unwritten data may queue before frames are dropped (disk too slow).
    void set_limits(uint64_t max_file_bytes, uint64_t max_queue_bytes);

private:
    struct Job { std::string path; std::vector<int32_t> data; };
    void writer();
    void write_header(std::FILE* f, uint64_t data_bytes) const;
    void write_samples(std::FILE* f, const std::vector<int32_t>& v, std::vector<uint8_t>& scratch) const;   // int32 samples -> the file's sample width
    Format fmt_; mutable std::mutex m_; std::condition_variable cv_; std::thread th_; bool quit_ = false;
    double rate_ = 0; std::vector<int32_t> ring_; size_t ring_frames_ = 0, head_ = 0, count_ = 0;       // frames; interleaved int32 (full scale 2^31)
    std::deque<std::vector<int32_t>> queue_; uint64_t queued_bytes_ = 0, dropped_ = 0, max_file_ = 3900ull * 1000 * 1000, max_queue_ = 256ull * 1000 * 1000;
    std::deque<Job> grabs_; bool recording_ = false, finishing_ = false, saving_ = false, stream_pending_ = false; std::string stream_path_; std::string path_, last_path_, error_; uint64_t rec_frames_ = 0, rec_bytes_ = 0;
    uint64_t gen_ = 0;                                                         // bumped by configure(): a recording started before it is finished first
};
}  // namespace fireside
