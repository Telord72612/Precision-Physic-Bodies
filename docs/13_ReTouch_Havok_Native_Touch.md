# 13 — ReTouch: Havok-Native Touch Detection (CBPC replaced)

**Module of the Precision Physic Bodies technical reference.**
Scope: the ReTouch layer — Havok-native touch detection built entirely on PPB's own capsules,
replacing CBPC for touch. Covers the architecture law, the geometry exports, the fingertip /
source-classifier, the mouth gate (the proof-of-concept feature), held-object identity, the
capsule→body-part mapping method, and the marker-seizure incident that hardened the rules.
**Audience: an engineer with zero prior context.** Everything below shipped and was verified
in-game 2026-07-24/25 (user: "this is pretty amazing stuff").

Companion docs: `04` (ReShape — the capsules ReTouch reads), `01` (pitfall ledger),
`tools/ppb-scratch/retouch_notes.txt` (the working notes + CBPC knowledge bank),
VRTouchEvents Module `12` (the migration guide written FROM this doc).

---

## 0. TL;DR / The Architecture Law

**Touch = pure geometry computed every frame against the NPC's REAL welded Havok capsules.
Nothing is ever placed, spawned, or tracked.** A capsule that is part of the ragdoll can never
be where the body is not — no lag, no orphans, no drift, no cleanup. Detection reads live
transforms at query time; identity comes from asking the owning system (HIGGS for held
objects, our own HandBox for the player's hand parts), never from meshes.

CBPC is fully replaced for touch: richer data (exact capsule, contact distance, L/R hand,
finger-vs-palm-vs-fist, held-object name), near-zero marginal cost (no parallel collision
system), and per-NPC correctness inherited from ReShape for free.

```
player HandBox tip (ours) ──┐
player finger node (fallback)├── per-frame distances ──> AND-gates ──> reactions (phoneme…)
held object (HIGGS identity)─┘        to REAL capsules       │
                                   (GrabDiag exports)        └──> log streams:
                                                                  MOUTHTOUCH / OBJTOUCH / TOUCHPROBE
```

## 1. The Geometry Exports (CapFix.h, namespace GrabDiag)

The foundation everything else stands on — live capsule geometry in world space, Skyrim units:

```cpp
bool ReadCapsuleWorldU(RE::Actor*, int slot, int child, float aOut[3], float bOut[3], float* rOut);
bool SlotBodyPoseU(RE::Actor*, int slot, float posOutU[3], float rotOut[9]);
const char* SlotLabel(int slot);                 // "hand","forearm",…,"com" (12 slots)
int  SlotLiveChildren(RE::Actor*, int slot);     // list child count; 0 = single capsule
```

Implementation walk: `FindNode(kSlotNode[slot]) → bhkCollisionObject → bhkRigidBody →
hkpRigidBody`; world pose from `hkp->motion.motionState.transform` (hkRotation col0/1/2 =
COLUMNS; translation × kHavokToSkyrim); child capsules via `hkpListShape::childInfo[i]`;
local verts transformed `world = pos + R·v × kHavokToSkyrim`.

**Because these read the LIVE bodies, everything the apply path wrote — ReScale, ReShape,
the head channel, per-NPC fits — is already inside every answer.** That one sentence is why
ReTouch inherits the whole PPB stack for free.

## 2. The Player's Hand: Fingertip + Source Classification

The player's hand colliders are PPB's own HandBox rig (4 boxes/hand: 0/1 = index-finger pair,
2 = fist slab, 3 = palm plate). Exports (HandBox.h):

```cpp
bool TipWorldU(int hand, int box, float outU[3]);      // box CENTER + half-length down local +Z
bool BoxCenterWorldU(int hand, int box, float outU[3]);
```

Tip = center extended `half[2] (METRES) × kHavokToSkyrim` along the box's local +Z
(down-the-bone), z-axis derived from the pose quat. Fallback when the rig isn't live:
the player's `NPC R/L Finger12 [RF12]/[LF12]` node (≈2u behind the true tip — thresholds absorb it).

**Source classifier** (the "was it a poke, a palm, or a fist?" answer — YES, per-box identity
is free because every box is ours):

```
per touched capsule: distance from each of the 4 box centers;
  FINGER = an index box clearly nearest (margin 1u)
  PALM   = the plate clearly nearest
  HAND   = the slab, or no clear winner (fist / full grip)
```

The L/R wand identity that VRTouchEvents had to recover by call-site-hooking cbp.dll is a
**pointer comparison** here. Logged in every probe line: `TOUCHPROBE R[FINGER] head.C1 d=0.4u`.

## 3. The Mouth Gate (proof-of-concept feature, working in-game)

Finger in an NPC's mouth → her lips form an O. Every piece generalizes.

**The head capsule map** (eye-confirmed by flash-probe, §5):

```
head.C1     UPPER LIP      head.C2/C3  CHIN (R/L jaw lines — far ends reach Y≈2, i.e. DEEP)
head.C4/C5  CHEEKS (R/L)   head.C6/C7  cheekbones (R/L)
head.C8     NOSE ★ (the capsule the head channel scales — same anatomy as the facegen landmark)
head.C9     PALATE (fat centreline bar)
```

**The gate — multi-capsule AND with staged depth (user-designed):**

- **ENTER (strict, front only):** fingertip within `mouthEnterU` (2.2) of **C9 AND C4 AND C5**
  simultaneously. A palm on a cheek is near ONE capsule, never all three. Additionally the
  source classifier must say FINGER (`mouthNeedFinger`) — a fist mashed into the face stays shut.
- **DEEP HOLD:** entry capsules sit forward; a finger pushed all the way in leaves their range.
  Once in: stay while **both chins < mouthDeepChinU (2.5) AND palate < mouthDeepPalU (5.0)**.
  The asymmetry is the safety: from UNDER the jaw (outside) the palate is far — below-jaw
  touches can't fake "inside."
- **EXIT:** only when the front condition (past `mouthExitU` 3.2) AND the deep hold both fail.
- **ENTER-THROUGH-THE-FRONT-ONLY is the design principle:** the permissive hold state is only
  reachable via the strict entry, so every false-positive path stays dead.

**The reaction — facegen phonemes from C++** (verified API, CommonLibVR-4.14.0):

```cpp
auto* fad = actor->GetFaceGenAnimationData();          // Actor virtual 063
fad->phenomeKeyFrame.SetValue(idx, value01);           // NOTE: "phenome" spelling; 16 slots
```

A wide-open O is a BLEND: `Oh` (11) rounds the lips but barely drops the jaw; `BigAah` (1)
underneath opens it. One normalized 0..1 ramp (rate knobs in/out), each channel applies its
own max (`Oh × 1.0 + BigAah × 0.5`). Ramp = smooth open/close, no animation files.

## 4. Held-Object Identity (OBJTOUCH) — "what is pressed against her?"

**Identity NEVER comes from the mesh — it comes from asking the system that holds the object.**
HIGGS interface (already acquired by PPB): `IsHoldingObject(isLeft)` + `GetGrabbedObject(isLeft)`
→ `TESObjectREFR` → base form name. Contact = the object's **worldBound** (center, minus its
radius = surface distance) vs her nearest capsule:

