#include "fireside/link.h"
#include <chrono>

namespace fireside {
uint32_t MidiLink::request(uint32_t opcode, uint32_t d1, uint32_t d2, bool want_reply, double timeout_s) {
    const uint16_t mid = id_++;                                  // wraps at 65536 like the Python reference
    t_.send(encode_request(mid, opcode, d1, d2));
    if (!want_reply) return 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout_s));
    for (;;) {
        const double left = std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
        auto f = t_.next_frame(left > 0 ? left : 0);
        if (!f) throw TimeoutError("no reply to opcode " + std::to_string(opcode) + " (id " + std::to_string(mid) + ")");
        auto r = decode_reply(*f);
        if (r && r->id == mid) return r->value;                  // stale or undecodable frames are skipped
    }
}
}  // namespace fireside
