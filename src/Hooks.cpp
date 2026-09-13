#include "PCH.h"
#include "Hooks.h"
#include "PPBHook.h"
#include "PushStep.h"
#include "RagFrame.h"
#include "Tuning.h"     // v7.1: the movement-params override values are knobs
#include "DismemberGuard.h"
#include "PivFix.h"     // PivGuard flag bracket (2026-07-29 v2)  // PlanckSetSetting — the loosen-scope restore (2026-07-29)
#include "Diag.h"
#include "HandBox.h"       // Diag::Armed / OnPhysicsStep — the physics-step timer sink

#include <atomic>
#include <string>
#include <cctype>
#include <Windows.h>

namespace logger = SKSE::log;

namespace Hooks {

    // =========================================================================
    // Pre-driveToPose chain-over hook at VR 0xB266AB.
    //
    // Call site:
    //   Inside BShkbAnimationGraph::UpdateAnimation, the 5-byte CALL that
    //   invokes hkbRagdollDriver::driveToPose(deltaTime, context, output).
    //   This is the seam between the animation-graph-computed pose and the
    //   ragdoll driver that actuates rigid bodies toward it.
    //
    // Contested territory — PLANCK owns this site:
    //   PLANCK (activeragdoll.dll) patches this same 5-byte CALL via
    //   Write5Call during SKSEPlugin_Load (its main.cpp: TryHook →
    //   PerformHooks → driveToPoseHookLoc=0xB266AB). Its hook runs a
    //   PreDriveToPoseHook (loosens ragdoll constraints + optionally
    //   memcpys hkbCharacter::poseLocal over TRACK_POSE for foot-IK)
    //   → original driveToPose → PostDriveToPoseHook.
    //
    //   We chain ON TOP. Install timing is the key: we register for
    //   kDataLoaded (which fires AFTER every plugin's SKSEPlugin_Load has
    //   returned), then call SKSE's write_call<5>. SKSE's trampoline reads
    //   the current 5-byte CALL, decodes its rel32 target — which is now
    //   PLANCK's hook (or AIHands' chain head, if it installed first) — and
    //   returns it to us before overwriting with our own rel32. We save the
    //   previous head and tail-call it from our hook. Multiple kDataLoaded
    //   chainers stack fine: each newcomer becomes the new head and
    //   tail-calls the previous head, so PPB + AIHands + PLANCK coexist.
    //
    //   If PLANCK is absent, write_call<5> hands back the game's original
    //   driveToPose — our chain works either way.
    // =========================================================================

    using DriveToPoseFn = void(*)(RE::hkbRagdollDriver* driver,
                                  float                 deltaTime,
                                  const void*           context,         // hkbContext&
                                  void*                 generatorOutput);// hkbGeneratorOutput&

    // Saved CALL target from the site at 0xB266AB at the moment of our
    // write_call. If PLANCK is loaded, this is PLANCK's DriveToPoseHook (or
    // another chainer's head). If PLANCK is absent, it's the game's original
    // driveToPose — our chain remains correct either way.
    static DriveToPoseFn s_chainedDriveToPose = nullptr;

    static std::atomic<bool>     s_preDriveFirstFire{ false };
    static std::atomic<uint64_t> s_preDriveFires{ 0 };

    // ── INNER-DRIVE chain (heel fix v3, 2026-07-02) ──────────────────────────
    // Inside hkbRagdollDriver::driveToPose the engine calls
    //   hkaRagdollRigidBodyController::driveToPose(this, dt, poseLocalSpace, worldFromModel&, stressOut)
    // at VR 0xA26C05 (PLANCK hooks this same site for its stress capture). The worldFromModel
    // PARAM here is the LAST STOP before the per-bone drive targets are computed — mutating the
    // track or hkbCharacter copies at the pre-drive hook was proven a NO-OP in-VR (the engine
    // driveToPose sources this value earlier/elsewhere; PLANCK's author left the same abandoned
    // experiment commented out in his PreDriveToPoseHook). So the heel fix biases the param HERE:
    // our outer PreDriveChainHook is on the stack for the whole drive, ApplyToPoseTrack parks the
    // current actor's heel offset in a thread-local, and this wrapper forwards a biased COPY.
    using InnerDriveFn = bool(*)(void* controller, float deltaTime, void* poseLocalSpace,
                                 const RE::hkQsTransform* worldFromModel, void* stressOut);
    static InnerDriveFn          s_chainedInnerDrive = nullptr;
    static std::atomic<bool>     s_innerDriveFirstBias{ false };

