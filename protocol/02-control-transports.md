# 2. Control transports

Opcodes (tune, antenna, ATU, ...) reach the radio over one of two transports. Fireside SDR uses the **mailbox** for everything it writes. The opcode
table is in [page 4](04-opcodes.md).

Every request is three 32-bit words: `opcode`, `data1`, `data2`. Reads return one 32-bit word. Whether an opcode is a read or a write is a property of the opcode.

## A. FireWire mailbox (verified)
Plain IEEE 1394 asynchronous transactions to the radio, inside the DICE register window `0xFFFF_E000_0000`. Quadlets are big-endian.

| Address | Name | Use |
|---|---|---|
| `0xFFFF_E000_1000` | CMD | Write `{opcode \| 0x8000_0000, data1, data2}` as **one 12-byte block write**. Bit 31 means busy/execute; the firmware clears it when the command has finished |
| `0xFFFF_E000_1004` | RESULT | Result of the last read command, valid once CMD bit 31 is clear |
| `0xFFFF_E000_1008` | EVENT | Input bitmap: paddles and PTT (below) |

**Sending a command**
1. Read CMD until bit 31 is clear (idle). We poll every 2 ms and give up after 1 s.
2. Write the 12-byte block to CMD.
3. For a **write** opcode you are done. The next command's step 1 waits for this one to finish.
4. For a **read** opcode: read CMD until bit 31 is clear again, then read RESULT.

Use one lock for the whole sequence: two interleaved commands corrupt each other. Always wait for idle before writing; sending while busy is untested and not
recommended. After a burst of writes, wait for idle once more before relying on the effect (we call this `sync()`).

**Byte order check.** Send `GET_FIRMWARE_REV` (page 1) after opening the bus and accept the link only if the answer is in the plausible range. A wrong byte
order gives a nonsense value, which stops you writing garbage to a radio. The radio is big-endian on the bus.

**Works alongside `snd-dice`.** Asynchronous requests are per node, not exclusive, so the mailbox can be used while the kernel driver streams audio (verified).
No "owner" registration is needed for the mailbox.

### EVENT bitmap (`0xFFFF_E000_1008`)
| Bit | Input |
|---|---|
| `0x01` | CW key / paddle dot |
| `0x02` | CW paddle dash |
| `0x04` | rear-panel RCA PTT |
| `0x08` | microphone PTT |
| `0x10`, `0x20`, `0x40` | mic DOWN, UP, FAST buttons: **FLEX-3000 only**; not wired on the FLEX-5000 |

Read EVENT by polling (about every 10 ms is fine for PTT; paddles for CW keying want a faster loop). Details of what each input should do: `analysis/front-panel-events.md`.

### DICE notifications (reported, not used by Fireside SDR)
The DICE notification quadlet is forwarded to user space by `snd-dice` (hwdep event `SNDRV_FIREWIRE_EVENT_DICE_NOTIFICATION`). Vendor bits seen in the
open-source driver code for this radio: `0x0100_0000` command complete, `0x0200_0000` EVENT changed. We have not exercised either; polling works.

### Do not touch
`0xFFFF_E010_0000` to `0xFFFF_E010_0034` is the standard TCAT **firmware loader** window. Writing it flashes firmware. Nothing in this repository writes there and
yours should not either.

## B. MIDI SysEx (reported; not used for writes here)
The radio also presents a MIDI port (ALSA rawmidi through `snd-dice`) that accepts the same opcodes in a SysEx envelope. This framing comes from PowerSDR's
open-source MIDI code. Fireside SDR implements it (`tools/firesidectl/protocol.py`, `app/core/src/protocol.cpp`) and uses it for read-only identification
only; **it does not know whether the firmware honours every opcode over SysEx.**

**Request**, 37 bytes: `F0 00 00 41 <32 ASCII hex chars, upper case> F7`. The 16 payload bytes, little-endian before hex encoding:

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | `msg_id`, chosen by you, echoed in the reply |
| 2 | u16 | protocol id, always 0 |
| 4 | u32 | opcode |
| 8 | u32 | data1 |
| 12 | u32 | data2 |

**Reply**, same framing: payload `u16 msg_id | u32 result` (u32 or f32 as the opcode defines). Write opcodes get no reply. Match replies to reads by `msg_id`.

Short MIDI messages carry a few real-time controls (PC -> radio: Note On/Off of note 0 = key/unkey the transmitter; radio -> PC: paddle and PTT notes). See
`analysis/front-panel-events.md` section 3. **Treat the MIDI note for the transmitter as dangerous** for the same reason the mailbox MOX opcode is.
