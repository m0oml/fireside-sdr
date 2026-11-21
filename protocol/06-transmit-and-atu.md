# 6. Transmit and the ATU

How a FLEX-5000 is made to transmit, and what any implementation must do so that it does not damage the radio or splatter. Read all of it before you key anything.

> **You are responsible for your licence, your antenna and your hardware.** A wrong PA bias, a wrong I/Q amplitude, a missing unkey or a bad antenna can destroy the power amplifier,
> and the radio has no dummy-load or SWR protection that you can rely on from software. Develop against the simulator (`fireside --simulate`, `app/core/src/sim_radio.cpp`) first. Everything here was
> done on one FLEX-5000A with a real antenna at low power (requested powers of 1 to 15 W; meter peaks during ATU tunes reached about 25 W), and **Fireside SDR's limits below are its own cautious choices, not the radio's specification.**

Status labels as on the [index](README.md): **Verified** = done on a real radio with this repository's code; **Reported** = from published open-source code, not exercised here.

## 1. What must be true before the first key-down
1. The radio is identified and the firmware revision is in range ([page 5](05-startup-and-eeprom.md)).
2. You hold a verified **backup of the calibration EEPROM**, and the EEPROM decodes with good checksums. Every number you need for transmit comes from it. If a table is blank or fails its checksum, do not transmit (Fireside SDR refuses).
3. Receive start-up has run, and the receive and transmit streams run in duplex ([page 3](03-streams.md)).
4. The antenna is connected and suitable for the band. A short wire straight into the connector works for low power; the ATU does the matching.
5. A person has asked for the transmission. Nothing in the chain should be able to key without a deliberate action.

## 2. Transmit setup writes (**Verified**)
Receive start-up writes none of these. Do them once before the first over, and confirm each.

**PA bias.** The bias of the output transistors is set by eight digital potentiometers whose state after a power cycle is unknown. A wrong value overheats the PA.
1. Read the PA bias table: its pointer is the `u16` at header `0x54` ([page 5](05-startup-and-eeprom.md)); row 0 is **eight bytes** `B0 .. B7` (on this radio at `0x2C0`).
2. Refuse if any of the eight bytes is `0x00` or `0xFF`: that radio was never biased.
3. Pot values: **even index `i`: `B[i]`; odd index: `255 - B[i]`.** Example: bytes `EB 7A E8 78 EB 7F E5 7C` give pots `EB 85 E8 87 EB 80 E5 83`.
4. For `i` = 0 to 7: `PA_POT_SET_RDAC` with `data1 = i`, `data2 = pot value`. Wait for the mailbox to go idle after **each**; unacknowledged bursts made the radio's reads go stale. `PA_POT_GET_RDAC` returns the last pot written.
5. Never write anything but these eight values. On shutdown send `SET_PA_BIAS` 0 ([page 5](05-startup-and-eeprom.md)).

**Transmit antenna and mode** (before every over):
| Opcode | data1, data2 | |
|---|---|---|
| `SET_TX_ANT` (1279) | 1..3, 0 | ANT1..ANT3. **Never change while keyed**: the manual warns it can damage the radio |
| `SET_TX_DSP_MODE` (1353) | mode, 0 | `DSPMode`: 0 LSB, 1 USB, 3 CWL, 4 CWU, 5 FM, 6 AM, 7 DIGU, 9 DIGL ... (the radio uses it for transmit frequency limits) |
| `SET_TX_FREQ_TW` (1349) | tuning word, f32 MHz | same tuning word formula as receive ([page 5](05-startup-and-eeprom.md)), **with no IF offset**; see section 4 |
| `SET_TX_DSP_FILTER` (1351) | 200, 3100 | the transmit passband in Hz |
| `SET_TX_OFFSET` (1352) | 0, 0 | |
| `SET_TRX_POT` (1331) | carrier-null word, 0 | the **carrier null** for this frequency: take the entry of the EEPROM TX carrier table whose frequency is nearest, and send its third word. Do it on every transmit-frequency change. Needed for clean SSB; verified with a silent key-down (no measurable carrier leakage) |

Write `SET_TX_ANT` / mode / frequency while **not** keyed, then wait for idle.

