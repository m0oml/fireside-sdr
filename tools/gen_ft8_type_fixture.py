#!/usr/bin/env python3
"""Regenerates app/tests/ft8/ft8code_message_types.txt: WSJT-X's own 77-bit encoding (`ft8code`, apt install wsjtx) of contest / DXpedition / EU VHF messages,
with the text WSJT-X decodes them to. The C++ test unpacks every line and must reproduce that text. One line per message: <77 bits> TAB <expected text>.
Hashed callsigns: ft8code resolves them (it hashed them while encoding); our test starts with an empty table, so those lines carry '<...>' for them."""
import re, subprocess, sys, os
sys.path.insert(0, os.path.dirname(__file__))
src = open(sys.argv[1]).read() if len(sys.argv) > 1 else None          # optional: packjt77.f90, for the section / multiplier lists
def table(name):
    lines = src.split("\n"); i = next(k for k, l in enumerate(lines) if re.search(r"^\s*data %s/" % name, l)); out = []; k = i
    while True:
        out += re.findall(r'"([^"]*)"', lines[k])
        if lines[k].rstrip().endswith("/"): break
        k += 1
    return [x.strip() for x in out]
msgs = []
if src:
    for sec in table("csec"): msgs.append(("W1AW K1ABC 1A %s" % sec, None))
    for mult in table("cmult"): msgs.append(("TU; W9XYZ K1ABC R 579 %s" % mult, None))
msgs += [(m, None) for m in ["WA9XYZ KA1ABC R 16A EMA", "WA9XYZ KA1ABC 6A EMA", "W1AW K1ABC R 32F DX", "W1AW K1ABC 17B WWA", "W1AW K1ABC R 1A AB", "W1AW K1ABC 16F WY",
    "W9XYZ K1ABC 579 0123", "W9XYZ K1ABC R 599 7999", "TU; W9XYZ K1ABC 529 0001", "K1ABC W9XYZ 539 X07", "K1ABC W9XYZ R 549 NWT", "TU; K1ABC W9XYZ R 569 PEI",
    "K1ABC RR73; W9XYZ <KH1/KH7Z> -12", "K1ABC RR73; W9XYZ <KH1/KH7Z> +05", "K1ABC RR73; W9XYZ <KH1/KH7Z> -30", "K1ABC RR73; W9XYZ <KH1/KH7Z> +00",
    "<PA3XYZ> <G4ABC/P> R 590003 IO91NP", "<PA3XYZ> <G4ABC/P> 520001 JO22AB"]]
out = []
for m, _ in msgs:
    o = subprocess.run(["ft8code", m], capture_output=True, text=True).stdout.split("\n")
    row = o[2]
    if "bad message" in row: print("skipped (ft8code rejects):", m); continue
    bits = o[5].strip()
    cols = re.split(r"\s{3,}", row.split(". ", 1)[1].strip()); text = cols[1].strip() if len(cols) > 1 else cols[0].strip()
    text = re.sub(r"<(?!\.\.\.>)[^>]*>", "<...>", text) if ("RR73;" in text or text.startswith("<")) else text    # hashed calls are unknown to a fresh decoder
    assert re.fullmatch(r"[01]{77}", bits), (m, bits)
    out.append("%s\t%s" % (bits, text))
open(os.path.join(os.path.dirname(__file__), "..", "app", "tests", "ft8", "ft8code_message_types.txt"), "w").write("\n".join(out) + "\n")
print(len(out), "messages")
