# 10 — Player Hand Colliders (finger boxes on the player's VR hands)

**Status: DESIGN ONLY. Nothing built, nothing written to any plugin. Approve before implementing.**
Date: 2026-07-08. Sources: vendored HIGGS v1.10.10 at `G:/Claude Workspace/tools/_research/higgs`
(matches the installed `higgs_vr.dll`), PLANCK at `tools/_research/planck_src`, our own
`tools/CollVizMarkers-plugin/src/Markers.cpp` and `tools/PPB-plugin/src/`. Every claim below carries a
`file:line` cite. Facts marked ★ were re-verified by reading the file during this write-up.

---

## ⚠ READ THIS BEFORE ANYTHING ELSE — the honest limitation

The premise "give the player better hand colliders so HIGGS grabbing improves" is **false**, and it's
worse than a partial miss:

> **HIGGS's grab does not use ANY collider. Not its own box, not ours, not anything.**

- **Grab candidate selection** is a `bhkSimpleShapePhantom` *sphere linear-cast* from the palm point
  along the palm vector — radius 0.08 m, distance 0.15 m, and the phantom's filterInfo is temporarily
  forced to `0x2C` (layer 44). `Hand::FindCloseObject`, `higgs/src/hand.cpp:320`, radius/distance at
  `hand.cpp:292,295`; installed `higgs_vr.ini:44-45` confirms the shipped values are the source defaults.
- **Grab point + finger wrap** is `GetClosestPointOnGraphicsGeometryToLine` over the target's **visual
  mesh triangles** (`hand.cpp:1212, 1432, 1754`) → `grabbedFingerValues[5]`.
- The hand box's only two jobs are (a) ordinary physics contact and (b) being `bodyA` of the physical
  grab constraint (`hand.cpp:1595, 1617`).

And there is **no way to tell HIGGS or PLANCK about extra bodies.** Both gate on *pointer identity*,
never on layer:

- ★ `IsHandRigidBody` = `wrapper == g_leftHand->handBody || wrapper == g_rightHand->handBody`
  (`higgs/src/physics.cpp:419-424`). `IsHandOrWeaponOrHeld` (`physics.cpp:436-444`) wraps it, and
  `PhysicsListener::contactPointCallback` early-outs when neither body passes (`physics.cpp:670-673`)
  → **no HIGGS haptics, no collision sound, no `AddCollisionCallback` for our bodies.**
- ★ PLANCK's `IsHiggsRigidBody` (`planck_src/src/main.cpp:538-546`) is the same identity test.
  Its consumers early-out → **no `PlanckHitEvent`, no melee/punch attribution, no NPC "you hit me"
  reaction, no hit haptics** from our finger boxes.
- The HIGGS interface (41 vfuncs, mirrored verbatim at `tools/PPB-plugin/src/HiggsInterface.h`) has
  **no** `AddHandRigidBody`, no `SetHandShape`, no hook of any kind for a third-party body.

### What the user actually gains

| Gained | Mechanism |
|---|---|
| **Finger-shaped push/dent on NPC ragdoll bodies** | Layer 56 ↔ Biped(8)/BipedNoCC(33)/DeadBip(32) is set in `higgsLayerBitfield`; PLANCK's filter callback returns `Continue` for that pair (`planck main.cpp:2486-2489`) → vanilla layer table → collide. PLANCK-driven NPC bodies are `MOTION_DYNAMIC`, so they get pushed and the PD controller pulls them back. **This is the payoff, and it is exactly what PPB's flesh-fitted capsules were built for.** |
| **Nudging clutter / props / debris / dead bodies with one fingertip** | Layers 4, 10, 19, 20, 32 all set in the bitfield. |
| **A place to hang OUR OWN touch events** | We own the bodies → a PPB `hkpContactListener` (or a per-frame contact poll) yields "index tip ↔ NPC L thigh" events for VRTouchEvents / SkyrimNet. **Nothing else in the load order can produce that.** |
| **Correct visual registration** | HIGGS's box rides the *first-person* hand node (raw controller pose, VRIK offset deliberately stripped — see §1). Ours ride the *third-person* finger bones, i.e. the hands the player actually sees. That alone fixes a large part of the "poorly placed rectangle" complaint. |

### What the user does NOT gain — say this out loud before building

1. **HIGGS grabbing does not improve. At all.** Not selection, not grip point, not finger wrap.
2. **No PLANCK hit events, no damage, no knockdown, no NPC aggro or reaction.** Poke an NPC with a
   finger box: flesh moves, she does not notice.
3. **No HIGGS haptics, no impact sound** (unless we build our own — §8, Track C).
4. **Zero CBPC benefit.** `cbp.dll` computes collisions in NiNode space off named skeleton bones
   (`04_Pitfall_Ledger.md` "CBPC facts"; `Report/VRTouchEvents Module/01_CBPC_Touch_Nodes.md:29`).
   Havok bodies are invisible to it. **Breast/butt/belly jiggle will not respond to a finger box.**
   Getting that requires CBPC collider *config* entries, which is a different mod-layer entirely.
5. **No punches.** A fist of keyframed finger boxes has no mass in PLANCK's accounting and PLANCK
   ignores them.

**And the one that will bite you:** HIGGS's slab is **10 × 3 × 18 cm**, centered 8.6 cm out along the
hand's +Z with half-length 9 cm → it spans ≈ 0 … 17.6 cm from the wrist. **It envelops the entire
hand, fingers included.** At stock size it makes first contact every single time and the four finger
boxes will never be felt. **The finger boxes are worthless unless the slab shrinks first.**

---

## 1. What HIGGS's player hand collider actually is

`Hand::CreateHandCollision` — `higgs/src/hand.cpp:559-614` ★ (read in full).

| Property | Value | Cite |
|---|---|---|
| Shape | **ONE `bhkBoxShape` → `hkpBoxShape`.** Not a capsule, not a list. | `hand.cpp:565-571` |
| Half-extents | `{0.05, 0.015, 0.09}` m = **10 × 3 × 18 cm**; in skyrim units `{3.500, 1.050, 6.299}` | `config.h:335` ★; installed `higgs_vr.ini:50-52` = source defaults, unmodified ★ |
| Convex radius | `0` (crisp box) | `config.h:337`, `hand.cpp:571` |
| Beast variant | half-extents `{0.1, 0.015, 0.2}` m, offset `{-0.007, -0.005, 0.2}` m | `config.h:338-340` ★ |
| Offset from node | `{0, -0.005, 0.086}` m (x negated for the left hand), applied in the hand-node frame | `config.h:336`, `hand.cpp:527-531` |
| **Node it rides** | the **first-person** hand bone `player->unk3F0[kNode_RightHandBone / kNode_LeftHandBone]`, captured *before* the VRIK offset is added (`m_handTransformWithoutVrikOffset = handNode->m_worldTransform;` at `hand.cpp:4100`, VRIK offsetting applied only afterwards at `:4102`). HIGGS comments it deliberately: *"handTransform must be where the hand is in real life, as the hand collision is what drives the grab constraint"* (`hand.cpp:523`). | `hand.cpp:2073-2079`, `:521-534` ★ |
| Motion | `MOTION_KEYFRAMED`, quality `HK_COLLIDABLE_QUALITY_KEYFRAMED`, `enableDeactivation=false`, `SOLVER_DEACTIVATION_OFF`, `maxAngularVelocity=500` | `hand.cpp:583-589` ★ |
| Material | `skinMaterialId` = `0x233db702` | `hand.cpp:570` |
| filterInfo | `(playerCollisionGroup<<16) \| 56 \| (1<<15) \| (ragdollBits<<8)`, **spawned with `(1<<14)` = collision OFF**, turned on later in `Update()` | `hand.cpp:576-582` ★ |
| ragdollBits | `RightHand=3`, `LeftHand=5` (5-bit ragdoll sub-layer) | `physics.h:213-222` ★ |
| Created when | world change or beast toggle only. **Never per-frame.** | `hand.cpp:646-655`, `main.cpp:601-602` |
| Moved when | `MoveHandAndWeaponCollision` → `ApplyHardKeyframeVelocityClamped`, inside `PrePhysicsStep` | `hand.cpp:690-700` ★ |
| Disabled when | `state == HeldBody \|\| IsTwoHanding()` (bit 14 + `UpdateCollisionFilterOnEntity`) | `hand.cpp:661-676` |
| Exposed to us? | **Yes.** `GetHandRigidBody(bool isLeft)` = vfunc **24**, returns `hand.handBody` (a `bhkRigidBody*`). | `pluginapi.cpp:367-371` ★; already declared at `tools/PPB-plugin/src/HiggsInterface.h:57` |

