"""Raw MIDI transport over ALSA rawmidi device nodes (/dev/snd/midiCxDy). No libraries.

Splits the incoming byte stream into SysEx frames and channel messages, so the
caller never sees partial reads. Works on any file-like char device, which lets
the tests drive it over a pty.
"""
import os, select, time, glob, re

NOTE_OFF, NOTE_ON = 0x80, 0x90


class RawMidi:
    def __init__(self, path: str):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self._buf = bytearray()
        self._in_sysex = False
        self.frames = []     # complete SysEx frames, oldest first
        self.events = []     # (status, d1, d2) channel messages (notes etc.)

    def close(self):
        if self.fd is not None:
            os.close(self.fd); self.fd = None

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def send(self, data: bytes):
        view = memoryview(data)
        deadline = time.monotonic() + 1.0
        while view:
            try:
                n = os.write(self.fd, view); view = view[n:]
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise TimeoutError("MIDI write blocked")
                select.select([], [self.fd], [], 0.05)

    def pump(self, timeout: float = 0.0):
        """Read whatever is available (waiting up to timeout) and parse it."""
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return
        try:
            chunk = os.read(self.fd, 4096)
        except BlockingIOError:
            return
        self._parse(chunk)

    def _parse(self, chunk: bytes):
        for b in chunk:
            if b >= 0xF8:                         # real-time bytes may interleave anywhere
                continue
            if b == 0xF7 and not self._in_sysex:  # stray end-of-sysex
                continue
            if b == 0xF0:
                self._buf = bytearray([b]); self._in_sysex = True
            elif b == 0xF7 and self._in_sysex:
                self._buf.append(b); self.frames.append(bytes(self._buf))
                self._buf = bytearray(); self._in_sysex = False
            elif self._in_sysex:
                if b & 0x80:                      # status byte inside SysEx: abort frame
                    self._in_sysex = False; self._buf = bytearray(); self._chan(b)
                else:
                    self._buf.append(b)
            else:
                self._chan(b)

    _pend = None
    def _chan(self, b: int):
        if b & 0x80:
            self._pend = [b]
        elif self._pend is not None:
            self._pend.append(b)
            st = self._pend[0] & 0xF0
            need = 3 if st in (0x80, 0x90, 0xA0, 0xB0, 0xE0) else 2
            if len(self._pend) == need:
                self.events.append(tuple(self._pend) + (0,) * (3 - len(self._pend)))
                self._pend = [self._pend[0]]       # running status

    def next_frame(self, timeout: float):
        end = time.monotonic() + timeout
        while True:
            if self.frames:
                return self.frames.pop(0)
            left = end - time.monotonic()
            if left <= 0:
                return None
            self.pump(min(left, 0.05))


def list_midi_devices():
    """[(path, card_name)] for every ALSA rawmidi node, with FLEX candidates first."""
    names = {}
    try:
        for line in open("/proc/asound/cards"):
            m = re.match(r"\s*(\d+)\s+\[(\S+)\s*\]:\s*(.*)", line)
            if m:
                names[int(m.group(1))] = f"{m.group(2)} {m.group(3)}"
    except OSError:
        pass
    out = []
    for p in sorted(glob.glob("/dev/snd/midiC*D*")):
        c = int(re.search(r"midiC(\d+)D", p).group(1))
        out.append((p, names.get(c, "?")))
    out.sort(key=lambda t: "flex" not in t[1].lower())
    return out
