// Command-line logic for `firesidecli` (C++ counterpart of tools/firesidectl-cli for the FireWire route). Lives in core so it is tested end to end against a fake bus.
//   firesidecli list
//   firesidecli --fw /dev/fwN [--byte-order auto|big|little] [--allow-write] identify | startup [--freq MHz] [--ant ANT1] [--dry-run] | shutdown | serve [--freq MHz] [--port N]
// Every command that touches the radio first proves the mailbox byte order (probe_order + PalMailbox::verify: review finding F4). Writes need --allow-write.
// `serve` = startup + Tuner + loopback CAT server until interrupted, then the reviewed shutdown. The 9 kHz IF offset is NOT offered (held until first light).
// Exit codes (as the Python CLI): 0 ok, 2 error/bad argument, 3 refused (writes disabled / safety), 4 timeout, 5 start-up stopped.
#pragma once
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include "fireside/firewire.h"

namespace fireside {
struct BusHandle { virtual ~BusHandle() = default; virtual IBus& bus() = 0; virtual bool is_local() const = 0; };
struct CliEnv {
    std::function<std::unique_ptr<BusHandle>(const std::string& path)> open_bus;   // throws on failure
    std::function<std::vector<FwNode>()> list_nodes;
    std::function<void(double)> sleep;                                             // seconds
    std::function<bool()> should_stop;                                             // serve: polled every ~20 ms
    std::function<void(uint16_t)> on_listening;                                    // serve: called with the CAT port once it is up (tests, scripts)
};
CliEnv real_env();                                                                 // /dev/fwN via FwDevice, real sleep, never stops by itself
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, const CliEnv& env);
}  // namespace fireside
