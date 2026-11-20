# PA SWR reading vs the ATU's own meter (2026-11-19)

Question: our PA meter showed SWR ~2-3 on every over (15 W FT8 overs on 40 m tripped the gate's SWR 3.0 straight after a passing tune) while the
ATU reported 1.0-1.3 after its tunes.

## Code check
- `core/src/tx_meter.cpp` matches FlexRadio's original PowerSDR FWCSWR: forward and reflected from READ_PA_ADC 7 / 6 through the PA bridge
  table (EEPROM 0x2E0), the per-band "PA SWR" factor (EEPROM 0x630) on the reflected reading only. This radio: 40 m 1.3549, 20 m 0.9834.
  (The KE9NS variant scales both readings, which cancels the factor out of the SWR.) PowerSDR would show the same SWR on this radio.

## Measurement (user, 40 m, 7.075 MHz, after a passing tune: ind 84 cap 0 Hi-Z)
The Test tone logs our raw ADC / watts / SWR beside the tuner's own FWD / REV / SWR reports every 0.5 s while holding (transmit.log 22:47):

| set  | our ADC fwd / rev | ours fwd / ref, SWR       | tuner fwd / ref, SWR   |
|------|-------------------|---------------------------|------------------------|
| 3 W  | 447 / 70          | 3.05 W / 0.14 W, 1.55     | 3.45 W / 0.00 W, 1.00  |
| 8 W  | 770 / 152         | 7.72 W / 0.67 W, 1.83     | 7.04 W / 0.00 W, 1.00  |
| 15 W | 1118 / 251        | 14.73 W / 1.67 W, 2.01    | 12.96 W / 0.00 W, 1.00 |

- Forward agrees within ~15 % (ours reads higher as power rises).
- The tuner reads 0.00 W reflected in every sample: the transmitter sees a match.
- Our reflected detector voltage is 0.155 / 0.196 / 0.224 of the forward voltage: a leakage (poor bridge directivity, ~ -13 dB) that the diode
  compresses at low level, not a real mismatch (which would be a constant ratio and would show on the tuner).
- Caveat: the tuner only measures at its own input; a fault between the PA bridge and the tuner (relay, internal cable) would not show there.

## Status: left as is (user, 2026-11-19)
The gate still trips on OUR SWR at 3.0, so higher power through this antenna can trip on the leakage alone. The user will put up a better
antenna first. Option discussed, NOT done (it relaxes a safety limit; needs the user's OK): on ANT1 through the ATU, trip on the tuner's
own SWR (polled every 0.5 s during overs) at 3.0, keep our meter as a backstop on reflected watts (e.g. > 25 % of the set power), show both
SWRs (ours labelled "PA bridge"); ANT2 / ANT3 unchanged. Re-measure with the new antenna first (Tune, then Test tone 3 / 8 / 15 W).
