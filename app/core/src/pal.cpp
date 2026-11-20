#include "fireside/pal.h"
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#include "fireside/client.h"
#include "fireside/identify.h"
#include "fireside/opcodes.h"

namespace fireside {
using Clock = std::chrono::steady_clock;
static Clock::time_point after(double s) { return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }

void PalMailbox::put(std::vector<uint8_t>& out, uint32_t v) const {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(order_ == ByteOrder::Big ? v >> (24 - 8 * i) : v >> (8 * i)));
}
uint32_t PalMailbox::rd(uint64_t addr) {
    auto b = bus_.read(addr, 4);
    if (b.size() != 4) throw FwError("short read from mailbox");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(b[i]) << (order_ == ByteOrder::Big ? 24 - 8 * i : 8 * i);
    return v;
}
void PalMailbox::wait_idle(const char* what, double timeout_s) {
    const auto end = after(timeout_s >= 0 ? timeout_s : timeout_);
    while (rd(kPalCmd) & kPalBusy) {
        if (Clock::now() > end) throw TimeoutError(std::string("PAL mailbox stuck busy (") + what + ")");
        if (poll_ > 0) std::this_thread::sleep_for(std::chrono::duration<double>(poll_));
    }
}
uint32_t PalMailbox::request(uint32_t opcode, uint32_t d1, uint32_t d2, bool want_reply, double) {
    std::lock_guard<std::mutex> lock(m_);
    wait_idle("before command");
    std::vector<uint8_t> blk; blk.reserve(12);
    put(blk, opcode | kPalBusy); put(blk, d1); put(blk, d2);
    bus_.write(kPalCmd, blk);                                   // one 12-byte block
    if (!want_reply) return 0;
    wait_idle("waiting for result");
    return rd(kPalResult);
}
void PalMailbox::sync(double timeout_s) { std::lock_guard<std::mutex> lock(m_); wait_idle("confirming the last command", std::max(timeout_s, timeout_)); }
bool PalMailbox::verify(double boot_wait_s, double poll_s) {
    const OpInfo* fw_op = find_opcode("RDAL_OP_GET_FIRMWARE_REV");
    if (!fw_op) throw std::logic_error("GET_FIRMWARE_REV missing from the opcode table");
    const auto end = after(boot_wait_s);
    for (;;) {
        try {
            const uint32_t fw = request(fw_op->value, 0, 0, true, timeout_);
            if (fw >= kFwMin && fw <= kFwMax) { verified_ = true; return true; }
            if (fw != 0) return false;               // a definite but implausible answer (wrong byte order or unsupported firmware): retrying cannot help, and would resend the probe
        } catch (const TimeoutError&) {} catch (const IoError&) {}
        if (Clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::duration<double>(poll_s));
    }
}
EventState PalMailbox::read_event() {
    std::lock_guard<std::mutex> lock(m_);
    EventState e; e.raw = rd(kPalEvent);
    for (const auto& b : kEventBits) e.bits[b.name] = (e.raw & b.bit) != 0;
    return e;
}

uint32_t swapped_opcode_word(uint32_t opcode) {
    const uint32_t w = opcode | kPalBusy;
    const uint32_t s = (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
    return s & 0x7FFFFFFFu;
}

namespace {
struct CountingBus : IBus {                                  // distinguishes "busy before we sent anything" from "sent, got no good answer"
    IBus& in; size_t writes = 0; explicit CountingBus(IBus& i) : in(i) {}
    std::vector<uint8_t> read(uint64_t o, size_t n) override { return in.read(o, n); }
    void write(uint64_t o, const std::vector<uint8_t>& d) override { ++writes; in.write(o, d); }
};
}  // namespace

ProbeResult probe_order(IBus& bus, double timeout_s) {
    const OpInfo* fw_op = find_opcode("RDAL_OP_GET_FIRMWARE_REV");
    if (!fw_op) throw std::logic_error("GET_FIRMWARE_REV missing from the opcode table");
    const uint32_t swapped = swapped_opcode_word(fw_op->value);
    if (find_opcode(swapped)) throw std::logic_error("swapped probe would alias a real opcode");
    ProbeResult res;
    for (ByteOrder order : {ByteOrder::Big, ByteOrder::Little}) {
        char buf[64]; CountingBus cbus(bus);
        try {
            PalMailbox mb(cbus, order, timeout_s);
            Flex fx(mb);
            const uint32_t fw = fx.get("GET_FIRMWARE_REV", 0, 0, timeout_s);
            std::snprintf(buf, sizeof buf, "%s: firmware word 0x%08X", order_name(order), fw);
            res.notes.push_back(buf);
            if (fw >= kFwMin && fw <= kFwMax) { res.order = order; return res; }
        } catch (const TimeoutError&) {
            res.notes.push_back(std::string(order_name(order)) + ": TimeoutError");
            if (order == ByteOrder::Big && cbus.writes == 0) {
                // Already busy before we sent anything. A busy mailbox can LOOK idle when read in the other byte order, so trying the other order
                // could write into a wedged mailbox. Stop here.
                res.notes.push_back("mailbox busy before any command was sent (wedged or still booting): nothing was written; power-cycle the radio and retry");
                return res;
            }
        } catch (const IoError&) { res.notes.push_back(std::string(order_name(order)) + ": IoError"); }
    }
    bool all_timeouts = !res.notes.empty();
    for (const auto& n : res.notes) if (n.find("TimeoutError") == std::string::npos) all_timeouts = false;
    if (all_timeouts)
        res.notes.push_back("mailbox never completed: if the radio looks wedged, power-cycle it and retry (nothing further was written)");
    return res;
}
}  // namespace fireside
