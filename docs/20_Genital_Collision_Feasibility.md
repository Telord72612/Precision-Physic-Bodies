# 20 — Genital collision & the third wand: feasibility study (2026-08-12)

**Status: DESIGN + FEASIBILITY ONLY. Nothing built. No knob exists. Read this before writing a line.**

Question asked: track genital visibility per actor (the way `heelFix` tracks the heel offset), create
genital collision capsules only while exposed — for NPCs *and* the player — and give the player a
**third wand** that collides with objects and with NPC capsules the way the hand does.

Method: 8 parallel source studies + 3 adversarial verifiers, read-only, every claim cited to
`file:line`. **All three verifiers returned REFUTED.** Their narrowed claims are what this document is
built on; each refutation kills a shortcut that looks obviously correct from outside.

---

## 1. VERDICT

Buildable, but **not as one feature**. The NPC half and the player half are different modules with
different body types and different lifecycles.

| Piece | Verdict | Confidence |
|---|---|---|
| Exposure gate (per-actor, NPC + player) | Buildable, cheap — signal currently **unmeasured** | 90% once measured |
| NPC genital rig (dynamic capsules on the bone chain) | Buildable by cloning the garment-rig machinery | 84% |
| Player "third wand" | Buildable — on the **HandBox** family, never the rig machinery | 74% |
| PCVR V4 non-conflict | Closed by construction (doc 19) | 95% |

### ★ THE FORK — the "biggest problem", and it is a Havok property, not a coding difficulty

A player-side collider must be keyframed or dynamic, and **each choice forfeits something the feature
wants**:

| | **MOTION_KEYFRAMED** (HandBox family) | **MOTION_DYNAMIC** (garment-rig family) |
|---|---|---|
| Mass | infinite; position 100% commanded (`HandBox.cpp:433`) | 0.05 kg servo-driven (`NpcFingerTest.cpp:1210`, `Tuning.h:968`) |
| HIGGS grab-selectable? | **No** — `IsMotionTypeMoveable` is DYNAMIC-only (`_research/higgs/include/utils.h:68-75`) | **Yes** → massless body + stiff constraint → solver NaN → **engine freeze** (ledger `01:486-507`) |
| Pushes NPC flesh (dynamic, PLANCK-driven)? | **YES** — the headline payoff | yes, weakly |
| Deflected by the player's own hand? | **NO — zero impulse.** Both are infinite-mass keyframed (doc `09:399`: *"keyframed-vs-keyframed produces zero impulse and both mods early-out"*) | yes — but see the freeze row |
| Stopped by walls/objects? | **NO** — statics are infinite mass too; contacts generate, nothing moves | yes, and oscillates (`NpcFingerTest.cpp:3196`: *"a leaned-on table would flicker-oscillate"*) |

**Stated plainly:** a keyframed wand can *push her*, and can *report* contact with your hand and the
world, but can never be *stopped* by your hand or by a wall. Physical resistance against the player's
own hand is unreachable without making one side dynamic, which re-opens the freeze class.
**Decide which half of "collide" the feature needs before building.**

Verifier 2's surviving form: *"it can copy only HandBox's BODY FACTORY and snapshot/keyframe idiom,
explicitly NOT `PlayerSpaceWarp` … and NOT the garment rig's dynamic body factory or its
NPC-drive-seam lifecycle (unreachable for the player, `PPBHook.cpp:1282`). And a purely keyframed rig
yields zero mechanical deflection from the player's own keyframed hand — it can be a detection
surface, not a physical one."*

---

## 2. THE EXPOSURE GATE

### The signal: The New Gentleman's slot 52 — not what the existing probe reads

* **The installed framework is TNG 4.2.5, not classic SOS.** `TheNewGentleman.log`:
  `Processed [20934] armor pieces`, `[4710]: were recognized to be covering`, `Installed Load3D hook.`
* TNG decides revealing by **ini `[CoveringRecord]`/`[RevealingRecord]` entries plus biped slot 52**,
  and it is **user-swappable at runtime** (`TNG_PapyrusUtil.psc:9-10,53`). Not keyword-derived.
* Runtime read = two dword tests: `skin->HasPartOf(kModPelvisSecondary)` — slot 52, `1 << 22`,
  `BipedAnim` index 22. ⚠ **Do not call `Actor::GetWornArmor` per poll** — it deep-copies the worn
  inventory into a `std::map` with a `make_unique` per entry. Walk
  `GetInventoryChanges(false)->entryList` instead.
* TNG already excludes races, children and creatures, and gates females behind `TNG_Gentlewoman`.
  **PPB re-implements none of that** — a gentlewoman correctly gets a rig, an ordinary female never
  does. Strongest argument for slot 52 over every alternative.

