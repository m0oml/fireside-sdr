# Start-up sequence and calibration EEPROM (from PowerSDR 2.7.2 source)

Written 2026-11-14. Sources: `upstream/powersdr-2.7.2/Source/Console/` (`console.cs`, `FWC/fwc.cs`,
`FWC/fwc_eeprom.cs`, `FWC/fwc_atu.cs`, `checksum.cs`); cross-checked against the KE9NS 2.8.0 tree
(opcodes identical, EEPROM layout identical). Implemented in `tools/firesidectl/startup.py` and `eeprom.py`.
**Not yet run against a real radio. Review the write sequence before the first power-on (see bottom).**

## 1. What PowerSDR does, in order
| Step | PowerSDR (console.cs) | Radio traffic |
|---|---|---|
| Enumerate | `PalManager.Init`, `RadiosAvailable.ScanPal` (CommonPal.Scan) | `GetNumDevices`, `GetDeviceInfo` -> GET_MODEL (1221), GET_SERIAL_NUM (1220) |
| Model | model code **0=5000A, 1=5000C, 2=5000 variant, 3=FLEX-3000**; anything but 3 is a 5000 | read-only |
| Identify | `FWCEEPROM.Init()` (fwc_eeprom.cs:201) | GET_SERIAL_NUM, GET_{TRX,PA,RFIO,RX2,ATU,VU}_{OK,REV,SN}, EEPROM header reads (cal date @0x40/0x44, flag @0x3F, image ver @0x1BE), GET_REGION; ATU: drain result buffer |
| Extended | `FWC.GetStatus()` after init | GET_STATUS |
| Firmware | `InitRadio()` (console.cs:12056): GET_FIRMWARE_REV; 0 = comms error ("starting PowerSDR too quickly after powering on the radio can cause this"); must be in **v2.1.4.8 .. v2.1.255.255** else PowerSDR forces a firmware update | read-only |
| Init | `FWC.Initialize()` then `SetTRXPreamp(false)` | INITIALIZE (1219, i32,i32 = 0,0), SET_TRX_PREAMP (1247) = 0 |
| Power on | `chkPower_CheckedChanged` (console.cs:52722) | SET_STANDBY (1320) = 0; SET_RX1_ANT (1278) = 1 (ANT1); driver `SetBufferSize` (no mailbox traffic; = ALSA period size on Linux); SET_RX1_FREQ_TW (1347) (tw, f32 MHz); sleep 100 ms; start audio |
| Power off | same handler, else-branch | MOX off (SET_MOX 0), SET_STANDBY (1320) = 1, stop audio |

