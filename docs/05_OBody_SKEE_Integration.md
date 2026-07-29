# 23 — OBody / SKEE Integration & the Slider-Morph Fallback

**The durable "how to read an NPC's body morphs" reference + the slider-morph layer that is now
ReShape's FALLBACK (Report 21 §8).** Split out of the old Report 21 so the integration surfaces
stay findable. The measured mesh system (Report 21) is the primary path; this layer runs only when
the body mesh is unreadable.

---

## 1. SKEE handshake — reading body morphs (`Interop.cpp AcquireSkee`)

At kPostPostLoad / kDataLoaded: check `skeevr.dll` (VR) or `skee64.dll` →
`Dispatch(0x9E3779B9, &msg, sizeof(ptr), "skee")` → `interfaceMap->QueryInterface("BodyMorph")` →
`IBodyMorphInterface*` (version 4). Live-verified: `PPB SKEE interface=OK version=4`.

- **⚠ VR vtable trap:** RaceMenu VR lacks the final `ClearMorphCache()` slot that SE added WITHOUT a
  version bump — **NEVER call `ClearMorphCache` on VR** (vtable misalignment → crash). All other
  methods are safe. (Also in KNOWLEDGEBASE.)
- **Read the NAKED shape:** `GetMorph(actor, slider, "OBody")`. OBody writes the naked preset under
  key `"OBody"`, the clothed ORefit push-up under `"OClothe"`. `GetBodyMorphs` SUMS all keys →
  would read a dressed NPC's clothed shape. Morphs appear a beat after 3D load → gate on `HasMorphs`
  / settled ticks.
- **Enumerate unmapped sliders:** `VisitMorphValues` reports every `"OBody"`-keyed slider with
  |v|>0.05; the latch logs any that no region table knows (`BodyScale <id> UNMAPPED OBody sliders:`)
  so a blind preset is a copy-paste table extension, never a mystery.

## 2. The 7 regions & slider vocabulary (`Interop.cpp kRegionAdd / kRegionInv`)

Each region sums several slider NAMES into a signed net (antonyms like "Small"/"Slim" subtracted).
Region → capsule slot: chest→spine2 · breasts→spine2 C11/C12 · **belly→spine1** · **waist→spine0**
· butt→COM cheeks · thighs→thigh · arms→upperarm. (Note the spine0/spine1 wiring: **spine0 sits
UNDER spine1**, so spine0=WAIST, spine1=BELLY — corrected 2026-07-18.)

Vocabulary harvested from all 117 installed SliderPresets XMLs by frequency — the modern 3BA volume
set (`BreastsNewSH` ×194 presets, `BreastsGone/Small2`, `BreastFlatness`, `ButtClassic/RoundAss/
AppleCheeks/MuscleButt/ButtPressed_v2`, `ThighOutside/InsideThicc_v2`, `MuscleLegs`, `Hips`,
`TummyTuck/HipNarrow_v2/WideWaistLine`, `ChestDepth`, `LegsThin`). Shape/position-only sliders
(Height/Gravity/Perkiness/Nipple*) are deliberately EXCLUDED — they move flesh, not volume.

- **Sign rule for new sliders:** a slider whose POSITIVE value means SMALLER goes in the INVERTED
  (subtract) table; one that stores negative-for-smaller works in the ADDITIVE table as-is.
- **★ Belly sign fix (2026-07-16):** in 3BA POSITIVE `Belly` = BIGGER belly. It was in the inverted
  table (belief "negative = flatter") → big-bellied presets read thinner. `Belly` is ADDITIVE now.
- Reading a problem preset's XML (`.../BodySlide/SliderPresets/*.xml`, big/small interpolated by
  actor WEIGHT) is THE method for closing a mapping gap.

## 3. The slider fallback response (Report 21 §8)

When ReShape has no readable mesh, this layer drives the capsules from the slider net. It is
**decoupled** from the measured gains — a FIXED conservative response: radius factor 0.12 (× the
shrink-side `bodyScaleLoBoost`), nets clamped ±1.5; slider translations bounded (breasts ±1.8, butt
±0.6). Measured gains (0.5–1.0) applied to slider-sized nets (±2) would double a capsule.

