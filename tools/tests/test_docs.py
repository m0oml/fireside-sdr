import os, re, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
from firesidectl.opcodes import OPCODES
from opcode_notes import NOTES
SRC = os.path.join(TOOLS, "..", "upstream", "powersdr-2.7.2", "Source")


class Notes(unittest.TestCase):
    def test_every_note_names_a_real_opcode(self):
        short = {re.sub(r"^(RDAL_OP_|FWC_OP_)", "", n) for n in OPCODES}
        self.assertEqual(sorted(k for k in NOTES if k not in short), [])

    def test_dangerous_things_are_labelled(self):
        for k in ("WRITE_TRX_EEPROM_UINT32", "SET_PA_BIAS", "SET_MOX"):
            self.assertRegex(NOTES[k].upper(), r"BLOCK|NEVER|DANGEROUS|CALIBRATION")


@unittest.skipUnless(os.path.isdir(SRC), "PowerSDR source tree not present")
class Generated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = os.path.join(tempfile.mkdtemp(), "ref.md")
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_opcode_docs.py"), SRC, cls.out],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        with open(cls.out) as f:
            cls.text = f.read()
        cls.rows = [l for l in cls.text.splitlines() if re.match(r"^\| \d+ \|", l)]

    def test_all_opcodes_once_and_well_formed(self):
        nums = [int(re.match(r"^\| (\d+) ", l).group(1)) for l in self.rows]
        self.assertEqual(sorted(nums), sorted(v[0] for v in OPCODES.values()))
        for l in self.rows:
            self.assertEqual(len(re.split(r"(?<!\\)\|", l)), 9, l[:80])     # 7 columns, escaped pipes allowed

    def test_key_rows_carry_the_researched_facts(self):
        row = lambda n: next(l for l in self.rows if l.startswith(f"| {n} "))
        self.assertIn("0xFFFFFFFF * f_MHz / 500.0", row(1347))
        self.assertIn("SetRX1FreqTW(uint tw, float freq)", row(1347))
        self.assertIn("WriteCodecReg", row(1243)); self.assertIn("0x16 mic", row(1243))
        self.assertIn("(cmd<<8)\\|arg1", row(1294))
        self.assertIn("BLOCKED", row(1227))                                   # WRITE_TRX_EEPROM_UINT32

    def test_every_called_opcode_has_a_wrapper_column(self):
        called = [OPCODES[n][0] for n in OPCODES if OPCODES[n][1] != "?"]
        for v in called:
            self.assertNotIn("(not called by PowerSDR", next(l for l in self.rows if l.startswith(f"| {v} ")))


if __name__ == "__main__":
    unittest.main()
