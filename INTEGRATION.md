# Integrating with the PPB Touch API

**Precision Physic Bodies (PPB) tells your mod when the player touches an NPC, where, with what,
and for how long.** This document is everything you need to consume it. No prior PPB knowledge
assumed.

There are three ways in, and they are equivalent in what they report:

| You are writing | Use | Effort |
|---|---|---|
| A Papyrus script | **Mod events** — `RegisterForModEvent("PPB_TouchStart", ...)` | 10 lines |
| A Papyrus script that polls | **Natives** — script `PPB_Touch`, 12 global functions | 15 lines |
| An SKSE (C++) plugin | **The native interface** — copy `src/PpbTouchAPI.h` | 30 lines |

Start with the mod events. Move to the native interface only if you need callbacks at frame rate,
live capsule geometry, or to avoid the Papyrus VM.

---

## What changed in 2.0.0 — read this if you already integrated

`GetBuildNumber()` now returns **20000**; feature-detect on `>= 20000`. `PpbTouchContact` is still a
frozen 160-byte POD and the vtable is unchanged, so an existing plugin keeps working untouched.

| change | what it means for you |
|---|---|
| **Males are covered.** | The old "males answer `IsDriven()==false`" rule is GONE — see §5. If you route males to a fallback, gate that on the build number. |
| **The raw stream had a real bug — fixed.** | Contacts in dense capsule clusters could be dropped **silently**: no event, no snapshot entry, no log. If you consume raw and have ever seen "nothing happened" for an obvious touch, this was why. See §6. |
| **New source: `kSourceGenital = 7`.** | The player's own genitals now report as a touch source, with `sourceName` = `"shaft"` or `"tip"`. |
| **New data: erection level.** | `_reserved[0]` on every contact — see §1. |
| **Male sub-regions were wrong.** | A male anal touch used to publish `kSubVaginal*` and `orificeKind = 1` (vaginal). Now correct. If you defended against this by matching capsule NAMES, you can stop. |

---

## 1. What a contact tells you

Five things, which were the whole point of the design:

| | Meaning | Example |
|---|---|---|
| **WHO** | the touched NPC | the event's `sender`, an `Actor` |
| **WHERE** | the body part, at three levels of detail | `Face` / `In mouth` / `palate` |
| **BY WHO** | the toucher | the player (see *Coverage*, below) |
| **WITH WHAT** | how they touched | `FINGER`, `FIST`, `PALM`, `GRAB`, `WEAPON:Iron Sword`, `OBJECT:Apple` |
| **DURATION** | how long the contact has lasted | `6.11` seconds |

Plus a distance in game units — **negative means inside the capsule**, so it doubles as a
penetration depth.

### Erection level (2.0.0) — `_reserved[0]`

Every contact whose **touched** actor carries a PPB genital rig reports the erection level PPB is
currently asserting — the same value that drives `SOSBend`, not an arousal guess:

| `_reserved[0]` | meaning |
|---|---|
| `0` | absent — older PPB, female, no genital rig, or dressed |
| `1` | level 0, flaccid |
| `N` | level N−1 |

The offset-by-one exists so the reserved tail's contractual zero already means "absent". The byte is
written on **every** contact including the zero, so a `0` arriving mid-contact means *"no longer
known"* — not *"unchanged"*. §9 tells you not to read `_reserved`; this byte is the documented
exception, and only this byte.

### The three levels of WHERE

```
Region       Face          13 of these. The coarse bucket, and what the digest groups by.
SubRegion    In mouth      33 of these. The finer bucket, and the depth ladder.
Capsule      palate        107 of these. The exact named capsule.
```

The full list of all 107 capsules with their region, sub-region, depth and override behaviour is
in **`PPB_Touch_API_Contact_List.xlsx`** (shipped alongside this file). Read it once; it will
answer most of your questions faster than this document will.

### The depth ladder — the part people get wrong

