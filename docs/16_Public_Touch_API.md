# 16 — The Public Touch API (as built and verified, 2026-07-30)

**Module of the Precision Physic Bodies technical reference.**
Scope: PPB's contact-reporting API for other mods — what it emits, how it decides, every
design rule and why, and the measured evidence behind each. VRTouchEvents is the first
consumer; its handbook (`Report/VRTouchEvents Module/13`) carries an AS-BUILT note because the
shipped names differ from its draft.

> **Status: VERIFIED IN VR** across five same-day sessions (2026-07-30). Every source kind, tails, weapons,
> multi-actor, sliding, penetration depth. The digest layer, dwell filter, sensor priority and
> held-hand suppression are **built and deployed but not yet exercised in VR** — they landed
> after the last test session. Verification matrix in §9.

---

## 1. What it delivers

Five variables per contact, the user's original spec:

| variable | how it arrives |
|---|---|
| **WHO** — the touched NPC | event `sender` = the Actor; `actorFormId` natively; `PPB_Touch.GetContactActor(i)` |
| **WHERE** — the body part | named from the 107-capsule map; digest reads `Region(longest part)` |
| **BY WHO** — the toucher | the player in revision 1 (`toucherFormId = 0x14`); NPC touchers are rev 2 |
| **WITH WHAT** | `FINGER / PALM / FIST / HAND / GRAB / WEAPON:<name> / OBJECT:<name>` |
| **DURATION** | seconds since the contact began; `PPB_TouchEnd`'s `numArg` **is** the final duration |

Three channels, three audiences:

1. **Papyrus mod events** — the drop-in path. `strArg = "WAND|SOURCE|BODYPART|SKELETON"`.
2. **Papyrus polling natives** — script class `PPB_Touch`, 9 `GetContact*` functions, snapshot-backed.
3. **Native SKSE interface** — `PPBAPI::IPpbTouchInterface1`, acquired with the HIGGS
   request/reply pattern: dispatch `PpbMessage::kGetTouchInterface` (`'PPBT'`) to sender `"PPB"`.

The consumer contract is **`src/PpbTouchAPI.h`** — self-contained (FormIDs only, no CommonLib
types), so any SKSE library can copy it in.

---

## 2. TWO STREAMS — the key architectural decision

A real touch wanders. A finger on a face crosses five capsules in six seconds without resting
half a second on any one. Per-capsule reporting either floods the consumer or — with a naive
per-capsule dwell filter — says **nothing at all**. Both failure modes were real: the flood was
observed, and the silence was a defect I shipped for about an hour before the user caught it.

So there are two streams and the author picks:

| | DIGEST (default) | RAW (`apiRawEvents`, ships off) |
|---|---|---|
| events | `PPB_TouchStart` / `PPB_Touch` / `PPB_TouchEnd` | `PPB_TouchRawStart` / `PPB_TouchRaw` / `PPB_TouchRawEnd` |
| identity | `(actor, wand, REGION)` | `(actor, wand, source class)` |
| BODYPART | `Face(cheek L)` | bare capsule name |
| dwell gate | per REGION (accumulated) | per capsule |
| native | `GetContacts()` (slot 03) | `GetRawContacts()` (slot 08) |
| Papyrus natives | yes | via raw events |
| callbacks | yes | no — poll `GetRawContacts()` |

**Digest identity deliberately excludes the source class**, per the user: *"switching from index
to fist should not restart the contact."* Time is accumulated per part **and** per source; the
report names whichever of each was held longest. So the canonical case reads:

```
R|FINGER|Face(cheek L)|human   dur=6.11s
```

one event for the whole visit, naming the capsule dwelt on longest and the pose held longest.

**Digest `distU` is the DEEPEST penetration reached during the visit**, not the current frame —
for a summary event "how far did it get" is the useful number. Raw carries live distance.

---

## 3. Regions

13 regions (`PPBAPI::Region`): Face, Neck, Chest, Belly, Waist, Pelvis, **Intimate**, Arm, Hand,
Leg, Foot, Tail, Hair. Mapped by `RegionOfPart(slot, child)`; exposed to consumers as
`RegionOf()` / `RegionName()`.

