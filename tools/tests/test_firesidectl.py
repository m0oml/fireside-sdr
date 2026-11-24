import os, struct, sys, time, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from firesidectl import protocol as P
from firesidectl.transport import RawMidi
from firesidectl.client import Flex, SafetyError, probe
from firesidectl.fake import FakeRadio
from firesidectl.opcodes import OPCODES


class Codec(unittest.TestCase):
    def test_request_layout(self):
        f = P.encode_request(0x0102, 1221, 5, 6)
        self.assertEqual(len(f), 37)
        self.assertEqual(f[:4], b"\xf0\x00\x00\x41"); self.assertEqual(f[-1], 0xF7)
        self.assertTrue(all(b < 0x80 for b in f[1:-1]))            # valid MIDI data bytes
        self.assertEqual(bytes.fromhex(f[4:-1].decode()),
                         struct.pack("<HHIII", 0x0102, 0, 1221, 5, 6))
        self.assertEqual(P.decode_request(f), (0x0102, 0, 1221, 5, 6))

    def test_reply_roundtrip_and_float(self):
        raw = P.pack_arg(14.2, "f32")
        self.assertAlmostEqual(P.as_f32(P.decode_reply(P.encode_reply(9, raw))[1]), 14.2, places=5)
        self.assertEqual(P.decode_reply(P.encode_reply(65535, 0xFFFFFFFF)), (65535, 0xFFFFFFFF))

    def test_arg_packing(self):
        self.assertEqual(P.pack_arg(-1, "i32"), 0xFFFFFFFF)
        self.assertEqual(P.as_i32(0xFFFFFFFE), -2)
        with self.assertRaises(ValueError): P.pack_arg(2**32, "u32")
        with self.assertRaises(ValueError): P.pack_arg(-1, "u32")

    def test_bad_frames(self):
        for bad in (b"", b"\xf0\x00\x00\x41ZZ\xf7", b"\xf0\x00\x00\x42" + b"00" * 16 + b"\xf7",
                    b"\xf0\x00\x00\x41" + b"0" * 31 + b"\xf7"):
            with self.assertRaises(ValueError): P.decode_request(bad)

    def test_opcode_numbers(self):
        for n, v in (("RDAL_OP_CREATE_INTERFACE", 1000), ("RDAL_OP_NOOP", 1018),
                     ("RDAL_OP_GET_MODEL", 1221), ("RDAL_OP_SET_RX1_FREQ", 1244),
                     ("RDAL_OP_SET_MOX", 1292), ("RDAL_OP_GET_STATUS", 1355)):
            self.assertEqual(OPCODES[n][0], v, n)
        vals = [v[0] for v in OPCODES.values()]
        self.assertEqual(len(vals), len(set(vals)), "duplicate opcode values")


class Parser(unittest.TestCase):
    def feed(self, data, step):
        m = RawMidi.__new__(RawMidi); m.frames = []; m.events = []; m._buf = bytearray(); m._in_sysex = False
        for i in range(0, len(data), step):
            m._parse(data[i:i + step])
        return m

    def test_fragmentation(self):
        f = P.encode_reply(3, 77)
        for step in (1, 2, 5, 100):
            self.assertEqual(self.feed(f + f, step).frames, [f, f])

    def test_notes_and_realtime_mixed_in(self):
        f = P.encode_reply(3, 77)
        data = bytes([0x90, 61, 127]) + f[:7] + b"\xf8" + f[7:] + bytes([0x80, 61, 0])
        m = self.feed(data, 4)
        self.assertEqual(m.frames, [f])
        self.assertEqual(m.events, [(0x90, 61, 127), (0x80, 61, 0)])

    def test_truncated_sysex_recovers(self):
        f = P.encode_reply(3, 77)
        m = self.feed(f[:10] + bytes([0x90, 63, 100]) + f, 3)
        self.assertEqual(m.frames, [f]); self.assertEqual(m.events, [(0x90, 63, 100)])


class OverPty(unittest.TestCase):
    def setUp(self):
        self.radio = FakeRadio(); self.t = RawMidi(self.radio.path)
        self.fx = Flex(self.t, allow_write=True)
    def tearDown(self):
        self.t.close(); self.radio.close()

    def test_probe(self):
        r = probe(self.fx)
        self.assertEqual(r["GET_MODEL"], 0); self.assertEqual(r["GET_SERIAL_NUM"], 12345)
        self.assertEqual(r["GET_RX2_OK"], 0);  self.assertEqual(r["GET_ATU_OK"], 1)
        self.assertFalse(any(isinstance(v, str) for v in r.values()), r)

    def test_set_float_frequency_arrives_intact(self):
        self.fx.set("SET_RX1_FREQ", 14.2345)
        self.fx.get("GET_MODEL")                    # round trip so the set has been processed
        name, d1, d2 = self.radio.writes[-1]
        self.assertEqual(name, "RDAL_OP_SET_RX1_FREQ")
        self.assertAlmostEqual(P.as_f32(d1), 14.2345, places=4); self.assertEqual(d2, 0)

    def test_stale_reply_and_notes_ignored(self):
        self.radio.noise = True
        self.assertEqual(self.fx.get("GET_SERIAL_NUM"), 12345)
        self.assertEqual(self.fx.get("GET_SERIAL_NUM"), 12345)
        self.assertIn((0x90, 61, 127), self.t.events)          # paddle events not lost

    def test_timeout(self):
        self.radio.drop_ids = {self.fx._id}
        with self.assertRaises(TimeoutError): self.fx.get("GET_MODEL", timeout=0.3)

    def test_msg_id_wraps(self):
        self.fx._id = 0xFFFF
        self.assertEqual(self.fx.get("GET_MODEL"), 0); self.assertEqual(self.fx.get("GET_MODEL"), 0)


class Safety(unittest.TestCase):
    def setUp(self):
        self.radio = FakeRadio(); self.t = RawMidi(self.radio.path)
    def tearDown(self):
        self.t.close(); self.radio.close()

    def sent(self):
        self.t.send(b""); time.sleep(0.15); return list(self.radio.writes)

    def test_writes_blocked_by_default(self):
        with self.assertRaises(SafetyError): Flex(self.t).set("SET_RX1_FREQ", 7.0)
        self.assertEqual(self.sent(), [])

    def test_dangerous_blocked_even_with_allow_write(self):
        fx = Flex(self.t, allow_write=True)
        for op in ("WRITE_TRX_EEPROM_UINT8", "SET_EEPROM_WC", "REG_SET", "I2C_WRITE_VALUE", "FLEXWIRE_WRITE_VALUE", "FLEXWIRE_WRITE_2_VALUE",
                   "SET_PA_BIAS", "SET_POWER_OFF", "NOOP", "SET_MOX", "ATU_SEND_CMD", "SET_TR"):
            with self.assertRaises(SafetyError, msg=op): fx.set(op, 1, 1)
        self.assertEqual(self.sent(), [])

    def test_get_of_write_opcode_refused(self):
        with self.assertRaises(SafetyError): Flex(self.t).get("WRITE_TRX_EEPROM_UINT8", 0, 0)
        with self.assertRaises(SafetyError): Flex(self.t).get("SET_MOX")

    def test_eeprom_reads_allowed(self):
        self.assertEqual(Flex(self.t).get("READ_TRX_EEPROM_UINT8", 0x10), 0)


if __name__ == "__main__":
    unittest.main()
