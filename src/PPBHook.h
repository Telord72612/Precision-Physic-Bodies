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
    // ★ 2.2.0: the step + NPC ApplyToPoseTrack was in on THIS thread (Hooks.cpp's catch names a thrown exception with it).
    void SpineBreadcrumb(const char*& stage, std::uint32_t& actorFormId);

    // Heel-fix per-thread drive bias for the CURRENT driveToPose (set by ApplyHeelFix inside
    // ApplyToPoseTrack; consumed by Hooks::InnerDriveChainHook while the outer frame is on the stack).
    float GetHeelDriveBias();
    bool  HeeledSticky(RE::Actor* actor);   // DEPRECATED 2026-08-29 (offset-is-the-state) — no runtime caller
    void  ClearHeeledSticky();

    // HEEL AUTHORITY = Heels Fix's OUTPUT: the lift it writes onto the XP32 "NPC" node
    // (npcNode->local.translate.z, band 0.01..40u). PPB does NO heel-height recognition of its own —
    // it mirrors that offset, whatever upstream route produced it (SPID carrier 0x817 → Papyrus adds
    // the worker ability → NiOverride lift). ⚠ The worker ABILITY is a Papyrus-churned intermediate
    // (removed on cell detach, remove/re-add on refresh, MCM-conditional) — NEVER gate on it again;
    // that was the 2026-08-29 grounded-heel bug. ResolveHeelsFix() is informational logging only.
    void ResolveHeelsFix();
    bool HeelsFixHeeled(RE::Actor* actor);  // DEPRECATED — kept only for the startup info line

    // The COMPENSATION half (v4): called from Hooks::PostPhysicsDriverChainHook AFTER downstream
    // postPhysics has written the pose — subtracts heelZ from the root bone so the visual stays planted.
    void ApplyHeelPostFix(RE::hkbRagdollDriver* driver, void* generatorOutputRaw);

    // XP32 POSE-CONFORM (2026-07-10): consumed by Hooks::InnerDriveChainHook (0xA26C05) — overwrites
    // the ragdoll-local drive pose's per-bone TRANSLATIONS with the XP32-chain values parked in the
    // per-drive thread-local by ApplyToPoseTrack (rotations untouched; root bone skipped). Also emits
    // the poseConformDump comparison (incoming vs XP32-derived) — the incoming drive pose is only
    // visible at this seam. No-op unless the outer hook armed it this drive; disarms on consume.
    void ApplyPoseConform(void* poseLocalSpace, const RE::hkQsTransform* worldFromModel);

    // ReDrive v2 (2026-08-28): per-bone drive authority via the engine's own
    // BodyData::m_boneWeights channel. Apply/Restore bracket the chained call inside
    // InnerDriveChainHook (0xA26C05 = the call to hkaRagdollRigidBodyController::driveToPose,
    // exactly where Havok's setBoneWeights doc says to set and null the pointer). Disarm is the
    // outer hook's leak guard for drives where the inner site is never reached.
    void ReDriveInnerApply(void* controller);
    void ReDriveInnerRestore(void* controller);
    void ReDriveDisarm();
    // Perf counters for Diag's `perf` report (steady-state conform ns per drive; map-build spikes
    // excluded and logged separately). Reset on perf-arm via ResetPoseConformStats.
    // PUSH SENSE v5 (2026-08-29): the horizontal gap between a trunk bone's Havok BODY and the
    // ANIMATION'S INTENT for it (the drive pose FK'd to world), rest-offset corrected in
    // bone-local space. slot uses the touch-API numbering (4 spine0 / 5 spine1 / 6 spine2 /
    // 11 com / 8 thighR). False until the rest calibration latches or if the reading is stale.
    bool GetPushSenseGap(std::uint32_t actorFormId, int slot, float minMagU,
                         float& dxOut, float& dyOut, float& magOut);
    // v11: PushStep stamps this when it queues a stagger/ragdoll; the sense path publishes ZERO and learns
    // nothing for `seconds` (a reaction animation's servo lag is not a push). Later of two stamps wins.
    void SenseStandDown(std::uint32_t actorFormId, float seconds);
    // ★ v12 TRAVEL SENSOR: the sensed XP32 node's position expressed in HER OWN frame (origin-relative,
    // de-yawed), so her walking and turning cancel out and only a BEND registers. PushStep captures it at
    // first contact and measures the distance travelled since. false = not published / stale.
    bool GetPushNodeLocal(std::uint32_t actorFormId, int slot, float outLocal[3]);
    // ★★ v13: the joint's offset from ITS ANIMATION POINT (the drive pose FK'd to world), RAW — no rest-lag
    // baseline. The travel sensor zeroes it at the instant of contact, which removes each joint's resting lag
    // without an EMA that could absorb the push. World units.
    bool GetPushGapRaw(std::uint32_t actorFormId, int slot, float outRaw[3]);
    // ★ v29 FOOT TRIP CHANNEL: the foot NODE's height above the foot the ANIMATION wants this frame (world game units;
    // foot 0 = R, 1 = L). A stride moves both; a trip or a player lift moves only the node. Its own map - no touch slot,
    // so no push path reads it. false = not published or stale.
    bool GetFootAnimGap(std::uint32_t actorFormId, int foot, float& gapZ);

    // ★★ 2026-09-12 THE FURNITURE VERDICT for the push system (push / shove / feet-lift) — the six-state rule + the lean
    // exception (PPBHook.cpp, "THE FURNITURE VERDICT"). FREE = her body is not in furniture (sit state Normal / WantToSit /
    // WantToSleep, whatever reservation handle she holds); BUSY = sit state 2/3/4/6/7/8; LEAN = busy by state on a
    // lean-only marker (pushable). Safe from any thread: the sit state is read live, the lean answer comes from the
    // pre-drive cache (<= 0.5 s old) — no 3D is touched here. pushStepFurnSitState 0 = the legacy handle rule.
    enum : std::uint8_t { kFurnFree = 0, kFurnBusy = 1, kFurnLean = 2 };
    std::uint8_t PushFurnitureVerdict(RE::Actor* actor);

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
