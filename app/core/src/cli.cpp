#include "fireside/cli.h"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <thread>
#include "fireside/cat.h"
#include "fireside/client.h"
#include "fireside/identify.h"
#include "fireside/pal.h"
#include "fireside/session.h"
#include "fireside/startup.h"
#include "fireside/tune.h"

namespace fireside {
namespace {
struct RealHandle : BusHandle {
    FwDevice dev; explicit RealHandle(const std::string& p) : dev(p) {}
    IBus& bus() override { return dev; } bool is_local() const override { return dev.is_local(); }
};
struct Args {
    std::string fw, order = "auto", cmd, ant = "ANT1"; bool allow_write = false, dry_run = false; double freq = 7.1, boot_wait = 10.0; long port = 4532;
};
struct Usage : std::runtime_error { using std::runtime_error::runtime_error; };

const char* kUsage =
    "usage: firesidecli list\n"
    "       firesidecli --fw /dev/fwN [--byte-order auto|big|little] [--allow-write] <command>\n"
    "commands: identify [--boot-wait S] | startup [--freq MHz] [--ant ANT1|ANT2|ANT3|NC|RX1IN] [--dry-run] [--boot-wait S] | shutdown |\n"
    "          serve [--freq MHz] [--ant A] [--port N] [--boot-wait S]   (startup, then CAT on 127.0.0.1:N, Ctrl-C = shutdown)\n"
    "Writes (startup, shutdown, serve) require --allow-write. The mailbox byte order is proven with a read-only firmware read before anything else.";

double to_double(const std::string& s, const char* what) {
    size_t n = 0; double v = 0;
    try { v = std::stod(s, &n); } catch (...) { n = 0; }
    if (n != s.size() || !std::isfinite(v)) throw Usage(std::string("bad ") + what + ": " + s);
    return v;
}
Args parse(const std::vector<std::string>& v) {
    Args a; size_t i = 0;
    auto next = [&](const char* opt) -> const std::string& { if (++i >= v.size()) throw Usage(std::string("missing value for ") + opt); return v[i]; };
    for (; i < v.size(); ++i) {
        const std::string& t = v[i];
        if (t == "--fw") a.fw = next("--fw");
        else if (t == "--byte-order") { a.order = next("--byte-order"); if (a.order != "auto" && a.order != "big" && a.order != "little") throw Usage("bad --byte-order"); }
        else if (t == "--allow-write") a.allow_write = true;
        else if (t == "--freq") a.freq = to_double(next("--freq"), "--freq");
        else if (t == "--ant") a.ant = next("--ant");
        else if (t == "--dry-run") a.dry_run = true;
        else if (t == "--boot-wait") a.boot_wait = to_double(next("--boot-wait"), "--boot-wait");
        else if (t == "--port") { const double p = to_double(next("--port"), "--port"); if (p < 0 || p > 65535 || p != std::floor(p)) throw Usage("bad --port"); a.port = long(p); }
        else if (!t.empty() && t[0] == '-') throw Usage("unknown option " + t);
        else if (a.cmd.empty()) a.cmd = t;
        else throw Usage("unexpected argument " + t);
    }
    if (a.cmd.empty()) throw Usage("no command given");
    if (a.cmd != "list" && a.cmd != "identify" && a.cmd != "startup" && a.cmd != "shutdown" && a.cmd != "serve") throw Usage("unknown command " + a.cmd);
    if (a.boot_wait < 0) throw Usage("--boot-wait must be >= 0");
    return a;
}

void print_plan(std::ostream& out, const Args& a) {
    if (!Tuner::in_range(a.freq * 1e6, kRadioIfOffsetHz)) throw std::invalid_argument("dial frequency out of range");
    const auto steps = startup_steps(a.freq - kRadioIfOffsetHz / 1e6, a.ant);       // validates before printing anything; the DDS sits 9 kHz below the dial
    out << "Plan (receive only; nothing here keys the transmitter, touches PA bias or the EEPROM):\n"
        << "  [read-only] prove the mailbox byte order (probe + firmware read in range v" << ver_str(kFwMin) << "..v" << ver_str(kFwMax) << ")\n"
        << "  [read-only] identify: firmware rev, model, serial, boards, region, status; refuse a FLEX-3000\n"
        << "  [read-only] if an ATU is fitted: drain its result buffer\n";
    for (const auto& s : steps) out << "  [write]     " << s.op << "(" << s.d1 << ", " << s.d2 << ")   # " << s.why << "\n";
    out << "  (dial " << a.freq << " MHz; the DDS is tuned " << kRadioIfOffsetHz / 1e3 << " kHz below it, as PowerSDR does: analyse captures with firesideiq's default --if-offset 9000)\n";
    out << "  then wait " << int(kSettleBeforeAudioS * 1000) << " ms and start the I/Q audio stream\n";
}
}  // namespace

CliEnv real_env() {
    CliEnv e;
    e.open_bus = [](const std::string& p) { return std::unique_ptr<BusHandle>(new RealHandle(p)); };
    e.list_nodes = [] { return list_fw_devices(); };
    e.sleep = [](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); };
    e.should_stop = [] { return false; };
    return e;
}

