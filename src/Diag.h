#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================================
//  Diag — the read-only performance/correctness instrument for the wave-2
//  torso-bake gating question (2026-07-08). Everything here observes the
//  CURRENTLY DEPLOYED skeleton (45 capsules, 11 list bodies, COM/spine single
//  capsules) — it bakes nothing and adds no ragdoll bodies. The one write it
//  can make is the DOUBLE-gated `capdis` disableChild spike (lodSpike + a
//  console request), which flips enable bits on the R-Thigh list already in
//  the NIF. See Report/Precision Physic Bodies Module/11_ListShape_Performance.md.
//
//  Instruments (all off until the `perf` console command arms them):
//   1. World hkpContactListener (read-only) — buckets contact points by
//      (bodyA,bodyB), tracks maxPointsInOnePair (THE SHIP GATE) + per-child rate.
//   2. Physics-step timer chained on 0xDFB722 (Hooks.cpp) — mean/p95/max step ms.
//   3. Collision-filter dump — bipedBitfields[0..23] + collision tolerance.
//   4. Runtime capsule census — per body: shape type + list-child count + geometry.
//   5. Gated disableChild spike (`capdis`).
//   6. `perf` console command — arm/reset, then dump everything + disarm.
// ============================================================================
namespace RE { class Actor; class hkpWorld; }

namespace Diag {

    // ── physics-step timing (called from Hooks::StepChainHook @0xDFB722) ──
    void OnPhysicsStep(double stepMs);   // accumulates only while armed
    bool Armed();                        // step hook gate (cheap atomic read)

    // ── frame boundary (HIGGS AddPostVrikPostHiggsCallback; NoArgCallback) ──
    void OnFrame();                      // snapshots steps-this-frame -> histogram
    void RegisterFrameCallback();        // wire OnFrame into HIGGS if present (idempotent)

    // ── per-driven-actor pre-drive tick (from ArmIK::ApplyToPoseTrack) ──
    //  When armed: registers the read-only contact listener on the actor's Havok
    //  world (idempotent per world). Always: applies any pending `capdis` spike
    //  for this actor (gated by lodSpike + a matching FormID request).
    void OnPreDrive(RE::Actor* actor);

    // ── console `perf` (zero-param toggle) ──
    //  1st press: reset counters, arm, dump census+filter for the selected NPC.
    //  2nd press: disarm, dump the full report to PPB.log + a one-line console summary.
    void TogglePerf(RE::Actor* selected, char* out, std::size_t outSz);

    // ── console `capdis <slot> <mask>` (referenceFunction=true) ──
    //  Records a deferred spike request; the flip happens on the next pre-drive
    //  fire under the world lock. mask = the DESIRED disabled-child set
    //  (bit i set -> child i disabled). mask 0 re-enables all. slot 8 = R Thigh.
    void CapDisRequest(RE::Actor* actor, int slot, std::uint32_t mask, char* out, std::size_t outSz);

    // kPreLoadGame teardown: the Havok world is rebuilt across a load — drop the
    // cached world pointer (forces re-registration) and any pending spike.
    void ClearOnLoad();
}