Within the mouth and within the intimate chain, sub-regions are ordered shallow → deep, and each
level **overrides** the ones below it:

```
Face surface  <  Mouth opening  <  In mouth  <  Mouth wall
0 surface        1 opening         2 inside     3 deepest
```

A cheek or a chin reports **`Face surface`**. Those capsules participate in mouth detection, but
only *in conjunction* — the mouth gate needs the palate AND both cheeks at once. On its own, a
cheek touch is a face touch and nothing more. **Do not treat a cheek contact as "near her mouth".**

The **palate** reports `In mouth`. If it registers, something *is* inside her mouth, and that
outranks any simultaneous lip or cheek reading.

The **throat wall** reports `Mouth wall` and outranks even the palate.

The intimate chain works the same way: `opening` → `deep` → `deepest` for both tracts.

If you only care *how far in*, ignore the names and read the depth number:

```papyrus
If PPB_Touch.GetContactDepth(i) >= 2      ; 2 = inside, 3 = deepest
```

---

## 2. Papyrus — mod events (the easy path)

```papyrus
Scriptname MyTouchWatcher extends ReferenceAlias

Event OnPlayerLoadGame()
    RegisterForModEvent("PPB_TouchStart", "OnPpbTouchStart")
    RegisterForModEvent("PPB_TouchEnd",   "OnPpbTouchEnd")
EndEvent

; strArg = "WAND|SOURCE|BODYPART|SKELETON"
; numArg = surface distance in units on Start (negative = inside)
;          the contact's total DURATION in seconds on End
Event OnPpbTouchStart(String eventName, String strArg, Float numArg, Form sender)
    Actor victim = sender as Actor
    String[] f = StringUtil.Split(strArg, "|")     ; PapyrusUtil, or split by hand
    ; f[0] = "L" or "R"      f[1] = "FINGER"
    ; f[2] = "Face(cheek L)" f[3] = "human"
    Debug.Notification(victim.GetDisplayName() + ": " + f[1] + " on " + f[2])
EndEvent

Event OnPpbTouchEnd(String eventName, String strArg, Float numArg, Form sender)
    If numArg > 3.0
        Debug.Notification("held for " + numArg + "s")
    EndIf
EndEvent
```

Three events fire per contact: `PPB_TouchStart`, then `PPB_Touch` repeatedly while it holds
(at `apiHz`, default 4/s), then `PPB_TouchEnd`.

> **Caprica users:** the compiler rejects new event declarations in non-native scripts. Declare
> your `OnPpbTouchStart` (empty body) in an imported ancestor stub so your script's copy is an
> override. Bethesda's compiler does not need this.

### Registering the callback

`RegisterForModEvent` must be re-run every game load. Put it in `OnPlayerLoadGame()` on a
player-alias script, or `OnInit()` plus `OnPlayerLoadGame()`. A quest script never receives
`OnPlayerLoadGame` — use a `ReferenceAlias` filled with the player.

---

## 3. Papyrus — the polling natives

For the sub-region, the depth, or when you would rather ask than listen. The script is
`PPB_Touch` (already compiled and shipped — you do not need the source to call it):

```papyrus
Int n = PPB_Touch.GetContactCount()
Int i = 0
While i < n
    Actor  who      = PPB_Touch.GetContactActor(i)
    String region   = PPB_Touch.GetContactRegion(i)      ; "Face"
    String sub      = PPB_Touch.GetContactSubRegion(i)   ; "In mouth"
    Int    depth    = PPB_Touch.GetContactDepth(i)       ; 2
    String part     = PPB_Touch.GetContactBodyPart(i)    ; "Face(palate)"
    String source   = PPB_Touch.GetContactSource(i)      ; "FINGER"
    String wand     = PPB_Touch.GetContactWand(i)        ; "L" or "R"
    Float  duration = PPB_Touch.GetContactDuration(i)    ; seconds
    Float  distU    = PPB_Touch.GetContactDistance(i)    ; negative = inside
    i += 1
EndWhile
```

