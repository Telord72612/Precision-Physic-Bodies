# PLANCK internals — the reference PPB should have had from day one

Source read 2026-08-03 from `tools/_research/planck` (it was already in our tree; PPB had been
reasoning about PLANCK from its *settings interface* alone). Same author as HIGGS (adamhynek);
the DLL is `activeragdoll.dll`. Line numbers are from `src/main.cpp` unless noted.

Everything below is **read from source**, not inferred.

---

## 1. `AddIgnoredActor` — what it actually does

`pluginapi.h:49-50` holds `ignoredActors` (an `unordered_set<Actor*>` + mutex). It is consumed in
exactly one place:

```cpp
bool IsAddableToWorld(Actor *actor)          // main.cpp:2924
{
    ... excludeRaces check ...
    { std::scoped_lock lock(g_interface001.ignoredActorsLock);
      if (g_interface001.ignoredActors.count(actor)) return false; }
    if (IsTemporaryIgnoredActor(actor)) return false;
    ... requires an anim graph WITH a ragdollDriver AND a ragdoll instance ...
}
```

**An ignore on an ALREADY-ACTIVE actor cleanly removes it** — this was an open question during the
floating-draugr work and the answer is unambiguous (`main.cpp:4316-4322`):

```cpp
if (!isAddableToWorld) {
    if (isActiveActor) {
        // Someone in range went from having a ragdoll or not being excluded, to ... being excluded
        RemoveRagdollFromWorld(actor);
        CleanupActiveGroupTracking(collisionGroup);
        isActiveActor = false;
    }
    if (collisionGroup != 0) g_hittableCharControllerGroups.insert(collisionGroup);
}
```

So PPB's `PlanckIgnore()` = "PLANCK removes this actor's ragdoll from its world and hands it back to
vanilla corpse physics", and the actor stays hittable via its charcontroller. **The DismemberGuard
victim fix is therefore sound by construction**, not just empirically.

Add/remove is also rate-limited: `minFramesBetweenActorAdds` gates `AddRagdollToWorld`.

---

## 2. PLANCK's OWN temporary ignore — and why it matters for OStim

`main.cpp:648-673`. A second, time-boxed exclusion set that PPB did not know existed:

```cpp
void AddTemporaryIgnoredActor(Actor *actor, double duration)
{ g_temporaryIgnoredActors[actor] = g_currentFrameTime + duration; }
```

Two triggers, both interesting:

| trigger | site | duration (config.h) |
|---|---|---|
| ragdoll warped too far (`rootOffset > maxAllowedDistBeforeWarp`) | `main.cpp:4936` | `warpDisableActorTime = 3.0` |
| **`Actor::SetPosition` called on the actor** | `main.cpp:5577` | `disableActorOnSetPositionTime = 3.0` **+ jitter up to 2.0** |

★ **The SetPosition hook is de-facto scene gating.** OStim and SexLab reposition their actors
constantly to keep alignment — every one of those calls drops that actor out of PLANCK for 3–5
seconds. So during a scene the participants are, in practice, **repeatedly un-managed by PLANCK**:
their ragdolls get `RemoveRagdollFromWorld`'d and re-added in a cycle driven by how often the scene
re-aligns them.

That has a direct consequence for the OStim-weirdness report: whatever misbehaves during a scene,
**PLANCK is mostly not the thing driving those bodies**. Any collision response the user sees is
either the engine's own ragdoll, the charcontroller, or something else pushing.

---

## 3. `loosenRagdollConstraintPivots` — the setting PPB fights

Lives in `PreDriveToPoseHook` (`main.cpp:4455`), i.e. **every drive of every managed actor** — it is
NOT combat-gated, NOT distance-gated, NOT state-gated. Two knobs:

- `loosenRagdollContraintsToMatchPose = true` (outer)
- `loosenRagdollConstraintPivots = true` (the pivot half PPB overrides)

The sequence per drive (`main.cpp:4585-4665`):

1. Map the high-res anim pose to low-res ragdoll bone world transforms.
2. **Overwrite every ragdoll rigid-body transform with the anim-pose transform**, saving the old ones.
3. Save each ragdoll constraint's limits *and pivots* into `originalConstraintPivots`.
4. Loosen limits (`hkpEaseConstraintsAction`) and **move the pivots** so the constraints can reach
   the anim pose.
5. Next drive: `hkpEaseConstraintsAction_restoreConstraints(...)`, then **write the saved pivots
   back**.

★ **Step 5 is why PPB's re-scale corrections revert.** PLANCK restores the pivots it captured at
step 3 — which are whatever was there *before* our write. Any PPB pivot edit made while the ease
action is live is overwritten wholesale on the next drive. This is the mechanism behind the
"correction does not survive" observation in the ReScale work, and it is also exactly what PivGuard
v2 was built to defeat.

★ **The loosen exists so the ragdoll CAN MATCH THE ANIM POSE.** That reframes PPB's scoped-0:
running a PPB-skeleton actor at `loosenRagdollConstraintPivots = 0` means the ragdoll is
**forbidden from reaching poses its baked joints cannot express**. For ordinary movement that is
what we want (our joints are correct and should not be collapsed). For **extreme animations** —
combat, staggers, killmoves, scene animations — it means the ragdoll cannot follow, and the visible
result is a body that looks stretched or wrongly-jointed. That matches the user's combat report,
and makes "loose only during combat" a well-motivated request rather than a guess.

---

## 4. HIGGS scene gating: there is none

For completeness, from `tools/_research/higgs/src/hand.cpp`:

```cpp
// hand collision, :658
bool shouldDisableCollision = state == State::HeldBody || IsTwoHanding() || GetOtherHand().IsTwoHanding();
// weapon collision, :863
bool shouldDisableCollision = !player->actorState.IsWeaponDrawn() || IsWeaponCollisionDisabled(isLeft);
```

No OStim, SexLab, scene, furniture or paired-animation check exists anywhere in HIGGS. PPB has none
either. **Nobody in the stack suppresses hand collision during an intimate scene** — so if PPB's
hand boxes disturb one, there is no existing mechanism that would have prevented it.

---

## 5. Consequences for PPB (design notes, not yet implemented)

1. **Scene suspension must be ours.** Neither HIGGS nor PLANCK will do it. The cheapest correct
   hook is a `ModCallbackEvent` sink for OStim's `ostim_start` / `ostim_end` (and the SexLab
   equivalents) — PPB already has that exact pattern in `main.cpp:473` (`ObodyEventSink` listening
   for `Obody_ApplyMorph`). OStim also exposes `OThread.IsRunning()` / `OThread.GetActors()` as
   Papyrus natives for a cross-check.
2. **Combat-gated loosen is coherent** now that we know the loosen's purpose. PivGuard already
   brackets it per-actor per-drive; adding "not while the actor is in combat" is a small edit to
   that condition. Cost: during combat our baked joints get collapsed to the anim pose, so capsule
   fit and touch accuracy degrade exactly then. If the trigger is really *specific extreme
   animations* rather than combat generally, a narrower condition would be better.
3. **Never assume PLANCK is driving an actor.** Between `ignoredActors`, `IsTemporaryIgnoredActor`
   (SetPosition/warp), race exclusions, the `activeRagdollStartDistance` / `EndDistance` band and
   `minFramesBetweenActorAdds`, an actor can silently be outside PLANCK's management at any moment.
   PPB code that reasons "PLANCK has this" is unsafe.
