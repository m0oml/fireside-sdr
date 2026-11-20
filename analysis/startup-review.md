# Review: start-up / shut-down write sequence (Opus, 2026-11-14)

**Verdict: approved for a first, receive-only power-on, subject to conditions A-C below, which are now enforced in code and tested.**
Scope: `tools/firesidectl/startup.py` (the only code that writes to the radio), checked line by line against PowerSDR 2.7.2
(`Console/console.cs`, `FWC/fwc.cs`). Ported to C++ afterwards (`app/core/src/startup.cpp`, 2026-11-14): see the end of this file.

## The sequence, checked against PowerSDR
| Step | Our write | PowerSDR evidence | Finding |
|---|---|---|---|
| Identify | reads only | `FWCEEPROM.Init`, `InitRadio` | OK. Firmware must be 2.1.4.8-2.1.255.255; model 3 (FLEX-3000) refused. A successful identify over a link also proves its byte order |
| Init | `INITIALIZE(0,0)` | `InitRadio()` calls `FWC.Initialize()` on **every launch** after the firmware check (console.cs:12098) | Accept. Effect not visible in source, but every FLEX-5000 owner has sent it on every PowerSDR start |
| Preamp | `SET_TRX_PREAMP(0)` | `InitRadio()`: `SetTRXPreamp(false)` | OK. Only lowers sensitivity |
| Power on | `SET_STANDBY(0)` | `chkPower` on -> `SetStandby(false)`; off -> `SetStandby(true)` (console.cs:52752, 53126) | Polarity confirmed |
| Antenna | `SET_RX1_ANT(1)` | Set from the database during `InitConsole` and re-sent only on change (`RX1Ant` setter) | Equivalent; 1 = ANT1 |
| Tune | `SET_RX1_FREQ_TW(tw, f)` | `UpdateRX1DDSFreq` -> `SetRX1FreqTW(tw, freq)`, `tw = (uint)(0xFFFFFFFF*f/500.0)` | Formula matches. **Design note** below |
| Shutdown | `SET_MOX(0)`, `SET_STANDBY(1)`, **`SET_PA_BIAS(0)` (added)** | `SetMOX` uses this mailbox opcode (its MIDI variant is commented out). `Console_Closing` always does `SetPABias(false)` | PA-bias-off was missing; added |

Nothing else PowerSDR does at start-up is needed for receive. PA bias is never switched **on** in normal operation (the `FWCPABias`
property is never assigned; the firmware handles bias with T/R). Gain pots/RDAC writes happen only in calibration and TX band changes.
No band-filter command is ever sent (the firmware chooses filters from the float frequency).

## Problems found in our implementation (all fixed, with tests)
**A. Exemptions were opcode-wide, not value-specific.** `run_steps` permitted `SET_MOX` past the dangerous-write gate for *any* value,
so a future caller could have keyed the transmitter through the "reviewed" path. Now `STEP_PERMITS` are predicates:
`INITIALIZE(0,0)`, `SET_MOX(0)`, `SET_PA_BIAS(0)` only; anything else raises `SafetyError` before sending.
(`test_startup.WithMailbox.test_exemptions_are_value_specific`)

**B. Writes could go over MIDI SysEx.** PowerSDR 2.x sends all of these over the FireWire mailbox. Over MIDI it only ever sent
`GET_MODEL` (to detect v1.x firmware) and a MOX note that is commented out. Driving `INITIALIZE`/`STANDBY` through the firmware's SysEx
handler would be an untested path. Writes now require the mailbox, and MIDI is refused **before any traffic**.
(`test_writes_over_midi_are_refused`)

**C. Byte order was a manual flag.** `firesidectl --fw` now defaults to `--byte-order auto`, using the read-only `GET_FIRMWARE_REV` probe.
Additionally, for **all 263 opcodes** the byte-swapped command word decodes to a non-existent opcode (tested), so even a wrong order
would make every write inert rather than wrong. (`test_hardware_day.Safety`)

## Design note (not safety): IF offset
PowerSDR does **not** put the DDS on the dial frequency by default: `if_shift = true`, `if_freq = 0.009` MHz (DDS = dial - 9 kHz) and
`spur_reduction = true` (tuning word snapped to a 16-bit grid, remainder done by the DSP oscillator). Our start-up tunes the DDS exactly to
the requested frequency. That is fine for first light (a known carrier lands at a predictable offset), but the receive chain should adopt
the 9 kHz IF so wanted signals stay away from DC (QSD DC offset, LO leakage, 1/f noise).

