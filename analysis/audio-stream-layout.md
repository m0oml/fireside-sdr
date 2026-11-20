# FLEX-5000 I/Q and audio stream layout

Written 2026-11-14 from PowerSDR 2.7.2 (`Console/audio.cs`, `DttSP/winmain.c`, `Console/dsp.cs`, `FWC/fwcmixform.cs`),
the owner's manual and the Windows driver INF. **Nothing here has been tested on hardware.**
Labels: **[source]** stated by code, **[inferred]** reasoned from code, **[OPEN]** conflicting or unknown: settle on the radio.

## 1. Stream format
| Item | Value |
|---|---|
| Channels | **8 in, 8 out**, full duplex, one stream (`StartAudio(..., 8, ...)`, audio.cs:6563) [source] |
| Sample rates | **48, 96 or 192 kHz** (Setup > Audio > Primary; manual p.136); default 48000 [source]. The I/Q bandwidth *is* the sample rate (panadapter width) |
| Block size | default **1024** frames (`block_size1`); manual suggests 512 or less for CW, 2048 is the large setting [source] |
| Latency | automatic (0) by default; PowerSDR can add manual latency in "Expert" [source] |
| Sample type | PortAudio **non-interleaved float32**, full scale +-1.0 (the Windows ASIO driver converts the DICE 24-bit words) [source/inferred]. ALSA/snd-dice will give interleaved S32_LE (24 bit in 32): convert with /2^31 |
| Driver coupling | "The FireWire Driver will automatically follow your audio Buffer Size and Sample Rate settings" (manual p.137); PowerSDR also calls `PalManager.SetBufferSize(block_size)` when powering on. On Linux that is the ALSA period size; a rate change means closing and reopening the PCM |
| Timing | PowerSDR sleeps **100 ms** after the power-on writes, before opening audio ("hardware may change sample rate") [source] |

## 2. Inputs, PC <- radio (physical channel numbers)
| Ch | Receive | Transmit | Notes |
|---|---|---|---|
| 0 | **RX1 I** | | `in_rx1_l = 0` for the 5000 (audio.cs:6547); the FLEX-3000 is swapped (L = ch1, R = ch0) [source] |
| 1 | **RX1 Q** | | DttSP builds the complex sample as `Cmplx(left, right)`, so ch0 = real, ch1 = imaginary [source]. **Spectrum sign: settled 2026-11-17, the I/Q must be MIRRORED (see below)** |
| 2, 3 | RX2 I, Q (RX2 option only) | **microphone L, R** | The 5000 sets `in_rx2_l/r = 2,3` and the TX input defaults to `in_tx_l/r = 2,3`, and the code clears RX2 input while transmitting (`rx2_auto_mute_tx`): the pair is shared [inferred]. Mic source (front mic / line-in RCA / balanced TRS / FlexWire) is chosen in the radio's codec, see `opcode-reference.md` |
| 4-7 | unused | unused | labelled "spare"; PowerSDR never reads them |

Your radio has no RX2 (believed), so ch2/3 should be silent during receive.

**Stream-loss watchdog [source, console.cs:50160]:** if the RX1 input buffer sums to exactly 0 for more than one second
(`sample_rate / block_size` consecutive buffers) PowerSDR stops and restarts audio. Our app needs the same.

## 3. DSP processing model (DttSP)
`DttSP.ExchangeSamples2` -> `Audio_Callback2` (winmain.c:378) works on up to **three stereo pairs ("threads")**; pair *n* reads
`input[2n], input[2n+1]` and writes `output[2n], output[2n+1]`. Threads: **0 = RX1, 1 = TX, 2 = RX2** (dsp.cs: `dsp_tx[0] = new DSPTX(1)`;
thread count 2 without RX2, 3 with it). In half duplex the TX thread idles ("pass") unless MOX. Diversity reception (RX2 fitted) adds
`(A + B*scalar) * gain` of the RX1 and RX2 I/Q before the RX1 thread.

DSP logical outputs [source, audio.cs comment]: ch0/1 RX1 L/R audio, ch2/3 TX L/R, ch4/5 RX2 L/R.

## 4. Outputs, PC -> radio: **SETTLED 2026-11-19: TX I/Q = ch0 (QSE I) / ch1 (QSE Q); see transmit-stage2.md section 1** (the history below is kept)
audio.cs contains two tables, both written by FlexRadio:

| | ch0 | ch1 | ch2 | ch3 | ch4 | ch5 | ch6 | ch7 |
|---|---|---|---|---|---|---|---|---|
| "output from DSP" (logical) | RX1 L | RX1 R | TX L | TX R | RX2 L | RX2 R | - | - |
| "output DAC lineup for FLEX-5000" (physical) | **QSE I** | **QSE Q** | Headphone **R** | Headphone **L** | Ext Spkr **R** | Ext Spkr **L** | RCA Line Out + FlexWire (mono) | Int Spkr (mono) |