**Intimate is split out of Pelvis on purpose.** "Touched her hip" and "inserted" must not look
alike to a consumer, so COM children 21–31 form their own region with their own dwell class.

---

## 4. Detection — what it is and is NOT

**Pure geometry on bodies PPB owns.** Point-to-capsule-surface (or segment-to-capsule for a
blade), every tick, at `apiHz`. There are **no Havok contact listeners**, so consuming this costs
no physics time — and, critically, **capsule motion is not the signal**. A user reasonably asked
"if the collision capsule moves, that records a contact, right?" — no. Havok collision and this
API are separate systems observing the same moment. The blade *pushed* her (Havok) while the API
saw nothing, because the API was measuring from the wrong point (see §6).

**Self-touch is structurally impossible.** An NPC's own hair/tail capsules can never trigger her
own body, because garment rigs are never *probe sources* — only the player's hands, weapon and
held object are. Not a tuned threshold; a property of the design.

**Probes** (per tick, per hand): the 4 HandBox boxes (tips for the index pair, centres for slab
and palm plate), the wielded weapon as a **segment**, the HIGGS-held object (worldBound centre +
radius). A grabbed *Actor* never becomes an OBJECT — it becomes GRAB.

**Targets**: 12 body slots × sides × live children, plus tail chords (pseudo-slot 100). Hair
(101) is declared but **gated off** — see §8.

**Two-level culling**: actor gate (any probe within 160u of the actor centre; segment probes test
both endpoints and the midpoint), then slot gate (child 0 within 60u stands proxy for the slot),
then children.

---

## 5. Source classification — VRIK first, geometry second

```
GRAB    HIGGS is holding THIS actor with THIS hand
VRIK    getFingerPos(hand, finger): 0 = closed .. 1 = open
          index open  + middle closed → FINGER
          index closed+ middle closed → FIST
          index open  + middle open   → PALM
          (0.45/0.55 dead band: mid-transition falls through to geometry, no flicker)
geometry which box led the contact: index (0/1) → FINGER, plate (3) → PALM, slab (2) → FIST
```

The VRIK layer exists because **geometry alone could not do it**, and the road there is
instructive (§9 fix ladder). `VrikInterface.h` vendors the interface with the vtable copied
verbatim from prog's published header and **truncated after slot 13** — PPB calls only
`getBuildNumber` (0) and `getFingerPos` (11); never call past the truncation without
re-vendoring.

**HIGGS's own `GetFingerValues` is a dead end** — source-checked: it returns
`grabbedFingerValues`, the grab-wrap pose, meaningful only while holding something.

**Held-hand suppression** (`apiSuppressHeldHand`, on): a hand holding a weapon or object stops
reporting bare-hand contacts — your palm is on the grip, so those were phantom `FIST` events
beside every knife touch. **The other hand is unaffected.** GRAB is unaffected too, since
grabbing an actor never sets the object probe.

---

## 6. The weapon — three readers, each forced by evidence

Worth reading as a method, not just a result.

1. **Body position** → missed. HIGGS's weapon rigid body sits at the **hilt, in your fist**;
   touching a neck with the blade leaves the hilt ~40–70u away.
2. **Walk the collision shape** → missed. A one-shot census logged what the shape actually is:
   a `kList` whose three children are **`kConvexTransform` (12)** wrappers — a type CommonLibVR
   has no header for. Guessing that layout is how plugins CTD, so that road was refused.
3. **The equipped FORM's bound box** → shipped. `TESBoundObject::boundData` is a stable public
   structure in the same model space the collision body transforms; its long axis is the blade
   segment, its second extent the radius. **No Havok layout guessing at all.**

A second weapon carried a plain `kBox` and used the direct shape path, so both branches are live.
Contacts now register anywhere along the edge, with real depth (`d=-10.06u` measured).

