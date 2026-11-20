"""Start-up / shut-down sequence, transcribed from PowerSDR 2.7.2 (Console/console.cs).

Sources (line numbers in console.cs of upstream/powersdr-2.7.2):
  ~1950      PalManager.Init + scan; model code 3 = FLEX-3000, anything else = FLEX-5000
  2293       FWCEEPROM.Init()  -> read-only identification of the radio and its boards
  12056      InitRadio(): GetFirmwareRev (0 = comms error; "starting too quickly after
             power-on causes this"), range check, FWC.Initialize(), SetTRXPreamp(false)
  52722      chkPower on : SetStandby(false), RX1 antenna, [driver buffer size], VFO ->
             SetRX1FreqTW(tw, f), Sleep(100), start audio
  53100      chkPower off: MOX off, SetStandby(true), stop audio
Deliberately NOT sent: PA bias ON, EEPROM writes, calibration (QSD/QSE/TR/Sig/Gen/Test), transmit, ATU tune commands.

REVIEWED 2026-11-14 (Opus, analysis/startup-review.md). Conditions enforced in code below:
  * writes go only over the FireWire mailbox, the path PowerSDR 2.x uses; never MIDI SysEx (unverified for writes)
  * the reviewed exemptions are value-specific: INITIALIZE(0,0), SET_MOX(0,0), SET_PA_BIAS(0,0) only, enforced inside the safety gate
  * shutdown also switches PA bias off, as PowerSDR does when it closes (Console_Closing)
"""
import time
from dataclasses import dataclass, field

FW_MIN, FW_MAX = 0x02010408, 0x0201FFFF           # PowerSDR GetMinVersion/GetMaxVersion (5000 and 3000)
MODELS = {0: "FLEX-5000A", 1: "FLEX-5000C", 2: "FLEX-5000 (variant 2)", 3: "FLEX-3000"}
DDS_CLOCK_MHZ = 500.0                              # console.cs fwc_corrected_dds_clock (user correction = 0)
RX_ANT = {"NC": 0, "ANT1": 1, "ANT2": 2, "ANT3": 3, "RX1IN": 4}   # FWCAnt enum / SetRX1Ant(n)
MAX_RX_MHZ = 65.0                                  # HF + 6 m; VHF/UHF use the VU module / transverter path
SETTLE_BEFORE_AUDIO_S = 0.100                      # PowerSDR Thread.Sleep(100) before Audio.Start()


class StartupError(Exception):
    pass


def ver_str(v):
    return ".".join(str((v >> s) & 0xFF) for s in (24, 16, 8, 0))


def serial_str(sn):                                 # Misc.SerialToString
    return f"{sn & 0xFF:02d}{(sn >> 8) & 0xFF:02d}-{(sn >> 16) & 0xFFFF:04d}"


IF_OFFSET_MHZ = 0.009     # the DDS sits 9 kHz BELOW the dial (PowerSDR if_shift; same as the C++ app's kRadioIfOffsetHz). The radio's I/Q has a ~2 Hz hole at 0 Hz
                          # (measured 2026-11-17), so a carrier tuned exactly onto the DDS is lost; the receiver shifts the I/Q back up by this amount.


def dds_for_dial(dial_mhz, offset_mhz=IF_OFFSET_MHZ):
    """DDS frequency (MHz) for a dial frequency. startup_steps()/run_startup() take the DDS frequency; the CLI --freq is the DIAL."""
    dds = dial_mhz - offset_mhz
    if not 0.0 < dds <= MAX_RX_MHZ or not 0.0 < dial_mhz <= MAX_RX_MHZ:
        raise ValueError(f"dial {dial_mhz} MHz: the DDS ({dds:.6f} MHz) must be inside 0..{MAX_RX_MHZ} MHz")
    return dds


def freq_to_tw(f_mhz, clock=DDS_CLOCK_MHZ):
    """PowerSDR Freq2TW for the 5000: (uint)(0xFFFFFFFF * f / clock)."""
    if not 0.0 < f_mhz <= MAX_RX_MHZ:
        raise ValueError(f"frequency {f_mhz} MHz outside 0..{MAX_RX_MHZ} MHz")
    return int(0xFFFFFFFF * f_mhz / clock)


