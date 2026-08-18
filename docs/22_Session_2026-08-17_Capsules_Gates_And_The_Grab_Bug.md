# 22 — 2026-08-17: chin seal, dress gate, capsule additions, and one OPEN bug

Everything below was dialled or measured in VR on 2026-08-17 with the user watching. Numbers are
GAME UNITS in child-body-local space. Axis conventions were **flash-probe confirmed**, not assumed:

* **spine / COM: +Y = FORWARD**, −Y = back. (Probe: `capSpine2C15` shoved +15u on Y appeared in
  front of her chest.) +Z = up, +X = right.
* **head: +Y = FORWARD** (nose sits at Y 9.85, cranium at Y 3.57).

---

## 1. ★ npcCap WINS over the global knob — the fact that shaped every decision here

`CapFix.cpp:2517-2519`: `haveNpc` is computed **independently** of `haveKnob` and overwrites a/b/r.
Its own comment: *"a `npcCap` entry for (this actor, slot, child) WINS over the global knob — and
drives the child even when no knob block covers it (Enable 0 / beyond the knob count)."*

Consequences used repeatedly below:
* A child can serve **two different purposes at once** — npcCap geometry for one named NPC, knob
  geometry for everyone else. No conflict, no new children needed.
* Any child carrying an `npcCap` line is **load-bearing even when it reads as a buried seed** in the
  NIF. `inv_FINAL.json` does not model npcCap; do not trust a "buried-seed" verdict alone.

---

## 2. CHIN SEAL — head C15/C16 (shipped, dialled)

**Problem (user, in VR):** *"There is a massive hole under the chin, I can put my finger right through
and into the mouth."* Known geometry: head C11 (under-jaw) is deliberately excluded from the
interior-sensor priority race precisely because it is *reachable from OUTSIDE under the jaw*.

**Fix:** the chin capsules C2/C3 duplicated onto C15/C16, shifted back, then dialled by eye.

```
capHeadC15  Enable 1  A=( 1.85, 0.08, 0.41)  B=( 0.62, 6.38, -1.16)  r=0.880
capHeadC16  Enable 1  A=(-1.85, 0.08, 0.41)  B=(-0.62, 6.38, -1.16)  r=0.880
```

Dial history: copied C2/C3 with Y−3 → forward 1u → back points 1u closer → back points 1u closer
again (±3.35 → ±2.85 → ±1.85).

**Why C15/C16 specifically:** they were Auri's antler carriers. Because npcCap wins, putting the seal
on those indices gave the seal to every female **and** left Auri's antlers intact. Her antlers were
then relocated anyway (§4) so that she gets the seal too.

---

## 3. THE DRESS GATE — COM C32/C33 (new children, dialled, gate NOT yet built)

Two pelvic capsules that block the orifices while clothed and retract when not. C33 mirrors C32 on
**X only**; everything else identical.

### CLOTHED — blocking position ★ user-confirmed "this is the proper location"
```
capComC32  A=( 1.0, 0.5, 1.9)   B=( 1.0, -8.0, 4.5)   r=1.0
capComC33  A=(-1.0, 0.5, 1.9)   B=(-1.0, -8.0, 4.5)   r=1.0
```

### UNCLOTHED — retracted position ★ user-locked
```
capComC32  A=( 1.0, 1.5, 9.4)   B=( 1.0, -6.5, 10.5)  r=1.0
capComC33  A=(-1.0, 1.5, 9.4)   B=(-1.0, -6.5, 10.5)  r=1.0
```

Delta clothed→unclothed: **A +1.0Y +7.5Z, B +1.5Y +6.0Z**. The runtime gate switches between the two
on the exposure signal (§5). Shipped state is the UNCLOTHED position with `Enable 1`.

⚠ These are a deliberate dialled pair. See the 2026-08-13 ledger entry — a capsule that looks like a
leftover can be load-bearing.

---

## 4. CAPSULE ADDITIONS — the four coupled edits, done together

| slot | before | after | skeletons |
|---|---|---|---|
| COM (11) | 32 | **34** | all four |
| head (3) | 23 | **25** | human + draenei only (beast heads stay 17) |

