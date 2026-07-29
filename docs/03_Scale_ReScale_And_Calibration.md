# 03 — Scale & Calibration (ReScale + the self-correcting per-NPC system)

**How every NPC's ragdoll + capsules land on their XP32 nodes, at the right SIZE, automatically.**
Combined reference (was Reports 20 + 16). Companion: Report 04 (ReShape) fits body SHAPE on top of
this; the two compose (`live shape = base × trueScale(scale) × shapeRatio(shape)`).

- **Part A — ReScale (arc-sum, generation 4, the CURRENT truth):** pose-invariant scale correction,
  joints + capsules unified; the bake gate (females-only); event-driven re-scale; compute-mode-is-dead.
- **Part B — the calibration framework:** root-conform constant, head trim, and the STATE EDGES
  (furniture / stand-up / rebuild / heel / hf-toggle) that re-measure so no transient becomes an
  NPC's false normal. ⚠ The **measured-scale TAPE** described within Part B is RETIRED (Part A §9
  superseded it with arc-sum); the root-conform / head-trim / state-edge machinery remains LIVE.

Superseded generations (dropped): gen-3 straight-line X/H (old Report 19) and the gen-2 measured-scale
rework (old Report 18 §2) — lineage in Part A §0. Their one durable non-scale lesson, the orphan-capsule
held-world `NiPointer<bhkWorld>` fix, lives in the Pitfall Ledger (Report 01) + Report 07 (rigs).

---

# 20 — The Arc-Sum Re-Scale: Pose-Invariant Scale Correction, Joints + Capsules Unified (2026-07-15)

**Status: DEPLOYED + live-verified across 3 NPCs (Lydia / Carmella / one unnamed). Supersedes Report 19's
straight-line X/H joint factor; closes Report 18 §2's "capsules-not-pivots" caveat and §5's wig-grab
freeze. READ THIS FIRST for anything touching joint/capsule scale — it is generation 4 of the scale
correction and the current truth.**

Final builds this arc: `f6489ba9` (arc joints + arc capsules + wig no-grab) → **current** (measuredScale
decoupling, the self-contained fix). Source: `PivFix.cpp` (`PivScaleCorrect`, `PivArcTrueScaleOf`),
`PivFix.h`, `CapFix.cpp` (`CapScaleOf`), `NpcFingerTest.cpp` (the wig shield). Tuning:
`pivReScale 1`, `pivReScaleApply 1`.

---

## 0. The scale-correction lineage (why this is gen 4)

| Gen | Report | Mechanism | Fell because |
|---|---|---|---|
| 1 | 16 | `bodySpan/nodeSpan` off **body origins** | body origin sits at the parent joint (~2.5u off) → read scale-1 Lydia at **1.033**, inflated her; disabled. Misdiagnosed the engine as "random build-timing." |
| 2 | 18 | reference-anchored **XP32-node** distances (COM→head/calf), capsules only | fixed the master (true 1.0), re-enabled. **Caveat: resized CAPSULES, not joint PIVOTS.** |
| 3 | 19 | joint factor = **X_head / H_head** (straight-line span ratio) | reference-free + direction-correct, signed off across 4 races — **but the straight-line span is pose-contaminated (§1).** |
| **4** | **20 (this)** | **pose-invariant ARC-SUM**, drives BOTH joints and capsules | current. |

The bodies land right ~97% of the time; the whole mechanism exists for the ~3% the engine mis-builds.
Every generation refined how that mis-build is *measured*.

---

## 1. THE BUG in gen 3: the straight-line span bends with the spine

Report 19's `factor = X_head / H_head` measures the COM→head **straight-line distance** on the XP32 nodes
(`X`) and on the Havok pivots (`H`). Direction-correct in principle — but the straight-line COM→head span
**is not a rigid quantity**: it shortens when the spine flexes/extends and lengthens when it straightens.
On a single-frame read it swung **0.95–1.06 (≈12%)** on Lydia alone. Because `PivScaleCorrect` fires
one-shot on the first settled frame, whatever pose the spine happened to be in at that instant became the
NPC's permanent scale. On Lydia (the scale-1 reference, who must read exactly 1.000) a flexed-spine frame
gave `X_head/H_head ≈ 0.9475` → the re-scale **spuriously shrank the reference ×0.9475**. Carmella and
others inherited pose-dependent errors the same way. This is the same disease as Report 16's poseConformRoot
("measured the wrong thing on one frame") one layer up.

**Root property:** any *straight-line* distance across the torso is a function of spine flexion. It is the
wrong ruler.

---

## 2. THE FIX: the arc-sum is pose-invariant

Replace the straight-line span with the **arc-sum along the spine chain** — the sum of consecutive node
distances **COM→Spn0→Spn1→Spn2→Neck→Head**. Each link is a rigid bone length; a bending spine changes the
*angles* between links, never the *lengths*. So the arc-sum is invariant to pose (validated flat to 0.001u
across 146 live samples; on Lydia it holds at 48.045–48.046 every frame while the straight-line `H` was
swinging 12%).

```
kArcNode  = { Spn0, Spn1, Spn2, Neck, Head }     // the 5 XP32 nodes (child ends)
kArcOther = { COM,  Spn0, Spn1, Spn2, Neck }     // their partners (the joint the pivot sits at)
nodeArc  = Σ dist(node[i],  node[i+1])            // from the XP32 NODE worlds
havokArc = Σ dist(pivot[i], pivot[i+1])           // from the live Havok CONSTRAINT pivots (jtrack read)
```

### 2a. The scale-1 base (extracted from the .nif — engine-noise-free)

`kBaseArc = 48.045f` = the **COM→...→Head arc-sum minus the COM→Spn0 first link**, i.e. the **Spn0→Head**
arc, read straight from the XPMSSE original female skeleton `.nif` (no engine, no race height, no pose).
The full COM→Head arc in that .nif is 54.5128; the Spn0→Head sub-arc is 48.0450. PPB's shipped
skeleton_female.nif is byte-identical to XPMSSE on these nodes (all 18 ragdoll bodies sit on the XP32 nodes
at 0.0000 delta — the "2011 body-frame offset" is NOT in the geometry). `trueScale = nodeArc / kBaseArc`.
This is **reference-free** — no Lydia, no live capture, no "engine lies about scale." (Lydia's live runtime
arc reads 48.045–48.046 = exactly the .nif base, which is why she maps to 1.000 and confirms the base.)

