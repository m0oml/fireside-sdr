// Safety-gated opcode client over any link. Port of firesidectl.client.Flex.
#pragma once
#include <string>
#include "fireside/link.h"
#include "fireside/safety.h"

namespace fireside {
class Flex {
public:
    explicit Flex(ILink& link, Safety safety = {}) : link_(link), safety_(safety) {}
    Safety& safety() { return safety_; }
    ILink& link() { return link_; }
    // Unknown names throw std::invalid_argument. Reads return the raw 32-bit word (use as_f32 for floats).
    uint32_t get(const std::string& op, uint32_t d1 = 0, uint32_t d2 = 0, double timeout_s = 1.0);
    void set(const std::string& op, double d1 = 0, double d2 = 0);
    void sync(double timeout_s = 1.0);                           // wait until the link has consumed the last command (read-only)
private:
    ILink& link_; Safety safety_;
    static const OpInfo& resolve(const std::string& op);
};
}  // namespace fireside