COM 34 == `kMaxListChildren` exactly, so **no raise was needed**. The four coupled edits
(`Tuning.h` arrays → `Tuning.cpp` kArrays → `CapFixChildSlot` ChildPtr bound → `CapFixChildKnobs`)
were applied in one commit; miss any one and the new children silently no-op.

NIF work by `tools/ppb-scratch/extend_com2_head2.py` — per-file backup, work on a temp copy, verify by
RELOAD including a **collateral check that no other body changed shape or child count**. Seeds are the
canonical buried rod `(0,0,0)→(0,0,2u) r=0.5u`, never a verbatim clone.

Newly reachable indices were swept for stale values **before** raising the bounds (all four absent),
per the 2026-07-29 near-miss where `capSpine1C9/C10` already held junk 25u behind the body.

**Auri's antlers relocated 15/16 → 23/24**, geometry byte-identical (`PPB_skeletons.txt:83-84`).
⚠ `npcCap` is parsed once at `kDataLoaded` — a **restart** per change.

---

## 5. ★★ THE EXPOSURE GATE — measured, and it is sex-asymmetric

### Males: TNG, and the signal is a PAIR (see doc 20 §2 for the full measurement)

```
exposed  ==  skin->HasPartOf(52)  &&  !GetWornArmor(52)
```
* `skin->HasPartOf(52)` = **capability** ("TNG assigned this actor a schlong", skin `TNG_Skin_B07`).
  Reads 1 dressed AND naked — it is *not* an exposure test.
* `GetWornArmor(52)` = **state**. 1 when dressed, 0 when naked, because TNG tags *covering* garments
  with slot 52. This was the column kept only as a control expected never to fire.

**User's design rule, and it is the right one:** gate on the same authority that *creates* the thing.
If TNG fails there is no schlong, so our capsule failing identically is correct by construction.
Do not substitute another authority that can be right when TNG is wrong.

### Females: slot 52 is FLAT — TNG does not manage them

Measured on Carmella dressed and naked: `SLOT52(skin)=0 worn=0` in **both** states. TNG gates females
behind `TNG_Gentlewoman`, so an ordinary female never gets a TNG skin. `bodyArmor` (slot 32) *does*
flip 1→0 correctly.

### Females: use Advanced Nudity Detection, with slot 32 as the failsafe

```
exposed  ==  AND.GetShowingGenitals()  ||  !bodyArmor
```
The second term is **bulletproof and needs no database** — nothing worn in the body slot means
definitively undressed. AND only refines the *dressed* case (skimpy outfits). Residual failure mode is
therefore **blocked-when-it-should-be-open**, which is the safe direction: a false block is an
annoyance, a false open lets objects into an orifice while dressed.

---

## 6. ADVANCED NUDITY DETECTION — integration surface (as-read)

`D:/Games/My Skyrim/mods/Advanced Nudity Detection/` — SKSE DLL + Papyrus.

**Eleven per-actor query natives** (`AND_ModEventListener.psc:51-62`), both sexes:
`GetShowingGenitals` · `GetShowingAss` · `GetShowingChest` · `GetShowingBra` · `GetShowingUnderwear` ·
`GetTopless` · `GetBottomless` · `GetNude` · `GetModestyRank` · `GetTopModestyRank` ·
`GetBottomModestyRank`. Plus `RegisterPlugin(String)`, which implies an intended integration path.

`GetBottomModestyRank` returns a **graded Int**, not a bool — a better match for capsules that move
*up and down* than an on/off test.

**Six state FACTIONS** (readable from C++ with one `IsInFaction`, no handshake):
```
AND_ShowingAssFaction      0x0300082E     AND_NudeActorFaction   0x03000831
AND_ShowingChestFaction    0x0300082F     AND_ToplessFaction     0x03000832
AND_ShowingGenitalsFaction 0x03000830     AND_BottomlessFaction  0x03000833
```
(resolve at runtime with `LookupForm(0x00082E.., "Advanced Nudity Detection.esp")` — ESL-flagged, so
the `FE###` index moves with load order.)

⚠ **No C++ interface exists.** `dumpbin /EXPORTS` shows only `SKSEPlugin_Load/Query/Version`. So the
options are the factions (synchronous, C++-native, but their toggle reliability is UNVERIFIED) or a
Papyrus shim calling the natives. **Next step: add faction columns to `GENPROBE` and watch a dress
cycle flip them** — that settles reliability without writing integration code.