### 2b. Two ways to apply it — Option A vs Option B (user chose B)

- **Option A (pure XP32 stamp):** stamp each pivot to `baked_pivot × trueScale` using hardcoded baked
  scale-1 pivots. Rejected: needs a fragile A/B entity-copy mapping and a hardcoded pivot table.
- **Option B (arc-ratio, self-correcting) — SHIPPED:** `factor = nodeArc / havokArc`. Both are
  pose-invariant arc-sums, so `factor = trueScale / build_scale`. The symmetric mul4 on all 17 constraints
  drives the Havok toward `baked × trueScale` **without** any hardcoded pivots — it self-corrects to
  wherever the engine actually built the ragdoll. If the engine built at the right scale, `factor ≈ 1` and
  nothing moves; if it under/over-built, the factor grows/shrinks it to match the nodes.

The joint write is the proven symmetric both-copies mul4 loop (all 17 constraints, one-shot per ragdoll
INSTANCE, re-fires only on a PLANCK/AI rebuild). Trigger: `|nodeArc − havokArc| > 0.5u`. Plausibility clamp
`0.3 < factor < 3.0`; no-op band `|factor−1| < 0.002`; gated behind `pivReScaleApply`.

### 2c. Capsules ride the SAME trueScale (closes Report 18 §2)

Report 18 resized capsules; Report 19 resized joints; they were separate systems on separate measurements.
Gen 4 unifies them: `PivScaleCorrect` publishes `s_arcTrueScale[id] = nodeArc / kBaseArc`, exposed via
`ObjectHold::PivArcTrueScaleOf(actor)`. `CapFix::CapScaleOf` reads it **first** (arc → else legacy effScale
→ else GetScale). So joints AND capsules move together off one pose-invariant number. The Report 18 §2
caveat ("if the blue balls still sit low after the capsule resize, feed trueScale into the conform") is
**closed** — the joints ARE re-scaled, by the same factor, in the same pass (`PPBHook.cpp` runs
`PivScaleCorrect` and `CapFixApply` in the same per-actor drive-hook tick).

### 2d. Self-contained (decoupled from the legacy tape)

The arc re-scale originally still bailed early on the old `MeasuredScaleOf` gate (which requires
`measuredScaleEnable` ON). Removed: `PivScaleCorrect` now reads the XP32 nodes + Havok pivots directly and
gates only on settle/anchor-together/speed + the 10m/attached/grab gates. So the whole arc scaling solution
(joints + published capsule scale) is independent of `measuredScaleEnable`. The legacy `effScale` tape
survives ONLY as the CapScaleOf fallback for the brief pre-latch window before the arc latches.

---

## 3. THE WIG-GRAB FREEZE FIX (closes Report 18 §5)

Report 18 §5 left open: the 0.05kg garment/wig bodies + a HIGGS grab = `a=F/m` NaN → Havok solver hang.
Fixed in `NpcFingerTest.cpp`:
- **Non-grab shield** (`FilterDecision`): the HIGGS near/far grab-select phantom forces its filter to layer
  **40 or 44**; when a rig capsule (identified by its IMMUTABLE signature — `layer==kHiggsLayer &&
  part==kFingerPart && bit15`) meets a layer-40/44 body, return **Ignore(2)**. This removes garment/wig
  bodies from HIGGS's grab hit-set by signature, not by the old g_rigGroups scan (which missed on group
  drift — the true defect the workflow falsified). PLANCK returns Continue for the same pair, so PPB's
  Ignore always wins.
- **Velocity clamp** (`CreateFingerBody`): bodies < 1kg get `maxLinearVelocity 10 / maxAngularVelocity 50`
  (vs 100/500) — a second guard against numeric blowup on a light body.

Wig bodies kept the light 0.05kg mass (cloth-friendly) AND stopped freezing. Live log this session: wig
NFING TRACK gaps 0.01–0.05u, 0–1 teleports, no NaN, no freeze. **NOTE (honesty): "no freeze in the log"
is not the same as a confirmed grab-test — a deliberate hair-grab-without-freeze still needs an in-VR pass.**

---

## 4. Live result (this session's PPB.log)

| NPC | FormID | engine-built havokArc | nodeArc | trueScale | re-scale | residual (locked) |
|---|---|---|---|---|---|---|
| Lydia | 000A2C94 | 45.55 (≈0.948, −5.5%) | 48.05 | **1.0000** | ×1.0548 | ×1.0005 |
| Carmella | 1301DE4F | 43.45 (≈0.904, −5.0%) | 45.71 | **0.9514** | ×1.0519 | ×1.003 |
| (unnamed) | 03003139 | 50.27 (≈1.046, +2.5%) | 49.05 | **1.0208** | ×0.9756 | ×0.998 |

Matches Report 19's signed-off values (Lydia within buffer, Carmella 0.951) and proves **both-directions**
correction (the third NPC was over-built and SHRUNK) — which no constant could do. The engine mis-builds
by varying amounts in either direction (−5.5% / −5.0% / +2.5%), confirming the per-NPC arc measurement is
the right tool, not a compute-mode constant (Report 19 §3 already rejected `1/NAM6`).

**The corrections are invisible-by-design.** They fire within ~1s of an NPC settling (the arc needs a
settled, connected ragdoll) and snap the joints+capsules onto the nodes. The user sees "she's good now,"
not a defect being repaired — because the mis-build is a physics-layer error the player can't see directly.

---

## 5. Open / watch items (honest)

1. **"Too big on walk-in" (~70% understood, NOT confirmed).** The arc only latches once the NPC is settled
   (~46s after walk-in on Lydia this session, at 17:15:46). In the pre-latch window `CapScaleOf` falls back
   to the legacy `effScale` tape, which read Lydia noisily (0.9873–1.0365, i.e. up to +3.6%). Most likely
   cause of a brief "too big" flash the user reported. Not proven — the first effScale log line postdates
   the arc latch, so I could not confirm effScale was latched high DURING the walk-in.
