# 5. Start-up, shutdown and the calibration EEPROM

What to read and write, in what order, to bring a FLEX-5000 up for receive and put it back to rest. Opcode numbers are in [page 4](04-opcodes.md). Everything below
the "Verified" label was done on a real FLEX-5000A with `app/core/src/startup.cpp`, `identify.cpp` and `eeprom.cpp`; the Python equivalents are in `tools/firesidectl/`.

**Rule that matters most:** nothing in the identification or EEPROM-reading steps writes anything. Do those first, check what comes back, and only then write.

## 1. Identify the radio (read-only, **Verified**)
Open the mailbox ([page 2](02-control-transports.md)) and read, in this order:

| Read | Opcode | Meaning |
|---|---|---|
| firmware | `GET_FIRMWARE_REV` (1200) | `a.b.c.d` in the four bytes. `0` means the radio is still booting: wait and ask again. This firmware range is the one the software the radio was built for accepts: **2.1.4.8 to 2.1.255.255**. Outside it, do not write |
| model | `GET_MODEL` (1221) | 0 = FLEX-5000A, 1 = 5000C, 2 = another 5000 variant, 3 = FLEX-3000 (a different radio; stop) |
| serial | `GET_SERIAL_NUM` (1220) | two bytes as two decimal digits each, then the upper 16 bits as four digits: `bbbb-wwww` (for example `0123-4567`) |
| boards fitted | `GET_{TRX,PA,RFIO,RX2,ATU,VU}_{OK,REV,SN}` (1201-1218) | `OK` is non-zero when the board answers; the REV and SN reads for a board that is not fitted have no meaning (this radio has no RX2 or VU) |
| region, status | `GET_REGION` (1350), `GET_STATUS` (1355) | region enum in page 4; non-zero status = extended capabilities |

A correct firmware answer also proves your byte order is right. If the ATU is fitted, drain its result buffer now (section 4).

## 2. Start-up for receive (**Verified**)
Five writes, then check that the radio took the last one:

| # | Opcode | data1, data2 | Why |
|---|---|---|---|
| 1 | `INITIALIZE` (1219) | 0, 0 | Sent once at start by the reference software. Its firmware effect is unknown |
| 2 | `SET_TRX_PREAMP` (1247) | 0, 0 | preamp off |
| 3 | `SET_STANDBY` (1320) | 0, 0 | leave standby (0 = operating, 1 = standby) |
| 4 | `SET_RX1_ANT` (1278) | antenna, 0 | 0 none, 1 ANT1, 2 ANT2, 3 ANT3, 4 RX1 IN |
| 5 | `SET_RX1_FREQ_TW` (1347) | tuning word, f32 MHz | see below |

Then wait for the mailbox to go idle (the firmware has consumed the last write), wait about 100 ms for the hardware to settle (it may change the sample rate), and start the audio
streams ([page 3](03-streams.md)). The streams can also be started before step 1: the radio produces I/Q in standby, which is how you can look at the bus without writing.

**Tuning.** `tuning_word = (uint32)(0xFFFFFFFF * f_MHz / 500.0)` where 500.0 is the DDS clock in MHz. data2 is the same frequency as an f32 in MHz: the firmware uses it to choose the
band filters itself, so you do **not** need `SET_RX1_FILTER` and friends. To receive with the IF offset ([page 3](03-streams.md)), tune the DDS 9 kHz below the dial.

**Not sent by receive start-up:** PA bias, any EEPROM write, the calibration opcodes (`SET_QSD`, `SET_QSE`, `SET_TR`, `SET_SIG`, `SET_GEN`, `SET_TEST`), `SET_MOX`, ATU tune commands,
fan control and keying lines. Transmit setup, which does write some of these, is the transmit page's subject.

## 3. Shutdown (**Verified**)
1. `SET_MOX` 0, 0: make sure the transmitter is not keyed. Do this first, always, including after an error.
2. `SET_STANDBY` 1, 0.
3. `SET_PA_BIAS` 0, 0: bias off.
4. Stop the streams and restore the DICE registers ([page 3](03-streams.md)).

Wait for the mailbox to go idle after the last write. A program that dies leaves the radio as it was; start-up should therefore never assume a clean state (check ENABLE, and `SET_MOX` 0 first if you ever might have been keyed).

## 4. ATU command buffer (**Verified**)
`ATU_SEND_CMD` (1294) sends a request: `data1 = (request << 8) | arg1`, `data2 = arg2`. `ATU_GET_RESULT` (1295) with `data1 = timeout in ms` returns four bytes, most significant first:
**bytes still queued**, `b3`, `b2`, response code. The value `0x00FFFFFF` means nothing is pending. To drain the buffer after power-up, read with timeout 0 until the queued-bytes byte is 0.

