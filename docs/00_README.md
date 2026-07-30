# 00 — Precision Physic Bodies (PPB): README & Reference Index

## What this project is
**Remake Skyrim VR's 2011 actor physics bodies (Havok ragdoll) and CBPC touch zones so they track
the XP32 animation skeleton perfectly — joints on the rig's joints, capsules spanning joint-to-joint
— adapting at runtime to scale, armor, and body morphs.** Every animation and body mesh in the load
order targets XP32; the physics layers never followed it, until this. Grew out of AIHands (NPC
hand/give mod) when bad collision sabotaged touch, now standalone.

## The mod & boundary doctrine
- **Ship mod:** `D:/Games/My Skyrim/mods/Precision Physic Bodies/` — a pure asset replacer that owns
  ALL body geometry (Havok + CBPC, static + dynamic). Owns `meshes/actors/character/character
  assets female/skeleton_female.nif` (+ the beast/orc/elf variants). **NO plugin** except a tiny
  ESL-flagged `Precision Physic Bodies.esp` for the beast-race skeleton split. Keep it enabled in
  MO2 above other skeleton mods. VRTouchEvents and AIHands are CONSUMERS (they depend on PPB).
- **The runtime tooling is `PPB.dll`** (`tools/PPB-plugin/`, deploys into the mod): heel fix, CapFix,
  PivFix/auto-seat/clavicle-follow, ReScale, ReShape, the follower rigs, the skeleton map, statue
  spell, consoles `hf`/`capfix`. **Only build with the game closed** (Windows locks a loaded DLL).
- The trf/trg visualizers live in the separate **Collision Visualizer VR** mod (`collviz_markers.dll`).

## The three-layer model (the frame for everything)
1. **Visual NiNodes** — where the heel offset lives.
2. **Behavior-graph pose** — the animated bone transforms (what the drive reads).
3. **Havok bodies** — the ragdoll rigidbodies + constraints PPB rebuilds and drives onto layer 2.
Scale, shape, and pose corrections all reconcile these three so collision matches what you see.

## Reference index (read order for a fresh agent)
| # | Doc | Read when |
|---|---|---|
| 01 | **Pitfall Ledger** | ALWAYS — the expensive lessons; do not re-learn any |
| 02 | **Tools & Knobs** | operating the workshop (PPB_tuning.txt, consoles, logs) |
| 03 | **Scale & Calibration** (ReScale + self-correcting system) | any NPC size / offset / "why is this NPC wrong" question |
| 04 | **Body Shape: ReShape** | capsules fitting body shape (OBody presets, custom bodies) |
| 05 | **OBody / SKEE Integration** | reading morphs, OBody NG API, spawn bases, the slider fallback |
| 06 | **Flesh-Fit Dialing & Bake** | live-dialing capsules by voice + baking into the NIF |
| 07 | **Follower & Garment Capsules** | tails / hair / cloth / finger rigs (SMP interop, always-on) |
| 08 | **Contact & Performance** | the many-capsule contact-cliff (understood, benign) + perf |
| 09 | **Player Hand & Finger Colliders** | HIGGS hand colliders + the Havok-vs-CBPC/SMP touch split |
| 10 | **Runtime Skeleton Map & Per-NPC** | per-race skeletons (no-ESP) + per-NPC capsule overrides |
| 11 | **Ecosystem Conflict Forecast** | before updating PLANCK / HIGGS / FSMP / XP32 |
| 12 | **Deep Reference** (toolchain / impact sound / dismemberment) | working a specific subsystem's internals |
| 13 | **ReTouch: Havok-Native Touch** | touch detection, mouth gate, held-object identity |
| 15 | **Capsule → Body-Part Map** | naming every touchable capsule for an API (+ capsule_body_part_map.json) |
| 14 | **Dismemberment & Decapitation Compat** | DF/NGD freezes, severed heads, head-only skeletons, mass/sound |

## Current state (2026-07-19)
The capsule-fitting stack is complete and four-body certified:
- **ReScale (03)** — arc-sum pose-invariant scale, joints + capsules unified, females-only bake gate,
  event-driven. `trueScale = nodeArc / 48.045`.
- **ReShape (04)** — measures each NPC's live morphed body mesh in scale-free space, divides by her
  body-type's neutral, applies the shape ratio exactly like ReScale (endpoints + radius, lateral).
  Universal (no per-race/per-NPC shape shims); OBody slider layer (05) is the fallback.
- **Follower rigs (07)** — tails/hair/cloth auto-detect per NPC, collide + player-push, silent-material.
- **Runtime skeleton map (10)** — per-race skeleton repoints + per-NPC capsule overrides, no ESP.
- Contact cliff (08) confirmed benign in-VR; perf tax ~+0.06ms/step (+7%, ~1% of the 90 Hz budget).

