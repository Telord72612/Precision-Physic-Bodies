# 15 — The Capsule → Body-Part Map (the API naming table)

**Module of the Precision Physic Bodies technical reference.**
Scope: a name for every touchable collision capsule on PPB's human female skeleton, so any
consumer (VRTouchEvents, SkyrimNet, AIHands, a future PPB touch API) can report *"he touched her
left inner thigh"* instead of *"capsule slot 8 child 3"*. Built 2026-07-29 by reading the shipped
`skeleton_female.nif` geometry directly, cross-checked against the code's own region table and
the dial-session records.

Companion docs: `13` (ReTouch — how touch is detected), `04` (ReShape — which children get
shape-fitted), `06` (FleshFit — how they were dialled), VRTouchEvents `13` (the consumer contract).
Machine-readable form: `capsule_body_part_map.json` beside this file.

---

## 0. How to read this, and how much to trust each row

Addressing is **`slot.child`**: 12 slots (`CapFix.cpp kSlotNode[12]`), each a `bhkListShape`
whose children are `main` (index 0) then `C1..Cn`. Left limbs are **X-mirrors** of the right with
identical child layout — verified on L Thigh — so one table covers both; the consumer supplies
the side from which slot it read (`thighR` vs its `L Thigh` twin).

Every row carries a **confidence tag**, because these came from three different sources:

| tag | meaning |
|---|---|
| **CODE** | the source itself assigns this child to this region (`RegionForSlotChild`) — certain |
| **EYE** | eye-confirmed in VR during a dial session and recorded in the docs/tuning — certain |
| **GEO** | classified by me from the capsule's coordinates in the NIF — anatomically sound, **not eye-verified** |

**Bone-local axes** (needed to read the coordinates): for the torso and head, **+Y = FRONT
(anterior)**, **+Z = up** for torso / up-the-skull for head, **±X = lateral**. For limbs, **+Z runs
down the bone** (thigh: hip→knee; calf: knee→ankle; upperarm: shoulder→elbow; forearm:
elbow→wrist). Confirmed by anchors whose identity is known: breasts protrude at +Y, butt cheeks
sit at −Y, the nose is the furthest +Y point on the head.

⚠ **GEO rows are a proposal, not a certification.** They're consistent with the geometry and with
how the body was dialled, but the honest verification path is one TOUCHPROBE session per region
(doc 13 §5) — touch it in VR, read the log line, confirm or correct the label. Treat GEO as "good
enough to ship behind a label the user can rename", not as eye-verified truth.

---

## 1. HEAD — slot 3 (23 capsules)

The most finely mapped region: the mouth gate lives here, and most of it is EYE-confirmed.

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 5.52 | **cranium / skull** | GEO |
| `C1` | 0.88 | **upper lip** | EYE |
| `C2` / `C3` | 0.88 | **jaw line / chin** (R/L) | EYE |
| `C4` / `C5` | 0.88 | **cheek** (R/L) | EYE |
| `C6` / `C7` | 1.65 | **cheekbone** (R/L) | EYE |
| `C8` | 0.77 | **nose** ★ (also the head-scale landmark) | EYE |
| `C9` | 1.32 | **palate** (roof of the mouth) | EYE |
| `C10` | 0.88 | **throat wall** — back of the mouth cavity | EYE |
| `C11` | 0.88 | **under-jaw / deep throat** | GEO |
| `C12` / `C13` | 1.21 | **temple / ear side** (R/L) — *these are the EAR capsules on beast heads* | EYE |
| `C14` | 3.30 | **back of head / occiput** | GEO |
| `C15..C22` | 0.50 | ⛔ **degenerate seeds** (A=B=origin, buried) — horn/antler/ear stock, **never touchable** | CODE |

**Mouth gate uses:** ENTER = C9 **and** C4 **and** C5 within 2.2u; DEEP = both chins (C2/C3)
< 2.5u **and** palate (C9) < 5.0u.

---

