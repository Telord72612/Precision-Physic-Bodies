# Precision-Physic-Bodies

**A complete rebuild of Skyrim VR's NPC Havok bodies — joints that track the XP32 skeleton, capsules
that fit the actual body mesh, both refitted live at runtime.**

> *To quote a pioneer of VR modding: "This mod is complicated."*

![status](https://img.shields.io/badge/status-beta-orange)
![platform](https://img.shields.io/badge/platform-Skyrim%20VR-blue)
![plugin](https://img.shields.io/badge/SKSE-VR-green)

---

## Contents

- [The problem](#the-problem)
- [What PPB does](#what-ppb-does)
- [ReScale — fitting the joints](#rescale--fitting-the-joints)
- [ReShape — fitting the capsules](#reshape--fitting-the-capsules)
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

Capped at the **8 nearest NPCs** with live rigs, following the player.

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