The callback never permutes the output pointers, so the two cannot both be right unless the radio's firmware swaps roles by
mode (e.g. receive audio on 0/1 gets routed to the headphone DAC and TX I/Q arrives on 2/3), or one table is stale. Other
comments (`out_l2 = transmit signal ... also sent to headphones in MON mode`, `out_l3 ... also ext speaker in MON mode`,
`out_l4` = scratch buffer used for the VAC feed and line-out) fit the *logical* table better. **Do not guess: settle with the experiment in section 7.**
**First measurement, 2026-11-17 (radio powered on, NOT started up, no writes; `firesideplay MyProduct`, 1 kHz at -40 dBFS, powered speakers on the external-speaker jack, headphones not plugged in):**
ch4 = external speaker **right**, ch5 = external speaker **left** (user-confirmed, one channel at a time). This matches the "physical" table. ch0..ch3 were silent (ch0/1 are the QSE on the physical table, so no jack; ch2/3 are the headphone pair and nothing was plugged in, so not yet tested). ch6/ch7 gave nothing reported; not confirmed either way. Still to do: headphones on ch2/3, ch6 (RCA line out), ch7 (internal speaker), and whether the mapping changes by mode or after start-up (codec registers).

What is certain: the receive path (inputs) does not depend on this, so first light (RX to PC speakers) is unaffected.

**Mirror settled, 2026-11-17 (live signals on ANT1, user listening):** the radio's I/Q must be **mirrored** for the sidebands to be correct (app: *Mirror I/Q* on). ch0 = I, ch1 = Q as captured; the spectrum sign is flipped relative to the DttSP convention.

## 5. Transmit [source / OPEN]
- Mic audio enters on ch2/3 (inputs) and is processed by the TX DSP thread.
- CW is generated **in software**: `CWSynth.Advance(tx_out_l, tx_out_r, ...)` writes the keyed tone into the TX output pair. Paddle events come
  from the radio (see `front-panel-events.md`); the keyer (`CWKeyer`, `FlexCW.dll`) is closed source, so we write our own.
- Whether TX I/Q goes out on ch0/1 or ch2/3 is the open question above. **Do not transmit until it is settled.**

## 6. Analogue audio inputs and outputs
Choosing mic / line-in RCA / balanced TRS / FlexWire input, headphone / powered speaker / line-out outputs, and their levels are
**radio-side codec registers** (`WRITE_CODEC_REG`), not host mixer settings: the Windows driver only mirrors them as a mixer.
Register map: see protocol/04-opcodes.md (WRITE_CODEC_REG) and `fwcmixform.cs:852-1039`. This corrects an earlier note that called the
mux "just the Windows mixer": the Windows mixer is the driver's face for the same radio commands.

## 7. Linux mapping and first-contact experiments (all read-only unless stated)
Expected: `snd-dice` presents an ALSA card with a capture PCM and playback PCM and a rawmidi port.
1. `cat /proc/asound/card*/firewire/formation` (and `.../clock`): how many PCM channels the radio offers **at each rate**. DICE devices
   often reduce channels at 192 kHz. If < 8 at 192 kHz, the I/Q-only receive path needs just 2 anyway.
2. `arecord -D hw:CARD=... -c 8 -f S32_LE -r 48000 -d 5 rx.wav` with an antenna on ANT1 after `firesidectl startup` has run (radio out of standby, RX1 tuned
   to a strong broadcast or signal-generator carrier). Check ch0/ch1 carry a quadrature pair (a single carrier shows one spectral line, not two), ch2-7 are
   ~silent. Note which side of the VFO the line appears on (settles the mirror/sign question).
3. Repeat at 96 and 192 kHz; confirm the bandwidth scales and the stream recovers after a rate change.
4. **Output lineup (settles section 4):** with RX or standby, no TX, headphone volume at minimum via codec regs, play a quiet 1 kHz tone to **one output
   channel at a time** and note which jack (headphone, ext speaker, line-out) sounds; repeat for all 8. Also measure with ch0/1 versus 2/3 whether the
   radio's TX path shows any I/Q with the transmitter unkeyed (it must stay unkeyed: no MOX).
5. Confirm the 1-second all-zero detector is a sensible trigger by pulling the antenna of nothing: a genuine dead stream gives exactly 0.0 samples.

## 96/192 kHz finding (2026-11-18)
Windows PowerSDR gets 192 kHz of band; Linux gets 48 kHz. Measured with a scratch script that wrote the DICE CLOCK_SELECT (global+0x4C) to 96k and 192k and restored 48k:
the radio LOCKS at both rates (status 0x401 / 0x601, rate reg 0x17700 / 0x2ee00) and its TX/RX stream sections still report 8 audio channels. So the hardware offers 8 ch at 192 kHz;
snd-dice reports 0 channels in the middle/high bands (`/proc/asound/card0/firewire/formation`), probably because its rate-change handshake at probe time fails
(dmesg: "snd_dice fw1.0: transaction failed: address error"). Options: a patched/out-of-tree snd-dice, or a userspace isochronous receiver over /dev/fw1 (firewire-cdev). Not built yet.
The Live dialog now offers only rates the card accepts with 8 channels (48 kHz today).

