# FLEX-5000A on Linux: source findings

Sources cloned into `upstream/`:
- `upstream/powersdr-2.7.2`: github.com/M0LTE/PowerSDR (fork of KE9NS fork of FlexRadio PowerSDR 2.7.2, SVN 5123)
- `upstream/powersdr-ke9ns-2.8.0`: github.com/ke9ns/PowerSDR-KE9NS-v2.8.0
- `upstream/dice.c`: Linux kernel `sound/firewire/dice/dice.c` (snd-dice)


## 2026-11-19 (evening): SWR reading checked; test-tone overshoot fixed
- SWR ~2.0 in every over: our meter is PowerSDR's, line for line (FWCPAPower: same channels 7 fwd / 6 rev, same bridge table and
  square-law interpolation, same SWR formula, reflected x the EEPROM PA SWR factor as FlexRadio's original). The reflected/forward ratio
  was the same 0.119 at 2 W and at 18 W (a detector offset would hit low power far harder): a real mismatch, consistent with the ATU's
  FAILs at 14.210 and 7.135 that afternoon. Not a meter bug.
- 15 W test tone held at 18-20 W: the ramp stepped 2 dB / 0.2 s on a meter that lags ~one step, and started holding at a reading of
  0.8 x target. Now: near the target it waits 0.45 s for the meter and steps to the target in proportion, holds only within +-10 %, and
  trims during the hold. The simulated radio got a meter lag (RadioModel::meter_lag_s); with 0.25 s lag: 2.00 / 5.00 / 14.99 W, no overshoot.

## 2026-11-19 (evening): every over a cycle late; the transmit frequency in duplex
- Log: "queued for 17:25:15 ... FT8 sent at 17:25:45": every over went 30 s late (DA0M answered, our report went late 4 times, they gave up).
  Cause: my fix for the window freezing moved rendering to the moment of sending, and render_ft (FT8 audio through the SSB filter at
  48 kHz + resampling) took 2.7-3.0 s (FT4 1.5 s) > the 2 s late limit. Now the analytic GFSK is generated directly at the stream rate
  (decode::synth_ft_iq: 0.03 s FT8 at 96 kHz, 0.07 s at 192 kHz); all FT transmit tests pass on it; a test keeps it under 0.5 s.
- Duplex: the over went out at the receive IF shift (shared RX1/TX DDS): key hook moves the DDS onto the dial for the over (heard on an
  FT-817 on 40 m); the receiver follows it while keyed.

## 2026-11-19: the 2-3 s after an over was the PC sound output, not the radio
Measured on the air (96 kHz direct): unkey -> playback closed 0.01 s, -> GUI 0.01 s, stream restart 0.14 s: receive was back 1.5 s before the next
slot. The audio was not: ~14 s with nothing pushed lets the sound server suspend the output, and waking it took 2-3 s. AudioOut now writes
silence whenever no receive audio has come for 120 ms (~100 ms queued), so the output never idles. Transmit messages also go to
~/.local/share/fireside/transmit.log. Seen in the same over: 10 W with ref 1.82 W, SWR 2.34 (ATU memory said 1.24 at 5 W).

## 2026-11-19: receive coming back after an over
After an over the receive stream restarts (DICE clock lock waits up to 1 s; TX is 96 kHz ALSA playback, RX was 192 kHz direct: a rate change each
way). The FT8/FT4 decoder used to drop any slot it joined > 0.05 s late, so a restart finishing after the boundary lost the whole answer slot.
Now a slot joined up to 0.4 s late is decoded (silence before; signals start at 0.5 s). The status says "back in X s, Y s before/after the slot
boundary" after each over: if it is often after, try receiving at 96 kHz (no clock change for TX) and compare.

## 2026-11-19 (first CQ test on the air): answers decoded, our report too late
Screenshot: callers answered the CQ, our report went out, they moved on (IQ4JO called SQ6JFJ next). Decode then took 1.95 s on a busy 20 m: the
reply was chosen after receive paused (14.2 s) + the full decode, so it joined ~2 s late or slipped 30 s. Now finish() runs a QUICK pass (one pass, no
deep stage) before the over: 0.40-0.65 s on the 20 m recording, 70-90 % of the slot's decodes; the full decode runs in the background and adds the rest
(Ft8Options::late_lines). The status shows "(before TX x s)". Test: FIRESIDE_FT8_12K=12 kHz float file FIRESIDE_TEST_FILTER=quick_pass.

## 2026-11-19: click to reply, auto sequence, auto log; auto start
- `fireside/qso_seq.*` (pure logic, tests): both standard exchanges, repeats (6; CQ 12), give-up, other callers wait, late double-clicks, one log.
- Found: with Repeat (calling CQ) the slot BEFORE each over was never decoded (receive paused 0.8 s early, the decoder rebuilt): answers were lost.
  Now closeLive() asks the decoder to finish (`TextDecoder::finish`, FT8/FT4 decode the slot so far if its signals are in: >= 14.04 s / 6.44 s).
- Found: `parse(ftx_line_text(line, &snr), snr)` read snr before the call set it (unspecified argument order): our report was +00.
- Decode time measured on the 20 m / 40 m recordings: 1.45 / 1.32 s per slot (16 cores); shown in the decoder status now.
- Start-up: the app starts receiving by itself (Radio tab box), transmit checks the EEPROM by itself; the Arm button is gone (Retry only on failure).

## 2026-11-19: station grid checked
The user's locator worked out from their own position: **IO92ar** (close to the AR/AS subsquare boundary). Wavelog's station profile (IO92AR) is right;
QRZ's record says IO92as (its lat/lon is just that square's centre) and should be corrected on qrz.com; the first CQs sent **IO91**, wrong by a whole square
(~100 km): the FT8 message default (Transmit card, TX lab) and the saved setting are now `CQ M0OML IO92`. 20 m / 40 m 60 s runs with QRZ lookups: 46 / 49
decodes, all but the last in-flight lookup filled (`fireside --decode-lookup`).

## 2026-11-19 (later still): QRZ.com callbook direct
User's choice: callbook data straight from QRZ (XML subscription), Wavelog only for worked-before (its lookup now without callbook=true). Credentials entered
on the Log tab, kept in ~/.config/fireside/qrz.env (0600). Found while testing: the cache reader trimmed the trailing tab of records with an empty last
field (dropped them); "RR73" is a valid Arctic locator (excluded); an old smoke step's lambda outlived its locals (scoped now). Not yet run against the
real QRZ server.

## 2026-11-19 (later): FT8/FT4 transmit tests, fewer questions, callers highlighted
- Tests on 5 x simulated time (`app/tests/test_atu_tx.cpp`, `FtRig`): every QSO message (CQ, grid, report, R-report, RR73, 73, CQ DX) decodes from prepare_ft,
  FT8 and FT4, 48 and 96 kHz, |DT| < 0.1 s, upper sideband; a whole FT8 over at 96 kHz decodes, is mirrored on the wire, starts after the slot start and
  ends inside it, keys once, stays under the gate and the hard clamp; no-mirror puts it above the carrier; the closed loop fixes an 80 % high drive level;
  one 1.7 x low trips the gate (forward > 1.5 x) in < 1.5 s and teaches nothing; SWR going bad mid-over unkeys; joining 1.2 s late stays on time; a dying
  playback device, wrong rate, < 1 W, off-band, disarmed all leave it unkeyed.