**Layer 56 = `L_HIGGSCOLLISION`**, installed by HIGGS into the first free vanilla slot with bitfield
`0x01053343161b7fff` (`physics.cpp:831-863`, `pluginapi.h:101-102`). Decoded, layer 56 collides with:
Static(1), AnimStatic(2), Clutter(4), Weapon(5), Projectile(6), Spell(7), **Biped(8)**, Props(10),
Trap(14), Ground(17), DebrisSmall/Large(19,20), **DeadBip(32)**, **BipedNoCC(33)**, Unused1(44),
**HIGGS(56)** — and explicitly **NOT CharController(30)**.

### Why it feels like "a poorly placed rectangle" — the three reasons, ranked

1. **It IS a rectangle.** One axis-aligned box, 10 cm wide × 3 cm thick × 18 cm long, with a crisp
   zero convex radius. It has no fingers, no palm curvature, no thumb, no taper.
2. **It's in the wrong place, by design.** It rides the *raw controller* hand pose, with VRIK's offset
   deliberately excluded (`hand.cpp:523`, `:4100-4103`). If the user runs VRIK with any hand offset or
   `handSize` scaling, **the collider is not where the rendered hand is.** HIGGS accepts that because
   it needs the physical hand for its grab constraint — but it means the box and the visible hand can
   sit centimetres apart.
3. **It's a spatula, not a hand.** Half-length 9 cm from a centre 8.6 cm out means the box reaches
   from the wrist to ≈ 17.6 cm past it — well beyond the fingertips of most bodies — and it is
   *rigid*: it never opens, never closes, never curls. Fisted or splayed, the player pushes the world
   with the same 10×3×18 slab.

---

## 2. The user's spec, restated precisely

Four colliders per hand, eight total, following the **live VR hand pose**:

| # | Group | Shape | Purpose |
|---|---|---|---|
| 1 | **Index — proximal** | long, narrow box | the pointing/prodding segment; MCP→PIP |
| 2 | **Index — distal** | long, narrow box | fingertip; PIP→tip |
| 3 | **Middle+Ring+Pinky — proximal** | large, narrow box (a slab spanning all three) | the "flat of the fingers" |
| 4 | **Middle+Ring+Pinky — distal** | large, narrow box | the three fingertips as one plate |

- **No thumb collider.** (`Finger00/01/02` are read only for diagnostics, never given a body.)
- The index finger is **independently articulated** — it gets its own two-box chain so pointing,
  prodding, and tracing work as distinct gestures.
- The three-finger group is **one plate per phalanx row**, not three separate boxes — the user's
  observation is that middle/ring/pinky curl together off a single grip axis, so a single wide box
  per row is a faithful and much cheaper approximation.
- "Narrow" = the boxes are **thin in the dorsal/palmar direction** (a finger is ~1.7 cm thick, not a
  cube) and **long along the bone**.
