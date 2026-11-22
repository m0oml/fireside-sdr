// Transmit control shared by the GUI and firesidekey: arm (EEPROM check + PA bias), a drive ramp that measures the band's drive calibration, an ATU tune, and
// putting saved ATU relays back without RF. Everything keys through a TxGate; samples go out only through txo::ToneSource / Stream (ch0/1, hard clamp).
// Proven on the radio 2026-11-19 (analysis/transmit-stage2.md section 8). Qt-free; one operation at a time; abort() makes a running one unkey at once.
// Lifetime: owned by RadioSession while the radio is started. disarm() aborts a running operation and WAITS for it, so the Flex is never used afterwards.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "fireside/atu.h"
#include "fireside/atu_memory.h"
#include "fireside/playback.h"
#include "fireside/tx_dsp.h"
#include "fireside/tx_gate.h"
#include "fireside/tx_setup.h"

namespace fireside::txc {
using PcmFactory = std::function<std::unique_ptr<IPcmOut>(double rate)>;
// Gate limits for FT8/FT4, voice and other programs: forward > fwd_limit_w for 0.15 s, or > hard_fwd_limit_w at once, unkeys.
// 1.5 x / 3 x the set power, capped at 1.1 x / 1.3 x the PA rating (txs::kMaxWatts).
inline constexpr double kFwdCeilingW = 1.1 * txs::kMaxWatts, kHardFwdCeilingW = 1.3 * txs::kMaxWatts;
double fwd_limit_w(double power_w);
double hard_fwd_limit_w(double power_w);
struct Progress { double t = 0, drive = 0; txm::Reading rd; };
using ProgressFn = std::function<void(const Progress&)>;
// mirror_q: the radio needs the TX I/Q mirrored, as for receive (verified on the air 2026-11-19: with it the test tone was at dial + 1 kHz on a WebSDR,
// without it nothing on USB).
struct Options { double rate = 48000, tone_hz = 1000; bool mirror_q = true; int tx_ant = 1; };   // tx_ant: ANT1..ANT3 (only ANT1 goes through the ATU)

// Drive calibration (W per drive^2) per band belongs to ONE radio: there is no built-in default (a value measured on one FLEX-5000 is not trusted for
// another). Measure it with ramp(); keep it in a file per radio. Text: "band=20m k=11150" per line.
std::map<int, double> parse_drive_text(const std::string& text);
std::string drive_text(const std::map<int, double>& k);

struct MeterCompare { double t = 0; int adc_fwd = 0, adc_rev = 0; double fwd_w = 0, rev_w = 0, swr = 0; std::optional<double> atu_fwd_w, atu_rev_w, atu_swr; };
struct RampResult { bool reached = false, unkey_failed = false; std::string stop; double max_fwd_w = 0, k = 0, seconds = 0;
                    std::vector<MeterCompare> compare; };   // while holding: our detector (raw counts, watts, SWR) beside the tuner's own meter, every ~0.5 s
struct TuneResult {
    std::optional<atu::Atu::TuneOutcome> outcome; std::string why_not, stop; double max_fwd_w = 0, last_fwd_w = 0; std::optional<double> last_swr;
    std::optional<atu::Setting> saved;                            // what a pass stored for the band
    bool unkey_failed = false;                                    // SET_MOX 0 never got through: the radio may still be keyed
    bool estimated_drive = false;                                 // no measured level on this band: the EEPROM table's drive was used
    std::string pass_rejected;                                    // the tuner said "pass" but it is not trusted (why): the outcome is shown as stopped, nothing saved
};

// Voice: 48 kHz mono audio in (the PC's capture, or the radio's mic input ch7 from the duplex stream).
class IAudioIn {
public:
    virtual ~IAudioIn() = default;
    virtual size_t read(float* x, size_t n, double timeout_s) = 0;   // up to n samples (48 kHz), 0 if none within the timeout; throws IoError when gone
    virtual std::string describe() const = 0;
};
struct VoiceOptions {
    Options o; bool lsb = false;                  // USB (the default above 10 MHz) or LSB
    double peak_w = 10;                           // peak envelope power (the processed audio's ceiling gives this), 1..100 W (txs::kMaxWatts)
    double max_key_s = 180;                       // the time-out timer
    tx::TxChainConfig chain = tx::voice_chain_defaults();   // the whole audio processing (EQ, expander, leveler, compressor, clipper, limiters, SSB filter)
};
struct VoiceResult { bool keyed = false, unkey_failed = false; std::string why_not, stop; double max_fwd_w = 0, seconds = 0, drive = 0, limited_to_w = 0; uint64_t underruns = 0; };

struct SendResult {
    bool sent = false, unkey_failed = false, estimated_drive = false; std::string why_not, stop; double limited_to_w = 0;   // > 0: sent at the band's drive ceiling (this many W), below the power asked
    double slot_utc = 0, late_s = 0, max_fwd_w = 0, mean_fwd_w = 0, drive = 0, k = 0; std::optional<double> swr;
    double unkeyed_utc = 0;              // when the gate dropped MOX (the system clock, for timing what follows)
};

class TxControl {
public:
    TxControl(Flex& fx, PcmFactory pcm, atu::Clock clock = atu::real_clock());
    ~TxControl();
    // The EEPROM image read now must equal the backup byte for byte; computes the calibration and writes the PA bias pots. Throws std::runtime_error
    // (with the reason) and stays disarmed on any refusal.
    void arm(const std::vector<uint8_t>& image_now, const std::vector<uint8_t>& backup, const LogFn& log = {});
    // The same, reading the EEPROM from the radio itself first (read-only, ~16 s).
    void arm_from_radio(const std::vector<uint8_t>& backup, const LogFn& log = {});
    // First-time set-up: reads the EEPROM TWICE (read-only), requires the two reads to be identical and a usable transmit calibration, and saves it as the
    // reference backup in `dir` (never overwriting). Nothing is written to the radio. Throws std::runtime_error with the reason (nothing is saved then).
    struct BackupSaved { std::vector<std::string> paths; std::vector<uint8_t> image; };
    BackupSaved backup_eeprom(const std::string& dir, const LogFn& log = {});
    void disarm();                                                // aborts and waits for a running operation; arm() again to transmit
    void retire();                                                // disarm for good (the session is going away): every call refuses from now on
    bool retired() const { return retired_.load(); }
    bool armed() const { return armed_.load(); }
    bool busy() const { return busy_.load(); }
    bool keyed() const { return keyed_.load(); }
    void abort() { abort_.store(true); }
    const txs::TxCal& cal() const { return cal_; }
    // Drive calibration (W per drive^2) per band: 0 until measured (ramp) or loaded (set_k / set_all_k).
    double k_for(int band) const;
    double test_cap(int band) const;                              // tunes, drive ramps and test tones: 30 W by the measured level (the table's 30 W without one)
    double drive_cap(int band) const;                             // the drive ceiling on this band (110 W by the measured level, else the table's 100 W; <= kAbsoluteMaxDrive)
    const std::map<int, double>& all_k() const { return k_; }
    void set_all_k(const std::map<int, double>& k) { for (auto& [b, v] : k) set_k(b, v); }
    void set_k(int band, double k);
    atu::Memory& memory() { return mem_; }

