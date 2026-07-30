# 15 — The Capsule → Body-Part Map (the API naming table)

**Module of the Precision Physic Bodies technical reference.**
Scope: a name for every touchable collision capsule on PPB's human female skeleton, so any
consumer (VRTouchEvents, SkyrimNet, AIHands, a future PPB touch API) can report *"he touched her
left inner thigh"* instead of *"capsule slot 8 child 3"*. Built 2026-07-29 by reading the shipped
`skeleton_female.nif` geometry directly, cross-checked against the code's own region table and
the dial-session records.

Companion docs: `13` (ReTouch — how touch is detected), `04` (ReShape — which children get
shape-fitted), `06` (FleshFit — how they were dialled), VRTouchEvents `13` (the consumer contract).

> ## ⚠ THE SOURCE OF TRUTH IS NOT THIS FILE
>
> **2026-07-29.** The authoritative record for the API is now generated, not written:
>
> | file | what it is |
> |---|---|
> | `ProposedPartName()` in `NpcFingerTest.cpp` | **the source of truth** — the shipped table |
> | `capsule_api_names.md` | flat human table, generated |
> | `capsule_body_part_map.json` | machine record, generated (`schema ppb.capsule-name-map/2`) |
> | `tools/ppb-scratch/gen_capsule_name_record.py` | the generator |
>
> The generator *parses* the C++ switch and then **cross-checks every name against the strings
> actually present in the shipped `PPB.dll`**, aborting if the record claims a name the binary
> doesn't contain. So the record cannot silently drift from the code. **Do not hand-edit the
> generated files** — change the code table and re-run the generator.
>
> Current: **107 named** across **113** table entries in **12** slots (**6** buried seeds).
>
> This file (`15`) remains the *narrative* — the axes, the confidence tags, the reasoning behind
> each label. Read it to understand; read the generated record to integrate.

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
| `C15..C22` | 0.50 | **buried seeds ON HUMAN ONLY** — see §9.2, corrected 2026-07-29 | CODE |