2. **The legacy tape still chatters.** `CapFix … MEASURED trueScale=1.0365 → resizing (engine scale off)`
   lines still fire (Lydia read 0.9873→1.0365, Carmella 0.9752) — but are **ignored** while the arc is
   latched (code-verified: `CapScaleOf` returns arc first; effScale is only the pre-latch fallback). The
   "resizing" text is misleading (nothing resizes to it). **Candidate cleanup:** retire the noisy tape from
   the CapScaleOf fallback so the pre-latch window uses stable GetScale and the misleading logs stop.
3. **Wig grab-test pending.** The shield is deployed and the log is freeze-free, but a deliberate in-VR
   hair-grab-without-freeze has not been confirmed this session.
4. **Tail is the one judgement call** on the non-grab shield (a player may WANT to grab a tail); currently
   the shield treats all rig capsules the same.

---

## 6. Code map

- `PivFix.cpp::PivScaleCorrect` — the arc measurement + Option-B mul4. Publishes `s_arcTrueScale[id]`.
  `PivArcTrueScaleOf(actor)` getter (returns 0 if unmeasured / out of 0.3–3.0). `PivReScaleClearAll()`
  clears `s_rescale` + `s_arcTrueScale` on kPreLoadGame AND kNewGame.
- `PivFix.h` — `float PivArcTrueScaleOf(RE::Actor*)` in `namespace ObjectHold`.
- `CapFix.cpp::CapScaleOf` — arc first (`PivArcTrueScaleOf` 0.3–3.0), else effScale (if
  measuredScaleEnable), else GetScale. Feeds every capsule endpoint + radius via `CapRegionScaleOf`
  (× OBody body-shape ratio).
- `NpcFingerTest.cpp` — `FilterDecision` non-grab shield + `CreateFingerBody` <1kg velocity clamp.
- Tuning (`PPB_tuning.txt`): `pivReScale 1` (master), `pivReScaleApply 1` (write, not diagnostic-only).

---

## 7. Reusable lessons

1. **A one-shot per-NPC measurement must use a POSE-INVARIANT quantity.** Straight-line spans across the
   torso bend with the spine; sum-of-rigid-bone-lengths (the arc) does not. Same family as Report 16's
   "measure the settled result, not the drive target" and Report 19's "head-prefer over calf."
2. **Extract the scale-1 base from the source asset, not from a live reference.** The .nif arc (48.045) is
   engine/race/pose-free; a live "reference NPC" carries all three. Reference-free beats reference-anchored.
3. **A self-correcting factor (Option B, nodeArc/havokArc) beats a hardcoded stamp (Option A).** It adapts
   to wherever the engine actually built the ragdoll, needs no baked-pivot table, and no-ops when the build
   is already correct.
4. **Unify measurements that drive the same physical thing.** Joints and capsules are the same scale;
   compute it once (arc) and feed both — don't run two systems on two measurements (the Report 18/19 split).
5. **A "fix" is not confirmed until it's the right kind of confirmed.** An invisible correction working =
   the feature doing its job (verify by log/jtrack, not by "looks fine"); a freeze fix = a deliberate
   repro that no longer freezes (log-absence ≠ tested). Don't conflate them when reporting.

## 8. THE BAKE GATE — females-only (2026-07-15, same-day follow-up; the critical scope fix)

**The bug the user caught:** the runtime re-scale + capsule fitter had **no skeleton check** — they resolved
the XP32 node names on EVERY driven actor and wrote to whatever bodies they found. So **males, undead/draugr
skeletons, and custom-skeleton females got female-calibrated writes applied to their stock bodies** — joints
re-scaled, capsules scaled by the FEMALE `.nif` base (48.045, meaningless for male proportions), arms looking
"half-built" (CapFix wrote the main capsules but a stock single-capsule body has none of the 121-capsule
flesh-fit children). The NIF is female-only; the DLL was whole-population. This is Report 17's **17A-15
(bake fingerprint) / 17A-19 (males)** open item — designed, never built — biting exactly as forecast.

**The fix (`GrabDiag::ActorCarriesBake`, shared by both geometry writers):** our bake converts the COM ragdoll
body to a **`bhkListShape`**; every stock skeleton keeps a single `bhkCapsuleShape` COM. List-shapes on
ragdoll bodies are UNPRECEDENTED in vanilla (Report 07: 0 across 19,057 NIFs), so **list-COM ⟺ our bake**.
`PivScaleCorrect` (re-scale) and `CapFixApply` (capsule fitter) both early-return if the actor's COM isn't a
list. Cached per FormID (null COM = 3D mid-load → retry, don't cache; erased on ragdoll rebuild for the
werewolf/VL skeleton swap; cleared on load). Pose-conform / clav-follow / heel stay whole-population (they
recompute from each actor's OWN live nodes — harmless on males, per 17A-19). **Empirically validated against
the deployed NIFs:** female COM = `bhkListShape [21 children]` (included); male (VRTouchEvents stock) COM =
`bhkCapsuleShape` (excluded); draugr COM = `bhkCapsuleShape` (excluded). Method chosen over the Report-17
`NiStringExtraData PPB_BAKE_vN` fingerprint because it needs no re-bake and is 100%-discriminating in this
load order; the fingerprint stays the belt-and-suspenders upgrade for the next bake (also catches a mod
re-sort reverting our bake).

## 9. THE LEGACY TAPE — fully retired (2026-07-15, resolves §5 open items 1 + 2)

`CapScaleOf` dropped the `effScale` fallback tier (arc → GetScale). The effScale SAMPLER is knob-gated on
`MeasuredScaleEnabled()` at all three of its entry points, and the compiled default flipped to
`measuredScaleEnable = 0`. Net: the tape no longer runs, no longer logs the misleading "resizing (engine
scale off)" lines, and no longer drives capsules. The pre-arc-latch fallback is now stable **GetScale**, which
also **kills the "too big on walk-in" transient** (§5 item 1 — the noisy tape was the cause). `MeasuredScaleOf`
is now dead (only a forward-decl in PivFix remains); `s_effScale`/`s_trueScale` are unpopulated with no
consumers. The knob is a dormant lever — do not re-enable (the arc owns scale).

## 10. THE SETTLE BUG → EVENT-DRIVEN RE-SCALE (2026-07-15)