Also: no KID/SPID ini ships with AND, and it exposes `FemaleAnalyze()`/`MaleAnalyze()` natives, so it
likely *analyses* worn items at runtime rather than relying purely on pre-tagged keywords — better for
custom-armour coverage than a keyword list would be.

---

## 7. ⛔ OPEN BUG — fingers pass THROUGH the limb being grabbed

**Symptom (user, reproducible):** grab an NPC limb with one hand and the OTHER hand's PPB finger boxes
pass through **that limb's capsule only**. HIGGS's own hand box still collides. Other limbs are fine.

**Log-confirmed**, not merely reported:
```
TOUCHPROBE R[FINGER] upperarm[R].C1  d=-1.71u      <- finger 1.71u INSIDE her arm
API START 1301DE4F L|GRAB|Arm(R upper arm (shoulder half))|human   <- grab live on that node
```

**Ruled out — HIGGS is not the cause.** Its comparison hook
(`hooks.cpp:699 bhkCollisionFilter_CompareFilterInfo_Hook`) has **no logic of its own**; it only
dispatches to registered callbacks. And the only filter rewrites to `(playerGroup<<16)|5` are for
**impacted projectiles** (`hand.cpp:1344`, `:3243`), not grabbed actors.

**Established by the deployed GRABBUG census:** our boxes **are** consulted during the grab (44,613
pairs) and we return Ignore on at least some. So the decision is **inside PPB** — not the broadphase,
not upstream. That leaves exactly two branches that can return Ignore for a pair involving one of our
boxes: `HandBox::FilterDecision`'s belt (`:1735`, box × HIGGS-own-body, **same group**) and
`NpcFinger::FilterDecision`'s terminal `return 2`.

**Why the census did not finish the job:** it records only the LAST pair, and across 44k pairs the last
was benign — `ourBox 0x01E1C438 (layer 56 part 4 grp 01E1)` × `other 0x01E18538 (layer 56 part 5 grp
01E1)`, i.e. our box vs **HIGGS's own hand**, which the belt Ignores correctly by design.

**The hypothesis worth testing (NOT confirmed):** both sides carried group `01E1` — the player's
group. Our boxes take their group from HIGGS's hand body, so ours is always the player's group. If a
grabbed limb ends up in that group too, the belt's same-group test starts matching her limb and `op`
decides the rest. Doc 09 records the player collision group churning during grabs.

**NEXT STEP (needs a game-closed build):** narrow the census to record the last pair whose OTHER side
is on a **biped layer (8/32/33)**, plus separate Ignore/fell-through counters for those pairs. One grab
of an upper arm then names the branch and the line.

**Why it matters beyond clipping:** a finger passing through a limb can surface on the far side and
report contact with something behind it — a touch the player never made and physically could not make.

---

## 8. Also shipped this session

* **Six origin-parked spine capsules buried** at Y +5.0 (inside the torso): `capSpine1C7/C8/C10`,
  `capSpine2C15/C16/C18`. They sat at the bone frame origin, which at the spine is *at the back
  surface*, so they were touchable and reported as raw indices (`Chest(spine2.C15)`) with no part
  name. C10/C18 needed `Enable 1` first — they were seeds no knob was writing, so disabling would
  have done nothing. **`Enable 0` does not remove a capsule whose NIF seed is identical to what the
  knob writes; you must WRITE a buried value.**
* **`maleGeometry` gate** (doc 21 §5) — ships 0; a baked male is refused by the geometry writers until
  a male neutral and male UV landmarks exist.
* **`genProbe 2`** = ~10 Hz fast mode for equip-latency measurement.
* **`GenitalProbe` now reads the actor SKIN** (`skin->HasPartOf(52)`), keeps `GetWornArmor(52)` as a
  second column — which turned out to be the real discriminator.
* ⚠ **`geomNear`/`geomHidden` are dead candidates by construction**: a *skinned* mesh is parented
  under the body node and merely weighted to the bones — never a child of them. They read 0 in every
  state regardless of what is visible. Delete rather than leave as noise.
