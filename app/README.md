# fireside: native Linux app for the FLEX-5000 (C++17 / Qt 6)

**Status 2026-11-20: receive and transmit both work on the real FLEX-5000A** (FireWire card; receive on air since 2026-11-17, first FT8 CQ answered 2026-11-19). Built with
`-Wall -Wextra -Wpedantic -Wshadow` (a few style warnings remain, mostly misleading-indentation on dense one-liners), with C++ and Python unit tests and GUI smoke tests under CTest. **Transmit happens only when you press a transmit button, through the transmit gate; read the "Transmitting" section of the top-level README first.**

## Daily use (the short version)
1. Start `build/gui/fireside`. On the Radio screen press **48 / 96 / 192** (kHz of band): it asks "Start the radio?" once (receive only; this also allows writes for that session), then connects,
   starts the radio, opens the live stream at that rate and starts receiving. **Stop radio** (same card) puts the radio in standby and closes the stream. Changing the rate on a running
   radio only restarts the stream (about a second); the radio keeps its tuning and preamp. 192 kHz is the direct FireWire stream described below; audio is always 48 kHz and plays on the PC.
2. The Device tab is only for changing something: device, byte order and antenna are remembered (never "Allow writes": every new window starts with writes off). It also has Connect/Identify,
   a **Start receiving** button (same as the rate buttons, at the remembered rate) and **ALSA capture...** (the old 48 kHz capture, only needed for audio out of the radio's own jacks).
3. Tune by wheel (towards you = up) or click on the spectrum or waterfall; **Ctrl+wheel** or the Zoom buttons (Ctrl+= / Ctrl+- / Ctrl+0) zoom both displays about the mouse. A red **tuning line** marks
   the dial on both; the shaded passband too. The spectrum's passband edges are dragged by their small triangles along the bottom; a click anywhere else in the trace (and anywhere on the waterfall) tunes.
4. Decoder card: FT8 / FT4 / CW / PSK31 / RTTY. The **my call** box (empty: the Log tab's Wavelog station) enables a-priori decoding of weak messages to you (shown with `a2`).
5. **Antennas tab**: pick the receive antenna per band (ANT1/ANT2/ANT3/RX1 IN; "Default" follows the default box; long/medium wave and 160-6 m rows). While the radio runs, moving the dial into
   another band switches the RX antenna (one reviewed write, `core/src/rxant.cpp`: SET_RX1_ANT 1..4 only, idempotent: a dial that stays in a band never writes); the start-up uses the band's antenna. The
   status line shows `RX ANT2`. Everything is remembered (`~/.config/fireside/antennas.conf`); the Device tab's old antenna box is hidden.
6. **Grab / Rec** (Source card, Radio screen, next to the 48/96/192 buttons): the receiver keeps the last minute of the live stream (band I/Q, raw as the radio sends it) and of the audio in
   memory all the time, so when you hear something press **Grab 30 s** (Ctrl+G) AFTER the fact and it saves the last 30 s (I/Q + audio, `~/fireside-sdr/recordings/grabs/`). **Rec** (Ctrl+Shift+R) starts/stops
   a normal recording that begins with the last 5 s. `...` = options (replay length 15-180 s, grab length, pre-roll, I/Q and/or audio, label, folder, files saved this session, open one in the receiver).
   Files: `<UTC date_time>_<kHz>kHz_<mode>[_label]_iq.wav` (32-bit stereo, same format as `firesidecapture --stream`) and `_audio.wav` (16-bit mono 48 kHz), each with a `.txt` note (dial, mirror, IF offset, and `utc` / `start_utc` = when the last / first sample was taken, to the millisecond: `tools/compare_ft8.py` uses it to align FT8/FT4 slots by itself);
   opening an I/Q file with **Open file...** applies those notes (also for the `firesidecapture` notes in `recordings/live`), so a grab replays correctly with one click. Core: `core/src/recorder.cpp`
   (writes on a background thread; the receive thread only copies), engine taps `RxEngine::set_taps`.
7. AM mode has an **AM detector** box: Sync AM (default), lower or upper sideband only, or the plain envelope detector.

## What is verified on the real radio
- Receive on ANT1 with the preamp (Preamp button), 9 kHz IF offset (DDS 9 kHz below the dial), mirrored I/Q; AM, SSB, CW, FT8, FT4 heard and decoded.
- Direct FireWire stream at 48, 96 and 192 kHz (see the section below), 0 xruns, the window keeps up at 192 kHz.
- FT8 against WSJT-X 3.3.0 (jt9) on two 10 minute 40 m captures: 94 % of jt9's decodes, DT within 0.01 s, SNR within 0.5 dB, about 0.1 % likely false. FT4 checked on one contest sample and one live slot.
  Details: `analysis/ft8-vs-wsjtx-2026-11-17.md`, `analysis/ft8-vs-wsjtx-2026-11-18.md`.
- Output channels ch4 / ch5 (external speaker right / left). Not tested: ch2/3 (headphones), ch6 (RCA), ch7 (internal speaker); SWR / PA / transmit anything.

## Decoders (core/src/decode_*.cpp, ft8_osd.cpp, ft8_demod.cpp)
FT8 pipeline per 15 s slot: ft8_lib candidates + belief propagation; up to 3 passes with **signal subtraction** (decoded signals are rebuilt, fitted and removed); **BP + OSD** fallback (after WSJT-X);
**deep stage** (per-candidate fine sync, five soft-bit sets from 1/2/3 coherent symbols, a-priori "a1" CQ / "a2" my-call passes); WSJT-X's plausibility rules (callsign shape, CQ rules); on weak decodes a **location check** (callsign prefix vs locator, `core/src/dxcc.cpp`, from cty.dat: 0 of 1190 genuine decodes flagged, removed the 2 likely false decodes). Message types ft8_lib cannot
unpack are added here: ARRL RTTY Roundup (full multiplier table), Field Day, DXpedition, EU VHF (bit-exact with WSJT-X: `app/tests/ft8/ft8code_message_types.txt`). SNR calibrated to WSJT-X (FT8 +1.7 dB, FT4 +2.4 dB).
FT4 has its own **deep stage** (2026-11-18, after WSJT-X ft4_decode.f90: 666.67 Hz baseband, sync over the full -0.5..+1.5 s range in three segments, soft bits from 1/2/4 coherent symbols, a1/a2 a-priori passes in the scrambled domain); it found all 24 of jt9's decodes in 34 real slots (before: 21). Debug switches: `FT8_DEBUG_DROP` (why a decode was refused), `FT8_DEBUG_DEEP` (what the deep stage saw), `FT8_DEBUG_OSD`, `FT8_NO_DEEP4` (FT4 without the deep stage). Tuning loop: `tools/ft8bench.py` (cached jt9 answers); full path: `tools/compare_ft8.py CAPTURE.wav [--mode ft4]`; `ft8bench --noise N` counts false decodes in noise.
WSJT-X (GPL v3) is credited in `NOTICE` for the reimplemented methods and message tables.

## AM
`Demodulator` AmDetector::Sync (default): second-order PLL on the carrier, coherent detection, optional single sideband via a Hilbert transformer; no fading distortion; lock and carrier offset shown in the status line.
`firesideiq --am env|sync|sync-lsb|sync-usb`. Tests: `tests/test_sam.cpp`.

## Band plan
Spectrum strip along the TOP: RSGB 2026 usage inside the Ofcom Full-licence allocations (OFW611, Feb 2024; `core/src/bandplan.cpp`); red = inside a band but not a UK allocation (the 60 m gaps);
hover for usage, primary/secondary and maximum power. FT8/FT4 dial frequencies as ticks. A guide only.

    cmake -S app -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
    ./build/gui/fireside              # pick a MIDI device, "Identify (read-only)"
    -DFLEX_BUILD_GUI=OFF             # core library + tests only (no Qt needed)

## Layout
- `core/` (`fireside_core`, static lib, no Qt): `protocol` (SysEx codec), `opcodes` (+ generated `opcodes_gen.h`), `safety` (write gates), `rawmidi`
  (POSIX raw MIDI + SysEx parser), `link` (MidiLink), `client` (safety-gated get/set), `identify` (read-only identity and firmware checks),
  `eeprom` (read, verify, decode, CSV, read-only backup files), `tuning` (DDS tuning word), `firewire` (`/dev/fwN` cdev client, `IBus`/`FwSys` seams), `pal` (`PalMailbox` link, `probe_order`, read-only event read), `startup` (start-up/shutdown writes: port of the Opus-reviewed sequence, FireWire-mailbox-only, value-specific exemptions, PA-bias-off at shutdown; **not yet independently diff-checked**).
- `tests/`: dependency-free harness, a pty simulated radio, and **conformance vectors generated by the Python implementation**
  (`tests/vectors/`: frames, replies, tuning words, a synthetic EEPROM image with its expected decode, byte-exact CSV).
- `gui/`: Qt 6 Widgets. **Look and layout (redesigned 2026-11-14):** one dark theme with a single accent (`Theme.h`), a header (digit-by-digit `FrequencyDisplay`: scroll over a digit or click its top/bottom half; tuning-step buttons; segmented `SMeter` with peak hold), spectrum over waterfall in one panel, and a control deck of labelled cards (Band, Mode, Filter, Audio, Noise, Source, Advanced). Filter widths per mode come from `dsp::filter_widths/passband_for`. Details below. Tab **Receiver (recorded I/Q)**: spectrum + waterfall (`SpectrumWidget`, `WaterfallWidget`, maths in the Qt-free `fireside/display.h`), mode/AGC/NB/mirror/IF-offset
  controls, play/pause/loop at real-time pace, optional speaker output (`AudioOut`, Qt Multimedia, 48 kHz mono, drops audio rather than growing latency), hover readout, status line with peak
  and (uncalibrated) signal level. Sources: `FileIqSource` (WAV, RX1 I/Q = ch1/ch2 of an 8-ch capture), `DemoIqSource` (synthetic). With a started radio the dial box and clicks tune it (see Daily use).
  Tab **Radio**: `RadioPanel` = FireWire session controls. Connect and Identify are read-only. **Writes are off by default**: Start needs the "Allow writes" box ticked *before* connecting, plus a confirmation dialog
  (receive only, the transmitter is never keyed). Once started, the receiver's dial box and click-to-tune retune the radio through the Tuner (rate-limited), the IF offset is locked to 9 kHz (the DDS sits 9 kHz below the dial, as PowerSDR; the DSP shifts it back),
  and closing the window or Stop runs the reviewed shutdown (a failed shutdown is reported, never swallowed). Connect/Start run on the GUI thread with a short boot wait (3 s). Also the MIDI identify row (reads only). Everything runs in the GUI thread (fine at 48 kHz; the receiver keeps up with the 192 kHz direct stream).
  New files default to "IF offset 0" (most recordings are centred on the dial); live radio sources and the demo use 9 kHz.
  Headless checks: `fireside --smoke-receiver [--screenshot out.png]` (manual stepping), `fireside --smoke-threaded` (real worker thread: GUI heartbeat stays ~100 %, audio flows, click-to-tune moves the dial) as CTests; `fireside --smoke-radio [--screenshot f.png]` drives connect/start/click-to-tune/stop against the simulated radio.

## Receive chain, CAT, tools (job 3, hardware-free)
- `dsp` (`fireside/dsp.h`): own radix-2 FFT, 9 kHz IF shift (`IfShifter`, optional I/Q `mirror`), `SpectrumAnalyzer` (Blackman-Harris, 50 % overlap, dBFS, optional averaging),
  `Waterfall`, streaming `Demodulator` (USB/LSB/AM/CW, FIR bandpass, integer decimation 192/96 -> 48 kHz), `ReceiveChain`, `DeadStreamWatchdog` (1 s of exactly-zero buffers),
  `Agc` (Fast/Med/Slow, instant attack, hang+decay; output never exceeds the target), `NoiseBlanker` (impulse), FM mode, in-passband signal meter (`signal_dbfs`, pre-AGC) with `s_reading`/`cat_smeter_value` (S9 = -73 dBm; needs the radio's full-scale dBm calibration, unknown), `synth_tones`, WAV read/write (PCM16/24/32, float, extensible; picks RX1 I/Q = ch0/ch1 of an 8-ch capture). **This is not WDSP** (no dev package on the host); the interfaces are
  small so WDSP can replace `Demodulator` later. **AGC Off needs a gain:** the radio's band I/Q is about -90 dBFS, so with the AGC off the audio is inaudible at unity gain. The Radio screen has a **Gain** slider (0-100 dB, default 60, active only while the AGC is Off, remembered; `DemodConfig::manual_gain_db`, soft-limited with tanh so strong stations cannot blast). No noise reduction, notch, squelch or de-emphasis yet. AGC/NB are off unless `DemodConfig` enables them.
- `rx_engine` (`fireside/rx_engine.h`): source -> ReceiveChain on a worker thread. Non-live sources are paced in real time, live ones consumed as delivered; spectrum frames and audio go into bounded queues (oldest dropped and counted);
  `threaded=false` gives deterministic manual stepping for tests. Thread-safe `take_frames/take_audio/status/configure`; a source error stops the engine and is reported in `status().error`. ThreadSanitizer clean.
- NR / notch / squelch (`dsp.h`): `AdaptiveLineFilter` (NLMS on a delayed reference): Prediction = noise reduction (measured +6.5 dB SNR on a tone in noise), Residual = auto-notch (a steady tone drops > 20 dB, noise floor kept);
  `Notch` (RBJ biquad, manual audio frequency); FM-agnostic squelch on a 10 ms in-passband level detector with 2 dB hysteresis and a click-free ramp. All off by default.
- `session` (`fireside/session.h`): `RadioSession` is the ONE orchestration of connect (read-only, byte order proven) -> identify -> start (reviewed start-up, then the Tuner is armed) -> tune -> shutdown. The `firesidecli` CLI and the GUI both use it.
  `sim_bus.h` is a simulated FLEX-5000A register window (also what the tests use): `fireside --simulate` runs the whole GUI session path with no hardware.
- `cat` (`fireside/cat.h`): Kenwood TS-2000 subset (ID FA FB FR FT MD IF PS AI AG SM RX), per-connection buffering, loopback-only TCP listener. **TX is refused** (`?;`, counted) and so is PS0:
  the output-channel question is open. Callbacks `on_rx_freq`/`on_mode` retune the radio through the Tuner in `firesidecli serve` (CAT on 127.0.0.1:4532).
- `firesideiq` (`tools/firesideiq.cpp`): file-only. `firesideiq in.wav --mode usb --audio out.wav --waterfall w.pgm --spectrum s.csv`; `firesideiq --make-test t.wav` writes a synthetic demo.
  Use it on `arecord -c 8 -f S32_LE` captures from hardware day (settles the I/Q mirror question: `--mirror` vs not, known carrier above/below the dial).
- `capture` (`fireside/capture.h`, `alsa_pcm.cpp`): **read-only live capture**. `PcmIqSource` over an `IPcm` (real: `AlsaPcm`, capture stream only; fake in tests) converts 8-ch S32_LE to I/Q (ch0/ch1),
  keeps per-channel RMS/peak, xrun count and the 1 s dead-stream flag; `list_capture_cards` (FLEX/DICE first), `probe_alsa_pcm`, `analyze_iq` (carrier + image rejection: tells a true quadrature pair from Q-missing/duplicated).
  Tested against fakes and against ALSA's silent `null` PCM (so the real libasound path runs in CI). A static test (`tools/tests/test_capture_readonly.py`) forbids playback/mixer/control/MIDI/radio-write calls in these files.
- GUI logging, two steps on the Radio screen: the **Lookup** card (callsign, Enter -> country, worked-before status in the small box); Enter again (or the Log button) opens the large **Log QSO** form, which takes the place of the Band and Mode/filter cards with the call, reports (59/599 by mode, or from a double-clicked FT8/FT4 decode), grid, name and comment prefilled; Enter in any field logs, Esc/Cancel closes it. The passband-width buttons live in the Mode/filter card. The Log tab keeps the connection settings and the history. Code: `gui/LogPanel.*`, core `logbook.*`. No logbook of our own; QSOs go to the user's Wavelog (REST API v2, Bearer `wl2_` token). Server URL + token live in `~/.config/fireside/wavelog.env` (mode 600, outside the repo, never in QSettings). Log QSO writes a local ADIF copy first (`~/.local/share/fireside/log.adi`), then POSTs; failures keep the QSO in `unsent.adi` for `Send unsent`. Typing a callsign does a worked-before lookup; optional push of dial frequency/mode to Wavelog's radio API (never power). Tested against a fake server (`fireside --smoke-log`) and, by hand, the real instance.
- Mirror I/Q is remembered per kind of source: the live radio comes up mirrored (the radio's I/Q is mirrored, confirmed on air), files and the demo keep their own setting (off unless you tick it for a recording captured from the radio). The Python `firesidectl startup --freq` takes the DIAL and tunes the DDS 9 kHz below it, like the C++ session.
- Receive preamp: a **Preamp** button in the Band card (usable only while the radio is started, off at every start-up and shutdown). One reviewed write path, `core/src/preamp.cpp` (`RxPreamp`, like `Tuner`): only `SET_TRX_PREAMP`, only 0/1, armed after start-up, idempotent, a mailbox fault disarms it; the session switches it off first at shutdown. Receive gain only; checked by `tools/tests/test_tune_guard.py`, `test_preamp.cpp` and the radio smoke test.
- Hardware-day tab: the `HARDWARE-DAY.md` order of work as tick boxes (remembered), a Copy button per command, and read-only live checks of this PC (FireWire controller on the PCI bus, `firewire_core`/`firewire_ohci` loaded, `/dev/fw*`, devices on the bus). It only looks at `/sys` and `/dev` file names: no device I/O, never runs anything, never contacts the radio. Code: `gui/HardwareDayPanel.*`; test `fireside --smoke-hwday` against a fake tree.
- FT4: the Decoder card also has FT4 (7.5 s slots, same ft8_lib). Verified on WSJT-X's own sample (`recordings/ft4/000000_000002.wav`, 7 messages decoded; not in git) and generated signals. FT4 SNR is not calibrated against WSJT-X yet (FT8's is). Double-clicking a decoded FT8/FT4 line fills the Log tab (callsign, grid, our report, their report if addressed to us) and runs the worked-before lookup. Lines addressed to my call (also `/P`, `EA8/`, `<hashed>`) are highlighted red, and the first station calling fills the entry and is looked up by itself (Wavelog + callbook); later callers leave it alone (the status line names them), a call you typed is never overwritten, and a station logged in the last 10 minutes does not refill it.
- QRZ.com callbook, straight from QRZ (not through Wavelog): the **QRZ.com callbook** card on the Log tab (username, password, **Save and log in**;
  saved in `~/.config/fireside/qrz.env`, mode 600, never in QSettings). Callers and the entry are always looked up; with **Also look up every decoded
  station** (default on) every FT8/FT4 station is too. At most one request a second, callers first, queue capped at 60; cache
  `~/.cache/fireside/qrz-cache.tsv` (30 days; not found 3 days). The decoder line gets `  │ Klaus, Ulm, Germany, 770 km` (grey; the line parser stops
  at the `│`); the lookup box shows QRZ's name, town, grid, distance and bearing from the station grid, LoTW / eQSL / paper QSL. E-mail and street are
  never parsed, kept or shown. The status shows QRZ's own 24-hour lookup count. Wavelog is now asked for worked-before only (no second callbook lookup).
  Core `fireside/qrz.*` (parsing, cache lines, locators), GUI `QrzClient.*`; tested against a fake QRZ in `fireside --smoke-log` (wrong password,
  login, lookup, cache, not found, session timeout and re-login).
- Filters: besides the preset width buttons the Mode/filter card has Low and High cut boxes (SSB; AM/CW/FM get a single Width) and a Soft/Normal/Sharp skirt setting; the edges of the shaded passband on the spectrum can be dragged. Cuts, shape and memories remember them. Core: `passband_for(mode, width, low_cut)`, `DemodConfig::edge_fraction/edge_floor_hz`.
- Spectrum mouse: click tunes; drag a passband edge to reshape the filter; Ctrl+drag inside the passband slides it (SSB); Alt+click adds or removes a manual notch (up to 4, red N markers, drag to move; the Notch box is number 1; SSB and CW only). Core: `DemodConfig::extra_notches_hz`.
- `firesideplay` (`tools/firesideplay.cpp`): the output-channel test. `firesideplay <card-id> --channel N` (or `--all`) plays a quiet 1 kHz tone (default -40 dBFS, refuses above -20) on one channel at a time so you can note which jack it comes out of. Audio only; radio volume at minimum first. Playback is opened in exactly one file (`core/src/alsa_play.cpp`), enforced by `tools/tests/test_capture_readonly.py`.
- GUI Audio card: `Output: PC sound` (default) or `Output: Radio jacks` (`RadioAudioOut`). The radio option needs a Live capture open (it names the card) and an explicit channel pair (none by default, confirm dialog); it is never remembered across runs. Do not use before the firesideplay test.
- `firesidecapture` (`tools/firesidecapture.cpp`): `--list` (cards, channels, S32_LE, 48/96/192 kHz) and `firesidecapture <card-id|device> --seconds 5 --out cap.wav`: records all channels, prints per-channel levels and the hardware-day checks.
  The Device tab's "ALSA capture..." button opens the same source in the GUI; `firesidecapture direct` is the direct-stream version.
- **Direct FireWire stream, 48/96/192 kHz** (`core/src/fw_iso.cpp`, `FwIsoPcm`): snd-dice offers the radio at 48 kHz only, so the app can receive the radio's
  isochronous stream itself on the controller's own node (`/dev/fw0`, third rule in `system/70-flexradio.rules`). Writes only three DICE sound-chip registers
  (clock, TX channel, enable), restored on close; no radio commands; refuses while ALSA is streaming. GUI: the Radio screen's 48 / 96 / 192 buttons (radio-jacks audio output is unavailable in this mode)
  (192 kHz preselected; radio-jacks audio output unavailable in this mode). CLI: `firesidecapture direct --rate 192000 --seconds 5 --out cap.wav`.
  `fireside --bench-direct 192000 10 [shot.png]` measures whether the window keeps up (it does: 100 % real time, 0 xruns). Details: analysis/audio-stream-layout.md.
- `tune` (`fireside/tune.h`): **the only retune path.** `Tuner` sends exactly one opcode (`SET_RX1_FREQ_TW`), range-checks (0 < DDS and dial <= 65 MHz, finite), rate-limits to one write per `min_interval_s`
  (default 50 ms) with coalescing (latest wins; `pump()` from a timer), works only after `arm()` (call after a completed `run_startup`), requires the FireWire mailbox and `allow_write`, and **disarms on any
  mailbox fault** (no retry storm). `connect_cat_to_tuner` is the CAT hook; CAT itself never touches the link. `if_offset_hz` is the 9 kHz IF offset (the session passes 9000: the DDS is tuned 9 kHz below the dial).
  `tools/tests/test_tune_guard.py` fails if tune.cpp writes anything else or if CAT/GUI code writes. Click-to-tune is NOT wired: the GUI has no radio session yet. Python has no mirror of this (app-only feature).
- `cli` + `tools/firesidecli.cpp`: the C++ command line (`firesidecli list | --fw /dev/fwN [--byte-order ..] [--allow-write] identify | startup | shutdown | serve`). Logic lives in `core/cli.cpp` behind an injectable
  bus (tested end to end on a fake bus, incl. `serve` with a real TCP client). **Review finding F4 is enforced**: every radio command first proves the byte order (`probe_order`, then `PalMailbox::verify()`: a firmware
  revision in the supported range read over *that* mailbox); `require_firewire` and `Tuner` refuse an unverified mailbox, even with an explicit `--byte-order`. `serve` = startup, then Tuner + loopback CAT
  (TX refused) until Ctrl-C, then the reviewed shutdown. Writes need `--allow-write` (checked before the bus is even opened). The 9 kHz IF offset is not offered. Exit codes as the Python CLI.
- **Any I/Q WAV works** (`open_iq_file`): files at 48/96/192 kHz are used as they are; anything else (16 kHz, 44.1 kHz, 62.5 kHz, 250 kHz, 2.4 MHz...) goes through a streaming polyphase resampler (`dsp::RationalResampler`, > 70 dB stopband, tones keep frequency and level)
  to 48/96/192 kHz. WAVs are streamed from disk (plain, extensible and RF64 headers), so multi-hundred-MB files are fine. Used by the app (Open file...), `firesideiq`, and the demo loader. Drop recordings in `~/fireside-sdr/recordings/` (git-ignored).
- **Text decoders** (`fireside/decode.h`, receive-only, on the demodulated 48 kHz audio; Decoder card in the Radio screen): **CW** (adaptive speed from pulse clustering, Farnsworth letter/word gaps, four-pole narrow filter, pitch tracking,
  drops the partly heard first character; a signal counts only if it is keyed: its envelope must sit at two separate levels, which stops band noise, crashes and switch-mode QRM printing streams of E / T / I), **PSK31** (differential BPSK, 16 candidate symbol timings, standard G3PLX Varicode, SNR gate from noise references, plus a PSK31 shape test: 0/180 degree phase steps and symbol timing that matters, so keyed CW and FT8 tones print nothing; coarse spectral centre search + AFC: signals up to +-14 Hz off) and
  **RTTY** (ITA2 letters/figures, unshift-on-space, any shift/baud/polarity). They run on the engine's worker thread (`RxEngine::set_decoder/take_text`), so no audio is lost if the display lags. Tested on generated signals (clean, noisy, faded,
  off-frequency, chunked, silence/noise only) and through the whole receive path (`gui_decode_smoke`). **CW is tuned on a real band** (the CWops CWT on 40 m, 2026-11-18: callsigns and exchanges copy; noise channels went from ~1100 junk characters in 38 minutes to 4); **PSK31**'s garbage rejection is tuned on the same recording (130 CW / FT8 channels: 645 -> 2 characters; weak PSK31 copy nearly unchanged) but no real PSK31 has been decoded yet; **RTTY is not yet tried on real on-air signals**: send a recording and I will tune against it. **FT8** (the Decoder card's fourth entry) uses the vendored MIT-licensed **ft8_lib** (`third_party/ft8_lib`, upstream commit in its README): sync search, LDPC(174,91), CRC and message unpacking come from the library; our wrapper
  (`core/src/decode_ft8.cpp`) does the 48 -> 12 kHz resampling, 15 s slot timing (UTC for live audio; a file is taken to start at a slot boundary, partly heard first slots are skipped), a persistent callsign hash table, background decoding
  (the engine is never stalled; lines appear a moment after each slot ends), and a **measured SNR estimate** (calibrated on real recordings: mean error 0 dB, scatter ~5 dB). Verified on **real on-air recordings** against WSJT-X's own decodes
  (`tests/ft8/`, four 15 s slots from ft8_lib's test set): ~75-85 % of WSJT-X's messages decoded, identical text, frequencies within 2 Hz, no false decodes; plus a generated-signal suite (all common message types, noise to about -12 dB, simultaneous
  signals, streaming slots, chunking). A 48 kHz copy of one real slot for trying it in the app: `recordings/FT8_real_191111_110615_48k_IQ.wav` (git-ignored; Open file..., then pick FT8).
- **Waterfall colours and mouse wheel** (Radio screen): four palettes (Classic, Inferno, Viridis, Gray) and a Floor slider recolour the whole waterfall history at once; the mouse wheel over the spectrum or waterfall tunes by the tuning step (Shift = 10x). Both are remembered.
- **Keys, memories, remembered settings** (Radio screen): Left/Right tune by the tuning step (Shift = 10x), PgUp/PgDn change band, M mutes, Space plays/pauses. A **Memories** card saves/recalls frequency + mode + filter by name.
  Mode, filter, AGC, noise settings, volume, step, dial, memories, window size and tab are remembered between runs (QSettings under ~/.config/fireside); **"Allow writes" is never remembered**. `FIRESIDE_NO_SETTINGS=1` disables this (the smoke tests set it).
  Launcher: `system/install-launcher.sh` (desktop entry + icon in ~/.local, no sudo; `--simulate` variant, `--remove`).
- Default build type is now RelWithDebInfo (the DSP tests are ~10x slower at -O0).
- The DSP and CAT code is new (no Python counterpart); its tests are analytical (known tones, levels, sideband rejection), not conformance vectors.

## Radio transmit on the Radio screen (2026-11-19)
Open **Transmit** under the decoder: the **Radio transmit (ATU, power, SWR)** card sits above the offline renderer. Everything goes through
`RadioSession::transmit()` -> `fireside/tx_control.h` (the same code `firesidekey` uses; the TxGate, its limits and the hard drive clamp live there).
- **Arm transmit** (runs by itself once the radio is started; **Retry transmit check** if it failed): reads the EEPROM (read-only, ~16 s), compares it with the newest
  `F5K_*_trx.bin` in `~/.local/share/fireside/backups` (or `$FIRESIDE_BACKUP_DIR`) byte for byte, then sets the PA bias from it. We never write the EEPROM: it is read
  only because the PA bias comes from it, and a corrupted EEPROM must never set the bias. **First time, with no backup:** the app asks "Save this radio's calibration?"; on yes it reads
  the EEPROM twice (about half a minute, both reads must match and be a usable calibration), saves the copy (read-only file, never overwritten) and arms. If the EEPROM
  ever differs from the saved copy, transmit is refused and the message says how many bytes differ. Stopping the radio disarms. Receive never depends on the backup.
- **Tune ATU** at the chosen power (default 3 W, ~4 W into a match) and SWR threshold; **Stop** unkeys at once. A pass is stored per band in
  `~/.config/fireside/atu-memory.txt`; **Restore ATU** (and, ticked, every band change) puts the band's relays back **without RF**.
- **Measure drive (2 W)**: a ramp that measures the band's drive level (`~/.config/fireside/tx-drive.txt`, shared with `firesidekey`); needed once per band
  before Tune works there. There is **no built-in default**: a level measured on one radio is not trusted for another (this radio's 20 m value, 11 150 W
  per drive^2, is in that file).
- **Test tone (2 W, 8 s)**: a steady tone exactly **1 kHz above the dial**, to find on another receiver. **Mirror TX I/Q** is on by default: verified on the
  air 2026-11-19 (WebSDR: heard at dial + 1 kHz with it, nothing on USB without it); the radio wants TX mirrored like receive.
- **Click to reply + auto sequence + auto log** (box "Auto sequence + log", on by default): double-click a decoded CQ, or a station calling you, and the
  card sets the opposite slot and the right message (`DX YOURCALL GRID`, or the report / R-report / RR73 if you click later in their exchange) and queues it.
  From then on `fireside/qso_seq.*` answers each step in your slots: their report -> `R-xx`, their RR73 -> `73` (sent once), or, answering a caller,
  their R-report -> `RR73`. A message is repeated up to 6 times (CQ up to 12) and then it gives up; Stop ends it. The QSO is logged to Wavelog by itself
  (call, grid, both reports, QRZ first name, and TX_PWR = the measured mean forward power of the last over) once the completing over has gone out. Send with a `CQ ...` message calls CQ the same way: the first
  station answering is worked. Timing: when receive pauses ~0.8 s before your over, the decoder first decodes the slot heard so far (the caller's
  signals end ~1.2 s before), so an answer is never lost and the next message is chosen from it (decode ~1.4 s on a busy band: the over then joins
  ~0.6 s late, inside the 2 s allowed). Tested on the simulated radio: a whole FT4 QSO, reply to 73 and the log entry (`fireside --smoke-radio`).
- Layout (user 2026-11-19): the **Transmit power** meter sits at the top next to the Signal meter, in the same segmented style (forward accent,
  reflected amber, square-root 0-30 W scale, peak hold; fwd / ref / SWR while keyed, the last over's figures after). The decoder is split: left, every
  decode; right, the **QSO pane**: only what is addressed to your call (red) and your own overs (amber `TX`; nothing else, not even the station you are working talking to others), always scrolled to the newest line, with a
  `── QSO with CALL ──` line when a QSO starts; double-click there to reply too. The top four lines of the decoded text are the **callers area**: everyone who has called your call stays there, fading, until you answer them (they then move to the QSO pane) or they have gone unanswered for 6 FT8 / 10 FT4 cycles; a repeat call refreshes the entry, only the newest four show, and a double-click replies. A **CQ** button next to Send calls `CQ yourcall yourgrid` (it needs your callsign in the **my call** box or the Log tab, and your locator from the Log tab's station profile or a CQ typed once in the message box: it never guesses; auto sequence works
  the first answer).
- Tabs (user 2026-11-19): **Radio** (the operating screen), Antennas, **Device** (FireWire / session, was "Radio"), Log, Audio; the Hardware
  day checklist is gone (its job was done). An **Antenna** card on the Radio screen picks the receive antenna for the band the dial is in
  (the Antennas table remembers it; an explicit choice switches even with automatic switching off); transmit stays on ANT1, the ATU's.
- Mode card (user 2026-11-19): **FT8 / FT4** buttons tune to the band's FT frequency (band plan marks), USB, the wide filter and that
  decoder; the Transmit card's FT8 / FT4 follows the decoder. **Auto LSB / USB** (on by default): entering a band in USB / LSB picks LSB on
  160 / 80 / 40 m, USB elsewhere; a recalled memory and the restored settings keep their own mode; voice Auto follows the receiver.
- Click-to-tune lands where you click (10 Hz). TX antenna per band (Antennas tab; Antenna card buttons RX / TX); only ANT1 goes through the
  ATU, so Tune / Restore ATU are refused on a band set to ANT2 / ANT3. Mirror I/Q, IF offset and Loop file moved to the Device tab; the
  Waterfall card is two columns.
- **NR2** (user 2026-11-20; `fireside/nr2.h`, `third_party/wdsp-nr2`): WDSP 2.11's spectral noise reduction (EMNR: 4096-point FFT, 4x overlap, trained gain, artifact elimination), vendored
  with a preprocessor-only patch (see its README / PATCHES.md; GPL v2+). `DemodConfig::spectral_nr`, after the line filter in `Demodulator::process`; voice modes only (CW ignored), the GUI
  does not pass it while a data decoder (PSK31 / RTTY / FT8 / FT4) is on. Optional: built only if `libfftw3.so.3` is found (`FIRESIDE_HAVE_NR2`); the button is hidden otherwise. Post-processing (WDSP's "post2", off upstream) is switched ON: it adds 3-4 dB. Measured through
  the app's own demodulator on WSJT-X speech in real 40 m noise (AGC off, output SNR vs the clean speech): +5.2 dB over no NR at 3 and 12 dB input SNR, where the line filter was 2.3 / 3.3 dB worse than nothing; adds ~86 ms of delay and ~1 % of a core (the line filter: 21 %).
- **Band memory** (user 2026-11-20): each band keeps where it was left (dial, mode, filter, low cut, decoder: `ReceiverPanel::BandMem`), updated as
  you tune or change mode; the band buttons and PgUp/PgDn go back there (`recallBand`). A band never visited: its usual start, or its FT8 / FT4
  mark while that decoder is on. QSettings `receiver/band_memory`. The antenna per band stays the Antennas tab's.
- **CAT server** (user 2026-11-20; `fireside/rigctl.h`, `gui/CatPanel.*`, Device tab): the Hamlib NET rigctl protocol (rigctld, model 2) on
  127.0.0.1:4532, on by default. get/set freq and mode (USB LSB AM CW FM, PKTUSB / PKTLSB = USB / LSB with the data flag; a data decoder reports
  PKT until the client picks), VFO A only, no split, \chk_vfo, \dump_state (no transmit ranges, ptt_type 0: Hamlib's client still sends PTT), \get_vfo_info, \get_lock_mode,
  the "+ ; | ," extended responses. Reply formats copied from Hamlib 4.7.2's rigctld; checked with `rigctl -m 2`. Tunes through
  `ReceiverPanel::setDialHz` (the one tuning path); refused (RPRT -9) while keyed or a transmit job runs. The Backend has no transmit call;
  send_morse, send_voice_mem and power-off answer RPRT -9. **PTT** (user 2026-11-20): `T 1/2/3` goes only to `rigctl::PttRequest`, whose one
  implementation is the Transmit card (below); with none attached, or refused, RPRT -9. `T 0` always passes. Static guards in tools/tests/test_tune_guard.py.
  The older Kenwood TS-2000 `cat.h` stays for the CLI and still refuses TX.
- **Other programs (WSJT-X, JS8Call, fldigi ...)** (user 2026-11-20): `system/53-fireside-virtual-audio.conf` (installed by `setup-system.sh`) makes two
  PipeWire null sinks. **Fireside RX audio**: the receiver pushes a second copy of its audio there at a fixed level (Audio tab, not the Volume slider or Mute);
  the other program records its monitor. **Fireside TX audio**: a loopback; the other program plays into its sink and we capture "Fireside TX audio (input)" (Qt does not list monitor sources). **Keying over CAT**: the Transmit
  card's "Let other programs key" (off in every new window, never remembered) and power (1..100 W, default 20). An accepted `T 1` runs `TxControl::voice()` with
  `tx::data_chain_defaults()` (no processing, SSB 100..3000 Hz, the same ceilings), that input, `max_key_s` 120, while: the tick is on, transmit is armed,
  the owning client holds PTT and has sent a command in the last 15 s; amateur band with a measured drive level; the direct duplex stream running. One owner;
  `T 0`, a disconnect, unticking, Stop or a radio stop unkey. Tests: rigctl PTT tests, a simulated data over (`test_atu_tx.cpp`), and `--smoke-radio`
  (a real socket client: refused without the tick, keyed, owner only, dial locked, T 0 / untick / disconnect / watchdog unkey, out of band refused).
- **External program** (decoder card, remembered): holds our decoder off (a decoder choice still sets its mode and filter), turns off auto sequence + log
  and the PSK Reporter upload (greyed), and turns on the Fireside RX audio feed; unticking restores the previous choices (QSettings `fireside/external`).
- **PSK Reporter tab** (user 2026-11-19): where this station is heard, live from the PSK Reporter MQTT feed (mqtt.pskreporter.info:1884 TLS,
  MQTT 3.1.1, topic pskr/filter/v2/+/+/<my call>/#; only my call is sent, generic client id, no account). Table of receivers (distance / bearing
  from the Log tab's grid, last / best SNR, reports), summary over 15 min .. 24 h (this band or all), a one-line summary in the Radio screen's
  status; every report appended to ~/.local/share/fireside/pskreporter-heard.csv with the "Antenna note" (for comparing antennas). Live only
  (no history from before the app opened). Uploading my decodes (tick box, off by default): live FT8 / FT4
  decodes (never a recording) -> IPFIX over UDP to report.pskreporter.info:4739 (fireside/pskr Uploader: our own template ids 0x6A42 / 0x6A43;
  one datagram per 5 min + 0..60 s, sooner only when full; the same source port; templates in the first three then hourly; each station once
  per band per hour; what is waiting is sent when the app closes). The receiver template matches pskdev.html byte for byte (test). The old test
  listener (port 14739) showed nothing on psk-analysis.pl; check that page after the first real upload.
- **Audio tab** (user 2026-11-19): receive output by name (PC speakers / radio powered speakers / headphones / line out); transmit input
  = the radio's front mic socket (input ch7 of the duplex stream, same clock, mic gain 0..-40 dB in the radio's codec) or any PC capture device
  (drift-corrected FIFO); input gain; a level meter (Test input, nothing transmits); processing: high / low-pass, downward expander, leveler,
  compressor, 10-band graphic EQ (32 Hz..16 kHz, +-15 dB). Saved in QSettings fireside/audio.
- **Voice (Transmit card)**: Auto (LSB < 10 MHz) / USB / LSB, PEP 1..100 W (default 20), PTT: hold the button, the space bar (if ticked; never while
  typing) or the mic / rear RCA PTT (READ_PTT polled, if ticked). `TxControl::voice`: input -> processing -> SSB -> the clamped transmit
  path; gate: forward 1.5 x PEP, SWR 3, 3-minute time-out; drive from the band's measured level, trimmed only down. Tested on the simulated
  radio (sideband, PEP, PTT, time-out, mic failure); not on the air yet (no microphone).
- **FT8 / FT4 send**: message, audio frequency (USB above the dial), even / odd slot, power, **Send** (no question: the status line shows what is queued and when; **Stop** cancels) and **Repeat** (calls in every slot of that
  parity until Stop). The signal is rendered at once; receive pauses only ~0.8 s before the slot and comes back after the over. Closed-loop power: the
  forward meter trims the drive to the set power in the first 3 s (constant envelope), and every clean over refines the band's drive level. Tested on the
  simulated radio (`app/tests/test_atu_tx.cpp`, FT8 overs on 5 x simulated time): every QSO message decodes, FT8 and FT4, 48 and 96 kHz, |DT| < 0.1 s;
  on the wire the signal is mirrored (below the carrier) and inside the slot; the loop fixes a drive level 40 % low or 80 % high; one 1.7 x too low trips
  the gate (forward > 1.5 x) within a second and teaches nothing; high SWR mid-over unkeys; joining up to 2 s late stays on time; a dying playback device,
  an empty message, a wrong rate, < 1 W or off-band all leave the radio unkeyed. On the air since 2026-11-19 (first CQ answered by DK0ULR).
- Meters: forward / reflected watts and SWR from the PA detector with this radio's EEPROM calibration; the **TX** lamp is red while keyed.
- If the unkey command ever fails (three tries), the card says UNKEY FAILED and a dialog tells you to power the radio off.
- Start-up: with "Start receiving when the app opens" (Device tab, remembered, on by default) the app connects, starts the radio and opens the stream at
  the last frequency and rate with no question. Order (user 2026-11-19): the radio starts, the EEPROM is read and compared with the backup (~16 s,
  read-only) and the PA bias set, and only then does receiving start. Once verified it is never read again while the app runs (a radio restart re-arms
  from the verified backup at once). There is no Arm button any more; "Retry transmit check" appears only if that check fails.
- **Tune** and **Measure drive** ask first ("listen first": a carrier on a band that may not be matched yet). Arm (no RF), Test tone and Send go straight
  ahead. While transmitting, the receive stream pauses (snd-dice playback and our direct receive stream cannot
  share the radio) and reopens afterwards.

## Transmit control, ATU and meters (connected to the GUI and used on the radio since 2026-11-19)
From PowerSDR's FWC/fwc_atu.cs (GPL), checked on the radio.
- `fireside/tx_gate.h` **TxGate**: the only code that keys (SET_MOX 1). Disarmed until `arm()`; every key-down has a deadline (max 15 s); meter
  readings unkey it above 20 W forward or SWR 5 (once >= 2 W); unkey always sends SET_MOX 0 (also after errors, and in the destructor) and waits for the
  firmware to take it. While keyed it holds the safety gate's **transmit grant**: without it SET_MOX 1 and ATU tune requests are refused everywhere.
  `run_atu_tune()`: the PowerSDR sequence (TX mode CWU, key, threshold + tune, poll tuner and PA meter every ~35 ms, unkey, mode back); refused unless a
  tune carrier is on the I/Q stream (supplied by `TxControl`).
- `fireside/atu.h` **Atu**: request / reply protocol (`[queue][b3][b2][code]`, 0x00FFFFFF = nothing), start-up init (auto off, standby, Lo-Z), thresholds
  1.1 .. 3.0 (sent right before every tune: PowerSDR never re-sent a changed threshold), `set_relays(ind, cap, hiz)` puts a saved tune back **without RF**
  (direct-set encoding verified on the radio: `SET_IND` carries Hi-Z in bit 7). Safety gate: relay / setting / report requests pass with a permit; memory tune / full tune only with
  the transmit grant; memory erase (42/43), reset (57), antenna 2 and unknown requests never.
- `fireside/atu_memory.h`: per-band ATU memory (inductor, capacitor, Hi/Lo-Z, SWR, power, frequency, time) as text for the settings file.
- `fireside/tx_meter.h`: forward / reflected watts and SWR from READ_PA_ADC 7 / 6 with this radio's EEPROM PA bridge and PA SWR (reflected correction)
  tables (PowerSDR FWCPAPower / FWCSWR, FlexRadio's original form). On a passing 20 m tune it read 11.4 W; the tuner said 10.6 W.
  (The EEPROM check's "PA SWR out of range" warning was a wrong rule: those are correction factors, 0.46 on 6 m is normal; range now 0.2..5.)
- Tests: `test_atu_tx.cpp` (16, against a simulated tuner that behaves like the radio's) and static guards in `tools/tests/test_tune_guard.py`
  (atu.cpp writes only ATU_SEND_CMD, tx_gate.cpp only SET_MOX / SET_TX_DSP_MODE, nothing else sets the grant). Deliberately breaking the power cap,
  the deadline or the erase refusal fails them.

## Transmit DSP (radio-free by design: buffers and WAV files)
`fireside/tx_dsp.h`, `fireside/tx_render.h`, `tools/firesidetx.cpp`. Nothing here can key the radio: two CTests (`tests/tx_isolation.cmake`) fail if this
code includes any radio header (followed transitively, allowlist: `dsp.h decode.h tx_dsp.h tx_render.h`) or if the `firesidetx` binary links any
symbol from a radio, MIDI or sound-card module of `libfireside_core.a` (or imports ALSA/ioctl); self-tests prove both layers catch what they should.
The signals it renders reach the radio only through `TxControl` and the TxGate (the section above), never directly.
- **SSB modulator**: audio -> one-sided complex band-pass FIR -> I/Q, USB/LSB, 150-2800 Hz, unity gain. Opposite sideband, carrier and out-of-band > 50 dB
  down; the low edge must sit at least one transition width above 0 Hz, so a DC offset can never become a carrier (> 60 dB down, tested).
- **Mic chain** (`MicChain`): mic gain, high-pass, EQ (+-24 dB), low-pass, noise gate, compressor, optional clipper, look-ahead peak limiter.
  **`TxChain`** = mic chain -> SSB -> a second limiter on the **I/Q envelope**: the SSB filter overshoots (clipped audio at 0.9 gives an envelope > 0.95,
  and overdriven speech reaches full scale), so the hard ceiling has to sit after it. The limiter cannot exceed its ceiling by construction (plus a
  backstop clamp), turns NaN/inf into silence, and its splatter when it acts is negligible (overdriven speech: -92 dBc opposite sideband, -88 dBc near the carrier).
- **CW**: raised-cosine shaped keying (1-20 ms, default 5: -62 dBc beyond 300 Hz, -88 beyond 1 kHz; hard keying -38 / -48), carrier on the dial
  (the receive CW mode makes the note), keyed length exact; `text_keying`; **iambic keyer** (modes A/B, element memory, weight, reverse) as pure logic.
- **Two-tone test + IMD3/IMD5 measurement** (`measure_imd`, matches a closed-form cubic to 0.01 dB). Default chain with compressor: IM3 -63 dBc, IM5 -72 dBc.
- **FT8/FT4 slot scheduler**: pure functions of UTC (slot, parity, next slot with lead time, late join). The 0.5 s start offset is the one in the WSJT-X authors' QEX paper (July/August 2020: "the nominal start time, 0.5 s after the beginning of a transmit interval"; one statement covering both modes).
- **`firesidetx`**: `firesidetx ssb in.wav out.wav [--lsb] [--no-mic-chain] [--gain dB]`, `firesidetx ft8|ft4 "CQ N0CALL AA00" out.wav [--freq 1000] [--lsb] [--level -6]`,
  `firesidetx cw "CQ DE N0CALL" out.wav [--wpm 20] [--edge 5]`, `firesidetx twotone out.wav [--seconds 4] [--peak -6]`. Writes 48 kHz float32 I/Q (I = ch0, Q = ch1);
  every output ends in an I/Q limiter at -1 dBFS and louder requests are refused. Open the file in fireside (Open file...) to see it on the waterfall, or
  `fireside --decode-file out.wav ft8` / `fireside --screenshot-file out.wav out.png ft8`. Checked end to end: the receive app decodes what firesidetx writes.
- **Transmit section of the Radio screen** (`gui/TxPanel.*`; the "Transmit" button on the decoder row opens it under the decoder, because a transceiver is one display): a lab for the above with nothing connected. Pick FT8, FT4, CW (with six stored message memories), PSK31, RTTY, voice (from a WAV file), two-tone, or CW keyer practice (Z and X as paddles, recorded and run through the iambic keyer; no sound from this tab), USB or LSB;
  it shows whether an FT8/FT4 message can be encoded, the UTC clock with the even/odd slot countdown, renders on a worker thread, reports duration and peak (always
  <= -1 dBFS) and, for two-tone, the measured IM3/IM5. **Save WAV** writes the I/Q; **Preview on the receiver** loads it into the Radio screen (with the matching decoder)
  so you see it on the waterfall and the receive chain decodes it. There is no transmit key, no radio output and no sound output on that tab, and CTests keep it so:
  `tx_isolation_gui_source` allows only Qt, `Theme.h` and `tx_render.h` there and forbids words for a transmit key, a radio connection, the other panels and sound/network
  classes (each rule has a self-test); `gui_tx_smoke` renders every mode and decodes the preview.
- Round trips: FT8, FT4, PSK31, RTTY and CW go through the modulator and back through our own receiver and decoders.

## Python is the reference
`tools/firesidectl` is the prototype and the specification: behaviour must match. After changing the opcode table or EEPROM layout run
`python3 -I tools/gen_cpp_assets.py app`; `tools/tests/test_cpp_assets.py` fails if the checked-in header/vectors are stale.

## Deliberately not ported yet
- WDSP proper (AGC, NB, NR, notch, FM), GUI spectrum/waterfall widgets, wiring CAT to the radio, and any connection of the transmit DSP to the radio
  (output channel, levels and calibration, PA/ALC feedback, PTT/MOX, the guarded transmit gate): see the hardware-day list in `../NOTES.md`.
- JSON backup metadata (the Python tool writes it; C++ writes `.bin` and the PowerSDR-style `.csv`).

## Known deliberate differences from Python
- Unknown numeric opcodes are rejected (the Python client allows them with `allow_dangerous`).
- Undecodable frames while waiting for a reply are skipped (Python raises).

## Licence
GNU GPL version 3 or later (see `LICENSE` and `NOTICE` in the repository root). The notice lists what was borrowed: PowerSDR (GPL v2 or later), ft8_lib (MIT), KISS FFT (BSD-3).
