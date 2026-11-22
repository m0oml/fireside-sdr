// Read-only calibration EEPROM backup and decoder. Port of tools/firesidectl/eeprom.py (layout and checksum rules from PowerSDR fwc_eeprom.cs).
// NEVER writes: only READ_*_EEPROM_UINT32 requests.
#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "fireside/client.h"

namespace fireside {
inline constexpr size_t kTrxLen = 0xC1 * 16, kRx2Len = 0x200;
struct EepromReadError : std::runtime_error { using std::runtime_error::runtime_error; };

struct TableResult {
    std::string name;
    unsigned pointer_addr = 0, pointer = 0, address = 0, rows = 0, per_row = 0, checksum_addr = 0;
    bool pointer_ok = false, checksum_ok = false, has_nan = false;
    uint8_t checksum_stored = 0, checksum_calc = 0;
    std::optional<bool> in_range;                                // nullopt: no plausible-range rule
    std::vector<std::vector<double>> values;
};
struct Header { uint32_t serial_raw = 0; std::string serial; uint8_t checksum_flag = 0; bool checksums_present = false; std::optional<std::string> last_cal; uint8_t image_version = 0; };
struct CarrierEntry { double freq_mhz; uint32_t value; };
struct Decoded {
    std::string which; size_t length = 0;
    bool header_valid = false; Header header;
    std::vector<TableResult> tables;
    size_t unverified = 0;
    std::optional<std::vector<CarrierEntry>> tx_carrier; bool tx_terminated = false;
    std::vector<std::string> problems;
};

Decoded decode_eeprom(const std::vector<uint8_t>& data, const std::string& which = "TRX");   // never throws on garbage
// The factory receive-level calibration: table "RX level", one row per band (160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6 m) of {display, preamp, meter} in dB.
// Read only. Valid only with its pointer and checksum good, 11 rows of 3, no NaN and every value in a plausible range (display -90..-30, preamp -30..5, meter -70..-10).
struct RxLevelCal { bool valid = false; std::string problem; std::array<double, 11> display{}, preamp{}, meter{}; };
RxLevelCal rx_level_cal(const Decoded& d);
std::string csv_text(const std::vector<uint8_t>& data);          // PowerSDR Dump() format: hex bytes with NO leading zeros
std::vector<uint8_t> parse_csv(const std::string& text);          // LF or CRLF; throws std::invalid_argument on a cell that is not a byte

std::vector<uint8_t> read_image(Flex& fx, const std::string& which = "TRX", size_t length = 0, int retries = 3, double timeout_s = 1.0);
std::vector<uint8_t> read_verified(Flex& fx, const std::string& which = "TRX", size_t length = 0, int passes = 2);
// Writes F5K_<serial>_<utc>_<which>.bin (+ PowerSDR-style "F5K_<serial> backup.csv" if absent) read-only (0444), never overwriting. Returns paths.
std::vector<std::string> save_backup(const std::string& dir, const std::map<std::string, std::vector<uint8_t>>& images);
}  // namespace fireside
