# 02 — Tool & Knob Reference (how to operate the workshop)

> **★ 2026-07-07 SPLIT (report 08)**: everything below now lives in **PPB.dll** (own mod), not
> AIHands. New paths: tuning = `D:\Games\My Skyrim\mods\Precision Physic Bodies\SKSE\Plugins\PPB_tuning.txt`,
> log = `My Games/Skyrim VR/SKSE/PPB.log`, statue spell = `Precision Physic Bodies.esp` "PPB Statue
> Toggle" (`help PPB 4 SPEL` → `player.addspell FExxx803`; the old AIHands FE700803 is a no-op stub).
> trf/trg live in `collviz_markers.dll` inside the Collision Visualizer VR mod (colors in its own ini —
> no MO2 reorder needed). Build: `tools/PPB-plugin` / `tools/CollVizMarkers-plugin`, same cmake preset.
> NEW knobs: `pivWristUpU … pivAnkleUpU` (11, game u, world-Z lift added on top of auto-seat targets).
> **NEW console `statue` (2026-07-07): select an NPC + type `statue` = toggle the bind-pose calibration
> statue with NO spell/ESP needed** (donor: first of ShowScenegraph/TestAllCells/ToggleWaterSystem/
> ShowQuestStages found; same referenceFunction=false + selected-ref recipe as trf/trg). The A-pose
> spell still exists as a backup but the console command is the primary, ESP-free path. NOTE: console
> `statue` does the rotation-stomp only (no SetDontMove/package-override that the spell's Papyrus added
> — fine for a stationary calibration NPC; if she wanders/warps, use the spell or dismiss her as follower).
> Two-Documents-folder gotcha: logs go to `C:\Users\mad72\OneDrive\Documents\My Games\Skyrim VR\SKSE\`
> (OneDrive-redirected), NOT the plain `C:\Users\mad72\Documents\...` one.
> Everything else below (knob names, field order, console recipes, log markers) is UNCHANGED.

Everything runs through **the tuning file** + **console commands** + **the log**. The DLL re-reads the
file at ~1 Hz even while idle; a knob edit lands in-game in ≤1s. The user dials BY VOICE in VR;
Claude edits the file. Runtime switches must be IN-GAME (spell/console) — never ask the user to edit files.

## The tuning file
`D:\Games\My Skyrim\mods\Precision Physic Bodies\SKSE\Plugins\PPB_tuning.txt` — `key value` lines, `#` comments.
A knob change bumps the generation counter (32→167-float snapshot memcmp) → per-actor re-apply.
NOTE: only knob VALUE changes bump the gen — comment edits do nothing. Forcing a re-apply = nudge any
value by an invisible epsilon (e.g. a radius by 0.001).

### Capsule slots (8 floats each, ORDER IS LOAD-BEARING: Enable,AX,AY,AZ,BX,BY,BZ,R; game units,
### child-body-local; A/B = endpoints, R = radius)
| Slot | Keys | Body | Notes |
|---|---|---|---|
| 0 | capHand* | NPC R Hand [RHnd] | INERT since the 3-capsule bake (shape is a list, not a capsule) |
| 1 | capFore* | NPC R Forearm [RLar] | endpoints AUTO-FIT owned |
| 2 | capUpper* | NPC R UpperArm [RUar] | endpoints AUTO-FIT owned |
| 3 | capHead* | NPC Head [Head] | manual; stock = near-degenerate ball r8.2 |
| 4-7 | capSpine0/1/2*, capNeck* | Spn0/Spn1/Spn2/Neck | endpoints AUTO-FIT owned |
| 8-9 | capThigh*, capCalf* | RThg/RClf | endpoints AUTO-FIT owned |
| 10 | capFoot* | NPC R Foot [Rft ] | manual (no toe joint); note trailing space in bracket |
| 11 | capCom* | NPC COM [COM ] | manual (hip-to-hip axis); trailing space in bracket |
Hand plate children (the baked bhkListShape): `capHandC1*` center / `capHandC2*` thumb / `capHandC3*`
pinky — same 8-float shape, live-tunable; writes also repatch the list's cached union AABB.

