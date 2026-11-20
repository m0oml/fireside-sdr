"""Static guard: the live-capture code (alsa_pcm.cpp, capture.cpp, firesidecapture, the GUI's live path) may only open an ALSA CAPTURE stream.
No playback, mixer/control writes, rawmidi, or radio writes. Audio OUT to the radio's jacks lives in exactly one file (alsa_play.cpp), checked below."""
import os, re, unittest
APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "app")
FILES = ["core/src/alsa_pcm.cpp", "core/src/capture.cpp", "core/include/fireside/capture.h", "tools/firesidecapture.cpp"]
FORBIDDEN = [r"SND_PCM_STREAM_PLAYBACK", r"snd_pcm_writei", r"snd_pcm_writen", r"snd_pcm_mmap_writei", r"snd_mixer", r"snd_ctl_", r"snd_hctl",
             r"snd_rawmidi", r"snd_seq", r"allow_write", r"allow_dangerous", r"\.set\(", r"run_startup", r"run_steps", r"PalMailbox", r"/dev/fw", r"\bFlex\b"]


def code(path):
    with open(os.path.join(APP, path)) as f:
        t = f.read()
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", l) for l in t.splitlines())     # comments may mention these words


class CaptureIsReadOnly(unittest.TestCase):
    def test_no_forbidden_apis(self):
        for f in FILES:
            c = code(f)
            for pat in FORBIDDEN:
                self.assertIsNone(re.search(pat, c), f"{f}: {pat}")

    def test_only_capture_direction_is_opened(self):
        c = code("core/src/alsa_pcm.cpp")
        self.assertEqual(len(re.findall(r"snd_pcm_open\(", c)), 2)           # open + probe
        self.assertEqual(len(re.findall(r"SND_PCM_STREAM_CAPTURE", c)), 2)   # both capture
        self.assertNotIn("STREAM_PLAYBACK", c)

    def test_playback_is_opened_in_exactly_one_file(self):
        hits = []
        for root, _, names in os.walk(APP):
            if any(x in root for x in ("third_party", "build")): continue
            for n in names:
                if n.endswith((".cpp", ".h")):
                    rel = os.path.relpath(os.path.join(root, n), APP)
                    if "STREAM_PLAYBACK" in code(rel): hits.append(rel)
        self.assertEqual(hits, ["core/src/alsa_play.cpp"])
        c = code("core/src/alsa_play.cpp")
        self.assertEqual(len(re.findall(r"snd_pcm_open\(", c)), 1)
        for pat in (r"snd_mixer", r"snd_ctl_", r"snd_hctl", r"snd_rawmidi", r"PalMailbox", r"/dev/fw", r"allow_write", r"run_startup", r"\.set\("):
            self.assertIsNone(re.search(pat, c), pat)

    def test_gui_live_path_uses_only_capture_helpers(self):
        c = code("gui/ReceiverPanel.cpp")
        self.assertIn("open_alsa_pcm", c)
        for pat in (r"snd_", r"allow_write", r"\bFlex\b", r"PalMailbox"):
            self.assertIsNone(re.search(pat, c), pat)


if __name__ == "__main__":
    unittest.main()