## 2. SPINE2 — slot 6 (15 capsules) — chest, breasts, shoulders

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 6.00 | **chest ring** (upper torso circumference) | CODE (chest) |
| `C1` / `C2` | 3.00 | **flank / lat** (R/L side of ribcage) | GEO |
| `C3` / `C4` | 3.00 | **front rib / upper chest** (R/L) | GEO |
| `C5` | 3.00 | **lower chest / diaphragm** | GEO |
| `C6` / `C7` | 2.50 | **trapezius / shoulder top** (R/L) | GEO |
| `C8` / `C9` | 2.00 | **collarbone** (R/L) | GEO |
| `C10` | 4.00 | **sternum / cleavage** (between the breasts) | GEO |
| `C11` / `C12` | 3.00 | **BREAST** (R/L) ★ | CODE |
| `C13` / `C14` | 2.00 | **deltoid / shoulder cap** (R/L) | GEO |

★ C11/C12 are **fully computed** by ReShape from the UV nipple landmark — sag, inflation and
protrusion are per-NPC. They're the only torso children exempt from ring scaling.

---

## 3. COM — slot 11 (21 capsules) — pelvis, butt, hips, intimate anatomy

The densest slot, and the one with genuine care required: the **orifice ring must never fatten**
(a bigger ring radius *shrinks* the opening), which is why the code protects it structurally.

| child | r (u) | body part | tag |
|---|---|---|---|
| `main`, `C1`, `C2` | 1.00, 0.50, 0.50 | **orifice ring stack** (rear centreline, stacked upward) ⚠ never scaled | CODE |
| `C3` / `C4` | 1.00 | **inner pelvis wall / groin rail** (R/L) ⚠ never scaled | CODE |
| `C5` | 1.00 | **pubic mound / front pelvis** | GEO |
| `C6` / `C7` | 1.00 | **groin crease** (R/L) | GEO |
| `C8` / `C9` | 1.00 | **front hip / iliac** (R/L) | GEO |
| `C10` / `C11` | 1.00 | **rear centreline** (between the cheeks) | GEO |
| `C12` / `C13` | 2.00 | **upper glute / sacrum** (R/L) | GEO |
| `C14` / `C15` | 0.50 | **inner pelvis upper rail** (R/L) ⚠ never scaled | CODE |
| `C16` / `C17` | 4.00 | **BUTT CHEEK** (R/L) ★ | CODE |
| `C18` / `C19` | 3.50 | **HIP** (R/L) | GEO |
| `C20` | 1.00 | ⛔ **duplicate of C10** (identical coordinates) — spare seed | GEO |

★ C16/C17 take the butt shape measure (3D translation + radius, per-NPC).

---

## 4. SPINE0 — slot 4 (8 capsules) — **WAIST** (top of pelvis / bottom of belly)

⚠ Note the naming inversion, corrected in code 2026-07-18: **spine0 = waist, spine1 = belly.**
Spine0 sits *under* spine1.

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 4.00 | **waist ring** | CODE (waist) |
| `C1` / `C2` | 2.00 | **lower belly / front waist** (R/L) | GEO |
| `C3` | 3.00 | **front waist band / navel line** | GEO |
| `C4` | 3.00 | **lower abdomen** (centre) | GEO |
| `C5` / `C6` | 3.00 | **lower back** (R/L) | GEO |
| `C7` | 3.00 | **flank / love handle** (R — unpaired; the L twin child was trimmed) | GEO |

## 5. SPINE1 — slot 5 (7 capsules) — **BELLY** (navel band)

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 6.00 | **belly / navel** | CODE (belly) |
| `C1` / `C2` | 3.50 | **belly side** (R/L) | GEO |
| `C3` / `C4` | 3.00 | **midriff side** (R/L) | GEO |
| `C5` / `C6` | 2.00 | **flank** (R/L) | GEO |

## 6. NECK — slot 7 (1 capsule)

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 3.25 | **neck / throat** — the choke target | CODE (base) |

---

## 7. ARMS

### UPPERARM — slot 2 (4 capsules)

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 3.00 | **upper arm, shoulder half** (biceps/triceps upper) | CODE (upperArms) |
| `C1` | 2.60 | **upper arm, elbow half** | GEO |
| `C2` | 2.00 | **shoulder / deltoid cap** | GEO |
| `C3` | 2.20 | ⚠ **duplicate span of C1** — second lower rod (thin overlay) | GEO |

### FOREARM — slot 1 (2 capsules)

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 2.00 | **forearm, elbow half** | GEO |
| `C1` | 2.00 | **forearm, wrist half** | GEO |

