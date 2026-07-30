# 12 — Deep Reference: Original Toolchain · Impact Sound · Dismemberment/Ragdoll Sync

Combined single-topic deep references (was 06_Original_Toolchain + 07_Impact_Research +
07_Dismemberment). Verified addresses / NIF byte layouts / mechanism research that the main system
docs cite but don't reproduce. Not part of the main build flow — consult when working the specific
subsystem.

---

# 13 — Ragdoll Collision Remake & Live Capsule Calibration (2026-07-02/03)

## Purpose
The 2011 vanilla Havok ragdoll no longer matches modern bodies (XP32's 700+ bones, CBBE 3BA meshes) —
capsules sit wrong, spill out of the silhouette, and the hand collision didn't even cover the palm. This
report records the COMPLETE toolchain built to fix that: live in-game capsule editing (voice-driven,
~1s latency), permanent NIF baking, the heel fix, and the calibration methodology. Everything here is
TRANSFERABLE — the same techniques work for any SkyrimVR ragdoll surgery in any future project.

## The three-layer model (read this first)
On a PLANCK-driven NPC there are THREE parallel skeletons that only mostly agree:
1. **Visual skeleton** (NiNodes) — what the player sees; includes the NiOverride heel offset on `"NPC"`.
2. **Pose/animation layer** (behavior-graph TRACK_POSE, hkbCharacter::poseLocal) — what animations and
   our IK write; unraised (no heel offset).
3. **Havok ragdoll bodies** (bhkRigidBody per major bone) — what PLANCK/HIGGS/melee actually collide
   with; driven per-frame toward `worldFromModel × pose`; also unraised.
Every hard bug this session was a disagreement between layers. Instruments: GA-TRACK (palm/bone/ragdoll
per 0.4s), GrabDiag (visVsCol), Collision Visualizer VR (draws layer 3 — with caveats below).

## THE HEEL FIX (shipped, verified, auto-on)
Heels raise layer 1 by heelZ (~8u) but not 2 or 3 → all touch/hit zones sit heelZ below the visible body.
- **What does NOT work** (all in-VR proven no-ops): mutating TRACK_WORLD_FROM_MODEL at the pre-drive
  hook (PLANCK's author left the same abandoned experiment commented out, planck main.cpp:4470);
  mutating `hkbCharacter::worldFromModel` (+0x88).
- **What works**: (a) chain-hook the INNER `hkaRagdollRigidBodyController::driveToPose` call (VR
  **0xA26C05**) and bias the `worldFromModel` PARAMETER (+heelZ, havok m) — the ragdoll rises exactly
  heelZ; (b) the raised ragdoll then drags the RENDERED body up through the postPhysics feedback →
  chain `hkbRagdollDriver::postPhysics` (VR **0xB268DC**), run AFTER downstream, subtract heelZ from
  TRACK_POSE **bone 0** (GAME units — the pose buffer is skyrim-u; the wfm param is havok metres).
- Per-NPC amount read live off the `"NPC"` node local Z (stuck/failed heels read 0 → auto-skip; late
  offsets picked up same-frame; sitting NPCs auto-suspend). Gate off while `IsInRagdollState()`.
- Console `hf` toggles; auto-on via tuning default at DataLoaded. Thread-local per-driver bias, zeroed
  at the top of every ApplyToPoseTrack (leak-proof).

## LIVE CAPSULE EDITING (the calibration loop)
Runtime `hkpCapsuleShape` float writes (vertexA/vertexB/radius) **do take effect physically** — verified
by readback AND felt collision. The pieces that make it a usable loop:
- **The visualizer draws stale geometry**: Collision Visualizer VR (collviz_vr) tessellates each shape
  ONCE and then only moves it with the body. `resetOnToggle=1` → **toggle the visualizer off/on to
  rebuild** — that's the "refresh" button for live shape edits.
- **The degenerate-capsule trap**: point1 == point2 EXACTLY → zero axis → NaN tessellation → the shape
  is INVISIBLE to the visualizer while still colliding. (This hid a botched June hand patch for a week.)
  Never write A==B; the tool auto-nudges B.z +0.15u.
- **Delivery**: a ~1 Hz file poll in the physics hook re-reads the tuning file EVEN WHILE IDLE and
  auto-applies capsule-knob changes (generation counter → per-actor re-apply). Claude edits the file,
  the change is live in ≤1s, the user toggles the visualizer. No console typing. (SkyrimVR has no cgf;
  hijacked console commands with ARGUMENTS died in the console compiler — zero-arg commands work.)
- **4 slots**: hand / forearm / upper arm / head (`capHand*/capFore*/capUpper*/capHead*`), right side;
  the left is mirrored at NIF-bake time. Non-capsule shapes are detected + logged, not touched.

## Calibration methodology (the "thin rod" protocol — user-invented, keep it)
1. Set radius small (~1.5u) → the capsule becomes a visible line showing exactly where A→B sit.
2. Nudge ONE axis by a big amount; the user names the direction it moved → axis calibrated.
   Hand-bone frame (right side): **+X thumb · +Y knuckles/back · +Z fingertips**.
3. From then on, instructions are anatomical ("wrist point 1u toward the knuckle") and translate 1:1.
4. Restore the real radius last; sign off; bake.
Dialed hand (BAKED both sides, mirrored X): **A=[±2.0, 0.7, 1.0] B=[±0.17, −0.3, 6.3] r=2.0**.

## NIF byte-patch reference (skeleton_female.nif — the live winner is AIHands' copy)
- bhkCapsuleShape layout from the material signature (uint32 591247106 = skin): `material@0,
  bhkRadius(f)@+4, [8 unused]@+8, point1(3f)@+16, radius1@+28, point2(3f)@+32, radius2@+44`.
  Patch ALL THREE radius fields. Units: havok = game × 0.0142875.
- Offsets in the current file: R hand @ **0x1067F**, L hand @ **0x11BAE** (re-verify by dump after any
  structural change; float patches don't move offsets).
- STOCK hands were ~degenerate balls r=4.80 at z≈3.83 (knuckles); the June patch had shrunk them to 2.5
  and moved them to the WRIST (both wrong) — the source of the receive "wrist ambush".
- Tolerance-scan UNALIGNED for float signatures; never trust 4-byte alignment (see report 28 conform).

## Constraint pivots (the blue balls) — NEXT
The visualizer's blue spheres = ragdoll constraint pivots (`ragdollConstraintColor=0000ff`). User
confirmed wrist, elbow AND shoulder pivots sit off the anatomical joints. Facts: pivots connect two
BODIES (not bones); zero effect on animations/.hkx/XP32 bones; they define where limbs articulate in
full ragdoll AND fight the motors on driven NPCs when misplaced (pose rotates about the anatomy, the
constraint about its pivot → chronic jitter/strain). Editing = the same float class, but the pivot is
stored TWICE (once per body frame) — both must land on the same bind-pose world point or the joint
spawns pre-stressed. Position-only edits; leave twist/plane axes alone.

## Roadmap
1. Forearm + upper arm + head capsules via the live loop (tool ready; head may be a non-capsule —
   the first apply-generation's BEFORE log line will say).
2. Wrist/elbow/shoulder pivot re-seating (needs the dual-frame editor — next build).
3. Spine/legs capsules (same loop, new slots — user: spine sticks out both ways, legs too thick).
4. Flat-palm precision: bhkConvexVerticesShape hull or bhkListShape multi-capsule (NIF structural
   surgery; pynifly ships quickhull/bhk_autopack). NOT live-swappable (runtime shape allocation = the
   proven CTD class).
5. Player-hand: the VR "rectangle" is HIGGS's runtime-created hand collision (not NIF). Crude size via
   higgs_vr.ini today; a HIGGS fork with per-finger boxes following curl is feasible (open source).

## Code map
- `ArmIK.cpp`: ApplyHeelFix / ApplyHeelPostFix / GetHeelDriveBias (thread-local); CapFixPollFile+
  CapFixApply call site (after the player check, before the patched-actor gate).
- `Hooks.cpp`: InnerDriveChainHook (0xA26C05) + PostPhysicsDriverChainHook (0xB268DC) — both byte-check
  E8 before write_call<5>, both chain PLANCK.
- `PalmCollider.cpp`: heelFix/capsule knobs + parser (⚠ MSVC C1061: split the else-if chain every ~40
  keys); CapFixPollFile (1 Hz, 32-float snapshot → gen bump); CapFixSlot accessor.
- `GrabDiag.cpp`: GetNodeCapsule + CapFixApply (4-slot sweep, BEFORE/APPLIED logs, degenerate guard).
- `main.cpp`: console commands (TestSeenData→HeelFix, DumpNiUpdates→CapFix) via
  SCRIPT_FUNCTION::LocateConsoleCommand; RegisterConsoleCommands + InitHeelFixDefault at kDataLoaded.
- Scratch scripts (session scratchpad): dump_hand_caps.py / patch_palm_capsules.py / bake_hand_final.py.

## Session addendum (2026-07-03, end-of-session state)
- **Console-args commands DIED in the console compiler**: a hijacked command slot with 7 optional float
  params never reached our exec (zero log entries; entry-logging added since). ZERO-ARG hijacked commands
  (`hf`, bare `capfix`) work fine. The 1 Hz file-poll loop replaced args entirely and is the proven path.
- **The 4-slot editor is LIVE**: `capHand*/capFore*/capUpper*/capHead*` — CapFixSlot reads each slot's 8
  contiguous GrabTune floats (Enable,AX..R — keep that field order!). Stock starting values are staged in
  the tuning file (forearm z 0.75→13.38 r3.7; upper z 3.13→14.27 r6.07; head unknown until the first
  BEFORE log line — may not be a capsule).
- **MSVC C1061**: the tuning parser's else-if chain overflows the block-nesting limit around ~100 keys —
  split into independent `if` chains every ~40 keys (keys are unique; semantically identical).
- **Voice-driven loop, as-proven**: user speaks a move → Claude edits the file → ≤1s auto-apply → user
  toggles the visualizer. The whole hand was dialed this way in ~10 exchanges.
- **NEXT for the remake, per Report 14** (READ IT — the impact research: consumer map, 3 hard rules,
  pitfalls incl. PLANCK's touch-time self-collision): forearm+upper dial → one-bone bhkListShape proof
  test → pivot editor build (dual-frame) → inertia/COM bake pass for drastic resizes → beast skeletons.

## Addendum 2026-07-03 — the bhkListShape proof test is BUILT & DEPLOYED (awaiting VR verdict)
- **RIGHT hand only** (left stays the dialed single capsule = in-game A/B control): the R hand rigid body now
  holds a bhkListShape (skin material) with **3 slim capsule children** fanning wrist→knuckles — center =
  the dialed line r1.3, thumb A=[2.9,0.7,1.0] B=[1.97,−0.3,6.05] r1.25, pinky A=[1.1,0.7,1.0]
  B=[−1.63,−0.3,6.05] r1.25 (game u; ~6u wide × 2.6u thick flat palm vs the old 4u round slab).
- **Authored with pynifly, NOT binary append** — discovery: only SET-on-existing capsule props is NYI; the
  CREATION path works (null round-trip BYTE-IDENTICAL 97,440B; `body.add_shape(listProps)` re-points shapeID;
  `list.add_shape(capsuleProps)` children carry full props at creation; orphaned old capsule dropped by the
  writer; block IDs renumber on save — verify by reload, never cache pre-save IDs). Script:
  `hand_listshape_surgery.py` (session 0146d49c scratchpad). Verified: 750/750 nodes, all 5 arm-chain
  constraints intact, L-hand floats untouched, skin-sig accounting exact (+4 new −1 orphan).
- Deployed over the live winner with backup `skeleton_female.nif.bak_pre_3cap`. VR test protocol: load OK →
  visualizer shows 3 rods on R (1 on L) → push/HIGGS-grab the hand → give sequence → kill/ragdoll settle.
- If verdict = NO (CTD/no-collision/jitter): rename the .bak back; fall back to Route A convex hull
  (vanilla-precedented, pynifly `add_shape`+`setCollConvexVerts`).
- Note for the tuner: the CapFix hand slot now meets a LIST on the R hand — GetNodeCapsule skips non-capsule
  shapes by design (capHandEnable stays 0; live child editing = a future tuner extension, mind the cached
  union AABB caveat in Report 14 §6).

## See also
- **14 — Havok Body Remake Impact Research**: the pre-surgery briefing (consumer risk map, scaling/races,
  SMP/CBPC verdicts, runtime-variation menu, shape-upgrade costs, HIGGS hand plan, pitfalls, sequence).
- **05 — Havok Collision**: the body/filter/CBPC fundamentals + the (now-corrected) June hand-patch record.
- **KNOWLEDGEBASE.md**: heel-fix mechanism, IsDead()-restrained trap, VR console commands, degenerate
  capsules — the portable one-liner versions of everything here.
# 14 — Havok Ragdoll Body Remake: Impact Research (Pre-Surgery Briefing)

**Date:** 2026-07-03
**Status:** Research complete, surgery not started. This is the go/no-go briefing for remaking the NPC ragdoll collision bodies (layer 3) to anatomically fit modern bodies.
**Prerequisites:** Report 13 (Ragdoll Collision Remake & Live Calibration) is the hands-on companion — the three-layer model, the live capsule editor, the NIF byte-patch reference. Report 05 (collision bodies/filters), Report 28 in the chronological archive (skeleton binary surgery technique).

**Evidence tags used throughout:**
- **CONFIRMED** — read directly from primary source this research pass: local PLANCK/HIGGS source trees, NIF binary dumps, DLL string scans, houseCARL reads of the live load order, or in-game measurement. The strongest grounding we have.
- **REPORTED** — single-source (a Nexus page, an author comment, a GitHub repo) not independently cross-checked.
- **SPECULATION** — inference or training knowledge, explicitly untested. Every one of these has a test procedure listed.

*Honesty note: no separate adversarial verifier pass ran over this digest (the verifier queue came back empty). "CONFIRMED" here means primary-source-read by the researching session, not double-checked by a second agent. The local-source claims (PLANCK/HIGGS code, NIF dumps) are the most trustworthy; treat web-sourced claims as one notch softer than their tag.*

---

## 1. Executive Verdict

**Is the remake safe? YES, within three hard rules.**

1. **Resizing and repositioning capsules is proven-safe.** Mass, inertia tensor, and center of mass are *stored numbers* in each rigid body's NIF block — the engine never re-derives them from the shape at runtime, and PLANCK contains zero inertia-recompute code. Changing a capsule's radius or endpoints desyncs nothing; the failure mode is physical implausibility (a corpse that rocks oddly), not a crash. **CONFIRMED** [pynifly nifdefs.py bhkRigidBody buffer fields 558-560; grep over tools/_research/planck; Report 13].
2. **Shape-TYPE changes (capsule → list, capsule → hull) must be baked into the NIF, never swapped at runtime.** Our two past CTDs were both use-after-free from allocating our own rigid bodies; runtime shape-swap is the same family. Havok 2010.2 has a documented-safe `setShape`, but no VR address exists for it and both substrate authors (PLANCK, HIGGS) conspicuously avoid it on rigid bodies — HIGGS rebuilds the whole body instead. **CONFIRMED** for the CTD history and the avoidance pattern [Report 05 lines 219-255; higgs/src/hand.cpp:559-660; planck/src/main.cpp:4021-4118].
3. **Never add BODIES, spend sub-shapes freely.** Broadphase, motors, constraints, and PLANCK bookkeeping all scale with body count; sub-shapes inside an existing body cost tens of microseconds across a whole 90Hz scene. **CONFIRMED** for the mechanism, modeled for the numbers (§6).

**What's genuinely novel here.** In 14 years of Skyrim modding, *nobody has ever remade the ragdoll shape geometry.* The entire "ragdoll mod" tradition (Realistic Ragdolls and Force, Ragdoll Friction and Edits, XP32 True Ragdoll Physics 2025) remade constraints, weights, friction, restitution, inertia *types* — everything except the shapes. **REPORTED** [Nexus 1439, 107266, 161116]. The only anatomical capsule edit ever shipped is a single pelvis-capsule float tweak, VR-motivated, tuned to one body preset, by an author who then quit VR (no me molestes, tooandrew 2022). **REPORTED** [Nexus 72482]. The two most capable collision engineers in the scene both looked straight at the problem and routed around it: Ersh called the shapes "wildly inaccurate" and shipped Precision (reroute melee onto the existing bodies, don't fix the bodies); adamhynek shipped PLANCK with "bad collision... of the enemy's ragdoll" as an acknowledged limitation and a source tree full of runtime workarounds instead of asset fixes. **REPORTED** for the quotes, **CONFIRMED** for the workaround pattern in his source [Nexus 76783, 66025; planck/src/main.cpp:3013, 4120-4147, 5044-5053].

Within the remake itself, one construct is genuinely unprecedented: a **multi-capsule list shape on a ragdoll bone** appears nowhere in 19,057 vanilla NIFs nor in all 127 loose skeleton NIFs across the entire 2,092-mod load order. **CONFIRMED** [scratchpad scan over vanilla_ext + mods]. It is *plausible* (the engine runs list shapes on actor bumper phantoms and even on the player's own character proxy every frame) but it needs a one-bone in-game proof test before we commit. Convex hulls on ragdoll bones, by contrast, have direct vanilla precedent (draugr head/jaw, centurion chassis lid). **CONFIRMED** [scan_detail.py over vanilla_ext].

**Why has nobody done this — the one-line answer:** *On flatscreen, a living actor's ragdoll bodies are gameplay-inert — the character-controller capsule handles movement, projectiles, and melee — so for 14 years the shapes only mattered to corpses; VR + PLANCK flipped them into the live touch surface only ~4 years ago, and the few authors capable of the surgery kept working around the assets because float edits were obscure, structural edits sat behind an unobtainable Havok toolchain, and one static NIF can't match per-NPC morphed bodies anyway.* **REPORTED** for the incentive history [Ersh on Nexus 76783], **CONFIRMED** for the workaround pattern and toolchain wall [planck source + README].

We are positioned unusually well: we already have live per-actor capsule float editing from the physics hook (CapFixApply), a byte-patch NIF pipeline proven on skeleton surgery (Report 28 class), the collision visualizer workflow, and — decisive for the "one-size NIF" objection — a working runtime write path that could later follow OBody morphs. The remake is not blocked; it is a sequencing problem (§9).

---

## 2. The Consumer Map

Every system that touches NPC ragdoll bodies, ranked by breakage risk if capsule geometry changes drastically (assuming mass/inertia/COM initially untouched). All PLANCK/HIGGS claims read from local source. **CONFIRMED** unless noted.

### HIGH RISK — these will bite immediately if we get shapes wrong

| # | Consumer | Mechanism | Why it breaks |
|---|----------|-----------|---------------|
| 1 | **Ragdoll self-collision during touch/grab** | PLANCK enables non-adjacent-bone self-collision for NPC-race actors *exactly while the player touches or holds them* (`doBipedSelfCollision`/`ForNPCs` default true; main.cpp 4356-4376, filter 2445-2456) | Oversized/overlapping capsules fight each other precisely during AIHands interactions — arms shoved off the torso, chronic buzz. **This is the #1 calibration constraint: new capsules must not interpenetrate non-adjacent neighbors in normal pose space.** |
| 2 | **Player body-blocking distance** | PLANCK routes the player's character controller to collide with NPC ragdoll bodies and ignore NPC charcontrollers (filter 2491-2556, `enablePlayerBipedCollision` default true) | Capsule width literally sets how close you can stand to an NPC. Felt instantly, every play session. Biped-vs-biped collision also means oversized capsules make NPCs bump each other at a distance. |
| 3 | **Player melee hit registration** | PLANCK melee is weapon/hand rigid body vs ragdoll capsule contact; the capsule IS the hitbox, hit position = the contact point on the capsule surface, body-part attribution = which capsule's node was hit (nodeName exported in the extended PlanckHitEvent for downstream/locational mods) (contactPointCallback 1905-2237, DoHit 1817-1848) | Shape changes move where hits land and which part gets credit; missing/undersized capsules cause whiffs through visible flesh. |

### MEDIUM RISK — tolerant, but degradable

| # | Consumer | Mechanism | Risk |
|---|----------|-----------|------|
| 4 | **Full-ragdoll settle & getup** | Engine `Actor_IsRagdollMovingSlowEnoughToGetUp` is velocity-based; PLANCK hooks it and has a bugged-getup re-knockdown fallback (main.cpp 4760) | Capsules that rock or buzz never settle → delayed getup; a chronically unstable rig could loop the re-knockdown path. |
| 5 | **Projectiles on active actors** | PLANCK adds the Projectile bit to the Biped layer (main.cpp 4000-4002, default true) → arrows hit ragdoll capsules directly on PLANCK-active NPCs; the charcontroller still catches on inactive ones | Impact position/part shifts; not a whiff class (charcontroller fallback exists). |
| 6 | **HIGGS grab & loot** | Selection = sphere linear cast vs capsules (hand.cpp 320-422); loot slot resolved from which capsule's node was hit + skinning (2513-2596); but the actual palm grab point comes from VISUAL mesh triangles (1393-1440) | Capsule changes shift which limb/slot gets selected and reach — but not the final grab anchor. Tolerant. |
| 7 | **Touch/shove/aggression range** | PLANCK contact sets (collidedRefs, 2770-2776, 3651-3683) built from hand/weapon vs capsule contacts | Capsule size = the NPC's social "touch skin". Oversized → touched-aggression from a distance; undersized → shove breaks. |

### LOW / NO RISK

| Consumer | Why safe |
|----------|----------|
| **PLANCK drive motors & stress** | Gains are GLOBAL config constants applied uniformly per bone (hierarchyGain 0.6 / velocityGain 0.6 / positionGain 0.05, main.cpp 4817-4855); the keyframe utility computes impulses from pose error + STORED mass/inertia. Nothing reads shape. Stress is extracted (4963-4974) but has no functional consumer — `GetStress` is uncalled; footYank uses displacement, not stress. **CONFIRMED** |
| **PLANCK shape reads, total census** | The only shape reads in the entire plugin: impact-material lookup via shapeKey (1470-1472) and the PLAYER charcontroller resize (4032-4122). **CONFIRMED** |
| **Vanilla NPC melee (NPC-vs-player, NPC-vs-NPC)** | Engine reach/cone calculation, not ragdoll-shape-based; only PLAYER melee is physical under PLANCK. **CONFIRMED** for PLANCK's scope, **SPECULATION** (training knowledge) for the vanilla internals |
| **CBPC / HDT-SMP / VRTouchEvents** | Separate physics worlds reading bone transforms, never Havok ragdoll bodies (§4). **CONFIRMED** |
| **Navigation, NPC-vs-NPC blocking** | Their own charcontroller capsules, untouched. **CONFIRMED** for what PLANCK touches |
| **Killmoves / decapitation** | Paired animations + mesh dismember partitions, not capsule geometry. **SPECULATION** (training knowledge, no local source) |
| **Corpse activation/loot pick** | Points at DeadBip ragdoll bodies — a real consumer, but tolerant of anatomically-reasonable resizing. **SPECULATION** for internals |

### The critical question, answered

**Does resizing a capsule desync anything derived? NO.** Inertia matrix, center of mass, and mass live in the bhkRigidBody NIF block (pynifly nifdefs.py fields 558-560), load as-is, and Havok 2010 keeps inertia in the motion object set at construction — never re-derived from the shape. **CONFIRMED.** The consequences of size-without-inertia mismatch are bounded plausibility errors, visible mainly in full ragdoll: (a) stored COM ending up outside the new capsule → rocking/tumbling; (b) surface much farther from COM than stored inertia implies → exaggerated spin from contacts; (c) foot capsules penetrating ground while driven → leg buzz. PLANCK's per-bone velocity clamps (500 linear, main.cpp 2972-2973) bound explosions. **Mitigation available in-format:** bake matched inertia/COM floats into the same NIF block — the same byte-patch class as the radius fix. **CONFIRMED** for the mechanism, **SPECULATION** for extreme-size behavior.

---

## 3. Scaling & Races

### How capsules scale (the mechanism)

Actor scale is baked into a **per-actor scaled shape clone at 3D-load** — not a transform scale, not a wrapper shape. The engine's NiCloningProcess carries a scale vector (offset +0x68, higgs/include/RE/misc.h:51-54), and cloning a shape through it multiplies the shape's floats. HIGGS itself proves the engine builds collision shapes through this path (its comment "Undo the scaling of the original shape done when creating it", hand.cpp:743-752). Our own measured identity — **live radius / 4.80 NIF radius = GetScale, per actor** — shows the radius float itself is scaled, which is impossible via a rigid transform (no scale slot) or a wrapper (the NIF chain is bhkRigidBody → bhkCapsuleShape directly, no wrapper). **CONFIRMED** [higgs source; pynifly dump; in-game measurement].

Consequences:
- **Every actor owns its own shape floats** → per-actor capsule writes don't leak across NPCs. Residual 1-minute check: log CapFix BEFORE lines on two same-skeleton NPCs after patching one. **CONFIRMED** [Report 05 "Live body ≠ NIF body literally"].
- **Race Height multiplies every capsule uniformly** at clone time. Live values: Nord 1.03, Orc 1.045, HighElf 1.08, Argonian 1.01M/1.00F, Khajiit 1.00M/0.95F. **CONFIRMED** [houseCARL live reads].
- **Mid-game SetScale probably does NOT resize already-cloned shapes until 3D reload** — the bake happens at model instantiation; PLANCK's ragdoll add/remove reuses existing bodies. **SPECULATION.** Test: `setscale` a visible NPC, toggle Collision Visualizer (rebuilds tessellation), compare; then force a 3D reload and compare again.

### What does NOT scale capsules

- **Weight 0-100** is a `_0`/`_1` mesh morph. No weight term exists in the measured scaling identity. **CONFIRMED** [in-game measurement].
- **RaceMenu height/limb sliders** are NiOverride node-transform edits — visual layer 1 only; the heel-fix saga already proved NiOverride transforms never reach layers 2/3. **CONFIRMED** [Report 13].
- **BodySlide/OBody morphs** — same story. This is the structural reason one static NIF can never truly match modern bodies, and why §5's runtime layer exists.

### Who shares skeletons (live load order, houseCARL winners)

| Race | Skeleton (M / F) | Height (M/F) |
|------|------------------|--------------|
| Nord, Orc, HighElf (and all non-beast playables) | `skeleton.nif` / `skeleton_female.nif` | 1.03 / 1.045 / 1.08 |
| Argonian | `skeletonBeast.nif` / `skeletonBeast_female.nif` | 1.01 / 1.00 |
| Khajiit | same beast pair | 1.00 / 0.95 |

All five share BodyPartData `00001D:Skyrim.esm`. **CONFIRMED** [houseCARL].

**Live VFS winners for the four NIF variants** (modlist.txt, top wins — AIHands line 45/46 > VRTouchEvents V2.0 line 48/49 > SOFTBODY ~606 > XP32 ~813). **CONFIRMED** [modlist + pynifly dumps]:

| File | Winner | State |
|------|--------|-------|
| skeleton_female.nif | **AIHands** | Our baked hands (r=2.0gu, len 5.7gu) + COM 8.05gu reshape — the only remade file so far |
| skeleton.nif (male) | VRTouchEvents V2.0 | ALL STOCK — COM r=0.1306, degenerate r=4.8gu knuckle-ball hands |
| skeletonBeast.nif (male) | VRTouchEvents V2.0 | Full stock 23-body set incl. 5 tail bodies |
| skeletonBeast_female.nif | VRTouchEvents V2.0 | **BUG: only 18 bodies / 9 constraints — the 5 tail bodies + 5 constraints were dropped** (likely rebuilt from the female humanoid superset). Female Argonian/Khajiit tails currently have ZERO Havok collision in-game. |

### Beast races: tails, snouts, horns

- **XP32 stock beast skeletons = the identical humanoid 18-body set (same capsule floats, COM mass 40F/50M) PLUS TailBone01-05 capsules** (r 1.57→1.05gu, segments ~12.5-13.6gu, mass 1.0, bhkBlendCollisionObject) chained by 5 extra bhkRagdollConstraints (23 bodies/14 constraints vs 18/9). The tail IS a real constrained collision chain at NIF level. **CONFIRMED** [pynifly dumps of XP32 files].
- **BUT tail bodies are not in the runtime ragdoll instance**: the ragdoll skeleton comes from `skeleton.hkx`/`skeleton_female.hkx` — shared by beast and human races, no beast-specific hkx exists — and contains exactly 18 `Ragdoll_*` bones, no tails. PLANCK's ModifyConstraints/driveToPose iterate only `ragdoll->m_rigidBodies`. So tail bodies are animation-following blend-collision bodies: real collidable geometry for touch/hit coverage, but never PD-driven and excluded from death ragdoll. **CONFIRMED** for the hkx contents and PLANCK's iteration [XP32 skeleton XMLs; planck main.cpp 2961-3005]; one in-game GrabDiag CollectBodies probe on a male beast NPC would confirm world membership.
- **Horns: no bones, no bodies, anywhere** — zero horn-named nodes in any of the five skeletons (head-mesh geometry only). Coverage option: a longer head capsule. **CONFIRMED** [pynifly node scan].
- **Snouts:** the beast head is a bhkCapsuleShape (block 548) — a longer snout capsule is a pure NIF float edit. **CONFIRMED**.

### The per-race path

- **Remaking skeleton_female.nif does NOTHING for beast races** — separate NIFs. But the beast pass is mechanical, not creative: bones and stock capsule values are byte-identical to the humanoid rig, so anatomical patch numbers transfer 1:1. Only the file offsets differ and **must be re-derived per file with the unaligned signature scan — never reuse absolute offsets.** **CONFIRMED** [NIF dumps; Report 28 technique].
- **Granularity is per-skeleton-path × gender, not per-race.** One humanoid pass covers all 8 non-beast playable races (each just applies its uniform Height multiplier); one beast pass covers Argonian + Khajiit together.
- **True per-race shapes come "for free" only by repointing a RACE record's SkeletalModel at its own NIF copy** — confirmed viable via the live winners (ArgonianRace winner Aetherius.esp, KhajiitRace winner fluffy khajiit.esp, both pointing at the vanilla beast paths). Splitting Argonian from Khajiit = tiny ESP forwarding those winners with a new path. Zero SKSE code. **CONFIRMED** [houseCARL].
- **Base any beast edit on VRTouchEvents V2.0's live-winner copy** (it carries the CME/softbody node additions), and ship inside AIHands, which wins the file conflict. **CONFIRMED** [modlist].

### Open decisions

1. **Restore the beast-female tail bodies** (graft the 5 bodies + 5 constraints from XP32 into the live copy — Report-28-class surgery) or leave tail motion to HDT-SMP? Recommend: restore — it's a regression, and tail touch coverage is free.
2. **Humanoid male and both beast skeletons still ship the stock degenerate ball hands** — the June hand bake benefits humanoid females only.

### KB discrepancy to fix

The KNOWLEDGEBASE "THREE collision systems" entry (2026-06-20) claims a live bonescan found only 9 ragdoll bodies and that arms/hands/head/feet have none. All five skeleton NIFs define 18 bodies including arms/hands/head/feet, and Report 13's live editing physically felt hand collision. **The 18-body set is authoritative; correct the KB entry.** **CONFIRMED** [NIF dumps vs KB].

---

## 4. SMP/FSMP + CBPC Verdicts

**FSMP: CANNOT be broken by the remake.** Faster HDT-SMP runs a fully closed **Bullet** physics world. The DLL string scan shows full Bullet symbols (btDiscreteDynamicsWorldMt, btRigidBody, hdt::SkyrimSystemCreator::readShape → btCollisionShape) and ZERO game-Havok runtime symbols; the only "bhk"/"Havok" strings are an embedded nif.xml format table for NIF *parsing*. Its schema (hdtSMP64.xsd) allows collision filtering exclusively among SMP-declared shapes by bone/tag name — there is no config element that could even reference a game-Havok body. SMP fakes its own floor ("VirtualGround" triangle shape) — it doesn't collide with the real game world at all. **CONFIRMED** [hdtsmp64.dll strings; hdtSMP64.xsd; configs.xml]. Upstream source corroborates (SkinnedMeshWorld, Bullet types, no Havok). **REPORTED** [github aers/hdtSMP64].

**The one real coupling is indirect, through bone poses:** PLANCK drives visual NiNodes from the ragdoll (layer 3 → layer 1), and SMP reads those NiNodes as kinematic anchors (bare `<bone>` entries — hands, all 30 finger bones, forearms+twists, spine, pelvis, thighs, calves, feet, head — become mass-0 anchors driven from node world transforms each frame). Consequences:
- A *correct* remake makes SMP cloth/hair follow the body exactly as before. Desired.
- A *jittery* remake (self-colliding overlapped capsules, PD fight) jitters the anchors and SMP cloth/hair **amplifies** it. Quality risk, not a system conflict — and it would be visible as body jitter anyway. **CONFIRMED** for the anchor mechanism [3BBB-Amazing.xml; Hands.xml].
- **Hard constraint: never rename or delete NiNodes** that SMP XMLs reference by name — SMP looks bones up by name and silently loses colliders. Pure bhkCapsuleShape float patches and rigid-body edits don't touch the node tree and are safe. **CONFIRMED**.
- Symmetrically: our new Havok capsules can never PUSH SMP cloth (different worlds). If ever wanted, that's done by adding SMP shapes on the same bones in XML.

**CBPC: unaffected.** cbp.dll byte-scan shows zero hkp/bhkWorld/Havok/Bullet strings; colliders are recomputed per frame as analytic sphere/capsule checks in NiNode space (matches the existing KB entry, UpdateThingColliderPositions RVA 0x87E0). Same indirect bone-pose caveat only. **CONFIRMED** [cbp.dll strings; KNOWLEDGEBASE].

*Footnote:* third-party crash logs showing Havok frames "in" hdtsmp64.dll onFrame are stack adjacency/misattribution, not evidence of Havok interaction. **SPECULATION** (interpretation) [Nexus 179975].

---

## 5. Runtime Variation Menu

What can vary per-actor at runtime, on top of the baked NIF. The proven write mechanism throughout is **capsule float editing (vertexA/vertexB/radius) from the physics hook** — CapFixApply already does per-actor in-place writes with a generation counter. **CONFIRMED** [GrabDiag.cpp:550-590; KNOWLEDGEBASE 1789-1801].

| Variation | Rating | Mechanics | What's missing |
|-----------|--------|-----------|----------------|
| **Armor-conditional thickness** (heavy armor → fatter torso/limb capsules) | **FEASIBLE NOW** | Same write class as CapFixApply, keyed off SKSE TESEquipEvent + ArmorType; per-actor shape instances established (§3). Lerp for smoothness. Keep the degenerate A==B guard. | A day's plumbing: equip-event sink + per-slot target tables. **CONFIRMED** for the write path. |
| **OBody-following capsules** (per-NPC BodySlide morph → per-NPC radii) | **NEEDS BUILD** | Read side exists: OBody NG (enabled in profile) writes through skee BodyMorph under key "OBody"; readable via `NiOverride.GetBodyMorph(actor, morph, "OBody")` (natives present in installed skeevr.dll), via skee's C++ IBodyMorphInterface (**REPORTED** — public skee source, not locally verified), or via `OBodyNative.GetPresetAssignedToActor` + parsing the preset XML on disk. **CONFIRMED** for the Papyrus surface [OBodyNative.psc; skeevr.dll scan]. | The slider→radius mapping is the only unknown — calibrate it with the existing live capsule editor loop. This is the feature that finally beats the "one-size NIF" problem nobody else solved. |
| **Tail collision (beast races)** | **FEASIBLE NOW** (restore) / **RESEARCH** (ragdoll-driven) | Premise "tails lack bodies" is FALSE — XP32 ships 5 tail capsule bodies + constraints. Real work item: graft them back into the live beast-female winner (they were dropped — §3 bug) and ship in AIHands. **CONFIRMED**. | Making tails PD-driven/death-ragdolled requires adding Ragdoll_TailBone bones + mapper entries to the **shared** skeleton hkx (HCT surgery — HCT works via the hkxPoser registry fix — but the hkx is shared with tail-less human NIFs). High-risk, own research pass. **SPECULATION**. |
| **Horns** | **FEASIBLE NOW** (approximation) | No horn bones exist anywhere; only option without skeleton-node surgery is a longer/offset head capsule baked per beast NIF. **CONFIRMED** for the absence. | Exact-horn shapes would need new nodes + bodies — body-count rule says don't. |
| **Snout/head per beast race** | **FEASIBLE NOW** | Beast head is a bhkCapsuleShape (block 548); longer snout capsule = pure NIF float edit, zero runtime code. **CONFIRMED**. | Argonian≠Khajiit split needs a RACE SkeletalModel repoint ESP (tiny, no SKSE). |
| **Runtime shape-type switching** (e.g. fist vs open hand geometry) | **RESEARCH** (mostly NO) | Allocation/swap = CTD class. One legal door: `hkpListShape_enableChild`/`disableChild` exist in-engine (offsets.cpp 286-287) — children of a *baked* list shape can be toggled without allocation. **CONFIRMED** the functions exist; toggling on a ragdoll bone is untested. | Requires the list-shape bake to land first (§6). |

---

## 6. Shape Upgrades: List Shapes, Hulls, Multi-Capsule Limbs

### Precedent census (full scan: 19,057 vanilla NIFs + 127 loose skeleton NIFs across all mods) — **CONFIRMED**

| Construct | Precedent | Verdict |
|-----------|-----------|---------|
| bhkListShape under ONE bhkRigidBody | Routine in vanilla — 163 files via bhkCollisionObject (e.g. albinospiderbutt.nif: convex-transform + capsule children) + 78 via mopp (hhsteps06narrow.nif: 6 convex children) | Engine-proven pattern in general |
| Multi-capsule lists on ACTOR skeletons | Yes — but only on the bhkSimpleShapePhantom bumper: XP32 werewolf (2 capsules), deer (3), dragon (mopp→8 capsules) | Actors run list shapes fine — just not on ragdoll bones |
| List shape on a RAGDOLL body (bhkBlendCollisionObject) | **ZERO hits anywhere** — vanilla + entire load order | **Unprecedented-but-plausible. One-bone in-game proof test required before committing.** |
| bhkConvexVerticesShape on a ragdoll body | **Vanilla precedent**: draugr skeletons.nif "NPC Head [Head]" + "[Jaw]", dwarven sphere "NPC ChassisLid" — live, engine-driven actors | Proven engine pattern; safe route |

Neither substrate assumes single-convex bodies: PLANCK's only shape casts are on the player proxy — which is *itself* an hkpListShape (author's own comment, main.cpp 4033); stress is motor/mass-based, shape-agnostic; hit materials already resolve per-child via shapeKey (main.cpp 1471) — **so every child shape must carry the skin material uint32 591247106.** HIGGS routes closest-points through the Havok dispatcher by shape type (hand.cpp 215; collection agents serve lists) and its stage-2 grab point is visual-mesh triangles anyway. **CONFIRMED**.

### The performance model, in real numbers

- **Broadphase:** one AABB per *body*, regardless of child count. Keep the 18-body humanoid census (measured: vanilla male = 18 capsule bodies; our female = same 18) → broadphase cost EXACTLY unchanged. **CONFIRMED** for the counts, general Havok knowledge for the sweep-and-prune (**REPORTED/training**).
- **Narrowphase:** only overlapping pairs pay. A list adds ~10-20ns child-AABB culls + ~100-500ns extra GJK per surviving child (capsule-capsule analytic ~50-150ns), agents warm-started across frames. **SPECULATION** on exact nanosecond figures (order-of-magnitude model).
- **Scene model:** 5-10 driven NPCs × ~8 multi-capsuled bodies (1→3-4 children) ≈ **10-60µs extra per 90Hz physics step — under 1% of the 11.1ms frame even with 5-10× pessimism.** Each player-hand touch adds ~1-2 GJK + 2-3 AABB rejects per touched body ≈ 0.3-1µs.
- **Where the budget actually lives:** per-BODY motor drive, ~17 constraints per NPC, solver iterations, PLANCK's main-thread world-lock work. **Engineering rule: never add bodies; spend sub-shapes freely (3-4 per body on 8-10 bodies is noise).**

### Verdicts

| Upgrade | Verdict | Route |
|---------|---------|-------|
| **Flat palm** | **GO — highest payoff** (palm-contact fidelity is the product), runtime cost ~zero | Route A: convex hull — vanilla ragdoll precedent, pynifly CAN author it (add_shape + setCollConvexVerts), but not live-tunable, slightly worse deep-penetration recovery (**SPECULATION** on the last point). Route B: 2-3 slim-capsule list — live-tunable children, unprecedented on a ragdoll bone, needs binary block-APPEND surgery or NifSkope. **Recommended: prove Route B on one hand bone in-game; if stable prefer it for tunability, else fall back to Route A.** |
| **3-capsule upper arm** | **SKIP** | Near-cylindrical; the tuned single capsule (tool already built, capUpper* slots) captures the silhouette. Same surgery cost as the palm for almost no anatomical gain. |
| **Multi-segment femur** | **2-segment first** | Thick-upper/thinner-lower captures the taper a single capsule can't; 4 segments = diminishing returns. |

### Engineering notes for the bake

- **Appending blocks at the END of a NIF keeps every existing block reference valid** (refs are absolute indices; data laid out in index order before the footer). Adding a list + children = extend header arrays, append block data, repoint ONE shape ref on the target rigid body. Easier than the Report-28 surgery (which *removed* nodes). NifSkope is the interactive fallback. **CONFIRMED** [nif_blocks.py parser proves the layout].
- pynifly declares list/capsule buffer types but capsule WRITING is broken per project history → hull = pynifly, list-of-capsules = binary append or NifSkope. **CONFIRMED**. **⚠ SUPERSEDED 2026-07-03: only SET-on-existing is broken; the CREATION path (addBlock via `bhkWorldObject.add_shape`/`bhkListShape.add_shape`, props carried at creation) works perfectly — null round-trip byte-identical on skeleton_female.nif, and the 3-capsule palm list was authored with it same day. pynifly IS the list-shape tool; see the KB entry.**
- **Live-tuning caveats for lists:** (a) the CapFix tuner currently skips non-capsule shapes — must be extended to drill into `m_childInfo[i].m_shape`; (b) hkpListShape caches a union AABB at construction — live-editing a child beyond the original envelope risks missed collisions until `recalcAabbExtents` (**SPECULATION ~75-85%**, SDK headers not vendored locally); (c) degenerate-capsule visualizer trap + NIF mirroring rules apply PER CHILD; (d) verify once that per-actor GetScale recurses into list children on a scaled NPC.
- Runtime list machinery already REd: bhkListShape ctor 0xE18840, disableChild 0xA9C3F0 / enableChild 0xA9C420 — toggling without allocation is possible later; **creation stays NIF-baked.** **CONFIRMED** [planck offsets.cpp 153-154, 286-287].

Reusable scan tooling from this pass (scratchpad, session 0146d49c): `nif_blocks.py` (full parser, proves append-safe layout), `scan_struct.py`/`scan_detail.py` (shape/collision classifier), `scan_listshape.py`, plus `vanilla_ext/` (~2GB full Meshes0.bsa extraction — reusable, deletable). *Tool bug worth a KB entry: AutoMod archive extract IGNORED `--filter` and extracted the entire BSA.*

---

## 7. The HIGGS Player-Hand Plan

Local source at `tools/_research/higgs` is v1.10.10, verified identical to GitHub master (last commit May 2026) and to the installed mod — any fork we build is generation-compatible with what's running. **CONFIRMED**.

**What ships today:** ONE flat box per hand — 10 × 3 × 18 cm palm slab (half-extents {0.05, 0.015, 0.09}m), convex radius 0, skin material, keyframed, layer 56 L_HIGGSCOLLISION, created in `Hand::CreateHandCollision` (hand.cpp 559-614) only on world change or beast toggle (the proven-safe allocation pattern), hard-keyframed to the first-person hand node (i.e. the *controller*, deliberately without the VRIK offset) with room-space compensation. Collision disabled while physically holding or two-handing. PLANCK fetches it via `GetHandRigidBody` and upgrades it to KEYFRAMED_REPORTING. **CONFIRMED** [hand.cpp; main.cpp; planck main.cpp 3867-3893].

**The 80% win largely already ships.** The default box IS a flat open palm covering palm + extended fingers, fully tunable in `higgs_vr.ini` (HandCollisionBoxHalfExtents/Offset/Radius + Beast variants, installed ini lines 50-69, all at source defaults); Radius > 0 rounds it capsule-like. **Trap:** with `reloadConfigIfModified=1` the OFFSET applies live, but half-extents/radius are baked at body creation — changes need a world transition or beast toggle. **CONFIRMED**.

**The real limit is fist-vs-open-palm** — one static slab can't be both. Options, in ascending cost:
1. **Ini-tune the slab** (free, today).
2. **Runtime-scale the box half-extents from average finger curl** — single body, no allocations; hkpBoxShape half-extent live edits are unverified but analogous to our KB-verified capsule float edits. **SPECULATION — spike test.**
3. **Per-finger colliders (the fork).** HIGGS never reads VRIK finger state (the API exposes getFingerPos; HIGGS never calls it) — it poses fingers itself via FingerAnimator node lerps on the third-person skeleton. **The ideal attach point is right after `NiAVObject_UpdateNode(thirdPersonRoot)` in PostVRIKPCUpdateHook (hooks.cpp:542)** — finger node world transforms are final there regardless of who posed them (game/VRIK/index tracking/HIGGS). No VRIK API needed. Fork surface: hand.h members; hand.cpp Create/Remove/Update/Move collision; main.cpp world-change block + RegisterPlayerSpaceBody; physics.cpp identity checks + contact-listener haptics; hooks.cpp projectile redirect; **keep the palm box and ADD finger bodies** — PLANCK consumes GetHandRigidBody. ~300-500 lines following existing patterns; the real friction is the skse64-fork VS build. **CONFIRMED** for the code reads. **Caveat (SPECULATION):** PLANCK's IsHiggsRigidBody only recognizes the single hand/weapon/held bodies — new finger bodies would be ordinary layer-56 bodies to it; hit attribution vs NPCs needs a spike test.

Bonus already there: HIGGS computes per-finger curl geometrically at grab time (FingerCheck curve-vs-triangle sweeps → grabbedFingerValues[5], exposed via GetFingerValues) — the same 2-stage system Report 11 documents for the NPC grab port. **CONFIRMED**.

**Nobody has done this either:** all 9 GitHub forks are 0-1 trivial commits ahead; no Nexus mod touches hand collision shape. **REPORTED** [GitHub API + web search].

---

## 8. PITFALLS — the blunt list

1. **Self-collision jitter during touch/grab.** PLANCK switches on non-adjacent self-collision exactly while AIHands is being used. Oversized capsules overlapping non-adjacent neighbors = limbs shoved, buzz. *Notice:* jitter that appears ONLY while touching/holding an NPC. *Rule:* keep new capsules inside the silhouette enough to clear non-adjacent neighbors in pose space. **CONFIRMED**
2. **Runtime allocation/shape-swap = the proven CTD class.** Two documented UAF crashes from our own body allocation; HIGGS/PLANCK both avoid rigid-body setShape; no VR address for it exists. *Rule:* shape-TYPE changes are NIF-baked, full stop. Float edits only at runtime. **CONFIRMED**
3. **Inertia/COM mismatch on drastic resizes.** Stored numbers stay 2011-vanilla; a capsule that leaves the stored COM outside itself rocks/tumbles in death ragdoll; long lever arms vs small stored inertia spin. *Notice:* corpses settling weirdly, knockdowns flopping unnaturally. *Fix:* bake matched inertia/COM floats — same byte-patch class. **CONFIRMED** mechanism / **SPECULATION** severity
4. **Feet must stay BOX inertia** or NPCs walk in place on inclines (XP32 TRP's explicit warning — an engine coupling, not a physics nicety). *Notice:* NPCs treadmilling on slopes. **REPORTED**
5. **Renaming/deleting NiNodes silently breaks SMP** (bone lookup by name → colliders/anchors vanish without error). Float patches don't touch the node tree. *Notice:* cloth/hair suddenly ignoring a body region. **CONFIRMED**
6. **Getup delay / re-knockdown loop.** Velocity-based getup check + PLANCK's bugged-getup fallback: a rig that can't settle could cycle knockdowns. *Notice:* NPCs taking forever to stand, or standing and collapsing repeatedly. **CONFIRMED** paths / **SPECULATION** loop
7. **Player body-blocking distance changes are felt instantly.** Too fat = doorway/crowd snags (Bigger Bounds precedent); too thin = standing inside NPCs. *Notice:* first minute of play. **CONFIRMED** mechanism / **REPORTED** precedent
8. **Melee hit position and body-part attribution shift** (nodeName in PlanckHitEvent feeds downstream/locational mods). *Notice:* hits landing "off", wrong-part reactions. **CONFIRMED**
9. **Beast races don't inherit humanoid edits** — separate NIFs; and **absolute file offsets never transfer between NIFs** — re-derive per file with the unaligned signature scan every time. *Notice:* a "patched" beast file with garbage floats = instant weirdness or silent no-op. **CONFIRMED**
10. **One static NIF cannot match per-NPC morphed bodies.** Only uniform GetScale is engine-compensated. Until the OBody runtime layer exists, tune to the *median* body and accept per-NPC error. *Notice:* visual-vs-collision mismatch on outlier presets. **CONFIRMED**
11. **The visualizer lies.** Stale tessellation (needs off/on toggle to redraw after edits) and degenerate A==B capsules hid real geometry for weeks in Report 13. Keep the degenerate guard in every write path. *Notice:* "nothing changed" after an edit that definitely landed — toggle first, trust readback floats over pictures. **CONFIRMED**
12. **List-shape unknowns (three).** (a) Unprecedented on ragdoll bones — one-bone proof test before committing; (b) cached union AABB may go stale on live child edits beyond the bake envelope (**SPECULATION**) — bake generous, recalc if possible; (c) GetScale recursion into children unverified — check liveR on one scaled NPC. *Notice:* (a) CTD/no collision on spawn; (b) touches missing at the child's far end; (c) child radii not matching GetScale identity.
13. **Beast-female tail collision is ALREADY missing in the live game** (VRTouchEvents' file dropped 5 bodies + 5 constraints). Any tail work that doesn't fix this first will produce confusing test results on female beasts. **CONFIRMED**
14. **Mid-game SetScale is probably stale until 3D reload** — don't calibrate against a freshly-rescaled NPC. **SPECULATION** — run the §3 test once.
15. **Heel systems interfere** (RaceMenuHH vs no me molestes precedent; our own heel-fix saga). Any pelvis/leg capsule retune should be re-checked with heels equipped. **REPORTED** precedent / **CONFIRMED** our own layer findings
16. **KB carries a wrong 9-body claim** — future sessions reading it will mis-plan. Correct it to the 18-body truth before surgery starts. **CONFIRMED**

---

## 9. Recommended Sequence

Ordered to keep every step independently testable and revertible. The live capsule editor + collision visualizer + readback logging is the verification harness throughout.

1. **Paper fixes first (½ day).** Correct the KB 9-body entry (#16). Add the AutoMod `--filter` bug to KB. Decide the beast-female tail restoration (recommend yes).
2. **Exhaust the single-capsule live loop on skeleton_female.nif** (Report 13 roadmap 1-3): tune all 18 capsules by eye in VR — torso, pelvis (respecting the No-Me-Moleste intent), thighs 2-segment *later*, arms, head. Self-collision check at every step: grab/touch while watching for buzz (#1). Bake winning floats via byte-patch.
3. **Bake matched inertia/COM** for any body whose size/center moved substantially (#3). Death-ragdoll test: kill, watch settle; knockdown test: getup timing (#6).
4. **Regression gate** before anything structural: doorway walk, crowd walk, melee session (hit position + part attribution), arrow test, incline walk (#4), heels-equipped pelvis check (#15), two-NPC leak check (per-actor writes), SMP cloth sanity.
5. **One-bone list-shape proof test:** append a 2-capsule bhkListShape onto ONE hand body, spawn, touch, grab, kill, reload. If stable → Route B for the palm. If not → Route A convex hull (pynifly, vanilla-proven).
6. **Palm upgrade both hands; femur 2-segment list.** Extend the CapFix tuner to drill into list children; set material 591247106 on every child; verify GetScale recursion on one scaled NPC (#12c).
7. **Propagate to the other three NIFs:** humanoid male (currently ALL stock — hand bake + new tuning), beast male, beast female (+ tail graft #13). Re-derive all offsets per file by signature scan (#9). Base beast edits on VRTouchEvents V2.0's copies; ship all four in AIHands.
8. **Runtime variation layer:** armor-conditional thickness lerp (feasible now), then OBody-following calibration (the live loop maps sliders → radii). This is the step that makes the remake correct for *every* body, which no static-NIF mod ever achieved.
9. **HIGGS hand fork — separate track, separate risk budget.** Start with ini tuning (free), then the curl-scaled box spike (**SPECULATION** #2-adjacent, but single-body), then per-finger colliders with the PLANCK-attribution spike test.
10. **After each phase:** update this report series and KNOWLEDGEBASE per the standing rule; the reusable scan tooling lives in the session scratchpad and should be promoted to `tools/` if reused.

**Confidence statement for the go decision:** the *core remake* (single-capsule anatomical retune + inertia bake, steps 2-4) is ≥90% — every mechanism is primary-source-confirmed and the write path is already proven live. The *structural upgrades* (step 5-6) are gated behind their own proof test precisely because the list-on-ragdoll-bone construct is unprecedented — do not skip the one-bone test. The *runtime layers* (step 8) are 80-88% pending one calibration unknown each, with test procedures defined above.
# 07 — Dismemberment ↔ Ragdoll Sync (research, 2026-07-06)

Source-grounded (Nexus pages + installed files + PLANCK source read) analysis of the Dismembering
Framework, why it fights the VR ragdoll, and whether PPB can fix it. Companion to
`06_BoneFollow_Tails_And_Cavities_Research.md`. Confidence tags: CONFIRMED (primary source this pass) /
REPORTED (single web source) / SPECULATION (inference).

## What's installed (load-order reality)
- **Dismembering Framework (DF)** — Seb263, Nexus 126203, **v1.2.2.0**, SKSE C++ DLL on CommonLibSSE-NG
  (one binary = SE/AE/**VR**). ENABLED, modlist line 161 (`D:\Games\My Skyrim\mods\Dismembering Framework\`
  = DLL/.esm/.bsa/.ini/JSON/MCM). Needs an asset pack to do anything (ships pre-cut severed + stump
  meshes + node-config JSON). Reads **Precision** hit-location to sever at the aimed spot.
- **Next-Gen Decapitations** — Seb263, **v1.4.3.0**, ENABLED line 163. Treats the severed head as a
  movable Havok object (`fHeadMass=10`, impulse/spin, per-frame scale/collision updates).
- **DF - Official Creature Asset Pack** (158), **Enhanced Blood Textures** (+patch) (159-160).
- ⚠ **ACTIONABLE CONFLICT**: **Simple Beheading - NG** (also Seb263) is ENABLED, line 162 — DF's own
  page says **"DON'T use Simple Beheading with the Dismembering Framework"** (both hook the vanilla
  decapitation function). A prime suspect for the "some crashes occurred in VR" reports. User's call to
  disable one; flag, do not touch the load order unasked.

## How DF severs (mechanism)
Skyrim has NO runtime mesh-cutting. "Losing a member" is always: **(hide the original triangles) +
(show a pre-authored stump) + (optionally spawn a detached rigid body with impulse)**. DF 1.2.2:
matches the hit to a **skeleton node name** → swaps the region for an **artist-pre-cut "severed" asset**
(stump stays on the body) → **spawns the detached limb as its own object** with a physics impulse
(`bAllowLimbScaling` scales it to the actor). The cut is baked in Blender at the bone level; DF does
NOT edit geometry at runtime. (DF 2.0/CIF alpha = procedural node-detach from just a node name, NOT the
installed version.) **The ragdoll's 18 bodies are never re-created** — the corpse keeps its full
18-body ragdoll minus whatever mesh was hidden. CONFIRMED [Nexus 126203 + installed files].

## Why the VR ragdoll breaks (the conflict)
**One sentence**: the dismember edits the VISUAL layer (hide partition / scale the node toward 0), but
the severed limb's **Havok ragdoll body stays in the world**, node-following to a now-degenerate
transform and still PD-driven by PLANCK every frame — and in VR that body is the LIVE touch surface, so
you get an invisible hitbox where the limb was. It is the heel/clavicle **layer-mismatch pattern again**,
now applied to a whole limb, and it is exactly PPB's own **A==B "invisible-but-colliding" trap** at
limb scale.

Failure modes, ranked (CONFIRMED against PLANCK main.cpp + our reports):
1. **Ghost limb body** (highest, fires every hit) — mesh hidden, body untouched → invisible melee
   hitbox / HIGGS-grab-able air / wrong body-part attribution in PlanckHitEvent.
2. **Degenerate-transform self-collision** — bone→0 collapses the body onto its parent joint; PLANCK
   enables non-adjacent self-collision exactly during player touch → shove/buzz/jitter.
3. **Constraint pulls a phantom joint** — if the mod moves the body (spawns debris) while the ragdoll
   constraint still binds it, the parent chain gets yanked → stump flail (worst in death ragdoll).
4. **Detached limb falls through floor → whole corpse unloads** — the ONLY mode PLANCK documents by
   name: PLANCK sets biped-layer bodies to not collide with ground (so living ragdolls don't snag), so
   a truly-detached L_BIPED limb falls through and the engine unloads the actor chasing it. PLANCK's
   fix = relayer stray **non-ragdoll** biped debris to L_DEADBIP (`PotentiallyConvertBipedObjectToDeadBip`,
   main.cpp 1692-1733; **early-returns for in-instance ragdoll bodies** 1715-1720). REPORTED [Nexus
   66025 changelog "Maximum Carnage limbs … fall through the ground and unload the body"] + CONFIRMED.
5. **Getup/settle desync / re-knockdown loop** — a body that never settles trips PLANCK's bugged-getup
   re-knockdown (4760-4768) / warp guard (`maxAllowedDistBeforeWarp=15`, 4926-4937).

**Why VR is worse** (all CONFIRMED): (a) PLANCK PD-drives all 18 bodies every frame — flatscreen leaves
a living actor's ragdoll gameplay-inert; (b) the ragdoll IS the live touch surface in VR; (c) death
ragdoll is player-interactive (you grab/yank corpses) so a broken/degenerate body gets pulled, exposing
the flail. PLANCK never reads shape (gains are global constants) so it can't "notice" a limb is gone.

## PPB severing options, rated (against the 3 HARD RULES + PLANCK's per-frame re-assertion)
| Option | Class | CTD risk | PLANCK fights? | Verdict |
|---|---|---|---|---|
| **(c) shrink capsule (float) + no-collide filter** | float + filter word — allocation-free, in-rules | Lowest | filter MAYBE re-asserted on death-layer swap → re-stamp per-frame | **✅ LEAD** |
| (a) break the ragdoll constraint at the cut | pivot float / setEnabled | Low | **YES, strongly** — PLANCK loosens/restores pivots + reconverts every frame; only clean via PLANCK's own `disableConstraints` config | Defer / must route through PLANCK |
| (b) remove distal bodies from world (0x61A0E0) | — | dangling-ref risk | wrong tool — 0x61A0E0 SKIPS the 18 ragdoll bodies by design | ❌ N/A |
| (d) spawn a NEW severed-limb body | add body + shape alloc | **HIGHEST — the proven CTD class** | crashes first | ❌ NO-GO — leave to DF + PLANCK's DeadBip relayer |
| (5) `disableChild`/RemoveNonRagdoll per body | — | — | — | ❌ no legal per-body disable for a standalone ragdoll body |

## Recommended: "Ghost the stump" — PPB's natural 4th runtime feature
In the existing 0xB266AB per-frame hook, per actor: watch each limb's dismember NiNode **scale
collapsing toward 0** (the universal hide signal — works for classic node-scale AND procedural
node-name systems; or accept a keyword/StorageUtil flag from DF). On sever, mark that limb's ragdoll
body and, **every frame thereafter**: (1) float-shrink its capsule radius to a small-but-**real** value
(NOT 0 — the degenerate A==B trap: invisible to collviz, still colliding) **and** (2) stamp its
collision-filter word to a private no-collide layer (the 60/61 pattern already neutralized in the 64×64
table). Zero new bodies, zero shape allocation, zero constraint fighting — the same edit-class PPB ships
today (CapFix floats + marker filter words), retargeted by a sever event. Re-asserting per-frame beats
both PLANCK's death-layer swap and PPB's own auto-heal trying to undo it (both writes are cheap).
Confidence the write-classes are safe ≈85% (each individually CONFIRMED-safe); the combination on a
live severed limb is untested.

Cross-consumer fit: sits beside heel-fix / clavicle-follow / variation-layer (all per-frame per-actor
float/filter writes on the per-actor clones). "Keep the physics body consistent with the visible mesh"
= the same charter as "keep CBPC consistent with armor." VRTouchEvents (no phantom touch on a gone limb)
and AIHands (grab/give) both benefit.

## The GO/NO-GO that decides the whole feature (open question E)
**IF DF already removes the ragdoll body on sever, there is no phantom stump and PPB has nothing to fix.**
Verify FIRST, one VR session: dismember an NPC, toggle `trd`, and look for a leftover **invisible
collider** where the limb was (and try to melee/HIGGS-grab the empty space). If a ghost body is there →
build "ghost the stump." If not → the feature is moot; only the load-order conflict (Simple Beheading)
and PLANCK's existing DeadBip fix matter.

Other in-game unknowns before building: **A** auto-heal/auto-fit must treat "severed" as the new sticky
target (a `severed[]` flag, like `autoChild[]`) or the stump re-inflates in ~1s; **B** whether a PPB
no-collide filter survives PLANCK's death-layer swap or must be re-stamped after PLANCK's pass (top
technical test); **C** confirm PPB's hook body actually runs the CapFix path on a **dead** ragdoll
(PLANCK keeps dead actors driven, so it should — but PPB's statue/auto-seat gates target live actors;
add a dead-ragdoll path if needed); **D** verify with `trd` the ghosted stump truly stops colliding.

## Sources
PLANCK source (CONFIRMED): `tools/_research/planck_src/src/main.cpp` 1692-1733 (DeadBip convert +
in-instance early-return), 2373-2393, 2961-3005 (force-dynamic + hinge→ragdoll), 4455-4947 (PD gains,
constraint loosen/restore, warp/re-knockdown), 5083-5087 (`disableConstraints`), 5278-5352
(RemoveNonRagdollRigidBodies = furniture/non-ragdoll only); `include/config.h` defaults.
Web (REPORTED): Nexus 126203 (DF), 135254 (Next-Gen Decapitations), 66025 (PLANCK changelog: limb
falls through floor / DeadBip), 146873 (Core Impact Framework), Seb263 Patreon (DF 2.0 alpha),
GECK/niftools (BSDismemberSkinInstance, SBP50DECAPITATEDHEAD, gorecaps). Our reports: AIHands Module 14
(§1-2 fixed shared ragdoll, VR-live-surface, node-follow, HARD RULES), PPB Module 03/04/06.
