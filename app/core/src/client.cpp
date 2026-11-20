#include "fireside/client.h"

namespace fireside {
const OpInfo& Flex::resolve(const std::string& op) {
    if (const OpInfo* o = find_opcode(op)) return *o;
    throw std::invalid_argument("unknown opcode '" + op + "'");
}
uint32_t Flex::get(const std::string& op, uint32_t d1, uint32_t d2, double timeout_s) {
    const OpInfo& o = resolve(op);
    safety_.check(o, true);
    return link_.request(o.value, d1, d2, true, timeout_s);
}
void Flex::set(const std::string& op, double d1, double d2) {
    const OpInfo& o = resolve(op);
    const uint32_t r1 = pack_arg(d1, o.arg1), r2 = pack_arg(d2, o.arg2);
    safety_.check_write(o, r1, r2);                              // values are part of the decision (F1)
    link_.request(o.value, r1, r2, false, 0);
}
void Flex::sync(double timeout_s) { link_.sync(timeout_s); }
}  // namespace fireside