@dataclass
class Identity:
    model: int = -1
    serial: int = 0
    firmware: int = 0
    region: int = 0
    extended: bool = False
    boards: dict = field(default_factory=dict)     # name -> {"ok": bool, "rev": int|None, "sn": int|None}
    warnings: list = field(default_factory=list)

    @property
    def model_name(self): return MODELS.get(self.model, f"unknown model {self.model}")
    @property
    def has_atu(self): return self.boards.get("ATU", {}).get("ok", False)
    @property
    def has_rx2(self): return self.boards.get("RX2", {}).get("ok", False)

    def describe(self):
        lines = [f"{self.model_name}  serial {serial_str(self.serial)}  firmware v{ver_str(self.firmware)}"
                 f"  region {self.region}  extended={self.extended}"]
        for n, b in self.boards.items():
            if b["ok"]:
                lines.append(f"  {n:5s} present  rev 0x{b['rev']:08X}  sn 0x{b['sn']:08X}" if b["rev"] is not None
                             else f"  {n:5s} present")
            else:
                lines.append(f"  {n:5s} not fitted")
        lines += [f"  WARNING: {w}" for w in self.warnings]
        return "\n".join(lines)


def identify(fx, boot_wait=10.0, poll=0.5, sleep=time.sleep, clock=time.monotonic):
    """Read-only. Waits up to boot_wait seconds for a radio that is still booting."""
    end = clock() + boot_wait
    while True:
        try:
            fw = fx.get("GET_FIRMWARE_REV")
            if fw:
                break
        except (TimeoutError, OSError):
            pass
        if clock() >= end:
            raise StartupError("no usable reply from the radio (firmware revision 0 / timeout). "
                               "PowerSDR notes that starting too soon after power-on causes this; "
                               "wait ~15 s after switching the radio on and try again")
        sleep(poll)

    idn = Identity(firmware=fw)
    idn.model = fx.get("GET_MODEL"); sleep(0.01)
    idn.serial = fx.get("GET_SERIAL_NUM"); sleep(0.01)

    def board(name, ok_op, rev_op=None, sn_op=None):
        ok = bool(fx.get(ok_op)); sleep(0.01)
        b = {"ok": ok, "rev": None, "sn": None}
        if ok and rev_op: b["rev"] = fx.get(rev_op); sleep(0.01)
        if ok and sn_op: b["sn"] = fx.get(sn_op); sleep(0.01)
        idn.boards[name] = b

    board("TRX", "GET_TRX_OK", "GET_TRX_REV", "GET_TRX_SN")
    board("PA", "GET_PA_OK", "GET_PA_REV", "GET_PA_SN")
    if idn.model in (0, 1, 2):                      # 5000 family only (as PowerSDR)
        board("RFIO", "GET_RFIO_OK", "GET_RFIO_REV", "GET_RFIO_SN")
        board("RX2", "GET_RX2_OK", "GET_RX2_REV", "GET_RX2_SN")
        board("ATU", "GET_ATU_OK", "GET_ATU_REV", "GET_ATU_SN")
        board("VU", "GET_VU_OK", "GET_VU_REV", "GET_VU_SN")
    idn.region = fx.get("GET_REGION")
    idn.extended = bool(fx.get("GET_STATUS"))

    if idn.model == 3:
        raise StartupError("this is a FLEX-3000, not a 5000 (model code 3); not supported by this tool")
    if idn.model not in MODELS:
        idn.warnings.append(f"unknown model code {idn.model}; treating as a 5000 like PowerSDR does")
    if not FW_MIN <= fw <= FW_MAX:
        raise StartupError(f"firmware v{ver_str(fw)} is outside PowerSDR's supported range "
                           f"v{ver_str(FW_MIN)}..v{ver_str(FW_MAX)}; update it with the Windows tools "
                           "(software/ke9ns/F5K_firmware_old.zip, via the Windows VM) before using firesidectl")
    if not idn.boards["TRX"]["ok"]:
        idn.warnings.append("TRX board reports not OK")
    if not idn.boards["PA"]["ok"]:
        idn.warnings.append("PA board reports not OK")
    return idn