**COM/butt cheeks are structural everywhere** (Report 21 §5): only children 16/17 take the butt
region; the orifice ring never scales.

---

## 4. OBody NG integration surfaces (OBody NG 4.4.3, open source github.com/Aietos/OBody-NG + PDB)

1. **Native C++ API** (best for auto re-fit): copy `include/API/API.h`; at kPostPostLoad
   `Dispatch(0xc0B0D9cc, &req, sizeof(req), "OBody")` → `IPluginInterface` (use only after
   `OBodyIsReady()`). Register an `IActorChangeEventListener` — `OnActorGenerated(actor, preset)`,
   `OnActorPresetChangedWithoutGeneration`, `OnActorMorphsCleared` — to invalidate one actor's
   latch. Also `GetPresetAssignedToActor / AssignPresetToActor / ActorIsProcessed/Blacklisted`.
2. **SKSE ModEvent `"Obody_ApplyMorph"`** (sender = the Actor) fires whenever morphs are ACTUALLY
   applied — the cleanest "re-fit now" moment (`OnActorGenerated` can fire while morphs are still
   queued under OBody's detached-thread apply). **PPB uses this**: a `ModCallbackEvent` sink queues
   the FormID (VM thread) → main-thread drain → erase latch → freshLatch re-dress. Manual dial
   pulses are obsolete for preset changes.
3. **Papyrus `OBodyNative.psc`** — the test harness: `GetAllPossiblePresets()`,
   `AssignPresetToActor(npc, name, true)`, `ApplyPresetByName`, `GetPresetAssignedToActor`; the
   in-game **O hotkey** opens the preset picker on the crosshair NPC.

How OBody works: sinks `TESInitScriptEvent` (3D loaded + ActorTypeNPC + !child) and `TESEquipEvent`;
markers are themselves morphs (processed = distributionKey under "OBody"; blacklist =
"obody_blacklisted"); slider values interpolate by actor WEIGHT between the preset's min/max.

## 5. ★ SPAWN-BASE LESSON — the test NPC must have a NAME

OBody's only target resolution is `Game.GetCurrentCrosshairRef()` **with a silent fallback to the
PLAYER** (`OBodyNGScript.psc TargetOrPlayer`). A base record with **no FULL/Name never builds a
crosshair ref** — so chargen face-preset NPCs (`KhajiitFemalePreset01` etc.) can NEVER be O-keyed,
and every press silently edits the PLAYER instead. The MCM "Reset Actor" has the same fallback in
menu mode → wipes the PLAYER's preset. The O-key is also dead while the console is open.

**Correct spawn bases = the named `TreasCorpseCommoner<Race>Female` family** (Skyrim.esm; named,
non-unique, real FaceGen via Modpocalypse; spawn clothed → `removeallitems`):

| race | command | rendered scale |
|---|---|---|
| Khajiit  | `player.placeatme 000457FA 1` | 0.889 |
| Argonian | `player.placeatme 000457FB 1` | 1.000 |
| High Elf | `player.placeatme 000457F9 1` | 1.056 |
| Orc      | `player.placeatme 00045806 1` | 1.028 |
| Imperial | `player.placeatme 00045802 1` | 0.931 |
| **Redguard** | `player.placeatme 00048117 1` | **1.000** — the neutral-capture body |

Race height ≠ 1 is FINE for measured mode (mesh space is scale-free). Only the visual differs.

## 6. Body-mesh inventory (pynifly-measured — informs the fingerprint table, Report 21 §4)

| body | main shape / verts | notes |
|---|---|---|
| CBBE base | 'CBBE' / **13554** | 1 shape |
| 3BA base | '3BA' / **18436** (+ vagina 1905, anus 201) | mid-torso built +15-19% vs Softbody |
| Softbody (generic build) | 'Softbody' / **15460** (+ collision proxies) | the body most NPCs wear; the main shape carries full breast/butt geometry |

All three occupy the SAME coordinate space (z 11–114) — a breast-tip vertex is at the same position
on any of them; that + density-immune girth is why one measurement works across body types. Custom
followers built in BodySlide inherit one of these topologies → fingerprint to the right family.
