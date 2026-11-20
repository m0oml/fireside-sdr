// Offline renderers for the transmit DSP: text, audio or test tones -> baseband I/Q buffers and WAV files. FILE OUTPUT ONLY: nothing here
// can reach the radio (no link, session, safety, pal, startup, opcodes or MIDI; a CTest enforces that). Every renderer ends with an I/Q
// limiter at -1 dBFS, so no output ever exceeds that, whatever the input. Output convention: 48 kHz, I = channel 0, Q = channel 1, dial at 0 Hz.
#pragma once
#include <string>
#include <vector>
#include "fireside/tx_dsp.h"

namespace fireside::tx {
inline constexpr double kRenderRate = 48000;
inline constexpr double kRenderCeiling = 0.8912509;                     // -1 dBFS

// Real audio at kRenderRate through the voice transmit chain (mic chain optional) and SSB. gain_db is the mic gain (-40..40).
std::vector<cf> render_ssb(const std::vector<float>& audio, Sideband sb, bool mic_chain = true, double gain_db = 0);
// A whole FT8 (15 s) or FT4 (7.5 s) slot carrying `text` at audio frequency freq_hz (200..3000, tone 0). level_db is the amplitude in dBFS (<= -1).
// Throws std::runtime_error if the message cannot be encoded.
std::vector<cf> render_ft(const std::string& text, double freq_hz, bool ft4, Sideband sb, double level_db = -6);
// True if `text` can be sent as an FT8/FT4 message (cheap: encodes only, no SSB filtering). Same encoder as render_ft.
bool ft_message_ok(const std::string& text);
// CW text at wpm (5..60) with shaped edges; carrier at offset_hz from the dial (0 = on the dial).
std::vector<cf> render_cw(const std::string& text, double wpm = 20, double edge_ms = 5, double level_db = -6, double offset_hz = 0);
// PSK31 (freq_hz 200..3000) / RTTY (mark_hz 200..3000, 170 Hz shift, 45.45 baud) text, 2 s / 0.5 s lead. Text up to 500 characters. Throws std::invalid_argument.
std::vector<cf> render_psk31(const std::string& text, double freq_hz, Sideband sb, double level_db = -6);
std::vector<cf> render_rtty(const std::string& text, double mark_hz, Sideband sb, double level_db = -6);
// Paddle input (a recorded list of paddle changes) through the iambic keyer and the CW shaper. Each event sets both paddles from time t_s (seconds from the
// start of the recording, non-decreasing, at most 600 s, at most 100000 events); a 0.5 s lead and a 1.5 s tail are added. Throws std::invalid_argument.
struct PaddleEvent { double t_s; bool dit, dah; };
std::vector<cf> render_paddles(const std::vector<PaddleEvent>& events, double wpm = 20, IambicMode mode = IambicMode::B, bool reverse = false, double weight = 50,
                               double edge_ms = 5, double level_db = -6, double offset_hz = 0);
// The standard two-tone test (peak_db = peak of the sum, <= -1).
std::vector<cf> render_two_tone(double seconds, double peak_db, Sideband sb);
// Reads a WAV (mono or stereo; channel 0 is used) and resamples it to kRenderRate if needed. Throws std::runtime_error.
std::vector<float> read_audio_wav(const std::string& path);
void write_iq_wav(const std::string& path, const std::vector<cf>& iq, double fs = kRenderRate);
double peak_magnitude(const std::vector<cf>& iq);
}  // namespace fireside::tx