### Joint pivot slots (4 floats: Enable,X,Y,Z — ABSOLUTE child-body-frame position, game units)
Index order (code + logs): 0 wrist, 1 elbow, 2 shoulder, 3 spine0, 4 spine1, 5 spine2, 6 neck,
7 head, 8 hipR, 9 kneeR, 10 ankleR. Keys: pivWrist*, pivElbow*, pivShoulder*, pivSpine0/1/2*,
pivNeck*, pivHead*, pivHip*, pivKnee*, pivAnkle*.
With auto-seat active these knobs are COLD-START FALLBACKS ONLY — the statue measurement overrides
and persists (sticky). Manual dialing still works when pivAutoSeat=0.

### Mode switches
- `pivAutoSeat 1` — statue-gated auto-placement of every joint ball onto its XP32 bone origin;
  measurement is STICKY after the statue drops. Covers ALL 11 joints incl. disabled slots.
- `pivClavFollow 1` — per-frame R-shoulder chest-anchor riding the live clavicle chain (unbakeable).
- `capAutoFit 1` — 8 capsules span ball-to-ball from the seated joints (radius stays the knob;
  the gen sweep writes radius-only for auto-fit-owned slots).
- `heelFix 1` — startup default for the heel fix (console `hf` = live toggle).
(Plus the entire AIHands hold/give knob set in the same file — not PPB's concern; don't disturb.)

## Console commands (hijacked vanilla slots; ZERO-ARG only — args die in the console compiler)
| Cmd | Donor slot | Does |
|---|---|---|
| `hf` (HeelFix) | TestSeenData | toggle heel fix |
| `capfix` | DumpNiUpdates | selected NPC: dump R-hand capsule (args path is DEAD — file loop instead) |
| `trf` (BoneViz) | ToggleMagicStats | selected NPC: toggle RED 1u cubes on every "NPC "-prefixed bone (96) |
| `trg` (CbpcViz) | ToggleCombatStats | selected NPC: toggle GREEN CBPC zone markers (real config replica) |
Registration recipe that WORKS: referenceFunction=false + zero params + RE::Console::GetSelectedRef().
KNOWN-DEAD combos: any params (compiler eats the call), referenceFunction=true + zero params (same).