int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, const CliEnv& env) {
    Args a;
    try { a = parse(args); } catch (const Usage& e) { err << "firesidecli: " << e.what() << "\n" << kUsage << "\n"; return 2; }
    auto sleep = env.sleep ? env.sleep : [](double) {};
    try {
        if (a.cmd == "list") {
            const auto nodes = env.list_nodes ? env.list_nodes() : std::vector<FwNode>{};
            for (const auto& n : nodes) {
                char g[24] = "-"; if (n.guid) std::snprintf(g, sizeof g, "%016llX", static_cast<unsigned long long>(*n.guid));
                out << "FW   " << n.path << "   guid=" << g << "   " << (!n.error.empty() ? "error: " + n.error : n.is_local ? "local controller" : "remote node") << "\n";
            }
            if (nodes.empty()) out << "FW:   no /dev/fw* nodes (no FireWire controller, or firewire-ohci not loaded)\n";
            return 0;
        }
        if (a.cmd == "startup" && a.dry_run) { print_plan(out, a); return 0; }      // no radio needed
        const bool writes = a.cmd == "startup" || a.cmd == "shutdown" || a.cmd == "serve";
        if (writes && !a.allow_write) { err << a.cmd << " writes to the radio; re-run with --allow-write" << (a.cmd == "startup" ? " (or use --dry-run)" : "") << "\n"; return 3; }
        if ((a.cmd == "startup" || a.cmd == "serve") && !Tuner::in_range(a.freq * 1e6, kRadioIfOffsetHz)) throw std::invalid_argument("dial frequency out of range");
        if (a.cmd == "startup" || a.cmd == "serve") startup_steps(a.freq - kRadioIfOffsetHz / 1e6, a.ant);   // validate arguments before touching the bus
        if (a.fw.empty()) { err << "firesidecli: --fw /dev/fwN is required (see `firesidecli list`)\n"; return 2; }
        if (!env.open_bus) { err << "firesidecli: no bus available\n"; return 2; }

        RadioSession session(env.open_bus, sleep);
        SessionOptions so; so.path = a.fw; so.byte_order = a.order; so.allow_write = a.allow_write; so.boot_wait_s = a.boot_wait;
        session.connect(so);                                                        // read-only: open, refuse the local controller, probe + verify the byte order (F4)
        const LogFn log = [&](const std::string& m) { out << m << "\n"; };

        if (a.cmd == "identify") { out << session.identify().describe() << "\n"; return 0; }
        if (a.cmd == "shutdown") { session.shutdown(log); return 0; }
        // startup / serve
        session.start(a.freq, a.ant, log);
        out << "radio is up on " << a.freq << " MHz (" << a.ant << "; DDS " << kRadioIfOffsetHz / 1e3 << " kHz below the dial); start the I/Q stream now\n";
        if (a.cmd == "startup") return 0;

        int rc = 0;
        try {                                                                       // G10: once started, every exit path below runs the shutdown
            auto tuner = session.tuner();
            CatState st; st.freq_a = st.freq_b = uint64_t(std::llround(a.freq * 1e6));
            CatSession cat(st); connect_cat_to_tuner(cat, *tuner);
            CatTcpServer srv(cat); const uint16_t port = srv.start(uint16_t(a.port));
            out << "CAT (Kenwood TS-2000 subset) listening on 127.0.0.1:" << port << "; transmit is refused; Ctrl-C to shut down\n" << std::flush;
            if (env.on_listening) env.on_listening(port);
            while (!(env.should_stop && env.should_stop())) {
                sleep(0.02); session.pump();
                if (!tuner->armed()) { err << "tuning stopped after a mailbox fault: " << tuner->last_error() << "\n"; rc = 4; break; }
            }
            srv.stop();                                                             // no CAT retune can race the shutdown
            const auto c = tuner->counters(); out << "tuner: " << c.sent << " write(s), " << c.coalesced << " coalesced, " << c.rejected << " rejected\n";
        } catch (const std::exception& e) { err << "serve stopped: " << e.what() << "\n"; rc = 2; }
        out << "shutting down\n";
        try { session.shutdown(log); }
        catch (const std::exception& e) { err << "SHUTDOWN FAILED: " << e.what() << "\n"; return rc ? rc : 4; }
        return rc;
    } catch (const SafetyError& e) { err << "refused: " << e.what() << "\n"; return 3; }
    catch (const StartupError& e) { err << "startup stopped: " << e.what() << "\n"; return 5; }
    catch (const TimeoutError& e) { err << "timeout: " << e.what() << "\n"; return 4; }
    catch (const std::invalid_argument& e) { err << "invalid argument: " << e.what() << "\n"; return 2; }
    catch (const std::exception& e) { err << "error: " << e.what() << "\n"; return 2; }
}
}  // namespace fireside
