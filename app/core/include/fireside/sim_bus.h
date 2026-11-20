// A simulated FLEX-5000 on the FireWire register window: behaves like the firmware as pal.dll expects it (big-endian bus, CMD busy bit, RESULT).
// Used by the tests and by `fireside --simulate` (a demo/rehearsal with no hardware). It records every command it runs, so write sequences can be inspected.
#pragma once
#include <functional>
#include <map>
#include <vector>
#include "fireside/firewire.h"
#include "fireside/opcodes.h"
#include "fireside/pal.h"

namespace fireside::sim {
inline std::vector<uint8_t> be_words(std::initializer_list<uint32_t> ws) { std::vector<uint8_t> o; for (uint32_t w : ws) for (int i = 3; i >= 0; --i) o.push_back(uint8_t(w >> (8 * i))); return o; }
inline uint32_t be_word(const std::vector<uint8_t>& v, size_t o = 0) { return uint32_t(v[o]) << 24 | uint32_t(v[o + 1]) << 16 | uint32_t(v[o + 2]) << 8 | v[o + 3]; }

struct DiceBus : IBus {
    std::map<uint64_t, std::vector<uint8_t>> mem;
    int busy_polls, left; bool stuck; int reads = 0;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> writes;                  // every block written to CMD
    std::map<uint32_t, uint32_t> answers;                                           // opcode -> result
    std::map<uint32_t, std::function<uint32_t()>> dynamic;                          // per-request behaviour (takes precedence over `answers`)
    std::vector<uint32_t> executed;                                                 // opcodes the "firmware" ran, in order
    explicit DiceBus(int busy_polls_ = 2, int start_busy = 0, bool stuck_ = false) : busy_polls(busy_polls_), left(start_busy), stuck(stuck_) {
        mem[kPalCmd] = be_words({start_busy ? kPalBusy : 0}); mem[kPalResult] = be_words({0}); mem[kPalEvent] = be_words({0});
        answers[find_opcode(std::string("RDAL_OP_GET_MODEL"))->value] = 0; answers[find_opcode(std::string("RDAL_OP_GET_SERIAL_NUM"))->value] = 0xA1B2C3D4;   // minimal; call set_flex_5000a() for a full radio
    }
    // FLEX-5000A, serial 5748-style 12345, firmware 2.1.4.8, TRX/PA/RFIO/ATU fitted, no RX2/VU.
    void set_flex_5000a() {
        const std::pair<const char*, uint32_t> d[] = {{"RDAL_OP_GET_MODEL", 0}, {"RDAL_OP_GET_SERIAL_NUM", 12345}, {"RDAL_OP_GET_FIRMWARE_REV", 0x02010408},
            {"RDAL_OP_GET_TRX_OK", 1}, {"RDAL_OP_GET_PA_OK", 1}, {"RDAL_OP_GET_RFIO_OK", 1}, {"RDAL_OP_GET_ATU_OK", 1}, {"RDAL_OP_GET_RX2_OK", 0}, {"RDAL_OP_GET_VU_OK", 0}};
        answers.clear(); for (auto& x : d) answers[find_opcode(std::string(x.first))->value] = x.second;
    }
    std::vector<uint8_t> read(uint64_t off, size_t) override {
        ++reads;
        if (off == kPalCmd && !stuck && left > 0 && --left == 0) execute();
        auto it = mem.find(off); if (it == mem.end()) throw FwError("simulated bus: address error");
        return it->second;
    }
    void write(uint64_t off, const std::vector<uint8_t>& d) override {
        writes.emplace_back(off, d);
        if (off != kPalCmd || d.size() != 12) throw FwError("simulated bus: only one 12-byte block at CMD may be written");
        mem[kPalCmd] = {d.begin(), d.begin() + 4}; mem[kPalResult] = {d.begin() + 4, d.begin() + 8};
        left = busy_polls;
    }
    void execute() {
        const uint32_t op = be_word(mem[kPalCmd]);
        if (!(op & kPalBusy)) return;
        const uint32_t code = op & ~kPalBusy;
        executed.push_back(code);
        auto it = answers.find(code); auto dy = dynamic.find(code);
        mem[kPalResult] = be_words({dy != dynamic.end() ? dy->second() : it == answers.end() ? 0 : it->second});
        mem[kPalCmd] = be_words({code});                                            // firmware clears bit 31
    }
};
}  // namespace fireside::sim
