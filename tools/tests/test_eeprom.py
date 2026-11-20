import datetime, os, stat, struct, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))
from firesidectl import eeprom as E, startup as S
from firesidectl.client import Flex, MidiLink
from firesidectl.fake import FakeRadio
from firesidectl.transport import RawMidi
from eeprom_image import build_trx


def rb(path, mode="rb"):
    with open(path, mode) as f:
        return f.read()


class Decode(unittest.TestCase):
    def test_valid_image_is_clean(self):
        d = E.decode(build_trx())
        self.assertEqual(d["problems"], [])
        h = d["header"]
        self.assertEqual(h["serial"], "0301-1234"); self.assertTrue(h["checksums_present"])
        self.assertEqual(h["image_version"], 3)
        unix = (132_000_000_000_000_000 - 116_444_736_000_000_000) / 1e7      # independent FILETIME -> Unix epoch
        self.assertEqual(h["last_cal"][:19], datetime.datetime.fromtimestamp(unix, datetime.timezone.utc).isoformat()[:19])
        self.assertEqual(h["last_cal"][:10], "2019-04-17")
        self.assertEqual(len(d["tables"]), 9)
        self.assertTrue(all(t["checksum_ok"] and t["pointer_ok"] for t in d["tables"]))
        rx = d["tables"][0]["values"]
        self.assertEqual((rx[0][0], rx[0][1], rx[3][2]), (-60.0, -7.0, -32.25))
        self.assertEqual([c["freq_mhz"] for c in d["tx_carrier"]["entries"]], [7.05, 14.1])
        self.assertTrue(d["tx_carrier"]["terminated"])
        self.assertEqual([u["name"] for u in d["unverified"]], ["PA bias", "VU level", "VU power"])

    def test_corrupt_byte_detected_by_checksum(self):
        img = bytearray(build_trx()); img[0x2E0 + 5] ^= 0x10                # inside PA bridge table
        d = E.decode(bytes(img))
        bad = [t for t in d["tables"] if not t["checksum_ok"]]
        self.assertEqual([t["name"] for t in bad], ["PA bridge"])
        self.assertTrue(any("PA bridge: checksum" in p for p in d["problems"]))

    def test_bad_pointer_nan_and_range(self):
        img = bytearray(build_trx())
        struct.pack_into("<H", img, 0x5A, 0x0999)                            # PA SWR pointer wrong
        struct.pack_into("<f", img, 0x1C0, float("nan"))                     # RX image phase NaN
        struct.pack_into("<f", img, 0x3F0, 7.5)                              # PA power > 1.0
        probs = " | ".join(E.decode(bytes(img))["problems"])
        for frag in ("PA SWR: pointer", "RX image phase: contains NaN", "PA power: value outside"):
            self.assertIn(frag, probs)

    def test_blank_and_short_and_uncalibrated(self):
        self.assertIn("blank", E.decode(b"\xff" * E.TRX_LEN)["problems"][0])
        self.assertIn("too short", E.decode(b"\0" * 64)["problems"][0])
        img = bytearray(build_trx()); img[0x3F] = 0x00
        self.assertTrue(any("never calibrated" in p for p in E.decode(bytes(img))["problems"]))

    def test_rx2_decodes_only_its_tables(self):
        d = E.decode(build_trx(length=E.RX2_LEN), "RX2")
        self.assertEqual([t["name"] for t in d["tables"]], ["RX level", "RX image gain", "RX image phase"])
        self.assertEqual(d["problems"], []); self.assertEqual(d["unverified"], [])

    def test_garbage_never_raises(self):
        import random
        rnd = random.Random(1)
        for _ in range(50):
            E.decode(bytes(rnd.randrange(256) for _ in range(E.TRX_LEN)))
        self.assertIn("serial", E.report(E.decode(build_trx())))

    def test_powersdr_csv_format_and_roundtrip(self):
        img = build_trx()
        t = E.csv_text(img); lines = t.splitlines()
        self.assertEqual(lines[0], ",0,1,2,3,4,5,6,7,8,9,A,B,C,D,E,F,")
        self.assertEqual(len(lines), 1 + 0xC1)
        self.assertEqual(lines[1], "0," + "FF," * 16)                         # bytes 0x00-0x0F are blank
        # row 1 = bytes 0x10..0x1F; serial 0x04D20103 sits at 0x18 little-endian; hex has NO leading zeros
        self.assertEqual(lines[2], "1," + "FF," * 8 + "3,1,D2,4," + "FF," * 4)
        self.assertEqual(E.parse_csv(t), img)