### Want events less often than PPB sends them?

PPB emits at `apiHz` (4/s). If your mod only needs a heartbeat, filter on your side — the contact's
own duration makes this a one-liner, and it costs nothing:

```papyrus
; act at most once per second per contact
Float dur = PPB_Touch.GetContactDuration(i)
If dur - lastActedAt >= 1.0
    lastActedAt = dur
    ; ... your reaction ...
EndIf
```

There is deliberately no per-consumer rate knob in PPB: the host cannot know what each mod needs,
and a filter you own is one comparison.

**Indices are not stable between polls.** The list is a snapshot refreshed at `apiHz`; read
everything you need for a contact in one pass, then move on. Do not cache index `3` and expect it
to be the same contact next tick.

### Using events *and* the full data

A Papyrus mod event carries one packed string and **one** number, so an event-only handler cannot
see depth, weapon class or provenance, and can never have distance *and* duration at once. That is
a limit of Papyrus events, not of the API — bridge it with `FindContact`:

```papyrus
Event OnPpbTouchStart(String eventName, String strArg, Float numArg, Form sender)
    Int i = PPB_Touch.FindContact(sender as Actor, "")     ; "" = either hand
    If i >= 0
        If PPB_Touch.GetContactDepth(i) >= 2               ; 2 = inside
            String w = PPB_Touch.GetContactWeaponClass(i)  ; "Dagger"
            String e = PPB_Touch.GetContactWeaponEdge(i)   ; "Pierce"
            Float  d = PPB_Touch.GetContactDistance(i)     ; and duration, sub-region, ...
        EndIf
    EndIf
EndEvent
```

Every value is separate and typed — nothing needs parsing out of a string. `FindContact` returns
-1 once the contact has ended, which is normal inside a `PPB_TouchEnd` handler: read what you need
on Start, and take the final duration from the End event's own `numArg`.

Full function list: `GetContactCount`, `GetContactActor`, `GetContactBodyPart`,
`GetContactSource`, `GetContactWand`, `GetContactSkeleton`, `GetContactDuration`,
`GetContactDistance`, `GetContactPacked`, `GetContactRegion`, `GetContactSubRegion`,
`GetContactDepth`, `GetContactWeaponClass`, `GetContactWeaponEdge`,
`GetContactIsEngine`, `FindContact`.

---

## 4. SKSE C++ — the native interface

Copy **`src/PpbTouchAPI.h`** into your project. It is deliberately self-contained: no CommonLib
types, no SKSE types, actors addressed by FormID. It compiles under CommonLibSSE, CommonLibVR or
classic skse64.

### Acquiring it

The same request/reply pattern HIGGS and PLANCK use. Any time at or after `kPostLoad`:

```cpp
#include "PpbTouchAPI.h"
PPBAPI::IPpbTouchInterface1* g_ppb = nullptr;

void AcquirePPB() {
    PPBAPI::PpbMessage msg{};
    SKSE::GetMessagingInterface()->Dispatch(
        PPBAPI::PpbMessage::kGetTouchInterface, &msg, sizeof(msg), "PPB");
    if (msg.GetApiFunction)
        g_ppb = static_cast<PPBAPI::IPpbTouchInterface1*>(msg.GetApiFunction(1));
    logger::info("PPB touch API: {}", g_ppb ? "acquired" : "not present");
}
```

`GetApiFunction` null ⇒ PPB is not installed. `GetApiFunction(1)` returning null ⇒ PPB does not
speak revision 1 (it will, for the foreseeable future). **Both are normal.** Ship a fallback path;
never assume PPB is there.

### Receiving contacts

Either register a callback:

```cpp
void OnTouch(const PPBAPI::PpbTouchContact* c, int phase) {
    if (phase != PPBAPI::kPhaseStart) return;
    logger::info("{:08X} {} / {} / {} by {} for {:.2f}s at {:.2f}u",
                 c->actorFormId,
                 g_ppb->RegionName(c->region),        // "Face"
                 g_ppb->SubRegionName(c->subRegion),  // "In mouth"
                 c->bodyPart,                         // "Face(palate)"
                 (int)c->sourceKind, c->durationS, c->distU);
    if (c->depth >= PPBAPI::kDepthInside) { /* something went in */ }
}
g_ppb->AddTouchCallback(&OnTouch);
```

...or poll a snapshot whenever it suits you:

```cpp
PPBAPI::PpbTouchContact buf[16];
int n = g_ppb->GetContacts(buf, 16);
```

The contact pointer handed to a callback is valid **only for the duration of the call** — copy
what you need. Callbacks fire on the **main thread**, at most at `apiHz`. Do not block in them.

### The rest of the interface

```cpp
GetBuildNumber()                          // version probe
IsDriven(formId)                          // is this actor covered at all?
GetSkeleton(formId, out, cap)             // "human" / "argonian" / "khajiit" / "draenei"
GetContacts(out, max)                     // digest snapshot
GetRawContacts(out, max)                  // verbose snapshot (see §6)
ReadCapsule(formId, slot, left, child, a, b, &r)   // live world geometry, game units
CapsuleName(slot, child)                  // "palate"
ChildCount(formId, slot)                  // live child count of a slot
RegionOf(slot, child) / RegionName(r)
SubRegionOf(slot, child) / SubRegionName(s) / SubRegionDepth(s)
```

`ReadCapsule` is the one worth knowing about beyond touch: it hands you the live world-space
endpoints and radius of any capsule on any driven NPC, already carrying that NPC's scale, body
shape and pose. If you need to know where a body part physically *is*, that is the call.

---

## 5. Coverage — read this before you ship

**PPB drives NPCs of mapped races, BOTH SEXES as of 2.0.0**: the human catch-alls (covering elf, orc
and most custom races on a typical load order), Argonian, Khajiit, Draenei (female), plus anything
the user adds to `PPB_Skeletons_Added_Race.ini`.

**Males joined in 2.0.0** — three male skeletons ship (human, Khajiit, Argonian). Two consequences
if you integrated earlier:
* the old "males always answer `IsDriven() == false`" rule is **gone**. Gate any male fallback on
  `GetBuildNumber() >= 20000` rather than on sex.
* **part names are sex- and skeleton-routed.** The male COM carries anal cover / anus / rectum where
  the female carries the vaginal ladder, and beast heads (Khajiit/Argonian) have their own table —
  snout, jaw, crest, horn/ear. **Consume the name strings; never assume the female reference map.**

Male coverage is a host setting (`maleGeometry`, shipped on). A user who turns it off puts males
back to `IsDriven() == false`, so still handle that answer.

**Children and creatures are not covered**, nor is anyone on an unmapped custom skeleton. They
answer `IsDriven() == false` and never appear in the contact stream. Route them to your own
fallback — do not assume silence means "nothing happened".

**The toucher is the player.** NPC-vs-NPC touch is a planned revision-2 feature; `toucherFormId`
is `0x14` today and you should not hardcode that assumption in a way that breaks later.

**Hover counts as contact.** The default threshold is 1.0 unit (~1 cm), so a near-miss registers
briefly. If you want presses only, filter on `distU < 0`.

**A weapon must be drawn to touch anything.** `WEAPON:` contacts require the player's weapon to be
actually drawn (`IsWeaponDrawn()` — the VR combat-stance flag). Sheathe, or drop out of combat
stance, and the weapon probe sleeps; the hand keeps reporting normally as `FINGER` / `PALM` /
`FIST`. (Before 1.4.2 a sheathed weapon reported phantom contacts — HIGGS keeps its weapon
collision body alive on the controller after you sheathe, so an invisible blade rode the player's
hand. Those events were never real touches and are gone as of 1.4.2.)

---

