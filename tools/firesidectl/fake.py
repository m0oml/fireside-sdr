"""A simulated FLEX-5000 on the far side of a pty, for testing without hardware.

GetModel codes from PowerSDR (fwc_eeprom.cs / CommonPal.cs): 0 = FLEX-5000A, 1 = 5000C,
2 = another 5000 variant, 3 = FLEX-3000 (anything but 3 is treated as a 5000). Other values
below are made up; this only exercises our code.
"""
import os, threading, tty, select, struct
from . import protocol as P
from .opcodes import OPCODES, BY_VALUE

DEFAULTS = {
    "RDAL_OP_GET_MODEL": 0, "RDAL_OP_GET_SERIAL_NUM": 12345,
    "RDAL_OP_GET_FIRMWARE_REV": 0x02010408, "RDAL_OP_GET_TRX_OK": 1,
    "RDAL_OP_GET_PA_OK": 1, "RDAL_OP_GET_RFIO_OK": 1, "RDAL_OP_GET_ATU_OK": 1,
    "RDAL_OP_GET_RX2_OK": 0, "RDAL_OP_GET_VU_OK": 0,
}


class FakeRadio:
    def __init__(self, drop_ids=(), noise=False):
        self.master, self.slave = os.openpty()
        tty.setraw(self.master); tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.eeprom = {"TRX": bytearray(b"\xff" * 0x1000), "RX2": bytearray(b"\xff" * 0x400)}
        self.answers = dict(DEFAULTS)      # name -> int, or a callable returning int (per-request behaviour)
        self.writes = []                   # [(name, d1, d2)] every set received
        self.gets = []
        self.drop_ids = set(drop_ids)      # msg ids to ignore (timeout tests)
        self.noise = noise                 # send a stale reply + MIDI notes first
        self._stop = False
        self._buf = bytearray()
        self._t = threading.Thread(target=self._run, daemon=True); self._t.start()

    def note(self, status, d1, d2=0):
        """Inject a radio->PC MIDI message (e.g. paddle: 0x90, 61, 127)."""
        os.write(self.master, bytes([status, d1, d2]))

    def _run(self):
        while not self._stop:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                self._buf += os.read(self.master, 4096)
            except OSError:
                return
            while True:
                if 0xF7 not in self._buf:
                    break
                end = self._buf.index(0xF7)
                frame = bytes(self._buf[:end + 1]); del self._buf[:end + 1]
                self._handle(frame)

    def _handle(self, frame):
        mid, _proto, op, d1, d2 = P.decode_request(frame)
        name = BY_VALUE.get(op, f"#{op}")
        acc = OPCODES[name][1] if name in OPCODES else "?"
        is_get = acc in ("r", "rw")
        if not is_get:
            self.writes.append((name, d1, d2)); return
        self.gets.append((name, d1, d2))
        if mid in self.drop_ids:
            return
        if self.noise:
            os.write(self.master, bytes([0x90, 61, 127, 0x80, 61, 0]))
            os.write(self.master, P.encode_reply((mid - 1) & 0xFFFF, 0xDEAD))   # stale
        if name in ("RDAL_OP_READ_TRX_EEPROM_UINT32", "RDAL_OP_READ_RX2_EEPROM_UINT32"):
            img = self.eeprom["TRX" if "_TRX_" in name else "RX2"]
            raw = int.from_bytes(img[d1:d1 + 4], "little") if d1 + 4 <= len(img) else 0xFFFFFFFF
        else:
            raw = self.answers.get(name, 0)
            raw = raw() if callable(raw) else raw
        reply = P.encode_reply(mid, raw)
        for i in range(0, len(reply), 7):            # dribble out, like a slow link
            os.write(self.master, reply[i:i + 7])

    def close(self):
        # Join first: closing while the thread is in select() lets a later FakeRadio
        # reuse the same fd numbers and have this dead thread steal its traffic.
        self._stop = True
        self._t.join(timeout=2)
        for fd in (self.master, self.slave):
            try: os.close(fd)
            except OSError: pass
