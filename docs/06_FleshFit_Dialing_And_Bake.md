# 09 — Flesh-Fit Dialing: Method, Axis Maps, Conventions, State & Bake Plan (2026-07-07)

Captures the live capsule-dialing session (post-split). The EXACT capsule numbers live in
`D:\Games\My Skyrim\mods\Precision Physic Bodies\SKSE\Plugins\PPB_tuning.txt` (persists across launches)
and the per-limb taper splits in `tools/ppb-scratch/flesh-fit-bake-notes.md`. THIS doc is the DURABLE
KNOWLEDGE (method, axis maps, conventions, bug, bake plan) that isn't recoverable from those files.

## The flesh-fit workflow (auto-fit baseline → manual dial → bake)
1. **Auto-fit placed the baseline** — endpoints ball-to-ball on the seated joints (capAutoFit).
2. **FREEZE (done 2026-07-07)**: auto-fit OVERWRITES manual endpoints every tick, so to hand-dial an
   endpoint auto-fit must be off. It's a GLOBAL flag, and the spine/neck/leg slots had no manual
   endpoints (would go degenerate at [0,0,0]). So the freeze script seeded ALL 8 auto-fit capsules'
   manual A/B from their live `AUTOFIT` log values, then set `capAutoFit 0`. Nothing moved; every
   endpoint is now hand-dialable. Backup: `PPB_tuning.txt.bak_prefleshfit`. **Reversible**: `capAutoFit 1`
   re-derives from the joints (but that OVERWRITES all the dialed arm capsules — do not re-enable
   mid-flesh-fit). This freeze IS the natural pre-bake transition (the bake needs fixed values anyway).
3. **Dial each capsule endpoint by voice**, from-current deltas (the hard user rule: every correction is
   a delta from the CURRENT visible state). Claude edits PPB_tuning.txt; ~1 Hz poll applies it live.
4. **Taper split** (upper arm, forearm, legs): dial the main capsule as the UPPER (thick) half, then say
   "record" to lock the original FAR end, then bring the main capsule's far end up to mid-bone. At bake,
   a THINNER second capsule fills [mid-bone] → [recorded far end]. See the bake-notes file.
5. **Bake** into skeleton_female.nif (mirror left), add child knobs for the new taper capsules, retire
   PivFix + auto-fit.