## 6. Two streams — digest and raw

A real touch wanders. A finger on someone's face crosses five capsules in six seconds without
resting half a second on any one of them. Reporting each capsule either floods you or — with a
naive per-capsule dwell filter — reports **nothing at all**.

So there are two streams, and you pick:

| | **DIGEST** (default) | **RAW** (opt-in) |
|---|---|---|
| events | `PPB_TouchStart` / `PPB_Touch` / `PPB_TouchEnd` | `PPB_TouchRawStart` / `PPB_TouchRaw` / `PPB_TouchRawEnd` |
| one contact per | (actor, hand, **region**) | (actor, hand, source class) |
| BODYPART | `Face(cheek L)` | `cheek L` |
| dwell gate | accumulated time **in the region** | accumulated time **in the capsule group** |
| native | `GetContacts()` | `GetRawContacts()` |
| callbacks | yes | no — poll |

**Use the digest.** It is what you want ~95% of the time: one `Face(cheek L) dur=6.11s` event for
a whole visit, naming the capsule dwelt on longest.

Two deliberate properties of the digest:

* **Changing hand pose does not restart the contact.** Going finger → fist mid-touch is one
  continuous contact; `SOURCE` reports the pose held longest.
* **`distU` is the deepest penetration reached during the visit**, not the current frame. For a
  summary event, "how far did it get" is the useful number. Raw carries live distance.

The raw stream's **events** ship off (`apiRawEvents 0` in `PPB_tuning.txt`) because they are chatty
— but `GetRawContacts()` polling works regardless of that knob. Ask users to enable the events only
if your mod genuinely needs per-capsule transitions pushed to it.

### ⚠ Raw stream bug, fixed in 2.0.0 — if you consume raw, read this

Before 2.0.0 the raw stream timed its dwell on the **exact capsule** and restarted the timer every
time the nearest capsule changed. `BODYPART` also reported whichever capsule was nearest on the
emitting tick. That is the naive filter this section opens by warning about, and it had the failure
this document predicted: in a dense cluster the nearest flickers between neighbours every tick, the
timer never survived its own gate, and the contact was **dropped in total silence** — no event, no
`GetRawContacts()` entry, and no log line, because raw had no logging at all.

It hit hardest exactly where it hurt most: **the denser the region, the more certain the drop.** The
intimate ladder is 11 capsules; the chest carries ring, breast supports, shoulder caps and rib cage.
Two real seven-second contacts reached a consumer as nothing whatsoever.

As of 2.0.0 raw behaves like the digest and like this document always described: the **capsule
group** is the dwell identity, per-capsule time accumulates, and `BODYPART` names the capsule
**dwelt on longest**. Neighbour flicker inside a group no longer restarts anything; cheek → mouth
still starts a new contact. Raw Start/End also log now (`apiLog`), as `API RAW START` / `API RAW END`.

If you built a workaround for missing raw contacts, you can retire it.

---

## 6b. `durationS` counts from FIRST CONTACT (pinned 2026-08-23)

`durationS` is measured from the moment PPB first *detected* the contact — not from the moment it
was first *emitted* to you. The two differ by the dwell gate: a contact is tracked at full rate and
only reported once it has lingered (§7), so the first event you receive already carries a non-zero
`durationS` equal to roughly the dwell.

This matters if you rebase your own timing off it — e.g. `max(myDelay - durationS, 0)`. That
computation is correct against this definition. At the shipped 0.25 s gates the offset is small, but
a user who raises `apiDwellComS` to 2.0 makes it a two-second error, so the epoch is contractual:
**`startMs` is stamped when the contact record is created, before qualification.**

---

## 7. Dwell — why you may see nothing

Tracking is always full-rate. **Emission** is gated: a contact must linger before it is reported
at all. A contact that never qualifies emits nothing — no Start, no End, and it never appears in
the snapshot.

