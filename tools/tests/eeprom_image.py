"""Builds a synthetic but structurally valid FLEX-5000 TRX EEPROM image for tests (layout from fwc_eeprom.cs)."""
import struct
from firesidectl import eeprom as E


def xor(b):
    x = 0
    for v in b: x ^= v
    return x


def build_trx(serial_raw=0x04D20103, filetime=132_000_000_000_000_000, length=E.TRX_LEN, carriers=((7.05, 0x1234), (14.1, 0x2345)), which=None):
    which = which or ("TRX" if length >= E.TRX_LEN else "RX2")
    d = bytearray(b"\xff" * length)
    struct.pack_into("<I", d, 0x18, serial_raw)
    d[0x3F] = E.CHECKSUM_PRESENT_FLAG
    struct.pack_into("<II", d, 0x40, filetime >> 32, filetime & 0xFFFFFFFF)
    d[0x1BE] = 3
    plausible = {"RX level": lambda i, j: [-60.0, -7.0 if i == 0 else -14.0, -33.0][j] + 0.25 * i,
                 "RX image gain": lambda i, j: 10.0 + i, "RX image phase": lambda i, j: -5.0 + 0.5 * i,
                 "TX image gain": lambda i, j: 12.0 + i, "TX image phase": lambda i, j: -3.0 + 0.5 * i,
                 "PA bridge": lambda i, j: 0.1 + 0.05 * j, "PA power": lambda i, j: 0.02 * (j + 1),
                 "PA SWR": lambda i, j: 1.0 + 0.1 * i, "ATU SWR": lambda i, j: 1.2 + 0.1 * i}
    for name, ptr, addr, rows, per, csum, rng, whiches in E.TABLES:
        if which not in whiches:
            continue
        struct.pack_into("<H", d, ptr, addr)
        for i in range(rows):
            for j in range(per):
                struct.pack_into("<f", d, addr + (i * per + j) * 4, plausible[name](i, j))
        d[csum] = xor(d[addr: addr + rows * per * 4])
    if which != "TRX":
        return bytes(d)
    struct.pack_into("<H", d, 0x54, 0x2C0); struct.pack_into("<II", d, 0x2C0, 0x55, 0x66)
    struct.pack_into("<H", d, E.TX_CARRIER_PTR, E.TX_CARRIER_ADDR)
    off = E.TX_CARRIER_ADDR
    for f, v in carriers:
        vita = int(f * 1e6 * 2 ** 20)
        struct.pack_into("<III", d, off, vita >> 32, vita & 0xFFFFFFFF, v); off += 12
    struct.pack_into("<I", d, off, E.END_OF_TABLE)
    return bytes(d)
