# PAL mailbox protocol (FLEX-5000 FireWire control)

How the radio's command mailbox is driven over FireWire. The facts here were worked out for interoperability (so that software other
than PowerSDR can control a radio its owner has) and checked against a real FLEX-5000A: from PowerSDR's open-source GPL code, from
its traffic on the wire, and from the radio's own behaviour. Opcodes are in `opcode-reference.md`.

## Transport
Everything Flex-specific is **plain IEEE 1394 asynchronous reads and writes** at node offset `0xFFFF_E000_xxxx`, the standard DICE
register window. FireWire quadlets are big-endian on the bus. On Linux this is done from user space through `/dev/fwN`
(firewire-cdev), with no byte swapping to do in the application beyond big-endian packing.

Operations offered to the application (the names PowerSDR uses): Init, Exit, GetNumDevices, SelectDevice, GetDeviceInfo (asks the radio for its
model and serial number through the mailbox), ReadOp, WriteOp, SetCallback, SetBufferSize (the audio driver's buffer size; not used here).

## Mailbox (Flex firmware, DICE offset 0x1000)
| Address | Name | Use |
|---|---|---|
| 0xFFFF_E000_1000 | CMD | write `{opcode \| 0x80000000, data1, data2}` as **one 12-byte block write**. Bit 31 = busy/execute; the firmware clears it when done |
| 0xFFFF_E000_1004 | RESULT | read after bit 31 clears (it overlaps the data1 slot) |
| 0xFFFF_E000_1008 | EVENT | input bitmap (below) |

**ReadOp(opcode, d1, d2) -> value**
1. take a lock (one command at a time)
2. read CMD until bit 31 is clear (idle)
3. write the CMD block `{opcode|0x80000000, d1, d2}`
4. read CMD; while bit 31 is set, wait 10 ms and read again. PowerSDR's own library never gives up here; ours times out
5. read RESULT
6. release the lock

**WriteOp(opcode, d1, d2)**: step 3 only. PowerSDR's library neither takes the lock nor waits for idle, relying on the firmware being fast. We always wait
for idle first.

**GetDeviceInfo** variant: the opcode is written **without** bit 31, then the library waits for DICE notification bit 24 (100 ms timeout, retried)
instead of polling, then reads RESULT.

## DICE notifications
The driver forwards the DICE notification quadlet. `0xFFFF0000` is a sentinel; the vendor bits are:
| Bit | Meaning |
|---|---|
| 0x01000000 | command complete (used by GetDeviceInfo) |
| 0x02000000 | EVENT changed: read EVENT (0xFFFF_E000_1008) and act on the value |
| 0x04000000 | nothing waits on it |

On Linux, snd-dice receives these notifications and exposes them to user space through its hwdep device (`SNDRV_FIREWIRE_EVENT_DICE_NOTIFICATION`).
Without that, EVENT can simply be polled.

## EVENT bitmap (decoded by PowerSDR's `FWC.Notify`, Console/FWC/fwc.cs)
| Bit | Input |
|---|---|
| 0x01 | CW paddle dot |
| 0x02 | CW paddle dash |
| 0x04 | rear RCA PTT |
| 0x08 | mic PTT |
| 0x10 | mic DOWN button |
| 0x20 | mic UP button |
| 0x40 | mic FAST button (toggle on each event) |

## Do not touch
`0xFFFF_E010_0000` to `...0034`: the standard TCAT **firmware loader** section (opcode / return status / parameter area). This is
firmware flashing, so none of our tools ever write there.

## Checked on the real radio
Byte order of the mailbox quadlets (big-endian), that a Linux user-space write to CMD works while snd-dice owns the device (async requests are per node,
not exclusive), and that no "owner" registration is needed for the mailbox (snd-dice claims the DICE owner register when streaming).
