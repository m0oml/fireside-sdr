# FLEX-5000 control and stream protocol

How to talk to a FlexRadio FLEX-5000 / 5000A over FireWire from your own software. These pages describe what the radio does on the bus; the code
that does it in this repository is named on each page so you can read a working implementation next to the description.

**Status labels.** Every statement is one of:
- **Verified**: seen to work on a real FLEX-5000A (firmware 2.1.4.12) with the code in this repository.
- **Reported**: taken from published open-source code or standards, not exercised by Fireside SDR.
- **Unknown**: stated so you know it is a gap.

This is independent, unofficial interoperability documentation. It is not made or endorsed by FlexRadio (see `NOTICE`). It was written from one radio; other
units, firmware revisions and the FLEX-3000 may differ.

## Pages
| Page | What it covers |
|---|---|
| [1. Overview and a first read](01-overview.md) | The pieces, how they fit, and reading the firmware revision byte by byte |
| [2. Control transports](02-control-transports.md) | The FireWire mailbox (verified) and MIDI SysEx (reported) |
| [3. Stream control and audio format](03-streams.md) | DICE registers, isochronous packet format, channel map, I/Q conventions, receive and duplex transmit |
| [4. Opcode reference](04-opcodes.md) | Every known opcode with types, safety class and what is in use (generated) |
| [5. Start-up, shutdown and the EEPROM](05-startup-and-eeprom.md) | Identification, the five start-up writes, shutdown, the ATU buffer, the calibration EEPROM layout and its hazards |
| [6. Transmit and the ATU](06-transmit-and-atu.md) | PA bias, transmit setup, key-down, drive calibration, the safety checks to build in, meters, the tuner. **Read before keying** |
| [7. Front-panel and key events](07-events.md) | Paddles, PTT, the EVENT register (reported, lightly tested) |
| [8. Porting guide](08-porting-guide.md) | A level-by-level order of work for your own implementation, and the pitfalls we hit |
| [9. Test vectors](09-test-vectors.md) | Byte-exact examples, checked against two implementations |

**Machine-readable:** [`opcodes.json`](opcodes.json), [`opcodes.csv`](opcodes.csv), [`test-vectors.json`](test-vectors.json).

The research notes these pages were written from, with dates and the history of what was tried, stay in `analysis/`.

## Before you write to a radio
A FLEX-5000's factory calibration lives in its EEPROM, and the transmitter can be keyed by one command. Read the opcode safety classes before sending anything
that is not a plain read, back up the EEPROM first, and never write the firmware-loader window (page 2). The Fireside SDR app never writes the EEPROM (one reviewed single-byte recovery tool exists, see page 5).
