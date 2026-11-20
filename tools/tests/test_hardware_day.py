import os, re, struct, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS); sys.path.insert(0, HERE)
import hardware_day as H
from firesidectl import pal
from firesidectl.fake import FakeRadio, DEFAULTS
from firesidectl.opcodes import OPCODES
from firesidectl.transport import RawMidi
from eeprom_image import build_trx
from test_pal import FakeDiceBus

READ_OPS = {v[0] for v in OPCODES.values() if v[1] in ("r", "rw")}
ALL_OPS = {v[0] for v in OPCODES.values()}


class Safety(unittest.TestCase):
    def test_module_has_no_write_path(self):
        src = open(os.path.join(TOOLS, "hardware_day.py")).read()
        code = "\n".join(l for l in src.splitlines() if not l.strip().startswith("#"))
        for forbidden in (r"\.set\(", r"run_startup", r"run_steps", r"shutdown_steps", r"allow_write\s*=\s*True", r"allow_dangerous\s*=\s*True", r"\.permit\("):
            self.assertIsNone(re.search(forbidden, code), forbidden)

    def test_wrong_byte_order_probe_cannot_alias_a_real_opcode(self):
        self.assertEqual(H.swapped_opcode_word(OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]), 0x30040080)
        self.assertNotIn(0x30040080, ALL_OPS)
        for op in ALL_OPS:                                   # true for EVERY opcode, so a wrong-order write sequence is inert too
            self.assertNotIn(H.swapped_opcode_word(op), ALL_OPS, op)


class LittleEndianBus:
    """A radio whose mailbox is little-endian; records every opcode it is asked to run.
    wedge=False: an unknown opcode completes (busy cleared). wedge=True: firmware leaves the busy bit set forever."""
    def __init__(self, wedge=False):
        self.wedge = wedge
        self.mem = {pal.CMD: b"\0" * 4, pal.RESULT: b"\0" * 4, pal.EVENT: b"\0" * 4, pal.DICE_BASE: struct.pack(">I", 10)}
        self.seen = []
    def read(self, off, n): return self.mem[off]
    def write(self, off, data):
        w, d1, d2 = struct.unpack("<III", data)
        code = w & 0x7FFFFFFF; self.seen.append(code)
        if code == OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]:
            self.mem[pal.RESULT] = struct.pack("<I", 0x02010408); self.mem[pal.CMD] = struct.pack("<I", code)
        elif self.wedge:
            self.mem[pal.CMD] = struct.pack("<I", w)         # unknown opcode: busy bit stays set
        else:
            self.mem[pal.CMD] = struct.pack("<I", code)      # unknown opcode: completes with nothing done


class ByteOrder(unittest.TestCase):
    def test_big_endian_radio(self):
        bus = FakeDiceBus(); bus.answers[OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]] = 0x02010408
        order, notes = H.probe_order(bus)
        self.assertEqual(order, "big", notes)

    def test_little_endian_radio_and_the_wrong_first_guess_is_harmless(self):
        bus = LittleEndianBus()
        order, notes = H.probe_order(bus)
        self.assertEqual(order, "little", notes)
        self.assertEqual(bus.seen, [0x30040080, 1200])       # big-endian attempt looked like an unknown opcode, then the real read
        self.assertNotIn(0x30040080, ALL_OPS)

    def test_wedging_firmware_fails_cleanly_without_writing_into_a_busy_mailbox(self):
        bus = LittleEndianBus(wedge=True)
        order, notes = H.probe_order(bus)
        self.assertIsNone(order)
        self.assertEqual(bus.seen, [0x30040080], "second attempt must not write while the mailbox is busy")
        self.assertTrue(any("power-cycle" in n for n in notes), notes)

    def test_neither_order_works(self):
        bus = FakeDiceBus()                                  # firmware answers 0: implausible in both orders
        order, notes = H.probe_order(bus)
        self.assertIsNone(order); self.assertEqual(len(notes), 2)


class Rehearsal(unittest.TestCase):
    def setUp(self):
        self.radio = FakeRadio(); img = build_trx(); self.radio.eeprom["TRX"][:len(img)] = img
        self.bus = FakeDiceBus(); self.bus.mem[pal.DICE_BASE] = struct.pack(">I", 10)
        self.bus.answers = {OPCODES[n][0]: v for n, v in DEFAULTS.items()}
        self.out = tempfile.mkdtemp(); self.bk = tempfile.mkdtemp()
    def tearDown(self):
        self.radio.close()

    def go(self, **kw):
        return H.run(self.out, midi_factory=lambda: RawMidi(self.radio.path), bus_factory=lambda: self.bus,
                     sysfs="/nonexistent", procfs="/nonexistent", devdir="/nonexistent", backups_dir=self.bk, eeprom_prefer="midi", **kw)

    def test_full_run_is_read_only_on_both_transports(self):
        rep = self.go()
        self.assertEqual(self.radio.writes, [], "MIDI: no set messages may be sent")
        for off, blk in self.bus.writes:                     # FireWire: every mailbox write is a GET opcode
            self.assertEqual(off, pal.CMD)
            op = struct.unpack(">III", blk)[0] & 0x7FFFFFFF
            self.assertIn(op, READ_OPS, op)
        self.assertGreater(len(self.bus.writes), 5)
        f = rep.facts
        self.assertEqual((f["bus_order"], f["midi_works"], f["firewire_works"], f["routes_agree"]), ("big", "yes", "yes", "yes"))
        self.assertTrue(f["eeprom_ok"].startswith("yes")); self.assertEqual(f["model"], "FLEX-5000A")

    def test_report_files(self):
        rep = self.go()
        text = open(os.path.join(rep.dir, "report.md")).read()
        self.assertIn("Read-only run: nothing was written", text)
        self.assertIn("Do not transmit before it", text)
        for raw in ("identify-midi.txt", "identify-firewire.txt", "registers.json", "eeprom.txt", "system-check.txt"):
            self.assertTrue(os.path.exists(os.path.join(rep.raw, raw)), raw)
        self.assertEqual(len([n for n in os.listdir(self.bk) if n.endswith(".bin")]), 1)

    def test_events_are_recorded_read_only(self):
        self.bus.mem[pal.EVENT] = struct.pack(">I", 0x01)
        self.radio.note(0x90, 61, 127)
        rep = self.go(events_seconds=0.3)
        ev = open(os.path.join(rep.raw, "events.txt")).read()
        self.assertIn("EVENT 0x00000001 ['dot']", ev); self.assertIn("MIDI (144, 61, 127)", ev)
        self.assertEqual(self.radio.writes, [])

    def test_dice_global_section_is_read_and_decoded(self):
        H.add_dice_global(self.bus.mem, nickname=b"FLEX-5000")
        rep = self.go(skip_eeprom=True)
        self.assertEqual(rep.facts["dice_nickname"], "'FLEX-5000'"); self.assertEqual(rep.facts["dice_owner"], "no owner")
        g = H.read_dice_global(self.bus); self.assertEqual((g["global_offset"], g["sample_rate"]), (40, 48000))
        self.assertTrue(all(off == pal.CMD for off, _ in self.bus.writes), "only mailbox GETs are ever written")

    def test_no_routes_still_reports(self):
        rep = H.run(self.out, midi_factory=None, bus_factory=None, sysfs="/nonexistent", procfs="/nonexistent",
                    devdir="/nonexistent", skip_eeprom=True)
        self.assertTrue(os.path.exists(os.path.join(rep.dir, "report.md")))
        self.assertEqual(rep.facts["firewire_works"], "no node")


if __name__ == "__main__":
    unittest.main()