> ⚠ **The `C15..C22` row above was wrong twice** and is corrected here. (a) They are **not
> degenerate** — `A≠B`: the spec is `p1=(0,0,0) p2=(0,0,2u) r=0.5u`, deliberately, because `A==B`
> is the invisible-capsule trap. (b) They are **not seeds on every race**: the head was never
> index-padded, so argonian/khajiit have only **17** head children (C15/C16 = horn-spine / ears)
> and draenei has 23 with **C15..C22 all real** (horns + ears). Only human has 8 seeds there.
> Never resolve a head index above C14 to a human name.

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
| `C20` | 1.00 | **rear centreline R (twin)** — byte-exact duplicate of `C10`. **LIVE, not a seed** (§9.1); named 2026-07-29 so the API can't report it as unknown | GEO |

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
| `C3`, `C4` | 1.00 | **palm centre** — user-confirmed: they sit with the palm at the centre of the hand (they share C1's baked coordinates, so they are NOT reportable as distinct rods, but they are not dead either) | EYE |

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

---

## 12. ★ THE 2026-07-29 EXTENSION — 15 new capsules on all four skeletons

User directive: the map was missing back/shoulder-blade coverage and all internal pelvic anatomy.
Added with `tools/ppb-scratch/extend_pelvis_back_sensors.py` (pynifly `add_shape`, backup +
verify-by-reload + collateral check; the proven `extend_head_seeds8.py` pattern).

| slot | before | after | new anatomy |
|---|---|---|---|
| COM (11) | 21 | **32** | `C21` clitoris · `C22/C23` entrance outer/inner · `C24/C25` cervix front/back · `C26/C27` uterus lower/upper · `C28/C29` anus outer/inner · `C30/C31` rectum lower/upper |
| spine1 (5) | 7 (Argonian 9) | **11** | `C9/C10` = BACK (R/L) |
| spine2 (6) | 15 (Argonian 17) | **19** | `C17/C18` = SHOULDER BLADE (R/L) |

### ⚠ Why the shorter skeletons got FILLER seeds (the index-alignment law)
The Argonian NIF already used **spine1 C7/C8** and **spine2 C15/C16** for its dorsal RIDGE. Appending
naively would have made `capSpine1C7` mean *back* on a human and *ridge* on an Argonian — the exact
trap the head C15/C16 ear/horn knobs created (README). So the new anatomy was placed **above the
Argonian maximum**, and the shorter NIFs carry buried filler at those indices.
**LAW: when adding a child to one skeleton, add to ALL of them so a knob index means ONE anatomy
everywhere. Pad rather than offset.** All four NIFs now read COM 32 / spine1 11 / spine2 19.

### The five coupled DLL edits (the ledger's four, plus the ceiling)
Growing knob-addressable children is never one edit:
1. `CapFix.cpp` **`kMaxListChildren` 24 → 34** ← the ceiling; COM at 32 would have been silently clipped
2. `Tuning.h` array sizes: `spine1C[10]→[12]`, `spine2C[16]→[20]`, `comC[20]→[32]`
3. `Tuning.cpp` `kArrays` counts: `capSpine1C 12`, `capSpine2C 20`, `capComC 32`
4. `Tuning.cpp` `CapFixChildKnobs`: spine1 `9→11`, spine2 `17→19`, com `21→32`
5. `Tuning.cpp` `CapFixChildSlot`'s `ChildPtr` bounds: `12` / `20` / `32`
Miss #5 and the loop reaches the child but the value-fetcher rejects it — fingerprint is a
`BEFORE` debug line with no matching `APPLIED`.

### Seed convention (unchanged, proven)
`p1=(0,0,0) p2=(0,0,2u) r=0.5u`, material cloned from a live sibling. Never `A==B` (the
degenerate-invisible trap). All 15 ship **`Enable 0`** — nothing changes in game until dialled.

### Dial state (update as the session proceeds)
Dialled on **Lydia** (the neutral reference; COM barely varies between NPCs, so her values carry to
all four skeletons). COM frame confirmed twice over: **+Y = FORWARD** (pubic mound C5 at Y+2.67),
**−Y = REAR** (butt cheeks C16/C17 at Y−6.8), **+Z = up**.

**DIALLING COMPLETE 2026-07-29** — eye-confirmed in VR by the user ("It's perfect"), all
`Enable 1`. Values below are the live `PPB_tuning.txt` contents, read back from the deployed file:

| child | anatomy | status | values (COM frame, units) |
|---|---|---|---|
| `com.C21` | clitoris | **EYE** | A=(0.0, 2.0, 2.3) B=(0.0, 2.4, 4.3) r=0.5 |
| `com.C22/C23` | vaginal opening R/L | **EYE** | A=(±2.0, 1.0, 2.0) B=(±2.0, −2.0, 2.0) r=0.3 |
| `com.C24/C25` | cervix R/L | **EYE** | A=(±2.0, 0.6, 4.8) B=(±2.0, −2.4, 4.8) r=0.3 |
| `com.C26/C27` | uterus R/L | **EYE** | A=(±1.5, 0.6, 7.8) B=(±1.5, −2.4, 7.8) r=0.3 |
| `com.C28/C29` | anus R/L | **EYE** | A=(±2.0, −4.2, 3.2) B=(±2.0, −7.2, 4.0) r=0.3 |
| `com.C30/C31` | rectum R/L | **EYE** | A=(±2.0, −3.7, 8.2) B=(±2.0, −6.7, 9.0) r=0.3 |
| `spine1.C9` | back (lower) | **EYE** | Y=5.485 r=6.033 (main belly moved to Y=6.485, both r−0.5) |
| `spine2.C17` | back (upper) | **EYE** | Y=1.338 r=6.099 (main chest at Y=2.338, both r−0.5) |
| `spine1.C10`, `spine2.C18` | unmirrored twins | **buried seed** | only one back capsule each was wanted |

The front/back split on spine1/spine2 works at **0.5u separation** because ReTouch resolves the
nearest capsule *surface*, not centre — the user was right and my initial 3u proposal was wrong.
Shrinking the radius further would have cost real chest collision volume for nothing.

### ⚠ Open risk before these go live (Report 08)
COM going 21 → 32 children escalates the **contact-point cliff**: Havok stages ≤256 contact points
per BODY PAIR, and two actors' COMs overlapping now presents far more child pairs than the 441 that
were already flagged as unresolved. It costs nothing while they are buried seeds. The architecturally
correct answer is that **internal sensors should not collide at all** — ReTouch detection is pure
geometry (doc 13), so these can be collision-disabled (`hkpListShape::disableChild`, ledger) and add
ZERO pairs while still being readable. Decide before enabling many at once; keep radii small.

---

## 9. Three API traps found by verifying the record (2026-07-29)

Generating the record forced a cross-check against the NIFs and the code, which caught three
things that would each have been a live bug in a shipped API. Scripts:
`tools/ppb-scratch/classify_unnamed_children.py`, `dump_child_counts.py`, `dump_slot_geometry.py`.

### 9.1 Two capsules were LIVE but unnamed on all four skeletons

`nullptr` in the name table was being read as "buried seed". It isn't — it just means *unnamed*.
Classifying every unnamed index against the known seed spec (`p1=(0,0,0) p2=(0,0,2u) r=0.5u`)
found two that are real, touchable geometry on **every** skeleton:

| address | actual geometry | verdict |
|---|---|---|
| `upperarm.C3` | p1=(1.80,0.00,11.20) p2=(1.10,0.50,20.20) r=2.20 | co-located twin of `C1` (elbow half), only thinner (r 2.60→2.20) |
| `com.C20` | p1=(0.01,−7.33,6.63) p2=(−2.50,−7.34,4.63) r=1.00 | **byte-exact duplicate of `C10`** (rear centreline R) |

Both are now named (`upper arm (elbow half, inner twin)`, `rear centreline R (twin)`). The same
pattern explains hand `C3/C4`, which the user identified by eye as sitting at the palm centre —
also now named rather than left to report as unknown.

> **Rule:** before shipping a name table, classify every `nullptr` against the seed spec. A
> live capsule that resolves to "unknown" is an API hole, and the table alone cannot tell you
> which `nullptr`s are seeds.

### 9.2 Index alignment holds ONLY inside the named range

The extension script padded spine1/spine2/COM so one index means one anatomy everywhere — and
that worked. But the **head was never padded**, and slot 4 differs too:

| slot | human | argonian | khajiit | draenei | what diverges |
|---|---|---|---|---|---|
| 3 head | 23 | **17** | **17** | 23 | argonian C15/C16 = horn/spine · khajiit C15/C16 = ears · draenei C15..C22 = horns + ears · human C15..C22 = seeds |
| 4 spine0 | 8 | **10** | 8 | 8 | argonian C8/C9 = dorsal ridge, exist nowhere else |
| 5 spine1 | 11 | 11 | 11 | 11 | C7/C8 = seeds on human, argonian dorsal **RIDGE** |
| 6 spine2 | 19 | 19 | 19 | 19 | C15/C16 = seeds on human, argonian dorsal **RIDGE** |

Names stop at C14 (head), C7 (spine0), C6+C9 (spine1), C14+C17 (spine2) — i.e. exactly below every
divergence — so **the named range is safe on all four**. That is not luck; it is why those indices
are unnamed. A consumer must never resolve an unnamed index to a human name.

### 9.3 `neck` is a single capsule, not a list

Slot 7 is a `bhkCapsuleShape` on all four skeletons — **0 list children**. `neck / throat` is
addressed as child 0 purely by convention. Code that assumes every slot is a `bhkListShape` and
iterates `children` will silently find nothing on the neck.

### 9.4 Positions are NOT in the NIF

Every dialled capsule — all 11 COM sensors and both back capsules — still sits at the buried seed
spec **in the NIF**. The dial values live only as `cap*` knobs in `PPB_tuning.txt` and are applied
at runtime by CapFix. Confirmed by reading both: `dump_slot_geometry.py` shows C21..C31 at
`(0,0,0)→(0,0,2) r=0.5`, while the tuning file holds the real values.

> **Rule:** an API must read capsule positions from the **live Havok body**, never from the NIF.
> The NIF is base geometry; the shipped shape is NIF + knobs + ReScale + ReShape + per-NPC fit.
