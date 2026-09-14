# Precision Physic Bodies

**Skyrim VR NPCs with real bodies: collision that fits their actual shape, so you can touch them, push
them, dress and undress them with your hands, and they react like people.**

> *To quote a pioneer of VR modding: "This mod is complicated."*

![status](https://img.shields.io/badge/status-beta-orange)
![platform](https://img.shields.io/badge/platform-Skyrim%20VR-blue)
![plugin](https://img.shields.io/badge/SKSE-VR-green)
![version](https://img.shields.io/badge/version-2.2.0-blue)

**Latest: 2.2.0**
- **Push and shove.** Press an NPC and she steps back; push harder and she stumbles; shove and she goes
  down. Sweep her legs and she falls. Each one is a separate install choice.
- **Dress and undress NPCs with your hands.** Hold clothing, armour or a device against the right part of
  her body to put it on; grab a worn piece with both hands and pull it off; work a plug out with a
  fingertip. What you put on stays on.
- **Your head is part of your body.** A kiss on the lips is a real, detected event.
- **Children and mannequins are excluded** from everything, whatever load order you run.
- Futa support, and a full event feed for mods like VRTouchEvents that narrate what you do.

Changes in detail: [CHANGELOG.md](CHANGELOG.md). Mod authors: [INTEGRATION.md](INTEGRATION.md).

---

## Contents

- [What it does](#what-it-does)
- [Touch](#touch)
- [Push and shove](#push-and-shove)
- [Dressing and undressing NPCs by hand](#dressing-and-undressing-npcs-by-hand)
- [Your head, and the kiss](#your-head-and-the-kiss)
- [Fixes](#fixes)
- [Install](#install)
- [Settings](#settings)
- [Compatibility](#compatibility)
- [For mod authors](#for-mod-authors)
- [Status](#status)
- [Credits](#credits)

---

## What it does

Every NPC in Skyrim has an invisible **physics body**: a set of collision shapes and joints that the
game only ever used for ragdolls. It is crude, 19 balls that stick out well past the skin, made in 2011
for a game you never touched anyone in. Then VR arrived with VRIK, HIGGS and PLANCK, and suddenly you
*can* touch, grab and shove NPCs. That old body is why an NPC launches across the room when she snags a
chair, and why your hand passes through her chest but stops a foot in front of her shoulder.

PPB rebuilds that body. Each NPC gets about **130 collision capsules** instead of 19, hand-placed to
follow the real shape: fingers, forearms, breasts, belly, thighs, calves, feet, and a full face with
lips, cheeks and jaw. The joints follow the XP32 skeleton exactly, including the shoulders.

**It fits every NPC automatically.** NPCs come in every height, race scale, body preset and slider
setting, so PPB measures each one live:

- **Size** is read from the NPC's own spine, not from the race record (which is often wrong).
- **Shape** is read from the body mesh itself: PPB looks up fixed points on the skin texture map, finds
  where those points actually sit on this NPC, and moves the capsules to match. Weight, OBody presets,
  custom follower bodies and RaceMenu morphs all come out right. Breasts are measured the same way.

It works on any XP32 skeleton with any CBBE-based body (3BA, SoftBody, custom NPCs, OBody), for
**both sexes**: four female skeletons (human, Argonian, Khajiit, Draenei) and three male ones (human,
Argonian, Khajiit), each with its own face and head layout.

## Touch

Because the capsules match the skin, PPB knows exactly **where** you are touching an NPC, **with what**,
and **how long**:

- **Your hands.** Fingertips, palm and fist are told apart. HIGGS's grab hand counts too.
- **Weapons and held objects.** A sword against her neck, an apple pressed to her cheek. Weapon
  contact comes from the game's own physics, so it is exact.
- **Your head** (see [below](#your-head-and-the-kiss)).
- **Your genitals**, with TNG + SPS: touching drives erection up, and it decays afterwards. Futa
  followers (TNG Gentlewoman, TRX-ERF) get the same.

**Hair and tails.** With Faster HDT-SMP or HDT-SMP Flex, SMP wigs and tails get collision too: your
hand moves the hair. Only the two nearest NPCs carry live hair rigs, because a dense wig is up to 200
physics bodies. That is the main performance dial (`npcFollower`).

**The mouth and the intimate openings** are real openings. PPB senses how deep a finger or a plug is
and opens the mouth (with MFG Fix) or the body (with Penetration Physics' bone rings) to match. A
fingertip and a plug at the same depth open differently.

## Push and shove

Put a hand, a weapon or a held object against an NPC and she reacts like a body:

| What you do | What she does |
|---|---|
| **Press steadily** | steps back, faster as you press harder, and stops when you stop |
| **Push firmly** enough to bend her back | stumbles, turned to face where the push came from |
| **Shove hard** | goes down (ragdoll), then gets back up on her own |
| **Sweep both her feet** off the floor, with a hand or a weapon | falls, like anyone would |

**How PPB knows she is being pushed:** not from your hand. It watches her own joints. When her body is
driven away from where her animation wants it, that is a push, and how far it is driven decides walk,
stumble or knockdown. So a palm, a sword and a held crate all push the same way, and a hand resting on
her does nothing. One leg lifted while the other stays planted does not trip her.

**Who is left alone:**
- NPCs who are fighting or in a kill move.
- NPCs busy in furniture: sitting, sleeping, working at a crafting station, doing a chore. Walking to a
  chair or just leaving one is not "busy".
- NPCs **leaning** on a wall, rail, bar or counter are braced: they can't be pushed, but their legs can
  still be swept.
- Children and mannequins, always.

Talking to someone is deliberately **not** a reason to leave them alone.

**Each outcome is a switch.** The installer asks for push walk, push stumble, shove knockdown and leg
sweep separately, and each is a line in `SKSE/Plugins/PPB.ini` you can flip mid-game.

## Dressing and undressing NPCs by hand

Three gestures. All of them use PPB's own touch sensing, so they know exactly which body part you are at.

**Put something on her.** Hold a piece of clothing, armour or a device against the matching part of her
body for about a second, then **let go**. Boots go at the feet, a collar at the neck, a gag at the mouth,
cuffs at the wrists. Placement is forgiving: bring the item roughly where it goes; the second of holding
and the deliberate release are what make it intentional. It works for ordinary gear and for **Devious
Devices, ZaZ and Diary of Mine** devices, which go on through their own frameworks.

**Take something off her.** Grab the worn piece with both hands and pull them apart. The moment HIGGS's
hold breaks and your hand snaps back to the controller, the piece comes off, and it lands in that hand.
Locked Devious Devices need the key, as they would from the menu.

**Plugs.** Hold a bare fingertip at an occupied opening for a second and the plug works loose into your
hand. A worn chastity belt locks it in.

**When it refuses, the item just falls:**
- the slot is already taken (she keeps what she has on; nothing is silently swapped);
- ordinary clothing covers the spot (a plug under a dress, a collar under a scarf);
- you held it against the wrong body part.

**What you put on stays on.** Skyrim normally re-dresses an NPC from her outfit record every time she
reloads, and resets generic NPCs completely when a cell respawns. PPB stops both:

1. The piece is equipped with the game's own **"do not remove"** flag, so the outfit refresh leaves it.
2. Generic NPCs are made **persistent**: PPB holds them in a quest, so a cell respawn can't rebuild them
   from their record.
3. The game's **"she has nothing of her outfit on, re-dress her"** check is answered *she's fine* for
   those NPCs, so an NPC you undressed stays undressed.

Followers managed by SeverActions get the change recorded into SeverActions instead, so its outfit
presets don't fight you.

No messages pop up on screen for any of this. The whole feature is one installer choice
(`bEquipGestures`). If Gift by Hand handles equipping for you, turn it off here.

## Your head, and the kiss

Your head is a collider too. It rides your **headset**, not VRIK's head bone, because VRIK builds your
body from the headset with fixed proportions: stand taller than your character and your head bone stops
at chin height while your real eyes are above it. The headset is always right.

Leaning your forehead on her shoulder, a cheek against hers, a nuzzle into her neck: all of those are
real, named contacts. And **a kiss** is a kiss: your mouth meeting her upper lip is its own event, held
steady for as long as you hold it. Your head never pushes anyone.

## Fixes

- **High heels.** NPCs in heels had their whole physics body sitting below them by the heel height. PPB
  raises it to match.
- **Shoulders.** XP32 gave the shoulders forward/back movement; the physics joint never followed. Now it
  does.
- **Dismembering Framework freeze.** One NPC dismembering another could hard-freeze Skyrim VR: a thread
  deadlock in DF, older than PPB. The optional installer patch turns DF's threaded hit processing off,
  and PPB supplies the death confirmation DF loses that way. DF keeps all its own settings, sounds and
  physics. If you use DF and skip the patch, set `dgDeathCut 0` in `PPB_tuning.txt` or you may get
  doubled cuts.
- **Next-Gen Decapitations.** The severed head is really a full clone of the victim, which PLANCK would
  drive like a living body. PPB excludes it from both mods.
- **HIGGS fingers.** PPB adds finger and palm colliders that follow the hands you see, and turns off the
  HIGGS finger-close animation that used to curl your hand before a poke landed. Your `higgs_vr.ini` is
  never touched.
- **Collision sound.** A body touching anything used to play the "body hits ground" thud. Silenced.

## Install

**Requirements:** [SKSE VR](https://skse.silverlock.org/) ·
[VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101) ·
[Skyrim VR ESL Support](https://www.nexusmods.com/skyrimspecialedition/mods/106712) ·
[PLANCK](https://www.nexusmods.com/skyrimspecialedition/mods/66025) ·
[HIGGS VR](https://www.nexusmods.com/skyrimspecialedition/mods/43930) ·
[XP32 Maximum Skeleton](https://www.nexusmods.com/skyrimspecialedition/mods/1988) ·
[a CBBE-based body](https://www.nexusmods.com/skyrimspecialedition/mods/30174)

**Optional, per feature:**
- Hair and tails: [FSMP](https://www.nexusmods.com/skyrimspecialedition/mods/57339) or
  [SMP Flex](https://www.nexusmods.com/skyrimspecialedition/mods/101564), plus SMP hair/tail mods
- Body shape from presets: [OBody NG](https://www.nexusmods.com/skyrimspecialedition/mods/77016)
- Genitals: TNG + SPS (or TNG Gentlewoman / TRX-ERF for futa)
- Openings: Penetration Physics · MFG Fix
- Devices: Devious Devices, ZaZ Animation Pack, Diary of Mine
- Follower outfits: SeverActions
- Scenes: OStim / SexLab (bodies follow the animation exactly during a scene)
- [Dismembering Framework](https://www.nexusmods.com/skyrimspecialedition/mods/126203) ·
  [Heels Fix](https://www.nexusmods.com/skyrimspecialedition/mods/64442)

Install with a mod manager, above other skeleton mods. The installer asks:

| Page | Choice |
|---|---|
| Performance | SMP hair and tail collision on, or off for a weaker machine |
| Optional Features | Push and shove on/off · Equip gestures on/off |
| Push and Shove Options | Push walk · Push stumble · Shove knockdown · Leg sweep knockdown, each on/off |
| Compatibility Patch | the Dismembering Framework freeze fix |

Every choice is a line in a settings file, so you can change your mind without reinstalling.

## Settings

| File (in `SKSE/Plugins/`) | What it holds |
|---|---|
| `PPB.ini` | the feature switches from the installer, and the equip refusal rules. Applies within a second. |
| `PPB_tuning.txt` | every dial: push distances, hair rig budget, timings. Applies within a second. |
| `PPB_Skeletons_Added_Race.ini` | which races get a PPB body. Removing it turns the mod off. |
| `PPB_skeletons.txt` | the skeleton map, read once at load |

`PPB.log` (in `My Games/Skyrim VR/SKSE/`) prints the active switches at startup on its `PPB FEATURES:`
lines. When something doesn't react, look there first.

## Compatibility

There should be no conflicts. PPB fits its bodies at runtime, so another mod overriding the same
skeleton file doesn't matter. Everything is gated by plain config files, and a missing file is a no-op.

- **Gift by Hand:** both equip by hand. Install PPB with *Equip gestures off* if you prefer Gift by Hand.
- **Physical Collision VR:** no conflict; they coexist.
- **OStim / SexLab:** during a scene every PPB body follows the animation exactly and can't be shoved,
  so scene alignment is untouched. Touch still works in a scene.
- **Children and mannequins** never get a PPB body, are never pushed, and are never gesture targets.
- **Follower Bump Guard** and **VRTouchEvents** read PPB's head and touch data; harmless without them.

## For mod authors

PPB publishes everything it senses: every touch (who, where, with what, how deep, how long), every
push outcome, every equip, undress and refusal, the kiss, and an early "this press may become a push"
signal, as SKSE mod events and a C++ interface. VRTouchEvents
uses it to narrate all of this to SkyrimNet.

Everything you need is in **[INTEGRATION.md](INTEGRATION.md)**: the Papyrus and C++ examples, every
event and field, the versioning rules, and how to see exactly what PPB sends.
[`src/PpbTouchAPI.h`](src/PpbTouchAPI.h) is the contract itself, and
`PPB_Touch_API_Contact_List.xlsx` lists all 107 named capsules.

## Status

**Beta**: not because it doesn't work, but because of how deep it reaches into the engine. Nothing
else changes the physics bodies to this extent. Please report issues with `PPB.log`.

Known limitation: the Next-Gen Decapitations severed head still can't be picked up. It is created by
the vanilla engine, not NGD; unresolved.

A lot of this was built with AI assistance, for understanding deep engine mechanisms and checking
thousands of installed mods for conflicts.

## Credits

PPB is a layer on top of other people's work:

- **FlyingParticle**: PLANCK, HIGGS and Collision Visualizer VR. PPB is built directly on their plugin
  APIs, and every capsule was placed by eye with the visualizer.
- **Groovtama** (and xp32): XP32 Maximum Skeleton. The rig PPB's joints follow.
- **Caliente and ousnius**: CBBE and BodySlide. The body standard PPB measures against.
- **The SKSE team** (SKSE VR) and **alandtse** (VR Address Library).
- Also relied on: FSMP / Faster HDT-SMP, SKEE / RaceMenu, OBody NG, MFG Fix, TNG, SPS, Penetration
  Physics.