Requests (reported by the reference software; the tune requests transmit RF): 1/2 inductor up/down, 3/4 capacitor up/down, 5 memory tune, **6 full tune**, 8/9 Hi-Z/Lo-Z, 10/11 antenna,
40 update all, 41 version, 44 standby, 45 active, 46 store, 50-56 SWR threshold. Sequencing a tune safely belongs to the transmit page.

## 5. The calibration EEPROM
The radio's factory calibration (receive levels, image rejection, PA bias, power and SWR tables) lives in the TRX board EEPROM. **Back it up before anything else and never write it.**

**Reading** (**Verified**): `READ_TRX_EEPROM_UINT32` (1226), `data1 = byte offset`, returns four bytes, **lowest address in the low byte** (little-endian). The whole image is **3088 bytes** (`0xC1` rows of 16).
Read it twice and require identical results, then store it read-only with a checksum. Fireside SDR's backup tool does exactly this (`firesidectl-cli eeprom-dump`; the app offers it on first start).
The RX2 board has its own EEPROM with the same low layout (`READ_RX2_EEPROM_UINT32`).

**Header:** `u32` serial at `0x18` | **`0xC5` at `0x3F`: "checksums present"** | last-calibration FILETIME high `u32` at `0x40`, low at `0x44` | `u16` table pointers at `0x48 .. 0x62` | image version byte at `0x1BE`.
A byte other than `0xC5` at `0x3F` makes the reference software stop trusting the tables; a blank image reads all `0xFF`.

**Tables** (float32, little-endian, 11 bands in the order 160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6 m; checksum = XOR of all bytes of the table):
| Table | pointer at | data at | shape | checksum at | sane values |
|---|---|---|---|---|---|
| RX level | `0x48` | `0x100` | 11 x 3 (display, preamp, meter) | `0x18F` | display about -60, preamp about -14, meter about -33 |
| RX image gain | `0x4A` | `0x190` | 11 | `0x1BF` | abs < 500 |
| RX image phase | `0x4C` | `0x1C0` | 11 | `0x1EF` | abs < 500 |
| TX image gain | `0x4E` | `0x1F0` | 11 | `0x21F` | abs < 500 |
| TX image phase | `0x50` | `0x220` | 11 | `0x24F` | abs < 500 |
| PA bias | `0x54` | `0x2C0` | `u32` pairs; see the transmit page | `0x27E` | non-zero, not `0xFFFFFFFF` |
| PA bridge | `0x56` | `0x2E0` | 11 x 6 | `0x3EF` | 0 to 2.5 |
| PA power | `0x58` | `0x3F0` | 11 x 13 (1, 2, 5, 10, 20, 30, ... 100 W) | `0x62F` | 0 to 1 |
| PA SWR | `0x5A` | `0x630` | 11 | `0x65F` | 0.2 to 5. A per-band reflected-power correction, **not an SWR**: 0.46 on 6 m is normal |
| ATU SWR | `0x5C` | `0x660` | 11 | `0x68F` | 0.5 to 5; blank on a radio whose ATU was never programmed |
| VU level / power | `0x5E` / `0x60` | `0x690` / `0x6A0` | 4 / 48 | | VU module only |
| TX carrier | `0x62` | `0x760` | `{u32 hi, u32 lo, u32 value}` entries until `0xDEADBEEF` | `0x27F` | frequency = ((hi << 32) + lo) x 2^-20 x 10^-6 MHz |

The PA bias, VU and TX-carrier tables follow different rules and are read, not verified.

### Hazards
- The reference software has a self-test (`CheckAll`) that **writes random values into the calibration EEPROM**. It is never called in normal use and you must never call it or copy it.
  Its factory-test forms expose raw register, EEPROM and bias access: do not use them on a radio you want to keep.
- **The reference software can clear the `0x3F` flag by itself.** We saw it go from `0xC5` to `0xFF` after a run, caused by a timing race in its own checksum reads. Nothing else changed (verified by a full double read against the backup) and
  Fireside SDR's one-off recovery put `0xC5` back. After running that software against your radio, read the header and compare with your backup.
- `SET_EEPROM_WC` (1336) is the write-control line. Do not touch it outside a factory process.
- Firmware flashing is a separate path entirely (the loader window on page 2). Never write it.

## Checking your own implementation
The first session against a real radio should be exactly: open the bus, read the firmware revision, identify, dump the EEPROM twice, compare, store. Only then try the five start-up writes, with the transmitter
unable to key (no antenna needed to receive; do not connect a transmit path at all). Fireside SDR's simulator (`--simulate`, `app/core/src/sim_radio.cpp`) is a fake radio you can point your code at first.