**Terminology (user rule):** the constraint pivots that must sit on the XP32 nodes are called **"Havok joints"**
(the blue balls in the visualizer). Same objects the code logs as `PIVARC`/`PIVRESCALE` and older reports call
"pivots" — treat pivot ≡ Havok joint.

**The bug:** the re-scale was ONE-SHOT per Havok-joint instance, and it fired on a SETTLING TRANSIENT. On a
rebuild/load the ragdoll passes through transient scales before settling (Lydia measured havokArc **45.557**
mid-settle, then settled to her real **50.51**). Writing ×1.0546 on the 45.557 transient corrected the wrong
value; the one-shot then blocked the real correction, leaving her stuck oversized. **Log signature: PIVARC
measures too-big every frame but NO PIVRESCALE write.** The mul4 math itself is CORRECT and LINEAR (Carmella
grew 43.45 ×1.0519 → 45.71 and held) — the bug was purely TIMING.

**Interim fix (deployed): settle-debounce + retired one-shot.** Write only when the arc is STABLE vs the
previous ~1 Hz check (`|havokArc − lastHavokArc| < 0.5u`), and re-apply whenever the settled arc is off
(covers rebuild/cell-load drift). Added `settled=` to the PIVARC log. This WORKS (Lydia + Lajjan corrected to
factor ≈1.004, and Lajjan self-healed through a warp) — but it re-measures the arc ~1 Hz forever, which is the
burden the final design removes.

