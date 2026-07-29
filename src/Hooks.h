#pragma once

namespace Hooks {
    // Pre-driveToPose chain-over hook at VR 0xB266AB. This is the CALL to
    // hkbRagdollDriver::driveToPose inside BShkbAnimationGraph::UpdateAnimation
    // — the seam between the animation pose and the ragdoll driver. PLANCK
    // owns this site; we chain ON TOP by installing via kDataLoaded (after
    // PLANCK's SKSEPlugin_Load has patched it), so our hook fires first and
    // then tail-calls through to PLANCK's saved hook. See Hooks.cpp for the
    // full rationale. AIHands chains the same site the same way — multiple
    // kDataLoaded chainers stack: each newcomer becomes the new head and
    // tail-calls the previous head, so PPB + AIHands + PLANCK coexist.
    void InstallPreDriveHook();

    // READ-ONLY: log + decode the bytes at the post-physics seam candidate (0xB268DC) so we can
    // verify it's a 5-byte CALL before installing a write_call there. No patch.
    void VerifyPostPhysicsAddresses();

    // INNER-DRIVE chain hook at 0xA26C05 — the CALL to hkaRagdollRigidBodyController::driveToPose
    // INSIDE hkbRagdollDriver::driveToPose (PLANCK hooks it too; we chain on top at kDataLoaded).
    // The heel fix biases the worldFromModel PARAMETER here (the last stop before bone targets) —
    // mutating the track/character copies at the pre-drive seam was proven a no-op. Self-aborts if
    // the site isn't a 5-byte CALL.
    void InstallInnerDriveHook();

    // POST-PHYSICS DRIVER chain hook at 0xB268DC — the CALL to hkbRagdollDriver::postPhysics. The
    // heel fix's compensation half: after the (raised) ragdoll is mapped back into the output pose,
    // subtract heelZ from the root bone so the rendered body stays planted. Self-aborts if not a CALL.
    void InstallPostPhysicsDriverHook();

    // PHYSICS-STEP timing chain hook at 0xDFB722 — the 5-byte CALL to hkpWorld::stepDeltaTime inside
    // bhkWorld::Update (the ONLY seam that contains the Havok step). HIGGS Write5Calls the same site;
    // PLANCK does not touch it — so a kDataLoaded write_call<5> lands ON TOP of HIGGS's hook and forwards
    // its hkpStepResult return. Diagnostic-only (feeds Diag::OnPhysicsStep while `perf` is armed).
    // Self-aborts via the p[0]!=0xE8 runtime guard: .text is DRM-encrypted so this is the ONLY proof.
    void InstallPhysicsStepHook();
}
