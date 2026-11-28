// Logging support: a QSO record, ADIF text, and the JSON bodies for a Wavelog instance (https://docs.wavelog.org/developer/api/). Pure string work: no network, no radio.
// The app does not keep its own logbook; it hands QSOs to Wavelog and keeps a local ADIF safety copy so nothing is lost when Wavelog is unreachable.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fireside::logbook {
struct Qso {
    std::string call;                    // the station worked (required)
    double freq_hz = 0;                  // dial frequency in Hz (required)
    std::string mode;                    // the app's mode name: USB LSB CW AM FM FT8 PSK31 RTTY ...
    int64_t time_on_utc = 0;             // seconds since the epoch, UTC (required)
    int64_t time_off_utc = 0;            // 0 = same as time_on
    std::string rst_sent, rst_rcvd, name, grid, comment, station_callsign, my_grid, operator_call;
    double tx_pwr_w = 0;                 // our transmit power in W (ADIF TX_PWR); 0 = not known (left out)
};
struct AdifMode { std::string mode, submode; };

std::string band_for_hz(double hz);                    // ADIF band name ("20m"), "" when outside the amateur bands
AdifMode adif_mode(const std::string& app_mode);       // FT8 -> FT8, PSK31 -> PSK/PSK31, USB/LSB -> SSB (sideband as submode kept out: SSB is the ADIF mode)
bool valid_call(const std::string& c);                 // letters/digits and '/', 3..14 characters, at least one digit and one letter
// One FT8/FT4 message ("CQ DX K1ABC FN42", "M0OML DL2AL -07", "DL2AL M0OML R-12"): who sent it, to whom, and any grid / report in it.
struct FtxMessage { std::string from, to, grid, report; bool cq = false; bool ok = false; };
FtxMessage parse_ftx_message(const std::string& text);
std::string ftx_line_text(const std::string& decoder_line, int* snr = nullptr);   // the message part of "HHMMSS  -6  0.7  683 ~  CQ TA6CQ KN70" (and its SNR); "" when the line is not a decode
std::string adif_field(const std::string& name, const std::string& value);   // "<NAME:len>value" (empty value -> "")
std::string adif_record(const Qso& q);                 // one record ending in <eor>; throws std::invalid_argument for a bad QSO (no call, no frequency, no time, off-band)
std::string adif_header(const std::string& program, const std::string& version);   // ADIF header ending in <eoh>

// Wavelog REST API v2 (Wavelog >= 3.1, Bearer "wl2_" token; https://docs.wavelog.org/developer/api-v2/). Paths are relative to the instance's base URL.
inline const char* kWavelogQsoPath = "/index.php/api/v2/qso";
inline const char* kWavelogRadioPath = "/index.php/api/v2/radio";
inline const char* kWavelogTokenPath = "/index.php/api/v2/token";
inline const char* kWavelogStationPath = "/index.php/api/v2/station";
std::string json_escape(const std::string& s);
std::string url_encode(const std::string& s);
std::string wavelog_adif_import_body(int station_profile_id, const std::string& adif_records, bool dryrun);       // POST kWavelogQsoPath; dryrun = parse only, store nothing
std::string wavelog_radio_body(const std::string& radio_name, double freq_hz, const std::string& mode, double power_w = 0);   // POST kWavelogRadioPath (token scope radio:write); power in whole watts, left out when below 0.5 W (unknown)
std::string wavelog_all_qsos_path(int page, long long since_id = 0);   // the log, 5000 QSOs a page (GET, read-only; page from 1), only QSOs with id > since_id when > 0: for the new DXCC / grid tags
std::string wavelog_recent_path(int limit);   // the newest QSOs of the account (GET, read-only), 1..50
std::string wavelog_lookup_path(const std::string& callsign, const std::string& band, const std::string& mode, bool callbook = false);   // callbook=true also asks Wavelog's configured callbook (name, QTH): one extra upstream request
std::string wavelog_history_path(const std::string& callsign);                                                    // GET: the newest QSO with that callsign (limit=1); meta.total is how many there are   // GET, full path with query string (callsign upper-cased and escaped)
std::string wavelog_normalise_base_url(const std::string& url);                                                   // trims blanks and trailing '/', requires http(s)://, "" when unusable

// Local ADIF safety copy (append only; the header is written when the file is new). Throws std::runtime_error on I/O failure.
void append_adif_file(const std::string& path, const Qso& q, const std::string& program, const std::string& version);
// The same QSO worked again a few minutes later (they missed our RR73 and called again): its record `old_rec` (exactly as adif_record() wrote it) is
// replaced by `q`'s, in place. False (file untouched) if the old record is not in the file exactly once. Throws std::runtime_error on I/O failure.
bool replace_adif_record(const std::string& path, const std::string& old_rec, const Qso& q);
}  // namespace fireside::logbook