The "receive mux" in the power-on path (`Mixer.SetMux`) is the **Windows mixer interface to the driver**; it is not sent as a
startup command. The radio's analogue input/output selection and levels are codec registers (`WRITE_CODEC_REG`), which an app only needs
when it wants the radio's own headphone/speaker/line outputs or a non-default microphone input (see audio-stream-layout.md section 6).
For first light (RX to the PC's speakers) none of this is needed.

Firmware older than v2.x does **not** show up through PAL at all (`RadiosAvailable.NumPresent == 0`);
PowerSDR then finds it over MIDI (`FWCMidi.Open`, `GetModelMidi`) and insists on an update. So:
if `identify` reports firmware < 2.1.4.8 (or nothing), the radio needs a firmware update from the Windows
VM (`software/ke9ns/F5K_firmware_old.zip` has the 1.3.0.8 updater and driver 3.5.0.7171; the 2.7.2 installer
carries the newer firmware). firesidectl never flashes anything.

### Tuning
`tw = (uint)(0xFFFFFFFF * f_MHz / 500.0)` and the radio gets `SET_RX1_FREQ_TW(tw, (float)f_MHz)`
(console.cs `Freq2TW`, `FWCDDSFreq`). 500.0 is `fwc_corrected_dds_clock` (user correction defaults to 0).
The float MHz value lets the firmware choose the band filters itself: **PowerSDR never sends
SET_RX1_FILTER/SET_MANUAL_RX1_FILTER during normal operation.** PowerSDR also has a "spur reduction" mode:
it snaps the tuning word to a 16-bit grid (`tw & 0xFFFF0000`) and shifts the remainder in the DSP
oscillator; that is an optimisation for later. VFO offset / IF shift are ignored (zero) for now.
RX1 antenna numbers (`SetRX1Ant`): 0 N/C, 1 ANT1, 2 ANT2, 3 ANT3, 4 RX1 IN (more for sig-gen/VHF/UHF).

### Deliberately not sent
PA bias, any EEPROM write, calibration sequences (the only `SetQSD/SetQSE/SetTR/SetSig/SetGen/SetTest` calls
are in calibration routines), MOX/TUNE, ATU tune commands, fan/PWM, RCA/amp keying, VU-module relays.

### ATU (user believes the donated radio has one)
`ATU_SEND_CMD (b1<<8|b2, b3)` and `ATU_GET_RESULT (timeout_ms)` form a small command/response buffer;
a result is 4 bytes `cmd, b2, b3, b4` with **b4 = bytes still queued**. `FlushBuffer(false)` = read with
timeout 0 until b4 == 0 (that is what `flush_atu` does). Requests: 0 no-op, 1/2 inductor up/down, 3/4 cap up/down,
5 memory tune, 6 full tune, 8/9 Hi/Lo-Z, 10/11 antenna, 40 all-update, 41 version, 44 standby, 45 active,
46 store, 50-56 SWR threshold. Tune commands transmit; they are dangerous-gated.

## 2. Calibration EEPROM (TRX board; RX2 has its own with the same low layout)
Backup tool: `./firesidectl-cli eeprom-dump` (read-only; reads twice and requires identical results; writes
`backups/F5K_<serial>_<utc>_trx.bin`, `.json` (decoded + sha256) and PowerSDR's own `F5K_<serial> backup.csv`
format; files are mode 0444 and never overwritten).

PowerSDR's own Dump(): 0xC1 rows x 16 = **3088 bytes**, 32-bit reads (low byte first), file named by
the serial at 0x18. We match that. RX2 EEPROM size is unknown; we read 0x200 bytes, which covers all documented RX2 fields.

Header: u32 serial @0x18 | 0xC5 "checksums present" @0x3F | FILETIME last-cal hi @0x40, lo @0x44 |
u16 pointers @0x48..0x62 | image version byte @0x1BE.

| Table | pointer @ | data @ | shape (float32 LE) | checksum @ | plausible range (PowerSDR SanityCheck) |
|---|---|---|---|---|---|
| RX level | 0x48 | 0x100 | 11 bands x 3 (display, preamp, meter) | 0x18F | display -60+-5, preamp -14+-2 (-7 on 160m), meter -33+-5 |
| RX image gain / phase | 0x4A / 0x4C | 0x190 / 0x1C0 | 11 | 0x1BF / 0x1EF | abs < 500 |
| TX image gain / phase | 0x4E / 0x50 | 0x1F0 / 0x220 | 11 | 0x21F / 0x24F | abs < 500 |
| PA bias | 0x54 | 0x2C0 | 2 x u32 (more?) | 0x27E | non-zero, not 0xFFFFFFFF |
| PA bridge | 0x56 | 0x2E0 | 11 x 6 | 0x3EF | 0..2.5 |
| PA power | 0x58 | 0x3F0 | 11 x 13 | 0x62F | 0..1 |
| PA SWR | 0x5A | 0x630 | 11 | 0x65F | 0.5..5 |
| ATU SWR | 0x5C | 0x660 | 11 | 0x68F | 0.5..5 |
| VU level / power | 0x5E / 0x60 | 0x690 / 0x6A0 | 4 / 48 | 0x68E / 0x68D | VU module only |
| TX carrier | 0x62 | 0x760 | entries (u32 vita_hi, u32 vita_lo, u32 value) until 0xDEADBEEF | 0x27F | f = ((hi<<32)+lo) * 2^-20 * 1e-6 MHz |

Bands in order: 160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6 m. Checksum = XOR of all raw bytes of the table
(`checksum.cs Calc(byte[])`). PA bias, VU and TX-carrier use different rules and are copied but not verified.

**Hazards in PowerSDR itself:**
- `FWCEEPROM.CheckAll()` (and every `Check*`) **writes random floats into the calibration EEPROM** as a self-test. It is
  defined but never called by the normal program. `SanityCheckAll()` is read-only and is what the factory test form uses.
- RDAL_OP_SET_EEPROM_WC (1336) is the EEPROM write-control line; firesidectl treats every write path as dangerous.
- The "FLEX-5000 Final Test / Prod Test / LLHW / Debug" forms under `Console/FWC/` are factory tools with raw register,
  EEPROM and bias access. Never open them on this radio.
- PowerSDR's FWCEEPROM.TX carrier sanity check has a C# bug (`vita_high << 32` on a uint is a no-op); we combine the words properly.

## 3. Bugs found in our own tooling while doing this (all fixed, covered by tests)
- Opcode table: `READ_*_EEPROM_UINT32` was typed as float result because PowerSDR also reads it via ReadOp2.
  Would have made the backup tool reinterpret raw words as floats. Generator now defaults to u32 unless float-only.
- Simulator returned model 3 for a "5000"; real 5000A is model 0 (3 is the FLEX-3000). Firmware default changed to 2.1.4.8.
- CLI printed a Python traceback on an out-of-range frequency.

## 4. Review before the first real power-on: DONE 2026-11-14, see startup-review.md (approved with conditions, now enforced)
1. `INITIALIZE (0,0)`: PowerSDR calls it every start, but its firmware effect is unknown (cannot be read from source). Order matters little but it should run first.
2. Whether SET_RX1_FREQ_TW before audio streaming is needed/harmful (PowerSDR does standby-off, antenna, frequency, then audio).
3. `SET_STANDBY` polarity (PowerSDR: `SetStandby(false)` when powering on) and `SET_MOX 0` at shutdown.
4. Byte order of the FireWire mailbox (see analysis/pal-mailbox-protocol.md) and that MIDI route answers all of these opcodes.
5. First hardware session plan: `list` -> `identify` (read-only) -> `eeprom-dump` (read-only) -> only then `startup --dry-run` and `startup --allow-write`.
