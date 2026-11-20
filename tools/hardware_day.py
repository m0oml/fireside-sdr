"""Hardware-day checklist: a STRICTLY READ-ONLY first-contact report for the FLEX-5000.

It never calls startup/shutdown/set, never opens an audio stream and never writes the EEPROM. Every Flex object it
creates has allow_write=False, so a bug here cannot send a write opcode (the safety gate would raise).
The only bytes it ever writes to the radio are GET (read) requests; see `probe_order` for why even a wrong byte-order
guess is harmless.

Output: <out>/<utc>/report.md plus raw/ artefacts (sysfs attributes, config ROM, kernel log, ALSA files, identify output,
EEPROM backup path). Everything it learns answers an item in NOTES.md "open questions".
"""
import contextlib, datetime, json, os, re, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from firesidectl import startup as S, eeprom as E
from firesidectl.client import Flex, MidiLink
from firesidectl.pal import PalMailbox, DICE_BASE, CMD, RESULT, EVENT, EVENT_BITS, probe_order, swapped_opcode_word
from firesidectl.opcodes import OPCODES
from firesidectl.transport import RawMidi, list_midi_devices

PASS, FAIL, INFO, SKIP = "PASS", "FAIL", "INFO", "SKIP"
FLEX_RE = re.compile(r"flexradio|flex-?[35]000", re.I)


class Report:
    def __init__(self, out_dir):
        self.dir = out_dir; self.raw = os.path.join(out_dir, "raw"); os.makedirs(self.raw, exist_ok=True)
        self.steps, self.facts = [], {}

    def save(self, name, text):
        p = os.path.join(self.raw, name)
        with open(p, "w") as f:
            f.write(text if isinstance(text, str) else json.dumps(text, indent=1, default=str))
        return os.path.relpath(p, self.dir)

    def step(self, name, status, detail="", files=()):
        self.steps.append((name, status, detail, list(files)))
        print(f"[{status:4s}] {name}: {detail.splitlines()[0] if detail else ''}")

    def fact(self, key, value):
        self.facts[key] = value

    def write(self):
        n = {s: sum(1 for x in self.steps if x[1] == s) for s in (PASS, FAIL, INFO, SKIP)}
        L = ["# FLEX-5000 hardware-day report", "", f"Generated {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}  "
             f"({n[PASS]} pass, {n[FAIL]} fail, {n[INFO]} info, {n[SKIP]} skipped). **Read-only run: nothing was written to the radio.**", "",
             "## Steps", "", "| step | result | detail |", "|---|---|---|"]
        for name, st, detail, files in self.steps:
            L.append(f"| {name} | **{st}** | {detail.replace('|', '/').replace(chr(10), '<br>')}" + (f" ({', '.join(files)})" if files else "") + " |")
        L += ["", "## Answers to the open questions", ""]
        questions = [("bus_order", "FireWire mailbox byte order"), ("midi_works", "MIDI SysEx route answers"), ("firewire_works", "FireWire mailbox route answers"),
                     ("routes_agree", "Both routes report the same identity"), ("vendor_id", "Numeric FireWire vendor id (for the udev rule)"),
                     ("snd_dice_bound", "snd-dice bound to the radio"), ("dice_category", "DICE category byte (needs 0x04 for snd-dice generic match)"),
                     ("pcm_formation", "PCM channels per sample rate (snd-dice formation)"), ("events_seen", "Events seen (paddle/PTT/etc.)"),
                     ("firmware", "Firmware version"), ("model", "Model"), ("eeprom_ok", "EEPROM backup verified"),
                     ("dice_nickname", "DICE nickname (a readable name confirms the register layout and byte order)"),
                     ("dice_owner", "DICE owner (who receives notifications; snd-dice sets this when bound)")]
        for k, q in questions:
            L.append(f"- **{q}:** {self.facts.get(k, 'not determined')}")
        L += ["", "## Not done on purpose (needs the reviewed start-up sequence, then separate approval)",
              "- `firesidectl startup` / `shutdown` and any write",
              "- ALSA capture (starts isochronous streaming): channel layout and mirror-sign test, section 7 of analysis/audio-stream-layout.md",
              "- Output-channel experiment (settles the two conflicting DAC tables). **Do not transmit before it.**", ""]
        with open(os.path.join(self.dir, "report.md"), "w") as f:
            f.write("\n".join(L))
        return n


