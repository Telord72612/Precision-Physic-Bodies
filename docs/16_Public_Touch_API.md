# 16 — The Public Touch API (as built, 2026-07-30)

**Status: BUILT AND COMPILED, ⚠ NOT YET RUN IN GAME.** The DLL is staged (game was open at
build time); first in-game verification is pending. `apiLog 1` is set in the dev tuning file so
the first session produces `API START/END` receipt lines; the packager refuses to ship it on.

## What it is

The five-variable contact report the user specified, exposed three ways:

| variable | delivery |
|---|---|
| **WHO** (touched NPC) | event `sender` = the Actor; native `actorFormId`; Papyrus `GetContactActor(i)` |
| **WHERE** (capsule) | the 107-name map via `NpcFinger::PartName`, side-prefixed on limb slots; unnamed children fall back to `<slot>.C<n>` — never silently dropped |
| **BY WHO** | the player, always, in revision 1 (`toucherFormId = 0x14`); NPC touchers are the planned rev-2 pillar |
| **WITH WHAT** | `FINGER / PALM / FIST / HAND / GRAB / WEAPON:<name> / OBJECT:<name>` |
| **DURATION** | seconds since contact began; `PPB_TouchEnd`'s `numArg` IS the final duration |

**Channels:** ① mod events `PPB_TouchStart` / `PPB_Touch` (continuous at `apiHz`) / `PPB_TouchEnd`,
`strArg = "WAND|SOURCE|BODYPART|SKELETON"`; ② Papyrus polling natives, class `PPB_Touch` (9
functions, snapshot-backed); ③ the native interface `PPBAPI::IPpbTouchInterface1` acquired via
the HIGGS request/reply pattern (`PpbMessage::kGetTouchInterface = 'PPBT'` dispatched to sender
`"PPB"`). The consumer contract lives in **`src/PpbTouchAPI.h`** — a deliberately self-contained
header (FormIDs only, no CommonLib types) with the versioning rules written into it: append-only
vtable, **no virtual destructor** (slot 0 is `GetBuildNumber`, forever — the HDT-SMP v1/v2
listener lesson), and a frozen 160-byte `PpbTouchContact` POD with a reserved tail.

## How it works (files)

* `src/PpbApi.cpp` — the engine. Roster of driven actors filled by `NpcFinger::OnPreDrive`
  (`NoteDriven`, same-frame-only pointers — the `g_gt.best` pattern), consumed by `PpbApi::OnFrame`
  on the HIGGS PostVrikPostHiggs callback, throttled to `apiHz` (default 20).
* Probes per tick: the 4 HandBox boxes per hand (tips for the index pair, centres for slab/plate),
  the HIGGS weapon body (`NpcFinger::WeaponPointU`, new export), and the HIGGS-held object
  (worldBound centre + radius; a grabbed *Actor* is never an OBJECT — it becomes GRAB).
* Targets: 12 slots × sides × live children via `GrabDiag::ReadCapsuleWorldUSide`. Detection =
  point-to-segment minus radius. **Two-level culling**: actor gate (any probe within 160 u of the
  actor), then slot gate (child 0 within 60 u stands proxy), then children.
* **Contact identity = (actor, wand, source class)** — the capsule updates live while the hand
  slides, the contact and its duration survive the slide. Mark-sweep per tick; hysteresis
  (`apiTouchU` enter, `+apiExitPadU` exit).
* Classifier: GRAB if HIGGS holds *this* actor with that hand; FIST if the index-distal tip sits
  within `apiFistTipPalmU` of the palm plate; FINGER if an index box made the nearest contact;
  PALM if the plate did; else HAND.
* `SkeletonOf` mirrors PPBHook's `oursPPB` idiom (female + race skeleton path contains `\PPB\`),
  then classifies khajiit / draenei / argonian(beast) / human. `DismemberGuard::IsExcluded`
  actors answer not-driven — the doc-13 exclusion contract.
* Snapshot: 4-deep rotation (TargetBuf pattern); natives read it from VM threads lock-free.
* Teardown: `PpbApi::ClearOnLoad()` at kPreLoadGame — contacts never survive a load, no phantom
  End events.

## Self-touch is structurally impossible (the user's worry)

An NPC's own hair/tail capsules can never trigger her own body: garment rigs are **never probe
sources** — only the player's hands, weapon and held object are. Not a threshold; a property of
the design. (Corollary: garment capsules are also not *targets* in rev 1 — touching her wig
reports nothing. Candidate for rev 2 alongside NPC touchers.)

## Knobs

`apiTouch 1 · apiHz 20 · apiTouchU 1.0 · apiExitPadU 0.75 · apiMaxActors 3 · apiRangeU 300 ·
apiFistTipPalmU 7 · apiEvents 1 · apiLog 0` — all live (PK_NOSNAP).

## Verification plan (first session)

1. `API: touch interface handed to ...` only appears if some plugin asks — absence is normal.
2. Touch Lydia with a fingertip → `API START <id> R|FINGER|<part>|human d=…` then, on release,
   `API END … dur=…s`. Slide across her arm → part changes in the continuous stream, duration keeps
   counting.
3. Fist, open palm, grab her, poke with a sword, press a held apple against her — each should
   reclassify. FIST threshold (`apiFistTipPalmU 7`) is a guess pending VR feel.
4. `cgf "PPB_Touch.GetContactCount"` in the console while touching → ≥ 1.

## Known limits (rev 1, all deliberate)

Player-as-toucher only · body capsules only (no garment targets) · weapon contact uses HIGGS's
weapon *body* which sits at the hilt (blade-tip contact reads coarse) · `PPB_Touch.psc` compiled
with Caprica (needs `--flags tools/Caprica/TESV_Papyrus_Flags.flg` — the vanilla flg is not in
the VR install's loose files).
