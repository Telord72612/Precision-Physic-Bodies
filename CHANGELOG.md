# Changelog

## 1.4.1

### Per-NPC head fitting finally works (the random-skull bug)

Reported as NPCs spawning with visibly wrong Havok head sizes — one skull ~150%. Three stacked
defects, all fixed:

1. **Any mesh could compete for the face landmarks.** The nose/chin UV walk admitted every
   readable mesh in the actor's tree; on some outfits a boots/body vertex pair won and hit the
   1.6x clamp. Candidates must now be **skinned to the head bone**.
2. **The real head was locked out.** The facegen path gated on the trishape's vertex count, which
   the engine reports wrongly (tens of thousands for a 3,832-vert head); the partition's count is
   the true one. Verified by dissecting facegen NIFs on disk: parts are small (mouth 141, eyes
   176, head 996–8464), and the old ≥500 floor excluded them all — so head fitting had silently
   never worked and the walk fell through to the clothing lottery above.
3. **Custom head parts could read a landmark from outside the buffer.** Head parts carrying a
   skin vertex map (some custom NPCs) could map one landmark index past the position block —
   heap garbage, a 116u "face", and a correct head thrown away by the sanity gate. Maps are now
   fully validated with an in-envelope fallback.

Verified in VR across mapped and unmapped head parts: three NPCs, three genuinely different
measured faces (x0.888 / x1.011 / x1.029), zero garbage candidates.

### Dismembered victims no longer hang in the air

The permanent PLANCK-ignore that protects severed-head props was applied to every DF/NGD-touched
actor — including real victims, whose only restore path was gated on a flag that never cleared.
A decapitated draugr could float forever. Victims now stay under PLANCK (the state the game is in
without PPB installed); only genuine head-clone props keep the permanent ignore.
`dgVictimPlanck` (ships 1) reverts to the old behaviour at 0.

### The synchronized "spasm" near two NPCs

Standing near two roughly equidistant NPCs made both twitch in lockstep ~1/s. The mouth-gate's
nearest-actor tracker flapped between them every second, and its phoneme writer dirtied the
tracked actor's facegen 90x/s even at a constant zero. Tracking is now sticky (a challenger must
be 20% closer) and phonemes write only on change.

### OStim / SexLab scenes

Hand colliders now suspend during scenes (`sceneSuspendHands`, ships at mode **2**: suspended in
third person, restored in first person so precise touch is kept where hands are
controller-tracked). First/third detection measures HMD-to-head distance — the engine's
`IsInFirstPerson()` never changes in VR. Addresses the scene-collision weirdness reports.

### Combat stretching

`pivGuardCombatLoose` (ships 1): PLANCK's constraint loosening — whose purpose is letting the
ragdoll MATCH the animation pose — is allowed on PPB actors while they are in combat, so extreme
combat animations no longer read as a stretched body. Capsule fit is intentionally traded away
only for the duration of combat.

### Also fixed

- HIGGS's hand collision shape was rewritten every frame; now once per body (hygiene).
- A re-scale sequence abandoned by walking out of range now re-arms on approach instead of
  waiting for a ragdoll rebuild that may never come (NPCs stuck with a low skull).
- Every silent exit on the re-scale path now logs a receipt.

### Hair coverage

+28 touch rigs: Valkyr HDT-SMP Hairstyles (all 6), Tullius SMP Hair (14), KS Hairdos SMP for Men
(7), and KS Lavender (wrongly skipped by the original census). 267 rigs total. The generator is
now maintained in-repo.

### Compatibility

- **Mu Joint Fix**: its FixHeight/AutoHighheel writes the same skeleton node offset PPB's heel
  handling reads — heel-offset errors on heeled NPCs. Disable `[FixHeight]` in MuJointFix.ini
  when running both.
- Note for scene mods: PPB touch capsules and OStim VR coexist as of this release; OStimVR's own
  `DisablePLANCKduringScenes` (its ini, defaults 0) covers the PLANCK half and is worth enabling.


## 1.4.0

### Touch API: sub-regions and a depth ladder

The touch API reported a coarse **region** (`Face`) and the exact **capsule** (`palate`). It now
also reports a **sub-region** in between (`In mouth`), plus a **depth** rung — and the sub-region
carries the override semantics that were previously only implicit.

