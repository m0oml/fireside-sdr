#include "fireside/rigctl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace fireside::rigctl {
namespace {
// Hamlib mode bits (rig.h): AM 1, CW 2, USB 4, LSB 8, FM 0x20, PKTLSB 0x400, PKTUSB 0x800
constexpr unsigned kModes = 0x1 | 0x2 | 0x4 | 0x8 | 0x20 | 0x400 | 0x800, kSsbModes = 0x4 | 0x8 | 0x400 | 0x800;

std::string num(double v) { char b[48]; std::snprintf(b, sizeof b, "%.0f", v); return b; }

struct Cmd { char s; const char* name; int nargs; };
// nargs: arguments taken from the line. Unsupported commands are listed so their arguments are consumed (the client stays in step).
const Cmd kCmds[] = {
    {'f', "get_freq", 0}, {'F', "set_freq", 1}, {'m', "get_mode", 0}, {'M', "set_mode", 2}, {'v', "get_vfo", 0}, {'V', "set_vfo", 1},
    {'t', "get_ptt", 0}, {'T', "set_ptt", 1}, {'s', "get_split_vfo", 0}, {'S', "set_split_vfo", 2}, {'i', "get_split_freq", 0},
    {'I', "set_split_freq", 1}, {'x', "get_split_mode", 0}, {'X', "set_split_mode", 2}, {'_', "get_info", 0},
    {0, "get_powerstat", 0}, {0, "set_powerstat", 1}, {0, "chk_vfo", 0}, {0, "dump_state", 0}, {0, "get_vfo_info", 1}, {0, "set_vfo_opt", 1}, {0, "get_lock_mode", 0}, {0, "set_lock_mode", 1},
    {'q', "quit", 0}, {'Q', "quit", 0},
    // not available here (RPRT -11); send_morse / send_voice_mem would transmit: rejected (RPRT -9)
    {'l', "get_level", 1}, {'L', "set_level", 2}, {'u', "get_func", 1}, {'U', "set_func", 2}, {'p', "get_parm", 1}, {'P', "set_parm", 2},
    {'j', "get_rit", 0}, {'J', "set_rit", 1}, {'z', "get_xit", 0}, {'Z', "set_xit", 1}, {'y', "get_ant", 1}, {'Y', "set_ant", 2},
    {'b', "send_morse", 1}, {0, "send_voice_mem", 1},
};
const Cmd* find(const std::string& tok) {
    if (tok.size() > 1 && tok[0] == '\\') { const std::string n = tok.substr(1); for (const auto& c : kCmds) if (n == c.name) return &c; return nullptr; }
    if (tok.size() == 1) for (const auto& c : kCmds) if (c.s == tok[0]) return &c;
    return nullptr;
}
bool parse_double(const std::string& t, double& v) { if (t.empty()) return false; char* e = nullptr; v = std::strtod(t.c_str(), &e); return e && *e == 0 && std::isfinite(v); }
bool vfo_is_a(const std::string& v) { return v == "VFOA" || v == "VFO" || v == "currVFO" || v == "Main" || v == "MainA" || v == "A"; }
}  // namespace

const char* mode_name(int index, bool data) {
    switch (index) {
    case 0: return data ? "PKTUSB" : "USB";
    case 1: return data ? "PKTLSB" : "LSB";
    case 2: return "AM";
    case 3: return "CW";
    case 4: return "FM";
    default: return "USB";
    }
}
int mode_from_name(const std::string& n, bool* data) {
    bool d = false; int i = -1;
    if (n == "USB") i = 0; else if (n == "LSB") i = 1; else if (n == "AM" || n == "AMS") i = 2; else if (n == "CW" || n == "CWR") i = 3; else if (n == "FM") i = 4;
    else if (n == "PKTUSB") i = 0, d = true; else if (n == "PKTLSB") i = 1, d = true; else if (n == "PKTFM") i = 4, d = true;
    if (data) *data = d;
    return i;
}

