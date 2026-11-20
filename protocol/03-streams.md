# 3. Stream control and audio format

How to start and stop the radio's isochronous audio streams yourself, what is in the packets, and which channel carries what. Everything marked **Verified**
was done on a real FLEX-5000A with `app/core/src/fw_iso.cpp`. If you would rather let the kernel do the streaming, use `snd-dice` through ALSA and skip to
"Channel map" and "I/Q conventions"; those hold either way.

## DICE registers
The DICE register window starts at bus offset `0xFFFF_E000_0000`. Its first quadlets hold the offset and size of each section (in quadlets; multiply by 4 for
bytes): global, TX, RX, external sync. This radio (**Verified**, read-only dump) has global at byte `0x28`, TX at `0x1A4` (one stream), RX at `0x3DC` (one stream).
"TX" and "RX" are from the radio's point of view: **TX is radio -> host (receive I/Q), RX is host -> radio (transmit I/Q).**

**Global section** (offsets from the start of the section):
| Offset | Register | Meaning |
|---|---|---|
| `0x4C` | CLOCK_SELECT | bits 15:8 = rate index (2 = 48 kHz, 4 = 96 kHz, 6 = 192 kHz); bits 7:0 = clock source (`0x0C`, internal, on this radio; keep what you read) |
| `0x50` | ENABLE | write 1 and the radio **immediately starts sending**; write 0 to stop |
| `0x54` | STATUS | bit 0 = clock locked; bits 15:8 = rate index in use. Seen: `0x401` (96 kHz locked), `0x601` (192 kHz locked) |
| `0x5C` | SAMPLE_RATE | nominal rate in Hz (e.g. `0x17700` = 96000, `0x2EE00` = 192000) |
| `0x64` | CLOCK_CAPS | bit *n* set = rate index *n* offered. This radio: bits 2, 4, 6 (48, 96, 192 kHz) |

**TX section (radio -> host), stream 0 at section + `0x08`:** `+0x00` ISOCHRONOUS (channel number, or `0xFFFFFFFF` for none), `+0x04` NUMBER_AUDIO (8), `+0x08` NUMBER_MIDI (1).
**RX section (host -> radio), stream 0 at section + `0x08`:** `+0x00` ISOCHRONOUS, `+0x04` SEQ_START (0), `+0x08` NUMBER_AUDIO (8), `+0x0C` NUMBER_MIDI (1).

The radio offers 8 audio channels at all three rates (**Verified**). `snd-dice` showed no channels at 96 and 192 kHz on this radio, which is why the direct path exists.
No registers other than CLOCK_SELECT, ENABLE and the two ISOCHRONOUS fields are written by Fireside SDR.

## Start and stop (**Verified**)
**Start, receive only**
1. Read CLOCK_SELECT, ENABLE, STATUS. If ENABLE is not 0 someone else is streaming; do not touch it (Fireside SDR refuses).
2. Allocate an isochronous channel and bandwidth from the bus's isochronous resource manager (on Linux: `FW_CDEV_IOC_ALLOCATE_ISO_RESOURCE` on the local node, `/dev/fw0`),
   and open a receive context on that channel *before* the radio starts sending.
3. Write CLOCK_SELECT with the wanted rate index in bits 15:8 and the old source bits in 7:0. Wait up to 1 s for STATUS to show locked with that rate.
4. Write TX[0].ISOCHRONOUS = the channel.
5. Write ENABLE = 1. Packets start immediately.

**Stop:** ENABLE = 0, TX[0].ISOCHRONOUS = `0xFFFFFFFF`, CLOCK_SELECT back to what you read at the start, and wait (about 0.1 s measured, 1 s limit) for the clock to lock again so
the next user, such as `snd-dice`, finds it settled. Free the isochronous resources.

**Duplex (receive and transmit streams together):** also allocate a second channel and write RX[0].ISOCHRONOUS = that channel *before* ENABLE = 1; clear it on stop.

**If your program dies** the registers stay as they were: the radio keeps streaming with ENABLE = 1 on a channel the kernel has since freed. A new start sees ENABLE != 0.
Fireside SDR clears such a leftover only when the stream on the radio uses exactly the channel its own receiver has just been given (so nobody else can own it), and otherwise
refuses. Register the same recovery in your own program, and close the streams on SIGINT, SIGTERM and SIGHUP.

## Receive packets (radio -> host)
One isochronous packet per bus cycle (8000/s), IEC 61883-6 AM824 in blocking mode: three cycles in four carry data blocks, the fourth carries a CIP header only.

CIP header (two big-endian quadlets): `q0 = SID << 24 | DBS << 16 | DBC`, `q1 = 0x8000_0000 | FMT << 24 | FDF << 16 | SYT`. FMT is `0x10` (AM824).

