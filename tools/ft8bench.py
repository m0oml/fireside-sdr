#!/usr/bin/env python3
"""Score our FT8 decoder against jt9 on the 15 s slot files of one or more compare_ft8.py work folders, fast (decoder tuning loop).

  tools/ft8bench.py WORKDIR [WORKDIR ...] [--bench build/ft8bench]

jt9's decodes are cached in WORKDIR/jt9_decodes.json on the first run. Messages are matched per slot with hashed calls collapsed (<...> == <CALL>)."""
import argparse, glob, json, os, re, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
import compare_ft8
from compare_ft8 import norm, MODE

MODE_NAME = 'ft8'

def jt9_cache(work):
    p = os.path.join(work, f"jt9_decodes{'_ft4' if MODE_NAME == 'ft4' else ''}.json")
    if os.path.exists(p): return {tuple(json.loads(k)): v for k, v in json.load(open(p)).items()}
    J = {}; files = sorted(glob.glob(os.path.join(work, "jt9", "s[0-9][0-9][0-9].wav")))
    for k, f in enumerate(files):
        t = os.path.join(work, "jt9", "t.wav"); shutil.copy(f, t)
        out = subprocess.run(["jt9", MODE[MODE_NAME]['flag'], "-d", "3", "-p", str(int(MODE[MODE_NAME]['slot'])), t], capture_output=True, text=True, timeout=120, cwd=work).stdout; os.remove(t)
        for l in out.splitlines():
            m = re.match(r"\d{6}\s+(-?\d+)\s+(-?[\d.]+)\s+(\d+)\s+[~+]\s+(.*)$", l.strip())
            if m: J.setdefault((k, norm(m[4])), (int(m[1]), float(m[2]), int(m[3])))
    json.dump({json.dumps(list(k)): v for k, v in J.items()}, open(p, "w")); return J

def ours(work, bench):
    files = sorted(glob.glob(os.path.join(work, "jt9", "s[0-9][0-9][0-9].wav")))
    r = subprocess.run([bench] + (['--ft4'] if MODE_NAME == 'ft4' else []) + files, capture_output=True, text=True, timeout=1800)
    O = {}
    for l in r.stdout.splitlines():
        m = re.match(r"(\d+) (-?\d+) (-?[\d.]+) ([\d.]+) ~ (.*)$", l)
        if m: O.setdefault((int(m[1]), norm(m[5])), (int(m[2]), float(m[3]), float(m[4])))
    return O, r.stderr.strip()

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("work", nargs="+"); ap.add_argument("--mode", choices=list(MODE), default="ft8"); ap.add_argument("--bench", default=os.path.join(os.path.dirname(__file__), "..", "build", "ft8bench"))
    a = ap.parse_args(); global MODE_NAME; MODE_NAME = a.mode; a.work = [os.path.abspath(w) for w in a.work]; tj = to = tb = tf = 0; bins = [[0, 0] for _ in range(4)]   # absolute: jt9 runs with cwd=work
    for w in a.work:
        J = jt9_cache(w); O, timing = ours(w, a.bench); both = J.keys() & O.keys(); false = O.keys() - J.keys()
        print(f"{os.path.basename(w.rstrip('/'))}: jt9 {len(J)}  ours {len(O)}  both {len(both)} ({100 * len(both) / max(1, len(J)):.1f}%)  ours-only {len(false)}   [{timing}]")
        for k in sorted(false): print(f"    ours only: slot {k[0]} {O[k]} {k[1]}")
        tj += len(J); to += len(O); tb += len(both); tf += len(false)
        if both:
            import statistics as st
            dd = lambda i: [O[k][i] - J[k][i] for k in both]
            print(f"    ours-jt9: SNR mean {st.mean(dd(0)):+.1f} sd {st.pstdev(dd(0)):.1f} dB | DT mean {st.mean(dd(1)):+.2f} sd {st.pstdev(dd(1)):.2f} s | freq mean {st.mean(dd(2)):+.1f} Hz")
        for k, v in J.items():
            i = 0 if v[0] > -10 else 1 if v[0] > -15 else 2 if v[0] > -20 else 3; bins[i][1] += 1; bins[i][0] += k in O
    print(f"TOTAL: jt9 {tj}  ours {to}  both {tb} ({100 * tb / max(1, tj):.1f}%)  ours-only {tf}")
    print("  by jt9 SNR:  " + "   ".join(f"{n} {b[0]}/{b[1]} ({100 * b[0] / max(1, b[1]):.0f}%)" for n, b in zip(("> -10", "-10..-15", "-15..-20", "<= -20"), bins)))

if __name__ == "__main__": main()
