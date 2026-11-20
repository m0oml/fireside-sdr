#pragma once
#include <string>
#include "fireside/opcodes_gen.h"

namespace fireside {
const OpInfo* find_opcode(uint32_t value);                 // nullptr if unknown
// Accepts the full name (RDAL_OP_GET_MODEL) or the short one (GET_MODEL / get_model), like firesidectl's resolve().
const OpInfo* find_opcode(const std::string& name);
std::string short_name(const OpInfo& op);                  // strips RDAL_OP_ / FWC_OP_
inline bool is_read(const OpInfo& op) { return op.access == 'r' || op.access == 'x'; }
}  // namespace fireside