    static bool InnerDriveChainHook(void* controller, float deltaTime, void* poseLocalSpace,
                                    const RE::hkQsTransform* worldFromModel, void* stressOut)
    {
        // XP32 POSE-CONFORM (2026-07-10): overwrite the drive pose's per-bone TRANSLATIONS with the
        // XP32-chain values parked by PoseConformPrepare in the outer hook (same thread-local per-drive
        // pattern as the heel bias below; disarms on consume). Runs BEFORE we chain so downstream
        // (PLANCK's rigid-body-T pass + the engine controller) consumes the conformed pose. Bone-level;
        // the heel bias is root-level (worldFromModel) — independent, both apply. In-place mutation of
        // poseLocalSpace is the proven pattern here (PLANCK's own inner hook mutates it the same way).
        ArmIK::ApplyPoseConform(poseLocalSpace, worldFromModel);

        // ReDrive v2: set the per-bone weights the outer hook parked, exactly where Havok's own
        // setBoneWeights doc prescribes ("before calling driveToPose ... then HK_NULL again").
        // arg0 is the hkaRagdollRigidBodyController (PLANCK's typedef for this same site).
        // Restore runs on EVERY return path below; Apply is a no-op when nothing is armed.
        ArmIK::ReDriveInnerApply(controller);

        bool ret = false;
        const float dz = ArmIK::GetHeelDriveBias();          // set per-driver by ApplyHeelFix (0 = no bias)
        if (dz != 0.f && worldFromModel && s_chainedInnerDrive) {
            alignas(16) RE::hkQsTransform biased = *worldFromModel;
            alignas(16) float t[4];
            _mm_store_ps(t, biased.translation.quad);
            t[2] += dz;
            biased.translation.quad = _mm_load_ps(t);
            if (!s_innerDriveFirstBias.exchange(true, std::memory_order_relaxed))
                logger::info("INNER-DRIVE first biased call: worldFromModel z {:.4f} -> {:.4f} (havok m)", t[2] - dz, t[2]);
            ret = s_chainedInnerDrive(controller, deltaTime, poseLocalSpace, &biased, stressOut);
        } else {
            ret = s_chainedInnerDrive ? s_chainedInnerDrive(controller, deltaTime, poseLocalSpace, worldFromModel, stressOut)
                                      : false;
        }
        ArmIK::ReDriveInnerRestore(controller);
        return ret;
    }

    // ═══════════════════════════════════════════════════════════════════════════════════
    //  PUSHWALK HOOK — the CALL site into Actor::CheckAndHandleMotionOrAnimationDrivenChange,
    //  VR 0x5E0885. PLANCK Write5Calls this exact site (activeragdoll main.cpp:6424) and, for
    //  its grabbed actors, replaces the engine's re-evaluation with its drag-walk ritual. PPB
    //  chains the same site at kDataLoaded (chain head; PLANCK installed at SKSEPlugin_Load, so
    //  our tail call reaches its hook) and does the identical thing for PRESSURE-pushed actors.
    //  Skipping the chain for an actor we drive is the load-bearing part: the original clears
    //  planner-direct-control every frame.
    // ═══════════════════════════════════════════════════════════════════════════════════
    static std::string ResolveModuleName(std::uintptr_t addr);   // defined below

    using MotionCheckFn = void (*)(RE::Actor*);
    static MotionCheckFn s_chainedMotionCheck = nullptr;

