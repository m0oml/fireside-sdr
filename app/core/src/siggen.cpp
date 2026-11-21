#include "fireside/siggen.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include "fireside/tuning.h"

namespace fireside::siggen {
namespace {
void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }   // PowerSDR: Thread.Sleep(50) between the relay writes
// SET_FULL_DUPLEX is on the dangerous list: permitted for exactly this one write, and the gate itself only passes the wire values 0 or 1 (safety.cpp).
void full_duplex(Flex& fx, bool on) {
    Safety& s = fx.safety(); const std::string n = "RDAL_OP_SET_FULL_DUPLEX"; const bool had = s.permitted(n); s.permit(n);
    try { fx.set(n, on ? 1.0 : 0.0, 0.0); fx.sync(); } catch (...) { if (!had) s.unpermit(n); throw; }
    if (!had) s.unpermit(n);
}
}  // namespace
void on(Flex& fx, double gen_mhz) {
    if (!(gen_mhz > 1.0 && gen_mhz < 55.0)) throw std::invalid_argument("generator frequency must be 1..55 MHz");
    try {
        full_duplex(fx, true); pause();
        fx.set("RDAL_OP_SET_TX_FREQ_TW", double(freq_to_tw(gen_mhz)), gen_mhz); fx.sync(); pause();
        fx.set("RDAL_OP_SET_GEN", 1.0, 0.0); fx.sync(); pause();
        fx.set("RDAL_OP_SET_TEST", 1.0, 0.0); fx.sync(); pause();
        fx.set("RDAL_OP_SET_SIG", 1.0, 0.0); fx.sync(); pause();
    } catch (...) { off(fx); throw; }
}
std::string off(Flex& fx) {
    std::string err; auto attempt = [&](const char* what, auto&& f) { try { f(); } catch (const std::exception& e) { err += std::string(err.empty() ? "" : "; ") + what + ": " + e.what(); } pause(); };
    attempt("full duplex off", [&] { full_duplex(fx, false); });
    attempt("SET_GEN 0", [&] { fx.set("RDAL_OP_SET_GEN", 0.0, 0.0); fx.sync(); });
    attempt("SET_TEST 0", [&] { fx.set("RDAL_OP_SET_TEST", 0.0, 0.0); fx.sync(); });
    attempt("SET_SIG 0", [&] { fx.set("RDAL_OP_SET_SIG", 0.0, 0.0); fx.sync(); });
    return err;
}
}  // namespace fireside::siggen
