#pragma once
#include <cstdint>   // std::uint64_t — the pose-conform perf-stat surface

// ============================================================================
//  PPBHook — the per-actor pipeline that rides PPB's own 0xB266AB pre-drive
//  chain hook, plus the heel-fix halves and the statue store. The namespace
//  stays ArmIK (verbatim port from AIHands' ArmIK.cpp PPB segments) so every
//  ported call site and log line survives the split unchanged.
// ============================================================================
namespace ArmIK {

    // The hook body: runs for EVERY driven non-player actor (no AIH graph-var gate).
    // Order (copied from AIHands ApplyToPoseTrack 4458-4524): TLS heel-bias reset ->
    // guards -> actor resolve -> player skip -> ApplyHeelFix -> CapFixPollFile ->
    // CapFixApply -> PivFix block (bind-rel cache + PivFixApply + PivFollowShoulder)
    // -> statue rotation stomp. The caller (Hooks::PreDriveChainHook) then tail-calls
    // the chained driveToPose.
    void ApplyToPoseTrack(RE::hkbRagdollDriver* driver, float deltaTime, void* generatorOutputRaw);

    // Heel-fix per-thread drive bias for the CURRENT driveToPose (set by ApplyHeelFix inside
    // ApplyToPoseTrack; consumed by Hooks::InnerDriveChainHook while the outer frame is on the stack).
    float GetHeelDriveBias();
    bool  HeeledSticky(RE::Actor* actor);   // spell seen once -> node offset is the authority
    void  ClearHeeledSticky();

    // HEEL AUTHORITY = Heels Fix (HeelsFix.esp), resolved once at kDataLoaded. PPB does NO heel-height
    // recognition of its own: Heels Fix flags an actor with HeelsFixSpell iff it decided she is actively
    // heeled (and drops it the instant the heel goes away). Absent form (Heels Fix not installed) leaves
    // HeelsFixHeeled() always false, so every heel-fix half no-ops.
    void ResolveHeelsFix();
    bool HeelsFixHeeled(RE::Actor* actor);

    // The COMPENSATION half (v4): called from Hooks::PostPhysicsDriverChainHook AFTER downstream
    // postPhysics has written the pose — subtracts heelZ from the root bone so the visual stays planted.
    void ApplyHeelPostFix(RE::hkbRagdollDriver* driver, void* generatorOutputRaw);

    // XP32 POSE-CONFORM (2026-07-10): consumed by Hooks::InnerDriveChainHook (0xA26C05) — overwrites
    // the ragdoll-local drive pose's per-bone TRANSLATIONS with the XP32-chain values parked in the
    // per-drive thread-local by ApplyToPoseTrack (rotations untouched; root bone skipped). Also emits
    // the poseConformDump comparison (incoming vs XP32-derived) — the incoming drive pose is only
    // visible at this seam. No-op unless the outer hook armed it this drive; disarms on consume.
    void ApplyPoseConform(void* poseLocalSpace, const RE::hkQsTransform* worldFromModel);
    // Perf counters for Diag's `perf` report (steady-state conform ns per drive; map-build spikes
    // excluded and logged separately). Reset on perf-arm via ResetPoseConformStats.
    void GetPoseConformStats(std::uint64_t& fires, std::uint64_t& sumNs, std::uint64_t& maxNs);
    void ResetPoseConformStats();

    // STATUE MODE (A-pose spell v3): bind-pose statue on/off per actor (PPB_Native.SetStatuePose).
    void SetStatuePose(RE::Actor* npc, bool on);
    // Console `statue` toggle (ESP-free path): flip the flag on the console-selected NPC, return new state.
    bool ToggleStatuePose(RE::Actor* npc);
    // Read-only statue-flag query (the `probe` command's statue context).
    bool IsStatueFlagged(std::uint32_t id);

    // kPreLoadGame teardown: the Havok world + drivers are rebuilt across a load.
    void ClearBindRelCache();       // drop the per-driver bind-relation / clavicle-follow cache
    void ClearStatueSet();          // drop all statue flags
    void ClearPoseConformCache();   // drop the per-actor node/ragdoll conform map (root + ragdoll pointers dangle)
}