    static void MotionCheckChainHook(RE::Actor* actor)
    {
        bool handled = false;
        try {
            handled = PushStep::OnMotionDrivenCheck(actor);
        } catch (...) {
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true, std::memory_order_relaxed))
                logger::error("PushStep::OnMotionDrivenCheck threw; suppressing future throws.");
        }
        if (!handled && s_chainedMotionCheck)
            s_chainedMotionCheck(actor);
    }

    void InstallPushWalkHook()
    {
        constexpr std::uintptr_t kVROffset = 0x5E0885;
        const auto site = REL::Offset(kVROffset).address();
        const auto* p = reinterpret_cast<const std::uint8_t*>(site);
        if (p[0] != 0xE8) {
            logger::warn("PushWalk hook NOT installed: byte at 0x5E0885 is 0x{:02X}, not E8 — "
                         "site shape unexpected, refusing to patch", p[0]);
            return;
        }
        SKSE::AllocTrampoline(14);   // every installer funds its own write_call<5> (the missing
                                     // line here asserted Trampoline.cpp(117) on startup, 2026-08-29)
        auto& tr = SKSE::GetTrampoline();
        const auto prev = tr.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&MotionCheckChainHook));
        s_chainedMotionCheck = reinterpret_cast<MotionCheckFn>(prev);
        logger::info("PushWalk hook installed at 0x5E0885 (chained; prev target = {})",
                     ResolveModuleName(prev));
    }

    // ═══ MOVEMENT-PARAMS OVERRIDE (v7, 2026-08-30) — PLANCK's OverwriteMovementParameters
    // replica. THE 08:27 DATA: the fade commanded 26→0 u/s while she ran ~150 u/s (321u in
    // 1.85s) — the MovementPlannerArbiter maps our target speed onto the ACTOR'S OWN
    // gait/acceleration parameters, so without overriding them the command is a suggestion.
    // PLANCK hooks the two CALL sites where the arbiter computes speeds (planck_080
    // main.cpp:6763-6822) and substitutes a stack IMovementParameters clone — accel/decel/
    // rotation replaced with its config values, walkRunPercent + type KEPT from the actor.
    // We chain the SAME two sites (PLANCK installed first → our tail call reaches its hook;
    // an actor is never both ours and grabbed — the grab stand-down enforces it).
    // Values are PLANCK's shipped ini: accel 10 / decel 10 / rotation 2.5 / angleAccel 10.
    struct IMoveParamsMirror {   // vtable contract proven by PLANCK handing its clone to the engine
        virtual ~IMoveParamsMirror() = default;              // 0
        virtual float GetWalkRunPercent() = 0;               // 1
        virtual float GetAcceleration() = 0;                 // 2
        virtual float GetDeceleration() = 0;                 // 3
        virtual float GetAngleAcceleration() = 0;            // 4
        virtual float GetRotationPercent() = 0;              // 5
        virtual std::uint32_t GetType() = 0;                 // 6
        virtual void Write(void*) = 0;                       // 7
        virtual void Read(void*) = 0;                        // 8
    };
    struct PushMoveParams final : IMoveParamsMirror {
        float walkRun, accel, decel, angAccel, rotPct; std::uint32_t type;
        // v7.1: all five values are live knobs (defaults = PLANCK's shipped ini). walkRunPercent
        // keeps the ACTOR'S own value unless pushStepWalkRun >= 0 overrides it (the gait dial).
        explicit PushMoveParams(IMoveParamsMirror* src)
            : walkRun(ObjectHold::PushStepWalkRun() >= 0.f ? ObjectHold::PushStepWalkRun()
                                                           : src->GetWalkRunPercent()),
              accel(ObjectHold::PushStepAccel()), decel(ObjectHold::PushStepDecel()),
              angAccel(ObjectHold::PushStepAngAccel()), rotPct(ObjectHold::PushStepRotPct()),
              type(src->GetType()) {}
        float GetWalkRunPercent() override { return walkRun; }
        float GetAcceleration() override { return accel; }
        float GetDeceleration() override { return decel; }
        float GetAngleAcceleration() override { return angAccel; }
        float GetRotationPercent() override { return rotPct; }
        std::uint32_t GetType() override { return type; }
        void Write(void*) override {}
        void Read(void*) override {}
    };
    using CalcSpeedsFn = void (*)(void*, void*, void*, void*, float*, float*, float*);
    using CalcRotFn    = void (*)(void*, void*, void*, void*, float*, float*);
    static CalcSpeedsFn s_chainedCalcSpeeds = nullptr;
    static CalcRotFn    s_chainedCalcRot    = nullptr;

    static bool PushDriven(void* actorState)
    {
        // ActorState sits at Actor+0xB8 (PLANCK main.cpp:6797, same arithmetic).
        auto* actor = reinterpret_cast<RE::Actor*>(
            reinterpret_cast<std::uintptr_t>(actorState) - 0xB8);
        return actor && PushStep::IsDrivingActor(actor->GetFormID());
    }
    static void CalcSpeedsChainHook(void* st, void* params, void* qs, void* mv,
                                    float* a, float* b, float* c)
    {
        if (!s_chainedCalcSpeeds) return;
        if (PushDriven(st)) {
            PushMoveParams o(reinterpret_cast<IMoveParamsMirror*>(params));
            s_chainedCalcSpeeds(st, &o, qs, mv, a, b, c);
        } else {
            s_chainedCalcSpeeds(st, params, qs, mv, a, b, c);
        }
    }
    static void CalcRotChainHook(void* st, void* params, void* qs, void* mv, float* a, float* b)
    {
        if (!s_chainedCalcRot) return;
        if (PushDriven(st)) {
            PushMoveParams o(reinterpret_cast<IMoveParamsMirror*>(params));
            s_chainedCalcRot(st, &o, qs, mv, a, b);
        } else {
            s_chainedCalcRot(st, params, qs, mv, a, b);
        }
    }
    void InstallMoveParamsHooks()
    {
        constexpr std::uintptr_t kSpeeds = 0x116D362;   // ActorState_CalculateSpeedsWithAcceleration CALL
        constexpr std::uintptr_t kRot    = 0x116D39D;   // ActorState_CalculateRotSpeeds CALL
        const auto sSite = REL::Offset(kSpeeds).address();
        const auto rSite = REL::Offset(kRot).address();
        if (*reinterpret_cast<const std::uint8_t*>(sSite) != 0xE8 ||
            *reinterpret_cast<const std::uint8_t*>(rSite) != 0xE8) {
            logger::warn("MoveParams hooks NOT installed: site bytes 0x{:02X}/0x{:02X} not E8 — "
                         "refusing to patch (pushed actors will keep gait-speed, not commanded)",
                         *reinterpret_cast<const std::uint8_t*>(sSite),
                         *reinterpret_cast<const std::uint8_t*>(rSite));
            return;
        }
        SKSE::AllocTrampoline(28);   // two write_call<5>, funded here (the 2026-08-29 lesson)
        auto& tr = SKSE::GetTrampoline();
        const auto prevS = tr.write_call<5>(sSite, reinterpret_cast<std::uintptr_t>(&CalcSpeedsChainHook));
        const auto prevR = tr.write_call<5>(rSite, reinterpret_cast<std::uintptr_t>(&CalcRotChainHook));
        s_chainedCalcSpeeds = reinterpret_cast<CalcSpeedsFn>(prevS);
        s_chainedCalcRot    = reinterpret_cast<CalcRotFn>(prevR);
        logger::info("MoveParams hooks installed at 0x116D362/0x116D39D (chained; prev = {} / {})",
                     ResolveModuleName(prevS), ResolveModuleName(prevR));
    }

    // Resolve a code address to the file name of the module that owns it    // Resolve a code address to the file name of the module that owns it
    // (e.g. "activeragdoll.dll" for PLANCK, "SkyrimVR.exe" for the game's own
    // driveToPose). Makes the chain-over verdict self-evident in the log
    // instead of printing a bare address you'd have to resolve by hand.
    static std::string ResolveModuleName(std::uintptr_t addr)
    {
        if (!addr) return "<null>";
        HMODULE hmod = nullptr;
        if (::GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &hmod) && hmod) {
            char path[MAX_PATH] = {};
            if (::GetModuleFileNameA(hmod, path, MAX_PATH)) {
                std::string s(path);
                const auto pos = s.find_last_of("\\/");
                return (pos == std::string::npos) ? s : s.substr(pos + 1);
            }
        }
        return "<unknown-module>";
    }

    // Case-insensitive "is this PLANCK's DLL?" (activeragdoll.dll).
    static bool IsPlanckModule(const std::string& name)
    {
        std::string lower = name;
        for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return lower.find("activeragdoll") != std::string::npos;
    }

    // Human-readable verdict for the chained-downstream target.
    // IMPORTANT: PLANCK patches 0xB266AB via its OWN branch trampoline —
    // allocated memory that is NOT inside activeragdoll.dll's mapped image — so
    // GetModuleHandleExA on the chained target returns <unknown-module> even
    // though PLANCK IS downstream (this caused a false "PLANCK absent" verdict).
    // Cross-check against whether activeragdoll.dll is loaded at all.
    static std::string ChainVerdict(const std::string& targetMod)
    {
        const bool planckLoaded = (::GetModuleHandleA("activeragdoll.dll") != nullptr);
        if (IsPlanckModule(targetMod))
            return "PLANCK (activeragdoll.dll) CONFIRMED downstream — PPB chained ON TOP, chain intact.";
        if (planckLoaded)
            return "activeragdoll.dll IS loaded; chained target is its trampoline/thunk (not in the DLL image) "
                   "— PLANCK CONFIRMED downstream (possibly via another chainer, e.g. AIHands), chain intact.";
        return "activeragdoll.dll NOT loaded — chained target is the game's own driveToPose (PLANCK absent). "
               "Chain still runs.";
    }

    static void PreDriveChainHook(RE::hkbRagdollDriver* driver,
                                  float                 deltaTime,
                                  const void*           context,
                                  void*                 generatorOutput)
    {
        if (!s_preDriveFirstFire.exchange(true, std::memory_order_relaxed)) {
            const std::string mod = ResolveModuleName(
                reinterpret_cast<std::uintptr_t>(s_chainedDriveToPose));
            logger::info(
                "FIRST-FIRE pre-drive chain hook @ 0xB266AB. "
                "driver=0x{:X} chained_target=0x{:X} -> [{}] {}",
                reinterpret_cast<std::uintptr_t>(driver),
                reinterpret_cast<std::uintptr_t>(s_chainedDriveToPose),
                mod,
                ChainVerdict(mod));
        }
        s_preDriveFires.fetch_add(1, std::memory_order_relaxed);

        // PPB per-actor pipeline: heel fix + CapFix/PivFix calibration + statue stomp, all BEFORE
        // the (chained) driveToPose consumes the pose. Runs for EVERY driven non-player actor —
        // gated internally per feature.
        //
        // Any failure must not propagate: wrap in try/catch so a bad pose
        // resolve can't break the chain.
        // ★ 2.2.0: every exit speaks. The old catch printed one bare line and went silent, so the 2026-09-12 20:56:35
        // throw (a dying summoned Frost Atronach) could not be attributed. Now: what threw, on which NPC, at which step
        // of the spine (ArmIK::SpineBreadcrumb), and how many times — the first 5 in full, then every 100th with the count.
        // Still contained exactly as before: the chained driveToPose below always runs.
        {
            // The message is COPIED inside the catch: e.what() points into the exception object, which is destroyed
            // the moment the catch block ends — logging it afterwards would read freed memory.
            char what[192] = {};
            bool threw = false;
            try {
                ArmIK::ApplyToPoseTrack(driver, deltaTime, generatorOutput);
            } catch (const std::exception& e) {
                threw = true;
                std::snprintf(what, sizeof what, "%s", e.what() ? e.what() : "std::exception (no message)");
            } catch (...) {
                threw = true;
                std::snprintf(what, sizeof what, "%s", "non-standard exception");
            }
            if (threw) {
                static std::atomic<std::uint32_t> s_spineThrows{ 0 };
                const std::uint32_t n = s_spineThrows.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 5 || (n % 100) == 0) {
                    const char* stage = "?"; std::uint32_t who = 0;
                    ArmIK::SpineBreadcrumb(stage, who);
                    logger::error("ArmIK::ApplyToPoseTrack threw #{}: '{}' | actor {:08X} | step: {} "
                                  "(contained — this NPC skips PPB's per-actor work for this frame only; the drive still runs)",
                                  n, what, who, stage ? stage : "?");
                }
            }
        }

        if (s_chainedDriveToPose) {
            s_chainedDriveToPose(driver, deltaTime, context, generatorOutput);
            // ReDrive leak guard: if PLANCK's DriveToPoseHook early-returned (dead actor, no
            // g_activeRagdolls entry), the inner site never fired and the armed weights would
            // survive into the NEXT actor's drive on this thread. Consume-or-disarm, always.
            ArmIK::ReDriveDisarm();
            // PivGuard flag bracket: the pre-drive set PLANCK's pivot-collapse flag to 0 for a
            // PPB-skeleton actor; PLANCK consumed it inside the chained call. Restore the saved
            // global NOW so every other actor sees stock behaviour.
            if (ObjectHold::PivGuardScopeActive()) {
                DismemberGuard::PlanckSetSetting("loosenRagdollConstraintPivots",
                                                 ObjectHold::PivGuardRestoreValue());
                ObjectHold::PivGuardClearScope();
            }
        } else {
            // Should never happen — write_call returns the prior target.
            // If it's null, the site was unpatched and we have no original
            // to call. Log loudly; do nothing (better than crashing).
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                logger::error("Pre-drive hook: s_chainedDriveToPose is NULL — cannot chain!");
            }
        }
    }

    void InstallPreDriveHook()
    {
        constexpr std::uintptr_t kVROffset = 0xB266AB;
        const auto moduleBase = REL::Module::get().base();
        const auto callSite   = moduleBase + kVROffset;

        // Self-abort guard (2026-07-08): this is the site the whole PPB->collviz->AIHands->PLANCK chain
        // runs through, and it was the ONLY one of PPB's three installers lacking the 0xE8 check. .text is
        // DRM-encrypted so this runtime byte read is the only proof the site is a 5-byte CALL — a
        // write_call<5> onto a non-CALL would corrupt the instruction and CTD.
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(callSite);
            if (p[0] != 0xE8) {
                logger::error("pre-drive site 0x{:X} (module+0x{:X}) is NOT a CALL (byte {:02X}) — "
                              "PPB chain hook NOT installed (heel/capfix/pivfix/statue pipeline disabled this run).",
                              callSite, kVROffset, p[0]);
                return;
            }
        }

        // 14 bytes is SKSE's scratch requirement for a 5-byte relative CALL
        // redirect (jmp [rip+0] + 8-byte absolute target).
        SKSE::AllocTrampoline(14);
        auto& trampoline = SKSE::GetTrampoline();

        const auto prevTarget = trampoline.write_call<5>(
            callSite,
            reinterpret_cast<std::uintptr_t>(&PreDriveChainHook));

        s_chainedDriveToPose = reinterpret_cast<DriveToPoseFn>(prevTarget);

        const std::string mod = ResolveModuleName(prevTarget);
        logger::info(
            "Installed pre-drive CHAIN hook at VR 0x{:X} (module+0x{:X}). "
            "Chained downstream target = 0x{:X} -> [{}]. {}",
            callSite, kVROffset, prevTarget, mod,
            ChainVerdict(mod));
    }

    // ── POST-PHYSICS DRIVER chain (heel fix v4 compensation) ─────────────────
    // The CALL to hkbRagdollDriver::postPhysics at 0xB268DC (byte-verified by VerifyPostPhysicsAddresses;
    // PLANCK hooks it too). Downstream (engine + PLANCK) maps the RAISED ragdoll back into the output pose
    // with the unbiased transform — the whole rendered body rose with the fix. We run AFTER downstream and
    // subtract heelZ from the written pose's root bone, so the visual stays planted on her heels.
    using PostPhysicsFn = void(*)(RE::hkbRagdollDriver*, const void*, void*);
    static PostPhysicsFn s_chainedPostPhysics = nullptr;

    static void PostPhysicsDriverChainHook(RE::hkbRagdollDriver* driver, const void* context, void* inOut)
    {
        if (s_chainedPostPhysics) s_chainedPostPhysics(driver, context, inOut);
        try {
            ArmIK::ApplyHeelPostFix(driver, inOut);
        } catch (...) {
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true, std::memory_order_relaxed))
                logger::error("ApplyHeelPostFix threw; suppressing future throws.");
        }
    }

    void InstallPostPhysicsDriverHook()
    {
        constexpr std::uintptr_t kVROffset = 0xB268DC;   // CALL → hkbRagdollDriver::postPhysics
        const auto site = REL::Module::get().base() + kVROffset;
        const auto* p = reinterpret_cast<const std::uint8_t*>(site);
        if (p[0] != 0xE8) {
            logger::info("postPhysics site 0x{:X} (module+0x{:X}) is NOT a CALL (byte {:02X}) — "
                         "heel-fix compensation hook NOT installed.", site, kVROffset, p[0]);
            return;
        }
        SKSE::AllocTrampoline(14);
        auto& tr = SKSE::GetTrampoline();
        const auto prev = tr.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&PostPhysicsDriverChainHook));
        s_chainedPostPhysics = reinterpret_cast<PostPhysicsFn>(prev);
        const std::string mod = ResolveModuleName(prev);
        logger::info("Installed POST-PHYSICS DRIVER chain hook at VR 0x{:X} (module+0x{:X}) — heel-fix visual "
                     "compensation. Chained -> 0x{:X} [{}]. {}", site, kVROffset, prev, mod, ChainVerdict(mod));
    }

    void InstallInnerDriveHook()
    {
        constexpr std::uintptr_t kVROffset = 0xA26C05;   // CALL → hkaRagdollRigidBodyController::driveToPose
        const auto site = REL::Module::get().base() + kVROffset;
        const auto* p = reinterpret_cast<const std::uint8_t*>(site);
        if (p[0] != 0xE8) {   // not a 5-byte CALL → a write_call would corrupt the instruction → CTD. Abort.
            logger::info("Inner-drive site 0x{:X} (module+0x{:X}) is NOT a CALL (byte {:02X}) — "
                         "heel-fix inner hook NOT installed (heelfix will be a no-op).",
                         site, kVROffset, p[0]);
            return;
        }
        SKSE::AllocTrampoline(14);
        auto& tr = SKSE::GetTrampoline();
        const auto prev = tr.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&InnerDriveChainHook));
        s_chainedInnerDrive = reinterpret_cast<InnerDriveFn>(prev);
        const std::string mod = ResolveModuleName(prev);
        logger::info("Installed INNER-DRIVE chain hook at VR 0x{:X} (module+0x{:X}) — the heel-fix bias point. "
                     "Chained -> 0x{:X} [{}]. {}",
                     site, kVROffset, prev, mod, ChainVerdict(mod));
    }

    // READ-ONLY address check for the post-physics seam (no patch, cannot CTD). Logs the 5 bytes at the
    // candidate site + decodes the rel32, so we can confirm it's a 5-byte CALL into the expected function
    // BEFORE ever installing a write_call there (a wrong rel32 target = instant CTD).
    void VerifyPostPhysicsAddresses()
    {
        const auto base = REL::Module::get().base();
        auto check = [&](std::uintptr_t off, const char* name) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(base + off);
            if (p[0] == 0xE8) {
                const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(p + 1);
                const auto target = base + off + 5 + static_cast<std::intptr_t>(rel);
                logger::info("ADDR-CHECK 0x{:X} [{}]: CALL rel32 -> 0x{:X} [{}] (bytes E8 {:02X} {:02X} {:02X} {:02X})",
                             off, name, target, ResolveModuleName(target), p[1], p[2], p[3], p[4]);
            } else {
                logger::info("ADDR-CHECK 0x{:X} [{}]: NOT a 5-byte CALL — first byte 0x{:02X} (bytes {:02X} {:02X} {:02X} {:02X} {:02X}) — a hook here would CTD",
                             off, name, p[0], p[0], p[1], p[2], p[3], p[4]);
            }
        };
        check(0xB268DC, "hkbRagdollDriver::postPhysics?");
    }

    // ── PHYSICS-STEP timing chain (0xDFB722) ─────────────────────────────────
    // The 5-byte CALL to hkpWorld::stepDeltaTime(ahkpWorld*, float) inside bhkWorld::Update — the ONLY
    // seam whose window contains the Havok step. HIGGS Write5Calls this exact site; PLANCK does not. A
    // kDataLoaded write_call<5> reads the current rel32 (HIGGS's hook, sitting in a VirtualAlloc'd
    // trampoline page) as `prev`, saves it, and lands on top; we time the chained call and forward its
    // hkpStepResult. This measures `stepMs`, which is NOT the ship gate (that is the contact counter) —
    // it is the frame-time confounder. The hkpStepResult return type is int32; forward it verbatim.
    using StepFn = std::int32_t(*)(void* ahkpWorld, float dt);
    static StepFn s_chainedStep = nullptr;

    static std::int32_t StepChainHook(void* world, float dt)
    {
        // Unconditional, ahead of the Diag gate: this is the ONLY place PPB can see the real
        // integration delta, and the hand-jitter diagnostic needs it even when Diag is disarmed.
        HandBox::NotePhysicsStepDt(dt);
        RagFrame::NoteStep();          // step counter for the RAGFRAME receipt
                                       // (relaxed atomic only — physics thread, T4)
        if (!Diag::Armed() || !s_chainedStep)
            return s_chainedStep ? s_chainedStep(world, dt) : 0;

        LARGE_INTEGER t0, t1;
        ::QueryPerformanceCounter(&t0);
        const std::int32_t r = s_chainedStep(world, dt);
        ::QueryPerformanceCounter(&t1);
        static const double kMsPerCount = [] {
            LARGE_INTEGER f; ::QueryPerformanceFrequency(&f);
            return 1000.0 / static_cast<double>(f.QuadPart);
        }();
        Diag::OnPhysicsStep(static_cast<double>(t1.QuadPart - t0.QuadPart) * kMsPerCount);
        return r;
    }

    void InstallPhysicsStepHook()
    {
        constexpr std::uintptr_t kVROffset = 0xDFB722;
        const auto site = REL::Module::get().base() + kVROffset;
        const auto* p = reinterpret_cast<const std::uint8_t*>(site);
        if (p[0] != 0xE8) {   // MANDATORY: .text is DRM-encrypted; a wrong/non-CALL site = corrupted instr = CTD
            logger::error("PHYSICS-STEP site 0x{:X} (module+0x{:X}) is NOT a CALL (byte {:02X}) — step timer "
                          "NOT installed. The contact-point ship gate does NOT depend on this.", site, kVROffset, p[0]);
            return;
        }
        SKSE::AllocTrampoline(14);
        auto& tr = SKSE::GetTrampoline();
        const auto prev = tr.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&StepChainHook));
        s_chainedStep = reinterpret_cast<StepFn>(prev);
        const std::string mod = ResolveModuleName(prev);
        logger::info("Installed PHYSICS-STEP chain hook at VR 0x{:X} (module+0x{:X}) — hkpWorld::stepDeltaTime. "
                     "Chained -> 0x{:X} [{}]. NOTE: HIGGS's hook lives in a VirtualAlloc'd trampoline page, so an "
                     "unnamed/odd module here is EXPECTED and is NOT a failure.", site, kVROffset, prev, mod);
    }

}  // namespace Hooks
