# 8. Porting guide: implementing this in your own software

A suggested order of work for another project (another language, another OS, a different SDR program) that wants to drive a FLEX-5000. Each level is useful on its own and safe to stop at.
The level boundaries are where the risk changes, so do not skip ahead: each level needs the one before.

Machine-readable inputs: [`opcodes.json`](opcodes.json) / [`opcodes.csv`](opcodes.csv) and [`test-vectors.json`](test-vectors.json) ([page 9](09-test-vectors.md)).

## Level 0: look, don't touch (reads only)
1. Open the radio's FireWire node and do plain asynchronous reads and block writes at 48-bit offsets (on Linux: `/dev/fwN` through firewire-cdev; other systems: their 1394 stack's equivalent).
2. Read the config ROM and find the node. The vendor ID is `0x001C2D`. **Do not write anything yet.**
3. Read the DICE header and the global, TX and RX section offsets ([page 3](03-streams.md)): a plausible layout (global below `0x1000`, TX and RX each one stream) tells you it is the right device.
4. Send `GET_FIRMWARE_REV` through the mailbox ([page 1](01-overview.md)) and check the byte order and the range.
5. Run the identification reads ([page 5](05-startup-and-eeprom.md) section 1). **Done when** your output matches the radio's serial plate and installed boards.

## Level 1: back up the EEPROM (reads only)
Read the whole 3088-byte TRX image twice with `READ_TRX_EEPROM_UINT32`, require the two reads to match, decode the header and tables, verify the checksums, and store it somewhere safe and read-only.
Do this before you write anything else, even if you never intend to transmit: if something later goes wrong, the backup is the only way back to a calibrated radio.

## Level 2: receive
1. The five start-up writes ([page 5](05-startup-and-eeprom.md) section 2), each confirmed by waiting for the mailbox to go idle.
2. Choose between `snd-dice` through ALSA (simple, 48 kHz, works with any audio library) and starting the streams yourself ([page 3](03-streams.md): 96 and 192 kHz, no kernel driver involved).
3. Unpack the audio, take channel 0 and 1 as I and Q, apply the **mirror** and the IF offset, and demodulate. Check on a known carrier that sidebands land on the right side.
4. Add the stream-loss watchdog (all-zero channels for a second means restart).
5. Shut down properly, restoring the DICE registers if you started the streams yourself ([page 5](05-startup-and-eeprom.md) section 3, [page 3](03-streams.md)).
**Done when** a known station sounds right on the right sideband, the receiver survives a USB or cable pull, and the radio is left as you found it after a normal exit and after a killed process.

## Level 3: transmit (read [page 6](06-transmit-and-atu.md) twice first)
1. Write only against the simulator until you trust your unkey path: Fireside SDR's `--simulate` mode and `app/core/src/sim_radio.cpp` implement a fake mailbox, EEPROM, ATU and PA meter you can point your code at (see also `app/core/include/fireside/sim_bus.h`).
2. Build the safety layer **before** the transmit path: a single owner of `SET_MOX`, a deadline, unkey on every error and exit, a hard amplitude cap in the sample path, a ramp, meter limits.
3. PA bias from the EEPROM, confirmed pot by pot. Then the transmit setup writes.
4. First transmission: low power into a dummy load or a proper antenna, ramping up, with the unkey tested first, on a frequency where nothing is listening for you. Calibrate drive per band.
5. Only then the ATU, then modes.

## Pitfalls that cost us time
- **Byte order:** the mailbox is big-endian; the SysEx payload is little-endian; the EEPROM image is little-endian; I/Q samples are 24-bit signed in the low three bytes. The firmware revision check catches a wrong mailbox byte order.
- **Wait for idle before every command.** Do not send writes while the mailbox is busy.
- **Dual-wire at 192 kHz:** pair slots as `2c` and `2c+1` (time `t` and `t+1`); the other pairing produces a false image 87 kHz away.
- **The I/Q mirror applies to both directions.** If your transmit signal is in the wrong sideband, you forgot it on Q.
- **One DDS:** the receive IF offset must be removed from the DDS before keying and restored after.
- **The EEPROM power table reads low**, and the PA bridge SWR reads high at low mismatch. Calibrate; do not trust the tables blindly.
- **Starting the streams yourself:** only the ENABLE, the clock select and the two ISOCHRONOUS fields change; a program that crashes leaves the radio streaming and must be recoverable on the next start.
- **PowerSDR on the same radio** can clear the EEPROM checksum flag; compare against your backup afterwards.
- **Bursts of unacknowledged writes made the radio's reads go stale;** the confirmed, one-at-a-time form is what works.

## What we do not know
SysEx for everything except identification, the notification bits, the DICE RX-stream behaviour when a second host writes it, SSB voice and CW on the air, the VU module, RX2, and everything about other units or firmware.
If you learn something, please say so; the labels on these pages ("Verified", "Reported", "Unknown") are meant to be changed as evidence arrives.

## Licence note
This documentation is part of a GPL-3.0-or-later project ([LICENSE](../LICENSE)). It was written for interoperability. It is unofficial and not endorsed by FlexRadio; see [NOTICE](../NOTICE).