Within the mouth and the intimate chain, sub-regions are ordered shallow to deep and each level
overrides the ones below it:

```
Face surface  <  Mouth opening  <  In mouth  <  Mouth wall
0 surface        1 opening         2 inside     3 deepest
```

The correction that motivated this: a **cheek or a chin is an ordinary face touch**. Those capsules
participate in mouth detection, but only *in conjunction* — the gate needs the palate and both
cheeks at once. A **palate** touch means something IS inside, and outranks any lip or cheek
reading. The **throat wall** outranks even the palate. The intimate chain follows the same shape.

**Additive only — nothing moved.** The interface is still revision 1: three methods appended at
the end of the vtable (`SubRegionOf`, `SubRegionName`, `SubRegionDepth`), three bytes taken from
`PpbTouchContact`'s reserved tail (`region`, `subRegion`, `depth`), and three Papyrus natives
added (`GetContactRegion`, `GetContactSubRegion`, `GetContactDepth`). A plugin built against the
previous header still runs unchanged.

The mod-event string is **deliberately unchanged** at four `|` fields, because consumers are told
they may split on the first three. `apiSubRegionInEvent` (ships **off**) appends the sub-region as
a fifth field for authors who opt in.

### Tail contacts were always three segments, not one

`tail (base)` / `tail (mid)` / `tail (tip)`, computed as equal thirds of the chord chain — so
"tip" means the same place on a 4-chord foxtail and a 14-chord fluffy tail. This shipped with the
tail work; only the documentation was wrong.

### New: developer integration guide

`INTEGRATION.md` — how to consume the API from Papyrus (events or natives) or from an SKSE
plugin, with the coverage contract, the dwell gates that explain "why am I seeing nothing", and
the gotchas. Alongside it, `docs/PPB_Touch_API_Contact_List.xlsx`: all 107 capsules with their
region, sub-region, depth, dwell and override behaviour, generated from the source tables and
verified against the shipped DLL.

### Fixes

- The public header and `PPB_Touch.psc` both documented `apiHz` as defaulting to 20/s; it ships
  at 4/s. Corrected — a wrong number in a public contract is one modders build against.
- The published capsule record carried three invented knob prefixes (`capForearm`, `capUpperarm`,
  `capSpine`); the real keys are `capFore`, `capUpper`, `capSpine0`. Anyone following the record
  to tune a forearm capsule was editing a key the plugin never reads. Corrected, and the
  generator now verifies every cited knob against the shipped tuning file.

### Post-review fixes (from live VR verification, 2026-07-31)

- **Grab mutes the weapon** vs the grabbed actor — a hand holding an NPC's limb no longer
  reports phantom weapon contacts on her from the weapon riding that grip.
- **Blade radius capped** (`apiWeaponRMaxU 6`) — broad weapons carry the blade *plane's*
  breadth as their bound radius (an axe reads 23u); uncapped this read deep phantom contacts
  from mere proximity. Every weapon still self-describes (collision shape → form bound →
  grip point); the cap only ceilings the radius.