| Rate | FDF | DBS (quadlets per block) | Blocks per data packet | Layout |
|---|---|---|---|---|
| 48 kHz | `0x02` | 9 | 8 | one sample per channel per block |
| 96 kHz | `0x04` | 9 | 16 | one sample per channel per block |
| 192 kHz | `0x04` | 17 | 16 | **dual-wire**: two samples per channel per block, so blocks run at 96 kHz |

- An empty packet is 8 bytes (CIP only) with FDF `0xFF`, SYT `0xFFFF`. DBC counts data blocks modulo 256 continuously across packets; an empty packet carries the DBC of the next data packet.
- Each block holds the audio channels in order and then the MIDI quadlet. Audio quadlet: label `0x40` in the top byte, **24-bit two's-complement sample** in the low 24 bits
  (convert with a sign-extend and a divide by 2^23). MIDI quadlet: label `0x80`, `0x80000000` when there is no data.
- **Dual-wire (192 kHz):** slot `2c` is channel *c* at time *t* and slot `2c+1` is channel *c* at time *t+1*; slot 16 is the MIDI quadlet. So I is slots 0 and 1, Q is slots 2 and 3.
  The wrong pairing gives an I/Q correlation of 0.79 and a false image; the right one gives about 0.
- Measured: no missing cycles, DBC continuous in every packet, I/Q image 42-43 dB down.

## Channel map
**Radio -> host (receive stream)**
| Channel | Content | Status |
|---|---|---|
| 0, 1 | RX1 **I** and **Q** | Verified |
| 2, 3 | silent in receive; the microphone L/R in transmit; RX2 I/Q if an RX2 board is fitted | Reported |
| 4 - 7 | unused (about -88 dBFS) | Verified |

**Host -> radio (playback stream)**
| Channel | Jack / path | Status |
|---|---|---|
| 0, 1 | **transmit I/Q** (the exciter). Must be exactly zero whenever you are not transmitting | Verified |
| 2, 3 | headphones (R, L) | Reported |
| 4, 5 | external speaker, **R then L** | Verified |
| 6 | RCA line out and FlexWire (mono) | Reported |
| 7 | internal speaker (mono) | Reported |

Input selection (mic, line-in, balanced) and the output levels are radio-side codec registers set through opcodes, not host mixer controls.

## I/Q conventions (**Verified** on the air)
- **Receive:** the I/Q must be mirrored: build the complex sample as `I - jQ` (conjugate of `I + jQ`) before demodulating, or the sidebands are swapped.
- **Receive IF offset:** the DDS is set 9 kHz below the dial so DC and the radio's own artefacts fall off the signal of interest. A carrier on the dial appears 9 kHz from
  the centre of the raw I/Q (observed at -9 kHz in the raw samples). Software shifts it back.
- **Transmit:** the same mirror applies: send Q negated. With that, a USB tone lands above the transmit frequency.
- **One DDS:** receiver and exciter share it. Before keying, put the DDS on the dial (no IF offset) and put it back 9 kHz below after unkeying; otherwise the transmission goes out 9 kHz low.
  The radio mutes its receiver while keyed, so the receive spectrum shows nothing during an over.

## Transmit packets (host -> radio, duplex)
Same layout as receive (AM824, 8 audio + 1 MIDI, DBS 9 or 17, FDF as in the table, blocking mode, MIDI quadlet `0x80000000`). Audio quadlet: `0x40000000 | (sample_s32 >> 8)`, where the
sample is full-scale signed 32-bit. The CIP `SID` is your local node id (`& 0x3F`).

**Media clock: replay the receive timing.** The radio generates the clock, so do not invent one. For each packet received at cycle *r* with *N* data blocks and presentation time `SYT = s`,
send a packet at cycle *r + L* with *N* blocks and the SYT moved on by the same *L* cycles. Cycles with nothing to send get an empty packet. If you fall behind, drop packets and count them.
This is what `snd-dice` does ("sequence replay") and it **Verified** on the air: the radio plays your stream (heard on the speakers), and transmit worked with receive running through the over.

Never feed transmit I/Q unless a keyed-state check and a level limit sit between your source and the stream: see the transmit page (to be written) and `app/core/include/fireside/tx_gate.h`.

## Health checks worth copying
- **Stream-loss watchdog:** if the receive channels are exactly zero for more than a second, the stream has stopped; restart it.
- **Do not decode at the rate you hoped for:** read the packet's FDF and DBS, not just the rate you asked for, and reject packets that are not whole numbers of blocks.
- **Throughput:** a parser that handles one sample at a time in a scripting language cannot keep up at 192 kHz (it managed about 150 kHz). Use a dedicated thread and native code.
