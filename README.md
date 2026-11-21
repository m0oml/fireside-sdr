# Fireside SDR

A native Linux application for the **FlexRadio FLEX-5000A** (the FireWire / DICE transceiver), written in C++17 with Qt 6. It talks to the radio over
FireWire, with no Windows, no PowerSDR and no Wine. The 5000A is a legacy radio and PowerSDR does not run natively on Linux, so this keeps it on the air.

This is an independent, unofficial project. It is not made, endorsed or supported by FlexRadio. See [NOTICE](NOTICE) for credits and licences.

## What it does
- **Receive:** the radio's own I/Q stream at 48, 96 or 192 kHz, spectrum and waterfall, USB / LSB / AM / CW / FM, AGC, noise blanker, notch, noise reduction
  (including an optional spectral noise reduction from WDSP), band memory, recording.
- **Decoders:** FT8 (94 % of WSJT-X's decodes on the same recordings), FT4, CW, PSK31, RTTY.
- **Transmit:** reviewed start-up, PA bias from the radio's EEPROM, ATU tune with a per-band memory, power and SWR meters, closed-loop power, FT8 and FT4
  (with an auto-sequencer). FT8 transmit has been verified on the air; SSB voice and CW are not yet used on the air from the app.
- **Around the radio:** logging to a [Wavelog](https://www.wavelog.org/) server, QRZ.com lookups, PSK Reporter uploads, and a Hamlib NET rigctl server
  so other programs (WSJT-X, JS8Call, fldigi ...) can follow the radio, plus two virtual audio devices for their receive and transmit audio. They can key the radio over CAT only while you allow it for that session.
- **Tools:** `firesidectl` (Python control and test tools), `firesidetx` (offline transmit rendering), `firesidekey` (command-line transmit).

## You need
- A FLEX-5000A with a FireWire (IEEE 1394) card. The Linux kernel's `snd-dice` driver and the `firewire-ohci` stack are used; see `system/` for the udev
  rules and the PipeWire / WirePlumber rule that keeps the sound server away from the radio's card.
- Qt 6 (Widgets, Network; Multimedia optional), CMake, Ninja, a C++17 compiler, ALSA, FFTW3 (for the spectral noise reduction).
- Python 3 for the tools in `tools/`.

## Build and test
    cmake -S app -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
    python3 -m unittest discover -s tools/tests

Run `build/gui/fireside`. Add `--simulate` to practise on a simulated radio, with nothing connected. Many tests that depend on one particular radio's EEPROM
image skip themselves when it is absent (see `app/tests/test_atu_tx.cpp`).

## Transmitting: read this first
This program can put RF on the air and write to a radio's settings. **You are responsible for your licence conditions, your antenna, your hardware and
your signal.** There is no warranty (see the licence).
- The app only transmits when you press a transmit button; the only code that keys the radio is the transmit gate (`app/core/include/fireside/tx_gate.h`),
  with static checks in `tools/tests/test_tune_guard.py`. Another program can key it over CAT only while "Let other programs key" is ticked (never remembered), through the same gate and limits.
- Drive levels are per radio and per band and are measured by the app (no built-in default). Start with low power and a proper antenna or a dummy load.
- Radio writes are off in every new window until you agree to "Start the radio?". Nothing writes the radio's EEPROM.
- It was developed against one radio. Treat the first transmissions on your own radio with care, and check the output with an external meter.

## Where to read more
- [app/USER-GUIDE.md](app/USER-GUIDE.md): using the app. [app/README.md](app/README.md): features and architecture.
- [protocol/](protocol/README.md): the FLEX-5000 control and stream protocol for anyone writing their own software (mailbox, MIDI SysEx, DICE streams, audio format).
- [HANDOFF.md](HANDOFF.md) and [NOTES.md](NOTES.md): project state, rules and a running log. `analysis/`: findings (the audio stream
  layout, transmit, decoder comparisons with WSJT-X). [UPSTREAM.md](UPSTREAM.md): the PowerSDR source this learned from.

## Licence
GNU General Public License, version 3 or later ([LICENSE](LICENSE)). Third-party code and its licences are listed in [NOTICE](NOTICE).