- They must track the *live* pose: whatever posed the fingers (game animation, VRIK, Index knuckle
  finger tracking, or HIGGS's own `FingerAnimator` during a grab), the boxes follow.

**Explicit non-goals for this feature** (from §0): grab improvement, damage, aggro, CBPC.

---

## 3. THE DECISION — modify HIGGS's box, or create our own?

### **Recommendation: BOTH, and land Track A first.** They are complementary, not alternatives.

#### Track A — reshape HIGGS's existing box (float writes)

- **Size: YES, this works.** `GetHandRigidBody(isLeft)` → `bhkRigidBody` → `hkpRigidBody` →
  `collidable.shape` → `RE::hkpBoxShape` (`halfExtents` @ 0x30, `radius` @ 0x20, sizeof 0x40 —
  `CommonLibVR-4.14.0/include/RE/H/hkpBoxShape.h`). It is a plain float write to a stored number,
  which is **exactly rule 1 of the pitfall ledger** ("capsule/pivot FLOAT edits are safe at runtime")
  applied to a box — and this body is *keyframed and non-ragdoll*, so it is strictly safer than the
  CapFix writes we already ship. **Confidence 85% (see Risk 2).**
- **Position: NO, we cannot.** The centre comes from `Config::options.handCollisionBoxOffset`, applied
  **fresh every frame** in `ComputeHandCollisionTransform` (`hand.cpp:521-534`), and HIGGS re-keyframes
  the body in `PrePhysicsStep`, which runs **after** `TriggerPrePhysicsStepCallbacks`
  (`hooks.cpp:706-712`). There is no seam left where our write survives to the physics step.
- **`SetSettingDouble` (vfunc 39) cannot reach the half-extents.** `ReadVector` calls `ReadFloat`, not
  `RegisterFloat`, so `HandCollisionBoxHalfExtentsX/Y/Z` and `...Offset*` are **never inserted into
  `floatMap`** (`config.cpp:84-91` vs `:130-134`) → `SetSettingDouble` returns false
  (`config.cpp:154-173`). Only `HandCollisionBoxRadius`/`...RadiusBeast` are registered, and those are
  read **only at body creation**. The interface is a dead end here; the direct float write is the
  only path.

**Track A alone delivers:** a hand collider whose silhouette is a *palm ridge* instead of a spatula,
while **keeping** HIGGS's haptics, HIGGS's collision sound, PLANCK's punch/hit/aggro attribution, and
the grab constraint anchor — because it is still, byte-for-byte, the same `bhkRigidBody*` that every
identity gate in HIGGS and PLANCK recognises. **This is the single highest-value change in the whole
document, and it is ~30 lines of code.**

#### Track B — our own four follower bodies per hand

- **Pitfall-ledger legality:** rule 3 says *"never add BODIES"* — but the rule's scope is the
  **ragdoll instance** (fixed 18-body + constraint mapping). It does **not** forbid creating
  independent, non-ragdoll, keyframed bodies attached to a bone. Precedent, both ours and vanilla's:
  - vanilla beast skeletons ship **5 tail bodies outside the ragdoll instance**
    (`06_BoneFollow_Tails_And_Cavities_Research.md`);
  - `collviz_markers.dll` **creates its own capsule-shaped rigid bodies at runtime, successfully,
    today** (`CollVizMarkers-plugin/src/Markers.cpp:281-411`);
  - AIHands' `PalmCollider.cpp:565-600` created a **live, colliding layer-56 body**.
  These are *our* bodies, added to the world with `hkpWorld_AddEntity`, never registered with any
  ragdoll driver, never mutated after creation. **Rule 2 (no runtime shape-type change) is also
  untouched: shape geometry is solved once at creation and never edited.**
- **Track B alone delivers:** nothing the player can feel, because HIGGS's stock slab shadows the
  fingers (§0). It is *only* useful downstream of Track A.

#### Verdict

| | Track A (reshape HIGGS box) | Track B (4 finger boxes/hand) |
|---|---|---|
| Effort | ~30 lines | ~350-450 lines, 2 new TUs |
| Risk | one unverified assumption (broadphase AABB refresh) | UAF/lifecycle surface, velocity spikes, filter word |
| Keeps HIGGS haptics + PLANCK hits | **YES** | no (identity gate) |
| Gives finger-shaped contact | no (just a narrower slab) | **YES** |
| Gives us our own touch events | no | **YES** |
| Useful standalone | **YES** | **NO** — invisible behind the stock slab |

**Ship A, then B.** A is the prerequisite and the safety valve: if the halfExtents write turns out to
be unsafe (Risk 2), **the whole feature is dead and we've spent 30 lines finding out**, not 450.

---

## 4. The proven runtime-body machinery we already own

`tools/CollVizMarkers-plugin/src/Markers.cpp` is a working, shipped, in-game-verified runtime rigid-body
factory. **Boxes are the easy case** — easier than the capsule it already builds.

### Transfers verbatim (zero changes)

| Piece | Where | Note |
|---|---|---|
| Engine bindings (VR RVA) | `Markers.cpp:100-108` | `bhkBoxShape_ctor 0x2AEB70`, `bhkRigidBodyCinfo_ctor 0xE06110`, `bhkRigidBody_ctor 0x2AEC80`, `bhkRigidBody_setActivated 0xE085D0`, `hkpWorld_AddEntity 0xAB0CB0`, `hkpWorld_RemoveEntity 0xAB0E50`, `hkpRigidBody_setPosition 0xAA8FD0`, `hkpRigidBody_setRotation 0xAA9000`, `applyHardKeyFrame 0xAF6DD0` |
| SDK link-time stubs | `Markers.cpp:65-85` | `hkReferencedObject::addReference/removeReference/getClassType/calcContentStatistics`, `hkpEntity::activate`, `hkMemoryRouter::s_memoryRouter`, two `hkpConstraintData` virtuals. **⚠ PivFix.cpp:70-90 already defines all of these in PPB.dll** — see Risk 8. |
| `PortBhkShape` (0x28) / `PortBhkRigidBody` (0x40) / `PortBhkRigidBodyCinfo` | `Markers.cpp:127-199` | byte-validated, `static_assert`-guarded |
| `GameHeapAllocate` = `RE::MemoryManager::Allocate(bytes, 0, false)` | `Markers.cpp:231-238` | boxes only need this variant |
| `WorldWriteLock` (bhkWorld+0xC598), `GetHkpWorld` (+0x10), `IsLikelyPointer`, `HkOf` | `Markers.cpp:208-253` | |
| `RemoveBody` — guarded by `hk->getWorld() == hkpWorld` | `Markers.cpp:255-271` | the exact UAF guard we need at world change |
| `MarkerKeyframeRot` (pos + quat via `applyHardKeyFrame`) | `Markers.cpp:421-429` | our per-frame call, verbatim |
| `bhkWorld` acquisition | `CollVizMarkers-plugin/src/Viz.cpp:150-151` (`actor->GetParentCell()->GetbhkWorld()`) | fallback if the HIGGS callback's `void* world` is ever null |

### Does NOT transfer

- **No hand-built shape, no vtable stamping.** The capsule path (`Markers.cpp:339-411`) exists only
  because no engine ctor for `hkpCapsuleShape` was ever bound. For a box, `fn_bhkBoxShape_ctor()`
  builds a fully-formed, correctly-vtabled `hkpBoxShape` **and accepts non-uniform half-extents** —
  see HIGGS doing exactly that at `hand.cpp:565-569`. Nothing hand-rolled, nothing 16-byte-aligned by
  hand, no RTTI worries.
- **No ghost layer.** `MarkerCreate` uses layers 60/61 with empty layer-table rows precisely so
  physics never feels them. Ours must be **real, colliding** bodies on layer 56 (§7).

### Deltas from `MarkerCreate` (`Markers.cpp:281-330`) — the complete list

1. `halfExtents.set(hx, hy, hz, 0)` — **non-uniform** instead of `(h,h,h,0)`.
2. `hkc.m_rotation.set(qx,qy,qz,qw)` at creation (copy the line from `MarkerCreateCapsule:396`).
3. `setRadiusUnchecked(0.f)` — crisp, exactly matching HIGGS.
4. `hkc.m_maxAngularVelocity = 500.f` (`hand.cpp:589`).
5. Real filter word, **not** a ghost layer (§7).
6. Per-frame `MarkerKeyframeRot` **plus the velocity clamp** — see the correction below.

### ★ CORRECTION to the recon: `ApplyHardKeyframeVelocityClamped` is apply-then-teleport, not if/else

Read at `higgs/src/physics.cpp:865-886`. The real sequence is:

1. `hkpKeyFrameUtility_applyHardKeyFrame(pos, rot, invDt, hkBody)` — **always, first**.
2. *Then* if the resulting `getLinearVelocity()` exceeds
   **`bhkRigidBody_GetMaxLinearVelocityMetersPerSecond(body)`** → take the world write lock and
   `hkpRigidBody_setPosition(hkBody, nextPosition)`.
3. Same again for angular vs `bhkRigidBody_GetMaxAngularVelocity(body)`.

Two consequences for our port:
- The clamp threshold is **read off the body**, not a config constant. The recon's "handBoxMaxVel
  default 25 m/s" is our own invention. Either (a) set the body's max-velocity fields at creation and
  read them back, or (b) simply compare against our own `handBoxMaxVel` knob — functionally identical,
  and simpler. **Recommend (b).**
- The teleport happens **under the bhkWorld write lock**. Our `WorldWriteLock` already does that.

---

## 5. Reading the live VR finger pose

**Source of truth = the third-person finger node WORLD transforms.**

With VRIK loaded, VRIK owns the fingers (`vrikinterface001.h:38 getFingerPos(bool isLeft, int
fingerIndex)`, 0=closed…1=open; indices 0=thumb 1=index 2=middle 3=ring 4=pinky), and HIGGS's
`FingerAnimator` overrides them during a grab — **both on the third-person skeleton**. HIGGS resolves
the root exactly this way (★ read at `higgs/src/finger_animator.cpp:37-53`):

```
NiNode* root = player->GetNiRootNode(g_isVrikPresent ? 0 : 1);   // 0 = 3rd person, 1 = 1st person
fingerNodes[i][0] = root                ->GetObjectByName(name[i][0]);
fingerNodes[i][1] = fingerNodes[i][0]   ->GetObjectByName(name[i][1]);   // nested, not root-wide
fingerNodes[i][2] = fingerNodes[i][1]   ->GetObjectByName(name[i][2]);
```

CommonLibVR equivalent: `player->Get3D(false)` = third person, `Get3D(true)` = first person
(`RE/T/TESObjectREFR.h:370`). VRIK presence: `GetModuleHandleA("vrik.dll")` (mirrors
`higgs/src/main.cpp:930`).

> **Reading the third-person nodes is a deliberate divergence from HIGGS**, and it is the fix for
> complaint #2 in §1: our boxes land on the hand the player *sees*, not the one the controller
> reports. Any VRIK hand offset, `handSize` scaling and the 0.8517 hand-node scale all fold in for
> free because we read **world** transforms.

### Exact node names

Verified against `higgs/src/main.cpp:959-1000` ★ and present in the shipping
`D:/Games/My Skyrim/mods/Precision Physic Bodies/meshes/actors/character/character assets female/skeleton_female.nif`
(30 `NPC ? Finger??` nodes, 3 segments each, **no tip node**):

| Group | Nodes (right hand; mirror `R`→`L`, `RF`→`LF`) |
|---|---|
| **Index** → boxes 1 & 2 | `NPC R Finger10 [RF10]` → `NPC R Finger11 [RF11]` → `NPC R Finger12 [RF12]` |
| **Middle** ┐ | `NPC R Finger20 [RF20]` → `Finger21` → `Finger22` |
| **Ring**   ├ → boxes 3 & 4 | `NPC R Finger30 [RF30]` → `Finger31` → `Finger32` |
| **Pinky**  ┘ | `NPC R Finger40 [RF40]` → `Finger41` → `Finger42` |
| **Thumb — EXCLUDED per spec** | `Finger00/01/02` |
| Hand (scale reference) | `NPC R Hand [RHnd]` |

Segment 0 = proximal (MCP), 1 = middle (PIP), 2 = distal (DIP).
**Bone axis = node-local +Z** (every child's local translation is `(0,0,L)`).
**Dorsal ≈ node-local +Y** — HIGGS's `PalmVector` is `(-0.018, -0.965, 0.261)`, i.e. the palm faces
−Y (`config.h:333`, `higgs_vr.ini:25-27` ★).

### Bind-pose local translations (skyrim units; `NPC R Hand` local scale = **0.8517**)

```
F10 (2.282,-0.109,7.213)   F11 (0,0,2.094)  F12 (0,0,1.612)     index
F20 (0.280,-0.083,7.080)   F21 (0,0,2.727)  F22 (0,0,1.691)     middle
F30 (-1.357,-0.639,6.794)  F31 (0,0,2.276)  F32 (0,0,1.710)     ring
F40 (-2.738,-1.583,6.239)  F41 (0,0,1.500)  F42 (0,0,1.519)     pinky
knuckle X span F20→F40 = 3.018 u ;  knuckle Z spread = 0.841 u
```

Fingertips are extrapolated (no tip node exists):
`tip_k = F_k2.world.translate + zAxis(F_k2) · tipFrac · |F_k2 − F_k1|`, `tipFrac` default **0.85**.

### The 4 boxes

Box basis = columns of the rotation matrix: `colX = across (thumb-ward)`, `colY = up (dorsal)`,
`colZ = forward (down the bone)`. Half-extents map `(hx,hy,hz) = (halfW, halfT, halfL)`.
`kSkyrimToHavok = 0.0142875` m/unit.

| # | Name | Rides frame | Anchor (node-local, measured once) | halfL |
|---|---|---|---|---|
| 1 | index proximal | `F10` | `center = F10 + zAxis(F10)·halfL₁` | `0.5·\|F11−F10\|` = **0.01274 m** |
| 2 | index distal | `F11` | `center = F11 + zAxis(F11)·halfL₂` | `0.5·(\|F12−F11\| + tip)` = **0.01814 m** |
| 3 | group proximal | `F20` | min/max-projection midpoint of {F20,F30,F40, F21,F31,F41} in F20's frame | **0.0217 m** |
| 4 | group distal | `F21` | min/max-projection midpoint of {F21,F31,F41, tip₂,tip₃,tip₄} in F21's frame | **0.0189 m** |

Cross-section knobs (metres, at player scale 1.0):

```
index  halfW 0.0095   halfT 0.0085          (box 2 tapers ×0.90)
slab   halfT 0.0100   halfW ≈ 0.0279        (= 0.5·(knuckleXspan · 0.8517 · 0.0142875) + fingerHalfW)
capPad 0.0010
```

**Anchors and half-extents are solved ONCE at creation, then CONSTANT.** Bone lengths never change,
and middle/ring/pinky curl from one grip axis under VRIK, so they move near-rigidly relative to `F20`.
Per-frame work is **position + rotation only** → exactly `MarkerKeyframeRot`. **No live shape edits;
no new CTD surface.**

- Gate creation on an **open hand** (angle between `zAxis(F20)` and `zAxis(F21)` < 15°); otherwise fall
  back to the bind constants above, scaled by `handNode->world.scale / 0.8517`.
- Recreate on: world change, `|Δ handScale| > 1%`, beast toggle.
- Expose an `hbox recal` path for a manual re-solve.

### Frame seams — both from the HIGGS interface, no new hooks

1. **`AddPostVrikPostHiggsCallback(cb)` (vfunc 33)** — fires at `hooks.cpp:547`, immediately after
   `NiAVObject_UpdateNode(thirdPersonRoot)` (`hooks.cpp:542`) and after `fingerAnimator.Update()`.
   Finger world transforms are **final** here regardless of who posed them (game / VRIK / Index
   knuckle tracking / HIGGS).
   → snapshot 15 world transforms + `handScale` into a POD double-buffer. **No Havok touched.**
2. **`AddPrePhysicsStepCallback(cb)` (vfunc 21)** — fires at `hooks.cpp:708` with `void* world` = the
   live `bhkWorld*`, after every `driveToPose`, before `hkpWorld::stepDeltaTime`.
   → create-if-needed, refresh filter, `applyHardKeyFrame` the 8 bodies. **This is the same seam
   PLANCK uses**, and it is where HIGGS itself moves its hand box (`main.cpp:483/495`).

Both vfunc indices are already correct in `tools/PPB-plugin/src/HiggsInterface.h:54,66`.

---

## 6. Collision layer / filter — the exact word

**Derive it from HIGGS's own hand body every frame.** That inherits everything HIGGS re-stamps: player
group changes on horse mount/dismount (`hand.cpp:679-681`), beast recreation, the creation delay,
`DisableHand`.

```
hb   = (bhkRigidBody*) higgs->GetHandRigidBody(isLeft);            // vfunc 24
base = hb->referencedObject->collidable.broadPhaseHandle.collisionFilterInfo;
       // == (playerGroup<<16) | 56 | (1<<15) | (3 or 5)<<8 | [HIGGS's bit 14]

ours = (base & ~0x00001F00u) | (4u << 8);          // ragdoll sub-layer 4 = SkipBoth
if (higgs->IsHoldingObject(isLeft) || higgs->IsTwoHanding() || higgs->IsDisabled(isLeft))
    ours |= (1u << 14);                            // collision OFF

// on change: write it, then hkpWorld_UpdateCollisionFilterOnEntity(hkpWorld, entity, 0, 0)
//            [VR 0xAB3110, per AIHands PalmCollider.cpp:1250-1251] under WorldWriteLock
```

### Why each field

- **Layer 56.** The only layer whose bitfield lets us hit bipeds while excluding CharController(30)
  (`pluginapi.h:102`: *"Same as L_WEAPON layer, but + self-collision (layer 56) and − charcontroller
  collision"*).
- **playerGroup + bit 15.** The same-group rule: a charController does **not** set bit 15, so
  `!(a & b & 0x8000)` ⇒ **the player's own charController is never pushed.** This is precisely the
  mechanism documented at `CollVizMarkers-plugin/src/Markers.h:21-23` ("the charController branch
  IGNORES the layer table but honors same-group") and in `04_Pitfall_Ledger.md`, and it is why HIGGS's
  own box doesn't shove the player around. **Copy HIGGS's group + bit 15 verbatim.**
- **NPC charControllers** are handled for us by PLANCK: `otherLayer == g_higgsCollisionLayer &&
  otherGroup == playerGroup` → `Ignore`, unless the NPC is a "hittable charcontroller"
  (`planck main.cpp:2508-2528`). That is a **layer** check, so our boxes inherit it free.
- **Ragdoll sub-layer 4 (`SkipBoth`).** ★ Confirmed by reading `higgs/include/physics.h:210-222`:
  > *"5-bit ragdoll layer. If bit 15 is set, then if layer differs by 1 we don't collide. If layer
  > differs by !=1 we do."* — `SkipNone=0, SkipRight=2, RightHand=3, SkipBoth=4, LeftHand=5, SkipLeft=6`.

  `|4−3| = |4−5| = 1` ⇒ **our boxes never touch HIGGS's left/right hand box or either weapon box.**
  Exactly the intent: our fingers must not fight the palm slab.
- **1st-person body** has no Havok collision at all. Player biped bodies (if present) are
  `MOTION_KEYFRAMED`; keyframed-vs-keyframed produces zero impulse and both mods early-out.
- **Held objects** get sub-layer 2/6 (`hand.cpp:760`); `|4−2| = 2` ⇒ they *would* collide. Hence the
  explicit `IsHoldingObject` bit-14 gate above — a superset of HIGGS's own `state==HeldBody` disable
  (`hand.cpp:669`).

### Motion / quality

`MOTION_KEYFRAMED` + `HK_COLLIDABLE_QUALITY_KEYFRAMED`. **Do NOT use `KEYFRAMED_REPORTING`** — PLANCK
upgrades HIGGS's bodies to it (`planck main.cpp:3886-3893`) and branches on it (`:1833`). Staying on
plain `KEYFRAMED` keeps us out of those paths while still generating full contacts against dynamic NPC
ragdoll bodies.

### Benign, verified side-effect

HIGGS's near-cast phantom (filterInfo forced to `0x2C`, layer 44) **will** report our four boxes
(44↔56 is set in the bitfield). They are discarded harmlessly: `GetRefFromCollidable` returns null and
the loop skips them (`hand.cpp:353-357`). HIGGS's own hand box already takes that exact path today.

---

## 7. Proposed knob set (`PPB_tuning.txt`) — *not yet added, no code reads these*

```
# ── Track A: reshape HIGGS's own hand box (metres; -1 = leave HIGGS's value alone) ──
higgsSlabHalfX   -1
higgsSlabHalfY   -1
higgsSlabHalfZ   -1
higgsSlabFistZ    0      # optional: scale halfZ by finger curl so a fist retracts the slab

# ── Track B: our four finger boxes per hand ──
handBoxEnable     0      # SHIP DEFAULT 0
handBoxTipFrac    0.85
handBoxIdxHalfW   0.0095
handBoxIdxHalfT   0.0085
handBoxSlabHalfT  0.0100
handBoxPad        0.0010
handBoxSubLayer   4      # ragdoll sub-layer; 4 = SkipBoth (see Risk 3)
handBoxMaxVel    25      # m/s; above this, teleport instead of keyframe (Risk 6)
handBoxBeast      0      # disabled on beast races until measured
```

Hot-reload rides the existing ~1 Hz `CapFixPollFile()` poll (`Tuning.cpp`). Track A's suggested first
value once we spike: `higgsSlabHalfX 0.030 / Y 0.010 / Z 0.09` — a palm ridge, ceding the hand's width
and silhouette to the finger boxes while keeping HIGGS's haptics, PLANCK's hit attribution, and the
grab constraint anchor on the same body pointer.

---

## 8. Risks, failure modes, rollback, and the minimal first spike

### THE MINIMAL FIRST SPIKE — do exactly this, and nothing else

> **Spike A (≈30 lines, one file):** in a `PrePhysicsStep` callback, read
> `higgs->GetHandRigidBody(isLeft)`, walk to its `hkpBoxShape`, and write
> `halfExtents = {0.030, 0.010, 0.09}` **every frame, idempotently**. Nothing else. No new bodies, no
> new TU, no CMake change.
>
> **Pass criteria:** (1) no CTD over 5 minutes of grabbing, punching, two-handing, mounting a horse,
> and a cell change; (2) the palm visibly narrows against a `trg`-visualised NPC in the Collision
> Visualizer VR mod; (3) HIGGS haptics and PLANCK punch attribution still fire.
>
> **This single test validates the riskiest unverified assumption in the whole design.** If it fails,
> the feature is dead and we spent 30 lines finding out — Track B is worthless without it (§0).

Then, and only then: (2) land the four bodies with `handBoxEnable 0` by default and verify their
placement in the Collision Visualizer. (3) Only then dial.

### Risk register

| # | Risk | Likelihood / Impact | Mitigation |
|---|---|---|---|
| **1** | **The slab masks the fingers.** HIGGS's 10×3×18 cm box envelops the whole hand. Without Track A, the four finger boxes never make first contact and the feature is invisible. | HIGH / HIGH | Land Track A first, as a standalone spike. |
| **2** | **Live `hkpBoxShape::halfExtents` write is UNVERIFIED (~85%).** Analogous to PPB's proven CapFix capsule float writes, but on a box, and it depends on the broadphase AABB being recomputed each step for keyframed bodies (no explicit geometry-refresh call exists). Already flagged "SPECULATION — spike test" at `07_Impact_Research.md:214`. | MED / MED | **Failure mode is missed collisions, not CTD** — the shrink direction is the safe direction. **Test with a shrink, never a grow.** If the AABB is stale, contacts are *lost*, not phantom. |
| **3** | **Ragdoll sub-layer choice (4 = SkipBoth)** rests on HIGGS's inline comment (`physics.h:210-222`) plus indirect proof from PLANCK explicitly disabling hand↔weapon contacts (`main.cpp:2056-2062`, which only need to exist if they *do* collide). The vanilla `CompareFilterInfo` body at `0xE2BA10` was not disassembled. | LOW / LOW | If the rule is really "diff ≤ 1 ⇒ no collide", or the field is wider than bits 8..12, our boxes could contact HIGGS's own hand/weapon boxes — keyframed-vs-keyframed ⇒ **zero impulse**, but wasted contact points and possible spurious HIGGS haptics. Make it a knob (`handBoxSubLayer`). `hbox` diagnostic dumps the filter word + live contact counts. |
| **4** | **No hit attribution, ever.** Poking an NPC pushes flesh but produces **no reaction, no sound, no haptic** (§0). Users read that as "the mod is broken." | CERTAIN / HIGH (perception) | **Accept and document**, or build **Track C**: our own `hkpContactListener` + haptics path (~150 lines, needs `hkpWorld_addContactListener`). NOT solvable through the HIGGS interface. Decide *before* building B. |
| **5** | **Zero CBPC benefit.** If the mental model is "better hand collision ⇒ better soft-body response", this delivers nothing there. | CERTAIN / MED | Set expectations before building. CBPC response needs collider *config* entries, a different layer of work. |
| **6** | **Player-movement / teleport velocity spike.** HIGGS registers its hand body in `g_playerSpaceBodies` and compensates room-space deltas (`main.cpp:483-496`). We don't. A teleport or fast-travel arrival gives our boxes a one-frame huge implied velocity and could launch a nearby NPC. | MED / HIGH | **Mandatory, not optional:** port `ApplyHardKeyframeVelocityClamped` (`physics.cpp:865-886`) — apply the hard keyframe, then if `\|v\| > handBoxMaxVel` hard `setPosition`/`setRotation` (0xAA8FD0 / 0xAA9000) under the world write lock. See the ★ correction in §4. |
| **7** | **World-change / load lifecycle.** `bhkWorld` is torn down on cell change and on load. | MED / CRITICAL (UAF CTD) | Remove bodies guarded by `hk->getWorld() == hkpWorld` (exactly `Markers.cpp:255-271`), or re-`AddEntity` into the new world. Drop **all** pointers at `kPreLoadGame` — PPB `main.cpp:294-301` already has the hook and the teardown convention. |
| **8** | **Second Havok-SDK TU.** `PivFix.cpp:70-90` already defines `hkReferencedObject::addReference/removeReference/getClassType/calcContentStatistics`, `hkpEntity::activate`, `hkMemoryRouter::s_memoryRouter`, and two `hkpConstraintData` virtuals at namespace scope **in this DLL**. | HIGH (if forgotten) / build-break | `HandBoxes.cpp` must **NOT** repeat them → duplicate-symbol link error. (`Markers.cpp` has them only because it is collviz's sole Havok TU.) Also `#undef NEAR` / `#undef FAR` before the SDK headers (`Markers.cpp:23-28`). |
| **9** | **Beast races / werewolf / vampire lord.** Finger node names exist on `skeletonbeast*.nif` but hand proportions differ wildly, and HIGGS swaps to a 20×3×40 cm box. | LOW / MED | Ship `handBoxBeast 0`. Track A must also read `handCollisionBoxHalfExtentsBeast` semantics (a 0.09 halfZ on a beast box is a *massive* shrink). |
| **10** | **Calibration pose.** Geometry is solved once at creation; if the hand happens to be fisted at that instant the slab bounds are wrong. | MED / LOW | Gate on PIP angle < 15° with fallback to the bind constants; `hbox recal` command. Cheap to get wrong, cheap to fix. |
| **11** | **HIGGS version drift.** Everything is pinned to v1.10.10 vtable ordering (41 vfuncs). A wrong-index call is an **instant CTD**. | LOW / CRITICAL | PPB already logs `GetBuildNumber()` at handshake (`Interop.cpp:40`). Add a **hard gate**: refuse to register callbacks below the build that introduced `AddPrePhysicsStepCallback` (vfunc 21) / `AddPostVrikPostHiggsCallback` (vfunc 33). |
| **12** | **Perf.** 8 extra keyframed bodies on a layer that collides with statics, terrain, clutter and every biped. Contacts against fixed geometry produce no impulse but still cost narrowphase; HIGGS's near-cast now collects 4 extra hits per hand per frame. | LOW / LOW | Expected negligible. Measure before shipping. `handBoxEnable 0` is the ship default until measured. |

### Rollback

| Layer | Rollback |
|---|---|
| Track A | Set `higgsSlabHalfX/Y/Z = -1` → PPB stops writing, HIGGS's own value stands from the next body creation (world change) — **and immediately, since we only ever wrote the shape's floats and HIGGS re-reads config only at creation**. Truly instant rollback: `-1` + a cell change. |
| Track B | `handBoxEnable 0` → bodies destroyed via `MarkerDestroy`/`RemoveBody`, world untouched. |
| Whole feature | Restore `PPB.dll` from `tools/ppb-scratch/bake-2026-07-07/PPB.dll.bak_pre_fleshfit`. **No NIF is touched by this feature — the skeleton bake is entirely unaffected.** |

### Files the implementation would touch (for the approval decision, not to be written now)

| File | Change |
|---|---|
| `tools/PPB-plugin/src/HandBoxes.{h,cpp}` | NEW. 2nd Havok-SDK TU. `void*`/POD surface, `Markers.h` pattern. |
| `tools/PPB-plugin/src/PlayerHands.{h,cpp}` | NEW, `RE::` only. Node walk, tip extrapolation, one-time geometry solve, the two HIGGS callbacks, filter derivation, Track-A slab reshape. |
| `tools/PPB-plugin/src/Tuning.{h,cpp}` | The §7 knob block. |
| `tools/PPB-plugin/src/main.cpp` | Register the two HIGGS callbacks after `Interop::AcquireHiggs()`; new `hbox` console command on donor `ToggleWaterSystem` (free — `statue` takes `ShowScenegraph`, `probe` takes `TestAllCells`, `hf`=`TestSeenData`, `capfix`=`DumpNiUpdates`); zero params ⇒ `referenceFunction=false` (`main.cpp:236-239`). Drop pointers at `kPreLoadGame`. |
| `tools/PPB-plugin/CMakeLists.txt` | Add the two sources; extend `set_source_files_properties(src/PivFix.cpp src/HandBoxes.cpp ...)` to scope `PPB_HAVOK_SDK_SOURCE` to both. |

Track A alone touches only `main.cpp` + `Tuning.*` + one small file. **No CMake change, no SDK TU.**

---

## 9. Confidence per subsystem

| Subsystem / claim | % | Basis |
|---|---|---|
| HIGGS box shape / size / node / filter / lifecycle | **100** | source, re-read line-by-line ★ |
| HIGGS's grab ignores every collider | **98** | `hand.cpp:320` sphere-cast + `:1212/1432/1754` mesh-triangle grab point |
| No HIGGS haptics / sound for our bodies | **97** | `physics.cpp:419-424` pointer identity + `:670-673` early-out ★ |
| No PLANCK hit / damage / aggro for our bodies | **95** | `planck main.cpp:538-546` identity test ★ + consumer early-outs |
| CBPC completely unaffected | **96** | `cbp.dll` is NiNode-space; pitfall ledger "CBPC facts" |
| Runtime body creation on layer 56 works | **95** | collviz_markers ships it; AIHands `PalmCollider.cpp:565-600` precedent |
| Finger node names + access path (`Get3D(false)` walk) | **97** | `finger_animator.cpp:37-53` ★ + `main.cpp:962-992` ★ + shipping NIF |
| Our boxes push PLANCK-driven NPC ragdoll bodies | **90** | layer bitfield + `planck main.cpp:2486-2489` `Continue` |
| Player's own charController never pushed | **92** | same-group + bit-15 rule; `Markers.h:21-23`; HIGGS's box behaves this way today |
| Slab must shrink or fingers won't be felt | **90** | pure geometry: box spans 0…17.6 cm from the wrist |
| **Live `hkpBoxShape::halfExtents` write is safe (Track A)** | **85** | analogous to proven CapFix float writes, but broadphase-AABB refresh for keyframed bodies is **unverified** — `07_Impact_Research.md:214` |
| Sub-layer 4 (`SkipBoth`) prevents finger↔palm contact | **88** | `physics.h:210-222` inline spec ★; `CompareFilterInfo` @0xE2BA10 not disassembled |
| Rigid-body-relative anchors stay valid through a full fist | **85** | bone lengths are constant; middle/ring/pinky curl off one grip axis under VRIK — but Index knuckle tracking can splay them independently |
| One-frame teleport velocity spike is fully tamed by the clamp | **88** | HIGGS relies on the same clamp, plus `g_playerSpaceBodies` compensation we won't have |

**Overall: GO, with the ordering constraint.** Track A is a high-value, low-cost, instantly-reversible
improvement to a body every downstream mod already respects. Track B is a genuinely novel capability
(finger-shaped Havok contact + our own touch-event source) that is **strictly gated on Track A passing
its spike**, and whose value proposition must be honestly framed first: *it moves flesh; it does not
make the NPC notice.*

---

## Open questions for the user

1. **Track C (our own contact listener + haptics)** — build it, or ship "flesh moves, she doesn't
   react" and document it? This changes Risk 4 from a perception bug into a feature. Decide before B.
2. **Track A alone might be enough.** A narrowed palm ridge on HIGGS's *own* body keeps every existing
   integration and fixes the "poorly placed rectangle" feel. Is the finger articulation worth the
   450 lines and the lifecycle surface, given it buys no grab, no hits, no CBPC?
3. **Index knuckle controllers** (per-finger tracking) would splay middle/ring/pinky independently,
   breaking the "one plate per row" rigidity assumption. Does the user run Index controllers?
4. **Left/right asymmetry:** HIGGS negates `offset.x` for the left hand. Our boxes read world
   transforms so they mirror for free — but confirm the left hand's `NPC L Hand` node scale is also
   0.8517 in the shipping skeleton before hard-coding the fallback constant.

---

## ★ USER ARCHITECTURE DECISION (2026-07-08) — both "blocking" risks are dissolved, not mitigated

The recon flagged two showstoppers. The user resolved both. **This section overrides the pessimism above.**

### RISK 1 "the slab masks the fingers" → NOT A BLOCKER. Just dial the slab down.
HIGGS's player-hand collider (~10x3x18 cm box) is a Havok body like any other: it **renders in the
collision visualizer**, so the user can see and dial it exactly the way every NPC capsule was dialed.
The mitigation isn't a separate "Track A spike" — it's the first move of the normal dialing loop.
- **SHRINK IS THE SAFE DIRECTION.** A live `hkpBoxShape::halfExtents` write is ~85% verified (analogous
  to PPB's proven CapFix capsule float writes, but on a box). Its failure mode when SHRINKING is *missed
  collisions*, never a CTD. **Never grow it to test** — grow is the unverified direction.
- ⚠ OPEN DETAIL: half-extents are shape floats and should persist. **POSITION may not** — if HIGGS
  re-applies the hand body's transform every frame, a positional offset gets stomped. Check whether the
  box is directly on the hand body (position owned by HIGGS, must write after HIGGS in the 0xB266AB chain)
  or wrapped in an hkpConvexTransformShape (offset lives in the shape = persists like the half-extents).
  Resolve this BEFORE promising positional dialing.

### RISK 4 "no hit attribution, ever" → TRUE OF HAVOK, AND IRRELEVANT. Use CBPC.
Havok gives no per-box hit attribution through HIGGS's 2-stage grab. So don't ask Havok.
**CBPC zones are NAMED, NiNode-space analytic spheres that never touch Havok** — and **VRTouchEvents
already consumes CBPC nodes by name** (that is literally its detection mechanism). Put a CBPC node at
each fingertip and per-finger attribution is free, in a language the stack already speaks.

### THE RESULTING SPLIT — remember this, it generalizes
- **Havok boxes/capsules = PHYSICS.** Contact, push, ragdoll force transfer. Unnamed, unattributable.
- **CBPC named nodes = SEMANTICS.** Which finger, which body region, which event. Free attribution.
Do not try to make Havok do semantics (that was the mistake behind chasing hkpShapeKey here). Note this
does NOT retire task #11 (per-capsule grab ID on the NPC's *ragdoll* sub-shapes) — HIGGS grabs Havok
bodies, so that one still needs the shape key or the geometric fallback. Different problem, same lesson.

### Build order that follows
1. Dial HIGGS's hand slab DOWN in the visualizer (shrink only), same loop as the NPC capsules.
2. Add the 4 follower boxes per hand (2 index segments + 2 for the middle/ring/pinky group, no thumb),
   riding the live VR finger bones. Reuse collviz_markers' proven runtime-body machinery.
3. Add CBPC fingertip nodes for attribution; VRTouchEvents picks them up by name with no new plumbing.

---

# ★★ THE 2026-07-29 RUNAWAY-COLLIDER BUGS (user-reported, both fixed)

A user reported *"finger collision stuck somewhere"* — colliders acting from a place their hand
wasn't. Their `PPB.log` contained two independent defects. Both are now fixed; both are worth
understanding because each is a general class, not a one-off.

## Bug 1 — REBUILD CHURN: 60 destroy/create cycles per session, all obsolete

**Evidence:** 64 `HBOX CREATE` + 62 `HBOX DESTROY` in one 38-minute session, **every destroy
`reason=scaleChange`**, with the player hand scale merely jittering (0.8005 · 0.8245 · 0.8252 ·
0.8492 · 0.8500 · 0.8755).

**Root cause:** the trigger fired on **1% hand-scale drift**
(`fabs(scale − scaleAtCreate) > 0.01f * scaleAtCreate`) — and that teardown had been **obsolete
since the 2026-07-10 rewrite**. `SolveGeometry`'s own header says it: anchors, tilt, length AND
half-extents are re-solved LIVE every consumed snapshot, *"no recreate needed"*. The destroy path
was simply never removed when the live re-solve landed.

**Why churn breaks the colliders (this is the interesting part):**
- every `CREATE` starts **collision-OFF (bit 14) with a 100 ms enable delay** → at 60 rebuilds the
  boxes spend meaningful time inert, so touches silently miss;
- every `CREATE` **re-derives the filter word from HIGGS's hand body** — and that user's log shows
  the player collision GROUP changing **16 times** (`word=0x0009C438` → … → `0x0889C438`), so churn
  hands a stale anchor repeated chances to be sampled.

**Fix:** threshold is now the knob **`handBoxRebuildFrac` (default 0.15)**, accessor-clamped to a
0.02 floor — only a genuine form change (werewolf / vampire lord) rebuilds. Churn 60 → ~0.

**The transferable lesson:** *when a rewrite makes a teardown unnecessary, DELETE the teardown.*
It survived here as a comment claiming it wasn't needed sitting six lines above the code doing it.

## Bug 2 — NO POSITION LEASH: a velocity clamp cannot catch "arrived wrong and stopped"

**What HIGGS does** (verified in `higgs/src/physics.cpp ApplyHardKeyframeVelocityClamped`, and it
is exactly what the user intuited): keyframe toward the target, then **if the resulting velocity
exceeds the body's max, hard `setPosition` to the target**. PPB already replicated this — and
improves on it by re-applying the keyframe after the teleport so the residual velocity is ~0
(HIGGS leaves the big velocity on the body).

**The gap:** that check is on **VELOCITY ONLY**. It catches *"can't keep up"*; it can never catch
*"tracked a wrong target perfectly."* PPB's boxes run `handBoxFollowMode 2`, anchored to **HIGGS's
hand rigid body**, not the skeleton hand node. If that anchor is ever stale or displaced (hand
disabled, two-handing, beast recreation, a tracking glitch, or the churn above), our boxes follow
it **smoothly** to the wrong place — no velocity spike, no teleport, wrong forever.

**Fix — a two-part leash, both measured against the skeleton hand node (always-valid ground truth):**
1. **Reject the runaway ANCHOR at source** (`UpdateEffFrames`): if HIGGS's hand body is further than
   `handBoxLeashU` (30u ≈ 43 cm) from the hand node, ignore it and ride the node this frame. This
   fixes the *cause* — teleporting to a bad target would have achieved nothing.
2. **Snap the BODY home** (`KeyframeAll`): if a box itself ends up beyond the leash regardless of
   velocity, teleport + re-keyframe, reusing the proven spike path.
Both log up to 3× per hand with the measured distance, so the next report arrives with evidence.

**The transferable lesson:** *a rate/velocity guard and a position guard catch disjoint failures.*
Any system that follows an external anchor needs a sanity check against a source it OWNS.

## What the same log ALSO proved was healthy
- **No orphaned bodies.** The world-change destroy logged `ownerHeld=true`, i.e. the strong
  `NiPointer<bhkWorld>` held the creation world alive and removal came from the correct world. The
  2026-07-14 orphan-capsule fix is holding. (`removedInWorld=false` there is only the diagnostic
  "the stored world != the live world" flag — NOT a failed removal.)
- PPB listening for **both** sender names (`hdtsmp64` and `hdtSMP64`) is what let the FSMP handshake
  be diagnosed at all — see below.

## Third finding from the same log: two silent misconfigurations
Not PPB bugs, but they made that user's install run two features short — worth checking in any
"PPB is wrong for me" report:
- **`Heels Fix NOT installed`** → the heel offset is disabled outright; every heeled NPC keeps a
  barefoot-height Havok body. PPB logs it as a required dependency.
- **`FSMPLINK: interface major mismatch — link stays OFF`** → their engine announced itself as
  **`hdtSMP64` interface 1.0.0**; PPB compiles against **2.0.0**. The capital-S sender name is
  **HDT-SMP Flex** (`hdtSMP64.dll`, build string `Flex for VR - .8.0.1`), not an old Faster HDT-SMP
  (`hdtsmp64.dll`, interface 2.0.0, verified working at **4.0.1**). Consequence: hair/tail capsules
  still ride and collide, but PPB cannot PUSH the SMP bones — the headline SMP feature is silently
  off. Flex *does* implement the interface (its DLL exports `PluginInterface@hdt`), just at major 1.
  **Open idea:** accept interface 1 in READ-ONLY mode (census reads may be ABI-safe since Bullet
  matches at 3.24; only the push forces ride the compiled layout) so Flex users get touch detection
  instead of nothing. Needs Flex's interface-1 header verified before it is attempted.

---

## THE RUNAWAY-FINGER FIX, CONFIRMED FROM LOGS (2026-07-29)

Both halves of the fix are now backed by measured data rather than reasoning. Two logs:
**`kemer`** (the original reporter, SMP Flex, interface 1.0.0) and **the author's own machine**
(FSMP 4.0.1, interface 2.0.0). Analysis script:
`scratchpad/analyze_hbox.py` — it replays a log's real scale sequence through both the old and new
rule and counts the rebuilds each would produce.

### Half 1 — the LEASH: caught in the act, 272u out

The author's log contains the failure itself, with both stages firing 131 ms apart:

```
[21:13:44.339] HBOX leash: R HIGGS hand anchor is 272.7u from the hand node (> 30u)
                            — falling back to the skeleton node this frame
[21:13:44.339] HBOX leash: L HIGGS hand anchor is 271.2u from the hand node (> 30u) — ...
[21:13:44.470] HBOX leash: R box 0 was 269.6u from the hand node (> 30u) — snapped home
[21:13:44.470] HBOX leash: R box 1 was 270.1u ... L box 0/1/2 was 264.7/265.7/267.7u — snapped home
[21:16:25.304] HBOX leash: L HIGGS hand anchor is 45.8u ...
[21:17:16.762] HBOX leash: R HIGGS hand anchor is 47.5u ...
```

**272.7u ≈ 3.9 metres**, on BOTH hands simultaneously. That is the "finger collision stuck
somewhere" report, measured.

The two stages are not redundant, and this log proves why:
* **Part 1** (reject the anchor) fired first — but the boxes had *already* been carried out there.
* **Part 2** (position snap) was still needed 131 ms later to recover the displaced bodies.

A body sitting 270u away with **zero velocity** is invisible to every velocity clamp — HIGGS's own
and ours. Without Part 2 those boxes stay wrong forever. This is the concrete case behind the
ledger rule *"a velocity clamp and a position leash catch disjoint failures."*

Smaller excursions (45.8u, 35.6u, 47.5u) were also caught, so the failure is not purely a
once-per-session teleport event.

### Half 2 — the CHURN: 60 → 0 rebuilds, replayed against real scale data

| log | window | scaleChange destroys | worldChange | scale range | **max drift from built-at** |
|---|---|---|---|---|---|
| kemer | 23.0 min | **60** | 2 | 0.8005 – 0.8755 | **8.57 %** |
| author | 25.0 min | **10** | 0 | 0.8264 – 0.9639 | **14.26 %** |

Replaying both sequences: the old ~1 % rule produces 30 rebuilds per hand (kemer) and 5 per hand
(author); the new rule produces **0** in both. The 2 `worldChange` rebuilds correctly survive — a
cell change genuinely invalidates the rig.

Each suppressed rebuild also removes a 100 ms collision-OFF window and a re-derivation of the
collision filter from HIGGS's hand body — i.e. the churn was *also* re-sampling a possibly stale
anchor 60 times, feeding Half 1.

### ⚠ The first threshold was too tight — and a zero-value footgun

`handBoxRebuildFrac` shipped at **0.15**, chosen before the author's log was analysed. Against that
log's measured **14.26 %** drift it left only **0.74 percentage points** of margin. Raised to
**0.50**.

Worse, the accessor floor-clamped to `0.02`, so setting the knob to the intuitive `0` produced a
**2 % threshold — more aggressive than the 15 % it replaced.** A knob whose zero value is more
dangerous than its default is a trap. Corrected: `<= 0` now means *never rebuild on scale*
(returns an unreachable `1e9`), and positive values keep the 0.02 floor so a typo cannot reinstate
the churn.

The rebuild is **vestigial in the first place**: geometry is re-solved live every consumed
snapshot, and a beast-form change is handled by a different path entirely
(`handBoxBeast` → `DestroyHand("off")` in `RigLifecycle`). Nothing needs the scale rebuild; a wide
threshold costs nothing and a narrow one costs collider flicker.

> **Rule:** pick a guard threshold from MEASURED data, not from what sounds conservative. 15 %
> sounded generous and was nearly breached by the very next log examined.

### Diagnostic gap closed: the log cap hid the true rate

Both leash sites logged only the **first 3 events per hand**, and both hit that cap in the author's
log — so the log could not distinguish *"fired 6 times"* (a real one-off glitch, now fixed) from
*"fires every frame"* (a false positive silently degrading follow-mode 2 to mode 0, which would
feel like sluggish colliders rather than absent ones). Both sites now keep a running total and emit
one summary line every 256 events. **A capped diagnostic that reaches its cap has stopped being a
measurement.**
