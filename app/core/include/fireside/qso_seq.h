// FT8 / FT4 QSO sequencing (the standard WSJT-X exchange), as pure logic: what to send next, given what the other station sent us.
//   answering a CQ:        me: DX ME GRID   <- DX: ME DX -12   me: DX ME R-08   <- DX: ME DX RR73   me: DX ME 73        (logged when we send 73)
//   answering a caller:    <- DX: ME DX GRID   me: DX ME -08   <- DX: ME DX R-12   me: DX ME RR73                       (logged when we send RR73)
//   calling CQ:            me: CQ ME GRID  <- the first station answering becomes DX, then as "answering a caller"
// The same message is repeated in each of our slots until the other station moves on, at most `max_tries` times; then it gives up. No radio here:
// the GUI renders and sends what next_message() says, in the slot parity it says. Reports: our SNR of their last message, clamped -30..+30, "-08" / "+05".
#pragma once
#include <optional>
#include <string>

namespace fireside::seq {
enum class Stage { Idle, Cq, CalledDx, SentReport, SentRReport, SentRR73, Sent73, Done, GaveUp };
const char* stage_name(Stage s);

struct Heard {                       // one decoded FT8/FT4 message, parsed
    std::string to, from, grid, report;   // report as sent: "-12" / "+05" (no R)
    bool cq = false, r_report = false, rr73 = false, rrr = false, seventy3 = false, ok = false;
    int snr = 0;                     // our SNR of it (from the decoder line)
};
Heard parse(const std::string& message_text, int snr);    // "M0OML DK0ULR R-12" ...
std::string report_text(int snr);                         // "-08", "+05" (clamped -30..+30)

struct QsoLog {                      // what a finished QSO logs
    std::string call, grid, rst_sent, rst_rcvd;
};

class Sequencer {
public:
    Sequencer(std::string my_call, std::string my_grid4, int max_tries = 6);
    // Starting points. Each returns false (and leaves the state alone) if `h` is not something to answer.
    bool reply_to(const Heard& h);        // a double-clicked line: their CQ, or a message to me (any step), or a message to someone else (call them)
    void call_cq();                       // CQ ME GRID until someone answers
    void stop();                          // Idle
    // A decoded message. Returns true if it moved the QSO on (the next message changed).
    bool heard(const Heard& h);
    // Our slot is due: the message to send now (empty = nothing: idle, done or gave up). Counts a try of the current message.
    std::string next_message();
    // When the QSO is complete enough to log (once): set after the message that completes it was handed out by next_message().
    std::optional<QsoLog> take_log();
    Stage stage() const { return st_; }
    bool active() const { return st_ != Stage::Idle && st_ != Stage::Done && st_ != Stage::GaveUp; }
    const std::string& dx() const { return dx_; }
    const std::string& current() const { return msg_; }
    std::string describe() const;         // one line for the status: "QSO DK0ULR: sent R-08, rcvd -12 (waiting for RR73)"
    int tries() const { return tries_; }
private:
    std::string me_, grid_, dx_, dx_grid_, sent_, rcvd_, msg_; Stage st_ = Stage::Idle; int max_tries_, tries_ = 0; bool log_due_ = false, logged_ = false, last_send_ = false, again_ = false, rr73_done_ = false; int reopened_ = 0;
    void set(Stage s, std::string m);
    bool to_me(const Heard& h) const;
};
}  // namespace fireside::seq
