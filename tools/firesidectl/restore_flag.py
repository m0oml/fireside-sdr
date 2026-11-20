"""One-off: restore TRX EEPROM byte 0x3F ("checksums present") from 0xFF back to 0xC5.

PowerSDR 2.7.2 cleared it on 2026-11-18 after a timing race made its checksum reads return stale values
(hardware-day/capture-20261118-powersdr/NOTES.md). Nothing else in the EEPROM changed (full double read vs the backup).

Refuses unless ALL of these hold, and writes nothing in a dry run (the default):
  * the radio's serial number is the backup's;
  * a full double read of the TRX EEPROM is identical in both passes;
  * it differs from the backup in exactly one byte: 0x3F, which reads 0xFF (if it already reads 0xC5 and everything matches: nothing to do).
Then (only with --write) ONE command: WRITE_TRX_EEPROM_UINT8(0x3F, 0xC5), the same call PowerSDR makes (FWCEEPROM.TRXChecksumPresent = true).
The safety gate permits that opcode with exactly these values only (client.REVIEWED_VALUES). Afterwards a full double read
must equal the backup byte for byte. No retry: if the check fails, it stops and reports.
"""
import argparse, sys, time
from . import eeprom as E

ADDR, OLD, NEW = 0x3F, 0xFF, 0xC5
OP_WRITE = "RDAL_OP_WRITE_TRX_EEPROM_UINT8"


def plan(backup: bytes, now: bytes):
    """-> ("write" | "done" | "refuse", message)."""
    if len(now) != len(backup):
        return "refuse", f"length {len(now)} != backup {len(backup)}"
    diff = [i for i in range(len(now)) if now[i] != backup[i]]
    if not diff:
        return "done", "EEPROM already equals the backup (0x3F = 0x%02X): nothing to do" % now[ADDR]
    if diff != [ADDR]:
        return "refuse", "EEPROM differs from the backup at %d byte(s) (%s): not only 0x3F, so nothing is written" % (
            len(diff), ", ".join(f"0x{i:04X}" for i in diff[:8]))
    if now[ADDR] != OLD or backup[ADDR] != NEW:
        return "refuse", f"0x3F reads 0x{now[ADDR]:02X}, backup 0x{backup[ADDR]:02X}: expected 0x{OLD:02X} now and 0x{NEW:02X} in the backup"
    return "write", f"only 0x3F differs (0x{OLD:02X} now, 0x{NEW:02X} in the backup)"


def run(fx, backup: bytes, serial_expected: int, write: bool, log=print, settle_s=0.5, passes=2):
    """Returns 0 on success / nothing to do / dry run, 1 if refused or the check after the write failed."""
    sn = fx.get("GET_SERIAL_NUM", as_float=False)
    if sn != serial_expected:
        log(f"REFUSED: radio serial 0x{sn:08X} is not the backup's 0x{serial_expected:08X}"); return 1
    log(f"radio serial {E.serial_str(sn)} matches the backup")
    now = E.read_verified(fx, "TRX", len(backup), passes=passes)
    action, msg = plan(backup, now)
    log(("REFUSED: " if action == "refuse" else "") + msg)
    if action != "write":
        return 1 if action == "refuse" else 0
    if not write:
        log(f"dry run: would send {OP_WRITE}(0x{ADDR:02X}, 0x{NEW:02X}) once, then re-read and compare. Run with --write to do it."); return 0
    fx.sync()                                                   # mailbox idle before the one write
    with fx.permit(OP_WRITE):
        fx.set(OP_WRITE, ADDR, NEW)
    fx.sync(); time.sleep(settle_s)                             # let the firmware finish the EEPROM write before reading back
    log(f"sent {OP_WRITE}(0x{ADDR:02X}, 0x{NEW:02X}); reading the whole EEPROM back")
    after = E.read_verified(fx, "TRX", len(backup), passes=passes)
    diff = [i for i in range(len(after)) if after[i] != backup[i]]
    if diff:
        log("CHECK FAILED: after the write the EEPROM differs from the backup at " + ", ".join(f"0x{i:04X}=0x{after[i]:02X}" for i in diff[:8])
            + ". Stopped (no retry). Keep the radio as it is and investigate."); return 1
    log("OK: the EEPROM now equals the backup byte for byte (0x3F = 0xC5)"); return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fw", required=True, help="the radio's /dev/fwN"); ap.add_argument("--backup", required=True, help="the backup _trx.bin")
    ap.add_argument("--write", action="store_true", help="really write (default: dry run)")
    a = ap.parse_args(argv)
    backup = open(a.backup, "rb").read()
    serial = E._u32(backup, 0x18) if len(backup) > 0x1C else None   # serial number word in the image (as eeprom.py decodes it)
    from .firewire import FwDevice
    from .pal import PalMailbox, probe_order
    from .client import Flex
    with FwDevice(a.fw) as dev:
        if dev.is_local:
            print(f"{a.fw} is the local controller"); return 1
        order, notes = probe_order(dev)
        if order is None:
            print("byte order unknown: " + "; ".join(notes)); return 1
        mb = PalMailbox(dev, byteorder=order)
        if not mb.verify():
            print("mailbox did not return a plausible firmware revision; nothing written"); return 1
        return run(Flex(mb, allow_write=True, allow_dangerous=False), backup, serial, a.write)


if __name__ == "__main__":
    sys.exit(main())
