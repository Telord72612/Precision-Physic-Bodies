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
        float capHandC6Enable = 0.f;                              // buried spare — HAND IS OFFSET: this is list child 5
        float capHandC6AX = 0.0f, capHandC6AY = 0.0f, capHandC6AZ = 0.0f;
        float capHandC6BX = 0.0f, capHandC6BY = 0.0f, capHandC6BZ = 2.0f;
        float capHandC6R  = 0.5f;
        float capHandC7Enable = 0.f;                              // buried spare — list child 6 (capHandC(N+1) mapping)
        float capHandC7AX = 0.0f, capHandC7AY = 0.0f, capHandC7AZ = 0.0f;
        float capHandC7BX = 0.0f, capHandC7BY = 0.0f, capHandC7BZ = 2.0f;
        float capHandC7R  = 0.5f;
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
        // ══ 2026-08-22 MALE SCULPT LIMB HEADROOM ═══════════════════════════════════════════
        // +2 buried spares on every limb node (L and R alike — the twins mirror), plus the two
        // LANDMARK CHORD probes (capUpperC5 = arm, capThighC9 = leg). Spares ship Enable 0 with
        // the canonical limb burial seed (0,0,0)->(0,0,2u) r 0.5 — bone-frame origin is deep
        // inside the limb, so an accidental Enable can never produce a touchable collider.
        // ⚠ Limb slots are EXPLICIT switch cases in CapFixChildSlot — a new field here needs a
        // matching `case` line AND a PK8() registration in Tuning.cpp or it silently does nothing.
        float capUpperC4Enable = 0.f;                              // buried spare (child 4)
        float capUpperC4AX = 0.0f, capUpperC4AY = 0.0f, capUpperC4AZ = 0.0f;
        float capUpperC4BX = 0.0f, capUpperC4BY = 0.0f, capUpperC4BZ = 2.0f;
        float capUpperC4R  = 0.5f;
        float capUpperC5Enable = 0.f;                              // ARM LANDMARK CHORD probe (child 5) — STRIP BEFORE SHIP
        float capUpperC5AX = 0.0f, capUpperC5AY = 0.0f, capUpperC5AZ = 0.0f;
        float capUpperC5BX = 0.0f, capUpperC5BY = 0.0f, capUpperC5BZ = 2.0f;
        float capUpperC5R  = 0.5f;
        float capForeC2Enable = 0.f;                              // buried spare (child 2)
        float capForeC2AX = 0.0f, capForeC2AY = 0.0f, capForeC2AZ = 0.0f;
        float capForeC2BX = 0.0f, capForeC2BY = 0.0f, capForeC2BZ = 2.0f;
        float capForeC2R  = 0.5f;
        float capForeC3Enable = 0.f;                              // buried spare (child 3)
        float capForeC3AX = 0.0f, capForeC3AY = 0.0f, capForeC3AZ = 0.0f;
        float capForeC3BX = 0.0f, capForeC3BY = 0.0f, capForeC3BZ = 2.0f;
        float capForeC3R  = 0.5f;
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
        float capThighC7Enable = 0.f;                              // buried spare (child 7)
        float capThighC7AX = 0.0f, capThighC7AY = 0.0f, capThighC7AZ = 0.0f;
        float capThighC7BX = 0.0f, capThighC7BY = 0.0f, capThighC7BZ = 2.0f;
        float capThighC7R  = 0.5f;
        float capThighC8Enable = 0.f;                              // buried spare (child 8)
        float capThighC8AX = 0.0f, capThighC8AY = 0.0f, capThighC8AZ = 0.0f;
        float capThighC8BX = 0.0f, capThighC8BY = 0.0f, capThighC8BZ = 2.0f;
        float capThighC8R  = 0.5f;
        float capThighC9Enable = 0.f;                              // LEG LANDMARK CHORD probe (child 9) — STRIP BEFORE SHIP
        float capThighC9AX = 0.0f, capThighC9AY = 0.0f, capThighC9AZ = 0.0f;
        float capThighC9BX = 0.0f, capThighC9BY = 0.0f, capThighC9BZ = 2.0f;
        float capThighC9R  = 0.5f;
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
        float capCalfC5Enable = 0.f;                              // buried spare (child 5)
        float capCalfC5AX = 0.0f, capCalfC5AY = 0.0f, capCalfC5AZ = 0.0f;
        float capCalfC5BX = 0.0f, capCalfC5BY = 0.0f, capCalfC5BZ = 2.0f;
        float capCalfC5R  = 0.5f;
        float capCalfC6Enable = 0.f;                              // buried spare (child 6)
        float capCalfC6AX = 0.0f, capCalfC6AY = 0.0f, capCalfC6AZ = 0.0f;
        float capCalfC6BX = 0.0f, capCalfC6BY = 0.0f, capCalfC6BZ = 2.0f;
        float capCalfC6R  = 0.5f;
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
        float capFootC4Enable = 0.f;                              // buried spare (child 4)
        float capFootC4AX = 0.0f, capFootC4AY = 0.0f, capFootC4AZ = 0.0f;
        float capFootC4BX = 0.0f, capFootC4BY = 0.0f, capFootC4BZ = 2.0f;
        float capFootC4R  = 0.5f;
        float capFootC5Enable = 0.f;                              // buried spare (child 5)
        float capFootC5AX = 0.0f, capFootC5AY = 0.0f, capFootC5AZ = 0.0f;
        float capFootC5BX = 0.0f, capFootC5BY = 0.0f, capFootC5BZ = 2.0f;
        float capFootC5R  = 0.5f;
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
        // ── RAYCAST / QUERY TELEMETRY (2026-08-22, RayTel.cpp) — the instrument that turns doc 08
        // §3.5's MODELLED ray cost into a MEASURED one. 0 = OFF (shipping default; the five
        // bhkCollisionFilter sub-object vtable slots hold the untouched engine pointers, so the
        // disabled state is byte-identical to vanilla and costs literally nothing). 1 = arm the
        // counters (per-child ray walks, per-child narrowphase walks, per-body ray tests, broadphase
        // pairs) and log one line per second. 2 = also accumulate __rdtsc deltas around each filter
        // call for a measured filter-CPU millisecond figure (integer intrinsic, hard-rule-1 safe;
        // costs ~2 rdtsc per call, so it perturbs what it measures — read it as a floor).
        float rayTel           = 0.f;
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
        // fsmpFlexCompat (2026-07-29): accept an interface-1.x SMP engine — i.e. HDT-SMP Flex
        // (OgreWorks, "hdtSMP64.dll", 8.0.x) — using v1-shaped onEvent listeners instead of the
        // v2 BSTEventSink ones. Everything else about the two interfaces is identical: same
        // PluginInterface vtable, same event structs, same Bullet 3.24. Without this, a Flex
        // user gets hair/tail CAPSULES (they never depended on the link) but NO SMP push, so
        // strands do not react. See src/fsmp/PluginAPI_v1.h for the ABI proof.
        // ⚠ Not yet exercised in game (Flex is installed but disabled in the dev profile). The
        // receipt is an "FSMPLINK LIVE ... (interface 1)" line; set to 0 if Flex misbehaves.
        float fsmpFlexCompat   = 1.f;    // 1 = support SMP Flex, 0 = reject interface 1 as before
        // ── GARMENT-RIG BUDGET (2026-07-30) ──────────────────────────────────────────────
        // A wig rig is not cheap: every chord is an INDEPENDENT dynamic Havok body with its
        // own broadphase entry, deactivation disabled (they never sleep), and a per-frame
        // main-thread applyHardKeyFrame. Generated hair tables run 160-200 chords on 8 of the
        // 239 wigs, and kMaxRigs is 8 — i.e. an unbounded worst case of ~1600 driven bodies.
        // Before this there was NO distance limit at all: any ragdoll-driven actor could take
        // a slot. And since only the NEAREST actor ever publishes push, a distant NPC's wig
        // was paying full body cost for exactly zero push benefit.
        //   npcRigRangeU     0 = no limit. Garment rigs are not created beyond this, and are
        //                    destroyed (reason "range") once past range + npcRigRangeHystU.
        //   npcRigMaxActors  0 = no limit (capped by kMaxRigs 8 regardless). How many ACTORS
        //                    may hold garment rigs at once; a nearer candidate EVICTS the
        //                    farthest current holder rather than being refused.
        // Both are live knobs — they only gate rig lifecycle, never feel.
        // higgsPokeFix (2026-07-30): HIGGS plays a finger-CLOSE animation near any grabbable,
        // which curls the hand and defeats PPB's finger colliders — the most common "poking
        // does nothing" report. 1 = set HIGGS's SelectedCloseFingerAnimMaxHandSpeed to -1 via
        // HIGGS's OWN settings API (their higgs_vr.ini is never touched), re-applied on each
        // game load because HIGGS re-reads its ini. 0 = leave HIGGS entirely alone.
        float higgsPokeFix     = 1.f;
        // ── PUBLIC TOUCH API (2026-07-30, PpbApi.cpp / PpbTouchAPI.h) ────────────────────
        // Player-as-toucher contact reporting: mod events (PPB_TouchStart/Touch/TouchEnd),
        // Papyrus natives (PPB_Touch.GetContact*), and the native IPpbTouchInterface1.
        float apiTouch         = 1.f;    // master switch for the contact engine
        float apiHz            = 4.f;    // tick + event rate per second (user 2026-07-30: "4hz is
                                         // perfect, faster is wasted resource")
        float apiTouchU        = 1.0f;   // surface distance that counts as CONTACT (game units)
        float apiExitPadU      = 0.75f;  // hysteresis: existing contact survives out to touch+pad
        float apiMaxActors     = 3.f;    // nearest driven actors scanned per tick
        float apiRangeU        = 300.f;  // roster range (matches the ghost tracker's reach)
        // Geometric FIST fallback, used only when VRIK is absent (VRIK's getFingerPos is the
        // primary classifier). 2, not the original guessed 7: measured curl on a real rig
        // spans 3.7-9.1u, so 7 sits mid-range and misclassifies. Code default now MATCHES the
        // shipped tuning value — a code default that disagrees with the shipped file is the
        // trap that bit touchProbe and npcFingerLog earlier the same day.
        float apiFistTipPalmU  = 2.f;
        float apiEvents        = 1.f;    // 1 = fire the Papyrus mod events (natives always work)
        float apiLog           = 0.f;    // 1 = log START/END lines (debug; ships off)
        // Dwell filter (2026-07-30, user spec): a body part is only SENT through the API
        // after the probe lingered on it this long (seconds). Tracking is unaffected; a
        // contact that never qualifies emits nothing at all. 0 = instant.
        float apiDwellS        = 0.25f;   // default class (limbs, torso rings, etc.)
        float apiDwellHeadS    = 0.25f;   // the face — brushes are common, meaning needs intent
        float apiDwellComS     = 0.25f;   // pelvis/butt — the most brushed-in-passing slot
        float apiDwellSensorS  = 0.25f;   // interior sensors — insertion is already deliberate
        float apiDwellTailS    = 0.25f;   // tail chords
        float apiWeaponRMaxU   = 6.f;    // cap on the blade-segment radius (game u). The form
        // Cap on the held-object segment radius (game u). Same reasoning as the blade cap:
        // a wide flat item's second extent is its BREADTH, and an uncapped barrel reads
        // "deep" from well off-axis. 4u matches the VRTE addon's own probe. 0 = uncapped.
        float apiObjectRMaxU   = 4.f;
                                         // bound's second extent is the blade PLANE breadth on
                                         // broad weapons (axe = 23u -> a 46u barrel that read
                                         // "cervix -17u" from a hand merely holding her leg).
                                         // 6 ~ blade thickness; swords/knives are unaffected.
                                         // 0 = uncapped.
        float apiSubRegionInEvent = 0.f; // 1 = append a 5th '|' field (the SUB-REGION) to every
                                         // touch mod-event string. SHIPS OFF: the documented
                                         // format is 4 fields and a consumer told to "split on
                                         // the first three '|'" would read "human|In mouth" as
                                         // the skeleton. Native/Papyrus consumers get the
                                         // sub-region without this; the knob is for event-only
                                         // scripts whose author has opted in.
        float apiRawEvents     = 0.f;    // 1 = also fire the verbose PPB_TouchRaw* events (one
                                         // per capsule per source class). Ships OFF: the digest
                                         // stream is what consumers want; raw is opt-in.
        float apiSuppressHeldHand = 2.f; // 2 = STRICT (2026-09-06 user ruling: "there is a reason the player is
                                         // holding the apple, it's deliberate" — the held thing reports, the hand
                                         // does not; was 1 = the 07-31 index exception). 1 = a hand holding a weapon/object stops reporting
                                         // bare-hand contacts (your palm is on the grip). The
                                         // OTHER hand is unaffected.
        // ── BREAST TOUCH REACH (2026-09-03, VRTE_API_Change_Request_BreastTouchReach) ──────
        // User report: "Breast capsules are easily half the size of the breast ... touch is now
        // way late. It's particularly worse on bigger breasts."
        // MEASURED CAUSE, from PPB's own numbers: the breast radius is r = lmBrRc + lmBrRm*cup
        // (-0.6418 + 0.3645*cup) and `cup` SATURATES at lmBrCupSat 10.40 — but the captured
        // zero-slider neutrals are CBBE 9.97 / 3BA 9.63, i.e. the clamp sits ~4% above the BASE
        // BODY. So the whole dynamic range of the capsule radius, from a zero-slider CBBE to the
        // largest preset in the game, is 2.99 -> 3.15u (~5%) while the flesh can comfortably
        // double. Above the clamp the capsule stops growing and the mesh keeps going — which is
        // exactly why the complaint scales with breast size.
        //   ⛔ The clamp is NOT the bug and must not be unclamped: it exists so an extreme preset
        // gets the boundary response instead of an extrapolation that could reshape the middle
        // of the calibrated range (CapFix.cpp:2470). This carries the above-clamp response
        // OUTSIDE the fit, leaving the model untouched.
        //   The pad is a CONTACT-side term only: it is subtracted from the reported/gating
        // distance and NEVER from the ranking distance, so a padded breast can never steal the
        // nearest-capsule race from the sternum, ribs or collarbone beside it ("rank by true
        // geometry, gate by reach", v9.2). Nothing physical changes — not the Havok radius, not
        // the bake, not the sculpt, not jiggle.
        //   ⚠ A pad is shape-blind: it also extends reach ~padU FORWARD past the nipple (a hover
        // counts as contact, an extension of what apiTouchU 1.0 already does) and backward into
        // the chest (harmless — the rib capsules are nearer there).
        float apiBreastPadU     = 1.5f;   // base pad, GAME units. 1.5 = ghostBreastPadU, PPB's
                                          // own eye-calibrated capsule->skin gap in the visualiser.
                                          // 0 disables the feature entirely.
        float apiBreastPadSlope = 0.3645f;// extra pad per unit of cup ABOVE lmBrCupSat. 0.3645 =
                                          // lmBrRm, i.e. it restores exactly what saturation
                                          // withheld. The geometric argument for reaching skin
                                          // says the true slope is nearer 0.5 — dial by eye.
        float apiHairTarget    = 0.f;    // 1 = hair chords are touch TARGETS too. Ships OFF:
                                         // hair drapes the face/head, so it wins the nearest-
                                         // capsule race against cheeks and shadows face touch.
        float npcRigRangeU     = 700.f;  // ~10 m (game unit ≈ 1.43 cm)
        float npcRigRangeHystU = 100.f;  // extra slack before a range destroy (anti-thrash)
        float npcRigMaxActors  = 2.f;    // the "two closest" budget
        float npcGenCap        = 1.f;    // 2026-08-22 MALE GENITAL rig (tbl 7): Havok chord capsules
                                         // TRACKING the CBPC/SMP-driven Gen01..06 chain (the tail
                                         // pattern). Males only; rides the same probe as garments.
        float npcGenCapFemale  = 1.f;    // ★★ 2.2.0: SHIPS ON (user 2026-09-12: "On, we added it for a reason").
                                         // ★★ FUTA (2026-09-12, user request). Extend the tbl-7 GEN
                                         // rig to FEMALES wearing a futa schlong (TNG Gentlewoman /
                                         // TRX-ERF). Both such meshes are skinned to the SAME standard
                                         // chain (NPC GenitalsBase + Genitals01..06 — verified in both
                                         // mods' nifs), PPB's skeleton_female.nif already carries all 8
                                         // of those bones, and PairTableSexed(7) is sex-independent —
                                         // so nothing but the sex test stood in the way.
                                         // ⛔ SHIPS 0, and the gate is NOT slot 52. On a female slot 52
                                         // is measured FLAT in both states (GenitalProbe.h), and a slot
                                         // is a CLAIM: an unrelated slot-52 item, or a skin that happens
                                         // to carry the slot, would hand a vanilla naked female a GEN rig
                                         // and publish phantom "shaft" contacts. The female path instead
                                         // requires VISIBLE GEOMETRY parented under the Gen chain
                                         // (GenitalProbe::HasVisibleGenGeometry) — evidence of the mesh
                                         // itself, so no-futa answers false by construction.
                                         // ⚠ NEVER RUN IN VR (the author has neither mod installed).
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
        // ★ GEN (tbl 7) DIAL KIT (2026-08-22, user: "pretty flimsy ... don't slip from the hand,
        // make more tangible. It's not a fluffy thing"). All three levers were previously either
        // hardcoded in TuneOf or GLOBAL (shared with hair/tails) — dialling them globally would
        // have regressed every wig, the Carmella lesson a second time. Per-table now.
        float npcGenMassKg     = 2.0f;   // was NpcGarmentMassKg 0.05 (HAIR weight) — the documented
                                         // "can't hold a push and gets flung" signature.
        float npcGenR          = 1.6f;   // capsule radius (was hardcoded 1.1) — fatter = harder to slip
        float npcGenAlpha      = 0.85f;  // tracking stiffness for GEN only (global npcFingerAlpha 0.6);
                                         // higher = the capsule holds its place on the bone against a
                                         // hand instead of being shoved off it. Keep < 1 (Z65 lesson).
        // ★ GEN BEND (2026-08-22, user spec): touch-driven erection. +1 SOSBend per genBendUpMs of
        // hand contact on the GEN chords, cap genBendMax; HOLD genBendHoldSec after the last touch;
        // then decay 1 level per genBendDecaySec — ONLY while sla_Arousal < genBendArousal (a high-
        // arousal actor stays erect). Player: same machine off the HIGGS hands vs his own Gen04
        // node, but SPS stays the owner — we only send while OUR level EXCEEDS the arousal-implied
        // estimate (round(arousal*0.07)), re-asserted every 2 s; below it we go silent and SPS's
        // next tick restores its state ("whichever got the highest SOSBend is in control").
        float genBend          = 1.f;    // master switch (NPC side)
        float genBendPlayer    = 1.f;    // player self-touch side
        float genBendMax       = 7.f;    // user: "7 seems to be the right bend"
        float genBendUpMs      = 1000.f; // contact ms per +1 level
        float genBendHoldSec   = 60.f;
        float genBendDecaySec  = 5.f;
        float genBendArousal   = 60.f;   // sla_Arousal floor that blocks decay
        float genBendTouchU    = 8.f;    // hand-to-chord distance that counts as touching
        // ★ FRONT NECK (2026-08-22): the neck was a bare bhkCapsuleShape on every skeleton —
        // no list, so slot 7 had no child support at all. The argonian vehicle's neck is now a
        // 2-child list: child 0 = the neck proper (capNeck*), child 1 = "front neck", the
        // front-of-throat capsule that becomes the CHOKING target. Females get it after the
        // argonian dial is settled, so this ships with the beast vehicle only for now.
        CapChild neckC[1] = { { 1.f, 0.f, 4.20f, 3.00f, 0.f, 4.20f, 6.20f, 1.60f } };
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
        // ★ MALE NEUTRAL SHAPE (2026-08-21) — the lmNeutM* family. Same conventions as the
        // female lmNeut* block above (bone-local, scale-free base-mesh units, live so a male
        // re-capture never needs a rebuild), derived offline from the load order's WINNING
        // HIMBO male body against the shipped PPB male skeleton (sculpt.nif):
        // tools/ppb-scratch/derive_male_landmarks.py + derive_male_landmarks_results.md.
        // The code defaults ARE the shipping defaults (knob rule) and stay INERT until
        // maleGeometry 1 — the user eye-verifies before trusting them. 0 on a neutral =
        // uncaptured -> that channel self-disables (doc 04 §10b). No breast fields ON
        // PURPOSE: the male landmark table has no breast channel to neutralise.
        // ★ 2026-08-22 PASS 2 — re-derived on the TRUE winning male mesh (Bodyslide Output,
        // 11007 verts) at the pass-2 UVs, with the runtime's own math: ring driver =
        // sqrt(localX^2+localY^2); limb driver = the 3-D chord. Bind frames re-checked against
        // sculpt.nif this pass and IDENTICAL to the compiled constants (the sculpt moved
        // capsules, not joints). ⚠ The previous values measured the `ashe - fire and blood`
        // FOLLOWER body — legChord 11.924 / armChord 9.963 reproduce it to 3 d.p. On the real
        // base body those neutrals gave a ratio of 0.86/0.88 instead of 1.00, i.e. every male
        // thigh ~14% and arm ~12% too THIN, on the reference body itself.
        float lmNeutMChestX =  0.000f, lmNeutMChestY = 12.832f, lmNeutMChestZ = 8.096f;   // dial: capsule -1%
        float lmNeutMBellyX =  0.000f, lmNeutMBellyY = 12.450f, lmNeutMBellyZ = 5.149f;   // pass-3 belly
        float lmNeutMWaistX =  0.000f, lmNeutMWaistY = 11.645f, lmNeutMWaistZ = 4.179f;
        float lmNeutMButtX  =  2.931f, lmNeutMButtY  = -8.389f, lmNeutMButtZ  = 5.531f;
        float lmNeutMLegChord = 12.100f;
        float lmNeutMArmChord =  7.491f;
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
        float touchProbeHud    = 0.f;   // mapping session HUD: show slot.child + proposed name
                                        // in-view on touch (needs touchProbe on)
        float touchProbeHudU   = 1.0f;  // how close counts as a TOUCH for the HUD (units)
        // ── SCENE BUMPER GATE (2026-08-23): during an OStim scene the layer-30 CharacterBumper
        // (20u x 76u, the capsule whose JOB is shoving characters apart) grinds against the other
        // actor for the whole scene — the NPC-vs-NPC noise report, and a constant shove fighting
        // the animation. 1 = drop its collision while the actor carries OStim excitement, the
        // reversible twin of what the engine does for FURNITURE (RemoveNonRagdollRigidBodies-
        // FromWorld). Inert with no OStim installed (faction absent -> gate never arms).
        // sceneMode (2026-08-23): HOW the scene gate neutralises physics.
        //   1 = collision OFF on all 19 bodies (2.1 shipped behaviour — fixes alignment, but
        //       nothing can be touched physically and src=ENG stops)
        //   2 = MOTION_KEYFRAMED (recommended): the bodies stop being SOLVED — no constraint
        //       solve, no contacts, no gravity — so they converge on the animated pose instead of
        //       being dragged off it by the partner. They carry infinite mass so neither actor can
        //       shove the other, and they KEEP COLLIDING so the player can still touch them.
        //       ⛔ This does NOT mean "tracks the animation exactly" (the original wording, wrong):
        //       PLANCK clears kSyncOnUpdate every frame, so the engine's exact node->body copy is
        //       off and the body follows driveToPose's PD servo — convergent but LAGGING, and via
        //       postPhysics that lag is visible on the mesh too. Doc 25 §0b.
        //       Safe w.r.t. PLANCK's DYNAMIC write: that happens once from AddRagdollToWorld, not
        //       per frame — but re-adds are frequent (warp, cell change, distance band), which is
        //       why the gate re-asserts at 2 Hz rather than setting once.
        // ── DIVERGENCE PROBE (2026-08-25) — measure only, writes nothing. Doc 25 section 7.7.
        // Answers "is this body sitting off its animated target, and is the gap PERSISTING?"
        float divProbe          = 0.f;   // master. Ships OFF: it is an investigation tool.
        float divProbeEveryN    = 8.f;   // run the compare every Nth drive (per actor)
        float divProbeGapU      = 4.0f;  // gap (GAME u) above which a bone counts as "off target"
        float divProbeAlpha     = 0.25f; // EMA smoothing on the gap (contact bounce is transient)
        float divProbeHoldN     = 4.f;   // consecutive ticks over threshold before it is PERSISTENT
        float divProbePlayerU   = 20.f;  // a hand/weapon/object this close => the player caused it
        float divProbeReportS   = 3.f;   // seconds between report lines, per actor
        // ── ReDrive (2026-08-25): PER-BONE DRIVE AUTHORITY ──────────────────────────────
        // A multiplier on how hard the animated pose pulls this bone, versus how much the
        // Havok result is allowed to stand. PLANCK stamps ONE global gain into all 18 bones;
        // this reclaims the per-bone array the engine already has.
        //   < 1  the bone YIELDS (contacts win -- it gives against a wall or another actor)
        //   = 1  stock, exactly what PLANCK asked for  <-- ALL DEFAULTS ARE 1.0
        //   > 1  the bone HOLDS its animated pose harder
        // Intended shape: high on COM/spine so she stays put, falling DOWN each limb chain so
        // the extremity gives first. Suggested gradient is in PPB_tuning.txt. Scales
        // positionGain AND velocityGain together to keep the damping ratio sane.
        float reDrive           = 0.f;   // master, ships OFF
        float reDriveCom         = 1.f;
        float reDriveSpine0      = 1.f;
        float reDriveSpine1      = 1.f;
        float reDriveSpine2      = 1.f;
        float reDriveNeck        = 1.f;
        float reDriveHead        = 1.f;
        float reDriveUpperArmR   = 1.f;
        float reDriveForearmR    = 1.f;
        float reDriveHandR       = 1.f;
        float reDriveUpperArmL   = 1.f;
        float reDriveForearmL    = 1.f;
        float reDriveHandL       = 1.f;
        float reDriveThighR      = 1.f;
        float reDriveCalfR       = 1.f;
        float reDriveFootR       = 1.f;
        float reDriveThighL      = 1.f;
        float reDriveCalfL       = 1.f;
        float reDriveFootL       = 1.f;
        // HandStop (2026-08-28): the player's visible hand stops at NPC capsule surfaces.
        // 0 = off · 1 = PROBE (log-only depth episodes) · 2 = reserved for the clamp (stage 2).
        float handStop          = 0.f;
        // The Brace (2026-08-29): one trunk bone goes MOTION_KEYFRAMED (infinite mass) while a
        // player hand pushes it at braceDepthU or deeper; released braceHoldS after the last
        // qualifying contact, with both velocities zeroed. Rest of the body stays dynamic.
        // PushStep (2026-08-29): sustained trunk pressure -> the engine's own bump-walk,
        // re-issued every pushStepRepeatS until pressure has been zero for pushStepStopS.
        float pushStep          = 0.f;
        float pushStepRepeatS   = 0.7f;
        float pushStepStopS     = 1.0f;
        float pushStepWalkU     = 60.f;  // (bump-fallback distance; the FBG handshake)
        float pushStepSpeedU    = 85.f;  // PushWalk planner speed, game units/sec (~walk pace)  // base walk distance per push bump (per-region scaled)
        float pushStepWeapon    = 1.f;   // 1 = weapon pressure on the trunk also drives steps
        // v4 (user spec 2026-08-29): DISPLACEMENT is the trigger, not contact. "I want it to be
        // when her body get pushed away from what she is supposed to be" — engage only when a
        // trunk bone sits ≥ pushStepDispU off its XP32 home (baseline-corrected), and walk a
        // distance PROPORTIONAL to that displacement (3u chest push → ~10u stabilizing step).
        float pushStepDispU     = 2.0f;  // engage threshold: bone displaced ≥ this many game units
        float pushStepDispGain  = 3.5f;
        float pushStepSensor    = 5.f;   // 5 = v5 intent-gap sensor (body vs FK'd drive pose); 4 = legacy body-vs-node
        // v6 envelope (2026-08-30, user spec: gentle push = slow drift, running shove = fast+far;
        // ramp in over ~1s, fade out over ~2s, no stop-pause-restart chain under sustained contact)
        float pushStepSettleS   = 0.35f; // she must be settled this long before a fresh measurement (was 0.8 hardcoded)
        float pushStepRefracS   = 0.25f; // beat between full stop and the next engage (was 0.5 hardcoded)
        float pushStepSpeedRefU = 3.0f;  // the displacement that earns exactly pushStepSpeedU of speed
        float pushStepSpeedMinU = 18.f;  // clamp: gentlest walk
        float pushStepSpeedMaxU = 90.f;  // clamp: hardest shove response
        float pushStepRampInS   = 1.0f;  // seconds to reach target speed (starts at 15%)
        float pushStepRampOutS  = 2.0f;  // seconds to fade to a stop once pressure is gone
        float pushStepExtendMul = 3.0f;  // sustained contact may extend the walk to budget x this
        float pushStepStopU     = 0.75f; // v7: live gap below this = the push ended -> fade out
        // v7.1 (2026-08-30): every movement variable on a knob (user request).
        float pushStepMulChest  = 0.85f; // distance multiplier when the CHEST was pushed
        float pushStepMulBelly  = 1.00f; // ... belly (spine1)
        float pushStepMulWaist  = 1.10f; // ... waist (spine0)
        float pushStepMulCom    = 1.25f; // ... hips/COM ring
        float pushStepMulThigh  = 1.25f; // ... thigh (where a hip press lands)
        float pushStepRampFloor = 0.15f; // ramp-in starting fraction of target speed (0..1)
        float pushStepResumeS   = 0.2f;  // contact this fresh + a real re-push may resume a fade
        float pushStepExitSpeedU= 2.0f;  // fade hands control back below this speed
        float pushStepNearU     = 0.5f;  // contact counts as pressure at/below this surface distance
        float pushStepDirOffDeg = 0.f;   // TEST lever: rotate the walk direction by this many degrees
        float pushStepAccel     = 10.f;  // movement-params override: acceleration  (PLANCK ships 10)
        float pushStepDecel     = 10.f;  // ... deceleration (PLANCK ships 10)
        float pushStepRotPct    = 2.5f;  // ... rotation percent (PLANCK ships 2.5)
        float pushStepAngAccel  = 10.f;  // ... angle acceleration (PLANCK ships 10)
        float pushStepWalkRun   = -1.f;  // gait dial: -1 = keep the actor's own walkRunPercent, else override
        float pushStepVelGainS  = 0.15f; // v7.3: gap GROWTH counts toward the trigger (eff = gap + rate x this)
        float pushStepLeverF    = 1.0f;  // v7.5: measure the push this far up the bone segment (0 = at the joint, 1 = at the child joint) so BEND registers
        float pushStepBreastMul = 2.5f;  // v8.0: a press whose ONLY contacts are BREAST capsules needs dispU x this (their spring levers ~1-1.3u of real body motion from a mere touch)
        float pushStepRampMinFrac= 0.2f;
        float pushStepRateVetoU = 2.0f;
        float pushStepSideGain  = 1.8f;
        float pushStepMulHead   = 1.0f;  // v8.3: distance multiplier for a HEAD push
        float pushStepMulNeck   = 1.0f;  // v8.3: ... neck
        float pushStepHeadTrig  = 1.5f;
        float pushStepPressU    = 0.3f;
        // ★★ TRUE COLLISION-BOX CONTACT for a held object (2026-09-03, user ruling). 1 = rank and
        // gate a held item by its REAL Havok collision shape's oriented box; 0 = the legacy
        // point-plus-capped-pad. The legacy path could not see a big item at all — a 50u armour
        // resting a corner on her chest has its ORIGIN 25u away, so the equip gesture never fired
        // — while inflating the sphere instead is what made a yoke engulf her whole body. A box is
        // big where the item is big and small where it is small, so neither cap applies to it.
        // ⚠ When the box is in use, objectPadMaxU and apiObjectRMaxU are BYPASSED by construction
        // (pad = 0): the box IS the honest reach, so there is nothing to inflate or to cap.
        float apiObjectBox      = 1.f;
        // Refuse a box whose half-extent exceeds this (game units) and fall back to the legacy
        // path. A real two-handed item is ~40u; 150 is a garbage/uninitialised-shape guard, not a
        // tuning value — it must never be the thing that trims a legitimately large armour.
        float objectBoxMaxU     = 150.f;
        float objectPadMaxU     = 8.f;   // v8.9: CAP the held object's bound-radius inflation. A yoke's bound sphere is ~30u: uncapped it engulfs her whole body (measured "d=-30.59u vs head.C2"), every capsule ties for closest, and the equip site becomes a lottery. Interior orifice capsules ignore the pad entirely (see PpbApi).
        // v9.3 (user spec 2026-08-30): the trigger DEPTH scales with push VELOCITY - a gentle
        // touch must travel further before she reacts, a shove almost none. "gate the distance
        // of the shove between .5u at fast speed, and 3u at low speed."
        float pushStepDispSlowU = 3.0f;  // depth required when the gap is barely growing
        float pushStepDispFastU = 0.5f;  // ... when it is growing at pushStepRateFastU
        float pushStepRateFastU = 15.f;  // the growth rate that counts as a full-speed shove
        // ★ v9.5 REACTION TIERS (user spec 2026-08-30): a shove big and fast enough plays the
        // vanilla STAGGER (facing the push), and an enormous one RAGDOLLS her. Both are
        // ANIMATION-side, so they work in the animation-driven states where planner control is
        // forbidden - which is exactly the weapon-push case.
        float pushStepReaction  = 1.f;   // master for the stagger/ragdoll tiers
        float pushStepStaggerU  = 5.f;   // displacement that earns a stagger
        float pushStepStaggerRate = 0.f; // ★ v11 rule 3 (user 2026-09-06 23:30): 0 = NO speed gate on the stagger — "at 10 u it's a shove, so the NPC should do a stumble. 10 u means she bends backward, that's unstable". Set back to 10 in the tuning file to restore the v9.9 speed test HOT, without a rebuild. (was 10.f: "and it must be growing at least this fast (a SHOVE, not a lean)")
        float pushStepRagdollU  = 12.f;  // displacement that knocks her down outright
        float pushStepReactCoolS= 1.5f;
        // ★ v9.7 THE KNOCKDOWN SETTLE. A hard push has the hand/BLADE deep INSIDE her (measured
        // -2.48u for a sword in the chest). The instant she goes dynamic, the solver sees a huge
        // overlap with an infinite-mass object and ejects her - "straight to the ceiling with a
        // sword". For a short window after a knockdown we CLAMP her bodies' speed, so she
        // collapses instead of launching. Hands penetrate less, which is why they were milder.
        float pushStepRagSettleS= 0.6f;  // how long to police her speed after a knockdown
        float pushStepRagMaxVelU= 120.f; // ... and the speed ceiling while we do (game units/s)  // per-actor cooldown between reactions
        // ★ v9.8 ESCALATION (user, 2026-09-06): re-evaluate the stagger/ragdoll tiers every frame
        // DURING a walk with the live gap, so a push that keeps building can promote a gentle
        // back-away into a stumble or a knockdown; a stagger also ENDS the walk (the animation
        // owns her). 0 = the pre-09-06 behaviour: tiers only at engage, a stagger still engages.
        float pushStepEscalate  = 0.f;   // v10.3: OFF by default — the during-walk sensor is corrupted by the walk (see PushStep.cpp); was 1
        // ★ v9.9 (user in VR 2026-09-06): the lateral stiffness gain (pushStepSideGain) also scales the
        // depth that meets the ENGAGE bar and the STAGGER bar, so a sideways push is recognised as
        // readily as a frontal one (the knockdown bar stays raw). 0 = raw depth everywhere (v9.8).
        float pushStepLatTrig   = 1.f;
        // ★ v9.9: the reaction tiers judge speed over this window (s) instead of one frame — 0.6u of
        // sensor wobble at 75 Hz read as +45 u/s and staggered her one frame after engage. The engage
        // bar keeps the one-frame rate (the fast-engage the user likes). Speed reads 0 until the
        // history spans half the window.
        float pushStepRateWinS  = 0.10f;
        // ★ v10.0 LIVE SPEED (2026-09-06, user in VR: "too fast and deliberate ... smoother ... ramping
        // up the speed as the push continues ... ramping down as the push stops"): the walk speed is
        // no longer locked at engage. Every walking frame recomputes a target from the live push depth
        // (× the lateral factor) and the commanded speed SLEWS toward it — up at pushStepSpeedUpU,
        // down at pushStepSpeedDownU (game u/s per second). 0 = the v6 envelope (ramp-in to an
        // engage-locked speed, timed fade-out; pushStepRampInS/Floor/MinFrac/RampOutS then apply).
        float pushStepSpeedTrack = 1.f;
        float pushStepSpeedUpU   = 60.f;
        float pushStepSpeedDownU = 120.f;
        // ★ PALM PROBE (2026-09-06, user in VR: "HIGGS's box IS the palm"): HIGGS's own hand body
        // joins the touch probes as box 5 (PpbApi.cpp CollectProbes). 0 = the four finger boxes only.
        float apiPalmProbe       = 1.f;
        // ★ v10.1 (2026-09-06 ~21:50, user in VR after v10.0 — "the shove happens almost right away";
        // "sideways she stumbled in place, not facing me"; "ramp it up faster, exponential to the push";
        // "the finger is still super prime"):
        float pushStepReactGraceS   = 0.30f; // no stagger/ragdoll tier for this long after engage (the walk-onset transient read as a shove)
        float pushStepStaggerFace   = 1.f;   // on a stagger turn her FIRST: face the player (front/side) or away (behind), then stumble
        float pushStepStaggerFaceDeg= 110.f; // aggressor within ±this of her front = "front or side" → face the player; beyond → face away
        float pushStepStaggerMagMin = 0.25f; // staggerMagnitude floor (was a 0.25 literal; below ~0.25 the vanilla anim barely reads)
        float pushStepSpeedExp      = 2.f;   // walk target = SpeedU × (depth/ref)^exp; the up-slew scales by the same factor
        float pushStepEngageWinRate = 1.f;   // the engage bar reads the WINDOWED growth rate (0 = one-frame, the v9.x "too prime" bar)
        // ★ v10.2 (2026-09-06 ~22:30, user in VR after v10.1):
        float pushStepStaggerDirFlip = 0.f;  // HOT: swap staggerDirection 0 <-> 0.5 if the animation stumbles the wrong way after the facing turn
        float pushStepLiftRag       = 1.f;   // both FEET bodies lifted above her floor (by the player) → ragdoll; the char-controller fall trigger cannot see a ragdoll lift
        float pushStepLiftFeetU     = 12.f;  // how high both feet must be above her origin (game u) for pushStepFallFrames frames
        // ★ v10.3 (2026-09-06 ~23:15): the 22:37 session — five FALSE lifts from trunk pushes 0.2–0.3 s after a walk
        // engaged (walk-onset feet transient), the real GRAB lifts never seen; facing from the player's position rejected
        // ("where the push is acting on the NPC"); the during-walk tiers read a sensor the walk itself corrupts.
        float pushStepLiftSettleS   = 0.6f;  // no lift verdict while walking or this long after a walk ended
        float pushStepStaggerFaceSrc= 0.f;   // 0 = face the PUSH's acting direction (aggressor = opposite the retreat); 1 = the player's position (v10.2)
        float pushStepStaggerTurnMode= 0.f;  // 0 = Actor::SetRotationZ; 1 = TESObjectREFR::SetAngle + Update3DPosition(true); 2 = no turn, vanilla directional stagger
        // ═══ ★ v11 THE FLAT LADDER (report 32 §1, user 2026-09-06 23:30 → 09-07 00:05) ══════════════
        // "push/shove starts to happen after 2 u of detected push from the player from any direction, and
        // that 2 u is not how much the hand is moving, it's how much the Havok joint moved from the
        // XP32-expected location for the current idle animation." The RATE no longer sets the bar — it
        // feeds the walk RAMP (rule 2). The sideways equaliser (pushStepLatTrig/SideGain) stays.
        // MEASURED 2026-09-07 08:32 session: converged PUSHSENSE IDLE noise is 0.01–0.03 u p2p per slot,
        // so a 2 u bar sits ~60x above the floor; the lowest displacement that engaged all session was
        // 2.10 u, and of 8 refused pushes only one (3.51 u) newly engages at 2 u.
        // RETIRED by this block (still parsed, no longer read): pushStepDispU / DispSlowU / DispFastU /
        // RateFastU / BreastMul / HeadTrig / EngageWinRate.
        float pushStepBarU          = 2.0f;  // flat engage bar, every tracked bone, game units
        float pushStepBarHeadU      = 3.5f;  // ★ ABSOLUTE, not a multiplier like the retired pushStepHeadTrig.
                                             // Raised until the head's 3–4 u body-vs-intent offset is found (report 32 §2.4).
        // Rule 7 — held OBJECTS as a push source. ⛔ MASTER SHIPS 0 ON PURPOSE: press-to-equip holds an
        // object against her for equipDwellS (1.0 s) and DeviceGesture exposes no "dwell in flight" query
        // (DeviceGesture.h has only Install/OnFrame/Reset/SetPaused), so arming this without that mute
        // walks her away mid-equip — report 32 §3.5's own warning. Build the mute, then flip.
        float pushStepObjectPush    = 0.f;
        float pushStepObjectBarMul  = 1.75f; // object bar = pushStepBarU x this (~3.5 u) — "most go on the chest"
        // ★ Rule 5 — the feet lift is measured from each NPC's own REST height, not her origin.
        // MEASURED 09-07: Carmella logs heelZ=8.0u all session and her feet sit 8.2–9.0 u above her
        // ORIGIN at rest, so the retired 12 u origin-relative bar demanded ~4 u of REAL lift and never
        // fired (56 watch samples, every one "0 frame(s) up"). NEW KEY on purpose — an old file's
        // `pushStepLiftFeetU 12` must never be re-read as "12 u above rest". pushStepLiftFeetU is
        // RETIRED (still parsed, no longer read).
        float pushStepLiftRestU     = 3.0f;   // v28 2026-09-10 user: "3u is probably high enough" (was 1.5; the live
                                              // file held 8, which with v23's rise stacked on top meant 11u)
        // 0 = log the lift watch EVERY frame (the report 32 §2.3 during-walk data session); else Hz.
        // Ships at 1 Hz so the code default stays a shipping default (package_release MUST_BE_OFF rule).
        float pushStepLiftLogHz     = 1.0f;
        // Rule 9 on the WALK, not only the stumble: front 220° → face the push and walk BACKWARD;
        // rear 140° → face the RETREAT and walk FORWARD (before this she turned ~180°, which reads as a
        // spin). The split angle is pushStepStaggerFaceDeg (110 → 220/140), shared with the stagger so
        // one dial moves both.
        float pushStepWalkFaceSplit = 1.f;
        // ★ v11 PUSHTRIAD probe (user, 2026-09-07): per sense bone, node<->intent / body<->intent / node<->body,
        // 1 Hz idle and 20 Hz while a probe is within 20 u. Read-only. Decides whether the sensor should read the
        // XP32 node (the user's model) instead of the rigid-body origin. SHIPS 0.
        float pushSenseTriad = 0.f;
        // ★ v11 SENSE REWRITE (2026-09-07, from the PUSHTRIAD session): the sensor reads the XP32 NODE against the
        // drive pose, in 3-D, with a baseline that only learns from idle-looking data and stands down during reactions.
        float pushSenseZ            = 1.f;   // include Z in the published magnitude (a backward bend is half DOWN); 0 = XY only
        float pushSenseBaseFreezeU  = 1.5f;  // the baseline never learns while the horizontal gap exceeds this (game u)
        float pushSenseBaseHoldS    = 2.0f;  // ...nor for this long after the last contact/reaction (the rebound is not idle)
        float pushSenseStandDownS   = 2.5f;  // after a stagger/ragdoll we fire: publish zero, learn nothing (must outlast the animation)
        // ★ v11 13:03 (user rule 4 — "the Havok joint that activated the push"): the tiers read the TOUCHED bone.
        // 1 = the largest gap over the trunk chain (a waist push then knocks her down on the chest's swing) — A/B only.
        float pushStepTierChain     = 0.f;
        // ★ v11 TIER HOLD (user 13:15 — "Strong push yes, but no more than 3u deep"): a palm at d=-0.10u spiked
        // the chest 12 u and it rebounded within 60 ms; the keyframed hand cannot go deep because the solver
        // throws her out of its way, so DEPTH is not a measure of a push and DURATION is. The gap must stay above
        // the stumble/knockdown bar this long before that tier fires. 0 = fire on the first frame (13:15 behaviour).
        float pushStepTierHoldS     = 0.15f;
        // ★ v11 HAND TRAVEL (user 14:20 — "wait 3u of hand shove before she start moving. I shove 1u and she move"):
        // the walk may not START until the pushing hand has moved this far since it first touched her. The body bar
        // alone is crossed after ~1 u of hand motion (the keyframed hand throws her ahead of itself). Tiers untouched.
        float pushStepHandTravelU     = 3.0f;  // front/back pushes
        float pushStepHandTravelSideU = 0.f;   // sideways (> ~45° off her front/back axis); 0 = no requirement (user: "except sideway")
        // ★ v11 15:45 PER-BONE BARS (user: "let make those number good for COM/Spine0, but for Spine1 and 2, let's
        // make them 6, 15 and 25 ... same for head/neck"). The same push moves a bone further the higher up the
        // chain it sits — one chest push measured waist 14.93 u / chest 20.77 u / head 54.74 u — so one flat bar
        // makes the trunk stumble late and the head stumble instantly. LOW (COM 11, Spine0 4, thighs) uses the
        // base pushStepBarU / StaggerU / RagdollU; MID = Spine1/Spine2; HIGH = Neck/Head.
        // ⛔ pushStepBarHeadU is RETIRED by this — the head is the HIGH group.
        float pushStepBarMidU        = 6.f;
        float pushStepStaggerMidU    = 15.f;
        float pushStepRagdollMidU    = 25.f;
        float pushStepBarHighU       = 6.f;
        float pushStepStaggerHighU   = 15.f;
        float pushStepRagdollHighU   = 25.f;
        // ★ v11 15:45: how far from her the knock's explosion origin is placed, on the aggressor's side. Only the
        // DIRECTION matters to the engine; the magnitude passed to KnockExplosion stays 0.
        float pushStepKnockSrcU      = 60.f;
        // ★ v11.1 (2026-09-07 evening, user design — report 33 §8.3): the hand-travel anchor is planted on the
        // ENGINE's own contact (Havok narrowphase, every physics step), not on the 4 Hz API snapshot — the snapshot
        // planted it up to 250 ms late, so her head had a quarter-second head start on the hand and the walk gate
        // could never win (16:45:42.990: gap 13.02 u, hand travel 0.00, stagger 155 ms later).
        float pushStepAnchorSrc       = 1.f;   // 1 = engine contact (API anchor stands in while none); 0 = API snapshot only (the 16:32 behaviour)
        float pushStepAnchorTrunkOnly = 0.f;   // 1 = only a TRUNK capsule contact (slots 3/4/5/6/7/8/11) may start the clock; 0 = ANY capsule (the user's words)
        // ★ v11.1: the lift verdict and its rest latch ask the ACTOR whether she is moving on her own (Actor::IsMoving
        // + the walking/running/sprinting gait flags), not only whether PPB's walk drives her — 16:45:48 a walking
        // Sofia ragdolled from a thigh brush, and her rest latch carried a 6 u spread from her own gait.
        float pushStepLiftGait        = 1.f;   // 0 = the 16:32 behaviour (PPB-walk only)
        // ★★ v12 THE TRAVEL SENSOR (user's design, 2026-09-07 evening, verbatim): "Player's hand contact, start
        // calculating distance of push. Pass 3u, start walking sequence. Pass 10u, do stumble. That's it, no
        // complicated gate, swing, state, just distance from origin on the XP32."
        // At first contact PPB captures every sensed node's position IN HER FRAME (origin-relative, de-yawed, so
        // walking and turning cancel); the push is |now - captured| on the TOUCHED bone. In this mode the engage
        // bar, the hand-travel gate, the press gate, the rebound veto, the hemisphere clamp, the peak latch, the
        // rate terms, the lateral factor and the tier hold are ALL bypassed — the distance is the whole test.
        // 0 = the v11 gap sensor with all of its gates (full A/B rollback, hot).
        float pushStepTravelMode      = 1.f;
        float pushStepTravelWalkU     = 3.0f;   // start walking
        float pushStepTravelStumbleU  = 10.0f;  // stumble
        float pushStepTravelRagdollU  = 20.0f;  // knockdown
        // How long the contact may lapse before the NEXT touch counts as a new push (a fresh origin).
        float pushStepOriginHoldS     = 0.5f;
        // ★ v12 (user): "belly and breast got FSMP physic, they MUST be excluded." A soft-body surface yields
        // long before her frame moves, so a contact there is not evidence her BODY was pushed. Those capsules
        // stay fully touchable — the touch API still reports them — they just cannot start or feed a push.
        float pushStepSoftExclude     = 0.f;   // v13: OFF — the contact no longer decides the number (see PushStep.cpp)
        // ★ v12a (2026-09-07 19:06): while she moves under her OWN power her trunk bones travel 10-17 u per stride
        // in her own frame, so the travel sensor cannot see a push through her gait — and an animation-driven actor
        // refuses planner control anyway, leaving the stumble as the only thing that can fire. While self-moving the
        // origin is re-captured every frame and neither the walk nor the tiers may fire. 0 = measure through her gait.
        float pushStepSelfMoveGate    = 1.f;
        // ★★ v13a: how many of the tracked joints VOTE on the push magnitude, in the order
        // COM, Spine0, Spine1, Spine2, Neck, Head. 3 = the trunk BASE only. Measured 00:16:49 on one slight
        // chest push: COM 1.4 / spine0 2.4 / spine1 5.2 / spine2 8.4 / neck 9.5 / head 9.8 — the head reads 7x
        // the COM because it sits at the end of the lever, so letting it vote made every push a stumble.
        // All eight joints are still measured and printed on every receipt; this only chooses who sets the number.
        float pushStepVoteN           = 3.f;
        // *** v14 THE LEVER (user 2026-09-08): "the bar decide the movement, the lever is for deciding the
        // ramp and speed ... detecting how fast it goes from 0 to 5, so make a ramp goes accordingly. The
        // starting ramp should be aggressive, the ending ramp should be smooth."
        // The lever is the CROSSING RATE: how fast her worst voting joint travelled the WALK BAR's distance
        // from its animation point. Measured on the SAME number the bar uses, so it re-normalises for free
        // whenever the bar moves. It replaces `eff` (= displacement + rate x 0.25), which mixed a distance
        // and a velocity with no natural scale and needed a hand-picked reference: 42 x (disp/2)^2 hit the
        // 60 u/s ceiling at 2.39 u, BELOW the 5 u bar itself, so every walk engaged pinned at max speed and
        // max distance. Speed and distance are now INTERPOLATIONS across their own bands and cannot pin.
        float pushStepLever           = 1.f;    // master. 0 = the pre-v14 eff mapping (hot A/B)
        float pushStepCrossRefU       = 30.f;   // crossing rate (u/s) that earns FULL speed and distance
        float pushStepCrossExp        = 1.f;    // curve on the normalised lever (1 = linear)
        float pushStepCrossMinFrac    = 0.40f;  // the gentlest push still walks this fraction of the cap
        // v14a: the CROSSING CLOCK floor. capturedS is pinned to the FIRST frame of a contact episode (the
        // non-forced captures early-return once an origin is live), so a hand that RESTS on her for 2 s and
        // then shoves divided the bar by 2.15 s and graded the hardest shove as the gentlest lean. While her
        // travel is under this floor the push has not begun, so the stopwatch keeps re-arming.
        float pushStepCrossFloorU     = 0.5f;
        // *** v14d PER-NODE BARS (user 2026-09-08: "add 3u to COM and Spine0, to all three ladder").
        // Every node is still measured 1:1 on its OWN movement from its OWN animation point -- nothing is
        // scaled, nothing is multiplied. A node is simply allowed its OWN threshold, because the same shove
        // moves her pelvis and her head by very different amounts: pushing the pelvis translates the whole
        // body (every node reads alike), while pushing the chest pivots her about the hips so the head
        // travels much further than the base. This offset is added to all three rungs (walk/stumble/ragdoll)
        // for that node only. 0 = judged on the shared ladder.
        float pushStepOffCom          = 3.f;
        float pushStepOffSpine0       = 3.f;
        float pushStepOffSpine1       = 0.f;
        float pushStepOffSpine2       = 0.f;
        float pushStepOffNeck         = 0.f;
        float pushStepOffHead         = 0.f;
        // *** v29c PER-NODE RUNGS (user 2026-09-11: "the ladder is 10/20/35, let's make it 10/30/40 for head and
        // neck. keep everything else as is"). pushStepOff<node> above shifts ALL THREE rungs together, so it cannot
        // give one node a different stumble and ragdoll. These are ABSOLUTE bars for that node's rung:
        // 0 = the shared ladder + pushStepOff<node>, exactly as before. The WALK rung stays pushStepOff<node>'s
        // business. The during-walk terms (pushStepOffWalkAdd + pushStepWalkOff<node>) are still added on top of an
        // override, because they exist to cancel her own gait's reading. The ladder can never invert (a stumble bar
        // below the walk bar is raised to it, a ragdoll bar below the stumble bar likewise).
        // ⚠ WHY head/neck: measured 2026-09-11 (20:18-20:21), the HEAD was the deciding joint in 18 of 20 reactions
        // — one ragdoll read COM 1.9 / spine0 4.5 / spine1 7.6 / spine2 13.5 / neck 27.9 / HEAD 34.2. It is the end
        // of the lever, so on the shared 35 it crossed first almost every time.
        float pushStepBarStumbleCom    = 0.f;
        float pushStepBarStumbleSpine0 = 0.f;
        float pushStepBarStumbleSpine1 = 0.f;
        float pushStepBarStumbleSpine2 = 0.f;
        float pushStepBarStumbleNeck   = 30.f;   // user 2026-09-11: neck 10/30/40
        float pushStepBarStumbleHead   = 30.f;   // user 2026-09-11: head 10/30/40
        float pushStepBarRagCom        = 0.f;
        float pushStepBarRagSpine0     = 0.f;
        float pushStepBarRagSpine1     = 0.f;
        float pushStepBarRagSpine2     = 0.f;
        float pushStepBarRagNeck       = 40.f;
        float pushStepBarRagHead       = 40.f;
        // *** v29d EQUIP SETTLE (user 2026-09-11: "just filter out a lift made due to an equip event … check the new
        // height .25sec after the equip, and it's now the new floor. Same for all the other node we check for that.
        // Don't want her start to walk back or something. An equip event prevent ragdoll or stumble."). Measured
        // 20:46:00.461: hand-equipping heeled boots with a hand on her thigh raised BOTH feet ~8 u and fired
        // LIFTED — BOTH FEET (+8.5 / +8.1) against a rest latched barefoot. Seconds; 0 = off.
        float pushStepEquipSettleS     = 0.25f;
        // *** v16 THE WALK WINDOW (user 2026-09-08: "is it possible to recognize that the NPC is still
        // being pushed while she walk?"). The travel origin was captured ONCE, at walk-engage, so
        // everything after it accumulated - including her own backward-walk animation and its servo lag.
        // Measured: the head crossed the 20u stumble bar 0.30-0.41 s after engage on three separate walks,
        // each firing the instant pushStepReactGraceS (0.30) expired. She was stumbling out of her own
        // walk. Re-capturing on a cadence changes what the number MEANS while she is driving: no longer
        // "how far has she moved since the walk began", but "how far has the player pushed her BEYOND what
        // the walk is already doing" - which is exactly the question the user asked for.
        // 0 = off (the single capture-at-engage, pre-v16).
        float pushStepWalkReCapS      = 0.4f;
        // *** v16 DURING-WALK BAR BONUS (user: "i like the current setting for starting, but once it
        // started walking, if it's too sensible, it is a problem"). Added to every voting node's three
        // bars WHILE SHE IS DRIVING, on top of that node's own offset. So the engage sensitivity and the
        // during-walk sensitivity are separate dials: pushStepOffHead tunes the head at engage,
        // this tunes everything once she is already moving. 0 = same bars walking as standing.
        float pushStepOffWalkAdd      = 0.f;
        // *** v17 STEER THE WALK (user 2026-09-08: "the push of my sword wasn't completely in the same
        // direction as the walk travel, and cause of the offset, she end up stumbling as i was pushing
        // sideway from her walk ... i just grabbed her hand and pulled her toward me, and that direction
        // change is what i want"). dirX/dirY and angleZ were captured at engage and fed unchanged every
        // frame, so a push that drifted off that axis piled up as sideways displacement and crossed the
        // stumble bar. Measured across 6 during-walk stumbles: the bearing had swung 36-188 degrees from
        // the engage bearing, every time. The direction is ALREADY computed every frame (TravelOf hands
        // back the world dx/dy with the magnitude) - it was simply being discarded.
        float pushStepDirTrack        = 1.f;    // 0 = engage-locked direction (pre-v17)
        float pushStepDirTrackDeg     = 120.f;  // max degrees/second the walk may turn toward the live push
        float pushStepDirTrackMinU    = 2.f;
        // *** v18 NO DISTANCE CAP (user 2026-09-09: "That max 48u is wrong. Delete that. No max. What
        // decide max is the time of the push."). The walk used to end on a DISTANCE allowance:
        // pushStepWalkU x region = the budget, x pushStepExtendMul = a HARD cap, and a cap-fade was
        // marked final-no-resume. Measured at 90 u/s that whole allowance was spent 0.9 s after engage,
        // leaving ~1.8 s of unresponsive coast during which the player was still pushing - so the
        // displacement piled up with no walk left to relieve it and she stumbled at the end of her own
        // retreat. With this on, the walk ends when the PUSH ends and nothing else: pressure gone for
        // pushStepStopS starts the fade, and the fade is never final, so a renewed push resumes it.
        // The deceleration is then genuinely dynamic - the live follower tracks the push down through
        // pushStepSpeedDownU - instead of a distance running out underneath her.
        // 0 = the pre-v18 budget/extend/hard-cap behaviour (hot A/B; the knobs stay parsed).
        float pushStepNoDistCap       = 1.f;
        // *** v19 RAGDOLL ESCALATION. pushStepReactCoolS is a flat per-actor cooldown that blocks EVERY
        // reaction. Because the displacement climbs continuously, she crosses the stumble bar on the way
        // to the knockdown bar on every push - so the stagger fires first, stamps the cooldown, and the
        // ragdoll she is now earning is locked out for the next 1.5 s. Measured: 13 staggers to 2
        // ragdolls, and both ragdolls were pushes fast enough (+180 / +99 u/s) to jump the whole band
        // between two evaluations. A HIGHER tier is an escalation, not a repeat, so it may now pre-empt
        // the cooldown. Ragdoll -> ragdoll stays fully gated, so she can never chain-fall.
        float pushStepRagEscalate     = 1.f;    // 0 = flat cooldown for every tier (pre-v19)
        float pushStepRagEscalateMinS = 0.15f;  // a stagger must have stood this long before a knockdown
                                                // may pre-empt it - stops both firing on one frame
        // *** v20 PER-NODE WALK OFFSETS (user 2026-09-09: "augment the threshold of some node once the
        // walk start"). While she walks, her own gait rotates her about the hips and every node reads
        // high - by an amount that scales with height up the spine. MEASURED p90 lift, 99 standing vs
        // 120 walking samples: COM +3.5, Spine0 +6.8, Spine1 +13.8, Spine2 +23.9, Neck +41.7, Head +47.1.
        // Standing, the head is the QUIETEST node (p90 3.4); walking it sits +47 above its animation
        // point before the player has touched her - past the stumble bar AND the knockdown bar.
        // These are added to that node's THREE rungs while driving, on top of its standing offset.
        // ⛔ ALL THREE RUNGS, deliberately: this compensates a contaminated READING, not a threshold
        // balance. Lift only the stumble bar and the knockdown bar stays under the gait lift, so she
        // would ragdoll from walking instead of stumbling from it.
        // At these values every node lands 4-5u below its own walk bar at walking p90, so the gait
        // engages nothing and whichever node the PUSH actually loads is the one that crosses.
        // v29 (report 35 §0.5.1, user 2026-09-10 "yes"): the ZEROS the user approved on 09-09 are now the compiled defaults
        // too - the tuning file held 0 while these read 5/5/15/25/40/45, so a reverted file silently raised the head's walking
        // ragdoll bar to 80. Behaviour with the live file (all six at 0) is unchanged.
        float pushStepWalkOffCom      = 0.f;
        float pushStepWalkOffSpine0   = 0.f;
        float pushStepWalkOffSpine1   = 0.f;
        float pushStepWalkOffSpine2   = 0.f;
        float pushStepWalkOffNeck     = 0.f;
        float pushStepWalkOffHead     = 0.f;
        // *** v21 THE STOP IS A DISTANCE (user 2026-09-09: "ramp down start right away and as fast as
        // the ramp down, i want her to travel 1/3 of her current travel distance"). pushStepSpeedDownU
        // is a constant slew, so the distance she coasts depended entirely on the speed she happened to
        // be carrying - 90 u/s at 50 u/s/s runs on for ~81u, 30 u/s for only ~9u. The stop is now solved
        // from the DISTANCE instead: a = v^2 / 2d. `d` is latched the moment the push stops, as this
        // fraction of the distance she had walked up to that point, so a long retreat coasts long and a
        // short shove stops short - both in proportion. 0 = the old constant slew.
        float pushStepStopFrac        = 0.3333f;
        float pushStepStopMinU        = 5.f;    // never solve for less than this - she must not stop dead
        float pushStepStopMaxA        = 400.f;  // ceiling on the solved deceleration (u/s per second)
        // *** v23 THE LIFT GATE, from the 09-09 watch-only session (63 would-fires, 3 real / 4 false).
        // Height alone does NOT separate them: the sitting false positive reached 27.9u, higher than a
        // real lift's peak. What separates them is that a real lift takes BOTH feet up TOGETHER and they
        // KEEP RISING, while the false one was one foot parked 26u above the other, frozen for 3.5 s.
        float pushStepLiftSymU        = 8.f;   // the two feet must be within this of each other
        float pushStepLiftRiseU       = 3.f;   // ...and both must have RISEN this far since the episode
                                               // began. A frozen foot, however high, is not a lift.
        float pushStepLiftRestSpreadU = 4.f;   // rest-latch sanity: reject a sample window looser than
                                               // this (was a hard-coded 8; only 1 latch took all session
                                               // and its spread was already 4.6)
        // *** v28 (2026-09-10) THE LIFT ZONE + RISE FROM CONTACT. User: "Higher thigh contact is for a push/shove,
        // anything lower than mid thigh is for a leg lift, and if there is lift from com, it's a leg lift."
        // PUSH ARMING IS UNTOUCHED (any capsule still arms the push, v13d) - these only decide who may be BLAMED
        // for her feet leaving the floor. MEASURED first: the one real between-the-legs lift (12:09) touched
        // "mid thigh" (child 4), which sits ~0.60 of the way hip->knee - a split by capsule NAME would have lost it,
        // so the thigh is split by the contact POINT's position along the bone.
        float pushStepLiftZoneRule        = 1.f;    // 1 = only calf/foot/pelvis/LOWER-thigh contacts attribute a lift; 0 = any (v11)
        float pushStepLiftThighSplit      = 0.5f;   // thigh contact point as a fraction hip(0)->knee(1); >= this = lift zone
        float pushStepLiftRiseFromContact = 1.f;    // 1 = the RISE term counts from the lift contact (v28); 0 = from the
                                                    // first above-the-bar frame (v23), which stacked the rise ON the bar
        // *** v29 (2026-09-10) THE COLLISION CAPSULE, ONE LEG, THE TRIP, THE FLOOR. User: "Use the collision capsule, we own
        // them" / one leg "Yes, ~5 u" / "keep the walk, but ragdoll condition trump walk ... if she walk and her feet end up
        // lifting higher than the intended walk location, than ragdoll" / "can we do a floor level check". ⚠ v28's "C4 sits
        // ~0.60 hip->knee" above was the COMPILED default capsule; the live dial puts C4 at 0.41-0.56 (report 37 §2.1).
        float pushStepLiftEngineAttr  = 1.f;    // 1 = the Havok collision capsule a hand / palm / weapon hit attributes a lift (physics rate)
        float pushStepLiftThighMask   = 112.f;  // thigh children that credit a lift, bit N = child N (112 = C4 mid, C5 lower, C6 knee);
                                                // the rod (child 0) is placed by pushStepLiftThighSplit
        float pushStepLiftOneLegU     = 5.f;    // ONE LEG: the touched leg's foot rises this far from the contact -> ragdoll; 0 = off
        float pushStepLiftWalkVerdict = 1.f;    // 1 = while walking / moving on her own, judge the feet against the ANIMATION and fire;
                                                // 0 = no verdict while walking (v28)
        float pushStepLiftWalkFrames  = 10.f;   // consecutive frames a trip must hold while walking (strides and servo lag are brief)
        float pushStepLiftGroundU     = 1.5f;   // FLOOR: after a knockdown / seat both feet within this of rest (and the floor clamp); 0 = off
        float pushStepLiftGroundS     = 0.2f;   // ...for this long before the lift re-arms
        // *** v29e THE FLOOR WAIT HAS A DEADLINE (user 2026-09-11). The floor re-arm above only ever had to cover the
        // GET-UP's own leg motion (+3 to +7 u within ~30 ms of RECOVERED). But it waits for both feet to come back to
        // the band, and if the player keeps sweeping her feet up they never do — measured 21:42:50 (feet +36.3/+32.3,
        // 35 qualifying frames) and 21:43:55 (+46.0/+43.5) both sat at `floor WAITING` and were IGNORED; every lift
        // that did fire came 1-2 s after a `FLOOR re-armed` line. So the wait expires this long after she is back on
        // her feet (knock normal / off furniture), armed or not. 0 = no deadline (pre-v29e).
        float pushStepLiftGroundMaxS  = 0.6f;
        float pushStepLiftSitGuard    = 1.f;    // 1 = no lift verdict while sitting / sleeping / in furniture / swimming / mid-killmove
        // ★★ 2026-09-12 THE SIX-STATE FURNITURE RULE (user ruling, from the v34 FURNPROBE sessions). "In furniture" used to
        // mean "sit state != Normal OR an occupied-furniture HANDLE". The handle is a RESERVATION, not a pose: measured
        // 19:11:52 a chair handle 251u from its seat with sit=Normal while she walked to it (Travel package); 19:12:13 a
        // handle to ANOTHER chair 932u away for 70+ s after she stood up (MovementBlocked); 19:26 a bedroll handle 22,230u
        // away (another cell) on an idle NPC. Each blinded the push sensor ("0 of 8 joints read") and held the lift in
        // `floor WAITING SEATED`. Busy is now her BODY's sit state only: 2 WaitingForSitAnim, 3 IsSitting, 4 WantToStand,
        // 6 WaitingForSleepAnim, 7 IsSleeping, 8 WantToWake. 1 WantToSit / 5 WantToSleep ("on her way") are FREE.
        // Applies to push / shove / feet-lift ONLY; ObjectHold::ActorRagdollAttached (ReScale / conform calibration) is
        // untouched. 0 = the old handle-or-any-state rule, exactly (A/B lever).
        float pushStepFurnSitState    = 1.f;
        // ★★ 2026-09-12 THE LEAN RULE (user ruling, v36, REPLACES v35's "a lean stays pushable"): "all lean are not pushable,
        // but can be leg sweep — someone leaning against something is really stable, but will still fall if their legs are
        // swept." A LEAN = busy by sit state AND either her nearest marker on the occupied furniture is lean-only (within
        // 150u — WallLeanMarker, RailLeanMarker, the ZaZ bed's lean marker) OR the furniture carries a lean keyword: the
        // census (39_research/furniture_census.txt) found counter / bar-counter / lean-table / soldier-wall leans are SIT
        // markers, so they are named by keyword (FurnitureCounterLeanMarker, isBarCounter, isLeanTable, isIdleSoldierWall,
        // ZaZ zpfFurnitureWallLean / zpfFurnitureRailLean — verified in the load order). Modes:
        //   0 = a lean is as busy as a chair: no push, no shove, no sweep
        //   1 = (default, the ruling) no push / shove / stumble / push-knockdown; the FEET path (sweep lift + unsupported
        //       fall) still knocks her down
        //   2 = a lean is fully free: push and sweep (v35's behaviour, kept as the A/B lever)
        float pushStepFurnLean        = 1.f;
        // *** v29b THE PELVIS BAR (user 2026-09-10 "Pelvis bar 5 u"). The 22:47 log: a 4.8 s finger touch on her pelvis lifted
        // both feet +3.3 / +2.8 against the 3 u bar - 0.2 u from a false ragdoll - while the real 12:09 crotch lift reached 26-34 u.
        float pushStepLiftComU        = 5.f;    // a both-feet lift credited to a PELVIS capsule needs both feet >= this in height AND
                                                // rise since contact; leg contacts keep the normal bar; 0 = the same bar as the legs
        // *** v24 COMBAT GATE (user 2026-09-09: "Not in combat and other stuff like that thou").
        // An NPC who is FIGHTING does not get shoved around by the player: no walk-back, no stumble, no
        // knockdown, no lift ragdoll. She is busy in a way the game itself is arbitrating, and a push
        // reaction there both reads wrong and steps on the combat system.
        float pushStepCombatGate      = 1.f;   // 0 = off (pre-v24: reactions fire in combat too)
        // Killmoves and paired animations: both participants are driven by one synchronised clip, so
        // knocking one down mid-scene looks broken and can desync the pair.
        float pushStepKillMoveGate    = 1.f;    // the live travel must exceed this before its direction is
                                                // trusted - just after a re-capture the reading is ~0 and
                                                // its direction is pure noise
        // ★ MOUTH PROBE (2026-09-06, user: "a smaller collision box at the bottom front of the head collider … the
        // player's mouth"): a short segment on the head box's FRONT face, below eye level, pushed mouthProbeOutU
        // proud; the touch scan names it "mouth" (against "face"/"head"). Sensing only — the head box is the collider.
        float mouthProbe        = 1.f;
        float mouthProbeOffXU   = 0.f;    // lateral, head frame
        float mouthProbeOffZU   = -4.5f;  // below the box centre (eye level) — the mouth
        float mouthProbeOutU    = 0.f;    // proud of the front face (a probe must not claim reach the collider lacks; apiTouchU already senses 1 u out)
        float mouthProbeHalfWU  = 1.5f;   // half-width of the mouth segment
        float mouthProbeR       = 1.2f;   // radius
        // ★★ KISS (2026-09-12). The mouth probe above had never been seen firing in VR. Two SUSPECTS
        // (not proven causes — mouthProbeLog settles it), both in how it was POSED, not in the scan:
        //  (1) it took its axes from the head box rider, UprightHmdNode (+0x580), which PPB and
        //      two sibling mods ASSUME is yaw-only — if so, it ignores head pitch and roll;
        //  (2) every offset was multiplied by the 3rd-person HEAD BONE's scale, which has nothing
        //      to do with where the player's real face is (it reads 1.000 on this rig, so a no-op here).
        // mouthProbeSource 1 lets the headset node that carries FULL rotation — PlayerCharacter+0x570,
        // which CommonLibVR 4.14.0 mislabels `GamepadNode` (VRIK's own PDB enum: kNode_HmdNode = 48 ->
        // 0x3F0 + 48*8 = 0x570; HIGGS/PLANCK/VRIK all read the HMD there) — STEER where on the face the
        // mouth is, while the probe stays PINNED to the collider's front face (HandBox MouthProbeSegment).
        // ⛔ Corrected after review: the first cut pushed the probe along the PITCHED forward and it left
        // the face — inside the box looking down, proud of it looking up. Pinning to the face fixes that.
        // 0 = the old pose exactly (A/B lever). Falls back to 0 on its own if +0x570 fails a sanity check.
        float mouthProbeSource  = 1.f;
        float mouthProbeLeverU  = 2.f;    // ★ kiss steering ARM: how far the mouth sits in front of the eyes (u).
                                          // Anatomy, NOT the box half-depth (using halfYU 6u lifted the probe to eye
                                          // level by 30 deg of upward gaze). No effect at level gaze. Source 1 only.
        float mouthKissLips     = 1.f;    // ★ user ruling 2026-09-12: a kiss ALSO fires PPB_MouthLips.
                                          // HARD-CAPPED AT LIPS — the head feeds a separate flag that
                                          // the ENTER/THROAT logic never reads, so it can never open her mouth.
        float mouthKissLipU     = 2.2f;   // mouth probe -> her UPPER LIP capsule (head C1) surface gap
                                          // that counts as a kiss. C1 ONLY: the fingertip LIPS rule also
                                          // needs both chin lines, which a mouth on her lips rarely reaches.
        float mouthKissExitU    = 3.2f;   // ★ KISS HYSTERESIS (2026-09-12 VR, first kiss session): with one
                                          // threshold the kiss FLICKERED — 6 LIPS-ON edges in 8 s at gaps
                                          // 1.69..2.19u, each a PPB_MouthLips 1/0 pair to VRTE and an oral
                                          // 0.25 open/close. A kiss now STARTS under mouthKissLipU and ENDS only
                                          // past this. Applies ONLY to a LIPS stage the kiss itself raised — a
                                          // finger-raised LIPS never borrows it (a mouth hovering at 3u that never
                                          // met the entry gate must not hold a kiss). Accessor floors at mouthKissLipU.
        float mouthProbeLog     = 0.f;    // DIAGNOSTIC: ~0.5 Hz while a lip is within 12u — the kiss gaps to
                                          // C1/C2/C3, and forward.z of BOTH headset nodes (settles whether
                                          // UprightHmdNode is yaw-only in one session: look down, compare).
        float furnProbe         = 0.f;    // ★ DIAGNOSTIC, READ-ONLY (2026-09-12, report 39 §3 prerequisite): a FURNPROBE
                                          // line per driven NPC within 1000u on every change of her furniture state,
                                          // plus a 1 s heartbeat while furniture is involved. Prints each engine
                                          // signal SEPARATELY — sit/sleep state, occupied furniture + distance to its
                                          // nearest marker + that marker's animation type (sit/sleep/lean), knock,
                                          // ragdoll, AI, killmove, the running package — and the ActorRagdollAttached
                                          // verdict they add up to. Why: the 17:26-17:27 session showed the push
                                          // sensor blind ("0 of 8 joints read") in a chair AND on the stairs on her
                                          // way to furniture, and the SEATED label only printed the union. Writes nothing.
        // ★ OUTFIT GUARD (2026-09-06): hook Actor::HasOutfitItems so a hold-pool NPC stripped naked is
        // never re-issued her outfit by the engine (OutfitGuard.h). Installs only if the runtime prologue
        // decodes cleanly — fail closed. 0 = do not install.
        float outfitGuard       = 1.f;
        float pushStepFacePush  = 1.f;   // v9.3: she TURNS toward the push and walks backward, instead of strafing with locked facing
        float pushStepBaseAlpha = 0.03f; // v8.8: how fast the sensor's REST-LAG baseline tracks her own idle deviation (frozen whenever a player probe is within 20u of the bone)
        float pushStepPeakWindowS= 0.45f; // v8.7: a latched push PEAK stays usable this long (fast pushes are impulses: they spike and collapse before the gates open)
        float pushStepIdleProbe = 0.f;   // v8.6 DIAGNOSTIC: 1 = log all 7 deviations at 1 Hz for the last-touched actor even with NOTHING touching her (is the body shaking, or the reference?)  // v8.4: ENGAGE needs a contact actually touching (<= this); hover at nearU only tracks (crowding with resting hands must not walk her)  // v8.3: head trigger needs dispU x this (easiest bone to displace; face touches must stay inert)  // v8.2: lateral gaps count this x toward trigger/speed/distance (the trunk is stiffer sideways and twist is lever-invisible)  // v8.1: engage vetoed while the gap COLLAPSES faster than this (rebound echo, not a push) // v8.0: hard pushes shrink the ramp-in to as little as this fraction (time-to-speed follows push intensity)
        float pushStepLeverGateU= 0.35f; // v7.9: the lever's ROTATION term only counts once the joint ITSELF displaced this far - a real push always moves the joint; pure rotation with a still joint is breast/belly torque ringing
        float pushStepHemiDot   = 0.0f;  // v7.8: reject engage directions pointing INTO the player's hemisphere (dot vs away-from-player must exceed this; -1 disables). A push can never PULL
        float pushStepChain     = 1.f;   // v7.6: spine NEIGHBORS of the touched bone may also trigger (whichever link registers first); pelvis only when touched  // walk distance = displacement × this, capped by pushStepWalkU × region
        float brace             = 0.f;
        float braceNearU        = 0.5f;
        float braceHoldS        = 0.25f;
        float sceneMode         = 2.f;
        float bumperSceneOff    = 1.f;
        float bumperSceneExcite = 1.f;   // excitement rank at/above which collision drops. The
                                         // faction rank DECAYS after a scene, so 1 covers the
                                         // scene plus its settle; raise it to gate later/tighter.
        float planckLoosenOurs = 1.f;   // PivGuard v2: PLANCK pivot collapse 0 during PPB-skeleton
        // ── LIVE PLANCK DRIVE GAINS (2026-09-09, the extremity-jitter hunt) ──────────────
        // PLANCK re-stamps these three into hkaKeyFrameHierarchyUtility::ControlData for EVERY
        // driven body EVERY frame (planck main.cpp ~4819), and registers them by name in its own
        // settings registry (planck config.cpp:328), so a runtime write lands on the next frame
        // with no ragdoll rebuild. -1 = leave PLANCK alone (default). >=0 = force that value.
        //   hier: Havok default 0.17, PLANCK ships 0.6. 0 = target in MODEL space (Havok's own
        //         header: "much stiffer and more stable"); 1 = target relative to the PARENT's
        //         physical state, so servo error compounds root->leaf and the EXTREMITIES ring.
        //   vel/pos: PLANCK ships 0.6 / 0.05.
        // ★ Even at -1 the values are READ BACK and logged once at startup — that is how we learn
        //   what PLANCK actually holds (its ini is NOT hot-reloaded and it logs no values).
        // ── RIGHT-ARM 4-CHANNEL PROBE (2026-09-09, the extremity-shake hunt) ─────────────
        // 1 = log, every second, for NPC R UpperArm / R Forearm / R Hand, the per-frame TRAVEL of
        // each of four channels so we can finally say WHICH layer is moving:
        //   node = the visible XP32 bone (what you SEE)
        //   body = the Havok ragdoll rigid body (what PHYSICS holds)
        //   anim = the incoming drive target (where the ANIMATION says it should be)
        //   xp   = PPB's XP32 chain value (where the CONFORM says it should be)
        // path = distance travelled summed over the second; net = start-to-end; ratio >>1 = it
        // vibrated in place. A channel with path ~0 while the user SEES shaking is not moving.
        float armProbe = 0.f;
        // ── LEG PROBE (2026-09-09, user: "add the foot probe") ──────────────────────────
        // ARMPROBE's twin for the legs, because the reported jitter is in the hands AND FEET and
        // only the arm was instrumented. Slots: R Thigh, R Calf, R Foot, L Foot - the right leg is
        // a CHAIN (where does the shake enter?) and the left foot is a SYMMETRY CONTROL (a cause at
        // the ROOT moves both feet together, a cause at one body does not).
        // It also carries the channel ARMPROBE lacks: `arot`, the drive target's own ANGULAR path.
        // anim/xp are parent-LOCAL translations, i.e. a constant bone length, which is why the arm
        // log reads anim path 0.00u forever. Rotation is where a limb's motion actually lives, so
        // arot is the only channel that can say whether the ANIMATION POSE is the thing shaking.
        float legProbe = 0.f;
        // ── DRIVE VELOCITY DAMPING (2026-09-09) ─────────────────────────────────────────
        // hkaKeyFrameHierarchyUtility::ControlData::m_velocityDamping — Havok's own words:
        // "This gain dampens the velocities of the bodies. The current velocity of the body is
        // scaled by this parameter on every frame before the controller is applied." SDK default
        // is 0.0, and NOTHING in this stack ever writes it: PLANCK re-stamps ONLY hierarchyGain,
        // velocityGain and positionGain each frame, so a value we write SURVIVES. This is the one
        // gain Havok built to kill a servo limit cycle, and it has been sitting at zero forever.
        // -1 = do not touch (default). 0..1 = write that value into EVERY ragdoll body's element.
        // Start ~0.1. Too high = the limbs feel sluggish/laggy following the animation.
        float driveDamping = -1.f;
        // ── FORCE KEYFRAME (2026-09-09, the decisive shake test) ────────────────────────
        // 1 = set every ragdoll body of every PPB-driven actor to MOTION_KEYFRAMED. A keyframed
        // body is NOT solved: infinite mass, no gravity, no contact response — it simply goes
        // where it is told. So this splits the question nothing else has:
        //   shake STOPS  -> the vibration is manufactured in the PHYSICS SOLVE.
        //   shake STAYS  -> the solve is innocent; the pose handed to it already shakes
        //                   (animation graph, or something writing the pose track — e.g. the
        //                    conform's stale-alternate-frame target at poseConformEveryN > 1).
        // ⚠ CAVEAT (master ref Part 05 §5): under PLANCK a keyframed body does NOT track the
        // animation exactly — PLANCK clears kSyncOnUpdate every frame, so it still follows the
        // servo WITH LAG and postPhysics writes that lag to the mesh. A keyframed body that
        // shakes LESS is therefore ambiguous; one that goes STILL, or shakes exactly as much,
        // is decisive. DIAGNOSTIC ONLY — she cannot be pushed or ragdolled while this is on.
        float forceKeyframe = 0.f;


        float planckGainHier = -1.f;
        float planckGainVel  = -1.f;
        float planckGainPos  = -1.f;
        float planckLoosenGlobal = -1.f; // LIVE A/B on PLANCK's GLOBAL loosenRagdollConstraintPivots.
                                         // -1 = leave PLANCK alone (default, PLANCK's own value).
                                         //  0 = force OFF, 1 = force ON. Applied on CHANGE from the
                                         // frame callback and READ BACK, so the log records what
                                         // PLANCK actually holds. PLANCK has no MCM in VR; this is
                                         // the only way to judge the band-aid by eye.
                                        // drives (global stays stock 1), with stranded-pivot self-heal
        float dgHeadPlanck     = 0.f;   // 1 = do NOT PLANCK-ignore severed-head clones (grab test)
        // dgVictimPlanck (2026-08-02, the floating-draugr fix — user-reported, A/B-confirmed):
        // 1 = a dismembered VICTIM (a real actor that died) is LEFT UNDER PLANCK, exactly as it
        // is when PPB is not installed. Only genuine head-clone PROPS (NGD IsHead, or an FF-ref
        // spawn that never died) keep the permanent ignore PPB was built for.
        // 0 = pre-fix behaviour: any DF/NGD-touched actor is PLANCK-ignored PERMANENTLY, which
        // stranded decapitated draugr in mid-air because the ONLY restore path is gated on
        // !permanent and `permanent` is never cleared.
        float dgVictimPlanck   = 1.f;

        // genProbe (2026-08-02) — RESEARCH INSTRUMENT for the coming MALE bodies. Logs, per
        // actor, which candidate signal actually tracks "the schlong is exposed": chain-node
        // presence, geometry under the chain, the kHidden flag, and body-slot armor/revealing.
        // Edge-logged (one line per state CHANGE). Ships OFF; measures only, moves nothing.
        float genProbe         = 0.f;

        // ── MALE GEOMETRY GATE (2026-08-13) — SHIPS OFF, and off is the SAFE state ───────────
        // PPB decides "do we own this actor's geometry?" with a SHAPE test: COM is a bhkListShape
        // (GrabDiag::ActorCarriesBake). That was sufficient proof only while PPB shipped FEMALE
        // skeletons exclusively — every stock skeleton (male, draugr, creature) keeps a single
        // bhkCapsuleShape COM, so "COM is a list" meant "we baked it".
        //
        // A male PPB skeleton breaks that inference, and BOTH geometry writers re-arm on males:
        //   * CapFixApply       writes the globally FEMALE-DIALLED cap* knobs into his capsules;
        //   * PivScaleCorrect   rewrites his 17 ragdoll pivots off the female kBaseArc 48.045;
        //   * ReShape feeds those writes ratios measured from kUvLandmarks — CBBE/3BA FEMALE
        //     atlas coordinates (nipple / breast up+down / chest centre / butt cheek). On a male
        //     mesh they land on unrelated geometry. That exact failure was already eye-confirmed
        //     for beast HEADS and is hard-gated there; there is no equivalent male gate on the
        //     body channel.
        // That combination IS the 2026-07-15 "males re-scaled + capsules shrunk" regression,
        // returning through a new door.
        //
        // 1 = trust a male's bake and let the geometry writers touch him. Flip this ONLY once a
        // male neutral body has been captured and male UV landmarks exist — otherwise the fitter
        // measures him against a female atlas. Live-polled (not cached), so a flip takes effect
        // within one tuning poll, and it IS snapshot-participating so flipping it on re-dresses
        // everyone rather than waiting for an unrelated knob to arm a generation.
        float maleGeometry     = 0.f;

        // ── HAND-BOX JITTER (2026-08-02 deep dive) ──────────────────────────────────────────
        // handBoxRelAlpha: the mode-2 relation filter in UpdateEffFrames. The box target is
        //   E = T_higgs · EMA(R), R = T_higgs⁻¹ · T_handnode. The two sources are sampled at
        //   DIFFERENT points in the frame: the bone snapshot is fresh this frame, while the HIGGS
        //   anchor body has had its velocity set but has NOT been stepped yet (we run before
        //   stepDeltaTime), so it still holds LAST step's result. R therefore carries a whole
        //   frame of motion, and a 0.05 EMA (~20-frame time constant) cannot track it — the
        //   residual (EMA(R) − R) displaces the box from the hand every frame, scaling with
        //   speed and frame-time variance. That is "shakes, worse outdoors, worse when the
        //   engine struggles". HIGGS has no equivalent: it never measures a relation at runtime
        //   (cachedTransform · a CONSTANT config offset), one source, no filter.
        //   1.0 = unfiltered; algebraically identical to mode 1's target (EMA(R) == R, E == T_hand).
        //   0.05 = pre-2026-08-02 shipped behaviour.
        // SHIPS AT 0.05 (unchanged) — this build alters nothing until you set it. Live knob.
        float handBoxRelAlpha  = 0.05f;
        // handBoxPhaseLog: HBOXPH diagnostic. Logs per hand at ~10 Hz: the real physics step dt
        // (from our own chain hook, which has always received it and discarded it), our clock's
        // dt, their ratio (the keyframe gain), how far the box target sits from the hand bone,
        // how much that moved THIS frame (= the shake, measured), and where the body actually
        // ENDED UP versus where it was commanded (outcome, not command).
        float handBoxPhaseLog  = 0.f;
        // handBoxStepDt (2026-08-02): source invDt from the REAL physics time accumulated across
        // the frame's substeps, not a wall clock. The wall clock was measurably wrong — 764 live
        // samples gave it 3.4x the variance of the true step and a gain of 0.11..1.87 (mean 0.92).
        // ⚠ HISTORY: turning this on ALONE made the hands jitter, because gain then became exactly
        // 1.000 = DEADBEAT tracking, and the box reproduced its target's own frame-to-frame noise
        // (dR 0.007..0.057 u/frame). The old clock's chronic under-shoot had been an ACCIDENTAL
        // low-pass, and it was load-bearing. The damping is now explicit (handBoxTrack), so this
        // can be correct without being twitchy. Both must be set together.
        // ⚠ 2026-08-02 OUTCOME: the hand jitter these knobs were built to chase was NOT PPB.
        // Verified by the user on a NEW GAME WITHOUT PPB INSTALLED — the jitter was still there.
        // All three below therefore default to ORIGINAL (pre-investigation) behaviour and exist
        // only as instruments. Measured for the record: out of combat stance the player's hand
        // BONE moves up to 4.2 u in one frame relative to HIGGS's controller-derived hand body;
        // in combat stance the same walk gives 0.02 u. ~85x on the peak. That oscillation is
        // upstream (VRIK/HIGGS/engine); PPB never writes the player's hand bone
        // (PPBHook.cpp: "if (actor == PlayerCharacter) return; // VRIK owns player arms").
        float handBoxStepDt    = 0.f;

        // handBoxTrack — the fraction of the remaining position error the box closes each frame.
        // 1.0 = deadbeat (follows target noise exactly — the jitter). Lower = smoother.
        // This is the DELIBERATE version of what the broken clock was doing by accident, with the
        // crucial difference that it is CONSTANT: the old gain swung 0.11..1.87 frame to frame and
        // could overshoot (>1); this never exceeds 1, so the box can only ever approach its target.
        // Steady-state lag is bounded and tiny: lag = v * dt * (1-k)/k, i.e. at k=0.88, dt=12 ms
        // and a 1 m/s hand that is ~1.6 mm. Dial DOWN if any twitch remains, UP toward 1 if the
        // hand feels like it trails.
        float handBoxTrack     = 1.f;

        // handBoxWarp (2026-08-02) — the player-space locomotion warp. Teleports all 8 boxes by
        // the player's per-frame position delta BEFORE the keyframe, copied from HIGGS.
        // ⚠ WE DO NOT NEED IT, AND IT IS PHASED WRONG.
        //   * HIGGS needs it because its hand transform is ROOM-space; ours is derived from the
        //     hand bones' world.translate, which ALREADY contains the player's movement — so the
        //     warp applies locomotion a SECOND time.
        //   * HIGGS's own source notes the player's position is not updated until end-of-frame,
        //     which is why HIGGS uses the char-proxy + room nodes instead of GetPosition(). Ours
        //     samples GetPosition() at pre-physics, so the delta is a frame stale.
        // Net: a position error proportional to PLAYER SPEED — zero standing still, growing as
        // you run. User-reported and matched exactly ("jitter when I move, fine when still"),
        // and independent of the slab / clock / relation filter, which is why none of those
        // fixed it. 0 = off (the keyframe closes the full world-space error anyway), 1 = legacy.
        float handBoxWarp      = 1.f;
        // ── PLAYER GENITAL WAND (2026-08-18, doc 20 step 7 v1) ─────────────────────────────
        // Two keyframed segment boxes riding the player's OWN SOS chain (GenBase→mid→tip),
        // HandBox-family: the player is excluded from the per-actor seam (PPBHook.cpp:1282),
        // so the wand lives beside the hand boxes, driven from the same snapshot cadence.
        // TEST INSTRUMENT first: proves existence + push vs clutter / NPC flesh / own hands.
        // playerWandPart 9 by DESIGN: not 2 (PerfSys cross-pelvis Ignore), not 8/14 (its
        // self-thigh Ignore), not 2/3/5/6 (HandBox belt + HIGGS hand parts — and |9-h|>1 so
        // the OWN HANDS still collide), not 29/30 (marker/rig signatures), adjacent to 8
        // (player L thigh) so it never fights the leg it hangs beside.
        float playerWand       = 0.f;    // master enable (hot)
        float playerWandR      = 1.3f;   // lateral half-extent, GAME UNITS (~1.9 cm)
        float playerWandPart   = 9.f;    // filter part bits (sanitized in the accessor)
        float playerWandLog    = 0.f;    // ~1 Hz WAND line (pose, word, node resolution)
        // ── PLAYER HEAD BOX (2026-09-03, user spec) ────────────────────────────────────────
        // ONE keyframed box riding the VRIK-posed third-person "NPC Head [Head]" node. Purpose
        // (user, verbatim): "i don't want my head to be stopped, i want the NPC's body to move
        // when my head contact with it and push the NPC with it, like the hand does. which will
        // make me not get inside their body anymore."
        //   WHY IT CANNOT STOP YOU, and why that is fine: a keyframed body has INFINITE MASS and
        //   its position is 100% commanded — nothing (wall, NPC, your own hand) can ever stop it.
        //   What blocks the player is the bhkCharacterController capsule pair + the roomscale
        //   linearCast on L_GROUND, and layer 56 excludes CharController(30) by construction, so
        //   this box can never talk to either. It instead PUSHES her PLANCK-driven (dynamic)
        //   ragdoll bodies, exactly as the hand boxes do — she yields, which is the same outcome
        //   with no camera lurch (see Report/Follower Bump Guard Module/01 Part B).
        //   ⚠ THE VELOCITY RISK (Ragdoll Research Module 02, mechanism H2): a keyframed body hands
        //   its OWN velocity to a dynamic bone through the contact's velocity constraint. Walking
        //   into her must not become a launch. Two defences, both live: PlayerSpaceWarp teleports
        //   this body by the player's locomotion delta BEFORE the keyframe (so locomotion never
        //   becomes velocity), and headBoxMaxVel clamps what is left — above it the body teleports
        //   and re-keys to ~0 residual, so a fast approach resolves as DEPENETRATION (Havok caps
        //   that near 1 m/s) instead of an impulse. Deliberately far tighter than handBoxMaxVel.
        // Half-extents and offsets are GAME UNITS in the head node's own frame (+Y = forward /
        // anterior, +Z = up the skull, +X = her right) — the same axes every capsule is dialled
        // in. The node origin sits at the NECK JOINT, not the face, so the default offset walks
        // the box forward and up into the skull; dial by eye in the Collision Visualizer.
        // ★ 2026-09-06 (user in VR, v10.2): "the head collision box feels great — make this permanent."
        // Ship defaults = the dialled live values. The offsets are HEADSET-relative (headBoxRider 1):
        // 0/0 = centred on the HMD at eye level, which IS head centre.
        float headBox          = 1.f;    // master enable (hot). SHIP DEFAULT 1 (was 0 until 2026-09-06).
        float headBoxHalfXU    = 5.5f;   // half-width  (~7.9 cm) — lateral   (was 3.5: "smaller than a head")
        float headBoxHalfYU    = 6.0f;   // half-depth  (~8.6 cm) — front/back (was 4.5)
        float headBoxHalfZU    = 7.0f;   // half-height (~10 cm)  — up the skull (was 4.5); ≈ the NPC cranium r 5.52
        float headBoxOffXU     = 0.f;    // centre offset, lateral
        float headBoxOffYU     = 0.f;    // forward from the HEADSET (was 4.0 bone-relative — void under the rider)
        float headBoxOffZU     = 0.f;    // up from the HEADSET: 0 = eye level = head centre (was 5.0 bone-relative)
        float headBoxPart      = 10.f;   // filter part bits (sanitized in the accessor).
                                         // 10 BY DESIGN: |10-9| == 1 so same-group adjacency skips
                                         // the wand for free; every other exclusion is an explicit
                                         // belt in FilterDecision (private group kills adjacency).
        float headBoxMaxVel    = 3.f;    // m/s; above -> teleport + re-key (see the risk note)
        float headBoxLog       = 0.f;    // ~1 Hz HEADBOX line (pose, word, node-vs-HMD distance)
        // ★ VRIK HEAD-HIDE COMPENSATION (2026-09-03, measured). VRIK deliberately shoves the
        // 3rd-person head node BACKWARDS to keep your own face out of the camera — vrik.ini's
        // `hidePlayerHeadDistance` ("This setting pushes the player head backwards to hide it
        // from view"), 12.0 on this rig. The head box rides that node, so it inherited the hide
        // and sat ~9u behind the headset: contact registered late, which is exactly what the
        // user felt. We read VRIK's OWN value at runtime through its interface and add it back
        // along the head's forward axis, so the box lands where the face actually is on ANY
        // user's config instead of a number hardcoded from this machine.
        //   1 = read hidePlayerHeadDistance from VRIK and compensate (default)
        //   0 = ignore it; headBoxOffYU alone positions the box
        // ⚠ headBoxOffYU is now the RESIDUAL dial from the un-hidden head to the NOSE, not the
        // whole journey — it should end up small.
        float headBoxVrikComp  = 1.f;
        // ★★ THE RIDER (2026-09-03, user-diagnosed in VR and this is the real fix).
        //   "when i'm sitting, it's at my eye level, and when i'm standing, it's at my chin
        //    level? So it's not tracking the head correctly when i stand up? Like it need to
        //    track the headset, not the physical bodies right?"
        //   Exactly right, and the reason is structural. VRIK solves a BODY from your headset
        // with the feet on the floor and the character's proportions fixed. Sitting, the body
        // crouches under the HMD and the head bone lands near it. Standing at a real height the
        // rig cannot reach, the bone tops out at the character's height and your headset floats
        // above it — the chin-vs-eye gap the user feels. It is not a tracking bug; the bone is
        // simply not the headset, and NO fixed offset can fix a gap that changes with posture
        // (which is why the 2-point dial kept solving to "5.6u vertical, sign unknown").
        //   0 = ride the VRIK 3P head bone (the old behaviour, kept for A/B)
        //   1 = ride the HEADSET (PlayerCharacter::GetVRNodeData()->UprightHmdNode) — DEFAULT
        // ⚠ UprightHmdNode is YAW-ONLY by construction (that is what "upright" means), so the
        // box does not pitch when you look down. That is CORRECT for a skull-shaped pusher and
        // it also means the box can never tilt into her chest when you glance at the floor.
        // ⚠ On rider 1 the VRIK hide compensation is INERT by construction: the headset was
        // never hidden, so there is nothing to add back. headBoxOff*U return to being small
        // offsets from the headset to the skull centre (a few units BACK is the honest value —
        // the HMD sits on your face, the head's centre is behind it).
        float headBoxRider     = 1.f;
        // ── UNSUPPORTED -> RAGDOLL (2026-09-03, user spec) ─────────────────────────────────
        // "If both feet IK are lifted from the ground together, like we detect she is now not
        // supporting herself on the ground, she should go ragdoll, cause it's obvious that NPC
        // can't support itself. Or the player sweeps the NPC's feet from under them, and they
        // should fall."
        //   Before this, being lifted airborne PREVENTED a knockdown: the engine flips her to
        // animation-driven, planner control is refused, and the push logged "displaced 11.50u
        // but animation-driven — standing down". The user's point is that this is backwards —
        // an actor with nothing under her feet is the CLEAREST case for a ragdoll, not an
        // exception to it.
        //   ⚠ We do NOT read foot IK. The engine already answers this on the character
        // controller: hkpCharacterControl::SupportedState (kUnsupported 0 / kSliding 1 /
        // kSupported 2). Reading the authority that decides it beats inferring it from bones.
        //   GATED ON THE PLAYER BEING THE CAUSE — otherwise every NPC who steps off a rock or
        // walks down stairs ragdolls. Requires a recent player contact on that actor.
        float pushStepFallRag   = 0.f;   // master. SHIPS OFF until VR-verified.
        float pushStepFallFrames = 6.f;  // consecutive unsupported frames before it fires (~0.1s)
        float pushStepFallGraceS = 1.0f; // a player contact this recent = the player is the cause
        // ── RAGFRAME (2026-09-03) — the ragdoll-onset receipt ──────────────────────────────
        // Report/Ragdoll Research Module/05. A push-knockdown throws her instead of collapsing
        // her, and NO log has ever recorded a body velocity, motion type, layer, knock state or
        // drive track at the moment it happens — so the three candidate mechanisms sit at
        // 0.28 / 0.27 / 0.18 and every fix is a guess. This arms a per-frame receipt for ~10
        // frames BEFORE the event (kept in a ring) and `ragFramePost` frames after.
        // ⚠ IT WRITES NOTHING — every field is read. Safe to leave armed; costs a per-frame
        // 18-body read for ONE actor while on, which is why it still ships OFF.
        float ragFrame         = 0.f;    // master. 1 = arm the receipt.
        float ragFramePost     = 20.f;   // frames logged after F0 (clamped 1..120)
        // ── THE ONSET SELECTOR (2026-09-03) — Ragdoll Research 04 stage 1c / 05 section 4 ────
        // How PPB asks for the knockdown. Two of the three are ALSO the candidate fixes for the
        // launch, which is why this is a knob and not a code edit per experiment.
        //   0 = graph "Ragdoll" (BASELINE). The Master wildcard cross-fades into FullyRagdoll
        //       over 0.2 s, and PLANCK has no branch for "fading into ragdoll" — it keeps
        //       driving her for ~13 frames with motors at 500, gravity zeroed and no floor on
        //       layer 8, until its own watchdog fires PushActorAway(0) >= 159 ms later.
        //   1 = ENGINE-FIRST: AIProcess::KnockExplosion(actor, pos, 0) — the engine's knock
        //       state and IsInRagdollState land on the EVENT frame, so PLANCK stamps
        //       getUpMaxForce 0 and returns before zeroing gravity, and its x0.3 ragdolled hit
        //       multiplier applies from frame 0 instead of ~13 frames late. This is what PLANCK
        //       itself calls for every deliberate ragdoll. Predicted milder under H10 AND H3.
        //   2 = graph "RagdollInstant" — no transition effect, so the driven sweep disappears.
        //       Predicted much milder under H10 ONLY, which is what makes it discriminating.
        // Run all three on the same kind of push with ragFrame 1 and compare (07 section 3).
        // ★★ 2.2.0: the default is 1 — the 2026-09-04 verdict (38 RAGFRAME windows: onset 1 removes the launch spike
        // ~30x while keeping the blend) made it THE fix, and the live file has run 1 since. "A code default is a
        // shipping default" — it was still 0 here and on the packager's must-be-off list, which would have ABORTED
        // the 2.2.0 pack.
        float pushStepOnset    = 1.f;
        // ── PRIVATE COLLISION GROUP (2026-08-18) — the grab-through fix ─────────────────────
        // ROOT CAUSE (verified in HIGGS source, physics.cpp:677-683): while HIGGS holds a body it
        // sets CONTACT_IS_DISABLED on every contact between that held body and any body that is
        // (not a HIGGS body) AND (in the PLAYER's collision group). Our boxes inherit the player
        // group from HIGGS's hand word and fail HIGGS's pointer-identity test, so our contacts
        // with a grabbed limb are killed AFTER the filter already voted Collide. HIGGS's own palm
        // is spared only because it IS handBody by pointer — and there is NO public API to be
        // recognized (all 41 IHiggsInterface001 methods checked), so we break the OTHER conjunct:
        // move our bodies out of the player group.
        //   0 = off (legacy: inherit the player group — the bug is present)
        //   1 = always private
        //   2 = private ONLY while HIGGS is holding  ← DEFAULT, confines any mistake to the
        //       window the bug already lives in, and keeps native adjacency the rest of the time
        // ⚠ The player group is not free real estate: it buys the same-group ragdoll ADJACENCY
        // rule (part±1 -> skip), which is how HandBox part 4 skips HIGGS's hands (3/5) and the
        // wand part 9 skips the player L-thigh (8). Under a private group those become plain
        // layer-56 matrix COLLIDES, so FilterDecision carries explicit belts that reproduce every
        // exclusion adjacency was doing for free. Do not raise this knob without those belts.
        float handBoxPrivGroup   = 2.f;
        // The private group id. High and fixed: the engine allocates groups upward from 10 and
        // observed live values are ~0x0009-0x024B, so a clash needs ~65k allocations in one
        // session. Knob-exposed anyway so a clash is fixable without a build (PPB.log prints it).
        float handBoxPrivGroupId = 65520.f;   // 0xFFF0
        // PLANCK-hit fix. 0 = off; 1 = always-on; 2 = AUTO (off at game start, on after the
        // first save/game load) — the user's safe-boot design. Default 2.
        float handBoxNullUserData = 2.f;

        // sceneSuspendHands (2026-08-03) — destroy the player's hand colliders while an
        // OStim/SexLab scene is running. During a scene OStimVR keeps your hands CONTROLLER-
        // TRACKED (TrackHands=1) and the animation puts them inside the partner, so our boxes
        // are live colliders in the middle of it. Nothing else suppresses them: HIGGS gates only
        // held-object/two-handing (hand.cpp:658), and PLANCK's scene behaviour is incidental.
        // NOTE FIRST: OStim Standalone VR ships DisablePLANCKduringScenes in OStimVR.ini and it
        // defaults to 0 — that is the PLANCK half and is likely the bigger lever. This is the PPB
        // half. 0 = ships (unchanged), 1 = suspend.
        //   0 = never suspend (ships)
        //   1 = suspend for the WHOLE scene
        //   2 = suspend ONLY while the scene camera is in THIRD person; the boxes come
        //       back the moment you return to first person, so precise touch is kept
        //       exactly where the hands are controller-tracked and lost only where the
        //       animation is driving them (which is what put them inside the partner).
        float sceneSuspendHands = 0.f;
        // sceneFirstDistU (2026-08-07): mode 2's first/third-person signal. The engine's
        // IsInFirstPerson() NEVER flips in VR (verified in-game: one THIRD-person line at scene
        // start, none after), so mode 2 measures what the mode actually IS: the distance from
        // the CAMERA (HMD) to the scene body's head. First person = the HMD rides the head
        // (a few units); third person = you float away as a ghost (tens to hundreds).
        float sceneFirstDistU  = 40.f;
        // apiWeaponDrawnOnly (2026-08-08): the weapon probe only reports while the player's
        // weapon is actually DRAWN. Off = pre-fix behaviour (a VRIK-sheathed weapon kept
        // registering phantom contacts and pushing wigs from the hip). 1 = ships.
        float apiWeaponDrawnOnly = 1.f;
        // weaponSheathedColOff (2026-08-08): while the weapon is sheathed, actively disable
        // HIGGS's weapon COLLISION via its public API (and restore on draw). Measured: the
        // collision body rides the controller ~13u from the palm even when sheathed, and
        // nothing else ever turns it off -- it physically shoves hair from an empty hand.
        float weaponSheathedColOff = 1.f;

        // pivGuardCombatLoose (2026-08-03) — let PLANCK's pivot loosen apply to PPB-skeleton
        // actors WHILE IN COMBAT. Coherent because (18_PLANCK_Internals_Reference) the loosen
        // exists so the ragdoll CAN MATCH THE ANIM POSE. PivGuard runs our actors at loosen-0 so
        // our baked joints are never collapsed — right for ordinary movement, but it forbids the
        // ragdoll from reaching poses our joints cannot express, and extreme combat animations are
        // exactly that. A body that cannot follow its animation reads as STRETCHED.
        // COST: during combat our joints collapse to the anim pose, so capsule fit and touch
        // accuracy degrade exactly then. 0 = ships (unchanged), 1 = loose while in combat.
        float pivGuardCombatLoose = 0.f;
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
        float ghostViz      = 0.f;    // visual markers on the ghost zones (dial aid). 2.2.0: default 0 — a dial
                                      // aid must never float in a player's world (user: "don't want to spam the player")
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
        float touchProbe   = 0.f;     // TOUCHPROBE mapping logs (nearest capsule per fingertip).
                                      // SHIPS OFF: it is a capsule-dialling aid that logs every 45
                                      // frames, so a user with no tuning file would get log spam.
        float touchProbeRangeU = 3.f; // only log when closer than this
        // ══ UV ReShape (2026-07-23) — THE shape system. Replaces the girth/slider generation ══
        // Master switch. The old bodyScale/meshShape pair is GONE: one measurement source (UV
        // landmarks), one response path, no fallback that can silently take over.
        float lmReShape = 1.f;
        // RING CHANNELS — chest/belly/waist. Each is a pure radial ratio of the NPC's landmark
        // distance-from-its-node against Lydia's. Radius and radial position scale together;
        // Z is never touched (height belongs to ReScale). Gain 1.0 = capsule follows the flesh
        // exactly; these exist only so a channel can be trimmed by eye without a rebuild.
        // ★ MALE region gains (2026-08-22, the Carmella near-miss): lmGain* is GLOBAL and
        // dialling the male through it regresses every female. The male response gets its
        // own family, sex-routed in UvRegionRatio. Waist/belly defaults come from the first
        // male OBody dial (Imperial FF00172C, big preset): user "spine0 20% less, spine1 10%
        // less" at measured flesh ratios 1.352/1.218 -> gain 0.23/0.44.
        // ★ 2026-08-22 FINAL MALE DIAL (Imperial FF0028D8, scale 1.0000, two-preset session):
        // waist/belly/thigh converged at ~0.78 dialled independently — one systematic projection
        // factor (front-point growth overstates ring growth ~25%). Butt/arm never eye-called;
        // shrink direction unverified (male presets are scarce) — doc 23 has the open list.
        float lmGainMChest = 1.f;
        float lmGainMBelly = 0.80f;
        float lmGainMWaist = 0.76f;
        float lmGainMButt  = 1.f;
        float lmGainMThigh = 0.79f;
        float lmGainMArm   = 1.f;
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
                                         // ★ 2026-08-23 TRUE-SILENCE MODES — 1-4 only pick a QUIETER
                                         // ENTRY in another material's impact set (grass = a rustle),
                                         // and only when the other side is skin; two PPB capsules
                                         // (NPC-vs-NPC) is both-null and untested. 5 and 6 remove the
                                         // sound instead of softening it:
                                         //   5 = unknown material hash — in no impact set, so neither
                                         //       direction resolves. No null pointers; safer.
                                         //   6 = null the shape->wrapper pointer — the engine cannot
                                         //       reach a material at all. Absolute, but assumes the
                                         //       engine null-checks that read; a CTD on first contact
                                         //       IS the verdict, fall back to 5.
                                         // A/B these in VR; the winner becomes the shipped default.
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
                                         // proof rustle, DEFAULT), 4 none, 5 unknown-hash / 6 null-userData
                                         // (the 2026-08-23 true-silence pair — see npcBodyMat above).
                                         // Applied at rig CREATE — change the knob then flip
                                         // npcFollower 0 -> 1 to rebuild rigs with the new sound.
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
        // npcFingerLog: 1 Hz NFING TRACK/REACT telemetry to PPB.log — ONE LINE PER CHORD.
        // SHIPS OFF (2026-07-29→30): the cost scales with chord count, which was fine when a
        // rig meant 4-7 tail chords but is not now that generated HAIR tables reach 85 chords
        // (kMaxChords is 200). Measured on a real session: 6692 TRACK lines in 117 s =
        // 57 lines/sec, a 954 KB log in 9 minutes, all formatted by spdlog on the main thread.
        // Flip to 1 for a dialing/telemetry session, then back. Same lesson as touchProbe: a
        // diagnostic's code default IS a shipping default.
        float npcFingerLog     = 0.f;
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
        float handBoxRebuildFrac = 0.50f; // hand-scale drift that forces a rig REBUILD. **0 = NEVER
                                        // rebuild on scale** (see HandBoxRebuildFrac()).
                                        // 2026-07-29, raised 0.15 -> 0.50 from measured logs: the
                                        // author's own session drifted 14.26% from the built-at
                                        // scale (range 0.8264..0.9639) — 0.15 left only 0.74pp of
                                        // margin. A third user's log peaked at 8.57%. The rebuild
                                        // is VESTIGIAL anyway: geometry is re-solved live every
                                        // snapshot, and beast form is handled by a separate path
                                        // (handBoxBeast -> DestroyHand("off")), so nothing actually
                                        // needs this. It survives only as a safety net.
        float handBoxLeashU    = 30.f;   // max units a hand box (or its HIGGS anchor) may sit from
                                        // the skeleton hand node before it is snapped home; 0 = off
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
        // ══ ORIFICE DRIVE (2026-08-19, Orifice.cpp) ═══════════════════════════════════════════
        // Native orifice opening on ANY contact (finger / held object / weapon / wand), outside
        // scenes. All read LIVE per frame -> PK_NOSNAP (a dial must never re-dress capsules).
        // The MASTER ships 0: nothing in this block can move a bone until the user opts in.
        float orificeEnable    = 0.f;   // ★ master. 0 = every ring restored + the module inert
        float orificeVaginal   = 1.f;   // per-ring, under the master (female skeletons only —
        float orificeAnal      = 1.f;   //   the male sculpt has the Anus2 bones and no vaginal
        float orificeOral      = 1.f;   //   ones, so that is handled by EXISTENCE, not a gate)
        // The ellipse test's slack. A probe is INSIDE when distL + distR <= sep * (1 + tol),
        // i.e. inside an ellipsoid whose foci are the two sensor AXES (clamped segments, so the
        // sum grows off the ENDS exactly as it grows off-axis — the slack is isotropic).
        // SOLVE IT, do not guess it. Measured level-0 pair capComC22/C23: X = +/-2.0 -> sep 4.0,
        // segments span Y +1.0 -> -2.0, radius 0.3. A point offset d from the midline sums
        // 2*sqrt((sep/2)^2 + d^2), so it passes while d <= (sep/2)*sqrt((1+tol)^2 - 1):
        //     tol 0.35 -> d <= 1.81u   ← what shipped first, and it is WRONG: 1.8u of slack in
        //                               EVERY direction lets a fingertip resting on the labia,
        //                               well outside her, drive the ring open. That is precisely
        //                               the outside graze the gate exists to reject.
        //     tol 0.10 -> d <= 0.92u   ← shipped. Under a unit of slack around a 4.0 x 3.0u
        //                               cavity: shallow entries still pass, an outside graze
        //                               does not.
        // Raise it if shallow entries are missed; lower it if an outside graze ever reads as
        // penetration. Note it is scale-free — sep is measured live, so ReScale/ReShape/body
        // morphs move the numbers together.
        float orificeGateTol   = 0.10f;
        // Per-frame easing rate of the gape, 1/s. The SENSE only refreshes at apiHz (~4 Hz by
        // design); this is what stops that reading as steps. ~6 = a 1/6 s time constant.
        float orificeEaseHz    = 6.f;
        // How much the INTRUDER RADIUS contributes against pure depth. 0 = depth only (exactly
        // what PPA does — it has no radius); 1 = radius only; 0.5 = even blend, which is the
        // point of the feature: a fingertip and a 3u plug at the same depth must differ.
        float orificeRadiusGain = 0.5f;
        float orificeRefRadU   = 3.0f;  // the radius that counts as "fully stretched" (game units)
        float orificeFingerRadU = 0.8f; // hand boxes carry no pad — this is the fingertip radius
        // Gape range per ring, game units, from PPA's own toml (Scale -> ScaleMax) so both mods
        // deform to the same amounts. openness = min + (max-min) * f(depth, radius).
        float orificeVagMinU   = 1.5f,  orificeVagMaxU  = 3.0f;
        float orificeAnalMinU  = 2.0f,  orificeAnalMaxU = 4.5f;
        // Probe cull before the 10 live capsule reads, measured from her PELVIS (the COM body),
        // NOT from actor->GetPosition() — the ref origin is at the FEET, ~67-69u below the sensor
        // ladder, so an origin-anchored radius under her HEIGHT rejects a probe that is literally
        // inside her (that is exactly how the first build shipped, and it could never open).
        float orificeRangeU    = 60.f;
        // Foreign-write arbitration: how long the ring must sit COMPLETELY STILL before we take
        // it back from another writer (PPA during a scene). Short enough to feel immediate after
        // a scene, long enough that a mid-ramp pause cannot be mistaken for release.
        float orificeForeignHoldS = 1.5f;
        float orificeLog       = 0.f;   // level-change edges + a 1 Hz open line
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
        // 2026-07-29: spine1 10->12 (C9/C10 back), spine2 16->20 (C17/C18 shoulder blade),
        // comC 20->32 (C21..C31 = the 11 pelvis sensors). All four skeletons carry the same
        // counts so one knob index means one anatomy everywhere.
        // 2026-08-22 MALE SCULPT headroom (sculpt.nif only — per-race-by-existence keeps every
        // female skeleton at its current child count, so these extra indices are unreachable there):
        //   spine0 12 (7 -> 13 children: 5 buried spares + the WaistLine landmark probe at C12)
        //   spine1 13 (8 -> 14 children: 5 buried spares + the Belly probe at C13)
        //   spine2 24 (19 -> 25 children: 5 buried spares + the Chest probe at C24)
        //   headC  34 (25 -> 35 children: 10 buried spares, no probe)
        //   comC   37 (27 -> 38 children: 10 spares + the ButtCheekR probe at C37)
        // ⚠⚠ THE FOUR TORSO PROBES ABOVE (spine0 C12, spine1 C13, spine2 C24, com C37) ARE
        //    REFERENCE RULERS — **STRIP BEFORE SHIP**, knobs AND the sculpt.nif tail children.
        //    They are array-backed so they carry no per-index field to mark; the authoritative
        //    strip list (all 6 knobs + all 8 nif children) lives in
        //    Report/Precision Physic Bodies Module/23_Male_Body_Build_And_Sculpt.md
        //    §"LANDMARK PROBES". Enable 0 does NOT neutralise them — the geometry is baked in
        //    sculpt.nif, so a knob-only strip still ships live capsules proud of the skin.
        // ⚠ FIVE coupled edits per slot (04_Pitfall_Ledger): this size, kArrays' count in
        // Tuning.cpp, CapFixChildKnobs(slot), CapFixChildSlot's ChildPtr bound, and
        // CapFix.cpp's kMaxListChildren.
        CapChild spine0C[12]{}, spine1C[13]{}, spine2C[24]{}, headC[34]{}, comC[37]{};
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
    int      RayTelMode();              // rayTel knob: 0 off / 1 counters / 2 counters + __rdtsc timing
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
    // Reads ONE key straight off PPB_tuning.txt, for code that runs BEFORE the pre-drive
    // hook (installed at kDataLoaded) has ever polled the file — e.g. FsmpLink's SMP
    // handshake at kPostPostLoad. Mutates no state; does not disturb the poller's mtime.
    float    EarlyReadKnob(const char* key, float fallback);
    bool     NpcFollowerEnabled();
    bool     NpcGenCapEnabled();   // tbl-7 male genital rig master switch      // master switch for the always-on garment rigs (> 0.5)
    bool     NpcGenCapFemaleEnabled();   // ★ futa: extend the tbl-7 GEN rig to females (ships OFF)
    float    HiggsPokeFix();            // 1 = force HIGGS's finger-close anim off so poking works
    bool     ApiTouchEnabled();         // touch-API master (apiTouch)
    float    ApiHz();                   // touch-API tick rate (clamped 1..90)
    float    ApiTouchU();               // contact distance
    float    ApiExitPadU();             // contact-exit hysteresis pad
    int      ApiMaxActors();            // actors scanned per tick (min 1)
    float    ApiRangeU();               // roster range
    float    ApiFistTipPalmU();         // fist-detection tip-to-palm distance
    bool     ApiEventsEnabled();        // mod-event emission
    bool     ApiLogEnabled();           // debug START/END logging
    // Contact logging can be turned on from the USER-facing ini (PPB_Skeletons_Added_Race.ini,
    // key `contactLog = 1`) as well as from the dev tuning file's apiLog. Either enables it, so
    // a user chasing a bug never has to find the tuning file.
    void     SetContactLogIni(bool on);
    float    ApiDwellS();               // dwell filter: default class
    float    ApiDwellHeadS();           // dwell filter: head slot
    float    ApiDwellComS();            // dwell filter: pelvis slot (non-sensor)
    float    ApiDwellSensorS();         // dwell filter: interior sensors C21-31
    float    ApiDwellTailS();           // dwell filter: tail pseudo-slot
    bool     ApiRawEventsEnabled();     // verbose PPB_TouchRaw* stream (ships off)
    float    ApiWeaponRMaxU();          // blade-radius cap (6u; 0 = uncapped)
    float    ApiObjectRMaxU();         // held-object segment radius cap (4u; 0 = uncapped)
    bool     ApiObjectBoxOn();         // use the held object's REAL collision box for contact
    bool     ApiPalmProbeOn();         // HIGGS's own hand box as touch probe 5 (2026-09-06)
    float    ObjectBoxMaxU();          // refuse a box half-extent bigger than this (garbage guard)
    bool     ApiSubRegionInEvent();     // append the sub-region as a 5th packed field (off)
    bool     ApiSuppressHeldHand();
    bool     ApiSuppressHeldHandStrict();   // knob=2: full mute, no index exception     // mute a hand that is holding something
    float    ApiBreastPadU();       // capsule-side breast touch pad, base (game units)
    float    ApiBreastPadSlope();   // ...plus this per unit of cup above the saturation clamp
    bool     ApiHairTarget();           // hair chords as touch targets (ships off)
    float    NpcRigRangeU();            // garment-rig create/keep range in game units (0 = unlimited)
    float    NpcRigRangeHystU();        // extra slack before a range destroy (anti-thrash)
    int      NpcRigMaxActors();         // how many ACTORS may hold garment rigs (0 = unlimited)
    float    NpcFingerTipU();
    float    NpcFingerMassKg();         // floored at 0.01 kg (a 0-mass dynamic body is a solver hazard)
    float    NpcGarmentMassKg();        // light garment-capsule mass, floored at 0.01 kg
    float    NpcTailMassKg();           // meaty tail-capsule mass (tbl 1/2), floored at 0.01 kg
    float    NpcGenMassKg();            // GEN (tbl 7) capsule mass
    float    NpcGenR();                 // GEN capsule radius
    float    NpcGenAlpha();             // GEN tracking stiffness (clamped like NpcFingerAlpha)
    bool     GenBendEnabled();
    bool     GenBendPlayerEnabled();
    int      GenBendMax();
    float    GenBendUpMs();
    float    GenBendHoldSec();
    float    GenBendDecaySec();
    float    GenBendArousal();
    float    GenBendTouchU();
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
    float    LmRegionGainM(int region);   // MALE family (sex-routed; the Carmella lesson)
    float    LmNeutLimbChord(int region); // leg/arm neutral chord (0 = not a limb region)
    // MALE neutrals (2026-08-21, lmNeutM*): same index/region conventions as the female pair
    // above. Breast indices (0..2) intentionally return 0 — no male breast channel exists.
    void     LmNeutralPosM(int idx, float out[3]);
    float    LmNeutLimbChordM(int region);
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
    bool     DgHeadPlanckOn();    // leave head clones under PLANCK (dgHeadPlanck)
    bool     DgVictimPlanckOn();  // leave dismembered VICTIMS under PLANCK (dgVictimPlanck)
    bool     GenProbeOn();        // genital dress-signal research probe (genProbe)
    bool     GenProbeFast();     // genProbe 2 = ~10 Hz poll, for measuring TNG's equip latency
    bool     MaleGeometryOn();   // let the geometry writers touch a male carrying a PPB bake (maleGeometry)
    float    HandBoxRelAlpha();   // mode-2 relation filter; 1 = unfiltered (jitter fix)
    bool     HandBoxPhaseLogOn(); // HBOXPH jitter diagnostic (handBoxPhaseLog)
    bool     HandBoxStepDtOn();   // invDt from real physics dt (handBoxStepDt)
    float    HandBoxTrack();      // per-frame error-closure fraction (handBoxTrack)
    bool     HandBoxWarpOn();     // player-space locomotion warp (handBoxWarp)
    bool     PlayerWandOn();      // player genital wand master enable
    float    PlayerWandR();       // wand lateral half-extent (game units, floored 0.3)
    unsigned PlayerWandPart();    // sanitized: forbidden parts remap to 9
    bool     PlayerWandLogOn();
    bool     HeadBoxOn();         // player head box master enable
    float    HeadBoxHalfXU();     // half-extents, GAME UNITS, head-node frame (floored 0.3)
    float    HeadBoxHalfYU();
    float    HeadBoxHalfZU();
    float    HeadBoxOffXU();      // centre offset in the head-node frame (game units, +-40)
    float    HeadBoxOffYU();
    float    HeadBoxOffZU();
    unsigned HeadBoxPart();       // sanitized: forbidden parts remap to 10
    float    HeadBoxMaxVel();     // m/s, floored 0.25 — the anti-launch clamp
    bool     HeadBoxLogOn();
    bool     HeadBoxVrikCompOn();  // add VRIK's hidePlayerHeadDistance back onto the box
    bool     HeadBoxRideHmd();     // true = ride the headset, false = ride the VRIK head bone
    bool     PushStepFallRagOn();  // unsupported (feet off the ground) -> ragdoll
    float    PushStepFallFrames();
    float    PushStepFallGraceS();
    bool     RagFrameOn();        // the ragdoll-onset receipt (read-only)
    float    RagFramePost();      // frames logged after F0 (clamped 1..120)
    float    PushStepOnset();     // 0 graph Ragdoll / 1 engine-first / 2 RagdollInstant
    int      HandBoxPrivGroupMode();   // 0 off / 1 always / 2 only-while-holding
    unsigned HandBoxPrivGroupId();     // sanitized into 16 bits, never 0
    // maleGeometry is TRI-STATE (semantics CORRECTED 2026-08-18):
    //   0 = refuse all geometry writers on males
    //   1 = FULL — capsule knobs + ReScale + ReShape
    //   2 = SCULPT (default) — capsule knobs + **ReScale ON**, only ReShape suppressed.
    // ⚠ ReScale is NOT female-calibrated: it reads the actor's own XP32 nodes/Havok pivots and
    // kBaseArc 48.045 measures identically on the male skeleton. Disabling it for males (the
    // original mode-2 behaviour) is what left baked males' joints off their XP32 nodes. ONLY
    // ReShape is female-specific (kUvLandmarks = the CBBE female UV atlas).
    int      MaleGeometryMode();
    // handBoxNullUserData (2026-08-18) — the PLANCK-hit / IWP-stab fix. Nulls the hkpRigidBody
    // userData back-pointer on our player boxes + wand so PLANCK early-returns before turning a
    // finger touch on a HELD NPC into a real weapon hit (which IWP then dramatises into a stab +
    // ragdoll). SELF-VERIFYING: only writes when the field still holds OUR OWN wrapper pointer, so
    // a wrong offset can never corrupt anything. Default 0 (OFF at game start, per the user) —
    // hot, so set 1 live once in-game and it persists across reloads.
    int      HandBoxNullUserDataMode();   // 0 off / 1 always-on / 2 auto (off at boot, on after a load)
    int      SceneSuspendHandsMode(); // 0 off / 1 whole scene / 2 third-person only
    float    SceneFirstDistU();       // mode-2 HMD-to-head threshold (game units)
    bool     ApiWeaponDrawnOnly();    // weapon probe only while drawn (apiWeaponDrawnOnly)
    bool     WeaponSheathedColOff();  // disable HIGGS weapon collision while sheathed
    bool     PivGuardCombatLooseOn(); // allow PLANCK loosen during combat
    float    HandStopMode();      // HandStop stage knob (handStop)
    bool     PushStepEnabled();   // PushStep master (pushStep)
    float    PushStepRepeatS();   // bump cadence while pressed (pushStepRepeatS)
    float    PushStepStopS();     // zero-pressure time that ends the episode (pushStepStopS)
    float    PushStepWalkU();     // walk-distance CAP (pushStepWalkU × region multiplier)
    float    PushStepSpeedU();    // PushWalk planner speed (pushStepSpeedU)
    float    PushStepDispU();     // v4 engage threshold (pushStepDispU)
    float    PushStepDispGain();  // v4 displacement→distance gain (pushStepDispGain)
    float    PushStepSensor();    // 5 = intent-gap sensor, 4 = legacy (pushStepSensor)
    float    PushStepSettleS();   // v6 (pushStepSettleS)
    float    PushStepRefracS();   // v6 (pushStepRefracS)
    float    PushStepSpeedRefU(); // v6 (pushStepSpeedRefU)
    float    PushStepSpeedMinU(); // v6 (pushStepSpeedMinU)
    float    PushStepSpeedMaxU(); // v6 (pushStepSpeedMaxU)
    float    PushStepRampInS();   // v6 (pushStepRampInS)
    float    PushStepRampOutS();  // v6 (pushStepRampOutS)
    float    PushStepExtendMul(); // v6 (pushStepExtendMul)
    float    PushStepStopU();     // v7 (pushStepStopU)
    float    PushStepMulChest();  float PushStepMulBelly();  float PushStepMulWaist();
    float    PushStepMulCom();    float PushStepMulThigh();
    float    PushStepRampFloor(); float PushStepResumeS();   float PushStepExitSpeedU();
    float    PushStepNearU();     float PushStepDirOffDeg();
    float    PushStepAccel();     float PushStepDecel();     float PushStepRotPct();
    float    PushStepAngAccel();  float PushStepWalkRun();  float PushStepVelGainS();  float PushStepLeverF();  float PushStepChain();  float PushStepHemiDot();  float PushStepLeverGateU();  float PushStepBreastMul();  float PushStepRampMinFrac();  float PushStepRateVetoU();  float PushStepSideGain();  float PushStepMulHead();  float PushStepMulNeck();  float PushStepHeadTrig();  float PushStepPressU();  float PushStepIdleProbe();  float PushStepPeakWindowS();  float PushStepBaseAlpha();  float ObjectPadMaxU();  float PushStepDispSlowU();  float PushStepDispFastU();  float PushStepRateFastU();  float PushStepFacePush();  float PushStepReaction();  float PushStepStaggerU();  float PushStepStaggerRate();  float PushStepRagdollU();  float PushStepReactCoolS();  float PushStepRagSettleS();  float PushStepRagMaxVelU();  float PushStepEscalate();  float PushStepLatTrig();  float PushStepRateWinS();  float PushStepSpeedTrack();  float PushStepSpeedUpU();  float PushStepSpeedDownU();  float PushStepReactGraceS();  float PushStepStaggerFace();  float PushStepStaggerFaceDeg();  float PushStepStaggerMagMin();  float PushStepSpeedExp();  float PushStepEngageWinRate();  float PushStepStaggerDirFlip();  bool PushStepLiftRagOn();  bool PushStepLiftRagFires();  float PushStepLiftFeetU();  float PushStepLiftSettleS();  float PushStepStaggerFaceSrc();  float PushStepStaggerTurnMode();  float PushStepBarU();  float PushStepBarHeadU();  float PushStepObjectPush();  float PushStepObjectBarMul();  float PushStepLiftRestU();  float PushStepLiftLogHz();  float PushStepWalkFaceSplit();  float PushSenseTriad();  float PushSenseZ();  float PushSenseBaseFreezeU();  float PushSenseBaseHoldS();  float PushSenseStandDownS();  float PushStepTierChain();  float PushStepTierHoldS();  float PushStepHandTravelU();  float PushStepHandTravelSideU();  float PushStepBarMidU();  float PushStepStaggerMidU();  float PushStepRagdollMidU();  float PushStepBarHighU();  float PushStepStaggerHighU();  float PushStepRagdollHighU();  float PushStepKnockSrcU();  bool MouthProbeOn();  float MouthProbeOffXU();  float MouthProbeOffZU();  float MouthProbeOutU();  float MouthProbeHalfWU();  float MouthProbeR();  int MouthProbeSource();  float MouthProbeLeverU();  bool MouthKissLipsOn();  float MouthKissLipU();  float MouthKissExitU();  bool MouthProbeLogOn();  bool FurnProbeOn();  bool OutfitGuardOn();  float PushStepPeakWindowS();
    // v11.1 (2026-09-07): engine-contact hand-travel anchor + the lift gait gate (PushStep.cpp)
    float    PushStepAnchorSrc();  float PushStepAnchorTrunkOnly();  float PushStepLiftGait();
    // v12 travel sensor
    float    PushStepTravelMode();  float PushStepTravelWalkU();  float PushStepTravelStumbleU();
    float    PushStepLever();  float PushStepCrossRefU();  float PushStepCrossExp();  float PushStepCrossMinFrac();  float PushStepCrossFloorU();
    float    PushStepOffCom();  float PushStepOffSpine0();  float PushStepOffSpine1();  float PushStepOffSpine2();  float PushStepOffNeck();  float PushStepOffHead();
    float    PushStepWalkReCapS();  float PushStepOffWalkAdd();
    // v29c per-node ABSOLUTE stumble / ragdoll bars (0 = the shared rung + PushStepOff<node>)
    float    PushStepBarStumbleCom();  float PushStepBarStumbleSpine0();  float PushStepBarStumbleSpine1();
    float    PushStepBarStumbleSpine2();  float PushStepBarStumbleNeck();  float PushStepBarStumbleHead();
    float    PushStepBarRagCom();  float PushStepBarRagSpine0();  float PushStepBarRagSpine1();
    float    PushStepBarRagSpine2();  float PushStepBarRagNeck();  float PushStepBarRagHead();
    float    PushStepEquipSettleS();   // v29d
    float    PushStepDirTrack();  float PushStepDirTrackDeg();  float PushStepDirTrackMinU();
    float    PushStepNoDistCap();
    float    PushStepRagEscalate();  float PushStepRagEscalateMinS();
    float    PushStepWalkOffCom();  float PushStepWalkOffSpine0();  float PushStepWalkOffSpine1();
    float    PushStepWalkOffSpine2();  float PushStepWalkOffNeck();  float PushStepWalkOffHead();
    float    PushStepStopFrac();  float PushStepStopMinU();  float PushStepStopMaxA();
    float    PushStepLiftSymU();  float PushStepLiftRiseU();  float PushStepLiftRestSpreadU();
    bool     PushStepLiftZoneRule();  float PushStepLiftThighSplit();  bool PushStepLiftRiseFromContact();   // v28
    bool     PushStepLiftEngineAttr();  float PushStepLiftThighMask();  float PushStepLiftOneLegU();  bool PushStepLiftWalkVerdict();   // v29
    float    PushStepLiftWalkFrames();  float PushStepLiftGroundU();  float PushStepLiftGroundS();  bool PushStepLiftSitGuard();  bool PushStepFurnSitState();  bool PushStepFurnLean();  bool PushStepFurnLeanPush();  float PushStepLiftComU();  float PushStepLiftGroundMaxS();   // v29 (+ v29b pelvis bar, v29e floor deadline)
    float    PushStepCombatGate();  float PushStepKillMoveGate();
    float    PushStepTravelRagdollU();  float PushStepOriginHoldS();  float PushStepSoftExclude();  float PushStepSelfMoveGate();  float PushStepVoteN();
    bool     PushStepWeapon();    // weapon contacts count (pushStepWeapon)
    bool     BraceEnabled();      // Brace master (brace)
    float    BraceNearU();        // arm when a trunk contact's distU <= this (braceNearU)
    float    BraceHoldS();        // release hysteresis seconds (braceHoldS)
    bool     ReDriveEnabled();    // ReDrive master (reDrive)
    float    ReDriveFor(const char* nodeName);   // per-bone multiplier by XP32 node name
    bool     DivProbeEnabled();   // divergence probe master (divProbe)
    float    DivProbeEveryN();
    float    DivProbeGapU();
    float    DivProbeAlpha();
    float    DivProbeHoldN();
    float    DivProbePlayerU();
    float    DivProbeReportS();
    float    SceneModeSel();      // 1 = collision-off, 2 = keyframed (sceneMode)
    bool     BumperSceneOff();    // scene bumper gate master (bumperSceneOff)
    float    BumperSceneExcite();// excitement rank threshold (bumperSceneExcite)
    bool     PlanckLoosenOursOn();// PivGuard v2 master (planckLoosenOurs)
    float    PlanckLoosenGlobal();      // -1 leave alone / 0 force off / 1 force on
    float    PlanckGainHier();       // -1 leave alone / >=0 force (Havok default 0.17, PLANCK 0.6)
    float    PlanckGainVel();        // -1 leave alone / >=0 force (PLANCK 0.6)
    float    PlanckGainPos();        // -1 leave alone / >=0 force (PLANCK 0.05)
    float    ArmProbe();
    float    LegProbe();
    float    DriveDamping();
    float    ForceKeyframe();
    bool     TouchProbeHudOn();  // mapping HUD (touchProbeHud)
    float    TouchProbeHudU();   // HUD touch distance (touchProbeHudU)
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
    // ── ORIFICE DRIVE accessors (Orifice.cpp; read LIVE per frame). Every one clamps before
    //    it returns — a NaN or a negative in the tuning file must never reach the bone write.
    bool     OrificeEnabled();          // master
    bool     OrificeVaginal();
    bool     OrificeAnal();
    bool     OrificeOral();
    float    OrificeGateTol();          // ellipse-test slack, clamped 0..2
    float    OrificeEaseHz();           // per-frame ease rate, clamped 0.1..60
    float    OrificeRadiusGain();       // depth-vs-radius blend, clamped 0..1
    float    OrificeRefRadU();          // "fully stretched" intruder radius, clamped 0.1..20
    float    OrificeFingerRadU();       // fingertip radius for pad-less hand boxes, clamped 0..10
    float    OrificeOpenMinU(int kind); // 0 = vaginal, 1 = anal — the ring's Scale
    float    OrificeOpenMaxU(int kind); // ...and its ScaleMax (never below Min)
    float    OrificeRangeU();           // probe->actor cull, clamped 1..400
    float    OrificeForeignHoldS();     // stillness before ownership is reclaimed, clamped 0.1..30
    bool     OrificeLogEnabled();
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
    float    HandBoxLeashU();
    float    HandBoxRebuildFrac();
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