**2026-07-31 — the broad-bound caveat became a live false positive and is now fixed twice over.**
The user held the NPC's leg with the axe hand while the OTHER hand touched her; the axe (bound
radius 23u = the blade PLANE's breadth, making the probe a 46u-diameter barrel) reported
"cervix -16.9u dur=13.6s" — phantom data that drowned the real finger readings. Fixes:
1. **`apiWeaponRMaxU 6`** — the blade-segment radius is capped at blade *thickness*; the axis
   must now genuinely be near a capsule to read contact. Swords/knives carry slim bounds and
   are unaffected. 0 = uncapped.
2. **Grab mutes the weapon vs the grabbed actor** — a hand HIGGS-grabbing an actor is holding
   her, not wielding at her; its weapon probe is skipped for HER (stays live vs everyone else).
   `HandProbes::grabActorId`, masked per-actor in `ScanActor`.

---

## 7. Emission filtering — the dwell system

The user's framing: *"if the player touch 20 capsule in one interaction, only the one he linger a
certain amount of time get sent thru the API. It's not so much that we don't track them, it's
more about having them sent."*

Tracking is always full-rate. **Emission** is gated:

| class | knob | shipped | history |
|---|---|---|---|
| Intimate | `apiDwellSensorS` | **0.25s** | was 0.5 |
| default | `apiDwellS` | **0.25s** | was 1.0 |
| Tail/Hair | `apiDwellTailS` | **0.25s** | was 1.0 |
| Face | `apiDwellHeadS` | **0.25s** | was 1.5 |
| Pelvis | `apiDwellComS` | **0.25s** | was 2.0 |

**2026-07-31 — all five gates dropped to 0.25 s = ONE TICK at `apiHz 4` (user decision, VRTE
report 16 §2).** The original per-region values were tuned as a spam filter, but the first real
consumer (VRTE) owns a 17-part x 2-action x 4-armor delay table that is strictly better than five
constants — and PPB's gates were highest exactly where VRTE's are lowest: its 12 zero-dwell cells
sat in the 1.0/2.0 s gates, a 3x-20x regression. Doctrine now: **PPB reports as soon as it knows;
the consumer decides what a meaningful touch is.** Cost accepted: brushes reach consumers (filter
on `durationS`), and the first reported capsule is the first grazed, not the modal one (raw
self-corrects next tick). The knobs remain per-class so a user can restore filtering.

A contact that never qualifies emits **nothing** — no Start, no End, and it never appears in the
Papyrus snapshot. In the digest stream the gate is on **accumulated region time**, which is what
makes the wandering-face case work. In raw it is per capsule.

This is a **host-side global filter**, not per-consumer. Per-consumer thresholds would mean each
mod registering its own; deferred deliberately — one place to tune, and no way for two consumers
to disagree about what happened.

### Interior-sensor priority

The general race keeps the **most-penetrated** capsule, so a big thigh capsule at `-11u` always
beat an `r=0.3` orifice sensor whose depth cannot exceed about `-0.3`. Measured consequence: a
5-second knife insertion reported `L upper thigh d=-11.43u` and **no sensor was ever named**.

Fix: interior sensors run a **separate race that overrides** the general winner, and among
sensors the deepest wins — so WHERE literally answers "how far it reached"
(`vaginal opening` → `cervix` → `uterus`).

**2026-07-31 — the mouth chain joined the priority set** (VRTE report 16 §4.3 caught it, code
verified). The palate (r 1.32) and throat wall (r 0.88) sit INSIDE the cranium (r 5.52) and were
racing it unprotected — the same masking bug one region over; desk math had the throat LOSING to
the cranium by ~0.13u. `isSensor` now also covers slot 3 children 9/10. Head C11 (under-jaw) is
deliberately excluded: GEO-tagged, reachable from OUTSIDE under the jaw — promoting it would let
a chin scritch outrank the face.

### The mouth gate events (2026-07-31)

The nearest-capsule race can never express the mouth gate's logic (AND-of-three capsules,
per-race beast child sets, finger-only). So the gate — previously log-only — now re-emits its
stages as edge-triggered mod events, one per transition, no dwell:

