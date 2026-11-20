// The ONE way the app switches the receive antenna after start-up. Modelled on RxPreamp / Tuner (same reviewed conditions):
//   1. only SET_RX1_ANT is ever sent, and only with the wire values 1..4 (ANT1, ANT2, ANT3, RX1 IN: PowerSDR's FWCAnt, console_small.cs); NC, RX2 IN, tap, signal generator,
//      VHF and UHF are not reachable from here (tools/tests/test_tune_guard.py checks the source)
//   2. FireWire mailbox only; armed only after a completed start-up (arm()); any mailbox fault disarms until the operator re-arms
//   3. idempotent: asking for the antenna the radio already uses sends nothing; so a dial that stays inside one band never writes
// Receive path only (the TX antenna is SET_TX_ANT, which this app never sends: nothing is ever transmitted). Thread-safe.
#pragma once
#include <mutex>
#include <optional>
#include <string>
#include "fireside/client.h"

namespace fireside {
enum class RxAnt { Ant1 = 1, Ant2 = 2, Ant3 = 3, Rx1In = 4 };                    // the wire value is the enum value
std::optional<RxAnt> rx_ant_from_name(const std::string& name);                  // "ANT1" "ANT2" "ANT3" "RX1IN" (also "RX1 IN"); nullopt for anything else (including "NC")
const char* rx_ant_name(RxAnt a);                                                // "ANT1" ... "RX1IN"
enum class AntResult { Sent, Unchanged, NotArmed, Fault };

class RxAntenna {
public:
    explicit RxAntenna(Flex& fx);                      // throws StartupError unless fx's link is the proven FireWire mailbox
    void arm(const std::string& started_with);         // call only after run_startup() succeeded; started_with = the antenna name the start-up selected ("NC" = none known). Throws SafetyError if writes are disabled
    void disarm();
    bool armed() const;
    AntResult request(RxAnt a);                        // NotArmed until arm(); Fault disarms
    std::optional<RxAnt> current() const;              // what the radio is set to (nullopt if the start-up chose NC)
    std::string last_error() const;
private:
    Flex& fx_; mutable std::mutex m_; bool armed_ = false; int cur_ = 0; std::string err_;
};

// The bands the antenna table is keyed by: the UK amateur bands (Ofcom Full licence edges) plus "long and medium wave" below 160 m. Everything else = "other" (index -1).
struct RxBand { const char* id; const char* name; double lo_hz, hi_hz; };
const RxBand* rx_bands(size_t& count);
int rx_band_index(double hz);                          // index into rx_bands(), -1 outside every band
}  // namespace fireside
