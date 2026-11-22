#include "fireside/qso_seq.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>
#include "fireside/logbook.h"
#include "fireside/qrz.h"

namespace fireside::seq {
namespace {
std::string upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
bool grid4(const std::string& w) { return w.size() == 4 && w[0] >= 'A' && w[0] <= 'R' && w[1] >= 'A' && w[1] <= 'R' && std::isdigit(static_cast<unsigned char>(w[2])) && std::isdigit(static_cast<unsigned char>(w[3])) && w != "RR73"; }
bool report(const std::string& w, std::string& out, bool& r) {
    size_t i = 0; r = false; if (!w.empty() && w[0] == 'R' && w.size() == 4) { r = true; i = 1; }
    if (w.size() != i + 3 || (w[i] != '+' && w[i] != '-') || !std::isdigit(static_cast<unsigned char>(w[i + 1])) || !std::isdigit(static_cast<unsigned char>(w[i + 2]))) return false;
    out = w.substr(i); return true;
}
std::string base(const std::string& c) { const std::string b = qrz::base_call(c); return b.empty() ? upper(c) : b; }
}  // namespace

const char* stage_name(Stage s) {
    switch (s) { case Stage::Idle: return "idle"; case Stage::Cq: return "calling CQ"; case Stage::CalledDx: return "calling"; case Stage::SentReport: return "sent report";
                 case Stage::SentRReport: return "sent R-report"; case Stage::SentRR73: return "sent RR73"; case Stage::Sent73: return "sent 73"; case Stage::Done: return "done"; case Stage::GaveUp: return "gave up"; }
    return "?";
}
std::string report_text(int snr) { char b[8]; std::snprintf(b, sizeof b, "%+03d", std::clamp(snr, -30, 30)); return b; }

Heard parse(const std::string& text, int snr) {
    Heard h; h.snr = snr; std::vector<std::string> w; { std::stringstream ss(upper(text)); std::string x; while (ss >> x) w.push_back(x); }
    if (w.size() < 2) return h;
    if (w[0] == "CQ") {
        const auto m = logbook::parse_ftx_message(text); if (!m.ok || m.from.empty()) return h;
        h.cq = true; h.from = m.from; h.grid = m.grid.size() >= 4 ? m.grid.substr(0, 4) : ""; h.ok = true; return h;
    }
    auto strip = [](std::string c) { if (c.size() > 2 && c.front() == '<' && c.back() == '>') c = c.substr(1, c.size() - 2); return c; };
    h.to = strip(w[0]); h.from = strip(w[1]); if (h.from == "..." || h.from.empty() || !logbook::valid_call(h.from)) return h;
    if (w.size() >= 3) {
        const std::string& p = w[2]; bool r = false;
        if (p == "RR73") h.rr73 = true; else if (p == "RRR") h.rrr = true; else if (p == "73") h.seventy3 = true;
        else if (report(p, h.report, r)) h.r_report = r; else if (grid4(p)) h.grid = p;
    }
    h.ok = true; return h;
}

Sequencer::Sequencer(std::string my_call, std::string my_grid4, int max_tries) : me_(upper(std::move(my_call))), grid_(upper(std::move(my_grid4)).substr(0, 4)), max_tries_(max_tries) {}
bool Sequencer::to_me(const Heard& h) const { return !h.cq && base(h.to) == base(me_); }
void Sequencer::set(Stage s, std::string m) { st_ = s; msg_ = std::move(m); tries_ = 0; again_ = false; limit_ = 0; }
void Sequencer::stop() { st_ = Stage::Idle; msg_.clear(); tries_ = 0; }
void Sequencer::call_cq() { dx_.clear(); dx_grid_.clear(); sent_.clear(); rcvd_.clear(); logged_ = false; log_due_ = false; rr73_done_ = false; reopened_ = 0; set(Stage::Cq, "CQ " + me_ + " " + grid_); }

bool Sequencer::reply_to(const Heard& h) {
    if (!h.ok || h.from.empty() || base(h.from) == base(me_)) return false;
    if (to_me(h) && h.seventy3) return false;                                                         // their 73: nothing to answer (checked before any state changes)
    dx_ = h.from; dx_grid_ = h.grid; sent_.clear(); rcvd_.clear(); logged_ = false; log_due_ = false; last_send_ = false; rr73_done_ = false; reopened_ = 0;
    const std::string pre = dx_ + " " + me_ + " ";
    if (h.cq || !to_me(h)) { set(Stage::CalledDx, pre + grid_); return true; }                     // their CQ, or they are working someone else: call them
    if (h.rr73 || h.rrr) { set(Stage::Sent73, pre + "73"); last_send_ = true; log_due_ = true; return true; }   // late double-click: finish politely
    if (!h.report.empty() && h.r_report) { rcvd_ = h.report; sent_ = report_text(h.snr); set(Stage::SentRR73, pre + "RR73"); log_due_ = true; return true; }
    if (!h.report.empty()) { rcvd_ = h.report; sent_ = report_text(h.snr); set(Stage::SentRReport, pre + "R" + sent_); return true; }
    sent_ = report_text(h.snr); set(Stage::SentReport, pre + sent_); return true;                    // they called me with their grid
}

bool Sequencer::heard(const Heard& h) {
    if (!h.ok || !to_me(h)) return false;
    if (st_ == Stage::Cq) {                                                                          // someone answered our CQ (a grid or a report): they become DX
        if (h.rr73 || h.rrr || h.seventy3) return false;                                             // a late RR73/73 from an earlier QSO is not an answer to the CQ
        return reply_to(h);
    }
    // RR73 went out and nothing came back in time, so we stopped; their R-report again a little later (they missed it): RR73 once more (twice at most)
    if (st_ == Stage::Done && rr73_done_ && reopened_ < 2 && base(h.from) == base(dx_) && (h.r_report || !h.report.empty())) {
        ++reopened_; set(Stage::SentRR73, dx_ + " " + me_ + " RR73"); return true; }
    if (!active()) return false;
    if (base(h.from) != base(dx_)) return false;                                                     // someone else calling: not now
    if (!h.grid.empty()) dx_grid_ = h.grid;
    const std::string pre = dx_ + " " + me_ + " ";
    switch (st_) {
        case Stage::CalledDx:
            if (!h.report.empty() && !h.r_report) { rcvd_ = h.report; sent_ = report_text(h.snr); set(Stage::SentRReport, pre + "R" + sent_); return true; }
            if (h.r_report) { rcvd_ = h.report; sent_ = report_text(h.snr); set(Stage::SentRR73, pre + "RR73"); log_due_ = true; return true; }
            if (h.rr73 || h.rrr) { set(Stage::Sent73, pre + "73"); last_send_ = true; log_due_ = true; return true; }
            return false;
        case Stage::SentReport:
            if (h.r_report) { rcvd_ = h.report; set(Stage::SentRR73, pre + "RR73"); log_due_ = true; return true; }
            if (!h.report.empty()) { rcvd_ = h.report; set(Stage::SentRR73, pre + "RR73"); log_due_ = true; return true; }   // a plain report back: treat as R
            if (h.rr73 || h.rrr || h.seventy3) { log_due_ = !rcvd_.empty(); st_ = Stage::Done; msg_.clear(); return true; }
            return false;
        case Stage::SentRReport:
            if (h.rr73 || h.rrr) { set(Stage::Sent73, pre + "73"); last_send_ = true; log_due_ = true; return true; }
            if (h.seventy3) { log_due_ = true; st_ = Stage::Done; msg_.clear(); return true; }       // they skipped RR73: complete
            return false;                                                                            // their report again: ours again
        case Stage::SentRR73:
            if (h.seventy3 || h.rr73) { st_ = Stage::Done; msg_.clear(); return true; }               // they confirmed: nothing more to send
            if (h.r_report || !h.report.empty()) { again_ = true; return true; }                     // their R-report again (they missed our RR73): RR73 again
            return false;
        // Sent73: 73 goes out once (next_message) and nothing they send changes that, as in WSJT-X
        default: return false;
    }
}

std::string Sequencer::next_message() {
    if (!active()) return "";
    if (st_ == Stage::Sent73 && tries_ >= 1 && last_send_) { st_ = Stage::Done; msg_.clear(); return ""; }   // 73 goes out once (WSJT-X does the same)
    if (st_ == Stage::SentRR73 && tries_ >= 1) {                                                   // RR73 goes out again only when they repeat their R-report (WSJT-X)
        if (!again_) { st_ = Stage::Done; rr73_done_ = true; msg_.clear(); return ""; }
        again_ = false; if (tries_ >= max_tries_) { st_ = Stage::Done; rr73_done_ = true; msg_.clear(); return ""; }
    }
    if (tries_ >= (st_ == Stage::Cq ? std::max(12, max_tries_) : (limit_ > 0 ? limit_ : max_tries_))) { st_ = Stage::GaveUp; return ""; }   // CQ: up to 12 overs (6 min of FT8, like WSJT-X's watchdog)
    ++tries_;
    return msg_;
}
std::optional<QsoLog> Sequencer::take_log() {
    if (!log_due_ || logged_ || dx_.empty()) return std::nullopt;
    if (st_ != Stage::Done && tries_ == 0) return std::nullopt;                                     // not sent yet
    logged_ = true; log_due_ = false; return QsoLog{dx_, dx_grid_, sent_, rcvd_};
}
std::string Sequencer::describe() const {
    if (st_ == Stage::Idle) return "";
    if (st_ == Stage::Cq) return "Calling CQ (try " + std::to_string(tries_) + ")";
    std::string s = "QSO " + dx_ + ": " + stage_name(st_);
    if (!sent_.empty()) s += ", sent " + sent_;
    if (!rcvd_.empty()) s += ", rcvd " + rcvd_;
    if (active()) s += " (try " + std::to_string(tries_) + "/" + std::to_string(max_tries_) + ")";
    return s;
}
}  // namespace fireside::seq