std::string dump_state() {
    std::ostringstream o; char b[160];
    o << "1\n2\n0\n";                                                                          // protocol 1, rig model 2 (NET rigctl), ITU region
    std::snprintf(b, sizeof b, "100000.000000 %.6f 0x%x -1 -1 0x1 0x1\n", kMaxHz, kModes); o << b;   // receive: 100 kHz .. 65 MHz
    o << "0 0 0 0 0 0 0\n";
    o << "0 0 0 0 0 0 0\n";                                                                    // no transmit ranges listed (Hamlib still sends PTT; the app decides)
    for (int st : {10, 100, 1000, 10000}) { std::snprintf(b, sizeof b, "0x%x %d\n", kModes, st); o << b; }
    o << "0 0\n";
    std::snprintf(b, sizeof b, "0x%x 2400\n0x%x 1800\n0x%x 3600\n0x2 500\n0x2 250\n0x1 6000\n0x20 12000\n0 0\n", kSsbModes, kSsbModes, kSsbModes); o << b;
    o << "0\n0\n0\n0\n";                                                                       // max RIT, XIT, IF shift, announces
    o << "\n\n";                                                                               // preamp, attenuator lists (none offered)
    o << "0x0\n0x0\n0x0\n0x0\n0x0\n0x0\n";                                                     // get/set func, level, parm
    o << "vfo_ops=0x0\nptt_type=0x0\ntargetable_vfo=0x0\nhas_set_vfo=1\nhas_get_vfo=1\nhas_set_freq=1\nhas_get_freq=1\nhas_set_conf=0\nhas_get_conf=0\n"
         "has_power2mW=0\nhas_mW2power=0\nhas_get_ant=0\nhas_set_ant=0\ntimeout=0\nrig_model=2\nrigctld_version=fireside rigctl 1\ndone\n";
    return o.str();
}

std::string Session::feed(const std::string& bytes) {
    std::string out;
    for (char c : bytes) {
        if (closed_) break;
        if (c == '\r') continue;
        if (c == '\n') { out += line(buf_); buf_.clear(); continue; }
        buf_.push_back(c);
        if (buf_.size() > 1024) { buf_.clear(); out += "RPRT -1\n"; }                         // a runaway line: dropped
    }
    return out;
}