class WithRadio(unittest.TestCase):
    def setUp(self):
        self.img = build_trx()
        self.radio = FakeRadio(); self.radio.eeprom["TRX"][:len(self.img)] = self.img
        self.t = RawMidi(self.radio.path); self.fx = Flex(MidiLink(self.t))      # NOTE: no write permission at all
    def tearDown(self):
        self.t.close(); self.radio.close()

    def test_read_image_is_exact_little_endian(self):
        self.assertEqual(E.read_image(self.fx, "TRX"), self.img)

    def test_two_passes_and_zero_writes(self):
        self.assertEqual(E.read_verified(self.fx, "TRX", passes=2), self.img)
        self.assertEqual(self.radio.writes, [])
        self.assertEqual(len(self.radio.gets), 2 * E.TRX_LEN // 4)
        self.assertTrue(all(g[0] == "RDAL_OP_READ_TRX_EEPROM_UINT32" for g in self.radio.gets))

    def test_flaky_link_between_passes_is_caught(self):
        n = [0]; real = self.radio.eeprom["TRX"]
        orig = self.radio._handle
        def flaky(frame):
            n[0] += 1
            if n[0] == E.TRX_LEN // 4 + 10:                    # inside pass 2: flip a bit once
                real[0x100] ^= 0x01
            orig(frame)
        self.radio._handle = flaky
        with self.assertRaisesRegex(E.EepromReadError, "changed between pass 1 and pass 2"):
            E.read_verified(self.fx, "TRX", passes=2)

    def test_timeout_is_retried_then_gives_up(self):
        self.radio.drop_ids = {0}                               # first request unanswered, retry succeeds
        self.assertEqual(E.read_image(self.fx, "TRX", length=16, timeout=0.2), self.img[:16])
        self.radio.drop_ids = set(range(0, 100))
        with self.assertRaisesRegex(E.EepromReadError, "no reply reading 0x0000"):
            E.read_image(self.fx, "TRX", length=16, timeout=0.05)

    def test_dump_to_file_end_to_end(self):
        out = tempfile.mkdtemp()
        logs = []
        rc = E.dump_to_file(self.fx, out, include_rx2=False, log=logs.append)
        self.assertEqual(rc, 0)
        names = sorted(os.listdir(out))
        self.assertEqual(len(names), 3, names)
        b = [n for n in names if n.endswith("_trx.bin")][0]
        self.assertTrue(b.startswith("F5K_0301-1234_"))
        self.assertEqual(rb(os.path.join(out, b)), self.img)
        self.assertEqual(E.parse_csv(rb(os.path.join(out, "F5K_0301-1234 backup.csv"), "r")), self.img)
        for n in names:
            self.assertFalse(os.stat(os.path.join(out, n)).st_mode & stat.S_IWUSR, f"{n} should be read-only")
        self.assertEqual(self.radio.writes, [], "a backup must never write to the radio")
        self.assertTrue(any("saved" in l for l in logs))

    def test_corrupt_eeprom_is_saved_but_flagged(self):
        self.radio.eeprom["TRX"][0x2E5] ^= 0x10
        out = tempfile.mkdtemp()
        rc = E.dump_to_file(self.fx, out, include_rx2=False, log=lambda *_: None)
        self.assertEqual(rc, 6); self.assertEqual(len(os.listdir(out)), 3)     # still kept: it is what the radio holds

    def test_never_overwrites(self):
        out = tempfile.mkdtemp()
        E.dump_to_file(self.fx, out, include_rx2=False, log=lambda *_: None)
        before = {n: rb(os.path.join(out, n)) for n in os.listdir(out)}
        img_path = [n for n in before if n.endswith("_trx.bin")][0]
        with self.assertRaises(FileExistsError): E._write_new(os.path.join(out, img_path), b"x")
        self.assertEqual(rb(os.path.join(out, img_path)), before[img_path])

    def test_rx2_included_when_fitted(self):
        self.radio.answers["RDAL_OP_GET_RX2_OK"] = 1
        self.radio.eeprom["RX2"][:E.RX2_LEN] = build_trx(length=E.RX2_LEN, which="RX2")
        out = tempfile.mkdtemp()
        self.assertEqual(E.dump_to_file(self.fx, out, log=lambda *_: None), 0)
        self.assertTrue(any(n.endswith("_rx2.bin") for n in os.listdir(out)))


if __name__ == "__main__":
    unittest.main()
