# 21 — ReShape: Runtime Body-Shape Fitting (measured mesh)

**Status: FEATURE-COMPLETE (2026-07-19, DLL `00a445bb`). Four-body certified (Lydia / Aela /
Sofia / M'rissi). This is the AS-BUILT reference — how the system works and how to modify it.**
Companion: **Report 20** = ReScale (skeleton size); **Report 23** = OBody/SKEE morph reads + the
slider fallback; **Report 04** = the mesh-sampling pitfalls.

---

## 1. What ReShape is

ReScale (Report 20) fits every capsule to the NPC's SKELETON size. **ReShape fits them to her BODY
SHAPE** — the difference between a skinny NPC and a chubby one at the same skeleton scale.

The method (this is the user's `trueShape` design):
1. **Measure** her live, morphed body mesh — base body + her OBody preset, baked into the vertices
   — in the mesh's own **scale-free reference space** (pre-skin, pose-independent).
2. **Divide** each region's measurement by the neutral for *her own body type* → a per-region
   **shape ratio** (1.0 = neutral shape; >1 chubbier; <1 skinnier).
3. **Apply** that ratio EXACTLY like ReScale applies skeleton scale: capsule endpoints move
   relative to their havok joint AND radius scales, per region.

**Universality is law (user directive):** ReShape is ONE calculation for every NPC. Thin↔chubby is
a single spectrum any race/body can sit on. There are **no per-race or per-NPC shape shims** — if
an NPC misfits, the *math* is wrong (measurement or response), not a body that needs special-casing.
(ReScale keeps race awareness because races inherit *scale*; shape inherits none.) Every residual
in this system's history turned out to be a real math error a shim would only have hidden.

**Scale and shape never contaminate each other.** Measurement is in pre-skin mesh space, so render
scale is invisible to it — empirically proven: a 0.944-scale Redguard and a 1.0 Lydia at zero
slider read BYTE-IDENTICAL girths. ReScale handles size; ReShape handles shape; they compose. Two
NPCs with the same world-space waist but scales 1.1 vs 0.9 have mesh-space waists of 18.2 vs 22.2
→ one reads skinny (capsules pull in), the other fat (capsules push out). Correct, automatically.

---

## 2. The vertex source (the hard-won part — `ResolveMeshSource` in CapFix.cpp)

**In Skyrim VR, body meshes are STATIC `BSTriShape`s, not `BSDynamicTriShape`s.** RaceMenu/OBody
morphs are applied by REGENERATING the raw render vertex buffer — `dynamicData` is an SE-facegen-
only surface (this also explains the project-long `vertDepth=0.00` on the head measure: it was
reading the wrong, empty surface). Read the buffer through three sources, in order:

1. **`GetDynamicTrishapeRuntimeData().dynamicData`** — SE facegen path (float4, stride 16). Rare
   on VR bodies; kept for completeness.
2. **`GetGeometryRuntimeData().rendererData->rawVertexData`** — the geometry's own render buffer.
3. **Skin partition** — skinned meshes (bodies, hair, outfits) keep the SHARED full-mesh buffer on
   `skinInstance->skinPartition->partitions[0].buffData->rawVertexData`. **`NiSkinPartition::
   vertexCount` is the true whole-mesh count** — the geometry's own `vertexCount` is GARBAGE for
   skinned shapes (it reported partition-sized fragments in the dump). This is the path VR bodies
   actually use.

Buffer format: **positions are HALF-FLOAT unless `VF_FULLPREC`**; stride = `vertexDesc.GetSize()`.
(CommonLibVR 4.14: `GEOMETRY_RUNTIME_DATA.rendererData` = `BSGraphics::TriShape{rawVertexData@0x20}`;
`Vertex::VF_VERTEX`, `VF_FULLPREC`=0x400. `HalfToFloat` decoder in CapFix.)

**⚠ Descriptors cannot be trusted blindly — SELF-CALIBRATING DECODE (`PlausibleDecode`).** A
declared-half decode once produced ±65 000-unit coordinate garbage. So: probe 24 spread vertices
under each candidate (stride, precision) drawn from BOTH descriptors, and accept the first
interpretation that is **body-plausible** — finite, |coord| < 500u, real z-spread. First plausible
wins.

**Finding the body (`FindBodyMesh`):** the largest readable trishape with a body-like height
(vertex z-span 55–145u, zmin −10..45u — a head is ~25u, an origin-space garment is off-range). Two
proxy rejects (see §Pitfalls): skip any shape whose name contains **"overlay"/"ovl"** (RaceMenu
render proxies sharing one template buffer), and reject any candidate whose world transform sits
**>200u from the actor root** (parked proxy geometry). `verbose` mode logs every candidate
(`CAND '<name>' vc= src=dyn/raw-half/raw-full stride= zspan= zmin=`).

---

## 3. The sampler (`SampleBodyGirths`)

Bands are **z-fractions of the mesh's own bbox** (ankles≈0, neck≈1), one per region, all live
knobs (`meshBand<Region>Lo/Hi`). An arm/torso x-split (`meshArmFrac`, only applied above zn 0.55 so
thighs never misclassify) separates limb from torso.

**Girth = near-max (the 8th-largest) radial distance** in the band — density-immune (positions,
never averages/counts; the denser mesh just votes with more points; a junk vertex can occupy slots
1–7 without owning the reported girth or the marker). Per-region:

| Region | Measure | Sector rule (why) |
|---|---|---|
| chest | radial from ring centroid | **sides+back only** (front excluded → breast-proof at any height) |
| waist | radial from ring centroid | **front+sides only** (rear excluded → butt-proof; glutes are the cheeks' job) |
| belly | radial from ring centroid | full ring (narrow 2u slice keeps ribs/hips out) |
| breasts | **front protrusion** = (p.y − rear-anchor y) | front-only; rear anchor = the ring's rear-half mean y |
| butt | **rear protrusion** = (front-anchor y − p.y) | rear-only; front anchor = front-half mean y |
| thighs | radial, per side, max side | torso split excludes it above zn 0.55 |
| arms | **\|Δy\| front-back thickness** | edge-zone gated (≤6u of the side's outer edge → breast bleed, 8u+ inboard, can't reach) |

Two extra breast measures ride the front verts:
- **`breastZ`** = breast front-mass mean height (u above mesh bottom) → drives measured sag.
- **`cup`** = MOUND vertical extent: z-extent (4th-high − 4th-low) of verts protruding ≥
  `meshCupFrac` × near-max. (The earlier all-front-verts cup measure SATURATED — the flat chest
  wall spans the band for everyone; the mound threshold reads only breast tissue. Frac 0.8 gives
  clean discrimination: M'rissi's tiny cups read 5.62 vs a neutral 9.24.)

Each region also records the **girth-defining vertex world position** → the eye-verification markers
(§7).

---

## 4. The neutrals — per body type (fingerprinted)

**Each base body is built to different proportions** (offline-measured: 3BA base is +14.7% belly /
+18.7% waist vs Softbody; CBBE base is bustier). So "shape" must be a body's deviation from **its
own** base, not from a single global neutral — otherwise every 3BA-body NPC reads falsely chubby.

Bodies are keyed by their **vertex-count fingerprint** (unique per body type). `RegionData.meshVC`
stores the sampled count; `NeutralOf/NeutralBreastZOf/NeutralCupOf` select the matching set:

| body | fingerprint | chest | breasts | belly | waist | butt | thighs | arms | breastZ | cup |
|---|---|---|---|---|---|---|---|---|---|---|
| **Softbody** (generic build) | 15460 | 13.42 | 8.79 | 7.62 | 9.82 | 12.73 | 6.60 | 3.40 | 84.81 | 9.24 |
| **3BA** base | 18436 | 13.33 | 8.65 | 8.73 | 11.67 | 14.22 | 7.28 | 3.27 | 85.06 | 9.63 |
| **CBBE** base | 13554 | 13.65 | 11.08 | 8.36 | 10.59 | 13.30 | 6.54 | 3.27 | 84.70 | 9.97 |

- **Softbody** = the `meshNeutral*` knob set, captured in-game (§below). It is the generic body
  most NPCs wear (BodySlide "Softbody" superset build).
- **3BA / CBBE** = hardcoded tables in CapFix, derived OFFLINE from the base nifs with the sampler's
  exact math (`tools/pynifly` script). The Softbody row of that derivation matched the in-game
  Lydia capture digit-for-digit → the derivation method is proven; extend it to any new body type.
- **Unknown fingerprint** → falls back to the knob set (Softbody). Most custom followers were built
  in BodySlide against one of these references, so their topology fingerprints to the right family
  automatically and their custom curves read as personal shape deviation. Only a truly hand-sculpted
  novel mesh misses.

**Capturing a Softbody neutral (the `meshNeutral*` set):** spawn a naked, zero-slider NPC on the
generic body (the Redguard `player.placeatme 00048117 1` is scale-1 and ideal, but scale is
irrelevant — mesh space is scale-free), pulse `meshShapeDump`, read the `MESHGIRTH ... breastZ= cup=`
log line, write the seven girths + breastZ + cup into the knobs. Must be NAKED (a dressed NPC's
readable mesh is her outfit's body).

---

## 5. The response — ReShape ≡ ReScale (`ApplyScaleShape`)

**The measured shape ratio applies with the SAME transform ReScale uses for skeleton scale.** This
was the model correction that fixed everything: the old "shape = radius only" rule left capsule
RINGS at neutral distance from the bone (no radius change can pull a ring inward), so nothing moved
on skinny NPCs. Endpoints must scale too.

Let `es` = ReScale's trueScale, `q` = the region's shape ratio (§6). Per capsule class:

- **Torso rings** (spine0/1/2 and their children): `endpoint.xy ×= es·q`, `endpoint.z ×= es`
  (shape NEVER scales height/along-bone), `radius ×= es·q`. Multiplicative from the havok joint,
  which sits near the BACK of the torso → rear capsules (small offsets) barely move while front/side
  capsules carry the growth. **"Nothing sticks out the back" is automatic anatomy**, no special case.
- **Long bones** (forearm / upperarm / thigh / calf): radius only (`qEnd = 1`) — shape never
  lengthens a limb.
- **COM main + ring / inner-pelvis children**: NOTHING, ever. A fatter ring shrinks the orifice.
  Protection is STRUCTURAL: `RegionForSlotChild(slot 11)` returns `kButt` ONLY for children 16/17
  (the two cheeks); every other COM child returns −1 and takes no shape.
- **The four FREE capsules** (breasts C11/C12, cheeks C16/C17): exempt from endpoint scaling —
  ReShape TRANSLATION owns their position (§below).

## 6. Region gains (`MeshRegionRatio`)

The raw ratio is `girth / neutral`, damped by a **per-region gain, then clamped**
(`bodyScaleClampLo/Hi` = 0.7/2.0). **The `bodyScale<Region>` factor IS the gain**, multiplied by a
global `meshRadiusGain` trim. Because a gain only scales *deviation from 1.0*, an NPC at the neutral
(Lydia, deviation 0) is mathematically untouched by any gain value — which is why one shared knob
set fits skinny and chubby simultaneously.

Final calibrated gains (four-body certified): `chest 0.12 · waist 1.04 · belly 0.55 · thighs 0.55 ·
butt 0.12 · arms 0.12 · breasts(cup) 0.20`; `meshRadiusGain 1.0`. `factor < 0.05` = region OFF for
radius (documented "disable" semantics). `bodyScaleLoBoost` (2) may strengthen the shrink side
(net<0) if slim bodies under-pull — currently only used by the slider fallback.

## The four free capsules (breasts / cheeks) — TRANSLATION + cup radius (`MeshShiftDeltas`)

Position is a DIRECT geometric shift — **u per u of measured surface movement** (never slider-unit
inversion, which once turned Sofia's real 1.77u of butt growth into an 8.2u shift):
- **breasts**: forward `+= (breastGirth − neutral)·meshShiftBreastFwdK`; vertical `+= (breastZ −
  neutralZ)·meshShiftBreastZK` — big breasts go DOWN because they MEASURE down (sag), not by a
  guessed constant.
- **cheeks**: backward `−= (buttGirth − neutral)·meshShiftButtK` (K=0.4). The cheek offset is
  bounded ±12u.
- **radius**: cheeks ride the butt ratio; **breasts ride the CUP ratio** (protrusion = position;
  cup vertical extent = SIZE — two breasts protruding equally can differ 2× in cup). Breast base
  `capSpine2C11R/C12R` = 3.60.

---

## 7. Eye-verification — the marker protocol (the METHOD that made this work)

**Numbers lie; the eye is ground truth.** Plausible-looking girths hid a wrong mesh, junk vertices,
a chest band sitting on the arm, and a saturated cup — every one was caught only by looking. Build
around it:

`meshMarkers 1` places ghost capsules (part-29 layer-56, collide with nothing, drawn by the
collision visualizer) at each region's girth-defining vertex, refreshed every latch. They **pin to
the nearest NPC** (≤400u, 4s stickiness) and the `MESHMARK` line logs their world coords.

**One-region-at-a-time band certification** (live, no rebuilds): empty every band but one
(`meshBand*Lo/Hi` = 0) → a single marker appears → judge/dial its band → certify → next region.
Correction meaning: at a 2u slice the marker reads as the slice CENTER ±1u; "up/down Xu" moves the
band; regions in a "valley" (belly on a slim body) need a NARROW slice or a wider neighbor wins.

**Certified bands (2u slices unless noted):** thigh 0.30–0.46 · butt 0.48–0.60 · waist 0.64–0.66 ·
belly 0.70–0.72 · chest 0.80–0.82 (= spine2's spot) · breasts 0.76–0.90 (wide OK, front-protrusion;
a marker floating a few u forward of skin is EXPECTED — breasts have physics bones, the measurement
is the pre-physics reference shape).

**⚠ Never collapse bands for a marker check while measured mode is live for others** — the emptied
regions fall to the slider fallback and its response can jolt (see §8). Prefer markers with all
bands live (they place regardless), or accept that other NPCs momentarily mis-dress.

---

## 8. The slider fallback (decoupled)

When no body mesh is readable (non-OBody NPC, unreadable buffer), ReShape falls back to the OBody
**slider-morph** layer (Report 23). That layer is DECOUPLED from the measured gains: it runs a
FIXED conservative response — radius factor hardcoded 0.12 (+ loBoost on the shrink side), nets
clamped ±1.5, slider translations bounded (breasts ±1.8, butt ±0.6). **The measured gains (0.5–1.0)
must never touch slider-sized nets (±2)** — they doubled a thigh once. Two separate response
regimes sharing only the region→capsule mapping.

---

## 9. Knob reference (PPB_tuning.txt)

| knob | final | meaning |
|---|---|---|
| `meshShape` | 1 | master: measured mode on |
| `meshShapeDump` | edge | bump to re-latch + re-sample everyone (dialing pulse) |
| `meshMarkers` | 0 | ghost markers on the girth verts |
| `meshBand<Region>Lo/Hi` | see §7 | z-fraction band per region (chest/breast/belly/waist/butt/thigh/arm) |
| `meshArmFrac` | 0.40 | arm/torso x-split as a fraction of bbox half-width |
| `meshRadiusGain` | 1.0 | global trim on every region gain |
| `meshCupFrac` | 0.8 | mound threshold: cup counts verts ≥ frac × near-max |
| `meshShiftButtK` | 0.4 | cheek backward shift, u per u of protrusion delta |
| `meshShiftBreastFwdK` | 1.0 | breast forward, u per u |
| `meshShiftBreastZK` | 1.0 | breast vertical (sag), u per u of height delta |
| `meshNeutral<Region>` | §4 | Softbody neutral girths (captured) |
| `meshNeutralBreastZ / Cup` | 84.81 / 9.24 | Softbody sag-height / cup neutrals |
| `bodyScale<Region>` | §6 | the per-region GAIN (measured) / factor (slider) |
| `bodyScaleClampLo/Hi` | 0.7 / 2.0 | ratio clamp |

**Dialing pulse:** during measured-mode calibration, `meshShapeDump` alone re-measures but a knob
edit re-dresses; changing any `meshBand*`/gain knob is self-arming. (The old radius-only era needed
a `capComC20R` nudge; the freshLatch fix + measured knobs made that obsolete.)

---

## 10. What ReShape never touches
- **Fur / shaders** — GPU rendering effects, not in the vertex buffer. Cannot pollute the measure.
- **Rigid clothing / armor** — rejected by band density (active torso bands need ≥150 verts; a
  skirt has ~60). A DRESSED NPC's readable mesh is her outfit's body-shaped mesh — usually correct
  (that IS the visible silhouette), but the NEUTRAL must be captured naked.
  **★ KNOWN-BODY BYPASS (2026-07-19h, the Auri false-positive — DLL 15ed5bb9):** the density guard
  had a false-positive class — a petite/preset REAL body can legitimately thin one 2u torso band
  under 150 (naked A-posed Auri's bosmer waist read 117 → whole-actor slider fallback → base
  capsules + the clamped 0.6-net × 7.1u = 4.26u butt shove = her "spines +10%, butt out 4u,
  breasts never moved"). Fix: a mesh whose vertex count matches a body FINGERPRINT (Softbody
  15460 / 3BA 18436 / CBBE 13554) bypasses the whole-actor sparse rejection — outfits never carry
  a body VC, and a genuinely empty band on a known body still falls back PER-REGION. The sparse
  log line now prints `vc=` for diagnosis, and the shift readout prints the CLAMPED effective
  values tagged `fallback-shift` (the old line printed the raw product — "butt back=17.96u" when
  4.26u was actually applied — and cost a false "ReShape is broken" scare).
- **Hands** — structurally excluded; skeleton scale only.
- **Capsule heights / along-bone length** — shape scales lateral + radius only.
- **The COM orifice ring** — structural (§5).

## 10b. breastZ is SPINE2-RELATIVE (2026-07-20; v2 = the bind CONSTANT, DLL b7710e81)
The measured-sag reference changed: `breastZ` (MESHGIRTH key now `breastZrel=`) = mound front-mass
Z **minus `kSpine2BindZ` = 91.2488u** — the spine2 bind Z of our shared skeleton, derived offline
(`derive_neutrals.py`; every PPB female skeleton has an identical chain, so it is ONE constant).
Typically a few u POSITIVE (mound ~5u above the mid-back joint origin). The old absolute
above-mesh-bottom measure inflated ~14u on hoofed bodies (Yvanni floated +13.7u — girth bands
self-normalize as bbox fractions, the absolute height did not). ⚠ v1 read the spine2 bind pos from
runtime NiSkinData skinToBone — those transforms are UNPOPULATED in-engine (nan/±1e37 garbage;
Ledger 10c); the constant is v2. Validity: 0 = invalid/uncaptured → the vertical shift
self-disables (fabs>0.01 guards). **Neutral capture state**: Softbody knob `meshNeutralBreastZ` 0 —
capture from Lydia's `breastZrel=` next session (expect ≈ +4.7); 3BA/CBBE table rows 0 (anchors
TBD); **Yvanni's row carries +5.32 from her file derivation and is LIVE**. Also 2026-07-20: the
Softbody girth neutrals were RE-CAPTURED from live Lydia (chest 14.06 / breasts 7.92 / waist 9.41 /
butt 12.71 / cup 9.01 — the 07-19 capture had drifted ~5%), and body-type fingerprints gained
**vc=12888 → kYvanniNeutral** (her custom Draenei CBBE build; her own-file neutral makes her ratios
≈1.0, killing the +40% spine0 / +20% spine1 / +3u breast-fwd her Softbody-referenced measures
produced). The MESHGIRTH success line now prints `vc=` so a fingerprint match is verifiable.

## 11. Open threads (not ReShape blockers)
- Furniture/green-ragdoll (PLANCK loosens ragdolls on furniture) — separate research.
- Teen skeleton bake (teen races run own un-baked NIFs).
- `poseConformEveryN` budget nag at 18-NPC crowds (cosmetic).

---

# ★★★ THE UV LANDMARK SYSTEM (2026-07-22) — THE reference solution. READ THIS FIRST.

**Every geometric attempt to LOCATE anatomy failed. Addressing it by UV works.** This section is the
transferable skill: it applies to ANY "find this body part on an arbitrary mesh" problem, not just PPB.

## The idea
CBBE / 3BA / Softbody (and most derivatives) **share ONE UV layout** — that is exactly why body
textures are interchangeable between them. Therefore **a UV coordinate is the SAME anatomical point
on every one of those bodies**, regardless of vertex count, topology, bone names, or body shape.

    look up the vertex nearest <u,v>  ->  read its position  ->  subtract the owning joint
    = a straight line from the anatomy to its Havok joint. NOTHING is inferred.

No neutral. No per-body-type fingerprint table. No protrusion heuristic. No base-mesh contamination.
Every one of those was an attempt to *infer* location from geometry; a UV is an **address**.

## How to get the UVs (the user's method, 10 minutes)
Open the actual game texture — `textures/actors/character/female/femalebody_1.dds` — in any DDS
viewer. Read the landmark off the visible skin as a fraction of the image:
`U = x/width`, `V = 1 - y/height`  (⚠ the V FLIP: image Y counts from the top, UV from the bottom).

## The validated landmark table (PPB ships these in CapFix.cpp `kUvLandmarks`)
| landmark | U | V | joint |
|---|---|---|---|
| R_nipple | 0.424242 | 0.698242 | spine2 |
| chest_center | 0.500000 | 0.654296 | spine2 |
| navel | 0.500000 | 0.839843 | spine2 |
| waist_center | 0.500000 | 0.917968 | spine2 |
| butt_cheek | 0.065917 | 0.649902 | COM |

## Why we believe it (validation, CBBE vs 3BA, offline pynifly)
- the three **centreline** landmarks resolve to **X = 0.00 exactly** on both bodies. Not a coincidence.
- heights order correctly: chest 101 > nipple 96 > navel 80.8 > waist/butt 71.8
- **butt resolves to −Y while nipple/chest resolve to +Y** — this is what settled the axis question
- uvErr 0.0005–0.003 (essentially exact hits)
- the two bodies **agree where they should** (navel/waist/chest ≈ identical — structural) and
  **differ where they should**: 3BA's nipple +0.6 wider / +1.3 further forward, butt 1u further back.
  THAT is real body-shape difference, cleanly isolated — which nothing before this managed.

## ★ AXIS TRUTH (settled empirically; my derived answer was WRONG)
**+X = her RIGHT · +Y = FORWARD · +Z = UP.** The earlier sector analysis concluded `−Y = front` from
"Sofia's chest reads 15.6 front vs 8.4 rear" — **inverted**, and it poisoned every measure built on it.
Proof: the butt landmark lands at −Y and the nipple at +Y. Also caught in-game when a probe moved
BACKWARD on a "forward" command. NEVER derive an axis from a statistic when a landmark can settle it.

## ★ CAPSULE-FRAME Z OFFSET = 6.22u (measured, not derived)
Capsule endpoints live in the **ragdoll BODY** frame, whose origin sits at the body's **PARENT joint**,
NOT on the bone node (Ledger: the same trap that broke the measured-scale sampler). Empirically:
user-dialled nipple Z **11.07** vs mesh-measured **4.85** ⇒ **+6.22u**, X and Y agreeing within 0.5u.
So `kSpine2FrameZ = 91.3157 − 6.22 = 85.0957`. ⚠ **COM's own offset is NOT yet eye-verified.**

## Fail-loud rule
A landmark with **uvErr > 0.02** is REJECTED and logged, never measured. A body that does not share the
CBBE layout must announce itself rather than silently return the wrong anatomy — the opposite of every
failure in this saga.

---

# UV ReShape — the generation that shipped (2026-07-23)

**Everything above this line describes superseded generations.** The girth sampler, the
body-type fingerprint, the OBody-slider fallback and the `bodyScale`/`meshShape` switch pair
are **deleted from the source**, not disabled. Read this section as the current system.

## The rule

One measurement source, one response path, **no fallback**. Each channel is a straight line
between one anatomical point (found by UV) and the XP32 node that owns it, compared against
Lydia's neutral. If a landmark is missing the channel does nothing — it never falls back to a
different measurement system.

| channel | node | UV landmark | drives |
|---|---|---|---|
| chest | Spine2 | `chest_center` | spine2 ring capsules — radial position + radius |
| belly | Spine1 | `belly` | spine1 ring — radial position + radius |
| waist | Spine0 | `waist` | spine0 ring — radial position + radius |
| butt | COM | `butt_cheek` | C16/C17 — 3D translation + radius |
| breast | Spine2 | nipple / brUp / brDn / chest | C11/C12 — **fully computed** |

**Rings scale radially only.** `a[2] *= es` and nothing else touches Z — height belongs to
ReScale, and that separation is structural, not a convention.
**Long bones (arms, legs) take the ratio on RADIUS ONLY**; `qEnd` stays 1 so endpoints never move.
**COM never moves** except the two cheeks, which move by their own rule.

## The ring ratio

```
ratio = 1 + (|landmark_xy| / |neutral_landmark_xy| - 1) * gain     clamped [0.70, 1.60]
```
`|·|_xy` = radial distance from the owning node in the XY plane — "how far the flesh sits from
the spine axis at that band". Gain defaults to **1.0**: the capsule follows the flesh exactly.
The old per-region gains (chest 0.12, waist/belly 0.47) were fudge factors compensating for an
unreliable measure; a reliable measure does not need them.

## The breast model

The one channel with no ring geometry and no constant, so it is **computed outright** rather
than nudged from a knob — both endpoints and the radius come straight from her measurements:

```
A.Y = 3.3588 + 0.2065 * breastDistance      B.Y = 8.1069 + 0.5688 * breastDistance
A.Z = 9.9623 + 1.3371 * sag                 B.Z = 10.0039 - 1.3596 * sag
R   = -0.6418 + 0.3645 * cup
A.X = 2.353                                 B.X = 4.904 + 0.25 * (B.Y - 8.788)
```

Drivers (all verified against the code, not assumed):
- `breastDistance` = `|spine2->nipple| - |spine2->chest_center|`, **3D norms**. The forward-only
  `breastSize` in the UVMEAS log is a *different, wrong* number — it was quoted by mistake twice.
- `cup` = `|breast_up - breast_dn|`
- `sag` = `nipple.Z - midpoint(up,dn).Z`. **Higher value = capsule rides LOWER.**

**The Z channels have opposite slopes and that is correct.** The root rises with sag while the
front drops: the capsule **pivots**. Tilt across the set — Sofia −1.90 (saggy, points down),
M'rissi +0.50, Lydia −0.44, Carmella +1.01 (perkiest, points up). This only became measurable
once the whole capsule was modelled instead of the front point alone.

### Holdout validation
Fitted from **two** anchors (M'rissi, Sofia). **Lydia was not in the fit.** The model put her
front point **+1.10** forward of her baked capsule; she had been independently hand-corrected to
**+0.98**. A **0.12u miss on an unseen body.** The previous front-point-only model predicted
+1.68 and needed a 0.7u correction. User verdict on the rebuilt model, unprompted: *"like if i
had adjusted myself, best result so far."*

## Why the old system had to be deleted, not switched off

`bodyScale 1 / meshShape 0` read like "shape frozen". It was not. `MeshShiftDeltas()` required
**both** knobs, so with `meshShape 0` it bailed and **the slider fallback became the only active
path** — silently adding up to **1.6u** per NPC. Two separate log lines then *misreported* it:

1. `fallback-shift ... (inactive — measured shifts win)` — the suffix keyed on `meshSampled`
   alone, ignoring the two gates that actually decide. It printed "inactive" while that path was
   the only thing running.
2. `region=breasts ratio=1.163` — **not** the value applied to the radius. Measured off the
   `APPLIED` line, the real ratios were **1.046** (Sofia) and **0.852** (M'rissi — *below 1*,
   i.e. shrinking). Trusting the logged number made a converted capsule **0.90 too fat** and got
   it rejected on sight, twice.

**Rule that came out of this:** when a value drives geometry, read it from the `APPLIED` line,
never from a summary line written to describe a different stage. `logVerbose 1` enables
`CapFix <id> APPLIED spine2.C11 A=[..] B=[..] r=..` — the final capsule after every offset and
multiply. It only fires on a fresh latch.

## Switches (all live, 1 Hz)

| knob | default | meaning |
|---|---|---|
| `lmReShape` | 1 | **the** master switch — gates sampler, latch and every channel |
| `lmGainChest/Belly/Waist/Butt` | 1.0 | per-channel trim; 0 = channel off |
| `lmClampLo/Hi` | 0.70 / 1.60 | ring ratio sanity bounds |
| `lmBr*` (13) | fitted | the breast model, re-fittable without a rebuild |
| `lmNeut*` (21) | Lydia | the neutral shape, re-capturable without a rebuild |
| `lmButtFit`, `lmButtOutU`, `lmButtNeut*` | 1, 0.6, Lydia | butt landmark fit |

`bodyScale` and `meshShape` are **retired** — commented out of the tuning file with a note.

## Not yet done
- **Legs and arms** have no UV landmark. `LmRegionGain` returns 0 for them, so they are inert.
- **Head** still uses the old bounding/vertex extent measure (`headSrc`/`headRaw`/`headRef`) —
  kept deliberately because it is the one region with a working non-UV source. See
  `03_Scale_ReScale_And_Calibration.md` for the head-UV feasibility notes.

## Vertical rework: tilt + height (2026-07-23, the Imperial finding)

**Sag has two kinds, and the scalar only measures one.** `sag = nipple.Z − moundMid.Z` sees
INTERNAL droop-shape but is blind to the whole mound sliding down the chest: the Imperial's
preset dropped her nipple 1.11u and mound 0.87u while the scalar moved 0.03 — capsule held
height, eye said "1u too high". Faralda was the opposite (internal droop, mound nearly in
place), which is why the sag-only model looked perfect on her and dead on the Imperial.

```
B.Z = 9.8965 − 0.7820×sag + 0.994×(moundMidZ − 5.330)      sag clamped [−0.60, +1.00]
A.Z = 9.9623 + 1.3371×sag + 0.994×(moundMidZ − 5.330)
R   = −0.6418 + 0.3645×min(cup, 10.40)
```

Solved exactly through the three direct eye verdicts (neutral Lydia / Faralda "perfect" /
Imperial "1u too high"). Height gain ≈ 1:1 — the capsule follows the flesh. The M'rissi and
Sofia anchors (fallback-arithmetic era) deviate +0.70/−0.95 under this model — **re-verify
both by eye** before trusting or re-fitting anything against them.

**Saturation**: drivers are clamped to the calibrated range (cup ≤ 10.40, sag ∈ [−0.6, +1.0]),
so an extreme preset gets the boundary response — a correction made at an extreme can never
again reshape the middle of the range (the mistake that briefly inverted this channel).

## Also landed 2026-07-23
- **DECOY detection** — the Faralda 17:57 case: an outfit's embedded reference body, frozen at
  base shape, beat her real skin via biggest-mesh-wins and passed every gate. Now: base-exact
  measurement + real morphs present → re-run the finder excluding that mesh; if a second body
  candidate reads differently, it is the true skin (`DECOY` / `DECOY-SUSPECT` log lines).
- **UV-knob auto re-latch** — editing any landmark UV re-latches everyone; no more manual
  `bodyScaleDump` bumps (stale landmarks look exactly like fresh ones — that trap is closed).
- **ARMOR-CHECK verdicts** (scenario 1/2/3 per sample) — first day in the wild caught a real
  scenario 3 ('Farm 1 Skirt' aliasing the centreline) and dropped it.
- **Head**: nose/chin UVs are in (facegen never moves UVs; HPH shares the layout). The face
  finder is diagnostic-first — first pulse logs readable AND unreadable head candidates; the
  real facegen head never resolved in session one (only clothing/hair aliased it), so the next
  session's HEADUV dump tells us where facegen vertices actually live.

## Channel status — 2026-07-23 session close

Every body channel is calibrated and eye-verified. **The neutralShape (Lydia zero-slider) is
the base for everything, and every constant has provenance** (`tools/ppb-scratch/
neutral_shape_lydia.txt` + `capsule_fit_calibration.txt` — read those before touching any
number).

| channel | driver | gain | verified on |
|---|---|---|---|
| chest/belly/waist | landmark radial ÷ neutral | 1.0 | zero-slide trio (3-decimal agreement) |
| butt | landmark 3D fit + radial ratio | posGain 0.482 | Lydia + probe sessions |
| breast | computed model (bd/cup/sag/moundZ) | — | Lydia, Faralda, Imperial, M'rissi, Sofia |
| legs/arms | chord ÷ neutral chord → radius only | 1.0 | Redguard zero + chubby ("look great") |
| head | nose/chin UV | — | BLOCKED on skinned-path fix (compiled, undeployed) |

Limb verdict detail: zero-slider chords 11.378/6.436 (ratio 1.000, baked capsules fit);
chubby 12.511/6.945 (ratio 1.099/1.079, capsules followed). **Gain 1.0 both — the chord maps
1:1 onto the radius.** Calf/forearm do not take the ratio; eye said nothing was left behind
on the chubby preset — wire them to the parent limb's ratio only if a future preset shows it.

Known cosmetic debt: the `region=... (factor=NN)` log suffix prints the RETIRED factor knob;
the applied ratio is the UV one. Clean up at the next code pass.

## HEAD CHANNEL — ReShape COMPLETE (2026-07-24)

The last channel. Driver: **face height** (nose→chin, 3D) vs Lydia's neutral **5.161**,
applied to the head capsule scale on top of es; clamps shared with the rings; the old
extent measure stays as fallback for beast races / failed facegen.

**The measurement pipeline** (runtime facegen positions are GPU-only in VR — measured fact):
1. Baked facegen NIF read from disk via `BSResourceNiBinaryStream` (BSA + loose + MO2 VFS).
2. Header walk → each `BSDynamicTriShape` block's tail = `[u32 dynSize][nv float4 positions]`
   with `dynSize == nv*16` and the block ending exactly after (self-validating arithmetic).
3. Positions COUNT-MATCHED to the live candidate (partition vertexCount), UVs from the
   runtime partition buffer (hand-decoded: UV half2 @0 | 4 half weights | 4 u8 bones = 16B,
   weights summing to 1.000 = the proof), paired by index — index identity is load-bearing
   for the renderer itself.
4. Anatomy gates: both landmarks < 0.02 uvErr, nose on centreline, above chin, face 4–15u.
   Day one they accepted the real head AND rejected the hair baked into the same file.

**Neutral (Lydia)**: nose (−0.02, 10.91, 1.12) · chin (0.00, 9.15, −3.73) · noseDepth 10.972 ·
noseChin 5.161. Head body frame sits **~1.5u below the node** (eye-measured via probe rod) —
irrelevant for radius, required for future placement work.

**The Faralda proof — why mesh ratio is the only honest driver**: her sculpt measures 0.914
of neutral while her High Elf race scale is 1.08. The sculpt visually cancels the race scale
(rendered face ≈ normal), so es alone oversized her head capsules ~9% — the user's original
complaint, quantified. `es × meshRatio = 1.08 × 0.914 ≈ 0.99` = her true rendered face.
The race record alone prescribes exactly the wrong correction.

**Five silent-failure bugs burned four pulses** getting here (loader exits, guard exits,
parser exits, `NiBinaryStream::read` returning BOOL not bytes — read the header, never guess
an API twice, and every exit speaks). All catalogued in 01_Pitfall_Ledger.

Knobs: `lmNeutNoseChin 5.161` · `lmGainHead 1.0` (+ the tuning-file head block).
Status: **every ReShape channel is now UV-landmark-driven and closed.**


---

## BODY-MOD COVERAGE: which bodies do the UV landmarks actually address? (2026-07-30)

ReShape treats a UV coordinate as an **address for anatomy** â€” find the vertex nearest âŸ¨u,vâŸ©,
read its position, subtract the owning joint. That only works on bodies sharing the layout the
landmarks were authored against. This section records what has been **measured**, as opposed to
assumed, because the assumption was wrong once already.

### Measured: BHUNP shares the CBBE layout

| | CBBE 3BA (Bodyslide Output) | BHUNP 3BBB Advanced Ver 3 / Ver 4 |
|---|---|---|
| shape probed | `Softbody` (and `FakeOverlay`, identical result) | `BaseShape` |
| verts | 15,460 / 16,247 | 19,039 / 19,035 |
| UV bbox | u 0.0104â€“0.9897 Â· v **0.0256â€“0.9902** | u 0.0116â€“0.9888 Â· v **0.0255â€“0.9902** |
| landmark `uvErr` | 0.0005â€“0.0030 | 0.0007â€“0.0022 |

The v-extent matching to four decimals is effectively a layout fingerprint. Probing a 20Ã—20 UV
grid and comparing the 3D point each UV addresses on the two bodies (230 points had a vertex
within `uvErr` 0.02 on BOTH; the other 170 fell in empty UV space):

```
mean 1.21 u (~1.7 cm) Â· median 0.99 u Â· p90 2.16 u Â· max 3.92 u Â· within 5 u: 100 %
```

All five landmarks (`R_nipple` 0.424242/0.698242, `chest_center` 0.5/0.654296, `navel`
0.5/0.839843, `waist_center` 0.5/0.917968, `butt_cheek` 0.065917/0.649902) land on the same
anatomy on both. The residual is real body-shape difference between the mods â€” the largest
(3.92 u, the nipple) is exactly where two body mods legitimately differ most.

**So ReShape needs no change to serve BHUNP users**: no extra landmark set, no per-family
branching, no fingerprinting. Script: `tools/ppb-scratch/uv_layout_compare.py`.

### âš  This CORRECTS the knowledgebase

`KNOWLEDGEBASE.md` asserted *"UNP-family does NOT share the layout"*. That was never measured and
is wrong for BHUNP. Corrected in place 2026-07-30 with the numbers above.

**Scope â€” do not over-generalise the correction.** Only the BHUNP 3BBB Advanced **body** was
tested. Classic UNP / UUNP / 7B and other derivatives remain **unknown**; BHUNP was rebuilt and
may not represent its ancestors. Hands, feet and head are separate meshes with their own layouts
(and the head channel reads the facegen NIF, a different mechanism entirely).

### The method trap this exposed â€” probe the OUTPUT body, never the ShapeData reference

The first run of this comparison reported "DIFFERENT ANATOMY" with 25â€“51 u drift and was
**wrong**. It probed `CBBE 3BA (3BBB)/CalienteTools/BodySlide/ShapeData/CBBE 3BA Reference/
CBBE 3BA Ref.nif` â€” the BodySlide *reference* â€” instead of the installed body. Symptoms worth
recognising:

* every landmark collapsed onto roughly ONE point (z â‰ˆ 118, head height) instead of a descending
  chest â†’ navel â†’ waist â†’ hip ladder;
* `uvErr` ran 0.025â€“0.094, i.e. **above ReShape's own 0.02 rejection threshold**.

That second symptom is the tell, and it is why the gate exists: ReShape rejects `uvErr > 0.02`
precisely so a wrong mesh announces itself instead of returning a plausible-looking wrong
answer. The correct target is `Bodyslide Output/meshes/actors/character/character assets/
femalebody_1.nif` (or the equivalent installed body), which gives `uvErr` 0.0012.

> **Rule:** when a UV probe says "wrong layout", check the `uvErr` FIRST. Above the gate means
> *you are on the wrong mesh*; below the gate with bad positions would mean a genuine layout
> difference. Also confirm the V convention â€” a v-flip here raised mean `uvErr` 0.0012 â†’ 0.0147,
> which is under the gate and would have produced quietly wrong landmarks rather than a failure.

