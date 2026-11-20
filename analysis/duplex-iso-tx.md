# Direct isochronous TRANSMIT (duplex FireWire stream): research, 2026-11-19

Goal: send the transmit I/Q to the radio over our own FireWire stream while the receive stream keeps running, so receive never
pauses for an over (today TX goes through snd-dice ALSA playback at 96 kHz and the direct receive stream must close for it).

Sources read (GPL; read for protocol facts only, nothing copied): Linux `sound/firewire/dice/dice-stream.c`, `dice-interface.h`,
`sound/firewire/amdtp-stream.c`, `amdtp-am824.c` (fetched into `upstream/linux-firewire-sound/`), `/usr/include/linux/firewire-cdev.h`.

## The radio (read-only register dump, 2026-11-19, `dice_read.py`: reads only)
- DICE sections: global @0x28, TX @0x1a4 (1 stream, 280 bytes), **RX @0x3dc (1 stream, 280 bytes)**.
- RX stream 0 (host -> radio): ISOCHRONOUS 0xffffffff (unused), SEQ_START 0, **NUMBER_AUDIO 8, NUMBER_MIDI 1**: the same layout as the
  radio's own stream to us (8 audio + 1 MIDI quadlet = DBS 9; 192 kHz dual-wire: 16 + 1 = DBS 17).
- CLOCK_SELECT 0x40c (96 kHz, internal), CLOCK_CAPS 0x10000054.

## How snd-dice runs both directions (dice-stream.c)
1. Allocate an iso channel + bandwidth for each direction.
2. Write TX[0].ISOCHRONOUS (radio -> host channel; also TX_SPEED) and **RX[0].ISOCHRONOUS** (host -> radio channel).
3. Write GLOBAL ENABLE = 1: "the device immediately starts packet transmission when enabled".
4. Start the host's streams. Both directions use **blocking mode** (`CIP_BLOCKING`).
5. Stop: ENABLE 0, both ISOCHRONOUS back to 0xffffffff.

## Outgoing packets (amdtp-stream.c, amdtp-am824.c)
- One packet per bus cycle. Blocking: a data packet carries exactly SYT_INTERVAL data blocks (8 at 48 kHz, 16 at 96 kHz;
  192 kHz dual-wire is 96 kHz on the wire: 16 blocks of 2 frames); the other cycles carry an empty CIP-only packet (NO-DATA:
  FDF 0xFF, SYT 0xFFFF), the data block counter unchanged.
- CIP q0 = SID (local node id & 0x3f) << 24 | DBS << 16 | DBC; q1 = EOH | FMT 0x10 << 24 | FDF << 16 | SYT. FDF = AM824 SFC
  (48 kHz 0x02, 96 kHz 0x04, dual-wire 192 kHz 0x04). DBC = blocks sent so far (mod 256), the first block of this packet.
- Audio quadlet = 0x40 label | 24-bit sample (S32 >> 8). MIDI quadlet without data = 0x80 000000. Dual-wire: channel c's two
  samples at slots 2c (time t) and 2c+1 (t+1), as in the receive direction (our decoder already reads it that way).
- **Media clock: "sequence replay"** (amdtp_domain_start(..., replay_seq = true)): the outgoing stream copies the incoming one.
  For each packet received at cycle r with N data blocks and SYT s, a packet goes out at cycle r + L with N data blocks and
  SYT s shifted by L cycles (the kernel subtracts its transfer delay on the way in and adds it back on the way out, which cancels).
  So the radio receives exactly the sample rate it sends, with presentation times in its own clock.

## firewire-cdev transmit
- Context type TRANSMIT on the local node, its own channel allocation (bandwidth for the max payload).
- Each queued packet: control = PAYLOAD_LENGTH | HEADER_LENGTH(8) | TAG(1) | (INTERRUPT), the 8 CIP bytes in the packet
  descriptor's header (copied at queue time), the payload in the mmap'ed buffer. SKIP = no packet that cycle.
- START_ISO cycle >= 0 starts on that cycle (15 bits: 2 of seconds, 13 of cycle). Completion events carry per-packet timestamps.

