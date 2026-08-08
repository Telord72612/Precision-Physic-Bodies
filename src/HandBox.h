#pragma once
#include <cstdint>

// ============================================================================
//  HandBox — PLAYER HAND 4-BOX COLLIDER v2 ("the 4 rectangles", 2026-07-09).
//  Track A: live SHRINK-ONLY dial of HIGGS's 10x3x18cm palm slab (per-frame
//  idempotent halfExtents write, clamped to the first-seen baseline).
//  Track B: 4 keyframed follower boxes per hand riding the player's finger
//  bones (2 index segments + one wide 3-finger slab + a fingertip plate, NO
//  thumb), pushed with applyHardKeyFrame so PLANCK's dynamic NPC flesh
//  genuinely yields to a finger-width contact instead of the whole-hand shove.
//  Built from ppb-scratch/spec2_handbox.md AS AMENDED BY review2_handbox.md
//  (N1 substep gate, N4/N5 bit-14 delay+inherit, N6 authoritative world, ...).
//  Inert until handBoxEnable AND handBoxArm (double gate, review N9); the slab
//  dial is inert while all three higgsSlab* knobs are -1.
//
//  Honest framing (spec preamble): these boxes PUSH flesh only — no HIGGS
//  haptics/sound, no PLANCK hit/damage/aggro, zero CBPC response (all three
//  are pointer-identity-gated on HIGGS's own bodies). Attribution comes later
//  via CBPC fingertip nodes, not via Havok. NPCs outside PLANCK's active-
//  ragdoll set have keyframed biped bodies -> zero response (parity with the
//  stock palm slab today).
//
//  Header exposes only void*/POD (the Markers.h discipline) — the .cpp is a
//  Havok-SDK TU (CMake scopes the SDK include path to it).
// ============================================================================
namespace HandBox {

    // Called from Hooks::StepChainHook with the EXACT float hkpWorld::stepDeltaTime is about to
    // integrate. PPB has always received this value at that seam and discarded it; the jitter
    // diagnostic needs it to compute the real keyframe gain (dtStep x our invDt). Cheap relaxed
    // atomic store, safe unconditionally on the physics thread — no allocation, no float math.
    void NotePhysicsStepDt(float dt);

    // Scene suspension (2026-08-03). Set true while an OStim/SexLab scene is running: the hand
    // rigs are destroyed for the duration via the SAME path handBoxEnable 0 uses, so no new code
    // path is introduced. Restored automatically on scene end.
    void SetSceneSuspended(bool on);
    bool IsSceneSuspended();


    // Register the HIGGS PrePhysicsStep callback (vfunc 21). Idempotent; called
    // from PerfSys::RegisterHiggs (the kPostPostLoad + kDataLoaded handshake
    // site). ALL body create/destroy/keyframe work runs inside that callback —
    // the tuning poll just flips floats (review N14).
    void RegisterHiggs();

    // Frame boundary (HIGGS PostVrikPostHiggs, via PerfSys's lambda): advance the
    // frame counter + flush deferred telemetry. 2026-07-10 LAG FIX: the finger-bone
    // snapshot itself (and its frame-dt capture) is taken inside the FIRST
    // PrePhysicsStep fire of each frame — fresh hand pose at the step boundary; the
    // old PostVrikPostHiggs snapshot trailed the hand by a frame whenever that
    // callback landed after the step. The consumer still keyframes each box exactly
    // ONCE per frame with invDt = 1/frame-dt at snapshot time — never per physics
    // substep (review N1).
    void OnFrame();

    // 0=Continue 1=Collide 2=Ignore; pure bit math, physics-thread-safe
    // (relaxed atomics only). Spliced into PerfSys::FilterCB after NpcFinger's
    // decision. Belt only: Ignores box-vs-HIGGS-own-body pairs even if the
    // sub-layer adjacency rule was misread; the payoff pair (box vs NPC biped)
    // falls through to the vanilla table's layer-56 row.
    int  FilterDecision(std::uint32_t infoA, std::uint32_t infoB);

    // kPreLoadGame: remove bodies via the freshly-resolved live world (the
    // stored bhkWorld* is COMPARE-ONLY), then drop ALL pointers — bodies,
    // cached world, HIGGS slab baselines (HIGGS rebuilds its bodies on load).
    void ClearOnLoad();

    // ReTouch (2026-07-24, upgraded 07-25): TipWorldU returns the box's TIP — center extended
    // half-length down the bone axis — i.e. the actual fingertip for the index box. 0=R 1=L,
    // box 0..3. False when the rig is not live — caller falls back to the finger node.
    bool TipWorldU(int hand, int box, float outU[3]);
    // World CENTER of any box — the per-box identity that makes FINGER/PALM/FIST source
    // classification possible (each box is our own object; we always know which is which).
    bool BoxCenterWorldU(int hand, int box, float outU[3]);
}
