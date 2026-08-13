# 21 — Verified capsule inventory + the male-skeleton preconditions (2026-08-13)

**Status: DATA + PRECONDITIONS. No NIF was modified. `inv_FINAL.json` (516 rows) is the authoritative
per-child record.** Produced by three independent inventories (pynifly / raw bytes / audited existing
scripts), reconciled by a **fourth** from-scratch byte parser, then adversarially verified by a
**fifth**. Cross-check: 474 capsules × {point1, point2, bhkRadius, radius1, radius2, block id} →
**0 mismatches**. Self-validation: 750/750 node walks consumed exactly their declared `blockSize`;
every `bhkCapsuleShape` block is exactly 48 bytes; all four files reconcile through the `NiFooter`.

---

## 1. Slot child counts — the one authoritative table

| slot / host node | human | argonian | khajiit | draenei | male source | knob bound |
|---|---|---|---|---|---|---|
| 11 `NPC COM [COM ]` | 32 | 32 | 32 | 32 | 1 capsule | 32 |
| 3 `NPC Head [Head]` | **23** | 17 | 17 | **23** | 1 capsule | 23 |
| 4 `NPC Spine [Spn0]` | **8** | **10** | 8 | 8 | 1 capsule | 10 |
| 5 `NPC Spine1 [Spn1]` | 11 | 11 | 11 | 11 | 1 capsule | 11 |
| 6 `NPC Spine2 [Spn2]` | 19 | 19 | 19 | 19 | 1 capsule | 19 |
| 7 `NPC Neck [Neck]` | **plain capsule, 0 list children, 1 collider** | ← | ← | ← | 1 capsule | slot path only |
| 2 upperarm / 1 forearm / 0 hand | 4 / 2 / 5 | ← | ← | ← | 1 each | 4 / 2 / 5 |
| 8 thigh / 9 calf / 10 foot | 7 / 5 / 4 | ← | ← | ← | 1 each | 7 / 5 / 4 |

`kMaxListChildren = 34` (`CapFix.cpp:48`) is **ours** and raisable. The knob bound overhangs the real
child count on slots 4/5/6/11, but **no shipped NIF reaches the overhang today**.

⚠ **`dump_child_counts.py` has a bug**: it collapses "not a list" to the integer 0, so the neck reads
as having no collider. It has one — a bare `bhkCapsuleShape`, `p1=[0,1.1,1.2] p2=[0,0.3,9.8] r=3.25`
game units, byte-identical on all four NIFs, driven by `CapFixSlot` (`CapFix.cpp:4192/4237`), never by
`ApplyListSlot`. `CapFixChildSlot` has no `case 7`.

---

## 2. ★ THE SPINE0 0.5u CAPSULE — answered

**It is a PAIR: `capSpine0C8` and `capSpine0C9` (child indices 8 and 9).**

**Mechanism: UNREACHABLE.** `skeleton_female.nif` slot 4 has **8** children (indices 0–7).
`ApplyListSlot` loops the actor's OWN child count, so indices 8 and 9 are never reached on a human,
khajiit or draenei female — the knobs are `Enable 1.0` with seed values and write nothing at all.
They are reached **only on the Argonian**, whose spine0 has 10 children.

This is category (d) — not "knob absent", not "disabled", not "seed over seed". Mechanically proven,
not inferred from shape.

---

## 3. ⛔ THE SIX ROWS THAT LOOK LIKE BURIED ANATOMY — DO NOT ACT ON THEM

Six children on **`skeletonbeast_female.nif` (Argonian) only** have a live knob writing a 0.5u seed
over NIF geometry that is not a seed:

| slot | child | knob | NIF baked (game units) | knob writes |
|---|---|---|---|---|
| 4 spine0 | 8, 9 | `capSpine0C8/C9` | p1 `[1.271, 7.5, 4.035]` p2 `[6.702, 6.034, 9.616]` r `2.0` | `(0,0,0)→(0,0,2)` r `0.500` |
| 5 spine1 | 7, 8 | `capSpine1C7/C8` | p1 `[2.7, 7.2, 5.515]` p2 `[3.892, 8.0, 11.476]` r `3.5` | `(0,0,0)→(0,0,2)` r `0.495` |
| 6 spine2 | 15, 16 | `capSpine2C15/C16` | p1 `[4.5, 2.5, 5.0]` p2 `[6.153, -0.767, 12.841]` r `3.0` | `(0,0,0)→(0,0,2)` r `0.500` |

**Each of the six is byte-identical to child 1 of its own list** (spine0 c1 = block 549, spine1 c1 =
565, spine2 c1 = 582). That is exactly what the `extend_*.py` scripts produce — the ledger records
that they *"clone child[1] verbatim because a live knob overwrites it within 1 s"*. So the most
likely reading is **placeholder clones behaving exactly as designed**, not buried dorsal ridge.

**Duplication is evidence, not proof of intent. Nothing here may be disabled on the strength of it.**
An earlier session disabled four of these on the ridge theory and had to revert (ledger, 2026-08-13).
Settling it needs the user's eye on an Argonian in VR, not more static analysis.

**Adjacent, 12 rows:** on human/khajiit/draenei, `capSpine1C7/C8` and `capSpine2C15/C16` write a seed
over a baked seed. They bury nothing but are **not inert** — each produces a real ~0.5u collider at
the Spine1/Spine2 frame origin.

---

## 4. ★★ npcCap IS A THIRD GEOMETRY SOURCE — and it makes head indices load-bearing

The adversarial verifier caught what all three inventories missed. `PPB_skeletons.txt` carries **18
active `npcCap` lines**, and `haveNpc` is computed **independently of** `haveKnob`
(`CapFix.cpp:2517-2519`), driving a child *"even when no knob block covers it (Enable 0 / beyond the
knob count)"*.

