# 07 — Follower & Garment Capsules (tails · hair · cloth · fingers)

**The always-on multi-rig system: any Khajiit/Argonian SMP tail, hair, or dress auto-detects per NPC
and gets collision + player-push, no per-mod knobs.** Combined reference (was Reports 15 + 14 +
06_BoneFollow).
- **Part A — the product spec** (always-on auto-detection, multi-rig, finger touch write-back).
- **Part B — SMP bone access & the pushable-garment recipe** (FSMP/hdtSMP64 interop; §9 = the
  matured end-to-end technique: fur-offset, contact gate, sensor/grip split, node anchoring).
- **Part C — bone-follow / tails / cavities research** (the origin mechanics; the SMP-vs-Havok
  "two worlds" note).
⚠ Runtime rig bodies MUST hold a strong `NiPointer<bhkWorld>` or they orphan on cell change (the
2026-07-14 fix; Pitfall Ledger). Garment mass = 0.05 kg (heavy = drags the SMP); grab-stability vs
light-mass fights — make push-surfaces NON-grabbable (Ledger).

---

# 15 — ALWAYS-ON Follower Capsules (Tails + Fingers) — Product Spec

**Status: SPEC LOCKED 2026-07-12 (user directive) — implementation in progress.**
User's words: *"I want ALL the tails to be fully implemented. So that next time I see a Khajiit
with a SMP tail, an Argonian, M'rissi or Uliss, their tails work, no more 'rig testing'. Those
rig testings end up forgotten each time you compact your context. We also need the fingers
implemented, with collision ALL the time, not just in AIHands. If she puts a finger on her hips
and contacts the skin and her finger moves, that's a good thing."*

## The push mechanism lives in Report 14 §9

This report is the PRODUCT layer (auto-detection, multi-rig, deployment). The underlying TECHNIQUE — how
a capsule both feels the hand (Havok) and pushes the SMP garment (FSMP force), the fur-offset, the
contact gate, the sensor/grip chord split, the per-garment feel numbers, and node anchoring — is
**Report 14 §9 (THE COMPLETE PUSHABLE-GARMENT RECIPE)**. Read that first if you need the mechanics.

## What graduates from test harness to product

The NpcFingerTest rig (dynamic Havok capsules servo-riding bone chords, HIGGS/weapon-collidable,
FSMP push sensors) stops being a single-actor, knob-gated test and becomes an always-on system:

1. **Multi-rig**: one rig per driven NPC (PerfSys driven set), not "one rig, one actor, ever".
   Budget: cap simultaneous rigs (start 8); nearest actors win.
