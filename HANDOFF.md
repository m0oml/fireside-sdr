# Handoff for a fresh session

Start Claude Code in the repository root (project memory is keyed to that directory). Read this file, then `NOTES.md` ("Where things stand"), then the doc for whatever you work on.

## State in one paragraph (2026-11-19)
A native Linux Qt6/C++17 **transceiver** for the user's own FLEX-5000A, running on the real radio over FireWire (TI XIO2213B card, `/dev/fw0` local node, `/dev/fw1` radio).
Receive: reviewed start-up, own isochronous I/Q stream at 48/96/192 kHz, spectrum/waterfall, USB/LSB/AM (sync)/CW/FM, FT8 (94 % of WSJT-X), FT4 (deep stage), CW and PSK31
(noise-gated, tuned on a real CWT recording), RTTY, Wavelog logging. **Transmit works on the air (2026-11-19):** PA bias from the EEPROM, ATU tune + per-band ATU memory (relays back
without RF), power/SWR meters from the EEPROM calibration, closed-loop power, TX image correction, FT8/FT4 send from the Radio screen. First FT8 CQ (5 W, 20 m) was answered
by DK0ULR. Full story: `analysis/transmit-stage2.md`; GUI: app/README.md "Radio transmit on the Radio screen".

## Map
- `app/` C++17/Qt 6: `cmake -S app -B build -G Ninja && cmake --build build && ctest --test-dir build` (24 CTest groups, ~8 min). `app/README.md` is the feature and architecture reference (start at "Daily use").
- `tools/` Python reference implementation (`firesidectl-cli`, `hardware-day`) and its tests: `python3 -m unittest discover -s tools/tests` (107 tests). `tools/compare_ft8.py`, `tools/ft8bench.py`: decoder comparison against WSJT-X's jt9.
- `analysis/` findings: `audio-stream-layout.md` (radio streams, direct-stream spike and results), `ft8-vs-wsjtx-*.md` (decoder numbers), `startup-review.md` (the safety review of every write), `opcode-reference.md`.
- `system/` udev rules (radio node AND the controller's local node, group `audio`), WirePlumber rule (PipeWire leaves the radio's sound card alone), `setup-system.sh`, `fireside-system-check.sh`.
- `NOTES.md` running log, newest first at the top | `UPSTREAM.md` pins of the ignored `upstream/` clones (PowerSDR source, read-only reference) | `NOTICE` credits (PowerSDR, ft8_lib, WSJT-X, KISS FFT, snd-dice).
- Ignored by git: `docs/`, `software/`, `upstream/`, `hardware-day/`, `backups/` (EEPROM backups: contain the radio's calibration; keep copies elsewhere), `recordings/`, `winvm.md`, build output.

## Robustness checks (sanitizers)
Last run 2026-11-18, all clean: ASan + UBSan on all 404 core tests (523 now) and on the 16 GUI test groups; TSan on the threaded code (engine, direct FireWire stream, session, decoder thread): 0 reports.
Repeat after threaded or low-level changes (separate build dirs, git ignores `build*/`):
`cmake -S app -B build-asan -G Ninja -DFLEX_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_C_FLAGS="<same>" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`
then `build-asan/tests/fireside_tests` (~4 min); for TSan use `-fsanitize=thread` and `FIRESIDE_TEST_FILTER=engine,fw_iso,session,ft8_streams,...` (TSan is 5-15x slower: the real-time engine test
`engine_decoder_runs_on_the_worker_thread_with_a_live_consumer` fails under it by timing only); GUI under ASan: configure without `-DFLEX_BUILD_GUI=OFF`, run `QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 ctest -R gui_`.

## Rules that matter
- **Transmit only as the user asks, and only through the TxGate** (`fireside/tx_gate.h`, the only code that sends SET_MOX 1; static guards in `tools/tests/test_tune_guard.py`).
  The user presses every RF button; Tune and Measure ask "listen first" (Arm / Test tone / Send do not, by the user's choice 2026-11-19). No dummy load / SWR meter here: keep tests short and low power. Never raise a gate limit or the hard drive
  clamp without saying so and getting the user's OK. Never write the EEPROM except the one reviewed restore (`tools/firesidectl/restore_flag.py`); PowerSDR can clear byte 0x3F
  (checksum race): after any PowerSDR run, compare a fresh dump with the backup.
- Drive levels are per radio and per band (`~/.config/fireside/tx-drive.txt`, no built-in default); ATU memory `~/.config/fireside/atu-memory.txt`. TX I/Q is MIRRORED (verified on air).
- **Every radio write goes through a reviewed path**: `Tuner` (SET_RX1_FREQ_TW only), `RxPreamp` (SET_TRX_PREAMP 0/1 only), `RxAntenna` (SET_RX1_ANT 1..4 only, per-band antenna switching), the start-up/shutdown sequences in `startup.cpp`, and `DiceStreamControl` (three DICE sound-chip registers, restored on close). `tools/tests/test_tune_guard.py` checks this statically; the GUI reaches the radio only through `RadioSession`.
- "Allow writes" is off in every new window; the "Start the radio?" question is the consent for that session. Don't weaken it.
- WSJT-X (`jt9`, `ft8code`, from the `wsjtx` package) is the ground truth for decoder work; its source (`github.com/WSJTX/wsjtx`) was the reference for the reimplemented algorithms (GPL v3, like this project).
- A killed `firesidecapture` (SIGKILL) leaves the radio's stream on: restore by hand: DICE ENABLE = 0, TX[0] iso channel = 0xFFFFFFFF, CLOCK_SELECT = 0x020C (SIGINT/SIGTERM are handled and restore it).

## What is done
Receive (see above) plus transmit: the ATU sequence observed from PowerSDR on the radio, TxGate / TxControl / ATU / meters / image
correction / FT8-FT4 send (core, shared by the GUI card and `build/firesidekey`), simulated radio (`fireside/sim_radio.h`, `fireside --simulate` practises transmit), GUI Transmit card
right of the decoder. Tests: 24 CTest groups (523 core tests) + 124 Python; the GUI smoke test runs arm / measure / tune / FT4 send / stop on the simulated radio.

## Open / next
1. ~~FT8 reply / auto-sequence~~ DONE 2026-11-19 (click to reply, `qso_seq`, auto log; tested on the simulated radio, not yet on the air) (double-click a CQ or a call to M0OML: report, RR73, 73, then log to Wavelog). The user has not made a complete QSO yet.
   Done towards it (2026-11-19): lines to M0OML highlighted red in the decoder; the first caller fills the log entry and is looked up (Wavelog + callbook);
   later callers are left alone (status says so); their RR73 after logging does not refill. Send / Repeat / Arm / Test tone no longer ask a question.
   QRZ.com XML lookups direct from QRZ (Log tab card; qrz.env 0600; cache; 1/s; callers first): info on every decoded line. Not yet tried with the user's real login.
2. Drive levels on other bands: DONE 2026-11-20 for 80, 40, 30, 20, 15, 10 m (~/.config/fireside/tx-drive.txt). Not calibrated: 17, 12, 160, 60, 6 m (12 m has a stored ATU tune but no drive level;
   the user did "all the bands I can", so those may not match on the current antenna: re-try after a better antenna).
3. ~~Direct isochronous TRANSMIT~~ WORKS 2026-11-19 (duplex stream: analysis/duplex-iso-tx.md; 2 W test tone with receive running). Was: Direct isochronous TRANSMIT (keep the 192 kHz receive stream during TX; today TX goes through snd-dice playback, which offered only 96 kHz after PowerSDR).
4. PSK31 / RTTY on real signals (no recording yet). Synchronous AM verdict. Output channels ch2/3, ch6, ch7 untested.
4b. SWR question: ANSWERED 2026-11-19, analysis/swr-meter.md. Our PA bridge reads a leakage (SWR 1.55 / 1.83 / 2.01 at 3 / 8 / 15 W) where the
   tuner reads 0.00 W reflected; same formula as PowerSDR. Left as is until the user has a better antenna (the gate still trips on our SWR 3.0);
   the tuner-SWR trip option needs the user's OK.
4c. DONE 2026-11-20: band memory (each band returns where it was left) and a Hamlib NET rigctl CAT server (Device tab, 127.0.0.1:4532,
   frequency / mode only, cannot transmit). Not yet tried with WSJT-X / GridTracker / WaveLogGate on the user's PC (checked with `rigctl -m 2`).
   CAT PTT for other programs would need their audio into our TX path too: a separate step, and it needs the user's OK (a new transmit route).
4d. PSK Reporter uploads VERIFIED 2026-11-20: 180 reports from M0OML (IO92AR, 20 m FT8, 07:55-08:37 UTC) are in PSK Reporter's database
   (retrieve.pskreporter.info query, receiverCallsign=M0OML), matching the app's count. psk-analysis.pl showed "0 packets" all the same: not a usable check.
4e. WDSP spike 2026-11-20 (scratch only, nothing in the repo): TAPR OpenHPSDR-wdsp 2.11 (GPL v2+, compatible with our GPLv3) builds on Linux with a small Win32
   shim (AetherSDR's third_party/wdsp/port, GPLv3) and links against libfftw3 (runtime present, no -dev package needed: WDSP bundles fftw3.h). Fed our centred I/Q
   (WDSP's I/Q sense is the OPPOSITE of ours: conjugate). Test: WSJT-X "Testing_long.wav" speech injected as USB into real 40 m band noise from our own grab,
   at 3 and 12 dB in-band SNR, AGC off, output SNR vs the clean speech: WDSP NR2 (EMNR) +1.3 / +2.1 dB better than plain; our adaptive line filter NR is 2.3 / 3.3 dB
   WORSE than plain on speech (it suits tones); WDSP LMS NR and SNBA do not help. CPU: WDSP 0.04x real time plain, 0.056x with NR2; ours 0.076x plain, 0.29x with NR.
   Cost: WDSP adds ~87 ms delay plain, ~150 ms with NR2 (ours ~10 ms). One speech clip, one noise type, no impulse noise / QRM / CW: not conclusive. A real 20 m grab
   with an SSB QSO is needed (14.200 MHz window recorded 09:43 UTC held no identifiable SSB). Listening files: recordings/wdsp-spike/.
4f. DONE 2026-11-20: NR2 (WDSP EMNR, vendored in app/third_party/wdsp-nr2, GPL v2+) as an optional voice noise reduction: button on the Noise card, not for CW, off while a
   data decoder runs, post-processing on (+5.2 dB output SNR over no NR in the test below). Optional build (needs libfftw3.so.3). Created through `AudioStage` / `set_spectral_nr_factory` (dsp.h) because the TX isolation check forbids receive-only
   code in dsp.h and the firesidetx binary: do NOT widen that allowlist, keep it behind the factory. Not yet heard on a real SSB QSO (needs a Ctrl+G grab or live listening).
4g. WDSP, the rest (2026-11-20, same speech-in-real-noise test, AGC off): NR2 post-processing ON = +5.2 dB over nothing (was +1.3 / +2.1 without it): adopted. Gain method,
   noise estimator, artifact elimination: no real difference (WDSP defaults kept). WDSP's impulse blanker (NB2) was tried against ours with 25 ignition-like impulses/s at 30x and 10x
   the noise: ours restores +4.9 dB (clean = +5.0), WDSP's best +4.9 only when tuned (threshold 8, 0.1 ms) and typically 4.6-4.8, with 26 ms more delay: ours stays. WDSP LMS NR,
   auto-notch (ANF) and SNBA made speech WORSE or no better here (they target tones / narrowband QRM, which this noise did not have): not adopted. WDSP's AGC not tried (no objective
   test; ours works). Real SSB on the air still to listen to. Listening files: recordings/wdsp-spike/ (snr3/snr12: 1 plain, 2 our line-filter NR, 3 NR2 as shipped).
5. S-meter relative only (no test gear). The Win11 VM failed to boot (Automatic Repair) on 2026-11-18: needs looking at separately.

## First prompt for a new session
"Read ~/fireside-sdr/HANDOFF.md and NOTES.md, then continue with the first open item. Keep every radio write on a reviewed path, transmit only when I ask and only through the TxGate, and run the full test suite before committing."