## ★ Per-race geometry BY EXISTENCE (the beast-split payoff)
`CapFix`'s child-apply loop is bounded by the ACTOR's OWN child count — a child that doesn't exist
in a race's NIF can never be written (the DLL logs `"N children live but K knob blocks — extra
skipped"`). So **extra list children in one race's skeleton NIF are exclusive to that race**, even
though the `cap*C*` knobs are global. As-built child counts:
| NIF | head | spine0 | spine1 | spine2 | carries |
|---|---|---|---|---|---|
| `skeleton_female.nif` (human/elf/orc/M'rissi/Auri/half-dragons) | **23** | 8 | 7 | 15 | C1..14 human skull + **C15..22 = 8 head-joint seeds** (horns/antlers, buried) |
| `skeleton_female_draenei.nif` (Yvanni, via race map) | **23** | 8 | 7 | 15 | byte-copy of human — her horn/hoof dial+bake target |
| `skeletonbeast_female_khajiit.nif` | 17 | 8 | 7 | 15 | C15/C16 = ears (baked) |
| `skeletonbeast_female.nif` (Argonian) | 17 | 10 | 9 | 17 | horns + dorsal ridge (baked) |
`kMaxListChildren` 24 = ceiling. ⚠ Growing a list needs **all four coupled edits** (Ledger): the
`Tuning.h` array size, `kArrays` count, `CapFixChildKnobs(slot)`, `CapFixChildSlot`'s `ChildPtr`
bound — UNLESS the extra children are seed-only (no knobs), driven per-NPC via npcCap: ApplyListSlot
loops the actor's real child count and leaves a no-knob/no-npcCap child as its buried seed
(CapFix.cpp:1409-1437), so **the 8 head seeds needed NO DLL rebuild** (2026-07-19). All THREE
skeleton heads are baked (human/khajiit/argonian); elf+orc share the human head (their separate NIFs
DELETED 2026-07-19); `capHead*` knobs OFF. ⚠ The human head now HAS children 15/16 too, so the
dormant `capHeadC15/C16` ear/horn knobs can land on humans — beast head re-dials MUST sculpt-gate.

## Critical numbers & paths
- Units: **havok m = game u × 0.0142875** (÷ ≈ 69.99 back). Live shape = NIF × GetScale (per-actor clones).
- XP32 bind lengths: forearm→hand 16.05u · upperarm→forearm 22.83u · clavicle→upperarm 14.04u.
- Tuning: `Precision Physic Bodies/SKSE/Plugins/PPB_tuning.txt` (hot-polled ~1 Hz). Skeleton map:
  `PPB_skeletons.txt` (parsed ONCE at kDataLoaded — needs a RESTART per change).
- Log: `My Games/Skyrim VR/SKSE/PPB.log` (truncates each launch). Offline scripts + scratch:
  `tools/ppb-scratch/`, `tools/pynifly/`. Source vendored at `tools/_research/` (higgs, planck_src, collviz_src).
- SMP garment-binding operator runbook: `Report/Collision Capsule SMP binding procedure/`.

## ⚠ Renumber map (2026-07-19 consolidation: 27 files → 13)
Legacy docs and memory notes cite the OLD report numbers. Translate:
| old | now | old | now |
|---|---|---|---|
| 09 FleshFit | **06** | 16 Calibration | **03** (Part B) |
| 10 Hand colliders | **09** | 17 + 17A Conflict | **11** |
| 11 + 12 + 13 Contact | **08** | 18 Orphan/scale rework | **03** + 01 (superseded) |
| 14 SMP interop | **07** (Part B) | 19 X/H scale | **03** (superseded, dropped) |
| 15 Follower spec | **07** (Part A) | 20 Arc-sum ReScale | **03** (Part A) |
| 06 BoneFollow | **07** (Part C) | 21 ReShape | **04** |
| 06 Orig toolchain / 07 Impact / 07 Dismember | **12** | 22 Skeleton map | **10** |
| 04 Pitfalls | **01** | 23 OBody/SKEE | **05** |
Dropped (narrative/stale/superseded, durable facts folded into the above + this README): old 01
Chronicle · 03 Systems-AsBuilt · 05 Roadmap · 08 Standalone-Split · 19.
Internal "Report NN" cites inside the merged docs (Parts of 03/07/08/11/12) still use old numbers —
use this map.

## This folder is REFERENCE, not a track record
Full session history lives in the auto-memory (`ragdoll_remake_project.md`). Keep it that way: new
findings become reference edits to the doc that owns the topic, not a new dated banner.

- **14_Dismemberment_Decapitation_Compat** — DF/NGD/PLANCK/FSMP: the VR hard-freeze root cause and
  the API-based fix, the severed-head clone architecture, head-only skeleton authoring, the
  mass/inertia/friction field map, and the foreign-DLL hooking technique.
- **13_ReTouch_Havok_Native_Touch** — the touch layer: CBPC replaced by pure-geometry detection on the live capsules; mouth gate + phoneme reactions; held-object identity; the marker-seizure laws.