| event | numArg | meaning |
|---|---|---|
| `PPB_MouthLips` | 1/0 | at the lips (C1+C2+C3 within gate) / left |
| `PPB_MouthEnter` | 1/0 | entered the mouth / exited |
| `PPB_MouthThroat` | 1/0 | reached the end of the cavity / pulled back |

`sender` = the NPC, `strArg` = `"WAND|STAGE"`. Emitted from the gate's own transitions in
`NpcFingerTest.cpp` via `PpbApi::EmitMouthStage` — the consumer gets the SAME verdict the phoneme
reaction acts on. This is the authoritative "in her mouth" signal; the capsule-level palate/throat
contacts are the positional detail.

---

## 8. Coverage and scope contract

* **Female NPCs of mapped races only** — human catch-all, Argonian, Khajiit, Draenei, plus
  `PPB_Skeletons_Added_Race.ini`. Males, children, creatures answer `IsDriven() = false`.
  Consumers must route those to a fallback (VRTE keeps CBPC for exactly this).
* `DismemberGuard::IsExcluded` actors are outside every per-actor system → not driven.
* **Tails are targets; hair is not.** `kSlotHair` is declared in the public header with an
  explicit "consumers may never see this" warning. Hair drapes the face and would win the
  nearest-surface race against cheeks, shadowing face touch — the user's call, and correct.
* M'rissi reports `skeleton=human`: her race is repointed to the human PPB skeleton and her
  foxtail is an equipped rig, not skeleton anatomy. Not a bug.
* Hover is a first-class output: `apiTouchU 1.0` means ~1cm near-misses register briefly.
  Consumers wanting presses only should filter `distU < 0`.

---

## 9. Verification — five VR sessions, and the fix ladder

### Verified from receipts

| item | evidence |
|---|---|
| FINGER | `L\|FINGER\|CLITORIS d=0.70u dur=3.31s vrik=I1.00/M0.00` |
| PALM | `L\|PALM\|cheekbone R vrik=I1.00/M0.49`; tail strokes at `I1.00/M1.00` |
| FIST | `L\|FIST\|chin L vrik=I0.00/M0.00` |
| GRAB | `L\|GRAB\|upper glute R → BUTT CHEEK R dur=2.28s`; her cranium; her hand |
| WEAPON | `R\|WEAPON:Iron War Axe\|BREAST R → R upper arm d=-10.06u dur=6.36s` |
| OBJECT | `R\|OBJECT:Knife\|BUTT CHEEK L → R`, an 11s hold; `Apple Dumpling` |
| tail | `PALM\|tail (tip) → tail (base)` both hands; axe slide `tail (base) → … → nose` 9.71s |
| mouth | `MOUTHTOUCH LIPS → ENTER → THROAT REACHED d=1.42u` (separate gate stream, both insertions) |
| multi-actor | Lydia + M'rissi interleaved in one tick stream |
| slide identity | contact survives sliding; source reclassifies live (FIST→FINGER mid-touch) |
| duration | 0.25s … 10.99s | 
| penetration | `+1.0u` hover through `-11.43u` buried |
| wands | L and R independently |

### The scripted session (2026-07-31 17:16) — user-scripted ground truth

The user narrated their touches, making the log gradeable line by line. All matched: breast
(PALM), fist-to-face (FIST at vrik I0/M0), L-grab-her-hand WHILE R-finger poked her other hand
(two parallel wand streams), axe-to-belly (Waist, hover), L-grab-head 6.61s. Fixes verified in
the same pass: axe depths bounded by the radius cap (deepest -9.23 on a deliberate press),
no phantom weapon lines, border flapping gone (exit grace), zero errors.

**★ R-grab-to-neck produced `R|GRAB|Neck(neck / throat)` d=0.13u** — the neck capsule WINS a
choke grab (alternating with `Chest(trapezius L)`, which the grab hand also covers). This
answers VRTE's Phase-0 question (their report 16 §8.2 expected the opposite): their
neck+GRAB choke arming condition is viable.

