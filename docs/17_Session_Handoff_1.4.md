# 17 — SESSION HANDOFF: PPB 1.4 (2026-07-31 → 08-01)

**READ THIS FIRST after a context compaction.** Written deliberately mid-session because context
was about to compact with three problems open. Everything below is verified from logs, code or
git — nothing here is remembered.

---

## 0. STATE AS OF THIS WRITING

| | |
|---|---|
| Deployed DLL | **`d2b7e196`** in `D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB.dll` |
| `PPB_Touch.pex` | `871ba159` (16 natives) |
| GitHub HEAD | `649c7f5` — everything below is pushed |
| Version | plugin reports **1.4.0**, `GetBuildNumber()` = **10400** |
| Dev-machine knobs | `apiLog 1`, `logVerbose 1`, `contactLog = 0`, `apiDwell* 0.25`, `apiWeaponRMaxU 6` |
| MO2 | `Precision Physic Bodies` (dev) enabled; `Precision Physic Bodies V1.3` + `Precision V1.1` kept as **known-good reference binaries** — do not delete, they partitioned a two-day bug in one test |

---

## 1. ⚠ THE THREE OPEN PROBLEMS

### 1.1 ReShape sampler NEVER RUNS — highest priority, possibly a regression I caused

**Symptom (user, in VR):** Carmella's face is ~3u too low. User asked "why didn't she get ReScale?"

**Measured from the log, whole session, ALL actors:**
- `HEADUV` = 0 · `UVMEAS` = 0 · `MESHGIRTH` = 0 · `BONEGIRTH` = 0 · `DECOY` = 0 · latch receipts = 0
- `logVerbose 1`, so these are NOT hidden by log level
- Both gates ON: `lmReShape 1`, `boneShape 1`
- **CapFix itself is healthy** — 234 lines, 102 `APPLIED` on her incl. the COM sensors, all `gen=1`
- **ReScale is NOT broken** — corrected 2026-08-01 from a later log: it completes fine
  (`1301DE4F nodeArc=45.710 medHavokArc=43.446 -> factor x1.0521 trueScale=0.9514`, applied to 17
  constraints). The earlier "armed but never completed" reading was a too-short log window. **The
  fault is ReShape ONLY.**

**The gate** (`CapFix.cpp` ~line 2116):
```cpp
if (ObjectHold::LmReShapeEnabled() && ObjectHold::BoneShapeEnabled()) {
    if (!IsBeastSkeletonActor(actor))
        SampleHeadNose(actor, id, d);      // the only caller
```

