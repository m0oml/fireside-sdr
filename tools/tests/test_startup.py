import os, struct, sys, unittest
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from firesidectl import startup as S, protocol as P
from firesidectl.client import Flex, SafetyError
from firesidectl.fake import FakeRadio
from firesidectl.transport import RawMidi
from firesidectl.client import MidiLink
import time


class Pure(unittest.TestCase):
    def test_formatting(self):
        self.assertEqual(S.ver_str(0x02010408), "2.1.4.8")
        self.assertEqual(S.serial_str(0x04D20103), "0301-1234")      # Misc.SerialToString

    def test_tuning_word_matches_powersdr_formula(self):
        for f in (0.1, 1.8, 7.1, 14.2345, 29.7, 50.313, 54.0):
            self.assertEqual(S.freq_to_tw(f), int(0xFFFFFFFF * f / 500.0), f)
        self.assertEqual(S.freq_to_tw(7.1), 0x03A29C77)
        for bad in (0, -1, 65.01, 500):
            with self.assertRaises(ValueError): S.freq_to_tw(bad)

    def test_dds_sits_9_khz_below_the_dial(self):
        self.assertAlmostEqual(S.dds_for_dial(7.1), 7.091, places=9)
        self.assertEqual(S.freq_to_tw(S.dds_for_dial(7.1)), S.freq_to_tw(7.091))
        self.assertEqual(S.startup_steps(S.dds_for_dial(7.1))[4][1], S.freq_to_tw(7.091))
        for bad in (0.005, 0.009, 65.5, -1.0):                      # the DDS would be <= 0 or the dial above 65 MHz
            with self.assertRaises(ValueError): S.dds_for_dial(bad)

    def test_step_order_and_contents(self):
        st = S.startup_steps(14.2, "ANT2")
        self.assertEqual([s[0] for s in st],
                         ["INITIALIZE", "SET_TRX_PREAMP", "SET_STANDBY", "SET_RX1_ANT", "SET_RX1_FREQ_TW"])
        self.assertEqual(st[2][1], 0)                    # standby False
        self.assertEqual(st[3][1], 2)                    # ANT2
        self.assertEqual(st[4][1], S.freq_to_tw(14.2)); self.assertAlmostEqual(st[4][2], 14.2)
        with self.assertRaises(ValueError): S.startup_steps(7.1, "ANT9")

    def test_no_tx_or_calibration_opcodes_in_startup(self):
        banned = ("MOX", "PA_BIAS", "EEPROM", "QSD", "QSE", "SET_TR", "SIG", "GEN", "TEST", "POT", "REG")
        for op, *_ in S.startup_steps(7.1):
            self.assertFalse(any(b in op for b in banned if op != "SET_TRX_PREAMP"), op)


class WithRadio(unittest.TestCase):
    """Read-only identification over the simulated MIDI radio; MIDI must be refused for writes."""
    def setUp(self):
        self.radio = FakeRadio(); self.t = RawMidi(self.radio.path)
        self.fx = Flex(MidiLink(self.t), allow_write=True)
    def tearDown(self):
        self.t.close(); self.radio.close()

    def settle(self):
        self.fx.get("GET_MODEL")                          # round trip: earlier sets have been processed

    def test_identify(self):
        idn = S.identify(self.fx, boot_wait=1)
        self.assertEqual((idn.model_name, idn.serial), ("FLEX-5000A", 12345))
        self.assertEqual(idn.firmware, 0x02010408)
        self.assertTrue(idn.has_atu); self.assertFalse(idn.has_rx2)
        self.assertEqual(self.radio.writes, [])
        self.assertIn("FLEX-5000A", idn.describe())

    def test_atu_drain_loops_until_empty(self):
        left = [3]
        def res():
            v = left[0] << 24; left[0] = max(0, left[0] - 1); return v
        self.radio.answers["RDAL_OP_ATU_GET_RESULT"] = res
        self.assertEqual(S.flush_atu(self.fx), 4)

    def test_writes_over_midi_are_refused(self):
        for call in (lambda: S.run_startup(self.fx, 7.1, boot_wait=1), lambda: S.run_steps(self.fx, S.shutdown_steps())):
            with self.assertRaisesRegex(S.StartupError, "FireWire mailbox"):
                call()
        self.settle(); self.assertEqual(self.radio.writes, [])
        self.assertEqual(self.radio.gets, [("RDAL_OP_GET_MODEL", 0, 0)], "refusal happens before any traffic")

    def test_waits_for_booting_radio(self):
        n = [0]
        def fw():
            n[0] += 1; return 0 if n[0] < 4 else 0x02010408
        self.radio.answers["RDAL_OP_GET_FIRMWARE_REV"] = fw
        naps = []
        idn = S.identify(self.fx, boot_wait=60, poll=0.5, sleep=lambda s: naps.append(s))
        self.assertEqual(idn.firmware, 0x02010408); self.assertGreaterEqual(naps.count(0.5), 3)

    def test_gives_up_if_radio_never_answers(self):
        self.radio.answers["RDAL_OP_GET_FIRMWARE_REV"] = 0
        t = [0.0]
        with self.assertRaisesRegex(S.StartupError, "too soon after power-on"):
            S.identify(self.fx, boot_wait=5, poll=1, sleep=lambda s: t.__setitem__(0, t[0] + s), clock=lambda: t[0])


