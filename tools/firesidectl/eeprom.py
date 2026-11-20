"""READ-ONLY backup and decoder for the FLEX-5000 calibration EEPROMs.

The factory calibration (receive level offsets, image rejection, PA bridge/power curves, TX
carrier nulls, ...) lives in an EEPROM on the TRX board (and a second one on RX2). It is not
recoverable if lost, so the first thing to do with a donated radio is take this backup.

This module only ever issues READ_*_EEPROM_UINT32 requests. Layout, checksums and plausible
ranges are transcribed from PowerSDR 2.7.2 Console/FWC/fwc_eeprom.cs and checksum.cs:
  * header: u32 serial @0x18, 0xC5 "checksums present" flag @0x3F, FILETIME last-cal date
    (hi u32 @0x40, lo u32 @0x44), u16 table pointers @0x48..0x62, image version byte @0x1BE
  * tables of little-endian float32, one row per band (160,80,60,40,30,20,17,15,12,10,6 m)
  * checksum of a table = XOR of all its raw bytes, stored in a byte after the table
PowerSDR's own backup reads 0xC1 rows x 16 bytes = 3088 bytes of the TRX EEPROM with 32-bit
reads (low byte first); we do the same and also write its CSV format.

DO NOT run PowerSDR's FWCEEPROM.CheckAll(): it overwrites the calibration with random numbers
(it is never called by the normal program, only defined).
"""
import csv, datetime, hashlib, json, os, struct, time

TRX_LEN = 0xC1 * 16          # 3088 bytes, as PowerSDR's Dump()
RX2_LEN = 0x200              # covers every documented RX2 field (header, level, image tables)
BANDS = ["160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"]
CHECKSUM_PRESENT_FLAG = 0xC5
END_OF_TABLE = 0xDEADBEEF
SAMPLES = {"TRX": "READ_TRX_EEPROM_UINT32", "RX2": "READ_RX2_EEPROM_UINT32"}


# name, pointer addr, expected table addr, rows, floats per row, checksum addr, (lo, hi) plausible range, which EEPROMs
TABLES = [
    ("RX level",       0x48, 0x100, 11, 3,  0x18F, None,       ("TRX", "RX2")),
    ("RX image gain",  0x4A, 0x190, 11, 1,  0x1BF, (-500, 500), ("TRX", "RX2")),
    ("RX image phase", 0x4C, 0x1C0, 11, 1,  0x1EF, (-500, 500), ("TRX", "RX2")),
    ("TX image gain",  0x4E, 0x1F0, 11, 1,  0x21F, (-500, 500), ("TRX",)),
    ("TX image phase", 0x50, 0x220, 11, 1,  0x24F, (-500, 500), ("TRX",)),
    ("PA bridge",      0x56, 0x2E0, 11, 6,  0x3EF, (0.0, 2.5),  ("TRX",)),
    ("PA power",       0x58, 0x3F0, 11, 13, 0x62F, (0.0, 1.0),  ("TRX",)),
    ("PA SWR",         0x5A, 0x630, 11, 1,  0x65F, (0.2, 5.0),  ("TRX",)),   # a per-band REFLECTED-power correction factor (PowerSDR swr_table), not an SWR: 0.46 on 6 m is normal
    ("ATU SWR",        0x5C, 0x660, 11, 1,  0x68F, (0.5, 5.0),  ("TRX",)),
]
# Present but their checksum rule is not a plain XOR of the table bytes, so not verified here:
UNVERIFIED = [
    ("PA bias",    0x54, 0x2C0, "2 x u32", 0x27E),
    ("VU level",   0x5E, 0x690, "4 x f32", 0x68E),
    ("VU power",   0x60, 0x6A0, "48 x f32", 0x68D),
]
TX_CARRIER_PTR, TX_CARRIER_ADDR, TX_CARRIER_CSUM = 0x62, 0x760, 0x27F


class EepromReadError(Exception):
    pass