## Residual risks (accepted)
- The firmware's exact `INITIALIZE` behaviour (mitigated: PowerSDR always sends it).
- Firmware handling of unknown opcodes (only reachable by the byte-order probe; worst case the mailbox wedges -> power-cycle, nothing else written).
- Whether the mailbox works without a DICE owner (i.e. without snd-dice bound). `hardware-day` now reads the DICE owner and nickname registers (read-only)
  to show this; the nickname (expected "FLEX-5000") also independently confirms the register layout.

## First power-on procedure
1. Radio switched on at least 15 s before (PowerSDR warns that early start-up returns firmware 0). FireWire card fitted; `system/fireside-system-check.sh` clean.
2. Antenna or 50-ohm dummy load on ANT1. Nothing on the TX1-3 keying lines or amplifier outputs. No microphone PTT pressed.
3. `tools/hardware-day --events 20` passes: identify on both routes, EEPROM backup **verified**, byte order found, DICE nickname readable.
4. `tools/firesidectl-cli --fw /dev/fwN startup --freq 7.1 --dry-run`, then the same with `--allow-write`.
5. Only then the ALSA capture tests (audio-stream-layout.md section 7). Finish with `firesidectl-cli --fw /dev/fwN --allow-write shutdown`.
Stop on: any identify warning, EEPROM checksum problems, the two routes disagreeing, firmware out of range, or anything unexpected (relays chattering, fan
running flat out, a TX indicator).

## C++ port (Sonnet, 2026-11-14) - awaiting Opus diff-check
`app/core/src/startup.cpp` mirrors `startup.py`; tests in `app/tests/test_startup.cpp` mirror `test_startup.py`. A-C hold: value-specific predicates, `require_firewire`
(dynamic_cast to `PalMailbox`, refused before any traffic, tested against the pty MIDI radio), shutdown ends with `SET_PA_BIAS(0)`.
Deliberate differences from Python, for the diff-check: (1) `run_steps` validates every exemption value up front, so a bad step later in a list sends nothing
(Python raised mid-list, after earlier steps were sent); (2) the permit is an RAII scope, released even if the write throws (tested);
(3) the exemption is permitted by full opcode name via `find_opcode`. Nothing else was changed in safety.cpp.
Check specifically: `Flex::link()` is a new accessor; `PalMailbox` is the only ILink the cast accepts; no CLI/GUI calls the startup yet.

## Opus diff-check of the C++ port (2026-11-14) - findings, NOT yet fixed (usage limit hit)
Checked `app/core/src/startup.cpp` line by line against `tools/firesidectl/startup.py`, `client.py`, `protocol.cpp` (packing) and `pal.cpp` (wire path).
**Verdict: sequence, order, values, tuning word, antenna map, firmware/model gates and MIDI refusal match Python exactly. Conditions A-C hold
for single-threaded use. Safe to keep as is for first power-on via the Python tool; fix F1 before anything multi-threaded (CAT/GUI) can write.**
The three deliberate differences (validate-all-first, RAII permit, full-name permit) are approved: all are strictly safer.

- **F1 (fix before wiring CAT/GUI writes): permits are opcode-wide inside `Safety`, values are only checked in `run_steps`.** During the permit window
  another thread using the same `Flex` could send `SET_MOX(1)` past the gate: finding A re-introduced via concurrency. Python has the same shape
  (`Flex.permit`). Fix in both: move the value predicates into the safety layer (`Safety::check_write(op, d1_raw, d2_raw)`, called from `Flex::set` with the
  packed words): a permitted INITIALIZE/SET_MOX/SET_PA_BIAS passes only with the reviewed values, whoever calls. Add a test with a second thread.
- **F2 (tighten): `SET_MOX(0,*)` and `SET_PA_BIAS(0,*)` accept any d2.** PowerSDR always sends d2 = 0. Require d2 == 0 too (C++ and Python, plus tests).
- **F3 (improve): nothing confirms the LAST write was consumed.** `PalMailbox` only waits for idle before the next command, so after shutdown's
  `SET_PA_BIAS(0)` the program can exit without knowing the firmware ran it. Add a read-only `sync()` (poll CMD until bit 31 clears, with timeout) at the end
  of `run_steps`, and report a stuck mailbox instead of exiting silently. Mirror in Python.