## The statue spell (the calibration stance)
`player.addspell FE700803` (FE700802 is the MGEF — addspell on it = the console's "Script error").
Cast on NPC = statue ON: DLL stomps every bone ROTATION to referencePose per frame (rotations ONLY —
full transforms broke fingers vs the 0.85 hand scale) + MQ105DoNothing package override + SetDontMove
(the AI-warp teleport defense). Recast = OFF. Naked test spell by user rule — no watchdogs.
The statue = the runtime bind pose = auto-seat's measurement window = the honest audit stance.

## Visualizers (all render THROUGH Collision Visualizer VR — `trd` must be on; three layers stack)
- `trd` = collviz (their mod): Havok shapes + blue constraint balls. Rebuilds tessellation ONLY on
  toggle → after any geometry change, toggle trd off/on. NEW shapes appear without toggling (lazy).
- `trf` = XP32 bones (RED 1u cubes, layer 60). `trg` = CBPC zones (GREEN spheres, layer 61; v2 scans
  all CBPCollisionConfig*.txt, evaluates IsFemale/IsRaceName/ActorName/IsUnique + AND/OR/NOT, picks
  highest passing Priority — the log prints every file's verdict; unsupported conditions eval false).
- Colors: AIHands ships a collviz_vr.ini override mapping 60:ff0000, 61:00ff00 — but collviz's own
  copy OUTRANKS it in MO2 (user action pending: drag collviz below AIHands, or edit its ini). White
  markers = the conflict, not a bug.
- Markers are keyframed boxes with bare layer 60/61 + the ACTOR'S OWN system group + bit 15.
  `NeutralizeMarkerLayers()` zeroes rows 60/61 in the engine's 64×64 layer table at DataLoaded
  (undefined rows default 0xFFFFFFFF = collide-with-everything — the 07-05 explosion). The group
  stamp defeats the charController filter-bypass branch (the second explosion). NEVER bit 14 —
  collviz main.cpp:1311 refuses to draw non-collidable bodies.

## Log markers (`AIHands.log`, truncates each launch)
- `PivFix STRAIN <id> arm W/E/S | spacing W-E(16.0) E-S(22.8)` + `core s0..head | legR ...` — 2s meter:
  strain = the joint's two stored copies' world disagreement (healthy ≤0.1u); spacing = pivot-pair
  distances vs true bone lengths.
- `PIVTRACK <id> ball-vs-bone(u): ...` — THE instrument: per-joint distance Havok ball ↔ XP32 bone.
  ~0 = synced. Uniform offsets across a chain = displacement entering upstream (lever/clavicle class).
- `PIVBAKE <id> <joint> <type> childIsA=N child=[..] other=[..] (havok m)` — full-precision bake feed,
  printed at every write. THE numbers the NIF bake consumes (take them from a STATUE-time line).
- `PivFix APPLIED/HEAL/DEFERRED/REPLACED` — write, self-heal (factory reset detected), scramble skip
  (>40u/s), constraint instance swapped (PLANCK converts hinges→ragdoll constraints at add).
- `CapFix FILE-CHANGE detected -> gen N` — the DEFAULT-level receipt that a tuning edit was picked up
  (armed a new generation). This is what you watch in a normal dial session.
- `CapFix BEFORE/APPLIED/AUTOFIT/MIRROR-L` — per-slot capsule discovery/writes/ball-to-ball fits.
  **2026-07-13 (build 5f4b0ce8): these are now `debug`-level and INVISIBLE at the default log level**
  (they were the ~85%-of-log spam a busy scene produced). To see them live during a dial session, set
  `logVerbose 1` in `PPB_tuning.txt` (~1 s to apply; flips the whole logger to debug). The MEASURED
  effScale / stood-up / `body identity changed` self-heal lines STAY at info — those you always see.
  `VIZ trf/trg ON/OFF`, `trg cfg '<file>' prio pass zones` — visualizer receipts.
- `STATUE ON/OFF`, `HEELFIX`, `PivFollowShoulder FIRST APPLY`, `Marker layers BEFORE row60=...`.

## Build & deploy
- C++: `cd G:/Claude Workspace/tools/AIHands-plugin && VCPKG_ROOT=C:/vcpkg "<VS2026 cmake>" --build
  build/vr --config Release` — POST_BUILD auto-copies AIHands.dll into the AIHands mod (FAILS while
  the game runs — only build with the game closed; the error is loud).
- Papyrus: `powershell -File G:/Claude Workspace/tools/aihands-scratch/compile_test.ps1 <ScriptName>`
  (Caprica; outputs straight into the mod's Scripts/). Recompile AIHands_Native.pex too when adding natives.
- Skeleton NIF (the ship artifact): `D:/Games/My Skyrim/mods/Precision Physic Bodies/meshes/actors/
  character/character assets female/skeleton_female.nif`.

---

## ★ KNOBS & COMMANDS ADDED 2026-07-07 → 2026-07-09 (post-flesh-fit / bake / diagnostics)
All in `PPB_tuning.txt` (~1 Hz hot-reload) unless noted. The DLL applies these as ABSOLUTE values every
frame on top of the baked NIF, so a knob value == the baked seed = identity (no jump).

### Per-child list-capsule knobs (the sub-capsule editors)
- `cap<Slot>C<N>Enable / AX AY AZ BX BY BZ R` — drives child N of a body that was baked into a bhkListShape.
  Child 0 = the MAIN capsule, still driven by the plain slot knobs (`capThigh*`, `capCom*`, `capHead*`…).
  Slots + counts as baked (wave-2b): capUpperC1, capForeC1, capThighC1-5, capCalfC1-3, capFootC1-2,
  capHandC1-3, capSpine0C1-7, capSpine1C1-6, capSpine2C1-12, capComC1-20, capHeadC1-10.
- Graceful-skip: if a body isn't a list or child N doesn't exist, the DLL logs once and skips (never CTD).
- Adding/removing children is a BAKE (structure change) — knobs only move/resize the children that exist.

### Global / system knobs
- `capMirrorL 1` — every capsule slot/child write also replays on the LEFT twin body (X-negated, same r).
  Centerline bodies (COM/spine) are excluded. Lets you dial the RIGHT side and see both live pre-bake.
- `pivGrabGate 1` — pauses the PivFix re-seat/heal while the NPC is HIGGS-grabbed (fixes the grab-bounce).
- `fingerCapTrack` / `fingerCapR` — per-frame finger-capsule endpoint tracking (present, default 0;
  NOT needed for the raw finger test — a follower capsule node-follows the finger bone on its own).
- `lodSpike` (default 0) — gates the `capdis` disableChild spike.

### Console commands (donor-hijack; zero-param; select an NPC first where noted)
- `hf` / `capfix` / `statue` — heel-fix toggle / capsule discovery-log / bind-pose stomp (as before).
- `probe` — dumps the SELECTED actor's full physics+animation state to PPB.log (per-ragdoll-body motion
  type/velocity/worldZ, IsInRagdollState, knock/life/sitsleep/killmove, bAnimationDriven, furniture,
  charController support). USE ON A FLOATING/STUCK NPC BEFORE A-posing it (A-pose destroys the evidence).
- `perf` — arms/dumps the read-only CONTACT-POINT counter + physics-step timer. Press once (arm + census),
  run the scenario, press again (report to PPB.log). Reports maxPointsInOnePair (⚠ ACCUMULATOR = inflated
  upper bound; the TRUE per-frame peak needs the `contactPointCallbackDelay=0` build), step ms, STEPS/FRAME.
- `capdis [slot] [mask]` — the gated disableChild spike (needs `lodSpike 1`); default `capdis 8 56` disables
  R-Thigh children 3/4/5. Proves live child on/off works; de-prioritized (contact overflow is benign).

### The 816640→817152 diagnostic/wave-2b DLL is the current deployed build (has all of the above).

---

## ★ KNOB FAMILIES ADDED 2026-07-15 → 07-17 (authoritative comments live in Tuning.h + the tuning file)

- **Tail/garment**: `npcTailMassKg` (tbl 1/2 capsule mass, file=4.0) · `npcSilentVelMS` (silent-contact
  drive cap, DEFAULT 0 OFF — the 07-15 regression cause, see Report 15 post-mortem) · `npcGarmentMat`
  (touch-SOUND material 0 skin/1 cloth/2 snow/3 grass DEFAULT/4 none — the "body-fall thud" fix; applied
  at rig CREATE, re-dial = flip `npcFollower` 0→1) · `fsmpMassScale` (push force × boneMass/0.4 — the
  tip-jitter fix; 0 = raw) · `fsmpPushMult` (global push multiplier, 1.0).
- **Finger curl** (contact-driven finger conforming, Report 15): `npcFingerCurlGain`/`Decay`/`Max`/
  `Mode` (0 additive / 1 grasp-override) / `CurlLagGate` (chord-speed gate vs arm-swing false curls).
- **Finger collision widening** (DEFAULT OFF, live A/B): `npcFingerVsClutter` / `VsWorld` (statics =
  infinite-mass flicker risk) / `VsNpc`. ⚠ These three are read via the `g_filterKnobs` integer cache —
  NEVER add a float read inside FilterDecision (the movaps CTD, Pitfall Ledger 2026-07-16).
- **NEW CONFIG FILE — `PPB_skeletons.txt`** (same folder): the runtime skeleton map (`race` lines) +
  per-NPC capsule overrides (`npcCap` lines). ⚠ Parsed ONCE at kDataLoaded — needs a game RESTART per
  change, unlike PPB_tuning's 1 Hz hot poll. Receipts: `SKELMAP:` log lines. Full spec: Report 22.

- **ReShape body-shape knobs** (`mesh*`, `bodyScale*` — the measured body-shape fitting layer): full
  table + meaning in **Report 21 §9**. In brief: `meshShape` (master), `meshBand<Region>Lo/Hi` +
  `meshArmFrac` (measurement bands), `bodyScale<Region>` (per-region gain), `meshRadiusGain` (global
  trim), `meshShift*K` + `meshCupFrac` (breast/cheek translation & cup radius), `meshNeutral*`
  (captured Softbody neutrals), `meshMarkers` (eye-verification ghosts). OBody/SKEE morph reads and
  the slider fallback = **Report 23**.

## Dismemberment / decapitation compat (`dg*`) — full mechanisms in **14**
The DF/NGD/PLANCK layer. Live-editable like everything else; the two ⛔ knobs are abandoned
approaches kept only so their default stays OFF.

| knob | default | meaning |
|---|---|---|
| `dgEnable` / `dgLog` | 1 / 1 | master + per-action log lines |
| `dgDeathCut` | 1 | PPB asks DF (public API, main thread) to dismember once death is confirmed — it replaces DF's own worker-thread death-confirm and **requires DF `bDeferredHitProcess = 0`** |
| `dgDeathCutDelayS` | 0.20 | delay after death before asking |
| `dgDeathNodeTries` | 1 | limb nodes offered per death (1 = DF's natural one-consideration feel) |
| `dgHitLocated` | 1 | sever the limb nearest the killing blow (measured to the limb SPAN, not the joint origin) |
| `dgHitMaxDistU` | 0 | accuracy gate re-creating DF's proximity radius. **0 = off**: the post-hit weapon-node position proved unreliable (85–632u readings) |
| `dgHeadSkel` / `dgHeadSkelHoldS` | 1 / 3.0 | swap the race skeleton to `PPB\<skeleton>_head.nif` while a severed head loads its 3D, then restore (observed window ≈70 ms) |
| `dgHeadPark` | 1 | park the head clone's other bodies ON the COM anchor + disable its constraints, so nothing orbits or drags the head |
| `dgHeadTrack` | 1 | log head creation at NGD Step01/Step02 (research instrument) |
| `dgGraceDeadS` | 15 | how long a fresh corpse stays PLANCK-ignored |
| `dgCloneStrip` | **0** | ⛔ legacy runtime capsule collapse — **leave 0**, it mutates SHARED shapes (Pitfall Rule 0) |
| `dgDeferDf` | **0** | ⛔ abandoned ProcessDismemberment hook — leave 0 (see 14 §4d) |

**Sibling-mod ini overlays PPB ships** (its MO2 priority 49 beats DF 164 / NGD 166 — the same trick
as the HIGGS override): `SKSE/Plugins/DismemberingFramework.ini` with **`bDeferredHitProcess = 0`**
(required — DF's worker thread is the VR freeze) and `NextGenDecapitations.ini` (verbosity;
`fHeadMass`, `iScalesUpdateFrequencyFactor` live here too). ⚠ Delete these overlays if PPB ever stops
shipping the `dg*` layer, or DF loses its death-confirm with nothing replacing it.

## Collision SOUND selectors
`npcBodyMat` (the 18 body capsules) · `npcGarmentMat` (hair/tail/cloth rigs). Values:
**0** skin/NIF (the loud body-thud) · **1** cloth · **2** snow · **3** grass (soft rustle) ·
**4 = NONE → true silence** (the impact lookup finds no material and plays nothing).
⚠ Before changing a material, check WHAT is actually rigged: a 2026-07-27 "awful collision noise"
turned out to be the auto-pinned **finger rig** (`npcFingerEnable` left at 1) teleporting ~40×/min on
an idle NPC's hand — not the body material at all.

## PivGuard — the per-actor PLANCK pivot-collapse split (2026-07-29, SHIPPING)
PLANCK's `loosenRagdollConstraintPivots` collapses every ragdoll joint pivot to the anim pose each
frame — the band-aid the 2011 skeletons need, and the thing that un-calibrates PPB's baked joints
(ankle +6u/−3u, shoulders 2u inward, user-measured). PLANCK has NO per-actor settings, so PPB
synthesizes one: the pre-drive hook wraps PLANCK's, sets the flag to 0 for a PPB-skeleton actor's
own drive, restores it after the chain. A guard captures each fresh ragdoll's baked pivots
(fresh = baked by construction) and writes back any pivot found collapsed (~2 Hz, shoulders
excluded — clavicle-follow owns them; PIVRESCALE/descale invalidate the capture).

| knob | default | meaning |
|---|---|---|
| `planckLoosenOurs` | 1 | the whole system; 0 = PLANCK stock everywhere |
| `poseConform` | 1 | REQUIRED for planted feet: bodies land on the XP32 nodes, PLANCK's foot-IK plants the visible feet where the ragdoll feet are. Gated to PPB skeletons. |

**The shipping trio** (verified together 2026-07-29): global loosen **1** (stock, no override
file), `planckLoosenOurs 1`, `poseConform 1`. Receipts: `PIVGUARD diag … getOk=1 setOk=1`,
zero heals, `PIVARC` node-vs-havok arc within 0.25%.
⚠ vtable law when touching the PLANCK interface decl: copy the BASE interface order
(planckinterface001.h) — Get/SetSettingDouble are slots 13/14 AT THE END. See ledger.