def serial_str(sn):
    return f"{sn & 0xFF:02d}{(sn >> 8) & 0xFF:02d}-{(sn >> 16) & 0xFFFF:04d}"


# ------------------------------------------------------------------ reading
def read_image(fx, which="TRX", length=None, progress=None, retries=3, timeout=1.0):
    """Read the EEPROM image with 32-bit reads (low byte first). Returns bytes."""
    op = SAMPLES[which]
    length = length or (TRX_LEN if which == "TRX" else RX2_LEN)
    if length % 4:
        raise ValueError("length must be a multiple of 4")
    out = bytearray()
    for addr in range(0, length, 4):
        for attempt in range(retries):
            try:
                word = fx.get(op, addr, timeout=timeout, as_float=False)      # raw 32 bits, never reinterpret
                break
            except TimeoutError:
                if attempt == retries - 1:
                    raise EepromReadError(f"{which} EEPROM: no reply reading 0x{addr:04X} after {retries} tries")
        out += struct.pack("<I", word)
        if progress and (addr % 256 == 0 or addr + 4 == length):
            progress(which, addr + 4, length)
    return bytes(out)


def read_verified(fx, which="TRX", length=None, passes=2, progress=None, timeout=1.0):
    """Read `passes` times and require identical images; guards against a flaky link."""
    first = read_image(fx, which, length, progress, timeout=timeout)
    for n in range(1, passes):
        again = read_image(fx, which, length, progress, timeout=timeout)
        if again != first:
            diff = [i for i in range(len(first)) if first[i] != again[i]]
            raise EepromReadError(f"{which} EEPROM changed between pass 1 and pass {n + 1} "
                                  f"({len(diff)} bytes differ, first at 0x{diff[0]:04X}); not saving")
    return first


# ------------------------------------------------------------------ decoding
def _u16(d, o): return struct.unpack_from("<H", d, o)[0]
def _u32(d, o): return struct.unpack_from("<I", d, o)[0]
def _f32(d, o): return struct.unpack_from("<f", d, o)[0]


def filetime_to_datetime(ft):
    if ft in (0, 0xFFFFFFFFFFFFFFFF):
        return None
    try:
        return datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc) + datetime.timedelta(microseconds=ft / 10)
    except OverflowError:
        return None


def xor_bytes(b):
    x = 0
    for v in b:
        x ^= v
    return x