### ⛔ The existing GenitalProbe cannot answer this, and has never run

Verifier 1 refuted "a validated signal exists". What is true:

1. **Wrong slot.** `GenitalProbe.cpp:143` reads `BipedObjectSlot::kBody` (slot 32). TNG's addon is in
   **slot 52**, which the probe never reads.
2. **Wrong revealing test.** `LooksRevealing()` (`:83-97`) substring-matches `"reveal"` over keywords,
   mis-reading `TNG_RevealingOnlyWomen`/`Men`.
3. **Never armed.** `PPB_tuning.txt:1819 genProbe 0`; live `PPB.log` has **zero** `GENPROBE` lines; no
   module report records a result.
4. **Never runs for the player.** Its only call site `PPBHook.cpp:1291` sits *below* `:1282` —
   `if (actor == PlayerCharacter::GetSingleton()) return;`
5. **No `ClearOnLoad`** (`main.cpp:1015-1034` omits it).

### The female-skeleton problem — worse than assumed, and solved for free

Byte scan of every skeleton in the tree: **all ten `Genitals*` names are on all eight PPB female
skeletons**, on the XP32 **male** skeleton, on `_1stperson`, `vampirelord`, `werewolfbeast`, and
seven-to-eight on **bear, wolf, draugr, giant, horse, deer, sabrecat, riekling, chaurus, falmer,
troll, skeever, goat**. A bone test fires on a bear. The slot-52 skin test is immune at zero cost.

### Gate shape (heelFix template, with one inversion)

`heelFix`'s latch is sticky **because the applied magnitude comes from a live externally-owned
signal**. Exposure is a **boolean with no magnitude**, so invert it: latch the **capability** (skin
carries slot 52) per FormID, monotone within one 3D build, cleared on `kPreLoadGame` **and**
`kNewGame`; **poll the state** at ~1 Hz with hysteresis (N agreeing polls before a create/destroy
edge). Gate on `ObjectHold::ActorRagdollAttached()` (`PivFix.cpp:1304`), never bare
`IsInRagdollState` — that is the still-unfixed heel-fix bug (`01:111-127`).

---

## 3. TWO LAYERS

### (a) API layer — no NEW Havok body, but "pure geometry" is not "no body"

Verifier 3 refuted the shortcut. Narrow truth: for an actor PPB **already drives**, reporting genital
contact needs no new rigid body and no contact listener — the 11 pelvic sensors already exist as
children C21..C31 of her COM body's `hkpListShape`, resolved by point/segment-to-capsule distance
(`PpbApi.cpp:829-1000`) with the deepest-sensor priority override (`:921-980`).

