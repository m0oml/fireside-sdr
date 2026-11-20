// Transmit power and SWR from the PA's forward / reflected detector (READ_PA_ADC channel 7 = forward, 6 = reflected), calibrated with the radio's own
// EEPROM tables. Port of PowerSDR console.cs FWCPAPower / FWCSWR (FlexRadio's original form: the per-band "PA SWR" factor scales the REFLECTED
// reading only, as the meter code does; the KE9NS variant that scaled both cancels it out of the SWR).
//   PA bridge (EEPROM 0x2E0, 11 bands x 6): detector volts at 1, 2, 5, 10, 20 and 90 W; between points watts are interpolated in volts squared.
//   PA SWR    (EEPROM 0x630, 11 bands):     reflected-power correction factor per band (about 0.46 .. 1.74 on this radio): NOT an SWR value.
// Checked on the capture of 2026-11-18: the passing 20 m ATU tune reads 11.4 W here; the tuner itself reported 10.6 W.
#pragma once
#include <array>
#include <optional>
#include <string>
#include "fireside/eeprom.h"

namespace fireside::txm {
inline constexpr int kBands = 11;                                 // 160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6 m (the EEPROM's row order)
const char* band_name(int band);                                  // "20m"; "" if out of range
// The transmit band for a frequency (amateur allocations, as PowerSDR's tx_band); -1 outside them.
int band_of(double freq_hz);
int band_from_name(const std::string& name);                      // "20m" -> 5; -1 if unknown

inline constexpr int kAdcFwd = 7, kAdcRev = 6;                    // READ_PA_ADC channels

struct PaCal {
    std::array<std::array<double, 6>, kBands> bridge{};           // volts at the six reference powers
    std::array<double, kBands> rev_corr{};                        // reflected-power factor
    bool valid = false; std::string problem;                      // invalid: no meter (never guess calibration)
    static PaCal from_eeprom(const Decoded& d);                   // needs "PA bridge" and "PA SWR" with good pointers and checksums
};

double watts(const PaCal& c, int band, int adc);                  // forward or (uncorrected) reflected watts; 0 below 2 counts
struct Reading { double fwd_w = 0, rev_w = 0; std::optional<double> swr; };   // swr nullopt: too little power to judge (both <= 1 W)
// Forward / reflected watts and SWR (capped at 25) for one pair of ADC readings.
Reading read(const PaCal& c, int band, int adc_fwd, int adc_rev);
}  // namespace fireside::txm
