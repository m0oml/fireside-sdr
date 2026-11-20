// Shared test fixtures: big-endian helpers and a fake DICE register file behaving like the Flex firmware as the real firmware does.
#pragma once
#include <functional>
#include <map>
#include <vector>
#include "fireside/cli.h"
#include "fireside/opcodes.h"
#include "fireside/pal.h"
#include "fireside/sim_bus.h"
#include "test_util.h"

namespace fakedice {
using namespace fireside;
inline std::vector<uint8_t> be(std::initializer_list<uint32_t> ws) { std::vector<uint8_t> o; for (uint32_t w : ws) for (int i = 3; i >= 0; --i) o.push_back(uint8_t(w >> (8 * i))); return o; }
inline std::vector<uint8_t> le(std::initializer_list<uint32_t> ws) { std::vector<uint8_t> o; for (uint32_t w : ws) for (int i = 0; i < 4; ++i) o.push_back(uint8_t(w >> (8 * i))); return o; }
inline uint32_t be32(const std::vector<uint8_t>& v, size_t o = 0) { return uint32_t(v[o]) << 24 | uint32_t(v[o + 1]) << 16 | uint32_t(v[o + 2]) << 8 | v[o + 3]; }
inline uint32_t opv(const char* n) { return find_opcode(std::string(n))->value; }

// The register file lives in the library now (fireside/sim_bus.h) so the GUI demo can use it too.
using FakeDiceBus = fireside::sim::DiceBus;

// The simulated radio's identity (mirror of firesidectl.fake.DEFAULTS): FLEX-5000A, serial 12345, firmware 2.1.4.8, ATU fitted, no RX2/VU.
inline void flex_defaults(FakeDiceBus& b) { b.set_flex_5000a(); }
// Wraps a bus as the BusHandle the CLI / RadioSession open.
struct FakeHandle : BusHandle { FakeDiceBus& b; bool local; FakeHandle(FakeDiceBus& b_, bool l = false) : b(b_), local(l) {} IBus& bus() override { return b; } bool is_local() const override { return local; } };
// Every non-read command the bus was asked to run, as (opcode, d1, d2).
struct Wr { uint32_t op, d1, d2; bool operator==(const Wr& o) const { return op == o.op && d1 == o.d1 && d2 == o.d2; } };
inline std::vector<Wr> write_ops(const FakeDiceBus& b) {
    std::vector<Wr> out;
    for (auto& w : b.writes) { const uint32_t op = be32(w.second, 0) & 0x7FFFFFFFu; const OpInfo* o = find_opcode(op); if (!o || !is_read(*o)) out.push_back({op, be32(w.second, 4), be32(w.second, 8)}); }
    return out;
}
}  // namespace fakedice