**As of 2026-07-31 every gate ships at 0.25 s — one tick at the default `apiHz 4`.** PPB emits as
soon as it knows. The dwell knobs (`apiDwell*` in `PPB_tuning.txt`: `S`, `HeadS`, `ComS`,
`SensorS`, `TailS`) still exist per region class and a user can raise them, but the shipped
position is: **PPB does not decide what a meaningful touch is — you do.**

The consequence you must own: **incidental brushes reach you.** `apiTouchU 1.0` means hover
counts, so walking past an NPC in a corridor produces short contacts. Filter on `durationS` (or
your own per-part delay) before reacting — a brush cannot hold a body part for a second. Do not
use a long per-NPC cooldown as your filter, or a doorway brush will mute a deliberate touch that
follows it.

One subtlety at one-tick dwell: the first reported capsule is whichever the hand grazed first, not
the one it settles on. **Both streams now converge on the capsule dwelt on longest** as the visit
goes on (raw joined the digest in 2.0.0), so re-resolve your body part on every event, not just on
Start — the same contact can legitimately rename itself mid-visit as the true winner emerges.

---

## 8. Gotchas worth knowing up front

**Self-touch is impossible by construction.** An NPC's own hair or tail can never trigger her own
body: garment rigs are never probe *sources*, only targets. Not a threshold — a property of the
design.

**Contact duration is timed per CAPSULE GROUP**, not per capsule and not per coarse region. A
finger wandering cheek -> chin -> nose stays ONE contact (all `Face surface`); sliding from the
cheek INTO the mouth starts a new one, because the group — and the meaning — changed.

**Both hands report independently — every contact carries its own wand.** Two hands on the same
NPC are two parallel contact streams (even on the same region), so read the `L`/`R` field rather
than assuming one toucher.

**A held weapon or object suppresses that hand's palm/fist contacts — but not a deliberate poke.**
Your palm is wrapped around the grip, so palm and fist reads from a holding hand were phantom
noise and stay muted. The index finger is the exception (2026-07-31): while VRIK reads it as
extended, the holding hand's fingertip reports normally — so a hand carrying a knife can still
poke, and two-hand interactions stay two streams. The *other* hand is always unaffected, and
`GRAB` is unaffected. `apiSuppressHeldHand 2` restores the strict full mute.

**The player's own genitals are a touch source (2.0.0).** `sourceKind = kSourceGenital (7)`, from a
2-segment collider riding the player's SOS/TNG chain that is sized live from the bones, so it tracks
erection and size. Three things differ from every other source:
* `sourceName` names **which part of him** made contact — `"shaft"` or `"tip"` — where other kinds
  put a weapon or object name there;
* **`wand` is meaningless and reads 0.** It is not a hand. Switch on `sourceKind`, never on `wand`;
* it exists only while he is actually **exposed** (TNG slot 52 occupied by his skin, no covering
  garment). Trousers make these contacts stop entirely — that is the gate, not a bug.

One asymmetry to expect: these contacts are reported against tails and hair, but the collider does
not yet physically *push* them. The API sees it; the physics does not.

**Male sub-regions were wrong before 2.0.0.** The COM sensor ladder is sex-specific — the male COM
carries anal cover (C21/22), anus (C23/24) and rectum (C25/26) where the female carries the vaginal
ladder — but the sub-region stamp was applied by index from the female map. A male anal touch
therefore published `kSubVaginalOpening…Deepest` and, worse, **`orificeKind = 1` (vaginal)** on an
actor with no vaginal chain. Only the NAME had been correct. Fixed: males now stamp
`kSubAnalOpening` / `kSubAnalDeep` with `orificeKind = 2`, and `kSubIntimateExternal` for the cover.
If you defended against this by matching capsule name strings, you can retire that.

⚠ Related: the native `SubRegionOf(slot, child)` answers from the **female / reference** map — it is
a static question with no actor and cannot know sex. Live contacts carry the sex-correct value in
`PpbTouchContact::subRegion`; prefer that.