**★ And it caught a same-day regression: the axe on her NECK reported `Face(throat wall)`**
(d 0.21 → -0.84). C10 sits close behind the exterior throat surface, so the morning's priority
promotion let an OUTSIDE press claim the mouth's deepest rung — the exact false positive C11
was excluded for. Fixed with the throat inside-test: C10 only counts as priority when the probe
is also within `mouthDeepPalU` of the palate (the mouth gate's own inside/outside asymmetry —
from outside the throat the palate is far). The gate itself was never fooled (zero MOUTHTOUCH
lines); only the capsule naming lied.

### The fix ladder — five sessions, five diagnoses

1. **Everything classified FIST.** The curl heuristic's *guessed* 7u threshold swallowed every
   gesture. GRAB/OBJECT/slide/duration proven anyway.
2. **Curl values VARY (3.7–9.1u).** My "the fingers don't articulate" diagnosis was **wrong** —
   an artifact of one grip held through session 1. Box-led classification proved out via a knob,
   no rebuild.
3. **VRIK layer.** The user's own suggestion — read the controller state instead of inferring it.
   Values are binary-clean.
4. **Weapon + tail.** The three-reader weapon saga (§6); tail chords added as pseudo-slot 100
   after the user caught that garments were never targets.
5. **Insertion + spam.** Sensor priority (the masking bug), the dwell filter, regions/digest,
   held-hand suppression.

**The pattern worth keeping**: every one of these was settled by making the plugin *log what it
actually saw* — curl values, weapon shape type, list child types — and then building against the
answer. Three guesses were replaced by three measurements.

---

## 10. Files and knobs

| file | role |
|---|---|
| `src/PpbTouchAPI.h` | **the consumer contract** — copy into your project |
| `src/PpbApi.cpp` | the engine (~1035 lines): probes, scan, raw + digest layers, natives, interface |
| `src/PpbApi.h` | internal hooks: `NoteDriven`, `OnFrame`, `OnPluginMessage`, `RegisterNatives`, `ClearOnLoad` |
| `src/VrikInterface.h` | vendored VRIK interface (truncated at slot 13) |
| `scripts/PPB_Touch.psc` | Papyrus declarations (compile with Caprica + `tools/Caprica/TESV_Papyrus_Flags.flg`) |

Wiring: roster from `NpcFinger::OnPreDrive` (same-frame pointers only), tick on the HIGGS
PostVrikPostHiggs callback, natives registered from `Natives.cpp`, interface answered from
`main.cpp`'s messaging listener, teardown at `kPreLoadGame`.

Knobs (all live, `PK_NOSNAP`): `apiTouch 1 · apiHz 4 · apiTouchU 1.0 · apiExitPadU 0.75 ·
apiMaxActors 3 · apiRangeU 300 · apiFistTipPalmU 2 · apiEvents 1 · apiRawEvents 0 ·
apiSuppressHeldHand 1 · apiHairTarget 0 · apiDwell{S 1.0, HeadS 1.5, ComS 2.0, SensorS 0.5,
TailS 1.0} · apiLog 0`.

### Versioning contract (do not violate)

The vtable is **append-only** — 11 slots today, never reordered, never re-signatured. There is
**no virtual destructor**: slot 0 is `GetBuildNumber`, forever. `PpbTouchContact` is a frozen
160-byte POD with a reserved tail. This is not stylistic: the HDT-SMP v1/v2 split (doc 07) showed
exactly what a destructor-at-slot-0 mismatch does — the engine calls your destructor once per
event. Regions, raw contacts and the tail pseudo-slot were all added *by appending*, which is why
the interface is still revision 1.

---

## 11. Rev 2 — what is deliberately not built

* **NPC touchers.** The whole design is player-as-toucher. NPC-vs-NPC means probing other actors'
  hand nodes; same maths, but cost scales with the crowd and it needs the rig-budget treatment.
  AIHands is the natural first source (it already computes NPC finger positions).
* **Hair as a target** — one knob away, awaiting the user's decision.
* **Per-consumer dwell thresholds** — see §7.
* **A blade-radius cap** for broad weapons — see §6.
