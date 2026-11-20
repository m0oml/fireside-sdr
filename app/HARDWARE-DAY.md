# Hardware day: what to type, in order

The app's **Hardware day** tab shows this list with tick boxes, copy buttons for each command, and read-only live checks of the FireWire card, kernel modules and `/dev/fw*` on this PC. It never talks to the radio or runs anything.

Rehearsed against the simulators on 2026-11-15 (`tools/hardware-day --rehearse`, `fireside --simulate`, system check, firesideplay/firesidecapture `--list` on this PC). Everything here before step 7 is read-only. **Nothing here transmits.**

Before the first session run `system/setup-system.sh` once: it installs the udev rule and a WirePlumber rule (`system/52-flexradio-pipewire-ignore.conf`) so PipeWire leaves the radio's sound card alone (otherwise it grabs the card, can make the radio your default output, and the tools get "Device or resource busy").

0. Fit the radio's PCIe FireWire card (power off first), connect the radio, power it on, boot. Check `lspci -nn | grep -i 1394` (TI = vendor 104c).
1. `system/fireside-system-check.sh`. Expected today: groups, udev rule and the four kernel modules already OK; the two failures (no controller, no node) should turn OK. If `/dev/fw*` is missing: `sudo modprobe firewire-ohci`, then `dmesg | tail -30`.
2. `tools/hardware-day --out hardware-day` : read-only report (identity over MIDI and FireWire, DICE registers, EEPROM backup read twice and compared). Open the `report.md` it prints. **Skip `--events` for now** (it listens for paddles and PTT; do not press PTT).
3. If the report shows FAIL on "firewire identify": stop and send me `report.md` and `raw/`. Do not run anything with `--allow-write`.
4. `build/firesidecli list`, then `build/firesidecli --fw /dev/fwN identify` (read-only; proves the mailbox byte order).
5. Output-channel test: turn the radio's AF gain and headphone volume to **minimum**, then `build/firesideplay <card-id> --all` (one channel at a time, -40 dBFS). Write down which jack and side you hear for each channel 0..7. That settles `analysis/audio-stream-layout.md` section 4. Only then choose an output pair in the app.
6. With the radio in receive and a signal on ANT1: `build/firesidecapture --list`, `build/firesidecapture <card-id> --seconds 5 --out cap.wav` (checks ch2..7 silent, ch0/1 quadrature, stream alive), then `build/firesideiq cap.wav` with and without `--mirror`.
7. First writes (only after 1 to 6 passed): `build/firesidecli --fw /dev/fwN startup --dry-run`, then `--allow-write startup`, then in the app tick **Allow writes** and press Start (confirms). First light: the 9 kHz IF offset is applied after the first signal is seen.

Never run PowerSDR's factory-test forms or `FWCEEPROM.CheckAll` (it writes random data into the calibration EEPROM). No transmit, keyer, PTT or voice work until the output channels are settled and you have a dummy load.

After first light: tune the CW/PSK31/RTTY decoders on real signals, calibrate the S-meter (one offset in `SMeter.h`), FT4 SNR.
