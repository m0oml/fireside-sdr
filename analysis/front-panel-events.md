# FLEX-5000 front panel, mic, paddle and PTT events

Written 2026-11-14 from PowerSDR 2.7.2 (`FWC/fwc.cs Notify`, `FWC/fwc_midi.cs`, `console.cs`, `cw/CWInput.cs`), the owner's manual and `FlexCW.dll` strings.
The radio itself has **no tuning knob or menu**: everything on the front panel is jacks. The events below arrive from the radio.

**FLEX-5000 vs FLEX-3000:** PowerSDR's event decoder is shared by both radios. The mic UP / DOWN / FAST bits exist for the **FLEX-3000**. The FLEX-5000's 8-pin mic
connector (manual Table 2) is: 1 N/C, 2 +5 V (65 mA max), 3 N/C, 4 N/C, 5 chassis ground, 6 PTT, 7 mic -, 8 mic +. Pins 1, 3 and 4 (where Yaesu-style mics carry UP/DWN/FAST)
are **not connected**, so on a 5000A only **dot, dash, RCA PTT and mic PTT** can occur.

## 1. Where events come from
| Source | How it reaches the PC |
|---|---|
| Straight key or paddles (1/4" TRS KEY jack) | EVENT bits 0 and 1 (dot, dash); also MIDI notes 61 and 63 |
| PTT on the microphone connector (pin 6 grounded to **pin 5**, chassis ground; *not* to pin 7, mic ground) | EVENT bit 3; read it with READ_PTT (1266) |
| PTT on the rear RCA PTT jack | EVENT bit 2 |
| Mic **UP / DOWN / FAST** buttons | **FLEX-3000 only**: EVENT bits 4, 5, 6; MIDI notes 68, 66, 70. Not wired on the 5000 |
| FlexWire peripheral bus (rear DB9: also carries mic-level "FlexWire In" audio on pin 2 and AF out on pin 9) | FlexWire opcodes (1304/1305/1342/1343): not analysed yet |
| FlexControl knob | **Not via the radio**: separate USB device (`FlexControl.dll`); out of scope unless we want to support it |

Two equivalent delivery paths exist (both implemented in firesidectl):
- **FireWire mailbox:** 32-bit EVENT register at `0xFFFF_E000_1008`; DICE notification bit `0x02000000` says it changed. (`firesidectl monitor --fw`)
- **MIDI:** radio -> PC channel messages. Note on/off, velocity ignored. (`firesidectl monitor`)

## 2. EVENT bitmap (PowerSDR `FWC.Notify`, also returned by READ_PTT bits 0-3)
| Bit | Meaning | PowerSDR action |
|---|---|---|
| 0x01 | CW dot paddle (level) | edge-detected: enqueues a "sensor" item (dot, pressed/released); swapped with dash if "reverse paddles" |
| 0x02 | CW dash paddle (level) | same; ignored if `ignore_dash` (straight-key use) |
| 0x04 | rear RCA PTT (level) | sets `FWCRCAPTT` (PTT) |
| 0x08 | mic PTT (level) | sets `FWCMicPTT` (PTT) |
| 0x10 | mic DOWN (level), FLEX-3000 | `MicDown` |
| 0x20 | mic UP (level), FLEX-3000 | `MicUp` |
| 0x40 | mic FAST, FLEX-3000 | **toggles** fast mode on each event (edge) |

The value is a level bitmap with edge handling done in software (`last_bmp`). A notification does not say which bit changed: compare with the previous value.

## 3. MIDI messages (fwc_midi.cs)
Radio -> PC: `Note On/Off` (0x90/0x80), note 61 = dot, 63 = dash, 66 = mic down, 68 = mic up, 70 = mic fast; controllers 64 and 69 (hold pedals) are defined but unused.
PC -> radio short messages: `Note On/Off note 0 velocity 127` = **MOX** key/unkey; `Note On note 1 velocity N` = read PA ADC channel N (the reply is the SysEx result).

## 4. What the inputs do
- **Mic UP / DOWN (FLEX-3000 hardware; nothing to do on a 5000)**: a press moves VFO A by **+-50 Hz** (**+-1 kHz** when FAST is on, `MicUp`/`MicDown` setters). The hold-to-repeat
  logic (**>600 ms** hold, then **+-100 Hz** per **50 ms**, **500 Hz** in fast mode) runs in a thread named "F3K Mic Feature Thread" that PowerSDR starts only for the FLEX-3000 (console.cs:52931).
- **FAST** (3000): toggles the step size.
- **PTT (mic or RCA)**: keys the transmitter (MOX) while held, in voice modes.
- **Paddles**: see below. A `CW_POLL()` helper also reads dot/dash/PTT through READ_PTT without notifications, used by the CWX panel.
All of these are *software behaviour*; the radio only reports contacts.

## 5. CW keying
- The **keyer is PC software** (`CWKeyer`, in the closed-source .NET assembly `FlexCW.dll`; only `cw/CWInput.cs` is open). We must write our own
  (iambic A/B is a public algorithm: dot/dash memory, weight, WPM).
- Its settings, from the DLL's properties: `WPM`, `Weight`, `Iambic`, `CurrentIambicMode` (A/B), `AutoCharSpace`, `BreakIn`, `BreakInDelay`,
  `HWKeyDownDelay`, `MinQSKTime`, `Pitch`, `RampTime`, `AudioLatency`, `SampleRate`, `MOX`. These make a good config list for our keyer.
- Key-down makes the **software** synthesise the keyed tone into the TX output stream (`CWSynth.Advance` writes `tx_out`, audio.cs) [source]. PowerSDR also sends the CW pitch (SET_CW_PITCH) and a break-in flag
  (SET_BREAK_IN) to the radio; what the firmware does with them is [inferred], not documented in the source. Paddles can instead come from a serial port's DTR/RTS lines or CAT (`CWInput`: "Radio", "CAT", "COMn", "None").
- A key-up/transmit frequency limit applies to CW modes: PowerSDR tells the radio the TX mode (SET_TX_DSP_MODE) "so the radio believes we are in CW for TX freq limits".

## 6. Poll vs notify (design choice for our app)
Prefer notifications (instant paddles matter for CW timing); fall back to polling READ_PTT every ~10 ms. Over MIDI the notes arrive in order with the SysEx replies
(our parser already separates them). Over FireWire, use the snd-dice hwdep notification stream if available; else poll the EVENT register.

## 7. Unknowns for hardware day
1. Do MIDI notes and the EVENT register both fire, or only one? Do the MIDI note velocities carry anything?
2. Debounce: does the radio debounce the paddle / PTT contacts or must we? Do bits 4-6 ever fire on a 5000 (expected: no)?
3. Paddle latency over each transport (measure with a scope or by logging timestamps).
