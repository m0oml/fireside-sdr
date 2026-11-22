#!/usr/bin/env python3
"""Regenerates app/core/src/dxcc_table.inc from cty.dat (Jim Reisert AD1C, https://www.country-files.com/ , freely used by logging software including WSJT-X).

  curl -o cty.dat https://www.country-files.com/cty/cty.dat
  tools/gen_dxcc.py cty.dat

Only derived numbers are kept: for every callsign PREFIX (exact-call "=" entries are skipped) the entity's centre (lat, lon; cty.dat's longitudes are positive WEST, flipped here
to east) and a radius in km: how far from that centre a station of this entity can plausibly be. The default is generous; big entities have their own. Used only to reject
a weak decode whose locator is impossible for its callsign's country (decode_ft8.cpp), and the entity's NAME (the PSK Reporter tab's country column).
Also kept, for the decoder's "new country / zone / continent" tags: the continent, CQ zone and ITU zone (with cty.dat's per-prefix overrides), and the DXCC entity the
prefix counts for (cty.dat's WAE-only entities, marked "*", count for their parent: Sicily -> Italy). Exact-call "=" entries go to a second table (dxcc_exact.inc), used only
for those tags, never by the decoder's locator check."""
import re, sys

# radius (km) from the entity centre that still counts as "in the country". cty.dat splits many far-flung parts into their own entities already (Hawaii, Alaska, Asiatic Russia...).
BIG = {"United States": 3300, "Canada": 5200, "European Russia": 3200, "Asiatic Russia": 5500, "Brazil": 3600, "Australia": 4200, "China": 4200, "Argentina": 3200, "Chile": 2900,
       "India": 2800, "Indonesia": 3800, "Mexico": 2800, "Kazakhstan": 2800, "Greenland": 3200, "Algeria": 2200, "Libya": 1900, "South Africa": 1900, "Iran": 1900, "Mongolia": 1900,
       "Japan": 2400, "Peru": 1700, "Colombia": 1600, "Egypt": 1700, "Saudi Arabia": 2000, "Sudan": 1800, "Turkey": 1900, "Alaska": 2800, "Norway": 2200, "Sweden": 1500, "Finland": 1500,
       "Spain": 1400, "France": 1400, "Italy": 1500, "Poland": 1000, "Ukraine": 1400, "Chad": 1500, "Niger": 1500, "Mali": 1700, "Angola": 1500, "Namibia": 1500, "Mauritania": 1600,
       "Democratic Republic of the Congo": 2000, "Bolivia": 1400, "Venezuela": 1500, "Pakistan": 1700, "Myanmar": 1500, "Afghanistan": 1300, "Thailand": 1300, "Vietnam": 1500,
       "Philippines": 1700, "Malaysia": 1700, "New Zealand": 1800, "Papua New Guinea": 1600, "Madagascar": 1300, "Iceland": 800, "Ethiopia": 1300, "Tanzania": 1300, "Nigeria": 1100,
       "Svalbard": 1000, "Antarctica": 99999, "Fed. Rep. of Germany": 800, "United Kingdom": 1000, "Ireland": 500, "England": 600, "Scotland": 700, "Wales": 300,
       "Kaliningrad": 300, "Cyprus": 300, "Malta": 100, "Monaco": 50}
DEFAULT_R = 1500

# cty.dat's WAE / CQ-only entities ("*" before the primary prefix) and the DXCC entity they count for
WAE_PARENT = {"Vienna Intl Ctr": "Austria", "Shetland Islands": "Scotland", "African Italy": "Italy", "Sicily": "Italy", "Bear Island": "Svalbard", "European Turkey": "Asiatic Turkey"}
TOKEN = re.compile(r"^(=?)([A-Z0-9/]+)((?:\([0-9]+\)|\[[0-9]+\]|<[-0-9./]+>|\{[A-Z]+\}|~[-0-9.]+~)*)$")

def overrides(mods, cq, itu, cont):
    m = re.search(r"\(([0-9]+)\)", mods); cq = int(m.group(1)) if m else cq
    m = re.search(r"\[([0-9]+)\]", mods); itu = int(m.group(1)) if m else itu
    m = re.search(r"\{([A-Z]+)\}", mods); cont = m.group(1) if m else cont
    return cq, itu, cont

def parse(path):
    text = open(path, encoding="latin-1").read()
    out = []; exact = []; ents = 0
    for block in text.split(";\n"):                                   # an entity: header line + alias lines, ended by ';'
        block = block.strip()
        if not block or ":" not in block: continue
        lines = block.split("\n"); head = lines[0]
        f = [x.strip() for x in head.split(":")]
        if len(f) < 8: continue
        name, cq0, itu0, cont0, lat, lon = f[0], int(f[1]), int(f[2]), f[3], float(f[4]), -float(f[5])           # lon: cty.dat is positive WEST
        if f[7].startswith("*"):
            if name not in WAE_PARENT: sys.exit("unknown WAE-only entity %r: add it to WAE_PARENT" % name)
            dxcc = WAE_PARENT[name]
        else: dxcc = name
        ents += 1; radius = BIG.get(name, DEFAULT_R)
        for tok in re.split(r"[,\s;]+", " ".join(lines[1:])):
            if not tok: continue
            m = TOKEN.match(tok)
            if not m: continue
            eq, pre, mods = m.group(1), m.group(2), m.group(3)
            cq, itu, cont = overrides(mods, cq0, itu0, cont0)
            if eq:
                if pre != "VERSION": exact.append((pre, cont, cq, itu, dxcc))
                continue
            la, lo, r = lat, lon, radius
            ov = re.search(r"<([-0-9.]+)/([-0-9.]+)>", mods)
            if ov: la, lo, r = float(ov.group(1)), -float(ov.group(2)), min(radius, DEFAULT_R)
            out.append((pre, round(la, 2), round(lo, 2), r, name, cont, cq, itu, dxcc))
    return out, exact, ents

def esc(t): return t.replace("\\", "").replace('"', "'")

def main():
    rows, exact, ents = parse(sys.argv[1] if len(sys.argv) > 1 else "cty.dat")
    best = {}
    for row in rows: best.setdefault(row[0], row[1:])      # a prefix listed twice: the first (primary) entity wins
    items = sorted(best.items())
    with open("app/core/src/dxcc_table.inc", "w") as f:
        f.write("// GENERATED by tools/gen_dxcc.py from cty.dat (Jim Reisert AD1C, www.country-files.com). {prefix, lat, lon (east), radius km, entity name, continent, CQ zone, ITU zone, DXCC entity}, sorted by prefix.\n")
        for pre, (la, lo, r, name, cont, cq, itu, dxcc) in items: f.write('{"%s", %.2ff, %.2ff, %d, "%s", "%s", %d, %d, "%s"},\n' % (pre, la, lo, r, esc(name), cont, cq, itu, esc(dxcc)))
    ex = {}
    for row in exact: ex.setdefault(row[0], row[1:])
    with open("app/core/src/dxcc_exact.inc", "w") as f:
        f.write("// GENERATED by tools/gen_dxcc.py from cty.dat's exact-call entries. {callsign, continent, CQ zone, ITU zone, DXCC entity}, sorted by callsign.\n")
        for c, (cont, cq, itu, dxcc) in sorted(ex.items()): f.write('{"%s", "%s", %d, %d, "%s"},\n' % (c, cont, cq, itu, esc(dxcc)))
    print(len(items), "prefixes and", len(ex), "exact calls from", ents, "entities")
main()
