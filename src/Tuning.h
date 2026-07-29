#pragma once
#include <cstdint>

// ============================================================================
//  Tuning — PPB_tuning.txt: the live hot-reload knob set for the Precision
//  Physic Bodies stack (CapFix capsules, PivFix joint pivots, auto-seat,
//  clavicle-follow, capsule auto-fit, heel fix, per-joint world-Z lift).
//  Split verbatim from AIHands' GrabTune (PalmCollider.cpp) — ONLY the PPB
//  keys; the weld/give/vis knobs stay in AIHands. The namespace stays
//  ObjectHold so every ported call site compiles unchanged.
// ============================================================================
namespace ObjectHold {

    // ── bhkListShape CHILD KNOB BLOCK (2026-07-08, wave-2 torso bake) ────────────────────────────
    // 8 CONTIGUOUS floats — the exact layout every named slot block already uses. CapFixChildSlot
    // hands out `&child.enable` and reads f[0]..f[7], so the field ORDER and the absence of padding
    // are both LOAD-BEARING (the static_assert below is the guard).
    // Defaults = a legal 1u +Z rod at the body origin, r 0.2: insurance so an accidental Enable with
    // no matching file line can never write a zero-radius degenerate capsule.
    struct CapChild {
        float enable = 0.f;
        float ax = 0.f, ay = 0.f, az = 0.f;
        float bx = 0.f, by = 0.f, bz = 1.f;
        float r  = 0.2f;
    };
    static_assert(sizeof(CapChild) == 8 * sizeof(float), "CapFixChildSlot pointer math depends on this");

    struct GrabTune {
        // ── CAP FIX (2026-07-02, live capsule calibration): rewrite the HAND collision capsule's geometry
        // (two endpoints + radius, BODY-LOCAL skyrim units) on the RIGHT hand of every driven NPC, applied
        // when the console command `capfix` is typed (it re-reads this file + arms a new generation). The
        // live hand shape is a DEGENERATE capsule (both endpoints at the wrist, r=2.4) — objects hit it
        // before reaching the visible palm. Dial it onto the palm with the collision visualizer, then the
        // final values get BAKED into skeleton_female.nif (binary float-patch). RIGHT hand only for now.
        float capHandEnable = 0.f;
        float capHandAX = 0.f, capHandAY = 0.f, capHandAZ = 0.f;   // endpoint A (body-local, skyrim u)
        float capHandBX = 0.f, capHandBY = 0.f, capHandBZ = 0.f;   // endpoint B
        float capHandR  = 2.4f;                                    // radius
        // Same knob set for the rest of the arm + the head (2026-07-03: the full-ragdoll remake sweep).
        float capForeEnable = 0.f;                                 // NPC R Forearm [RLar]
        float capForeAX = 0.f, capForeAY = 0.f, capForeAZ = 0.f;
        float capForeBX = 0.f, capForeBY = 0.f, capForeBZ = 0.f;
        float capForeR  = 3.7f;
        float capUpperEnable = 0.f;                                // NPC R UpperArm [RUar]
        float capUpperAX = 0.f, capUpperAY = 0.f, capUpperAZ = 0.f;
        float capUpperBX = 0.f, capUpperBY = 0.f, capUpperBZ = 0.f;
        float capUpperR = 5.8f;
        float capHeadEnable = 0.f;                                 // NPC Head [Head]
        float capHeadAX = 0.f, capHeadAY = 0.f, capHeadAZ = 0.f;
        float capHeadBX = 0.f, capHeadBY = 0.f, capHeadBZ = 0.f;
        float capHeadR  = 6.0f;
        // 3-capsule hand LIST children (the R hand is a bhkListShape since the 2026-07-03 bake): child
        // order in the NIF = C1 center / C2 thumb / C3 pinky. Same 8-contiguous-float layout per child
        // (CapFixChildSlot depends on the field order). Defaults = the baked values.
        float capHandC1Enable = 0.f;                               // list child 0 — center rod
        float capHandC1AX = 2.0f,  capHandC1AY = 0.7f,  capHandC1AZ = 1.0f;
        float capHandC1BX = 0.17f, capHandC1BY = -0.3f, capHandC1BZ = 6.3f;
        float capHandC1R  = 1.3f;
        float capHandC2Enable = 0.f;                               // list child 1 — thumb-side rod
        float capHandC2AX = 2.9f,  capHandC2AY = 0.7f,  capHandC2AZ = 1.0f;
        float capHandC2BX = 1.97f, capHandC2BY = -0.3f, capHandC2BZ = 6.05f;
        float capHandC2R  = 1.25f;
        float capHandC3Enable = 0.f;                               // list child 2 — pinky-side rod
        float capHandC3AX = 1.1f,   capHandC3AY = 0.7f,  capHandC3AZ = 1.0f;
        float capHandC3BX = -1.63f, capHandC3BY = -0.3f, capHandC3BZ = 6.05f;
        float capHandC3R  = 1.25f;
        // 2026-07-12 palm fill (user request): 2 MORE rods between C1 (center) and C3 (pinky),
        // seeded at the 1/3 and 2/3 interpolations of the DIALED Lydia-master C1/C3 values.
        float capHandC4Enable = 0.f;                               // list child 3 — palm fill rod A
        float capHandC4AX = 1.8f,   capHandC4AY = -1.0f, capHandC4AZ = -2.1f;
        float capHandC4BX = 1.603f, capHandC4BY = -1.733f, capHandC4BZ = 4.15f;
        float capHandC4R  = 1.0f;
        float capHandC5Enable = 0.f;                               // list child 4 — palm fill rod B
        float capHandC5AX = 0.9f,   capHandC5AY = -1.0f, capHandC5AZ = -2.1f;
        float capHandC5BX = 0.337f, capHandC5BY = -1.967f, capHandC5BZ = 3.8f;
        float capHandC5R  = 1.0f;
        // Full-body capsule slots (2026-07-05): spine chain + neck + R leg + pelvis core.
        float capSpine0Enable = 0.f;
        float capSpine0AX = 0.f, capSpine0AY = 0.f, capSpine0AZ = 0.f, capSpine0BX = 0.f, capSpine0BY = 0.f, capSpine0BZ = 0.f, capSpine0R = 5.f;
        float capSpine1Enable = 0.f;
        float capSpine1AX = 0.f, capSpine1AY = 0.f, capSpine1AZ = 0.f, capSpine1BX = 0.f, capSpine1BY = 0.f, capSpine1BZ = 0.f, capSpine1R = 5.f;
        float capSpine2Enable = 0.f;
        float capSpine2AX = 0.f, capSpine2AY = 0.f, capSpine2AZ = 0.f, capSpine2BX = 0.f, capSpine2BY = 0.f, capSpine2BZ = 0.f, capSpine2R = 5.f;
        float capNeckEnable = 0.f;
        float capNeckAX = 0.f, capNeckAY = 0.f, capNeckAZ = 0.f, capNeckBX = 0.f, capNeckBY = 0.f, capNeckBZ = 0.f, capNeckR = 5.f;
        float capThighEnable = 0.f;
        float capThighAX = 0.f, capThighAY = 0.f, capThighAZ = 0.f, capThighBX = 0.f, capThighBY = 0.f, capThighBZ = 0.f, capThighR = 5.f;
        float capCalfEnable = 0.f;
        float capCalfAX = 0.f, capCalfAY = 0.f, capCalfAZ = 0.f, capCalfBX = 0.f, capCalfBY = 0.f, capCalfBZ = 0.f, capCalfR = 5.f;
        float capFootEnable = 0.f;
        float capFootAX = 0.f, capFootAY = 0.f, capFootAZ = 0.f, capFootBX = 0.f, capFootBY = 0.f, capFootBZ = 0.f, capFootR = 5.f;
        // COM defaults = the live BEFORE-log values (PPB.log 2026-07-07: hip-to-hip ±X rod), so enabling
        // the slot with no file lines is an IDENTITY write — a missing key can never degenerate the axis.
        float capComEnable = 0.f;
        float capComAX = 2.79f, capComAY = -3.83f, capComAZ = 5.33f, capComBX = -2.79f, capComBY = -3.83f, capComBZ = 5.33f, capComR = 7.66f;
        // ── FLESH-FIT LIST CHILDREN (2026-07-07, task #7 rebuild): the bake converts limb bodies to
        // bhkListShape (R-hand-plate pattern). Child 0 stays the MAIN capsule — still driven by the slot
        // knobs above once the shape is a list — and children 1..N are the ADDED capsules, driven by these
        // knobs (capUpperC1 = list child 1, capThighC3 = list child 3, ...). Same LOAD-BEARING
        // 8-contiguous-float layout per child (CapFixChildSlot depends on the field order).
        // Patterns: arms = end-to-end taper (1 added child), thigh = 5 superposed rings, calf = 3
        // superposed rings, foot = 2 parallel sole rods. Seeds from tools/ppb-scratch/flesh-fit-bake-notes.md:
        // taper far ends are the RECORDED originals; ring/rod positions are placeholders to dial live.
        float capUpperC1Enable = 0.f;                              // upper-arm taper (child 1)
        float capUpperC1AX = 1.8f, capUpperC1AY = 0.0f, capUpperC1AZ = 11.2f;   // near = dialed capUpperB
        float capUpperC1BX = 1.1f, capUpperC1BY = 0.5f, capUpperC1BZ = 20.2f;   // far = recorded elbow end
        float capUpperC1R  = 2.0f;
        // 2026-07-09 SHOULDER LOCK (+2): fill the deltoid/socket so a blade can't pass through the
        // spine2<->upperarm seam. Arm frame: +X inward, +Y frontward, +Z elbow-ward (shoulder ~ Z 0).
        float capUpperC2Enable = 0.f;                              // shoulder ring (child 2)
        float capUpperC2AX = 1.8f, capUpperC2AY = 0.0f, capUpperC2AZ = 2.0f;
        float capUpperC2BX = 1.8f, capUpperC2BY = 0.0f, capUpperC2BZ = 5.0f;
        float capUpperC2R  = 2.0f;
        float capUpperC3Enable = 0.f;                              // socket cap, reaches INTO the torso (child 3)
        float capUpperC3AX = 1.8f, capUpperC3AY = 0.0f, capUpperC3AZ = -1.0f;
        float capUpperC3BX = 1.8f, capUpperC3BY = 0.0f, capUpperC3BZ = 2.0f;
        float capUpperC3R  = 2.0f;
        float capForeC1Enable = 0.f;                               // forearm taper (child 1)
        float capForeC1AX = 0.8f, capForeC1AY = 1.3f, capForeC1AZ = 4.3f;       // near = dialed capForeB
        float capForeC1BX = 0.8f, capForeC1BY = 1.3f, capForeC1BZ = 10.3f;      // far = recorded wrist end
        float capForeC1R  = 1.5f;
        float capThighC1Enable = 0.f;                              // superposed rings along hip->knee
        float capThighC1AX = -1.9f, capThighC1AY = -2.9f, capThighC1AZ = -4.3f;
        float capThighC1BX = -1.9f, capThighC1BY = -2.9f, capThighC1BZ = -1.3f;
        float capThighC1R  = 3.4f;
        float capThighC2Enable = 0.f;
        float capThighC2AX = -1.3f, capThighC2AY = -2.3f, capThighC2AZ = 2.1f;
        float capThighC2BX = -1.3f, capThighC2BY = -2.3f, capThighC2BZ = 5.1f;
        float capThighC2R  = 3.2f;
        float capThighC3Enable = 0.f;
        float capThighC3AX = -0.8f, capThighC3AY = -1.8f, capThighC3AZ = 8.5f;
        float capThighC3BX = -0.8f, capThighC3BY = -1.8f, capThighC3BZ = 11.5f;
        float capThighC3R  = 3.0f;
        float capThighC4Enable = 0.f;
        float capThighC4AX = -0.2f, capThighC4AY = -1.2f, capThighC4AZ = 14.8f;
        float capThighC4BX = -0.2f, capThighC4BY = -1.2f, capThighC4BZ = 17.8f;
        float capThighC4R  = 2.8f;
        float capThighC5Enable = 0.f;
        float capThighC5AX = 0.3f, capThighC5AY = -0.7f, capThighC5AZ = 19.6f;
        float capThighC5BX = 0.3f, capThighC5BY = -0.7f, capThighC5BZ = 22.6f;
        float capThighC5R  = 2.6f;
        // 2026-07-09 KNEE HOOK (+1): straddles the knee (thigh main B ~ Z 27.4). +Z runs DOWN the limb.
        float capThighC6Enable = 0.f;
        float capThighC6AX = 0.5f, capThighC6AY = -0.5f, capThighC6AZ = 21.0f;
        float capThighC6BX = 0.7f, capThighC6BY = -0.3f, capThighC6BZ = 29.0f;
        float capThighC6R  = 2.5f;
        float capCalfC1Enable = 0.f;                               // superposed rings along knee->ankle
        float capCalfC1AX = 0.6f, capCalfC1AY = -1.1f, capCalfC1AZ = -1.7f;
        float capCalfC1BX = 0.6f, capCalfC1BY = -1.1f, capCalfC1BZ = 1.3f;
        float capCalfC1R  = 2.6f;
        float capCalfC2Enable = 0.f;
        float capCalfC2AX = 0.8f, capCalfC2AY = -1.1f, capCalfC2AZ = 5.3f;
        float capCalfC2BX = 0.8f, capCalfC2BY = -1.1f, capCalfC2BZ = 8.3f;
        float capCalfC2R  = 2.3f;
        float capCalfC3Enable = 0.f;
        float capCalfC3AX = 0.9f, capCalfC3AY = -1.0f, capCalfC3AZ = 12.2f;
        float capCalfC3BX = 0.9f, capCalfC3BY = -1.0f, capCalfC3BZ = 15.2f;
        float capCalfC3R  = 2.0f;
        // 2026-07-09 KNEE HOOK (+1): straddles the knee from below (calf main A ~ Z -6.4). +Z runs DOWN.
        float capCalfC4Enable = 0.f;
        float capCalfC4AX = 0.6f, capCalfC4AY = -1.1f, capCalfC4AZ = -9.0f;
        float capCalfC4BX = 0.8f, capCalfC4BY = -1.0f, capCalfC4BZ = -1.0f;
        float capCalfC4R  = 2.3f;
        float capFootC1Enable = 0.f;                               // parallel sole rods (foot: +X inward, +Y up, +Z toe)
        float capFootC1AX = 2.2f, capFootC1AY = 3.0f, capFootC1AZ = 5.3f;
        float capFootC1BX = 2.2f, capFootC1BY = 4.0f, capFootC1BZ = -1.2f;
        float capFootC1R  = 1.2f;
        float capFootC2Enable = 0.f;
        float capFootC2AX = -0.2f, capFootC2AY = 3.0f, capFootC2AZ = 5.3f;
        float capFootC2BX = -0.2f, capFootC2BY = 4.0f, capFootC2BZ = -1.2f;
        float capFootC2R  = 1.2f;
        // 2026-07-09 ANKLE LOCK (+1): third sole rod, seeded on the midline between C1 (+2.2) and C2 (-0.2).
        float capFootC3Enable = 0.f;
        float capFootC3AX = 1.0f, capFootC3AY = 3.0f, capFootC3AZ = 5.3f;
        float capFootC3BX = 1.0f, capFootC3BY = 4.0f, capFootC3BZ = -1.2f;
        float capFootC3R  = 1.2f;
        // ── LIVE LEFT-MIRROR (2026-07-07): every capsule slot/child write mirrors to the LEFT twin body
        // as {-AX,AY,AZ}/{-BX,BY,BZ}/same r — verified two ways (bake script + NIF dump: every L/R pair
        // is an exact X-negation in body-local space). Center slots (head/spine/neck/com) have no twin.
        float capMirrorL = 1.f;
        // ── PIV GRAB GATE (2026-07-07): pause the 1 Hz pivot re-seat/heal while the PLAYER is grabbing
        // this actor (HIGGS IsHoldingObject/GetGrabbedObject poll; drift heuristic when HIGGS is absent)
        // plus a 1.5 s post-release grace. The 07-07 in-VR bug: a grab displaced wrist/elbow ~12u UNDER
        // every existing guard threshold, so the heal snapped the pivots back each second -> bounce.
        float pivGrabGate = 1.f;

