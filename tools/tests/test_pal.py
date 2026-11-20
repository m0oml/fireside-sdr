import os, struct, sys, unittest
from unittest import mock
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from firesidectl import pal, firewire as fw, protocol as P
from firesidectl.client import Flex, SafetyError
from firesidectl.opcodes import OPCODES


class FakeDiceBus:
    """Register file behaving like the Flex firmware as pal.dll expects it (big-endian bus)."""
    def __init__(self, busy_polls=2, start_busy=0, stuck=False):
        self.mem = {pal.CMD: struct.pack(">I", pal.BUSY if start_busy else 0),
                    pal.RESULT: b"\0" * 4, pal.EVENT: b"\0" * 4}
        self.start_busy, self.busy_polls, self.stuck = start_busy, busy_polls, stuck
        self.writes, self.reads, self._left = [], 0, start_busy
        self.answers = {OPCODES["RDAL_OP_GET_MODEL"][0]: 0, OPCODES["RDAL_OP_GET_SERIAL_NUM"][0]: 0xA1B2C3D4}
        self.executed = []

    def read(self, off, n):
        self.reads += 1
        if off == pal.CMD and not self.stuck:
            if self._left > 0:
                self._left -= 1
                if self._left == 0:
                    self._execute()
        return self.mem[off]

    def write(self, off, data):
        self.writes.append((off, bytes(data)))
        assert off == pal.CMD and len(data) == 12, "must be one 12-byte block at CMD"
        self.mem[pal.CMD] = data[:4]; self.mem[pal.RESULT] = data[4:8]
        self._left = self.busy_polls

    def _execute(self):
        op = struct.unpack(">I", self.mem[pal.CMD])[0]
        if op & pal.BUSY:
            code = op & ~pal.BUSY
            self.executed.append(code)
            self.mem[pal.RESULT] = struct.pack(">I", self.answers.get(code, 0))
            self.mem[pal.CMD] = struct.pack(">I", code)          # firmware clears bit 31


class Mailbox(unittest.TestCase):
    def test_read_op_wire_format_and_result(self):
        bus = FakeDiceBus()
        mb = pal.PalMailbox(bus, poll=0)
        self.assertEqual(mb.request(1221, 7, 9, True), 0)
        off, blk = bus.writes[0]
        self.assertEqual(off, 0xFFFF_E000_1000)
        self.assertEqual(blk, struct.pack(">III", 0x80000000 | 1221, 7, 9))

    def test_waits_for_idle_before_writing(self):
        bus = FakeDiceBus(start_busy=3)
        pal.PalMailbox(bus, poll=0).request(1220, 0, 0, True)
        self.assertGreaterEqual(bus.reads, 3 + 1)
        self.assertEqual(len(bus.writes), 1)

    def test_stuck_busy_times_out_instead_of_hanging(self):
        bus = FakeDiceBus(start_busy=1, stuck=True)
        with self.assertRaises(TimeoutError):
            pal.PalMailbox(bus, timeout=0.05, poll=0).request(1221, 0, 0, True)
        self.assertEqual(bus.writes, [])                         # never wrote over a busy mailbox

    def test_set_does_not_wait_but_next_command_does(self):
        bus = FakeDiceBus(busy_polls=3)
        mb = pal.PalMailbox(bus, poll=0)
        self.assertIsNone(mb.request(1277, 2, 0, False))        # SET_ANT
        self.assertEqual(mb.request(1220, 0, 0, True), 0xA1B2C3D4)
        self.assertEqual(bus.executed, [1277, 1220])

    def test_little_endian_switch(self):
        bus = FakeDiceBus()
        bus.read = lambda off, n: b"\0" * 4
        mb = pal.PalMailbox(bus, byteorder="little", poll=0)
        mb.request(1221, 1, 0, False)
        self.assertEqual(bus.writes[0][1], struct.pack("<III", 0x80000000 | 1221, 1, 0))

    def test_event_decode(self):
        bus = FakeDiceBus(); bus.mem[pal.EVENT] = struct.pack(">I", 0x01 | 0x08)
        raw, bits = pal.PalMailbox(bus).read_event()
        self.assertEqual(raw, 9); self.assertTrue(bits["dot"] and bits["mic_ptt"]); self.assertFalse(bits["dash"])

    def test_flex_client_over_mailbox(self):
        bus = FakeDiceBus()
        fx = Flex(pal.PalMailbox(bus, poll=0), allow_write=True)
        self.assertEqual(fx.get("GET_MODEL"), 0)
        fx.set("SET_RX1_FREQ", 7.0745)
        off, blk = bus.writes[-1]
        op, d1, d2 = struct.unpack(">III", blk)
        self.assertEqual(op, 0x80000000 | 1244)
        self.assertAlmostEqual(P.as_f32(d1), 7.0745, places=4)
        with self.assertRaises(SafetyError): fx.set("SET_MOX", 1)


