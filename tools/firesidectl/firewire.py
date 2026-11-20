"""Minimal Linux firewire-cdev client (/dev/fwN): async quadlet/block reads and writes.

Struct layouts and ioctl numbers were taken from <linux/firewire-cdev.h> by compiling a
probe program on this host (x86-64): get_info=40B, send_request=40B, event_response=24B+data.
No libraries; just fcntl.ioctl + os.read for the response events.
"""
import ctypes, fcntl, glob, os, select, struct, time

IOC_GET_INFO, IOC_SEND_REQUEST = 0xC0282300, 0x40282301
EV_BUS_RESET, EV_RESPONSE = 0x00, 0x01
TCODE_WRITE_QUADLET, TCODE_WRITE_BLOCK, TCODE_READ_QUADLET, TCODE_READ_BLOCK = 0x0, 0x1, 0x4, 0x5
RCODES = {0: "complete", 4: "conflict", 5: "data error", 6: "type error", 7: "address error",
          16: "send error", 17: "cancelled", 18: "busy", 19: "generation", 20: "no ack"}


class FwError(IOError):
    pass


class FwDevice:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDWR)
        self._closure = 0
        self.rom = b""
        self._get_info()

    def close(self):
        if self.fd is not None:
            os.close(self.fd); self.fd = None

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def _get_info(self):
        rom = ctypes.create_string_buffer(1024)
        reset = ctypes.create_string_buffer(40)
        arg = bytearray(struct.pack("<IIQQQI4x", 4, 1024, ctypes.addressof(rom),
                                    ctypes.addressof(reset), 0, 0))
        fcntl.ioctl(self.fd, IOC_GET_INFO, arg, True)
        rom_len = struct.unpack_from("<I", arg, 4)[0]
        self.rom = rom.raw[:min(rom_len, 1024)]
        self._apply_reset(reset.raw)

    def _apply_reset(self, ev):
        (self.node_id, self.local_node_id, _bm, _irm, _root,
         self.generation) = struct.unpack_from("<IIIIII", ev, 12)

    @property
    def is_local(self):
        return self.node_id == self.local_node_id

    def rom_quadlets(self):
        """Config ROM quadlets. The kernel copies them in HOST byte order (measured 2026-11-18: read big-endian on x86 the
        radio's GUID 001c2d04007fffff came out quadlet-swapped)."""
        n = len(self.rom) // 4
        return list(struct.unpack(f"={n}I", self.rom[:n * 4]))

    def guid(self):
        q = self.rom_quadlets()
        return (q[3] << 32) | q[4] if len(q) > 4 else None

    def _request(self, tcode, offset, data_or_len, timeout=1.0):
        is_read = tcode in (TCODE_READ_QUADLET, TCODE_READ_BLOCK)
        length = data_or_len if is_read else len(data_or_len)
        buf = (ctypes.create_string_buffer(length) if is_read
               else ctypes.create_string_buffer(bytes(data_or_len), length))
        for attempt in range(3):
            self._closure += 1
            closure = self._closure
            arg = struct.pack("<IIQQQI4x", tcode, length, offset, closure,
                              ctypes.addressof(buf), self.generation)
            fcntl.ioctl(self.fd, IOC_SEND_REQUEST, arg)
            ev = self._wait_response(closure, timeout)
            if ev is None:
                continue                                    # bus reset: retry on new generation
            rcode, data = ev
            if rcode == 19:                                 # stale generation
                continue
            if rcode != 0:
                raise FwError(f"{self.path} offset 0x{offset:012X}: rcode {rcode} ({RCODES.get(rcode, '?')})")
            return data if is_read else None
        raise FwError(f"{self.path}: request failed after bus resets")

    def _wait_response(self, closure, timeout):
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"{self.path}: no FireWire response")
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                continue
            ev = os.read(self.fd, 4096)
            etype = struct.unpack_from("<I", ev, 8)[0]
            if etype == EV_BUS_RESET:
                self._apply_reset(ev); return None
            if etype == EV_RESPONSE and struct.unpack_from("<Q", ev, 0)[0] == closure:
                rcode, length = struct.unpack_from("<II", ev, 12)
                return rcode, ev[20:20 + length]

    # public API: data is raw bus bytes (FireWire is big-endian)
    def read(self, offset, length):
        tc = TCODE_READ_QUADLET if length == 4 else TCODE_READ_BLOCK
        return self._request(tc, offset, length)

    def write(self, offset, data: bytes):
        tc = TCODE_WRITE_QUADLET if len(data) == 4 else TCODE_WRITE_BLOCK
        self._request(tc, offset, data)


def list_devices():
    """[(path, guid, is_local)] for every readable /dev/fw* node."""
    out = []
    for p in sorted(glob.glob("/dev/fw[0-9]*"), key=lambda s: int(s[7:])):
        try:
            with FwDevice(p) as d:
                out.append((p, d.guid(), d.is_local))
        except OSError as e:
            out.append((p, None, f"error: {e.strerror}"))
    return out