class WithMailbox(unittest.TestCase):
    """The write sequences over the simulated FireWire mailbox, the only transport they are allowed on."""
    def setUp(self):
        from test_pal import FakeDiceBus
        from firesidectl.fake import DEFAULTS
        from firesidectl.opcodes import OPCODES
        from firesidectl.pal import PalMailbox
        self.bus = FakeDiceBus(); self.bus.answers = {OPCODES[n][0]: v for n, v in DEFAULTS.items()}
        mb = PalMailbox(self.bus, poll=0); assert mb.verify()
        self.fx = Flex(mb, allow_write=True)
        self.READS = {v[0] for v in OPCODES.values() if v[1] in ("r", "rw")}

    def writes(self):
        out = []
        for _off, blk in self.bus.writes:
            op, d1, d2 = struct.unpack(">III", blk); op &= 0x7FFFFFFF
            if op not in self.READS:
                out.append((op, d1, d2))
        return out

    def test_full_startup_sends_exact_sequence(self):
        S.run_startup(self.fx, 7.1, "ANT1", boot_wait=1)
        w = self.writes()
        self.assertEqual([op for op, *_ in w], [1219, 1247, 1320, 1278, 1347])   # INITIALIZE, TRX_PREAMP, STANDBY, RX1_ANT, RX1_FREQ_TW
        self.assertEqual(w[0][1:], (0, 0)); self.assertEqual(w[1][1:], (0, 0))
        self.assertEqual(w[2][1:], (0, 0)); self.assertEqual(w[3][1:], (1, 0))
        self.assertEqual(w[4][1], 0x03A29C77); self.assertAlmostEqual(P.as_f32(w[4][2]), 7.1, places=5)

    def test_shutdown_unkeys_standby_then_pa_bias_off(self):
        S.run_steps(self.fx, S.shutdown_steps())
        self.assertEqual(self.writes(), [(1292, 0, 0), (1320, 1, 0), (1285, 0, 0)])  # SET_MOX 0, SET_STANDBY 1, SET_PA_BIAS 0

    def test_exemptions_are_value_specific(self):
        for bad in (("SET_MOX", 1, 0, "key"), ("SET_PA_BIAS", 1, 0, "bias on"), ("INITIALIZE", 1, 0, "odd")):
            with self.assertRaisesRegex(SafetyError, "not a reviewed value"):
                S.run_steps(self.fx, [bad])
        self.assertEqual(self.writes(), [])

    def test_permit_covers_only_the_reviewed_value_even_inside_the_window(self):
        """F1: another caller (or thread) using the same Flex while a permit is open cannot send a different value."""
        import threading
        with self.fx.permit("SET_MOX"):
            with self.assertRaises(SafetyError): self.fx.set("SET_MOX", 1)            # same thread, inside the window
            err = []
            t = threading.Thread(target=lambda: err.append(self._try(lambda: self.fx.set("SET_MOX", 1))))
            t.start(); t.join()
            self.assertIsInstance(err[0], SafetyError)                                 # another thread, inside the window
            self.fx.set("SET_MOX", 0)                                                  # the reviewed value still passes
        for op, d1, d2 in (("SET_PA_BIAS", 1, 0), ("INITIALIZE", 0, 1)):
            with self.fx.permit(op):
                with self.assertRaises(SafetyError): self.fx.set(op, d1, d2)
        self.assertEqual(self.writes(), [(1292, 0, 0)])

    @staticmethod
    def _try(f):
        try: f(); return None
        except Exception as e: return e

    def test_second_argument_must_be_zero_too(self):
        """F2: SET_MOX(0, x) and SET_PA_BIAS(0, x) with x != 0 are not reviewed values."""
        for bad in (("SET_MOX", 0, 5, "odd d2"), ("SET_PA_BIAS", 0, 1, "odd d2")):
            with self.assertRaisesRegex(SafetyError, "not a reviewed value"):
                S.run_steps(self.fx, [bad])
        self.assertEqual(self.writes(), [])

    def test_allow_dangerous_still_overrides_explicitly(self):
        from firesidectl.pal import PalMailbox
        fx = Flex(PalMailbox(self.bus, poll=0), allow_write=True, allow_dangerous=True)
        fx.set("SET_PA_BIAS", 0, 0)                                                    # operator override path unchanged
        self.assertEqual(self.writes(), [(1285, 0, 0)])

    def test_last_write_is_confirmed_consumed(self):
        """F3: run_steps waits (read-only) until the firmware has taken the final command."""
        S.run_steps(self.fx, S.shutdown_steps())
        self.assertEqual(self.bus.executed[-3:], [1292, 1320, 1285])                   # PA bias off really ran
        self.assertFalse(struct.unpack(">I", self.bus.mem[0xFFFF_E000_1000])[0] & 0x80000000)

    def test_mailbox_that_never_takes_the_last_write_is_reported(self):
        from firesidectl.pal import PalMailbox
        mb = PalMailbox(self.bus, poll=0, timeout=0.05); self.assertTrue(mb.verify())
        self.bus.busy_polls = 10**9                                                    # firmware stops consuming commands
        fx = Flex(mb, allow_write=True)
        with self.assertRaises(TimeoutError):
            S.run_steps(fx, [("SET_STANDBY", 1, 0, "standby")])

    def test_unverified_mailbox_is_refused_before_any_traffic(self):
        """F4: a mailbox whose byte order was never proven (even a correct guess) cannot carry start-up writes."""
        from firesidectl.pal import PalMailbox
        fx = Flex(PalMailbox(self.bus, poll=0), allow_write=True)
        w0, r0 = len(self.bus.writes), self.bus.reads                                  # (setUp's own verify() already used the bus)
        for call in (lambda: S.run_startup(fx, 7.1, boot_wait=1), lambda: S.run_steps(fx, S.shutdown_steps())):
            with self.assertRaisesRegex(S.StartupError, "not been verified"):
                call()
        self.assertEqual((len(self.bus.writes), self.bus.reads), (w0, r0))             # refused before ANY traffic

    def test_verify_proves_the_order_and_rejects_the_wrong_one(self):
        from firesidectl.pal import PalMailbox
        from firesidectl.opcodes import OPCODES
        good = PalMailbox(self.bus, poll=0); self.assertFalse(good.verified); self.assertTrue(good.verify()); self.assertTrue(good.verified)
        bad = PalMailbox(self.bus, byteorder="little", poll=0, timeout=0.05); self.assertFalse(bad.verify()); self.assertFalse(bad.verified)
        self.bus.answers[OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]] = 0x01000000                 # plausible-looking but out of range
        self.assertFalse(PalMailbox(self.bus, poll=0).verify())

    def test_verify_fails_fast_on_a_definite_wrong_answer(self):
        """Opus G12: only firmware 0 (booting) or a timeout is worth retrying; anything else fails at once, so the probe word is not resent."""
        from firesidectl.pal import PalMailbox
        from firesidectl.opcodes import OPCODES
        self.bus.answers[OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]] = 0x01030008
        w0 = len(self.bus.writes); naps = []
        self.assertFalse(PalMailbox(self.bus, poll=0).verify(boot_wait=30, sleep=naps.append))
        self.assertEqual(naps, []); self.assertEqual(len(self.bus.writes) - w0, 1)

    def test_verify_waits_for_a_booting_radio(self):
        from firesidectl.pal import PalMailbox
        from firesidectl.opcodes import OPCODES
        n = [0]; fwop = OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0]; real = self.bus._execute
        def execute():
            real()
            if self.bus.executed and self.bus.executed[-1] == fwop:
                n[0] += 1
                if n[0] < 3: self.bus.mem[0xFFFF_E000_1004] = struct.pack(">I", 0)           # firmware 0 while booting
        self.bus._execute = execute
        naps = []; t = [0.0]
        mb = PalMailbox(self.bus, poll=0)
        self.assertTrue(mb.verify(boot_wait=30, sleep=lambda s: (naps.append(s), t.__setitem__(0, t[0] + s)), clock=lambda: t[0]))
        self.assertEqual(len(naps), 2)

    def test_refuses_without_allow_write(self):
        from firesidectl.pal import PalMailbox
        with self.assertRaises(SafetyError):
            S.run_startup(Flex(self.fx.link), 7.1, boot_wait=1)
        self.assertEqual(self.writes(), [])

    def test_permit_is_scoped(self):
        S.run_startup(self.fx, 7.1, boot_wait=1)
        for op in ("INITIALIZE", "SET_MOX", "SET_PA_BIAS"):               # still refused outside the sequences
            with self.assertRaises(SafetyError): self.fx.set(op, 0, 0)

    def test_bad_firmware_or_model_stops_before_any_write(self):
        from firesidectl.opcodes import OPCODES
        fwop, mop = OPCODES["RDAL_OP_GET_FIRMWARE_REV"][0], OPCODES["RDAL_OP_GET_MODEL"][0]
        for fw in (0x01030008, 0x02010407, 0x02020000):
            self.bus.answers[fwop] = fw
            with self.assertRaisesRegex(S.StartupError, "outside PowerSDR's supported range"):
                S.run_startup(self.fx, 7.1, boot_wait=1)
        self.bus.answers[fwop] = 0x02010408; self.bus.answers[mop] = 3
        with self.assertRaisesRegex(S.StartupError, "FLEX-3000"):
            S.run_startup(self.fx, 7.1, boot_wait=1)
        self.assertEqual(self.writes(), [])


if __name__ == "__main__":
    unittest.main()