- Found: prepare_ft accepted an empty message (would send a blank over): now refused. FT4's resampled envelope peaks at 1.025 (harmless: clamp after).
  The FT8 decoder finds nothing in audio far above +-1 full scale (a test fed it x50): real receive audio is in range, but worth knowing.
- Questions: only Tune and Measure ask now (user's choice). GUI smoke covers Repeat (re-queues the same parity, Stop ends it) and the empty message.
- Decoder: lines to my call (also /P, prefix/, <hashed>) highlighted; ReceiverPanel::callerDecoded -> LogPanel::callerDecoded (fill + lookup, first caller
  wins, no refill within 10 min of logging them). Log smoke covers it against the fake Wavelog.

## 2026-11-18 evening / 2026-11-19: transmit, on the air
PowerSDR run for the ATU sequence (PowerSDR cleared EEPROM 0x3F via a checksum-read race: restored, identical to the backup); transmit research
(TX I/Q on out ch0/1, PA bias pots, drive table, carrier null, image correction); TxGate / ATU / meters / TxControl; firesidekey steps on the radio (relays without RF: Hi-Z = SET_IND
bit 7; bias; silent key-down ~0 W; ramp to 2 W; ATU tunes at 3/5/10 W all pass); GUI Transmit card; TX sign mirrored (WebSDR); FT8/FT4 send with closed-loop power; first FT8 CQ
answered by DK0ULR. Decoder fixed to survive the receive pause during an over. Details: analysis/transmit-stage2.md, app/README.md.

## Where things stand (2026-11-18): the radio is live
Hardware day was 2026-11-17; since then the app runs on the real FLEX-5000A. One-line history of the last two days (details in the dated sections and `analysis/`):
- Radio brought up over FireWire; mirrored I/Q and the radio's 0 Hz hole found, fixed with the 9 kHz IF offset (DDS 9 kHz below the dial); preamp control; PipeWire told to leave the radio's card alone.
- snd-dice hides 96/192 kHz although the radio locks and streams 8 channels at 192 kHz: the app now has its own isochronous receiver (`fw_iso.cpp`), 48/96/192 kHz, verified on air.
- One-click start (rate buttons 48/96/192, Stop radio), remembered Radio-tab settings, zoom, tuning line, UK band plan, spectrum averaging, wheel direction.
- FT8: from 57 % to 94 % of WSJT-X's decodes on live 40 m (signal subtraction, OSD, deep stage with a-priori decoding, contest message types, SNR and DT calibrated to WSJT-X); FT4 checked; my call M0OML.
- Synchronous AM detector (default for AM).
Open items and rules: `HANDOFF.md`. Feature reference: `app/README.md`. Decoder numbers: `analysis/ft8-vs-wsjtx-2026-11-17.md` and `-11-18.md`.

## Architecture (FLEX-5000/3000, "FWC")
| Layer | What | Open? |
|---|---|---|
| GUI / radio logic | C# WinForms (`Console/`) | GPL |
| DSP | DttSP (C) | GPL |
| Radio control API | `Console/FWC/fwc.cs`: every function is `WriteOp/ReadOp(opcode, data1, data2)` | GPL |
| Opcode list | `PowerSDR.Shared/OpCode.cs` (`RDAL_OP_*`, e.g. SET_RX1_FREQ=1244, SET_TX_FREQ=1246, READ_PTT=1266, SET_ANT=1277) | GPL |
| EEPROM / calibration layout | `Console/FWC/fwc_eeprom.cs` | GPL |
| Transport | plain IEEE 1394 asynchronous reads and writes to the DICE register window: see protocol/02-control-transports.md | standard |
| I/Q + audio | Standard DICE isochronous audio, 8 ch, via ASIO/PortAudio (`audio.cs`, callback8) | standard |
| Firmware | DICE ARM firmware in radio | closed (don't need to touch) |

## Control route 1: MIDI SysEx (documented in PowerSDR's GPL source)
`Console/FWC/fwc_midi.cs`. The radio exposes a MIDI port named "...FLEX..."/"...Flex...".
- Request: `F0 00 00 41` + 32 ASCII-hex chars + `F7` (37 bytes).
  Payload before hex-encoding (16 bytes, little-endian):
  `[0..1] u16 msg id`, `[2..3] 0`, `[4..7] u32 opcode`, `[8..11] u32 data1`, `[12..15] u32 data2`.
  Hex encoding: each byte -> two uppercase ASCII hex digits.
- Reply: SysEx; strip first 4 bytes and trailing F7, hex-decode -> `[0..1] id`, `[2..5] u32/float result`.
- Short messages: Note 0 = MOX (NoteOn/Off, vel 127), Note 1 = PA ADC read (velocity = channel).
  Radio -> PC: Note 61 Dot, 63 Dash, 66 MicDown, 68 MicUp, 70 MicFast (NoteOn/NoteOff).
- Caveat: PowerSDR 2.x mostly uses PAL; only GetModelMidi/paddles/MOX use MIDI. Whether firmware
  accepts *all* opcodes over SysEx needs testing on hardware.

## Control route 2: FireWire mailbox (see protocol/02-control-transports.md)
The PAL mailbox is plain 1394 async transactions into the DICE register window:
- CMD    0xFFFF_E000_1000: write {opcode|0x80000000, data1, data2} as one 12-byte block; bit 31 = busy
- RESULT 0xFFFF_E000_1004: read after bit 31 clears
- EVENT  0xFFFF_E000_1008: paddle/PTT/mic-button bitmap (0x01 dot, 0x02 dash, 0x04 RCA PTT, 0x08 mic PTT,
  0x10 down, 0x20 up, 0x40 fast); DICE notification bit 0x02000000 signals a change
Implemented for Linux in tools/firesidectl/firewire.py (firewire-cdev, no libs) + pal.py; `firesidectl-cli --fw /dev/fwN`.
Same opcodes as the MIDI route, so **both routes now exist**; whichever the firmware honours fully wins.

## Linux audio
snd-dice has a generic catch-all entry (`IEEE1394_MATCH_VERSION, version = 0x000001`) guarded by
`check_dice_category()` (config ROM GUID category byte must be 0x04). If Flex kept the TCAT standard,
snd-dice binds and gives ALSA PCM (I/Q) + ALSA MIDI (control route 1) automatically. Untested.
Fallback: FFADO / PipeWire ffado module.

## Hardware status
This host has no FireWire controller (no /sys/bus/firewire). Need a PCIe card with TI or VIA (not JMicron) chip. The radio will come with the donor's PCIe card (expected SIIG, TI chip); host has PCIe x16 slots only. Verify with `lspci -nn` (vendor 104c = TI).

## Status 2026-11-14: hardware-free work done
- `tools/firesidectl/`: Python prototype of control route 1 (MIDI SysEx). Codec, raw ALSA MIDI transport,
  safety-gated client, read-only `probe`, simulator, 17 passing tests. See tools/README.md.
- `tools/firesidectl/opcodes.py`: all 263 opcodes with computed values (C# implicit enum numbering handled),
  and per-opcode arg/result types taken from how PowerSDR calls them
  (WriteOp1 u32,u32 / WriteOp2 f32,u32 / WriteOp3 i32,i32 / WriteOp4 u32,f32; ReadOp1 -> u32, ReadOp2 -> f32).
  Frequencies are sent as **float32** (SET_RX1_FREQ takes data1 = f32, units to confirm; PowerSDR passes MHz).
- 148 opcodes are write-only, 46 read-only, 69 never called by PowerSDR 2.7.2.
- Final app plan: C++ / Qt 6, linking DttSP or WDSP; Python stays for tooling. Windows VM is only for capture.

## Open questions only the radio can answer
1. Does snd-dice bind, and does the radio expose an ALSA MIDI port?
2. Does the firmware answer SysEx for *all* opcodes? (Less critical now: the FireWire mailbox is the route PowerSDR really uses.)
5. Mailbox byte order on the wire (assumed big-endian; `--le` switch).
3. ~~Is model code 3 a FLEX-5000?~~ Resolved from source: 0=5000A, 1=5000C, 2=variant, **3=FLEX-3000**.
4. Does a get reply carry only id+value, or more? (decoder tolerates extra bytes)

## Status 2026-11-14 (later): start-up sequence + EEPROM backup (Sonnet), see analysis/startup-and-eeprom.md
- `firesidectl startup --dry-run | --allow-write` (init + power-on exactly as PowerSDR does; receive only), `shutdown`, `identify`.
- `firesidectl eeprom-dump`: read-only, double-read-verified backup of the calibration EEPROM with checksum verification and decode.
  **Do this first on the real radio.**
- 57 tests across tests/test_firesidectl.py, test_pal.py, test_startup.py, test_eeprom.py (`python3 -I tests/<file>`).
- Found and fixed: EEPROM opcodes mistyped as float (would have corrupted the backup); wrong model code in simulator.
- Open: an Opus review of the write sequence before first power-on (list in the analysis doc, section 4).

## Status 2026-11-14 (latest): system set-up + three analysis jobs (Sonnet)
- System: `system/70-flexradio.rules` installed to /etc/udev/rules.d (audio-group access to the radio's /dev/fwN, matched on the config-ROM text
  "FlexRadio Systems" / "FLEX-5000", the same IDs as the Windows INF `1394\FlexRadio_Systems&FLEX-5000`). `system/setup-system.sh` (idempotent, --dry-run, --remove)
  and `system/fireside-system-check.sh` (read-only hardware-day report incl. DICE category byte and snd-dice binding; tested against a fake sysfs).
  Verified: firewire-core/ohci, snd-firewire-lib, snd-dice modules present; snd-dice has a generic ieee1394 ver=1 alias and nothing is blacklisted.
  Windows driver extracted (innoextract installed) to scratch only; FlexRadio.inf shows the driver covers FLEX-5000 and FLEX-3000.
- analysis/audio-stream-layout.md: 8-ch stream, 48/96/192 kHz, RX1 I/Q on in ch0/1, mic/RX2 shared on ch2/3, 3-thread DttSP model, 1 s dead-stream watchdog.
  **OPEN:** FlexRadio's two output-channel tables disagree (RX1/TX/RX2 vs QSE/headphone/ext-spkr); do not transmit until the output experiment settles it.
- protocol/04-opcodes.md: generated for all 263 opcodes (types, safety class, in-use flag) + 51 hand notes (tools/opcode_notes.py).
- analysis/front-panel-events.md: EVENT bitmap, MIDI notes, CW keyer (closed FlexCW.dll: we write our own). **Mic UP/DOWN/FAST are FLEX-3000 only**: the 5000 mic connector has no such pins.
- Corrections made: the audio 'mux' is the Windows face of radio codec registers (WRITE_CODEC_REG map documented), not a no-op.

## Status 2026-11-14 (later still): hardware-day script + C++/Qt skeleton (Sonnet), paused for usage check
- `tools/hardware-day` (hardware_day.py): strictly read-only first-contact report (system check, sysfs/config ROM, kernel log, ALSA formation, MIDI + FireWire
  identify, byte-order detection, optional event watch, EEPROM backup) into `hardware-day/<utc>/report.md` with an answer sheet for the open questions.
  `--rehearse` runs it against simulated radios. Tests prove every mailbox write is a GET opcode and a wrong byte-order guess decodes to opcode 0x30040080
  (not a real opcode); a wedging firmware fails cleanly without writing into a busy mailbox.
- `app/`: C++17/Qt 6 skeleton, see app/README.md. Ported read-only pieces only; start-up writes are a stub that throws `NotReviewed`.
  C++ and Python are tied together by generated conformance vectors + a drift test.
- Tests: Python 7 suites (82 tests) + C++ 21 tests + GUI smoke.
- NEXT: Opus review of the start-up write sequence (before any first power-on), then port it; then FireWire link in C++, WDSP receive chain, CAT server.

## Status 2026-11-14: Opus review of the write sequence DONE (analysis/startup-review.md)
Approved for a first receive-only power-on. Fixed during review: exemptions were opcode-wide (SET_MOX any value!) -> now value-specific;
writes now FireWire-mailbox-only (MIDI refused before any traffic); shutdown adds SET_PA_BIAS 0 like PowerSDR's exit; `--fw` auto-detects byte order.
hardware-day now reads the DICE owner/nickname/status registers (read-only). Design note: adopt PowerSDR's 9 kHz IF offset in the receive chain.
Tests: Python 7 suites (75 tests) + C++ 21 + GUI smoke, all passing.

## Status 2026-11-14 (job 1): C++ FireWire link (Sonnet), paused for review
- `app/core`: `firewire.{h,cpp}` (cdev client using <linux/firewire-cdev.h>, static_asserts lock ioctl numbers/struct sizes to the Python constants; syscalls behind `FwSys` so tests fake them) and `pal.{h,cpp}` (`PalMailbox : ILink`, byte-order switch, wait-idle-before-every-command with timeout, `read_event`, `swapped_opcode_word`, `probe_order`).
- `app/tests/test_pal.cpp` mirrors tools/tests/test_pal.py (mailbox + cdev packing) and the probe tests of test_hardware_day.py (big/little/wedged/neither, swapped word aliases no opcode for all 263). C++ now 42 tests; Python suites unchanged and passing.
- Read-only: nothing opens /dev/fw* in tests; no write path beyond the mailbox CMD block, which is still behind Flex's Safety gate. Never run on hardware.
- Not wired into the GUI or a CLI yet; startup port (job 2) is next.

## Status 2026-11-14 (job 2): start-up/shutdown ported to C++ (Sonnet), awaiting Opus diff-check
- `app/core/{include/fireside,src}/startup.*` + `tests/test_startup.cpp` (+ shared `tests/fake_dice.h`). 60 C++ tests pass, GUI smoke OK, 7 Python suites OK.
- Review conditions A (value-specific permits), B (mailbox-only, pre-traffic refusal), C (PA bias off at shutdown) all enforced and tested. Details and the three deliberate differences from Python: analysis/startup-review.md, last section.
- Nothing calls run_startup yet (no C++ CLI/GUI hook); never run on hardware. NEXT: short **Opus** diff-check of startup.cpp vs startup.py, then job 3 (WDSP receive chain, CAT).

## Status 2026-11-14 (job 3): receive chain + CAT, hardware-free (Sonnet)
- `app/core`: `dsp.{h,cpp}` (IF shift, spectrum, waterfall, USB/LSB/AM/CW demod, decimation, watchdog, synthetic I/Q, WAV I/O), `cat.{h,cpp}` (Kenwood TS-2000 subset + loopback TCP). `app/tools/firesideiq.cpp` runs recordings through the chain.
  93 C++ tests (33 new) + GUI smoke; Python suites unchanged. Not WDSP: WDSP dev files are not installed, so this is our own minimal chain behind small interfaces.
- Design choices to remember: DDS-at-dial-9kHz is modelled in the DSP (dial arrives at +9 kHz, shifted to 0) but the reviewed start-up still tunes the DDS exactly to the dial. Adopting the offset
  there changes a reviewed write (SET_RX1_FREQ_TW value), so it needs the Opus reviewer's OK: a flag `if_offset` on `startup_steps` using `dds_for_dial_mhz`, then pass the same offset to `ReceiveConfig`.
- CAT never keys: `TX`, `MX`, `KY` answer `?;` and increment `refused_tx()`; `PS0` refused. CAT command formats are from memory of the TS-2000 reference, unchecked against live Hamlib/WSJT-X.
- Open on hardware: I/Q mirror [SETTLED 2026-11-17: must be mirrored, see analysis/audio-stream-layout.md], real S32_LE capture channel order, whether the 1 s watchdog threshold suits ALSA period sizes.
- NEXT (Sonnet): live ALSA capture -> ReceiveChain (read-only, after hardware day), GUI spectrum/waterfall widgets, wire CAT `on_rx_freq` to `SET_RX1_FREQ_TW` (a write: needs review of the exact value/path first), AGC/NR.
  Separate **Opus** session still owed: startup.cpp diff-check (analysis/startup-review.md, last section).

## Status 2026-11-14 (job 3b): AGC, S-meter, FM, noise blanker (Sonnet)
- dsp.h: `Agc` (instant attack so |out| <= target 0.25, hang re-armed while signal stays near the held peak, presets Fast 50/0, Med 250/50, Slow 1000/500 ms decay/hang, max gain 80 dB),
  `NoiseBlanker` (zero samples > 6x running mean of un-blanked magnitude, +2 hold), `Mode::FM` (audio = +-1 at 5 kHz deviation), `Demodulator::signal_dbfs()` (pre-AGC in-passband power, ~100 ms), `s_reading`, `cat_smeter_value`.
  All off by default, so earlier behaviour/tests unchanged. 104 C++ tests; `firesideiq --agc --nb --mode fm`.
- S-meter is relative (dBFS) until the radio's full-scale level is measured with a signal generator; CAT SM scale (0..15 for S0-S9, then 4 dB/step to 30) is from memory of the TS-2000 reference.
- Bug caught by tests while writing: hang timer expired during a steady tone (now re-armed near the held peak).
- NEXT: Qt spectrum/waterfall widgets + file source, then ALSA capture backend behind a common IqSource.

## Status 2026-11-14 (job 3c): receiver GUI on recorded I/Q (Sonnet)
- core: `iqsource.h` (`IqSource`, `FileIqSource`, `DemoIqSource`), `display.h` (colour map, auto-scale, ticks, pixel mapping; unit-tested), `dsp::default_passband`. gui: `ReceiverPanel` + `SpectrumWidget` + `WaterfallWidget` + `AudioOut` as a tab in MainWindow.
- 110 C++ tests + `gui_smoke` + `gui_receiver_smoke` (offscreen: demo -> waterfall rows, peak at +1 kHz, mode/AGC/mirror rebuild, bad-file message, real WAV load). Python suites unchanged. Screenshot checked by eye.
- Not done / next: ALSA live capture implementing `IqSource` (read-only; test everything but the device), DSP off the GUI thread for 192 kHz, click-to-tune (would be a write: review first), CAT retune wiring (same), bookmarks.
- Live audio output untested here (headless box has no sound device; AudioOut reports "no audio output").

## Status 2026-11-14 (job 3d): live ALSA capture, read-only (Sonnet)
- core: `capture.{h,cpp}` (`PcmIqSource`, `IPcm`, card listing, `analyze_iq`), `alsa_pcm.cpp` (the only libasound file; CAPTURE only; no-ALSA build stubs throw). tools: `firesidecapture`. gui: Live capture button, live-aware pacing (read what the device delivered), status shows frames/xruns/DEAD.
- Tests: 124 C++ (13 new; includes the real ALSA path via the `null` PCM), `gui_receiver_smoke` extended (live null source reports STREAM DEAD), new Python static guard `test_capture_readonly.py`.
- Dead-stream rule is time-based (> 1 s of exactly-zero RX1 I/Q), fixed during testing: the first version counted read blocks, so bigger reads delayed it.
- FT4 + click-to-fill (2026-11-15): Ft8Options::ft4, synth_ft4, GUI decoder index 5; parse_ftx_message/ftx_line_text in logbook.cpp; double-click in the decoder text -> LogPanel::fillFromDecode (Log tab gets a dot). FT4 SNR uncalibrated.
- Logging (2026-11-15): app is a front end for the user's Wavelog (API v2). Core `logbook.{h,cpp}` (ADIF, v2 bodies), GUI `LogPanel`. Token file ~/.config/fireside/wavelog.env. Test QSOs on the real server: user said to test freely and will clear the log before going on air. Not done: FT8 decode click-to-fill, auto-log of FT8 QSOs (needs TX).
- Audio out to the radio (2026-11-14): `playback.h`, `playback.cpp` (RadioAudioOut: mono 48 kHz -> chosen channel pair of 8, others silent), `alsa_play.cpp` (the only playback-opening file), tool `firesideplay`, GUI selector (PC sound default; Radio needs live card + explicit pair + confirm, not persisted). Audio only, no opcodes; pair unknown until the output-channel test.
- `firesidecapture --list` opens each capture card briefly to query formats (no recording). It will also touch the PC's own sound card; harmless but noted.
- Still open on hardware: whether snd-dice binds and gives 8 ch S32_LE at each rate; I/Q mirror; period size vs the radio's timing; xrun behaviour under GUI load.
- NEXT: Opus session for the start-up diff-check; hardware day. Sonnet leftovers: NR/notch, DSP thread, bookmarks. Held for review: click-to-tune, CAT->radio retune, IF-offset start-up.

## Status 2026-11-14: Opus diff-check of startup.cpp DONE (findings only; fixes NOT applied - usage limit)
See analysis/startup-review.md, last two sections: port matches Python; F1 (move value checks into Safety: concurrency hole) must be fixed before any
CAT/GUI write path; F2 (d2 == 0), F3 (sync after last write), F4 (require probe_order) recommended. IF offset, CAT retune and click-to-tune approved with conditions.
NEXT (Sonnet is fine): implement F1-F3 in C++ and Python with tests, then F4 when wiring a C++ CLI.

## Status 2026-11-14: review findings F1-F3 APPLIED (Opus), both languages
Value checks moved into the safety gate (thread-safe; a permit covers only (0,0)); d2 must be 0; run_steps confirms the last write with a read-only sync.
Python 8 suites OK (startup now 20 tests), C++ 130 tests + 2 GUI smokes OK, TSAN clean in core/. F4 remains a rule for future wiring.
CAT retune / click-to-tune are now unblocked by F1, but still need the conditions in startup-review.md (single tune function, range check, rate limit, RX1_FREQ_TW only).

## Status 2026-11-14: single tuning function (Sonnet), awaiting short Opus diff-check
- `app/core/{include/fireside,src}/tune.*`, `tests/test_tune.cpp` (10 tests), `tools/tests/test_tune_guard.py` (static: one opcode, CAT never touches the link). C++ 140 tests.
- Implements the review's conditions 1-4 (one opcode / range / rate-limit+coalesce / armed-only, mailbox-only, disarm on fault). CAT wired via `connect_cat_to_tuner`; no GUI click-to-tune yet (no radio session in the GUI).
- For the Opus diff-check: tune.cpp is ~70 lines; look at send_locked (the only write), request() ordering of the checks (armed -> range -> unchanged -> rate limit), arm() (needs allow_write), and that `last_sent_` is only updated after a successful write.
- NEXT: C++ CLI (probe_order -> identify -> startup -> tuner), review F4; DSP off GUI thread; NR/notch. Then hardware day.

## Status 2026-11-14: C++ CLI `firesidecli` + F4 (Sonnet), awaiting short Opus diff-check
- `app/core/{include/fireside,src}/cli.*`, `app/tools/firesidecli.cpp`, `tests/test_cli.cpp` (13 tests). F4 (`PalMailbox::verify`, verified flag) in C++ and Python. `probe_order` abort-on-busy fix (both languages).
- C++ 157 tests + 2 GUI smokes; Python 9 suites (startup 23, pal 12).
- NEXT: Opus diff-check of cli.cpp/tune.cpp/verify (see the end of analysis/startup-review.md), then hardware day. Sonnet leftovers: DSP off the GUI thread, NR/notch, click-to-tune (needs a GUI radio session).

## Status 2026-11-14: DSP off the GUI thread, NR / auto-notch / notch / squelch (Sonnet)
- `RxEngine` (core, Qt-free, 9 tests) + ReceiverPanel refactor: GUI thread ticks 149/150 while the engine runs; click-to-tune on the spectrum moves the dial (label only until a radio session exists).
- `AdaptiveLineFilter` (NR N=128/D=16/mu=0.005 chosen by sweep: +6.5 dB; auto-notch N=64/D=16/mu=0.02), `Notch`, squelch with its own 10 ms detector (the first version reused the 100 ms S-meter smoothing and closed ~0.5 s late: caught by the test).
- C++ 172 tests + 3 GUI smokes. ThreadSanitizer: nothing in core/ (13 pre-existing reports are in tests/fake_radio.h).
- NEXT (Sonnet): GUI radio session with explicit allow-writes + click-to-tune through the Tuner, over a simulated radio for demo/smoke; then Opus diff-check of everything write-related.

## Status 2026-11-14: radio session in the GUI + click-to-tune, simulated radio (Sonnet), awaiting the Opus diff-check of ALL write-related code
- core: `session.{h,cpp}` (RadioSession; CLI refactored onto it), `sim_bus.h` (simulated 5000A, moved out of the test fixtures). gui: `RadioPanel`, Radio tab, `fireside --simulate`, `--smoke-radio`.
- Tests: C++ 181 + 4 GUI smokes (receiver, threaded, radio, basic); Python 9 suites incl. new guards (GUI only via RadioSession, writes off by default, session.cpp only reviewed paths).
- OPUS CHECKLIST (all in analysis/startup-review.md, end): F1-F3 (done earlier) + F4/verify + probe-busy fix + cli.cpp/session.cpp orchestration + tune.cpp + RadioPanel + closeEvent shutdown + IF lock.
- Sonnet work is otherwise DONE. Remaining ideas (optional): CW keyer (no TX!), bookmarks, S-meter calibration UI, FM de-emphasis.

## Status 2026-11-14: Opus diff-check of all write-related code DONE; approved for hardware day
Fixes G1, G5, G10, G11, G12 applied with tests (see analysis/startup-review.md, last section). C++ 184 tests + 4 GUI smokes, Python 9 suites, TSAN clean in core/.
NEXT: hardware day (HANDOFF.md, item 4). Nothing else is owed before it.

## Status 2026-11-14: Receiver GUI redesigned (Sonnet)
- User found the PowerSDR/Thetis-style screens 'haphazard', so the Receiver tab was redesigned from scratch: Theme.h (one accent), FrequencyDisplay (digit tuning), SMeter, band/mode/filter/AGC as button groups in labelled cards, filters per mode (`dsp::filter_widths`, tested), spectrum+waterfall in one panel. All 4 GUI smokes pass; logic/API unchanged (RadioPanel untouched).
- NEXT (cosmetic): restyle the Radio tab into the same card layout; waterfall palette options; S-units once calibrated; memories/bookmarks; keyboard shortcuts.

## Status 2026-11-14: resampler + streaming WAV (Sonnet)
- `dsp::RationalResampler`, `WavStream` (RF64, unpatched sizes, truncation), `ResamplingIqSource`, `open_iq_file`; GUI and firesideiq use them. 195 C++ tests. Verified on real public recordings: FadeStats SDRuno demo (62.5 kHz), HamSCI WWV AM (16 kHz), HamSCI NBFM (250 kHz).
- Public recording links that actually load (checked 2026-11-14): rfconsult.uk/fadestats/SDRuno_DEMO_{200,1295,3957,7240,17665}kHz.wav; github.com/HamSCI/baseband_iq_examples (HamSCI_AM_modulated_WWV_iq.wav, HamSCI_nbfm_modulate_iq.wav).
  FlexRadio/WA5QPZ/SDRplay/Zenodo pages block scripted fetches or 404; the user must download those in a browser into ~/fireside-sdr/recordings/.

## Future goal (user, 2026-11-14): hardware VFO knob
3D-printed enclosure, rotary encoder + a few buttons, on a Teensy / Raspberry Pi Pico / ESP32. Not started. Design hooks to keep:
- ALL tuning input must keep flowing through one path: `ReceiverPanel::setDialHz` -> `dialChanged` -> `RadioSession::tune` -> `Tuner` (range check, rate limit, one opcode). A knob is just another source of relative steps into that path.
- Planned host side: a `ControlSurface` interface (relative tune ticks with acceleration, step up/down, band up/down, mode, mute; never PTT/TX while the output-channel question is open), plus a serial/HID transport. Test it with a pty-based virtual knob before any hardware exists.
- Transport options: native USB serial (Pico / Teensy / ESP32-S3) is simplest; USB HID needs no host code but gives less feedback; ESP32 Wi-Fi/BLE only if wireless is wanted. Optional radio->knob feedback (frequency/mode on a small OLED).
- Look at FlexRadio's own FlexControl knob (docs/flexradio/FlexControl-Quick-Start-Guide.pdf): emulating its serial protocol would make the DIY knob work with other Flex-aware software too.

## Status 2026-11-14: validation on real recordings, chip verdict, launcher (Sonnet)
- AM demod vs HamSCI reference audio: correlation 0.9936 (reference = |I/Q|). NBFM: ours 0.878 (15 kHz filter) / 0.908 (6 kHz) vs reference; a textbook numpy discriminator scores 0.877, so the FM demod is correct and the reference is simply a different (cleaner) signal. `tools/tests/test_real_recordings.py` (optional, skips without files/numpy/scipy; ~40 s). firesideiq gained `--width`.
- `system/fireside-system-check.sh` now says which FireWire chip maker is fitted (TI/VIA/LSI/Ricoh good, JMicron FAIL, NEC/SiS/unknown noted); tested on a fake sysfs (`tools/tests/test_system_check.py`).
- `system/install-launcher.sh [--dry-run|--remove|--simulate]` installs a desktop entry + icon in ~/.local (done: 'FLEX-5000 Receiver').

## Status 2026-11-14: GUI polish (Sonnet)
- Keyboard shortcuts (arrows, PgUp/PgDn, M, Space), Memories card, remembered settings + window geometry (never 'Allow writes'), Radio tab restyled into cards (`theme::makeCard`), window title 'FLEX-5000 Receiver'. New CTest `gui_settings_smoke` (temporary settings dir; shortcuts, band stepping, memories, restore). 5 GUI smokes total.
- Left: waterfall palette options, S-units once calibrated, hardware VFO knob host side (see 'Future goal'), tuning-knob feel (mouse wheel over the spectrum).

## Status 2026-11-14: waterfall palettes + wheel tuning (Sonnet)
- `display::Palette` (Classic/Inferno/Viridis/Gray, tested for smoothness and brightness order), WaterfallWidget keeps its row history so palette/floor changes recolour everything; mouse wheel over spectrum/waterfall = stepDial (accumulated notches, Shift = x10). Smoke tests cover wheel, palette recolour and persistence.

## Status 2026-11-14: CW / PSK31 / RTTY decoders (Sonnet)
- core/decode.h + decode_cw/psk/rtty.cpp, generators (morse_envelope, synth_morse/psk31/rtty), engine tap, GUI Decoder card, `gui_decode_smoke`. C++ 231 tests + 6 GUI smokes.
- Bugs found by the tests along the way: CW presence test needed a non-zero floor; first-pulse sliver poisoned the speed estimate; single-cluster pulse runs misread as dits (judge against known speed); QSB/neighbour needed 4 poles; Farnsworth word gaps; PSK31 Varicode lost a character after an ODD number of idle zeros; PSK31 gate and coarse AFC thresholds; ITA2 figures table (C octal escape ate a digit); filter buttons were being deleted/recreated (half-painted card).
- Open: real on-air validation (none available offline), FT8, adding a CW-keyer (never TX), decoder text logging to file.

## Status 2026-11-14: FT8 (Sonnet)
- Vendored ft8_lib (MIT) in app/third_party/ft8_lib (+ft8_quiet.h force-include silences its logging; C language enabled in CMake). Wrapper `Ft8Decoder` (background thread per slot, UTC slot timing for live, partial-slot skip, hash table), `synth_ft8` generator, `busy()`.
- Real data finally: ft8_lib ships real on-air slots + WSJT-X answers; 4 are in tests/ft8. Decoded 17/22 (110615), 4/5 (110130) etc., identical text. SNR estimate from the waterfall + the known tone sequence, calibrated -4 dB vs WSJT-X (38 points: mean 0, scatter 5 dB).
- GUI: Decoder card entry 'FT8' (USB, 3.6k filter, tone box disabled, tall text window). C++ 242 tests + 6 GUI smokes (decode smoke covers FT8). Third-party licence kept at app/third_party/ft8_lib/LICENSE.
- Not done: FT4, sending anything (receive-only by design), logging decoded lines to a file, PSKReporter-style spotting (needs network; would be an opt-in later).

## PLAN 2026-11-15: hardware-free transmit DSP (agreed with the user; ALL 9 STEPS DONE 2026-11-15, pushed)
User OK'd building transmit DSP offline while the radio is away. Hard rule: this code only produces sample buffers / WAV files. No link to firewire/link/session/safety/pal/startup/opcodes, no PTT, no MOX. Connecting it to the radio waits for hardware day (output-channel question, levels, PTT, guarded transmit gate).
Already exists: FT8/FT4 encoders as test generators (`synth_ft8`, `synth_ft4` in decode_ft8.cpp), `synth_morse`/`morse_envelope`, `synth_psk31`, `synth_rtty`, `ComplexFir`, `Demodulator`, `SpectrumAnalyzer`, `write_wav_f32`, 48 kHz complex I/Q convention (I + jQ, dial at 0 Hz).
Steps, one at a time, tests first (new module core/include/fireside/tx_dsp.h + core/src/tx_dsp.cpp, namespace fireside::tx; add to core/CMakeLists.txt and tests/CMakeLists.txt):
1. [DONE 2026-11-15: tx_dsp.h/.cpp, tests/test_tx_ssb.cpp, 277/277 green, uncommitted] SSB modulator: real audio (48 kHz) -> I/Q. One-sided complex band-pass FIR (reuse ComplexFir with a lowpass shifted to the passband centre, as Demodulator::band_ does). USB = +150..2800 Hz, LSB mirrored, no carrier. Tests: 1 kHz tone lands at +1000 (USB) / -1000 (LSB), opposite sideband > 50 dB down, out-of-band rejected, round trip through `Demodulator` gives the tone back.
2. [DONE 2026-11-15: tests/test_tx_roundtrip.cpp, 282/282, uncommitted] FT8 round trip: FT8/FT4 audio -> SSB modulator -> Demodulator -> 12 kHz -> `Ft8Decoder::decode_slot` returns the same message (also PSK31/RTTY/CW through their decoders).
3. [DONE 2026-11-15 (Opus): Biquad, NoiseGate, Compressor, PeakLimiter (look-ahead, ceiling by construction + backstop clamp, NaN/inf -> 0, works on I/Q magnitude), MicChain, TxChain = mic -> SSB -> I/Q limiter; tests/test_tx_mic.cpp, 290/290, uncommitted. Found: SSB filter overshoots clipped audio (0.9 square -> I/Q > 0.95), so the hard ceiling is on the I/Q after SSB, not the audio. Limiter ceiling is the largest float <= the configured double.] Mic chain: high-pass + EQ biquads, noise gate, compressor/leveler, optional clipper, ALC peak limiter with a hard ceiling (never exceeds full scale). Tests: ceiling holds, gain behaviour, no overshoot.
4. [DONE 2026-11-15 (Opus): CwShaper, text_keying, IambicKeyer; tests/test_tx_cw.cpp, 302/302, CTest 8/8, uncommitted. Carrier is ON THE DIAL by default (offset_hz = 0), not +pitch: the receive CW mode listens +-250 Hz around the dial and makes the note itself. Half-amplitude points sit edge/2 after each key change, so weight is exact. Measured 5 ms edges: -62 dBc beyond 300 Hz, -88 beyond 1 kHz (hard keying -38 / -48). Mode A = memory kept for taps, dropped when a squeeze is released; Mode B sends the extra element.] CW: shaped keying (raised cosine 3-8 ms, no key clicks: check spectrum) producing I/Q at +pitch; iambic keyer state machine (modes A/B, weight, speed) driven by paddle events as pure logic. Tests on timing and spectral width.
5. [DONE 2026-11-15 (Sonnet): two_tone_audio (cosines, peak of the SUM exact), measure_imd (Blackman-Harris DFT at the exact product frequencies; products that fold outside 150-2800 Hz are NaN and ignored); tests/test_tx_imd.cpp, 309/309, uncommitted. Default tones 600/1500 so both IM3 products (300, 2400 Hz) are in band; 700/1900 puts 2f2-f1 at 3100 Hz, outside. Measured: measurement matches closed-form cubic (-40.80 dBc) to 0.01 dB; SSB + linear chain <= -135 dBc; default chain with compressor IM3 -63 / IM5 -72 dBc (stated limit -55 / -65); clipper at 0.5 -17 dBc. The limiter adds nothing on a steady two-tone (window spans many beat periods): true for the test signal, not a claim about speech.] Two-tone test generator + IMD3 measurement with `SpectrumAnalyzer`/`Fft`; check the whole chain stays below a stated IMD figure.
6. [DONE 2026-11-15 (Sonnet): slot_containing / next_slot / next_slot_with_parity / plan_tx (late join with skip_s, min lead), signal_seconds, default_start_offset_s; tests/test_tx_slots.cpp, 318/318, uncommitted. Pure functions, utc is passed in. Parity = slot index parity = minute-relative parity (FT8 even :00/:30, odd :15/:45; FT4 even :00/:15/:30/:45, odd :07.5/:22.5/...). Signal lengths from the library constants: FT8 12.64 s, FT4 5.04 s (105 symbols incl. 2 ramp; my earlier 4.48 s was wrong). The 0.5 s start offset was verified later the same day (see the note after step 9). Checked by deliberately breaking the lead logic: 3 tests fail.] FT8/FT4 slot scheduler (pure function: UTC -> next slot start, even/odd period); tests only.
7. [DONE 2026-11-15 (Sonnet): core/tx_render.h/.cpp (render_ssb/ft/cw/two_tone, read_audio_wav, write_iq_wav; every output ends in an I/Q limiter at -1 dBFS), tools/firesidetx.cpp (ssb|ft8|ft4|cw|twotone), fireside --screenshot-file in.wav out.png [ft8|ft4|cw]; tests/test_tx_render.cpp incl. running the real firesidetx binary; 326/326, CTest 8/8, uncommitted. Verified end to end: firesidetx ft8 -> fireside decodes CQ M0OML IO91 at 1200 Hz, waterfall picture sent to the user. Notes: two-tone peak of -6 dBFS requested shows -5.8 in the file (SSB filter overshoot, still < -1 dBFS ceiling); the app forces USB for digital decoders, and an LSB-rendered FT8 file still decodes there because the synthetic file is noiseless (leakage is 45+ dB down, measured), FT4 LSB does not.] A `firesidetx` tool: writes TX I/Q to a WAV (SSB from a WAV, FT8 text, CW text, two-tone) so it can be opened in the receive app and seen on the waterfall (screenshot for the user). File output only.
8. [DONE 2026-11-15 (Opus): tests/tx_isolation.cmake + 6 CTests. ALLOWLISTS, not denylists: source mode follows #include "fireside/..." transitively from tx_dsp/tx_render/firesidetx (allowed: dsp.h decode.h tx_dsp.h tx_render.h; system includes must be plain standard headers; roots must not mention snd_/ioctl/"/dev/"/open_alsa/rawmidi/PcmIqSource); binary mode checks firesidetx strong symbols come only from dsp/decode_*/tx_dsp/tx_render objects of libfireside_core.a and imports no snd_*/ioctl (currently: dsp, decode_cw, decode_ft8, tx_dsp, tx_render). Self-tests (fixtures/tx_isolation) prove each layer catches a transitive radio include, /dev access, <alsa/...>, and radio code in a binary (firesidecli), matched on the violation text so a script error cannot pass. Also checked by hand: adding session.h to tx_render.cpp fails tx_isolation_source. CTest 14/14, uncommitted.] Safety test: a CTest that scans tx_dsp.h/.cpp and firesidetx for forbidden includes (firewire, link, session, safety, pal, startup, opcodes, rawmidi) so TX code can never reach the radio by accident.
9. [DONE 2026-11-15 (Opus): README section "Transmit DSP", 333/333, CTest 14/14, committed. REVIEW of the modulator/limiter/TX code before hardware day, findings and fixes:
   (1) one huge-but-finite mic sample (3e38) x mic gain overflowed to inf, poisoned the biquad state with NaN and silenced the chain for good -> internal levels clamped (+60 dBFS), Biquad resets a non-finite state;
   (2) "no carrier" was not enforced: lo_hz = 0 was accepted -> lo_hz must be >= edge_hz (DC offset > 60 dB down, tested); edge_hz 20..1000 Hz (a tiny edge built a multi-million-tap filter);
   (3) limiter look-ahead/release, EQ gain (+-24 dB), compressor make-up (+-40 dB), sample rates in keyer/CW/text_keying were unchecked -> bounded;
   (4) plan_tx late join ignored min_lead_s (started "now") -> starts at utc + lead, skip covers what is past, refused once the whole signal is past;
   (5) utc unchecked -> 0..1e12 (slot index overflow was UB).
   Checked and fine: limiter ceiling proof (box average of a min-hold <= need of the outgoing sample; backstop margin 1e-6 >> float eps), NaN/inf handling, CW envelope <= amplitude <= 1,
   firesidetx option parsing (NaN/garbage refused). MEASURED: the I/Q limiter's splatter when it acts on +30 dB overdriven speech: -92 dBc opposite sideband, -97 above 3.1 kHz, -88 near
   the carrier (test asserts < -70); below full drive it never acts. Without it the SSB overshoot reaches full scale, so it stays.
   The 0.5 s FT8/FT4 start offset was verified afterwards (see below). Still open for hardware day: all items in "Waits for the radio".] After all: update app/README.md, NOTICE if any new third-party code (none planned), tests green (currently C++ 270, 8/8 CTest groups), commit with the usual trailers, Opus review of the modulator/limiter before hardware day.
Waits for the radio: output levels and calibration, PA/ALC feedback, FireWire TX channel (DAC table question), PTT/MOX, latency, PC-sound-card clock-drift matching, the guarded transmit gate, anything on air.

**Start offset verified 2026-11-15 (Sonnet), primary source:** Franke, Somerville, Taylor, "The FT4 and FT8 Communication Protocols", QEX July/August 2020 (wsjt.sourceforge.io/FT4_FT8_QEX.pdf). It says "the nominal start time, 0.5 s after the beginning of a transmit interval" in a discussion covering both modes (no separate FT4 figure); that a late station's WSJT-X "will skip the missed part of the transmission and start sending a correctly synchronized message for the remainder" (matches plan_tx skip_s); and that without AP info callers starting more than 5 s late are not decoded (8 s with AP "mycall"). Table 4: symbol periods FT4 0.048 s, FT8 0.160 s; FT4 is "twice as fast as FT8" (7.5 s period). The April 2019 FT4 draft (FT4_Protocol.pdf) says T/R 6 s and transmissions 4.48 s: superseded, ignore it. Our 12.64 s / 5.04 s signal lengths agree with the final paper (79 and 105 symbols). Method note: the web-fetch agent cannot read PDFs; save the file and run pdftotext (use the non-layout mode for two-column papers).

**Sanitizers on the TX code 2026-11-15 (Sonnet):** all 75 transmit-related tests (tx_*, ssb_*, cw_*, keyer_*, imd_*, slots_*, plan_tx*, render_*, firesidetx tool) pass under ASan + UBSan (-fno-sanitize-recover) with leak detection: no reports. Recipe: `cmake -S app -B /tmp/asan -G Ninja -DFLEX_BUILD_GUI=OFF -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`, then `FIRESIDE_TEST_FILTER="tx_,ssb_,cw_,keyer_,imd_,slots_,plan_tx,render_,two_tone,firesidetx" ./tests/fireside_tests`. `FIRESIDE_TEST_FILTER` (comma-separated name substrings) is new in tests/main.cpp; unset runs everything, a filter matching nothing fails.

**Offline transmit tab 2026-11-15 (Sonnet), at the user's request ("transmit ui stuff with nothing connected"):** gui/TxPanel.{h,cpp}, tab "Transmit (offline)" in MainWindow; core/tx_render gained ft_message_ok. Modes FT8/FT4/CW/voice-from-WAV/two-tone, USB/LSB, worker-thread render, ceiling -1 dBFS, IM3/IM5 readout, Save WAV, Preview on the receiver (MainWindow glue loads the temp WAV into ReceiverPanel and picks the decoder). NO transmit key, radio output, or sound output exists on the tab. Guards: tx_isolation.cmake GUI=ON (allowlist: Qt headers without a slash, Theme.h, TxPanel.h, fireside_core dsp/decode/tx_dsp/tx_render; forbidden words PTT/MOX/session/Tuner/RadioPanel/ReceiverPanel/MainWindow/AudioOut/QAudio/QMedia/QSerialPort/QTcp/QUdp/QLocalSocket/QProcess/QNetwork) with 5 self-tests (each rule tested separately); `fireside --smoke-tx [--screenshot f.png]` as CTest gui_tx_smoke (renders all modes, checks ceiling, error paths, and that the receiver decodes the preview). The isolation check caught the word "session" in my own header comment on its first run. CTest 21/21. NEXT, when the radio is here: the connection (output channel, levels, key, guarded gate) is a separate, reviewed step; MainWindow is where the preview glue lives and the only place TxPanel meets other code.

**Receiver display steadiness 2026-11-15 (Sonnet), user complaints while using the Transmit tab preview:** (1) the signal card resized with its text ("peak line -6.6 dBFS") -> the label reserves the width of the longest text; gui_receiver_smoke now drives the label with short/typical/longest texts and checks the card width is constant (fails 326..409 px without the fix). (2) the spectrum axis breathed: fixed 5 %-per-frame smoothing for growing and shrinking, tied to the frame rate -> `smooth_asymmetric` (time based: grows with tau 0.6 s so a new strong signal is never clipped, shrinks with tau 20 s). (3) Still irritating on a noiseless synthetic file: exact digital silence is -200 dB, which dragged the axis to a 200 dB span and swung it 85..160 dB -> `ScaleTracker` in core/display: silent frames/bins (<= -199.5 dB, under 10 % live) are ignored, the span is capped at 140 dB after smoothing (top follows the peak, bottom is raised), grid lines every 20 dB when the range is tall. Measured on the CW preview: edges move < 0.5 dB after settling (were ~10 dB). Tests: display_scale_* (5) incl. one that replays the keyed noiseless signal through ReceiveChain; they fail if the silent-skip or the cap is removed. Real radios never reach -200 dB, so this mostly matters for synthetic files, but the tracker is also calmer for real fades.

**Transmit moved onto the Receiver screen 2026-11-16 (Sonnet), user: "it's a transceiver, transmit functions should be in the same display as the receiver":** the separate tab is gone. `TxPanel` is now a compact three-card strip (Transmit / Settings / Result with a spectrogram of the rendered signal) that `ReceiverPanel::setTransmitWidget` places under the decoder; the "Transmit" button on the decoder row opens and closes it (state remembered, closed by default). TxPanel is still isolated (ReceiverPanel only receives it as a plain QWidget from MainWindow; the isolation scans are unchanged). Still offline: renders files, no key, no sound, no radio.

## 2026-11-18: synchronous AM (Opus)
`Demodulator` AmDetector::Sync (default for AM; Mode card "AM detector": Sync / Sync lower / Sync upper / Envelope). Second-order PLL (30 Hz) on a 200 Hz-smoothed carrier
estimate, pulls in +-200 Hz, coasts through deep carrier fades; one sideband by I -+ Hilbert{Q}. Status line: "Sync AM locked, carrier N Hz".
Measured (test_sam.cpp): no distortion under selective fading (carrier fading to 10 % twice a second: sync THD < 3 %, envelope > 3x that); one sideband removes a 2.5 kHz
whistle by > 30 dB; noise: equal above the envelope threshold, +1.6 dB near a 0 dB CNR (an honest small gain: the big wins are fading and sideband choice).
On air: Radio 5 Live 693 kHz, 30 s capture 2026-11-18 11:28 UTC (daytime ground wave): locked, carrier +0.6 Hz from the dial. Listening files:
recordings/live/am_compare_693/693_{env,sync,sync-lsb,sync-usb}.wav (by day they should sound alike; the difference shows on fading night-time signals).
`firesideiq --am env|sync|sync-lsb|sync-usb` demodulates a recording either way.
