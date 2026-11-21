#pragma once
// Hamlib "NET rigctl" server protocol (what rigctld speaks; Hamlib model 2): lets WSJT-X, JTDX, fldigi, GridTracker, Wavelog's
// WaveLogGate, CQRLOG ... read and set the dial frequency and mode of this app. Qt-free protocol engine; the GUI owns the sockets.
// Reply formats were copied from Hamlib 4.7.2's own rigctld (dummy rig, 2026-11-20) and checked with `rigctl -m 2` against this code.
//
// SAFETY: this module never talks to the radio and Backend has no transmit call. Keying requests (set_ptt) go to a separate, optional
// PttRequest; without one (or when it refuses) set_ptt 1 answers RPRT -9 (rejected) and is counted. set_ptt 0 is always passed on and
// never refused. get_ptt reports the real keyed state read-only. The dial is refused (RPRT -9) while the app is transmitting or about to.
// Whether a request may key is decided by the app (TransmitCard, through TxControl::voice and its TxGate), never here.
// Frequencies outside 0 < f <= 65 MHz are refused (RPRT -1). Power off is refused. The app's own tuning path does the rest.
#include <cstdint>
#include <string>
#include <vector>

namespace fireside::rigctl {
inline constexpr int kDefaultPort = 4532;                // rigctld's default
inline constexpr double kMaxHz = 65e6;                   // HF + 6 m, the receive path's tuning range

// Hamlib return codes used here
inline constexpr int kOk = 0, kEInval = -1, kENimpl = -4, kERejected = -9, kENavail = -11;

// What the app offers. Mode index: 0 USB, 1 LSB, 2 AM, 3 CW, 4 FM (ReceiverPanel's order).
struct Backend {
    virtual ~Backend() = default;
    virtual double freq() const = 0;
    virtual int mode_index() const = 0;
    virtual double width() const = 0;                     // passband width, Hz
    virtual bool digital() const = 0;                     // a data decoder (PSK31 / RTTY / FT8 / FT4) is on: USB / LSB report as PKTUSB / PKTLSB
    virtual bool keyed() const = 0;                       // transmitting now (get_ptt)
    virtual bool tx_busy() const = 0;                     // transmitting or a transmit job running: the dial must not move
    virtual void set_freq(double hz) = 0;                 // only called with a valid frequency while not tx_busy
    virtual void set_mode(int index, double width) = 0;   // width > 0: that passband; <= 0: as the mode leaves it (its default on a change, else unchanged)
};

// The ONLY way a CAT client can ask to transmit. `client` identifies the connection. request_ptt(on = true) returns whether the request was
// accepted; request_ptt(false) releases (the return value is ignored). heartbeat() is called for every command a client sends (a keying
// client that goes quiet is unkeyed by the app's watchdog).
struct PttRequest {
    virtual ~PttRequest() = default;
    virtual bool request_ptt(int client, bool on) = 0;
    virtual void heartbeat(int client) = 0;
};

// One client connection: bytes in, reply bytes out. Commands may come several to a line ("F 7074000 M USB 2400") or one per line,
// in short ("f") or long ("\get_freq") form, with the extended-response prefixes "+", ";", "|", ",".
class Session {
public:
    explicit Session(Backend& b, PttRequest* ptt = nullptr, int client = 0) : b_(b), ptt_(ptt), client_(client) {}
    std::string feed(const std::string& bytes);
    bool closed() const { return closed_; }                // the client sent q / \quit
    uint64_t refused_ptt() const { return refused_ptt_; }
    uint64_t commands() const { return commands_; }
    std::string last_command() const { return last_; }
private:
    Backend& b_; PttRequest* ptt_; int client_; std::string buf_; bool closed_ = false; int data_req_ = -1;   // -1: the client never set a mode (data follows the decoder); 0 / 1: what it last asked for
    uint64_t refused_ptt_ = 0, commands_ = 0; std::string last_;
    std::string line(const std::string& text);
};

std::string dump_state();                                  // the \dump_state block (protocol 1), for tests
const char* mode_name(int index, bool data);               // "USB", "PKTUSB", ...
int mode_from_name(const std::string& name, bool* data);   // -1 if not one we can do
}  // namespace fireside::rigctl
