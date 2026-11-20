#include "fireside/startup.h"
#include <cstdio>
#include <map>
#include "fireside/pal.h"
#include "fireside/protocol.h"
#include "fireside/tuning.h"

namespace fireside {
static const std::map<std::string, int>& rx_ants() {
    static const std::map<std::string, int> m = {{"NC", 0}, {"ANT1", 1}, {"ANT2", 2}, {"ANT3", 3}, {"RX1IN", 4}};   // FWCAnt enum
    return m;
}

std::vector<Step> startup_steps(double f, const std::string& ant, double clock) {
    auto it = rx_ants().find(ant);
    if (it == rx_ants().end()) throw std::invalid_argument("rx_ant must be one of ANT1, ANT2, ANT3, NC, RX1IN");
    const uint32_t tw = freq_to_tw(f, clock);
    char buf[96]; std::snprintf(buf, sizeof buf, "tune RX1 to %g MHz (tuning word 0x%08X)", f, tw);
    return {
        {"INITIALIZE", 0, 0, "InitRadio(): FWC.Initialize()"},
        {"SET_TRX_PREAMP", 0, 0, "InitRadio(): SetTRXPreamp(false)"},
        {"SET_STANDBY", 0, 0, "power on: leave standby"},
        {"SET_RX1_ANT", double(it->second), 0, "RX1 antenna " + ant},
        {"SET_RX1_FREQ_TW", double(tw), f, buf},
    };
}
std::vector<Step> shutdown_steps() {
    return {{"SET_MOX", 0, 0, "make sure the transmitter is not keyed"},
            {"SET_STANDBY", 1, 0, "power off: enter standby"},
            {"SET_PA_BIAS", 0, 0, "PA bias off, as PowerSDR does when it closes (Console_Closing)"}};
}

bool is_reviewed_exemption(const std::string& op) { return op == "INITIALIZE" || op == "SET_MOX" || op == "SET_PA_BIAS"; }
bool exemption_value_ok(const std::string& op, double d1, double d2) {     // same rule the safety gate applies (one source of truth)
    const OpInfo* o = find_opcode(op);
    if (!o) return false;
    try { return Safety::reviewed_value(o->name, pack_arg(d1, o->arg1), pack_arg(d2, o->arg2)); }
    catch (const std::out_of_range&) { return false; }
}

void require_firewire(Flex& fx) {
    auto* mb = dynamic_cast<PalMailbox*>(&fx.link());
    if (!mb)
        throw StartupError("the start-up/shut-down writes are only sent over the FireWire mailbox (`--fw /dev/fwN`), the path PowerSDR 2.x uses. "
                           "Writes over MIDI SysEx are unverified for these opcodes.");
    if (!mb->verified())
        throw StartupError("the FireWire mailbox byte order has not been verified (call PalMailbox::verify(): a plausible firmware revision must be read "
                           "over THIS mailbox before any write)");
}

int flush_atu(Flex& fx, int max_reads) {
    int n = 0;
    while (n < max_reads) { ++n; if (((fx.get("ATU_GET_RESULT", 0, 0) >> 24) & 0xFF) == 0) break; }
    return n;
}

namespace {
struct PermitScope {                                         // permit() only for the duration of one reviewed write
    Safety& s; std::string name;
    PermitScope(Safety& s_, const std::string& op) : s(s_), name(find_opcode(op) ? find_opcode(op)->name : op) { s.permit(name); }
    ~PermitScope() { s.unpermit(name); }
};
std::string fmt_call(const Step& s) { char b[96]; std::snprintf(b, sizeof b, "%s(%g, %g)", s.op.c_str(), s.d1, s.d2); return b; }
}  // namespace

void run_steps(Flex& fx, const std::vector<Step>& steps, const LogFn& log) {
    require_firewire(fx);
    for (const auto& s : steps)                              // validate everything first: a bad step later must not leave earlier ones sent
        if (is_reviewed_exemption(s.op) && !exemption_value_ok(s.op, s.d1, s.d2))
            throw SafetyError(fmt_call(s) + " is not a reviewed value; refusing");
    for (const auto& s : steps) {
        if (log) log(fmt_call(s) + "  # " + s.why);
        if (is_reviewed_exemption(s.op)) { PermitScope scope(fx.safety(), s.op); fx.set(s.op, s.d1, s.d2); }
        else fx.set(s.op, s.d1, s.d2);
    }
    fx.sync();                                               // F3: confirm (read-only) the firmware consumed the last write
}

Identity run_startup(Flex& fx, double freq_mhz, const std::string& rx_ant, const LogFn& log, const IdentifyOptions& ident) {
    auto steps = startup_steps(freq_mhz, rx_ant);            // validate arguments before touching the radio
    require_firewire(fx);                                    // fail fast, before any traffic
    Identity idn = identify(fx, ident);                      // firmware-in-range also proves the mailbox byte order is right
    if (log) log(idn.describe());
    if (idn.has_atu()) { const int n = flush_atu(fx); if (log) log("ATU present: drained result buffer in " + std::to_string(n) + " read(s)"); }
    run_steps(fx, steps, log);
    return idn;
}
}  // namespace fireside
