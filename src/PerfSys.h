#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================================
//  PerfSys — the collision PERF system (2026-07-09), built from
//  tools/ppb-scratch/PERF_BUILD_PLAN.md (spec_perf.md as amended by
//  review_perf.md F1-F12). Two levers, both OFF until perfPreset > 0:
//
//  1. FILTER (review F1 correction): a HIGGS collision-filter comparison
//     callback (vtable 20). PLANCK returns Continue for same-group biped
//     pairs it has enabled self-collision for, so our later-registered
//     callback sees them and returns Ignore for LThigh(8) x RThigh(14) —
//     the measured #1 offender (1772 pts). Filter-level = the agent is never
//     created = cuts the manifold-walk volume that causes the double-step
//     stutter. Optional cross-actor part-2 (COM/Spn0) drop, default OFF.
//
//  2. CHILD-LOD ("the 50/80/90 drop"): per-actor tiers FULL / REDUCED / CORE
//     applied to the 17 bhkListShape bodies via the verified enabledChildren
//     bitmask (a disabled child emits no shape key, no agent, no manifold).
//     Child 0 (MAIN capsule) is never disabled — the collision envelope
//     stays intact; only flesh DETAIL degrades. Policy (F2-safe):
//       - HIGGS-held actor -> FULL (P90: REDUCED); two distinct held -> both REDUCED
//       - the single nearest actor inside lodNearDist -> FULL (P90: REDUCED)
//       - inside lodFarDist -> REDUCED (P90: CORE)
//       - beyond -> CORE
//     Downgrades need lodSettleFrames of stability; upgrades are staged
//     (lodStageBits child-enables per body per frame) so an envelope never
//     pops into an existing contact at full size (F6). Restore-on-undriven
//     + relaxed enable for out-of-world bodies guard PLANCK's ragdoll
//     remove/re-add capacity trap (F5). No body pointers are cached across
//     frames (F9) — bodies are re-resolved at apply time.
//
//  Hard-rule compliance: bitmask + AABB-cache int writes on existing shapes
//  only (pitfall rule 1); no shape types touched; no bodies added.
// ============================================================================
namespace RE { class Actor; }

namespace PerfSys {

    // Register the HIGGS comparison callback + per-frame callback (idempotent;
    // call after Interop::AcquireHiggs at kPostPostLoad and kDataLoaded).
    void RegisterHiggs();

    // Per driven actor from the 0xB266AB pre-drive chain (PPBHook), main thread.
    void OnPreDrive(RE::Actor* actor);

    // Frame boundary (HIGGS PostVrikPostHiggs): frame counter, nearest-actor
    // election, two-held clamp, undriven-actor restores.
    void OnFrame();

    // kPreLoadGame: wipe per-actor state (3D + Havok world rebuilt across load;
    // fresh bodies come back from the NIF with all children enabled).
    void ClearOnLoad();

    // Nearest driven actor elected in OnFrame (0 = none; only within lodNearDist)
    // — NpcFingerTest's knob-attach target. Relaxed-atomic read, any thread.
    std::uint32_t NearestDrivenId();

    // TRUE once the HIGGS comparison + frame callbacks are live — NpcFingerTest's
    // creation gate (finger bodies must never exist without the filter callback).
    bool HiggsRegistered();

    // 2026-08-22 (RayTel): how many currently-driven actors sit at each child-LOD
    // tier, so the raycast telemetry line can show the tier split next to the
    // measured child-walk counts. Main thread only (takes the state mutex).
    // All three come back 0 when nothing is driven; note that lodEnable 0 leaves
    // every actor at FULL, which is what the shipped tuning file does today.
    void TierCensus(int& a_full, int& a_reduced, int& a_core);
}