- **F4 (note for wiring): byte order must be established by `probe_order` first.** If a caller builds a `PalMailbox` with the wrong order, `identify()`'s
  boot-wait loop resends the swapped (inert, 0x30040080) word up to ~20 times. Inert per finding C, but PowerSDR never does it. When a C++ CLI/GUI calls
  `run_startup`, require the order to come from `probe_order` (e.g. a `verified` flag on `PalMailbox`).
- Minor: `PermitScope` unpermits on exit even if the caller had permitted that opcode beforehand (Python restores the previous set). Stricter, so harmless; fixed for free by F1.

## Held-back changes (Opus assessment)
- **9 kHz IF offset at start-up: approve in principle.** Only the `SET_RX1_FREQ_TW` value changes (dial - 9 kHz, same opcode, same formula). Do it as an explicit
  `if_offset_hz` argument to `startup_steps` (default 0 until first light has confirmed the plain path), and pass the same value into `ReceiveConfig`, so DDS and DSP can never disagree.
  Do it after hardware day step 4, not before: first light should change one thing at a time.
- **CAT -> radio retune and click-to-tune: approve with conditions.** Both reduce to `SET_RX1_FREQ_TW` only. Conditions: (1) after F1; (2) a single tuning function in core
  (`tune_rx1(fx, dial_hz, if_offset)`) that range-checks 0 < f <= 65 MHz and is the only caller; (3) rate-limit (e.g. <= 20 writes/s, coalescing) so a spinning knob or CAT poll
  storm cannot flood the mailbox; (4) never any other opcode from CAT; TX stays refused until the output-channel experiment is done.

### Applied 2026-11-14 (Opus): F1, F2, F3 in Python and C++
- **F1:** the reviewed values now live in the safety gate (`client.REVIEWED_VALUES` / `Safety::reviewed_value`) and are checked on the packed wire words in every `set()`.
  A permit for INITIALIZE/SET_MOX/SET_PA_BIAS passes only (0, 0); a value-less check never passes a permit. Other values fall through to the normal
  dangerous check, so `allow_dangerous` stays the only (explicit, operator) override. C++ `Safety` is now mutex-protected. Tests: same-caller and
  other-thread attempts inside an open permit are refused (`test_permit_covers_only_the_reviewed_value_even_inside_the_window`, both languages).
  ThreadSanitizer run: no races in `core/` (the only reports are in the test fixture `tests/fake_radio.h`, pre-existing, harmless).
- **F2:** d2 must be 0 as well (`test_second_argument_must_be_zero_too`). `run_steps` validates with the same predicate as the gate (one source of truth).
- **F3:** `run_steps` ends with a read-only `sync()` (poll CMD until bit 31 clears); a firmware that never takes the last write raises TimeoutError
  instead of the program exiting silently (`test_last_write_is_confirmed_consumed`, `test_mailbox_that_never_takes_the_last_write_is_reported`). MIDI links: no-op.
- **F4:** still a requirement for whoever wires a C++ CLI/GUI to `run_startup` (establish byte order with `probe_order` first). Nothing calls it today.

### F4 applied + probe fix (Sonnet, 2026-11-14), for the next Opus look
- **F4:** `PalMailbox::verify()` / `.verified` (C++ and Python): reads GET_FIRMWARE_REV over the mailbox and accepts the byte order only if it is in range. `require_firewire` (start-up/shutdown) and `Tuner` refuse an
  unverified mailbox before any traffic; the CLIs call verify() for every FireWire command, including `--byte-order big|little`.
- **Found while testing:** `probe_order` could try the second byte order after the first attempt timed out BEFORE sending anything (mailbox already busy). A busy mailbox can read as idle in the other byte order, so
  the probe could have written into a wedged mailbox. Now such a first attempt aborts the whole probe ("busy before any command was sent; power-cycle"); the second order is only tried after a command was actually sent.
  Tests in both languages (`test_busy_mailbox_aborts_the_probe...`, `pal_probe_busy_mailbox_aborts...`, `cli_unusable_radio_is_reported_not_written`).
- **For the reviewer:** `core/src/cli.cpp` is the only new orchestration of writes: check the order (validate args -> allow-write -> open bus -> local-controller refusal -> probe/verify -> identify -> run_startup -> sleep -> tuner/CAT -> shutdown),
  that serve never shuts down after a failed start-up, and that a failed shutdown is reported (exit 4) rather than swallowed.

