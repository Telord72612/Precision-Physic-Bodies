#!/usr/bin/env python3
"""Correct the knob prefixes in capsule_body_part_map.json against PPB_tuning.txt.

The record carried three invented prefixes (capForearm / capUpperarm / capSpine); the real
knobs are capFore / capUpper / capSpine0 (Tuning.cpp: PK8(capFore), PK8(capUpper), ...).
Every prefix written here is proven by the existence of "<prefix>Enable" in the shipped
tuning file, and every per-child knob by the existence of that exact key.
"""
import json, re, sys
from pathlib import Path

TUNE = Path(r"D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB_tuning.txt")
TARGETS = [Path(r"G:/Claude Workspace/Report/Precision Physic Bodies Module/capsule_body_part_map.json"),
           Path(r"G:/Claude Workspace/tools/ppb-repo-work/docs/capsule_body_part_map.json")]

declared = set(re.findall(r"^\s*([a-zA-Z]\w+)\s+[-\d.]", TUNE.read_text(errors="ignore"), re.M))
valid_prefixes = {k[:-len("Enable")] for k in declared if k.endswith("Enable") and k.startswith("cap")}

FIX = {"capForearm": "capFore", "capUpperarm": "capUpper", "capSpine": "capSpine0"}

for path in TARGETS:
    if not path.exists():
        print(f"skip (absent): {path}"); continue
    d = json.loads(path.read_text())
    changed, unproven = 0, []
    for s in d["slots"]:
        pre = FIX.get(s["knobPrefix"], s["knobPrefix"])
        if pre not in valid_prefixes:
            unproven.append(f'slot {s["slot"]} prefix {pre} (no {pre}Enable in PPB_tuning.txt)')
            continue
        if s["knobPrefix"] != pre:
            s["knobPrefix"] = pre; changed += 1
        for ch in s["children"]:
            want = pre if ch["child"] == 0 else f'{pre}C{ch["child"]}'
            if f"{want}Enable" not in declared and f"{want}AX" not in declared:
                unproven.append(f'slot {s["slot"]} child {ch["child"]}: {want} not declared')
                continue
            if ch.get("knob") != want:
                ch["knob"] = want; changed += 1
    if unproven:
        sys.exit("ABORT - could not prove against PPB_tuning.txt:\n  " + "\n  ".join(unproven))
    d["knobPrefixNote"] = ("knobPrefix and child knob values are PREFIXES: the real tuning keys are "
                           "<knob>Enable / <knob>AX / AY / AZ / BX / BY / BZ / R. Verified present "
                           "in the shipped PPB_tuning.txt.")
    path.write_text(json.dumps(d, indent=1) + "\n")
    print(f"{path.name}: {changed} knob field(s) corrected, all verified")
