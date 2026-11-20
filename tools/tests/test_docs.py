import os, re, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
from firesidectl.opcodes import OPCODES
from opcode_notes import NOTES


class Notes(unittest.TestCase):
    def test_every_note_names_a_real_opcode(self):
        short = {re.sub(r"^(RDAL_OP_|FWC_OP_)", "", n) for n in OPCODES}
        self.assertEqual(sorted(k for k in NOTES if k not in short), [])

    def test_dangerous_things_are_labelled(self):
        for k in ("WRITE_TRX_EEPROM_UINT32", "SET_PA_BIAS", "SET_MOX"):
            self.assertRegex(NOTES[k].upper(), r"BLOCK|NEVER|DANGEROUS|CALIBRATION")


class Generated(unittest.TestCase):
    def test_protocol_opcode_page_is_up_to_date(self):
        import gen_protocol_opcodes
        page = os.path.join(TOOLS, "..", "protocol", "04-opcodes.md")
        with open(page) as f:
            self.assertEqual(f.read(), gen_protocol_opcodes.build(), "regenerate: python3 -I tools/gen_protocol_opcodes.py")
        for name, text in (("opcodes.json", gen_protocol_opcodes.build_json()), ("opcodes.csv", gen_protocol_opcodes.build_csv())):
            with open(os.path.join(TOOLS, "..", "protocol", name)) as f:
                self.assertEqual(f.read(), text, "regenerate: python3 -I tools/gen_protocol_opcodes.py")

    def test_every_opcode_is_listed_once(self):
        import gen_protocol_opcodes
        text = gen_protocol_opcodes.build()
        rows = [int(m.group(1)) for m in re.finditer(r"^\| (\d+) \|", text, re.M)]
        bare = [int(n) for n in re.findall(r"\((\d+)\)", text.split("## Opcodes with no documented use")[1])]
        self.assertEqual(sorted(rows + bare), sorted(v[0] for v in OPCODES.values()))

    def test_dangerous_opcodes_are_not_called_safe(self):
        text = gen_protocol_opcodes_text()
        for name in ("SET_MOX", "SET_PA_BIAS", "ATU_SEND_CMD", "WRITE_CODEC_REG"):
            row = next(l for l in text.splitlines() if f"`{name}`" in l)
            self.assertIn("dangerous", row)
        row = next(l for l in text.splitlines() if "`WRITE_TRX_EEPROM_UINT32`" in l)
        self.assertIn("never", row)


def gen_protocol_opcodes_text():
    import gen_protocol_opcodes
    return gen_protocol_opcodes.build()


if __name__ == "__main__":
    unittest.main()
