// The radio's audio codec: the ONE reviewed write is the mic input gain, register 0x16 (PowerSDR fwcmixform.cs tbMic: 0 dB = 0x00,
// -1..-128 dB = 0x100 + dB, 0x80 = muted). The safety gate passes WRITE_CODEC_REG only for 0x16 with those values (safety.cpp); this
// module takes and drops the permit around that single write. Reads (READ_CODEC_REG) are free. An audio input level: it cannot key,
// route or switch anything else (analysis/voice-tx.md).
#pragma once
#include <cstdint>
#include "fireside/client.h"

namespace fireside::codec {
inline constexpr uint32_t kMicGainReg = 0x16;
uint8_t read_reg(Flex& fx, uint32_t reg);                   // READ_CODEC_REG (read-only)
uint8_t mic_gain_value(int db);                             // 0 .. -128 dB -> the register value (-128 = 0x80 = muted); throws outside
void set_mic_gain_reg(Flex& fx, uint8_t value);             // 0x00 or 0x80..0xFF only (throws std::invalid_argument before any write)
inline void set_mic_gain_db(Flex& fx, int db) { set_mic_gain_reg(fx, mic_gain_value(db)); }
inline void mute_mic(Flex& fx) { set_mic_gain_reg(fx, 0x80); }
}  // namespace fireside::codec
