# Fireside SDR: short user guide

Receive only. The app never transmits and has no key, PTT or send path. Radio writes (start-up, tuning) are off until you tick **Allow writes** and confirm; that is never remembered.

## Starting
- Launcher: **Fireside SDR** (install with `system/install-launcher.sh`; its right-click menu has **Simulated radio (no hardware)**).
- Terminal: `build/gui/fireside --simulate` runs against a simulated radio; `build/gui/fireside` is the real one.
- No radio? **Open file...** plays any I/Q WAV (recordings go in `recordings/`).

## Radio screen
- **Tune:** click the spectrum, mouse wheel (Shift = 10x), Left/Right keys, PgUp/PgDn for band. Space = play/pause, M = mute.
- **Mode and filter:** USB, LSB, CW, AM, FM. Low and High cut are separate; drag the shaded edges on the spectrum, Ctrl+drag inside to slide the whole passband. Skirt: Soft / Normal / Sharp.
- **Noise reduction:** the Noise card has **NR2** for voice (SSB, AM, FM): it learns the noise and turns it down. Tested on speech in real band noise it improved the signal-to-noise ratio of the audio by about 5 dB (the older NR made it 2 to 3 dB worse on speech). It adds about 0.1 s of delay, is not used for CW, and switches itself off while PSK31, RTTY, FT8 or FT4 are decoding (they work better on the untouched audio). **NR** is the older line filter: it keeps steady tones, so use it on CW or whistles, not speech. NR2 needs the system library `libfftw3` (package `libfftw3-3`); without it the button is missing.
- **Notches:** Alt+click the spectrum to add one (up to 4), Alt+click one to remove, drag to move. Works in SSB and CW.
- **S-meter:** S1 to S9+60 with a dBm number. Marked *uncal*: the scale is a placeholder until the radio is calibrated.
- **Band plan** (Waterfall card): coloured strip on the spectrum (green CW, violet digital, blue phone, amber beacons) and FT8/FT4 calling frequencies. IARU Region 1, a guide only.
- **Decoder:** CW, PSK31, RTTY, FT8, FT4. For FT8/FT4, double-click a line to fill the Lookup box. Lines are tagged **NEW**, **new band/mode** or **worked** from your Wavelog. A line calling you is highlighted red and the caller is looked up by itself (the first caller; a call you typed is left alone). With your QRZ.com login on the Log tab (QRZ.com callbook card), each station's name, town, country and distance appear after its line.
- **Memories:** save and recall frequency, mode and filter by name.
- **Band memory:** each band button (and PgUp/PgDn) takes you back to where you left that band: frequency, mode, filter and decoder (FT8 on 20 m stays FT8 on 20 m). A band you have not used yet starts at its usual frequency, or at its FT8/FT4 frequency if that decoder is on. Remembered between runs. The antenna per band is set on the Antennas tab.
- **CAT for other programs** (Device tab, on by default): WSJT-X, JTDX, fldigi, GridTracker, WaveLogGate and other Hamlib programs can follow and set the frequency and mode. In the other program choose the rig **Hamlib NET rigctl**, address `127.0.0.1:4532`, PTT **None** or **VOX**. Only this computer can connect. It cannot transmit: a transmit request from CAT is refused and counted on the card.

## Logging (Wavelog)
- Set up once on the **Log** tab: server URL, API v2 token, **Test connection**. The token is kept in `~/.config/fireside/wavelog.env` (mode 600), never in the app's settings.
- **Lookup box:** type a callsign, Enter = who is it and have I worked them. Enter again (or **Log**) opens the log form; Enter in a field logs; Esc cancels.
- Every QSO is saved locally first (`~/.local/share/fireside/log.adi`). If Wavelog is unreachable it is kept in `unsent.adi`; press **Send unsent** later.
- **Last contacts** shows the newest 12 from Wavelog (or from this PC when offline).

## Not done yet (needs the radio)
S-meter calibration, tuning the CW/PSK31/RTTY decoders on real signals, the radio output test, and anything that transmits.
