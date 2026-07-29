#pragma once
#include <cstdint>

// ============================================================================
//  PivFix — live joint-pivot re-seat + clavicle-follow (Precision Physic
//  Bodies). Ported verbatim from AIHands' PalmCollider.cpp PPB half. This
//  header exposes ONLY POD/RE:: so callers need no Havok SDK — the .cpp is
//  the single TU that includes the Havok 2010 SDK (CMake-scoped).
// ============================================================================
namespace ObjectHold {

    // PIV FIX (live joint-pivot re-seat, R side): pivWrist*/pivElbow*/pivShoulder* knobs = the pivot's
    // ABSOLUTE child-bone-frame position (skyrim u). Writes BOTH stored constraint transforms in lock-step.
    // The partner side is computed from the RUNTIME BIND relation (referencePose math, pose-independent —
    // the 07-04 fix for the self-consistent contamination trap), supplied per joint by the hook pipeline:
    struct PivBindRel {
        bool  valid = false;
        float R[9];    // child-bone -> other-bone bind rotation (row-major)
        float t[3];    // child-bone origin in other-bone frame at bind (skyrim u)
    };
    void PivFixApply(RE::Actor* actor, const PivBindRel rel[11], bool isStatue);  // per-tick deterministic sweep
    // CLAVICLE-FOLLOW (2026-07-05): per-frame update of a shoulder's chest-side anchor from the
    // LIVE clavicle-chain animation — the ragdoll has no clavicle body, so a fixed bind anchor can't
    // follow shoulder protraction (hands-on-hips displaced the whole arm 4.7u, PIVTRACK-proven).
    // left=false: child NPC R UpperArm [RUar]; left=true (2026-07-10 — the left arm displaced ~4u in
    // protracted poses): child NPC L UpperArm [LUar]; both partner NPC Spine2 [Spn2].
    // armParentNode = the upper arm bone's parent node name; refR/refT = the upper arm's PARENT-LOCAL
    // bind transform (from referencePose). Gated internally by pivClavFollow + shoulder enable/autoseat.
    void PivFollowShoulder(RE::Actor* actor, bool left, const char* armParentNode, const float refR[9], const float refT[3]);
    // Read a joint's stored pivot translation (game units) in the CHILD body's frame (childSide=true)
    // or the partner body's frame (false). The capsule auto-fit derives limb capsule endpoints from
    // these: a bone's capsule runs joint-ball to joint-ball, both known once the joints are seated.
    bool PivReadJointLocal(RE::Actor* actor, int joint, bool childSide, float outU[3]);
    // JOINT TRACK (2026-07-10, console `jtrack`): READ-ONLY audit of every ragdoll joint — the world
    // gap between where each Havok constraint pivot actually sits and where its XP32 bone node is, for
    // all 17 joints (left side included, same tables as PivDescaleApply). Writes NOTHING to any
    // constraint (no heal, no descale, nothing armed) — pure measurement that settles the actor-scale
    // pivot-drift question. Dumps one line per joint + a scale-law summary to PPB.log.
    void PivJointTrackDump(RE::Actor* actor);
    bool PivHeadGapZ(RE::Actor* actor, float& outZ);   // head pivot-vs-node Z (game u); + = above
    bool ActorRagdollAttached(RE::Actor* actor);       // FALSE in furniture/sit/sleep/ragdoll — never measure then
    float PivArcTrueScaleOf(RE::Actor* actor);         // arc trueScale (nodeArc / .nif base) latched by the re-scale; 0 = unmeasured
    // JOINT-TRACK tuning-knob trigger (2026-07-10): the console donors for `jtrack` were all taken in
    // this load order (PPB.log: "Console-command donors for 'jtrack' all missing"), so the `jtrackNow`
    // knob is the working trigger. Call per driven actor from the pre-drive pipeline (main thread only).
    // Edge-triggered like handBoxDumpNow: on the knob's 0 -> non-0 transition, every driven actor seen
    // within ~2 s dumps PivJointTrackDump ONCE, then the latch disarms until the next edge (knob back
    // to 0, then non-0 again to re-fire).
    void PivJointTrackTick(RE::Actor* actor);
    // A body wrapper's Havok position in SKYRIM units (the 3-point heel tracker's ragdoll read).
    bool BodyPosU(void* wrapper, float out[3]);
    // kPreLoadGame teardown: drop the per-actor pivot state map (sticky auto-seat targets included) —
    // the Havok world is rebuilt across a load, so every constraint pointer in it would dangle.
    void PivFixClearAll();

    // PIVOT DESCALE (2026-07-09): the engine multiplies NIF constraint pivots by the actor's GetScale
    // at load (probe-proven: all 22 loaded values = baked x 0.9514 on scale-0.9514 Carmella), but the
    // ragdoll wants the UNSCALED values. Undo it once per constraint INSTANCE (re-fires on PLANCK's
    // hinge->ragdoll replacement + AI-warp rebuilds), ALL 17 joints incl. the LEFT side. Gated by the
    // pivDescale knob (default ON). Call per driven actor from the pre-drive pipeline.
    void PivDescaleApply(RE::Actor* actor);
    void PivDescaleClearAll();          // kPreLoadGame (constraint pointers dangle across loads)

    // UNIFORM RE-SCALE (2026-07-14, user's authoritative spec — REPLACES the old 17-joint per-joint seat).
    // Every NPC already gets a scale-1 baked Havok body scaled to their RECORD scale by the engine —
    // correct ~97-98% of the time (do nothing). A few (~2-3%) have a record scale that does NOT match
    // their real XP32 skeleton scale, so their Havok body is the wrong SIZE. This finds those and fixes
    // them by scaling the WHOLE Havok body (all joint pivots) by ONE uniform factor — NOT by seating
    // joints individually.
    //   CHECK (3 points only — anchor=COM, head, calf; never 17): (1) anchor-together gate — the
    //     spine0<->COM constraint's two stored pivot copies must coincide (a satisfied, connected
    //     ragdoll); PLANCK loosening/disconnection is the only thing that separates them -> back off.
    //     (2) anchor->head and anchor->calf DISTANCES, once from the XP32 NODES (X_head,X_calf) and once
    //     from the HAVOK joint pivots (H_head,H_calf). (3) TRIGGER if |X_head-H_head|>0.5u OR
    //     |X_calf-H_calf|>0.5u; else PASS (leave the NPC EXACTLY alone — this is how a correctly-scaled
    //     NPC, incl. Lydia, is untouched).
    //   FIX (one uniform factor): the engine built pivots = baked × GetScale; we want baked × trueScale
    //     (so the Havok joint distances match the XP32 node distances). factor = trueScale / GetScale,
    //     applied to BOTH stored copies of every constraint (the proven PivDescale both-copies pattern),
    //     ONCE per constraint INSTANCE (re-fires only on ragdoll rebuild). trueScale = MeasuredScaleOf
    //     (the 10-sample-median XP32 span ratio); unknown -> the WHOLE actor is left alone.
    //   GATES: 10 m distance (+ abandon-on-leave), ActorRagdollAttached, player-grab, per-actor throttle
    //     (~1 Hz), and the loosened backoff (5 s recheck; after 2 min still loosened -> every 2 min).
    // Call per driven actor. No bind-relation table needed (a uniform scalar needs no per-joint math).
    void PivScaleCorrect(RE::Actor* actor);
    void PivReScaleClearAll();          // kPreLoadGame (scaled-instance pointers dangle across loads)
}
