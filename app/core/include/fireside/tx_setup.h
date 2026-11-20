// Transmit set-up from the radio's own calibration EEPROM, as PowerSDR does it (analysis/transmit-stage2.md section 4):
//   * PA bias: 8 digital pots from EEPROM PA bias row 0 (pointer @0x54): pot i = byte i for even i, 255 - byte i for odd i (FLEX5000DebugForm.cs).
//     Must be written before any key-down: the pots' state after a power cycle is unknown and a wrong bias overheats the PA.
//   * TX carrier null: SET_TRX_POT with the TX carrier table word of the nearest frequency (console.cs ~33171), on every TX frequency change.
//   * drive: the PA power table (11 bands x 1, 2, 5, 10, 20, 30 .. 100 W), interpolated in squared volts, / 2.23 (console.cs ~54590).
// The safety gate allows the pot writes only with exactly these EEPROM-derived values (Safety::set_tx_calibration; this module is the only caller).
#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "fireside/client.h"
#include "fireside/eeprom.h"
#include "fireside/startup.h"      // LogFn
#include "fireside/tx_meter.h"

namespace fireside::txs {
struct TxCal {
    bool valid = false; std::string problem;
    std::array<uint8_t, 8> eeprom_bias{}, bias_pots{};            // raw row 0, and the values written to the pots
    std::vector<CarrierEntry> carrier;                            // TX carrier-null table (frequency -> SET_TRX_POT word)
    std::array<std::array<double, 13>, txm::kBands> power{};      // PA power table
    txm::PaCal meter;                                             // PA bridge + reflected correction
    std::array<double, txm::kBands> image_gain{}, image_phase{};  // TX image correction per band (PowerSDR units; 0 = none)
};
inline constexpr double kAudioVolts = 2.23;                       // PowerSDR audio_volts1
inline constexpr double kPowerPoints[13] = {1, 2, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
inline constexpr double kAbsoluteMaxDrive = 0.04;                 // ~20 W at this radio's measured drive levels (12 500 W per drive^2 on 20 m): nothing above it, whatever is asked

// From a full TRX image (read_verified). Invalid (with the reason) unless: header valid with checksums present, PA bias pointer 0x2C0..0x7FF and row 0 not
// blank (0x00 / 0xFF), PA power + meter tables usable, at least one carrier-null entry.
TxCal from_image(const std::vector<uint8_t>& image);
uint32_t carrier_word(const TxCal& c, double tx_mhz);             // nearest entry (by frequency); throws std::logic_error if the table is empty
// The I/Q amplitude PowerSDR would use for `watts` on `band` (0 for watts <= 0). Never above kAbsoluteMaxDrive.
double drive_for_watts(const TxCal& c, int band, double watts);

// Registers the calibration with the safety gate and writes the 8 bias pots (then waits until the firmware has taken the last one).
// Requires allow_write. Throws std::invalid_argument if `c` is not valid.
void write_bias(Flex& fx, const TxCal& c, const LogFn& log = {});
void write_carrier_null(Flex& fx, const TxCal& c, double tx_mhz);
}  // namespace fireside::txs
