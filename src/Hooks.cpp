#include "PCH.h"
#include "Hooks.h"
#include "PPBHook.h"
#include "Diag.h"       // Diag::Armed / OnPhysicsStep — the physics-step timer sink

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

        const float dz = ArmIK::GetHeelDriveBias();          // set per-driver by ApplyHeelFix (0 = no bias)
        if (dz != 0.f && worldFromModel && s_chainedInnerDrive) {
            alignas(16) RE::hkQsTransform biased = *worldFromModel;
            alignas(16) float t[4];
            _mm_store_ps(t, biased.translation.quad);
            t[2] += dz;
            biased.translation.quad = _mm_load_ps(t);
            if (!s_innerDriveFirstBias.exchange(true, std::memory_order_relaxed))
                logger::info("INNER-DRIVE first biased call: worldFromModel z {:.4f} -> {:.4f} (havok m)", t[2] - dz, t[2]);
            return s_chainedInnerDrive(controller, deltaTime, poseLocalSpace, &biased, stressOut);
        }
        return s_chainedInnerDrive ? s_chainedInnerDrive(controller, deltaTime, poseLocalSpace, worldFromModel, stressOut)
                                   : false;
    }

    // Resolve a code address to the file name of the module that owns it
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
        try {
            ArmIK::ApplyToPoseTrack(driver, deltaTime, generatorOutput);
        } catch (...) {
            static std::atomic<bool> ikWarned{ false };
            if (!ikWarned.exchange(true, std::memory_order_relaxed)) {
                logger::error("ArmIK::ApplyToPoseTrack threw; suppressing future throws.");
            }
        }

        if (s_chainedDriveToPose) {
            s_chainedDriveToPose(driver, deltaTime, context, generatorOutput);
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
