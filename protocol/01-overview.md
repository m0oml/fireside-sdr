# 1. Overview and a first read

## What is on the bus
The FLEX-5000 is an IEEE 1394 (FireWire) device built on a TCAT **DICE** audio chip. It appears as one node with three things you can use:

| Part | What it is | Page |
|---|---|---|
| **DICE registers** | The standard DICE register window at bus offset `0xFFFF_E000_0000`: clock, stream enable, isochronous channel numbers. | 3 |
| **Command mailbox** | Flex-specific registers at DICE offset `0x1000` for sending opcodes (tune, antenna, ATU, ...) and reading results. | 2 |
| **Isochronous audio** | An 8-channel AM824 stream radio -> host carrying receive I/Q, and one host -> radio carrying transmit I/Q and audio. | 3 |

On Linux the kernel's `snd-dice` driver can run the audio for you through ALSA. Fireside SDR can also run the streams itself through `/dev/fwN`
(firewire-cdev), which gives 192 kHz receive and a receive stream that keeps running during transmit; on this radio `snd-dice` showed no channels at 96 and 192 kHz.
The two cannot stream at once: whoever sets the DICE ENABLE register owns the stream.

Everything the radio does is driven by 32-bit **opcodes**, each with two 32-bit data words. The same opcode set is used on both control transports.

## Conventions
- FireWire quadlets are **big-endian** on the bus (verified). The MIDI SysEx transport is little-endian inside its payload (page 2).
- Bus addresses are 48-bit offsets on the radio's node, written `0xFFFF_E000_xxxx`.
- `f32` arguments are IEEE-754 single-precision bit patterns in the 32-bit word.
- Frequencies for tuning opcodes are **MHz as f32**.

## Linux access (what the code here uses)
- Open the radio's `/dev/fwN` (firewire-cdev). Asynchronous operations: read quadlet, write block, to the offsets in these pages.
- Isochronous streams and the bus resource manager (channel and bandwidth allocation) also need the controller's own node, `/dev/fw0`, which is root-only by default.
  `system/70-flexradio.rules` shows the udev rules used here.
- Source: `app/core/src/firewire.cpp` (bus access), `pal.cpp` (mailbox), `fw_iso.cpp` (streams).

## A first read: the firmware revision (verified)
Opcode `GET_FIRMWARE_REV` is 1200 (`0x4B0`). It takes no arguments and returns the revision as four bytes `a.b.c.d`.

1. Write the 12-byte command block to `0xFFFF_E000_1000`. The opcode has bit 31 set (the busy/execute flag); data1 and data2 are zero:

        80 00 04 B0   00 00 00 00   00 00 00 00        (big-endian quadlets: 0x800004B0, 0, 0)

2. Read the quadlet at `0xFFFF_E000_1000` until bit 31 is clear. The firmware clears it when the command has run.
3. Read the quadlet at `0xFFFF_E000_1004`. For firmware 2.1.4.12 it returns `0x0201040C`.

A result of `0` means the radio is still booting: wait and ask again. Fireside SDR treats any answer outside `2.1.4.8 .. 2.1.255.255` as "not a radio we know
how to drive" and refuses to write. Reading is always safe; every other opcode has a safety class in [page 4](04-opcodes.md).

Reference implementations: `tools/firesidectl/pal.py` (Python, a page of code), `app/core/src/pal.cpp` (C++).
