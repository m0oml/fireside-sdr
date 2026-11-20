#pragma once
// PSK Reporter (pskreporter.info, Philip Gladstone N1DQ): where this station is being heard.
// Live reports come from the PSK Reporter MQTT feed (mqtt.pskreporter.info, run with N1DQ's permission; facts from its page, 2026-11-19):
//   topic  pskr/filter/v2/{band}/{mode}/{tx_call}/{rx_call}/{tx_grid}/{rx_grid}/{tx_dxcc}/{rx_dxcc}, "+" one segment, "#" the rest
//   JSON   {"sq": sequence, "f": Hz, "md": "FT8", "rp": SNR dB, "t": epoch s, "t_tx": epoch s (may be absent), "sc"/"sl": sender call / locator,
//           "rc"/"rl": receiver call / locator, "sa"/"ra": ADIF DXCC numbers, "b": "20m"}
// Only what is needed here: an MQTT 3.1.1 client codec (CONNECT, SUBSCRIBE QoS 0, PINGREQ, DISCONNECT out; CONNACK, SUBACK, PUBLISH QoS 0,
// PINGRESP in), the report parser and a "heard by" summary. Qt-free; the GUI owns the socket.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fireside::pskr {
inline constexpr const char* kBroker = "mqtt.pskreporter.info";
inline constexpr int kPortTls = 1884, kPortPlain = 1883;

// "pskr/filter/v2/+/+/M0OML/#": every report of `call` as the transmitter. Empty for a call that cannot be a topic segment
// ("/" is the level separator and the feed does not document an encoding for portable calls; "+", "#" and spaces are not callsigns).
std::string sender_topic(const std::string& call);

namespace mqtt {
std::vector<uint8_t> connect(const std::string& client_id, uint16_t keepalive_s);   // clean session, no credentials
std::vector<uint8_t> subscribe(uint16_t packet_id, const std::string& topic);        // one topic, QoS 0
std::vector<uint8_t> pingreq();
std::vector<uint8_t> disconnect();
enum class Type { Connack = 2, Publish = 3, Suback = 9, Pingresp = 13, Other = 0 };
struct Packet { Type type = Type::Other; uint8_t flags = 0; std::string topic; std::string payload; int connack_code = -1; int suback_code = -1; };
// Streaming decoder: feed bytes as they arrive, take complete packets. A malformed length marks the stream broken (reconnect).
class Decoder {
public:
    void feed(const uint8_t* p, size_t n);
    std::optional<Packet> next();
    bool broken() const { return broken_; }
private:
    std::vector<uint8_t> buf_; bool broken_ = false;
};
}  // namespace mqtt

struct Spot {
    uint64_t seq = 0; double freq_hz = 0; std::string mode, band; int snr = 0; bool has_snr = false; int64_t t = 0;
    std::string sender, sender_grid, receiver, receiver_grid; int sender_dxcc = -1, receiver_dxcc = -1;
};
// Parses one feed payload (a flat JSON object). nullopt if it is not an object or has no sender / receiver.
std::optional<Spot> parse_spot(const std::string& json);

// One receiver that heard us, over a window of time.
struct HeardBy {
    std::string receiver, grid, band, mode, country; int best_snr = -99, last_snr = -99, count = 0; int64_t first_t = 0, last_t = 0;   // country: cty.dat name from the callsign
    double km = -1, bearing = -1;                     // from my grid to theirs; -1 when either is unknown
};
// Collects spots of my signal (other senders are ignored), keyed by receiver and band (heard on 20 m and 40 m = two rows).
class Heard {
public:
    explicit Heard(std::string my_call = "", std::string my_grid = "");
    void set_station(std::string my_call, std::string my_grid);
    bool add(const Spot& s);                           // false if not my call or a sequence number already seen
    std::vector<HeardBy> since(int64_t t0, const std::string& band = "") const;   // newest first; band "" = every band
    struct Summary { int stations = 0, spots = 0, countries = 0; double max_km = -1; std::string furthest; int best_snr = -99; std::string best; std::map<std::string, int> by_band; };
    Summary summary(int64_t t0, const std::string& band = "") const;
    size_t size() const { return rows_.size(); }
    void prune(int64_t before);                        // forget receivers last heard before `before`
private:
    std::string call_, grid_; std::map<std::string, HeardBy> rows_; std::map<uint64_t, int64_t> seen_;
};

