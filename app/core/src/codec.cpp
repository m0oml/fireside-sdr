#include "fireside/codec.h"
#include <stdexcept>

namespace fireside::codec {
uint8_t read_reg(Flex& fx, uint32_t reg) { return uint8_t(fx.get("RDAL_OP_READ_CODEC_REG", reg) & 0xFF); }
uint8_t mic_gain_value(int db) {
    if (db > 0 || db < -128) throw std::invalid_argument("mic gain must be 0..-128 dB");
    return db == 0 ? uint8_t(0) : uint8_t(0x100 + db);
}
void set_mic_gain_reg(Flex& fx, uint8_t v) {
    if (!(v == 0x00 || v >= 0x80)) throw std::invalid_argument("mic gain register value must be 0x00 or 0x80..0xFF");
    Safety& s = fx.safety(); const std::string n = "RDAL_OP_WRITE_CODEC_REG"; const bool had = s.permitted(n); s.permit(n);
    try { fx.set(n, kMicGainReg, v); fx.sync(); } catch (...) { if (!had) s.unpermit(n); throw; }   // the only write in this file
    if (!had) s.unpermit(n);
}
}  // namespace fireside::codec