**Two hypotheses, NOT yet distinguished:**
1. **Stale latch** — the sampler only runs on a fresh latch, so "already latched" and "latch failing"
   look identical in the log. (Doc 04: *"It only fires on a fresh latch, so an empty grep means 'no
   latch happened', not 'no data'."*)
2. **A regression from today's `CapFix.cpp` edits** — I added `SlotBodyRaw`, the LIST-AABB weapon
   tier, and reworked the blade radius, all in that file. A sampler that runs for NOBODY is exactly
   the shape of an accidental break.

**A GATE DIAGNOSTIC IS NOW DEPLOYED.** `RESHAPEGATE <id>: lmReShape= boneShape= attached= latched=
skee= hasMorphs= attachedTicks= beast= dead= excluded=` prints once per actor at the latch decision.
Whichever field reads false is the answer — read that line first, it replaces all guessing below.

**THE CHEAP DISCRIMINATOR (if the gate line is inconclusive):** `bodyScaleDump` is **edge-triggered** — any value
change re-latches everyone. It currently reads **21**. Change it to 22 and watch:
- `HEADUV` lines appear + her face corrects → it was a stale latch, no code bug.
- Still nothing → the sampler is genuinely broken; `git diff 41095aa..HEAD -- src/CapFix.cpp` is
  the suspect list.

⚠ Do NOT "fix" this by disabling anything — ledger rule: fix the math, never disable the feature.

### 1.2 ENG vs GEO weapon coverage — instrumented, never read

Weapon contacts come from two probes (see §3.3). Observed split:
- **Face / neck / chest / under-jaw → all `src=GEO`**
- **Waist / thigh / calf → all `src=ENG`**

Ratios seen: one session 36 ENG/16 GEO, the next 8 ENG/19 GEO on weapon lines. Consistent *within*
an area, so it is not noise.

**Two diagnostics are BUILT AND DEPLOYED but have never been read** (they need one slow rapier pass
from her face down to her feet):
- `API weapon ENGINE-CONTACT coverage after N: hand= fore= uarm= head= spn0= … ` — per-slot running
  totals, dumped every 64 engine contacts. **A zero for head/neck/spn2 means Havok genuinely does
  not report weapon contacts there → not a bug, it is the design.** Non-zero means something
  downstream loses them.
- `API weapon ENG-vs-GEO agreement over N samples: mean |diff| X u, max Y u` — dumped every 64.
  Decides whether the "metric discontinuity" I worried about is real. If small, mixing is free.

**Not a release blocker:** GEO is a good fallback since the thin-cross-section fix, part names are
right either way, and `engineContact` tells consumers which they got.

### 1.3 Packaging 1.4.0 — not started

1. `apiLog` → **0** in the shipped tuning (dev-only; the packaging pre-flight ABORTS if it ships at 1)
2. `logVerbose` → 0
3. FOMOD + zip strings → 1.4.0 (`tools/ppb-scratch/package_release.py`, `OUT` path still says 1.3.0)
4. Run the pre-flight (it checks 10 dev diagnostics + 7 expect-on knobs)
5. `NEXUS_DESCRIPTION_1.4.txt` is written and ready to paste

⚠ Before publishing, re-check the Nexus line *"Weapon contacts come from the game's own physics"* —
if §1.2 shows coverage is genuinely partial, soften it.

---

## 2. WHAT SHIPPED IN 1.4 (all verified in VR unless noted)

**The Touch API** — five variables plus extras, three access paths (mod events / 16 Papyrus natives
/ C++ `IPpbTouchInterface1`, 16 vtable slots, append-only, no virtual destructor).

Contact payload: WHO · WHERE (region → sub-region → capsule, + depth 0–3) · BY WHO (wand) · WITH
WHAT (finger/palm/fist/grab/weapon/object) · weapon class + edge · DURATION · signed `distU` ·
`engineContact` · skeleton.

Verified in VR: FINGER/PALM/FIST/GRAB/WEAPON/OBJECT, both hands in parallel, mouth gate
LIPS→ENTER→THROAT (`Face(throat wall)` −0.59u over 3.32s), the intimate depth chain
(CLITORIS → vaginal opening → cervix → uterus), tails base/mid/tip, held-hand suppression,
weapon class `Sword/Blade` on a non-vanilla Iron Rapier.

**Fixes this session** (all VR-confirmed): the SMP handshake steal (§3.1), the Heels Fix spell
flicker (§3.2), wig-reported-as-tail, weapons with no OBND, engine-truth weapon contacts, the
digest boundary flap, empty `OBJECT:` names, plugin version stale at 0.1.0.

---

## 3. THE THREE THINGS THAT COST THE MOST — do not re-learn

### 3.1 ★ SKSE stores ONE listener per (plugin, sender) — the wildcard is a LAND GRAB

`RegisterListener(nullptr, h)` does **not** mean "wildcard listener". Per SKSE's own
`PluginManager.cpp` it **inserts the caller into EVERY loaded plugin's listener list**, and a later
NAMED registration finds the caller already present and **`return true`s without storing the new
handler**. So the touch API's wildcard silently stole the SMP engine's slot: FSMP's `MSG_STARTUP`
was delivered *to the touch-API handler*, which filtered on its own type and dropped it. Hair push
died outright; tails limped on the SMP-native `ppbHands.xml` collider (which needs no interface).

**Fix in place:** ONE registration, ONE handler (`OnAnyPluginMessage` in `main.cpp`) fanning out to
`FsmpLink::HandleEngineMessage` + `PpbApi::OnPluginMessage`, re-asserted at `kPostLoad`.
**Any future `RegisterListener` addition must go into the fan-out, never beside it.**

Also corrected: SKSE's sender lookup is **case-SENSITIVE** (`hdtSMP64` returns "no such plugin" on
an FSMP install). The old ledger note claiming case-insensitive was wrong.

### 3.2 Heels Fix's refresh FLICKERS its ability spell

Its periodic stuck-heel check does `RemoveSpell → Wait(0.5) → re-add`. Gating per-frame on
`HasSpell` inherits that hole, and if the Papyrus re-add loses its race the spell stays off for
minutes while the NiOverride node offset persists. Fix: the spell is a **one-time introduction per
actor**; afterwards the node offset is the authority (`HeeledSticky`).
⚠ **The heel fix is TWO HALVES that must read the SAME gate** — the drive bias and the postPhysics
compensation. I converted one and left the other, and her whole body floated 8u.

