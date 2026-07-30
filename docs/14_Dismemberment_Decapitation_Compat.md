# 14 — Dismemberment & Decapitation Compatibility (DF · NGD · PLANCK · FSMP)

**Module of the Precision Physic Bodies technical reference.**
Scope: making Dismembering Framework (DF) and Next-Gen Decapitations (NGD) work in VR alongside
PLANCK-driven ragdolls — the hard-frozen-game bug and its fix, the severed-head architecture, the
head-only skeletons, and the physics field map (mass/inertia/friction) that came out of it.
**Audience: an engineer with zero prior context.** Everything below was reverse-engineered from the
shipping DLLs (both carry full PDBs) and verified in-game, 2026-07-26/27.

Companion docs: `01` (pitfall ledger — the laws below are repeated there), `12` (older
dismemberment/ragdoll-sync research), `10` (runtime skeleton map — the mechanism the head swap
rides), `02` (knob reference).
Working notes with the raw evidence: `tools/ppb-scratch/dismember_investigation.txt`.

---

## 0. TL;DR — the four things that matter

1. **THE HARD FREEZE WAS NEVER PPB.** It is a deadlock on the **SKSE task-queue lock** between DF's
   worker threads and a main thread stuck in the FSMP-hooked skinning pass. It predates PPB. Proven
   with 5 full-process dumps, our code progressively removed.
2. **HAVOK SHAPES ARE SHARED BETWEEN ACTORS OF THE SAME SKELETON. BODIES ARE NOT.** Editing a
   capsule *shape* at runtime for one actor edits it for **every** actor on that NIF. Editing a
   *body* (mass, transform, motion type, collision filter, constraints) is per-actor and safe.
   This one law explains a whole evening of self-inflicted damage.
3. **NGD's "severed head" is a full clone ACTOR** of the victim (same race → our full 130-capsule
   body bake), not a head prop. It is fixed by giving it a **head-only skeleton file**, not by
   mutating its shapes.
4. **A COM body carries the WHOLE ACTOR's mass** (40 kg, friction 2.5). Move head collision onto it
   without copying the head's physics and the head behaves like a welded corpse.

---

## 1. The freeze — root cause, and why it looked like "NPC kills NPC"

### Symptom
Hard freeze (not a CTD — no exception, no crashlog), always on **NPC-vs-NPC** kills, never on the
player's own kills. Predates PPB by the user's account.

### The dumps (procedure worth reusing)
Capture a *live* hang — a frozen game still holds every thread stack:
```
rundll32 C:\Windows\System32\comsvcs.dll, MiniDump <pid> C:\path\freeze.dmp full
icacls C:\path\freeze.dmp /grant "Everyone:(R)"        # dump ACL blocks the store-app debugger
cdbX64.exe -z freeze.dmp -y "srv*<symcache>*https://msdl.microsoft.com/download/symbols;<mod PDB dirs>" -c "~*k 14; q"
```
WinDbg installs via `winget install Microsoft.WinDbg`; `cdbX64.exe` lands in
`%LOCALAPPDATA%\Microsoft\WindowsApps`. DF, NGD **and** PLANCK all ship PDBs next to their DLLs —
point the symbol path at those folders and every frame resolves by name.

### The verdict (identical in all 5 dumps)
- **Main thread**: parked in `WaitForSingleObjectEx` at `SkyrimVR+0x57cbcf`, reached through the
  armor-attach/skinning path with `hdtsmp64` (FSMP) in the chain — i.e. mid death-frame gear attach,
  **holding the SKSE task-queue lock**.
- **DF worker threads (3 of them)**: blocked in `RtlEnterCriticalSection` inside
  `sksevr!SKSE::TaskInterface::AddTask`, called from `WaitAndCall<ProcessHitTemplate>`,
  `WaitForGameReady<PlaceLimbCenter>` and `DelayedToggleCollisionLayers`.
- Neither side can proceed. Deadlock.

### Why player kills survive and follower kills freeze
A player kill resolves during the player's own weapon contact on the main thread, so the death-frame
attach/skinning has **already settled** by the time DF's worker calls `AddTask`. An NPC-vs-NPC kill
lands during the AI/animation phase, while the victim's gear attach and FSMP skinning are still in
flight holding the lock. Same code, different timing window. (User's observation — it is what
cracked the case.)

---

## 2. Dismembering Framework — reverse-engineered internals

