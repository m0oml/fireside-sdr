"""restore_flag: refuses on anything but the exact situation, writes one reviewed value, verifies; the safety gate allows nothing else."""
import struct, sys, os, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from tools.firesidectl import restore_flag as R
from tools.firesidectl.client import Flex, SafetyError
from tools.firesidectl.opcodes import OPCODES

BACKUP = os.path.join(os.path.dirname(__file__), "..", "..", "backups", "reference_trx.bin")


class FakeMailbox:
    """EEPROM model behind the opcode link: 32-bit reads, UINT8 writes; records every command."""
    def __init__(self, image, serial, apply_write=True):
        self.mem = bytearray(image); self.serial = serial; self.sent = []; self.apply_write = apply_write
    def request(self, opcode, d1, d2, want_reply, timeout=None):
        self.sent.append((opcode, d1, d2))
        if opcode == OPCODES["RDAL_OP_GET_SERIAL_NUM"][0]: return self.serial
        if opcode == OPCODES["RDAL_OP_READ_TRX_EEPROM_UINT32"][0]: return struct.unpack_from("<I", self.mem, d1)[0]
        if opcode == OPCODES["RDAL_OP_WRITE_TRX_EEPROM_UINT8"][0]:
            if self.apply_write: self.mem[d1] = d2 & 0xFF
            return None
        raise AssertionError(f"unexpected opcode {opcode}")
    def sync(self): pass
    def writes(self): return [s for s in self.sent if s[0] == OPCODES["RDAL_OP_WRITE_TRX_EEPROM_UINT8"][0]]


def image():
    b = bytearray(3088); b[0x18:0x1C] = struct.pack("<I", 0x0A900A04); b[0x3F] = 0xC5; b[0x100] = 0x12; return bytes(b)


class RestoreFlag(unittest.TestCase):
    def go(self, now, serial=0x0A900A04, write=True, apply_write=True):
        mb = FakeMailbox(now, serial, apply_write); out = []
        rc = R.run(Flex(mb, allow_write=True, allow_dangerous=False), image(), 0x0A900A04, write, log=out.append, settle_s=0)
        return rc, mb, "\n".join(out)

    def cleared(self): b = bytearray(image()); b[0x3F] = 0xFF; return bytes(b)

    def test_restores_the_one_byte_and_verifies(self):
        rc, mb, out = self.go(self.cleared())
        self.assertEqual(rc, 0, out); self.assertEqual(mb.writes(), [(OPCODES["RDAL_OP_WRITE_TRX_EEPROM_UINT8"][0], 0x3F, 0xC5)])
        self.assertEqual(bytes(mb.mem), image()); self.assertIn("equals the backup", out)

    def test_dry_run_writes_nothing(self):
        rc, mb, out = self.go(self.cleared(), write=False)
        self.assertEqual(rc, 0); self.assertEqual(mb.writes(), []); self.assertIn("dry run", out)

    def test_refuses_another_radio(self):
        rc, mb, out = self.go(self.cleared(), serial=0x12345678)
        self.assertEqual(rc, 1); self.assertEqual(mb.writes(), []); self.assertIn("REFUSED", out)

    def test_refuses_any_other_difference(self):
        b = bytearray(self.cleared()); b[0x200] ^= 1
        rc, mb, out = self.go(bytes(b)); self.assertEqual(rc, 1); self.assertEqual(mb.writes(), []); self.assertIn("0x0200", out)

    def test_refuses_an_unexpected_value_at_0x3f(self):
        b = bytearray(image()); b[0x3F] = 0x00
        rc, mb, out = self.go(bytes(b)); self.assertEqual(rc, 1); self.assertEqual(mb.writes(), [])

    def test_nothing_to_do_when_already_restored(self):
        rc, mb, out = self.go(image()); self.assertEqual(rc, 0); self.assertEqual(mb.writes(), []); self.assertIn("nothing to do", out)

    def test_reports_a_failed_write_without_retrying(self):
        rc, mb, out = self.go(self.cleared(), apply_write=False)
        self.assertEqual(rc, 1); self.assertEqual(len(mb.writes()), 1); self.assertIn("CHECK FAILED", out)

    def test_gate_allows_only_this_address_and_value(self):
        mb = FakeMailbox(image(), 0x0A900A04); fx = Flex(mb, allow_write=True, allow_dangerous=False)
        with self.assertRaises(SafetyError): fx.set("RDAL_OP_WRITE_TRX_EEPROM_UINT8", 0x3F, 0xC5)          # not without the permit
        with fx.permit("RDAL_OP_WRITE_TRX_EEPROM_UINT8"):
            for d1, d2 in ((0x3F, 0xFF), (0x3E, 0xC5), (0x40, 0xC5), (0x3F, 0x00)):
                with self.assertRaises(SafetyError): fx.set("RDAL_OP_WRITE_TRX_EEPROM_UINT8", d1, d2)
            with self.assertRaises(SafetyError): fx.set("RDAL_OP_WRITE_TRX_EEPROM_UINT32", 0x3C, 0xC5000000)
        self.assertEqual(mb.writes(), [])
        with self.assertRaises(SafetyError): Flex(mb, allow_write=False).set("RDAL_OP_WRITE_TRX_EEPROM_UINT8", 0x3F, 0xC5)

    @unittest.skipUnless(os.path.exists(BACKUP), "the backup is not in this checkout")
    def test_plan_on_the_real_backup(self):
        b = open(BACKUP, "rb").read(); now = bytearray(b); now[0x3F] = 0xFF
        self.assertEqual(R.plan(b, bytes(now))[0], "write"); self.assertEqual(R.plan(b, b)[0], "done")


if __name__ == "__main__":
    unittest.main()