        // ── PIV FIX (2026-07-03, live joint-pivot re-seat, RIGHT side): the visualizer's blue balls are
        // constraint pivots stored TWICE (once per connected body frame). Knob = the pivot's ABSOLUTE
        // position in the CHILD bone's local frame (skyrim u; hand frame for wrist, forearm for elbow,
        // upper arm for shoulder — same axes as the capsule dial). The DLL writes BOTH stored copies
        // through the live body transforms so they always agree. Enable 0 → BEFORE log only (discovery).
        // 4 contiguous floats per joint: Enable, X, Y, Z (PivFixSlot depends on the field order).
        float pivWristEnable = 0.f;
        float pivWristX = 0.f, pivWristY = 0.f, pivWristZ = 0.f;
        float pivElbowEnable = 0.f;
        float pivElbowX = 0.f, pivElbowY = 0.f, pivElbowZ = 0.f;
        float pivShoulderEnable = 0.f;
        float pivShoulderX = 0.f, pivShoulderY = 0.f, pivShoulderZ = 0.f;
        // Full-body joints (2026-07-05, Precision Physic Bodies): spine chain + neck/head + R leg.
        float pivSpine0Enable = 0.f;  float pivSpine0X = 0.f, pivSpine0Y = 0.f, pivSpine0Z = 0.f;
        float pivSpine1Enable = 0.f;  float pivSpine1X = 0.f, pivSpine1Y = 0.f, pivSpine1Z = 0.f;
        float pivSpine2Enable = 0.f;  float pivSpine2X = 0.f, pivSpine2Y = 0.f, pivSpine2Z = 0.f;
        float pivNeckEnable = 0.f;    float pivNeckX = 0.f,   pivNeckY = 0.f,   pivNeckZ = 0.f;
        float pivHeadEnable = 0.f;    float pivHeadX = 0.f,   pivHeadY = 0.f,   pivHeadZ = 0.f;
        float pivHipEnable = 0.f;     float pivHipX = 0.f,    pivHipY = 0.f,    pivHipZ = 0.f;
        float pivKneeEnable = 0.f;    float pivKneeX = 0.f,   pivKneeY = 0.f,   pivKneeZ = 0.f;
        float pivAnkleEnable = 0.f;   float pivAnkleX = 0.f,  pivAnkleY = 0.f,  pivAnkleZ = 0.f;
        // pivAutoSeat: STATUE-ONLY auto-placement — child target := the XP32 bone origin read live
        // (in body frame), partner := bind math. The DLL walks every ball onto its bone numerically.
        float pivAutoSeat = 0.f;
        // pivClavFollow: per-frame chest-side shoulder anchor riding the live clavicle chain.
        float pivClavFollow = 1.f;
        // capAutoFit: arm capsule endpoints derived from the SEATED joints (elbow/wrist/shoulder balls)
        // — the capsule spans ball-to-ball per the bury-the-ball doctrine. Radius stays the slot knob.
        float capAutoFit = 0.f;
        // MEASURED-SCALE MASTER SWITCH (2026-07-13): 1 = apply the 1 Hz tape-measure's
        // per-NPC effScale to capsule writes (the self-correcting scale system); 0 =
        // BYPASS it and use GetScale (pre-measured-scale behavior) so the DIALED capsules
        // load as-is. **DEFAULT 0 (OFF) 2026-07-13 (user call):** the tape-measure reads
        // ~1.033 for the scale-1.0 master (Lydia) because the ragdoll bodies sit ~3u off
        // the XP32 nodes — a SYSTEMATIC baseline, not a true build-scale — which inflated
        // her capsules past her mesh. Off = the hand-dialed bodies are the source of truth.
        // Re-enable (set 1) only after the baseline is calibrated against the master's own
        // reading. Live — a flip re-dresses every driven NPC (edge-detected in CapFixPollFile).
        float measuredScaleEnable = 0.f;   // RETIRED 2026-07-15 (default OFF). The pose-invariant ARC-SUM
                                           // re-scale (Report 20) fully supersedes this legacy tape: it
                                           // drives BOTH joints and capsules and is decoupled from this
                                           // knob. Off = the sampler no longer runs (no noisy "resizing"
                                           // logs) and CapScaleOf falls back to stable GetScale pre-latch.
                                           // Knob kept as a dormant lever; do NOT re-enable (arc owns scale).
        float measuredScaleRefId = 666260.f;  // the reference actor's FormID (Lydia = 0x000A2C94 = 666260).
                                           // PPB captures HER XP32 node spacing as "scale 1"; every other
                                           // NPC is measured against it. 0 = no reference (system inert).
        // ── BODY SCALE (2026-07-14): a THIRD system, SEPARATE from the uniform re-scale — it sizes the
        // collision CAPSULES to an NPC's actual body SHAPE (not weight; OBody negates weight), routed
        // THROUGH the existing CapFix scale-from-joint apply (CapRegionScaleOf = CapScaleOf x regionRatio;
        // never a new positioning scheme, never a ragdoll joint). Two data sources: BODY = OBody/SKEE
        // named-morph net deviations from the zero-slider base (O(1), cached); HEAD = a cheap head-mesh
        // bounding-extent measure (vertex fallback), once + cached. Region ratio = 1 + net x regionFactor
        // (head: 1 + (raw/ref - 1) x factor), hard-CLAMPED so a bad read can never explode a capsule.
        // bodyScale is the MASTER gate (default 1 = ON). One-shot per NPC on 3D-load + settled; the
        // per-region factor + clamp knobs are read LIVE at apply, so a factor edit re-dresses within ~1 s.
        float bodyScale        = 1.f;   // master gate: 0 = OFF (capsules stay at the sculpted zero-slider base)
        float bodyScaleChest   = 1.f;   // per-region deviation GAINS (0 = that region never scales)
        float bodyScaleBreasts = 1.f;
        float bodyScaleBelly   = 1.f;
        float bodyScaleWaist   = 1.f;
        float bodyScaleButt    = 1.f;
        float bodyScaleThighs  = 1.f;
        float bodyScaleArms    = 1.f;   // upperArms
        float bodyScaleHead    = 1.f;   // gain on the head mesh-extent ratio
        float bodyScaleClampLo = 0.7f;  // hard floor on every region ratio (accessor re-bounds to 0.3..1.0)
        float bodyScaleClampHi = 1.4f;  // hard ceiling on every region ratio (accessor re-bounds to 1.0..3.0)
        // bodyScaleDump: edge-triggered (change -> clears the per-NPC ratio cache so every driven NPC
        // re-latches + re-logs its region reads). NOSNAP (a diagnostic flip must not itself re-dress).
        float bodyScaleDump    = 0.f;
        // ── HEEL FIX (2026-07-02, prototype): shift each heeled NPC's WORLD_FROM_MODEL track +heelZ at the
        // pre-drive hook, so PLANCK drives the ENTIRE ragdoll up to the heel-raised visual skeleton (measured:
        // the ragdoll hand sits exactly heelZ (~8u) below the visible hand — every PLANCK/HIGGS touch zone and
        // our weld anchor with it). 0 = off (ship default until proven); 1 = on. Watch for the two failure
        // modes: the NPC visually RISING ~heelZ (double-raise via the ragdoll→pose feedback) or foot-IK fights.
        float heelFix = 0.f;
        // ── PER-JOINT WORLD-Z LIFT (2026-07-07, PPB split — NEW): raise a joint's seated pivot straight
        // UP in WORLD space by this many game units (negative = down). Applied in PivFixApply AFTER the
        // child target is selected (manual knob AND sticky auto-seat alike): the world offset [0,0,upU]
        // is transformed into the child body frame (childTarget += R_bodyᵀ·[0,0,upU]). The partner copy
        // inherits automatically through the bind-relation mapping. 0 = off. Same joint order as PivFixSlot.
        float pivWristUpU = 0.f, pivElbowUpU = 0.f, pivShoulderUpU = 0.f;
        float pivSpine0UpU = 0.f, pivSpine1UpU = 0.f, pivSpine2UpU = 0.f;
        float pivNeckUpU = 0.f, pivHeadUpU = 0.f;
        float pivHipUpU = 0.f, pivKneeUpU = 0.f, pivAnkleUpU = 0.f;
        // ── FINGER ENDPOINT TRACKING (2026-07-08): the finger bodies are NIF-authored FOLLOWER bodies
        // (the vanilla-tail recipe), NOT ragdoll members. Their far endpoint is rewritten EVERY FRAME
        // onto the second XP32 node of the pair (FingerX0 -> FingerX2): the span crosses the PIP joint,
        // so no static bake can be correct — even at bind it is 18% too long and off-axis. Pure float
        // edits (pitfall rule #1). fingerCapTrack 0 = off (the capsules keep their last written shape).
        // fingerCapR is in GAME UNITS. NOTE the recon's warning: 0.2u (2.9 mm) is ~6x below the proven
        // VR tunneling floor (the R-hand rods are 1.25-1.30u); 0.2 is the IDENTITY value matching the
        // NIF seed, so nothing jumps on load. Dial to ~0.5 (anatomically honest) once verified.
        // 2026-07-08 (perf-diagnostic build): default flipped 1 -> 0. NO finger bodies are deployed in
        // the current NIF, so tracking is a graceful no-op EITHER way — but at default 1 the per-frame
        // writer still pays two ~749-node GetObjectByName walks per driven actor (the finger probe sits
        // ABOVE the `if (!fg.any) return;` early-out), which pollutes the physics-step / contact
        // measurement this build exists to take. Default 0 removes that cost. No functional change on the
        // deployed skeleton; re-enable via PPB_tuning.txt once the finger bake lands.
        float fingerCapTrack = 0.f;
        float fingerCapR     = 0.2f;
        // ── CHILD-LOD SPIKE GATE (2026-07-08 perf diagnostic): the ONLY write this build can make to live
        // ragdoll geometry, and it is DOUBLE-gated. lodSpike 0 (default) = the `capdis` console command is
        // inert (records nothing, flips nothing). Set lodSpike 1 AND type `capdis <slot> <mask>` to fire a
        // deferred, world-locked hkpListShape::disableChild bit-flip on the next pre-drive hook fire.
        // Targets the R-Thigh 6-child bhkListShape already present in the deployed NIF. See Diag.cpp.
        float lodSpike = 0.f;
        // ── PERF SYSTEM (2026-07-09, PerfSys.cpp; PERF_BUILD_PLAN.md) — all PK_NOSNAP (never re-dress
        // capsules). perfPreset 0 = everything off; 50/80/90 = the installer presets. -1 knobs resolve
        // from the preset. Filter = HIGGS comparison callback (thigh-x-thigh Ignore); LOD = per-actor
        // FULL/REDUCED/CORE enabledChildren budgets on the 17 list bodies (child 0 never disabled).
        float perfPreset       = 0.f;
        float filterSelfThigh  = -1.f;   // -1 preset / 0 off / 1 on
        float filterCrossPelvis= -1.f;   // -1 preset (OFF in all presets until validated) / 0 / 1
        float lodEnable        = -1.f;   // -1 preset / 0 off / 1 on
        float lodNearDist      = 180.f;  // nearest-actor FULL band (game u; > weapon reach per review F12)
        float lodFarDist       = 700.f;  // REDUCED band; beyond = CORE
        float lodHystPct       = 0.10f;  // band hysteresis
        float lodSettleFrames  = 45.f;   // stable frames required before any tier DOWNGRADE
        float lodStageBits     = 6.f;    // max child-ENABLES per body per frame (staged upgrades)
        // ── CONTACT DECIMATION (2026-07-10, Diag.cpp contact listener): keep only 1 NEW contact
        // point in N on NPC-involved dynamic pairs (bit-15 body present, NO fixed body — ground
        // contacts stay full so ragdolled NPCs never sink). The user's "1 collision out of 10"
        // test. -1 = follow preset (50 -> keep 1-in-2, 80 -> 1-in-5, 90 -> 1-in-10); 0 = off;
        // else N directly. Disabled points get hkContactPointMaterial kIsDisabled (solver skips).
        float perfContactKeep  = -1.f;
        // ── PIVOT DESCALE (2026-07-09): undo the engine's pivot-x-GetScale at NIF load (probe-proven:
        // loaded = baked x 0.9514 on scale-0.9514 Carmella; scaled NPCs' joints chain-drift 1-4u without
        // it). Once per constraint instance, all 17 joints incl. left.
        // ★ DEFAULT FLIPPED 0 (OFF) 2026-07-14: SUPERSEDED by pivReScale (the uniform re-scale system).
        // Descale drove EVERY non-scale-1 NPC's pivots to baked scale-1 — which is WRONG for a correctly
        // scaled NPC (GetScale == real skeleton scale): it pushed her joints OFF her own nodes. The new
        // re-scale system leaves those correctly-scaled NPCs byte-for-byte untouched and only re-scales
        // the genuinely mis-scaled few. Its call is REMOVED from the pipeline; this knob is now inert
        // reference (re-enabling it would fight pivReScale's baked×GetScale starting point).
        float pivDescale       = 0.f;
        // ── UNIFORM RE-SCALE (2026-07-14, PivScaleCorrect): the authoritative mis-scale fixer. Every NPC
        // gets a scale-1 baked Havok body scaled to their RECORD scale by the engine — right ~98% of the
        // time (do nothing). The ~2-3% whose record scale != real XP32 scale get the wrong-size body; a
        // 3-point check (anchor=COM, head, calf: Havok joint distance vs XP32 node distance, 0.5u trigger)
        // finds them and ONE uniform factor (trueScale/GetScale) scales the WHOLE ragdoll's constraint
        // pivots (both copies of every joint, L+R) so the Havok distances match the XP32. Capsules ride
        // CapFix's effScale in lock-step. One-shot per ragdoll build; gated by 10 m distance, grab,
        // furniture/ragdoll, and 5 s->2 min loosened backoff. Default ON — a correctness fix.
        float pivReScale       = 1.f;
        // pivReScaleApply 0 = DIAGNOSTIC-ONLY: PivScaleCorrect still measures + logs (ARCDIAG/NAM6DIAG) but
        // SKIPS the pivot write. Lets the arc-sum pose-invariance test run without the buggy single-frame
        // re-scale touching the NPC. Default 1 = normal.
        float pivReScaleApply  = 1.f;
        // ── JOINT-TRACK TRIGGER (2026-07-10): tuning-knob trigger for the read-only `jtrack` joint
        // audit (PivJointTrackDump) — the console donors were all taken in this load order (PPB.log:
        // "Console-command donors for 'jtrack' all missing"), so the knob is the working trigger.
        // EDGE-TRIGGERED like handBoxDumpNow: 0 -> non-0 arms a ~2 s window in which every driven
        // actor's pre-drive tick dumps ONCE (nearby NPCs all dump), then disarms until the next edge.
        float jtrackNow        = 0.f;
        // refCapNow: DETERMINISTIC one-shot measured-scale reference read (2026-07-14). Flip >0.5 while
        // standing by the reference actor (Lydia) — reads her COM/head/calf nodes NOW and logs the exact
        // scale-1 spans to hardcode (bypasses the flaky attached/10-sample sampler). Set back to 0 after.
        float refCapNow        = 0.f;
        // ── XP32 POSE-CONFORM (2026-07-10, PPBHook.cpp prep + Hooks.cpp 0xA26C05 consume): the drive
        // pose the inner hkaRagdollRigidBodyController::driveToPose call receives is built from
        // ANIMATION-DATA translations while the RENDERED skeleton uses the XP32 NIF's node locals —
        // the delta rotates with pose. The conform overwrites the drive pose's per-bone TRANSLATIONS
        // with what the live XP32 node chain implies (rotations stay animation-owned) so the ragdoll
        // follows the visible body in EVERY pose. All PK_NOSNAP (per-frame reads / edge-detected —
        // must never re-dress the capsule slots).
        // poseConform: 0 = off (first-session default — validate the math with the dump before
        // trusting the write). 1 = overwrite translations each drive.
        float poseConform       = 0.f;
        // poseConformDump: edge-triggered like jtrackNow (0 -> non-0 arms a ~2 s window; every driven
        // actor logs ONE per-bone incoming-vs-XP32 comparison at the inner hook, then disarms until
        // the next edge). Works with poseConform 0 (read-only diagnostic) or 1 (dump shows pre-write).
        float poseConformDump   = 0.f;
        // poseConformRoot (2026-07-12): ALSO add a captured ROOT-height delta to the drive.
        // ★ RETIRED 2026-07-13 — DEFAULT 0, DO NOT RE-ENABLE. The original theory (a uniform
        // ~4.3u Z offset from the drive root riding the vanilla rig height vs XP32's COM bind)
        // was DISPROVEN by the live joint-track test on Lydia (the scale-1 master):
        //   poseConformRoot 1 -> meanGapChild 3.76u / max 8.02u, and she floats ~2u off the floor;
        //   poseConformRoot 0 -> meanGapChild 0.21u / max 0.30u (joints ON the XP32 nodes), on the floor.
        // The per-bone conform (poseConform) already places every bone correctly RELATIVE to a
        // root sitting at the animation's own (feet-on-floor) height; adding a root lift on top
        // just shoves the whole correctly-built chain up. Worse, the delta is captured ONCE and
        // is only ~0 if captured in BIND pose — captured mid-animation (the normal case) it
        // latches a spurious lift (why it "worked" on statued Carmella but broke live Lydia).
        // Left as a knob (not deleted) purely as an escape hatch if a future scaled-NPC case
        // ever proves it needed; re-validate with jtrack BEFORE trusting it. 0 = OFF (correct).
        float poseConformRoot   = 0.f;
        // poseConformEveryN: recompute the XP32 chain compositions every Nth drive per actor and reuse
        // the cached translations between. The deltas move only with pose, so N=2-4 trades <=N frames
        // of target lag (at 90 Hz, imperceptible under the PD drive's own servo lag) for 1/N of the
        // compute — the built-in mitigation lever if the ~50us/actor-frame budget is ever exceeded.
        // 1 = recompute every frame (default).
        float poseConformEveryN = 1.f;
        // ── NPC FINGER TEST v2 (2026-07-09, NpcFingerTest.cpp; FINGER_BUILD_PLAN.md): 4 runtime
        // DYNAMIC capsules on ONE NPC's right-hand finger chords, velocity-driven onto the live
        // bone, colliding only with the player's HIGGS hand/wielded weapon. All PK_NOSNAP (read
        // live per frame — must never re-dress the 12 capsule slots). npcFinger* names because
        // fingerCap* is TAKEN by GrabDiag::FingerCapTrack (round-1 F6).
        float npcFingerEnable  = 0.f;    // 0 off; 1 = attach to the NEAREST driven NPC (console `nfing` pins the SELECTED one)
        // ── TAIL TEST (2026-07-11, NpcFingerTest tail mode): same rig on the fluffy-tail
        // SMP bones (HDTS TailBone02..09.001, 4 chords). Tail wins when both knobs are on;
        // `nfing` pins the target NPC in this mode too. See 06_BoneFollow research.
        float npcTailTest      = 0.f;    // follower mode: 0 off / 1 tail / 2 wig(AmberLights) / 3 RETIRED (was DXNecro dress)
        float npcTailR         = 1.2f;   // LEGACY (2026-07-13): garment capsule radii now come from the
                                         // per-table TuneOf() dial numbers in NpcFingerTest.cpp — this
                                         // knob has NO consumer left (like fsmpPushForce/fsmpPushMaxForce).
        float npcTailOffScale  = 1.f;    // fur-center offset scale (M'rissi HDT table only): 1 = measured
                                         // per-bone fur-centroid offsets (capsules ride the fur line);
                                         // 0 = capsules on the bone axis (pre-2026-07-12 behavior)
        float nodeCensusNow    = 0.f;    // EDGE 0->1: dump every *ail*-named node on the pinned/nearest actor
        // -- FSMP PUSH (2026-07-11 stage 2, FsmpLink.cpp): capsule-sensor -> SMP-force actuator.
        // When a hand/glove/weapon displaces a tail capsule off its chord, the displacement is
        // published and the PreStep listener applies fsmpPushForce x displacement (game-unit
        // force, clamped) to the position-matched SMP bone rigid. SMP gravity on a 0.3-mass
        // bone is ~200 force units, so meaningful pushes live in the 50-400 band.
        float fsmpPush         = 0.f;    // 0 off / 1 on (needs FSMPLINK ACCEPTED in the log)
        float fsmpPushForce    = 40.f;   // LEGACY (2026-07-13): unused by the per-target path —
                                         // gain now rides each PushTarget (TuneOf per-table numbers)
        float fsmpPushMaxForce = 400.f;  // LEGACY (2026-07-13): unused by the per-target path
        float fsmpPushMinDispU = 0.3f;   // deadzone: ignore sub-jitter displacements (still global)
        float fsmpPushMult     = 1.f;    // GLOBAL multiplier (clamped 0-10) on the per-table push
                                         // gain AND clamp (TuneOf in NpcFingerTest.cpp) — one dial
                                         // scales every garment's push feel proportionally
        float npcFollower      = 1.f;    // master switch for the always-on garment rigs: auto-probe
                                         // every driven NPC for tables 1-3 (tail/foxtail/wig; dress
                                         // RETIRED 2026-07-13); 0 destroys all auto-probed garment rigs
        float npcFingerOwnBody = 0.f;    // AIHANDS MODE (2026-07-12): 0 = fingers collide with HIGGS
                                         // hand/weapon only (the original test); 1 = fingers ALSO rest on
                                         // the NPC's OWN body (force-collide vs own ragdoll flesh, arm
                                         // bodies excluded) and on world statics/clutter (vanilla row) —
                                         // the PPB half of AIHands' contact-driven finger curl.
        float npcFingerFollow  = 0.8f;   // TOUCH WRITE-BACK (2026-07-13, user: "the capsule moved but
                                         // the finger didn't"): fraction of the capsule's contact
                                         // displacement written onto the finger BONES each frame
                                         // (tip full share, knuckle half; clamped 2u; 0 = off).
                                         // Animation re-poses fingers every frame, so this is a
                                         // per-frame decoration — no feedback accumulation.
        float npcFingerCount   = 1.f;    // 1..4 fingers rigged, in kFingerPairs order (index first). DEFAULT 1 = the
                                         // MANDATORY Risk-1 spike (dynamic creation via the engine ctor is unproven at
                                         // 80% — verify ONE capsule for 5 min, then raise to 4). Applied at rig creation.
        float npcFingerR       = 1.0f;   // capsule radius, GAME units. review2 note 10: the spec's 0.5 seed sat AT the
                                         // proven tunneling floor — start in the 1.0-1.25u band, shrink after TRACK passes
        float npcFingerTipU    = 1.3f;   // chord extension past Finger12 toward the missing tip node, game units
        float npcFingerMassKg  = 0.15f;  // dynamic mass; inertia derived (solid capsule, COM moved to the capsule center)
        float npcTailMassKg    = 2.0f;   // 2026-07-15: MEATY mass for TAIL capsules (tbl 1/2 fluffy/foxtail).
                                         // 0.05 (hair) is too light for a tail — it flies + can't hold a
                                         // push. Dial for feel/drag; recreate the rig (reload/reequip) to apply.
        // ── CONTACT-DRIVEN FINGER CURL (2026-07-16): capsule displacement -> the finger BONES curl
        // to conform (AIHands FP_Fist shares 1.40/1.50/1.30 rad x curl, rotations dual-written at the
        // pre-drive seam). Leaky integrator: curl rises while the capsule is displaced, relaxes after.
        float npcFingerCurlGain  = 2.5f; // integrator gain (/u/s): ~0.4 s to full curl under a 1u press. 0 = curl OFF.
        float npcFingerCurlDecay = 1.5f; // relax rate (/s) once contact ends (~0.7 s back to open)
        float npcFingerCurlMax   = 1.0f; // curl ceiling (1 = full fist share; 0.5 = half-curl max)
        float npcFingerCurlMode  = 0.f;  // 0 = ADDITIVE (deflects whatever the animation pose is — default)
                                         // 1 = BIND-RELATIVE (grasp override, replaces the animation's finger pose)
        float npcFingerCurlLagGate = 25.f; // servo-lag gate (u/s): displacement only CHARGES curl while the
                                         // finger chord moves slower than this (a swinging arm's lag is NOT
                                         // contact — the ledger's "displacement sensor reads motion" trap).
                                         // A HIGGS hand within 20u bypasses the gate (deliberate touch during
                                         // her motion still curls). 0 = gate off (charge on any displacement).
        // ── FINGER COLLISION WIDENING (2026-07-16, live A/B knobs — finger rig only). Default OFF:
        // statics are infinite-mass (a leaned-on table can flicker-oscillate the servo — the documented
        // reason they were excluded). Flip live to test; the curl system reduces the flicker window by
        // conforming the finger away from sustained contact.
        float npcFingerVsClutter = 0.f;  // 1 = collide with clutter/props/dropped weapons/debris (layers 4/5/6/19/20)
        float npcFingerVsWorld   = 0.f;  // 1 = collide with world statics/ground (layers 1/2/10/14/17) — flicker risk
        float npcFingerVsNpc     = 0.f;  // 1 = collide with OTHER NPCs' ragdolls (layers 8/32/33, different group)
        float fsmpMassScale    = 1.f;    // 2026-07-17 mass-scaled push force: F = gain x disp x (boneMass/0.4).
                                         // Base bones (0.4 kg, what the 12000 was dialed on) keep the exact
                                         // dialed feel; light tip bones self-derate (the tip-jitter fix).
                                         // 0 = off (raw gain on every bone). Once ON, fsmpPushMult returns to 1.0.
        float fsmpContactGate  = 1.f;    // 2026-07-17 contact-VERIFIED push gate (the "floaty tail" fix): publish
                                         // force only while the capsule's predicted-vs-actual motion deviates
                                         // (= something physically obstructed the hard-keyframe command). The
                                         // proximity gate alone stays open the whole time a hand is within 20u,
                                         // feeding servo lag into the push as drag on the tail's OWN motion.
                                         // 0 = legacy proximity-only publish (A/B the floatiness live).
        float fsmpContactDevU  = 0.8f;   // deviation threshold, game units/frame. The check is dt-FREE (actual
                                         // position vs the commanded landing position), so frame-time jitter
                                         // contributes ~nothing; residual noise is damping-level. Lower = more
                                         // sensitive; above ~2 a slow gentle press latches late.
        float fsmpContactHoldMS = 200.f; // contact latch hold (ms, WALL-clock) after the last verified deviation —
                                         // keeps the force continuous through the intermittent contact of a
                                         // moving push. Under slow-motion (sgtm < 1) the hold covers
                                         // proportionally less game-time — raise it if pushes pulse in slow-mo.
                                         // Clamped 0..5000 at read.
        // ── 2026-07-18 ROUTE B: MEASURED trueShape (Report 21 "ROUTE B GREENLIT") ──────────────
        // Per-region girth sampled from the LIVE morphed body mesh, ratioed against a captured
        // neutral. meshShape 1 + ALL seven meshNeutral* captured -> measured mode replaces the
        // slider vocabulary (custom bases like Sofia/M'rissi finally measurable). Bands are
        // FRACTIONS of the body-mesh z-bounding-box (ankles≈0, neck≈1), live-dialable during the
        // Redguard calibration session. meshShapeDump: change value -> re-sample + MESHGIRTH logs.
        float meshShape        = 0.f;    // master (OFF until the neutral capture session)
        float meshShapeDump    = 0.f;    // edge-triggered re-latch + verbose girth dump
        float meshMarkers      = 0.f;    // 1 = place visible ghost capsules on the girth verts
                                         // at every latch (band-placement verification by eye)
        float meshBoneTest     = 0.f;    // EDGE: skin-weight bone-anchor + 12-angle girth DIAGNOSTIC
                                         // (Path-B test 2026-07-20; BONETEST log lines; moves nothing)
        // ── ReShape v2 ("Path B", 2026-07-21): BONE-ANCHORED measurement replaces the bbox-fraction
        // band sampler. boneShape 1 = v2 live (legacy sampler stays compiled, just unused — flip to
        // 0 to revert instantly). Neutrals are LYDIA's bone-anchored values (she IS neutralShape);
        // ONE neutral for every body type — no per-body fingerprint tables in v2 by design.
        // 0 = UNCAPTURED -> that region stays inert (ratio 1.0) until captured from Lydia.
        float boneShape          = 1.f;
        float boneNeutralChest   = 10.44f;  // captured from Lydia zero-slider 2026-07-21
        float boneNeutralBreasts = 0.f;     // UNCAPTURED: anchor-protrusion measure is new
        float boneNeutralBelly   = 8.46f;
        float boneNeutralWaist   = 8.62f;
        float boneNeutralButt    = 0.f;     // UNCAPTURED: anchor-protrusion measure is new
        float boneNeutralThighs  = 7.37f;
        float boneNeutralArms    = 0.f;     // UNCAPTURED: XY radial is invalid on diagonal A-pose arms
        float boneNeutralCup     = 0.f;     // UNCAPTURED: mound z-extent measure is new
        float boneNeutralBreastZ = 0.f;     // UNCAPTURED: mound-height (SAG) neutral, spine2-relative
        // ★ v2.3 ABSOLUTE-POSITION neutrals (bone-local mound Y). These drive the SHIFT; the
        // wall-relative ones above drive SIZE. 0 = uncaptured -> that shift stays off.
        float boneNeutralBreastAbs = 0.f;
        float boneNeutralButtAbs   = 0.f;
        // ★ LIVE UV LANDMARKS (2026-07-22): the landmark table used to be a compiled constexpr, so
        // every by-eye correction cost a rebuild. These are the same values as live knobs — edit +
        // pulse and the landmark moves. Defaults = the validated table; belly V carries the user's
        // 2026-07-22 correction (0.75465 -> 0.82031, the old point sat too high).
        float lmNipU   = 0.424242f, lmNipV   = 0.698242f;
        float lmBrUpU  = 0.424242f, lmBrUpV  = 0.662100f;
        float lmBrDnU  = 0.424242f, lmBrDnV  = 0.740200f;
        float lmChestU = 0.500000f, lmChestV = 0.705550f;
        float lmBellyU = 0.500000f, lmBellyV = 0.820310f;
        float lmWaistU = 0.500000f, lmWaistV = 0.878900f;
        float lmButtU  = 0.444330f, lmButtV  = 0.146480f;
        // LIMB pairs (2026-07-23): two points per limb; the DISTANCE between them is the girth
        // chord. Arms/legs grow in radius only, so a chord is the whole measurement.
        float lmLegAU  = 0.278320f, lmLegAV  = 0.781250f;
        float lmLegBU  = 0.112300f, lmLegBV  = 0.781250f;
        // HEAD nose landmark (2026-07-23). PLACEHOLDER UV — awaiting the user's read off
        // femalehead.dds. Facegen sculpts move vertices, never UVs, so one nose UV is the
        // same anatomical point on every human head (incl. High Poly Head — same layout).
        float lmNoseU  = 0.480950f, lmNoseV  = 0.468750f;   // user-read 2026-07-23
        float lmChinU  = 0.484400f, lmChinV  = 0.695800f;   // user-read 2026-07-23
        float lmArmAU  = 0.244140f, lmArmAV  = 0.561520f;
        float lmArmBU  = 0.244140f, lmArmBV  = 0.419920f;
        // ★ LANDMARK BUTT FIT (2026-07-22): the cheek capsules are PLACED from the UV butt landmark
        // instead of ridden by a girth-derived shift. Neutral = Lydia's landmark pushed lmButtOutU
        // outward (she measured 0.6u inside her own skin — a pre-skin-vs-posed bias, same on every
        // body, so it cancels in the delta and only anchors the neutral). Every other NPC's cheek is
        // translated by how far HER landmark sits from that anchor — full 3D, so width, depth AND
        // height all follow the actual flesh.
        //   OWN SWITCH ON PURPOSE: bodyScale/meshShape must never silently enable or freeze this.
        //   That exact coupling (meshShape 0 handing the cheeks to the slider fallback) is what made
        //   a single placed probe read 1u OUT on Sofia and 0.7u IN on Lydia at the same time.
        float lmButtFit   = 1.f;   // float like every other switch — the knob table is float-only
        float lmButtOutU  = 0.6f;
        // PROPORTIONAL gain on the cheek DISPLACEMENT (not a flat push). At the neutral shape
        // the delta is ~0, so this cannot disturb a zero-slider body no matter its value — it
        // only damps how hard extreme presets throw the cheeks out. A flat lmButtOutU change
        // moves EVERY body and is the wrong lever for a high-end overshoot.
        float lmButtPosGain = 0.482f;
        float lmButtNeutX =   7.08f;   // Lydia butt_cheek landmark (6.75,-10.24,5.23), COM-local,
        float lmButtNeutY = -10.74f;   // pushed 0.6u along its own outward radial (+X/-Y)
        float lmButtNeutZ =   5.23f;
        // ★ NEUTRAL SHAPE (2026-07-23) — LYDIA's UV landmarks, bone-local, scale-free base-mesh
        // units. THE reference body: every other NPC's ReShape is an interpolation away from
        // these. Live knobs so the neutral can be re-captured without a rebuild.
        // Full record + provenance: tools/ppb-scratch/neutral_shape_lydia.txt
        float lmNeutNipX   =  7.240f, lmNeutNipY   = 13.000f, lmNeutNipZ   =  5.520f;
        float lmNeutBrUpX  =  4.460f, lmNeutBrUpY  = 10.370f, lmNeutBrUpZ  =  9.620f;
        float lmNeutBrDnX  =  7.290f, lmNeutBrDnY  = 11.810f, lmNeutBrDnZ  =  1.060f;
        float lmNeutChestX =  0.000f, lmNeutChestY = 12.090f, lmNeutChestZ =  4.020f;
        float lmNeutBellyX =  0.000f, lmNeutBellyY = 11.010f, lmNeutBellyZ =  1.400f;
        float lmNeutWaistX =  0.000f, lmNeutWaistY = 10.850f, lmNeutWaistZ =  2.680f;
        float lmNeutButtX  =  6.750f, lmNeutButtY  =-10.240f, lmNeutButtZ  =  5.230f;
        // LIMB neutrals are a CHORD, not a point: arms and legs grow in radius only, so the
        // distance across the limb at a fixed band is the entire measurement. Lydia zero-slider.
        float lmNeutLegChord = 11.382f;
        float lmNeutArmChord =  6.437f;
        // HEAD neutral (2026-07-24): Lydia's nose->chin distance from the disk-facegen pipeline.
        // The Faralda proof: her sculpt measures 0.914 of this while her race scale is 1.08 —
        // the sculpt visually CANCELS the race scale, so only the mesh ratio knows her true
        // face size (es alone made her head capsules ~9% oversized, the original complaint).
        float lmNeutNoseChin = 5.161f;
        float lmGainHead     = 1.f;
        // ══ DISMEMBER GUARD (2026-07-26, DF/NGD ↔ PLANCK) ═════════════════════════════════════
        // See DismemberGuard.h. dgEnable = master; dgGraceDeadS = how long a fresh corpse stays
        // PLANCK-ignored (the killing-blow surgery window); dgLog = per-action log lines.
        float dgEnable      = 1.f;
        float dgGraceDeadS  = 15.f;
        float dgLog         = 1.f;
        float dgHeadTrack    = 1.f;   // log head-actor creation during decap (race-swap research)
        // DEATH-CONFIRMED DISMEMBERMENT (replaces DF's worker-thread confirm; needs DF's own
        // bDeferredHitProcess = 0 so no DF worker exists and the VR deadlock cannot happen).
        float dgDeathCut       = 1.f;   // master: ask DF to dismember once death is confirmed
        float dgDeathCutDelayS = 0.20f; // wait after death before asking (lets the ragdoll settle)
        float dgDeathNodeTries = 1.f;   // how many limb nodes to offer DF per death (1 = natural)
        float dgHitLocated     = 1.f;   // choose the limb nearest the killing blow (0 = rotation)
        float dgHitMaxDistU    = 45.f;  // accuracy gate: blow must land within this of a limb's
                                        // span or nothing is severed (0 = no gate)
        // HEAD SKELETON SWAP: give the severed head a skeleton whose only collision is the head
        // (PPB\<skeleton>_head.nif) instead of the victim's full body bake.
        float dgHeadSkel       = 1.f;   // master switch
        float dgHeadSkelHoldS  = 3.0f;  // max seconds to hold the swap while the head loads 3D
        float dgHeadStripHair  = 1.f;   // unequip hair-slot armor (SMP wigs) from a severed head —
                                        // wig strands intercept HIGGS's palm-triangle pick
        float dgHeadPlanck     = 0.f;   // 1 = do NOT PLANCK-ignore severed-head clones (grab test)
        float dgHeadGrabFix    = 1.f;   // clear the severed head's anchor ragdoll sub-layer so
                                        // HIGGS's hand can contact it (Report 09 §6)
        float dgHeadPark       = 1.f;   // park every severed-head body ON the COM anchor + cut its
                                        // constraints (per-actor, safe: bodies are not shared)
        float dgDeferDf      = 1.f;   // DF FREEZE FIX (2026-07-27): hook ProcessDismemberment and
                                      // marshal off-thread cuts to the main thread. 0 => no
                                      // foreign-DLL patch (and the VR dismember freeze returns).
        // v2 (2026-07-26): dgDeathIgnore OPT-IN — the death-instant PLANCK ignore starved DF
        // of the victim's ragdoll (3/3 dismember declines); default OFF, flip to 1 only if
        // the NPC-vs-NPC freeze reproduces without it (then capture the dump). dgCloneStrip:
        // reduce NGD head-clones to their COM body so the severed head is grabbable/throwable.
        float dgDeathIgnore = 0.f;
        float dgCloneStrip  = 1.f;
        float dgStripDelayS = 3.f;
        // ══ ReTouch GHOST ZONES + MOUTH TOUCH (2026-07-24, Path B test) ═══════════════════════
        // Detection is virtual (recomputed from live transforms every frame — zero Havok cost,
        // tracks the body exactly). Markers refresh ~4 Hz for dialing only.
        float ghostZones    = 1.f;    // master switch for the whole ghost/touch layer
        float ghostViz      = 1.f;    // visual markers on the ghost zones (dial aid)
        float ghostRangeU   = 250.f;  // only the nearest driven NPC within this range is tracked
        float ghostBreastPadU = 1.5f; // breast ghost = live C11/C12 capsule + this pad (skin gap)
        float ghostButtPadU   = 1.0f; // butt   ghost = live C16/C17 capsule + this pad
        // mouth ghost capsule, HEAD BODY frame (dial by eye; behind the lips)
        float mouthGhostAX = -1.2f, mouthGhostAY = 8.6f, mouthGhostAZ = 0.2f;
        float mouthGhostBX =  1.2f, mouthGhostBY = 8.6f, mouthGhostBZ = 0.2f;
        float mouthGhostR  =  1.0f;
        float mouthEnterU  =  0.5f;   // finger closer than this -> IN the mouth
        float mouthExitU   =  1.5f;   // hysteresis: must leave past this to count as out
        float mouthPhonemeIdx = 11.f; // facegen phoneme driven (11 = Oh; 12 = OohQ alternative)
        float mouthPhoneme2Idx = 1.f; // second blended phoneme (1 = BigAah: drops the jaw)
        float mouthPhoneme2Max = 0.5f;// how far the jaw opens in the blend
        float mouthNeedFinger = 1.f;  // gate requires the INDEX boxes to be the nearest hand part
        float mouthDeepChinU = 2.5f;  // deep hold: both chin lines within this
        float mouthDeepPalU  = 5.0f;  // ...and the palate within this loose range
        // THROAT (2026-07-26, user: "she closed her mouth as my finger was at the back"):
        // a dormant wave-2b head child repositioned as the END-OF-MOUTH wall. Near it =
        // unambiguously deep — holds the mouth open alone, and raises the THROAT event
        // ("something reaching the end of her throat"). Child index is a knob so the wall
        // can move to another spare child if a custom skeleton uses C10 differently.
        float mouthThroatChild = 10.f; // which head child is the throat wall (default C10)
        float mouthThroatU     = 1.5f; // tip within this of the throat wall = reached the end
        // BEAST MOUTH (2026-07-26, TOUCHPROBE-mapped by the user on spawned Khajiit+Argonian):
        // beast heads have no palate/throat children we control (v4 gates all head knobs off),
        // so the gate ANDs their own BAKED face capsules — user design: "if all three detect
        // the finger box, open the mouth". Two different bakes = two index sets. Deep hold =
        // both chins (no palate condition, no throat event on beasts). All live-dialable.
        float mouthKhaE1 = 6.f,  mouthKhaE2 = 7.f,  mouthKhaE3 = 8.f;   // Khajiit muzzle trio
        float mouthKhaC1 = 2.f,  mouthKhaC2 = 3.f;                      // Khajiit chin pair
        float mouthArgE1 = 1.f,  mouthArgE2 = 6.f,  mouthArgE3 = 8.f;   // Argonian lip + muzzle bar + end
        float mouthArgC1 = 2.f,  mouthArgC2 = 4.f;                      // Argonian chin pair
        float mouthBeastEnterU = 2.5f; // beast entry gate (muzzle capsules sit above the mouth line)
        float mouthPhonemeMax = 0.9f; // how wide the O opens (0..1)
        float mouthRampIn  = 5.f;     // phoneme units/sec opening
        float mouthRampOut = 3.f;     // closing
        float mouthFingerBox = 1.f;   // which HandBox box is the pointing fingertip (0..3)
        float touchProbe   = 1.f;     // TOUCHPROBE mapping logs (nearest capsule per fingertip)
        float touchProbeRangeU = 3.f; // only log when closer than this
        // ══ UV ReShape (2026-07-23) — THE shape system. Replaces the girth/slider generation ══
        // Master switch. The old bodyScale/meshShape pair is GONE: one measurement source (UV
        // landmarks), one response path, no fallback that can silently take over.
        float lmReShape = 1.f;
        // RING CHANNELS — chest/belly/waist. Each is a pure radial ratio of the NPC's landmark
        // distance-from-its-node against Lydia's. Radius and radial position scale together;
        // Z is never touched (height belongs to ReScale). Gain 1.0 = capsule follows the flesh
        // exactly; these exist only so a channel can be trimmed by eye without a rebuild.
        float lmGainChest = 1.f;
        float lmGainBelly = 1.f;
        float lmGainWaist = 1.f;
        float lmGainButt  = 1.f;    // butt cheek RADIUS (position comes from the landmark fit)
        float lmGainThigh = 1.f;    // leg  RADIUS only - endpoints never move (user spec)
        float lmGainArm   = 1.f;    // arm  RADIUS only
        float lmClampLo   = 0.70f;  // sanity bounds on any ring ratio
        float lmClampHi   = 1.60f;
        // BREAST CHANNEL — fully COMPUTED, not knob+delta. Fitted 2026-07-23 from the two
        // bodyScale-0 anchors (M'rissi, Sofia) and validated on Lydia as a holdout: the model
        // put her front point +1.10 vs baked where she had been hand-corrected to +0.98.
        //   A.Y,B.Y = f(breastDistance)   A.Z,B.Z = f(sag)   R = f(cup)   B.X = lateral rule
        float lmBrAYc =  3.3588f, lmBrAYm = 0.2065f;
        float lmBrBYc =  8.1069f, lmBrBYm = 0.5688f;
        // VERTICAL, reworked 2026-07-23 (the Imperial finding): sag has TWO kinds. The scalar
        // (nipple vs mound-mid) only sees INTERNAL droop-shape and is blind to the whole mound
        // sliding down the chest — the Imperial's mound fell 0.87u while the scalar moved 0.03.
        // So the channel is now tilt + height:  Z = c + m*sag + hK*(moundMidZ - midZ0).
        // Constants solved exactly through the three DIRECT eye verdicts (neutral Lydia,
        // Faralda "perfect", Imperial "1u too high"). M'rissi/Sofia anchors came through the
        // fallback-shift arithmetic era — they deviate +0.7/-0.95 and need re-verification.
        float lmBrAZc =  9.9623f, lmBrAZm = 1.3371f;
        float lmBrBZc =  9.8965f, lmBrBZm =-0.7820f;
        float lmBrHK  =  0.9940f;   // mound-height gain: the capsule follows the flesh ~1:1
        float lmBrMidZ0 = 5.3300f;  // Lydia zero-slider mound-mid Z (spine2-local)
        // SATURATION (extreme-preset guard): drivers are CLAMPED to the calibrated range so a
        // correction made at an extreme can never reshape the middle of the range again.
        //   cupSat from the approved extreme radius 3.148; sag range = anchors + margin.
        float lmBrCupSat = 10.40f;
        float lmBrSagLo  = -0.60f;
        float lmBrSagHi  =  1.00f;
        float lmBrRc  = -0.6418f, lmBrRm  = 0.3645f;
        float lmBrAX  =  2.353f;    // capsule root X — never dialled on any anchor
        float lmBrLatK=  0.25f;     // lateral = lmBrLatK * (B.Y - lmBrLatY0), user's 25% rule
        float lmBrLatX=  4.904f;    // Lydia front-point X
        float lmBrLatY0= 8.788f;    // Lydia front-point Y
        // ★ TIP PROBE (2026-07-21): live mesh-space offset applied to the BREAST tip marker so the
        // user can walk the ghost capsule onto the REAL nipple by eye. Once parked, PPB scans the
        // vertices at that point and reports what is actually there — measured, not inferred.
        // Mesh space: +X = her left, -Y = FORWARD, +Z = up.
        float tipProbeX = 0.f;
        float tipProbeY = 0.f;
        float tipProbeZ = 0.f;
        // ★ UV LANDMARK PROBE: set a target UV and PPB drops the ghost capsule on the vertex whose
        // UV is nearest it — on EVERY body. CBBE/3BA/Softbody share one UV layout, so if this lands
        // on the nipple of all three, that UV is the universal landmark key. 0 = off.
        float nipUVu = 0.f;
        float nipUVv = 0.f;
        float meshNeutralChest   = 0.f;  // 0 = uncaptured (measured mode stays inert)
        float meshNeutralBreasts = 0.f;
        float meshNeutralBelly   = 0.f;
        float meshNeutralWaist   = 0.f;
        float meshNeutralButt    = 0.f;
        float meshNeutralThighs  = 0.f;
        float meshNeutralArms    = 0.f;
        float meshBandChestLo  = 0.80f;  float meshBandChestHi  = 0.90f;
        float meshBandBreastLo = 0.76f;  float meshBandBreastHi = 0.90f;  // FRONT half of the ring
        float meshBandBellyLo  = 0.64f;  float meshBandBellyHi  = 0.74f;
        float meshBandWaistLo  = 0.56f;  float meshBandWaistHi  = 0.64f;
        float meshBandButtLo   = 0.48f;  float meshBandButtHi   = 0.60f;  // REAR half of the ring
        float meshBandThighLo  = 0.30f;  float meshBandThighHi  = 0.46f;  // per-side
        float meshBandArmLo    = 0.78f;  float meshBandArmHi    = 0.95f;  // per-side, |x| beyond meshArmFrac
        // ── 2026-07-19b GEOMETRIC RESPONSE (the Sofia/M'rissi ladder lessons) ──────────────────
        float meshRadiusGain   = 0.6f;   // radius follows the widest-point ratio DAMPED: applied =
                                         // 1 + (measured−1)×gain (Sofia: honest 1.37 chest ratio
                                         // was ~2u too much capsule at 1:1)
        float meshShiftButtK   = 1.0f;   // cheek back-shift, u per u of measured protrusion delta
                                         // (replaces the slider-era net inversion that turned
                                         // Sofia's real 1.77u into an 8.2u shift)
        float meshShiftBreastFwdK = 1.0f;   // breast forward, u per u of protrusion delta
        float meshShiftBreastZK   = 1.0f;   // breast VERTICAL, u per u of measured front-mass
                                            // height delta — sag goes DOWN because it MEASURES down
        float meshNeutralBreastZ  = 0.f;    // neutral breast front-mass height (u above mesh bottom)
        float meshNeutralBreastCup = 0.f;   // neutral CUP vertical extent (u) — 0 = radius falls
                                            // back to the protrusion ratio until captured
        float meshCupFrac          = 0.6f;  // MOUND threshold: cup counts verts with protrusion
                                            // >= frac x near-max (the wall saturated the old cup)
        float meshArmFrac      = 0.55f;  // arm/torso x-split, fraction of bbox half-width (review
                                         // 2026-07-18: 0.35 clipped wide ribcages/hips; the split
                                         // is also z-gated to zn>0.55 so thighs never misclassify)
        float npcBodyMat       = 3.f;    // 2026-07-18 BODY capsule touch-sound material (all 18 slots incl.
                                         // list children): same selector as npcGarmentMat (0 skin/NIF,
                                         // 1 cloth, 2 snow, 3 grass, 4 none). Default grass = the proven
                                         // direction-proof near-silent choice — kills the body-fall thud
                                         // on feet-drag / NPC-vs-NPC / hand-touch now that ragdolls are
                                         // always on. NOTE death-fall thuds go quiet too (same bodies).
        // ── 2026-07-18 MORPH-DRIVEN TRANSLATION (Report 21): displacement capsules MOVE with the
        // region morph instead of inflating. u per morph unit, zero-scale bone-local (es-scaled).
        float bodyShiftBreastsUpU  = 1.25f;  // breasts: +2u UP at +1.62 morph (user-dialed 07-17)
        float bodyShiftBreastsFwdU = 0.6f;   // breasts: forward(+Y) share — UNCALIBRATED seed, dial in VR
        float bodyShiftButtBackU   = 7.1f;   // butt: 2.5u BACK at +0.35 morph (user-dialed 07-17)
        float bodyScaleLoBoost     = 2.f;    // shrink-side factor multiplier (net<0): petite meshes shrink
                                             // more than the grow-side factors predict (07-18 in-VR verdict)
        float npcGarmentMat    = 3.f;    // 2026-07-16 garment (hair/tail/dress) capsule touch-sound material:
                                         // 0 skin (the old body-fall THUD), 1 cloth (soft swish IF the engine
                                         // keys our set), 2 snow (direction-proof whump), 3 grass (direction-
                                         // proof rustle, DEFAULT), 4 none. Applied at rig CREATE — change the
                                         // knob then flip npcFollower 0 -> 1 to rebuild rigs with the new sound.
        float npcSilentVelMS   = 0.f;    // 2026-07-15 silent-contact drive-velocity cap (m/s) while a hand/
                                         // weapon is near a garment chord. DEFAULT OFF (0) — the 0.15 trial
                                         // fed servo lag into the FSMP push gate and dragged/whipped the SMP
                                         // tail (Report 15 post-mortem). Re-test live: needs < 0.2 (HIGGS
                                         // haptic gate) to silence; <= 0.01 disables the clamp entirely.
        float npcGarmentMassKg = 0.05f;  // 2026-07-14: LIGHT mass for garment (tail/hair/dress) capsules — a
                                         // heavy finger mass ×11-15 capsules dragged the SMP cloth down. Light
                                         // = a touch-surface that doesn't weigh on the hair. Floored at 0.01.
        float npcFingerAlpha   = 0.6f;   // velocity-drive stiffness: fraction of the gap closed per frame (0..1)
        float npcFingerSnapU   = 20.f;   // gap above this -> hard setPosition/setRotation teleport, game units
        float npcFingerMaxVel  = 8.f;    // m/s cap on the implied drive velocity; above -> teleport branch
        float npcFingerGravity = 0.f;    // m_gravityFactor at creation (keep 0; the drive overwrites velocity anyway)
        float npcFingerLog     = 1.f;    // 1 Hz NFING TRACK/REACT telemetry to PPB.log
        float logVerbose       = 0.f;    // PERF (2026-07-13 log demotion): 0 = per-slot CapFix
                                         // geometry lines (BEFORE/APPLIED/MIRROR-L/AUTOFIT) are
                                         // debug-level and invisible; 1 = spdlog level drops to
                                         // debug (dial sessions). Applied by CapFixPollFile ~1 Hz.
        // ── PLAYER HAND 4-BOX COLLIDER (2026-07-09, HandBox.cpp; spec2_handbox.md + review2 N-fixes).
        // Track A: live SHRINK-ONLY dial of HIGGS's palm slab (metres; -1 = leave alone; every write
        // clamped to the first-seen baseline — grow is the unverified direction; rollback = extents
        // recover at the next HIGGS body recreation). Track B: 4 keyframed follower boxes per hand
        // (2 index segments + 3-finger slab + tip plate, no thumb), DOUBLE-gated (review N9):
        // handBoxEnable AND handBoxArm must both be 1. All PK_NOSNAP.
        // 2026-07-10 BAKED DEFAULTS (user sign-off: "bake those 5 boxes together at those locations"):
        // the in-VR dialed slab dims become the compiled defaults (was -1/-1/-1 = leave stock).
        float higgsSlabHalfX   = 0.045f;  // width  90% of stock 0.050
        float higgsSlabHalfY   = 0.015f;  // thickness = stock
        float higgsSlabHalfZ   = 0.0405f; // length 45% of stock 0.090 (halved, then 90%)
        // higgsSlabAllowGrow (2026-07-10): 0 = shrink-only (report 10 Risk 2 default); 1 = let the X
        // (width) half-extent GROW past the first-seen baseline, hard-clamped to 2.0x baseline X. Y/Z stay
        // shrink-only regardless. NOTE: growing HIGGS's own slab past stock is a deliberate, user-requested
        // deviation from report 10's shrink-only rule.
        float higgsSlabAllowGrow = 0.f;
        float handBoxEnable    = 0.f;    // SHIP DEFAULT 0
        float handBoxArm       = 0.f;    // second arming gate (review N9) — both required
        float handBoxFollowMode= 0.f;    // 0 = per-bone world transforms; 1 = hand-frame fixed offsets
                                         // (review N3: the GetFingerValues curl drive is REJECTED — stale when empty-handed)
        float handBoxTipFrac   = 1.15f;  // fingertip extrapolation past FingerX2 — index boxes 0/1
                                         // (2026-07-10 BAKED: user dialed +0.4u reach, 0.85 -> 1.15)
        float handBoxPlateTipFrac = 0.85f; // box 3 (tip plate) extrapolation — decoupled from handBoxTipFrac
                                         // (2026-07-10: user needs index long, plate short)
        float handBoxPlateLenFrac = 0.5f;// box 3 down-the-bone length scale, FRONT (fingertip) face pinned
                                         // (2026-07-10 BAKED: user's "cut it by half")
        float handBoxSlabLenFrac = 1.0f; // box 2 (3-finger slab) down-the-bone length scale, BASE
                                         // (hand/knuckle-ward mn[2]) face pinned — the cut comes off the
                                         // fingertip-ward end, box 3 covers the tips (accessor clamps 0.2..1.0)
        // Box-3 TILT (2026-07-10): spin the plate in the rider frame. Axis 2 (down-the-bone) spins it
        // flat on the fingers; + degrees = toward the index. Axis/sign are knobs so a wrong guess is a
        // tuning-file edit, never a rebuild.
        float handBoxPlateTiltDeg  = -30.f; // 2026-07-10 BAKED: user's dialed plate orientation
        float handBoxPlateTiltAxis = 2.f;   // 0=X 1=Y 2=Z (down-the-bone)
                                         // (accessor clamps 0.2..1.0)
        // Box-3 SECOND tilt stage (2026-07-10): composed ON TOP of stage 1 (rotOff := R2·R1·rotOff), so
        // the plate can be canted on two axes at once without a rebuild (e.g. stage-1 flat-on-fingers
        // about Z, then stage-2 toward the dorsum about Y). Default axis 1 (Y). Accessor clamps ±180.
        float handBoxPlateTilt2Deg  = 0.f;
        float handBoxPlateTilt2Axis = 1.f;  // 0=X 1=Y (default) 2=Z (down-the-bone)
        float handBoxIdxHalfW  = 0.0095f;// index box half-width, metres
        float handBoxIdxHalfT  = 0.0085f;// index box half-thickness, metres (accessor floors at 0.010 — review N10)
        float handBoxSlabHalfT = 0.0100f;// 3-finger slab half-thickness, metres (same floor)
        float handBoxPad       = 0.0010f;// slab cross-section padding, metres
        float handBoxSubLayer  = 4.f;    // ragdoll sub-layer; 4 = SkipBoth (adjacency-skips HIGGS's hand 3 / 5)
        float handBoxMaxVel    = 25.f;   // m/s; above this -> teleport (setPosition) instead of the keyframe ride
        float handBoxBeast     = 0.f;    // 0 = no boxes (and no slab writes) on beast skeletons
        float handBoxDump      = 0.f;    // edge-triggered (0 -> 1) one-shot diagnostic dump to PPB.log; set back to 0 to re-arm
        // ── PER-BOX LIVE DIALS (2026-07-10): SolveGeometry now runs EVERY consumed snapshot, so every
        // box knob is live like the capsule dials (no game restart / recreate per tweak). Offsets are
        // GAME UNITS added to the solved box center in the rider frame; the plate half OVERRIDES are
        // GAME UNITS (-1 = keep the solved value); tilt spins the box about a rider axis. All PK_NOSNAP.
        float handBoxPlateOffX = 0.f, handBoxPlateOffY = 0.f, handBoxPlateOffZ = 0.f;   // box 3 center nudge (rider frame)
        float handBoxPlateHalfW = -1.f;  // -1 = use solved half[0] (across);  else GAME-UNIT override
        float handBoxPlateHalfT = -1.f;  // -1 = use solved half[1] (dorsal thickness);  else GAME-UNIT override
        float handBoxIdxOffX = 0.f, handBoxIdxOffY = 0.f, handBoxIdxOffZ = 0.f;         // boxes 0/1 center nudge (rider frame)
        float handBoxIdxTiltDeg  = 0.f;  // spin index boxes 0/1 in the rider frame, degrees
        float handBoxIdxTiltAxis = 2.f;  // 0=X 1=Y 2=Z (down-the-bone, default)
        float handBoxDumpNow   = 0.f;    // edge-triggered (0 -> non-0): one line per box per hand (off/halfU/tilt/rider,
                                         // GAME UNITS) to PPB.log — the readback for baking the dialed values into defaults
        // ── TORSO LIST CHILDREN (2026-07-08 wave-2 bake). Child 0 = MAIN = the capSpine0*/capSpine1*/
        // capSpine2*/capCom* slot knobs above; children 1..N = capSpine0C1..C10 / capSpine1C1..C10 /
        // capSpine2C1..C10 / capComC1..C20. Baked seeds are TINY (r 0.2, 1u) rods buried on the main's
        // axis — an Enable-0 child keeps its baked geometry, so a badly-seeded child would be a live
        // invisible collider (it is not: the seeds are coaxial and strictly inside the main's surface).
        // Torso bodies are CENTERLINE — kSlotNodeL[4..7] and [11] are nullptr, so capMirrorL never
        // touches them (no left twin exists).
        // 2026-07-08 wave-2b bake: spine2C grown 10->12 (2 breast-support children), headC[10] added
        // (head becomes a bhkListShape for the first time). spine0C/spine1C stay [10] MAX capacity —
        // the wave-2b bake gives the NIF only 7/6 children; array size is a cap, the apply loop is
        // min-bounded, so no shrink is needed and the parked spare tuning lines stay inert no-ops.
        // 2026-07-09 head wave-3: headC grown 10->14 (+4 final head-detail children; NIF head list 11->15).
        // 2026-07-09 shoulder lock: spine2C grown 12 -> 14 (+2 shoulder-socket children; NIF list 13 -> 15).
        // 2026-07-16 head 14 -> 16: C15/C16 = the KHAJIIT EAR pair (mirrors of C12/C13). They exist ONLY
        // in skeletonbeast_female_khajiit.nif, so the CapFix loop (bounded by the actor's real
        // childInfo.size()) can never reach them on a human/Argonian head — per-race by EXISTENCE, no
        // race gate. ⚠ Growing this array needs all FOUR coupled edits (04_Pitfall_Ledger): this size,
        // kArrays' count, CapFixChildKnobs(3), and CapFixChildSlot's ChildPtr bound.
        // 2026-07-16 spine2C 14 -> 16: +C15/C16 for the ARGONIAN dorsal ridge (spine0/spine1 already had
        // spare array room at [10]). Same per-race-by-existence trick as the ears: the extra children live
        // ONLY in skeletonbeast_female.nif, so human/Khajiit heads+spines (fewer children) never see them.
        // 2026-07-19 head 16 -> 22: C15..C22 now cover the 8 HEAD-JOINT SEEDS carried by EVERY head
        // NIF (human 23-child bake + the draenei copy; beast heads stay 17 = C15/C16 only). Horn/
        // antler dial sessions drive them live behind a sculpt gate (Yvanni: bake into her draenei
        // NIF after; Auri: transcribe into her npcCap lines — the seeds must stay buried for the
        // shared human NIF). Four coupled edits done in lock-step per the Ledger.
        CapChild spine0C[10]{}, spine1C[10]{}, spine2C[16]{}, headC[22]{}, comC[20]{};
    };
    extern GrabTune g_tune;           // the live tune instance — overwritten by the hot-reload

