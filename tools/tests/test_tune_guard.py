"""Static guard for the single tuning path (Opus review conditions): app/core/src/tune.cpp may write exactly one opcode, SET_RX1_FREQ_TW,
and nothing else in the app may call a radio write from the CAT/tuning code."""
import os, re, unittest
APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "app")


def code(path, strip_strings=False):
    with open(os.path.join(APP, path)) as f:
        t = f.read()
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    t = "\n".join(re.sub(r"//.*$", "", l) for l in t.splitlines())
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', t) if strip_strings else t           # string literals are UI text, not calls (but tune.cpp's opcode name IS a literal)


class TuningIsOneOpcode(unittest.TestCase):
    def test_tune_cpp_writes_only_rx1_freq_tw(self):
        c = code("core/src/tune.cpp")
        self.assertEqual(re.findall(r"\.set\(\s*\"([A-Z0-9_]+)\"", c), ["SET_RX1_FREQ_TW"])
        self.assertEqual(len(re.findall(r"\.set\(", c)), 1)                       # no set() with a variable opcode name
        for bad in ("MOX", "PA_BIAS", "INITIALIZE", "STANDBY", "EEPROM", "allow_dangerous", "permit(", "link()"):
            self.assertNotIn(bad, c, bad)
        self.assertIsNone(re.search(r"[.>]request\(", c))                          # never the raw link; only Flex::set with the one opcode

    def test_preamp_cpp_writes_only_set_trx_preamp_with_zero_or_one(self):
        c = code("core/src/preamp.cpp")
        self.assertEqual(re.findall(r"\.set\(\s*\"([A-Z0-9_]+)\"", c), ["SET_TRX_PREAMP"]); self.assertEqual(len(re.findall(r"\.set\(", c)), 1)
        self.assertIn("on ? 1.0 : 0.0, 0.0", c)                                   # the wire value is a bool: nothing but 0 or 1, d2 always 0
        for bad in ("MOX", "PA_BIAS", "INITIALIZE", "STANDBY", "EEPROM", "allow_dangerous", "permit(", "link()"):
            self.assertNotIn(bad, c, bad)
        h = code("core/include/fireside/preamp.h"); self.assertNotIn(".set(", h); self.assertNotIn("allow_dangerous", h)

    def test_rxant_cpp_writes_only_set_rx1_ant_with_one_to_four(self):
        """The antenna switch may write exactly SET_RX1_ANT, d1 = 1..4 (ANT1..ANT3, RX1 IN), d2 = 0, through the single set() in request()."""
        c = code("core/src/rxant.cpp")
        self.assertEqual(re.findall(r"\.set\(\s*\"([A-Z0-9_]+)\"", c), ["SET_RX1_ANT"]); self.assertEqual(len(re.findall(r"\.set\(", c)), 1)
        self.assertIn('fx_.set("SET_RX1_ANT", double(v), 0.0)', c)
        self.assertIn("v < 1 || v > 4", c)                                          # the range check in front of the wire
        for bad in ("MOX", "PA_BIAS", "INITIALIZE", "STANDBY", "EEPROM", "allow_dangerous", "permit(", "link()", "SET_TX_ANT", "SET_ANT", "SET_RX2_ANT", "SET_RX1_FREQ"):
            self.assertNotIn(bad, c, bad)
        h = code("core/include/fireside/rxant.h"); self.assertNotIn(".set(", h); self.assertNotIn("allow_dangerous", h)
        self.assertEqual(re.findall(r"(Ant1|Ant2|Ant3|Rx1In) = (\d)", h), [("Ant1", "1"), ("Ant2", "2"), ("Ant3", "3"), ("Rx1In", "4")])   # no other antenna reachable

    def test_tune_header_exposes_no_generic_write(self):
        c = code("core/include/fireside/tune.h")
        self.assertNotIn(".set(", c); self.assertNotIn("allow_dangerous", c)

    def test_cat_never_talks_to_the_radio_itself(self):
        c = code("core/src/cat.cpp") + code("core/include/fireside/cat.h")
        for bad in ("Flex", "ILink", "PalMailbox", ".set(", "fx_", "link_"):
            self.assertNotIn(bad, c, bad)                                             # CAT only calls back; the Tuner is the sole route to the mailbox

    def test_rigctl_cat_cannot_transmit_or_touch_the_radio(self):
        """The Hamlib CAT server: no radio objects, no PTT call into the app (set_ptt is refused in the protocol engine itself)."""
        c = code("core/src/rigctl.cpp") + code("core/include/fireside/rigctl.h") + code("gui/CatPanel.cpp", True) + code("gui/CatPanel.h", True)
        for bad in ("Flex", "ILink", "PalMailbox", ".set(", "fx_", "link_", "MOX", "TxGate", "TxControl", "key(true", "startSend", "onSend", "setPtt", "set_ptt("):
            self.assertNotIn(bad, c, bad)
        b = code("core/include/fireside/rigctl.h"); backend = b[b.index("struct Backend"):b.index("};", b.index("struct Backend"))]
        self.assertNotRegex(backend, r"ptt|transmit\(|key\(", "Backend must offer no way to transmit")

    def test_gui_reaches_the_radio_only_through_radio_session(self):
        offenders = []
        for d, _, fs in os.walk(os.path.join(APP, "gui")):
            for f in fs:
                if not f.endswith((".cpp", ".h")): continue
                p = os.path.relpath(os.path.join(d, f), APP); c = code(p, strip_strings=True)
                for bad in (r"\brun_startup\b", r"\brun_steps\b", r"\bshutdown_steps\b", r"\bstartup_steps\b", r"\bPalMailbox\b", r"\bTuner\b", r"[.>]set\(", r"allow_dangerous", r"permit\(", r"\bFlex\b"):
                    if p == "gui/main.cpp": continue                                  # smoke tests inspect the simulated bus
                    if bad == r"\bFlex\b" and p == "gui/MainWindow.cpp": continue    # the pre-existing MIDI "Identify" button: a Flex with the default READ-ONLY Safety
                    if re.search(bad, c): offenders.append((p, bad))
        self.assertEqual(offenders, [])

    def test_gui_writes_are_off_by_default(self):
        c = code("gui/RadioPanel.cpp")
        self.assertIn("allow_->setChecked(false)", c)                                  # the Allow-writes box starts unticked
        self.assertIn("QMessageBox::question", c)                                      # and Start asks for confirmation unless a test passes confirmed=true
        self.assertIn("allow_write()", c)

    def test_session_cpp_writes_only_via_reviewed_paths(self):
        c = code("core/src/session.cpp")
        self.assertIsNone(re.search(r"[.>]set\(", c)); self.assertNotIn("allow_dangerous", c); self.assertNotIn("permit(", c)
        for needed in ("run_startup", "run_steps(*fx_, shutdown_steps()", "tuner->arm"):
            self.assertIn(needed, c)

    def test_fw_iso_writes_only_the_four_dice_stream_registers(self):
        """The direct FireWire stream (96/192 kHz) may write exactly CLOCK_SELECT, TX[0] ISOCHRONOUS, RX[0] ISOCHRONOUS (duplex: where the
        radio listens for our transmit stream, analysis/duplex-iso-tx.md) and ENABLE, through one helper, and never send a PAL/RDAL radio
        opcode (it can never key: MOX is only ever sent by the TxGate)."""
        c = code("core/src/fw_iso.cpp")
        self.assertEqual(len(re.findall(r"\bbus_\.write\(", c)), 1)                       # the single write, inside wq()
        self.assertRegex(c, r"void DiceStreamControl::wq\(uint64_t a, uint32_t v\) \{ bus_\.write\(a, be4\(v\)\); \}")
        calls = re.findall(r"\bwq\(([^;]*?),", c)
        self.assertTrue(calls)
        for target in calls:
            if target.strip() == "uint64_t a": continue                                    # the definition itself
            self.assertRegex(target, r"kDiceGlobalClockSelect|kDiceTxIso|kDiceRxIso|kDiceGlobalEnable", f"unexpected DICE write target: {target}")
        for banned in ("kPalCmd", "RDAL_OP", "SET_", "->set(", ".set(", "PalMailbox", "ioctl(fd_, FW_CDEV_IOC_SEND_REQUEST"):
            self.assertNotIn(banned, c)
        for d, _, fs in os.walk(os.path.join(APP, "core")):                                  # nothing else names the stream registers
            for f in fs:
                p = os.path.relpath(os.path.join(d, f), APP)
                if p in ("core/src/fw_iso.cpp", "core/include/fireside/fw_iso.h") or not f.endswith((".cpp", ".h")): continue
                self.assertNotRegex(code(p), r"kDiceGlobal(ClockSelect|Enable)|kDiceTxIso|kDiceRxIso", p)

    def test_codec_writes_only_the_mic_gain_register(self):
        """The audio codec module may write exactly WRITE_CODEC_REG to the mic gain register (0x16), through one set(); the safety gate passes
        only 0x00 / 0x80..0xFF for it. Nothing else in the code base writes the codec (analysis/voice-tx.md)."""
        c = code("core/src/codec.cpp")
        self.assertEqual(len(re.findall(r"\.set\(", c)), 1); self.assertIn("fx.set(n, kMicGainReg, v)", c)
        self.assertIn("kMicGainReg = 0x16", code("core/include/fireside/codec.h"))
        for d, _, fs in os.walk(APP):
            for f in fs:
                p = os.path.relpath(os.path.join(d, f), APP)
                if not f.endswith((".cpp", ".h")) or p in ("core/src/codec.cpp", "core/src/safety.cpp", "core/include/fireside/opcodes_gen.h") or "tests/" in p or "third_party" in p: continue
                self.assertNotIn('"RDAL_OP_WRITE_CODEC_REG"', code(p), p)

    def test_atu_cpp_writes_only_atu_send_cmd_through_one_send(self):
        """The ATU module may write exactly ATU_SEND_CMD, through the single set() in Atu::send; which requests pass is the safety gate's
        reviewed list (relays, thresholds, reports), tunes only with the TxGate's grant. It never keys, never touches the EEPROM."""
        c = code("core/src/atu.cpp")
        self.assertEqual(re.findall(r"\.set\(\s*([A-Za-z_]+)\s*,", c), ["n"]); self.assertEqual(len(re.findall(r"\.set\(", c)), 1)
        self.assertIn('const std::string n = "RDAL_OP_ATU_SEND_CMD"', c)
        for bad in ("MOX", "PA_BIAS", "INITIALIZE", "STANDBY\"", "EEPROM", "allow_dangerous", "set_tx_grant", "link()"):
            self.assertNotIn(bad, c, bad)
        self.assertIsNone(re.search(r"[.>]request\(", c))                          # never the raw link

    def test_tx_gate_cpp_writes_only_set_mox_and_tx_dsp_mode(self):
        """The transmit gate is the only code that keys: SET_MOX 1 / 0 (and the TX DSP mode around an ATU tune, as PowerSDR); it alone sets the
        transmit grant. Nothing else in the app may name SET_MOX with a 1, or set the grant."""
        c = code("core/src/tx_gate.cpp")
        names = re.findall(r"\.set\(\s*(\"[A-Z0-9_]+\"|kMox)\s*,", c)
        self.assertEqual(sorted(set(names)), ['"RDAL_OP_SET_TX_DSP_MODE"', "kMox"]); self.assertEqual(len(re.findall(r"\.set\(", c)), len(names))
        self.assertIn('const char* kMox = "RDAL_OP_SET_MOX"', c)
        for bad in ("EEPROM", "PA_BIAS", "INITIALIZE", "allow_dangerous", "link()"):
            self.assertNotIn(bad, c, bad)
        self.assertIsNone(re.search(r"[.>]request\(", c))
        for d, _, fs in os.walk(APP):
            for f in fs:
                p = os.path.relpath(os.path.join(d, f), APP)
                if not f.endswith((".cpp", ".h")) or p.startswith("tests/") or p in ("core/src/tx_gate.cpp", "core/include/fireside/safety.h"): continue
                self.assertNotIn("set_tx_grant", code(p), p)

    def test_tx_setup_cpp_writes_only_the_eeprom_calibrated_pots(self):
        """tx_setup.cpp writes exactly PA_POT_SET_RDAC (8 bias pots) and SET_TRX_POT (carrier null); the safety gate only lets the radio's own EEPROM values
        through, and only tx_setup.cpp registers them."""
        c = code("core/src/tx_setup.cpp")
        self.assertEqual(len(re.findall(r"\.set\(", c)), 2)
        self.assertIn('const std::string n = "RDAL_OP_PA_POT_SET_RDAC"', c); self.assertIn('const std::string n = "RDAL_OP_SET_TRX_POT"', c)
        for bad in ("MOX", "EEPROM_UINT", "WRITE_TRX", "allow_dangerous", "set_tx_grant", "link()"):
            self.assertNotIn(bad, c, bad)
        for d, _, fs in os.walk(APP):
            for f in fs:
                p = os.path.relpath(os.path.join(d, f), APP)
                if not f.endswith((".cpp", ".h")) or p.startswith("tests/") or p in ("core/src/tx_setup.cpp", "core/include/fireside/safety.h"): continue
                self.assertNotIn("set_tx_calibration", code(p), p)

    def test_firesidekey_writes_only_tx_routing_and_keys_only_through_the_gate(self):
        """The stage 3 tool may set the TX antenna, TX DSP mode, TX filter / offset and TX DDS frequency itself; everything else goes through reviewed paths (run_startup,
        run_steps(shutdown), tx_setup, Atu, TxGate). It never sends SET_MOX, EEPROM, bias or ATU commands directly."""
        c = code("tools/firesidekey.cpp")
        self.assertLessEqual(set(re.findall(r"fx\.set\(\s*\"([A-Z0-9_]+)\"", c)), {"RDAL_OP_SET_TX_ANT", "RDAL_OP_SET_TX_DSP_FILTER", "RDAL_OP_SET_TX_DSP_MODE", "RDAL_OP_SET_TX_FREQ_TW", "RDAL_OP_SET_TX_OFFSET"})
        self.assertEqual(len(re.findall(r"\.set\(", c)), len(re.findall(r"fx\.set\(\s*\"RDAL_OP_SET_TX_(ANT|DSP_MODE|FREQ_TW|DSP_FILTER|OFFSET)\"", c)))   # every set() is one of the three
        for bad in ("SET_MOX", "PA_POT_SET", "SET_TRX_POT", "ATU_SEND", "WRITE_TRX", "allow_dangerous", "set_tx_grant", "set_tx_calibration", "permit("):
            self.assertNotIn(bad, c, bad)
        self.assertIsNone(re.search(r"[.>]request\(", c))

    def test_tx_control_cpp_writes_only_tx_routing_and_keys_only_through_the_gate(self):
        """The shared transmit controller (GUI + firesidekey) sets only the TX antenna / mode / DDS / filter / offset itself; bias and carrier null go through
        tx_setup, ATU commands through Atu, keying through TxGate / run_atu_tune. It never sends SET_MOX, EEPROM writes or pot writes directly."""
        c = code("core/src/tx_control.cpp")
        self.assertEqual(sorted(set(re.findall(r"fx_\.set\(\s*\"([A-Z0-9_]+)\"", c))), ["RDAL_OP_SET_TX_ANT", "RDAL_OP_SET_TX_DSP_FILTER", "RDAL_OP_SET_TX_DSP_MODE", "RDAL_OP_SET_TX_FREQ_TW", "RDAL_OP_SET_TX_OFFSET"])
        self.assertEqual(len(re.findall(r"\.set\(", c)), 5)
        for bad in ("SET_MOX", "PA_POT_SET", "SET_TRX_POT", "ATU_SEND", "WRITE_TRX", "allow_dangerous", "set_tx_grant", "set_tx_calibration", "permit("):
            self.assertNotIn(bad, c, bad)
        self.assertIsNone(re.search(r"[.>]request\(", c))

    def test_only_tune_cpp_and_startup_cpp_write(self):
        offenders = []
        for root in ("core/src", "gui", "tools"):
            for d, _, fs in os.walk(os.path.join(APP, root)):
                for f in fs:
                    if not f.endswith((".cpp", ".h")): continue
                    p = os.path.relpath(os.path.join(d, f), APP)
                    if p in ("core/src/tune.cpp", "core/src/startup.cpp", "core/src/client.cpp", "core/src/preamp.cpp", "core/src/rxant.cpp",
                             "core/src/atu.cpp", "core/src/tx_gate.cpp", "core/src/tx_setup.cpp", "tools/firesidekey.cpp", "core/src/tx_control.cpp", "core/src/codec.cpp"): continue   # each has its own test below
                    if re.search(r"\b(fx|flex|client)\w*(\.|->)set\(", code(p)): offenders.append(p)
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