// ---------------------------------------------------------------- uploading my decodes (pskreporter.info/pskdev.html, read 2026-11-19)
// UDP to report.pskreporter.info:4739 (test listener: pskreporter.info:14739, results at /cgi-bin/psk-analysis.pl), IPFIX (RFC 5101):
//   header 00 0A len(2) export_time(4) sequence(4) observation_id(4), big-endian; sequence counts reports sent before this datagram
//   enterprise 30351 (0x768F): senderCallsign 1, receiverCallsign 2, senderLocator 3, receiverLocator 4, frequency 5 (Hz), sNR 6,
//   decoderSoftware 8, antennaInformation 9, mode 10 (ADIF), informationSource 11 (1 = automatically extracted); flowStartSeconds = IE 150
//   variable-length strings: 1-byte length + bytes; every set padded to a multiple of 4
// Rules kept: one datagram per 5 minutes at most (sooner only when one is full), not on clock boundaries (random 0..60 s), the same source
// port throughout, templates in the first three datagrams then hourly, each station once per band per hour.
inline constexpr const char* kUploadHost = "report.pskreporter.info";
inline constexpr int kUploadPort = 4739;
inline constexpr const char* kTestHost = "pskreporter.info";
inline constexpr int kTestPort = 14739;
inline constexpr uint16_t kReceiverTemplate = 0x6A42, kSenderTemplate = 0x6A43;   // our own ids (WSJT-X has its own): no clash on one host

struct Report { std::string call, grid, mode; uint32_t freq_hz = 0; int snr = 0; int64_t t = 0; };
// One decoder line ("HHMMSS  -6  0.7  683 ~  CQ TA6CQ KN70") heard with the dial at dial_hz -> the report of its SENDER (the second call,
// or the CQ caller; its grid when the message carries one). nullopt: not a decode, a hashed / invalid call, no sender, or my own call.
std::optional<Report> report_from_line(const std::string& line, double dial_hz, const std::string& mode, int64_t t, const std::string& my_call);

class Uploader {
public:
    Uploader(std::string my_call, std::string my_grid, std::string software, uint32_t observation_id, int64_t start, uint32_t jitter_seed);
    void set_station(std::string my_call, std::string my_grid);
    void set_antenna(std::string a) { antenna_ = std::move(a); }
    bool add(const Report& r);                          // false: this station was already reported on this band within the hour
    size_t pending() const { return pending_.size(); }
    bool due(int64_t now) const;                        // time for a datagram (or one is full)
    std::vector<uint8_t> build(int64_t now);            // the next datagram (takes what fits from the queue); empty if nothing to send
    uint32_t sent_reports() const { return seq_; }
    int datagrams() const { return datagrams_; }
    int64_t next_send() const { return next_; }
private:
    std::string call_, grid_, software_, antenna_; uint32_t obs_, seq_ = 0, rng_; int datagrams_ = 0; int64_t next_, last_templates_ = -1;
    std::vector<Report> pending_; std::map<std::string, int64_t> last_reported_;
    uint32_t jitter(uint32_t max_s);
};

// One line per spot for ~/.local/share/fireside/pskreporter-heard.csv (antenna = the user's note at the time):
// utc_epoch,band,mode,freq_hz,snr,receiver,receiver_grid,km,bearing,antenna
std::string csv_header();
std::string csv_line(const Spot& s, double km, double bearing, const std::string& antenna);
}  // namespace fireside::pskr