- **Throat wall requires the palate as witness** — the priority promotion let an *outside*
  neck press claim `Face(throat wall)`; it now only wins while the probe is also within
  deep-palate range (the mouth gate's own inside/outside asymmetry).
- **Digest exit grace** (2 ticks) — kills the End/Start ping-pong at region boundaries that
  one-tick dwell exposed.
- **Held-hand index exception** — a holding hand's palm/fist stay muted (grip noise) but its
  index reports while VRIK reads it extended, so a weapon hand can still poke and two-hand
  interactions stay two streams. `apiSuppressHeldHand 2` = strict full mute.
- **Keep-last-nonempty source names** — End events no longer print `OBJECT:` with no name on
  the release tick.
- **Mouth gate events** — `PPB_MouthLips` / `PPB_MouthEnter` / `PPB_MouthThroat`,
  edge-triggered, the authoritative "in her mouth" signal.
- Plugin version now reports 1.4.0 (was stale at 0.1.0 since the first build);
  `GetBuildNumber` → 10400.

### ReShape never fitted head capsules on most NPCs (2026-08-01)

Head collision was silently stuck on the generic baked face for a large share of NPCs, so
reaching for someone's face could miss it by a couple of units.

The head is the one capsule slot whose children are **baked, with no knobs** — the ReShape head
ratio is their only driver. That ratio returns exactly `1.0` for an actor who has not been
measured, and the apply is gated on `ratio != 1.0`. So an unmeasured NPC does not get a *rough*
head fit; she gets **no head write at all**, with nothing logged, because nothing failed.

What kept her unmeasured was the gate in front of the sampler, which used `Actor::IsDead()`.
That predicate is **true for `kRestrained`-class states**, so live NPCs read as corpses and were
refused — intermittently, since the state comes and goes, which is why this looked like "ReShape
randomly stops working" rather than a gate on a predicate that lies. In one session it reported
every actor present as dead and latched nothing at all for four minutes.

Now uses `GetLifeState()` (`kDead`/`kDying`), which is what the finger-rig code already used after
hitting the identical trap. The crash protection that motivated the original check is unchanged:
real corpses still match, and dismembered actors were always caught by the dismember guard, which
was the actual crash vector.

### Internal (2026-08-01)

- **`ReadJointWorlds` null-guards its out-params consistently.** Three of five were guarded and two
  were written unconditionally, which is invisible at the call site; a new caller passing `nullptr`
  for one of the unguarded pair took an access violation. All are guarded now.
- **Receipts for silent stalls.** A re-scale sequence could arm and then sit indefinitely without
  logging — three early returns on that path emitted nothing, and one stall lasted 3m40s. The grab
  gate now says so.
- **The heel receipt no longer asserts what it has not checked.** It claimed "the node offset
  persists" from *above* the line that reads the offset, so it printed that every 10 s while the
  offset was zero and no bias was armed. It now reports the measured value and distinguishes
  "bridging a Heels Fix refresh" from "Heels Fix dropped the offset and did not re-add".

## 1.3.0

### HDT-SMP Flex support

PPB previously refused any SMP engine announcing plugin interface 1.x — which is every HDT-SMP
Flex build. Two independent problems had to be fixed:

**The plugin link.** Interface 1 and 2 are identical in everything PPB touches (same
`PluginInterface` vtable, same event structs, same Bullet 3.24) *except* the listener base class:
v1 uses `hdt::IEventListener<T>` with `onEvent` at vtable slot 0 and no virtual destructor, while
v2 uses `RE::BSTEventSink<T>` where slot 0 is the destructor. Handing a v1 engine a v2-shaped
listener would make it call PPB's destructor once per physics step, so the version gate was
correct and stays — 1.3.0 adds a properly v1-shaped listener behind it instead of loosening it.
Knob `fsmpFlexCompat` (default on).

**Bone names.** The two engines rename armour-carried physics bones differently:

| engine | renamed bone |
|---|---|
| Faster HDT-SMP | `hdtSSEPhysics_AutoRename_Armor_XXXXXXXX <orig>` (space) |
| HDT-SMP Flex | `hdtA_XXXXXXXX_<orig>` (underscore; `hdtH_` for head parts) |

PPB matched only the space form, so on Flex **every** tail and wig table silently failed to
resolve and no rig was ever created. Both schemes are now handled; Faster HDT-SMP behaviour is
unchanged.

### Push follows your hand, not the table order

The chords allowed to push SMP bones were the first *N* in table order — which is authored
root-first. On a 200-chord wig that meant chords 14–199 could never push, wherever you touched.
The same *N* chords are now selected by proximity to your hand. The count is unchanged, so the
cost is unchanged.

### Garment-rig budget

`npcRigRangeU` (700 u ≈ 10 m), `npcRigRangeHystU` (100), `npcRigMaxActors` (2). Only the nearest
*N* NPCs carry hair/tail rigs; a nearer NPC evicts a farther one. There was previously **no
distance limit at all** — any ragdoll-driven actor could take a slot, and a dense wig is 85–200
independent dynamic Havok bodies, never sleeping, keyframed every frame on the main thread.

### HIGGS poke fix

HIGGS plays a finger-close animation near any grabbable, curling the hand and defeating PPB's
finger colliders — the most common "poking does nothing" report. PPB now sets HIGGS's
`SelectedCloseFingerAnimMaxHandSpeed` to `-1` through **HIGGS's own settings API**: your
`higgs_vr.ini` is never modified and every other HIGGS setting you tuned is untouched. Re-applied
after each game load, because HIGGS re-reads its ini. Knob `higgsPokeFix` (default on).

### Capsules and the touch API

15 new collision capsules (upper/lower back, 11 internal pelvic sensors), bringing the named set
to **107**. The full map ships as [`docs/capsule_api_names.md`](docs/capsule_api_names.md) and
[`docs/capsule_body_part_map.json`](docs/capsule_body_part_map.json) so other mods can resolve a
contact to a body part. Both are generated from the plugin's own table and verified against the
shipped binary, so they cannot drift from the code.

### Tail + wig on the same NPC: both now push

The push targets travel through a per-actor staging buffer that held 14 slots. M'rissi's foxtail
is the one table that legitimately claims all 14 by itself — so with a covered wig on the same
actor, the wig collided but never pushed and her hair sat dead. The stage is now 28 slots
(a max-sensor tail plus a max-sensor wig, both in full), and the three previously hand-
synchronized copies of that size are now one shared constant (`FsmpLink::kMaxPushTargets`) so
they can never drift again. Per-rig publish counts are unchanged.

### FOMOD: hair and tail rigs are now an install-time choice

A new first installer page, "Performance", offers SMP hair & tail collision **On (recommended)**
or **Off**. Off installs a tuning file generated from the canonical one at package time,
differing by exactly `npcFollower 0` — for machines without the headroom. Reversible any time by
editing that one line (live, no restart).

### Fixes

- **Runaway finger colliders.** They could follow a stale HIGGS hand anchor away from the hand —
  measured at 272 u (~3.9 m) in normal play and 7112 u (~100 m) at a world load. A velocity clamp
  cannot catch this, because a body that tracked the wrong target *perfectly* has zero velocity;
  the fix is a position leash against the skeleton hand node plus a snap-home recovery.
  Knob `handBoxLeashU` (30).
- **Finger-rig churn.** The rig was destroyed and rebuilt on ~1 % hand-scale jitter — 60 rebuilds
  in 25 minutes in one log. Each rebuild starts collision-off for 100 ms, so touches were being
  silently eaten. Now zero in a full session.
- **`handBoxRebuildFrac 0` meant 2 %**, i.e. *more* aggressive than the default, instead of "off".
- **Log spam.** `touchProbe` and `npcFingerLog` both shipped enabled. The latter logs one line per
  chord per second, which an 85-chord wig turned into 57 lines/sec and a 954 KB log in 9 minutes.
  Both now ship off.
- **Removed two dead SMP config files.** PPB shipped `configs.xml` (a pre-4.0 format neither engine
  reads) and `userConfigs.json` (Faster HDT-SMP's *user* settings file). On a fresh install the
  latter would have silently overridden the user's skeleton budget, first-person physics and
  culling distance.

### Internal

Build-time guard against a generated hair table exceeding `kMaxChords` — the largest sits exactly
on the limit, so a regeneration was one table away from overflowing every per-rig array.

### Verification status

Honest reporting, because not all of this got equal testing:

| item | status |
|---|---|
| SMP Flex link + bone names | **verified in VR** — tails and hair physics confirmed working on Flex |
| Runaway colliders, rig churn | **verified** from before/after session logs |
| Garment-rig budget | **log-confirmed** (correct eviction, no thrash), not judged by feel |
| Contact-ranked push | **partly verified** — multi-bone chains respond; no deliberate tip-vs-root A/B yet |
| HIGGS poke fix | **built, not yet exercised in VR** |

A performance drop was reported during Flex testing and is **not yet isolated**. Current evidence
points at the SMP engine's own preset rather than PPB — Flex was running 20 simultaneous actors
with 1323 collision objects while PPB's budget held it to 2 rigs — but PPB's own contribution has
not been measured. If you see it, `npcFollower 0` disables every PPB garment rig instantly and
tells you which side it came from.

---

## 1.1

PivGuard, pose-conform gate, capsule body-part map.
