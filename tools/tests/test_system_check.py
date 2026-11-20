"""system/fireside-system-check.sh against a fake sysfs: the FireWire chip-maker verdict (TI good, JMicron avoid, unknown noted) and the no-controller message."""
import os, subprocess, tempfile, unittest
SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "system", "fireside-system-check.sh")


def run(vendor=None):
    root = tempfile.mkdtemp()
    for d in ("bus/pci/devices", "bus/firewire/devices"): os.makedirs(os.path.join(root, d))
    if vendor:
        dev = os.path.join(root, "bus/pci/devices/0000:05:00.0"); os.makedirs(dev)
        for name, val in (("class", "0x0c0010"), ("vendor", vendor), ("device", "0x823f")):
            with open(os.path.join(dev, name), "w") as f: f.write(val + "\n")
    env = dict(os.environ, SYSFS=root, PROCFS="/nonexistent", DEVDIR="/nonexistent", MODDIR="/nonexistent", RULE="/nonexistent")
    r = subprocess.run(["bash", SCRIPT], capture_output=True, text=True, env=env)
    return r.stdout


class ChipVerdict(unittest.TestCase):
    def test_ti_is_good(self):
        out = run("0x104c"); self.assertIn("[ OK ] chip maker: Texas Instruments", out)
    def test_via_lsi_ricoh_are_fine(self):
        for v, name in (("0x1106", "VIA"), ("0x11c1", "LSI/Agere"), ("0x1180", "Ricoh")): self.assertIn(f"[ OK ] chip maker: {name}", run(v))
    def test_jmicron_is_a_failure(self):
        out = run("0x197b"); self.assertIn("[FAIL] chip maker: JMicron", out)
    def test_sis_and_unknown_are_noted_not_failed(self):
        out = run("0x1039"); self.assertIn("[ -- ] chip maker: SiS", out); self.assertNotIn("[FAIL] chip maker", out)
        out = run("0xabcd"); self.assertIn("[ -- ] chip maker 0xabcd", out)
    def test_no_controller_says_so(self):
        out = run(None); self.assertIn("[FAIL] no FireWire (IEEE 1394 OHCI) controller", out); self.assertIn("radio's PCIe FireWire card", out)


if __name__ == "__main__":
    unittest.main()