def sh(cmd, env=None, timeout=20):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        return r.returncode, (r.stdout + r.stderr)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)


def attr(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


# ------------------------------------------------------------------ steps
def step_system(rep, env):
    script = os.path.join(HERE, "..", "system", "fireside-system-check.sh")
    rc, out = sh(["bash", script], env=env)
    rep.step("system check", PASS if rc == 0 else FAIL, out.strip().splitlines()[-1] if out.strip() else "no output", [rep.save("system-check.txt", out)])


def step_nodes(rep, sysfs):
    base = os.path.join(sysfs, "bus/firewire/devices")
    nodes, radio = {}, None
    if os.path.isdir(base):
        for n in sorted(os.listdir(base)):
            d = os.path.join(base, n)
            nodes[n] = {a: attr(os.path.join(d, a)) for a in ("vendor", "vendor_name", "model", "model_name", "guid", "specifier_id", "version", "modalias")}
            drv = os.path.join(d, "driver")
            nodes[n]["driver"] = os.path.basename(os.path.realpath(drv)) if os.path.exists(drv) else None
            rom = os.path.join(d, "config_rom")
            if os.path.isfile(rom):
                with open(rom, "rb") as f:
                    nodes[n]["config_rom_hex"] = f.read().hex()
            if "." not in n and FLEX_RE.search((nodes[n]["vendor_name"] or "") + (nodes[n]["model_name"] or "")):
                radio = n
    f = rep.save("firewire-nodes.json", nodes)
    if not radio:
        rep.step("firewire radio node", FAIL, "no FlexRadio node on the bus (radio off? cable? controller?)", [f]); return None
    nd = nodes[radio]; g = nd["guid"]
    rep.fact("vendor_id", nd["vendor"])
    try:
        rep.fact("dice_category", f"0x{(int(g, 0) >> 32) & 0xFF:02X} ({'OK' if (int(g, 0) >> 32) & 0xFF == 4 else 'NOT 0x04'})")
    except (TypeError, ValueError):
        pass
    units = [v for k, v in nodes.items() if k.startswith(radio + ".")]
    rep.fact("snd_dice_bound", "yes" if any(u["driver"] == "snd-dice" for u in units) else f"no (unit drivers: {[u['driver'] for u in units]})")
    rep.step("firewire radio node", PASS, f"{radio}: {nd['vendor_name']} / {nd['model_name']}, vendor {nd['vendor']}, guid {g}", [f])
    return radio


def step_kernel_log(rep):
    for cmd in (["dmesg"], ["journalctl", "-k", "--no-pager", "-n", "600"]):
        rc, out = sh(cmd)
        if rc == 0 and out.strip():
            lines = [l for l in out.splitlines() if re.search(r"firewire|1394|snd.?dice|fw\d|ohci", l, re.I)]
            rep.step("kernel log (FireWire)", INFO, f"{len(lines)} relevant lines", [rep.save("kernel-firewire.txt", "\n".join(lines))]); return
    rep.step("kernel log (FireWire)", SKIP, "dmesg/journalctl not readable by this user (run `sudo dmesg | grep -i firewire` yourself)")


def step_alsa(rep, proc):
    cards = attr(os.path.join(proc, "asound/cards")) or ""
    m = re.search(r"^\s*(\d+) \[[^\]]*\]:.*(flex|dice)", cards, re.I | re.M)
    if not m:
        rep.step("ALSA card", INFO, "no ALSA card mentions FLEX/DICE (expected until snd-dice binds)", [rep.save("asound-cards.txt", cards)]); return
    card = m.group(1); got = {}
    fw = os.path.join(proc, f"asound/card{card}/firewire")
    if os.path.isdir(fw):
        for n in sorted(os.listdir(fw)):
            got[n] = attr(os.path.join(fw, n))
    rep.fact("pcm_formation", (got.get("formation") or "not exposed").replace("\n", " / "))
    rep.step("ALSA card", PASS, f"card {card} present", [rep.save("asound.json", {"cards": cards, "firewire": got})])


def read_dice_global(bus):
    """Standard DICE global section, READ-ONLY (offsets from linux sound/firewire/dice/dice-interface.h). Never write the
    owner register: only its owner may change it, with a compare-and-swap."""
    g_off = struct.unpack(">I", bus.read(DICE_BASE, 4))[0] * 4          # section offsets are in quadlets
    if not 0 < g_off < 0x1000:
        raise ValueError(f"implausible DICE global-section offset {g_off:#x}")
    G = DICE_BASE + g_off
    owner = int.from_bytes(bus.read(G + 0x00, 8), "big")
    nick_raw = bus.read(G + 0x0C, 64)
    nick = bytes(b for i in range(0, 64, 4) for b in reversed(nick_raw[i:i + 4])).split(b"\0")[0].decode("ascii", "replace")
    q = lambda off: struct.unpack(">I", bus.read(G + off, 4))[0]
    return {"global_offset": g_off, "owner": owner,
            "owner_text": "no owner" if owner == 0xFFFF000000000000 else f"node 0x{owner >> 48:04X}, address 0x{owner & 0xFFFFFFFFFFFF:012X}",
            "nickname": nick, "nickname_raw": nick_raw.hex(), "status": q(0x54), "sample_rate": q(0x5C), "version": q(0x60)}


def step_dice_global(rep, bus):
    try:
        g = read_dice_global(bus)
    except (OSError, TimeoutError, KeyError, ValueError, struct.error) as e:
        rep.step("DICE global section", FAIL, f"{type(e).__name__}: {e}"); return
    rep.fact("dice_nickname", repr(g["nickname"])); rep.fact("dice_owner", g["owner_text"])
    rep.step("DICE global section", PASS, f"nickname {g['nickname']!r}, owner: {g['owner_text']}, sample rate reg {g['sample_rate']}, "
             f"DICE version 0x{g['version']:08X}", [rep.save("dice-global.json", g)])


def identity_of(fx, rep, tag):
    try:
        idn = S.identify(fx, boot_wait=3.0)
    except S.StartupError as e:
        rep.step(f"{tag} identify", FAIL, str(e)); return None
    rep.step(f"{tag} identify", PASS, f"{idn.model_name} serial {S.serial_str(idn.serial)} firmware v{S.ver_str(idn.firmware)}",
             [rep.save(f"identify-{tag}.txt", idn.describe())])
    return idn


def run(out_root, *, sysfs="/sys", procfs="/proc", devdir="/dev", midi_factory=None, bus_factory=None, events_seconds=0.0,
        skip_eeprom=False, backups_dir=None, eeprom_prefer="firewire", sleep=time.sleep):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    rep = Report(os.path.join(out_root, stamp))
    env = dict(os.environ, SYSFS=sysfs, PROCFS=procfs, DEVDIR=devdir)

    step_system(rep, env)
    radio = step_nodes(rep, sysfs)
    step_kernel_log(rep)
    step_alsa(rep, procfs)

    ids = {}
    midi = None
    # ---- MIDI route
    if midi_factory is None:
        devs = [d for d in list_midi_devices() if "flex" in d[1].lower() or "dice" in d[1].lower()]
        midi_factory = (lambda p=devs[0][0]: RawMidi(p)) if devs else None
    if midi_factory:
        try:
            midi = midi_factory()
            fx = Flex(MidiLink(midi))                                   # allow_write False
            ids["midi"] = identity_of(fx, rep, "midi")
            rep.fact("midi_works", "yes" if ids["midi"] else "no")
        except OSError as e:
            rep.step("midi route", FAIL, str(e)); rep.fact("midi_works", "no")
    else:
        rep.step("midi route", SKIP, "no FLEX ALSA rawmidi device"); rep.fact("midi_works", "no device")

    # ---- FireWire route
    bus = None; fw_fx = None; order = None
    if bus_factory is None and radio:
        def bus_factory():
            from firesidectl.firewire import FwDevice
            return FwDevice(os.path.join(devdir, radio))
    if bus_factory:
        try:
            bus = bus_factory()
            try:
                regs = {n: bus.read(a, 4).hex() for n, a in (("dice_global_offset", DICE_BASE), ("cmd", CMD), ("result", RESULT), ("event", EVENT))}
                rep.step("firewire raw registers", INFO, "read OK: " + ", ".join(f"{k}={v}" for k, v in regs.items()), [rep.save("registers.json", regs)])
            except (OSError, TimeoutError, KeyError) as e:
                rep.step("firewire raw registers", FAIL, f"{type(e).__name__}: {e}")
            step_dice_global(rep, bus)
            order, notes = probe_order(bus)
            rep.fact("bus_order", order or "NEITHER worked: " + "; ".join(notes))
            if order:
                fw_fx = Flex(PalMailbox(bus, byteorder=order))
                ids["firewire"] = identity_of(fw_fx, rep, "firewire")
                rep.fact("firewire_works", "yes" if ids["firewire"] else "no")
            else:
                rep.step("firewire byte order", FAIL, "; ".join(notes)); rep.fact("firewire_works", "no")
        except OSError as e:
            rep.step("firewire route", FAIL, f"cannot open the radio node: {e}"); rep.fact("firewire_works", "no")
    else:
        rep.step("firewire route", SKIP, "no radio node"); rep.fact("firewire_works", "no node")

    got = [v for v in ids.values() if v]
    if got:
        rep.fact("firmware", f"v{S.ver_str(got[0].firmware)}"); rep.fact("model", got[0].model_name)
    if len(got) == 2:
        same = (got[0].model, got[0].serial, got[0].firmware) == (got[1].model, got[1].serial, got[1].firmware)
        rep.fact("routes_agree", "yes" if same else "NO: investigate"); rep.step("routes agree", PASS if same else FAIL, "MIDI and FireWire report " + ("the same identity" if same else "DIFFERENT identities"))

    # ---- events (optional, read-only)
    if events_seconds > 0 and (fw_fx or midi):
        print(f"Watching events for {events_seconds:.0f} s: tap the paddles now (do NOT press PTT: no transmit testing until the output channels are settled) ...")
        seen, t0, last = [], time.monotonic(), None
        mb = PalMailbox(bus, byteorder=order) if (fw_fx and bus and order) else None
        while time.monotonic() - t0 < events_seconds:
            if mb:
                raw, bits = mb.read_event()
                if raw != last:
                    seen.append(f"{time.monotonic() - t0:6.2f}s EVENT 0x{raw:08X} {[n for n, on in bits.items() if on]}"); last = raw
            if midi:
                midi.pump(0.02)
                while midi.events:
                    seen.append(f"{time.monotonic() - t0:6.2f}s MIDI {midi.events.pop(0)}")
            else:
                sleep(0.02)
        rep.fact("events_seen", f"{len(seen)} change(s): see raw/events.txt")
        rep.step("events", INFO if seen else FAIL, f"{len(seen)} event change(s) in {events_seconds:.0f} s", [rep.save("events.txt", "\n".join(seen))])
    else:
        rep.step("events", SKIP, "not requested (use --events SECONDS)")

    # ---- EEPROM backup (read-only)
    if skip_eeprom:
        rep.step("eeprom backup", SKIP, "skipped by option")
    else:
        mfx = Flex(MidiLink(midi)) if midi else None
        fx = (fw_fx or mfx) if eeprom_prefer == "firewire" else (mfx or fw_fx)
        if not fx:
            rep.step("eeprom backup", SKIP, "no working route")
        else:
            logs = []
            try:
                rc = E.dump_to_file(fx, backups_dir, None, 2, log=logs.append)
                rep.fact("eeprom_ok", "yes: all checksums verified, passes identical" if rc == 0 else f"saved but PROBLEMS (exit {rc}): see raw/eeprom.txt")
                summary = next((l for l in logs if l.startswith("read ")), "") + ("" if rc == 0 else f"  (problems found, exit {rc})")
                rep.step("eeprom backup", PASS if rc == 0 else FAIL, summary, [rep.save("eeprom.txt", "\n".join(logs))])
            except (E.EepromReadError, S.StartupError, TimeoutError, OSError) as e:
                rep.fact("eeprom_ok", f"no: {e}"); rep.step("eeprom backup", FAIL, str(e), [rep.save("eeprom.txt", "\n".join(logs))])

    if midi:
        midi.close()
    if bus is not None and hasattr(bus, "close"):
        bus.close()
    counts = rep.write()
    print(f"\nreport: {os.path.join(rep.dir, 'report.md')}   ({counts[PASS]} pass, {counts[FAIL]} fail)")
    return rep


def add_dice_global(mem, nickname=b"FLEX-5000", owner=0xFFFF000000000000):
    """Simulated DICE global section for rehearsals/tests (nickname stored quadlet-swapped, as real DICE devices do)."""
    G = DICE_BASE + 10 * 4
    mem[DICE_BASE] = struct.pack(">I", 10)
    mem[G] = owner.to_bytes(8, "big")
    n = nickname.ljust(64, b"\0")
    mem[G + 0x0C] = bytes(b for i in range(0, 64, 4) for b in reversed(n[i:i + 4]))
    mem[G + 0x54] = struct.pack(">I", 1); mem[G + 0x5C] = struct.pack(">I", 48000); mem[G + 0x60] = struct.pack(">I", 0x01000400)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(HERE, "..", "hardware-day"))
    ap.add_argument("--events", type=float, default=0.0, metavar="SECONDS", help="also watch paddle/PTT events for this long")
    ap.add_argument("--skip-eeprom", action="store_true")
    ap.add_argument("--rehearse", action="store_true", help="run against built-in simulated radios (no hardware)")
    a = ap.parse_args(argv)
    kw = {}
    if a.rehearse:
        sys.path.insert(0, os.path.join(HERE, "tests"))
        from firesidectl.fake import FakeRadio
        from eeprom_image import build_trx
        from test_pal import FakeDiceBus
        radio = FakeRadio(); img = build_trx(); radio.eeprom["TRX"][:len(img)] = img
        from firesidectl.fake import DEFAULTS
        class Bus(FakeDiceBus):                                   # answers consistent with the simulated MIDI radio
            def __init__(s):
                super().__init__(); s.mem[DICE_BASE] = struct.pack(">I", 10)
                s.answers = {OPCODES[n][0]: v for n, v in DEFAULTS.items()}
                add_dice_global(s.mem)
        kw = dict(midi_factory=lambda: RawMidi(radio.path), bus_factory=Bus, eeprom_prefer="midi",
                  sysfs="/nonexistent", procfs="/nonexistent")
    rep = run(os.path.abspath(a.out), events_seconds=a.events, skip_eeprom=a.skip_eeprom,
              backups_dir=os.path.join(os.path.abspath(a.out), "rehearsal-backups") if a.rehearse else None, **kw)
    return 0 if not any(s[1] == FAIL for s in rep.steps) else 1


if __name__ == "__main__":
    sys.exit(main())
