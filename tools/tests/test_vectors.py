"""Checks protocol/test-vectors.json against INDEPENDENT implementations of the rules written in protocol/*.md.
If the documentation is enough to implement the protocol, these pass; app/tests/test_vectors.cpp checks the same file's numbers against the C++ code."""
import json, math, os, struct, sys, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
VEC = json.load(open(os.path.join(TOOLS, "..", "protocol", "test-vectors.json")))


def quads(*v): return b"".join(struct.pack(">I", x & 0xFFFFFFFF) for x in v)
def tuning_word(mhz, clock=500.0): return int(0xFFFFFFFF * mhz / clock)
def serial_text(raw): return f"{raw & 0xFF:02d}{(raw >> 8) & 0xFF:02d}-{(raw >> 16) & 0xFFFF:04d}"
def fw_text(v): return ".".join(str((v >> s) & 0xFF) for s in (24, 16, 8, 0))
def xor(b):
    x = 0
    for c in b: x ^= c
    return x
def bias_pots(row): return bytes(b if i % 2 == 0 else 255 - b for i, b in enumerate(row))
def sysex(msg_id, opcode, d1, d2):
    payload = struct.pack("<HHIII", msg_id, 0, opcode, d1, d2)
    return b"\xF0\x00\x00\x41" + payload.hex().upper().encode() + b"\xF7"
def cip(sid, dbs, dbc, fdf, syt, fmt=0x10):
    return struct.pack(">II", sid << 24 | dbs << 16 | dbc, 0x80000000 | fmt << 24 | fdf << 16 | syt)
def audio_quadlet(s32): return 0x40000000 | ((s32 & 0xFFFFFFFF) >> 8)
def sample_from_quadlet(q):
    v = q & 0xFFFFFF
    return v - (1 << 24) if v & 0x800000 else v
def atu_message(raw):
    code, b2, b3, queue = raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF, raw >> 24
    if code == 0xFF and b2 == 0xFF and b3 == 0xFF: return None
    return dict(queue=queue, code=code, b2=b2, b3=b3)
def pa_watts(bridge, adc):
    if adc < 2: return 0.0
    ref = [1.0, 2.0, 5.0, 10.0, 20.0, 90.0]
    v = adc / 4096 * 2.5; v2 = v * v
    for i in range(6):
        if v < bridge[i]:
            vl, pl = (bridge[i - 1], ref[i - 1]) if i else (0.0, 0.0)
            return pl + (ref[i] - pl) * (v2 - vl * vl) / (bridge[i] ** 2 - vl * vl)
    raise NotImplementedError("above the 90 W point")
def pa_swr(fwd, rev):
    if rev >= fwd: return 25.0
    q = math.sqrt(rev / fwd); return min(25.0, (1 + q) / (1 - q))


class Vectors(unittest.TestCase):
    def h(self, s): return bytes.fromhex(s)

    def test_mailbox(self):
        for k in ("mailbox_read_firmware", "mailbox_write_standby"):
            v = VEC[k]; self.assertEqual(quads(v["opcode"] | 0x80000000, v["data1"], v["data2"]), self.h(v["cmd_block"]))
        v = VEC["mailbox_read_firmware"]; self.assertEqual(fw_text(int(v["result"], 16)), v["firmware"])

    def test_sysex(self):
        for k in ("sysex_read_firmware", "sysex_standby"):
            v = VEC[k]; f = sysex(v["msg_id"], v["opcode"], v["data1"], v["data2"])
            self.assertEqual(f, self.h(v["frame"])); self.assertEqual(len(f), 37)

    def test_start_up_numbers(self):
        v = VEC["tuning_word_14_074"]
        self.assertEqual(f"{tuning_word(v['mhz']):08X}", v["tuning_word"]); self.assertEqual(struct.pack(">f", v["mhz"]).hex().upper(), v["f32_bits"])
        v = VEC["serial_decode"]; self.assertEqual(serial_text(int(v["raw"], 16)), v["text"])
        v = VEC["eeprom_xor"]; self.assertEqual(f"{xor(self.h(v['bytes'])):02X}", v["checksum"])
        v = VEC["bias_pots"]; self.assertEqual(bias_pots(self.h(v["eeprom_row0"])), self.h(v["pots"]))

    def test_cip_and_audio(self):
        v = VEC["cip_packets_48k"]; sid, fdf, syt = v["sid"], int(v["fdf"], 16), int(v["syt"], 16)
        got = [cip(sid, 9, 0x00, fdf, syt), cip(sid, 9, 0x08, fdf, syt), cip(sid, 9, 0x10, 0xFF, 0xFFFF)]
        self.assertEqual([g.hex().upper() for g in got], v["headers"])
        self.assertEqual(f"{audio_quadlet(int(v['first_block_sample_ch0_s32'], 16)):08X}", v["first_audio_quadlet"])
        for c in VEC["audio_quadlet"]["cases"]: self.assertEqual(f"{audio_quadlet(int(c['s32'], 16)):08X}", c["quadlet"])
        self.assertEqual(sample_from_quadlet(0x40FFFFFF), -1); self.assertEqual(sample_from_quadlet(0x40800000), -(1 << 23))

    def test_dual_wire(self):
        v = VEC["dual_wire_block_192k"]; q = [int(x, 16) for x in v["quadlets"]]
        self.assertEqual(len(q), v["dbs"]); s = [sample_from_quadlet(x) for x in q[:16]]
        self.assertEqual((s[0], s[1]), (v["ch0_at_t"], v["ch0_at_t_plus_1"])); self.assertEqual((s[2], s[3]), (v["ch1_at_t"], v["ch1_at_t_plus_1"]))
        self.assertEqual(q[16], 0x80000000)

    def test_atu(self):
        for c in VEC["atu_result"]["cases"]:
            m = atu_message(int(c["raw"], 16))
            if not c["pending"]: self.assertIsNone(m); continue
            self.assertEqual((m["queue"], m["code"], m["b2"], m["b3"]), (c["queue"], c["code"], c["b2"], c["b3"]))
        m = atu_message(0x00A60205); self.assertAlmostEqual(((m["b2"] << 8) + m["b3"]) * 0.01, 6.78, places=6)

    def test_pa_meter(self):
        v = VEC["pa_meter"]; fwd = pa_watts(v["bridge_volts_at_1_2_5_10_20_90W"], v["adc_forward"])
        rev = pa_watts(v["bridge_volts_at_1_2_5_10_20_90W"], v["adc_reflected"]) * v["reverse_factor"]
        self.assertAlmostEqual(fwd, v["forward_w"], places=4); self.assertAlmostEqual(rev, v["reflected_w"], places=4)
        self.assertAlmostEqual(pa_swr(fwd, rev), v["swr"], places=4)


if __name__ == "__main__":
    unittest.main()