### 3.3 The two weapon probes answer DIFFERENT questions

- **GEO** — "is the weapon within `apiTouchU` of this capsule?" Proximity, from an approximated
  weapon (segment + radius). Deliberately includes **hover**. Covers capsules the engine never reports.
- **ENG** — "did Havok collide these shapes?" The engine's narrowphase against the weapon's REAL
  geometry; gives the exact capsule via `GetShapeKeys(n)[0]` (**= the list-child index**) and a true
  separating distance (in `separatingNormal.w`, read as a raw integer dword on the collision thread
  and bit-cast on the main thread, to honour the pure-integer rule).

Policy: **ENG owns identity whenever it exists**, its distance preferred but cross-checked against
the geometry of the same capsule (>6u disagreement → use GEO depth, keep engine identity).

⚠ The contact listener is armed by `apiTouch` — it used to install only under the `perf` counter,
so the whole engine path was **dead code in ordinary play** for a full test cycle.

---

## 4. METHOD LESSONS FROM THIS SESSION

1. **A known-good BINARY beats any amount of theory.** The user A/B'd `Precision V1.1` and released
   `1.3` against the dev build and partitioned a two-day bug in one test, after I had been wrong
   four times in a row. Keep old release folders for exactly this.
2. **The user's description of DEGREE is data.** "Tails react but not as good as before" pointed
   straight at "one of two channels is dead". I explained it away instead of enumerating channels.
3. **Never tell the user their observation is wrong.** They felt the tail move; they were right and
   I argued. Enumerate mechanisms that could produce what they saw.
4. **A capped diagnostic that reaches its cap has stopped being a measurement** — hit FOUR times in
   this project, twice today. Use running totals / histograms, never a first-N cap.
5. **My own tooling failure is not an external limit.** I declared "no network" because my curl
   used wrong paths and a nonexistent directory; the source was one correct URL away and the user
   called it out.
6. **Verify what you are standing on is running** before building on it (the dead contact listener).

---

## 5. REMAINING VR TEST PROTOCOL

1. **Bump `bodyScaleDump` 21 → 22**, watch Carmella's face (§1.1 discriminator). ← do first
2. **Slow rapier pass face → feet**, then read the coverage + agreement lines (§1.2)
3. `contactLog = 1` in the ini with `apiLog 0` — proves the new user-facing switch works
4. Optional/low risk: grab-mute (hold her leg with the weapon hand → expect zero weapon contacts),
   save/load cycle

---

## 6. KEY FILE MAP (for a fresh agent)

| What | Where |
|---|---|
| API engine | `tools/PPB-plugin/src/PpbApi.cpp` (~1400 lines) |
| Consumer contract | `tools/PPB-plugin/src/PpbTouchAPI.h` |
| SMP handshake | `tools/PPB-plugin/src/FsmpLink.cpp` + `main.cpp` `OnAnyPluginMessage` |
| Contact listener / engine hits | `tools/PPB-plugin/src/Diag.cpp` (`NoteWeaponContact`, `DrainWeaponContacts`) |
| Weapon segment fallback | `tools/PPB-plugin/src/CapFix.cpp` `WeaponSegmentU` |
| ReShape sampler (**BROKEN, §1.1**) | `tools/PPB-plugin/src/CapFix.cpp` `SampleHeadNose` ~1126, gate ~2116 |
| Heel fix | `tools/PPB-plugin/src/PPBHook.cpp` (`HeeledSticky`, both halves) |
| Garment classification | `tools/PPB-plugin/src/NpcFingerTest.cpp` `IsTailTable` |
| Modder guide | `tools/ppb-repo-work/INTEGRATION.md` |
| Nexus text | `tools/ppb-repo-work/NEXUS_DESCRIPTION_1.4.txt` |
| Capsule workbook | `Report/…/PPB_Touch_API_Contact_List.xlsx` (generated — never hand-edit) |
| Packaging | `tools/ppb-scratch/package_release.py` |

**Build:** `cd tools/PPB-plugin && "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build/vr --config Release`
— auto-copies to the mod folder; **fails with "Permission denied" while the game runs.** Always
verify with `md5sum` on both paths; the copy step's failure message does NOT contain "error".