Source path baked into its PDB: `F:\VS_Modding\DismemberingFramework-legacy\src`.

| symbol | RVA (v1.2.2) | what it is |
|---|---|---|
| `Events::MainEvent::ProcessHitTemplate` | — | the hit hook; **branches on `bDeferredHitProcess`** |
| `Events::MainEvent::ProcessHit` | 0x567C0 | synchronous path; calls ProcessDismemberment, returns its bool |
| `Events::MainEvent::ProcessDismemberment` | **0xAD200** | the real cut (logs "request" at Main.cpp:17, "Dismembered actor" at :130) |
| `Events::MainEvent::CutLimb` | 0xAA400 | places the severed limb, blocks activation, renames |
| `Events::MainEvent::ProcessDeferredHit` | 0x55AB0 | deferred-hit map walk; dispatches through an **indirect vcall** |
| `SettingsIni::bDeferredHitProcess` | 0x2601E0 | the threading switch |

### The threading switch (the crux)
```
ProcessHitTemplate+0x169:
    cmp byte [SettingsIni::bDeferredHitProcess], 0
    jne  -> call WaitAndCall<...>          ; spawns a RAW std::thread (1-frame death-confirm)
    else -> mov rcx,[_ProcessHit]; call    ; SYNCHRONOUS, on the main thread
```
- `= 1` (default): the whole dismemberment — including **scenegraph node detach** — runs on a raw
  worker thread. That worker also calls `SKSE AddTask`, which is where it deadlocks in VR.
- `= 0`: no worker at all → **the deadlock is structurally impossible**. Cost: DF loses its
  death-confirm, so most *kill* severs miss (it evaluates before the actor is flagged dead);
  cutting an already-dead body still works.

### The gates inside ProcessDismemberment (why replayed calls fail)
After the "request" log the flow is:
`IsDismemberable(actor)` → `TestActorTypes(cond, victim, **aggressor**)` → `TestWeaponType(cond, **hitData**)`.
Both read **live aggressor/weapon state**, so a call replayed a frame later with copied arguments
logs the request and then silently declines. This killed the "copy the args and re-issue" approach.

### Node selection is POSITIONAL, not weighted
`ValidActorNodes(..., Actor*, NiPoint3*, float, bool)` takes a **position + radius**. The JSON
templates (`OfficialHumanoidPack_*_LIMBS.json` + `*_TEMPLATES.json`) carry **no chance values** —
only `RefNode`, `BipedSlot`, `LimbArtObject`, `Translation`. So "it always cuts a leg" is a
consequence of *where blows land*, not a fallback or a weighted roll. Humanoid node set is exactly:
`NPC Neck [Neck]`, `NPC L/R Forearm [L/RLar]`, `NPC L/R Calf [L/RClf]`.

### DF's public API (the sanctioned entry — use this, not hooks)
Handshake: `Dispatch(byteswap('DF'), &g_API, sizeof(void*), nullptr)` at kPostLoadGame/kNewGame.
vtable order: `GetVersion / Dismember(target,node,aggressor,weapon,params) / IsDismembered /
IsDismemberedNode / PostDecapitate / RefreshActorDismemberedState`.
`params == nullptr` ⇒ `forceExecution = false` ⇒ **DF still applies all of its own conditions**.

---

## 3. Next-Gen Decapitations — reverse-engineered internals