class ProbeSafety(unittest.TestCase):
    def test_busy_mailbox_aborts_the_probe_before_trying_the_other_order(self):
        """A mailbox busy under the first interpretation can look idle under the other: the probe must not write into it."""
        bus = FakeDiceBus(start_busy=1, stuck=True)
        bus.mem[pal.CMD] = struct.pack(">I", pal.BUSY)
        order, notes = pal.probe_order(bus, timeout=0.05)
        self.assertIsNone(order); self.assertEqual(bus.writes, [])
        self.assertTrue(any("busy before any command" in n for n in notes), notes)
        self.assertEqual(len([n for n in notes if "TimeoutError" in n]), 1)                       # the second order was never tried


class CdevPacking(unittest.TestCase):
    """firewire.py can't run here (no controller); check its ioctl packing against the C layout."""
    def make(self, events):
        dev = fw.FwDevice.__new__(fw.FwDevice)
        dev.path, dev.fd, dev._closure, dev.generation = "/dev/fwX", 99, 0, 7
        dev.node_id = dev.local_node_id = 0
        sent = []
        def ioctl(fd, req, arg, *a):
            sent.append((req, bytes(arg)))
        evs = list(events)
        patches = [mock.patch.object(fw.fcntl, "ioctl", ioctl),
                   mock.patch.object(fw.select, "select", lambda r, w, x, t: (r, [], [])),
                   mock.patch.object(fw.os, "read", lambda fd, n: evs.pop(0)(sent))]
        for p in patches: p.start(); self.addCleanup(p.stop)
        return dev, sent

    @staticmethod
    def response(rcode, data=b""):
        return lambda sent: struct.pack("<QIII", struct.unpack_from("<Q", sent[-1][1], 16)[0],
                                        fw.EV_RESPONSE, rcode, len(data)) + data

    def test_quadlet_read(self):
        dev, sent = self.make([self.response(0, b"\x80\x00\x04\xc5")])
        self.assertEqual(dev.read(pal.CMD, 4), b"\x80\x00\x04\xc5")
        req, arg = sent[0]
        self.assertEqual(req, fw.IOC_SEND_REQUEST); self.assertEqual(len(arg), 40)
        tcode, length, offset, closure, _data, gen = struct.unpack("<IIQQQI4x", arg)
        self.assertEqual((tcode, length, offset, gen), (fw.TCODE_READ_QUADLET, 4, 0xFFFFE0001000, 7))

    def test_block_write_tcode(self):
        dev, sent = self.make([self.response(0)])
        dev.write(pal.CMD, b"\0" * 12)
        self.assertEqual(struct.unpack_from("<II", sent[0][1])[0:2], (fw.TCODE_WRITE_BLOCK, 12))

    def test_rcode_error_raised(self):
        dev, _ = self.make([self.response(7)])
        with self.assertRaisesRegex(fw.FwError, "address error"):
            dev.read(pal.CMD, 4)

    def test_bus_reset_then_retry_on_new_generation(self):
        reset = lambda sent: struct.pack("<QIIIIIII", 0, fw.EV_BUS_RESET, 0xFFC1, 0xFFC0, 0, 0, 0, 8)
        dev, sent = self.make([reset, self.response(0, b"\0\0\0\3")])
        self.assertEqual(dev.read(pal.RESULT, 4), b"\0\0\0\3")
        self.assertEqual([struct.unpack_from("<I", a, 32)[0] for _, a in sent], [7, 8])
        self.assertEqual(dev.node_id, 0xFFC1)


if __name__ == "__main__":
    unittest.main()