## Userspace isochronous receive spike (2026-11-18): 192 kHz WORKS, bypassing snd-dice
Script: `analysis/spikes/iso_spike_2026-11-18.py` (reference only; run with sudo because /dev/fw0, the local node, is root-only).
It allocates an iso channel+bandwidth at the IRM via /dev/fw0 (FW_CDEV_IOC_ALLOCATE_ISO_RESOURCE, freed on close), opens a single-channel
receive context (header_size 8), then writes only DICE registers on /dev/fw1: CLOCK_SELECT (global+0x4C), TX[0] iso channel (tx+8), ENABLE (global+0x50),
and restores all three in `finally` (verified after every run: clock 0x020c, enable 0, tx iso 0xffffffff). No PAL/RDAL opcodes.

| | 48 kHz | 192 kHz |
|---|---|---|
| Packets | 8000/s, no missing cycles | 8000/s, no missing cycles |
| CIP | FMT 0x10 (AM824), FDF 0x02, DBS 9 | FMT 0x10, **FDF 0x04 (96k), DBS 17** |
| Pattern | 3 of 4 packets carry 8 blocks, 1 empty | 3 of 4 carry 16 blocks, 1 empty |
| Blocks/s | 48 000 | 96 000 (dual-wire: 2 samples per block = 192 000 samples/s) |
| Labels | 0x40 audio, 0x80 MIDI slot | same |

- **Dual-wire layout at 192 kHz:** slot 2c = channel c at time t, slot 2c+1 = channel c at t+1 (slot 16 = MIDI). So I = slots 0,1; Q = slots 2,3.
  Checked: with this layout I/Q correlation is -0.003 and the strong carrier sits at -9 kHz exactly as in the 48 kHz stream; the alternative layout
  gives I/Q correlation 0.79 and a false image at +87 kHz. Audio-out slots (ch2/3) silent, ch4-7 at -88 dBFS (as at 48 kHz).
- **Receive event layout:** the iso-interrupt event's header array starts at byte 20 (closure 8, type 4, cycle 4, header_length 4); with
  header_size 8 each packet's entry is [iso header][timestamp], both big-endian as read; data_length = iso header >> 16; cycle = timestamp & 0x1FFF.
  (The first spike read from byte 24, i.e. each packet's length from the next packet's header; a fill-pattern test exposed it: corrected below.)
- **DBC:** standard IEC 61883 blocking: continuous over every packet (15 999 of 15 999 steps), an empty packet carries the next data packet's DBC.
- Radio-side: nothing beyond the clock select is needed; the radio was in standby, not started, and still produced I/Q at both rates.
- Python with per-sample parsing cannot keep up at 192 kHz (it fell to ~150 k samples/s); copying packets and parsing afterwards with numpy is fine.
  The real receiver must be C++ with a dedicated thread.

## Direct stream in the app (2026-11-18)
`app/core/src/fw_iso.cpp` (`FwIsoPcm`, an `IPcm`): the spike as C++, with the corrections above. Receive thread + 1 s ring; `PcmIqSource` on top, so
recording and DSP are unchanged. `DiceStreamControl` is the only writer (CLOCK_SELECT, TX[0] ISOCHRONOUS, ENABLE; refuses if ENABLE != 0, i.e. snd-dice is
streaming; waits for lock; restores all three and waits for re-lock; leaves the radio alone if another program took the stream over). Static guard:
`tools/tests/test_tune_guard.py::test_fw_iso_writes_only_the_three_dice_stream_registers`.
- Measured on the radio with `firesidecapture direct`: 48 kHz 144 000 frames / 3 s, 192 kHz 960 000 frames / 5 s, 0 xruns, I/Q image 42-43 dB down; ALSA works straight after;
  with ALSA streaming the direct path refuses before writing.
- GUI (`fireside --bench-direct 192000 10`): 100.0 % of real time, 0 xruns, 0 dropped spectrum frames or audio. Live... -> "FLEX-5000 direct FireWire stream".
- In direct mode the "radio jacks" audio output is disabled: it plays through snd-dice, whose duplex start would take the stream back.
- Needs `/dev/fw0` (the controller's local node) for group audio: the third rule in `system/70-flexradio.rules`.
- Found on the way: the kernel hands the config ROM over in HOST byte order; `FwDevice::rom_quadlets()` (and firesidectl) read it big-endian, so GUIDs were
  quadlet-swapped (042d1c00ffff7f00 for 001c2d04007fffff). Fixed in both.
