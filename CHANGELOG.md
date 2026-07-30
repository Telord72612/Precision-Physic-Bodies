# Changelog

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