    // Hot-reload the PPB params from a plain "key value" text file (PPB_tuning.txt). Lines starting
    // with '#'/';' are comments. Missing/unparseable -> keep current value (safe). Returns the path
    // actually opened (nullptr if none) so the poll commits its mtime only after a successful parse.
    const char* ReloadGrabTune();

    // CAP FIX control (the generation loop): the 1 Hz file poll arms a new generation when any
    // snapshotted knob changes; CapFixApply/PivFixApply consume the generation per actor.
    unsigned CapFixGen();
    void     CapFixSet(const float a[3], const float b[3], float r);   // console-args path: store + arm (no file)
    void     CapFixPollFile();   // ~1 Hz idle-safe file poll: auto-arm on knob change (Claude-driven loop)
    bool     CapFixSlot(int slot, float a[3], float b[3], float& r);   // 0=hand 1=forearm 2=upper 3=head 4-6=spine0-2 7=neck 8=thigh 9=calf 10=foot 11=com; returns enable
    // bhkListShape child accessor (generalized 2026-07-07): slot = the CapFixSlot index, child = the
    // LIST child index. Hand (slot 0): C1..C3 = children 0..2 (the 07-03 bake layout, unchanged).
    // Other list slots: child 0 = the MAIN capsule = the SLOT knobs; children 1..N = cap<Slot>C1..CN.
    // Returns false when the (slot,child) pair has no knob. Returns enable.
    bool     CapFixChildSlot(int slot, int child, float a[3], float b[3], float& r);
    int      CapFixChildKnobs(int slot);          // knob-addressable child count per slot (0 = slot has no list support)
    bool     CapMirrorLEnabled();                 // capMirrorL knob: mirror every capsule write to the LEFT twin
    bool     PivGrabGateEnabled();                // pivGrabGate knob: pause pivot heal while player-grabbed
    bool     PivFixSlot(int joint, float p[3]);   // 0..10 = wrist elbow shoulder spine0 spine1 spine2 neck head hipR kneeR ankleR
    float    PivFixUpU(int joint);                // NEW: per-joint world-Z lift knob (game units; same joint order)
    float    JTrackNow();                         // raw value; PivJointTrackTick edge-detects the 0 -> non-0 transition
    // ── XP32 POSE-CONFORM accessors (PPBHook.cpp; read live per frame) ──
    bool     PoseConformEnabled();                // poseConform knob: overwrite drive-pose translations with XP32-chain values
    float    PoseConformDump();                   // raw value; PPBHook edge-detects the 0 -> non-0 transition (jtrackNow idiom)
    bool     PoseConformRoot();                   // conform the root translation too (the 4.3u constant fix)
    int      PoseConformEveryN();                 // chain-recompute stride, clamped 1..16 (1 = every frame)
    bool     CapAutoFitEnabled();   // capAutoFit knob
    bool     MeasuredScaleEnabled();// measuredScaleEnable knob: apply the 1 Hz tape-measure effScale (else GetScale)
    float    MeasuredScaleRefId();  // reference actor FormID (Lydia) — her spacing is the scale-1 standard
    bool     RefCapNow();           // refCapNow knob: deterministic one-shot reference read (bake the logged spans)
    // ── BODY SCALE accessors (2026-07-14) — read LIVE at capsule apply so factor/clamp edits take
    // effect within ~1 s; the master gate + factors + clamps are SNAP (a change bumps the CapFix gen). ──
    bool     BodyScaleEnabled();            // bodyScale master gate (> 0.5)
    float    BodyScaleRegionFactor(int region);   // BodyScale::Region 0..7 -> deviation gain (default 1.0)
    float    BodyScaleClampLo();            // region-ratio floor, re-bounded to a sane 0.3..1.0
    float    BodyScaleClampHi();            // region-ratio ceiling, re-bounded to a sane 1.0..3.0
    float    BodyScaleDump();               // raw value; CapFixApply edge-detects the change (cache clear)
    bool     FingerCapTrackEnabled();   // fingerCapTrack knob: per-frame finger far-endpoint rewrite
    float    FingerCapR();              // fingerCapR knob: finger capsule radius (GAME units)
    bool     LodSpikeEnabled();         // lodSpike knob: arm the `capdis` disableChild spike (default off)
    // ── PERF SYSTEM accessors (PerfSys.cpp; -1 knobs resolved against perfPreset here) ──
    int      PerfPreset();              // sanitized: 0, 50, 80 or 90
    int      PerfContactKeepN();        // contact decimation: 0 off, else keep 1 NEW point in N
    bool     PerfFilterSelfThigh();     // resolved: drop LThigh(8) x RThigh(14) via the HIGGS callback
    bool     PerfFilterCrossPelvis();   // resolved: cross-actor part-2 drop (OFF in all presets for now)
    bool     PerfLodEnabled();          // resolved: the FULL/REDUCED/CORE child-LOD
    float    PerfLodNearDist();
    float    PerfLodFarDist();
    float    PerfLodHystPct();
    float    PerfLodSettleFrames();
    float    PerfLodStageBits();
    // ── NPC FINGER TEST v2 accessors (NpcFingerTest.cpp; all read live per frame) ──
    bool     NpcFingerEnabled();
    int      NpcFingerCount();          // 1..4 (default 1 = the mandatory single-capsule Risk-1 spike)
    float    NpcFingerR();              // GAME units (floored — a 0 radius is a degenerate collider)
    bool     NpcTailTest();             // any follower mode active (mode > 0)
    int      NpcTailMode();             // 0 off / 1 tail / 2 wig / 3 RETIRED (was dress)
    float    NpcTailR();                // LEGACY — no consumer (TuneOf() owns garment radii); kept for ABI
    float    NpcTailOffScale();         // fur-center offset scale (0 = bone axis, 1 = measured), clamped 0-2
    bool     NpcFingerOwnBody();        // AIHands mode: fingers collide with own body + world
    float    NpcFingerFollow();         // capsule->bone write-back share (0-1, clamped)
    float    NodeCensusNow();           // census edge knob (raw value; edge-detect at the caller)
    bool     FsmpPushEnabled();
    float    FsmpPushForce();           // LEGACY — declared but unused by the per-target push path
    float    FsmpPushMaxForce();        // LEGACY — declared but unused by the per-target push path
    float    FsmpPushMinDispU();
    float    FsmpPushMult();            // global multiplier on the per-table (TuneOf) gain+clamp, clamped 0-10
    bool     NpcFollowerEnabled();      // master switch for the always-on garment rigs (> 0.5)
    float    NpcFingerTipU();
    float    NpcFingerMassKg();         // floored at 0.01 kg (a 0-mass dynamic body is a solver hazard)
    float    NpcGarmentMassKg();        // light garment-capsule mass, floored at 0.01 kg
    float    NpcTailMassKg();           // meaty tail-capsule mass (tbl 1/2), floored at 0.01 kg
    float    NpcSilentVelMS();          // silent-contact drive-velocity cap (m/s); <= 0.01 = clamp OFF
    float    NpcGarmentMat();           // garment capsule sound material selector (see npcGarmentMat)
    float    NpcBodyMat();              // BODY capsule sound material selector (see npcBodyMat)
    float    BodyShiftBreastsUpU();     // morph-driven translation: breasts up, u/morph
    float    BodyShiftBreastsFwdU();    // morph-driven translation: breasts forward, u/morph
    float    BodyShiftButtBackU();      // morph-driven translation: butt cheeks back, u/morph
    float    BodyScaleLoBoost();        // shrink-side (net<0) factor multiplier
    bool     MeshShapeEnabled();        // Route B measured trueShape master
    float    MeshShapeDump();           // edge knob: re-sample + MESHGIRTH dump
    float    MeshBoneTest();            // edge knob: skin-weight bone-anchor diagnostic (BONETEST log)
    bool     BoneShapeEnabled();        // ReShape v2 master (bone-anchored measurement)
    float    BoneNeutral(int region);   // Lydia bone-anchored neutral per region (0 = uncaptured)
    float    BoneNeutralCup();          // Lydia breast-mound z-extent neutral (0 = uncaptured)
    float    BoneNeutralBreastZ();      // Lydia mound-height (sag) neutral (0 = uncaptured)
    float    BoneNeutralBreastAbs();    // Lydia breast mound Y, spine2-local (shift driver)
    float    BoneNeutralButtAbs();      // Lydia cheek  mound Y, COM-local    (shift driver)
    float    LmUV(int idx, int axis);   // live landmark UV: idx 0..6, axis 0=u 1=v
    bool     LmButtFitEnabled();        // cheeks placed from the UV butt landmark (own switch)
    float    LmButtOutU();              // outward push applied to the landmark (measure->skin bias)
    float    LmButtPosGain();           // proportional damp on the cheek displacement
    void     LmButtNeutral(float out[3]);  // the anchor: Lydia's landmark, already pushed
    float    LmNoseUV(int axis);           // head nose landmark UV (0=u 1=v)
    float    LmChinUV(int axis);           // head chin landmark UV (0=u 1=v)
    // NEUTRAL SHAPE: Lydia's landmark idx (same order as LmUV / kUvLandmarks), bone-local.
    void     LmNeutralPos(int idx, float out[3]);
    bool     LmReShapeEnabled();          // THE shape master switch (replaces bodyScale/meshShape)
    float    LmRegionGain(int region);    // per-ring trim; 1.0 = follow the flesh exactly
    float    LmNeutLimbChord(int region); // leg/arm neutral chord (0 = not a limb region)
    float    LmNeutNoseChin();            // Lydia nose->chin (head channel neutral)
    // ReTouch ghost/mouth accessors
    bool     GhostZonesEnabled();
    bool     DgEnabled();        // dismember guard master (dgEnable)
    float    DgGraceDeadS();     // corpse PLANCK-ignore grace window, seconds
    bool     DgLogOn();          // dismember guard log lines (dgLog)
    bool     DgDeathIgnoreOn();  // death-instant PLANCK ignore (dgDeathIgnore, v2 opt-in)
    bool     DgCloneStripOn();   // head-clone body strip (dgCloneStrip)
    float    DgStripDelayS();    // strip delay after clone confirmation, seconds
    bool     DgDeferDfOn();      // DF freeze-fix master (dgDeferDf)
    bool     DgHeadTrackOn();    // head-creation tracker (dgHeadTrack)
    bool     DgDeathCutOn();     // death-confirmed dismemberment master (dgDeathCut)
    float    DgDeathCutDelayS(); // delay after death before asking DF
    float    DgDeathNodeTries(); // limb nodes offered per death
    bool     DgHitLocatedOn();   // hit-located limb choice (dgHitLocated)
    float    DgHitMaxDistU();    // accuracy gate distance (dgHitMaxDistU)
    bool     DgHeadSkelOn();     // head-skeleton swap master (dgHeadSkel)
    float    DgHeadSkelHoldS();  // max hold time for the swap
    bool     DgHeadParkOn();     // park head bodies at the anchor (dgHeadPark)
    bool     DgHeadGrabFixOn();  // clear head anchor sub-layer for HIGGS (dgHeadGrabFix)
    bool     DgHeadPlanckOn();   // leave head clones under PLANCK (dgHeadPlanck)
    bool     DgHeadStripHairOn();// unequip wigs from severed heads (dgHeadStripHair)
    bool     GhostVizEnabled();
    float    GhostRangeU();
    float    GhostBreastPadU();
    float    GhostButtPadU();
    float    MouthGhost(int i);           // 0..5 = AX AY AZ BX BY BZ, 6 = R
    float    MouthEnterU();
    float    MouthExitU();
    float    MouthPhonemeIdx();
    float    MouthPhonemeMax();
    float    MouthPhoneme2Idx();
    float    MouthPhoneme2Max();
    bool     MouthNeedFinger();
    float    MouthDeepChinU();
    float    MouthDeepPalU();
    int      MouthThroatChild();   // head child index of the throat wall (mouthThroatChild)
    float    MouthThroatU();       // throat-reach distance gate
    // beast mouth gate (child indices + entry range) — family picked by skeleton filename
    int      MouthBeastChild(bool khajiit, int which);  // which: 0..2 entry trio, 3..4 chins
    float    MouthBeastEnterU();
    float    MouthRampIn();
    float    MouthRampOut();
    float    MouthFingerBox();
    bool     TouchProbeEnabled();
    float    TouchProbeRangeU();
    float    LmClampLo();
    float    LmClampHi();
    // breast model: fills c[13] = AYc AYm BYc BYm AZc AZm BZc BZm Rc Rm AX latK latX (+latY0)
    void     LmBreastModel(float out[14]);
    void     LmBreastModelEx(float out[5]);   // hK, midZ0, cupSat, sagLo, sagHi
    float    TipProbe(int axis);        // 0=X 1=Y 2=Z live probe offset (mesh space)
    float    NipUV(int axis);           // 0=u 1=v target UV landmark (0 = off)
    bool     MeshMarkersEnabled();      // visible girth-vertex ghost markers
    float    MeshRadiusGain();          // damped radius response (0..1)
    float    MeshShiftButtK();          // cheek shift, u per u
    float    MeshShiftBreastFwdK();     // breast forward, u per u
    float    MeshShiftBreastZK();       // breast vertical, u per u
    float    MeshNeutralBreastZ();      // neutral breast-mass height (0 = uncaptured)
    float    MeshNeutralBreastCup();    // neutral cup vertical extent (0 = uncaptured)
    float    MeshCupFrac();             // mound threshold fraction
    float    MeshNeutral(int region);   // captured neutral girth (0 = uncaptured), region 0..6
    float    MeshBandLo(int region);    // z-band fraction lo, region 0..6
    float    MeshBandHi(int region);    // z-band fraction hi
    float    MeshArmFrac();             // arm/torso x-split fraction
    float    FsmpMassScale();           // mass-scaled push force toggle (TBB-thread read — plain float, accepted residual)
    float    FsmpContactGate();         // contact-verified push gate toggle (main-thread read, OnPreDrive)
    float    FsmpContactDevU();         // predicted-vs-actual deviation threshold, game units/frame
    float    FsmpContactHoldMS();       // contact latch hold, ms