## Axis maps — A-POSE STATUE (pose-dependent; DIFFER from the hanging-arm recorded maps)
Body-local frames AS USED this session (per the user's corrections). **Axis maps are POSE-DEPENDENT** —
the bind/hanging maps rotate in the A-pose. First move on any bone's cross-section axis is a PROBE:
state the guess, let the user confirm, record it.
- **Upper arm** [RUar]: +X inward / −X outward · +Y frontward / −Y back · +Z elbow-ward ("down") / −Z up.
  (X and Z user-confirmed; +Y=frontward a probe never contradicted.)
- **Forearm** [RLar]: +X inward / −X outward · +Y forward / −Y back · +Z wrist-ward / −Z up (extend).
  (Used the upper-arm convention as a probe; user continued without correcting. The RECORDED HANGING map
  is different — +X palm, +Y thumb, +Z wrist — but the A-pose rotates the cross-section, so inward≈+X held.)
- **Hand** (3-capsule plate; children C1 center / C2 thumb / C3 pinky): +Z fingertips ("down") / −Z up ·
  **Y = inward(−) / outward(+)** · **X = forward(+) / back(−)**. ⚠ The hand's Y is INWARD/OUTWARD, not
  fwd/back — user CORRECTED this mid-session ("that's not back, that's inward"; the palm faces her body,
  so −Y palm = inward, +Y back-of-hand = outward). The recorded bind map (+X thumb, +Y knuckles/back,
  +Z fingertips) is anatomical, not body-relative.

## ★ THE FOCUS RULE — USER RULE (stated 2026-07-16, the general form)

**"When I give a new correction, that correction applies to the LAST capsule or point that I corrected."**

A bare correction ("back 8u", "radius +0.5", "up 2") carries NO subject — it inherits the one from the
previous command. The focus moves only when the user NAMES a new subject ("all except main capsule,
forward 8u" → focus becomes the children; "Main capsule, back 1u" → focus becomes the main).

- This GENERALISES the older shorten/extend focus note below (which scoped "focus" to which ENDPOINT of
  one capsule). Same idea, one level up: focus = the SUBJECT (which capsule/point), and it is sticky.
- **Claude got this wrong on 2026-07-16**: after "move all the head capsule 20u forward, move the main
  capsule backward 10u" (focus = MAIN), the next command "back 8u" was applied to ALL 15 head capsules
  instead of the main alone; the user had to spend a correction undoing it. Cost: one round-trip in VR.
- Track the focus explicitly in your head every turn, and STATE it back in the reply ("focus is now the
  main capsule") so a wrong inheritance costs one word to fix instead of a wasted dial cycle.
- Corollary: the focus persists across turns and across compaction — if you are unsure what the focus is,
  ASK; do not guess and do not silently widen the subject to "everything".

## "shorten" / "extend" convention — USER RULE (corrected 2026-07-07)
"shorten by Nu" / "extend by Nu" = move ONE endpoint — the point currently in focus — by N along the
bone axis. **NOT symmetric (both ends).** The user works one side at a time. (Claude twice did it
symmetric — upper-arm "shorten 1u" + forearm "shorten 1.5u" — and corrected. Residue: upper-arm top
A.Z sits at 1.5 vs the 2.0 a one-side shorten would give — user left it to eye, open to a −0.5 finish.)

## Current state (RIGHT side; values in PPB_tuning.txt — mirror ALL to LEFT at bake)
- **Upper arm** capUpper: main = thick UPPER half (r3.0), split recorded (2nd capsule → elbow [1.1,0.5,20.2]).
- **Forearm** capFore: main = thick UPPER half (r2.0), split recorded (2nd capsule → wrist [0.8,1.3,10.3]).
- **Hand** capHandC1/C2/C3: the baked 3-capsule plate, re-dialed onto the hand (all live, r1.0 each).
- **Legs** capThigh / capCalf: endpoints on the joint balls (frozen auto-fit), **radius set to 3.0** as a
  thin precise baseline — flesh-fit + taper-split PENDING (record at knee/ankle when dialing).
- Head / foot / COM: capHead untouched (near-degenerate ball, TODO); capFoot dialed r2.0 (heel saga);
  capCom disabled.
- Spine0/1/2, neck: frozen at auto-fit endpoints + their staged radii — flesh-fit PENDING.

## The taper second-capsule plan (per-limb, recorded live)
Method: `main-capsule-B (mid-bone) → recorded far end`, radius thinner than the main. At bake, convert
each limb body to a bhkListShape (R-hand-plate pattern), add the 2nd capsule, add live child knobs
`cap<Slot>C1/C2` (like capHandC1-3) so both stay dial-able. Recorded so far: upper arm, forearm (see
bake-notes file). Legs to come.

## ⚠ BUG: PivFix self-heal fights player grab (found in-VR 19:15)
Grabbing her arm displaced wrist/elbow bodies ~12u; PivFix's 1 Hz self-heal snapped the pivot back each
second → bounce + runaway (PIVTRACK W/E ran to 6u off-bone; E-S spacing blew 20.6→27.3u; strain 0→0.63).
The scramble guard only defers >40 u/s (grab was slower); the calm gate only >8u strain (stayed <1u).
**FIX: add a GRAB GATE to PivFixApply** — pause the pivot re-seat while the actor is HIGGS/PLANCK-grabbed
(AIHands' `IsActorGrabbed` pattern; PPB acquires the HIGGS interface). Consider rate-limiting the HEAL
(ease a large childOff back over frames, not one snap). **Calibration-only priority**: post-bake PivFix
RETIRES (pivots baked-true), so the grab bounce can't recur in shipping — PLANCK handles a grab on a
correctly-built joint. Only needed if we want to grab NPCs WHILE PivFix is live.

## What the BAKE retires vs keeps (answered this session)
- **Retired at bake**: PivFix (pivots baked into the constraint blocks — the `PIVBAKE` feed), auto-seat,
  auto-fit, CapFix's BASELINE (capsules baked into the NIF). Flip PLANCK `loosenRagdollConstraintPivots`
  back to 1 (no-op on true joints). The grab bounce dies with PivFix.
- **Stays runtime (unbakeable)**: heel fix (per-NPC heel offset, rides animation) + clavicle-follow
  (shoulder anchor rides the clavicle chain) + the future armor/OBody radius variation layer (uses the
  CapFix per-actor float-write path).

## Bake plan (next game-close — bundle)
1. Freeze the final PPB_tuning.txt values + the `PIVBAKE` pivot lines (archive PPB.log first — truncates).
2. NIF surgery on skeleton_female.nif: capsule float-patches for all dialed slots; convert upper-arm /
   forearm (and legs) bodies to bhkListShape + add the thinner 2nd taper capsules (R-hand-plate pynifly
   path); write pivots into the constraint blocks. MIRROR ALL to the left (X-mirror capsules; L joints).
3. Code (rebuild, game closed): add live child knobs `cap<Slot>C1/C2` for the new taper capsules;
   add the PivFix GRAB GATE.
4. Restore PLANCK `loosenRagdollConstraintPivots = 1`.
5. Regression gate (Report AIHands/14): doorway/crowd blocking, melee/arrow attribution, incline walk,
   heels, SMP jitter, death-ragdoll settle, PLANCK get-up, give/receive.

## Test / verify list (carry forward)
- [ ] Finish flesh-fitting: legs (thigh/calf + taper), spine0/1/2, neck, head (near-degenerate ball), COM.
- [ ] Upper-arm A.Z: keep 1.5 or finish the shorten to 2.0 (user's eye call).
- [ ] Heel-fix v2 test: strip Carmella's heels → does the knee/foot issue vanish? (confirms the feet-
      don't-rise theory; heel fix still ON at heelZ=8.0 this session, leg was clean anyway.)
- [ ] Confirm hand-plate axes (X fwd/back sign) once more, and the forearm inward/forward.
- [ ] `trg` capsule view (rebuilt): do the CBPC zones now draw as 2-ball capsules? Want proper capsule
      TUBES next (a capsule-shape marker — separate collviz_markers rebuild).
- [ ] Constraint-pivot ball size (collviz ini 0.02 = ~1.4u) — is it the right size to your eye?
- [ ] The two spells (statue FExxx803, invisibility FExxx805 — `help PPB 0`); console `statue`.
- [ ] Post-bake: PivFix off + loosenRagdollConstraintPivots=1 → grab an arm, confirm no bounce.

## ═══ THE 2026-07-07 CLOSED-GAME BUILD + LIST-SHAPE BAKE (wave 1) — AS-BUILT ═══
Executed while the game was closed (user request: "make it that Com can be moved, add the extra capsule
to the arms and legs, and prepare to a new launch"). Multi-agent build, all three lanes verified + deployed.

### 1. skeleton_female.nif — 10 bodies converted to bhkListShape (THE flesh-fit structural bake)
- Script: `tools/ppb-scratch/bake_listshapes.py` (reads main values LIVE from PPB_tuning.txt; refuses
  in-place + any D: output; double-bake guard: aborts if a target body is no longer a single capsule;
  null-roundtrip byte-identity gate; reload-verify every float).
- **R+L UpperArm / Forearm / Thigh / Calf / Foot → bhkListShape**: child 0 = the dialed MAIN capsule,
  children 1..N = the flesh-fit extras. 34 children total (was 10 single capsules → capsule census 21→45).
  **Counts (user-corrected: thigh=5, calf=3)**: upper arm +1 taper (→[1.1,0.5,20.2] r2.2), forearm +1
  taper (→[0.8,1.3,10.3] r1.6), thigh +5 superposed rings r5.0→3.2, calf +3 rings r3.4→2.3, foot +2
  parallel sole rods (X±1.2, Y−0.8, r1.2). LEFT = negate-X mirror of right (NIF-verified transform).
  Hands untouched (bit-identical; L-hand 3-rod conversion + R-hand re-dial freeze = future deliverable).
- No inertia/motion/material writes; constraints/strings/NiNode census unchanged; 97,618→99,470 B.
- Deployed md5 2eb52824ed6bfe4607cc8917f74e6b6c. **Backups: `tools/ppb-scratch/bake-2026-07-07/`
  (skeleton_female.bak_pre_fleshfit.nif md5 3670dfee…, PPB.dll.bak_pre_fleshfit).**

### 2. PPB.dll (747,520 B, deployed) — new knobs/systems
- **capCom full A/B/r** (was radius-only). Discovered stock = hip-to-hip ±X ROD r7.66
  (A=[2.79,−3.83,5.33] B=[−2.79,−3.83,5.33]), NOT a ball. Tuning seeded stock endpoints + user's r3.0
  (intended shrink), Enable 1. NOTE: recon saw NIF r6.30 vs live 7.66 — recheck `com BEFORE` next launch.
- **List-child knobs** capUpperC1 / capForeC1 / capThighC1–C5 / capCalfC1–C3 / capFootC1–C2 (full A/B/r
  each): drive list children 1..N; the EXISTING slot knobs keep driving child 0 (mains survive). ⚠ Child
  mapping convention: LIMBS = child0-main/C1..CN-extras; HAND keeps historic C1..C3 = children 0..2.
  Graceful-skip if a body isn't a list/child missing (log once, never allocate).
- **capMirrorL 1**: every capsule slot/child write replays on the LEFT twin (X-negated, same r). L twins
  joined the 1 Hz rebuild identity probe (12→18 bodies). While L-hand is still single-capsule it receives
  the mirrored C1/centerline values.
- **pivGrabGate 1**: PivFix re-seat/heal pauses while the NPC is HIGGS-grabbed (+1.5 s release grace;
  heuristic fallback only when HIGGS absent, 2-tick classify, 10 s hard cap). Log "PIVFIX grab-gate HOLD".
  Also skips statue-autoseat measurement + CapAutoFitArms while grabbed. Fixes the grab-bounce runaway.
- **`probe` console command** (donors TestAllCells→ToggleWaterSystem→ShowQuestStages): dumps the SELECTED
  actor's full physics+animation state to PPB.log — per-ragdoll-body motion type/velocity/worldZ,
  IsInRagdollState, knock/life/sitsleep/killmove, bAnimationDriven graph bool, occupied furniture,
  charController support. FOR THE FLOATING-NPC BUG: probe her BEFORE A-posing (A-pose destroys evidence).
- Tuning snapshot 178→276 floats; new sources Interop/HiggsInterface/Probe; same build preset
  (`cmake --build --preset vr`, VCPKG_ROOT=VS-bundled).

### 3. collviz_markers.dll (632,320 B, deployed) — CBPC capsules render as TUBES
- A CBPC '&' two-point zone now becomes ONE marker whose shape is a REAL engine hkpCapsuleShape
  (MemoryManager 0x50/0x10 aligned, vertex.w=radius, engine-VTABLE stamped — MANDATORY: collviz
  DYNAMIC_CASTs via game RTTI at main.cpp:1141; a wrapper vtable would silently draw nothing).
- Unequal '&' radii → capsule (r=min) + the two true-radius endpoint balls; identical positions → old
  two balls; pure spheres unchanged. Capsule markers ride pos+ROTATION (new MarkerKeyframeRot).
- ⚠ ~15% first-run risk on the vtable stamp: if trg markers vanish/CTD → fallback variant documented in
  MarkerCreateCapsule comments. Scale folds into endpoints at creation only (collviz caches tessellation).

### Torso session state (pre-build dialing): all 5 torso slots set r3.0 for precision dialing
(spine0/1/2 + neck + COM); spine/neck endpoint flesh-fit still PENDING. Right-leg mains FINAL:
thigh A=[-2.2,-3.2,-6.0] B=[0.7,-0.3,25.9] r3.0 · calf A=[0.5,-1.2,-4.9] B=[1.0,-0.9,18.4] r2.2 ·
foot A=[1.02,3.68,5.27] B=[1.02,4.68,-1.21] r1.5. Leg axis map: inward=+X, back=−Y; foot rotates ~90°
(up=+Y, toe=+Z, A=toe/B=ankle).

### First-launch checklist (next VR session)
1. PPB.log: "PPB HIGGS interface=OK" + GrabTune FILE LOADED (276 floats) + no CTD on load.
2. trd on any female NPC: thigh shows 5 rings + main, calf 3 + main, foot sole rods, arm tapers — BOTH sides.
3. trg: '&' CBPC zones = tubes now (if markers vanish/CTD → vtable fallback, see viz notes).
4. COM: hip rod now r3.0 (visible shrink); dial position via capComA*/B* (the user's "up 6u" test).
5. Grab an arm: expect "PIVFIX grab-gate HOLD", no pivot bounce.
6. NEXT FLOATING NPC: select in console → `probe` → read PPB.log BEFORE A-posing her.
Rollback: restore both .bak_pre_fleshfit files from tools/ppb-scratch/bake-2026-07-07/.

## ═══ SESSION 2026-07-08 — post-bake dialing of the new children + torso start ═══
The wave-1 list-shape bake landed and was in-VR CONFIRMED (all children visible on BOTH sides — the live
`capMirrorL` works; log shows `CapFix ... MIRROR-L <limb> (list children mirrored + L AABB repatched)`
every tick with zero skips). Everything below is knowledge NOT recoverable from PPB_tuning.txt.

### ★ SPINE AXIS MAP — INVERTED vs the limbs (do NOT carry the leg map over)
- **+Z = UP toward the head.** (The legs' +Z ran DOWN the limb. Opposite sign convention.)
  A = lower end, B = upper end.
- **+Y = forward / belly · −Y = back.**  **X = shoulder-to-shoulder (left ↔ right).**
- Verified by dialing spine0/spine1 in-VR; the horizontal-rod experiment (below) confirmed X.

### ★ spine0 was DELIBERATELY rebuilt as a HORIZONTAL shoulder-to-shoulder rod
User: *"make it horizontal, shoulder to shoulder for orientation."* spine0 is no longer a vertical spine
segment — it's an 11u crosswise rod (A=[-5.5,3.55,9.75] B=[5.5,3.55,9.75] r4.0) across the lower belly.
**Do not "correct" this back to vertical.** It doubles as the X-axis probe for the whole torso.

### ★ The "main = thin core rod, children = the flesh" pattern
Once each limb had its children, the user pulled the MAIN capsule's radius way down — thigh 3.0→2.0,
calf 2.2→1.5, foot 1.5. The main is now just a spine down the limb; the superposed rings / parallel rods
carry the actual flesh volume. Small main radii are INTENTIONAL, not a mistake.

### ★ "shorten"/"extend" — three variants, and the split is a footgun
1. Bare **"shorten Nu" / "extend Nu"** = ONE endpoint (the one in focus; default = the far/bottom end),
   moved N along the bone. The user's standing rule (they corrected Claude on this 07-07).
2. Explicit **"shorten both ends Nu"** = **N per end** (total 2N). Used on spine0 + spine1.
3. Bare **"extend Nu" on a CENTERED HORIZONTAL rod** (no top/bottom, e.g. the new spine0) = **N TOTAL,
   split N/2 per end**, keeping it centered. Claude chose this; user accepted.
⚠ (2) and (3) disagree on what N means. ALWAYS state which interpretation you applied.

### ★ ENDPOINT CROSSING (hit on spine0)
Shortening BOTH ends by more than half the capsule's length makes A pass B. The geometry is still a valid
capsule (a segment doesn't care about point order), but the A/B **labels invert**, so every later
"top point / bottom point" command silently flips. **Fix: swap the A and B values** — a pure geometric
no-op that restores A = lower point. Do this immediately, don't let it ride.

### ★ THE LEFT ARM IS OFF BECAUSE OF PIVOTS, NOT THE MIRROR (diagnosed + verified 2026-07-08)
User asked why the left arm's joint AND capsule look wrong. Verified empirically by dumping the PRE-BAKE
skeleton (`bake-2026-07-07/skeleton_female.bak_pre_fleshfit.nif`): **Bethesda's own stock L/R limb capsules
are exact X-negations, Y and Z identical** — e.g. R UpperArm p1=[0.087,0.087,14.272] vs L=[-0.087,…];
R Forearm [-0.794,…] vs L [0.794,…]; same for thigh/calf/foot. So `capMirrorL`'s negate-X transform is
**provably correct**, and the log confirms it fires on every limb.
**The real cause: PivFix / auto-seat / clavicle-follow are RIGHT-SIDE ONLY** (there isn't a single left
pivot knob in the tuning file). The left joints still sit at their 2011 positions, 2-6u off the XP32 bones,
and a mis-seated pivot spawns a **pre-stressed joint** that drags the connected bodies off their bones —
so the correctly-mirrored left capsules ride displaced bodies. ONE root cause, TWO visible symptoms.
→ FIX = **`pivMirrorL`** (task #12), same trick as capMirrorL; or it falls out of the wave-2 pivot bake.

### ★ "Havok joint shifts slightly when the leg moves" — that's PD SERVO LAG, not a defect
PLANCK PD-drives the ragdoll toward the animated pose: a spring chasing a moving target. Bodies trail
bones during motion and reconverge at rest. **Baking cannot remove it and shouldn't.** Discriminator:
move the limb, then let her settle / statue her. Slides back onto its bone cube → servo lag (harmless).
Stays displaced → real geometry error (bake fixes). Snaps rhythmically ~1 Hz → PivFix self-heal (bake fixes).
Standing rule (already in the ledger): **judge placement only on a still or statued body.**

### ★ Per-capsule grab identification (task #11) — the design that came out of this session
User wants a grab to know it hit the *glute* (thigh C3), not just "the thigh". You **cannot** name
sub-shapes: a rigid body follows exactly ONE bone, and separate bodies are forbidden (never add ragdoll
bodies). But the list children are individually addressable:
- **PREFERRED: `hkpShapeKey`.** A collision against a bhkListShape reports the child index — the engine's
  own per-sub-shape mechanism. ~60% confidence it survives HIGGS's grab path (HIGGS is 2-stage:
  collision-shape select → visual-mesh-triangle grab point; the key may be dropped at stage 2).
- **FALLBACK (~90%, zero engine risk): geometric.** Take the palm world pos, transform into body-local,
  point-to-segment distance vs each child; nearest/containing child wins.
- **Semantic names live in PPB config, not the NIF** (`thighC2 → "glute_R"`). Which is why the thigh's
  child indices must be re-ordered to anatomical top→bottom at the next bake (3-cycle, see bake notes).
- NOTE: VRTouchEvents differentiates by **CBPC named NiNodes**, a different mechanism — don't copy it.

### Torso state at session end
spine0 ✅ (horizontal rod) · spine1 ✅ (short fat barrel, r6.5) · **spine2 / neck / head / COM-placement
still TODO**. COM is now ENABLED with full A/B/r knobs; its stock shape turned out to be a **hip-to-hip
±X rod, r7.66** (not a ball), radius shrunk to 3.0, position never dialed — the "up 6u" test never ran.
⚠ Recon read NIF r=6.30 vs live BEFORE r=7.66 — recheck the `com BEFORE` log line next launch.
⚠ CBPC capsule-tube rendering (collviz) was BUILT but never visually confirmed in-VR — `trg` still unverified.
⚠ The `probe` console command was BUILT but never fired (no NPC floated this session).

## ═══ SESSION 2026-07-20 — BY-VOICE APPENDAGE DIAL (horns / antlers / hooves / breasts) ═══
The reusable SKILL for dialing a named NPC's extra capsules live by voice — Yvanni's horns + hooves +
breasts and Auri's antlers were all shaped this way. The VALUES land in PPB_tuning.txt (then her npcCap
lines / her NIF); THIS is the METHOD: the command grammar, the per-bone frames, the discipline. The
*finalization* (making a dial permanent for one NPC) is its own decision tree — Report 10 §9.

### The loop
Scope the gate to ONE actor (a `sculpt` line — Report 10 §8), knobs hot-reload ~1 Hz. The user speaks a
move in WORLD terms ("front point, back 1.5u, up .3u"); Claude edits that capsule's endpoints in
PPB_tuning.txt; it applies within a second; the user eyeballs and speaks the next move. Every move is a
delta FROM THE CURRENT STATE (the standing VR-calibration rule) — read the live values, apply the delta,
never an absolute.

### ★ Commands are WORLD-space; each capsule is written in its BONE's LOCAL frame
"forward / up / right" = what the user SEES in VR. Each cap* slot lives in its bone's local frame, so a
world delta must be mapped to local before it's written. Determine a bone's frame ONCE (the `world_rot`
dump — which local axis maps to world fwd/right/up), then:
- **Head / spine2 — AXIS-ALIGNED.** local +X = world-right, +Y = world-forward (toward the face),
  +Z = world-up. Write world deltas straight in. (Breasts protrude +Y on spine2; horns/antlers sit +Z.)
- **Calf — ROTATED, and it bites.** local +Y = world-forward (+0.99), local **+Z = world-DOWN** (so
  "up" = −Z), local **+X = world-LEFT** (so "right" = −X). The ANKLE end (B) is the high-Z/low end, the
  KNEE end (A) is the −Z/high end. Restate "up = toward the knee = local −Z" before every calf move.
- **Foot — TILTED ~35°, no axis lines up.** Project every world move: **Δ_local = Rᵀ · Δ_world** (R = the
  foot bone's world rotation), computed per-move. The three SOLE capsules (main + C1 + C2) are "the foot";
  C3 is the ankle-lock — leave it unless named.
- **capMirrorL auto-mirrors R-side LIMB edits to the left** (calf/foot) → dial only the RIGHT. CENTERLINE
  slots (head, spine2, COM) are NOT auto-mirrored: dial the right children, then "mirror CX on CY" the left.

### The command grammar (as the user speaks it)
- **"whole capsule" + dir** → translate BOTH endpoints. A BARE direction (no subject) → the capsule/point
  currently in FOCUS (the sticky-focus rule above; focus moves only when a new subject is named).
- **"front point / back point"** = endpoints by world-Y (front = +Y, toward the face). **"left / right
  point"** = by world-X (used when a capsule lies near-horizontal). **"top point / bottom point" = the
  GEOMETRIC higher-Z / lower-Z end — NOT creation order, NOT the A/B labels.** ⚠ As a capsule is dialed one
  endpoint can cross the other (B curls below A); the geometric labels then INVERT. Re-derive
  top/bottom/front/back/left/right from the CURRENT numbers EVERY command, and STATE your reading back
  ("top = A, the high end") so a wrong read costs one word, not a wasted VR dial cycle. (User's exact
  correction when this slipped on creation-order: *"come on, focus."*)
- **"shorten Nu / extend Nu"** = move the FOCUS endpoint N along the capsule's OWN axis toward / away from
  the other end (ONE end, never symmetric — the standing rule).
- **"radius +N / −N / N"** = delta or set the radius.
- **"front-to-back orientation" / "up-down orientation"** = RE-ORIENT the capsule to lie along that world
  axis (Y or Z), keeping its CENTER and LENGTH, discarding the previous direction — used to reset a seed's
  axis before shaping. **"Nu in length" / "length Nu"** = set the length along the current axis, centered.
- **"mirror CX on CY"** = write CX = the X-NEGATION of CY (both endpoints X-negated; Y/Z/R copied) — the
  left twin of a centerline appendage. **"copy CX on CY"** = CX becomes an exact copy of CY, THEN the focus
  moves to CX (so the next bare correction dials the new segment — the way each next horn/antler segment is
  spawned, often sharing an endpoint with its parent).
- **"first capsule" = C15** for a horn/antler dial (the head seed children are C15..C22; "C17" = the third).
  NOT C1 — C1..C14 are the baked human skull.

### Discipline (this cost real user trust TWICE this session)
EVERY move must be an ACTUALLY-APPLIED Edit, and the reply reports what the TOOL returned — never a
pre-written "C4 → (…)" result narrated without calling Edit. Twice a described-not-applied move desynced
the file from the reported state and the next Edit failed to match (*"You didn't applied the back 12u…
second time you do that"*). Read the live values → apply the delta → report the tool's actual result.
