"""gen_hair_tables.py — extend PpbHairTablesAll.inc to cover EVERY uncovered SMP hair rig
in the enabled load order. Replaces the lost gen_all.py; rules re-derived from the shipped
.inc itself (verified against anchor.xml / angelic.xml):

  * constraints are <generic-constraint bodyA=CHILD bodyB=PARENT>
  * a strand = a maximal parent chain of non-skeleton bones
  * DROP THE ROOT CHORD (bone1->bone2): the root sits at the skull and a capsule there clips
    the head. anchor.xml (4 chords in xml -> 3 in table) and angelic.xml (3/side -> 2) both
    confirm this.
  * SENSORS FIRST: the tip-most chord of each strand, longest strands first, cap 14
    (sensors are the rows that publish FSMP push force; NpcFingerTest reads rows 0..sensors-1)
  * sig = host bone of the first row; ResolveNodes then verifies the whole table before binding
  * hard cap 200 chords/table (kMaxChords) — keep whole strands, longest first

Byte-safe CRLF writes (ledger: never round-trip these sources through Python text mode).
"""
import pathlib, re, collections, sys

MODS = pathlib.Path(r"D:/Games/My Skyrim/mods")
PROF = pathlib.Path(r"D:/Games/My Skyrim/profiles/Version 1/modlist.txt")
INC  = pathlib.Path(r"G:/Claude Workspace/tools/PPB-plugin/src/PpbHairTablesAll.inc")
SKIPS = pathlib.Path(r"G:/Claude Workspace/tools/smp_hair_skips.txt")
KMAX, SENS_CAP = 200, 14

inc_txt = INC.read_text(encoding="utf-8", errors="replace")
tabled  = {m.strip().lower() for m in re.findall(r"//\s*([^\n/]+?\.xml)", inc_txt)}
skips   = {l.split("\t")[0].strip().lower() for l in SKIPS.read_text(errors="replace").splitlines() if l.strip()}
cur_cnt = int(re.search(r"constexpr int kHairCount = (\d+);", inc_txt).group(1))

enabled = [l[1:].strip() for l in PROF.read_text(encoding="utf-8", errors="replace").splitlines()
           if l.startswith("+")]

# collect candidate rig xmls from enabled mods, first-seen wins (dedup by filename)
cands = {}
for m in enabled:
    d = MODS / m
    if not d.is_dir(): continue
    for x in d.rglob("*.xml"):
        s = str(x).lower().replace("\\", "/")
        # HAIR DIRECTORIES ONLY. A broad smp/hdt filter also matches dresses, robes, chains,
        # hats, tongues and ppbHands.xml — and the ledger records that the DX Necromancer DRESS
        # rig was RETIRED because a near-massless garment body grabbed by HIGGS blew up the
        # Havok solver and froze the engine. Never auto-generate capsules for non-hair rigs.
        if not any(k in s for k in (
            "ks hairdo's/hdt/xml", "ks hairdo's/hdt_dint999", "ks hairdos smp for men",
            "xsummer/s4 hair", "sghairs/hdt_dint999", "fuse00hair", "fuse00/hairpack",
            "character assets/hair/smp", "/txml/")): continue
        n = x.name.lower()
        if n in tabled or n in skips or n in cands: continue
        cands[n] = x

def chains_of(xml):
    try:
        d = xml.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return []
    cons = re.findall(r'<generic-constraint(?:-default)?\s+bodyA="([^"]+)"\s+bodyB="([^"]+)"', d)
    parent = {}
    for child, par in cons:
        if child.startswith("NPC "): continue
        parent[child] = par
    kids = collections.defaultdict(list)
    for c, p in parent.items(): kids[p].append(c)
    roots = [p for p in kids if p not in parent]          # parent that is itself unparented
    out = []
    for r in roots:
        for first in kids[r]:
            chain, cur = [r, first], first
            while len(kids.get(cur, [])) == 1:
                nxt = kids[cur][0]
                if nxt in chain: break
                chain.append(nxt); cur = nxt
            if len(chain) >= 3:                            # >=3 bones => >=1 chord after root drop
                out.append(chain)
    out.sort(key=len, reverse=True)
    return out

new_tables, new_rows, report = [], [], []
idx = cur_cnt
for name in sorted(cands):
    xml = cands[name]
    ch = chains_of(xml)
    if not ch:
        report.append((name, 0, 0, "no parseable chain")); continue
    kept, tot = [], 0
    for c in ch:
        n = len(c) - 2                                     # chords minus the dropped root chord
        if n < 1 or tot + n > KMAX: continue
        kept.append(c); tot += n
    if not kept:
        report.append((name, 0, 0, "all chains too short/over cap")); continue
    sens = [(c[-2], c[-1]) for c in kept[:SENS_CAP]]       # tip-most chord per strand
    skey = set(sens)
    rows = []
    for si, c in enumerate(kept[:SENS_CAP]):
        lbl = (c[0][-12:] if len(c[0]) > 12 else c[0])
        rows.append((c[-2], c[-1], f"{lbl} {si+1}.{len(c)-2}", True))
    for si, c in enumerate(kept):
        lbl = (c[0][-12:] if len(c[0]) > 12 else c[0])
        for k in range(1, len(c) - 1):                     # start at 1 => root chord dropped
            if (c[k], c[k+1]) in skey: continue
            rows.append((c[k], c[k+1], f"{lbl} {si+1}.{k}", False))
    sig = rows[0][0]
    nm = f"kHair{idx}"
    body = "".join(f'  {{ "{h}", "{t}", "{l}" }},{"  // sensor" if s else ""}\r\n' for h,t,l,s in rows)
    new_tables.append(f"constexpr FingerPair {nm}[{len(rows)}] = {{ // {name}  sig={sig}\r\n{body}}};\r\n")
    new_rows.append(f'  {{ "{sig}", {nm}, {len(rows)}, {len(sens)} }},  // {name}\r\n')
    report.append((name, len(kept), len(rows), f"sig={sig}"))
    idx += 1

for n, s, c, note in report:
    print(("  %-34s strands=%3d chords=%4d  %s" % (n,s,c,note)).encode("ascii","replace").decode())
print(f"\ncandidates={len(cands)}  generated={len(new_rows)}  kHairCount {cur_cnt} -> {cur_cnt+len(new_rows)}")

if new_rows and "--dry" not in sys.argv:
    raw = INC.read_bytes()
    marker = b"constexpr int kHairCount = %d;" % cur_cnt
    assert marker in raw, "kHairCount marker moved"
    blob = ("\r\n// === auto-coverage pass 2026-08-07 (gen_hair_tables.py) ===\r\n"
            + "".join(new_tables)).encode("utf-8")
    raw = raw.replace(marker, blob + b"constexpr int kHairCount = %d;" % (cur_cnt+len(new_rows)), 1)
    last = raw.rindex(b"};")
    raw = raw[:last] + "".join(new_rows).encode("utf-8") + raw[last:]
    INC.write_bytes(raw)
    print("INC updated.")
