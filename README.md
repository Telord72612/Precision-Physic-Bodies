# Precision-Physic-Bodies

**A complete rebuild of Skyrim VR's NPC Havok bodies — joints that track the XP32 skeleton, capsules
that fit the actual body mesh, both refitted live at runtime.**

> *To quote a pioneer of VR modding: "This mod is complicated."*

![status](https://img.shields.io/badge/status-beta-orange)
![platform](https://img.shields.io/badge/platform-Skyrim%20VR-blue)
![plugin](https://img.shields.io/badge/SKSE-VR-green)
![version](https://img.shields.io/badge/version-1.4.0-blue)

**Latest: 1.4.0 — the Touch API.** PPB now tells other mods *who* was touched, *where*, *with what*,
*how deep* and *for how long*. Weapon contacts come from Havok's own narrowphase, so the reported
capsule is the one the engine actually collided with. See **[INTEGRATION.md](INTEGRATION.md)** to
consume it, and [CHANGELOG.md](CHANGELOG.md) for the full list.

---

## Contents

- [The problem](#the-problem)
- [What PPB does](#what-ppb-does)
- [ReScale — fitting the joints](#rescale--fitting-the-joints)
- [ReShape — fitting the capsules](#reshape--fitting-the-capsules)
- [The Touch API — a tutorial for mod authors](#the-touch-api--a-tutorial-for-mod-authors)
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
> OBody, arbitrary morphs. *(Female only for now.)*

## The Touch API — a tutorial for mod authors

**PPB knows exactly where you are touching an NPC. Since 1.4.0 it will tell your mod.**

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
   | `f[1]` **SOURCE** | `"FINGER"` | `FINGER` / `PALM` / `FIST` / `HAND` / `GRAB` / `WEAPON:<name>` / `OBJECT:<name>` |
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
| `sourceKind` | `GetContactSource` | `kSourceFinger` / `Palm` / `Fist` / `Hand` / `Grab` / `Weapon` / `Object` |
| `sourceName` | (in `SOURCE`) | weapon or object name, else `""` |
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

**PPB does not drive every NPC.** It covers **female** NPCs of mapped races: the human catch-all
(which covers elves, orcs and most custom races), Argonian, Khajiit, Draenei, plus anything the
user adds to `PPB_Skeletons_Added_Race.ini`. Males, children and creatures are never reported.

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

## Features

### Per-race Havok bodies

| Skeleton | Covers |
|---|---|
| Human female | human, elf, orc females |
| Argonian | beast skeleton, own face sculpt |
| Khajiit | beast skeleton, own face sculpt |
| Draenei | custom horns and hooves (Yvanni) |

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

Sculpting the body capsule by capsule also allows real orifices. The **mouth** is built from chin, jaw
and palate capsules that detect insertion of another capsule (currently the index finger).

Havok capsules can report **proximity**, not just collision. Watching the distance across four mouth
capsules detects a fingertip at the lips and tracks it toward the palate — native engine touch
detection, and very cheap. Wired to **MFG Fix** so the lips and chin respond.

An API for other plugins to consume this is planned if there's demand.

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

Install with a mod manager. Keep it above other skeleton mods.

## Configuration

| File | Purpose |
|---|---|
| `PPB_Skeletons_Added_Race.ini` | maps races to skeletons — **removing it disables the mod** |
| `PPB_skeletons.txt` | runtime skeleton map (read once at load) |
| `PPB_tuning.txt` | live-polled tuning knobs |

## Compatibility

**There should be no conflicts.**

Because ReScale and ReShape work at runtime, another mod overriding the same `.nif` doesn't matter —
PPB uses the one in its own folder and applies it to that NPC live. The heel fix, clavicle follow and
several other fixes work the same way.

Everything is gated behind plain config files, so bodies can be added or removed freely. A missing or
malformed file is a no-op.

## Status and roadmap

**Beta** — not because it doesn't work, but because of its size and how deeply it reaches into the
engine. No mod has changed the Havok bodies to this extent: hundreds of child capsules reshaped and
repositioned live, XP32 tracking, capsules bound to SMP bones, silenced impacts, UV-mapped anatomical
landmarks across wildly different body shapes, and capsules used as both collision and proximity
sensors.

Please report issues with logs.

**Planned**

- All vanilla and common male races
- Companion mods built on PPB — one in progress with SkyrimNet using the proximity sensors
- On demand: a tool for adding collision capsules to any SMP object, and a C++ API exposing PPB's
  proximity events

Identifying **which object was pressed against an NPC's skin** already works, by reading what the
HIGGS hand is holding.

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