    // 2 W (<= 15) single-tone ramp: from 1/10 of the table's 1 W drive, +2 dB / 200 ms, settles, holds 0.5 s at the target; stops on: no power at the 1 W
    // drive point (< 5 mW), > 1.5 x target, SWR > 3, meter error, abort(), the key-down limit. Clamped at the table's 100 W drive. A good ramp stores the band's k.
    // Key-down limit: 6 s (10 s above 5 W) plus hold_s. hold_s: how long to hold the target (0.5 for a measurement; up to 10 s as a steady test tone, e.g. to find it on another receiver).
    RampResult ramp(double freq_mhz, double target_w, const Options& o, const ProgressFn& p = {}, const LogFn& log = {}, double hold_s = 0.5);
    // ATU full tune at `power_w` (1..15) from the band's k (or, not measured yet, the EEPROM table's drive, which errs low): tuner init, settle, relays read back (refuses without), CWU, key, carrier, threshold + full tune.
    // During the search: forward reading <= 30 W, reflected <= 20 W, 15 s (no SWR stop, as PowerSDR). A pass stores the relays in memory().
    TuneResult tune(double freq_mhz, double power_w, double threshold, const Options& o, const ProgressFn& p = {}, const LogFn& log = {});
    // One FT8 / FT4 transmission of `text` at audio_hz (200..3000) above the dial, USB, in the next slot of `parity` (or the current one if at most 2 s
    // late), at power_w (1..kMaxWatts = 100; above the band's drive ceiling it is sent at the ceiling). It starts 3 dB low (soft start). Closed-loop power: the first 3 s of the signal trim the drive from the forward meter (FT8 / FT4 are constant envelope),
    // and the band's drive level is updated afterwards. Gate: one slot long, forward <= fwd_limit_w (0.15 s grace) and <= hard_fwd_limit_w (at once), SWR <= 3, abort() unkeys. Blocks until the over ends.
    SendResult send_ft(double dial_mhz, const std::string& text, double audio_hz, bool ft4, tx::Parity parity, double power_w, const Options& o,
                       const ProgressFn& p = {}, const LogFn& log = {});
    // The same in two parts, so the signal can be rendered while still receiving and the radio part started just before the slot (the GUI).
    struct FtPrepared { std::vector<std::complex<float>> buf; double rate = 0; bool ft4 = false; std::string text; double audio_hz = 0; };
    static FtPrepared prepare_ft(const std::string& text, double audio_hz, bool ft4, double rate);   // throws std::runtime_error / invalid_argument
    SendResult send_prepared(double dial_mhz, const FtPrepared& f, tx::Parity parity, double power_w, const Options& o,
                             const ProgressFn& p = {}, const LogFn& log = {}, double min_lead_s = 1.5);
    void set_utc_source(std::function<double()> utc) { utc_ = std::move(utc); }   // tests; default: the system clock
    // Called with true right before every key-down and with false right after the unkey (the session moves the shared RX1/TX DDS onto
    // the dial for the over and back to the receive IF shift afterwards, as PowerSDR does). Must not throw.
    void set_key_hook(std::function<void(bool keyed)> h) { key_hook_ = std::move(h); }
    // A safety stop is not a normal end of an over: a power / SWR / reflected limit, the key-down time limit, a meter, stream or audio failure, no power at the
    // 1 W point, the radio refusing the key, or an unkey that did not get through. The hook is called (on the worker thread) with the reason right after
    // any operation that ended that way, so the GUI can show it where it cannot be missed. Normal ends (finished, PTT released, Stop) never call it.
    static bool is_safety_stop(const std::string& reason);
    void set_trip_hook(std::function<void(const std::string& reason)> h) { trip_hook_ = std::move(h); }
    // Voice over: keyed while ptt() is true (and abort() not called), at most max_key_s. Audio from `in` -> TxChain -> SSB I/Q -> the live
    // ToneSource (mirror, image correction, HARD clamp) at the stream rate. Drive: the chain's I/Q ceiling gives peak_w by the band's
    // measured level (refused without one); it is only ever trimmed DOWN, when the forward meter reads peaks above 1.1 x peak_w.
    // Gate: forward <= fwd_limit_w(peak_w) (0.15 s grace), <= hard_fwd_limit_w at once, SWR <= 3, reflected <= 0.5 x peak_w (min 2 W), the time-out. The DDS key hook as for every over.
    VoiceResult voice(double dial_mhz, IAudioIn& in, const std::function<bool()>& ptt, const VoiceOptions& v, const ProgressFn& p = {}, const LogFn& log = {});
    // No RF: put a band's stored relays back. false if nothing is stored or the tuner does not report them.
    bool restore_relays(int band, const LogFn& log = {});
    // Receive-level check with the radio's built-in signal generator (siggen.h; PowerSDR CalibrateLevel for the FLEX-5000). Holds the transmit lock for
    // the whole check, so nothing can key meanwhile; it never keys itself. The generator sits at dial + tone_hz (inside the receive passband).
    // read(): {in-passband dBFS, strongest spectrum peak's offset from the dial in Hz}, from the receiver. preamp(on): switches the receive preamp and
    // returns false if it could not (optional: then the preamp is measured too, as PowerSDR does). The preamp ends as preamp_was_on.
    // The generator is switched off again on every path (the error, if any, in restore_error: then ok is false).
    struct GenCheck { bool ok = false, preamp_measured = false; std::string why, restore_error; double off_dbfs = -200, on_dbfs = -200, preamp_dbfs = -200, peak_hz = 0; };
    GenCheck sig_gen_check(double dial_mhz, double tone_hz, const std::function<std::pair<double, double>()>& read, const std::function<bool(bool)>& preamp,
                           bool preamp_was_on, const LogFn& log = {});
    std::optional<atu::Status> atu_report();                      // no RF: the tuner's relays

private:
    Flex& fx_; PcmFactory pcm_; atu::Clock clk_; std::function<double()> utc_; txs::TxCal cal_; atu::Atu atu_; atu::Memory mem_; std::map<int, double> k_;
    std::atomic<bool> armed_{false}, busy_{false}, keyed_{false}, abort_{false}, retired_{false}; std::mutex op_; std::function<void(bool)> key_hook_; std::function<void(const std::string&)> trip_hook_;
    void note_stop(const std::string& s) { if (trip_hook_ && is_safety_stop(s)) { try { trip_hook_(s); } catch (...) {} } }
    struct KeyHook { const std::function<void(bool)>& h; explicit KeyHook(const std::function<void(bool)>& f) : h(f) { if (h) h(true); } ~KeyHook() { if (h) try { h(false); } catch (...) {} } };   // declared before the TxGate: its end runs after the unkey
    void prepare(double freq_mhz, uint32_t dsp_mode, int tx_ant); // TX antenna (1..3, else std::invalid_argument before any write), mode, DDS, filter, offset, carrier null
    struct Busy { TxControl& c; explicit Busy(TxControl& x) : c(x) { c.busy_ = true; c.abort_ = false; } ~Busy() { c.busy_ = false; c.keyed_ = false; } };
};
}  // namespace fireside::txc