## Plan (our code, no GPL text)
1. `AmdtpEncoder` (pure): CIP header + AM824 payload from S32 frames, DBC, dual-wire, NO-DATA packets.
2. Replay scheduler (pure): incoming (cycle, blocks, SYT) -> outgoing (cycle + L, blocks, SYT + L); gaps filled with NO-DATA;
   falls behind -> drops and counts.
3. `CdevIsoTransmitter` (firewire-cdev), behind an `IIsoTransmitter` interface (fakes for tests).
4. `FwIsoPcm` duplex: the worker that decodes each received packet also queues the outgoing one; the samples come from a ring
   filled by an `IPcmOut` (`open_fw_iso_out()`), zeros when nothing is written. DiceStreamControl also sets RX[0].ISOCHRONOUS
   before ENABLE and restores it (the static guard in tools/tests/test_tune_guard.py is updated for that one extra register).
5. Transmit: when the direct stream runs, TxControl's playback is `open_fw_iso_out()`: receive is not paused, the rate is the
   stream's rate. ToneSource stays the only producer of ch0/1 samples (hard clamp, mirror, image correction unchanged); MOX only
   through the TxGate.
6. On the radio: (a) duplex with silence, MOX off: receive must be unaffected; (b) test tone at 2 W, listening; (c) FT8.

## On the radio, 2026-11-19
- Duplex with silence, 192 kHz, 20 m, receive-only start-up: receive identical to receive-only (80032 packets / 10 s, 0 dbc breaks,
  0 lost cycles, 0 rejected); 80032 packets / 1 920 672 frames sent, none refused, gap-filled or dropped; everything restored after.
- EXT_STATUS shows no ARX1 lock, for snd-dice's own playback too: not an indication for a playback stream.
- **The radio plays our stream**: `firesidecapture direct --duplex --beep 4`, 1 kHz at -40 dBFS on ch4/5 only (ch0/1 zero, no RF): heard by
  the user on the powered speakers at the external-speaker jack.
- Next: the 2 W test tone through the TxGate with the direct stream running (receive must keep going through the over).
- **First transmission over the duplex stream (2026-11-19, 14.105 MHz, the app's Test tone 2 W / 8 s, 96 kHz direct):** 2.1 W forward
  (ref 0.25 W, SWR 2.0); the receive stream kept running through the over (spectrum and waterfall live; the S-meter drops to S0 as the
  radio mutes its receiver while keyed), no receive pause / resume at all. Screenshots once a second.
- **Frequency (2026-11-19):** with receive running, the over went out at the receive IF shift (dial 14.101 -> 14.092 on a WebSDR): the
  FLEX-5000's RX1 and transmitter share one DDS. As PowerSDR (if_shift = false before MOX), a TxControl key hook now puts the DDS on the
  dial right before key-down and 9 kHz below after unkey (Tuner::set_if_offset); the user's FT-817 then heard the over on the dial (40 m).
  The receiver follows (IF offset 0 while keyed), so its spectrum is right during the over too.
- Seen: SWR reads ~2.0 in every over today on both bands while the ATU's own reports say 1.0-1.24 (our reflected reading, to look at);
  a 15 W test tone peaked at 20.7 W (20 m) / 18.2 W (40 m) on the ramp's way up (under its 1.5 x trip).

## Stream left enabled (2026-11-19 16:06): cause and fix
- Seen: ENABLE 1, TX[0] iso 1, RX[0] iso 2 with a NEW fireside instance (started 16:05:29) whose start was refused ("already streaming").
  The earlier instance (or a second copy started meanwhile) went without closing the stream; a process that dies (killed, crashed) cannot
  restore the registers, and the radio keeps streaming. app.log has shown every close since then restoring cleanly (5 of 5).
- Fixes: (1) DiceStreamControl::start clears a leftover only when the radio sends on exactly the channel our receiver now holds (and RX[0] is
  idle or on our transmit channel): the kernel frees a dead run's channels, so nobody else can own that stream; anything else is refused before
  any write, as before. Cleared -> ENABLE 0, ISOCHRONOUS none, then the usual start; stop() restores to idle. (2) stopRadio closes the live
  stream even if the shutdown sequence fails. (3) SIGINT / SIGTERM / SIGHUP close the window cleanly (exit status 0), as Super+Shift+Q does.
