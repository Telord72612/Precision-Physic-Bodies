# 04 — Pitfall Ledger (every expensive lesson; do NOT re-learn these)

## ★ RULE 0 — SHAPES ARE SHARED BETWEEN ACTORS. BODIES ARE NOT. (2026-07-27, paid in blood)
Actors built from the SAME skeleton NIF reference the **same `hkpCapsuleShape` objects**. Editing a
capsule's geometry at runtime "for one actor" edits it for **every actor on that skeleton**.
- **What it cost:** collapsing an NGD severed head's capsules (ONE actor passed to the call — the log
  proves it) stripped the collision off every live Redguard in the cell: "the NPCs I spawn have no
  havok bodies… their main body loses their havok body".
- **What IS per-actor and safe to write:** the **body** — mass, inertia, friction, transform, motion
  type, collision filter, constraints. All of PPB's runtime body work is legitimate; only *shape*
  geometry leaks.
- **The safe ways to give one actor different geometry:** (a) a **separate NIF** for that class of
  actor (how the severed head is solved — `14`), or (b) clone the shape first, then edit the clone.
- **Recovery without a reload:** nudge any snapshotted `cap*` knob (e.g. `capSpine0R 4.400 → 4.401`).
  That bumps the CapFix generation and re-writes correct geometry onto every driven NPC. Verified
  live. Actors already flagged `exclPpb` (dismember-touched) stay as-is until a reload — acceptable.
- CapFix's own per-actor writes are unaffected *in practice* because it re-dresses every driven actor
  each generation, so the last write wins for everyone and everyone gets the same correct values.

## The three hard rules (research-verified, non-negotiable)
1. Capsule/pivot FLOAT edits are safe at runtime and in the NIF (mass/inertia/COM are stored numbers,
   never re-derived from shape).
2. Shape-TYPE changes are NIF-baked ONLY — runtime allocation/setShape = the twice-proven CTD class.
3. Never add BODIES to the ragdoll instance. (Non-ragdoll FOLLOWER bodies on a bone are a different,
   allowed class — vanilla beast skeletons ship tail bodies outside the ragdoll; collviz_markers creates
   its own bodies at runtime.)
4. **★ STRUCK 2026-07-08 — the old rule 3 read "sub-shapes are ~free (<1% frame at realistic counts)".
   THAT WAS ASSERTED WITH ZERO MEASUREMENT AND IS WRONG.** (User was right to object.) The truth, from
   the disassembly investigation in `11_ListShape_Performance.md`:
   - **Broadphase: genuinely unaffected** (one proxy/body; getAabbImpl reads only the cached union AABB). ✔
   - **Narrowphase: culled** by a sorted 1-axis sweep over per-child AABBs before GJK. Cheap. ✔
   - **BUT RAYCASTS ARE O(children) WITH *NO* AABB REJECTION** (`castRayImpl`) — ~14× on a ray through a
     21-child torso. Hits crosshair rays, HIGGS grab rays, arrows, spells. Nobody expected this.
   - **AND THE REAL SHIP-BLOCKER IS A CORRECTNESS CLIFF, NOT ms:** Havok stages **≤256 contact points per
     BODY PAIR**, guarded by an assert that **compiles out of retail**. Our layout yields COM(21)×Spine2(11)
     = **231 child pairs** intra-actor and COM(21)×COM(21) = **441** cross-actor. At 3-4 pts/pair that
     breaches 256. Breach behavior (graceful reject vs memory corruption) is **UNRESOLVED — do not find
     out empirically.** **DO NOT BAKE COM AT 21 UNTIL A CONTACT-POINT COUNTER HAS RUN.**
   - Predicted frame cost (modelled, ±3×): +0.03-0.05 ms idle crowd; +0.5-1.5 ms worst realistic.
   - **`hkpListShape::disableChild(i)` IS a live, allocation-free child on/off switch** (VERIFIED 95%: it
     is a LEAF function — no `.pdata` unwind entry across all 118,450 RUNTIME_FUNCTION records ⟹ cannot
     call ⟹ cannot allocate; it's a u32 bit-RMW + u16 increment with a built-in idempotence guard). The
     bitmask is consulted **at query time every frame** in 5 disassembled sites ⟹ **no agent invalidation
     needed**. Requirements: replicate the idempotence guard, keep `numDisabledChildren` exact, call
     `hkpCollidable::boundingVolumeData.invalidate()` after mutation, never re-enable past the count baked
     at world-add time, teach RepatchListAabb to skip disabled children. NEVER spike-tested live → 75%.
   - **The correctness fix (not an optimization): CHILD LOD.** Full children only for the ≤2 actors the
     player is touching; every other actor collapses to child 0. Kills cross-actor COM×COM 441→21, kills
     the always-on per-body AABB rebuild tax, kills the 14× raycast multiplier for untouched NPCs.
     Does NOT fix intra-actor COM×Spine2=231 → that still needs **COM 21→~9**.

## Joint-editing lessons
- **The contamination trap** (cost: 3 sessions): deriving a constraint's partner copy from the LIVE
  pose re-encodes whatever error the data already has — the constraint DEFINES the driven equilibrium,
  so "healing at the current pose" photocopies the disease. Partner targets must come from
  pose-independent data (runtime referencePose bind relations). Every write path must be deterministic.
- **Three binds, none equal**: NIF-node bind ≠ runtime referencePose bind (2-6u per joint) ≠ any
  captured in-game equilibrium. Trust ONLY the runtime referencePose for math and the user's eye/
  auto-seat for placement. (Old "19:53 known-good" partner numbers = a deflected equilibrium — retired.)
- **Pivot pair consistency**: both stored copies must land on one world point or the joint spawns
  pre-stressed (chronic jitter). Strain meter = the gauge. Position-only edits; twist/plane axis
  re-aiming is a DEFERRED layer (limits are still 2011).
- **A bone's joint sits at the BOTTOM of the segment it drives** — the spine2 "chest" joint is
  mid-back; shoulders ride 15u above it on a rigid lever (small angle error × long lever = big arm
  displacement). Eye-intuition ("chest joint should be at the chest") is wrong; the red cubes are right.
- **The clavicle gap**: the ragdoll skips the clavicle → fixed shoulder anchors cannot follow
  protraction (uniform-chain-offset + zero-strain PIVTRACK signature). Runtime follow required.
- **Rotation centers hide inside flesh** (humeral head deep in the deltoid); a pure along-bone push
  can exit the flesh because the 2011 body axes ≠ the real bone axes.
- PLANCK converts wrist/elbow HINGES to ragdoll constraints at add (`convertHingeConstraintsToRagdollConstraints=1`)
  — runtime type ≠ NIF type; the BAKE targets the NIF's hinge blocks; instance REPLACED log = normal.
- Actor::IsDead() is TRUE for kRestrained actors (use GetLifeState). SetRestrained-style freezes invite
  AI-warp teleports (motive-kill via DoNothing package override; script teleports still bypass).
- **AI-warp teleports rebuild the ragdoll = factory reset of ALL live edits** — every runtime system
  needs self-heal (identity probes + stored-vs-target rewrites). Two warp classes: rebuild-warp
  (identity changes) and move-warp (same bodies, nothing resets).

## PLANCK interplay
- `loosenRagdollConstraintPivots` (our override = 0): PLANCK's per-frame pivot collapse — hid the 2011
  miscalibration for years and OVERWRITES live pivot edits. Off = calibration mode (stock-strain leans
  appear on unfixed NPCs). **Restore to 1 at bake**: collapse is a no-op on true joints; unfixed
  skeletons get the band-aid back. `loosenRagdollContraintsToMatchPose` (540) stays 1 throughout.
- PLANCK enables NON-ADJACENT self-collision exactly while the player touches an NPC → capsules must
  clear non-adjacent neighbors in rest poses (adjacent pairs never collide — cap overlap at joints is
  free). Held/given objects are moved into the NPC's group by AIHands (exempt).
- PD servo lag: bodies trail bones by a few units DURING motion, reconverge at rest — not an error.
  Judge placement only on a still or statued body.
- **Heel fix vs the SEATED leg (found in-VR 2026-07-06, verification session)**: the shipped heel fix is
  a brute whole-ragdoll +heelZ Z-lift (0xA26C05 drive bias + 0xB268DC root compensation; log
  `HEELFIX ... heelZ=8.0u`). PLANCK's foot-IK pins the foot to the ground; the lift raises the hip → the
  leg chain absorbs the ~8u by BENDING THE KNEE (~15-20° observed on a heeled Redguard). It was LATENT
  with the old mis-seated knee/ankle (wrong leg geometry hid it) and became visible the moment auto-seat
  put knee+ankle on their true XP32 origins — the bodies are now honest, exposing the band-aid's
  imprecision (this is a documented heel-fix watch-item: "feet/legs fighting foot-IK"). **Confirming
  test: console `hf` off → knee straightens (and zones drop to barefoot height) = heel-fix-caused.**
  **Fix = heel-fix v2 for the leg (roadmap): anatomical — plantar-flex the ankle/foot + raise the
  pelvis rather than stretch the chain, OR decouple the leg from the naive lift so foot-IK stops
  fighting.** Meanwhile: **calibrate the leg on BAREFOOT NPCs** (already the standing rule) to remove
  the confound. Upper body + spine auto-seat verified excellent this session (ball-vs-bone ~0.0-0.2u).
- **Full-body auto-seat verified 0.0 (2026-07-07)**: after a clean statue, PIVTRACK read 0.0u on ALL 11
  joints incl. the previously-jittery knee/ankle (strain <0.1). The earlier leg "jitter" was auto-seat
  convergence noise + the fat foot capsule's ground interaction, NOT the heel-offset (heel fix was ON at
  heelZ=8.0 and the leg was clean anyway — so the user's heel-spacing theory is still UNTESTED; needs the
  shoe-off experiment). Leg-jitter fix ≠ heel confirmation.
- **PivFix self-heal FIGHTS a player grab (found in-VR 2026-07-07)**: grabbing an NPC arm displaces the
  wrist/elbow bodies ~12u; PivFix's 1 Hz self-heal snaps the pivot back each second → bounce + runaway
  (PIVTRACK to 6u off-bone, E-S spacing 20.6→27.3u, strain 0→0.63, HEAL childOff 6-13u/sec). The
  scramble guard (>40 u/s defer) misses it (grab is slower); the calm gate (>8u strain) misses it
  (<1u). **FIX = grab gate on PivFixApply** (pause re-seat while HIGGS/PLANCK-grabbed) — but CALIBRATION-
  ONLY priority: PivFix RETIRES at bake (pivots baked-true), so shipping never sees this. Detail: report 09.
