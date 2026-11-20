# 7. Front-panel and key events

The FLEX-5000 has no knobs or menus: its front panel is jacks. The radio reports contact closures to the host; everything else (what a paddle press means, what PTT does) is host software.
**Status: Reported.** Fireside SDR reads the PTT state (`READ_PTT`) and the EVENT register as listed here, but we have not tested every input on the real radio.

## What can occur on a FLEX-5000
| Input | Where | Event |
|---|---|---|
| Straight key or paddles | 1/4" TRS KEY jack | dot (bit `0x01`), dash (bit `0x02`) |
| Microphone PTT | 8-pin mic connector: pin 6 shorted to **pin 5 (chassis ground)**, not to pin 7 | bit `0x08` |
| Rear PTT | rear RCA PTT jack | bit `0x04` |
| Mic UP / DOWN / FAST | **FLEX-3000 only**; their pins are not connected on a 5000 | bits `0x10`, `0x20`, `0x40`: never expected |

The FlexWire bus on the rear DB9 and the separate USB FlexControl knob are not covered here.

## Delivery
- **EVENT register** `0xFFFF_E000_1008` ([page 2](02-control-transports.md)): a **level** bitmap. A change notification does not say which bit changed: compare with the previous value and do your own edge detection.
- **`READ_PTT`** (1266): returns the same bits 0 to 3 as a read command, so it can be polled without the register or notifications. About every 10 ms is enough for PTT; use a faster loop for paddles.
- **MIDI** ([page 2](02-control-transports.md)): note on/off messages, velocity ignored: note 61 = dot, 63 = dash (66, 68, 70 = mic down, up, fast on the 3000).

Do both transports fire for the same contact? Does the radio debounce contacts or must you? What is the paddle latency over each path? **Unknown**: not measured.

## What the host does with them
- **PTT (mic or RCA)** keys the transmitter in voice modes while held. Route it through the same arming and safety checks as any other key request ([page 6](06-transmit-and-atu.md)): it is a request to key, not a command to.
- **Paddles** need a **keyer in host software**: the radio only reports the contacts. An iambic A/B keyer is a standard algorithm (dot/dash memory, weight, WPM). Key-down must synthesise the keyed tone into the transmit I/Q ([page 3](03-streams.md)); the radio also takes the CW pitch (`SET_CW_PITCH`) and a break-in flag
  (`SET_BREAK_IN`), but what the firmware does with them is not documented.
- The radio is told the transmit mode (`SET_TX_DSP_MODE`) so it applies the right frequency limits in CW modes.
