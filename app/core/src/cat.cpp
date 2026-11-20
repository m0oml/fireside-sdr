#include "fireside/cat.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace fireside {
namespace {
const char* kErr = "?;";
bool all_digits(const std::string& s, size_t from, size_t n) {
    if (s.size() < from + n) return false;
    for (size_t i = from; i < from + n; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
uint64_t to_u64(const std::string& s, size_t from, size_t n) { return std::stoull(s.substr(from, n)); }
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) { char b[96]; va_list ap; va_start(ap, f); std::vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }
bool valid_mode(int m) { return (m >= 1 && m <= 7) || m == 9; }
}  // namespace

std::string CatSession::feed(const std::string& bytes, CatConn& conn) {
    std::string reply;
    std::vector<std::function<void()>> notify;
    {
        std::lock_guard<std::mutex> l(m_);
        for (char c : bytes) {
            if (c == '\r' || c == '\n') continue;                    // tolerate line endings from terminal programs
            if (conn.discarding) { if (c == ';') conn.discarding = false; continue; }   // after a runaway line: skip to its terminator
            conn.buf.push_back(c);
            if (c == ';') { reply += execute(conn.buf.substr(0, conn.buf.size() - 1), notify); conn.buf.clear(); }
            else if (conn.buf.size() > 64) { conn.buf.clear(); conn.discarding = true; reply += kErr; }
        }
    }
    for (auto& n : notify) n();                                      // callbacks run outside the lock
    return reply;
}

std::string CatSession::execute(const std::string& cmd, std::vector<std::function<void()>>& notify) {
    if (cmd.size() < 2) return kErr;
    const std::string name = cmd.substr(0, 2), arg = cmd.substr(2);
    const bool get = arg.empty();
    auto rx_changed = [&] { if (on_rx_freq) { auto cb = on_rx_freq; const uint64_t hz = st_.rx_freq(); notify.push_back([cb, hz] { cb(hz); }); } };

    if (name == "ID") return get ? "ID019;" : kErr;                   // TS-2000
    if (name == "PS") { if (get) return st_.power_on ? "PS1;" : "PS0;"; return arg == "1" ? "" : kErr; }   // never power the radio off from CAT
    if (name == "FA" || name == "FB") {
        uint64_t& f = name == "FA" ? st_.freq_a : st_.freq_b; const int vfo = name == "FA" ? 0 : 1;
        if (get) return fmt("%s%011llu;", name.c_str(), static_cast<unsigned long long>(f));
        if (arg.size() != 11 || !all_digits(arg, 0, 11)) return kErr;
        const uint64_t v = to_u64(arg, 0, 11);
        if (v == 0 || v > kCatMaxHz) return kErr;                      // outside what the receive path can tune
        const bool changed = f != v; f = v;
        if (changed && st_.rx_vfo == vfo) rx_changed();
        return "";
    }
    if (name == "FR" || name == "FT") {
        int& v = name == "FR" ? st_.rx_vfo : st_.tx_vfo;
        if (get) return fmt("%s%d;", name.c_str(), v);
        if (arg != "0" && arg != "1") return kErr;                      // 2 = memory: not supported
        const int nv = arg[0] - '0'; const bool changed = v != nv; v = nv;
        if (changed && name == "FR") rx_changed();
        return "";
    }
    if (name == "MD") {
        if (get) return fmt("MD%d;", st_.mode);
        if (arg.size() != 1 || !all_digits(arg, 0, 1) || !valid_mode(arg[0] - '0')) return kErr;
        const int m = arg[0] - '0'; const bool changed = m != st_.mode; st_.mode = m;
        if (changed && on_mode) { auto cb = on_mode; notify.push_back([cb, m] { cb(m); }); }
        return "";
    }
    if (name == "IF") {
        if (!get) return kErr;
        // P1 freq(11) P2 step(4) P3 RIT/XIT freq(+/-5) P4 RIT on P5 XIT on P6 bank P7 mem ch(2) P8 TX/RX P9 mode P10 FR/FT-function P11 scan P12 split P13 tone P14 tone no.(2) P15 space
        return fmt("IF%011llu%04d%+05d%d%d%d%02d%d%d%d%d%d%d%02d%d;", static_cast<unsigned long long>(st_.rx_freq()), 10, 0,
                   0, 0, 0, 0, 0, st_.mode, st_.rx_vfo, 0, st_.tx_vfo != st_.rx_vfo ? 1 : 0, 0, 0, 0);
    }
    if (name == "AI") { if (get) return "AI0;"; return arg == "0" ? "" : kErr; }          // no auto-information push
    if (name == "AG") {
        if (arg == "0") return fmt("AG0%03d;", st_.af_gain);
        if (arg.size() == 4 && arg[0] == '0' && all_digits(arg, 1, 3)) { const int v = int(to_u64(arg, 1, 3)); if (v > 255) return kErr; st_.af_gain = v; return ""; }
        return kErr;
    }
    if (name == "SM") return arg == "0" ? fmt("SM0%04d;", st_.smeter) : kErr;
    if (name == "RX") return get || arg == "0" || arg == "1" ? "" : kErr;                // already receiving; nothing to do
    if (name == "TX") { ++refused_tx_; return kErr; }                                    // NEVER key the transmitter (see header)
    if (name == "MX" || name == "KY" || name == "TS" || name == "PT" || name == "SD") { if (name == "MX" || name == "KY") ++refused_tx_; return kErr; }
    return kErr;
}

// ---------------------------------------------------------------- TCP
uint16_t CatTcpServer::start(uint16_t port) {
    if (listen_fd_ >= 0) throw std::runtime_error("CAT server already started");
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    int one = 1; ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listen_fd_, 4) != 0) {
        const std::string e = std::strerror(errno); ::close(listen_fd_); listen_fd_ = -1; throw std::runtime_error("CAT bind/listen: " + e);
    }
    socklen_t len = sizeof a; ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
    stop_ = false; th_ = std::thread([this] { run(); });
    return ntohs(a.sin_port);
}
void CatTcpServer::stop() {
    stop_ = true; if (th_.joinable()) th_.join();                      // join BEFORE closing: fd numbers get reused
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}
void CatTcpServer::run() {
    std::vector<int> clients; std::map<int, CatConn> conns;
    while (!stop_) {
        std::vector<pollfd> p{{listen_fd_, POLLIN, 0}};
        for (int c : clients) p.push_back({c, POLLIN, 0});
        if (::poll(p.data(), nfds_t(p.size()), 50) <= 0) continue;
        if (p[0].revents & POLLIN) { int c = ::accept(listen_fd_, nullptr, nullptr); if (c >= 0) { if (clients.size() < 8) { clients.push_back(c); conns[c] = CatConn{}; } else ::close(c); } }
        for (size_t i = 1; i < p.size(); ++i) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[256]; const ssize_t n = ::recv(p[i].fd, buf, sizeof buf, 0);
            if (n <= 0) { ::close(p[i].fd); conns.erase(p[i].fd); clients.erase(std::find(clients.begin(), clients.end(), p[i].fd)); continue; }
            const std::string r = session_.feed(std::string(buf, size_t(n)), conns[p[i].fd]);
            if (!r.empty()) (void)!::send(p[i].fd, r.data(), r.size(), MSG_NOSIGNAL);
        }
    }
    for (int c : clients) ::close(c);
}
}  // namespace fireside
