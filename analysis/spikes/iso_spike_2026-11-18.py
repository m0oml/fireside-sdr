"""SPIKE (scratch, not committed): receive the FLEX-5000's DICE TX stream in userspace at a chosen rate, bypassing snd-dice.
Writes ONLY DICE sound-chip registers (clock select, TX iso channel, enable) and restores all three in `finally`.
No PAL/RDAL radio opcodes. Usage: sudo python3 iso_spike.py [rate_hz] [seconds]"""
import ctypes, fcntl, mmap, os, select, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from firesidectl.firewire import FwDevice

RATE = int(sys.argv[1]) if len(sys.argv) > 1 else 192000
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
RATE_IDX = {32000: 0, 44100: 1, 48000: 2, 88200: 3, 96000: 4, 176400: 5, 192000: 6}[RATE]

def IOC(d, nr, size): return (d << 30) | (size << 16) | (ord('#') << 8) | nr
GET_INFO, CREATE, QUEUE, START, STOP = IOC(3, 0, 40), IOC(3, 8, 32), IOC(3, 9, 24), IOC(1, 10, 16), IOC(1, 11, 4)
ALLOC = IOC(3, 0x0D, 24)
EV_ISO_INTERRUPT, EV_RES_ALLOC, EV_BUS_RESET = 3, 4, 0

N, CHUNK, PL, HDR = 512, 32, 1536, 8        # descriptors in the ring, packets per interrupt, payload bytes per packet, ctx header (iso hdr + timestamp)

B = 0xFFFFE0000000
dev = FwDevice("/dev/fw1")
q = lambda a: struct.unpack(">I", dev.read(a, 4))[0]
wq = lambda a, v: dev.write(a, struct.pack(">I", v & 0xFFFFFFFF))
sec = [q(B + 4 * i) * 4 for i in range(6)]
G, TX = B + sec[0], B + sec[2]
TX_ISO = TX + 8 + 0
orig_clock, orig_enable, orig_txiso = q(G + 0x4C), q(G + 0x50), q(TX_ISO)
print(f"start: clock={orig_clock:#06x} enable={orig_enable} tx_iso={orig_txiso:#x} tx_audio={q(TX + 8 + 4)} tx_midi={q(TX + 8 + 8)}")
if orig_enable != 0: sys.exit("DICE stream already enabled (snd-dice streaming?) - not touching it")

fd = os.open("/dev/fw0", os.O_RDWR)
reset = ctypes.create_string_buffer(64)
gi = bytearray(struct.pack("<IIQQQI4x", 5, 0, 0, ctypes.addressof(reset), 0, 0)); fcntl.ioctl(fd, GET_INFO, gi, True)
def read_event(timeout):
    r, _, _ = select.select([fd], [], [], timeout)
    return os.read(fd, 1 << 16) if r else None

