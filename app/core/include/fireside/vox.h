// Voice: VOX (keying by speech), its always-listening input with pre-roll, and the no-RF feedback test (the mic through the transmit processing to
// the PC's speakers). Qt-free; nothing here keys the radio: VOX only says "speech now"; the Transmit card decides and keys through TxControl::voice.
#pragma once
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "fireside/tx_control.h"
#include "fireside/tx_dsp.h"
#include "fireside/tx_out.h"

namespace fireside::vox {
// Speech detector on the mic (after the input gain): on as soon as the peak envelope stays above threshold_dbfs for attack_ms; off hang_s after it
// last was. Anti-VOX (anti_vox): the receive audio coming out of the speaker must not key it, so the mic must also be at least anti_gain_db above
// the speaker's level (the caller supplies that level per block).
struct VoxConfig { double sample_rate = 48000, threshold_dbfs = -30, attack_ms = 10, hang_s = 0.6; bool anti_vox = false; double anti_gain_db = 0; };
class VoxDetector {
public:
    explicit VoxDetector(const VoxConfig& c);                  // throws std::invalid_argument (threshold -80..0, hang 0.1..5 s, attack 1..200 ms)
    bool process(const float* x, size_t n, double speaker_dbfs = -200);   // returns on()
    bool on() const { return on_; }
    double level_dbfs() const { return 20 * std::log10(env_ + 1e-12); }
private:
    VoxConfig c_; double env_ = 0, rel_; size_t above_ = 0, below_ = 0, attack_, hang_; bool on_ = false;
};

// An always-listening input for VOX: a thread reads the real input all the time, feeds the detector and keeps the last pre_roll_s of audio.
// The voice over reads it as its IAudioIn and gets that pre-roll first, so the first syllable (spoken before the transmitter was keyed) is sent;
// the whole over is delayed by pre_roll_s. Between overs nothing is kept beyond the pre-roll. The hang must be longer than the pre-roll.
class VoxInput : public txc::IAudioIn {
public:
    VoxInput(std::unique_ptr<txc::IAudioIn> in, const VoxConfig& c, double input_gain_db, double pre_roll_s = 0.15, std::function<double()> speaker_dbfs = {});
    ~VoxInput() override;
    bool on() const { return on_.load(); }                    // speech now (any thread)
    double level_dbfs() const { return level_.load(); }
    bool failed() const { return failed_.load(); } std::string error() const;
    void set_reading(bool r);                                  // an over reads us now (true) / has ended (false): the FIFO keeps only the pre-roll otherwise
    size_t read(float* x, size_t n, double timeout_s) override;   // throws IoError when the real input failed
    std::string describe() const override;
private:
    std::unique_ptr<txc::IAudioIn> in_; VoxDetector det_; float gain_; size_t pre_; std::function<double()> spk_;
    std::thread th_; std::atomic<bool> stop_{false}, on_{false}, failed_{false}, reading_{false}; std::atomic<double> level_{-200};
    mutable std::mutex m_; std::condition_variable cv_; std::deque<float> q_; std::string err_;
    void run();
};

// The feedback test (no RF): the mic through the same processing as an over (TxChainConfig), the SSB signal turned back into audio (what a receiver
// would hear) and handed to `out` (the PC's speakers). Levels for the meters. Never touches the radio.
class MicLoop {
public:
    // `out` may be empty (no sound); `picture`, if set, gets the processed SSB I/Q (48 kHz, the over's units: the chain's ceiling = the set PEP) for the Audio tab's picture.
    MicLoop(std::unique_ptr<txc::IAudioIn> in, const tx::TxChainConfig& chain, std::function<void(const float* audio, size_t n)> out, std::shared_ptr<txo::IqTap> picture = nullptr);
    ~MicLoop();
    double in_dbfs() const { return in_db_.load(); } double out_dbfs() const { return out_db_.load(); }
    bool failed() const { return failed_.load(); } std::string error() const { std::lock_guard<std::mutex> l(m_); return err_; }
private:
    std::unique_ptr<txc::IAudioIn> in_; tx::TxChain chain_; std::function<void(const float*, size_t)> out_; std::shared_ptr<txo::IqTap> pic_;
    std::thread th_; std::atomic<bool> stop_{false}, failed_{false}; std::atomic<double> in_db_{-200}, out_db_{-200}; mutable std::mutex m_; std::string err_;
};
// What a receiver hears from our SSB I/Q (complex baseband, audio on the chosen side): its real part.
void ssb_to_audio(const std::vector<tx::cf>& iq, std::vector<float>& out);
}  // namespace fireside::vox