def flush_atu(fx, max_reads=64):
    """FWCATU.FlushBuffer(false): drain the ATU result buffer (read-only). Byte 4 of the result = bytes still queued."""
    n = 0
    while n < max_reads:
        n += 1
        if (fx.get("ATU_GET_RESULT", 0, 0) >> 24) & 0xFF == 0:
            break
    return n


def startup_steps(freq_mhz, rx_ant="ANT1", clock=DDS_CLOCK_MHZ):
    """The ordered write sequence as (opcode, data1, data2, why). Pure; used for dry-run and for execution."""
    if rx_ant not in RX_ANT:
        raise ValueError(f"rx_ant must be one of {sorted(RX_ANT)}")
    tw = freq_to_tw(freq_mhz, clock)
    return [
        ("INITIALIZE",     0, 0,                 "InitRadio(): FWC.Initialize()"),
        ("SET_TRX_PREAMP", 0, 0,                 "InitRadio(): SetTRXPreamp(false)"),
        ("SET_STANDBY",    0, 0,                 "power on: leave standby"),
        ("SET_RX1_ANT",    RX_ANT[rx_ant], 0,    f"RX1 antenna {rx_ant}"),
        ("SET_RX1_FREQ_TW", tw, float(freq_mhz), f"tune RX1 to {freq_mhz} MHz (tuning word 0x{tw:08X})"),
    ]


def shutdown_steps():
    return [("SET_MOX", 0, 0, "make sure the transmitter is not keyed"),
            ("SET_STANDBY", 1, 0, "power off: enter standby"),
            ("SET_PA_BIAS", 0, 0, "PA bias off, as PowerSDR does when it closes (Console_Closing)")]


# Reviewed exemptions from the dangerous-write gate. The allowed VALUES live in client.REVIEWED_VALUES and are enforced inside the gate itself
# (Opus diff-check F1/F2): INITIALIZE(0,0), SET_MOX(0,0), SET_PA_BIAS(0,0) only. allow_write is still required.
STEP_PERMITS = ("INITIALIZE", "SET_MOX", "SET_PA_BIAS")


def require_firewire(fx):
    from .pal import PalMailbox
    if not isinstance(getattr(fx, "link", None), PalMailbox):
        raise StartupError("the start-up/shut-down writes are only sent over the FireWire mailbox (`--fw /dev/fwN`), the path "
                           "PowerSDR 2.x uses. Writes over MIDI SysEx are unverified for these opcodes.")
    if not fx.link.verified:
        raise StartupError("the FireWire mailbox byte order has not been verified (call PalMailbox.verify(): a plausible firmware "
                           "revision must be read over THIS mailbox before any write)")


def run_steps(fx, steps, log=lambda *_: None):
    from .client import SafetyError, reviewed_value_ok
    require_firewire(fx)
    for op, d1, d2, why in steps:                      # validate all first: a bad step later must not leave earlier ones sent
        if op in STEP_PERMITS and not reviewed_value_ok(op, d1, d2):
            raise SafetyError(f"{op}({d1}, {d2}) is not a reviewed value; refusing")
    for op, d1, d2, why in steps:
        log(f"{op}({d1}, {d2})  # {why}")
        if op in STEP_PERMITS:
            with fx.permit(op):
                fx.set(op, d1, d2)
        else:
            fx.set(op, d1, d2)
    fx.sync()                                          # F3: confirm the firmware consumed the last write (read-only)


def run_startup(fx, freq_mhz, rx_ant="ANT1", log=lambda *_: None, **ident_kw):
    """identify -> validate -> (ATU flush) -> init + power on. Returns Identity. Caller waits SETTLE_BEFORE_AUDIO_S then starts audio."""
    steps = startup_steps(freq_mhz, rx_ant)            # validate arguments before touching the radio
    require_firewire(fx)                               # fail fast, before any traffic
    idn = identify(fx, **ident_kw)                     # firmware-in-range also proves the mailbox byte order is right
    log(idn.describe())
    if idn.has_atu:
        log(f"ATU present: drained result buffer in {flush_atu(fx)} read(s)")
    run_steps(fx, steps, log)
    return idn