    // ── SCULPT GATE (2026-07-17, user directive): PPB_skeletons.txt `sculpt <file.nif>` restricts
    // GLOBAL cap* knob application to actors whose race FEMALE skeleton is that NIF — a dial
    // session can never again move another skeleton's finished/baked capsules (the "Argonian dial
    // moved Lydia's head" trap). npcCap per-NPC overrides are exempt. Empty = off (legacy global).
    void        SetSculptTarget(const char* nifFilename, int slot);   // "" = gate off; slot -1 = all
    const char* SculptTarget();     // the sculpt skeleton FILENAME (basename-normalized), or ""
    int         SculptTargetSlot(); // the ONE gated slot (-1 = every slot is gated)
    // raceBias (2026-07-19e): flat per-race capsule bias from PPB_skeletons.txt — corrects
    // systematic MEASUREMENT error on furred bodies (M'rissi reads wider than her silhouette).
    void        ClearRaceBias();
    void        AddRaceBias(const void* race, float mult);
    float       RaceBiasOf(const void* race);   // 1.0 when unlisted

    // ── PER-NPC CAPSULE OVERRIDES (2026-07-17, the runtime "personal skeleton" layer) ──────────────
    // Parsed from PPB_skeletons.txt `npcCap` lines at kDataLoaded (main.cpp). A matching entry WINS
    // over the global cap*C* knob for that (actor, slot, child) — and applies even when the knob is
    // absent/disabled — so per-NPC geometry (Auri's antlers) rides seed children in the shared NIF
    // while every other actor keeps the buried seeds. Per-actor shape CLONES guarantee no leakage.
    // MAIN THREAD ONLY (parsed at kDataLoaded, read from CapFixApply's pre-drive tick).
    void ClearNpcCapOverrides();
    void AddNpcCapOverride(std::uint32_t formId, int slot, int child,
                           const float a[3], const float b[3], float r);
    bool NpcCapOverride(std::uint32_t formId, int slot, int child,
                        float a[3], float b[3], float* r);
    int  NpcCapOverrideCount();         // for the load-time receipt log
    float    NpcFingerCurlGain();       // contact-curl integrator gain (/u/s); <= 0.01 = curl OFF
    float    NpcFingerCurlDecay();      // contact-curl relax rate (/s)
    float    NpcFingerCurlMax();        // curl ceiling 0..1 (clamped)
    float    NpcFingerCurlMode();       // 0 additive / 1 bind-relative
    float    NpcFingerCurlLagGate();    // chord-speed gate (u/s); <= 0.01 = off
    float    NpcFingerVsClutter();      // finger-vs-clutter collision knob
    float    NpcFingerVsWorld();        // finger-vs-statics collision knob
    float    NpcFingerVsNpc();          // finger-vs-other-NPC collision knob
    float    NpcFingerAlpha();          // clamped to (0.05, 1] — alpha > 1 is super-critical (the Z65 lesson)
    float    NpcFingerSnapU();
    float    NpcFingerMaxVel();
    float    NpcFingerGravity();
    bool     NpcFingerLogEnabled();
    // ── PLAYER HAND 4-BOX accessors (HandBox.cpp) ──
    float    HiggsSlabHalfX();          // metres; -1 = leave HIGGS's slab alone
    float    HiggsSlabHalfY();
    float    HiggsSlabHalfZ();
    bool     HiggsSlabAllowGrow();      // X-only grow past baseline (2x cap); Y/Z stay shrink-only
    bool     HandBoxEnabled();          // handBoxEnable AND handBoxArm (review N9 double gate)
    int      HandBoxFollowMode();       // 0 per-bone / 1 hand-frame
    float    HandBoxTipFrac();
    float    HandBoxPlateTipFrac();     // box-3 tip-plate extrapolation (same 0..2 clamp as TipFrac)
    float    HandBoxPlateLenFrac();     // box-3 length scale, clamped 0.2..1.0 (front face pinned)
    float    HandBoxSlabLenFrac();      // box-2 slab length scale, clamped 0.2..1.0 (base/knuckle face pinned)
    float    HandBoxPlateTiltDeg();     // box-3 tilt in the rider frame, degrees (+ = toward the index)
    int      HandBoxPlateTiltAxis();    // box-3 tilt axis: 0=X 1=Y 2=Z (down-the-bone, default)
    float    HandBoxPlateTilt2Deg();    // box-3 SECOND tilt stage, degrees (±180); composed after stage 1
    int      HandBoxPlateTilt2Axis();   // box-3 stage-2 tilt axis: 0=X 1=Y (default) 2=Z
    float    HandBoxIdxHalfW();
    float    HandBoxIdxHalfT();         // floored at 0.010 m (review N10 tunneling floor)
    float    HandBoxSlabHalfT();        // floored at 0.010 m (review N10)
    float    HandBoxPad();
    unsigned HandBoxSubLayer();         // sanitized: 2/3/5/6 (HIGGS's own parts) remap to 4
    float    HandBoxMaxVel();
    bool     HandBoxBeast();
    float    HandBoxDump();             // raw value; HandBox edge-detects the 0 -> 1 transition
    // ── per-box LIVE DIALS (2026-07-10) — offsets clamped ±10u, plate halves 0.05..5u (-1=solved), tilt ±180 ──
    float    HandBoxPlateOffX();        // box-3 center nudge, GAME units (rider frame)
    float    HandBoxPlateOffY();
    float    HandBoxPlateOffZ();
    float    HandBoxPlateHalfW();       // -1 = solved; else GAME-unit override of half[0] (across)
    float    HandBoxPlateHalfT();       // -1 = solved; else GAME-unit override of half[1] (dorsal thickness)
    float    HandBoxIdxOffX();          // index boxes 0/1 center nudge, GAME units (rider frame)
    float    HandBoxIdxOffY();
    float    HandBoxIdxOffZ();
    float    HandBoxIdxTiltDeg();       // index boxes 0/1 tilt in the rider frame, degrees (±180)
    int      HandBoxIdxTiltAxis();      // index tilt axis: 0=X 1=Y 2=Z (down-the-bone, default)
    float    HandBoxDumpNow();          // raw value; HandBox edge-detects the 0 -> non-0 transition

    // HEEL FIX toggle (RUNTIME, console-controlled): shift heeled NPCs' drive target +heelZ so the
    // ragdoll matches the raised visual skeleton. Set/Toggle return the new state.
    bool HeelFixEnabled();
    bool SetHeelFix(bool on);
    bool ToggleHeelFix();
    void InitHeelFixDefault();   // read the tuning file's heelFix ONCE at DataLoaded (startup default)
}