std::string Session::line(const std::string& text) {
    std::vector<std::string> tok; { std::istringstream in(text); std::string t; while (in >> t) tok.push_back(t); }
    std::string out; size_t i = 0;
    while (i < tok.size() && !closed_) {
        std::string t = tok[i++]; bool ext = false; char sep = '\n';
        if (std::strchr("+;|,", t[0])) { ext = true; sep = t[0] == '+' ? '\n' : t[0]; t.erase(0, 1); if (t.empty()) { if (i >= tok.size()) break; t = tok[i++]; } }
        const Cmd* c = find(t);
        ++commands_; last_ = t; if (ptt_) ptt_->heartbeat(client_);
        if (!c) { out += "RPRT -4\n"; break; }                                                  // unknown: nothing done, the rest of the line dropped
        std::vector<std::string> a; while (int(a.size()) < c->nargs && i < tok.size()) a.push_back(tok[i++]);
        const std::string n = c->name;
        int code = kOk; std::vector<std::pair<std::string, std::string>> kv; std::string raw;
        const bool data = (b_.mode_index() == 0 || b_.mode_index() == 1) && (data_req_ < 0 ? b_.digital() : data_req_ == 1);   // the client's own choice wins once it made one
        auto need = [&](size_t k) { if (a.size() < k) { code = kEInval; return false; } return true; };
        if (n == "quit") { closed_ = true; break; }
        else if (n == "get_freq") kv = {{"Frequency", num(b_.freq())}};
        else if (n == "set_freq") {
            double hz = 0;
            if (!need(1)) {} else if (!parse_double(a[0], hz) || hz <= 0 || hz > kMaxHz) code = kEInval;
            else if (b_.tx_busy()) code = kERejected;
            else b_.set_freq(std::round(hz));
        }
        else if (n == "get_mode") kv = {{"Mode", mode_name(b_.mode_index(), data)}, {"Passband", num(b_.width())}};
        else if (n == "set_mode") {
            bool d = false; double w = 0; const int m = a.empty() ? -1 : mode_from_name(a[0], &d);
            if (!need(1)) {} else if (m < 0) code = kENavail;
            else if (a.size() > 1 && !parse_double(a[1], w)) code = kEInval;
            else if (b_.tx_busy()) code = kERejected;
            else { data_req_ = d ? 1 : 0; b_.set_mode(m, w > 0 && w <= 20000 ? w : -1); }
        }
        else if (n == "get_vfo") kv = {{"VFO", "VFOA"}};
        else if (n == "set_vfo") { if (need(1) && !vfo_is_a(a[0])) code = kENavail; }
        else if (n == "get_ptt") kv = {{"PTT", b_.keyed() ? "1" : "0"}};
        else if (n == "set_ptt") {                                                             // 0 off; 1 on, 2 on (mic), 3 on (data): Hamlib's RIG_PTT_*
            if (!need(1)) {}
            else if (a[0] == "0") { if (ptt_) ptt_->request_ptt(client_, false); }              // releasing is never refused
            else if (a[0] != "1" && a[0] != "2" && a[0] != "3") code = kEInval;
            else if (!ptt_ || !ptt_->request_ptt(client_, true)) { code = kERejected; ++refused_ptt_; }   // the app decides; no PttRequest = refused
        }
        else if (n == "get_split_vfo") kv = {{"Split", "0"}, {"TX VFO", "VFOA"}};
        else if (n == "set_split_vfo") { if (need(1) && a[0] != "0") code = kENavail; }
        else if (n == "get_split_freq") kv = {{"TX Frequency", num(b_.freq())}};
        else if (n == "get_split_mode") kv = {{"TX Mode", mode_name(b_.mode_index(), data)}, {"TX Passband", num(b_.width())}};
        else if (n == "set_split_freq" || n == "set_split_mode") code = kENavail;
        else if (n == "get_info") kv = {{"Info", "FLEX-5000A (fireside)"}};
        else if (n == "get_powerstat") kv = {{"Power Status", "1"}};
        else if (n == "set_powerstat") { if (need(1) && a[0] != "1") code = kERejected; }       // never powers the radio off
        else if (n == "dump_state") raw = dump_state();
        else if (n == "get_vfo_info") { if (need(1)) { if (!vfo_is_a(a[0])) code = kENavail; else kv = {{"Freq", num(b_.freq())}, {"Mode", mode_name(b_.mode_index(), data)}, {"Width", num(b_.width())}, {"Split", "0"}, {"SatMode", "0"}}; } }
        else if (n == "set_vfo_opt" || n == "set_lock_mode") {}
        else if (n == "get_lock_mode") { if (!ext) { out += "0\nRPRT 0\n"; continue; } kv = {{"Locked", "0"}}; }   // rigctld prints the value AND RPRT here
        else if (n == "chk_vfo") { out += ext ? "ChkVFO: 0\n" : "0\n"; continue; }                                 // rigctld: no header, no RPRT
        else if (n == "send_morse" || n == "send_voice_mem") code = kERejected;
        else code = kENavail;

        if (!raw.empty() && code == kOk) { out += raw; continue; }
        if (ext) {
            std::string head = n + ":"; for (const auto& s : a) head += " " + s;
            out += head; out.push_back(sep);
            if (code == kOk) for (const auto& [k, v] : kv) { out += k + ": " + v; out.push_back(sep); }
            out += "RPRT " + std::to_string(code) + "\n";
        } else if (code != kOk || kv.empty()) out += "RPRT " + std::to_string(code) + "\n";
        else for (const auto& kvp : kv) out += kvp.second + "\n";
    }
    return out;
}
}  // namespace fireside::rigctl
