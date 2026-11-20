// A link carries opcode requests to the radio. Implemented here: MidiLink. FireWire mailbox link: see pal.h (PalMailbox).
#pragma once
#include "fireside/rawmidi.h"

namespace fireside {
class ILink {
public:
    virtual ~ILink() = default;
    // Sends {opcode, d1, d2}. If want_reply, waits for and returns the 32-bit result; otherwise returns 0. Throws TimeoutError.
    virtual uint32_t request(uint32_t opcode, uint32_t d1, uint32_t d2, bool want_reply, double timeout_s) = 0;
    // Waits until the last command has been consumed (read-only). Default: nothing to wait for. Throws TimeoutError.
    virtual void sync(double /*timeout_s*/) {}
};

class MidiLink : public ILink {
public:
    explicit MidiLink(RawMidi& t) : t_(t) {}
    uint32_t request(uint32_t opcode, uint32_t d1, uint32_t d2, bool want_reply, double timeout_s) override;
    void set_next_id(uint16_t id) { id_ = id; }              // diagnostics/tests: e.g. to exercise the 16-bit wrap
private:
    RawMidi& t_; uint16_t id_ = 0;
};
}  // namespace fireside