### HAND — slot 0 (5 capsules)

⚠ **Naming trap:** the *knobs* `capHandC1/C2/C3` address children **0/1/2** (historic offset).
The API should use raw child indices and ignore the knob naming.

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 1.00 | **palm / hand centre rod** | EYE |
| `C1` | 1.00 | **thumb-side rod** | EYE |
| `C2` | 1.00 | **pinky-side rod** | EYE |
| `C3`, `C4` | 1.00 | ⛔ **duplicates of C1** (identical coordinates) — unused seeds | GEO |

---

## 8. LEGS

### THIGH — slot 8 (7 capsules) — `+Z` runs hip → knee

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 2.00 | **thigh bone rod** (full length) | CODE (thighs) |
| `C1` | 5.00 | **upper thigh / groin junction** | GEO |
| `C2` | 3.00 | **hip / glute fold** (above the joint) | GEO |
| `C3` | 5.50 | **upper thigh** (thickest ring) | GEO |
| `C4` | 5.00 | **mid thigh** | GEO |
| `C5` | 3.50 | **lower thigh** (above the knee) | GEO |
| `C6` | 1.00 | **knee** | GEO |

### CALF — slot 9 (5 capsules) — `+Z` runs knee → ankle

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 1.50 | **calf bone rod** (full length) | CODE (base) |
| `C1` | 3.50 | **upper calf** (below the knee) | GEO |
| `C2` | 3.00 | **calf belly** (thickest) | GEO |
| `C3` | 2.50 | **lower calf / shin** | GEO |
| `C4` | 1.00 | **knee (rear) / kneecap underside** | GEO |

### FOOT — slot 10 (4 capsules)

| child | r (u) | body part | tag |
|---|---|---|---|
| `main` | 1.50 | **sole rod (inner)** | EYE |
| `C1` | 1.20 | **sole rod (outer)** — full foot length | EYE |
| `C2` | 0.80 | **arch / inner sole** | GEO |
| `C3` | 1.20 | **ankle lock** | EYE |

---

## 9. Touchability summary

Of ~130 capsules on the human female skeleton:

- **~118 are real, reachable surfaces** with a body-part name.
- **8 are degenerate seeds** (head `C15..C22`, A=B at the origin) — buried inside the skull,
  never touchable. Filter them out: `A ≈ B` is the test.
- **~4 are duplicates** (`com.C20`, `hand.C3/C4`, `upperarm.C3` overlays) — a consumer should
  collapse them onto their twin's label rather than report a distinct part.
- **7 are protected anatomy** (COM ring + inner-pelvis rails) — touchable, but never radius-scaled
  by ReShape, by design.

**A consumer's filter, in order:** skip if `|A−B| < 0.1u` **and** `r < 0.6u` (seed); collapse
known duplicates; then look up the label.

---

## 10. Per-skeleton differences (⚠ do not assume this table everywhere)

This table is the **human female** NIF (`skeleton_female.nif`), which also covers elves, orcs and
Draenei bodies. The others differ:

| skeleton | head children | notes |
|---|---|---|
| `skeleton_female` | 23 | this table |
| `skeleton_female_draenei` | 23 | byte-copy of human head; horns dialled into the C15+ seeds |
| `skeletonbeast_female_khajiit` | 17 | **C12/C13 = right ear, C15/C16 = left ear** (dialled, not seeds) |
| `skeletonbeast_female` (Argonian) | 17 | horns + dorsal ridge baked; spine children differ (10/9/17) |

Torso/limb layouts are otherwise shared. Beast heads need their own mapping session before a
consumer can name their face capsules.

---

## 11. Naming convention for the API

Recommend `region_sub_side`, lowercase, stable:

```
head_lip_upper      head_chin_r        head_palate       head_throat
chest_breast_l      chest_sternum      chest_collarbone_r
belly_navel         waist_ring         waist_back_l
pelvis_butt_r       pelvis_hip_l       pelvis_groin_r     pelvis_ring
arm_upper_r         arm_forearm_wrist_l    hand_palm_r
leg_thigh_mid_r     leg_calf_belly_l   foot_sole_r        knee_r
```

Keep the raw `slot.child` alongside the name in every event — the name is for the LLM and the
user, the coordinate is for code, and a rename must never break a consumer.
