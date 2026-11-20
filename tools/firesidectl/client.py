"""High-level client with a safety policy.

Policy (a donated radio whose factory calibration lives in EEPROM must not be damaged):
  * reads (opcodes PowerSDR only ever calls via ReadOp*)   -> always allowed
  * writes                                                 -> need allow_write=True
  * dangerous writes (EEPROM / registers / I2C / bias /
    reset / power / anything that can key the transmitter: MOX, ATU tune, T/R) and opcodes PowerSDR
    never calls (unknown behaviour)                        -> need allow_dangerous=True
"""
import re, time
from . import protocol as P
from .opcodes import OPCODES, BY_VALUE

DANGEROUS = re.compile(
    r"EEPROM|REG_SET|WRITE_.*REG|_REG$|I2C_WRITE|FIRESIDEWIRE_WRITE|PA_BIAS|POT|RDAC|"
    r"RESET|PWRBT|POWER_OFF|POWER_DOWN|INITIALIZE|CLOCK|PLL|DDS_REG|GPIO|FAN|"
    r"MOX|ATU_SEND|ENABLE_ATU|SET_TR$|AMP_TX|TX_DAC|FULL_DUPLEX|XVTR|XVTXEN|KEY_2M")  # TX-capable


# Reviewed exemptions (analysis/startup-review.md): a permit() for these opcodes covers ONLY these raw wire values.
# Checked inside the safety gate on the packed words, so no caller and no other thread can use a permit to send another value.
REVIEWED_VALUES = {
    "RDAL_OP_INITIALIZE":  lambda d1, d2: d1 == 0 and d2 == 0,
    "RDAL_OP_SET_MOX":     lambda d1, d2: d1 == 0 and d2 == 0,     # un-key only
    "RDAL_OP_SET_PA_BIAS": lambda d1, d2: d1 == 0 and d2 == 0,     # bias off only
    # tools/firesidectl/restore_flag.py only: put back the "checksums present" flag PowerSDR cleared on 2026-11-18 (0x3F: 0xC5 -> 0xFF,
    # hardware-day/capture-20261118-powersdr). This address and this value only; every other EEPROM write stays refused.
    "RDAL_OP_WRITE_TRX_EEPROM_UINT8": lambda d1, d2: d1 == 0x3F and d2 == 0xC5,
}


def reviewed_value_ok(op, data1=0, data2=0):
    """True unless `op` is value-restricted and (data1, data2) do not pack to its reviewed wire values."""
    name, val, acc, args, res = resolve(op)
    pred = REVIEWED_VALUES.get(name)
    return pred is None or pred(P.pack_arg(data1, args[0]), P.pack_arg(data2, args[1]))


class SafetyError(Exception):
    pass


def resolve(op):
    """name or number -> (name, value, access, argtypes, result)."""
    if isinstance(op, str):
        key = op if op in OPCODES else "RDAL_OP_" + op.upper()
        key = key if key in OPCODES else "FWC_OP_" + op.upper()
        if key not in OPCODES:
            raise KeyError(f"unknown opcode {op!r}")
        v, acc, args, res = OPCODES[key]
        return key, v, acc, args, res
    name = BY_VALUE.get(int(op))
    if name is None:
        return f"#{int(op)}", int(op), "?", ("u32", "u32"), None
    v, acc, args, res = OPCODES[name]
    return name, v, acc, args, res


class MidiLink:
    """Opcode link over the radio's MIDI SysEx port (route 1)."""

    def __init__(self, transport, log=None):
        self.t = transport
        self.log = log or (lambda *_: None)
        self._id = 0

    def _next_id(self):
        i = self._id; self._id = (self._id + 1) & 0xFFFF; return i

    def request(self, opcode, data1, data2, want_reply, timeout=1.0):
        mid = self._next_id()
        frame = P.encode_request(mid, opcode, data1, data2)
        self.log("TX", opcode, frame)
        self.t.send(frame)
        if not want_reply:
            return None
        end = time.monotonic() + timeout
        while True:
            f = self.t.next_frame(max(0.0, end - time.monotonic()))
            if f is None:
                raise TimeoutError(f"no reply to opcode {opcode} (id {mid})")
            self.log("RX", opcode, f)
            rid, raw = P.decode_reply(f)
            if rid == mid:
                return raw                                  # stale replies are skipped


