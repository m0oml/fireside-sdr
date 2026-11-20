// Receive-only text decoders working on demodulated AUDIO (48 kHz mono float): Morse (CW), PSK31 and RTTY. Nothing here can transmit or touch the radio.
// A decoder is fed audio in any chunk size and appends decoded characters to a string. Test signal generators live next to them (decode_synth) so everything is tested offline.
#pragma once
#include <functional>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fireside::decode {
class TextDecoder {
public:
    virtual ~TextDecoder() = default;
    virtual void process(const float* audio, size_t n, std::string& out) = 0;   // appends decoded text
    virtual std::string status() const = 0;                                      // one line for the UI, e.g. "18 WPM, 702 Hz"
    virtual void reset() = 0;
    virtual void finish(std::string& /*out*/) {}
    virtual void peek(std::string& /*out*/) {}                                    // receive keeps running, we transmit next: decode what can be now, change nothing                                  // the audio stops now (receive paused for a transmission): decode what can be, wait, append it
};

// ---------------------------------------------------------------- Morse
struct CwOptions {
    double sample_rate = 48000, pitch_hz = 700;
    bool track_pitch = true;                                  // follow a tone that drifts or sits off the nominal pitch (+-150 Hz)
    double bandwidth_hz = 170;                                // detection bandwidth to start with (narrower = more sensitive, smears fast keying)
    bool adaptive_bandwidth = true;                           // once the speed is known, narrow it to about 4 / dit (60 Hz minimum)
    double min_snr_ratio = 4.5;                               // peak/floor of the envelope that counts as "a signal is present"
    double min_separation = 1.85, hold_separation = 1.5;      // and it must be keyed: on and off levels this far apart (in their spreads) over 3 s; 0 = off. Stops noise and QRM printing E/T/I
    double min_separation_short = 2.6;                        // or over the last 1.2 s alone (a station that has only just started)
};
class CwDecoder : public TextDecoder {
public:
    explicit CwDecoder(CwOptions o = {});
    ~CwDecoder() override;
    void process(const float* audio, size_t n, std::string& out) override;
    std::string status() const override;
    void reset() override;
    double wpm() const;                                       // current speed estimate (0 until it has seen enough keying)
    double pitch() const;
    bool signal_present() const;
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
const char* morse_for(char c);                                // ".-" for 'A'; nullptr if unknown
char char_for_morse(const std::string& dotdash);              // 0 if unknown

// ---------------------------------------------------------------- PSK31 (BPSK, 31.25 baud, Varicode)
struct PskOptions {
    double sample_rate = 48000, center_hz = 1000; bool afc = true;
    double min_snr = 2.2;                                     // signal magnitude / noise magnitude measured 250 Hz either side of the channel: ~1.2 in noise alone
    // and it must look like PSK31 (keyed CW, FT8 tones and band noise pass the SNR test and printed garbage): phase steps of 0 / 180 degrees (smoothed cos of twice the step:
    // ~0.9 for PSK31, ~0.3 or below on real CW / FT8 / noise channels) and symbol timing that matters (best / opposite timing: PSK31 ~2.4, a steady tone ~1.1). 0 = off.
    double min_phase_quality = 0.4, min_timing_contrast = 1.1;
};
class Psk31Decoder : public TextDecoder {
public:
    explicit Psk31Decoder(PskOptions o = {});
    ~Psk31Decoder() override;
    void process(const float* audio, size_t n, std::string& out) override;
    std::string status() const override;
    void reset() override;
    double center() const;                                    // follows small frequency errors when afc is on
    bool locked() const;
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
const char* varicode_for(unsigned char c);                    // "1011" for 'a', nullptr for >= 128

// ---------------------------------------------------------------- RTTY (ITA2 / Baudot, async 5N1.5)
struct RttyOptions { double sample_rate = 48000, mark_hz = 2125, shift_hz = 170, baud = 45.45; bool reverse = false, usos = true; double min_ratio = 3.0; };   // space = mark - shift: on the air the mark is the HIGHER RF tone, so in USB the higher audio tone; reverse swaps
class RttyDecoder : public TextDecoder {
public:
    explicit RttyDecoder(RttyOptions o = {});
    ~RttyDecoder() override;
    void process(const float* audio, size_t n, std::string& out) override;
    std::string status() const override;
    void reset() override;
    bool signal_present() const;
    size_t framing_errors() const;
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
char ita2_char(int code, bool figures);                       // 0 if the code prints nothing (shift codes, NUL, BEL...)
int ita2_code(char c, bool* figures);                         // -1 if not representable; *figures says which shift it needs (letters and space work in either)

// ---------------------------------------------------------------- FT8 (15 s slots, 8-GFSK, LDPC): wraps the MIT-licensed ft8_lib (third_party/ft8_lib)
struct Ft8Message { std::string time; int snr = 0; double dt = 0, freq = 0; std::string text; int ap = 0; };   // ap: 0 = plain decode, 1 = a-priori "CQ", 2 = a-priori your call (printed " a1" / " a2" as WSJT-X)   // snr is a rough estimate from the sync score (WSJT-X's differs); dt = seconds from the slot start; freq in Hz (audio)
struct Ft8Options {
    double sample_rate = 48000;
    bool wall_clock = false;                                  // live audio: slots follow UTC (:00 :15 :30 :45). False: the audio starts at start_offset_s into a slot (0 = a slot recording from the beginning)
    double start_offset_s = 0;
    bool ft4 = false;                                         // FT4: 7.5 s slots, 4-GFSK, 20.8 Hz tone spacing (same message format and library)
    std::string my_call;                                      // your callsign: enables a-priori decoding of weak messages addressed to you (WSJT-X "a2"); empty = off
    std::function<void(const std::string&)> late_lines;       // finish(): the slot's full decode (lines the quick pass missed) arrives here later, on another thread
};
class Ft8Decoder : public TextDecoder {
public:
    explicit Ft8Decoder(Ft8Options o = {});
    ~Ft8Decoder() override;                                   // waits for a decode in progress
    void process(const float* audio, size_t n, std::string& out) override;   // a slot is decoded on a background thread right after it ends; its lines arrive in a later call
    // Receive is about to pause (we transmit next): waits for a decode in progress, and decodes the current slot now if its signals are all in
    // (heard past the nominal end + 0.9 s: FT8 14.04 s, FT4 6.44 s), the rest silent: a quick pass now (one pass, no deep stage,
    // ~0.3 s) so an answer is known before our slot, then the full decode in the background, its extra lines through Ft8Options::late_lines.
    void finish(std::string& out) override;
    // Duplex (receive does not pause for our over): a quick pass over the slot so far (as finish), leaving the slot to fill on; its full decode
    // at the end then adds only the lines the quick pass did not already give.
    void peek(std::string& out) override;
    std::string status() const override;
    void reset() override;
    bool busy() const;                                        // a slot is being decoded on the background thread (its lines arrive in the next process() call after it finishes)
    // One slot of 12 kHz audio, synchronously (slot recordings, tests, tools). Messages sorted by frequency.
    static std::vector<Ft8Message> decode_slot(const float* audio12k, size_t n, const std::string& time_label = "", bool ft4 = false, const std::string& my_call = "");
    static std::string format(const Ft8Message& m);           // "HHMMSS  -6  0.7  683 ~  CQ TA6CQ KN70"
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
// A 15 s slot of FT8 audio carrying `message` at `freq_hz` (tone 0), starting `dt_s` into the slot. Returns false text via empty vector if the message cannot be encoded.
// Unpacks the 77-bit message types ft8_lib cannot (ARRL RTTY Roundup, Field Day, DXpedition, EU VHF); false for any other type or invalid contents. A hashed callsign prints as
// "<...>" unless one of `heard_messages` (encoded first, which hashes their callsigns) contained it. For tests and reuse.
bool unpack_extra_message(const uint8_t payload[10], std::string& text, const std::vector<std::string>& heard_messages = {});
std::vector<std::complex<float>> synth_ft_iq(const std::string& message, double f0, double fs, bool ft4, double dt_s);   // analytic (USB) FT8/FT4 at fs, a slot long, envelope 1; empty if not encodable
std::vector<float> synth_ft8(const std::string& message, double freq_hz, double fs = 48000, double amplitude = 0.3, double dt_s = 0.5);
// The same for FT4: a 7.5 s slot.
std::vector<float> synth_ft4(const std::string& message, double freq_hz, double fs = 48000, double amplitude = 0.3, double dt_s = 0.3);

// ---------------------------------------------------------------- test signal generators (also usable for demos)
// Keyed CW tone: raised-cosine edges (default 5 ms). Returns audio at `fs`. `wpm` is the PARIS speed; farnsworth_wpm (>0, < wpm) stretches the gaps.
// The keying envelope alone (0..1 at fs): useful to build complex baseband CW (envelope + 0j = a carrier exactly on the dial).
std::vector<float> morse_envelope(const std::string& text, double wpm, double fs = 48000, double edge_ms = 5.0, double lead_s = 0.5, double farnsworth_wpm = 0);
std::vector<float> synth_morse(const std::string& text, double wpm, double pitch_hz, double fs = 48000, double amplitude = 0.5, double edge_ms = 5.0, double lead_s = 0.5, double farnsworth_wpm = 0);
std::vector<float> synth_psk31(const std::string& text, double center_hz, double fs = 48000, double amplitude = 0.5, double lead_s = 1.0);   // idle reversals first, then Varicode text, then idle
std::vector<float> synth_rtty(const std::string& text, double mark_hz = 2125, double shift_hz = 170, double baud = 45.45, double fs = 48000, double amplitude = 0.5, double lead_s = 0.5, bool reverse = false);
// Adds white Gaussian noise of the given RMS (deterministic for a seed).
void add_noise(std::vector<float>& x, double rms, uint32_t seed = 1);
}  // namespace fireside::decode
