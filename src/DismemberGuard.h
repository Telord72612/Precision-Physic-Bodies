#pragma once

// ═══════════════════════════════════════════════════════════════════════════════════════════
// DISMEMBER GUARD (2026-07-26) — the DF/NGD ↔ PLANCK compatibility layer.
//
// Investigation (tools/ppb-scratch/dismember_investigation.txt) established:
//  · NGD's severed head is a FULL INVISIBLE CLONE ACTOR (same base, FF ref) cut inversely at
//    the neck by DF; the visible head rides the clone's repositioned COM body.
//  · PLANCK force-drives every animating actor and re-adds unmanaged ragdolls within 50u —
//    driving the clone's invisible skeleton (head float / SMP hair) and performing/receiving
//    skeleton surgery on LIVE-driven victims (the NPC-vs-NPC kill freeze, 11:43 deadlock).
//  · PLANCK exports IPlanckInterface001 with AddIgnoredActor: ignored actors are dropped from
//    management within a frame, and for already-ragdolled actors the bodies STAY in the world
//    (vanilla corpse physics = exactly the environment DF/NGD were designed for).
//
// This module:
//  1. On ANY NPC death (TESDeathEvent, both stages): PLANCK-ignore the victim immediately, so
//     a killing-blow dismemberment is surgery on an UN-driven ragdoll. Restored after
//     dgGraceDeadS unless the corpse turns out to be dismembered.
//  2. Detects NGD/DF-touched actors (worn gear from Next-Gen Decapitations.esp / Dismembering
//     Framework.esm — head clones, stump-capped corpses): PERMANENT PLANCK ignore + full PPB
//     per-actor skip (no rig build, no GHOST/NFING/PIVRESCALE — kills the arms-off artifact).
//  3. FF-ref (runtime-spawned) actors are probed on their first driven frame, BEFORE any PPB
//     rig lands on them — a confirmed clone never gets a rig at all.
//
// Knobs (PPB_tuning.txt): dgEnable (master), dgGraceDeadS, dgLog.
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace RE { class Actor; }

namespace DismemberGuard {
    // SKSE messaging handshake with PLANCK (idempotent — call at kPostPostLoad + kDataLoaded).
    void AcquirePlanck();
    // Acquire the DF + NGD plugin APIs (idempotent — call at kDataLoaded + kPostLoadGame/kNewGame,
    // the point their own headers recommend). Enables NGD IsHead() detection + DF state queries.
    void AcquireDfNgd();
    // Register the TESDeathEvent sink (idempotent — call at kDataLoaded).
    void Install();
    // Per driven actor from the pre-drive seam (PPBHook::ApplyToPoseTrack). Enrolls fresh
    // FF-ref actors (immediate first gear-probe) and runs the throttled maintenance sweep.
    void Tick(RE::Actor* driven);
    // Cache-only read — TRUE for confirmed NGD/DF-touched actors (and FF refs whose first
    // gear-probe hasn't resolved yet). The caller skips ALL PPB per-actor systems when true.
    bool IsExcluded(RE::Actor* a);
    // Execute DF dismemberment cuts captured on DF's worker thread (the VR freeze fix).
    // MAIN THREAD ONLY. Called from DismemberGuard::Tick and unconditionally from
    // NpcFinger::OnFrame. Never uses the SKSE task queue — that lock is the deadlock.
    void DrainQueuedCuts();
    // kPreLoadGame / kNewGame teardown: actor pointers/handles don't survive a load.
    void ClearOnLoad();
}
