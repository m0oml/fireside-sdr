#include "fireside/wspr_decode.h"

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

#include "fireside/dsp.h"
#include "fireside/slot_clock.h"
#include "fireside/dxcc.h"

extern char** environ;

namespace fireside::decode {
namespace {
constexpr double kRate = 12000;
constexpr double kLateJoinS = 0.8;           // WSPR signals start 1 s into the slot (wsprd allows a couple of seconds of DT)
constexpr double kRetimeS = 0.02;            // a slot boundary more than this off the SlotClock's time is moved to it
double system_utc() { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9; }
void mkdirs(const std::string& p) { std::string cur; std::istringstream is(p); std::string part; if (!p.empty() && p[0] == '/') cur = "/"; while (std::getline(is, part, '/')) { if (part.empty()) continue; cur += part + "/"; ::mkdir(cur.c_str(), 0755); } }
std::string default_dir() { const char* h = std::getenv("HOME"); return std::string(h ? h : "/tmp") + "/.local/share/fireside/wspr"; }
}  // namespace

std::string find_wsprd() {
    const char* path = std::getenv("PATH"); std::istringstream is(path ? path : "/usr/bin:/usr/local/bin"); std::string d;
    while (std::getline(is, d, ':')) { if (d.empty()) continue; const std::string f = d + "/wsprd"; if (::access(f.c_str(), X_OK) == 0) return f; }
    return "";
}

namespace {
bool is_own(const std::string& spot_call, const std::string& me0) {
    if (me0.empty()) return false;
    std::string me = me0, c = spot_call; for (auto& ch : me) ch = char(std::toupper(static_cast<unsigned char>(ch))); for (auto& ch : c) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (!c.empty() && c.front() == '<' && c.back() == '>') c = c.substr(1, c.size() - 2);
    if (c == me) return true;
    std::istringstream is(c); std::string part; while (std::getline(is, part, '/')) if (part == me) return true;   // EA8/M0OML, M0OML/P
    return false;
}
}  // namespace
bool wspr_spot_plausible(const wspr::Spot& s, std::string* why) {
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    std::string call = s.call; if (!call.empty() && call.front() == '<' && call.back() == '>') call = call.substr(1, call.size() - 2);
    if (call.empty() || call == "...") return true;
    if (!callsign_prefix_known(call)) return no("a prefix no country uses");
    if (call.find('/') != std::string::npos && s.snr <= -28) return no("a compound call at the bottom of the range");
    if (s.snr <= -25 && s.grid.size() >= 4 && !callsign_grid_plausible(call, s.grid.substr(0, 4))) return no("a locator impossible for the callsign's country");
    return true;
}

WsprRunner wsprd_runner(double timeout_s) {
    return [timeout_s](const std::string& wav, double mhz, const std::string& dir, std::string& out, std::string& err) -> bool {
        const std::string exe = find_wsprd(); if (exe.empty()) { err = "wsprd not found: install WSJT-X (it provides wsprd)"; return false; }
        int fd[2]; if (::pipe(fd) != 0) { err = std::string("pipe: ") + std::strerror(errno); return false; }
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd[1], 1); posix_spawn_file_actions_adddup2(&fa, fd[1], 2); posix_spawn_file_actions_addclose(&fa, fd[0]); posix_spawn_file_actions_addclose(&fa, fd[1]);
        char f[32]; std::snprintf(f, sizeof f, "%.6f", mhz);
        std::vector<std::string> args = {exe, "-f", f, "-a", dir, wav}; std::vector<char*> av; for (auto& a : args) av.push_back(a.data()); av.push_back(nullptr);
        pid_t pid = 0; const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, av.data(), environ);
        posix_spawn_file_actions_destroy(&fa); ::close(fd[1]);
        if (rc != 0) { ::close(fd[0]); err = std::string("could not start wsprd: ") + std::strerror(rc); return false; }
        const auto t0 = std::chrono::steady_clock::now(); char buf[4096]; bool timed_out = false;
        for (;;) {
            const double left = timeout_s - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) { timed_out = true; ::kill(pid, SIGKILL); break; }
            struct pollfd p{fd[0], POLLIN, 0}; const int r = ::poll(&p, 1, int(std::min(left, 1.0) * 1000));
            if (r < 0 && errno == EINTR) continue; if (r <= 0) continue;
            const ssize_t n = ::read(fd[0], buf, sizeof buf); if (n <= 0) break; out.append(buf, size_t(n));
        }
        ::close(fd[0]); int st = 0; while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (timed_out) { err = "wsprd took too long and was stopped"; return false; }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { err = "wsprd failed (exit " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1) + ")"; return false; }
        return true;
    };
}

