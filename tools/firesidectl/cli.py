"""firesidectl command line.   ./firesidectl-cli --help"""
import argparse, contextlib, re, sys, time
from .opcodes import OPCODES
from .transport import RawMidi, list_midi_devices
from .client import Flex, MidiLink, probe, resolve, SafetyError
from . import startup as S
from .eeprom import EepromReadError


def _num(s):
    try:
        return int(s, 0)
    except ValueError:
        return float(s)


def build_parser():
    ap = argparse.ArgumentParser(prog="firesidectl", description="FLEX-5000 control (prototype)")
    ap.add_argument("--device", help="rawmidi node, e.g. /dev/snd/midiC2D0 (default: first FLEX one)")
    ap.add_argument("--fake", action="store_true", help="use the built-in simulated radio (MIDI)")
    ap.add_argument("--fw", metavar="/dev/fwN", help="use the FireWire PAL mailbox instead of MIDI")
    ap.add_argument("--byte-order", choices=("auto", "big", "little"), default="auto",
                    help="FireWire mailbox byte order (auto: probe with a read-only GET_FIRMWARE_REV)")
    ap.add_argument("--le", action="store_true", help="same as --byte-order little")
    ap.add_argument("--allow-write", action="store_true")
    ap.add_argument("--allow-dangerous", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true", help="hex-dump MIDI traffic")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="list MIDI and FireWire devices")
    sub.add_parser("probe", help="read-only identification sweep")
    idp = sub.add_parser("identify", help="read-only: identity, boards fitted, firmware check (what PowerSDR does at launch)")
    idp.add_argument("--boot-wait", type=float, default=10.0, help="seconds to wait for a booting radio")
    ed = sub.add_parser("eeprom-dump", help="read-only: back up calibration EEPROM(s), verify and decode them")
    ed.add_argument("--out", help="directory for backups (default: $FIRESIDE_BACKUP_DIR, else ~/.local/share/fireside/backups, where the app looks)")
    ed.add_argument("--rx2", dest="rx2", action="store_true", default=None, help="also dump the RX2 EEPROM (default: if fitted)")
    ed.add_argument("--no-rx2", dest="rx2", action="store_false")
    ed.add_argument("--passes", type=int, default=2, help="read the EEPROM this many times and require identical results")
    for n in ("get", "set"):
        s = sub.add_parser(n); s.add_argument("op"); s.add_argument("data1", nargs="?", default="0"); s.add_argument("data2", nargs="?", default="0")
    o = sub.add_parser("ops", help="list known opcodes"); o.add_argument("pattern", nargs="?", default="")
    m = sub.add_parser("monitor", help="print paddle/PTT/mic-button events"); m.add_argument("--seconds", type=float, default=30)
    st = sub.add_parser("startup", help="PowerSDR's init + power-on sequence (receive only)")
    st.add_argument("--freq", type=float, default=7.1, help="RX1 DIAL frequency in MHz (default 7.1); the DDS is tuned 9 kHz below it, as PowerSDR")
    st.add_argument("--ant", default="ANT1", choices=sorted(S.RX_ANT), help="RX1 antenna")
    st.add_argument("--dry-run", action="store_true", help="print the plan; do not talk to the radio")
    st.add_argument("--boot-wait", type=float, default=10.0, help="seconds to wait for a booting radio")
    sub.add_parser("shutdown", help="MOX off + standby")
    return ap


@contextlib.contextmanager
def open_link(a):
    """Yield (flex, raw_midi_or_None, mailbox_or_None, fake_or_None)."""
    fake = None
    try:
        if a.fw:
            from .firewire import FwDevice
            from .pal import PalMailbox
            with FwDevice(a.fw) as dev:
                if dev.is_local:
                    raise OSError(f"{a.fw} is the local controller; pick the radio's node (see `list`)")
                order = "little" if a.le else a.byte_order
                if order == "auto":
                    from .pal import probe_order
                    order, notes = probe_order(dev)
                    if order is None:
                        raise OSError("could not determine the mailbox byte order: " + "; ".join(notes))
                mb = PalMailbox(dev, byteorder=order)
                if not mb.verify(boot_wait=getattr(a, "boot_wait", 0.0)):
                    raise OSError("the mailbox did not return a plausible firmware revision (wrong byte order, radio still booting, "
                                  "or firmware outside the supported range); nothing was written")
                yield Flex(mb, a.allow_write, a.allow_dangerous), None, mb, None
            return
        if a.fake:
            from .fake import FakeRadio
            fake = FakeRadio(); path = fake.path
        else:
            path = a.device
            if not path:
                devs = [d for d in list_midi_devices() if "flex" in d[1].lower()]
                if not devs:
                    raise OSError("No FLEX MIDI device found (is the radio on and bound to snd-dice?). "
                                  "Use `list`, --device PATH, --fw /dev/fwN or --fake.")
                path = devs[0][0]
        log = (lambda d, n, f: print(f"  {d} {n!s:8} {f.hex(' ')}", file=sys.stderr)) if a.verbose else None
        with RawMidi(path) as t:
            yield Flex(MidiLink(t, log), a.allow_write, a.allow_dangerous), t, None, fake
    finally:
        if fake:
            fake.close()


def _print_probe(fx):
    for n, v in probe(fx).items():
        print(f"{n:22s} {v if isinstance(v, str) else f'{v}  (0x{v:08X})'}")


def main(argv=None):
    a = build_parser().parse_args(argv)
    try:
        return _main(a)
    except ValueError as e:                       # bad user input (frequency range, antenna, hex number...)
        print(f"invalid argument: {e}", file=sys.stderr); return 2


def _main(a):

    if a.cmd == "ops":
        for n, (v, acc, args, res) in sorted(OPCODES.items(), key=lambda kv: kv[1][0]):
            if re.search(a.pattern, n, re.I):
                print(f"{v:5d} {acc:2s} {n:42s} args={','.join(args)} -> {res or '-'}")
        return 0
    if a.cmd == "list":
        from .firewire import list_devices
        devs = list_midi_devices()
        for p, name in devs:
            print(f"MIDI {p}   {name}")
        if not devs:
            print("MIDI: no ALSA rawmidi devices")
        fws = list_devices()
        for p, guid, local in fws:
            tag = "local controller" if local is True else (local or "remote node")
            print(f"FW   {p}   guid={guid and f'{guid:016X}'}   {tag}")
        if not fws:
            print("FW:   no /dev/fw* nodes (no FireWire controller, or firewire-ohci not loaded)")
        return 0
    if a.cmd == "startup" and a.dry_run:                       # no radio needed
        steps = S.startup_steps(S.dds_for_dial(a.freq), a.ant)  # validates before printing anything; --freq is the DIAL, the DDS sits 9 kHz below it
        print(f"Plan (receive only; nothing here keys the transmitter, touches PA bias or the EEPROM):")
        print("  [read-only] identify: firmware rev, model, serial, boards, region, status; refuse if "
              f"firmware outside v{S.ver_str(S.FW_MIN)}..v{S.ver_str(S.FW_MAX)} or model is a FLEX-3000")
        print("  [read-only] if an ATU is fitted: drain its result buffer")
        for op, d1, d2, why in steps:
            print(f"  [write]     {op}({d1}, {d2})   # {why}")
        print(f"  then wait {int(S.SETTLE_BEFORE_AUDIO_S * 1000)} ms and start the I/Q audio stream")
        return 0

    try:
        with open_link(a) as (fx, t, mb, _fake):
            if a.cmd == "probe":
                _print_probe(fx)
            elif a.cmd == "identify":
                print(S.identify(fx, boot_wait=a.boot_wait).describe())
            elif a.cmd == "get":
                v = fx.get(a.op, int(_num(a.data1)), int(_num(a.data2)))
                print(v if isinstance(v, float) else f"{v} (0x{v:08X})")
            elif a.cmd == "set":
                name = resolve(a.op)[0]
                fx.set(a.op, _num(a.data1), _num(a.data2)); print(f"sent {name}")
            elif a.cmd == "startup":
                if not a.allow_write:
                    print("startup writes to the radio; re-run with --allow-write (or use --dry-run)", file=sys.stderr)
                    return 3
                idn = S.run_startup(fx, S.dds_for_dial(a.freq), a.ant, log=lambda m: print(m), boot_wait=a.boot_wait)
                time.sleep(S.SETTLE_BEFORE_AUDIO_S)
                print(f"radio is up on {a.freq} MHz ({a.ant}; DDS {S.IF_OFFSET_MHZ * 1000:g} kHz below the dial); start the I/Q stream now")
            elif a.cmd == "shutdown":
                S.run_steps(fx, S.shutdown_steps(), log=lambda m: print(m))
            elif a.cmd == "eeprom-dump":
                from .eeprom import dump_to_file
                return dump_to_file(fx, a.out, a.rx2, a.passes, log=lambda m: print(m))
            elif a.cmd == "monitor":
                end = time.monotonic() + a.seconds
                if t is not None:
                    while time.monotonic() < end:
                        t.pump(0.1)
                        while t.events:
                            st_, d1, d2 = t.events.pop(0)
                            kind = {0x90: "NoteOn", 0x80: "NoteOff"}.get(st_ & 0xF0, hex(st_))
                            print(f"{kind} note={d1} vel={d2}")
                else:                                         # FireWire: poll the EVENT register
                    last = None
                    while time.monotonic() < end:
                        raw, bits = mb.read_event()
                        if raw != last:
                            print(f"0x{raw:08X}  " + " ".join(n for n, on in bits.items() if on)); last = raw
                        time.sleep(0.02)
        return 0
    except SafetyError as e:
        print(f"refused: {e}", file=sys.stderr); return 3
    except EepromReadError as e:
        print(f"EEPROM read failed, nothing saved: {e}", file=sys.stderr); return 6
    except S.StartupError as e:
        print(f"startup stopped: {e}", file=sys.stderr); return 5
    except TimeoutError as e:
        print(f"timeout: {e}", file=sys.stderr); return 4
    except OSError as e:
        print(f"error: {e}", file=sys.stderr); return 2


if __name__ == "__main__":
    sys.exit(main())