ctx = None; enabled = False
stats = dict(pkts=0, empty=0, blocks=0, dbc_err=0, dbs=set(), fdf=set(), fmt=set(), lens=set(), labels=set())
sumsq = [0.0] * 64; peak = [0] * 64; nsamp = 0; raw = bytearray()
try:
    a = bytearray(struct.pack("<QQII", 1, ((1 << 63) - 1) & ~1, 1200, 0)); fcntl.ioctl(fd, ALLOC, a, True)
    ch = None
    for _ in range(50):
        ev = read_event(0.2)
        if ev and struct.unpack_from("<I", ev, 8)[0] == EV_RES_ALLOC:
            _, _, h, ch, bw = struct.unpack_from("<QIIii", ev, 0); print(f"IRM: channel {ch}, bandwidth {bw}"); break
    if ch is None or ch < 0: sys.exit(f"iso resource allocation failed (channel {ch})")

    c = bytearray(struct.pack("<IIIIQI4x", 1, HDR, ch, 2, 2, 0)); fcntl.ioctl(fd, CREATE, c, True)
    ctx = struct.unpack_from("<I", c, 24)[0]
    mm = mmap.mmap(fd, N * PL, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    base = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    def queue(chunk):
        ctl = [PL | (HDR << 24) | ((1 << 16) if i == CHUNK - 1 else 0) for i in range(CHUNK)]
        pk = (ctypes.c_uint32 * CHUNK)(*ctl)
        qa = bytearray(struct.pack("<QQII", ctypes.addressof(pk), base + chunk * CHUNK * PL, CHUNK * 4, ctx)); fcntl.ioctl(fd, QUEUE, qa, True)
    for k in range(N // CHUNK): queue(k)
    fcntl.ioctl(fd, START, bytearray(struct.pack("<iIII", -1, 0, 15, ctx)), True)

    wq(G + 0x4C, (RATE_IDX << 8) | (orig_clock & 0xFF)); time.sleep(0.5)
    print(f"clock now {q(G + 0x4C):#06x} status {q(G + 0x54):#x} rate {q(G + 0x5C)} tx_audio={q(TX + 8 + 4)} tx_midi={q(TX + 8 + 8)}")
    wq(TX_ISO, ch); wq(G + 0x50, 1); enabled = True

    pkts = []; t0 = time.monotonic(); nxt = 0; last_dbc = None; first_cycle = None
    while time.monotonic() - t0 < SECS:
        ev = read_event(0.5)
        if ev is None: print("no iso event in 0.5 s"); continue
        typ = struct.unpack_from("<I", ev, 8)[0]
        if typ == EV_BUS_RESET: print("BUS RESET - aborting"); break
        if typ != EV_ISO_INTERRUPT: continue
        _, _, cyc, hlen = struct.unpack_from("<QIII", ev, 0)
        hdrs = ev[20:20 + hlen]
        for i in range(hlen // HDR):
            isoh, ts = struct.unpack_from(">II", hdrs, i * HDR); dlen = isoh >> 16
            off = (nxt * CHUNK + i) * PL
            pkts.append((ts, dlen, bytes(mm[off:off + min(dlen, PL)])))
        queue(nxt); nxt = (nxt + 1) % (N // CHUNK)
    el = time.monotonic() - t0
finally:
    if enabled:
        try: wq(G + 0x50, 0)
        except Exception as e: print("restore enable FAILED:", e)
    for addr, val, what in ((TX_ISO, orig_txiso, "tx iso"), (G + 0x4C, orig_clock, "clock")):
        try: wq(addr, val)
        except Exception as e: print(f"restore {what} FAILED:", e)
    if ctx is not None:
        try: fcntl.ioctl(fd, STOP, bytearray(struct.pack("<I", ctx)), True)
        except Exception: pass
    os.close(fd)
    time.sleep(0.3)
    print(f"restored: clock={q(G + 0x4C):#06x} enable={q(G + 0x50)} tx_iso={q(TX_ISO):#x} status={q(G + 0x54):#x}")

import numpy as np, math
lens = np.array([p[1] for p in pkts]); print(f"\n{RATE} Hz, {el:.2f} s: {len(pkts)} packets ({len(pkts)/el:.0f}/s); length histogram {dict(zip(*np.unique(lens, return_counts=True)))}")
cyc = np.array([(p[0] & 0x1FFF) for p in pkts]); d = np.diff(cyc) % 8000; print("cycle-step histogram", dict(zip(*np.unique(d, return_counts=True))))
data = [p for p in pkts if p[1] >= 8]
q0 = np.array([struct.unpack_from(">I", p[2], 0)[0] for p in data]); q1 = np.array([struct.unpack_from(">I", p[2], 4)[0] for p in data])
dbs = int(np.bincount((q0 >> 16) & 0xFF).argmax()); dbc = q0 & 0xFF; nb = (np.array([p[1] for p in data]) - 8) // (4 * dbs)
print(f"DBS {set(((q0>>16)&0xFF).tolist())} FMT {set(((q1>>24)&0x3F).tolist())} FDF {set(hex(x) for x in ((q1>>16)&0xFF).tolist())}  blocks/packet {dict(zip(*np.unique(nb, return_counts=True)))}")
print("first 12 (dbc, blocks):", list(zip(dbc[:12].tolist(), nb[:12].tolist())))
step = (dbc[1:] - dbc[:-1]) % 256; prevnb = nb[:-1]
print("DBC step vs previous packet's blocks: equal", int((step == prevnb).sum()), " step==8 always?", dict(zip(*np.unique(step, return_counts=True))))
blk = np.frombuffer(b"".join(p[2][8:8 + n * dbs * 4] for p, n in zip(data, nb)), dtype=">u4").reshape(-1, dbs)
print(f"data blocks {len(blk)} = {len(blk)/el:.0f}/s; labels {set(hex(x) for x in np.unique(blk >> 24).tolist())}")
v = (blk & 0xFFFFFF).astype(np.int32); v = np.where(v & 0x800000, v - (1 << 24), v)
for s_ in range(dbs): r = math.sqrt(float((v[:, s_].astype(np.float64) ** 2).mean())); print(f"  slot {s_:2d}: {20*math.log10(max(r,1e-9)/8388608):7.1f} dBFS rms")
np.save(f"/tmp/iso_{RATE}.npy", v)
