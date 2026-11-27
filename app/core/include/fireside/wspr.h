// WSPR (K1JT's Weak Signal Propagation Reporter): the message encoder, the signal, wsprd's output and the dial frequencies.
// Pure code: no process, no radio, no Qt. Decoding is done by WSJT-X's wsprd (wspr_decode.h runs it).
// Type-1 message: callsign (28 bits) + 4-character grid and power in dBm (22 bits) = 50 bits, convolutional code K=32 r=1/2
// (polynomials 0xF2D05351, 0xE4613C47) with 31 tail bits -> 162 bits, bit-reversal interleave, symbol = sync bit + 2 x data bit.
// 4-FSK, tone spacing 12000/8192 Hz (~1.465 Hz), 8192/12000 s per symbol (~0.683 s), 162 symbols = 110.6 s, starting 1 s after an even UTC minute.
// The sync vector was checked byte for byte against the one inside /usr/bin/wsprd (WSJT-X 3.3.0) on 2026-11-26.
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fireside::wspr {
inline constexpr int kSymbols = 162;
inline constexpr double kToneSpacingHz = 12000.0 / 8192.0;   // ~1.4648 Hz
inline constexpr double kSymbolS = 8192.0 / 12000.0;          // ~0.6827 s
inline constexpr double kSignalS = kSymbols * kSymbolS;       // ~110.59 s
inline constexpr double kSlotS = 120.0, kStartS = 1.0;        // two-minute slots on even UTC minutes; the signal starts 1 s in
inline constexpr double kAudioLoHz = 1400, kAudioHiHz = 1600;  // the WSPR sub-band, 200 Hz wide around 1500 Hz above the dial

extern const uint8_t kSync[kSymbols];

using Symbols = std::array<uint8_t, kSymbols>;
// Type-1 message only: a plain callsign (no prefix / suffix "/"), a 4-character Maidenhead grid ("IO91"), power in dBm that is a legal WSPR value
// (0..60, ending in 0, 3 or 7). Throws std::invalid_argument with the reason otherwise. Case does not matter.
Symbols encode(const std::string& call, const std::string& grid4, int dbm);
// Can `call` go in a type-1 message? (1-6 characters, a digit as the 2nd or 3rd character, letters / digits only.)
bool plain_call(const std::string& call);
// The legal dBm nearest to `watts` (1 mW .. 1 MW): 5 W -> 37, 1 W -> 30, 20 W -> 43.
int dbm_for_watts(double watts);
double watts_for_dbm(int dbm);

// The signal: continuous-phase 4-FSK, tone 0 at audio_hz above the dial (USB), envelope 1 with short raised-cosine ramps at the ends.
// iq: analytic (USB) at fs, `lead_s` of silence first (the start offset into the slot), then the 110.6 s signal, padded with silence to `total_s` (0 = no padding).
std::vector<std::complex<float>> synth_iq(const Symbols& s, double audio_hz, double fs, double lead_s = kStartS, double total_s = kSlotS);
std::vector<float> synth_audio(const Symbols& s, double audio_hz, double fs, double amplitude, double lead_s = kStartS, double total_s = kSlotS);

// A WAV file image: 16-bit mono PCM at `rate` (wsprd wants 12000 Hz). Samples are clipped to +-1.
std::vector<uint8_t> wav16(const std::vector<float>& x, int rate = 12000);

// One decode as wsprd prints it: "2258 -21  0.3  14.097069  0  K1ABC FN42 37" (time, SNR, dt, MHz, drift, then the message).
// The message may also be "K1ABC 37" (type 2) or "<K1ABC> FN42KM 37" (type 3): call and grid are taken as given.
struct Spot { std::string hhmm; int snr = 0; double dt = 0, mhz = 0; int drift = 0; std::string call, grid; int dbm = 0; };
std::optional<Spot> parse_wsprd_line(const std::string& line);
std::string format(const Spot& s);                  // our decoder line: "2258  -21  0.3  14.097069   0  K1ABC FN42 37"

// The WSPR dial frequency (USB) for a frequency inside an amateur band, 0 if the band has none. 60 m: 5.2872 MHz (signals 5288.6-5288.8 kHz).
double dial_hz_for(double hz);

// ---------------------------------------------------------------- wsprnet.org uploads (as WSJT-X's Network/wsprnet.cpp, read 2026-11-26)
// One HTTP POST per spot to kWsprnetUrl, body application/x-www-form-urlencoded; the server answers "... spot(s) added" on success.
//   function=wspr: rcall rgrid rqrg (dial MHz, 6 decimals) date (yymmdd) time (hhmm) sig dt drift tqrg (signal MHz) tcall tgrid dbm version mode (2 = WSPR-2)
//   function=wsprstat (a slot with no spots): rcall rgrid rqrg tpct (transmit %) tqrg (our transmit MHz) dbm (our power) version mode
inline constexpr const char* kWsprnetUrl = "http://wsprnet.org/post/";
std::string form_encode(const std::string& s);        // application/x-www-form-urlencoded value
std::string wsprnet_spot_body(const Spot& s, const std::string& date, const std::string& rcall, const std::string& rgrid, double dial_mhz, const std::string& version);
std::string wsprnet_stat_body(const std::string& rcall, const std::string& rgrid, double dial_mhz, int tx_percent, double tx_mhz, int dbm, const std::string& version);
}  // namespace fireside::wspr
