// Read-only identification of the radio: what PowerSDR does at launch (FWCEEPROM.Init + InitRadio checks), minus every write.
// Port of the read-only half of tools/firesidectl/startup.py.
#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "fireside/client.h"

namespace fireside {
inline constexpr uint32_t kFwMin = 0x02010408, kFwMax = 0x0201FFFF;     // PowerSDR GetMinVersion/GetMaxVersion
struct StartupError : std::runtime_error { using std::runtime_error::runtime_error; };

std::string ver_str(uint32_t v);                  // "2.1.4.8"
std::string serial_str(uint32_t sn);              // Misc.SerialToString: "5748-0000"

struct Board { bool ok = false; std::optional<uint32_t> rev, sn; };
struct Identity {
    uint32_t model = 0xFFFFFFFF, serial = 0, firmware = 0, region = 0;
    bool extended = false;
    std::vector<std::pair<std::string, Board>> boards;
    std::vector<std::string> warnings;
    std::string model_name() const;               // 0 5000A, 1 5000C, 2 5000 variant, 3 FLEX-3000
    bool has(const std::string& board) const;
    bool has_atu() const { return has("ATU"); }
    bool has_rx2() const { return has("RX2"); }
    std::string describe() const;
};

struct IdentifyOptions {
    double boot_wait_s = 10.0, poll_s = 0.5;
    std::function<void(double)> sleep;            // default: real sleep
    std::function<double()> clock;                // default: steady clock seconds
};
// Waits for a booting radio; throws StartupError if it never answers, is a FLEX-3000, or firmware is out of range.
Identity identify(Flex& fx, const IdentifyOptions& opt = {});
}  // namespace fireside