## 3. Transmit samples
Transmit I/Q goes to the radio on **output channels 0 and 1** of the host -> radio stream ([page 3](03-streams.md)), at the stream's rate. Rules (**Verified**):
- Q is **negated** (the same mirror as receive). Without it the signal lands on the wrong side of the carrier: a test tone meant for dial + 1 kHz was heard only with the mirror applied.
- Channels 0 and 1 must be **exactly zero whenever the radio is not keyed**, in every buffer. The radio's exciter takes whatever is on them.
- **The amplitude is tiny.** The radio's full 100 W is only about -30 dBFS, and 1 W is around -40 dBFS on the band we measured. A full-scale signal would be far beyond anything the PA can take.
- **Power follows amplitude squared:** `watts = k * drive^2` where `drive` is the peak I/Q amplitude (0..1). `k` differs per radio, per band and per match. On our radio on 20 m it measured
  11 000 to 16 800 W per unit drive^2 depending on the load and the power. The EEPROM "PA power" table (11 bands x 13 points: 1, 2, 5, 10, 20, 30 ... 100 W) gives the amplitude the
  reference software would use after dividing by 2.23; **on this radio that read 5 dB or more low** (its "100 W" drive gave only about 11 to 13 W). Do not trust the table: **calibrate per band by ramping up.**
- Image rejection: the EEPROM tables TX image gain and phase (per band) are applied in software to Q before sending, for SSB sideband suppression.

## 4. Key-down and key-up
One mailbox command keys the transmitter: `SET_MOX` (1292), `data1 = 1` keys, `0` unkeys. **It is the most dangerous opcode there is.**

Sequence for an over (**Verified**):
1. Not keyed. Antenna, mode, TX frequency, carrier null written and confirmed. Samples flowing but exactly zero.
2. **Receiver and exciter share one DDS.** While receiving, the DDS sits 9 kHz below the dial ([page 3](03-streams.md)); transmitting from there puts the signal 9 kHz low. Before key-down, put the DDS on the dial (`SET_RX1_FREQ_TW` and the TX frequency), wait for idle.
3. `SET_MOX` 1.
4. Start the transmit samples at a low level, **ramp up** (section 5), hold, and ramp down.
5. `SET_MOX` 0. Wait for idle. Put the DDS 9 kHz below the dial again for receive. The radio mutes its receiver while keyed, so the receive spectrum is blank for the over.
6. If **anything** fails, send `SET_MOX` 0 first and diagnose afterwards.

For a **tune carrier** (ATU tune) PowerSDR sets mode CWU and puts the TX frequency one CW pitch (600 Hz) below the dial before keying, and restores USB and the dial after. For **digital modes** the tone sits in the audio
passband above the dial and the dial itself carries no offset.

## 5. Safety design every implementation should have
These are the checks Fireside SDR enforces. Do not omit them; do not copy the numbers without checking them against **your** radio.

1. **A single owner of the key.** Exactly one component may send `SET_MOX` 1; static checks in `tools/tests/test_tune_guard.py` fail the build if any other code can. Keying is **disarmed** until the user arms it, and a disarm always unkeys.
2. **Deadline.** Every key-down has a hard time limit (Fireside SDR: 15 s at most for a tune, a few seconds up to about 20 s for a test over, depending on purpose; a full ATU tune took about 9 s). A timer unkeys when it passes.
3. **Unkey always works.** `SET_MOX` 0 is sent on every error path and when the program exits, and the code verifies it got through. Handle SIGINT, SIGTERM and SIGHUP the same way. A program that is killed cannot unkey: that is why the radio must never be left keyed by a crash-prone caller.
4. **Hard amplitude cap, independent of all other software.** The sample path clamps I and Q to a fixed absolute limit (Fireside SDR: 0.04, about 20 W on this radio) after every other scaling, and is forced to zero unless keyed. A bug upstream cannot exceed it.
5. **Ramp the first key-down.** Start about ten times below the 1 W drive, raise the drive by about 2 dB every 200 ms, and read the power meter between steps. Abort if there is no forward power by the 1 W drive point (wrong channel, sign or path), if power is above the target times a margin, if reflected power or SWR is above limit, or at the deadline. The meter lags the drive by about one reading: wait at least 0.15 s after a step and never step past a predicted overshoot.
6. **Power and reflected power limits.** The gate unkeys above a forward-power limit (Fireside SDR: 20 W) and above an SWR limit (5, only once there is enough power to judge, at least 2 W). During an ATU tune, where SWR is no guide, fixed limits apply: forward at most 30 W, reflected at most 20 W, 15 s.
7. **Amateur bands only.** Refuse to key outside the allocations the radio's PA bands cover (the 11 bands in the EEPROM tables). Your own licence is narrower than that; checking it is yours.
8. **Transmit only for a person who asked,** after listening to the frequency.