### GUI radio session + click-to-tune (Sonnet, 2026-11-14), for the Opus diff-check
New write-capable surface, all via `RadioSession` (core/src/session.cpp) and `Tuner`: `gui/RadioPanel.cpp`. Things to check:
1. **Defaults:** "Allow writes" unticked at start (guard test), Start refuses without it, and asks for confirmation (`QMessageBox::question`; `startRadio(true)` skips it for tests only).
2. **GUI never writes directly:** `tools/tests/test_tune_guard.py` fails if gui/ uses run_startup/run_steps/PalMailbox/Tuner/`.set(`/permit (except the pre-existing read-only MIDI identify `Flex`).
3. **Shutdown on exit:** `MainWindow::closeEvent` stops a started radio; a failed shutdown shows a warning. Check that closing during a retune cannot race the shutdown (Tuner is disarmed first inside `RadioSession::shutdown`, all under the session mutex).
4. **Threading:** connect/start/shutdown run on the GUI thread (they can block up to a few seconds). `tick()` pumps the tuner every 20 ms on the GUI thread; `dialChanged` calls `session.tune` from the GUI thread. No other thread touches the session.
5. **IF offset lock:** while started the DSP is forced to IF offset 0 (the reviewed start-up tunes the DDS exactly to the dial).
6. **CLI refactor:** `cli.cpp` now calls `RadioSession` instead of its own copy of the orchestration (same messages and exit codes; all 13 CLI tests unchanged and green).

## Opus diff-check of all write-related code (2026-11-14) - DONE
Read in full: `core/src/session.cpp`, `cli.cpp`, `tune.cpp` (+ `tune.h`), `pal.cpp` (`request`, `sync`, `verify`, `probe_order`), `gui/RadioPanel.cpp`, `MainWindow::closeEvent`,
`ReceiverPanel` (dial, IF lock, `adopt`), plus the static guards. **Verdict: approved for hardware day, with the fixes below applied.**

**Confirmed sound (no change):**
- Exactly one orchestration of writes (`RadioSession`); CLI and GUI both use it; GUI has no other write path (guard test). Writes off by default, Start confirms, the transmitter is unreachable.
- `Tuner`: one opcode, range check on dial AND DDS, rate limit with latest-wins coalescing, armed only after a completed start-up, disarmed on any fault, `last_sent_` updated only after a successful write.
  `RadioSession::shutdown` disarms the Tuner (under the Tuner's own lock) before the shutdown writes, so a retune can never land between or after them. In `serve` the CAT server is stopped before shutdown.
- Byte order: probe (with the busy-abort fix) then `verify()` over the very mailbox that will carry writes; `require_firewire` and the Tuner refuse unverified mailboxes.
- Value-restricted permits live inside `Safety` and are checked on the packed words (F1/F2); last write confirmed by `sync()` (F3).
- IF offset: locked to 0 while started, matching the reviewed start-up which tunes the DDS exactly to the dial.

**Found and fixed (tests added in both languages where applicable):**
- **G10 `serve` could exit without shutting down** after a successful start if anything later threw (e.g. CAT port in use). Now every exit path after start runs the shutdown (`cli_serve_shuts_down_even_if_cat_cannot_start`).
- **G12 `verify()` retried on any implausible answer** for the whole boot wait, resending the (inert) probe ~20 times with a wrong explicit byte order. Now it retries only on firmware 0 (booting) or a timeout; a definite wrong answer fails at once (C++ + Python tests).
- **G5 `RadioSession::tune/pump` used a raw Tuner pointer outside the lock.** Now `shared_ptr`, and `disconnect()` disarms first, so a Tuner held elsewhere can never write afterwards (`session_disconnect_disarms_a_tuner_handed_out_earlier`).
- **G11 a start that fails part-way** may leave the radio out of standby: the GUI now says to press Stop, and closing the window runs the shutdown in that case too (`--smoke-radio`: mailbox wedges after INITIALIZE).
- **G1 `adopt()` could move the locked IF offset** (e.g. pressing Demo while started). It now respects the lock (`--smoke-radio`).

**Accepted residual risks (documented, not code):**
- GUI connect/start/stop run on the GUI thread (up to ~3 s boot wait + mailbox timeouts): the window freezes briefly; harmless.
- A process killed (SIGKILL, crash, power cut) while started leaves the radio in receive, out of standby. Never transmitting, PA bias never switched on by us: safe, just not tidy.
- Click-to-tune uses offsets from whatever spectrum is displayed (could be a recording): range-checked and RX-only, so at worst a surprising retune.
- After a tuning fault there is deliberately no re-arm button: Stop (or power-cycle) is the recovery.