But it is **not bodyless**: sensor geometry and world pose are read out of the live `hkpRigidBody`
(`CapFix.cpp:2913-2955`) — **the NIF holds only degenerate 2u seeds** (doc `15:402-410`: *"an API must
read capsule positions from the live Havok body, never from the NIF"*). The toucher side reads PPB's
own four Havok hand boxes with **no node fallback** (`HandBox.cpp:1659`, `PpbApi.cpp:594-597`).

⚠ **Males are invisible to the entire API today**: `PpbApi.cpp:219`
`if (!base || !base->IsFemale()) return nullptr;` plus a required `\ppb\` skeleton path. Opening that
is a change to the published coverage contract in `PpbTouchAPI.h`, not a code tweak.

### (b) Physical layer

* **NPC genitals** → clone the garment-rig machinery (dynamic, part 30). Inherits the grab-phantom
  shield and therefore the freeze protection.
* **Player wand** → HandBox family (keyframed, own part). The garment rig has **no player execution
  path at all** — `PPBHook.cpp:1282` returns before `NpcFinger::OnPreDrive` at `:1328`.
* **Do NOT reuse `PlayerSpaceWarp`.** `Tuning.h:743-756` documents it as unnecessary *and phased
  wrong* — *"ours is derived from the hand bones' world.translate, which ALREADY contains the player's
  movement — so the warp applies locomotion a SECOND time"* — yet it ships at `1.f`. Derive targets
  from third-person bone world transforms and do not warp.

---

## 4. FILTER MATRIX

Rig word today: `56 | (30 << 8) | 0x8000 | (group << 16)`. Dispatch: `NpcFinger::FilterDecision` →
`HandBox::FilterDecision` → PerfSys; first non-Continue wins. `0` = Continue, `1` = Collide, `2` = Ignore.

| # | Body | vs | Today | Wanted | Edit | Risk |
|---|---|---|---|---|---|---|
| 1 | NPC genital | player HIGGS hand | **Collide** (`:3117-3122`) | Collide | none | — |
| 2 | NPC genital | PPB HandBox | **Collide** (same branch) | Collide | none | sub-layer adjacency rests on an un-disassembled comparator |
| 3 | NPC genital | own ragdoll (same grp) | Ignore | Ignore | none | must stay — own-body mode is tbl-0 only |
| 4 | NPC genital | statics / clutter | Ignore | Ignore | none | widening oscillates (infinite mass) |
| 5 | NPC genital | HIGGS grab phantom 40/44 | **Ignore** (`ppbSig` shield `:3066-3072`) | Ignore | none | **the wig-freeze shield — keep part 30 or re-implement** |
| 6 | NPC genital | PCVR bodies | Ignore (bit15 clear) | Ignore | none | both sides Ignore (doc 19 §5) |
| 7 | **Player wand** | NPC capsules (8/32/33) | Ignore | **Collide** | terminal `return 0` → vanilla 56 row | **headline payoff** |
| 8 | **Player wand** | NPC genital rig | Ignore | **Collide** | add wand part to the `op ==` list at `:3117` | trivially forgotten |
| 9 | **Player wand** | own HIGGS hand / weapon clone | Collide | **Ignore** | copy HandBox's belt verbatim | wasted points, spurious haptics |
| 10 | **Player wand** | own hand boxes | Ignore (`HandBox.cpp:1727-1745`) | **detection only** | new rule if reporting wanted | ⚠ **no physical push possible either way — §1** |

---

## 5. ⛔ THE SCENE TRAP — the use case is currently switched off

`sceneSuspendHands 2` ships **on** (`PPB_tuning.txt:1863`). During an OStim/SexLab scene read as third
person, PPB **destroys the player's hand collider rig** (`HandBox.cpp:1156-1186` → `DestroyHand`)
because *"the animation puts them inside the partner"*, with a 40u HMD-to-head heuristic as the only
first/third-person discriminator (`IsInFirstPerson()` never flips in VR).

**So during the exact scenes this feature exists for, every hand-class contact is structurally
silent today** — and a wand built on the HandBox lifecycle inherits that teardown. A scene policy
other than "destroy it" is a prerequisite, not polish.

Also unhandled: `PpbApi.cpp` has **zero** gates on ragdoll / knock / life / furniture state, so
COM-hosted sensors are read and reported while PLANCK has the ragdoll loosened (ledger `01:329-330`).

---

## 6. BUILD ORDER

| # | Step | Payoff | Conf |
|---|---|---|---|
| 1 | **Fix `GenitalProbe`, run ONE VR session**: read slot 52 not 32, drop the keyword match, log the occupant + TNG state, move the Tick above the player early-return. Creates nothing. | Settles the gate empirically; measures TNG equip latency | 95% |
| 2 | **Ship the gate as a pure receipt** — capability latch + hysteresis + `ActorRagdollAttached` + knob + console toggle. Still creates nothing. | Walk a town: flips exactly on dress/undress, silent for every ordinary female, never mentions a bear | 93% |
| 3 | **NPC rig, ONE capsule, one pinned actor** (`GenBase → Gen01`). | First physical capsule; rows 1–2 already Collide with no filter edits | 84% |
| 4 | Full chain + mass/radius/material tuning. | NPC feature complete and useful alone | 82% |
| 5 | **API: NPC genitals as touch targets** (new pseudo-slot + part naming, knob-gated). | Consumers get events; no player work yet | 85% |
| 6 | **Scene policy rework** (prerequisite for anything player-side in scenes). | Un-silences the use case | — |
| 7 | **Player wand, keyframed, HandBox family** — own part, terminal `return 0`, row-9 belt, re-implemented grab shield. | Third wand as a physical collider vs NPCs | 74% |
| 8 | **Player wand as API source** (`wand = 2`, new source kind), knob OFF, VRTE change request first. | Events. Last — the only step touching a published contract | 78% |

---

## 7. OPEN QUESTIONS — answer before code

1. **Which half of "collide" does the player wand need?** Push her (keyframed works) vs be stopped by
   your hand or a wall (impossible without a dynamic body and the freeze risk). §1 is the decision.
2. **Are the player's genital bones actually posed in VR?** Never measured — the probe has never run
   on the player. If nothing animates that chain on the player avatar, the wand has nothing to follow
   and the design changes.
3. **Scene policy**: what should hand and genital colliders do during an OStim scene, if not be
   destroyed? (§5)
4. **Male coverage**: does the touch API open to males (published contract change), or does the NPC
   half ship female-only-plus-gentlewomen first?
5. **Group choice for the player wand**: inherit the player group (self-Ignore) or take a fresh
   private group as PCVR does for its 19 player parts (charController interaction untested by PPB).
