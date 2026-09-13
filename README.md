# Precision-Physic-Bodies

**A complete rebuild of Skyrim VR's NPC Havok bodies — joints that track the XP32 skeleton, capsules
that fit the actual body mesh, both refitted live at runtime.**

> *To quote a pioneer of VR modding: "This mod is complicated."*

![status](https://img.shields.io/badge/status-beta-orange)
![platform](https://img.shields.io/badge/platform-Skyrim%20VR-blue)
![plugin](https://img.shields.io/badge/SKSE-VR-green)
![version](https://img.shields.io/badge/version-2.2.0-blue)

**Latest: 2.2.0.**

**2.2 — push, shove, hands and the kiss.**
- **NPCs react to being pushed.** A steady press walks them back, a firm push makes them stumble,
  and a hard shove or a sweep of both legs knocks them down.
- **The push is read from the NPC's own joints**, as the distance each Havok joint is driven from where
  its animation wants it, so it works the same with a hand, a weapon or a held object.
- **Busy and braced NPCs are respected:** sitting, sleeping, crafting, fighting, leaning.
- **Hand gestures:** equip, two-hand undress and fingertip plug extraction, for ordinary gear and for
  Devious Devices / ZaZ / Diary of Mine. What you put on stays on.
- **Your own head is a collider** that rides the headset, and a **kiss** is a first-class API event.
- **The futa genital rig**, and **per-feature install switches** for every push outcome and the
  gestures.

**2.0 — males.** Three hand-dialled male skeletons (human, Khajiit, Argonian) join the four female
ones. Male NPCs are now driven and reported like anyone else, with their own per-race head layouts
(snout, jaw, crest, horns) and their own COM ladder. Also: a **front-neck** capsule on every
skeleton — a real choking target — and **genital collision** with touch-driven erection.

**2.1 — scenes.** Havok collision was breaking OStim scene alignment. During an OStim scene (player
or NPC-vs-NPC) or a SexLab animation every PPB body is now keyframed to the animation, and restored
afterwards. **Touch detection is unaffected**: it measures live capsule geometry rather than
collision, so the API keeps reporting throughout a scene.

See **[INTEGRATION.md](INTEGRATION.md)** to consume the API, and [CHANGELOG.md](CHANGELOG.md) for
the full list.

---

## Contents

- [The problem](#the-problem)
- [What PPB does](#what-ppb-does)
- [ReScale — fitting the joints](#rescale--fitting-the-joints)
- [ReShape — fitting the capsules](#reshape--fitting-the-capsules)
- [The Touch API — a tutorial for mod authors](#the-touch-api--a-tutorial-for-mod-authors)
- [Push and shove](#push-and-shove)
- [Hand gestures — equip, undress, plugs](#hand-gestures--equip-undress-plugs)
- [Your head, and the kiss](#your-head-and-the-kiss)
- [Features](#features)
- [Fixes](#fixes)
- [Install](#install)
- [Configuration](#configuration)
- [Compatibility](#compatibility)
- [Status and roadmap](#status-and-roadmap)
- [Credits](#credits)

---

## The problem

Every NPC in Skyrim has a **Havok body** — invisible collision capsules and joints that move the body
when something physical strikes it. In the base game these only do anything during a ragdoll: death,
or a shout knockdown. They were never a priority, and hand-authored `.hkx` animation gave better,
more predictable results anyway. In 2011 that was the right call.

Everything else about NPCs was then modernised — skeletons (XP32), meshes (CBBE, 3BA, SoftBody), soft
physics (FSMP, CBPC), shape tools (RaceMenu, BodySlide), animation. **The Havok bodies never were.**

Then Skyrim VR shipped with no physics improvements at all — but the framework was there, and VRIK,
HIGGS and PLANCK turned NPCs into things you can actually touch, grab and throw.

Which exposed the gap. The vanilla body is **18 joints and 19 collision capsules** — crude balls
protruding well past the silhouette. It is why an NPC launches across the room after getting caught
on a chair, standing up through a table, or meeting a mammoth.

## What PPB does

**A Havok body that matches any NPC's scale and mesh one-to-one, and tracks XP32 at 100%.**

| | Vanilla | PPB |
|---|---|---|
| Rigid bodies | 18 | 18 |
| Collision capsules | 19 | **130** (more for horns/tails) |
| Fit to mesh | none | measured per NPC, live |
| Fit to scale | record value (often wrong) | measured from the skeleton |
| Tracks XP32 | partially | fully, including clavicle |

The original 19 capsules remain — they are now parents of child capsules that act together as one
rigid body. The engine treats them identically; the shape they describe is completely different.
Every capsule was hand-placed.

Fitting that to *every* NPC is the hard part. Each has an inherited race scale, a personal height,
possible randomised diversity, one of several base meshes, and any amount of slider, OBody or custom
body reshaping on top. Two systems solve it:

- **ReScale** — refits the Havok **joints** to the XP32 skeleton.
- **ReShape** — refits the collision **capsules** to the NPC's current mesh.

## ReScale — fitting the joints

The engine builds the Havok body at whatever scale the record claims. That value is frequently wrong.

PPB measures instead. It walks the XP32 chain — **anchor → Spine0 → Spine1 → Spine2 → Neck → Head** —
and sums those distances into the **Arc-Sum**:

```
trueScale = current Arc-Sum / XP32 reference Arc-Sum
```

Pose-invariant, and measured rather than guessed. Applied only when the body is neutral and tracking.

**Gates** (a measurement taken in the wrong state latches a permanently wrong "normal"):

- Not in furniture, not ragdolled, not dead
- Havok anchor sitting on the XP32 anchor node
- Within ~10 m of the player
- Failed check → retry ~every 10 s; after 2 minutes of no change → ~every 2 minutes
- On success: **10 readings, median taken** — one frame can catch a mid-settle transient
- Already correct within ~**0.5 game units** → writes **nothing** and idles

## ReShape — fitting the capsules

Scale alone doesn't fit real bodies: weight changes shape, OBody varies it endlessly, custom NPCs
ship their own meshes.

ReShape runs after ReScale, and again on any detected OBody morph. It reads the NPC's **UV skin map**,
takes **11 coordinates** marking the same anatomical points on every body, finds the nearest real
vertex to each, and measures how far that region actually sits from the skeleton — to the matching
landmark for limbs, or to the region's own XP32 node.

That yields **trueShape**, which together with trueScale places every capsule at the skin of that
specific body.

Works with any CBBE-based mesh and skin texture, including SoftBody.

**Breasts** use the same UV mapping with three vertices — nipple, above, below — measured against the
XP32 Spine2 node and triangulated for elevation and thickness against a known neutral, giving the
sag, inflation and offset the capsules need.

> **In short:** any NPC on an XP32 skeleton with any CBBE-based body — 3BA, SoftBody, custom NPCs,
> OBody, arbitrary morphs. *(Males since 2.0 run on their own hand-dialled skeletons.)*

## The Touch API — a tutorial for mod authors

**PPB knows exactly where you are touching an NPC. Since 1.4.0 it will tell your mod** — and since
2.0, for male NPCs too.

Because PPB rebuilds every driven NPC's collision as ~107 individually named capsules fitted to her
actual body, it can answer a question nothing else in the load order can. Not "the player touched
her", but:

```
R|FINGER|Face(cheek L)|human                   d=-0.31u  dur=2.41s
L|WEAPON:Iron Rapier|Neck(neck / throat)|human d=-0.71u  dur=0.50s
```

This section is a working tutorial. Copy the code, it runs. If you want the deep end — the raw
stream, dwell tuning, every gotcha — that is **[INTEGRATION.md](INTEGRATION.md)**.

---

### Step 1 — decide which door you need

| You are writing | Use | Read |
|---|---|---|
| A Papyrus script that reacts to touches | **mod events** | Step 2 |
| A Papyrus script that needs depth, weapon class, or exact numbers | **the 16 natives** | Step 3 |
| An SKSE plugin | **the C++ interface** | Step 4 |

Papyrus needs no dependency at all — PPB ships its own compiled script. C++ needs one header file
copied into your project. Neither requires PPB to be installed at build time.

---

### Step 2 — Papyrus in ten lines

Three events fire per contact: `PPB_TouchStart`, then `PPB_Touch` repeatedly while it holds, then
`PPB_TouchEnd`.

```papyrus
Scriptname MyTouchWatcher extends ReferenceAlias

Event OnPlayerLoadGame()
    RegisterForModEvent("PPB_TouchStart", "OnPpbTouchStart")
    RegisterForModEvent("PPB_TouchEnd",   "OnPpbTouchEnd")
EndEvent

Event OnPpbTouchStart(String eventName, String strArg, Float numArg, Form sender)
    Actor victim = sender as Actor                 ; WHO was touched
    String[] f = StringUtil.Split(strArg, "|")     ; "WAND|SOURCE|BODYPART|SKELETON"
    Debug.Notification(victim.GetDisplayName() + ": " + f[1] + " on " + f[2])
EndEvent

Event OnPpbTouchEnd(String eventName, String strArg, Float numArg, Form sender)
    Debug.Notification("held for " + numArg + " seconds")   ; numArg = DURATION on End
EndEvent
```

**The three things to get right:**

1. **`sender` is the NPC who was touched.** Cast it to `Actor`.
2. **`strArg` is four fields joined by `|`.** Split it:

   | Field | Example | Values |
   |---|---|---|
   | `f[0]` **WAND** | `"R"` | `L` / `R` — the two hands are tracked independently |
   | `f[1]` **SOURCE** | `"FINGER"` | `FINGER` / `PALM` / `FIST` / `HAND` / `GRAB` / `WEAPON:<name>` / `OBJECT:<name>` / `GENITAL:<shaft\|tip>` / `HEAD:<face\|head\|mouth>` |
   | `f[2]` **BODYPART** | `"Face(cheek L)"` | `Region(part)` — the region, and the capsule you spent longest on |
   | `f[3]` **SKELETON** | `"human"` | `human` / `argonian` / `khajiit` / `draenei` |

3. **`numArg` changes meaning between events.** On `Start`/`Touch` it is the surface distance in
   game units, negative meaning inside the capsule. On `End` it is the contact's **total duration
   in seconds**. This is the one place people get caught.

`RegisterForModEvent` must be re-run every game load. Put it in `OnPlayerLoadGame()` on a
**ReferenceAlias** filled with the player — a quest script never receives that event.

> **Caprica users:** the compiler rejects new event declarations in non-native scripts. Declare
> `OnPpbTouchStart` with an empty body in an imported ancestor stub so your copy is an override.
> Bethesda's compiler does not need this.

**Mouth events**, if that is what you are after — edge-triggered, no parsing needed:
`PPB_MouthLips`, `PPB_MouthEnter`, `PPB_MouthThroat`.

**The kiss** (build ≥ 20103) arrives on the same event. The player's mouth meeting her upper lip
sends `PPB_MouthLips` with strArg `"R|LIPS|HEAD"`, `numArg` 1 when the kiss starts and 0 when it
ends. Split on `|` and tolerate the extra third field:

```papyrus
Event OnPpbMouthLips(String eventName, String strArg, Float numArg, Form sender)
    String[] f = StringUtil.Split(strArg, "|")
    If f.Length >= 3 && f[2] == "HEAD"
        If numArg > 0.5
            ; kiss started on (sender as Actor)
        Else
            ; kiss ended
        EndIf
    EndIf
EndEvent
```

**The push event** (build ≥ 20104) tells you what a push *did* to an NPC, once per reaction.
`PPB_PushReaction` has strArg `"<kind>|<NPC name>"` with kind `push` (walked back), `shove`
(stumbled), `dropped` (shoved to the ground) or `sweeped` (legs swept), and `sender` = the NPC. It
is only sent for a reaction the game actually played. Details are in
[INTEGRATION.md §2b](INTEGRATION.md#2b-the-push-event--ppb_pushreaction-build-20104).

**Gesture events** tell you a hand just *did* something to a worn item: an undress armed or finished,
a plug worked in or out, a device or piece of gear put on. They are `PPB_GestureUndressArm`,
`PPB_GestureUndressEnd`, `PPB_GesturePlug`, `PPB_GestureDeviceEquipped`, `PPB_GestureGearEquipped`
and `PPB_GestureClaim`. Field layouts are documented at the top of [`src/PpbTouchAPI.h`](src/PpbTouchAPI.h).
To stand the gestures down while your scene poses an actor's hands, call
`PPB_Native.SetGesturePaused(True)`, then `False` afterwards.

---

### Step 3 — Papyrus: reading every variable

A mod event carries one string and one number, so it cannot hand you depth, weapon class, or
distance *and* duration at once. The natives can. The script is `PPB_Touch`, already compiled and
shipped — **you do not need the source to call it**, and you do not need PPB present to compile.

Poll the live snapshot:

```papyrus
Int n = PPB_Touch.GetContactCount()
Int i = 0
While i < n
    Actor  who    = PPB_Touch.GetContactActor(i)
    String region = PPB_Touch.GetContactRegion(i)       ; "Face"
    String sub    = PPB_Touch.GetContactSubRegion(i)    ; "In mouth"
    Int    depth  = PPB_Touch.GetContactDepth(i)        ; 2
    Float  dur    = PPB_Touch.GetContactDuration(i)     ; seconds
    i += 1
EndWhile
```

Or bridge from inside an event handler — resolve the contact, then read whatever you want:

```papyrus
Event OnPpbTouchStart(String eventName, String strArg, Float numArg, Form sender)
    Int i = PPB_Touch.FindContact(sender as Actor, "")      ; "" = either hand
    If i >= 0
        If PPB_Touch.GetContactDepth(i) >= 2                ; 2 = unambiguously inside
            ; ... and duration, distance and weapon class are all available here too
        EndIf
    EndIf
EndEvent
```

**All 16 natives on script `PPB_Touch`:**

| Function | Returns | Example |
|---|---|---|
| `GetContactCount()` | `Int` | number of live contacts |
| `GetContactActor(i)` | `Actor` | the touched NPC |
| `GetContactWand(i)` | `String` | `"L"` / `"R"` |
| `GetContactSource(i)` | `String` | `"FINGER"`, `"WEAPON:Iron Rapier"` |
| `GetContactBodyPart(i)` | `String` | `"Face(cheek L)"` |
| `GetContactRegion(i)` | `String` | `"Face"` |
| `GetContactSubRegion(i)` | `String` | `"In mouth"` |
| `GetContactDepth(i)` | `Int` | `0` surface · `1` opening · `2` inside · `3` deepest |
| `GetContactDistance(i)` | `Float` | game units, negative = inside |
| `GetContactDuration(i)` | `Float` | seconds since the contact began |
| `GetContactSkeleton(i)` | `String` | `"human"` / `"argonian"` / `"khajiit"` / `"draenei"` |
| `GetContactWeaponClass(i)` | `String` | `"Sword"`, `"Mace"`… `""` if not a weapon |
| `GetContactWeaponEdge(i)` | `String` | `"Blade"` / `"Blunt"` / `"Pierce"` |
| `GetContactIsEngine(i)` | `Bool` | `True` = the game's own physics reported it |
| `GetContactPacked(i)` | `String` | the whole `"WAND\|SOURCE\|BODYPART\|SKELETON"` |
| `FindContact(akWho, asWand)` | `Int` | index for that actor, or `-1` |

> **Indices are not stable between polls.** The list is a snapshot refreshed at `apiHz` (ships at
> 4/s — a host knob, do not hard-code it). Read what you need in one pass. `FindContact` returns
> `-1` inside a `PPB_TouchEnd` handler, which is normal: the contact has already ended, so read on
> `Start` or use the event's own `numArg` for the final duration.

---

### Step 4 — C++: acquiring the interface

Copy **one file** into your project: [`src/PpbTouchAPI.h`](src/PpbTouchAPI.h). It is deliberately
self-contained — no CommonLib types, no SKSE types beyond the dispatch you already make, actors
addressed by FormID. It works from CommonLibSSE, CommonLibVR or classic skse64.

Same request/reply pattern HIGGS and PLANCK use. Any time at or after `kPostLoad`:

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

`GetApiFunction` null means PPB is not installed. `GetApiFunction(1)` returning null means PPB does
not speak revision 1. **Both are normal** — ship a fallback and never assume PPB is there.

---

### Step 5 — C++: receiving contacts

Register a callback:

```cpp
void OnTouch(const PPBAPI::PpbTouchContact* c, int phase) {
    if (phase != PPBAPI::kPhaseStart) return;

    logger::info("{:08X} {} / {} / {} for {:.2f}s at {:.2f}u",
                 c->actorFormId,
                 g_ppb->RegionName(c->region),          // "Face"
                 g_ppb->SubRegionName(c->subRegion),    // "In mouth"
                 c->bodyPart,                           // "Face(palate)"
                 c->durationS, c->distU);

    if (c->depth >= PPBAPI::kDepthInside)   { /* something went in           */ }
    if (c->sourceKind == PPBAPI::kSourceWeapon) {
        logger::info("  {} — {}", c->sourceName,        // "Iron Rapier"
                     g_ppb->WeaponClassName(c->weaponClass));
    }
}

g_ppb->AddTouchCallback(&OnTouch);
```

...or poll a snapshot whenever it suits you:

```cpp
PPBAPI::PpbTouchContact buf[16];
int n = g_ppb->GetContacts(buf, 16);
```

**Threading:** callbacks fire on the **main thread**, at most at `apiHz`. The contact pointer is
valid **only for the duration of the call** — copy what you need. Do not block.

---

### Step 6 — what a contact actually contains

`PpbTouchContact` is a frozen 160-byte POD. Every field, and its Papyrus equivalent:

| C++ field | Papyrus | Meaning |
|---|---|---|
| `actorFormId` | `GetContactActor` | the touched NPC |
| `toucherFormId` | — | `0x14` = the player (always, in revision 1) |
| `wand` | `GetContactWand` | `0` = right hand, `1` = left |
| `sourceKind` | `GetContactSource` | `kSourceFinger` / `Palm` / `Fist` / `Hand` / `Grab` / `Weapon` / `Object` / `Genital` / `Head` |
| `sourceName` | (in `SOURCE`) | weapon or object name; `"shaft"`/`"tip"` for genital; `"face"`/`"head"`/`"mouth"` for head; else `""` |
| `bodyPart` | `GetContactBodyPart` | the named capsule |
| `region` | `GetContactRegion` | `kRegionFace`, `kRegionIntimate`… |
| `subRegion` | `GetContactSubRegion` | `kSubFaceSurface`, `kSubInMouth`… |
| `depth` | `GetContactDepth` | `0`–`3`, the ladder below |
| `distU` | `GetContactDistance` | game units, negative = inside |
| `durationS` | `GetContactDuration` | seconds |
| `skeleton` | `GetContactSkeleton` | `"human"`… |
| `weaponClass` | `GetContactWeaponClass` | `kWeapSword`, `kWeapMace`… `0` if not a weapon |
| `weaponEdge` | `GetContactWeaponEdge` | `kEdgeBlade` / `kEdgeBlunt` / `kEdgePierce` |
| `engineContact` | `GetContactIsEngine` | `1` = Havok's own narrowphase reported it |
| `slot`, `child`, `leftTwin` | — | the exact capsule address, for `ReadCapsule` |

---

### The depth ladder — the part people get wrong

Sub-regions are ordered, and each level **overrides** the ones below it:

```
Face surface  <  Mouth opening  <  In mouth  <  Mouth wall
0 surface        1 opening         2 inside     3 deepest
```

A cheek is a cheek. Those capsules only mean "mouth" *in conjunction* — the gate needs the palate
and both cheeks at once. But a **palate** touch means something *is* inside, full stop, and
outranks any simultaneous lip or cheek reading. The **throat wall** outranks even that.

The intimate chain has the same shape (`opening → deep → deepest`), which is why `Intimate` is its
own region and not part of `Pelvis`: *"touched her hip"* and *"inserted"* are categorically
different events and must never collapse into one.

**If you only care how far in something got, read `depth` and ignore the names entirely.** That is
what the number is for, and it keeps working when new sub-regions are appended.

---

### Weapons come from the game's own physics

A weapon's shape defeats geometric approximation — a rapier's swept hilt, an axe head and a club
all measure differently, and many modded weapons carry no bounding data at all. So PPB does not
approximate. It reads **Havok's own contact events**, which carry both colliding bodies and the
exact capsule that touched. The same event that vibrates your controller and pushes her body is
the one your mod receives.

Check `engineContact` (`GetContactIsEngine`) if you care: `1` = engine truth, `0` = PPB's geometric
fallback, which deliberately includes hover and near-misses. Only test it if you specifically want
to exclude hover.

---

### Coverage — read this before you ship

**PPB does not drive every NPC.** As of 2.0 it covers **both sexes** on mapped races: the human
catch-alls (which cover elves, orcs and most custom races), Argonian, Khajiit, Draenei (female),
plus anything the user adds to `PPB_Skeletons_Added_Race.ini`. **Children and creatures are never
reported**, nor is anyone on an unmapped custom skeleton.

⚠ Two things changed for consumers written against 1.x: the old "males always answer
`IsDriven() == false`" rule is **gone** — gate any male fallback on `GetBuildNumber() >= 20000`
rather than on sex — and **part names are sex- and skeleton-routed**, so read the name strings
instead of assuming the female reference map.

```cpp
if (!g_ppb || !g_ppb->IsDriven(formId)) {
    // not covered — use your fallback (e.g. CBPC), do not assume silence means "no touch"
}
```

Silence from this API means *"PPB is not driving her"* just as often as it means *"nothing touched
her"*. Always have a fallback path.

---

### Debugging your integration

Set `contactLog = 1` in `SKSE/Plugins/PPB_Skeletons_Added_Race.ini`. Every contact is then written
to `My Games/Skyrim VR/SKSE/PPB.log`:

```
API START 1301DE4F R|FINGER|Face(cheek L)|human d=-0.31u dur=0.00s src=GEO curl=9.1u
API END   1301DE4F R|WEAPON:Iron Rapier|Neck(neck / throat)|human d=-0.71u dur=2.41s src=ENG wpn=Sword/Blade
```

`src=ENG` is the game's own physics; `src=GEO` is PPB's proximity measure. This is the fastest way
to see whether PPB saw the touch at all — which settles "is my handler broken, or was nothing
sent?" in one line. Verbose by nature, so turn it back off for normal play.

---

**→ [INTEGRATION.md](INTEGRATION.md) — the full guide**: the raw stream, dwell filtering, the
versioning contract, and every gotcha we hit building it. `docs/PPB_Touch_API_Contact_List.xlsx`
lists all 107 capsules with their group, depth and override behaviour.

---

## Push and shove

Put a hand, a weapon or a held object against an NPC and she reacts like a body.

| Outcome | What you do | What she does |
|---|---|---|
| **Push walk** | press steadily | steps back, faster as you press harder, stopping as the pressure goes |
| **Push stumble** | push firmly enough to bend her back | the vanilla stagger, turned to where the push acts on her |
| **Shove knockdown** | shove hard | ragdolls from where your hand is, then gets up |
| **Leg sweep knockdown** | lift **both** her feet off the floor with a hand or weapon | ragdolls |

### How PPB reads a push

Not from your hand. Your hand's speed says nothing about how much force reached her. PPB reads the
NPC instead.

For each trunk joint (COM, Spine0, Spine1, Spine2, Neck, Head, and each thigh at the hip) it
compares the **Havok body** with where the **current animation** wants that joint this frame. A body
driven away from its animation is being pushed.

Every joint lags its animation a little even when nobody is near. That lag grows toward the head and
is real PD-servo lag, so each actor's resting lag is learned and **frozen the moment a probe comes
within reach**. A baseline must never absorb a push.

Once she moves, the ladder is measured as **travel**: how far each joint has been driven beyond what
her own walk carried it since the push began. A push that walks her back 5 u has not come 5 u closer
to a knockdown. Shipped bars:

| | walk | stumble | knockdown |
|---|---|---|---|
| COM, spine, thighs | 10 u | 20 u | 35 u |
| Neck, head | 10 u | 30 u | 40 u |

The head and neck get higher bars because they swing furthest for the least effort. On the shared
ladder the head decided 18 of 20 reactions.

### The details that make it feel right

- **The walk** is the engine's own movement planner under direct control, not a scripted translate.
  PPB also overrides her gait parameters, because the planner maps a speed onto the actor's *own*
  gait and otherwise treats the command as a suggestion.
- **The knockdown** starts through the engine's own knock (`AIProcess::KnockExplosion`), so PLANCK
  owns her ragdoll from the first frame. Across 38 recorded onsets the graph-driven path spiked body
  speed to ~7,000 u/s, which is the "launched to the ceiling" bug. The engine knock peaks around
  220 u/s.
- **The leg sweep** is measured from each NPC's own standing height, so heels are accounted for. One
  leg lifted with the other planted does not trip her. After she gets up, the floor re-arms within
  0.6 s even while you keep sweeping.
- **Equipping doesn't read as a push.** For 0.25 s after a hand equip or removal nothing reacts, and
  her height afterwards is the new floor. Putting heeled boots on her is not a lift.

### When she won't react

- **Fighting or in a kill move.** Not yours to interrupt.
- **Busy in furniture.** This is her *body's* sit state (sitting, getting up, sleeping, waking), not
  the furniture reservation. An NPC walking to a chair, or just leaving one, holds that reservation
  but is on her feet and fully pushable. Crafting stations and chores count as busy.
- **Leaning** on a wall, rail, bar or counter. She is braced: she **can't be pushed, but can still be
  swept**.
- **Idle animations that own her movement.** The engine refuses planner control there, so she can't
  be walked. She still stumbles or goes down in place.

Conversation is deliberately *not* a gate.

### Choosing what you want

Each outcome is a switch in `SKSE/Plugins/PPB.ini` under `[Features]` (`bPushShove`, `bPushWalk`,
`bPushStumble`, `bShoveRagdoll`, `bFeetLift`). The installer asks for each one, and every switch is
hot: save the file and it applies within a second. With the knockdown off and the stumble on, a hard
shove plays a stumble instead.

---

## Hand gestures — equip, undress, plugs

Three gestures, all built on PPB's contact stream, so they know exactly which body part you are at.

### Press to equip

Hold a piece of armour, clothing or a device against the matching body part for about a second,
then **let go**. The dwell arms the gesture; the release fires it. Placement is deliberately
forgiving: you bring a thing roughly where it goes, and the dwell plus the explicit release make it
intentional.

It works for **ordinary gear** and for **Devious Devices, ZaZ and Diary of Mine** devices. A device's
body site comes from its framework keywords: a gag wants the mouth, cuffs the wrists, a plug an
orifice, a piercing the nipple or clitoris. An optional `SKSE/Plugins/PPB_deviceSites.txt` can
override a device's site.

**Refusals instead of silent swaps.** The piece falls instead of going on when:

- the biped slot is already occupied (`bSlotOccupiedRefuse`), instead of vanilla's silent swap; or
- ordinary clothing covers the device's own region (`bClothingGate`), per region: gloves block
  wrist cuffs, a cuirass does not, and a plug under a dress is refused.

Framework devices never block each other; Devious Devices arbitrates its own layering.

### Two-hand undress

Grab the same worn piece with both hands and pull them apart. The trigger is the moment HIGGS's grab
snaps back to your controller, which is HIGGS breaking the hold at its stretch limit. That snap *is*
the gesture completing, and the piece lands in the pulling hand.

### Fingertip plug extraction

Hold a bare fingertip at an occupied orifice for a second and the plug works loose into your hand.

- **A worn chastity belt is the lock.** It is matched by Devious Devices class, not by slot 49, which
  belts share with corsets and underwear.
- **Devious Devices removal goes through DD's own unlock round trip.** PPB waits for DD to settle
  before handing the device to your hand. Dropping it on the unlock frame lets DD delete the
  inventory half, making the device vanish.
- **Quest and block-generic devices are refused** before anything is announced.

### What you put on stays on

Hand-equipped gear is meant to be permanent:

- **Ordinary gear** is equipped with the engine's prevent-removal flag, so outfit re-evaluation
  leaves it alone.
- **Generic NPCs** are held by a pool of optional quest aliases (`PPB_HoldPoolQuest`, 32 slots), so a
  cell respawn can't rebuild them from their record. Unique NPCs don't need it.
- **`OutfitGuard`** detours `Actor::HasOutfitItems`, so a pooled NPC you undressed is not re-issued her
  default outfit the next time her 3D loads. The detour reads the function prologue at runtime and
  installs only on a whitelisted instruction pattern; anything unexpected logs the bytes and
  installs nothing.
- **SeverActions followers** under an active outfit preset or lock get the change recorded into
  SeverActions, because its alias would otherwise strip the piece on the next load.

Gestures show no on-screen messages by default (`equipNotify` / `undressNotify` in
`PPB_tuning.txt`), and the whole layer is one switch: `bEquipGestures` in `PPB.ini`, read at load. Mod
authors: every gesture is published on the event bus described in the Touch API section above.

---

## Your head, and the kiss

**The player's head is a collider.** One keyframed box sized to a skull rides the headset, not the
head bone. VRIK builds your body from the HMD with fixed proportions, so when you stand taller than
the rig can reach the head *bone* tops out below your real eyes: at eye level sitting, chin level
standing. No fixed offset can correct a gap that depends on posture.

**It is a touch source,** `HEAD`, named by which part made contact:

- `face`: the front of the head
- `head`: anywhere else
- `mouth`: a small probe on the lower front of the face

A forehead on her shoulder, a cheek, a nuzzle into her neck all arrive as named contacts. A head
contact never reports an interior sub-region, and only the mouth can reach the intimate openings. The
head never pushes anyone.

**The kiss.** The mouth probe meeting her upper lip raises `PPB_MouthLips` with `"R|LIPS|HEAD"`,
start and end. It starts at 2.2 u and ends only past 3.2 u, so a real kiss holds steady instead of
flickering. A kiss is lips only; it can never open her mouth. The head box is removed during
OStim/SexLab scenes.

---

## Features

### Per-race Havok bodies

| Skeleton | Covers |
|---|---|
| Human female | human, elf, orc females |
| Argonian female | beast skeleton, own face sculpt |
| Khajiit female | beast skeleton, own face sculpt |
| Draenei female | custom horns and hooves (Yvanni) |
| Human male (2.0) | human, elf, orc males — own head layout and COM ladder |
| Argonian male (2.0) | snout, jaw, crest and horns |
| Khajiit male (2.0) | own head layout |

Any race can be mapped to any skeleton in `PPB_Skeletons_Added_Race.ini`.

### SMP tails, hair and clothing

Havok capsules are built to ride skeleton bones. SMP and HDT use their own bones, defined in XML — so
PPB attaches capsules to those instead.

The capsule tracks its SMP bone inertly until something that can move it makes contact (a HIGGS hand
or weapon), then pushes that bone's gravity toward its own displacement. Applied to **KS Hairdos SMP**
and **Vanilla Hair Remake SMP**; all HDT SMP tails; M'rissi's Fluffy tail.

**Both SMP engines are supported** (1.3.0) — Faster HDT-SMP *and* HDT-SMP Flex. They announce
different plugin-interface versions and rename physics bones differently (`hdtSSEPhysics_AutoRename_
Armor_XXXXXXXX <name>` vs `hdtA_XXXXXXXX_<name>`); PPB speaks both. On Flex before 1.3.0 no tail or
wig bound at all.

Which chords push is decided by **where your hand is**, not by their order in the table — so touching
the tip of a long tail or wig moves it, not just the root.

Live rigs are budgeted to the **nearest 2 NPCs within ~10 m** (`npcRigMaxActors` / `npcRigRangeU`);
a nearer NPC takes the slot from a farther one. A dense wig is 85–200 independent dynamic bodies
driven every frame, so this is the main performance dial.

### Proximity sensing ("orifices")

Sculpting the body capsule by capsule also allows real orifices.

- **The mouth** is built from lip, cheek, palate and throat capsules. The gate needs the palate and
  both cheeks at once, and the depth ladder runs from the lips to the throat. Wired to **MFG Fix** so
  the lips and jaw respond.
- **The vaginal and anal openings** are capsule ladders on the pelvis. The gate is an ellipse test
  on the capsule axes, so it survives ReScale, ReShape and body morphs.

**The orifice drive** opens the actual bone rings as something goes in, scaled by both depth and
girth, so a fingertip and a plug at the same depth differ. It uses the same bone rings and directions
as Penetration Physics (PPA), so both mods deform them alike. PPB arbitrates with PPA **by
measurement**: a bone still holding PPB's own
last write is PPB's, anything else is PPA animating and PPB stands down. When PPB stands down it
still restores exactly the bones it moved, so it can never leave a permanent gape.

All of it is published through the Touch API: mouth events, sub-regions and the depth number.

### Genital collision

A 4-capsule chain over the schlong bones, plus **GenBend**: touching drives erection up, holds, then
decays, negotiated with SPS's own arousal erections so whichever is higher wins. Requires TNG + SPS.
The rig exists only while the genitals are actually exposed.

**Futa (2.2):** females wearing a futa schlong (TNG Gentlewoman, TRX-ERF) get the same rig. The gate
is visible geometry on the genital chain, never slot 52, because a slot is only a claim.

## Fixes

### High heels
NPCs in heels never get their Havok body adjusted — every capsule sits below the body by the heel
offset. PPB tracks the offset and raises the body to match. It tracks the *fix*, not the shoes, so a
heel fix that fails to raise the NPC doesn't desync anything.

### Clavicle follow
XP32 gave the shoulder forward/back movement. The Havok shoulder joint never got it, so any animation
moving the shoulder left that arm's collision behind. Each Havok shoulder now follows its XP32 node
live.

### Dismembering Framework
DF works in VR, but one NPC dismembering another causes a **hard freeze**. It is a thread deadlock,
not a DF bug: DF cuts on a worker thread that needs the SKSE task queue, while the main thread holds
that same lock inside the death-frame gear-attach and skinning pass (FSMP in the chain). Neither
proceeds. **This predates PPB.**

PPB disables DF's threaded hit processing, making the deadlock structurally impossible, then restores
the death confirmation DF loses by watching hit and death events itself and calling **DF's own public
API** on the main thread. DF keeps all of its conditions, chances, sounds and limb physics — PPB only
decides *when* to ask.

The optional FOMOD step installs **stock DF settings with exactly one value changed**:

```ini
bDeferredHitProcess = 0     ; DF default: 1
```

If you have tuned DF yourself, copy that single line into your own ini instead of installing the
override. **If you use DF and apply neither, set `dgDeathCut 0` in `PPB_tuning.txt`** — otherwise DF's
threaded path and PPB's death-confirmed request can both fire and take two limbs.

### Next-Gen Decapitations
NGD's severed head is a **full actor clone** of the victim — same race, same base — not a head prop.
That means PLANCK will happily drive it like a living body, and PPB's per-NPC systems would try to
rescale and reshape it.

PPB detects the clone through NGD's own API (`IsHead`) and excludes it from **both** PPB and PLANCK, so
nothing drives it, reshapes it, or builds rigs on it. That is the whole of the integration — no files
are overwritten and no NGD settings are changed.

**Known limitation:** the severed head still is not a grabbable physics prop. It can be shoved with a
weapon, and HIGGS reports grabbing it, but it will not lift. Eliminated as causes: SMP hair, mass,
inertia, friction, motion type, collision layer and filter sub-layer, the skeleton it loads, the
orbiting satellite bodies, and PLANCK management. The head is created by the **vanilla engine**
(`Actor::Decapitate`), not by NGD, so the clone is engine behaviour rather than something NGD chose.
Unresolved — see roadmap.

### HIGGS fingers
PPB shortens HIGGS's hand slab to roughly half its length so it reads as a palm, and adds four boxes:
two narrow ones on the index finger, two larger spanning middle/ring/pinky. They follow your fingers
and close into a fist. They ride the **third-person finger bones** — the hands you actually see —
rather than the raw controller pose.

HIGGS plays a finger-*close* animation whenever your hand nears a grabbable object, which curls the
hand and defeats those colliders — the most common "poking does nothing" report. Since 1.3.0 PPB
turns that off through **HIGGS's own settings API**, so your `higgs_vr.ini` is never modified and
every other HIGGS setting you tuned is left alone. Knob `higgsPokeFix` if you would rather it didn't.

### Collision sound
Because Havok was only ever meant for knockdowns, a capsule striking *anything* plays "body hitting
ground" — the log-tumbling-down-stairs noise from an NPC caught on furniture. Set to silent.

## Install

**Requirements**

- [SKSE VR](https://skse.silverlock.org/)
- [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101)
- [Skyrim VR ESL Support](https://www.nexusmods.com/skyrimspecialedition/mods/106712)
- [PLANCK](https://www.nexusmods.com/skyrimspecialedition/mods/66025)
- [HIGGS VR](https://www.nexusmods.com/skyrimspecialedition/mods/43930)
- [XP32 Maximum Skeleton](https://www.nexusmods.com/skyrimspecialedition/mods/1988)
- [A CBBE-based body](https://www.nexusmods.com/skyrimspecialedition/mods/30174) (3BA, SoftBody, custom NPC bodies, OBody)

**Optional:** [OBody NG](https://www.nexusmods.com/skyrimspecialedition/mods/77016) · [FSMP](https://www.nexusmods.com/skyrimspecialedition/mods/57339) / [SMP Flex](https://www.nexusmods.com/skyrimspecialedition/mods/101564) · [Dismembering Framework](https://www.nexusmods.com/skyrimspecialedition/mods/126203) · [Heels Fix](https://www.nexusmods.com/skyrimspecialedition/mods/64442)
· SMP hair and tail mods

**Optional, for specific features:**
- **Genital collision:** TNG + SPS, or TNG Gentlewoman / TRX-ERF for futa.
- **Orifice drive:** Penetration Physics (PPA) bone rings.
- **Device gestures:** Devious Devices, ZaZ Animation Pack, Diary of Mine.
- **Outfit handoff for followers:** SeverActions.
- **Scene gating:** OStim / SexLab.
- **Lips react to the mouth sensor:** MFG Fix.

Install with a mod manager. Keep it above other skeleton mods.

### Installer choices

| Page | Choice |
|---|---|
| Performance | SMP hair and tail collision on, or off for weaker machines (`npcFollower`) |
| Optional Features | Push and shove on/off · Equip gestures on/off |
| Push and Shove Options | shown when push is on: Push walk · Push stumble · Shove knockdown · Leg sweep knockdown, each on/off |
| Compatibility Patch | the Dismembering Framework freeze fix |

Every choice is a line in a config file, so you can change your mind later without reinstalling.

## Configuration

| File | Purpose |
|---|---|
| `PPB_Skeletons_Added_Race.ini` | maps races to skeletons — **removing it disables the mod** |
| `PPB_skeletons.txt` | runtime skeleton map (read once at load) |
| `PPB_tuning.txt` | live-polled tuning knobs |
| `PPB.ini` | general settings, hot-reloaded: `[Equip]` refusal rules and the `[Features]` switches |

`PPB.log` (in `My Games/Skyrim VR/SKSE/`) prints the effective feature state at startup on the
`PPB FEATURES:` lines. When something doesn't react, check those first.

## Compatibility

**There should be no conflicts.**

Because ReScale and ReShape work at runtime, another mod overriding the same `.nif` doesn't matter —
PPB uses the one in its own folder and applies it to that NPC live. The heel fix, clavicle follow and
several other fixes work the same way.

Everything is gated behind plain config files, so bodies can be added or removed freely. A missing or
malformed file is a no-op.

- **Gift by Hand** also equips items by hand. If you prefer it, install PPB with *Equip gestures off*
  so the two don't both act on the same item.
- **Physical Collision VR (PCVR) 4.x:** no conflict. Both use HIGGS's collision layer and coexist.
- **Follower Bump Guard** and **VRTouchEvents** read PPB's player head box; it is harmless without them.

## Status and roadmap

**Beta** — not because it doesn't work, but because of its size and how deeply it reaches into the
engine. No mod has changed the Havok bodies to this extent: hundreds of child capsules reshaped and
repositioned live, XP32 tracking, capsules bound to SMP bones, silenced impacts, UV-mapped anatomical
landmarks across wildly different body shapes, and capsules used as both collision and proximity
sensors.

Please report issues with logs.

**Built on PPB:** **VRTouchEvents** turns PPB's contacts into SkyrimNet events, so NPCs react in
conversation to where and how they are touched. Its DD-ZaZ AddOn narrates the device gestures.

**Planned**

- Selective scene collision: ignore actor-vs-actor inside a scene while keeping the player's hands live
- A positional hand stop, so your hand rests on skin instead of passing into it
- On demand: a tool for adding collision capsules to any SMP object

Identifying **which object was pressed against an NPC's skin** already works, by reading what the
HIGGS hand is holding. Since 2.2 the gestures use it.

A lot of this was built with AI assistance — understanding deep engine mechanisms and searching
thousands of installed mods for conflicts, work that would otherwise have taken weeks.

## Credits

PPB is a layer on top of other people's work. Every one of these is load-bearing:

- **FlyingParticle** — **PLANCK**, **HIGGS** and **Collision Visualizer VR**. PPB is built directly on
  their plugin APIs, and the visualizer is how every capsule in this mod was placed by eye. None of
  this exists without them.
- **Groovtama** — **XP32 Maximum Skeleton Special Extended** (and xp32 for the original skeleton).
  XP32 is the rig PPB's joints track; the whole premise of this mod is "make the physics follow that
  skeleton".
- **Caliente and ousnius** — **CBBE** and BodySlide. The body standard ReShape measures against; the
  UV landmark system works because CBBE-based meshes share a layout.
- **The SKSE team** — **SKSE VR**. No script extender, no plugin, no mod.
- **alandtse** — **VR Address Library for SKSEVR**. PPB binds engine functions by address; this is
  what makes that possible without hardcoding offsets per game version.

Also relied on for optional features: **FSMP / Faster HDT-SMP** (the SMP bones the hair and tail
capsules ride), **SKEE / RaceMenu** and **OBody NG** (the morph data ReShape reads), and **MFG Fix**
(the facial reaction used by the mouth sensor).