**`skeleton_female.nif` head children 15–22 are canonical seeds in the NIF and are the ONLY carriers
of Auri's antler colliders** (`PPB_skeletons.txt:83-90`). They are reachable (head n=23 → 0..22) and
not beast-gated.

> **⚠ Strip those 8 seeds, or shrink the head list, and Auri loses her antlers — and every remaining
> npcCap head index shifts.** Any head extension must APPEND at 23/24/25 and never renumber.

Same class, lower severity: draenei slot 9 children 0–4, slot 10 children 0–2, slot 6 children 11–12
are Yvanni's hooves/feet/breasts (`PPB_skeletons.txt:42-51`).

Also latent: `skeleton_female_draenei.nif` head children 15–22 carry **real baked horn geometry**
where the human NIF has seeds (e.g. C21 `p1=[15.1,-0.6,1.3] p2=[10.59,-2.61,1.28] r=0.75`).
`capHeadC15..C22` are Enable 0 today, so the horns survive; enabling any at its current value would
bury them.

---

## 5. MALE SKELETONS — what survives scrutiny, and the one hard precondition

### Survives (refutation attempts that FAILED — do not re-litigate)

* **Node lookups do not break.** All **64** distinct node-name literals PPB looks up — 12 `kSlotNode`,
  6 `kSlotNodeL`, 30 finger names, `NPC Root [Root]`, `TailBone01..05`, all 10 genital-chain names —
  exist on `skeleton.nif` and `skeletonbeast.nif`. Male-only extras (`CamSpin`,
  `NPC Translate [Pos ]`) are referenced nowhere in PPB.
* **"Same locations" is geometrically valid.** Core bone lengths are **bit-identical** male vs female:
  upper arm 22.8340, forearm 16.0467, thigh 35.5953, calf 27.9497, COM→Spn0 6.4678, Spn0→Spn1 8.7487,
  Spn1→Spn2 9.8641, Spn2→Neck 22.0395, Neck→Head 7.3928. **`kBaseArc = 48.045f` measures 48.0450 on
  the male skeleton too.**
* **The surgery is doable and verifiable.** A pynifly load+save of `skeleton.nif` is byte-identical
  past the header (only the 20-byte export string changes). A proof-of-concept male NIF already
  reproduces the female block census, with **148 of 149 capsule blocks byte-identical** as multisets.
* **PPB's own skeleton is a clean transplant source** — 0 of 648 shared nodes differ from XP32 female
  by >0.001 game units; PPB added 101 nodes and moved nothing.

### ⛔ THE PRECONDITION — the male guard is a SHAPE test, not a sex test

`CapFix.cpp:3142-3159`: `const bool isBake = (shape->type == RE::hkpShapeType::kList);`

Its own rationale (`CapFix.cpp:399-406`): *"every STOCK skeleton — male, undead/draugr … keeps a
single `bhkCapsuleShape` COM … This gate stops the GEOMETRY writers … from deforming bodies we never
baked — the male/skeleton 're-scaled + capsules shrunk' bug."*

**The instant a list-shape COM lands on a male, `ActorCarriesBake()` returns true and both writers
re-arm:**

1. `CapFixApply` writes the **global female-dialled `cap*` knobs** into his live capsules.
2. `PivScaleCorrect` rewrites his 17 ragdoll pivots off the **female 48.045 base**
   (`pivReScale`/`pivReScaleApply` both ship ON).
3. `lmReShape 1` feeds those writes with ratios measured from the **CBBE/3BA female UV atlas** —
   `kUvLandmarks` is nipple / breast-up / breast-down / chest-centre / butt-cheek
   (`CapFix.cpp:910-925`, sourced from *"the real femalebody_1.dds"*). On a male mesh those UVs land
   on unrelated geometry. The identical failure was already eye-confirmed for beast heads and hard-
   gated there (`CapFix.cpp:2044-2049`); **there is no equivalent male gate on the body channel.**

> **A sex/skeleton gate must land AHEAD of `ActorCarriesBake` before any male NIF ships, or the
> 2026-07-15 regression returns wearing a new hat.**

### Other blockers (work, not danger)

* **Drive path is female-hardcoded** at ~11 sites in 5 files: `PpbApi.cpp:219/223/1307/1549`,
  `CapFix.cpp:2065/2428`, `NpcFingerTest.cpp:2106`, `DismemberGuard.cpp:789/862/875/887`. Several read
  `skeletonModels[kFemale]` **for a male actor**. Only `PPBHook.cpp:728` and `PivFix.cpp:704` are
  sex-aware.
* **Nothing can assign a male skeleton.** Every write targets `[kFemale]` (`main.cpp:574/649/780/854`)
  and the ini parser accepts only `race` / `femaleModelContains` / `npc` / `npcName`. A male PPB
  skeleton would sit on disk unreferenced.
* **The beast POC is not the female layout.** PPB's female beast **deletes** XP32's 5 tail rigid
  bodies (24 collision-bearing → 19, 0 tail collision). The POC male beast kept them (24 bearing, 23
  rigid bodies, 14 ragdoll constraints). Those 5 bodies + constraints must be removed to match.
* `PpbTouchAPI.h:42-43` publishes *"Males … are NOT covered"*. Opening male coverage is a contract
  change with a live rev-1 consumer (VRTouchEvents V3).
* The **player** is excluded regardless of sex — `PPBHook.cpp:1282` returns above the whole per-actor
  seam.

### Stale comment worth fixing
The bake-gate comment says the COM list has "21 children". The shipped list has **32**.
