# Transmit, stage 2: how PowerSDR feeds the FLEX-5000's exciter (research, 2026-11-19)

Sources: PowerSDR 2.7.2 source (GPL; `Console/audio.cs`, `console.cs`, `FWC/fwc.cs`, `FWC/FLEX5000DebugForm.cs`), the pal.dll capture of
2026-11-18 (`hardware-day/capture-20261118-powersdr/`), this radio's EEPROM backup, and our own measurements (`audio-stream-layout.md`).
Labels: **[source]** stated by code, **[capture]** seen in the log, **[measured]**, **[OPEN]** to settle on the radio.
Nothing here has transmitted from our app.

## 1. TX I/Q goes out on output ch0 / ch1 (settles the open question of audio-stream-layout.md section 4)
`audio.cs` Callback2 (~4884) [source]: DttSP's *logical* outputs are RX1 L/R on 0/1, TX on 2/3, RX2 on 4/5; the callback then builds the *physical* FLEX-5000
lineup itself ("save off RX1 output since this is where the QSE output info goes"):

| out ch | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| physical | **QSE I** | **QSE Q** | headphone R | headphone L | ext speaker R | ext speaker L | RCA line out + FlexWire | int speaker |
| receive | 0 (cleared) | 0 | RX audio x monitor vol | RX audio x monitor vol | RX audio | RX audio | RX audio | RX audio |
| MOX | **TX DSP L x FWCTXScale** | **TX DSP R x FWCTXScale** | (monitor) | | | | | |

