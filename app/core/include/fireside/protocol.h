// FLEX-5000 SysEx framing. Port of tools/firesidectl/protocol.py (behaviour must stay identical; see tests/vectors).
//   request: F0 00 00 41 <32 ASCII-hex chars> F7, payload LE: u16 id | u16 0 | u32 opcode | u32 d1 | u32 d2
//   reply  : same framing, payload: u16 id | u32 value (extra trailing bytes ignored)
#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "fireside/opcodes_gen.h"

namespace fireside {
using Frame = std::vector<uint8_t>;
struct Request { uint16_t id, proto; uint32_t opcode, d1, d2; };
struct Reply { uint16_t id; uint32_t value; };

Frame encode_request(uint16_t id, uint32_t opcode, uint32_t d1, uint32_t d2);
std::optional<Request> decode_request(const Frame& f);   // nullopt if malformed
Frame encode_reply(uint16_t id, uint32_t value);
std::optional<Reply> decode_reply(const Frame& f);       // nullopt if malformed

// Argument packing into the raw 32-bit word. Throws std::out_of_range for values that do not fit.
uint32_t pack_u32(long long v);
uint32_t pack_i32(long long v);
uint32_t pack_f32(double v);
uint32_t pack_arg(double v, ArgType t);
float as_f32(uint32_t raw);
int32_t as_i32(uint32_t raw);
}  // namespace fireside
