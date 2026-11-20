// DDS tuning word, exactly PowerSDR's Freq2TW for the 5000: (uint)(0xFFFFFFFF * f_MHz / 500.0).
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

namespace fireside {
inline constexpr double kDdsClockMHz = 500.0;
inline constexpr double kMaxRxMHz = 65.0;                       // HF + 6 m
inline uint32_t freq_to_tw(double f_mhz, double clock_mhz = kDdsClockMHz) {
    if (!(f_mhz > 0.0 && f_mhz <= kMaxRxMHz)) throw std::invalid_argument("frequency " + std::to_string(f_mhz) + " MHz outside 0..65 MHz");
    return uint32_t(4294967295.0 * f_mhz / clock_mhz);
}
}  // namespace fireside