class Flex:
    """Safety-gated opcode client over any link (MidiLink, pal.PalMailbox)."""

    def __init__(self, link, allow_write=False, allow_dangerous=False, log=None):
        self.link = link if hasattr(link, "request") else MidiLink(link, log)
        self.allow_write = allow_write
        self.allow_dangerous = allow_dangerous
        self._permitted = set()          # opcode names exempted from the dangerous check

    # kept for tests that poke the MIDI message id
    @property
    def _id(self): return self.link._id
    @_id.setter
    def _id(self, v): self.link._id = v

    def _check(self, name, access, is_get, raw=None):
        if is_get:
            if access in ("r", "rw") or name.split("_OP_")[-1].startswith(("GET_", "READ_")):
                if "EEPROM" in name and "WRITE" in name:
                    raise SafetyError(f"{name} is a write")
                return
            if not self.allow_dangerous:
                raise SafetyError(f"{name}: not a known read opcode; needs allow_dangerous")
            return
        if not self.allow_write:
            raise SafetyError(f"{name}: writes disabled (use --allow-write)")
        if name in self._permitted:
            pred = REVIEWED_VALUES.get(name)
            if pred is None or (raw is not None and pred(*raw)):
                return                   # still requires allow_write (checked above)
            # permitted opcode but not its reviewed value (or values unknown): fall through to the normal dangerous check
        if (DANGEROUS.search(name) or access == "?") and not self.allow_dangerous:
            raise SafetyError(f"{name}: dangerous/unknown write (use --allow-dangerous)")

    def permit(self, *names):
        """Context manager: exempt specific, reviewed opcodes from the dangerous-write check.
        Used by the start-up/shut-down sequences; allow_write is still required."""
        import contextlib
        full = {resolve(n)[0] for n in names}
        @contextlib.contextmanager
        def cm():
            before = set(self._permitted); self._permitted |= full
            try: yield self
            finally: self._permitted = before
        return cm()

    def get(self, op, data1=0, data2=0, timeout=1.0, as_float=None):
        name, val, acc, args, res = resolve(op)
        self._check(name, acc, True)
        raw = self.link.request(val, P.pack_arg(data1, "u32"), P.pack_arg(data2, "u32"), True, timeout)
        return P.as_f32(raw) if (as_float if as_float is not None else res == "f32") else raw

    def set(self, op, data1=0, data2=0):
        name, val, acc, args, res = resolve(op)
        raw = (P.pack_arg(data1, args[0]), P.pack_arg(data2, args[1]))
        self._check(name, acc, False, raw)
        self.link.request(val, raw[0], raw[1], False)

    def sync(self):
        """Wait until the link has consumed the last command (read-only; no-op for links without a mailbox)."""
        s = getattr(self.link, "sync", None)
        if s:
            s()


PROBE = ["GET_MODEL", "GET_SERIAL_NUM", "GET_FIRMWARE_REV", "GET_TRX_OK", "GET_TRX_REV",
         "GET_TRX_SN", "GET_PA_OK", "GET_PA_REV", "GET_PA_SN", "GET_RFIO_OK", "GET_RFIO_REV",
         "GET_RFIO_SN", "GET_ATU_OK", "GET_ATU_REV", "GET_ATU_SN", "GET_RX2_OK", "GET_RX2_REV",
         "GET_RX2_SN", "GET_VU_OK", "GET_REGION", "GET_STATUS", "READ_PTT"]


def probe(flex):
    """Read-only identification sweep. Returns {name: value or error string}."""
    out = {}
    for n in PROBE:
        try:
            out[n] = flex.get(n)
        except (TimeoutError, KeyError) as e:
            out[n] = f"ERR {e}"
    return out
