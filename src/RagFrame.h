#pragma once
#include <cstdint>

// ============================================================================
//  RagFrame — the RAGDOLL ONSET RECEIPT (2026-09-03).
//
//  WHY THIS EXISTS. A PPB push-knockdown throws the NPC instead of collapsing
//  her, and nothing in any log has ever recorded a body VELOCITY, a motion
//  type, a layer, a knock state or a drive track at the moment it happens. The
//  three candidate mechanisms (Report/Ragdoll Research Module/02) are ranked
//  0.28 / 0.27 / 0.18 — i.e. the model cannot pick a winner, and every fix is a
//  guess until one instrumented knockdown exists. This module IS that
//  measurement, and its whole design goal is that ONE knockdown names the
//  mechanism.
//
//  ⚠ IT WRITES NOTHING. Every field is read; no body, track, constraint, knob
//  or engine state is modified anywhere in this file. It is safe to leave armed.
//
//  HOW TO READ THE OUTPUT — Report/Ragdoll Research Module/05 §3 is the table.
//  In short, at F0 (the frame PPB asks for the ragdoll):
//    · rbOn=1 with rbFrac DECAYING over ~13 frames, pwMaxForce=500 on those
//      frames, velocities BUILDING rather than spiking, planckBug flipping 1
//      exactly once at the first rbOn=0 frame          -> H10, the blend-window drive
//    · a ONE-FRAME |v| step of hundreds-to-thousands of u/s on a single bone,
//      neighbours at ~35/25/15% of it, violent spin    -> H3, PLANCK's full-strength hit
//    · bone |v| TRACKING the hand/weapon body's own |v| frame by frame, present
//      BEFORE F0, deepest-contact bone leading         -> H2, contact velocity transfer
//
//  planckBug is PLANCK's OWN watchdog predicate (isPoweredOnly && allNoForce &&
//  usingRootBone && !IsInRagdollState), evaluated one call AHEAD of PLANCK on
//  the same generator output — the only way to time its PushActorAway(0), since
//  PLANCK's own log line carries no timestamp.
// ============================================================================
namespace RagFrame {

    // The pre-drive hook (PPBHook::ApplyToPoseTrack) hands over the drive-track
    // state it already has in hand, as plain numbers — the Hkb track structs stay
    // private to that TU. Called for every driven actor every frame; cheap and
    // gated internally, so an unarmed frame costs one atomic load.
    // `rwk` = the first word of hkbRagdollDriver::reportingWhenKeyframed (+0x58) and `allKf` =
    // whether the KEYFRAMED_RAGDOLL_BONES track is on: together with the per-body motion types
    // these answer whether the engine hard-keyframes the eight lower bones under PLANCK, which
    // would make the LEGS the launch source (06 section 3 Q5).
    void NoteDrive(std::uint32_t actorId, bool rbOn, float rbFrac, bool pwOn,
                   float pwMaxForce, int wfmMode, bool ragFlag, bool planckBug,
                   std::uint32_t rwk, bool allKf);

    // Which actor the ring should be sampling (PushStep's last-touched actor).
    void NoteSubject(std::uint32_t actorId);

    // Frame boundary — samples the subject into the ring, and while armed emits
    // the live frames. Called from PushStep::OnFrame (main thread, after the
    // touch snapshot is fresh).
    void OnFrame();

    // F0: the knockdown was just requested. Flushes the ring (the ~10 frames
    // BEFORE the event, which is where H2 and the hit-as-trigger case live) and
    // starts live logging. `magSrc` is where the triggering magnitude came from
    // (raw / peak / chain) — never logged before, and the 2026-08-30 idle probe
    // read maxima BELOW the trigger, so the value that actually fired is unknown.
    void Arm(std::uint32_t actorId, float mmag, int slot, const char* magSrc,
             float bearingDeg, bool ragdoll);

    // Register the TESHitEvent sink (kDataLoaded, idempotent). This is the H3 discriminator:
    // PLANCK's x0.3 "she is already ragdolled" reduction keys on an engine flag that lags the
    // graph by ~13 frames, so a hit landing inside the window is applied at FULL strength.
    void Install();

    // A TESHitEvent the sink saw. `planck` = PLANCK's magic stamp (flags & 0xFFFFFF00 ==
    // 0x59914000, read as a DWORD); `power` = the power-attack bit (PLANCK multiplies the
    // impulse by 1.75); `velMag` = PLANCK's own hit velocity, game u/s.
    void NoteHit(std::uint32_t actorId, bool planck, bool power, float velMag, bool isLeft);

    // Bumped from the 0xDFB722 physics-step chain hook. HIGGS steps twice per frame, so a
    // launch delivered inside one substep is invisible to a once-per-frame sample unless the
    // count is known. Physics thread — a relaxed atomic increment, nothing else.
    void NoteStep();

    // How many bodies PPB's settle clamp actually rescaled this frame. The clamp has NEVER been
    // observed running in VR, so its per-frame count inside the window is the receipt that it
    // did (or did not) act.
    void NoteSettle(std::uint32_t actorId, int capped);

    void ClearOnLoad();
}
