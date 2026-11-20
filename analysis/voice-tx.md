# Voice (SSB) transmit: research and plan, 2026-11-19

Builds on the duplex FireWire stream (analysis/duplex-iso-tx.md): all 8 input channels already arrive while we transmit, and our own
transmit I/Q goes back on the same stream; the DDS is put on the dial for each over by the TxControl key hook.

## Mic input (PowerSDR 2.7.2 source)
- `FWC/fwcmixform.cs` chkMicSel: selecting the mic writes codec register **0x16** = the mic gain and sets `Audio.IN_TX_L = 7`;
  `audio.cs` IN_TX_L: 7 -> IN_TX_R = 4. So the mic arrives on **input channel 7** (the pair 7/4; a mono mic: ch7). The other inputs:
  line-in RCA 0x14 -> ch5/6, balanced TRS 0x15 -> ch6/7, FlexWire/DB9 0x13 -> ch4/5. (Our older note "mic on ch2/3" came from the
  audio.cs default in_tx_l = 2 before the mixer form runs: superseded.)
- Gain: slider -128..0, register = (0x100 + v) for v < 0, 0 for 0 dB; **0x80 = muted**. PowerSDR's FLEX-5000 default (database.cs
  Mic_Input_Level) is **0** (0 dB). Our start-up writes 0x80 to 0x13..0x16 (inputs muted), as PowerSDR's capture did.
- Front mic connector (front-panel-events.md): pin 8 mic +, 7 mic -, 6 PTT to pin 5 (chassis), pin 2 +5 V.

## PTT
- EVENT register bit 3 = mic PTT, bit 2 = rear RCA PTT; `READ_PTT` (1266, read-only) returns the same bitmap (bit0 dot, bit1 dash).
- The app also gets an on-screen PTT (hold) and a key (space bar while the window has focus); either one keys through the TxGate.

## Plan
A. **No RF, with the user**: one new reviewed write, WRITE_CODEC_REG to 0x16 only, with values 0x00 (0 dB) or 0x80..0xFF (-128..-1 dB /
   mute); then `firesidecapture direct --duplex` while speaking into the mic: ch7 should come alive. `READ_PTT` polled while pressing the
   mic PTT: bit 3. Nothing keys.
B. **Offline**: a streaming SSB chain (mic 48 kHz -> high-pass / band-pass 200-2800 Hz -> compressor + look-ahead limiter -> SSB
   (USB/LSB) -> resample to the stream rate) feeding ToneSource in a new live mode through the same mirror, image correction and HARD
   clamp. Tests: sideband suppression, two-tone IMD, peak limit, latency.
C. **RF, low power**: PTT keys the TxGate (voice: max key 180 s, forward <= 1.5 x the set peak, SWR 3); ALC: the drive is set so the
   limiter's ceiling gives the set PEAK power (peak envelope), the forward meter trims it; the DDS key hook as for FT8. Listen on the FT-817.

## Status 2026-11-19 (evening)
Done, offline / simulated (no mic yet):
- Audio stages: 10-band graphic EQ (32 Hz..16 kHz), downward expander, leveler, plus the existing gate / compressor / clipper / limiters;
  `voice_chain_defaults()` drives speech peaks into the ceiling, so the power setting is PEP.
- `TxControl::voice()`: IAudioIn -> TxChain -> SSB (USB / LSB) -> stream rate -> ToneSource live mode (mirror, image correction, hard
  clamp); keyed while PTT through the TxGate (forward 1.5 x PEP, SWR 3, time-out 180 s); drive from the band's measured level, ALC only
  down; tests on the simulated radio (sideband, PEP, PTT release, time-out, mic failure, refusals).
- Mic gain: the one reviewed codec write (fireside/codec, register 0x16); `firesidekey mic-test` (no RF) waits for a microphone.
Next: the two input sources (the PC's capture with clock-drift correction; the radio's mic = input ch7 of the duplex stream), PTT (on-screen
button / space bar / the mic's PTT via READ_PTT), a TX audio settings panel (EQ sliders, expander, leveler, compressor, mic gain), then
on the air at low power with the FT-817 listening.
