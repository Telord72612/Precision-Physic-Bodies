# 16 — The Public Touch API (as built, 2026-07-30)

**Status: VERIFIED IN VR, 2026-07-30 — every source kind, from real receipts.** Four test
sessions same-day; the verified matrix and the fixes each session forced are in §Verification
at the bottom. `apiLog` stays 1 on the dev machine; the packager refuses to ship it on.

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
the design. (Tails ARE targets since same-day rev 1.1 — pseudo-slot 100. Hair stays out by
the same logic that protects self-touch: it drapes the face and would shadow head touches.)

## Knobs

`apiTouch 1 · apiHz 4 (user: "4hz is perfect") · apiTouchU 1.0 · apiExitPadU 0.75 ·
apiMaxActors 3 · apiRangeU 300 · apiFistTipPalmU 2 (measured; see fix ladder) · apiEvents 1 ·
apiLog 0 · apiHairTarget 0` — all live (PK_NOSNAP).

## Verification plan (first session)

1. `API: touch interface handed to ...` only appears if some plugin asks — absence is normal.
2. Touch Lydia with a fingertip → `API START <id> R|FINGER|<part>|human d=…` then, on release,
   `API END … dur=…s`. Slide across her arm → part changes in the continuous stream, duration keeps
   counting.
3. Fist, open palm, grab her, poke with a sword, press a held apple against her — each should
   reclassify. FIST threshold (`apiFistTipPalmU 7`) is a guess pending VR feel.
4. `cgf "PPB_Touch.GetContactCount"` in the console while touching → ≥ 1.

## Known limits (rev 1, all deliberate)

Player-as-toucher only (NPC touchers = rev 2) · TAIL chords are targets (pseudo-slot 100,
base/mid/tip); hair is declared but gated off (`apiHairTarget`) · weapon = form-bound blade
segment (see fix ladder below) · `PPB_Touch.psc` compiled with Caprica (needs
`--flags tools/Caprica/TESV_Papyrus_Flags.flg` — the vanilla flg is not in the VR install's
loose files).

---

## Verification (2026-07-30, four same-day VR sessions) — ALL PASSED

| item | receipt evidence |
|---|---|
| FINGER | `R\|FINGER\|CLITORIS d=0.70u dur=3.31s vrik=I1.00/M0.00` — the pointing pose from live VRIK state |
| PALM | `L\|PALM\|cheekbone R vrik=I1.00/M0.49` and tail strokes at `I1.00/M1.00` |
| FIST | `L\|FIST\|chin L vrik=I0.00/M0.00` |
| GRAB | `L\|GRAB\|upper glute R → BUTT CHEEK R dur=2.28s`; also her cranium and her hand (`L palm rod`) |
| WEAPON | `R\|WEAPON:Iron War Axe\|BREAST R → R upper arm d=-10.06u dur=6.36s` |
| OBJECT | `R\|OBJECT:Knife\|BUTT CHEEK L → R dur=3.07s`, an 11 s hold, `Apple Dumpling` in session 1 |
| tail | `PALM\|tail (tip) → tail (base)` both hands; axe slide `tail (base) → … → nose` dur=9.71s |
| multi-actor | Lydia + M'rissi contacts interleaved in one tick stream |
| slide identity | contact survives sliding; source reclassifies live (FIST→FINGER mid-touch as the hand opened) |
| duration | 0.25 s … 10.99 s, End events carrying the total |
| penetration | hover `+1.0u` through `-11.4u` (blade buried) |
| wands | L and R proven independently |

### What each session forced (the fix ladder)

1. **Session 1**: everything classified FIST — the curl heuristic's guessed threshold (7u) swallowed
   every gesture. GRAB/OBJECT/slide/duration all proven anyway.
2. **Session 2**: curl receipts revealed the values VARY (3.7–9.1u) — the "fingers don't
   articulate" diagnosis was WRONG, an artifact of one grip held through session 1. Box-led
   classification (index/plate/slab nearest) proved out live via the `apiFistTipPalmU 2` knob.
3. **Session 3**: VRIK layer live — `getFingerPos` (0=closed..1=open, the controller-driven
   state; vfunc 11 of the vendored `VrikInterface.h`) classifies first, geometry falls back.
   Values are binary-clean (`I1.00/M0.00`). HIGGS's own `GetFingerValues` was a dead end — it
   reports the GRAB-WRAP pose, only meaningful while holding.
4. **Session 4**: weapons + tail. The weapon needed THREE readers, each forced by evidence:
   the body position sits at the HILT (miss #1); the shape is a LIST whose children are
   `kConvexTransform` (12) wrappers with no CommonLibVR header (miss #2, census-logged); the
   shipped answer reads the equipped FORM's bound box (`TESBoundObject::boundData`, stable
   public CommonLib) — long axis = blade segment in the same model space the collision body
   transforms. A second weapon carried a plain `kBox` (4) and used the direct shape path.

### Known caveats (accepted, documented)

* Broad weapons read a broad probe radius (the axe bound's second extent = 23u) — contacts are
  generous for axes/hammers. If it feels too eager, cap the radius; swords are slim.
* Hand and held-object are SEPARATE simultaneous contacts from one wand (per-class identity) —
  a feature: "pressed the knife in" vs "pressed my knuckles in" are distinguishable.
* Hover starts: `apiTouchU 1.0` registers ~1 cm near-misses as brief contacts (`back of head
  d=0.70u` while passing). Deliberate — proximity is a first-class output; consumers wanting
  presses only should filter `distU < 0` or the host can lower `apiTouchU`.
* M'rissi reports `skeleton=human` — correct: her race is repointed to the human PPB skeleton;
  the foxtail is an equipped rig, not skeleton anatomy.
* Hair as a TARGET ships OFF (`apiHairTarget 0`): hair drapes the face, wins the nearest-surface
  race against cheeks, and would shadow face touch. `kSlotHair` is declared in the public header
  with a consumers-may-never-see-it warning.
