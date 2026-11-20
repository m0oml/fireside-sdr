"""The FlexRadio 'PAL' mailbox, (see analysis/pal-mailbox-protocol.md).

DICE register space starts at FireWire address 0xFFFF_E000_0000. Flex's firmware adds:
  0xFFFF_E000_1000  CMD    write {opcode|0x80000000, data1, data2} as one 12-byte block.
                           Bit 31 = busy; firmware clears it when the command has run.
  0xFFFF_E000_1004  RESULT read after CMD bit 31 clears (ReadOp's return value).
  0xFFFF_E000_1008  EVENT  paddle/PTT/mic-button bitmap (see EVENT_BITS).
DICE notification bits (vendor area, delivered via the driver's notification mechanism):
  0x01000000 command complete, 0x02000000 EVENT changed, 0x04000000 (nothing waits on it).

PowerSDR's ReadOp: lock; poll CMD until bit31 clear; write CMD block; poll (10 ms)
until bit31 clear (no timeout there); read RESULT; unlock. WriteOp just writes the
CMD block without waiting at all. We wait for idle before every command and time out.
Byte order: FireWire quadlets are big-endian on the bus, the convention Linux snd-dice
and FFADO use for DICE registers. TO VERIFY on hardware (`byteorder` switch exists for that).
"""
import struct, threading, time

DICE_BASE = 0xFFFF_E000_0000
CMD, RESULT, EVENT = DICE_BASE + 0x1000, DICE_BASE + 0x1004, DICE_BASE + 0x1008
BUSY = 0x8000_0000
NOTIFY_CMD_DONE, NOTIFY_EVENT, NOTIFY_UNUSED = 0x0100_0000, 0x0200_0000, 0x0400_0000
EVENT_BITS = {0x01: "dot", 0x02: "dash", 0x04: "rca_ptt", 0x08: "mic_ptt",
              0x10: "mic_down", 0x20: "mic_up", 0x40: "mic_fast"}


class PalMailbox:
    """Link implementation over any bus object with read(offset, len) / write(offset, bytes)."""

    def __init__(self, bus, byteorder="big", timeout=1.0, poll=0.002):
        self.bus = bus
        self.fmt = ">I" if byteorder == "big" else "<I"
        self.timeout, self.poll = timeout, poll
        self._lock = threading.Lock()
        self.verified = False            # F4: True only after verify() read a plausible firmware revision OVER THIS mailbox (proves the byte order)

    def verify(self, boot_wait=0.0, poll=0.5, sleep=time.sleep, clock=time.monotonic):
        """Read GET_FIRMWARE_REV (read-only) and accept the byte order only if the value is in PowerSDR's supported range.
        Waits up to boot_wait seconds for a booting radio (firmware 0). Required before any start-up/tuning write."""
        from .opcodes import OPCODES
        from . import startup as S
        op = OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]
        end = clock() + boot_wait
        while True:
            try:
                fw = self.request(op, 0, 0, True)
                if S.FW_MIN <= fw <= S.FW_MAX:
                    self.verified = True
                    return True
                if fw != 0:
                    return False             # a definite but implausible answer: retrying cannot help and would resend the probe
            except (TimeoutError, OSError):
                pass
            if clock() >= end:
                return False
            sleep(poll)

    def _rd(self, addr):
        return struct.unpack(self.fmt, self.bus.read(addr, 4))[0]

    def _wait_idle(self, what):
        end = time.monotonic() + self.timeout
        while self._rd(CMD) & BUSY:
            if time.monotonic() > end:
                raise TimeoutError(f"PAL mailbox stuck busy ({what})")
            time.sleep(self.poll)

    def request(self, opcode, data1, data2, want_reply, timeout=None):
        with self._lock:
            self._wait_idle("before command")
            blk = b"".join(struct.pack(self.fmt, v & 0xFFFFFFFF) for v in (opcode | BUSY, data1, data2))
            self.bus.write(CMD, blk)
            if not want_reply:
                return None
            self._wait_idle("waiting for result")
            return self._rd(RESULT)

    def sync(self):
        """Read-only: wait until the firmware has consumed the last command (CMD bit 31 clear). Raises TimeoutError if it never does."""
        with self._lock:
            self._wait_idle("confirming the last command")

    def read_event(self):
        """Current EVENT bitmap -> (raw, {name: bool})."""
        raw = self._rd(EVENT)
        return raw, {n: bool(raw & b) for b, n in EVENT_BITS.items()}


def swapped_opcode_word(opcode):
    """The opcode a radio of the OTHER byte order would decode from our CMD word for `opcode`."""
    w = (opcode | BUSY) & 0xFFFFFFFF
    return struct.unpack(">I", struct.pack("<I", w))[0] & 0x7FFFFFFF


def probe_order(bus, timeout=0.5):
    """Find the mailbox byte order using only GET_FIRMWARE_REV (a read). Returns (order or None, notes).
    A radio of the other order decodes our word as opcode 0x30040080, outside every real opcode (tested), so a wrong
    guess cannot run a command. If the firmware never completes an unknown opcode, the second attempt refuses to
    write into the still-busy mailbox and the probe fails cleanly."""
    from .client import Flex                       # local imports: pal must not depend on client at import time
    from .opcodes import OPCODES
    from . import startup as S
    fw_op = OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]
    assert swapped_opcode_word(fw_op) not in {v[0] for v in OPCODES.values()}, "swapped probe would alias a real opcode"
    notes = []

    class Counting:                                  # counts writes so a failed attempt can be told apart: "busy before we sent anything" vs "sent, no good answer"
        def __init__(self, inner): self.inner, self.writes = inner, 0
        def read(self, off, n): return self.inner.read(off, n)
        def write(self, off, data): self.writes += 1; return self.inner.write(off, data)

    for order in ("big", "little"):
        cbus = Counting(bus)
        try:
            fw = Flex(PalMailbox(cbus, byteorder=order, timeout=timeout)).get("GET_FIRMWARE_REV", timeout=timeout)
        except (TimeoutError, OSError) as e:
            notes.append(f"{order}: {type(e).__name__}")
            if order == "big" and cbus.writes == 0 and isinstance(e, TimeoutError):
                # The mailbox was already busy before we sent anything. A busy mailbox can LOOK idle when read in the other byte order, so trying
                # the other order could write into a wedged mailbox. Stop here.
                notes.append("mailbox busy before any command was sent (wedged or still booting): nothing was written; power-cycle the radio and retry")
                return None, notes
            continue
        notes.append(f"{order}: firmware word 0x{fw:08X}")
        if S.FW_MIN <= fw <= S.FW_MAX:
            return order, notes
    if notes and all("TimeoutError" in n for n in notes):
        notes.append("mailbox never completed: if the radio looks wedged, power-cycle it and retry (nothing further was written)")
    return None, notes