```
OBJTOUCH R 'Apple Dumpling' vs spine0.C3 d=0.12u
```

Verified in-game with an actual Apple Dumpling against Lydia's waist, then chin. For weapons
the same principle holds via the equip system (and CBPC's old triangle path is documented in
VRTouchEvents Report 03 — not needed here).

## 5. The Mapping Method (TOUCHPROBE + flash-probe)

- **TOUCHPROBE** (~2 Hz): logs the nearest solid capsule to each fingertip with the source tag —
  touch a part in VR, read the log, record the label. This is how the head map was built.
- **Flash-probe**: to identify a child, write its baked values + 15u forward (knob), the user
  names what's floating, then **write the baked values back**. ⚠ `Enable 0` does NOT revert a
  driven capsule — the live shape keeps the last write. Undo by WRITING (pitfall #: stopping
  is not undoing). Baked values come from the `BEFORE` debug lines (`logVerbose 1`).

## 6. The Marker-Seizure Incident (why the rules are laws now)

11 UV-landmark visualization markers auto-placed on a re-latch (knob left armed) put static
bodies inside rig/ragdoll territory — ON THE HIGGS (player-hand) LAYER. Every driven capsule
and the PLANCK ragdoll fought embedded statues: whole-body seizure, engine freezes, AND a
Papyrus **playerImpact event storm** (the user's profiler caught it — physical impacts on a
player-layer body raise player-impact script events; at 90fps against 11 statues the VM dumps).
Log shows placement→distress in TWO MILLISECONDS.

**Hardened rules:**
1. Visualization bodies are **L_NONCOLLIDABLE (layer 15)** — collidable by NOTHING, ever.
   A marker on a live layer is a fake player hand to every consumer in the stack.
2. Marker knob-off **actively removes** parked bodies (`ClearMeshMarkers`) — stopping ≠ undoing.
3. Detection uses **zero placed bodies** (the architecture law, §0) — so detection can never
   cause this class of bug even in principle.
4. Parked bodies die with their world (save-load) — but "the shaking stopped" may only mean
   the actor moved off the spot. Bodies persist until world death.

## 7. Performance

Per frame: ~10 capsule-distance checks for the mouth gate + 4 box classifications, only when a
tracked NPC is within `ghostRangeU` (250u). TOUCHPROBE/OBJTOUCH throttled to 2 Hz (~100 checks).
No Havok objects created, no contact listeners needed so far — the physics engine is never
asked to do anything extra. Contrast CBPC: a parallel per-frame collision system over every
actor pair within 1024u.

## 8. Reuse Map

- **VRTouchEvents** (the consumer): Module 12 there — migration from CBPC events to this stream.
- **AIHands** (the sibling): Module 17 there — the toolkit reads (NPC hand vs player body works
  symmetrically; the classifier concept applies to NPC finger contact).
- Knobs: all live in `PPB_tuning.txt` under the ghost/mouth block (gate distances, phoneme
  blend, classifier margin, ranges).