**Hair is declared but never emitted.** `kSlotHair` exists in the header with a warning. Hair
strands drape the face and head, so they would win the nearest-surface race against cheeks and
shadow every face touch. The host can enable it (`apiHairTarget`); write code that tolerates never
seeing it.

**Vanilla tails are not touchable.** Every NPC carries dormant `TailBone01..05` nodes, but without
an SMP rig nothing can drive them. Only HDT-SMP tails produce tail contacts.

**Tail position is thirds, not chord indices.** `base` / `mid` / `tip` are computed from the
chord's position in the chain, so `tip` means the same place on a 4-chord foxtail and a 14-chord
fluffy tail.

**Weapon contacts use two probes, and `engineContact` tells you which you got.** They answer
different questions and neither replaces the other:

| | asks | covers |
|---|---|---|
| `engineContact = 1` | *did Havok collide these shapes?* | the engine's own narrowphase against the weapon's **real** geometry — exact capsule, true separating distance |
| `engineContact = 0` | *is the weapon within ~1u of this capsule?* | a segment+radius approximation — includes **hover and near-misses**, and reaches capsules the engine never reports |

PPB gives you the best of both automatically: when the engine has an opinion it owns the identity
and the depth; the geometric probe fills every gap. You do not have to choose. Read the flag only
if you specifically want to ignore hover (`engineContact == 1` means a real collision happened).

**Weapon contacts are capped and grab-aware.** The blade segment comes from the equipped form's
bound box; on broad weapons the bound radius is the blade plane's breadth (an axe reads 23u), so
PPB caps it at `apiWeaponRMaxU` (6u ~ blade thickness) — without the cap a merely-nearby axe read
deep phantom contacts. And a hand that is HIGGS-grabbing an actor never reports weapon contacts
on *that* actor — the weapon just rides the grip. You will still see its weapon on other NPCs.

**For "is something in her mouth", use the mouth gate events, not capsule names.** The mouth is
the one place a single nearest-capsule verdict is structurally weaker than PPB's own detector:
the gate demands the palate AND both cheeks at once, uses per-race child sets on beast heads, and
requires the touch to be a finger. It fires as edge-triggered mod events (added 2026-07-31):

```
"PPB_MouthLips"    numArg 1/0    at the lips / left them
"PPB_MouthEnter"   numArg 1/0    something entered / exited the mouth
"PPB_MouthThroat"  numArg 1/0    reached the end of the cavity / pulled back
```

`sender` = the NPC, `strArg` = `"WAND|STAGE"`. One event per transition — no dwell, no spam.
The palate and throat wall also participate in the priority race (like the intimate sensors), so
capsule-level contacts name them too — but the gate is the authoritative signal.

**M'rissi reports `skeleton=human`.** Her race is repointed to the human PPB skeleton and her
foxtail is an equipped rig, not skeleton anatomy. Not a bug.

**The event string is four fields.** Split on the first three `|` if you want to be bulletproof
against exotic `OBJECT:` names. There is an opt-in host knob (`apiSubRegionInEvent`) that appends
the sub-region as a fifth field — it ships **off** precisely so this contract does not move under
you. Get the sub-region from the natives or the C++ struct instead.

---

## 9. Versioning contract

**The vtable is append-only.** Methods are never reordered, removed, or re-signatured. New
functionality is appended at the end, or arrives as an `IPpbTouchInterface2`.

**There is deliberately no virtual destructor.** Slot 0 is `GetBuildNumber`, forever. (The
HDT-SMP v1/v2 split taught the ecosystem what a destructor-at-slot-0 mismatch does: the engine
calls your destructor once per event.)

**`PpbTouchContact` is a frozen 160-byte POD** and grows only into its reserved tail. Do not read
`_reserved` — with one documented exception: **`_reserved[0]` is the erection level** as of 2.0.0
(§1). Everything past it stays undocumented; do not assume it stays zero.