- **★ HEEL FIX breaks the GETUP of a heeled NPC → stuck floating (found + log-analyzed in-VR 2026-07-07 —
  the user's long-hated bug; MECHANISM corrected twice before landing here)**: the heel fix biases
  worldFromModel +heelZ every frame (0xA26C05) AND its v4 half subtracts heelZ from the root pose bone
  (0xB268DC) — BY DESIGN it lifts the Havok bodies +8u while keeping the mesh planted (= the user's "the
  Havok body goes up, the mesh doesn't follow"). Gate: `if (actor->IsInRagdollState()) return;`
  (PPBHook.cpp:134/:169). **During GETUP she is NOT IsInRagdollState (she's transitioning out, being
  driven by the getup animation) → the heel fix FIRES → the +8u lifts her feet off the ground → the
  getup animation can't GROUND → getup fails → re-ragdoll → driven again → +8u again → getup fails again
  → stuck floating.** NOT a runaway accumulation (the earlier wrong guess). Log signature (20:24-29):
  PPB silent while fully ragdolled (0 HEELFIX/PIVTRACK) → HEELFIX RAMPS 1→4→13→9 as getup is attempted →
  PIVTRACK stays ~0 (whole actor lifts together, bodies track skeleton) → violent DEFERRED speeds
  (148-330 u/s) only when the player grabs/flings the floating body. **CONFIRM: barefoot NPC (heel fix
  never fires) gets up clean; heeled floats. Or `hf` off.** **FIX (SHIPPING priority — heel fix is
  unbakeable): gate off during GETUP/knocked/bleedout, not just IsInRagdollState — e.g. GetKnockState()
  != kNormal, or a "not standing on the charcontroller" check.** Sibling of the "kRestrained IsDead trap":
  engine state predicates lie about the getup/limp transition.
- **"shorten"/"extend" = ONE endpoint at a time, NOT symmetric** (user rule, 2026-07-07). "shorten by Nu"
  moves the point CURRENTLY IN FOCUS by N along the bone; it does not pull both ends toward center. Claude
  erred symmetric twice and corrected. Full flesh-fit method + per-bone axis maps (which are POSE-DEPENDENT
  and differ from the hanging-arm recorded maps): report 09. Hand's Y axis = inward/outward (NOT fwd/back).

## ★ THE COLLISION-CALLBACK RULE IS "NO FLOAT/SIMD", NOT "NO LOGGING" (2026-07-16, CTD on load)

The ledger + three code comments said *never log* from `FilterDecision` (fmt/spdlog SIMD spills fault on
HIGGS's misaligned trampoline stack — proven 2026-07-09). That framing was **too narrow and it cost a CTD**:
adding three plain `float` knob reads (`NpcFingerVsClutter/VsWorld/VsNpc`) to `NpcFinger::FilterDecision`
raised register pressure until MSVC spilled a NONVOLATILE xmm in the prologue — `movaps [rsp+0x20], xmm6` —
a 16-byte-ALIGNED store onto a stack HIGGS hands us at **RSP ≡ 8 mod 16**. #GP → CTD on save load.
- **Read the signature:** access violation at a `movaps` + address `0xFFFFFFFFFFFFFFFF` = alignment fault,
  not a bad pointer. Check `RSP mod 16` in the crash log to confirm (8 = this bug).
- **The rule:** a collision-thread callback does PURE INTEGER work. No floats, no SIMD, no logging, no
  allocation, no locks. Knobs are sampled on the MAIN thread into integer atomics — the pattern
  `HandBox::FilterDecision` (`g_boxPart`) and `PerfSys::FilterCB` (`g_cbSelfThigh`) already used;
  `NpcFinger::FilterDecision` was the lone violator (its pre-existing `HandBoxSubLayer()`/`NpcFingerOwnBody()`
  float reads were latent — they simply hadn't tipped the allocator yet). Fix: one `g_filterKnobs` uint32
  bitfield + `RefreshFilterKnobs()` on the main thread (OnFrame + top of OnPreDrive, so it is hot BEFORE any
  body enters the world).
- **A source review cannot catch this class** — the C++ is innocent; the defect is codegen. Verify by
  disassembling the built DLL and inspecting the callback's prologue for aligned xmm6+ spills.
- Full portable entry: KNOWLEDGEBASE.md 2026-07-16. Same family as every other "the caller's contract is not
  what you assumed" bug in this ledger.

## Console / visualizer / marker lessons
- Console compiler KILLS: any hijacked command with parameters; also referenceFunction=true+zero-params.
  Working recipe: referenceFunction=false, zero params, Console::GetSelectedRef(). Entry-log every exec.
- collviz: skips bit-14 (non-collidable) bodies outright (main.cpp:1311); tessellates once (toggle to
  refresh EDITED shapes; NEW shapes appear lazily); degenerate A==B capsules invisible while colliding;
  per-layer colors via layerColors ini key (our override loses the MO2 conflict until the user reorders).
- **Undefined collision-layer rows default 0xFFFFFFFFFFFFFFFF (collide with EVERYTHING)** — never
  assume empty; write the table (NeutralizeMarkerLayers).
- **The charController filter branch bypasses the layer table** — the only coexistence recipe it
  honors is same-system-group (+bit15), the same way an actor's own ragdoll coexists with her controller.
- MO2 modlist.txt: TOP of file = HIGHEST priority (proven via two known winners).

## Statue / pose lessons
- Statue must stomp ROTATIONS ONLY (translations+scale stay animation-owned) — full-transform stomp vs
  the 0.85-scaled hand sank fingers into the palm.
- The statue IS the runtime bind = the only honest audit stance; axis maps for manual dialing are
  POSE-DEPENDENT (a bone's +Y can read "thumb" hanging and "up" statued).
- Dual-write TRACK_POSE + poseLocal always (PLANCK's foot-IK memcpy).

## CBPC facts
- cbp.dll loads ALL CBPCollisionConfig*.txt, picks per-actor by [Options] Conditions (CK AND/OR/NOT;
  IsFemale/IsRaceName/ActorFormId/HasKeyword/…) + highest Priority. No equipment conditions natively.
- The `a | b` pipe in collision lines ≈ WEIGHT 0|100 interpolation (CBPC-wide convention), not capsule
  endpoints (07-05 correction; verify visually — trg v2 draws both tuples when they differ).
- CBPC zones are NiNode-space analytic spheres — never touch Havok; immune to the heel offset; the
  consolidation is by GENERATED matching configs, not by merging systems.

## NIF surgery (bake-time)
- pynifly: READS everything; CANNOT write existing bhkCapsuleShape ("SET type 19 NYI"); CAN create new
  shapes (add_shape — the 3-capsule hand plate proof); block IDs renumber on save (verify by reload);
  save() after a raw byte patch REVERTS it (raw patches go LAST).
- Byte layout bhkCapsuleShape (from skin-material sig uint32 591247106): material@0, bhkRadius@+4,
  [8]@+8, point1@+16, radius1@+28, point2@+32, radius2@+44 — patch ALL THREE radii. Scans UNALIGNED.
  Offsets never transfer between files. Append new blocks at END (absolute refs stay valid).
- Feet must keep BOX inertia (incline treadmill bug); drastic resizes need matched inertia/COM bakes;
  never rename/delete NiNodes (SMP resolves by name).
- Beast-female NIF pre-existing bug: its 5 tail bodies + constraints were DROPPED (fix before tails).

## ★ FINGER-BODY CTD (2026-07-09) — bhkBlendCollisionObject IS the ragdoll-body type
- **What happened**: added 4 `bhkBlendCollisionObject` capsule bodies (cloned from a beast TailBone,
  motionSystem 4, `constraintCount=0`) on `NPC R Finger10/20/30/40` to test finger collision.
  Result: **hard CTD on the FIRST Load3D of any female actor** (mannequin in Lakeview). Not the DLL
  (PPB.log clean); not Papyrus (native). Crash = `EXCEPTION_ACCESS_VIOLATION` read at
  `SkyrimVR.exe+0B08CD5  mov rax,[rsi]`, RSI=0, with a live `hkpPositionConstraintMotor*` in RDX/RBP/R13
  and `bhkBlendCollisionObject*` pointers sitting on the same stack (RSP+B8, RSP+338), `RootModifierList`
  in RDI, `BSFlattenedBoneTree` (NPC Root) nearby.
- **Root cause (CONFIRMED ~92%)**: `bhkBlendCollisionObject` **IS** the ragdoll/blend body type. Load3D
  walks every one of them to build the ragdoll `hkpPositionConstraintMotor`s. Every real ragdoll body
  carries `constraintCount=1` (COM is the root at cc=0); a finger body is cc=0 on a bone the **human
  behavior graph has no physics-bone mapping for** → the motor lookup returns null → deref → crash. The
  beast tail works ONLY because the beast behavior graph (skeletonbeast.hkx) *defines* the tail as a
  physics bone. **This REFUTES the finger recon's "membership is a pointer search, extra bodies are
  harmless" GO** (report 06 / 10 finger recon): a valid NIF that round-trips is NOT proof it loads.
- **RULE**: never put a `bhkBlendCollisionObject` on a bone that is not a ragdoll bone in the actor's
  own .hkx. A non-ragdoll follower collider MUST be a non-ragdoll collision object.
- **The in-file FIX template is `CharacterBumper`**: a `bhkSPCollisionObject` (shape phantom, layer 30,
  no constraint, no motionSystem) that coexists with the ragdoll precisely because it is NOT a ragdoll
  body. Three viable finger-collider paths, all non-ragdoll: (1) `bhkSPCollisionObject` phantom baked in
  the NIF (mirror CharacterBumper — needs raw block-append surgery; pynifly only makes blend objects);
  (2) runtime Havok body created + keyframed on the finger bone by PPB.dll (like HIGGS/CBPC — zero
  ragdoll-build risk, most control); (3) CBPC fingertip node (user's stated pref; integrates with the
  VRTouchEvents touch pipeline; no C++). Recon's flags-137/filter details are fine; only the *object
  class* was wrong.

## ★ EXTENDING A bhkListShape SLOT'S CHILD COUNT = FOUR coupled DLL edits (2026-07-09 head 10→14)
- Growing a slot's knob-addressable child count (e.g. head C1..C10 → C1..C14) is NOT one edit. **All four
  must change together or the new children silently no-op:**
  1. `Tuning.h` — the `g_tune.<slot>C[N]` array size (e.g. `headC[10]`→`headC[14]`).
  2. `Tuning.cpp` `kArrays[]` — the key-registration count (`{ "capHeadC", g_tune.headC, 10 }`→`14`) so
     `capHeadC11..14` map to keys at all.
  3. `Tuning.cpp` `CapFixChildKnobs(slot)` — the apply-loop upper bound (`case 3: return 11`→`return 15`,
     i.e. main + N).
  4. `Tuning.cpp` `CapFixChildSlot(slot,child,…)` — the `ChildPtr(arr, N, child, main)` bound (`10`→`14`).
- Miss #4 (the value-fetcher, easy to overlook — it's a SECOND function from the loop bound) and the loop
  REACHES the child but `CapFixChildSlot` returns false → `continue`. **Fingerprint in PPB.log: a
  `CapFix … <slot>.C<n> BEFORE …` line with NO matching `APPLIED <slot>.C<n>`** = CapFixChildSlot rejected
  it. (Exactly how head C11–C14 were caught: BEFORE logged at the seed, never APPLIED.)
- Also add the NIF children (bhkListShape 11→15 via `tools/ppb-scratch/extend_head_list.py` — appends seed
  capsules cloned from an existing child, reload-verifies count + body-count) and the `capHeadC11..14`
  tuning keys. Rebuild the DLL **with the game CLOSED** (the mod-folder DLL is locked while running).

## ★ SEED-ONLY list children need NO DLL rebuild — but extending a SHORT list can re-expose a shared-knob trap (2026-07-19, head 15→23)
Adding 8 head capsules to `skeleton_female.nif` (15→23, `extend_head_seeds8.py`) needed ZERO DLL work.
Two facts made it free, and one made it a footgun:
- **The four coupled edits are only for KNOB-addressable children.** If the new children are SEED-ONLY
  (driven per-NPC via npcCap, never a global cap*C* knob), skip them entirely: `ApplyListSlot` loops the
  actor's real `childInfo.size()` (≤`kMaxListChildren` 24) and a child with `!haveKnob && !haveNpc`
  hits `continue` = left as its buried NIF seed (CapFix.cpp:1409-1437, the 2026-07-17 npcCap-beyond-knob
  path). So the seeds are invisible for everyone until a per-NPC npcCap line positions them (Auri).
  (Same night: the head seeds DID get knobs after all — capHeadC 16→22, four coupled edits done, DLL
  3f41bc88 — because npcCap is kDataLoaded-parsed = a RESTART per change, useless for by-voice horn
  dialing. The workflow is now: dial via sculpt-gated capHeadC15..22 knobs → Yvanni bakes into her own
  draenei NIF / Auri transcribes into npcCap lines → knobs back to Enable 0. The no-rebuild claim was
  the seed-only interim, kept above because the PATTERN stays valid for future seed-only children.)
- **Seeds must be tiny buried rods, NOT verbatim clones of a real child.** The extend scripts clone
  child[1] verbatim because a live knob overwrites it within 1 s; a seed-only child is never overwritten,
  so a verbatim clone = duplicate visible skull geometry on every human. `extend_head_seeds8.py` clones
  child[1] (valid block + material 591247106) then OVERWRITES geometry to point1=(0,0,0) point2=(0,0,2u)
  r=0.5u (the proven Argonian dorsal-ridge seed). Never A==B.
- **⚠ FOOTGUN: extending a SHORT list to overlap a longer sibling's knob indices re-enables cross-race
  bleed.** The human head (15) was naturally immune to `capHeadC15/C16` (the beast ear/horn knobs) because
  it had no child 15/16 — the "per-race-by-existence" protection. Extending it to 23 gives it 15/16, so
  those dormant knobs CAN now land on humans. Harmless today (knobs `Enable 0`), but a future beast head
  re-dial MUST use a `sculpt` gate or every human woman sprouts ear/horn capsules. The natural-immunity
  trick only holds while the shorter list stays shorter.

## ★ THE 2026-07-10 NIGHT — four expensive lessons from the arm/scale/decimation saga

- **OFFLINE PIVOT MATH IS BANNED.** Constraint pivot copies live in the LIVE HAVOK BODY frame; NO
  offline frame reproduces them — not the NIF-node bind (the "three binds" rule strikes again), not
  the bhkRigidBody block's own stored world quat+translation. An "analytic" bake computed from NIF
  frames set all 34 copies and pre-stressed ALL 17 joints (copiesApart ~2u everywhere = the whole
  ragdoll visibly off). Only statue-time PIVBAKE lines (auto-seat, in-engine) are bake-grade.
- **A CONSTANT NIF CORRECTION CANNOT CANCEL A POSE-DEPENDENT RUNTIME ERROR.** The arm offset varied
  with pose (3.06u statued → 4.5u in other poses, dead clav-follow); baking a fixed 3.06u pivot shift
  produced "on sometimes, off sometimes" oscillation — mathematically guaranteed. Find the runtime
  writer first: uniform whole-chain displacement at ZERO strain = an ANCHOR/DRIVE error, not geometry.
- **LIVE-CAPTURED VALUES CARRY THE CALIBRATION NPC'S SCALE.** The statue medians were measured on
  scale-0.9514 Carmella → the baked numbers were HER numbers. Forcing them raw onto everyone
  (pivDescale) broke every other scale (Fianna 0.8 rode ~2u high). The engine's pivot×GetScale at
  load is the CORRECT mechanism — bake scale-1 base values (÷ the capture NPC's scale) and let it
  work. Applies to ANY value captured live on a scaled actor.
- **CONTACT DECIMATION (kIsDisabled on new points) IS UNSAFE AS A STANDING MODE.** Disabled points
  give no push-back → bodies interpenetrate deeper → manifolds bloat (10.4 → 18.4 pts/pair at 90%;
  town @50%: mean 152, max 1965) → Havok's contact pool exhausts → GLOBAL "all collision off" death
  (world-wide, heals only on world rebuild/reload). And it buys nothing: step time identical at 0%
  vs 90% — the cost is narrowphase DETECTION, not solving. Crowd perf work = filter layer (skip the
  pair before narrowphase), never contact starvation. `perfContactKeep 0`.
- **A WORLD-LEVEL hkpWorld LISTENER MUST IDENTITY-SCAN BEFORE ADDING ITSELF.** hkpWorld pointers get
  RECYCLED across interior/exterior transitions; "pointer changed" tracking re-registered the PPB
  contact listener on a world it never left (contactListeners 5 → 6) = every callback fired twice
  (doubled counters, decimation "50%" killed ~75%). Scan the listener array for your own pointer
  first (fixed in Diag.cpp EnsureContactListener, DLL 6cf54b5e).

## ★ THE 2026-07-11 SMP/TAIL ARC — lessons for all future SMP interop

- **SMP `<shared>` scope silently kills cross-actor collision.** `<shared>private</shared>` (copied
  from SOFTBODY's body_rigid, where self-collision is the point) scopes a shape to ITS OWN physics
  system — a hand collider marked private can NEVER touch another actor's tail, with zero warnings
  (FSMP parses `private` as valid; only truly unknown values warn "use default public"). Cross-actor
  colliders MUST be `<shared>public</shared>`. Cost: a full debug session. Corollary: the
  "unknown shared value" warnings in third-party XMLs are those files falling back to public.
- **A displacement sensor on a follower body also reads MOTION.** The push sensor (capsule
  displacement off its chord) cannot distinguish "hand pushed capsule" from "bone ran ahead of
  capsule" (PD servo lag) — at high force gain, walking became a visible backward DRAG on the tail
  ("stiff/extending where the capsules are", user-diagnosed). Deadzone alone was NOT enough (lag
  band overlaps light-touch band). THE fix = a CONTACT GATE: displacement publishes only while a
  HIGGS hand (<20u) / wielded weapon (<70u — body sits at the hilt) is near that chord. Generalize:
  any follower-displacement sensor needs a proximity/contact gate before amplification.
- **Force scale intuition for SMP pushes**: SMP world = plain GAME units; gravity on a 0.3-mass
  tail bone ≈ 200 force units; a deliberately-STIFF follower capsule displaces only 0.1–1u under
  contact, so the force constant must be HUGE (user-dialed 12000, clamp 32000; 40 was invisible).
  Stiff-feel and strong-sensor-signal are in tension — the gain compensates.
- **Slot-60 invisible equipped-item colliders: retired.** The auto-equip collider item CTD'd on
  glove equip (no crash log; cause undetermined — suspect slot/ARMA interaction). Safe attachment
  paths: VFS mesh/XML overlays (defaultBBPs), runtime follower bodies, or the FSMP PluginAPI. Do
  not resurrect the equipped-item route without a dedicated investigation.
- **FSMP 4.x reads `configs.json`, not configs.xml** — a configs.xml override is inert on 4.x.
  Also: mod versions move mid-session (FSMP went 3.3.0→4.0.1 under us; binary scans and source pins
  must be re-verified per session — pin source to the RELEASE TAG, and let the runtime
  getVersionInfo() handshake be the final authority).
- **DynamicHDT persistence is gated on the pex**: without Data/Scripts/DynamicHDT.pex the SKSE
  co-save record ('APFW') silently neither saves nor loads — enable the mod folder before relying
  on SwapPhysicsFile persist=true.
- **WebFetch's summarizer may refuse verbatim source reproduction** — for pinned-source work,
  `curl -sL raw.githubusercontent.com/<repo>/<TAG>/<path>` straight to disk; no middleman.

## FSMP rejects empty XML float attributes — the whole garment system is silently discarded (2026-07-12)
Symptom: an SMP outfit is completely rigid (rides the skeleton; brief jump on `smp reset` as systems rebuild), "like a heavy weight pulling it to one spot" (= bind pose). Cause: any empty float attribute (`<linearUpperLimit x="" y="0" z=""/>` in DX Necromancer robes.xml/robes3BA.xml) throws `not a float value` in `convertFloat` (XmlReader.cpp); the catch in `hdtSkyrimSystem.cpp readSystem` **returns nullptr — the ENTIRE physics system for that mesh is dropped**, not just the one constraint. Log signature: `[E] xml parse error - not a float value` — logged **with no filename**; identify the file from the `Found node ... creating bone` lines immediately above (bones parse before constraints; error at the constraint seam). History: tolerance died in FSMP commit `5edebd1a` ("perf: don't use setlocale", strtof→std::from_chars) first shipped **v3.4.0 (2026-06-29)**; ≤v3.2.1 read `""` as 0 (`strtof("")`=0, errno clean, endptr never checked — arbitrary garbage passed). SMP Flex descends from the tolerant lineage. FSMP's `smp report` XSD validator does NOT catch empty attrs (runtime stricter than validator). Fix: override copy with `"0"` (byte-faithful to the old parser's result) shipped in PPB at the same relative path (PPB priority 46 wins). NOTE: MO2 USVFS does not see files CREATED while the game runs — an override added mid-session needs a game restart.

## Keyframed follower bodies: spike-teleport MUST re-zero velocity, and player-riding bodies MUST be player-space-warped (2026-07-12)
The HandBox 4-box finger rig flew around during locomotion (worse at low FPS) and violently kicked whatever it touched (PLANCK shoves the charController back → screen jerk). TWO defects: (1) the spike path called `applyHardKeyFrame` (sets v = err×invDt) and then `setPosition` on a spike **without clearing the velocity** — the box overshot its own teleport target on the same pre-step (integration runs after the callback), got yanked back next frame, and contacts during those frames received the uncapped velocity as impulse. Correct order: teleport pos+rot FIRST, then re-apply the keyframe against the new pose (err≈0 → v≈0). (2) no player-space compensation — locomotion went into keyframe VELOCITY; at low FPS the physics sim advances less time than the frame, so v×simDt covers only part of the player's motion → persistent trail proportional to player speed. HIGGS is stable because `g_playerSpaceBodies` (higgs main.cpp SimulatePlayerSpace) moves its bodies POSITIONALLY by the player delta each frame; velocity carries only hand-relative motion. PPB now does both (`PlayerSpaceWarp` before `KeyframeAll`, one world-lock batch, deltas >50u skipped = teleport-class, recovered by the spike snap; prev-pos reset on load). RULE: any keyframed body that rides the player needs (a) positional warp for locomotion and (b) a spike path that leaves the body at v≈0.

## Conform loops must never re-measure through their own write (2026-07-13 sky-launch)
poseConformRoot v1 wrote the ragdoll drive's ROOT translation to a target measured from the LIVE node chain every conform. On PLANCK-driven actors the written root leaks back into the next measurement (the actor/model frame follows the driven ragdoll root), so the target integrates: NPCs accelerated into the sky within seconds. The per-bone conform never looped because parent-LOCAL translations are structurally anchored; only the root, measured against worldFromModel, was free. RULE: any per-frame corrective write must take its reference from (a) bind/reference-pose data, or (b) a one-shot capture from the pre-write epoch (v2: rootDelta captured on the first conform fire per ragdoll build, then applied as an additive CONSTANT — sanity-capped |d|<10u, latched in ConfCache). Corollary: ship every new conform-class write behind a live kill-switch knob and SOAK it (minutes, not seconds) before calling it validated — the knob is what saved this one.

## Never measure a PLANCK-loosened ragdoll (2026-07-13, the furniture poisoning)
Sitting, leaning, sleeping, and furniture idles put an NPC's ragdoll in the loosened ("green") state — bodies deliberately decoupled from the skeleton so they don't fight the furniture volume. ANY calibration sample taken there (span-scale, root capture, head trim) is pose-garbage that latches as the NPC's false "normal" (Carmella trimmed 1.5u low from a lean; Astanova's scale sampled on a chair read her ragdoll 2u behind). RULE: every sampler gates on `ObjectHold::ActorRagdollAttached()` (ragdoll state + sit/sleep state + occupied furniture) and RE-MEASURES on the stand-up edge — poisoned latches must self-heal, not persist. Corollary: the furniture detach itself is CORRECT and must stay — a seated pose co-occupies the furniture volume by design; accurate bodies don't change that, and live collision there would eject the NPC from the seat.

## The engine LIES about an NPC's scale — measure the ragdoll, don't trust records (2026-07-13)
`GetScale()`, Height/Weight records, and the drive pose are all unreliable proxies for where the ragdoll
actually IS. Two NPCs with the SAME record scale (0.95) got ragdolls the engine built at DIFFERENT
scales (1.0 vs 0.95 — spawn/build timing), so scale-1-dialed capsules fit one and missed the other by
5% ("whole body too small, feet off the floor"), and it SURVIVES disable/enable. Fix (Report 16): a 1 Hz
span-ratio tape measure (bodySpan/nodeSpan × root scale), 10-sample mean latched per FormID, replaces
every record-based guess. RULE: for any per-actor physics fit, measure the live geometry and fit to the
measurement — never compute from record/API scale values. (Corollary already known: the engine scales
ragdoll SHAPES by BASE scale at build; `CapScaleOf(GetScale)` covers the refscale layer — the measured
system supersedes both.) Full system: Report 16.

## Every latched calibration needs EDGE invalidation, and must gate on validity (2026-07-13)
Any per-NPC value latched from a measurement (scale, root offset, head trim) becomes WRONG the moment
the actor's state changes, and a stale latch becomes the NPC's permanent false "normal". Two failure
classes bit us: (1) measuring a PLANCK-loosened ragdoll (furniture/sit/sleep/ragdoll — see the entry
above); (2) measuring under one heel state then the state changing (shoe swap, late/failed NiOverride
arming, the `hf` toggle — a capture with the heel drive-bias ON is stale when it's OFF). RULE: gate every
sampler on `ActorRagdollAttached()` AND enumerate every state that changes the truth (pose-detach,
ragdoll rebuild, scale change, heel offset, bias toggle) — each transition invalidates the latch and
forces a clean re-measure, so poisoned latches self-heal instead of persisting. Full edge table: Report 16 §4c.

## Delegated refactors silently drop unlisted features — grep-verify consumers after (2026-07-13)
The multi-rig refactor agent faithfully executed its transformation contract — and dropped the finger
touch write-back, because the contract enumerated structures/knobs but not that feature, and the rewrite
of DriveRig didn't carry it. The compile stayed green (the knob remained declared, consumer gone), the
feature died silently, and only a later audit's "does every knob have a caller" grep caught it. RULES:
(1) a delegation contract for a rewrite must enumerate EVERY behavior in the rewritten region, not just
the structural transformation; (2) after any delegated refactor, grep-verify that each feature's CONSUMER
still exists (knob -> caller, export -> call site); (3) an audit's cheapest high-value check is exactly
that consumer grep.

## Three lessons from the perf-fix review (2026-07-13, build 5f4b0ce8)
The perf batch (single-pass bone resolver, probe stagger, mtime precheck, world-epoch UAF guard, log
demotion) passed a 4-lens adversarial review with these confirmed defects — all now fixed, all a
reusable CLASS:

1. **A "did it change?" precheck must commit its new state AFTER the guarded action SUCCEEDS, not
   before.** The tuning-file mtime precheck stored `s_mtime = newMtime` and then called the parser.
   On Windows `last_write_time` reads attributes (never blocked by share modes) while `ifstream::open`
   needs read access — so a tick during an editor/AV/sync lock STATS the new mtime but FAILS to open,
   commits the mtime anyway, and every later tick early-returns `mtime==s_mtime` → the edit is swallowed
   FOREVER (the live-dial loop goes dead, zero log evidence). Fix: make the guarded action report
   success (return the opened path) and commit the precheck state only on success; on failure leave it
   untouched so the next tick retries. General rule: **cache-invalidation keys are committed on the
   commit of the work they gate, never on the cheap detection that precedes it.**

2. **A generation/epoch guard keyed on CELL change is WRONG in exteriors — worldspace cells share one
   bhkWorld.** The world-epoch UAF guard bumped `g_worldEpoch` on every player parent-cell change to
   invalidate stale `bhkWorld*` fallbacks (freed-then-recycled world pointer). But every 4096u exterior
   cell-border crossing changes the cell while the SAME worldspace bhkWorld stays live → the guard
   refused the orphan-capsule removal fallback in its primary scenario. Fix: don't key safety on
   "cell changed"; RE-STAMP the epoch on every frame the rig is verified live in its world (a fresh
   `cell->GetbhkWorld() == rig.bhkWorld` compare THIS frame is proof), so the guard reflects
   "recently-confirmed-live" not "same cell." A rig whose actor left freezes at its last epoch and a
   later genuine world change still invalidates it (safe direction).

3. **kNewGame is a SEPARATE teardown path from kPreLoadGame.** SKSE dispatches `kNewGame` (not
   `kPreLoadGame`) when starting New Game from the menu. Any FormID-keyed latch/cache cleared only on
   kPreLoadGame survives from a previously-played save into the new game (persistent + recycled FF
   FormIDs inherit stale state). Every ClearOnLoad-class teardown must handle BOTH messages.

## poseConformRoot RETIRED — a "correction" measured on the TARGET, not the result (2026-07-13)
`poseConformRoot` was built to cancel a "~4.3u uniform root offset" a 5-NPC census had measured. It
shipped ON and quietly did HARM: on the scale-1 master (Lydia) it floated her ~2u off the floor and
knocked her Havok joints **3.76u mean / 8.02u max** off the XP32 nodes. Turning it OFF dropped her to
the floor and snapped the joints ON at **0.21u / 0.30u** (live jtrack, both states). Three reusable
lessons, all of which cost real debugging this session:
1. **Measure the RESULT, not the drive target.** The census's ~4.3u was measured on the drive TARGET
   (poseLocalSpace COM), not on where the PD-driven bodies actually settle. PLANCK's drive + the
   per-bone conform land the bodies on the nodes regardless of the root target's height. A "gap" in the
   command is not a gap in the outcome — always verify against the settled bodies (jtrack), never the
   input pose. This is the same class as the earlier `Actor::IsDead()` and body-origin-vs-node traps.
2. **A capture-once constant is only valid if captured in the RIGHT state.** poseConformRoot captures
   its delta ONCE; it's ≈0 only in bind pose. Statued Carmella captured ≈0 (harmless → "verified"),
   live Lydia captured 4.78u (spurious lift). Any capture-once calibration MUST gate its capture on the
   reference state (bind/statue) or it silently latches pose noise as a constant. Prefer measuring the
   already-settled result over capturing an input delta.
## Runtime Havok bodies across cell/world swaps: HOLD A STRONG `NiPointer<bhkWorld>` (2026-07-14, the orphan-capsule saga, DLL d5fdafce)
The always-on rig capsules (finger/tail/hair/dress) FLOATED and piled up (15 hair + 11 dress bodies per
exterior cell crossing) because each rig stored its world as a **compare-only `void*`** it refused to
dereference at teardown ("can't prove it's alive"), so on a cell swap the bodies were left in the
swapped-away world. I burned **THREE** deferred-removal attempts (park-and-sweep, worldspace-epoch,
exterior-exit-epoch) — each an adversarial-review-caught **use-after-free**, because the engine FREES the
Havok world on every cell/area change (KNOWLEDGEBASE.md:353) and a body parked from a freed world dangles.
**THE FIX (research → HIGGS precedent, verified clean):** give the rig a **strong `RE::NiPointer<RE::bhkWorld>
heldWorld`**. bhkWorld is NiRefObject-derived, so the held ref keeps the world ALIVE through any swap →
every teardown deterministically locks it and RemoveEntity, then releases. This **deleted** all the
scaffolding (both epochs, the pending list, ProcessPendingRemoval, the player-world fallback) and fixed
every rig type at once. Reusable rules:
1. **Runtime bodies you add to a bhkWorld must hold a strong ref to that world** (HIGGS/PLANCK both do:
   `NiPointer<bhkWorld>`). A compare-only pointer forces the orphan-vs-UAF dilemma; the strong ref dissolves it.
2. **The "orphan them, accept a bounded leak" folk-wisdom is WRONG for the persistent exterior worldspace**
   — it's never freed on cell crossings, so orphans accumulate forever (float + drag SMP). Only interior
   worlds self-clean, and even they are cleaner with the held ref.
3. **Never defer-then-dereference a body across a world-swap.** After the swap it may be freed; a still-mapped
   freed pointer passes every `IsLikelyPointer` value check. Remove it NOW while you hold its world alive.
4. **When you keep hitting UAF variants patching the same mechanism, the mechanism is wrong — stop and
   research the shipped precedent (HIGGS/PLANCK source in tools/_research).** The right answer was ~15 lines
   and *deleted* code; the wrong path was three builds of growing complexity.

## Measure the JOINTS/NODES, not body ORIGINS — and fix the math, don't disable the feature (2026-07-14)
The measured-scale sampler read the scale-1 master (Lydia) at **1.033** and inflated her. Two lessons:
1. **A ragdoll body's ORIGIN sits at its PARENT joint, not on its bone node** (~2.5u off on the head).
   The sampler measured `bodySpan/nodeSpan` using body origins → a systematic ~3% baseline even for a
   perfectly scale-1.0 NPC. FIX = measure XP32 **node** distances (COM→head, COM→calf), reference-anchored
   on the master (her spacing = "scale 1") → the master reads a true 1.0. When a "scale" measurement gives
   a non-1.0 for a KNOWN-correct reference, you're measuring the wrong POINTS (pivot/node, not body center)
   — same family as poseConformRoot's "measure the settled bodies, not the drive target."
2. **When a feature over-corrects, FIX ITS MATH — don't silently disable it.** I turned measured-scale OFF
   because it inflated Lydia; the instruction had been to fix the math, and disabling threw out the safety
   net that catches genuinely mis-scaled NPCs (Carmella, joints 1.34u off). Disabling a system to dodge a
   bug is a regression. Full as-built: Report 18.

3. **A knob defaulting 0 in code can still ship ON via a leftover tuning-file line.** The compiled
   default was 0 the whole time; a stale `poseConformRoot 1` in the deployed PPB_tuning.txt is what
   forced it on. When a "default off" feature is misbehaving in-game, CHECK THE TUNING FILE — the file
   overrides the compiled default. Ship the tuning file matching the compiled defaults for anything
   retired, and mark retired knobs LOUDLY in-code (not just "off pending validation") so no one flips
   the file back to 1 to "fix" a symptom.

## Predicting a hard-keyframed body from VELOCITY × this-frame-dt false-fires on every frame-time change — predict the POSITION (2026-07-17, review-caught pre-deploy)
A hard keyframe's contract is "land exactly on posCmd after the step" — the commanded POSITION is a
dt-free, jitter-immune prediction of an unobstructed body. Reconstructing it as lastCmdVel × dt_now
instead errs by cmdVel × |dt_now − dt_prev| with ZERO physical contact: exact at steady framerate,
but a VR reprojection flip (90↔45 = 11 ms Δdt), a load hitch, or an sgtm change pierces any sane
threshold at ordinary chord speeds — the contact detector then false-latches precisely in the jittery
sessions it was built for. All three review lenses independently converged on this. Corollaries from
the same review: (a) the prediction is only valid while the command stays under the BODY's own
m_maxLinearVelocity (10 m/s on light garment bodies) — a solver-truncated command undershoots and
false-latches every fast frame; (b) clamp every tuning float BEFORE an (int) cast (huge/NaN → UB →
INT_MIN → a time_point ~25 days in the past → the gate silently kills the whole feature); (c) never
log a BSFixedString's raw pointer after reassigning it — for a custom race's unique path you hold
the pool's only reference and the log reads freed memory.

## HIGGS's collision-filter callback runs on a MISALIGNED STACK — one float in the C++ = movaps CTD (2026-07-16, PROVEN twice)
The comparator trampoline (HIGGS Xbyak, written onto the CompareFilterInfo function HEAD) enters our
callback at RSP ≡ 0 mod 16 — 8 off the ABI. ANY float work in `FilterDecision` (or anything it calls)
can make MSVC park a value in a nonvolatile xmm register, whose prologue save is an ALIGNED
`movaps [rsp+N], xmm6` → #GP → CTD. Crash signature: `PPB.dll+... movaps [rsp+0x20], xmm6`, "tried to
read 0xFFFFFFFFFFFFFFFF" (= alignment fault, NOT a pointer), RSP mod 16 == 8, higgs_vr.dll
hooks.cpp:701 in the stack. **The old "never LOG from the filter" rule was too narrow — the real rule
is: collision-thread callbacks do PURE INTEGER work. No floats, no SIMD, no logging, no allocation.**
Fix pattern: sample every float knob on the MAIN thread into ONE relaxed uint32 bitfield atomic
(`g_filterKnobs` / `RefreshFilterKnobs`, refreshed from OnFrame + top of OnPreDrive so it's hot before
any body exists); the callback reads the integer. HandBox (`g_boxPart`) and PerfSys (`g_cbSelfThigh`)
already did this — NpcFinger's 3 new `npcFingerVs*` floats were the FIRST floats ever inlined there
and alone tipped the codegen. Source review CANNOT catch this class (the C++ looks innocent) — VERIFY
IN THE BINARY: dumpbin /DISASM, anchor the callback via the unique vtable-slot-20 `call [r8+0A0h]`
registration (no PDB ships), assert zero movaps/xmm in the whole reachable tree, cross-check
/UNWINDINFO for SAVE_XMM128, and ALWAYS run a positive control (the same grep must find spills
elsewhere in the DLL or the detector is broken). Only the COMPARATOR is exposed — HIGGS's other
callbacks (PrePhysicsStep via Write5Call, PostVrikPostHiggs) have correct stacks. Structural,
permanent, unfixable from our side. Full record: Report 15 + KNOWLEDGEBASE 2026-07-16.

## A near-massless dynamic body + a stiff servo constraint + a HIGGS grab = Havok solver NaN → ENGINE FREEZE (2026-07-13)
The garment-weight fix (Report 18 §3) dropped tail/hair/dress rig capsules to `npcGarmentMassKg` **0.05 kg**
to stop 15 heavy capsules dragging the SMP cloth down — correct for the weight, but it introduced a HARD
ENGINE FREEZE (not a CTD — a hang). Repro: the DX Necromancer **lower skirt** chords hang at the ankles;
when the player reaches for the NPC's **boot**, HIGGS highlights/grabs the skirt capsule body sitting there,
and the engine freezes. Mechanism: these are DYNAMIC bodies held to a keyframed bone-chord target by a stiff
motorized constraint AND pushed by FSMP. HIGGS adds its own grab constraint pulling hard on a **0.05 kg**
body → a = F/m explodes as m→0 → position/velocity go NaN → Havok's solver hangs iterating on NaN → freeze.
It "didn't do that before" because at the old 4 kg mass the body was heavy enough to stay numerically stable
under the grab. Reusable rules:
1. **Any dynamic body a player can GRAB (HIGGS/PLANCK) needs a grab-stable mass floor** — sub-~0.5 kg under
   a stiff constraint is a solver-blowup risk the moment a second hard constraint (the grab) attaches. Mass
   that's "light enough for cloth" can be "too light to be grabbed." These two requirements FIGHT.
2. **The clean fix is usually to make touch/push-surface bodies NON-GRABBABLE, not to re-heavy them** — they
   exist to collide/push cloth, not to be picked up; a no-grab collision layer keeps the light mass AND kills
   the freeze. (Tail is the one judgement call — a player may WANT to grab a tail; decide per garment.)
3. **When a mass/scale change is the last thing you shipped and a NEW freeze appears, suspect the numerics
   of every constraint that body participates in** — not just the feature you were fixing. A weight fix and a
   grab-stability regression look unrelated until you write F = ma with a tiny m.
Immediate action (user directive): removed the DX Necromancer dress rig entirely (kDressPairs + tbl-4
tune/selector/probe; build `ac3ea9fe`), which deletes the grabbed body. NOTE the wig (0.05 kg, kept) shares
the mechanism — grabbing the NPC's HAIR can still freeze until the general fix (rule 2) lands. Report 18 §5.

---

## ReShape / mesh-sampling pitfalls (2026-07-18/19) — full system in Report 21

1. **VR body meshes are STATIC `BSTriShape`, not `BSDynamicTriShape`.** Morphs regenerate the raw
   render vertex buffer; `dynamicData` is SE-facegen-only (this is why the head measure read
   `vertDepth=0.00` all project). Read `rendererData->rawVertexData`, or for skinned meshes the
   **skin partition** `partitions[0].buffData->rawVertexData`.
2. **A skinned mesh's geometry `vertexCount` is GARBAGE** — use `NiSkinPartition::vertexCount` for
   the true whole-mesh count.
3. **Vertex descriptors cannot be trusted blindly** — a declared-half decode produced ±65 000-unit
   garbage. Self-calibrate: probe spread verts under each (stride, precision) candidate, accept the
   first body-PLAUSIBLE decode (finite, |coord|<500, real z-spread).
4. **RaceMenu overlay proxies** (`FakeOverlay`, `Body [Ovl0]`) are body-shaped shapes sharing ONE
   template buffer → EVERY NPC reads byte-identical girths (looks plausible, is fatal). Reject by
   name ("overlay"/"ovl") AND by transform (parked >200u from the actor root). Caught only because
   the by-eye marker showed Lydia's number on the Redguard.
5. **Numbers lie — verify bands with the eye (markers), one region at a time.** A wrong mesh, junk
   vertices, a chest band eating ARM verts, and a saturated cup all produced sane-LOOKING girths.
   The marker protocol (Report 21 §7) is the ground truth, not the log.
6. **Girth must be MAX / near-max radial distance, never averages/sums/counts** — those drift with
   where the modder concentrated vertices (density). Max is density-immune.
7. **`meshShapeDump` re-MEASURES but does not re-DRESS** (the old radius-only era) — capsule
   geometry re-applied only on a cap* VALUE change. The freshLatch re-dress (a new latch falls
   through to the apply loop) fixed it at the root; during that era the pulse was dump + a
   `capComC20R` nudge.
8. **Shape must be deviation from HER OWN base body, not a global neutral.** Base bodies differ
   (3BA +15-19% mid-torso vs Softbody) — a global neutral reads every 3BA NPC as falsely chubby.
   Per-body-type neutrals keyed by vertex-count fingerprint (Report 21 §4). A per-race "bias" shim
   was tried and RETIRED — it hid the real base-offset cause. **Universal law: if an NPC misfits,
   the math is wrong, never add a per-race/per-NPC shape shim.**
9. **Measured gains vs slider nets are two regimes** — measured gains (0.5–1.0) on slider-sized
   nets (±2) double a capsule. Keep the slider fallback on its own fixed constants (Report 21 §8).
   ⚠ Collapsing bands for a marker check drops the other regions to that fallback — expect a jolt.
10. **ReShape ≡ ReScale (the model correction):** shape must scale capsule ENDPOINTS (lateral, from
    the havok joint) AND radius, exactly like skeleton scale — never radius-only (a ring at neutral
    distance can't be pulled in by radius alone; that was the root of every "M'rissi won't shrink").
    Height/along-bone never scale with shape; long bones radius-only; COM ring never.
10b. **★ CTD 2026-07-20 (crash-2026-07-20-16-06-36): a facegen BSDynamicTriShape's vertexCount is
    GARBAGE — and a partial-probe validator does not license a full-buffer walk.** The body-mesh
    candidate scan read Auri's "auriantlers" (facegen dynamic shape) whose vertexCount decoded as
    64,399; PlausibleDecode samples only spread verts near the buffer head, so it passed, and the
    full bbox pass walked ~1MB past the real allocation → EXCEPTION_ACCESS_VIOLATION (PPB.dll+
    0x40914, movss on a wild base). Latent for days; a live neutral-knob edit re-measured every
    NPC and crossed the cliff. FIX: BSDynamicTriShape candidacy DELETED from ResolveMeshSource —
    VR bodies are NEVER dynamic (proven 07-18); only facegen is, and it must never be scanned.
    RULES: (a) a probe that validates the head of a buffer says nothing about its LENGTH — never
    let it authorize a full walk sized by an untrusted count; (b) counts from geometry runtime
    data are trustworthy ONLY on the paths where a loader-truth source exists (NiSkinPartition::
    vertexCount for skinned; the geometry's own count for plain statics); facegen has neither.
10c. **Runtime NiSkinData transforms are UNPOPULATED for BSTriShape skinning — a compiled-clean
    read of them returns nan/±1e37/zeros (2026-07-20, breastZrel v1).** The engine keeps modern
    bind matrices in GPU/partition data; legacy NiSkinData::BoneData::skinToBone exists in memory
    but is never filled. My SkinBoneBindZ read it → per-NPC garbage (Yvanni nan, Ulliss −2.4e37,
    Auri/M'rissi zeros). The bones POINTER array + counts ARE valid (TailChainSkinned uses them);
    only the transforms are dead. THE RELIABLE SOURCE for bind positions: our own skeleton NIFs —
    every PPB female skeleton shares an identical chain, so a bone's bind Z is ONE offline-derived
    compile-time constant (kSpine2BindZ 91.2488, derive_neutrals.py). Corollary: after ANY new
    runtime data-source read, eyeball the actual logged VALUES before building on them — the v1
    garbage was visible in the very first MESHGIRTH line.
11. **Every body measure must be NORMALIZED or ANCHOR-RELATIVE — an absolute height breaks on the
    first non-human body (2026-07-20, Yvanni's floating breasts).** breastZ was "height above the
    mesh bottom"; her hoofed/digitigrade mesh bottom sits ~14u lower → breast capsules floated
    +13.7u while every GIRTH read fine (bands are bbox FRACTIONS — they self-normalize; the one
    absolute measure was the one that broke). Fix: measure relative to the bone the capsules hang
    from (spine2 bind pos via the mesh's own NiSkinData skinToBone — same space as the verts, so
    scale/pose/leg-length invariant). Audit rule: any NEW measure must state its reference frame,
    and "above the bottom / above the ground" is never an acceptable one. Semantics changes to a
    captured neutral ZERO the stored neutrals (0 = uncaptured → feature self-disables) and re-arm
    via the capture protocol — never reinterpret old numbers under new semantics.
12. **A skin-bind gate on ONE bone of a chain is not proof of the chain — jewelry/partial meshes
    bind chain ROOTS (2026-07-20, the Yvanni phantom tail).** Her tail RING weighted vanilla
    TailBone01/02 (165/103 real verts) and passed the root-bone TailChainSkinned → a straight-out
    phantom rig on the dormant chain. Real tails weight the WHOLE chain (CrbTail: 01..05 all
    330-595 verts). Gate chain features on the TIP (or root AND tip), never the root alone. And
    re-verify "X binds Y" memories against the current WINNING mesh before relying on them — the
    M'rissi vanilla-chain "fact" predated the Fluffy replacer winning her tail model and was false.
13. **A density/plausibility guard needs an identity bypass — and a diagnostic readout must print
    what was APPLIED, not the raw product (2026-07-19, the Auri "ReShape is broken" scare).** The
    dressed-mesh guard (torso bands ≥150 verts) rejected a REAL naked body: petite proportions put
    117 verts in the 2u waist band → whole-actor slider fallback → base capsules + the clamped
    4.26u butt shove — reading exactly like a broken ReShape. Two rules: (a) when a cheap identity
    check exists (body vertex-count fingerprint), it beats any density heuristic — bypass the guard
    for known identities and keep the heuristic only for unknowns; (b) the log printed
    "butt back=17.96u" while the apply path clamped to 4.26u — a readout that skips the apply-path
    clamps sends the debugger chasing a phantom. Print effective values, tag the regime
    (fallback vs measured), and include the identity (vc=) in every rejection line.

## npcCap's first REAL use exposed base-vs-reference + spaced-plugin (2026-07-20) — full context Report 10 §10
Two latent bugs surfaced the first time a per-NPC capsule override actually DROVE an NPC (Yvanni's hooves/
breasts, Auri's antlers — npcCap had only ever *parsed* before, never matched a live actor):
1. **A per-NPC value keyed on the BASE FormID must be looked up by the actor's BASE — never
   `actor->GetFormID()`.** GetFormID() on a placed/spawned actor is the REFERENCE FormID (FF00xxxx, a
   placeatme clone) and NEVER equals the base the store was keyed on (`LookupForm` → base NPC_). The lookup
   missed every actor, silently. The SKELMAP receipt logging "10 npcCap override(s)" (parse OK) MASKED it.
   FIX: `actor->GetActorBase()->GetFormID()`. Reusable: **parse-success ≠ apply-success** — for any
   FormID-keyed per-NPC data that "parses but does nothing," base-vs-reference is the first suspect; verify
   the KEY SPACE matches on both the store side and the lookup side.
2. **Any config token that can hold a plugin filename must tolerate SPACES.** `"Yvanni Follower.esp"` broke
   the `>>` read. FIX: accumulate tokens until the ref ends in `.esp/.esm/.esl` — the exact fix the `race`
   parser already carried; the npcCap parser predated that lesson and repeated it. When you fix a
   whitespace-in-token bug in one parser, grep for every OTHER parser reading the same token class.

## ★★ THE SAME BUFFER-OVERRUN CTD TWICE — I fixed the SYMPTOM, not the RULE I had just written (2026-07-21, Solitude)
`EXCEPTION_ACCESS_VIOLATION at PPB.dll+004CCF6  movss xmm1,[r10+r9+0x04]`, whole stack inside PPB,
`+004D26F` repeating 14x = the FindBodyMesh scene-graph recursion. **Byte-for-byte the same class as
crash-2026-07-20-16-06-36** (entry 10b) — for which I had ALREADY written this rule:
> *a probe that validates the HEAD of a buffer says nothing about its LENGTH — never let it authorize
> a full walk sized by an untrusted count.*
Then I "fixed" it by banning **BSDynamicTriShape**, the one shape type that happened to trigger it. A
plain **STATIC BSTriShape** in Solitude reported a `vertexCount` larger than its real allocation and the
bbox pass walked off the end. **The rule was right; the fix was narrower than the rule.**
- **THE ACTUAL FIX (`ExtentReadable`, CapFix.cpp)**: `VirtualQuery` the ENTIRE `n*stride` span before
  ANY walk — every byte must be `MEM_COMMIT` and not `PAGE_NOACCESS`/`PAGE_GUARD`. If the OS says the
  memory isn't there, the count was a lie and the mesh is rejected. One query per contiguous region,
  per candidate mesh, at latch only. Closes the CLASS: no count from geometry runtime data is trusted
  again (`NiSkinPartition::vertexCount` included — it is *more* reliable, not *reliable*).
- **THE META-LESSON, and the expensive one**: when a postmortem yields a RULE, the fix must discharge
  the RULE, not the instance. Ask "what ELSE does this rule forbid that I am still doing?" Banning one
  shape type left every other path still authorized to walk an untrusted length.
- Emergency mitigation for any mesh-scan CTD: **`meshShape 0`** (hot, no restart) gates the scan for
  BOTH the v2 bone sampler and the legacy sampler; ReScale/heel/tails/skeletons keep working.
- WARNING: the pre-crash `PPB.log` was LOST to a relaunch before it could be read (truncate-on-launch),
  so the offending mesh was never identified. **ARCHIVE PPB.log BEFORE RELAUNCHING after any crash.**

## ★★★ THE 2026-07-22 MEASUREMENT SAGA — five ways to locate anatomy, four of them wrong
Chasing "where is the nipple" cost a full session and ~8 builds. Every failure shares ONE root:
**inferring a location from geometry instead of addressing it.** The fix (UV landmarks) is in
Report 04. These are the traps, each of which LOOKED reasonable and produced plausible numbers:

1. **Radius from the ring CENTROID** — the centroid CHASES the protruding mass, so a BIGGER breast
   reads SMALLER. Report 04 §3 already said this; I re-introduced it anyway and it inverted Yvanni
   vs Sofia. *If a measure's reference moves with the thing being measured, it is not a measure.*
2. **Protrusion off the OPPOSITE-side anchor** (rear anchor for breast, front for butt) — imports
   TORSO DEPTH. A deeper 3BA ribcage read as "bigger breasts"; the butt read 12.78-12.91 on four
   wildly different bodies (the curvy one BELOW the neutral). Anchor on the SAME surface (wall vs
   bulge), never across the body.
3. **"Front-most N% of the bone-weighted vert set"** — the SET is a different anatomical region per
   mesh: 3BA gives Spine2 **6882** verts (incl. shoulders/upper back), Softbody **2652**. The number
   moved because the SAMPLE moved. Marker proof: the "nipple" sat on Lydia's **left collarbone**, and
   Lydia+Aela produced byte-identical coordinates on different meshes.
4. **Absolute mound position** — same sampling flaw; Aela measured on CBBE vs 3BA gave two different
   "absolute" positions for the same woman.
5. **UV landmark** — works. See Report 04.

**THE UNIFYING SYMPTOM:** every failed measure separated **BASE MESH** (Softbody vs 3BA, ~4.7u apart)
rather than **woman from woman**. If your metric clusters by mesh family, it is measuring the mesh.

## ★★ THE VERIFICATION TOOL WAS DISCONNECTED FOR THE WHOLE OF v2 (2026-07-21/22)
`meshMarkers` places ghost capsules on the vertices a measure picked — Report 04 §7 calls it ground
truth: *"numbers lie; the eye is ground truth."* It is fed by the LEGACY sampler's `wpos[]`. The
bone-anchored v2 path emitted **ZERO markers** and nobody noticed for its entire life. I spent three
builds reasoning about numbers with the verifier switched off; **one look found the bug in a minute**
(the marker was on a collarbone). RULES: (a) when you add a measurement path, WIRE ITS VERIFIER IN THE
SAME COMMIT; (b) before debating what a number means, place a marker and LOOK.

## ★★ USE A REAL CAPSULE AS THE PROBE, NOT A GHOST (2026-07-21)
Ghost markers are keyframed to a **fixed world position at latch** — they do not follow the body, and
a statued NPC still drifts 4-5u, which is the same order as the corrections being dialled. Instead
dial an ACTUAL capsule that rides the target joint (`capSpine2C11`): it sways with her, and its A/B
values ARE bone-local coordinates — exactly the number wanted. Freeze `meshShape 0` first so ReShape
does not modify the probe, and SAVE THE ORIGINAL VALUES (it is a live gameplay capsule).
⚠ Marker/probe placement must use the **GEOMETRY's** world frame, not the actor root's — using the
root put the ghost 5u in front of her neck while the coordinates were correct (a *rendering* bug
masquerading as a measurement bug; it cost a full verification cycle).

## ★ A SELF-CALIBRATING PROBE MUST REJECT DEGENERATE DATA, NOT JUST OUT-OF-RANGE DATA (2026-07-21)
The UV-offset detector accepted **offset 12 — the zero PAD** — because `0.0` passes an "is it in
[0,1]?" test, yielding `UV=(0.0000,0.0000)` on every body. Real UVs are at offset 16. FIX: demand
VARIANCE (spread > 0.05 on both axes), not just plausible range. Same family as PlausibleDecode:
a validator that only bounds values will happily bless a buffer of zeros.

---

## A diagnostic that describes the wrong stage is worse than no diagnostic (2026-07-23)

Three log lines in PPB reported something other than what the apply path did. Each cost real time:

1. **`fallback-shift ... (inactive — measured shifts win)`** — the suffix keyed on `meshSampled`
   alone while the apply path's real gate was `MeshShiftDeltas()`, which also demanded
   `bodyScale && meshShape`. With `meshShape 0` the fallback was the *only* active path and the
   log called it inactive. Capsules dialled as "frozen" carried up to 1.6u of hidden offset.
2. **`region=breasts ratio=1.163`** — not the value applied to the radius. Measured from the
   `APPLIED` line: 1.046 for Sofia, **0.852** for M'rissi (below 1 — it *shrinks*). Trusting the
   logged figure produced a capsule 0.90 too fat, rejected on sight.
3. **`BONEGIRTH ... cup=`** — the legacy mound-based cup, still printed next to the UV values.

**Rule:** when a value drives geometry, read it from the line that logs the FINAL WRITE, never
from a summary line written to describe an earlier stage. In PPB that is
`logVerbose 1` → `CapFix <id> APPLIED <slot>.C<n> A=[..] B=[..] r=..`. It only fires on a fresh
latch, so an empty grep means "no latch happened", not "no data".

**Corollary — delete dead paths, don't disable them.** A disabled path keeps its log lines, and
those lines keep describing a system that is no longer running.

## A reachable code path is not proof it ran (2026-07-23)

I read the source, established that the slider fallback *could* run during a dial session, and
"corrected" a hand-dialled anchor by that arithmetic. The user rejected it by eye immediately.
I inferred a second theory and it was rejected again.

The position half of the conversion turned out to be right the whole time — the radius half was
wrong, because it used a ratio from a log line that reported a different number than the code
applied. **Two errors in one change made the eye test uninterpretable**, which is its own lesson:
change one variable per test. Only measuring the actual written capsule settled it.

**Rule:** never rewrite a hand-dialled, eye-confirmed value from inference about code paths. The
eye is the authority on what was on screen. Measure, or leave it alone.

## Compaction mid-calibration (2026-07-23)

Context compacted during a dialling session and I twice rebuilt the constants wrong from the
summary — treating deltas as absolute positions, and re-fitting a channel the user had already
given a rule for (lateral = 0.25 x forward). Both were confidently wrong and both wasted a round.

**Rule:** after a compaction, before touching any calibrated value, read the actual `.jsonl`
transcript. Dialled numbers and the user's own rules do not survive summarisation intact. Write
the anchors, the constants AND their provenance to a scratch file *while dialling*, not after.

## The silent-failure quartet (2026-07-24, disk-facegen pipeline)

Four consecutive pulses produced NOTHING in the log, each from a different quiet exit in the
same feature: (1) `GetFile(0)` returned null → loader exited unlogged; (2) the caller's
null-guard skipped the loader silently; (3) `NiBinaryStream::read` returns BOOL, not bytes —
`true` treated as "1 byte" collapsed a 2MB file to ~32 bytes; (4) the parser's dozen bounds
checks all returned without a word. Each fix exposed the next.

**Rules distilled:**
- Every exit path of a loader/parser LOGS its stage. A quiet `return` costs a full
  close-relaunch-pulse cycle (~10 min) to even notice.
- Never guess an API signature twice: after the first misfire, READ THE HEADER
  (CommonLibVR include path is in the vcxproj). The bool-vs-bytes bug was one grep away.
- Resolve defining plugins by LOAD INDEX (formID top byte → data handler), not by
  form-API calls that may return null.
- `tell()`-delta accounting is the correct slurp pattern for NiBinaryStream.

## The haunted-floor seizure: placed marker bodies vs driven rigs (2026-07-25)

A re-latch auto-placed the 11 UV-landmark ghost markers (meshMarkers was still armed from a
mapping session). The four LIMB landmarks put static bodies INSIDE zones where the ragdoll,
finger rig and garment capsules live — and the markers ride the HIGGS layer, which our own
filters treat as "player hand," so every driven capsule fought immovable statues embedded in
the actor's flesh. Whole-body seizure, engine freezes, Papyrus VM dumps. The log shows
placement -> distress in TWO MILLISECONDS (01:59:59.717 MESHMARK -> .719 gap 10.31u).

Aggravators, each a lesson:
- Markers are WORLD-PARKED: the actor walking away ends the fight; walking back resumes it.
  A push that moved her off the spot LOOKED like a fix — the bodies persist until world death
  (save-load). "It stopped" is not "it's gone."
- Knob-off does not remove already-placed bodies (the flash-probe lesson again, third form:
  STOPPING IS NOT UNDOING). meshMarkers 0 prevents re-placement only.
- The 7 torso markers coexisted with rigs for weeks — the accident needed the NEW limb
  markers to overlap rig territory. A safe pattern plus a safe addition can compose unsafely.

FIXES (queued): (1) marker collision group excluded in every PPB filter — a visualization
body must never be collidable by anything of ours; (2) marker knob-off actively removes the
parked bodies (zero-clear call). POLICY: detection NEVER uses placed bodies — real welded
capsules or pure computation only (the ReTouch architecture rule, user-set, now proven twice).


---

## Dismemberment / foreign-DLL era (2026-07-26/27) — see `14` for the full mechanisms

**Hooking another mod's DLL**
- `SKSE::Trampoline::write_branch` is for RE-POINTING AN EXISTING CALL SITE, not detouring a
  function entry, and its rel32 jump **cannot reach a DLL loaded >2 GB away** — the user hit
  "skse/Trampoline.cpp(168): displacement out of range" as a hard crash. Use a **14-byte absolute
  jump** (`FF 25` + abs64), which has no range limit.
- **Validate the prologue, and wildcard the stack-displacement byte.** `48 89 5C 24 ?? 55 56 57 41
  54 41 55 41 56` — DF/NGD-Step02 use `0x20`, **NGD-Step01 uses `0x18`**. A rigid `memcmp` silently
  refused to install and cost a whole test session (the log said "prologue mismatch", which is why
  the guard must LOG and DISABLE rather than patch blindly).
- Many Havok SDK setters are **not linked** into PPB (`setEnabled`, `setPosition`, `setMotionType`,
  `setMass`). Bind the engine function by address (PLANCK's idiom:
  `hkpConstraintInstance::setEnabled` @ `0xAC06A0`) or write the fields directly
  (`const_cast<hkTransform&>(hk->getTransform()).setTranslation(...)`).

**Threading**
- **NEVER marshal via `SKSE::TaskInterface::AddTask` to escape a deadlock whose cause IS the task
  lock.** The VR dismember freeze is exactly that: main thread holds the SKSE task lock while stuck
  in the FSMP skinning pass; DF's workers block in `AddTask`. Marshaling through it blocks on the
  same critical section.
- **Never block a foreign mod's worker thread while calling that same mod's code from the main
  thread.** Lock inversion: the blocked worker holds a DF-internal lock the main thread then needs.
  Every handoff hit the 500 ms timeout and the user felt it as a stutter.
- A hang is not a crash: **no exception ⇒ no crashlog**. Capture the live process instead —
  `rundll32 comsvcs.dll, MiniDump <pid> <file> full`, then `icacls … /grant Everyone:(R)` (the dump
  ACL blocks the store-app debugger). DF, NGD and PLANCK all ship PDBs.

**Event assumptions**
- **`TESDeathEvent` with `dead=true` NEVER FIRES in this setup** — only the dying stage
  (`dead=false`). A feature gated on the final stage runs zero times. (4× `dead=false`, 0×
  `dead=true` in one session.)
- Do not gate ragdoll work on "health <= 0": it is also true for **bleedout / essential / knocked-out
  NPCs that recover**, and combined with a permanent PLANCK-ignore it stripped their ragdolls
  permanently ("the gate was applied to everyone"). Use real death events only.

**Diagnosis discipline**
- When a mod's own log shows the work happening (DF logged real severs ~16 ms *before* our death
  event), check whether YOUR system ran at all before "fixing" it. Ours had never executed once.
- Before blaming a sibling mod, strip your own code out and re-test: 4 dumps with PPB's rigs off and
  its hooks disabled reproduced the freeze identically ⇒ **not PPB**. The user said so first.

**Knob gating (2026-07-28)**
- **Do not gate a CLASSIFICATION behind an ACTION's knob.** The severed-head *park* keys off
  `isClone`/`stripAt`, but that classification sat inside `if (... && dgCloneStrip)`. When
  `dgCloneStrip` was set to 0 after the shared-shape incident, the park became dead code —
  zero `NFING PARK` lines while the user watched the joints orbit the head. Classify whenever ANY
  consumer is enabled; let each action's own knob decide what runs.
- Corollary for diagnosis: **an absent log line is evidence**. "No PARK lines" located this in
  seconds; without the per-action logging it would have looked like a physics failure.

## ★★ A VTABLE FOLLOWS THE BASE INTERFACE, NEVER THE DERIVED CLASS'S TEXTUAL ORDER (2026-07-29)
Chasing a failing PLANCK settings call, I "fixed" our local IPlanckInterface001 declaration to
match the DERIVED class in planck's pluginapi.h (which lists Get/SetSettingDouble between
Deprecated2 and AddIgnoredActor). WRONG: those are overrides — the vtable layout is defined by
the BASE interface in planckinterface001.h, which declares Get/SetSettingDouble at the END
(slots 13/14). The original declaration had been correct all along; the "fix" broke every call
for a day (settings calls landed on Remove/AddIgnoredActor; ignore calls landed on the
AGGRESSION ignore lists). Also retract the same-day claim that "PLANCK-ignore was a silent
no-op since 07-26" — it was working; only the 07-29 afternoon builds misdirected it.
RULES: (1) when declaring a foreign interface locally, copy the BASE-interface declaration
order, never a derived class's; (2) prove a vtable theory with a positive control before
"fixing" it (call GetBuildNumber through the suspect layout and check the value) — a runtime
probe would have falsified this in one launch; (3) diagnostic-first: the PIVGUARD first-fire
diag (knob → skeleton → getOk/setOk) found in one launch what three theory rounds missed.

## ★ NEVER REMOVE A BODY FROM THE WORLD WHILE A CONSTRAINT STILL REFERENCES IT (2026-07-28, CTD)
`crash-2026-07-28-22-15-55`: `EXCEPTION_ACCESS_VIOLATION` at `SkyrimVR.exe+0ABB2B4
cmp dword ptr [rcx+0xF8],0` with **RCX = 0**, **RDX = hkpRigidBody "NPC L Thigh [LThg]"** (base
00048117 — the decap Redguard), **RSI = hkpConstraintInstance whose Entity[0] is that same body**,
**RAX = hkpSimulationIsland\***. The severed-head park had removed the 17 satellite ragdoll bodies
from the Havok world; the engine's constraint pass then walked a constraint whose entity had **no
simulation island** and dereferenced null.
- **DISABLING a constraint is NOT REMOVING it.** `hkpConstraintInstance::setEnabled(false)` stops it
  solving; the instance still exists and still points at both entities. A body may only leave the
  world once **nothing references it** — PPB's own marker/rig bodies are safe precisely because they
  are standalone (no constraints), which is why `RemoveBody` had been safe everywhere else.
- Aggravator: NGD's maintenance re-added the bodies every ~2 s, so remove→re-add churned for 7
  minutes before hitting the bad window. **A removal that reports the same count every pass is not
  removing — it is losing a race**, and a race like that is a CTD waiting for its moment.
- The fix was to stop needing the removal at all (`bAdvancedNPCMaintenance = 0` — NGD's own switch)
  and revert to keyframe+sleep, which never crashed. See `14` §4f/§4g.

## ★ THE FEET-IN-THE-GROUND SHIP BUG (2026-07-29) — poseConform × loosenRagdollConstraintPivots=1
First user-visible regression of the shipped mod, and it reached MALES. Two compounding errors:
1. **poseConform is UNGATED** — its map is built by ragdoll-bone → "NPC …" node-NAME matching
   (PPBHook.cpp pass 1/2), which succeeds on EVERY XP32 humanoid, male or female. "PPB only
   touches mapped females" was true of CapFix/PivFix/rigs and FALSE of the conform. My log-census
   "proof" that no male was touched sampled ids from the CHATTY systems; PCONF logs one map-built
   line and occasional perf warnings, so its reach was invisible in an id census. **A per-actor
   system that logs almost nothing is invisible to log-based audits — census the CODE GATES, not
   the log volume.**
2. **The whole calibration era ran under loosenRagdollConstraintPivots=0** (PPB's dev override).
   Users run PLANCK's default **1**. Under 1, the conform sinks every conformed humanoid ~2u into
   the ground (mechanism unresolved — pivot collapse and the conform fight over the same
   equilibrium; PLANCK foot-IK expresses the result as sunken feet). We never saw it because our
   machine never ran the shipping configuration. **Test the SHIPPING config, not the dev config —
   retiring a dev override IS a behavior change and needs an in-game pass over the reference NPC.**
Verified live both ways: loosen 0→1 sank Lydia 2u; `poseConform 0` (hot) snapped her back.
HOTFIX: poseConform 0 in the shipped tuning (1.0.2). OPEN: root-cause the interaction, or retire
the conform after measuring what it buys; if kept, gate it to PPB-driven actors only.

## ⛔ PER-ACTOR SCOPING OF A SIBLING MOD'S FLAG MUST RESPECT ITS FULL CYCLE (2026-07-29, user-caught)
`planckLoosenOurs` set PLANCK's `loosenRagdollConstraintPivots` to 0 around a PPB-skeleton actor's
drive (restore-after-chain). LOOKED sound — the flag is consumed inside PLANCK's per-actor
PreDriveToPoseHook. BUT the collapse is a CYCLE: save originals → collapse → (next frame) RESTORE
originals — and the RESTORE is gated on the SAME flag. One frame collapsed under flag=1 (ragdoll
build, load order, any pre-bracket window) + every later frame at flag=0 ⇒ the restore never runs
⇒ pivots stranded at the collapsed snapshot ⇒ joints visibly off the XP32 nodes (Lydia, caught by
eye; log signature: settled `medHavokArc` ~1.7% short of `nodeArc`, PIVRESCALE "correcting" a
healthy NPC). RULE: before scoping another mod's setting per-actor, trace EVERY consumer of that
setting through its full state cycle — a flag that gates both a mutation AND its own undo cannot
be scoped by value alone. Feature REMOVED from the code entirely 2026-07-29 evening (not knobbed off — a footgun with a
dormant switch is still a footgun). If ever revived: force-restore the pivots ourselves on
scope-entry, per ragdoll instance, before the first scoped drive.

## ★ RAISING A CHILD-COUNT BOUND CAN WAKE LATENT JUNK KNOBS (2026-07-29, caught pre-launch)
Adding the pelvis sensors raised `CapFixChildKnobs(spine1)` from 9 to 11 — and `capSpine1C9/C10`
**already existed in the shipped tuning file, `Enable 1.0`, with AY = −25.515** (a stale dial from
some old session). They had been harmless ONLY because the apply loop stopped at C8. Raising the
bound would have made them live and hung capsules ~25u BEHIND every NPC on all four skeletons.
Caught by sweeping the newly-exposed index range for non-seed values before launching.
**RULES:** (1) whenever you raise a child bound, **sweep every newly-reachable index in the tuning
file for non-seed values** — a knob the loop never reached is untested, not safe; (2) generated knob
blocks must check for an existing definition (mine duplicated the same keys — two definitions of
`capSpine1C9Enable`, ambiguous last-wins parsing); (3) this is the third form of the same family as
"a knob defaulting 0 in code can still ship ON via a leftover tuning-file line."
Sibling finding, left alone deliberately: `capSpine1C7/C8` are `Enable 1` writing SEED geometry, so
on the Argonian they flatten its baked dorsal ridge every generation. Pre-existing behaviour, not a
regression from this change — but it means the ridge is knob-driven, not NIF-driven, on that body.

## ★ WHEN A REWRITE MAKES A TEARDOWN UNNECESSARY, DELETE THE TEARDOWN (2026-07-29, user-reported)
The hand rigs were destroyed+recreated on 1% hand-scale drift — 60 times in one user's session —
even though the 07-10 rewrite made all geometry live-solved and `SolveGeometry`'s own header says
"no recreate needed". The obsolete destroy sat six lines below that comment for three weeks. It was
not free: every recreate starts collision-OFF for 100 ms and re-derives the collision filter from
HIGGS's hand body, so the churn both blanked the colliders and repeatedly re-sampled a possibly
stale anchor — the user's "collision stuck somewhere". Full writeup: doc 09.
**Corollary (the second half of that fix):** a VELOCITY clamp and a POSITION leash catch disjoint
failures. HIGGS's own clamp (and ours) only fires when the body cannot keep up; neither can detect
"followed a wrong anchor perfectly and stopped there". Any body that rides an EXTERNAL anchor needs
a sanity check against a source you own — ours is now the skeleton hand node (`handBoxLeashU`).

## ★ `nullptr` IN A NAME TABLE MEANS "UNNAMED", NOT "ABSENT" (2026-07-29, found by verification)
Generating the public capsule-name record (doc 15 §9) turned up **two capsules that are live,
touchable geometry on all four skeletons yet had no name**: `upperarm.C3` and `com.C20`. Both had
been assumed to be buried seeds purely because the table held `nullptr` there. Doc 15 had even
written `com.C20` down as a "spare seed" — it is a byte-exact duplicate of `C10` and fully solid.
An API built on that table would have resolved a real touch to "unknown".

**The check that finds these** is mechanical: for every unnamed index, read the NIF geometry and
compare against the known buried-seed spec (`p1=(0,0,0) p2=(0,0,2u) r=0.5u`). Anything that
doesn't match is real anatomy. Run it per race — the same index can be a seed on one skeleton and
baked anatomy on another (argonian dorsal ridge at `spine1.C7/C8`, `spine2.C15/C16`; head above
C14 is per-race entirely, and the head was never index-padded: 23 children on human/draenei vs 17
on argonian/khajiit). Script: `tools/ppb-scratch/classify_unnamed_children.py`.

## ★ GENERATE THE RECORD, THEN VERIFY IT AGAINST THE BINARY (2026-07-29)
The capsule name record is now produced by parsing `ProposedPartName()` and then **checking every
emitted name against the strings actually present in the shipped `PPB.dll`**, aborting on mismatch
(`tools/ppb-scratch/gen_capsule_name_record.py`). Writing the record by hand is how it drifts; the
first hand-written draft of the generator also **invented node names** (`"NPC Hand [Hand]"`) that
do not exist — the real ones are `kSlotNode[12]` in `CapFix.cpp:48` (`"NPC R Hand [RHnd]"`, and
`"NPC R Foot [Rft ]"` with a load-bearing trailing space). Copy such tables verbatim from source;
never retype from memory.

Two structural facts an API consumer needs, both easy to get wrong:
* **`neck` (slot 7) is a `bhkCapsuleShape`, not a list** — 0 children on all four skeletons. Code
  that iterates `shape->children` for every slot silently finds nothing on the neck.
* **Dialled positions are NOT in the NIF.** All 11 COM internal sensors and both back capsules
  still sit at the buried seed spec in the NIF; the real values live only as `cap*` knobs in
  `PPB_tuning.txt` and are applied at runtime by CapFix. Read positions from the **live Havok
  body** — the shipped shape is NIF + knobs + ReScale + ReShape + per-NPC fit.

## ★ A DIAGNOSTIC'S CODE DEFAULT IS A SHIPPING DEFAULT (2026-07-29)
`touchProbe` defaulted to `1` in `Tuning.h`. The tuning file that ships overrode it to `0`, so the
bug was invisible — until a user deletes or omits their tuning file, at which point they get a
capsule-mapping log line every 45 frames forever. Fixed to `0` in code. **A knob's code default is
what a user with no config gets: set it to the shipping value, and never rely on the config file
to mask it.**

## * A VERSION GATE IS NOT A COMPATIBILITY BUG â€” LOOSENING ONE IS (2026-07-29, SMP Flex)
PPB refused HDT-SMP Flex with "interface major mismatch â€” link stays OFF". The tempting fix is to
widen the gate to accept major 1. That would have been **catastrophic**: v1 and v2 of the SMP plugin
interface differ in exactly one thing, the LISTENER BASE CLASS.
* v1: `hdt::IEventListener<T>` â€” **no destructor at all** => `onEvent(const T&)` is vtable **slot 0**
* v2: `RE::BSTEventSink<T>` â€” dtor is slot 0, `ProcessEvent` is slot 1

Attaching a v2-shaped sink to a v1 engine makes the engine call **our destructor once per physics
step**, on its TBB worker with the world lock held. Everything else is identical: the
`PluginInterface` vtable (6 slots, same order â€” the upstream v1â†’v2 diff appended and reordered
NOTHING), both event structs (16 bytes), `BULLET_VERSION{3,24,0}`, and `MSG_STARTUP == 0`.

**The correct shape of the fix: keep the gate, add a correctly-shaped path behind it.** A second
declaration (`src/fsmp/PluginAPI_v1.h`, namespace `hdtv1`), a second pair of sinks, and ONE shared
ABI-neutral body per event so the two engines can never drift apart in behaviour.

**And prove the vtable with a positive control** (this ledger's standing rule, earned the hard way
on PLANCK). Four confirmations were gathered before a line shipped: upstream source verbatim; Flex's
own shipped PDB (mangled `addListener` names carry `IEventListener@UPreStepEvent`, `onEvent` symbols
exist, `ProcessEvent` does not, and **no `IEventListener` destructor symbol** â€” checked against a
control symbol that IS present, so the negative means something); the raw DLL bytes at the
`g_pluginInterface` vptr; and a live user log printing the header's own constants. Note the third
and fourth are the ones that could not have been guessed.

**Scope discipline matters too.** "PPB doesn't work on SMP Flex" was wrong: `FsmpLink::Connected()`
has zero callers and the rig code never consults the link, so Flex users always had hair/tail
capsules, collision and touch detection. Exactly one layer â€” the SMP push â€” was dead. Measure which
features actually depend on a broken handshake before describing the blast radius.

## * A KNOB READ BEFORE kDataLoaded IS A KNOB THAT DOES NOTHING (2026-07-29)
`PPB_tuning.txt` is only ever parsed by `CapFixPollFile`, called from the pre-drive hook installed at
**kDataLoaded**. `FsmpLink`'s SMP handshake runs at the engine's **kPostPostLoad** â€” earlier â€” so a
knob consulted there reads its COMPILED DEFAULT and the user's file is silently ignored. The new
`fsmpFlexCompat 0` would have been a no-op. Fixed with `ObjectHold::EarlyReadKnob(key, fallback)`,
which reads one key straight off the same `kTunePaths`, mutates no global state, and does not disturb
the mtime the poller commits. **Any knob consulted before kDataLoaded must use it.**

This is the SECOND knob-semantics footgun found in one day (the other: `handBoxRebuildFrac`'s
floor clamp turned the intuitive "0 = off" into 2%, i.e. MORE aggressive than the default). Both
share a root cause worth generalising: **a knob is a contract with the user, and the contract
includes when it is read and what its zero means.** Check both when adding one.

## * WHY PPB INJECTS FORCES INSTEAD OF USING SMP'S OWN COLLISION (2026-07-29, settled)
Recurring question, now answered with data. PPB ships a real SMP-native hand collider
(`ppbHands.xml`, mapped via PPB's `defaultBBPs.xml` override, confirmed live in `hdtSMP64.log` as
`body HIMBO - Hands`) and it is engine-agnostic â€” both engines read the same per-mesh XML family. It
whitelists `<can-collide-with-tag>Tail</can-collide-with-tag>` and that genuinely works.

It cannot be extended to hair. Per the FSMP wiki, `can-collide-with-*` is **exclusive**, the other
shape must carry a matching `<tag>`, and `<shared>private</shared>` means same-mesh-only. Measured
over the whole install: **216 files declare `<tag>hair</tag>` and ZERO declare `<shared>public</shared>`**,
and they additionally whitelist only their own in-file virtual colliders. Both blockers live in other
mods' read-only XML. Force injection into the bone's `btRigidBody` bypasses SMP's filtering entirely
â€” that is the whole reason it exists. (Corollary: a hair mod shipping its own `VirtualHands`
collider already has hand-to-hair collision natively, with no help from PPB.)


## * A UV PROBE THAT SAYS "WRONG LAYOUT" IS USUALLY THE WRONG MESH (2026-07-30)
Asked whether BHUNP shares CBBE's body UV layout, the first comparison answered "DIFFERENT
ANATOMY" with 25-51 u drift. It was wrong: it probed `CBBE 3BA Ref.nif`, the BodySlide
**reference**, instead of the installed body. The correct answer is the opposite â€” BHUNP shares
the layout, mean drift **1.21 u** over a 230-point UV grid, 100% within 5 u (doc 04, and a
correction now stamped into KNOWLEDGEBASE.md, which had asserted the reverse without measuring).

**The tell was in the data, not in the conclusion.** `uvErr` ran 0.025-0.094 â€” *above ReShape's
own 0.02 rejection threshold* â€” and all five landmarks collapsed onto one point at head height
instead of forming a chest â†’ navel â†’ waist â†’ hip ladder. ReShape gates on `uvErr > 0.02`
precisely so a wrong mesh announces itself; the gate did its job and the first read ignored it.

Rules earned:
* **Probe the installed / BodySlide-OUTPUT body, never the ShapeData reference.**
* **Read the error metric before believing the result.** A conclusion drawn from samples that
  failed the code's own acceptance gate is not a measurement.
* **Sanity-check the axis convention.** A v-flip here moved mean `uvErr` 0.0012 â†’ 0.0147 â€” still
  *under* the gate, so it would have produced quietly wrong landmarks rather than a clean
  failure. Where a plausible-but-wrong convention exists, prove which one is right by which
  minimises the error, not by assumption.
* **Five samples is an anecdote.** The claim only became solid at 230 grid points plus the UV
  bounding boxes agreeing to four decimals (v 0.0255/0.0256 â†’ 0.9902/0.9902) â€” a layout
  fingerprint that a handful of landmarks could never establish.


## * MAKE THE PLUGIN LOG WHAT IT SAW, THEN BUILD AGAINST THE ANSWER (2026-07-30, the session's method)
Five separate dead ends this day were each settled the same way â€” not by reasoning harder, but by
adding a ONE-SHOT log of the thing being guessed at, then building against what came back:

| guess that failed | the log that settled it | the answer |
|---|---|---|
| "the fist threshold is ~7u" | print the measured curl on every receipt | values span 3.7-9.1u; 7 sits mid-range |
| "the fingers don't articulate" | the same receipts, second session | they DO â€” session 1 was one grip held throughout |
| "the weapon shape is a capsule" | `hkpShapeType=` one-shot | it is a LIST |
| "walk the list for a capsule child" | census the child types | three `kConvexTransform` (12), no public header |
| "SMP Flex is a bone-rename no-op" | dump `*ail*` nodes on a pinned actor | `hdtA_1CF71480_HDTS TailBone09.003` |

**Every one of those guesses was plausible.** The cost of being wrong was a wasted VR session each
time; the cost of the log line was three minutes. Corollary earned twice: **a one-shot diagnostic
that names an unknown TYPE (shape id, child types, interface version) is worth more than any
amount of reading**, because it converts "what could it be" into "what it is".

Second corollary: **a capped diagnostic that reaches its cap has stopped being a measurement.**
The leash logger capped at 3 lines per hand and hit the cap, so the log could not distinguish
"fired 6 times" from "fires every frame". Add a running total to any capped log.

## * DO NOT DEFEND A DESIGN THE USER IS QUESTIONING (2026-07-30, three times)
Three times today the user pushed back on something I had shipped, and three times they were right
and I had rationalised it:

* **hand + held-object both reporting from one wand.** I called it "a feature â€” consumers can tell
  the two apart". The user: your palm is on the grip, it is noise. Correct â€” suppressed.
* **per-capsule dwell filtering.** I built exactly what was asked, but the user's own face example
  exposed that it emits NOTHING for a real wandering touch. The fix (region accumulation) is
  strictly better than the spec I implemented.
* **"can you just check what HIGGS hand are doing?"** I had already concluded geometry was the only
  option. VRIK exposes the controller-driven finger pose directly; one interface call replaced two
  sessions of heuristics.

The pattern: I optimised the thing I had built instead of asking whether it was the right thing.
When the user proposes a mechanism, PRICE IT before defending the current one.

## * A CODE DEFAULT THAT DISAGREES WITH THE SHIPPED FILE IS A BUG (2026-07-30, four instances)
`touchProbe` (code 1 / shipped 0), `npcFingerLog` (1 / 0), `handBoxRebuildFrac` (0 meaning 2%),
`apiFistTipPalmU` (code 7 / shipped 2). Each is invisible on the dev machine â€” the tuning file
masks it â€” and each hits any user whose file is missing or partial. **Rule: the compiled default
IS a shipping default. Align it with the shipped file, and check what the knob's ZERO means.**

## * A NON-ZERO STRUCT INITIALISER CAN COST MEGABYTES (2026-07-30)
Adding `float lastD2 = FLT_MAX` to `FingerRig` grew the DLL by **1.28 MB** (1,870,336 â†’
3,154,944). It was the only non-zero default in the struct, so `g_rigs[8]` â€” each holding
`FingerBody bodies[200]` â€” moved out of `.bss` (zero-init, no file bytes) into `.data` (literal
bytes on disk, dirty pages at load). Fixed with an all-zero sentinel (`0.f` + a `bool valid`).
**Any sentinel in a large static array must be zero-valued.** Diagnose with
`dumpbin /HEADERS` and compare `.data` raw size.

## * SPLIT THE QUERY FROM THE COMMIT (2026-07-30, caught by adversarial review before shipping)
`GarmentBudgetAllows()` was used as a per-frame branch predicate AND evicted rigs as a side
effect â€” so any driven NPC in range destroyed a live 200-chord wig rig it would never use, the
victim rebuilt ~200 frames later, and the cycle repeated, leaking the whole rig each time
(never-free convention). **A predicate must not mutate lifecycle.** Split into `GarmentInRange()`
(pure, gates the cheap probe) and `GarmentBudgetAcquire()` (commits, called only once a table has
actually resolved and a create will definitely happen).

Related, same review: **do not treat "unknown" as "least valuable".** The eviction picked
never-driven rigs as the preferred victim with the hysteresis skipped â€” which selected the rig
born *that frame*. Stamp state at birth so "unknown" is unreachable, and refuse rather than guess
when it happens anyway.

## * WHEN A TOOL KEEPS CORRUPTING THE FILE, CHANGE TOOLS (2026-07-30)
Bash heredocs carrying Python that carries C++ mangled the source four times: swallowed escape
levels turning `\n` into raw newlines inside string literals, an unterminated-literal parse error,
and once a **literal NUL byte** written into a char constant (`'\0'` â†’ `'^@'`), which then survived
a naive scrub as `''`. Each cost a build cycle. **Use the Edit tool for source edits, or write the
patch script to a FILE and run it.** Reserve heredocs for text with no escapes. When a file
mutates under you (a linter reformatting between read and write), patch by line position or bytes
rather than fighting exact-match strings.

## * A GENERATED RECORD MUST BE VERIFIED AGAINST THE BINARY (2026-07-30)
The capsule-name record for the public API is produced by PARSING `ProposedPartName()` and then
checking every emitted name against the strings in the shipped `PPB.dll`, aborting on mismatch.
Writing it by hand is how it drifts â€” and the first hand-written draft of the GENERATOR invented
node names (`"NPC Hand [Hand]"`) that do not exist; the real ones are `kSlotNode[12]` in
`CapFix.cpp` (`"NPC R Hand [RHnd]"`, and `"NPC R Foot [Rft ]"` with a load-bearing trailing
space). **Copy such tables verbatim from source; never retype from memory.**

