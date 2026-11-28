// FT8 / FT4 QSO sequencing (the standard WSJT-X exchange), as pure logic: what to send next, given what the other station sent us.
//   answering a CQ:        me: DX ME GRID   <- DX: ME DX -12   me: DX ME R-08   <- DX: ME DX RR73   me: DX ME 73        (logged when we send 73)
//   answering a caller:    <- DX: ME DX GRID   me: DX ME -08   <- DX: ME DX R-12   me: DX ME RR73                       (logged when we send RR73)
//   calling CQ:            me: CQ ME GRID  <- the first station answering becomes DX, then as "answering a caller"
// The same message is repeated in each of our slots until the other station moves on, at most `max_tries` times; then it gives up. No radio here:
// the GUI renders and sends what next_message() says, in the slot parity it says. Reports: our SNR of their last message, clamped -30..+30, "-08" / "+05".
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fireside::seq {
enum class Stage { Idle, Cq, CalledDx, SentReport, SentRReport, SentRR73, Sent73, Done, GaveUp };
const char* stage_name(Stage s);

// Where to call CQ: a transmit audio frequency (the lowest tone, as the spin box) clear of the stations heard recently.
// `heard`: their audio frequencies (lowest tone, as decoded) and slot parities. Keeps `current_hz` if it is clear; otherwise the middle of the
// widest clear gap in lo..hi (whole signal inside it). If nothing is clear, only the stations in our own slots count (the others listen
// while we send); if still nothing, `current_hz`. Rounded to 10 Hz.
struct HeardSignal { double hz; int parity; };
double clear_tx_hz(const std::vector<HeardSignal>& heard, int my_parity, double current_hz, bool ft4, double lo_hz = 300, double hi_hz = 2700);
int line_parity(const std::string& decoder_line, bool ft4);   // the slot parity of a decoder line, from its HHMMSS (or S000) label

struct Heard {                       // one decoded FT8/FT4 message, parsed
    std::string to, from, grid, report;   // report as sent: "-12" / "+05" (no R)
    bool cq = false, r_report = false, rr73 = false, rrr = false, seventy3 = false, ok = false;
    int snr = 0;                     // our SNR of it (from the decoder line)
};
Heard parse(const std::string& message_text, int snr);    // "M0OML DK0ULR R-12" ...
std::string report_text(int snr);                         // "-08", "+05" (clamped -30..+30)

struct QsoLog {                      // what a finished QSO logs
    std::string call, grid, rst_sent, rst_rcvd;
    int64_t start_utc = 0;           // when it started (our first message to them); 0 = unknown, log the time of logging
};
// A QSO given up on after the reports went out: if that station's RR73 / RRR / 73 turns up within kLateFinishS (they got through after all,
// we only missed it in time), the QSO is finished with these reports and this start time instead of being started afresh.
struct Unfinished { std::string call, grid, sent, rcvd; int64_t start_utc = 0, gave_up_utc = 0; };
constexpr int64_t kLateFinishS = 600;

class Sequencer {
public:
    Sequencer(std::string my_call, std::string my_grid4, int max_tries = 6);
    // Starting points. Each returns false (and leaves the state alone) if `h` is not something to answer.
    bool reply_to(const Heard& h);        // a double-clicked line: their CQ, or a message to me (any step), or a message to someone else (call them)
    void call_cq(const std::string& text = "");   // CQ until someone answers: "CQ ME GRID", or `text` (a directed CQ such as "CQ DX ME GRID", checked by the caller)
    void stop();                          // Idle
    void limit_tries(int n) { limit_ = n; }   // the current message only (e.g. 2 tries to raise a waiting caller); the next step has the normal limit again
    // A decoded message. Returns true if it moved the QSO on (the next message changed).
    bool heard(const Heard& h);
    // Our slot is due: the message to send now (empty = nothing: idle, done or gave up). Counts a try of the current message.
    std::string next_message();
    // When the QSO is complete enough to log (once): set after the message that completes it was handed out by next_message().
    std::optional<QsoLog> take_log();
    // The QSO this gave up on (reports sent), kept for a late RR73 / 73; carried into a new Sequencer with remember().
    const std::optional<Unfinished>& unfinished() const { return unf_; }
    void remember(const std::optional<Unfinished>& u) { unf_ = u; }
    bool resumed() const { return resumed_; }   // the last reply_to() / heard() finished a given-up QSO (73 or RR73 to send, or logged at once)
    // Our slot is due and the current message has had all its tries: true ONCE, and the GUI sends nothing in this slot but listens to it, because the
    // other station's answer to our last try is decoded a few seconds into it (S57LU 2026-11-28: R-04 decoded 3 s after we had given up). A heard()
    // that moves the QSO on clears it; otherwise the next next_message() gives up.
    bool wait_before_giving_up();
    void set_clock(std::function<int64_t()> now) { now_ = std::move(now); }   // seconds since the epoch (tests)
    Stage stage() const { return st_; }
    bool active() const { return st_ != Stage::Idle && st_ != Stage::Done && st_ != Stage::GaveUp; }
    const std::string& dx() const { return dx_; }
    const std::string& current() const { return msg_; }
    std::string describe() const;         // one line for the status: "QSO DK0ULR: sent R-08, rcvd -12 (waiting for RR73)"
    int tries() const { return tries_; }
private:
    std::string me_, grid_, dx_, dx_grid_, sent_, rcvd_, msg_; Stage st_ = Stage::Idle; int max_tries_, tries_ = 0; bool log_due_ = false, logged_ = false, last_send_ = false, again_ = false, rr73_done_ = false; int reopened_ = 0, limit_ = 0;
    std::optional<Unfinished> unf_; std::optional<QsoLog> late_log_; bool resumed_ = false, waited_ = false; int64_t start_utc_ = 0; std::function<int64_t()> now_;
    int64_t now() const;
    bool resume(const Heard& h, bool keep_stage);
    void set(Stage s, std::string m);
    bool to_me(const Heard& h) const;
};
}  // namespace fireside::seq
