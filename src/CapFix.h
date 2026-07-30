#pragma once
#include <cstddef>

// ============================================================================
//  CapFix — the live capsule editor (Precision Physic Bodies). Ported verbatim
//  from AIHands' GrabDiag.cpp PPB half: the 12-slot generation sweep, the
//  3-capsule hand bhkListShape per-child tuning + union-AABB repatch, the
//  ball-to-ball capsule auto-fit, and the `capfix` console entry.
// ============================================================================
namespace GrabDiag {
    // The 12-slot gen sweep: applies the cap* knobs to this actor's bodies once per generation,
    // with a 1 Hz body-identity probe (ragdoll-rebuild self-heal) and the capAutoFit dispatcher.
    // Call each hook fire per driven actor — gated internally by the generation.
    void CapFixApply(RE::Actor* actor);
    void InvalidateBodyScale(std::uint32_t formId);  // OBody Obody_ApplyMorph push (any thread)

    // ── ReTouch exports (2026-07-24): live capsule geometry for the ghost-zone / touch layer.
    // World-space, Skyrim units, computed from the LIVE rigid bodies — so everything the
    // apply path wrote (ReScale, ReShape, the head channel) is already inside the answer.
    bool ReadCapsuleWorldU(RE::Actor* a, int slot, int child, float aOut[3], float bOut[3], float* rOut);
    // Sided variant (left = the L-twin node); SlotHasLeftTwin says which slots have one.
    bool ReadCapsuleWorldUSide(RE::Actor* a, int slot, bool left, int child, float aOut[3], float bOut[3], float* rOut);
    bool SlotHasLeftTwin(int slot);
    bool SlotBodyPoseU(RE::Actor* a, int slot, float posOutU[3], float rotOut[9]);
    const char* SlotLabel(int slot);                 // "hand","forearm",... "com" (12 slots)
    int  SlotLiveChildren(RE::Actor* a, int slot);   // list child count; 0 = single capsule/none
    // Console entry: apply=false prints the current R-hand capsule into `out`; apply=true writes
    // the args NOW + calls ObjectHold::CapFixSet so every other driven NPC follows.
    void CapFixConsole(RE::Actor* actor, bool apply,
                       float ax, float ay, float az, float bx, float by, float bz, float r,
                       char* out, std::size_t outSz);
    // FINGER ENDPOINT TRACKING (2026-07-08): rewrite each finger capsule's far endpoint onto the second
    // XP32 node of its pair, EVERY FRAME (the span crosses the PIP joint — no static bake can be right).
    // Call per driven actor, right after CapFixApply. Gated by `fingerCapTrack`; graceful-skips (one log
    // line) when the NIF bake hasn't landed. Pure float edits, never allocates.
    void FingerCapTrack(RE::Actor* actor);
    // kPreLoadGame (2026-07-13 audit): clear the FormID-keyed latches (measured effScale,
    // apply/identity state, finger-gate probes) — a recycled FormID in a DIFFERENT save
    // must not inherit the previous save's measured scale or body identities.
    void CapFixClearOnLoad();
    // The latched measured TRUE scale (0 = unlatched -> leave the joints alone). Consumed by PivFix's
    // Phase-2 joint re-scale. The reference actor (Lydia) returns her GetScale so she is centered too.
    float MeasuredScaleOf(RE::Actor* actor);
}
