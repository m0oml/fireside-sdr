"""Drift guard: the checked-in C++ opcode header and conformance vectors must equal what the Python source generates now."""
import filecmp, os, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); TOOLS = os.path.dirname(HERE); APP = os.path.join(TOOLS, "..", "app")


@unittest.skipUnless(os.path.isdir(APP), "app/ not present")
class Drift(unittest.TestCase):
    def test_generated_assets_are_current(self):
        tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(tmp, "core/include/fireside")); os.makedirs(os.path.join(tmp, "tests/vectors"))
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_cpp_assets.py"), tmp], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        stale = []
        for rel in ["core/include/fireside/opcodes_gen.h"] + [f"tests/vectors/{n}" for n in sorted(os.listdir(os.path.join(tmp, "tests/vectors")))]:
            if not filecmp.cmp(os.path.join(tmp, rel), os.path.join(APP, rel), shallow=False):
                stale.append(rel)
        self.assertEqual(stale, [], "regenerate with: python3 -I tools/gen_cpp_assets.py app")


if __name__ == "__main__":
    unittest.main()