So the two tables never disagreed: one is DttSP's buffer order, the other the jacks, and the callback copies between them. Consistent with the measurement of
2026-11-17 (ch4 = ext speaker R, ch5 = L). **ch0/1 must be exactly zero whenever we are not keyed** (PowerSDR clears them every buffer).
I/Q sign: receive needs *Mirror I/Q*; the TX sign convention is **[OPEN]**: check the first time with a tone on USB that the carrier lands above the TX frequency
(the radio cannot hear itself: listen on a WebSDR, or read the ATU's TX frequency report).

## 2. Drive level: the EEPROM "PA power" table [source + this radio]
`FWCTXScale = high_swr_scale * temp_scale * radio_volume` (audio.cs). For the FLEX-5000 `radio_volume` comes from the power slider through the per-band
PA power table (EEPROM 0x3F0, 11 bands x 13 points at 1, 2, 5, 10, 20, 30 ... 100 W), interpolated in *squared* volts, divided by `audio_volts1 = 2.23`
(console.cs ~54590, branch `new_power_cal`). This radio, 20 m:

| W | 1 | 2 | 5 | 10 | 20 | 30 | 50 | 100 |
|---|---|---|---|---|---|---|---|---|
| table | 0.0116 | 0.0142 | 0.0195 | 0.0254 | 0.0333 | 0.0392 | 0.0495 | 0.0725 |
| / 2.23 | 0.0052 | 0.0064 | 0.0087 | 0.0114 | 0.0149 | 0.0176 | 0.0222 | 0.0325 |

**Full drive is a tiny amplitude: 100 W is about -30 dBFS, 1 W about -46 dBFS.** Our offline TX renderer limits at -1 dBFS: sent to ch0/1 unscaled it
would hammer the PA. Hence the hard cap of section 6. **[OPEN]** which branch the 5000 takes (`new_power_cal`, set from Setup) and the DttSP tune-tone amplitude:
both are settled by measuring, not by trusting the table: the first key-down ramps up from far below 1 W while the PA meter (tx_meter.h) is read.
`high_swr_scale` is PowerSDR's SWR foldback (we unkey instead); `temp_scale` a PA temperature foldback.

## 3. The key-down sequence [capture, 3rd ATU tune]
```
SET_TX_DSP_MODE 4 (CWU)            DSPMode enum: LSB 0, USB 1, DSB 2, CWL 3, CWU 4, FM 5, AM 6, DIGU 7, ...
SET_TX_FREQ_TW  14.3354 MHz         the TX DDS: NO 9 kHz IF offset on transmit; in CWU it sits CW pitch (600 Hz) below the dial (14.336)
SET_MOX 1
  ... ATU / PA meter polling, TX I/Q on ch0/1 ...
SET_MOX 0
SET_TX_DSP_MODE 1 (USB)  ->  SET_TX_FREQ_TW 14.336 (back on the dial)
```
`SET_TX_OFFSET` (DttSP TXOsc, 11025 Hz at start-up, 0 at tune time) and `SET_TX_DSP_FILTER` (low, high) tell the firmware about the TX passband [capture].
PowerSDR generates the TUN carrier as a sine in DttSP (`Audio.TXInputSignal = SINE`) [source].

## 4. Start-up writes PowerSDR makes for transmit that our start-up does NOT (capture, before the first key-down)
| opcode | values | what | needed before TX? |
|---|---|---|---|
| `PA_POT_SET_RDAC` x8 | `EB 85 E8 87 EB 80 E5 83` | **PA bias digital pots**, from EEPROM PA bias (0x2C0: `EB 7A E8 78 EB 7F E5 7C`): **even index = byte, odd index = 255 - byte** (FLEX5000DebugForm.cs: `PAPotSetRDAC(1, 255-v)`) | **YES**. Digital pots: their state after a power cycle is unknown. Wrong bias overheats the PA. Write exactly these, read back if possible |
| `SET_TRX_POT` | `0x807B867E`, `0x807F8880` | TX **carrier null** pots, from the EEPROM TX carrier table, nearest frequency, on every TX frequency change (console.cs ~33171) | for clean SSB (carrier suppression); not for a tune carrier |
| `SET_TX_ANT` | 1 | TX antenna ANT1 | yes (cheap) |
| `SET_AMP_TX1/2/3` | 1 each | which rear RCA amplifier-keying outputs follow MOX | no amplifier here: **[OPEN]** leave as the radio has them; never needed for the tune |
| `SET_CW_PITCH` | 600 | CW pitch | for CW / tune offsets |
| `SET_TX_DSP_FILTER`, `SET_TX_OFFSET`, `SET_TX_DSP_MODE` | as above | TX passband / mode | yes (replay PowerSDR's values) |
| `WRITE_CODEC_REG` x32 | mic/line inputs 0x13..0x16 = 0x80 (muted), output volumes 0x0A..0x0F, mutes 0x07, 0x1B = 2 | the analogue jacks | not for I/Q TX (the I/Q DAC path is not in these registers; ch4/5 played without them) |
The TX image gain / phase tables (EEPROM 0x1F0 / 0x220) are applied in software by DttSP: our TX DSP must apply them per band for SSB image rejection.

## 5. Getting the samples to the radio
- **A. snd-dice ALSA, 48 kHz, duplex (first).** Playback through snd-dice is proven (`firesideplay`, 2026-11-17: tones on ch4/5). Write TX I/Q to ch0/1, keep
  ch2..7 for monitor audio. During a TX session the receiver also runs through snd-dice at 48 kHz (the direct 192 kHz stream and snd-dice cannot share
  the radio). 48 kHz of I/Q is plenty for every mode we transmit. snd-dice does the IEC 61883 timing (SYT, blocking mode, sync to the radio's stream).
- **B. Direct isochronous transmit (later, for 192 kHz panadapter while transmitting).** The radio's RX[0] section takes 8 audio + 1 MIDI channels
  (formation: `Rx 0: 8 ... MIDI 1`): AM824, DBS 9 at 48 kHz / 17 dual-wire at 192 kHz, label 0x40 audio, 0x80 MIDI; RX[0] ISOCHRONOUS at rx+8, SEQ_START at
  rx+0xC. The host must transmit in blocking mode with the data-block count and SYT of each packet derived from the radio's own stream (as ALSA's firewire-lib
  does). A firewire-cdev transmit context next to our receive one; a second DICE register (RX[0] iso channel) for DiceStreamControl.

## 6. Safety design for stage 3 (before the first RF from our app)
1. **PA bias** written from the EEPROM before any key-down (section 4), as its own reviewed step: `PA_POT_SET_RDAC` gets a value-restricted rule in the safety
   gate that allows exactly the 8 values derived from the backup.
2. **Output stage hard cap**, independent of all DSP: the TX output path multiplies by the band's drive scale and then *clamps* I and Q to the amplitude of the
   gate's power limit on that band (from the PA power table, 20 W by default) and never above an absolute 0.04 (about 100 W on 20 m). ch0/1 are forced to
   zero unless the TxGate is keyed, every buffer.
3. **First key-downs ramp**: start at a drive of 1/10 of the 1 W table value, +2 dB every 200 ms, read the PA meter each step; stop at the target (<= 5 W at
   first) or abort on: no forward power by the 1 W drive point (wrong channel / sign / path), SWR > 3, forward power above 1.5 x the target, or the gate's deadline.
4. Transmit only inside amateur allocations (`txm::band_of() >= 0`), on a frequency the user has listened to, the user presses the key.
5. Each key-down logged with drive, readings and outcome (like the capture), kept beside the recordings.

## 7. Stage 3 plan
1. No RF: ATU relays from memory (`Atu::set_relays`), read back. Verifies the direct-set encoding.
2. No RF: PA bias pots from the EEPROM (section 4); read back with PA_POT_GET_RDAC if it reports. Then key **with ch0/1 at zero** for 1 s: forward power must read ~0.
3. Low RF: the ramp of section 6.3 into the antenna with the ATU's last good tune on that band, stop at 2-5 W, then an ATU tune from our app (`run_atu_tune`).
4. Store the band's tune in the ATU memory, change band and back: relays restore with no RF.

## 8. Stage 3 results on the radio (2026-11-19 ~00:30 UTC)
- **Step 1, ATU relays without RF: PASS.** `firesidekey atu-report` -> ind 0 cap 0 hiz 1 (the 20 m tune of the previous evening was gone after a power cycle).
  `REQ_SET_IND` carries **Hi-Z in bit 7** (SET_IND 30 cleared Hi-Z, a separate HIZ request after it was ignored; 30|0x80 set it). `atu-relays 30 7 1`,
  `12 3 0`, `30 7 1` all read back exactly. Per-band ATU memory can restore a tune with no RF.
- **Step 2a, PA bias: PASS.** start-up, the 8 pots from the EEPROM (EB 85 E8 87 EB 80 E5 83, = PowerSDR's), each confirmed; PA_POT_GET_RDAC returns the
  last pot written (0x83); reviewed shutdown.
- **Step 2b, silent key-down: PASS.** 14.336 MHz, USB, ANT1, tuner on 30/7/Hi-Z, carrier null from the EEPROM, ch0/1 exactly zero, 1.0 s through the TxGate:
  forward / reflected ADC 2-3 counts throughout (detector floor, << 1 mW): **no measurable carrier leakage**. Unkeyed by the gate's deadline; stream 0 underruns.
- snd-dice offered only the 96 kHz band ("middle": 8 ch) after PowerSDR's session (it had left the radio at 96 kHz): `firesidekey --rate 96000`.
- Next: step 3, the low-RF drive ramp (section 6.3), the user keying.
- **Step 3, first RF from our app (2026-11-19 ~01:00 UTC), 14.336 MHz USB, +1 kHz tone, tuner 30/7/Hi-Z.** Run 1 stopped at 0.017 W (the no-power rule assumed the
  EEPROM table; the meter is unreliable at mW). Run 2 (clamp at the table's 100 W drive): power followed drive^2 (0.38 / 0.88 / 1.89 / 3.69 W per 2 dB step),
  radio bridge SWR 1.46-1.63, then the 1.5 x target rule unkeyed at 3.69 W after 3.2 s; clean shutdown. **Measured 20 m drive: ~11 100 W per drive^2: 1 W at
  -40.5 dBFS, 5 W at -33.5 dBFS** (the table read as PowerSDR's /2.23 says 1 W at -45.7 dBFS: 5.2 dB low). The meter lags the drive by about one reading:
  the ramp now waits >= 0.15 s after a step, never steps past a predicted 1.25 x target, and aims at the target from 0.3 W.
- **Run 3 (fixed ramp): PASS.** Held 1.89-1.92 W for 0.5 s, SWR (radio bridge) 1.47-1.52, unkeyed "ramp finished at the target" after 4.5 s, 0 underruns.
  Calibration repeated within 1 %: 11 216 W per drive^2 (1 W at -40.5 dBFS on 20 m).
- **ATU tune from our app, 3 W, relays cleared first (0/0/Lo-Z): stopped by our power cap after 3.0 s** (our meter 5.5 W at the 3 W drive, SWR 3.9 while the tuner
  searched; cap was 1.5 x = 4.5 W). The forward reading swings with the mismatch during the search; cap during a tune now 2.5 x the tune power, max 25 W.
- **5 W from cleared relays: stopped by the 2.5 x cap after 1.6 s**: 16 W forward read from the fixed 5 W drive at SWR 11 (reflected leaking into the forward reading
  at extreme mismatch). During tunes the limits are now FIXED (user's choice): forward reading <= 30 W, reflected <= 20 W, 15 s; drive fixed and clamped.
- **5 W again (fixed limits): stopped by reflected 21.3 W > 20 W after 1.9 s**, forward read 17 W from the 5 W drive: an open/short-like load (meaningless readings),
  the tuner reporting relays 0/0 and 0.09 W through its sensor. Both 5 W runs reset the relays inside the tune command right before keying; both 3 W runs had the
  reset a minute earlier. firesidekey now waits 1.5 s after reset + init, reads the relays back before keying (refuses without a report), and logs every meter reading.
- **5 W from the stored relays (30/7/Hi-Z): ATU tune PASS after 9.0 s, the first from our app.** Tuner: ind 29, cap 7, Hi-Z, fwd 6.78 W, rev 0.00 W, SWR 1.00.
  Our meter at the end: 7.0 W fwd / 0.3 W rev (the radio bridge reads ~0.3 W reflected into a 1:1 match: its SWR reads high); peaks during the search 11.9 W fwd,
  8.1 W rev. Saved to ~/.config/fireside/atu-memory.txt; relays read back 29/7/Hi-Z without RF. The "5 W" drive gave 7 W into the match: the ramp calibration
  (11 150 W / drive^2) was taken at a worse match; into 1:1 it is ~15 600. Starting from cleared relays the search drives the readings to ~4 x the drive
  (only ~3 W fits the tune limits); from stored relays it does not.

### ATU tune power comparison, 20 m 14.336 MHz, start from the stored relays 30/7/Hi-Z (2026-11-19 ~01:20-01:30 UTC)
| requested | actual (tuner) | result | time | relays | tuner SWR | search peaks fwd / rev (our meter) |
|---|---|---|---|---|---|---|
| 3 W | 3.9 W | PASS | 7.4 s | 30/7/Hi-Z | 1.00 | 4.9 / 2.3 W |
| 5 W | 6.8 W | PASS | 9.0 s | 29/7/Hi-Z | 1.00 | 11.9 / 8.1 W |
| 10 W | 15.1 W | PASS | 5.8 s | 29/9/Hi-Z | 1.00 | 25.4 / 10.4 W |
- Same match at every power (ind 29-30, cap 7-9, Hi-Z). **Low power tunes this antenna fine**; the user prefers it: `tune` defaults to 3 W (~4 W actual).
- Into a 1:1 match the drive gives more than the ramp calibration (taken at bridge SWR 1.5) and more so at higher power: 14 400 / 15 100 / 16 800 W per drive^2
  at 4 / 7 / 15 W (some PA gain expansion). "15 W requested" would be ~24 W with search peaks ~38 W: not run (the "10 W" run was already PowerSDR's 15 W).
- From cleared relays (0/0/Lo-Z) the search passes near short/open: readings ~4 x the drive, meaningless; one 3 W run completed (FAIL at 1.5, ended 30/6).
  In use a tune should start from the band's stored relays (per-band memory).

## 9. TX I/Q sign settled on the air (2026-11-19, user, GUI Test tone)
App dial 14.180000 MHz, test tone meant for dial + 1 kHz = 14.181 MHz, 1.81 W for 8 s (first transmission from the GUI). On a WebSDR tuned to 14.180 USB:
**heard with Mirror TX I/Q ticked, nothing with it unticked.** Mirrored puts the tone at 14.181 (a 1 kHz note in USB at 14.180); unmirrored at 14.179
(below the dial, removed by USB). So the radio needs the TX I/Q **mirrored, the same convention as receive**. Mirrored is now the default (GUI, firesidekey,
TxControl::Options); `firesidekey --no-mirror-q` only to re-test.

## 10. First FT8 on the air from our app (2026-11-19 ~09:33 UTC, 20 m, 14.074 + 1500 Hz, 5 W)
`CQ M0OML IO91` sent from the GUI (even slot). Answered in the next slot: `093315 -6 0.1 2576 ~ M0OML DK0ULR JN58` (DK0ULR, JN58, -6 dB here):
the transmitted signal decoded correctly at the far end, on time. The decoder kept running after the over (fresh decoder per reopened stream).
The user did not complete the QSO (not replying). Next: reply / auto-sequence from a double-clicked decode.