**★ THE FINAL DESIGN (user's failsafe state machine — the target architecture):** act only on a rebuild, no
continuous scanning:
1. **TRIGGER** = the Havok-joint constraint instance pointer changes (a rebuild — PPB already sees this; it is
   the engine's `AddRagdollToWorld 0x5D1B50` / PLANCK distance-activation / 3D reload). ALSO re-arm on the
   `ActorRagdollAttached` false→true edge (stand-up) — a seated NPC re-attaches WITHOUT a rebuild.
2. **SETTLE 1–2 s.**
3. **ANCHOR gate:** the COM-side Havok joint of the spine0↔COM constraint within 0.5u of the XP32
   `NPC COM [COM ]` node (the COM is the arc root = least-oversized joint = cleanest "driven onto the skeleton
   yet" signal). Fail → wait 5 s, retry, up to 12× → idle until the next trigger.
4. **STABILITY gate:** 5 arc reads @ 0.5 s; if the spread (max−min) > 0.5u (still settling) → wait 5 s, retry,
   up to 12× → idle.
5. **APPLY the MEDIAN of the 5** → idle (one pointer-compare per tick, watching for the next rebuild).
The applied correction **HOLDS** because PPB's `activeragdoll.ini` `loosenRagdollConstraintPivots=0` WINS the
loose-file conflict (PPB modlist priority 47 ≫ PLANCK 1993) — PLANCK does NOT collapse our Havok joints, so
"apply once → idle" genuinely stays put until the next rebuild. `loosenRagdollContraintsToMatchPose` stays 1.

**★★ IMPLEMENTED & DEPLOYED (2026-07-15) — `PivFix.cpp::PivScaleCorrect`, DLL 2026-07-16.** The continuous
scan is GONE. `ReScaleState` now holds `{anchorInst, wasAttached, phase(0 IDLE/1 SETTLE/2 MEASURE), armedAt,
lastRead, lastRetry, retryCount, reads[5], readN}`. Per driven actor per frame:
- **While IDLE (phase 0):** throttled to ~1 Hz (staggered by id). The only work is section (A): resolve the
  spine0↔COM instance (`PivFindJoint`) and compare it to `anchorInst`; on change — or `ActorRagdollAttached`
  false→true — ARM (phase 1, `armedAt=now`, counters reset). That pointer-compare is the whole idle cost.
- **(B) SETTLE:** `now − armedAt < 1500 ms` → return. Runs unthrottled once armed so the timers are frame-accurate.
- **(C) ANCHOR gate — the as-built DEVIATION from the design:** instead of "COM joint within 0.5u of the COM
  **node**" (an absolute joint-vs-node check), it uses **the spine0↔COM joint's two stored pivot COPIES
  coinciding** (`Dist3(s0Child,s0Other) > 0.5u → retry`). **Why:** the heel fix biases the COM ragdoll body up
  by heelZ (~8u on heels) while the mesh COM node stays at floor height, so a joint-vs-node position check would
  FALSELY fail on every heeled NPC. The copies-coincide check is RELATIVE (constraint satisfied ⇒ connected +
  settled), so it is heel-invariant and still exactly the "is the ragdoll attached to the skeleton yet" signal
  the design wanted. Same 0.5u tolerance, same 5 s / 12× retry.
- **(D) STABILITY:** 5 `havokArc` reads at 0.5 s each (`measureArc` lambda = the pose-invariant arc, mid-scramble
  >40 u/s rejected). Median + spread via a 5-element insertion sort (no `<algorithm>`/`<cstring>` dependency).
  `spread > 0.5u` → retry. The anchor gate (C) re-runs every tick during MEASURE, so a mid-measure loosen resets
  the reads.
- **(E) APPLY** `factor = nodeArc / median(havokArc)` to all 17 Havok joints (`mul4` both copies), publish
  `s_arcTrueScale` (capsules ride it), then **phase → 0 (idle) regardless of whether a write was needed** — a
  correctly-built NPC (|nodeArc−median|≤0.5u) writes ZERO and idles. Re-arms only on the next rebuild/stand-up.
- **Retries:** each gate failure calls `retry(why)` — waits 5 s, `++retryCount`; at 12 it parks to IDLE (logs
  "gave up") until the next trigger. A seated NPC (detached) burns its 12 tries in 60 s then idles; standing up
  re-arms via the attached-edge in (A).
- **Removed:** the `DIAG buildRec/buildMeas/...` PIVARC spam (compute-mode is dead, §11), the old 5 s→2 min
  loosened-backoff throttle, the `settled`/`lastHavokArc` debounce, the `scaled[]` one-shot array.

## 11. COMPUTE-MODE IS DEAD — the engine does NOT build the ragdoll at refScale×race (2026-07-15)

Tempting "clean" idea: compute the factor from records (`build_scale = refScale × race = Get3D()->world.scale
/ NAM6`) instead of the settling live havokArc, apply once, never measure. **A live diagnostic REFUTED it:**

| NPC | buildRec (records: world.scale / NAM6) | actual build (havokArc/48.045, uncorrected) |
|---|---|---|
| Lydia | **1.0300** | 1.0513 |
| Lajjan | **0.9500** | 0.9262 |

The records-derived build scale is ~2% off the ACTUAL ragdoll build scale, **per-NPC and in OPPOSITE
directions** (Lydia records-too-small, Lajjan records-too-big). So the engine does NOT scale the ragdoll by a
clean `refScale × race` — the clone-time scaling (in the DRM-encrypted exe, unreadable) does something records
cannot predict. **VERDICT: keep MEASURING the live Havok arc (after settle); compute-mode is out.** This
VINDICATES Report 19's rejection of compute-mode: the arc is pose/heel-invariant yet records STILL don't
match, so it was never a measurement-contamination problem — the records genuinely don't hold.
- Accessors (verified compiling): `root3d->world.scale` = the FULL visual composite (= `GetBaseHeight()` =
  refScale×race×NAM6); `actor->GetActorBase()->height` = raw NAM6. ⚠ `GetScale()` may return only refScale —
  use `Get3D()->world.scale` for the full visual.
- The `1.0511 = 1/0.9514` value that keeps appearing IS the baked-pivot oversizing (the ship bake stored
  scale-1 = Carmella-measured ÷0.9514, Report 05) — Lydia (NAM6≈0.97, GetScale 1.0) loads at 48.045×~1.051.
  But it is NOT uniform (Lajjan's build 0.9262 ≠ 0.8889×1.051): the per-NPC engine build scale rides on top,
  which is exactly why only a live arc measurement gets it right.

## Supersedes / updates
- **Report 19:** the JOINT factor is now `nodeArc/havokArc` (pose-invariant arc-ratio), NOT the
  straight-line `X_head/H_head`. The engine-scale-truth (`refScale × race × NAM6`, deterministic) and the
  compute-mode rejection (`1/NAM6`) both STAND.
- **Report 18 §2:** the "capsules-not-pivots" caveat is CLOSED — one arc drives both.
- **Report 18 §5:** the wig/garment grab freeze is FIXED (non-grab shield + velocity clamp).
- **Report 16:** "measure, don't trust records" doctrine upheld; its "random build-timing" mechanism was
  corrected by Report 19 and stands corrected.
# 16 — The Self-Correcting Per-NPC Calibration System (2026-07-13)

**The single most important subsystem in PPB.** It makes the Havok ragdoll bodies + capsules of *every*
NPC in the load order land on their XP32 nodes — automatically, at runtime, with no per-NPC tuning,
surviving every state transition. Built across one long debugging session (2026-07-12/13). This report
is written so another agent — or another project entirely — can rebuild it, extend it, or transplant
the principles. If you touch scale, root height, or "why is this NPC's body offset," READ THIS FIRST.

Final DLL of the arc: `5c35de52` (`tools/PPB-plugin/`, deploys to the PPB mod). Sources touched:
`CapFix.cpp` (scale), `PPBHook.cpp` (root conform + head trim + edges), `PivFix.cpp` (samplers +
attach gate), `Tuning.*` (knobs).

---

## 0. The problem, stated precisely

An NPC's *rendered* body (skin mesh) rides the **XP32 skeleton** — the standard every animation, body
mesh, and armor in the load order targets. The **Havok ragdoll** (the collision bodies PLANCK drives,
the thing our capsules attach to) was NEVER built to match XP32. Symptoms the user reported, all the
same root disease at different magnitudes: faces sitting 1u off, hands 2u short, whole bodies 5%
too small, feet off the ground, hand capsules poking out of the thumb. "It's not normal to have a face
sitting 1u off." Correct — and the fix is not one constant, it is a **measurement loop**.

**Core doctrine (the lesson that unlocked everything):** the engine LIES about scale and pose. Record
fields (Height, Weight), `GetScale()`, and even the animation drive pose are all unreliable proxies for
where the ragdoll actually IS. Stop trusting any of them. **Measure the live geometry and fit to the
measurement.** The engine can lie all it wants; a tape measure doesn't care.

---

## 1. The three offset layers (each was a separate bug with a separate fix)

The total error on any NPC decomposes into three independent contributions. We found and fixed each:

### Layer A — the ROOT constant (~4.3u vertical) — ★ FIX RETIRED 2026-07-13, THE THEORY WAS WRONG
> **`poseConformRoot` is OFF (retired) as of build `6ed7708e`.** The live joint-track test on Lydia
> (scale-1 master) disproved this whole layer: `poseConformRoot 1` → joints **3.76u mean / 8.02u max**
> off the nodes AND she floats ~2u; `poseConformRoot 0` → joints **0.21u / 0.30u** (ON the nodes) and
> on the floor. The per-bone conform ALONE already lands the joints; the "~4.3u inherited offset" the
> census saw was in the drive TARGET (poseLocalSpace COM: anim 63.9 vs an XP32 "target" 68.7), but that
> target was WRONG — the animation already places the root at the correct feet-on-floor height, so
> forcing the COM up 4.78u just lifts the whole correctly-built chain off the ground. It "worked" on
> statued Carmella only because a bind-pose capture yields rootDelta≈0; captured mid-animation (every
> real NPC) it latches a spurious lift. The knob remains as an escape hatch (default 0); re-validate
> with jtrack before ever trusting it. **Layer A does not exist as originally theorized.** §2 below is
> the historical record of the two attempts.

- **Original (disproven) mechanism**: the ragdoll drive's ROOT bone rides the **vanilla animation rig's**
  root height (`skeleton.hkx` reference pose, model-space Z ≈ 62.4 on a 0.95 NPC) while the rendered
  skeleton uses **XP32's** (COM bind 68.911, live model-space ≈ 66.85). Delta ≈ 4.3u. The per-bone
  pose-conform fixed every bone EXCEPT the root — so (the theory went) the whole chain inherited it.
- **Why the census was misleading**: the uniform dZ −3.7..−4.5 was measured on the drive TARGET, not on
  where the driven bodies actually settle. PLANCK's PD drive + the per-bone conform land the bodies on
  the nodes regardless of the root target's height (jtrack 0.21u with root conform off proves it).
- **Fix (RETIRED)**: `poseConformRoot`. See §2 for the two attempts and why both were wrong.

### Layer B — the SCALE of the ragdoll BUILD (per-NPC, engine-inconsistent)
- **Mechanism**: the engine scales the ragdoll shapes at BUILD time by the actor's **base scale** (NOT
  refscale). But WHICH scale it uses is inconsistent between NPCs of the *same record scale* —
  almost certainly spawn/build timing (when the ragdoll instantiates vs when the actor's scale lands).
- **The smoking-gun measurement** (jtrack bodySpan/nodeSpan ratio, same 0.95 record scale on all):
  | NPC | ragdoll-span ÷ node-span | reads |
  |---|---|---|
  | Carmella / Jetta / Lajjan | **1.05** (= 1/0.95) | ragdoll built at scale 1.0 on a 0.95 skeleton → our scale-1 Lydia capsules FIT → looks right |
  | Roisin / Astanova | **1.00** | ragdoll built at 0.95 → scale-1 capsules misfit 5% → too small, feet off floor |
  Same record, opposite build scale. **Survives disable/enable** (not a timing fluke you can rebuild away).
- **Fix**: the MEASURED-SCALE system (§3). Stop reading any record value; measure the ragdoll's actual
  build scale from the geometry and fit capsules to that.

### Layer C — the CAPSULE dial bias (the master calibration itself)
- The whole capsule set was dialed by eye on Lydia (the scale-1/weight-100 master). Any systematic
  error in that dial rides identically on every NPC. Found one: heads were +0.5u high (nose, chin, all
  head capsules) — a Lydia-session bias. Fixed by lowering all `capHead*` Z by 0.5u in the tuning file.
- **Lesson**: a master-body dial bias presents as "EVERY NPC is off the same way, including the ones
  that measured perfect." When a uniform error survives all the runtime correction, suspect the dial.

---

## 2. Layer A fix: root conform — v1 (feedback disaster) → v2 (capture-once)

**v1 (WRONG, shipped briefly, launched NPCs into the sky):** wrote the root to a target measured from
the LIVE node chain every frame. On PLANCK-driven actors the written root RAISES the ragdoll, and the
rise leaks back into the next measurement (the model/actor frame follows the driven root) → the target
integrates upward → NPCs accelerate into orbit within seconds. **This is the canonical "conform loop
re-measuring through its own write" trap** (now in the pitfall ledger). The live kill-switch knob
(`poseConformRoot 0`) is what saved it — SHIP EVERY CONFORM-CLASS WRITE BEHIND A LIVE KNOB.

**v2 (correct):** capture the root delta ONCE per ragdoll build, from the FIRST conform fire (before any
root write can contaminate the measurement), then apply it as an additive CONSTANT forever. Because the
delta is a bind-vs-bind property, one capture is the right answer for the actor's whole session. Sanity
cap: reject any single-frame delta > 10u (bad blend frame) and retry. Code: `PPBHook.cpp` `ConfCache`
holds `rootDelta[3]/rootDeltaValid`; the TLS `rootMode` (0 off / 1 capture / 2 apply) carries it into
the inner hook write.

**Principle (ledger rule):** a per-frame corrective write must take its reference from (a) bind/reference
data or (b) a one-shot capture from the PRE-write epoch — NEVER from state the write itself moves.

---

## 3. Layer B fix: the MEASURED-SCALE system (the user's design — cheap, lazy, self-calibrating)

> **★★ SUPERSEDED 2026-07-14 — READ REPORT 18.** The measured-scale system was REWORKED and RE-ENABLED
> (build `a35c315b`): it now measures XP32 **node** distances (COM→head, COM→calf) reference-anchored on
> Lydia, NOT the body-origin `bodySpan/nodeSpan` below. That fix makes the master read a true 1.0 (no
> inflation) while still correcting genuinely mis-scaled NPCs, and it is throttled for crowds. The
> disabled-by-default note below is the INTERIM state; the rework replaced it. §3's algorithm below is
> the OLD (body-origin) math — kept for history; the live math is Report 18 §2.
>
> **★ (interim) DISABLED BY DEFAULT 2026-07-13 (build 406d17e7) — `measuredScaleEnable` default 0.** In VR the
> tape-measure read **1.0333** for the scale-1.0 master (Lydia) and inflated her capsules past her mesh
> ("her Havok body is sticking out of her shell"). ROOT CAUSE: the ratio `bodySpan/nodeSpan` has a
> SYSTEMATIC baseline ≠ 1.0 even for a perfectly scale-1.0 NPC, because the ragdoll BODIES sit ~2-4u off
> the XP32 NODES (the 2011-relic body-frame offset) — over a ~100u thigh→head span that reads as ~3.3%.
> The capsule knobs were hand-DIALED on the master at `GetScale`=1.0, so multiplying by 1.033 double-counts.
> User's call: **turn the auto-tune off; the hand-dialed bodies are the source of truth.** `CapScaleOf`
> now gates the `s_effScale` lookup on `MeasuredScaleEnabled()` → falls back to `GetScale` when off.
> The system still SAMPLES (latch is inert) so flipping the knob live still works for testing.
>
> **To resurrect it correctly (the real fix, NOT yet built): BASELINE-CALIBRATE against the master.**
> Measure the reference NPC's own ratio (Lydia's ~1.033) and treat THAT as the 1.0 datum:
> `CapScaleOf = measured(actor) / measured(reference)`. Then the master maps to exactly-as-dialed and
> everyone else scales *relative to her*, so only GENUINE per-NPC build differences (the Astanova/Roisin
> case) move a body — the systematic offset cancels. Store the reference ratio as a knob/const.

The durable answer to "the engine lies about scale." User's exact framing: *"give them the Havok scaled
body they come with, do a distance check, and if we detect a consistent size difference, take a mean of
10 checks, then change their body for a fitting one — better for performance than doing math every frame."*

**Implementation (`CapFix.cpp`), piggybacked on the existing 1 Hz per-actor identity probe:**
- Once per second per actor: measure `bodySpan / nodeSpan` for two segments (thigh→head, thigh→calf) —
  BODY origins (`hkpRigidBody` motionState translation) vs NODE worlds (`NiAVObject::world.translate`).
- `ratio × root world scale` = one sample of the effective build scale. Sanity band 0.7–1.4 rejects
  garbage frames; pose spans must be > 10u.
- At **10 samples, latch the MEAN** as the actor's `effScale` in a `std::unordered_map<FormID,float>`.
- `CapScaleOf(actor)` returns the latched `effScale` if present, else falls back to `GetScale()`. This
  **replaces every record-based scale guess** — refscale, base scale, build-timing bugs, all subsumed.
- The latch forces a full re-dress (`ap.gen = 0`). A ragdoll rebuild resets sampling (old latch stays
  live meanwhile — no naked window).
- **Cost**: ~10 seconds of once-per-second, ~4-lookup sampling per NPC, then NOTHING. No per-frame math.

**Why span-ratio and not GetScale:** GetScale reads the refscale layer only and the engine's per-NPC
build-scale inconsistency is invisible to it. The span ratio measures what was ACTUALLY built. Note:
`CapScaleOf(GetScale)` was CORRECT for the refscale layer all along (proven by Lydia's `setscale 0.8`
test needing it) — the measured system SUPERSEDES it, it didn't reveal GetScale as wrong.

---

## 4. The refinement loop: buffer, head-trim, and the STATE EDGES

Raw measurement is noisy (posture wobble ±2–7%) and state-dependent. Three refinements make it stable:

### 4a. The 0.5u buffer (user request)
`if |measured − current| < 0.01` (≈0.5u at body spans) → keep the current dressing, log "within buffer",
NO re-dress. Stops noise-chasing on NPCs that are already fine (M'rissi, Carmella-class).

### 4b. The head trim (user design)
After the root delta latches, sample the HEAD joint's pivot-vs-node Z (`ObjectHold::PivHeadGapZ`, the
jtrack head-joint math extracted into `PivFix.cpp`) ~1 Hz; the 10-sample MEAN, if |mean| > 0.5u, trims
`rootDelta.z` by that amount ONCE (deadbanded one-shot per capture — self-stabilizing, not a loop). It
doesn't care WHY the head is off, only by how much — so it auto-heals residuals from any source (e.g. an
imperfect heel capture) with zero source-specific logic.

### 4c. The CALIBRATION EDGES — the heart of robustness
**Any latched calibration becomes WRONG the moment the actor's state changes.** Every sampler
(measured-scale, root capture, head trim) is gated on validity and RE-MEASURES on these edges. Each was
a separate user-caught bug:

| Edge | Detector | Why a stale latch is wrong |
|---|---|---|
| **Furniture / sit / sleep / ragdoll** | `ActorRagdollAttached()` = `!IsInRagdollState && GetSitSleepState()==kNormal && !GetOccupiedFurniture` | PLANCK LOOSENS the ragdoll ("green" bodies) in these states — the body is nowhere near the skeleton; any sample is pose-garbage. **NEVER measure a loosened ragdoll.** |
| **Get-up / knockdown** | `GetKnockState() != kNormal` (added 2026-07-13) | `kGetUp` flips `IsInRagdollState` false while the body is still mid-recovery — the stand-up edge would fire and capture get-up garbage. |
| **Swimming** | `IsSwimming()` (added 2026-07-13) | a horizontal swim pose fills a whole 10-sample window with bad spans; water-exit re-measures via the stand-up edge automatically. |
| **Killmove / paired sync** | `IsInKillMove()` (added 2026-07-13) | synced warps hold the body displaced against the partner; no furniture record, so this flag is the only tell. |
| **AI disabled** | `!IsAIEnabled()` (added 2026-07-13) | PLANCK 0.8.0 loosens the ragdoll of a DisableAI'd NPC while she STANDS (paralysis/capture/down-state mods) — engine bookkeeping still reads "attached." |
| **Stand-up** (attached false→true) | edge on the above | a poisoned latch (measured while seated) must self-heal, not persist. Astanova sampled on a chair → 2u-behind "normal"; Carmella trimmed while leaning → 1.5u-low skull. |
| **Ragdoll rebuild** | body-pointer identity change (the existing teleport self-heal probe) | new ragdoll = new build scale possibly. |
| **Live SetScale drift** | root `world.scale` >1% from `scaleAtLatch` at the 1 Hz probe (added 2026-07-13) | a growth/shrink spell or follower-framework SetScale grows the nodes with NO ragdoll rebuild — the closed window would never notice. |
| **Heel offset change** | `NPC` node local Z (source of the NiOverride lift) delta > 0.25u | shoe swaps, LATE heel arming, stuck-heel recovery. |
| **`hf` toggle** | `GetHeelDriveBias()` delta > 0.25u | a capture taken with the heel drive-bias ON is stale when it's toggled OFF, and vice versa. |

On any edge: invalidate `rootDeltaValid` + reset the trim + reset the scale sampler → clean 10-sample
re-measure. **This is why the system is bulletproof: no transient state can become an NPC's permanent
false normal.**

**Statistical hardening (2026-07-13, the conflict-forecast patch set).** Flags catch flaggable states;
STATISTICS catch what flags can't see (a scene warp / stagger / one bad frame that PLANCK's pose-error
loosening produces without any furniture record):
- **MEDIAN-of-10, not mean**, in both the effScale window (`ratioSamples[10]`) and the head-trim window
  (`headTrimSamples[10]`) — one warped frame is now mathematically irrelevant to the latch.
- **Debounce**: the effScale window opens only after 2 consecutive attached probe ticks; the root
  capture waits ~1 s of attached fires (`attachedFrames>=90`). The first attached frame after a get-up
  or warp often still carries displacement the flags don't see.
- **Save/load latch clear**: the FormID-keyed maps (`s_effScale`/`s_applied`/`s_gate`) are cleared on
  BOTH kPreLoadGame AND kNewGame — a recycled FormID must not inherit a previous save's measured scale.

---

## 5. Heels — the interactions (three separate heel facts)

1. **Hidden heels are everywhere.** Roisin/Astanova wore 1.5u NiOverride lifts on "normal vanilla
   boots" — the user couldn't see them; the detector logged them. Never assume an NPC is barefoot.
2. **Heel-aware root capture**: the conform hook sees the PRE-bias pose while the live nodes carry the
   heel lift → raw root delta = bind + heelZ, and the heel drive-bias then adds heelZ AGAIN every frame
   = double-count. Fix: `d[2] -= GetHeelDriveBias()` at capture so each system counts the lift once.
   (This was the Roisin/Astanova "easy 3u" = 2×1.5.)
3. **Stuck heels**: the NiOverride lift can silently fail to apply (heelZ reads 0, zero `HEELFIX`
   detections). The heel fix correctly SKIPS heelZ=0 actors, so `hf` toggling is a genuine no-op for
   them. In-game recovery: re-equip the shoes (re-arms NiOverride) → `HEELFIX heelZ=8.0` returns →
   the heel-offset edge fires → clean re-measure. **The furniture detach STAYS** — a seated pose
   co-occupies the furniture volume by design; accurate bodies don't change that, and live collision
   there would eject the NPC from the seat.

---

## 6. Code map (for the next agent who has to modify this)

- **`CapFix.cpp`**
  - `CapScaleOf(actor)` — returns latched `s_effScale[id]` else `GetScale()`. THE scale term; used at
    every capsule write site (list-child + main-slot) × endpoints AND radius.
  - `GetNodeBodyPos` / `GetNodePos` — the span samplers (body origin vs node world, game units).
  - `Applied` struct — per-actor probe state: `ratioSum/ratioN/effScale`, `wasAttached`.
  - the 1 Hz probe branch — furniture gate + stand-up reset + scale sampling + buffer + latch.
  - `s_effScale` — namespace-scope FormID→scale map read by `CapScaleOf`.
- **`PPBHook.cpp`** (the pose-conform, runs in the ragdoll drive hook chain)
  - `ConfCache` — per-actor: `rootDelta/rootDeltaValid`, `headTrim*`, `wasAttached`, `lastHeelZ`,
    `lastHeelBias`.
  - `PoseConformPrepare` — the edges (furniture/stand-up + heel offset + hf toggle), root-mode parking,
    head-trim sampling.
  - `ApplyPoseConform` — the inner-hook write: per-bone local conform + the root (mode 0/1/2, heel-aware
    capture `d[2] -= GetHeelDriveBias()`).
- **`PivFix.cpp`**
  - `ActorRagdollAttached(actor)` — the shared furniture/ragdoll gate (exported, `ObjectHold` ns).
  - `PivHeadGapZ(actor, &z)` — head joint pivot-vs-node Z (extracted from the jtrack math).
- **Tuning knobs**: `poseConform` (per-bone — THE working system, keep on), `poseConformRoot`
  (root — **RETIRED 2026-07-13, default 0, do not re-enable**), `poseConformDump` (edge-triggered
  census), `jtrackNow` (edge-triggered joint audit), `measuredScaleEnable` (**OFF by default** — the
  capsule-size tape measure; see §3). The shipped calibration = baked NIF pivots + `poseConform`
  (per-bone) + clav-follow + heel bias. Both auto-corrections (root lift, measured scale) are OFF.

---

## 7. Instruments (how to diagnose scale/offset bugs) — USE THESE, don't theorize

- **`jtrackNow` 0→1 edge** → `JTRACK` per-actor per-joint dump: `gapChild` (pivot vs node), `boneW`,
  `bodyW`. Fire with a real edge (write 0, wait ~2.5s for the poller, write 1). The **bodyW vs boneW
  SPAN RATIO** is what exposes build-scale (§1B) — compute inter-node distances, compare to inter-body.
- **`poseConformDump` 0→1** → `PCONF DUMP` (parent-local drive-vs-XP32 rows) + `PCONF FKW` (world rows).
  Header carries `scale(track)` (true composite) vs `scale(innerWFM)` (usually 1.0).
- **Log signatures to grep**: `MEASURED effScale=` (latched, with before/after or "within buffer"),
  `stood up -> re-measuring`, `heel state changed`, `PCONF HEADTRIM`, `HEELFIX … heelZ=`.
- **The eye vs the instrument disagree often**: jtrack reads LIVE physics; the Collision Visualizer
  caches tessellation per shape identity and shows STALE geometry after a cell load (toggle `trd`
  off/on to force re-tessellation before judging). And the eye reads the CAPSULE layer while jtrack
  reads the JOINT layer — a "too high" face with clean joints = a capsule-dial bias (§1C), not a joint bug.

---

## 8. Validation protocol (the disciplined loop, learned the hard way)

1. **SHIP behind a live knob** and **SOAK** (minutes, not seconds) watching for drift BEFORE judging fit.
   The v1 sky-launch was seen by the user because it wasn't soaked first. Non-negotiable for conform writes.
2. Stand with a group of NPCs ~15s → read the `MEASURED`/`HEADTRIM` log lines while eyeballing faces/feet.
3. Test EACH state: someone sitting (must NOT be measured), then stood up (must re-measure), a heeled
   NPC, an `hf` toggle, a `setscale`/`disable`/`enable`.
4. Cross-check eye vs jtrack when they disagree (visualizer cache; capsule-vs-joint layer).

---

## 9. Transferable principles (the distilled wisdom — good for ANY physics/skeleton project)

1. **Measure, don't trust the API.** Engine scale/pose values are proxies; the live geometry is truth.
2. **A corrective loop must never re-measure through its own write.** Capture from bind data or a
   pre-write one-shot.
3. **Every latched calibration needs edge-invalidation.** Enumerate the states that change the truth
   (pose-detach, rebuild, scale, external offsets like heels) and re-measure on each transition.
4. **Gate measurement on validity.** A decoupled/ragdolled/seated actor yields garbage — refuse it.
5. **Buffer + deadband + one-shot** beat continuous correction: cheaper, quieter, no oscillation.
6. **Ship corrections behind a live kill-switch and soak them.** The knob turns a disaster into a datapoint.
7. **Separate the layers.** One symptom ("body offset") was three independent bugs (root/scale/dial).
   Instrument each layer independently; don't fix in the dark.
8. **The eye and the instrument look at different layers.** Reconcile them before acting.