2. **Tail auto-detection, no knobs**: per driven actor, probe chord tables in order —
   HDTS (fluffy Khajiit, table A) → HDT TailBone001..0011 (M'rissi-convention equipped foxtail,
   table B, suffix-aware for the AutoRename armor prefix) → (future conventions appended here).
   First table whose bones ALL resolve binds. NO npcTailTest knob, no nfing pin needed.
   - **Unmatched tailed NPCs must be LOGGED** (has *tail* geometry but no table match) — that log
     line is how new conventions (Uliss's half-dragon, Argonian SMP variants) get their tables
     added. Do a nodeCensusNow-equivalent dump automatically on first sight (1x per FormID/session).
3. **Fingers always-on, own-body collision always-on**: every driven NPC gets the 4-chord finger
   rig with `npcFingerOwnBody`-style contact vs her OWN flesh from the start. The user explicitly
   accepts (wants) fingers displaced by own-body contact — the reviewer's hand-on-hip concern is
   the DESIRED behavior (capsule yields and rests on the flesh), not a defect. If flesh "buzz"
   appears in practice, tune the servo (alpha/maxVel), don't remove the contact.
4. **Finger dimensions**: HALF the original test size — radius 0.5u (was 1.0), chord length ×0.5
   (needs a `npcFingerLenScale` code knob — tipU 0.65 already approximates the tip half live).
5. **Push sensors under multi-rig**: FsmpLink buffer holds 8 targets — publish the NEAREST rigged
   actor's 4 sensor chords only (proximity-elected); grip capsules on everyone.
6. **The locked feel numbers** (per-garment force references, from the dial sessions):
   fluffy tail 12000/32000 r2.0 · M'rissi foxtail 2400/6400 r2.0 (jitter ceiling ~3000) ·
   hair 3000/16000 r0.3 (50% of strands, prefer longer) — future: per-bone mass-scaled force
   (F = gain × disp × m/m_ref) makes one knob fit all garments.

## Current state (as of 2026-07-12 end of session)

- Rig capacity 16 chords/rig; table B = 14 chords (4 sensors + 10 grips, fur-offset corrected);
  zombie-rig guard (missFrames>60); fur-offset table kTailOffsB measured from tailHDT.nif.
- Wig (Amber Lights) + dress (DX Necro) remain MANUAL knob modes (npcTailTest 2/3) — test-tier,
  productize later with auto chord generation from smp_strand_map.py.
- Fingers: single-actor knob mode (npcFingerEnable), own-body mode gated (npcFingerOwnBody 0),
  belt = own-arm parts within layers {8,33}, clutter/weapon fall to vanilla, statics/other NPCs
  Ignored (review: leaned-on statics = infinite-mass flicker; widen deliberately if AIHands needs).
- KNOWN OPEN: left-hand ragdoll list still 3 children (extend at next close); COM child 20 =
  delete from NIF at next close (knob-shrunk to 0.05 meanwhile).

## Implementation log (2026-07-13 close-window wave)

- [x] Heel-aware root capture: `d[2] -= GetHeelDriveBias()` at the poseConformRoot capture —
      fixes the Roisin/Astanova double-count (they carry 1.5u NiOverride lifts on "vanilla boots";
      jtrack proved M'rissi/Jetta/Lajjan PERFECT at 0.89-1.03 scale, so scale+conform architecture
      is validated; Carmella's -7.7 was just heelFix toggled off; Mirad = MALE skeleton, out of scope).
- [x] Permanent WIG table: 15/30 Amber Lights strands (F1 F3 B1 B3 B5 B7 L11 L14 L15 L19 L20
      R01 R02 R06 R09), chords link 2 -> last, per smp_strand_map.py 2026-07-13.
- [x] Permanent DRESS table: 5 panels x (2->6 upper, 5->last lower) + coincloth 2->7 = 11 chords.
- [x] Per-table feel constants TuneOf(tbl): tail-A 2.0/12000/32000 · tail-B 2.0/2400/6400 ·
      wig 0.3/3000/16000 · dress 1.0/3000/16000; fsmpPushMult = the new global multiplier knob.
- [x] Multi-rig refactor LANDED (2026-07-13, DLL 1bb1d3b9): kMaxRigs=8, per-rig bodies,
      group-set comparator (signature-first, relaxed scans), auto-probe tables 1-4 per driven
      NPC w/ 240-frame throttle (tbl 2 skipped when tbl 1 resolves), nearest-actor publish
      arbitration, per-target force/maxF in FsmpLink x fsmpPushMult, npcFollower master knob,
      legacy npcTailTest override path preserved. KNOWN LIMITS (agent-flagged, accepted v1):
      same-actor wig+dress tie on dist2 -> only first-driven publishes (both still collide);
      own-arm belt = single global (finger rig is single-actor anyway); nfing pin serves both
      override + finger rig; sensors fixed at first 4 chords. Adversarial review IN FLIGHT.
- [x] Finger touch WRITE-BACK SHIPPED (2026-07-13, DLL 6ae17f7f): capsule contact displacement
      written onto the finger bones each frame (tip full share x npcFingerFollow 0.8, knuckle half,
      2u clamp; heel-fix decoration pattern = animation re-poses each frame, no feedback).
      npcFingerOwnBody flipped ON (user: "collision ALL the time" — fingers rest on own hips/body).
      Review fixes in the same build: orphan-body removal via the player-world check (HIGH),
      collision-thread logging silenced in HandBoxSubLayer (HIGH), same-actor sensor MERGE to 8,
      own-body scoped to the finger rig's group, FsmpLink 4-buffer rotation, wig sensors = long
      side strands, stale sweep 3->300 ticks, probe stagger, ClearOnLoad belt clears.
- [ ] Male skeleton = separate future project (Mirad et al.).

## Implementation order (survives compaction — tick these off)

- [ ] Multi-rig refactor: FingerRig g_rig → g_rigs[kMaxRigs=8] keyed by FormID; per-rig bodies
      arrays; DestroyRig(reason, rigIdx); filter atomics become per-group SET (the comparator
      needs all live groups: array of atomics, linear scan ≤8).
- [ ] Auto-tail: OnPreDrive per driven actor — if no rig and slots free: probe A→B; on bone-match
      create tail rig. Census-log unmatched tail-geometry actors 1x.
- [ ] Auto-finger: every driven actor with free slot gets the finger rig (own-body policy default
      ON — flip the npcFingerOwnBody default to 1); tail and fingers coexist on the same actor =
      2 rigs (or 1 rig, 2 chord groups — implementer's choice; mind the 16-chord cap).
- [ ] npcFingerLenScale knob (default 0.5 per user).
- [ ] Nearest-rig sensor election for FsmpLink publish.
- [ ] Master kill-switch knob (npcFollower 0/1, default 1) for debugging.
- [ ] Review pass (bounds, atomics ordering, collision-thread discipline) before deploy.

## 2026-07-15 — FULL-COVERAGE fluffy tail + MEATY mass + SILENT contact (deployed)

The fluffy Khajiit tail (table A / HDTS, tbl 1) went from 4 sparse chords (tip bare past 09.002) to **19
OVERLAPPED chords covering the whole chain** — a capsule on every bone, each spanning point i→i+2
(`01→03, 02→04, …, 09.010→09.012`), so every interior bone is double-covered (the user's "strength": the
hand can't slip a seam). Changes in `NpcFingerTest.cpp`:
- `kMaxChords` 16→**24** (all rig arrays — `bodies[]`, `FingerNodes.a/c[]`, `MultiResolve.target/out/tlen[]`
  — are already `kMaxChords`-sized, so one bump scales everything). `kTailPairs` is now a 19-entry table;
  `PairTable(1).n = kTailChordCount(19)`; the build loop already uses `PairTable(tbl).n` for tails.
- **Endpoints ON the bones:** `kTailChordMinU` 2.0→**0.5**. The capsule vertexA=(0,0,0)/vertexB=(0,0,chordU)
  rides pA=bone[i] rotated toward bone[i+2], so it spans EXACTLY node-to-node **iff** chordU = the real span.
  The old 2.0u floor STRETCHED short tip chords past the last bone (= the "tip sticking out"). 0.5 is just a
  degenerate guard. (Measured the actual bones: uniform ~3–6u, base 3.44 / tip 4.18, 20 segments / 77.7u —
  NOT bunched; "too many capsules" is just the overlap × r2.0, not short joints.)
- **Meaty mass:** new knob `npcTailMassKg` (Tuning.h/.cpp), used for tbl 1/2 (fluffy/foxtail); hair (tbl 3)
  stays `npcGarmentMassKg` 0.05. At 0.05 a tail can't hold a push and gets flung ("flies away, can't move
  the tail"); user wants ~5 kg (`npcTailMassKg 5.0` in the tuning file — dial-able, applies on rig RECREATE).
  Keyframed bodies follow the SMP bone one-way, so mass does NOT drag the SMP tail — it only firms up contact
  + the FSMP push displacement signal.
- **SILENT contact (research-grounded):** the collision SOUND, the controller HAPTIC, and the "hairs are
  crazy" spam are ONE root — the NEAR-branch drive re-injects a large return velocity every frame while a hand
  holds a capsule off its bone, crossing both HIGGS's `collisionMinHapticSpeed` 0.2 m/s (physics.cpp:707) and
  the engine's `fMinSoundVel`. **There is NO per-body material/filter/flag that silences haptics** (only
  material silences the SOUND; haptics ignore material). Fix = **clamp the injected velocity below both
  thresholds when a hand is near a garment chord** (`kSilentContactVelMS 0.15`, gated on `chordGated`) — the
  capsule yields softly/silently; depenetration (position-based) still keeps the hand out. Applies to hair too.
- Live-verified on Lajjan: 19 bodies, chords track the bones at gap 0.02u.

## ★ 2026-07-15 — REGRESSION POST-MORTEM: the 19-chord tail was "really, really bad" → REVERTED to 4 chords

User verdict after the play session: Lajjan's tail was terrible ("was really good before"). A 4-agent
investigation (reports + transcript archaeology + code audit + the session's PPB.log) pinned it down.
**The revert (deployed, DLL 2026-07-16 01:30) restores the LAST-KNOWN-GOOD 4-chord config**; next session
= sculpt Lajjan's Khajiit head + fix the tail definitively.

### What the log proved (PPB.log, session 00:15–00:50, Lajjan = 132A4C9F)
- **The "double tail" was NOT two rigs.** Exactly ONE rig alive at all times (one 642 ms destroy/recreate
  at 00:29:18, reason=stale, removed=true; no tbl-2 double-bind, no zombie lines, zero warns/errors). The
  second "tail" was the 19-capsule Havok chain itself LAGGING up to **74 game units (~1 m)** behind the
  rendered SMP tail during whip events — a complete phantom tail-shaped collider beside the real one.
- **Whip amplification is structural to a long chord chain:** in every spike the max gap grows strictly
  monotonically down the 19 chords (12.8u at the base → 62–74u at the tip). 19 servo stages amplify what
  4 stages absorbed. 14,397 of her TRACK lines carried teleports; distal chords hit teleports=8.
- **The two sustained bad windows (00:27:18–56, 00:45:09–41) correlate EXACTLY with FSMP push-force
  counter jumps** (115→3921, then →6409) and 589 REACT "external impulse" events — a live cross-engine
  feedback loop, not random jitter.

### Root cause (ranked, from the code audit)
1. **(~85%) The silent-contact clamp fed servo lag into the FSMP push.** `kSilentContactVelMS 0.15`
   capped a gated chord's drive to ~0.12u/frame — and the gate (hand ≤20u OR **drawn weapon ≤70u** of the
   chord host) is the SAME condition that opens the push publish. Tail motion while gated → lag → lag
   published as displacement ×12000 (clamp 32000) → force DRAGS the SMP tail → more motion → more lag.
   This is precisely the 2026-07-11 "PD-servo lag reads as displacement" failure the contact gate was
   invented to prevent (see §8/`04_Pitfall_Ledger`), resurrected by the clamp. A weapon at the hip
   triggers it with NO deliberate touch.
2. **(~70%) Clamp → gap grows to snapU 20u → FAR-branch hard teleport (often into the hand) → eject.**
   And at 5 kg the body is no longer "light" (`lightBody = mass < 1`), so the anti-ejection velocity caps
   flipped from 10 m/s/50 rad/s to **100 m/s/500 rad/s** — violent kicks + haptic/sound spam, defeating
   the very silence the clamp was for. (Report 15's earlier claim that tail mass "only firms up contact"
   MISSED this lightBody side effect.)
3. **(~35%) Sensor relocation:** the new table's first 4 rows (the FSMP sensors) sat bunched on base
   bones 01–04 (max lever) instead of spread along the tail.
4. **Ruled OUT:** rig-capsule self-collision (FilterDecision ignores all rig×rig pairs) and double-rig.

### Transcript archaeology (NO git repo exists — recovered from session .jsonl)
- The true LKG table (in place 2026-07-11→15) was the **overlapped 4-chord v2**: `02→05, 04→07, 06→09,
  08→09.002` (3-segment spans staggered by 2 — the user's original coverage-hole fix). Restored verbatim.
- Tail-mass history: 0.15 default → **4.0 kg** (the 07-11 "It feel amazing" era, via npcFingerMassKg 4.0)
  → 0.05 garment split (07-13; the flung-tail cause) → npcTailMassKg knob. **The user's remembered "5 kg
  original" was actually 4.0 kg** — tuning file now `npcTailMassKg 4.0`. (4 kg also flips lightBody=false,
  same as the amazing era — the violence came from the clamp cycle, not mass alone.)

### The revert (as-built)
- `kTailPairs` → the 4-chord v2 table verbatim; `kTailChordCount = 4`. The 19-entry i→i+2 table is
  documented above + regenerable; **do not densify again without first fixing the lag→push feedback**.
- The clamp is now the **`npcSilentVelMS` knob, DEFAULT 0 = OFF** (hot-reload, PK_NOSNAP) — the silence
  goal stays a live-dial experiment (needs <0.2 m/s to beat HIGGS haptics; watch for the drag feedback).
- KEPT (safe/no-op at 4 chords): `kMaxChords 24`, `kTailChordMinU 0.5`, the npcTailMassKg mechanism.
- FIXED: NFING CREATE/DESTROY summary lines printed only 4 hardcoded array elements (under-reported the
  19-body rig and contradicted TRACK telemetry) — now print every live chord.

### 2026-07-16 — REVERT VALIDATED IN-GAME + tip extension to 8 chords + the THUD fix

- **User verdict on the 4-chord revert: "working great."** Log confirms: bodies=4, ZERO
  destroy/recreate churn, 0 teleports across all 1,108 TRACK lines, tail avg-of-max 0.37u, 45 REACT
  lines all "player hand contact" (the user playing with it). The event-driven re-scale also passed
  its first live session (Report 20 §10: 2 ARMs, both measured clean — Lydia ×1.0005, Lajjan ×1.0044
  at trueScale 0.8889 == GetScale — 0 writes needed, 0 give-ups).
- **Tip coverage (+4, user: "add 4 more to reach the tip, all-19 was too much")**: `kTailChordCount 8`
  — the 4 proven base chords UNTOUCHED (still rows 0–3 = the FSMP push sensors, so the tuned push
  behavior is unchanged) + 4 tip chords in the same staggered style: `09.001→09.004, 09.003→09.006,
  09.005→09.008, 09.009→09.012` (ends exactly on the last bone; the 09.008/09.009 seam is the one
  spot without span-overlap but the r2.0 end-spheres touch across the ~4.1u segment).
- **The "logs down the stairs" touch sound — ROOT-CAUSED + FIXED (see KNOWLEDGEBASE 2026-07-16)**: the
  ENGINE (not HIGGS) plays a material-pair impact sound when contact speed crosses fMinSoundVel (VR
  global 0x1E94F78). Our capsules were stamped MaterialSkin → skin's impact set maps nearly every
  partner to PHYBodyMediumDirtImpact = the ragdoll BODY-FALL THUD. Fix: garment rigs (hair/tail/dress)
  now stamp a quiet material via the **`npcGarmentMat` knob (0 skin / 1 cloth / 2 snow / 3 grass
  DEFAULT / 4 none)** — snow/grass are direction-PROOF (their own impact sets are NULL, forcing skin's
  soft whump/rustle entries); cloth is the nicest swish IF the engine keys our set (direction
  unverified — that's why grass is the default). Applied at rig CREATE: to re-dial live, change the
  knob then flip `npcFollower 0 → 1`. Fingers/HandBox stay skin. Escalation if any thud survives
  servo-spike moments: the PLANCK-proven proximity-gated fMinSoundVel raise (write 1e6 while
  chordGated, restore after — planck main.cpp:2008/2267 precedent), NOT the npcSilentVelMS clamp.

### 2026-07-16 — CONTACT-DRIVEN FINGER CURL + collision-widening knobs (deployed, awaiting VR test)

User ask: the finger capsules track the fingers but should AFFECT them — colliding with objects, the
NPC's own body, and the player's hand should visibly move/conform the fingers ("make the hand grab
stuff" realism). Built on the AIHands finger-curl recipe (ArmIK.cpp — the in-VR-proven FP_Fist system):

- **The curl loop:** DriveRig's tbl-0 path charges a per-finger curl factor with a LEAKY INTEGRATOR —
  while a capsule is displaced off its chord (> 0.15u deadzone), `curlT += npcFingerCurlGain ×
  (gap−dz) × dt`; contact gone → `curlT −= npcFingerCurlDecay × dt`; clamped to `npcFingerCurlMax`;
  EMA 0.35 (`curlA`). Equilibrium = the finger curled just enough that the displacement nulls — the
  AIHands "curl-until-touch" fixed point with the physics capsule as the sensor.
- **The rotation writer** (PPBHook.cpp, inside the s_bindRelCache block, BEFORE the statue stomp):
  `NpcFinger::GetFingerCurl(id, curl[4])` → for each charged finger, all 3 segments get
  `RotX(kFistShare[seg] × curl)` — shares {1.40, 1.50, 1.30} rad MCP/PIP/DIP, the AIHands FP_Fist
  proportions — dual-written TRACK_POSE + poseLocal (the PLANCK foot-IK-memcpy contract). 12 bone
  indices resolved once per driver into PivCache (bracketed XP32 names mandatory). `QuatMulRotX` =
  post-multiplied local-X flexion (positive = into the palm, AIHands-verified on XP32).
- **`npcFingerCurlMode`**: 0 = ADDITIVE (deflects the frame's animation pose — default) / 1 =
  BIND-RELATIVE (grasp override, the AIHands wrap style). Live A/B knob.
- **Translation write-back interplay:** the old shove now fades as curl rises
  (`follow × (1 − curlA)`) so one displacement isn't double-counted.
- **Collision widening (FilterDecision, finger rig only, all DEFAULT OFF):** `npcFingerVsClutter`
  (layers 4/5/6/19/20), `npcFingerVsWorld` (1/2/10/14/17 — statics = infinite mass, the documented
  flicker risk; the curl conforming away from sustained contact should shrink the flicker window —
  verify), `npcFingerVsNpc` (8/32/33, different group). All return-0 pass-throughs to the vanilla
  layer-56 row — PPB only lifts its own Ignore, never forces past vanilla. Scope caveat: the gate is
  the actor's GROUP, so a same-actor garment rig widens too (accepted for the single-NPC test knobs).
- All 7 knobs hot-reload (PK_NOSNAP); shipped in PPB_tuning.txt with comments. Curl ships ON
  (gain 2.5/decay 1.5/max 1.0/mode 0); widening ships OFF.
- **★ SERVO-LAG GATE (post-deploy self-review catch, same day):** the v1 integrator charged on ANY
  displacement — but a swinging arm's finger chords lag the servo (the ledger's "displacement sensor
  reads MOTION" trap, the tail-drag class) → walking would have fisted her hand in ~0.5 s untouched.
  Fix: charge only while the chord moves < `npcFingerCurlLagGate` (25 u/s default, knob; 0 = off)
  vs last frame's target, OR a HIGGS hand/weapon is within the 20u/70u gate points (deliberate touch
  during her motion still curls — the gate collection now also runs for the finger rig when curl is
  on). Decay still runs when gated. Deployed in the same-day rebuild.
- **★★ CTD ON LOAD (2026-07-16) — the collision-widening knobs crashed the game; FIXED + machine-code
  verified.** The 3 new `npcFingerVs*` float knobs were read INSIDE `NpcFinger::FilterDecision`, which runs on
  HIGGS's comparator trampoline — a stack misaligned 8 bytes vs the x64 ABI. Those three floats were enough
  register pressure for MSVC to spill a nonvolatile xmm in the prologue (`movaps [rsp+0x20], xmm6`) → #GP →
  CTD on save load (2 logs, identical, RSP ≡ 8 mod 16 in both). **Attribution is clean: FilterDecision was
  float-FREE before this feature** — `NpcFingerOwnBody()` returns `bool` and `HandBoxSubLayer()` returns
  `unsigned`, both pre-thresholded in Tuning.cpp; the VsClutter/VsWorld/VsNpc knobs were the first floats ever
  inlined there. FIX: `g_filterKnobs` — one uint32 bitfield (bits 0-3 flags, bits 8-12 HandBox part) sampled by
  `RefreshFilterKnobs()` on the MAIN thread from BOTH `OnFrame` and the top of `OnPreDrive` (so it is hot
  BEFORE any body enters the world), read by FilterDecision as pure integer math. Same pattern HandBox
  (`g_boxPart`) and PerfSys (`g_cbSelfThigh`) already used — NpcFinger was the lone violator.
  **VERIFIED IN THE SHIPPED BINARY** (dumpbin, no PDB → anchored on the unique vtable-slot-20 `call qword ptr
  [r8+0A0h]` registration): the whole trampoline-reachable tree = FilterCB (74 instr) + NpcFinger::FilterDecision
  (236, LEAF) + HandBox::FilterDecision (73, LEAF) = **zero movaps, zero xmm registers, zero calls out of the
  leaves**; unwind info = GPR saves only, no `SAVE_XMM128`; positive control found 108 aligned xmm6 spills
  elsewhere in the same DLL, so the null result is real. Rule + verification method: KNOWLEDGEBASE 2026-07-16 +
  Pitfall Ledger. The knobs themselves are unchanged and still live-dialable.
- **If a finger curls BACKWARD in-game: the SIGN of the angle is the one empirical unknown**
  (AIHands' too — theirs was correct as-is on XP32). Fix = negate `ang` in the writer, not the axis.
- Test protocol: press a finger capsule with the HIGGS hand → the finger should visibly curl toward
  the palm over ~0.4 s and relax ~0.7 s after release; dial gain/decay/max live; A/B mode 0 vs 1;
  then flip npcFingerVsClutter and try pressing her hand onto a table/prop.

### 2026-07-17 — ALL 8 TAIL CHORDS PUSH (per-table `sensors`) + MASS-SCALED FORCE (the tip-jitter fix)

**The dead tip, diagnosed:** user reported push strength fading down the tail to nothing at the tip.
Cause: the publish loop was capped by the GLOBAL `kFingerCount(4)` — when the table grew 4→8, only
rows 0–3 (bones 02/04/06/08) ever published; the entire chain past TailBone08 (13 bones) got zero
force, and the "fade" across the base was lever distance from the 4 base-bunched force points. (The
SMP masses DO taper 0.4→0.15 down the chain, but that alone would make the tip move MORE, not less —
the user's XML-difference hypothesis was half right, wrong direction.)

**Fix 1 — `sensors` is now a PER-TABLE feel property** (`TableTune{r, force, maxF, longCap, sensors}`):
fluffy tail = **8** (all chords push, user directive), foxtail/wig stay 4 (untouched byte-for-byte —
the reason it's per-table: a global bump would have silently re-tuned them). `kMaxSensors = 8` is the
hard ceiling, sized by THREE lock-stepped buffers: FsmpLink `TargetBuf::t[8]`, `g_pubStage[8]`, and
the publish loop — raising it means raising all three. ⚠ Accepted consequence: the tail claims all 8
slots, so a same-actor tail+wig NPC publishes NO wig targets (wig still collides, grip-only).

**Fix 2 — MASS-SCALED PUSH FORCE** (`FsmpLink.cpp` PreSink; Report 14 §9.3's designed endgame):
`F = gain × disp × (boneMass / 0.4)`, clamp scaled identically; mass read live off the matched
`btRigidBody` (`1/getInvMass()`); cap ×1.5, floor ×0.05; knob `fsmpMassScale` (default 1; 0 = raw).
m_ref = 0.4 kg = the fluffy BASE bones the 12000 gain was dialed on → base feel byte-identical, the
0.15 kg tip self-derates to ~4500 eff (inside the light-bone band the foxtail proved at 2400/jitter
ceiling ~3000). **Why:** publishing the tip at raw 12000 produced a SUSTAINED oscillation — log
signature: a monotonic standing-gap gradient down the chain (base 0.25u → mid 4.3u), driveVel pinned
3–4.5 m/s BETWEEN pushes, steady tip teleports, push counter +23k — the a=F/m loop (displacement →
force → overshoot → chase-lag → displacement). Stopgap during the live session was `fsmpPushMult
0.25` (global, flattened the base too); mult is back at 1.0 now that mass-scale ships. The knob read
on the TBB worker is a plain float = the accepted FsmpLink residual class (hazard-sweep rank 2).

### For the "fix definitively" session (open design problem)
Full coverage remains the goal (tip past 09.002 is bare again). The failure modes to solve first:
(a) break the gate coupling — the silence clamp and the push publish must not share a trigger, or the
push must subtract commanded-drive lag from measured displacement; (b) chain-lag amplification — denser
chords need either per-chord gating that doesn't freeze the drive, or snapU/alpha tuned so distal chords
can't run away; (c) sensors must stay SPREAD (rows 0–3 of the table are the sensors — order the table
accordingly); (d) decide lightBody caps for meaty tails explicitly instead of inheriting the <1 kg flip.

---

## ADDENDUM 2026-07-17 — CONTACT-VERIFIED PUSH GATE (the "floaty tail" fix)

**Symptom (user, live)**: jitter fixed, but the tail "stays in the air a lot" — falls in slow motion
while interacting. **Log signature**: FSMPLINK heartbeats show forces ONLY during the interaction
window (6,033 in ~90 s, ~0 at rest) — so no standing anti-gravity force; the floatiness is
motion-coupled drag.

**Root cause**: the push publish was gated on PROXIMITY only (HIGGS hand < 20u / weapon < 70u of the
chord host). The NEAR branch closes only `alpha` of the gap per frame, so a moving tail carries a
speed-proportional servo-lag gap; with a hand merely NEAR, that lag published as force opposing the
tail's own motion = artificial viscous drag. The 07-17 changes made it uniform along the chain
(8 sensors cover the tip; mass-scale makes deceleration ∝ velocity with the same coefficient on
every bone — a textbook damper). Same signal-aliasing family as the whip regression and the finger
curl lag gate: **a displacement sensor reads MOTION**.

**The fix (as-built, DriveRig)**: the NEAR branch commands "land exactly on posCmd" via hard
keyframe, so an unobstructed capsule ARRIVES at the command — deviation from it = something
physically obstructed/shoved the capsule. Per-FingerBody state: `lastCmdPosU[3]` (the post-clamp
command position), `predFrame` (g_frame stamp; 0 = invalid — FAR teleports and skipped frames
reset), `contactUntil` (latch). Detector (tail rigs, main thread only — zero collision-callback
work): if `predFrame+1 == g_frame`, dev = bodyPos − lastCmdPosU; |dev| > `fsmpContactDevU` (0.8u)
latches `contactUntil = now + fsmpContactHoldMS` (200); the deviating frame itself always counts.
Publish now needs proximity AND (latch OR `fsmpContactGate` < 0.5 = legacy A/B). At rest the command
is ~0, so a merely-RESTING own-body/world contact produces no deviation and never latches — the
latch fires only while a contact actively deflects the servo.

**Adversarial review (3 lenses, ALL converged) — fixes folded in**:
1. **The dt trap (HIGH)**: v1 stored commanded VELOCITY and multiplied by THIS frame's dt — exact at
   steady framerate, but any dt change (reprojection 90↔45 flips, hitches, sgtm) manufactures
   deviation ∝ cmdVel × Δdt, false-latching at ordinary swish speeds — re-firing the drag precisely
   in jittery sessions. **Predict the POSITION, never reconstruct from velocity × dt.**
2. Prediction skipped when `impliedVel ≥ 9.5 m/s`: light garment bodies carry
   `m_maxLinearVelocity = 10`; a solver-truncated command undershoots the prediction and would
   false-latch every fast frame if `npcFingerMaxVel` were dialed above 10.
3. `FsmpContactHoldMS()` clamps 0..5000 with a NaN guard — the raw `(int)` cast of a garbage file
   value is UB→INT_MIN, which would silently kill ALL tail push with gate ON.
4. (main.cpp, adjacent) SKELMAP receipt now COPIES the old skeleton path before `SetModel` — for a
   custom race that BSFixedString may be the pool's only reference; logging the raw pointer after
   release read freed memory.
5. Known accepted: the hold is wall-clock — under sgtm slow-mo it covers less game-time (knob
   comment documents; raise HoldMS if pushes pulse in slow-mo).

**Does the 2-bone chord span cause the drag?** No — the capsule ORIGIN rides the chord HOST end
(gap is origin-vs-tA; the far bone only sets orientation), so each sensor's lag gap comes from its
host bone's own motion. The span AMPLIFIED the feel two ways: overlapping spans double-cover regions
(two sensors drag two adjacent bones for one motion) and the long capsule's far half sweeps a big
lever arc. With the contact gate, both become benign (free motion publishes nothing) and overlap is
a FEATURE during real pushes (a press in the overlap moves two adjacent bones = smoother response).
Chord table unchanged.

**SKELMAP parser fix (same build)**: `race` line paths were read with `>>` → truncated at the first
space ("Actors\Character\Character") → M'rissi/Orc females repointed at a nonexistent skeleton →
3D never builds → invisible on `moveto` (the receipt caught it). Now: rest-of-line + trim.

---

## ADDENDUM 2026-07-17 (evening) — TABLE 5 RE-ACTIVATED (skinned vanilla-chain tails, M'rissi) + the tail-coverage model

**Deployed (md5 919d5502…), NOT yet play-tested — M'rissi is the first live test of table 5.**

### What shipped
- **Fluffy tail 8 → 7 chords** (kTailChordCount 7, TuneOf case-1 sensors 7): the 09.009→09.012 chord
  floated past the visible fur and snagged the hand (user). Chain now ends at 09.008.
- **Table 5 (`kTailPairsC`, vanilla `TailBone01..05`) RE-ACTIVATED** as touch/grip-only
  (TuneOf case 5 = { r2.0, force 0, maxF 0, longCap, sensors 0 }): FSMP has no rigids on the plain
  chain (2026-07-12 census), so motion stays CBPC; the capsules deliver HIGGS/weapon touch FEEL only.
- **`TailChainSkinned(obj, bone)`** — the gate that makes this safe: every PPB female skeleton CARRIES
  the dormant XP32 `TailBone01..05`, so bone existence ≠ a tail. The gate walks the actor's geometry
  and returns true only if a skinned mesh actually BINDS the chain (M'rissi's naked-skin tail does; a
  plain human's ponytail does not). ⚠ Count the skin's bones with **`NiSkinData::bones` (skinData+0x58)**
  — the loader truth HIGGS/PLANCK use — NOT `NiSkinInstance::numMatrices` (a render-lazy field, 0 until
  first render → would false-negative on a just-loaded/culled actor and stick the ghost rig).
- **Probe precedence**: when the cursor lands on table 2 AND the HDT ghost chain resolves AND the
  vanilla chain is skinned → promote to table 5. The `&& ResolveNodes(2).all` scope-guard keeps plain
  vanilla beast tails (Khajiit/Argonian with no ghost) OUT, so grip-only rigs can't eat the 8 global
  rig slots in a beast-heavy scene. Widen deliberately later if beast-tail touch coverage is wanted.
- Review-caught hardenings (all folded in pre-deploy): fast-reprobe now includes tbl 5 (door
  transitions rebuild her rig next tick, not ~3 s later); sensor-less rigs excluded from the publish
  election (a tbl-5 nearest actor no longer starves a pushing actor's force); a ~2 s skin-bind
  re-check destroys a tbl-5 rig if the tail mesh unequips without a root rebuild (the vanilla bones
  never vanish, so the nodesMissing detector can't fire for table 5); legacy npcTailTest=1 mirrors the
  same precedence.

### ★ THE TAIL-COVERAGE MODEL (answer to "will every tail need personal tweaking?")
**Coverage is per-CONVENTION, not per-NPC.** A tail mod names its bone chain in one of a few standard
ways; each convention = ONE chord table, built once; the auto-probe matches NPCs to tables with zero
per-actor work. Current tables: 1 = `HDTS TailBone02…` (the big fluffy replacer), 2 = `HDT TailBone001…`
(equipped SMP foxtails), 5 = `TailBone01…05` (CBPC/skinned vanilla), 6 = `Mal_Tail 1…13`
(Malignis-convention SMP tails — Yvanni the Draenei; added 2026-07-19, see the addendum below). Once a table exists, EVERY tail
of that convention works automatically.
- **M'rissi was NOT a new convention** — table 5 already existed. Her bug was an AMBIGUITY (she wears
  BOTH a skinned vanilla tail and an invisible equipped ghost; the probe picked the ghost). The
  skin-bind gate + precedence resolves that class GENERICALLY — any double-tailed NPC now works.
- **What needs new work**: only a tail using a bone-naming convention not yet mapped → a ONE-TIME
  table addition that then covers ALL tails of that type. This is exactly the job of
  `Report/Collision Capsule SMP binding procedure/` (delegate to a cheap model). The ecosystem
  standardized on a handful of conventions, so the list is short.
- Capsule RADIUS is one profile per table, auto-scaled by the NPC's measured body scale — not per-NPC.

---

## ADDENDUM 2026-07-17 (night) — FOXTAIL: ALL 14 CHORDS PUSH at the DIALED-LOW gain (user-corrected)

**USER CORRECTION (canon): the foxtail gain stays LOW.** Her tail bones are LIGHT (XML: 001/002 =
0.5 kg, 003-005 = 0.1, 006-0011 = 0.05) and 12000-class gain makes it "fly off violently" — the
2400 dial was deliberate. The dead mid/low tail was NOT gain: the 10 grip chords collided but never
published (sensors=4). NEVER raise table 2 toward 12000.

## ADDENDUM 2026-07-20b — tbl-5 RETIRED · Mal_Tail gain 30000 (DLL b7710e81)

- **Table 5 (vanilla-chain touch rig) RETIRED** (user: "useless now") — a bone-ANIMATED tail cannot
  be pushed, so touch-only capsules on Ulliss served nothing; and NO current NPC needs the
  M'rissi-class promote (her fluffy tail = table 2). The promote is commented out at both probe
  sites; kTailPairsC + TailChainSkinned + the tip-gate lesson stay for a future re-activation.
- **Mal_Tail gain 2800 → 30000 / maxF 80000** (user: "almost nothing"; the log proved pushes DID
  fire — 327 total — so it was gain, not the contact gate). 30000 at her 1.0 kg massRef is the
  EXACT acceleration the fluffy tail was dialed to (12000 @ 0.4 kg ≡ 30000 @ 1.0 kg through the
  mass-scale). Her bones are also heavily damped (XML 0.97/0.92) so fluffy-equivalent is the right
  seed; trim live with fsmpPushMult if violent.

---

## ADDENDUM 2026-07-20 — first live test: table 6 TRIMMED to the mesh + the tbl-5 TIP-GATE (DLL 0db81303)

Three corrections from the first in-game session with Ulliss + Yvanni:
- **Table 6 trimmed 7 → 4 chords, ending ON Mal_Tail 7** (`1→3, 2→4, 4→6, 5→7`): Yvanni's tail
  MESH only reaches bone 7 (offline weight scan: bone 8 = 42 weak verts, 9..13 = ZERO — bare
  skeleton). The 1→13 table read "twice the length of her tail". Sensors 4 (all). RULE reaffirmed:
  a chord table's reach comes from the MESH's weighted extent, not the bone chain's length —
  always run the weight scan (inspect_tails.py) before authoring a table.
- **tbl-5 TIP-GATE**: `TailChainSkinned` now must pass on BOTH the chain root (TailBone01) AND the
  tip (TailBone05). Root-only skinning is not a tail: Yvanni's "Tail Ring" jewelry weights
  TailBone01/02 only (165/103 verts) and spawned a straight-out phantom rig on her dormant chain.
  Real tails (Ulliss's CrbTail: every bone 330-595 verts) weight the whole chain.
- **★ MEMORY CORRECTION — M'rissi does NOT bind the vanilla chain.** Her worn fluffy tail
  (`Fluffy\tailHDT.nif` 'Sippo', the winning MRTMrissiRNakedTail ARMA model via
  Fluffy M'rissi_HDT ADDON.esp) binds ONLY the 11 HDT bones; her winning body (Bodyslide Output
  mrtt/mri, 18436) binds no TailBone either. The 07-17 "her naked-skin tail binds TailBone01-05"
  premise predates the Fluffy replacer winning her model. She rides **table 2** — tbl-5 gate
  changes cannot affect her (TailChainSkinned is false for her under any gate).
- Ulliss's tail = correctly-tracking tbl-5 touch capsules; it "doesn't move" because the tail is
  BONE-ANIMATED (idle sway), not SMP — nothing to push. SMP-converting her chain (the fluffy-
  replacer recipe: per-bone dynamic bone-defaults + chained constraints + Tail-tagged collider)
  is **OUT OF SCOPE for PPB** (user directive 2026-07-20) — maybe a separate project later.

---

## ADDENDUM 2026-07-19 (night) — TABLE 6 (Mal_Tail) + the tbl-5 ghost requirement DROPPED (DLL 15ed5bb9, deploy pending)

**Two coverage extensions for the special-follower wave (Ulliss + Yvanni), built + compiled, awaiting
first in-game test:**
- **Table 6 = `Mal_Tail 1..13`** (Malignis-convention SMP tails; Yvanni the Draenei is the first).
  Source of truth: her skin-carried `Yvanni Tail_0.nif` + `Mal_Tail.xml` (root kinematic, bones 2-13
  dynamic ~1.0 kg, chained generic constraints, `<shared>public</shared>`, tag MalTail). 7 staggered
  span-2 chords (`1→3, 2→4, 4→6, 6→8, 8→10, 10→12, 11→13` — last chord ENDS on the last bone), ALL
  sensors, TuneOf { r 2.0, gain 2800, maxF 7500, longCap, sensors 7, **massRef 1.0** } — massRef 1.0
  because her bones are 10× the foxtail's mids; gain seeded at the foxtail-safe 2800, DIAL IN VR.
  Bone names contain a space — the suffix matcher handles it (exact-or-" <orig>" ends-with; "Mal_Tail 1"
  cannot false-match "… Mal_Tail 13"). No fur-offset table (bones run the tail core). Probe walk now
  runs candidates 1→2→3→6 (walk slot 4 maps to table 6; tbl 4 stays retired). Armor-carried bones →
  the standard nodesMissing detector handles unequip (no tbl-5-style skin re-check needed).
- **Table-5 promote no longer requires the HDT ghost chain** (auto-probe AND legacy override, in
  lock-step): the old scope guard demanded M'rissi's exact signature (skinned vanilla chain + ghost
  HDT item), which left the **Ulliss class** — equipable `CrbTail.nif` skinned to vanilla
  `TailBone01..05`, no HDT anywhere — permanently un-rigged. `TailChainSkinned` is the real ghost
  protection (unskinned chains never rig; plain humans stay rig-free). Consequence to WATCH: any
  female whose body/worn mesh skins the vanilla chain (incl. plain beast tails) now gets a grip-only
  tbl-5 rig — receipts `NFING CREATE … tbl=5`; the 8-slot budget in beast-heavy scenes is the thing
  to watch in the log.

---

**As-built (deployed, md5 1c2699bf):**
- TuneOf case 2 = gain 2800 / maxF 7500 / **sensors 14** — every chord drives its host bone.
- **kMaxSensors 8 → 14** — THREE sizes in lock-step: kMaxSensors, g_pubStage[14], FsmpLink
  TargetBuf::t[14] + the PublishTargets clamp. (Review verified no stale 8s remain.)
- **Per-table `massRef` rides with every PushTarget** (FsmpLink.h): PreSink scales force by
  boneMass/massRef instead of the hard-coded /0.4. Fluffy = 0.4 (unchanged); foxtail = 0.1 — the
  bone class its gain was FELT on. Without this, mass-scale derated her 0.1/0.05 kg bones 4-8× and
  the newly-published chords would STILL have read dead (review MEDIUM, both lenses).
- **PreSink below-deadzone `break` → `continue`** (review HIGH): in a curled pose the long sensor
  chord leaves the fur and publishes zero while the co-located grip chord carries the real press —
  first-match-break left bones 002/004/006/008 dead. `continue` lets a later duplicate-host target
  drive the body; the post-force `break` still caps at ONE force per body per step.
- Review sanity: no multi-bone amplification possible (1.5u match vs 4-8u bone spacing), contact
  gate zeroes un-latched capsules, so a press drives ~1-3 bones — 2800×14 is NOT the violence
  regime. Feel-test pending in VR.
# 14 — SMP Bone Access & Interop (FSMP / hdtSMP64) — source-verified 2026-07-11

> **★ §9 (2026-07-13) is the definitive PUSHABLE-GARMENT RECIPE** — the matured end-to-end technique
> for making any tail/hair/cloth receive physics and be pushed by the player's hand (fur-offset,
> contact gate, sensor/grip split, per-garment feel, node anchoring). Read §9 for the method; §6–8 are
> its origin; Report 15 is the product/deployment layer (auto-detection, multi-rig).

**Official authoring bible (user-endorsed 2026-07-12)**: github.com/DaymareOn/FSMP-Validator/wiki —
XML grammar, springs/damping/motors runtime internals, collision filtering, troubleshooting. Consult
it BEFORE authoring SMP XMLs; it supersedes grammar reverse-engineering.
**Source of truth (engine internals)**: github.com/DaymareOn/hdtSMP64 — the EXACT source of the installed `Faster HDT-SMP`
3.3.0 DLL (build path `D:\a\hdtSMP64\hdtSMP64\src\PluginInterfaceImpl.cpp` embedded in the binary).
Branches: `dev` is default; some files also on `master`. Raw URL:
`https://raw.githubusercontent.com/DaymareOn/hdtSMP64/dev/src/<file>` (fall back to /master/).
Everything below was read verbatim from source by three agents on 2026-07-11; installed-DLL string
scan confirmed PluginInterface RTTI + all DynamicHDT natives are present in 3.3.0.

## 1. ★ THE BONE RENAME — why SMP bones "don't exist"

Armor-carried SMP bones (fluffy tails, wigs, SMP cloth) ARE merged into the actor's skeleton tree
at equip — but RENAMED (`ActorManager::armorPrefix`):

```
"hdtSSEPhysics_AutoRename_Armor_XXXXXXXX " + <original name>    (8-hex per-armor counter + SPACE)
"hdtSSEPhysics_AutoRename_Head_XXXXXXXX "  + <original name>    (head parts variant)
```

- `doSkeletonMerge`: bone names that ALREADY exist in the skeleton are REUSED unrenamed
  (⚠ CORRECTED 2026-07-17: M'rissi was the cited example but her `HDT TailBone001..0011` are in fact
  ARMOR-carried — only the single-zero `HDT TailBone01..06` series is skeleton-carried; her chain
  resolves via the SUFFIX match, not plain names); missing ones are CLONED
  under the corresponding parent (tail → under `NPC Pelvis`-ish; orphans → under `"NPC"`) and
  renamed in BOTH trees (the armor NIF's own nodes too, so the engine skins to the renamed clones).
- The prefix is also the GC tag: on unequip every node starting with the prefix is detached.
- The 8-hex id is a per-skeleton equip COUNTER, not a FormID — you cannot predict it; match by
  suffix. The ARMA FormID lives in the ARMOR NODE's name at chars 1–8 (`splitArmorAddonFormID`).
- **Corollary**: CBPC configs (VRTouchEvents tail spheres!) that name plain `HDTS TailBone*` are
  SILENTLY DEAD on armor-carried tails. Fix = suffix-aware resolution (C++) or config entries with
  prefixed names (impossible statically — counter varies) → runtime resolution only.

**Lookup recipes** (best first):
1. Tree walk matching exact OR ends-with-`" <orig>"` — implemented as `FindBoneSuffix` in
   PPB `NpcFingerTest.cpp` (deployed 2026-07-11).
2. Skin-instance route: equipped shape's `skinInstance->bones[]` = direct pointers to the very
   nodes SMP drives (what FSMP's own `generateMeshBody` uses).
3. FSMP PluginAPI userPointer route (§3) — ABI-coupled, use only if 1/2 insufficient.

## 2. Per-frame transform flow (read `world`, never `local`)

`SkyrimBone::writeTransform` writes `m_node->world.rotate/.translate` DIRECTLY from
`m_rig.getWorldTransform() * m_rigToLocal` (Skyrim units, unscaled). `local` is NOT maintained.
Write-back happens on the GAME thread at FrameSync (after the TBB physics task completes); the
physics step itself runs on a TBB worker (`doUpdate2ndStep`, world lock `m_lock` held).
Kinematic vs dynamic = XML mass 0 vs >0 (`CF_KINEMATIC_OBJECT`).

## 3. FSMP PluginAPI v2.0.0 (in installed 3.3.0; compiled unconditionally, VR first-class)

- Handshake: `RegisterListener("hdtsmp64", handler)` at SKSE **kPostLoad**; FSMP dispatches
  `MSG_STARTUP` with `hdt::PluginInterface*` at kPostPostLoad. MUST check
  `getVersionInfo().interfaceVersion` (=2.0.0) and `bulletVersion` (=3.24.0) before Bullet touches.
- Surface (ALL of it): `getVersionInfo()` + add/removeListener for `IPreStepListener` /
  `IPostStepListener` (= `RE::BSTEventSink<hdt::Pre/PostStepEvent>`). Events carry the ENTIRE
  `btAlignedObjectArray<btCollisionObject*>` + timeStep, per PHYSICS step (skipped when disabled/
  paused/no active skeletons). Both callbacks see true world-space transforms.
- Bone rigids: `btRigidBody::upcast(obj)` non-null, broadphase 0/0; `getUserPointer()` →
  `hdt::SkinnedMeshBone*` (`m_name` = RENAMED name, `m_rig`, `m_rigToLocal`, `m_currentTransform`);
  concrete `hdt::SkyrimBone` adds `m_node`, `m_skeleton`; actor = `m_skeleton->GetUserData()` →
  `TESObjectREFR*`. Mesh bodies: upcast null, custom concave shape named "btSkinnedMeshBody",
  NO user pointer.
- ⚠ Callbacks run on the TBB worker with the world lock held — atomics only, no logging, no
  blocking (the HIGGS-callback discipline). ⚠ userPointer casting = copying FSMP internal headers
  = ABI coupling per FSMP version + Bullet 3.24 layout.
- Contract: PreStep may ONLY apply forces/torques; everything else read-only. NO add-body method;
  world base is `protected btDiscreteDynamicsWorldMt` — cannot upcast/addRigidBody externally.

## 4. Dynamic HDT (integrated; natives COMPILED INTO FSMP 3.3.0)

Mod "Dynamic HDT - Papyrus Script Extension" (Nexus 63017) ships ONLY `DynamicHDT.pex` + psc —
the native side is in FSMP itself. **The pex must be present** or override persistence silently
no-ops (`checkPapyrusExtension()` gates the SKSE co-save record 'APFW'). Current natives
(class `DynamicHDT`, all global native):

| Native | Signature | Notes |
|---|---|---|
| ReloadPhysicsFile | bool(Actor, ARMA, path, persist, verbose) | per-ARMA XML swap |
| SwapPhysicsFile | bool(Actor, oldPath, newPath, persist, verbose) | by-current-path swap |
| QueryCurrentPhysicsFile | String(Actor, ARMA, verbose) | read current XML path |
| TogglePhysics | bool[](Actor, boneNames[], on) | flip kinematic per NAMED bone; returns prev states |
| ResetPhysics | void(Actor, full) | full=reloadMeshes / soft=poses preserved |

Swap internals: `ActorManager::instance()->lockGuard()` → `getSkeletons()` → match
`skeleton->GetUserData()->formID` → armors; rebuild via `SkyrimSystemCreator().createOrUpdateSystem`
(re-reads the XML incl. TAGS) + `transferCurrentPosesBetweenSystems` (name-matched after stripping
the AutoRename prefix; world transform + velocities carried, 0.8 damping) → SMOOTH swap, no snap.
Overrides: per-actor + per-original-path (NOT per-ARMA, NOT cross-actor), chain-collapsed,
auto-reapplied on every armor attach, persisted in the SKSE co-save.
C++ mutation pattern (TogglePhysicsImpl): `lockGuard()` + `SkyrimPhysicsWorld::get()->lockSimulation()`
→ flip `CF_KINEMATIC_OBJECT` on `bone->m_rig` → zero velocities → `updateConstraintsForBone(bone)`.

## 5. Collision model + CROSS-ACTOR verdict

- ONE `btDiscreteDynamicsWorldMt` singleton for ALL actors' systems. Bone rigids 0/0 (never
  broadphase-paired — they move via constraints); collision happens ONLY between `SkinnedMeshBody`
  per-vertex/per-triangle mesh shapes (group 1/1) via the CUSTOM dispatcher.
- Tag semantics (`SkinnedMeshBody::canCollideWith`, must pass BOTH directions):
  whitelist mode if `can-collide-with-tag` present, else blacklist via `no-collide-with-tag`;
  kinematic×kinematic never collide (kinematic hand vs dynamic tail = fine).
- **Cross-actor collision: ALLOWED BY DESIGN** — the dispatcher checks tags + kinematic flags ONLY,
  no actor/system identity. Player-hand SMP shapes vs NPC-tail shapes collide if tags mutually pass.
  Gates: both skeletons must be ACTIVE (frustum/budget culling); disable-tag dedup is per-skeleton
  (never suppresses cross-actor pairs).
- **Injecting foreign bodies into SMP's world: FUTILE** — the dispatcher casts both sides to
  `SkinnedMeshBody` (foreign → no collision; bone-branch blindly casts userPointer → foreign body
  = null-deref risk). Do not attempt. Sanctioned interaction = tags/XML (config) or PreStep forces.

## 6. THE ARCHITECTURE (tails, cloth, hair — identical mechanics)

- **FEEL (Havok side, one-way, ships first)**: runtime capsules keyframed onto the (renamed) SMP
  bone nodes — `FindBoneSuffix` + the finger-recipe rig (deployed in PPB.dll; `npcTailTest 1`).
  Gives HIGGS hand contact + haptics + VRTouchEvents-class detection. SMP does not see it (Bullet
  ≠ Havok) — the tail brushes the hand, the hand cannot move the tail through this layer.
- **PUSH (SMP side, two-way)**:
  1. CONFIG ROUTE (preferred): enable the Dynamic HDT mod (pex only) + ship OUR OWN tail XML
     (copy of `Tail_long_Khajiit.xml` with the hands blacklist removed / proper can-collide tags)
     inside PPB + `SwapPhysicsFile` per actor at runtime (persist=true → co-save + auto-reapply).
     Prereq to VERIFY: the PLAYER actually carries an SMP hand collider shape with tag `hands`
     (SOFTBODY's `softbodyFix.xml` body_rigid tag=hands rides femalebody — player male/beast hands
     unverified; may need to ship a small hand-collider XML + defaultBBPs entry).
  2. C++ ROUTE (fine control, later): FSMP PluginAPI PreStep — hand-capsule vs bone proximity math
     → `applyForce`/`applyCentralForce` on the dynamic tail bone rigids. Enables true finger-grab
     dynamics. ABI-coupled to FSMP build; version-check at handshake.
- **VRTouchEvents fix (separate mod, note for its agent)**: fluffy-tail CBPC spheres are dead due
  to the rename; fix = runtime-resolved prefixed names or suffix matching in the CBPC layer.

## 7. Status (2026-07-11 morning)

- **FEEL LAYER USER-VERIFIED** on Lajjan: `NFING CREATE mode=TAIL(HDTS)` via suffix match, 4
  capsules riding the SMP tail at 0.03u avg gap / 0 teleports, REACT contacts 1.0–2.4u on pokes,
  tail brushes the hand. SMP-write vs our-read timing: no observable lag. HIGGS-grab = separate
  mechanism (mesh-collision + bone-keyframe-to-hand), deliberately parked.
- **PUSH LAYER DEPLOYED (config route, awaiting test)** — and SIMPLER than planned: no tail XML
  override, no Dynamic HDT needed for v1. The novel-tag trick: PPB ships `ppbHands.xml`
  (kinematic per-triangle collider on the hands meshes, `tag ppbhand`, whitelist
  `can-collide-with-tag Tail` ONLY → collides with SMP tails and nothing else; the fluffy tail's
  hands/hand blacklist never matches it) + a `defaultBBPs.xml` override (= SOFTBODY winner
  verbatim + maps "HIMBO - Hands"/"Hands" → ppbHands.xml; hands-mesh winner = Bodyslide Output).
  ALL hands (player + NPC) push tails, per user choice. Re-merge defaultBBPs if SOFTBODY updates.
- Still open: in-game push test (poke tail → visible yield); Dynamic HDT enable + SwapPhysicsFile
  = the PER-ACTOR variant when/if global-by-tag needs scoping; M'rissi foxtail push (her XML's
  tags differ — check `HDT TailBone` config's tag rules); cloth/hair rollout (same recipe);
  PluginAPI client TU (C++ forces for finger-grab dynamics) — later tier.

## 8. STAGE 1+2 AS-BUILT (2026-07-11 afternoon — WORKING, user-verified "It feel amazing")

- **FsmpLink.cpp/h** + `src/fsmp/PluginAPI.h` pinned to the **v4.0.1 tag** (interface 2.0.0 verified
  matching at runtime: "MSG_STARTUP from 'hdtsmp64' — interface 2.0.0, bullet 3.24.0"). Listeners
  registered for BOTH senders at kPostLoad ('hdtsmp64' FSMP / 'hdtSMP64' Flex — Flex 0.8.0.17 has
  the interface, no DynamicHDT). SEH-wrapped getVersionInfo; major-gate; first-accepted-wins.
- **Bullet 3.24 vendored** (13-header closure, extern/bullet324, CMake-scoped to FsmpLink.cpp only).
- **ZERO FSMP-internal layout**: bones matched by WORLD POSITION (<1.5u; SMP world = game units).
  v4.0.1 SkinnedMeshBone layout noted for fallback: vptr + BSIntrusiveRefCounted → m_name@16.
- **Sensor** (DriveRig, tail mode): per-chord host-bone pos + capsule displacement, double-buffered
  (FsmpLink::PublishTargets). **CONTACT GATE**: displacement only publishes while a HIGGS hand
  (<20u) or wielded weapon (<70u) is near the chord — without it, walking servo-lag became a
  visible backward DRAG at high force ("tail stiff where the capsules are", user-diagnosed;
  deadzone alone insufficient).
- **Actuator** (PreSink, PreStep = the API's forces-only window): position-match dynamic rigids →
  applyCentralForce(fsmpPushForce × disp, clamp fsmpPushMaxForce, deadzone fsmpPushMinDispU,
  250ms staleness). TBB-thread discipline: atomics only; receipts via heartbeat counter.
- **TUNING (user-dialed)**: fsmpPushForce **12000**, max **32000**, deadzone 0.15. Force 40 was
  invisible — the deliberately-stiff capsules displace only 0.1–1u, so k must be huge; SMP gravity
  ≈200 on a 0.3-mass tail bone is the scale anchor. Idle/gravity/bounce untouched by construction
  (gate + deadzone + contact-only displacement).
- **Same build**: NpcFinger::FilterDecision admits the HandBox boxes (part=handBoxSubLayer, def 4)
  vs rig capsules — finger boxes previously passed through tails (only parts 2/3/5/6 listed).
- Weapon-swipe pushes work via the same path (weapon displaces capsule → force) — no SMP hand
  collider, no equipped items, engine-gated, Flex-compatible by handshake.

## 9. ★ THE COMPLETE PUSHABLE-GARMENT RECIPE (matured 2026-07-13 across tails, hair, dress)

This is the definitive, transferable technique for making ANY SMP garment (tail, hair, cloth) receive
physics and be pushed by the player's hand. §6–8 are the origin; this section is the matured method
after it was proven on the fluffy Khajiit tail, M'rissi's foxtail, Carmella's Amber Lights wig, and her
DX Necromancer dress. The runtime engine is `NpcFingerTest.cpp` + `FsmpLink.cpp`; the product/deployment
layer (auto-detection, multi-rig) is **Report 15**. Locked feel numbers are in both.

### 9.1 The two-layer model (this is the whole trick)
SMP runs in its OWN Bullet world; the game ragdoll + our capsules run in HAVOK. They can NEVER collide
cross-engine. So we bridge with TWO cooperating layers on the same bone chain:
- **FEEL layer (Havok, one-way)** — dynamic Havok capsules keyframed onto the SMP bone nodes each frame.
  Gives the player's HIGGS hands + weapons something solid to touch (haptics, contact, VRTouchEvents).
  This layer lets the hand feel the tail; it does NOT move the SMP simulation.
- **PUSH layer (SMP, two-way)** — the SAME capsule's displacement (how far the hand shoved it off its
  bone) is read as a SENSOR and converted to a FORCE applied to the matching SMP rigid via FSMP's
  PluginAPI. This is what makes the garment visibly YIELD and swing when pushed.
The capsule is simultaneously the touchable surface (Havok) and the displacement sensor (feeds SMP).

### 9.2 The pipeline, end to end
1. **Resolve the bone chain** on the actor. Garment SMP bones are RENAMED at equip
   (`hdtSSEPhysics_AutoRename_Armor_XXXXXXXX <orig>` — see §1); resolve with `FindBoneSuffix`
   (exact OR ends-with-" <orig>"). Skeleton-carried chains (M'rissi's) keep plain names.
2. **Lay CHORDS** across the chain — a capsule spans two bones. OVERLAPPED design (staggered spans, e.g.
   bone 2→5, 4→7, 6→9) so every interior region is double-covered: the hand can trap one capsule and its
   neighbor still guards the strand behind it. The FIRST 4 chords are the push SENSORS (they publish
   force targets); any additional chords are GRIP-only (collision feel, no publish) — put the longest/
   most-grabbable strands first so the sensors sit where the player actually grabs.
3. **★ Ride the FUR/MESH line, not the bone axis** (the M'rissi fix, critical). Mesh authors often run
   the bone chain along the garment's EDGE, not its visible center — so capsules on the raw bone axis
   sit 4–13u OFF the visible strand. Measure the per-bone offset ONCE offline: weighted vertex centroid
   (weight ≥ 0.3) minus the bone's bind position, expressed in the bone's OWN bind frame
   (`tools/ppb-scratch/tail_offset_measure.py`, pynifly). Bake it as a table; at runtime place each chord
   endpoint at `node.world.translate + node.world.rotate × offset × scale`. Knob `npcTailOffScale`
   (0 = bone axis, 1 = measured). **The SMP push MATCH position stays the RAW bone axis** (the SMP rigids
   live there); only the capsule VISUAL/sensor rides the fur line, and displacement is measured vs the
   capsule's own fur anchor (else the offset reads as a permanent phantom push).
4. **SENSE** (game thread, `DriveRig`): per chord, publish host-bone world pos + capsule displacement
   off its chord, double-buffered (`FsmpLink::PublishTargets`).
5. **★ CONTACT GATE** — publish displacement ONLY while a HIGGS hand is < 20u OR a wielded weapon body
   is < 70u of the chord host (weapon body sits at the hilt, blade reaches far). WITHOUT this, walking
   servo-lag reads as displacement and the force DRAGS the garment ("stiff where the capsules are",
   user-diagnosed). Deadzone alone is insufficient; the gate is mandatory.
6. **ACTUATE** (`FsmpLink` PreSink, PreStep = FSMP's forces-only window, TBB thread): position-match the
   dynamic SMP rigids to the published targets by WORLD POSITION (< 1.5u; SMP world = plain game units,
   ZERO FSMP-internal layout used) → `applyCentralForce(force × displacement)`, clamped, deadzoned,
   250 ms staleness. Atomics only on this thread — NO logging (CTD).

### 9.3 Per-garment FEEL (the locked dial-session numbers → `TuneOf(tbl)`)
Each garment class carries its own radius + force + clamp; the `fsmpPushMult` knob is a GLOBAL multiplier
on top (default 1) so one dial scales everything proportionally:
| Class | radius | force | clamp | notes |
|---|---|---|---|---|
| Fluffy Khajiit tail (HDTS) | 2.0u | 12000 | 32000 | heavy fur bones |
| M'rissi foxtail (HDT, light) | 2.0u | 2400 | 6400 | jitter ceiling ~3000 — light bones need far less force |
| Hair strands (wig) | 0.3u | 3000 | 16000 | skinny; ~50% of strands, prefer longer, scattered |
| Dress panels (cloth) | 1.0u | 3000 | 16000 | all panels, upper+lower chords + coincloth |
Why force is huge (12000, not 40): the deliberately-stiff capsules displace only 0.1–1u, so the gain `k`
must be large; SMP gravity ≈200 on a 0.3-mass bone is the scale anchor. The 5× gap between the heavy
fluffy tail (12000) and the light foxtail (2400) is the empirical anchor pair for a future per-bone
mass-scaled force (F = gain × disp × m/m_ref → one number fits all garments).

### 9.4 Hard-won gotchas (each cost real debugging)
- **`<shared>` scope**: the hand-collider SMP config must be `public`, not `private` — `private` silently
  scopes to the same system and kills cross-actor push.
- **Bind on the VISIBLE chain, not a ghost**: M'rissi had TWO tail chains — the visible one (skinned to
  her HDT bones) and an equipped INVISIBLE SMP tail dangling 5–40u below. Bind whatever the visible MESH
  rides (census + which chain the skin weights use). Also: XP32 ships a dormant `TailBone01-05` on EVERY
  humanoid — gate any vanilla-chain binding on the actor actually HAVING tail GEOMETRY, or every NPC
  gets phantom tail capsules.
- **Chord length cap must match the garment**: a 12u cap truncated 33u hair strands and 14–19u tail
  chords, eating the overlap → capsules end-to-end, hand passes through the seams. Long garments need a
  36u+ cap (`kTailChordMaxLongU`, per-table via `TuneOf.longCap`).
- **Multi-publish torn read**: with multi-rig, several rigs publish per frame; the FSMP target buffer was
  rotated 2→4 slots so a same-frame rewrite can't corrupt the TBB reader's in-flight reference.
- **Same-actor garments merge sensors**: a wig + dress on one NPC tie on distance; MERGE their sensor
  sets (up to FSMP's 8) so both push, don't let one silently win.
- **Node anchoring (user requirement)**: every capsule's TWO endpoints must sit on a corresponding
  XP32/XML node (chord = node-to-node), not float near the strand — this is what keeps the capsule
  tracking the garment through its full motion. The fur-offset (§9.3 step 3) is the node-accurate
  placement, not a fudge away from it.

### 9.5 Deployment (the config-route hand collider, the fallback path)
Beyond the C++ force path, the player's hand can also push SMP purely via config: PPB ships `ppbHands.xml`
(kinematic per-triangle collider on the hands meshes, novel `tag ppbhand`, whitelist `can-collide-with-tag`
the garment tag) + a `defaultBBPs.xml` override mapping the hands mesh to it. This works without the C++
TU but gives no force control — the C++ path (§9.2) is the fine-control production route and is what all
the dialed feel numbers assume. Flex-compatible by handshake (both `hdtsmp64` and `hdtSMP64` senders).
# 06 — Bone-Follow Collision, Beast Tails, and Cavity Rings (research, 2026-07-05 evening)

Source-grounded research (NIF forensics on XP32 + winner skeletons, PLANCK source read, web-verified
engine docs) answering: how do actor Havok bodies MOVE, how do the beast tails work, and how to build
the goals-charter cavity structures. Full agent outputs archived in session task files; all essential
numbers inlined here.

## 1. THE MOVEMENT MODEL (the big one — reframes how we think about every body)

**Constraints do not move bodies. Bodies do not need joints to track bones.** The five mechanisms by
which an actor-attached rigid body moves:

1. **Node-follow sync (engine-native, the default)** — every skeleton body is a
   `bhkBlendCollisionObject` with flags **137 = ACTIVE | SET_LOCAL | SYNC_ON_UPDATE**: the engine
   copies the animated NiNode's transform into the body every anim update
   (`bhkNiCollisionObject::UpdateCollisionFromNodeTransform`, virtual 2C; toggled per-actor by
   `BSAnimationGraphManager_DisableOrEnableSyncOnUpdate` 0x61A2E0). **No constraint involved.** This
   is how ALL 18 humanoid bodies track animation outside PLANCK, and how the 5 tail bodies track it
   always.
2. **Blend-weighted variant** — `bhkBlendCollisionObject.blendStrength` @+0x28 (0 = follow rigidbody
   sim, 1 = follow node; own `motionType` @+0x30). Every skeleton body is this class — there is **no
   plain bhkCollisionObject anywhere** in these skeletons.
3. **Ragdoll-driver PD-drive** — PLANCK's hkbRagdollDriver path. Membership = the
   `hkaRagdollInstance` from **skeleton.hkx (18 Ragdoll_* bones, shared by beasts — there is no
   skeletonbeast.hkx)**, NOT the NIF body census. A body outside the instance is never PD-driven,
   never motion-type-forced, never velocity-capped by PLANCK.
4. **Pure dynamic + constraint tree** — death/knockout ragdoll. THIS is what the constraints are for:
   they keep the chain attached when real physics takes over (and act as guardrails/force hinges in
   driven mode). The game even has `BSAnimationGraphManager_RemoveNonRagdollRigidBodiesFromWorld`
   (0x61A0E0) — "non-ragdoll rigid bodies" is an engine concept.
5. **Runtime keyframed ride** — our marker technique (`applyHardKeyFrame` per frame).

**Verdict on "connect a capsule to an XP32 bone node for movement": engine-native YES — it is
literally what every body already does while the actor is alive.** Author a blend collision object
on the bone's NiNode; add a constraint ONLY so the piece behaves at death.

## 2. Beast tails — forensics (XP32 male/female + VRTouchEvents V2.0 winners, block-level)

- **The tail bodies exist and are full physics**: TailBone01–05, chain per bone
  `NiNode → bhkBlendCollisionObject(137) → bhkRigidBody(non-T) → bhkCapsuleShape`. Tapering capsules
  r 1.57/1.46/1.31/1.15/1.05u, segments 12.5–13.6u, mass 1.0, motionSystem 4 (BOX_INERTIA),
  quality 1 — **byte-identical system/quality to core bodies**. The NIF carries NO "PD vs follow"
  marker; tails are follow-only purely because skeleton.hkx's ragdoll maps 18 bones and no tail.
- **They ARE constrained** (the "tails have no constraints" folklore is wrong): 5 bhkRagdollConstraints
  — TailBone01→**NPC COM** (not Pelvis), then 02→01, 03→02, 04→03, 05→04. Death-flop equipment.
- **Two authored discriminators vs core bodies**: collision layer **0** (core = 8 SKYL_BIPED) and
  mass 1.0. PLANCK's `entityAddedCallback` converts non-ragdoll Character bodies on layer Biped to
  **DeadBip** (`convertNonRagdollBipedObjectsToDeadBip` default TRUE, config.h:337; main.cpp
  1692-1733, 2373-2394) — layer-0-authored tails presumably get engine layer reassignment first
  (in-game read needed).
- **Hit/touch attribution works for ANY actor-attached NIF body**: PLANCK resolves refs via
  `GetRefFromCollidable` (0x3B4940) — collidedRefs / shove / physics damage never check ragdoll
  membership (main.cpp 2304-2338, 3655, 1993-2000). HIGGS-hand vs a tail body = touching the actor.
  (Raw SDK-created bodies WITHOUT the node/collision-object property chain do NOT resolve — our
  runtime markers are invisible to attribution; NIF-authored bodies are first-class.)
- **The female-winner bug, exactly**: VRTouchEvents V2.0's skeletonbeast_female.nif dropped **20
  blocks** — the complete TailBone01–05 physics set (5 collision objects + 5 bodies + 5 capsules +
  5 constraints incl. the COM anchor). The male winner kept everything (byte-equal to XP32 + 3
  NiNodes). Female TailBone NiNodes + full animation hierarchy SURVIVE (plus HDT Tail, CME chains;
  +102 grafted NiNodes vs XP32) — the tail physics was a casualty of the custom-skeleton graft.
  **Fix = block-APPEND the 20 blocks back from XP32's female original** (append keeps absolute refs
  valid — the proven low-risk surgery direction).
- **HDT fluffy-tail caveat (load-order reality)**: "HDT SMP fluffy beast race tail replacer" rigs the
  VISIBLE tail mesh to `HDT TailBone*` (SMP-animated); the Havok capsules ride vanilla TailBone01–05,
  which keep playing the vanilla tail animation invisibly → restored tail collision will NOT match
  the visible fluffy tail on NPCs using that replacer. CBPC tail spheres (CBPCollisionConfig_Tails.txt)
  already cover both chains. Decide per-race how much this matters before restoring.

### The SMP-vs-Havok tail architecture (verified against the user's actual config, 2026-07-06)

The user's live Khajiit tail = "HDT SMP Fluffy Beast Race Tail Replacer Tweaks", config
`meshes\actors\Character\Character Assets\Tail_long_Khajiit.xml`. What it actually is:
- **SMP SIMULATES the tail**: anchor (static) bone `HDTS TailBone01` + body bones as kinematic
  references; **~22 DYNAMIC bones** `HDTS TailBone02`…`HDTS TailBone09` then `HDTS TailBone09.001`…
  `.012`, mass-tapered 0.7→0.4→0.3→0.25→0.15, linear/angularDamping 0.9, joined by
  `generic-constraint` spring chains (linear/angularStiffness + limits) = the flick/hang/momentum.
- **Bones are `HDTS TailBone*`, NOT vanilla `TailBone01-05`** → the vanilla Havok tail capsules (the
  ones the female skeleton dropped) ride the WRONG bones. Restoring them = collision on the vanilla
  base-animation tail, desynced from the visible fluffy tail. **Attach any Havok collision capsule to
  the `HDTS TailBone*` chain instead.**
- **SMP EXPLICITLY excludes hands**: every tail `per-vertex-shape` carries
  `<no-collide-with-tag>hands</no-collide-with-tag>` + `hand`. So the SMP tail does NOT collide with
  the player's HIGGS/Havok hands today — that is precisely the gap a Havok follower capsule fills.

**The doctrine (motion vs collision are SEPARATE jobs — do NOT swap SMP for Havok):**
1. A `bhkBlendCollisionObject` capsule is a FOLLOWER, not a simulator — it copies wherever the bone is
   and supplies ZERO gravity/momentum. Remove SMP and rely on it → the tail plays only the base idle
   animation (stiff scripted wiggle) and the capsule shadows that. The feared "stiff tail" is real.
2. Making the Havok bodies themselves gravity-simulate (dynamic + their ragdoll-constraint chain,
   writing back to bones) needs the tail bones in the SHARED skeleton.hkx (HCT surgery, high-risk),
   gives 5 coarse segments vs SMP's 22 supple ones, and FIGHTS SMP if both run → worse than SMP.
3. **RIGHT architecture = LAYER**: keep SMP as the motion engine; author a Havok blend-collision
   capsule (or a subset, ~5–8 along the length) attached to the `HDTS TailBone*` bones SMP drives.
   SMP writes the gravity flick to those bones every frame → the capsule follows → collision rides the
   flick for FREE, no push needed. This is the collidable-tail deliverable.

**Two verifiable unknowns before building (in-game tests):**
- **Update ordering**: FSMP writes bone WORLD transforms late in the frame. Does
  `UpdateCollisionFromNodeTransform` (the blend-sync) read the bone AFTER SMP's write same-frame, or a
  frame stale / pre-SMP? A 1-frame lag is imperceptible; a full desync is not. Same test class as the
  PLANCK sync-on-update question (§3). Test with the visualizer on a moving Khajiit.
- **Force direction is ONE-WAY**: the follower capsule moves WITH the tail (has velocity → can brush/
  bump the player's hand), but the player's hand CANNOT push the tail back into SMP's sim (SMP doesn't
  read Havok contacts). Good enough for touch-detection + the tail swishing against you; true grab/push
  of the tail is a further step (SMP would need to see the hand as a collider).
- **Fallback if ordering fails**: PPB's own runtime keyframed-ride technique (read the `HDTS TailBone*`
  world pos in a late/post-physics hook — WE choose the timing — and place a collidable capsule).
  Sidesteps ordering, but runtime SDK bodies don't resolve to the actor ref → PLANCK hit-attribution
  won't see them (fine for a self-contained detector, not for PLANCK-routed hits).
- **Note**: for VRTouchEvents touch DETECTION, `CBPCollisionConfig_Tails.txt` may already cover the
  tail; the Havok capsule is for PLANCK/HIGGS physical interaction, the more ambitious goal.
- **Wings/new appendages recipe** (vampire lord/werewolf, own skeleton paths — winners "Vampire Body
  for VR" line 784 / "Werewolf Body for VR" 785): clone the tail pattern — blend body(137) on the
  wing bone + bhkRagdollConstraint to the nearest ragdoll body for death behavior. NIF-authored,
  zero runtime code. Mind the per-body perf rule: appendage bodies are the ONE sanctioned body-count
  exception (vanilla itself does it for tails), keep counts small.

## 3. PLANCK treatment of non-ragdoll bodies (line refs = tools/_research/planck_src/src/main.cpp)

- Drive/motion-force/hinge-conversion/filter-refresh/stress checks all iterate
  `driver->ragdoll->m_rigidBodies` ONLY (4427-4441, 2961-3005, 272-307, 726-757, 4888).
- Self-collision filter branch (2440-2561) keys on layer Biped + same group; DeadBip bodies bypass it.
- Getup check is the game's own 0x600DF0; PLANCK only adds grab gating (5584). Whether it counts
  non-ragdoll bodies: unknown.
- **The make-or-break in-game test before building on tails**: does PLANCK's
  `DisableOrEnableSyncOnUpdate(actor, true)` (4348) strip SYNC_ON_UPDATE from ALL graph bodies or
  only ragdoll-instance ones? If it strips the tail's sync, live tail bodies go stale under PLANCK.
  Test: restore female tails (or use a male khajiit), collviz + HIGGS-poke the tail, read layer +
  live motion (PIVTRACK-style ball-vs-bone on TailBone03).

## 4. Cavity rings (mouth + 2 pelvic) — engineering result

**User design confirmed: a ring of wall capsules around an empty core.** With one big catch found by
the numbers: **the void is currently SOLID** — the canal corridors sit inside the existing host
capsules, so the host must be CARVED (capsule → bhkListShape whose bulk children reproduce the body
while leaving the corridor empty) — the same one-time list conversion the hand plate proved, walls
added as siblings.

- **Anchor nodes all exist in PPB's skeleton_female.nif** (skeleton-space bind, gu):
  `Mouth00` [0, 3.94, 118.07] + MouthU/D/L/R (lips: half-width 1.34u, half-height ~1.0u, center
  [0, 7.1, 117.6]); `Anal`/`NPC Anus00` [0, −5.54, 68.35] + AnusF/B/L/R01→02 + Anus Deep1→2 chains
  (a SECOND duplicate `Anus` rig exists under Genitals — CBPC uses the `Anal` set); vaginal cluster
  VaginaB/F/L/R1 entrance ≈ [0, −4.4, 65.0] → VaginaDeep2 [0, −2.47, 72.57] (canal axis
  (0, 0.243, 0.970), length 7.8u); Clitoral1 [0, 1.52, 65.03]; NPC L/R Pussy02 [∓3, 0, 64.91].
  Multi-bone chains (Anus*, VaginaD*) = CBPC-animated opening bones — **walls stay static in the
  host body frame; opening animation will not move Havok walls (accepted for v1)**.
- **Hosts**: `NPC Pelvis [Pelv]` carries NO body — the pelvis body is on **NPC COM [COM ]**
  (bind identity at [0,0,68.91]; current capsule ±2.93x, −4.02y, 5.60z r6.30, **material 0, not
  skin** — decide at bake whether bulks inherit 0 or go skin; walls must be skin 591247106).
  Mouth host = **NPC Head [Head]** (bind [0, −1.548, 120.344], ~identity; capsule near-sphere r8.59).
- **Solidity math**: vaginal canal enters the COM capsule at t=0.42 (only ~3.3u of 7.8u open);
  anal opening sits exactly ON the r6.30 surface (rectum fully solid); lips at 8.45 from head center
  vs r8.59 (mouth fully solid). Carving is mandatory.
- **First-pass recipes** (body-local gu; ring: N capsules parallel to axis A on circle
  R = r_opening + r_wall):
  | Cavity | Host | N | r_wall | R (opening) | Segment | Wall gap |
  |---|---|---|---|---|---|---|
  | Vaginal | NPC COM | 6 | 1.5 | 4.0 (2.5) | [0,−4.64,−4.87]→[0,−2.82,2.41] | 1.0u |
  | Anal | NPC COM | 6 | 1.25 | 3.0 (1.75 = CBPC) | [0,−5.79,−1.25]→[0,−3.66,3.82] | 0.5u |
  | Mouth | NPC Head | 6 | 1.0 | 2.2 (1.2) | [0,9.15,−2.73]→[0,4.66,−2.47] | 0.2u |
  Carve bulks: COM 1→3 (front-belly r4.5 + two posterior-lateral hip r3.5, mid-sagittal posterior
  corridor left void); Head 1→2 (cranium r~7 + occiput/jaw side, mouth corridor void).
  Final child counts: **COM ≈ 15, Head ≈ 8**.
- **Adjacency luck**: COM is constraint-joined to Spine0 + BOTH thighs → the only bodies overlapping
  the wall region at rest never self-collide with it. Head joined to neck. Player hands (separate
  group) always collide = the intended function. Watch: NPC's OWN hands idling at lap/face during
  PLANCK self-collision windows grinding on walls.
- **Perf** (report-14 model): ~22 added children, body count unchanged → broadphase unchanged;
  worst case ≈ 2.3µs per touching hand per step; both hands + mouth ≈ 0.06% of the 11.1ms frame.
  Non-issue.
- **CBPC counterpart**: pelvic zones already exist in the winning config (VRTouchEvents V2.0
  CBPCollisionConfig_Female.txt — Pelvis genital sphere 0,−1,−3.5 r2.5; Anal 0,−5.5,−1.8 r1.75;
  VaginaB1/Clitoral1 r2.0; Pussy02 capsules; VaginaOpeningMultiplier=3/Limit=20, Anus same,
  BellyBulge=2/Max=20). **Zero Mouth/Tongue CBPC entries anywhere — a mouth zone on Mouth00 is new
  authoring** (node exists, declare in [AffectedNodes] + sphere line).

### In-game test list for the cavity build
1. >8 list children on a ragdoll bone body is unprecedented (max seen: 8 on a dragon bumper phantom,
   3 on our R hand) — test COM with 15 before trusting.
2. Carved bulks change the shape while mass/inertia/COM floats stay baked — watch corpse settle;
   re-bake inertia floats if drastic.
3. r_wall 1.0u (mouth) tunneling on fast VR hand swipes — R hand proved 1.25u; mouth is borderline.
4. NPC own-hand jitter vs walls during touch windows.
5. Live-editor gap: 12 capsule slots + 3 hand children exist today; ~23 new children need an editor
   extension (slot family or indexed child access) before by-eye dialing.

## 5. Corrections to earlier records
- Report 14's "tail bodies … never PD-driven" stands, but its implied "blend-collision (only)" framing
  understated: **tails have full constraint chains**; the follow-vs-drive split is purely hkx rig
  membership, not NIF structure.
- "The ragdoll has 18 bodies" is the humanoid census; beasts = 23 (18 + 5 tail) when the NIF is intact.
- The 22 bhkBlendControllers in beast skeletons sit on collision-less TOE bones — vestigial, not part
  of the tail mechanism.

---

## ★ SMP-vs-HAVOK "TWO WORLDS" — the hair/cloth/tail collision reality (2026-07-08 discussion, UNVERIFIED-in-VR)
When adding a follower Havok capsule to an SMP-driven hair/cloth/tail bone so the player can touch it, be
precise about what does and doesn't happen — this is genuinely finicky and the empirical answer is PENDING:
- **Two separate physics worlds.** The game's Havok world (ragdoll, our capsules, the HIGGS/PLANCK hand)
  is DISTINCT from SMP's own softbody solver world. SMP collides its mesh against ITS OWN config objects
  (per-bone capsules/spheres in the SMP XML), and by default does NOT import the game's Havok ragdoll.
- **What a Havok capsule on a hair bone DEFINITELY gives you: TOUCH DETECTION.** The HIGGS hand collides
  with it (game Havok world) → contact felt → drive events (VRTouchEvents / SkyrimNet reaction). Solid.
- **Whether it makes the hair MESH physically react (part/swing around the hand) is UNCERTAIN** and
  depends on the stack: SMP hair (HDT-SMP) is a separate sim → a Havok capsule likely gives touch-detect
  only, NOT mesh reaction (mesh reaction comes from the HAND's own SMP collision sphere, if configured).
  BUT if the hair is HDT-PE (older, Havok-based) it WOULD react. VERIFY per-setup, don't assert.
- **★ The user's sharper model (why hair/clothes already follow a body push):** SMP objects are ANCHORED
  to bones — wig root to the head bone, clothes to body bones, all ragdoll-driven. So HIGGS pushing the
  BODY's Havok capsule → PLANCK drives the ragdoll → the anchor bone moves → the SMP objects get carried
  and their dynamics play out from that moving anchor. Havok/HIGGS effectively WINS the anchor motion.
  The open part is only the strand-LEVEL reaction (SMP computes strands relative to the anchor; a follower
  capsule on a STRAND bone FOLLOWS the SMP strand, it doesn't drive it — SMP overwrites it each frame).
- **RESOLUTION METHOD = empirical, not theory.** Hang a Havok capsule on a wig bone, poke it in VR, watch:
  rigid-follow vs mesh-part. Same experiment class as the NPC finger-capsule test. Do it before investing.
- **Perf discipline (ties to reports 11-13):** hair/cloth capsules must be FILTERED to collide with the
  HAND only (a few pairs = cheap), NOT everything, or they pile onto the contact-volume stutter. Be
  selective — a handful of capsules on reachable outer bones, not one per SMP bone (30-50 of them).
- Hair/cloth bones come from EQUIPPED ITEMS (not the base skeleton) → RUNTIME attach (detect bones on
  equip + hang followers), unlike the fixed body skeleton we bake into skeleton_female.nif.

---

## ★★ FOLLOWER-BODY AS-BUILT RECIPE (2026-07-09, deployed + source-verified) — the reusable pattern for the WHOLE appendage tier
PPB's FIRST actual added bodies: 4 test collision capsules on the NPC RIGHT-hand fingers. This is the
proven, CTD-safe recipe for adding a touchable Havok collider to ANY non-ragdoll bone (fingers, hair,
tails, ears, horns) — reuse it verbatim.
- **Host nodes**: the target NiNodes (fingers used NPC R Finger10/20/30/40 = proximal of index/middle/
  ring/pinky, NO thumb). Any moving XP32 bone works.
- **Body chain (clone a vanilla TailBone body field-for-field)**: NiNode → `bhkBlendCollisionObject`
  (flags **137** = ACTIVE|SET_LOCAL|SYNC_ON_UPDATE, heirGain=velGain=1.0) → `bhkRigidBody` (non-T),
  **constraintCount=0** (NO ragdoll wiring — simpler/safer than vanilla tails which DO carry constraints)
  → `bhkCapsuleShape` material=SKIN, point1=(0,0,0)=host origin, point2 = the bind chord to the child
  node (bakes in the bone's world-scale), radius **0.5u** (0.2u tunnels on a moving VR hand — too small).
  Body fields cloned from `skeletonbeast_female.nif` TailBone01: motionSystem=4(DYNAMIC), qualityType=1,
  mass=1.0, isotropic inertia diag=0.0152254, damping/friction/restitution as vanilla.
- **★ THE COLLISION FILTER (the load-bearing bit, verified against PLANCK source)**: layer=**8**(kBiped)
  / part=**13**(the host limb's biped part) / systemGroup=**0**(engine assigns the actor's group at
  AddHavok). At world-add PLANCK's PotentiallyConvertBipedObjectToDeadBipTask tests instance membership
  by POINTER (not in the 18-body instance → true) and RELABELS the body to **DeadBip(32)**. Result:
  (a) COLLIDES with the player HIGGS hand (hand layer 56 ↔ 32 is set in higgsLayerBitfield → poke works);
  (b) does NOT self-collide with the NPC's own hand/body (the "non-adjacent self-collide while touched"
  branch fires only Biped↔Biped; the DeadBip relabel removes it — same protection vanilla tails get);
  (c) does NOT break the charController (same NPC group; DeadBip-vs-CharController = ordinary corpse-vs-
  controller, negligible for a 7mm capsule).
- **NO DLL CHANGE, NO ragdoll-driver risk**: the native `bhkBlendCollisionObject(137)` node-sync drives
  the capsule off the animated bone every update (zero DLL). The ragdoll instance is built from
  skeleton.hkx's 18 Ragdoll_* bones and every PLANCK loop is bounded by `ragdoll->m_rigidBodies` — bodies
  are matched by NODE, so a follower on a non-ragdoll node is NEVER in the instance and cannot perturb the
  driver. (Vanilla beast tails = the exact structural precedent: 5 non-ragdoll follower bodies.) The
  NO-GO "does the driver enumerate by count/index" fear is REFUTED with source cites.
- **Bake tool**: `bake_torso_wave2b.py <in> <out> --skip-torso --fingers --finger-r 0.5` (--skip-torso
  required or the double-bake guard aborts on the already-baked torso). Bodies +4 (18→22), ragdoll
  constraints UNCHANGED, +12 blocks. Deployed skeleton md5 b37d149d; rollback at
  `tools/ppb-scratch/bake-fingers-2026-07-09/skeleton_female.PRE_FINGERS.nif`.
- **Live-tune option (present, default OFF)**: `fingerCapTrack 1` + `fingerCapR` engages GrabDiag's
  per-frame far-endpoint rewrite to follow the BENT finger (else the static capsule is the bind chord).
  Not needed for the raw first-look. Static capsules have no live knob — re-bake to resize.
- **AWAITING IN-VR OBSERVATION**: does a poked finger capsule ride the bone rigidly (drag the hand/
  skeleton), bend, or just get pushed? That result decides whether NPC finger/hair/tail collision is
  worth building out — same experiment class as the SMP-vs-Havok hair question above.