def decode(data, which="TRX"):
    """Pure function: bytes -> dict. Never raises on garbage; problems are reported in the dict."""
    r = {"which": which, "length": len(data), "sha256": hashlib.sha256(data).hexdigest(),
         "header": {}, "tables": [], "unverified": [], "tx_carrier": None, "problems": []}
    if len(data) < 0x200:
        r["problems"].append(f"image too short ({len(data)} bytes) to decode")
        return r
    h = r["header"]
    h["serial_raw"] = _u32(data, 0x18)
    h["serial"] = serial_str(h["serial_raw"])
    h["checksum_flag"] = data[0x3F]
    h["checksums_present"] = data[0x3F] == CHECKSUM_PRESENT_FLAG
    ft = (_u32(data, 0x40) << 32) | _u32(data, 0x44)
    dt = filetime_to_datetime(ft)
    h["last_cal"] = dt.isoformat() if dt else None
    h["image_version"] = data[0x1BE]
    blank = all(b == 0xFF for b in data)
    if blank:
        r["problems"].append("EEPROM reads all 0xFF: blank, absent, or the bus is not working")
        return r
    if not h["checksums_present"]:
        r["problems"].append(f"checksum flag @0x3F is 0x{data[0x3F]:02X}, not 0xC5: radio was never calibrated at the factory?")

    for name, ptr, addr, rows, per, csum_addr, rng, whiches in TABLES:
        if which not in whiches or addr + rows * per * 4 > len(data):
            continue
        t = {"name": name, "pointer_addr": ptr, "pointer": _u16(data, ptr), "address": addr,
             "rows": rows, "per_row": per, "checksum_addr": csum_addr}
        t["pointer_ok"] = t["pointer"] == addr
        raw = data[addr: addr + rows * per * 4]
        t["values"] = [[round(_f32(raw, (i * per + j) * 4), 6) for j in range(per)] for i in range(rows)]
        t["checksum_stored"] = data[csum_addr]
        t["checksum_calc"] = xor_bytes(raw)
        t["checksum_ok"] = t["checksum_stored"] == t["checksum_calc"]
        flat = [v for row in t["values"] for v in row]
        t["has_nan"] = any(v != v for v in flat)
        t["in_range"] = None if rng is None else all(rng[0] <= v <= rng[1] for v in flat if v == v)
        if not t["pointer_ok"]:
            r["problems"].append(f"{name}: pointer @0x{ptr:02X} is 0x{t['pointer']:04X}, expected 0x{addr:04X}")
        if not t["checksum_ok"]:
            r["problems"].append(f"{name}: checksum stored 0x{t['checksum_stored']:02X} != calculated 0x{t['checksum_calc']:02X}")
        if t["has_nan"]:
            r["problems"].append(f"{name}: contains NaN")
        if t["in_range"] is False:
            r["problems"].append(f"{name}: value outside plausible range {rng}")
        r["tables"].append(t)

    if which == "TRX":
        for name, ptr, addr, desc, csum in UNVERIFIED:
            r["unverified"].append({"name": name, "pointer": _u16(data, ptr), "address": addr, "layout": desc,
                                    "checksum_stored": data[csum],
                                    "note": "checksum rule is not a plain XOR; copied but not verified"})
        if len(data) >= TX_CARRIER_ADDR + 12:
            entries, off = [], TX_CARRIER_ADDR
            while off + 12 <= len(data) and len(entries) < 100:
                hi = _u32(data, off)
                if hi == END_OF_TABLE:
                    break
                lo, val = _u32(data, off + 4), _u32(data, off + 8)
                entries.append({"freq_mhz": round(((hi << 32) + lo) * 2 ** -20 * 1e-6, 6), "value": val})
                off += 12
            r["tx_carrier"] = {"pointer": _u16(data, TX_CARRIER_PTR), "entries": entries,
                               "terminated": off + 12 <= len(data) and _u32(data, off) == END_OF_TABLE}
    return r


def report(d):
    h = d["header"]
    L = [f"{d['which']} EEPROM  {d['length']} bytes  sha256 {d['sha256'][:16]}..."]
    if h:
        L.append(f"  serial {h['serial']}  last calibrated {h['last_cal'] or 'never / unknown'}  "
                 f"checksums {'present' if h['checksums_present'] else 'ABSENT'}  image version {h['image_version']}")
    for t in d["tables"]:
        flag = "OK " if t["checksum_ok"] and t["pointer_ok"] and not t["has_nan"] and t["in_range"] is not False else "BAD"
        L.append(f"  [{flag}] {t['name']:15s} @0x{t['address']:04X} {t['rows']}x{t['per_row']} floats  "
                 f"checksum 0x{t['checksum_stored']:02X}{'' if t['checksum_ok'] else f' (calc 0x{t['checksum_calc']:02X})'}")
    for u in d["unverified"]:
        L.append(f"  [ -- ] {u['name']:15s} @0x{u['address']:04X} {u['layout']}  (copied, checksum rule not verified)")
    tc = d["tx_carrier"]
    if tc:
        L.append(f"  [ -- ] TX carrier       {len(tc['entries'])} entries" + ("" if tc["terminated"] else "  (no end marker found)"))
    for p in d["problems"]:
        L.append(f"  PROBLEM: {p}")
    return "\n".join(L)


