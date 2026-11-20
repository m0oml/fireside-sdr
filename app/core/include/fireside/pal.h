// The FlexRadio "PAL" mailbox (protocol/02-control-transports.md). Port of tools/firesidectl/pal.py.
// DICE register space starts at 0xFFFF'E000'0000; Flex adds CMD/RESULT/EVENT at +0x1000/+0x1004/+0x1008.
// A read command: wait idle, write CMD block {opcode|BUSY, d1, d2}, poll until BUSY clears, read RESULT. A write command: write only.
// We wait for idle before EVERY command and time out. Byte order on the wire is big-endian (verified on the radio).
#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "fireside/firewire.h"
#include "fireside/link.h"

namespace fireside {
inline constexpr uint64_t kDiceBase = 0xFFFF'E000'0000ULL, kPalCmd = kDiceBase + 0x1000, kPalResult = kDiceBase + 0x1004, kPalEvent = kDiceBase + 0x1008;
inline constexpr uint32_t kPalBusy = 0x8000'0000u;
inline constexpr uint32_t kNotifyCmdDone = 0x0100'0000u, kNotifyEvent = 0x0200'0000u, kNotifyUnused = 0x0400'0000u;

enum class ByteOrder { Big, Little };
inline const char* order_name(ByteOrder o) { return o == ByteOrder::Big ? "big" : "little"; }

struct EventBit { uint32_t bit; const char* name; };
inline constexpr EventBit kEventBits[] = {{0x01, "dot"}, {0x02, "dash"}, {0x04, "rca_ptt"}, {0x08, "mic_ptt"}, {0x10, "mic_down"}, {0x20, "mic_up"}, {0x40, "mic_fast"}};

struct EventState { uint32_t raw = 0; std::map<std::string, bool> bits; };

class PalMailbox : public ILink {
public:
    explicit PalMailbox(IBus& bus, ByteOrder order = ByteOrder::Big, double timeout_s = 1.0, double poll_s = 0.002)
        : bus_(bus), order_(order), timeout_(timeout_s), poll_(poll_s) {}
    // `timeout_s` is accepted for ILink compatibility; the idle-wait uses the mailbox's own timeout.
    uint32_t request(uint32_t opcode, uint32_t d1, uint32_t d2, bool want_reply, double timeout_s) override;
    EventState read_event();                                   // read-only
    // F4: reads GET_FIRMWARE_REV (read-only) and accepts the byte order only if the value is in PowerSDR's supported range; waits up to boot_wait_s for a booting
    // radio (firmware 0, or a timeout): any other out-of-range answer fails at once. Start-up writes and the Tuner require verified(). Never throws for timeouts.
    bool verify(double boot_wait_s = 0.0, double poll_s = 0.5);
    bool verified() const { return verified_; }
    void sync(double timeout_s) override;                      // read-only: poll CMD until bit 31 clears (the firmware has taken the last command)
private:
    IBus& bus_; ByteOrder order_; double timeout_, poll_; std::mutex m_; std::atomic<bool> verified_{false};
    uint32_t rd(uint64_t addr);
    void put(std::vector<uint8_t>& out, uint32_t v) const;
    void wait_idle(const char* what, double timeout_s = -1);
};

// The opcode a radio of the OTHER byte order would decode from our CMD word for `opcode`.
uint32_t swapped_opcode_word(uint32_t opcode);

struct ProbeResult { std::optional<ByteOrder> order; std::vector<std::string> notes; };
// Find the mailbox byte order using only GET_FIRMWARE_REV (a read). A wrong guess decodes as an opcode outside every real one.
// Throws std::logic_error if the swapped word would alias a real opcode.
ProbeResult probe_order(IBus& bus, double timeout_s = 0.5);
}  // namespace fireside