struct WsprDecoder::Impl {
    WsprOptions o; std::unique_ptr<dsp::RationalResampler> rs; std::vector<float> slot; size_t pos = 0; long skip = -1; bool started = false; long slot_index = 0; double t0_utc = 0;
    std::thread job; std::atomic<bool> running{false}; std::atomic<double> dial{14.0956}; mutable std::mutex m; std::string pending; std::string last_error, last_dropped; size_t decoded = 0, slots = 0, dropped_n = 0; bool found = true;
    std::vector<dsp::cf> tmp_in, tmp_out; SlotClock clk; uint64_t in_n = 0, out_n = 0;
    explicit Impl(WsprOptions op) : o(std::move(op)), clk(o.sample_rate) {
        if (o.work_dir.empty()) o.work_dir = default_dir();
        if (!o.runner) { found = !find_wsprd().empty(); o.runner = wsprd_runner(); }
        dial = o.dial_mhz; reset_state();
    }
    void join() { if (job.joinable()) job.join(); }
    void reset_state() { join(); rs = std::make_unique<dsp::RationalResampler>(o.sample_rate, kRate); slot.assign(size_t(WsprDecoder::kFileS * kRate), 0.0f); pos = 0; skip = -1; started = false; slot_index = 0; clk = SlotClock(o.sample_rate); in_n = out_n = 0; std::lock_guard<std::mutex> l(m); pending.clear(); }
    void labels(long idx, std::string& date, std::string& hhmm) const {
        char d[16], t[16];
        if (o.wall_clock) { const time_t tt = time_t(t0_utc + double(idx) * wspr::kSlotS); struct tm g; gmtime_r(&tt, &g); std::snprintf(d, sizeof d, "%02d%02d%02d", g.tm_year % 100, g.tm_mon + 1, g.tm_mday); std::snprintf(t, sizeof t, "%02d%02d", g.tm_hour, g.tm_min); }
        else { const long mins = idx * 2; std::snprintf(d, sizeof d, "000101"); std::snprintf(t, sizeof t, "%02ld%02ld", (mins / 60) % 24, mins % 60); }
        date = d; hhmm = t;
    }
    void launch() {
        join(); std::string date, hhmm; labels(slot_index++, date, hhmm); std::vector<float> copy = slot; const double mhz = o.dial_now && o.dial_now->load() > 0 ? o.dial_now->load() : dial.load(); running = true;
        job = std::thread([this, copy = std::move(copy), date, hhmm, mhz] {
            WsprSlot s; s.date = date; s.hhmm = hhmm; s.dial_mhz = mhz;
            mkdirs(o.work_dir); const std::string wav = o.work_dir + "/" + date + "_" + hhmm + ".wav";
            float peak = 0; for (float v : copy) peak = std::max(peak, std::fabs(v));
            std::vector<float> scaled = copy; if (peak > 0) for (auto& v : scaled) v *= 0.5f / peak;           // wsprd wants 16-bit PCM: use the range, keep headroom
            const auto bytes = wspr::wav16(scaled, int(kRate));
            std::string out, err;
            if (FILE* f = std::fopen(wav.c_str(), "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); s.ok = o.runner(wav, mhz, o.work_dir, out, err); std::remove(wav.c_str()); }
            else err = "cannot write " + wav;
            std::string lines;
            if (s.ok) { std::istringstream is(out); std::string ln; while (std::getline(is, ln)) if (auto sp = wspr::parse_wsprd_line(ln)) {
                sp->hhmm = hhmm; std::string why;
                if (is_own(sp->call, o.my_call)) continue;                                       // our own beacon, heard through the duplex stream
                if (!wspr_spot_plausible(*sp, &why)) { s.dropped.push_back(sp->call + " " + std::to_string(sp->snr) + ": " + why); continue; }   // never shown, never uploaded
                s.spots.push_back(*sp); lines += wspr::format(*sp) + "\n"; } }
            else s.error = err;
            { std::lock_guard<std::mutex> l(m); pending += lines; decoded += s.spots.size(); ++slots; last_error = s.ok ? "" : err; dropped_n += s.dropped.size(); if (!s.dropped.empty()) last_dropped = s.dropped.back(); }
            if (o.on_slot) { try { o.on_slot(s); } catch (...) {} }
            running = false;
        });
    }
    // Every block: the next sample goes where the SlotClock says it was heard. While waiting for a slot: the wait is set to end at its boundary; while
    // filling one (before its 114 s are in): the position moves forward (silence between) or back (overwriting) when it is more than kRetimeS off.
    void retime(size_t file_n) {
        if (!o.wall_clock || !started || !clk.valid()) return;
        const double tc = clk.utc_of(double(out_n) * o.sample_rate / kRate), start = t0_utc + double(slot_index) * wspr::kSlotS;
        const long tol = long(kRetimeS * kRate);
        if (skip > 0) { const long want = long(std::lround((start - tc) * kRate)); if (std::labs(want - skip) <= tol) return;
                        if (want > 0) skip = want; else { skip = 0; pos = std::min(size_t(-want), file_n - 1); } return; }
        if (pos >= file_n) return;                                                    // launched: the rest of the slot is not used
        const long d = long(std::lround((tc - (start + double(pos) / kRate)) * kRate));
        if (d > tol) pos = std::min(pos + size_t(d), file_n - 1); else if (d < -tol) pos = pos > size_t(-d) ? pos - size_t(-d) : 0;
    }
    void audio(const float* x, size_t n, std::string& out) {
        const size_t slot_n = size_t(wspr::kSlotS * kRate), file_n = slot.size();
        if (o.wall_clock) { in_n += n; clk.observe(o.now ? o.now() : system_utc(), in_n); }     // when this audio was heard (not when it arrived)
        if (!started) {
            started = true; double off = o.start_offset_s;
            if (o.wall_clock) { const double first = clk.utc_of(double(in_n - n)); off = std::fmod(first, wspr::kSlotS); t0_utc = first - off; }
            if (off <= kLateJoinS) { skip = 0; std::fill(slot.begin(), slot.end(), 0.0f); pos = size_t(std::lround(off * kRate)); }
            else { skip = long(std::lround((wspr::kSlotS - off) * kRate)); slot_index = 1; }
        }
        retime(file_n);
        tmp_in.resize(n); for (size_t i = 0; i < n; ++i) tmp_in[i] = dsp::cf(x[i], 0.0f);
        tmp_out.clear(); rs->process(tmp_in.data(), n, tmp_out);
        for (const auto& v : tmp_out) {
            ++out_n;
            if (skip > 0) { --skip; continue; }
            if (pos < file_n) slot[pos] = v.real();
            ++pos;
            if (pos == file_n) launch();
            if (pos == slot_n) {
                pos = 0; std::fill(slot.begin(), slot.end(), 0.0f);
                if (o.wall_clock && clk.valid()) {                                            // the next slot starts where the clock says, not where the count says
                    const double e = clk.utc_of(double(out_n) * o.sample_rate / kRate) - (t0_utc + double(slot_index) * wspr::kSlotS);
                    if (e < -kRetimeS) skip = long(std::lround(-e * kRate));                  // heard before the boundary: belongs to the slot just ended
                    else if (e > kRetimeS) pos = std::min(size_t(std::lround(e * kRate)), file_n - 1);   // heard after it: silence first
                }
            }
        }
        std::lock_guard<std::mutex> l(m); out += pending; pending.clear();
    }
};

WsprDecoder::WsprDecoder(WsprOptions o) : p_(std::make_unique<Impl>(std::move(o))) {}
WsprDecoder::~WsprDecoder() { p_->join(); }
void WsprDecoder::process(const float* a, size_t n, std::string& out) { p_->audio(a, n, out); }
void WsprDecoder::reset() { p_->reset_state(); }
void WsprDecoder::set_dial(double mhz) { if (mhz > 0) p_->dial = mhz; }
bool WsprDecoder::busy() const { return p_->running.load(); }
bool WsprDecoder::decoder_found() const { return p_->found; }
std::string WsprDecoder::status() const {
    if (!p_->found) return "WSPR: wsprd not found: install WSJT-X (it provides wsprd)";
    std::lock_guard<std::mutex> l(p_->m); char b[400];
    const double secs = double(p_->pos) / kRate;
    if (p_->skip > 0) std::snprintf(b, sizeof b, "WSPR: waiting for the next even minute (%.0f s)", double(p_->skip) / kRate);
    else std::snprintf(b, sizeof b, "WSPR: slot %.0f/120 s, %zu spot(s) in %zu slot(s)%s%s%s", secs, p_->decoded, p_->slots, p_->last_error.empty() ? "" : "; last: ", p_->last_error.c_str(),
                       p_->dropped_n ? ("; " + std::to_string(p_->dropped_n) + " false decode(s) dropped (last " + p_->last_dropped + ")").c_str() : "");
    return b;
}
}  // namespace fireside::decode