# ------------------------------------------------------------------ writing files
def csv_text(data):
    """PowerSDR Dump() format: header ',0,1,..,F,' then 'row,b0,..,b15,' with hex bytes WITHOUT leading zeros."""
    lines = ["," + ",".join(f"{i:X}" for i in range(16)) + ","]
    for row in range(len(data) // 16):
        lines.append(f"{row:X}," + "".join(f"{b:X}," for b in data[row * 16:(row + 1) * 16]))
    return "\n".join(lines) + "\n"


def parse_csv(text):
    """Inverse of csv_text (round-trip test, and lets old PowerSDR backups be read back)."""
    out = bytearray()
    for line in text.splitlines()[1:]:
        cells = [c for c in line.split(",")[1:] if c != ""]
        out += bytes(int(c, 16) for c in cells)
    return bytes(out)


def _write_new(path, data, mode="wb"):
    """Create-only: never overwrite an existing backup."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
    with os.fdopen(fd, mode) as f:
        f.write(data)
    return path


def save(out_dir, images, meta):
    """images: {"TRX": bytes, "RX2": bytes?}. Writes .bin, .json (+ PowerSDR-style .csv for TRX). Returns paths."""
    os.makedirs(out_dir, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    decoded = {w: decode(b, w) for w, b in images.items()}
    sn = decoded["TRX"]["header"].get("serial", "unknown") if "TRX" in decoded else "unknown"
    prefix = "F5K_" + sn                                       # identify() already rejected the FLEX-3000
    paths = []
    for which, data in images.items():
        paths.append(_write_new(os.path.join(out_dir, f"{prefix}_{stamp}_{which.lower()}.bin"), data))
    if "TRX" in images:
        compat = os.path.join(out_dir, f"{prefix} backup.csv")
        if not os.path.exists(compat):                         # PowerSDR never rewrites this one either
            paths.append(_write_new(compat, csv_text(images["TRX"]).encode(), "wb"))
    doc = {"tool": "firesidectl eeprom-dump", "created_utc": stamp, "meta": meta,
           "decoded": {w: d for w, d in decoded.items()}}
    paths.append(_write_new(os.path.join(out_dir, f"{prefix}_{stamp}.json"), json.dumps(doc, indent=1).encode()))
    return paths, decoded


# Where the app looks for the reference EEPROM backup: $FIRESIDE_BACKUP_DIR, else ~/.local/share/fireside/backups (the app also offers to make the first one)
DEFAULT_DIR = os.environ.get("FIRESIDE_BACKUP_DIR") or os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share"), "fireside", "backups")


def dump_to_file(fx, out_dir=None, include_rx2=None, passes=2, log=print, sleep=time.sleep):
    """CLI entry: identify, read (verified), decode, save. Returns process exit code."""
    out_dir = out_dir or DEFAULT_DIR
    from . import startup as S
    idn = S.identify(fx, boot_wait=10.0)
    log(idn.describe())
    which = ["TRX"] + (["RX2"] if (idn.has_rx2 if include_rx2 is None else include_rx2) else [])
    images, t0 = {}, time.monotonic()
    prog = lambda w, n, total: log(f"  reading {w} EEPROM {n}/{total} bytes") if n == total or n % 1024 == 0 else None
    for w in which:
        images[w] = read_verified(fx, w, passes=passes, progress=prog)
    log(f"read {sum(map(len, images.values()))} bytes x{passes} in {time.monotonic() - t0:.1f}s, passes identical")
    meta = {"model": idn.model, "model_name": idn.model_name, "radio_serial": serial_str(idn.serial),
            "firmware": S.ver_str(idn.firmware), "boards": {k: v["ok"] for k, v in idn.boards.items()}}
    paths, decoded = save(out_dir, images, meta)
    for d in decoded.values():
        log(report(d))
    trx = decoded["TRX"]["header"]
    if trx and trx["serial"] != serial_str(idn.serial):
        log(f"  WARNING: EEPROM serial {trx['serial']} differs from the radio's reported serial {serial_str(idn.serial)}")
    for p in paths:
        log(f"saved {p}")
    log("Backups are read-only files and are never overwritten. Copy them somewhere safe.")
    return 0 if not any(d["problems"] for d in decoded.values()) else 6
