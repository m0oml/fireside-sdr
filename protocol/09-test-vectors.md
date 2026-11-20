# 9. Test vectors

Byte-exact examples you can check your implementation against, in [`test-vectors.json`](test-vectors.json). All hex is upper case, no separators, bytes in wire order. They are verified two ways: against the C++ code
(`app/tests/test_vectors.cpp`) and against an independent re-implementation of the rules on these pages (`tools/tests/test_vectors.py`), so a disagreement means a bug in your code or in the page. Please tell us which.

They test formats and arithmetic, not a radio. They contain nothing from any radio's calibration.

## Mailbox command blocks (page 2)
Write at `0xFFFF_E000_1000`, one 12-byte block, big-endian quadlets.
| Command | Block |
|---|---|
| read `GET_FIRMWARE_REV` (1200), data 0, 0 | `800004B0 00000000 00000000` |
| write `SET_STANDBY` (1320), data 1, 0 | `80000528 00000001 00000000` |

A result of `0x0201040C` is firmware **2.1.4.12**.

## MIDI SysEx frames (page 2)
| Request | Frame (37 bytes) |
|---|---|
| id 1, `GET_FIRMWARE_REV` (1200), 0, 0 | `F0000041 3031303030303030423030343030303030303030303030303030303030303030 F7` (the 32 ASCII characters are `01000000B00400000000000000000000`) |
| id `0x1234`, `SET_STANDBY` (1320), 1, 0 | in `test-vectors.json` (`sysex_standby`) |

## Start-up numbers (page 5)
- `tuning_word(14.074 MHz) = (uint32)(0xFFFFFFFF * 14.074 / 500.0) = 0x0734B513`; the f32 sent beside it is `0x41612F1B`.
- Serial: raw `0x11D71701` decodes to `0123-4567` (byte 0 = 01, byte 1 = 23, upper 16 bits = 4567).
- Firmware: `0x0201040C` is `2.1.4.12`.
- EEPROM table checksum: the XOR of the bytes `01 02 04 08` is `0F`.

## PA bias pots (page 6)
EEPROM row 0 `EB 7A E8 78 EB 7F E5 7C` gives pot values `EB 85 E8 87 EB 80 E5 83` (even index: the byte; odd index: 255 minus the byte). This is example data, not any radio's.

## Isochronous packets (page 3)
8 channels at 48 kHz, source node 1, FDF `0x02`, SYT `0x1234`:
| Packet | CIP header (8 bytes) |
|---|---|
| data, 8 blocks, DBC 0 | `01090000 90021234` |
| data, 8 blocks, DBC 8 | `01090008 90021234` |
| NO-DATA after 16 blocks | `01090010 90FFFFFF` |

Audio quadlets (`0x40000000` | the top 24 bits of the S32 sample): `0x12345678` gives `40123456`; `0xFFFFFF00` gives `40FFFFFF`; `0x80000000` gives `40800000`. The MIDI quadlet with no data is `80000000`.

Dual-wire 192 kHz block (DBS 17): quadlets `40000001 40000002 40000003 40000004` then twelve `40000000`, then `80000000`. Channel 0 is the samples 1 (at time *t*) and 2 (*t*+1); channel 1 is 3 and 4.

## ATU response words (page 6)
| Word | Meaning |
|---|---|
| `00FFFFFF` | nothing pending |
| `00001D01` | inductor relay value 29 |
| `00A60205` | forward power 6.78 W ((2 << 8) + 166) / 100 |

## PA meter (page 6)
Example table (volts at 1, 2, 5, 10, 20, 90 W): `0.2 0.3 0.45 0.65 0.9 1.9`, reflected-power factor 0.5. Counts 1000 forward and 300 reflected: forward 8.8643 W, reflected 0.4191 W, SWR 1.5557
(`volts = counts / 4096 * 2.5`; power interpolated linearly in volts squared between the points).
