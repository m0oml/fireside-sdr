#include "fireside/opcodes.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace fireside {
const OpInfo* find_opcode(uint32_t value) {
    const OpInfo* end = kOps + kOpCount;
    const OpInfo* it = std::lower_bound(kOps, end, value, [](const OpInfo& o, uint32_t v) { return o.value < v; });
    return (it != end && it->value == value) ? it : nullptr;
}
const OpInfo* find_opcode(const std::string& name) {
    auto by_name = [](const std::string& n) -> const OpInfo* {
        for (size_t i = 0; i < kOpCount; ++i) if (n == kOps[i].name) return &kOps[i];
        return nullptr;
    };
    if (auto* o = by_name(name)) return o;
    std::string up = name; std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return std::toupper(c); });
    if (auto* o = by_name("RDAL_OP_" + up)) return o;
    return by_name("FWC_OP_" + up);
}
std::string short_name(const OpInfo& op) {
    std::string n = op.name;
    for (const char* p : {"RDAL_OP_", "FWC_OP_"}) if (n.rfind(p, 0) == 0) return n.substr(std::strlen(p));
    return n;
}
}  // namespace fireside