| symbol | RVA (v1.4.3) | note |
|---|---|---|
| `Events::MainEvent::OnDecapitate_Step01` | **0x49F00** | calls the ENGINE's `Actor::Decapitate()`; **returns the head ref** |
| `Events::MainEvent::OnDecapitate_Step02` | **0x4AFA0** | receives `(victim, HEAD, u32, bool, params)` |
| `Events::MainEvent::ProceedDecapitation` | 0x4B950 | ⚠ prologue is NOT 14-byte-stealable |
| `ModUtils::SetScalesAndCollisions` | 0x4CA80 | **periodic** (DoFor+WaitAndCall, `iScalesUpdateFrequencyFactor`) — re-inflates anything you shrink |
| `RE::Actor::Decapitate` (NGD's wrapper) | 0x95540 | thin wrapper over the engine function |

### The decap chain
`DF severs "NPC Neck [Neck]"` → `NGD Step01` → **engine `Actor::Decapitate()` creates the head** →
Step01 arms `WaitUntilRagdollReady` → `NGD Step02`.
The engine's own `DecapitateHandler` (`RE::VTABLE_DecapitateHandler`, slot 1 `ExecuteHandler`) is
**NOT in this path** — hooking it yields zero fires. Verified in-game.

### The head is a full clone ACTOR
`probe` on a severed head: `life=2(Dead)`, `IsInRagdollState=true`, **18/18 rigid bodies**, 48
collision objects, `race = <the victim's race>`, skeleton = **PPB's** `skeleton_female.nif`.
It inherits our full body bake purely because **it shares the victim's race**.
NGD then equips `Decapitated_Head_ARMO`/neck-stump armor, replicates head gear, applies impulse
(`fHeadImpulseFactor`), and **repositions `NPC COM [COM ]` to `fCOMNodeZPosition` (65)** — which is
why *the head mesh renders at the COM anchor*.

### NGD's public API
`Dispatch(byteswap('NGD'), ...)`; vtable: `GetVersion / Decapitate / IsDecapitated / **IsHead**`.
`IsHead(actor)` is the authoritative "this actor IS a severed head" test — replaces every heuristic.

### ★ The interception window (measured, not assumed)
At **Step01's RETURN** the head actor exists with **`Get3D() == nullptr`, 0 bodies** — it loads its
skeleton *afterwards, from its race*. By **Step02** the 3D is built (48 bodies) and it is too late.
That window is the whole basis of the head fix.

---

## 4. The as-built solution

### 4a. Dismemberment without the freeze (`dgDeathCut`)
- DF runs with **`bDeferredHitProcess = 0`** (ini overlay in PPB's folder — MO2 priority 49 beats
  DF's 164). No worker ⇒ no deadlock, ever.
- PPB supplies the death-confirm DF lost: a **TESHitEvent sink** records `{aggressor, weapon,
  hit-located node}` per victim; **TESDeathEvent** schedules a request `dgDeathCutDelayS` later; the
  main-thread sweep calls **`g_df->Dismember(victim, node, aggressor, weapon, nullptr)`**.
- **DF keeps full authority** — its conditions, chance, armour class, sounds and limb physics all
  run. PPB only says *when* and *which limb to consider*.
- **DF-first guard**: if `IsDismembered(victim)` is already true (DF's synchronous path took a limb
  ~16 ms before our death event), PPB stands down rather than removing a *second* limb.
- ⚠ **`ev->dead` NEVER FIRES in this setup** — only the dying stage (`dead=false`) arrives. Gating
  on the final stage means the feature never runs. Arm on any death event.

### 4b. Hit-located limb choice (`dgHitLocated`, `dgHitMaxDistU`)
TESHitEvent carries no position, so reconstruct one **at hit time** while the geometry still means
something: the **aggressor's weapon node world position** is the impact point. Measure to the
**limb SPAN** (bone→bone: calf→foot, forearm→hand, neck→head), not the joint origin — a joint origin
sits at the *top* of the limb, so nearest-origin misjudges a mid-shin blow.
`dgHitMaxDistU` re-creates DF's proximity requirement (its picker has a radius; its API does not),
so a blow landing near no limb severs nothing. **Field caveat:** measured distances came out at
85–632u (absurd) — the post-hit weapon node is not reliably at the impact point. Gate currently
**0 (off)**; a better source (sampling the weapon each frame and using the impact-frame sample) is
the open improvement.

### 4c. The severed head (`dgHeadSkel`, `dgHeadPark`)
1. **Head-only skeletons** (§5) ship in the mod: `PPB\<skeleton>_head.nif`.
2. At **Step01**, before the engine builds the head, PPB repoints that race's female skeleton to the
   matching `_head.nif`; the **Tick loop restores the real path the instant the head's `Get3D()`
   exists** (or on `dgHeadSkelHoldS` timeout). Observed window ≈ **70 ms**.
   ⚠ Residual risk: another actor of the same race loading 3D inside that window would also get the
   head skeleton. 3D building is main-thread, so the window is a frame or two.
3. **★ THE HEAD MUST BE MADE DYNAMIC.** An actor's ragdoll bodies are **KEYFRAMED** —
   animation-driven, which in Havok means **INFINITE MASS** — until something switches them to
   dynamic; normally that is PLANCK's `AddRagdollToWorld` → `ModifyConstraints`. But we
   **permanently PLANCK-ignore the head clone**, so nothing ever did, and the head could not be
   lifted *no matter what mass the NIF declared* (user: "it's like the head is the infinite mass I
   can't move it"). Keyframed bodies simply do not answer to force. Fix: drive the anchor through
   the **wrapper** call `bhkRigidBody::SetMotionType` (@`0xE08040`) to `MOTION_DYNAMIC` + 
   `bhkRigidBody::setActivated` (@`0xE085D0`) — the wrapper updates motion type, activation AND the
   collision filter together, which a raw field poke does not.
   Parked bodies get the same calls with `MOTION_KEYFRAMED` + `setActivated(false)` so the solver
   skips them: in the collision visualiser they read **green (asleep)** instead of **white
   (simulating)**. Only the head anchor should be white.
4. **`ParkBodiesAtAnchor`** (per-actor, safe): disable the head clone's constraints
   (`hkpConstraintInstance::setEnabled`, engine address **0xAC06A0**, PLANCK's idiom — the SDK
   symbol is not linked), then teleport every non-anchor body onto the COM, zero its velocities and
   set it `MOTION_KEYFRAMED`. Nothing orbits the head or drags it. Re-applied 6× because NGD's
   `SetScalesAndCollisions` runs on a timer.

### 4d. Things that DID NOT work (do not retry)
| approach | why it failed |
|---|---|
| `bDeferredHitProcess = 0` alone | no freeze, but kill-severs mostly miss (no death-confirm) |
| Hook ProcessDismemberment, copy args, replay next frame | gates re-read live aggressor/weapon ⇒ request logged, never severs |
| Hook it and marshal via **SKSE AddTask** | that lock **is** the deadlock — would block on the same critical section |
| Hook it and **block the worker** until the main thread runs it | lock inversion: the worker holds a DF lock the main thread then needs; every handoff hit the 500 ms timeout = a visible stutter |
| Shrink the head clone's capsules at runtime | **shared shapes** — stripped collision from every live NPC of that race |
| Hook the engine `DecapitateHandler` for head tracking | not in DF/NGD's path; zero fires |

---

## 4e. The 2026-07-28 round: crash + grab (three fixes in one build)
- **CTD `crash-2026-07-28-18-05-52`** (PPB.dll, `BodyScaleLatch → HeadVertDepth`): ReShape's head
  measure walked the FACE mesh of a decapitated corpse — on the decap frame the engine destroys
  that geometry, so the `BSDynamicTriShape` found a moment earlier is FREED-BUT-NONNULL and every
  pointer guard passes on garbage. No check can win that race. Fixes: (a) `BodyScaleLatch` skips
  dead/`IsExcluded` actors entirely (a generation bump re-latches EVERYONE, including old headless
  corpses — the broad window), (b) the leaf vertex walks are wrapped in **SEH `__try`** (the
  exact-frame race). PDB generation is now ON for Release (`/Zi` + `/DEBUG;/OPT:REF;/OPT:ICF` —
  REF+ICF forced back so the layout matches a plain Release and crash-log RVAs resolve; verified
  byte-exact against the crashed DLL).
- **Park blast radius**: the scenegraph body walk returns EVERYTHING under the actor root — the
  first park froze 44 bodies (a ragdoll has 18): ~26 were SMP wig/gear physics, keyframed into
  welded world anchors with 80 constraints cut. Rule: **only bodies on "NPC "-prefixed nodes are
  ours**, and a constraint is only cut when BOTH ends are ours (a wig chain rooted on the head
  bone keeps its root).
- **Grabbability**: DYNAMIC alone was not enough. HIGGS's cast selects any moveable body
  (`IsObjectSelectable`), but an ACTOR ref on the biped layer falls into HIGGS's looting/armor
  special cases, and the PLANCK-ignored clone's group is never registered grabbable. Fix: the
  anchor body is moved to **layer 4 (L_CLUTTER)** — HIGGS then runs its plain object grab path
  (pickable/throwable) while the head still collides with ground/statics/weapons.

### 4f. The grab-theft mechanism (2026-07-28 evening — the probe that settled it)
`probe` on a live severed head showed **all 18 bodies piled at one point** (the park's teleport
holds) but **every one of them DYNAMIC (motion 2/3)** — NGD's periodic pass restores motion types
faster than a 2 s re-assert, so keyframe+sleep never sticks. Consequence, straight from HIGGS's
source: the palm-triangle walk picks the first MOVEABLE body above the skinned bone — i.e. the
invisible, non-collidable, constraint-cut **head-bone ghost parked inside the head** (or an SMP
wig strand: kinematic off the skull, feather-light). Grabbing either moves nothing, because the
VISIBLE head rides the COM body alone. Weapons work because they can only touch the one COLLIDABLE
body — the anchor. HIGGS *did* grab (5 grab/drop pairs logged via the interface callback); it just
held a ghost every time.
**THE FIX: remove the 17 satellite ragdoll bodies from the Havok world entirely** (RemoveBody
primitive, ours-only — never the SMP/gear bodies). A body with `m_world == null` fails HIGGS's
walk unconditionally — no motion type NGD flips matters. Plus the hair strip (immediate-mode
unequip + RemoveItem, repeated over the first 8 passes because NGD replicates head gear at Step02,
seconds after our classify-time strip — queued-mode unequip on an AI-less clone NEVER executes).

### 4g. ★ THE ENGINE CREATES THE HEAD — AND NGD HAS SWITCHES FOR WHAT I WAS FIGHTING (2026-07-28)
Two corrections, both user-called, both expensive:

**(1) The severed-head actor is created by the VANILLA ENGINE, not by NGD.** `NGD::RE::Actor::Decapitate`
is a thin wrapper that resolves **REL::ID 36631** (VR and SE share it; AE 0x9307) and calls it.
`OnDecapitate_Step01`'s full call list contains NO object-creation call — no PlaceAtMe, no
CreateRefHandle — it calls the engine, arms WaitUntilRagdollReady, then DECORATES the result. So the
full-body actor clone (18 ragdoll bodies, victim's race + base) is the ENGINE's doing. Every attempt to
stop the body existing by editing NGD was aimed at the decorator, not the creator.

**(2) Do not fight a sibling mod with code when it ships a SETTING for the same thing.** Two of my
builds were unnecessary:
| what I wrote code for | NGD's own switch |
|---|---|
| unequip/remove the wig (queued unequip, immediate unequip, RemoveItem, 8 repeat passes) | `bReplicateHeadEquipment = 0` |
| keyframe/sleep/remove-from-world the 17 satellites, re-asserted forever | `bAdvancedNPCMaintenance = 0` ("ensures heads display correctly and do not reappear") |
Log proof my code lost: `18:08:11.364 PPB removed wig FE68480E` → `18:08:11.500 NGD equipped FE684807`
(a DIFFERENT form my slot query never saw); and `17 REMOVED from world` repeating on passes 28..37 —
i.e. re-added between every pass. **A removal that reports the same count every pass is not removing;
it is losing a race.** Other levers on the same file: `sExcludedRaces` (per-race FULL NGD bypass — the
controlled experiment for "what does vanilla decapitation alone give us"), `iScalesUpdateFrequencyFactor`,
`fHeadDespawnTimeout`, `fHeadMass`.
**RULE: before writing a line of interop code against a sibling mod, read its ini/API surface for a
switch that already does it.** PPB already ships the override file — the lever was one line away.

### 4h. ⛔ THE SEVERED-HEAD GRAB IS ABANDONED (2026-07-28) — status and what is left standing
**Not solved.** After the wig was eliminated as a cause (a Khajiit head with NO SMP hair behaved
identically), the grab still does not move the head. Everything in §4c–§4g is **switched OFF** and the
head-only skeletons are **deleted from the ship mod** (regenerate with `mkhead3.py` if ever resumed).
Knobs now 0: `dgHeadSkel`, `dgHeadPark`, `dgHeadStripHair`, `dgCloneStrip`, `dgHeadTrack`.
NGD's ini is restored to stock (`bReplicateHeadEquipment 1`, `bAdvancedNPCMaintenance 1`).

**What IS shipped and working from this arc** (do not remove): the freeze fix (§4a, user-confirmed),
death-confirmed dismemberment through DF's API, hit-located limb choice, the permanent PLANCK-ignore
for dismember-touched actors, the ReShape dead-actor crash guard, and Release PDBs.

**Facts established (so a future attempt does not re-derive them):**
- The head is an **engine-created actor clone** (REL::ID 36631), not an NGD object — see §4g.
- HIGGS **does** grab it (grab/drop callbacks fire), physics is sound (a weapon shoves it like a
  ball), all 18 bodies are dynamic and co-located, and the visible head rides the COM body.
- NOT the cause: SMP hair (disproven on a bald Khajiit), mass/friction (§6), motion type, collision
  layer, the head skeleton, or the orbiting satellites.
- **The one hypothesis never tested:** HIGGS routes actors through `ShouldUsePhysicsBasedGrab`
  (`selectedObject.isActor` ⇒ always true), which drives the held body with a **motorized constraint
  against the ragdoll**, not a keyframe. On a normal corpse that force propagates through the whole
  constraint chain; on this clone every constraint is cut and 17 of 18 bodies are inert, so the drive
  may be applying force to a body that transmits nothing — or HIGGS's chosen body may not be the COM
  at all. **The decisive next step is instrumenting HIGGS's chosen rigid body** (which node it
  resolved to at grab time), which no public API exposes — it needs a hook on
  `Hand::GetRigidBodyToGrabBasedOnGeometry` or reading HIGGS's `selectedObject` from its own data.
  Everything short of that is guesswork, and this arc spent ~6 builds proving that.

## 5. Authoring a head-only skeleton (binary NIF surgery)

Generator: `tools/ppb-scratch/mkhead3.py` (also copied to the session scratchpad). Run it on each
PPB skeleton; it writes `<name>_head.nif` beside it.

**Principle: never add or remove blocks** — that would require re-indexing every reference in the
file. Only field *values* change, so the output is byte-for-byte the same size and layout.

### Field map (verified empirically against all 18 bodies of `skeleton_female.nif`)
```
bhkCapsuleShape:  radius@4   firstPoint@16   radius1@28   secondPoint@32   radius2@44
bhkRigidBody:     shape ref@0   HavokFilter@4 (first byte = collision LAYER)
                  inertia diagonal @116 / @140 / @164
                  mass@180   linDamp@184   angDamp@188
                  friction@200   rollingFriction@204   restitution@208
NiNode:           name@0, extraDataList, controller, flags, translation(12), rotation(36),
                  scale(4), collisionObject ref  -> bhkCollisionObject{target(4),flags(2),body}
bhkListShape:     numSubShapes@0, subShape refs@4
```

### What the generator does
1. Walk `NiNode "NPC Head [Head]"` → collision → body → list → its capsules; same for
   `NPC COM [COM ]`.
2. **Move the head's real capsule set onto the COM body**, recentred on its own centroid — because
   NGD parks the COM where the head renders, the collision must be centred on the COM origin, *not*
   offset by the bind-pose head→COM distance.
3. **Copy the head body's physics onto the COM** (§6) — mass, inertia diagonal, damping, friction,
   restitution.
4. Collapse every other capsule to 0.01 and set every other rigid body to **collision layer 15
   (L_NONCOLLIDABLE)**.

Result per file: **1 collidable body, 17 non-collidable**, head shape span ≈ 6.5u (human),
8.1u (Khajiit), 15.1u (Draenei — larger, likely horns in the head list; check if her heads look big).

Built for: `skeleton_female` · `skeletonbeast_female` · `skeletonbeast_female_khajiit` ·
`skeleton_female_draenei`.

---

## 6. Mass, inertia & friction — the physics field map

Read straight out of `skeleton_female.nif` (mass@180, friction@200):

| body | mass (kg) | friction |
|---|---|---|
| **COM** | **40.0** | **2.50** |
| spine/upper arms | 6–7 | 0.30 |
| head | 4.0 | 0.80 |
| forearms, thighs | 4–6 | 0.30 |
| hands, feet | 2–3 | 0.02–0.30 |

**Mass lives on the BODY, not the capsule.** A body may own many capsules (PPB's head body has 23)
but has exactly **one** mass/inertia/friction. Capsules define *where it is solid*; the body defines
*what it is made of*. You cannot give two capsules on one body different weights — the 18 bodies are
the granularity.

**The COM trap:** the COM body exists to carry the *entire actor's* mass. Move head collision onto it
without copying the head's physics and the severed head weighs 40 kg with 3× ground friction —
"welded to the floor" (user-observed). The inertia matters as much as the mass: a 40 kg inertia
(0.2533 vs the head's 0.0256) makes the head **slide instead of roll**.

NGD also has its own runtime `fHeadMass = 10.0` (and `fHeadImpulseFactor` tuned against it) — if the
head still feels heavy after the NIF fix, that is the next dial, but lower mass ⇒ heads fly further.

**Ball-and-chain feasibility** (asked, worth recording): attaching a heavy body by a constraint to a
limb works and mass *ratio* is the dial — but Skyrim NPC **locomotion comes from the character
controller, not the ragdoll**, and PLANCK force-drives the ragdoll toward the animation. So a weight
will swing and yank the limb convincingly yet will **not** slow the NPC down; real hindrance needs a
movement-speed effect on top.

---

## 7. Collision SOUND (the "log falling down the stairs")

Contact sounds come from a **material-pair lookup** on the two bhk wrapper materialIds. PPB exposes
the selector per layer:
`0 = skin/NIF (the loud body-thud) · 1 cloth · 2 snow · 3 grass (soft rustle) · 4 = NONE`.
**`4` is true silence** — the engine's impact lookup finds no material and plays nothing; that is the
"fake sound" answer. Knobs: **`npcBodyMat`** (the 18 body capsules) and **`npcGarmentMat`**
(hair/tail/cloth rigs). Both re-dress live on edit.

⚠ A collision-sound complaint is often NOT the material: the 2026-07-27 case was PPB's own
auto-pinned **finger rig** teleporting ~40×/min on an idle NPC's hand with default materials
(`npcFingerEnable 1` left armed). Check what is actually rigged before changing materials.

---

## 8. Knob reference (all live-editable in `PPB_tuning.txt`)

| knob | default | meaning |
|---|---|---|
| `dgEnable` | 1 | dismember guard master |
| `dgLog` | 1 | per-action DG log lines |
| `dgDeathCut` | 1 | PPB asks DF to dismember once death is confirmed (needs DF `bDeferredHitProcess = 0`) |
| `dgDeathCutDelayS` | 0.20 | wait after death before asking |
| `dgDeathNodeTries` | 1 | limb nodes offered per death (1 = closest to DF's natural feel) |
| `dgHitLocated` | 1 | choose the limb nearest the killing blow |
| `dgHitMaxDistU` | **0** | accuracy gate (0 = off; see §4b caveat) |
| `dgHeadSkel` | 1 | swap the race skeleton to `_head.nif` while the head loads |
| `dgHeadSkelHoldS` | 3.0 | max hold for that swap |
| `dgHeadPark` | 1 | park head-clone bodies on the COM + cut their constraints |
| `dgCloneStrip` | **0** | ⛔ legacy runtime capsule collapse — **leave 0**, it mutates SHARED shapes |
| `dgGraceDeadS` | 15 | corpse PLANCK-ignore window |
| `dgDeferDf` | 0 | ⛔ the abandoned ProcessDismemberment hook — leave 0 |
| `dgHeadTrack` | 1 | log head creation at NGD Step01/Step02 (research) |

Sibling ini overlays PPB ships (its MO2 priority wins):
`DismemberingFramework.ini` → `bDeferredHitProcess = 0` (**required** by the design above)
`NextGenDecapitations.ini` → verbose knob; `fHeadMass`/`iScalesUpdateFrequencyFactor` live here too.

---

## 9. Hooking foreign DLLs — the technique that worked

- **Never `SKSE::Trampoline::write_branch` into another mod's DLL.** It is for re-pointing an
  existing call site, and its rel32 jump **cannot reach** a DLL loaded >2 GB away
  ("skse/Trampoline.cpp(168): displacement out of range" — a real user-facing crash).
- Use a **14-byte absolute-jump detour**: steal 14 bytes, build `[stolen 14][FF 25 <abs64> → src+14]`,
  patch the entry with `FF 25 <abs64> → hook`. `FF 25` (rip-relative indirect) has no range limit.
- **Validate the prologue before patching**, and treat the stack-displacement byte as a wildcard:
  `48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56` = `mov [rsp+XX],rbx` + 6 pushes = exactly 14 bytes of
  whole, position-independent instructions. DF's ProcessDismemberment and NGD's Step02 use `0x20`;
  NGD's **Step01 uses `0x18`** — a rigid comparison silently refuses to install (cost: one wasted
  test session). A mismatch must **disable the feature, never patch anyway**.
- Bind unlinked engine functions by address instead of linking the SDK symbol
  (`hkpConstraintInstance::setEnabled` @ **0xAC06A0**). Many Havok SDK setters are not linked into
  PPB — write the fields directly, or use `const_cast` on an accessible getter
  (`const_cast<hkTransform&>(hk->getTransform()).setTranslation(...)`).
