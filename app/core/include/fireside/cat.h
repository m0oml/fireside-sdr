// Kenwood TS-2000-style CAT server (what WSJT-X, fldigi, N1MM, Hamlib "TS-2000" speak). Hardware-free: it drives a CatState plus callbacks.
// SAFETY: transmit is refused. TX/RX-keying commands answer "?;" and are counted, because the output-channel question
// (analysis/audio-stream-layout.md section 4) is unsettled and nothing may key the radio yet. PS0 (power off) is refused too.
// Command formats are from the Kenwood TS-2000 command reference as remembered (not checked against a live Hamlib): verify with rigctl when convenient.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fireside {
struct CatState {
    uint64_t freq_a = 7100000, freq_b = 7100000;    // Hz
    int mode = 2;                                   // 1 LSB 2 USB 3 CW 4 FM 5 AM 6 FSK 7 CW-R 9 FSK-R
    int rx_vfo = 0, tx_vfo = 0;                     // 0 = A, 1 = B
    int af_gain = 128;                              // 0..255
    int smeter = 0;                                 // 0..30
    bool power_on = true;
    uint64_t rx_freq() const { return rx_vfo == 0 ? freq_a : freq_b; }
};
inline constexpr uint64_t kCatMaxHz = 65000000;     // HF + 6 m, same as the tuning-word range

// Per-connection input buffering: a half-received command from one client must never leak into the next client's stream.
struct CatConn { std::string buf; bool discarding = false; };

class CatSession {                                  // pure protocol engine: bytes in, reply bytes out; thread-safe
public:
    explicit CatSession(CatState initial = {}) : st_(initial) {}
    // Called (outside the lock) when the frequency the receiver listens to or the mode changes.
    std::function<void(uint64_t hz)> on_rx_freq;
    std::function<void(int mode)> on_mode;
    // Feed received bytes; returns the reply to send back (possibly empty). Partial commands are buffered across calls.
    std::string feed(const std::string& bytes) { return feed(bytes, default_conn_); }     // single-stream use (tests, a pty)
    std::string feed(const std::string& bytes, CatConn& conn);                           // one CatConn per client connection
    CatState state() const { std::lock_guard<std::mutex> l(m_); return st_; }
    void set_smeter(int v) { std::lock_guard<std::mutex> l(m_); st_.smeter = v < 0 ? 0 : v > 30 ? 30 : v; }
    uint64_t refused_tx() const { return refused_tx_; }
private:
    mutable std::mutex m_; CatState st_; CatConn default_conn_; std::atomic<uint64_t> refused_tx_{0};
    std::string execute(const std::string& cmd, std::vector<std::function<void()>>& notify);   // cmd without the ';'
};

// Loopback-only TCP listener (127.0.0.1). One thread, any number of sequential or concurrent clients. For a serial-only program use `socat`.
class CatTcpServer {
public:
    explicit CatTcpServer(CatSession& s) : session_(s) {}
    ~CatTcpServer() { stop(); }
    uint16_t start(uint16_t port = 0);              // 0 = pick a free port; returns the port. Throws std::runtime_error
    void stop();
private:
    CatSession& session_; int listen_fd_ = -1; std::atomic<bool> stop_{false}; std::thread th_;
    void run();
};
}  // namespace fireside