**New `SourceKind` and `PseudoSlot` values append.** 2.0.0 added `kSourceGenital = 7`. A consumer
that has never heard of a value sees an unknown number on an otherwise ordinary contact — handle
your `switch` default rather than asserting.

**Enum values are frozen; new ones append.** Do not assume the numeric spacing between sub-region
families is stable — switch on the names, or use `SubRegionDepth()`.

This is why regions, raw contacts, the tail pseudo-slot and the whole sub-region layer were all
added without a revision bump. A plugin built against the first release of this header still runs
against today's PPB.

---

## 9b. Debugging: see exactly what PPB sends

Set **`contactLog = 1`** in `SKSE/Plugins/PPB_Skeletons_Added_Race.ini` (the user-facing config,
no dev files needed) and every contact is written to `My Games/Skyrim VR/SKSE/PPB.log`:

```
API START 1301DE4F R|FINGER|Face(cheek L)|human d=-0.31u dur=0.00s src=GEO curl=9.1u vrik=I1.00/M0.00
API END   1301DE4F R|WEAPON:Iron Rapier|Neck(neck / throat)|human d=-0.71u dur=2.41s src=ENG wpn=Sword/Blade
```

| field | meaning |
|---|---|
| `START` / `END` | the contact opening or closing; `dur=` on END is its total length |
| `R` / `L` | which hand |
| `FINGER` / `WEAPON:name` / … | the source |
| `Face(cheek L)` | region and the capsule dwelt on longest |
| `d=` | distance in game units, negative = inside |
| `src=ENG` | the game's own physics reported this collision — exact capsule, true depth |
| `src=GEO` | PPB measured proximity — includes hover, and reaches capsules the engine never reports |
| `wpn=` | weapon class and edge (Blade / Blunt / Pierce) |

**The raw stream logs too, as of 2.0.0** — `API RAW START` / `API RAW END`, same format:

```
API RAW START 1301DE4F R|FINGER|CLITORIS|human d=0.98u dur=0.00s
API RAW END   1301DE4F R|FINGER|CLITORIS|human d=0.82u dur=7.23s
```

Before 2.0.0 the raw path logged **nothing**, which is how a real bug there cost a field report
instead of a glance at the log: seven-second contacts vanished with no trail at all. If you consume
raw and something does not arrive, these two lines now split the problem cleanly — a raw line
present means PPB emitted and the gap is on your side; no raw line means it is on PPB's.

If your handler is not firing, this tells you whether PPB saw the touch at all — which separates
"PPB missed it" from "my registration is wrong" in one line. It is verbose (several lines a second
while touching), so it ships **off**; `contactLog = 0` or removing the line disables it.

---

## 10. Reference

| File | What it is |
|---|---|
| `src/PpbTouchAPI.h` | the consumer contract — copy this into your project |
| `PPB_Touch_API_Contact_List.xlsx` | all 107 capsules: region, sub-region, depth, overrides |
| `docs/16_Public_Touch_API.md` | the as-built design record and why each decision was made |
| `docs/22_...Gates_And_The_Grab_Bug.md` | the exposure gate (slot 52) that decides when genital sources exist |
| `docs/15_Capsule_Body_Part_Map.md` | the capsule naming table and how it was verified |
| `Scripts/Source/PPB_Touch.psc` | the Papyrus declarations, with per-function notes |

Host-side knobs live in `SKSE/Plugins/PPB_tuning.txt` and hot-reload at ~1 Hz. The ones a consumer
cares about: `apiTouch` (master), `apiHz` (event rate), `apiEvents` / `apiRawEvents` (streams),
`apiDwell*` (the gates above), `apiTouchU` (hover threshold), `apiHairTarget`, `apiLog`
(log every Start/End to `My Games/Skyrim VR/SKSE/PPB.log` — turn this on first when debugging).

Questions, or a case the API cannot express: open an issue at
<https://github.com/Telord72612/Precision-Physic-Bodies>.