## 6. Reading the meters (**Verified**)
`READ_PA_ADC` (1274): `data1` = channel. **Channel 7 = forward, channel 6 = reflected**, 12-bit counts over a 2.5 V reference: `volts = counts / 4096 * 2.5`.
The EEPROM "PA bridge" table (11 bands x 6) holds the detector volts at 1, 2, 5, 10, 20 and 90 W per band; interpolate **power linearly in volts squared** between the points (extrapolate above 90 W). The PA SWR table is a per-band factor applied to the **reflected power only** (about 0.46 to 1.74 here): it is **not** an SWR value.
`SWR = (1 + q) / (1 - q)` with `q = sqrt(reflected / forward)`. Below about 2 counts there is no reading; below 1 W the SWR is not meaningful.

**Do not trust the PA bridge SWR at low mismatch.** On our radio it read 1.5 to 2.0 across 3 to 15 W where the ATU's own meter reported 0 W reflected: a detector leakage that follows the reference software's formula. Use it as a backstop for gross faults,
and the ATU's own report when a tuner is fitted.

## 7. The ATU (**Verified**)
The internal tuner is controlled with `ATU_SEND_CMD` and `ATU_GET_RESULT` (command format on [page 5](05-startup-and-eeprom.md)). Requests Fireside SDR uses: `NOOP` 0, `IND_UP`/`IND_DN` 1/2, `CAP_UP`/`CAP_DN` 3/4, `MEM_TUNE` 5, **`FULL_TUNE` 6**,
`HIZ`/`LOZ` 8/9, `ANT1` 10, `ALL_UPDATE` 40, `VERSION` 41, `STANDBY`/`ACTIVE` 44/45, `MANUAL_STORE` 46, SWR thresholds `SWR11 .. SWR30` 50 to 56 (1.1, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0), `AUTO_ON`/`OFF` 58/59, power and SWR reports 60 to 62,
`UPDATE_ON`/`OFF` 63/64, `SET_IND` 65, `SET_CAP` 66.

Responses (code in the low byte): 1 inductor, 2 capacitor, 3 Hi/Lo-Z, 4 antenna, 5 forward power, 6 SWR, 7 TX frequency, **9 tune passed, 10 tune failed** (reason: 0 no RF, 1 carrier lost, 2 SWR too high), 11 version, 12 clear done, 13 standby, 14 active, 15 store ok, 16 SWR threshold, 17 auto status, 18 reverse power, 19 update status.

Decoding a response word (four bytes, most significant first: queued, `b3`, `b2`, code; `0x00FFFFFF` = nothing pending):
- codes 1, 2, 3, 4 (inductor, capacitor, Hi/Lo-Z, antenna): the value is `b2`.
- code 5 forward power and code 18 reverse power: `watts = ((b2 << 8) + b3) * 0.01`. Example: word `0x00A60205` is 6.78 W.
- code 6 SWR: `ro = sqrt(b3 / 256)`, `SWR = (1 + ro) / (1 - ro)`.
- code 7 TX frequency: `p = (b2 << 8) + b3`; `MHz = 32768 / (p * 1.6)` when `p > 0`.
- code 10 tune failed: `b2` is the reason (0 no RF, 1 carrier lost, 2 SWR too high). Code 11 version: `(b3 >> 4) + (b3 & 15) * 0.1`.

Facts learned the hard way:
- **`SET_IND` carries Hi-Z in bit 7** (`value | 0x80`). A separate Hi-Z request after `SET_IND` was ignored. Setting the relays needs no RF, and you should read them back before keying.
- **Send the SWR threshold before every tune.**
- **Tune from the band's stored relays.** From cleared relays (0/0/Lo-Z) the search passes near short and open circuits; readings were about four times the drive and meaningless, and two 5 W attempts hit our power limits. Keep a per-band memory of the relays that last passed.
- A full tune needs a **carrier** (a steady tone on the I/Q stream) and the key held, takes **6 to 10 s**, and then reports pass or fail. Low power is enough: 3, 5 and 10 W requested all passed into the same match; we default to 3 W.
- After the tune, read the relays (and the tuner's forward, reflected and SWR) and store them. Restoring a stored tune needs no RF.
- The ATU's own SWR table in the EEPROM may be blank on a radio whose tuner was never programmed; that does not stop the tuner working.

## 8. The built-in signal generator (receive only) (**Reported**)
The FLEX-5000 has an internal signal generator that can be switched into the receiver in place of the antenna. Its factory RX level calibration (the EEPROM's RX level table, [page 5](05-startup-and-eeprom.md)) was made with it.
Source: PowerSDR 2.7.2 (GPL), `console.cs` `CalibrateLevel()` and Setup > Tests "Enable HW Signal Generator" (`setup.cs`). Fireside SDR does the same for its S-meter check; on a real radio this is not yet verified.

**On** (about 50 ms apart): `SET_FULL_DUPLEX` 1, the generator's frequency as the transmit DDS (`SET_TX_FREQ_TW`), `SET_GEN` 1, `SET_TEST` 1, `SET_SIG` 1.
No antenna command: the relays do the routing (PowerSDR shows the receive antenna as "Sig Gen"). **No** `SET_MOX`, `SET_TR`, `SET_QSE` or PA bias change: nothing is transmitted.
**Off** (PowerSDR's order): `SET_FULL_DUPLEX` 0, `SET_GEN` 0, `SET_TEST` 0, `SET_SIG` 0. Switch it off on every path, including errors: until then the receiver hears the generator, not the antenna.

**Level:** PowerSDR's production test takes the generator as **-24.0 dBm** at the receiver for every FLEX-5000 (`FLEX5000ProdTestForm.cs`, `udLevel`).
**How the RX level table was made**, per band (`CalibrateLevel`, DSB mode, 500 Hz filter, generator at the dial):
- preamp off: the signal-strength meter is read 50 times; meter offset = -24 - reading (the table's third column);
- preamp on: read again; the gain difference is stored negated (second column, about -14 dB);
- the spectrum's peak bin gives the display offset (first column).

**Units, important:** PowerSDR's signal-strength meter (DttSP `sdr.c`, `RXMETER_POST_FILT`) is 10 log10 of the power **summed** over the DSP buffer, and `CalibrateLevel` sets that buffer to 4096 samples.
So the meter column is 10 log10(4096) = **36.1 dB** below what a mean-power reading needs: dBm = (in-passband mean power in dB relative to full scale) + meter offset + 36.1, plus the preamp column while the preamp is on.
**Verified** on one FLEX-5000A, 20 m, 2026-11-21: the generator read -27.2 dBFS (meter offset +3.17 dB); the table's -32.74 + 36.12 = +3.38 dB. The preamp gain measured 14.06 dB against the table's 13.97 dB.
Fireside SDR's check puts the generator 1 kHz inside the receive passband, requires it 30 dB above the antenna's signal at the expected place, measures with the preamp off and on, and keeps the result per band (it never writes the EEPROM).

**Not to copy:** PowerSDR's transmit calibrations (carrier null, TX image) use the same test path with the transmitter running: `SET_TR` 1, `SET_QSE` 1, `SET_SIG` 1, `SET_TEST` 1, `SET_PDRVMON` 1, `SET_GEN` 0, the PWR slider at 100 and a MOX pulse at the end. They are outside this page's receive-only scope.
**Open question:** our notes say the radio has one DDS for receive and transmit in normal operation, yet in full duplex the generator follows the transmit DDS while receive stays put. Both may be true (full duplex enabling a second path); not verified.

## 9. What has been verified on the air
FT8 from the app at 5 W on 20 m, received and answered at the far end; test tones at 2 to 15 W with the receive stream running through the over; ATU tunes at 3, 5 and 10 W on 20 m and another on 40 m; TX I/Q sign confirmed on a WebSDR.
Not exercised: SSB voice, CW keying, anything above about 20 W, an amplifier on the rear keying lines (`SET_AMP_TX1` and friends), and the transverter and VU module outputs.
