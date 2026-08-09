#include "PCH.h"
#include "DismemberGuard.h"
#include "Tuning.h"
#include "NpcFingerTest.h"   // StripActorBodiesExcept — the clone body strip (2026-07-26 v2)

#include <Windows.h>   // GetModuleHandleA — PLANCK/DF presence + DF module base
#include <algorithm>   // std::find — head-spawn diff
#include <chrono>
#include <condition_variable>  // the synchronous cut hand-off
#include <cstring>     // memcpy — manual detour stub
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace logger = SKSE::log;

// ── PLANCK plugin API, consumer side ────────────────────────────────────────────────────────
// Mirror of PLANCK's planckinterface001.h (tools/_research/planck/include). The vtable slot
// ORDER below must match that header exactly — it is the interface consumers compile against.
// Types we never touch are declared as layout-compatible opaques (void* / float triples): the
// slot count and calling convention are what matter, not the names.
namespace {
    void StripHeadHair(RE::Actor* a, const char* when);   // defined below the clone logic
    struct PlanckHitData {   // 48 bytes: NiPoint3 ×2 + NiPointer + BSFixedString + bool
        float position[3]{};
        float velocity[3]{};
        void* node     = nullptr;   // NiPointer<NiAVObject> — opaque here (never called)
        void* nodeName = nullptr;   // BSFixedString         — opaque here
        bool  isLeft   = false;
    };

    struct IPlanckInterface001 {
        virtual unsigned int GetBuildNumber() = 0;
        virtual bool Deprecated1(const std::string_view& name, double& out) = 0;
        virtual bool Deprecated2(const std::string& name, double val) = 0;
        // ★★ 2026-07-29 CORRECTION: the vtable follows the BASE interface
        // (planckinterface001.h), which declares Get/SetSettingDouble at the END (slots 13/14) —
        // NOT pluginapi.h's derived textual order. The original decl here was RIGHT; the
        // afternoon "fix" that moved them up to slots 4/5 broke BOTH the settings calls (landed
        // on Remove/AddIgnoredActor) and the ignore calls (landed on the AGGRESSION lists).
        // Overrides never reorder a vtable. Verified against the v0.8.0 tag (build 80000 live).
        virtual void AddIgnoredActor(RE::Actor* actor) = 0;
        virtual void RemoveIgnoredActor(RE::Actor* actor) = 0;
        virtual void AddAggressionIgnoredActor(RE::Actor* actor) = 0;
        virtual void RemoveAggressionIgnoredActor(RE::Actor* actor) = 0;
        virtual void SetAggressionLowTopic(RE::Actor* actor, void* topic) = 0;
        virtual void SetAggressionHighTopic(RE::Actor* actor, void* topic) = 0;
        virtual void AddRagdollCollisionIgnoredActor(RE::Actor* actor) = 0;
        virtual void RemoveRagdollCollisionIgnoredActor(RE::Actor* actor) = 0;
        virtual PlanckHitData GetLastHitData() = 0;
        virtual void* GetCurrentHitEvent() = 0;
        virtual bool GetSettingDouble(const char* name, double& out) = 0;
        virtual bool SetSettingDouble(const char* name, double val) = 0;
    };

    // -- PLANCK 0.7.x vtable order (2026-08-08, the Nexus CTD report) ------------------------
    // PLANCK REORDERED its interface vtable between 0.7.1 and 0.8.x: in 0.7.1 (an OLDER
    // public release still on many users' machines — the current Nexus release is 0.8.1;
    // 0.7.1 layout source-verified from tools/_research/planck/include/pluginapi.h) the
    // settings sit at slots 3/4 and the ignore calls at 5/6; in 0.8.x the ignores sit at 3/4
    // and the settings at 13/14. The struct above matches 0.8.x ONLY, so an outdated 0.7.1
    // user's first settings call jumped into an RTTI descriptor -- execute-AV at
    // activeragdoll+0084FC0 with "loosenRagdollConstraintPivots" still in RDX, exactly as
    // reported. Slot 0 (GetBuildNumber) is identical in both layouts, so the build number
    // read through EITHER declaration is safe and selects the layout below.
    struct IPlanck071 {
        virtual unsigned int GetBuildNumber() = 0;                                  // 0 (same)
        virtual bool Deprecated1(const std::string_view& name, double& out) = 0;    // 1
        virtual bool Deprecated2(const std::string& name, double val) = 0;          // 2
        virtual bool GetSettingDouble(const char* name, double& out) = 0;           // 3
        virtual bool SetSettingDouble(const char* name, double val) = 0;            // 4
        virtual void AddIgnoredActor(RE::Actor* actor) = 0;                         // 5
        virtual void RemoveIgnoredActor(RE::Actor* actor) = 0;                      // 6
    };
    unsigned int g_planckBuild = 0;   // resolved at acquire; selects the layout for every call

    // The handshake message (planckinterface001.cpp): dispatch to "PLANCK", it fills the
    // function pointer, GetApiFunction(1) returns the v1 interface.
    struct PlanckMessage {
        enum { kMessage_GetInterface = 0x92F38745 };
        void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
    };

    IPlanckInterface001* g_planck = nullptr;

    // ── NGD + DF plugin APIs (consumer side; headers supplied by the user, same author) ───────
    // Only the vtable ORDER matters — types we never touch are opaque. Both use the SKSE
    // Dispatch(byteswap('KEY'), &g_API, sizeof(void*), NULL) handshake at kPostLoadGame/kNewGame.
    struct INGDecapitationsAPI {
        virtual std::size_t GetVersion() const = 0;
        virtual bool Decapitate(RE::Actor*, void* params) const = 0;
        virtual bool IsDecapitated(RE::Actor*) const = 0;
        virtual bool IsHead(RE::Actor*) const = 0;            // ★ the authoritative "is a severed head"
    };
    struct IDismemberingFrameworkAPI {
        virtual std::size_t GetVersion() const = 0;
        virtual void Dismember(RE::Actor*, const void*, RE::Actor*, void*, const void*) const = 0;
        virtual bool IsDismembered(RE::Actor*) const = 0;
        virtual bool IsDismemberedNode(RE::Actor*, const void*) const = 0;
        virtual void PostDecapitate(RE::Actor*, RE::Actor*) const = 0;
        virtual void RefreshActorDismemberedState(RE::Actor*) const = 0;
    };
    INGDecapitationsAPI*       g_ngd = nullptr;
    IDismemberingFrameworkAPI* g_df  = nullptr;

    // byteswap of the multichar keys, computed the same way the headers do (compiler multichar
    // rules match across both DLLs). 'NGD'=0x004E4744 -> 0x44474E00 ; 'DF'=0x00004446 -> 0x46440000.
    constexpr std::uint32_t Bswap32(std::uint32_t v) {
        return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    }
    constexpr std::uint32_t kNgdKey = Bswap32(static_cast<std::uint32_t>('NGD'));
    constexpr std::uint32_t kDfKey  = Bswap32(static_cast<std::uint32_t>('DF'));

    // ── guard state ─────────────────────────────────────────────────────────────────────────
    // The plugins whose worn gear marks an actor as dismember-touched. The clone wears NGD
    // replica gear within the same millisecond it spawns (log-proven 11:21:34.954), i.e.
    // before its first driven frame — the first probe is decisive.
    constexpr const char* kNgdEsp = "Next-Gen Decapitations.esp";
    constexpr const char* kDfEsm  = "Dismembering Framework.esm";

    // Gear re-probe schedule, seconds after enrollment (clone equips can trickle in; the
    // corpse's stump caps land ~0.15 s after death). -1 terminates.
    constexpr float kProbeAt[] = { 0.f, 1.f, 2.f, 4.f, 8.f, -1.f };

    struct Rec {
        RE::ActorHandle handle{};
        double enrollTime     = 0.0;   // steady seconds at enrollment
        double deathTime      = 0.0;   // 0 = never seen dying
        double stripAt        = 0.0;   // clone body-strip due time (0 = not scheduled)
        bool   planckIgnored  = false; // we currently hold a PLANCK ignore on it
        bool   permanent      = false; // NGD/DF-touched: ignore + PPB skip forever
        bool   planckPerm     = false; // the PLANCK ignore specifically is FOREVER (clone props
                                       // only). Kept separate from `permanent`, which also means
                                       // "detection settled, stop probing, PPB skip" — conflating
                                       // the two is what stranded decapitated victims (2026-08-02).
        bool   exclPpb        = false; // PPB per-actor systems skip this actor
        bool   firstProbeDone = false; // FF refs block PPB work only until this flips
        int    grabFixTries   = 0;     // Report 09 §6 sub-layer fix attempts
        bool   isClone        = false; // NGD head-clone (FF spawn + NGD gear + never died)
        bool   stripped       = false; // clone body strip executed
        int    severState     = 0;     // DF defer: 0 none / 1 deferred-in-flight / 2 done (pass through)
        int    stripCount     = 0;     // clone collapse re-applications (beats NGD's scale pass)
        double cutAt          = 0.0;   // when to ask DF for the death dismemberment (0 = never)
        bool   cutDone        = false; // death-dismemberment attempt already made
        bool   headLogged     = false; // one-shot severed-head diagnostic dump
        bool   graceDone      = false; // grace expired + restored — never re-ignore this corpse
                                       // (without this, the health<=0 path re-ignores every dead
                                       // body in range forever: ignore->restore->ignore cycle)
        int    probeIdx       = 0;     // next kProbeAt slot; past-end = probing finished
    };

    std::mutex g_lk;
    std::unordered_map<std::uint32_t, Rec> g_recs;
    double g_lastSweep = 0.0;
    bool   g_sinkInstalled = false;
    bool   g_planckAbsentLogged = false;

    double NowS()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    bool DgOn()  { return ObjectHold::DgEnabled(); }
    bool DgLog() { return ObjectHold::DgLogOn(); }

    // Does this actor WEAR anything from NGD/DF? Identity from the owning plugin file — the
    // same "ask the system, never the mesh" law as ReTouch. One inventory walk (~µs, map
    // alloc) — called only at enrollment and on the throttled re-probe schedule, never per
    // frame.
    bool WearsDismemberGear(RE::Actor* a)
    {
        if (!a) return false;
        auto inv = a->GetInventory([](RE::TESBoundObject& o) { return o.IsArmor(); });
        for (auto& [obj, entry] : inv) {
            if (!obj || !entry.second || !entry.second->IsWorn()) continue;
            const auto* file = obj->GetFile(0);
            if (!file) continue;   // dynamic form — not ours
            if (_stricmp(file->fileName, kNgdEsp) == 0 || _stricmp(file->fileName, kDfEsm) == 0)
                return true;
        }
        return false;
    }

    const char* ActorName(RE::Actor* a)
    {
        if (auto* base = a ? a->GetActorBase() : nullptr)
            if (const char* n = base->GetFullName(); n && *n) return n;
        return "<unnamed>";
    }

    void PlanckIgnore(RE::Actor* a, Rec& r, const char* why)
    {
        if (!a || r.planckIgnored) return;
        if (!g_planck) {
            if (!g_planckAbsentLogged) {
                g_planckAbsentLogged = true;
                logger::info("DG: PLANCK interface absent — dismember guard runs detection-only "
                             "(PPB skip still active, no ignore calls).");
            }
            return;
        }
        if (g_planckBuild >= 80000) g_planck->AddIgnoredActor(a);
        else reinterpret_cast<IPlanck071*>(g_planck)->AddIgnoredActor(a);
        r.planckIgnored = true;
        if (DgLog())
            logger::info("DG: PLANCK-ignore {:08X} '{}' ({})", a->GetFormID(), ActorName(a), why);
    }

    void PlanckRestore(RE::Actor* a, Rec& r, const char* why)
    {
        if (!r.planckIgnored) return;
        r.planckIgnored = false;   // clear even if the actor is gone — never double-restore
        if (!g_planck || !a) return;
        if (g_planckBuild >= 80000) g_planck->RemoveIgnoredActor(a);
        else reinterpret_cast<IPlanck071*>(g_planck)->RemoveIgnoredActor(a);
        if (DgLog())
            logger::info("DG: PLANCK-restore {:08X} '{}' ({})", a->GetFormID(), ActorName(a), why);
    }

    // Run one probe on a rec. Returns true when the rec flipped to permanent. Detection is now
    // NGD's authoritative IsHead()/IsDecapitated() when the API is live, with the worn-gear scan
    // as the fallback (and the DF-side signal for dismembered corpses).
    bool Probe(RE::Actor* a, Rec& r)
    {
        r.firstProbeDone = true;
        if (!a) { r.probeIdx = 99; return false; }   // actor gone — stop probing

        const bool isHead = (g_ngd && g_ngd->IsHead(a));               // ★ the real head test
        const bool touched = isHead
                          || (g_ngd && g_ngd->IsDecapitated(a))
                          || (g_df  && g_df->IsDismembered(a))
                          || WearsDismemberGear(a);                     // fallback / no-API path
        if (!touched) return false;

        r.permanent = true;
        r.exclPpb   = true;
        r.probeIdx  = 99;
        if (DgLog())
            logger::info("DG: {:08X} '{}' dismember-touched ({}) -> PPB skip + PLANCK ignore "
                         "PERMANENT", a->GetFormID(), ActorName(a),
                         isHead ? "NGD IsHead" : (g_ngd || g_df ? "API" : "worn gear"));
        // ── PLANCK TEST (2026-07-28) ─────────────────────────────────────────────────────────
        // HYPOTHESIS: PLANCK is what makes a grabbed ragdolled actor MOVE. A corpse you can drag
        // is one PLANCK manages; we have PLANCK-ignored the severed-head clone permanently since
        // day one of this work. That would explain the whole grab saga: HIGGS grabs and holds the
        // head (callbacks fire), the physics are sound (a weapon shoves it), but nothing drives
        // the body in response to the grab constraint.
        // dgHeadPlanck 1 = LEAVE head clones under PLANCK's management (the test).
        // ⚠ This is the setting whose absence originally caused the artifacts PPB was built to
        // stop (PLANCK driving the clone's invisible skeleton — floating head, driven SMP hair).
        // If those return, that is the trade and the answer is "PLANCK is required for the grab".
        // ── VICTIM vs CLONE PROP (2026-08-02, the floating-draugr fix) ──────────────────────
        // User report, A/B-confirmed twice: decapitated draugr hang in mid-air, and the bug
        // disappears with PPB removed OR dgEnable 0 — while dgDeathCut 0 alone does NOT fix it,
        // which acquits our death-cut and convicts this ignore.
        // The defect: `permanent` gates the ONLY PlanckRestore call site (`!r.permanent`) and is
        // never cleared anywhere, so ANY DF/NGD-touched actor was dropped from PLANCK FOREVER —
        // including a real victim that merely lost a limb. PLANCK's documented "re-adds unmanaged
        // ragdolls within 50u" recovery is defeated by the ignore too, so nothing ever took the
        // body back.
        // The reasoning behind the fix is the reporter's own known-good state rather than any
        // theory about PLANCK's internals: WITH PPB UNINSTALLED THE VICTIM STAYS UNDER PLANCK.
        // So to match the state we know works, a victim must stay under PLANCK. Only genuine
        // head-clone PROPS — the invisible full-skeleton actors PPB was built to stop PLANCK
        // driving — keep the permanent ignore. Same test the clone classifier already uses.
        // `exclPpb` still applies to victims, so the arms-off artifact stays fixed.
        const bool cloneProp = isHead || (r.deathTime == 0.0 && a->GetFormID() >= 0xFF000000);
        const bool leaveVictim = !cloneProp && ObjectHold::DgVictimPlanckOn();
        r.planckPerm = cloneProp;   // ONLY props are ignored forever

        const bool skipIgnore = isHead && ObjectHold::DgHeadPlanckOn();
        if (skipIgnore) {
            if (DgLog())
                logger::info("DG: {:08X} head clone LEFT UNDER PLANCK (dgHeadPlanck 1) — grab test",
                             a->GetFormID());
        } else if (leaveVictim) {
            // A dismembered VICTIM. Never add an ignore here, and release one an earlier stage
            // (the pre-sever decapitate-handler ignore) may still be holding — that one exists
            // only to make the sever happen on an un-driven ragdoll, and its job is done by now.
            if (r.planckIgnored) PlanckRestore(a, r, "victim — sever done, back to PLANCK");
            if (DgLog())
                logger::info("DG: {:08X} '{}' is a dismembered VICTIM (not a clone prop) -> PPB "
                             "skip, but LEFT UNDER PLANCK so the ragdoll falls normally "
                             "(dgVictimPlanck 1)", a->GetFormID(), ActorName(a));
        } else {
            PlanckIgnore(a, r, "dismember-touched");
        }
        // HEAD CLONE: authoritative when IsHead() says so; else the heuristic (FF spawn wearing
        // NGD gear that never died = the born-as-a-prop clone). Schedule the body collapse after
        // NGD's spawn-time scale/collision storm settles.
        // ── HEAD TRACKING (2026-07-27, for the race-swap design): when NGD's IsHead() confirms
        // a severed head, dump everything that decides its physics — race, its skeleton path
        // (the file PPB repointed = why it inherits our 130-capsule body bake), scale, base
        // form, and the live ragdoll body count. This is the evidence we need to pick the
        // interception point for "spawn it as <race>Head with a head-only skeleton".
        if (isHead && !r.headLogged) {
            r.headLogged = true;
            auto* base = a->GetActorBase();
            auto* race = base ? base->GetRace() : nullptr;
            const char* rEd  = (race && race->GetFormEditorID()) ? race->GetFormEditorID() : "?";
            const char* skel = (race) ? race->skeletonModels[RE::SEXES::kFemale].GetModel() : nullptr;
            const char* skelM= (race) ? race->skeletonModels[RE::SEXES::kMale].GetModel()   : nullptr;
            int bodies = 0;
            if (auto* root = a->Get3D()) {
                constexpr int kCap = 64;
                void* w[kCap]; const char* nm[kCap]; int n = 0;
                NpcFinger::CountActorBodies(root, w, nm, n, kCap);
                bodies = n;
            }
            logger::info("DG: ===== SEVERED HEAD {:08X} =====", a->GetFormID());
            logger::info("DG:   base={:08X} race={} ({:08X})  scale={:.3f}  isFemale={}",
                         base ? base->GetFormID() : 0, rEd, race ? race->GetFormID() : 0,
                         a->GetScale(), base && base->GetSex() == RE::SEX::kFemale);
            logger::info("DG:   skeletonF='{}'", skel ? skel : "<null>");
            logger::info("DG:   skeletonM='{}'", skelM ? skelM : "<null>");
            logger::info("DG:   live collision bodies={} (a head should need ONE)", bodies);
            logger::info("DG:   -> race-swap target: give this race a head-only skeleton, or "
                         "spawn the head on a dedicated head race");
        }
        // ⚠ 2026-07-28: classification must NOT be gated on dgCloneStrip. That knob was set to 0
        // after the shared-shape incident, which silently killed the head PARK too (it keys off
        // isClone/stripAt) — zero "NFING PARK" lines while the user watched the joints orbit.
        // Classify whenever ANY head treatment is enabled; the action knobs decide what runs.
        const bool cloneLikely = isHead ||
            (r.deathTime == 0.0 && a->GetFormID() >= 0xFF000000);
        if (!r.isClone && cloneLikely &&
            (ObjectHold::DgCloneStripOn() || ObjectHold::DgHeadParkOn() ||
             ObjectHold::DgHeadGrabFixOn())) {
            r.isClone = true;
            r.stripAt = NowS() + ObjectHold::DgStripDelayS();
            if (DgLog())
                logger::info("DG: {:08X} classified HEAD-CLONE ({}) -> body collapse in {:.1f}s",
                             a->GetFormID(), isHead ? "IsHead" : "heuristic",
                             ObjectHold::DgStripDelayS());
            // ── HAIR STRIP (2026-07-28, user-directed test) ─────────────────────────────────
            // The 17:21 grab hunt proved HIGGS DOES grab the head (5 grab/drop pairs logged) but
            // holds the WRONG thing: it picks the body skinned to the mesh triangle nearest the
            // palm, and a head wrapped in an SMP wig resolves to a feather-light HAIR-STRAND
            // body hanging kinematically off the skull — pulling it moves nothing. Unequip the
            // wig from the clone so the palm resolves to the FACE -> head bone -> walks up to
            // the one dynamic body (the COM anchor) -> the head itself is held.
            if (ObjectHold::DgHeadStripHairOn()) StripHeadHair(a, "classify");
        }
        return true;
    }

    // ── DF DISMEMBERMENT DEFER (2026-07-26, the freeze fix) ─────────────────────────────────
    // The 22:46/22:55 freezes: DF severs limbs on a POOL THREAD while PLANCK is still driving
    // the LIVING victim's ragdoll on the main thread — surgery concurrent with the death-frame
    // gear-attach storm deadlocks the engine (dump-proven, twice, identical stack). Fix: chain-
    // hook DF's own Events::MainEvent::ProcessDismemberment (module+0xAD200, clean 5-byte
    // prologue). On the FIRST cut for a victim we (1) PLANCK-ignore it so PLANCK releases over
    // the next frames, (2) deep-copy the HitData, (3) SKIP the cut, (4) re-issue it ~1s later on
    // the MAIN THREAD via the SKSE task queue — by then the actor is dead, PLANCK-released, and
    // the sever runs on an un-driven ragdoll on the right thread. HitData is copied by value
    // (trivially copyable — handles/formids/floats, no owned pointers), so no lifetime worry.
    // ProcessDismemberment returns bool (SA_N in the mangled name); the caller (ProcessHit)
    // captures it (movzx ebx,al), so our hook MUST return a valid bool.
    using ProcDismemberFn = bool(*)(RE::Actor*, RE::HitData*, void*);
    ProcDismemberFn g_dfProcDismemberOrig = nullptr;
    std::uint32_t   g_mainThreadId = 0;   // captured at Install() (kDataLoaded = main thread)

    // DF's DismembermentParams (from the author's API header) — DF passes a pointer to a STACK-
    // LOCAL one (lea r8,[rsp+20]) that dies when the worker returns, so we must deep-copy it.
    // Layout matches DF's (same MSVC + same header) so reads are ABI-safe.
    struct DfDismembermentParams {
        bool         forceExecution = false;
        std::string  specificNode;
        bool         ignoreArmorClass = false;
        bool         noLimbImpulse = false;
        bool         noSoundEffect = false;
        bool         noPlayerEffect = false;
        RE::HitData* hitData = nullptr;
    };

    // Manual ABSOLUTE-JUMP detour (2026-07-26): the target is inside DismemberingFramework.dll,
    // loaded >2GB from PPB.dll — a rel32 jump (SKSE write_branch) overflows ("displacement out
    // of range"). A 14-byte `FF 25 [rip+0] <abs64>` jump has no range limit. We steal the first
    // 14 bytes (7 whole position-independent prologue instructions: mov [rsp+20],rbx + push
    // rbp/rsi/rdi/r12/r13/r14 — no rip-relative operands, so a verbatim copy relocates safely),
    // build a stub [stolen 14][abs-jmp back to src+14], and patch src with an abs-jmp to our hook.
    void* InstallAbsDetour14(std::uintptr_t src, void* hook)
    {
        constexpr std::size_t kSteal = 14;
        void* stub = ::VirtualAlloc(nullptr, kSteal + 14, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
        if (!stub) return nullptr;
        auto* s = static_cast<std::uint8_t*>(stub);
        std::memcpy(s, reinterpret_cast<const void*>(src), kSteal);          // stolen prologue
        s[kSteal + 0] = 0xFF; s[kSteal + 1] = 0x25;                          // jmp qword [rip+0]
        *reinterpret_cast<std::uint32_t*>(s + kSteal + 2) = 0;
        *reinterpret_cast<std::uint64_t*>(s + kSteal + 6) = src + kSteal;    // -> back into DF
        ::FlushInstructionCache(::GetCurrentProcess(), stub, kSteal + 14);

        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(src), kSteal, PAGE_EXECUTE_READWRITE, &old)) {
            ::VirtualFree(stub, 0, MEM_RELEASE); return nullptr;
        }
        auto* p = reinterpret_cast<std::uint8_t*>(src);
        p[0] = 0xFF; p[1] = 0x25;                                            // jmp qword [rip+0]
        *reinterpret_cast<std::uint32_t*>(p + 2) = 0;
        *reinterpret_cast<std::uint64_t*>(p + 6) = reinterpret_cast<std::uint64_t>(hook);
        ::VirtualProtect(reinterpret_cast<void*>(src), kSteal, old, &old);
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(src), kSteal);
        return stub;
    }

    // ── SYNCHRONOUS HAND-OFF (2026-07-27 v3) ────────────────────────────────────────────────
    // v2 deep-copied the args and let DF's worker return immediately. That killed the freeze but
    // ALSO killed dismemberment: ProcessDismemberment's gates after its "request" log are
    // IsDismemberable -> TestActorTypes(victim, AGGRESSOR) -> TestWeaponType(HITDATA), i.e. they
    // re-read live aggressor/weapon state, which is stale a frame later (log: request, never a
    // sever). So instead: the worker BLOCKS while the main thread runs the real function with the
    // ORIGINAL pointers (kept alive by the blocked worker's own stack) and hands back the real
    // bool. Semantically identical to DF calling it itself — only the executing thread changes.
    struct PendingCut {
        RE::Actor*   actor  = nullptr;
        RE::HitData* hit    = nullptr;
        void*        params = nullptr;
        bool         result = false;
        bool         done   = false;
    };
    std::mutex               g_cutLk;
    std::condition_variable  g_cutCv;
    std::vector<PendingCut*> g_cutQueue;

    // ── THE FREEZE FIX (2026-07-27): marshal DF's off-thread cut to the MAIN thread ──────────
    // Root cause (RE + 4 dumps): with bDeferredHitProcess=1, DF runs the whole dismemberment
    // (ProcessHitTemplate -> WaitAndCall std::thread -> ProcessHit -> ProcessDismemberment ->
    // CutLimb -> scenegraph node detach) on a RAW WORKER THREAD. Editing an actor's 3D
    // scenegraph off the main thread races the engine's (FSMP-hooked) parallel skinning in VR
    // and deadlocks. bDeferredHitProcess=0 avoids the thread but breaks the death-confirm so
    // kill-dismemberment misses. THE FIX: keep bDeferredHitProcess=1 (death-confirm intact) and,
    // when ProcessDismemberment is entered OFF the main thread, deep-copy its args and re-issue
    // it on the MAIN thread via the SKSE task queue — the surgery then runs where the engine
    // does its own scenegraph work, no race. Return true so DF treats the cut as handled (it is
    // — just one frame later, main-thread). On-main-thread calls (dead-body cutting) pass through
    // inline unchanged.
    bool ProcessDismembermentHook(RE::Actor* actor, RE::HitData* hitData, void* params)
    {
        // Feature off / no PLANCK (no race) / ALREADY on the main thread -> run inline, real return.
        if (!DgOn() || !ObjectHold::DgDeferDfOn() || !g_planck || !actor ||
            ::GetCurrentThreadId() == g_mainThreadId) {
            return g_dfProcDismemberOrig(actor, hitData, params);
        }

        // OFF the main thread: hand the call to the main thread and BLOCK until it has run, so
        // every argument stays alive and valid (they live on this blocked worker's stack) and we
        // can return DF's real result.
        // ⚠ NEVER SKSE::AddTask HERE (freeze5 dump): the deadlock IS the SKSE task lock — the
        // main thread holds it while stuck in the FSMP-hooked skinning pass while DF's workers
        // sit in RtlEnterCriticalSection inside TaskInterface::AddTask. Our own queue + condvar
        // touches no engine lock.
        const std::uint32_t id = actor->GetFormID();
        PendingCut pc;
        pc.actor = actor; pc.hit = hitData; pc.params = params;
        {
            std::scoped_lock lk(g_cutLk);
            g_cutQueue.push_back(&pc);
        }
        if (DgLog())
            logger::info("DG: DF-HANDOFF {:08X} '{}' -> waiting for the main thread to cut",
                         id, ActorName(actor));

        {   // bounded wait: if the main thread never drains (paused/loading), give up rather
            // than hanging DF's worker forever. Timing out means no cut — never a freeze.
            std::unique_lock lk(g_cutLk);
            if (!g_cutCv.wait_for(lk, std::chrono::milliseconds(500), [&] { return pc.done; })) {
                // remove our entry so a late drain can't touch this dead stack frame
                for (auto it = g_cutQueue.begin(); it != g_cutQueue.end(); ++it)
                    if (*it == &pc) { g_cutQueue.erase(it); break; }
                logger::warn("DG: DF-HANDOFF {:08X} TIMED OUT (main thread never drained) — cut "
                             "skipped this pass", id);
                return false;
            }
        }
        return pc.result;   // DF's real answer, from the main-thread run
    }

    // Drained on the MAIN thread (NpcFinger::OnFrame + DismemberGuard::Tick). Runs the real cut
    // with DF's ORIGINAL arguments while the worker that supplied them is blocked.
    void DrainQueuedCutsImpl()
    {
        std::vector<PendingCut*> due;
        {
            std::scoped_lock lk(g_cutLk);
            if (g_cutQueue.empty()) return;
            due.swap(g_cutQueue);
        }
        for (auto* c : due) {
            bool r = false;
            if (g_dfProcDismemberOrig && c->actor) {
                if (DgLog())
                    logger::info("DG: DF-CUT {:08X} executing on the main thread",
                                 c->actor->GetFormID());
                r = g_dfProcDismemberOrig(c->actor, c->hit, c->params);
            }
            {
                std::scoped_lock lk(g_cutLk);
                c->result = r;
                c->done   = true;
            }
        }
        g_cutCv.notify_all();
    }

    // ── DECAPITATE-HANDLER CHAIN HOOK (2026-07-26, the gib race) ────────────────────────────
    // The engine DecapitateHandler (IHandlerFunctor<Actor, BSFixedStringCI>, slot 1 =
    // ExecuteHandler) is the seam NGD itself hooks. SKSE loads DLLs alphabetically, so NGD
    // (N < P) installed before us — the pointer we save IS NGD's hook, and our PLANCK-ignore
    // lands BEFORE NGD spawns the clone or touches a single body. Earliest possible decap
    // signal; the health<=0 check in Tick covers DF gibs that never raise this event.
    using DecapExecFn = bool(*)(void* self, RE::Actor* actor, const void* command);
    DecapExecFn g_decapOrig = nullptr;

    bool DecapExecHook(void* self, RE::Actor* actor, const void* command)
    {
        if (actor && DgOn()) {
            const double now = NowS();
            std::scoped_lock lk(g_lk);
            auto& r = g_recs[actor->GetFormID()];
            if (!r.graceDone) {
                if (r.enrollTime == 0.0) {
                    r.handle         = actor->GetHandle();
                    r.enrollTime     = now;
                    r.firstProbeDone = true;
                    r.probeIdx       = 1;
                }
                if (r.deathTime == 0.0) {
                    r.deathTime = now;
                    if (DgLog())
                        logger::info("DG: DECAPITATE {:08X} '{}' -> ignore BEFORE the severing",
                                     actor->GetFormID(), ActorName(actor));
                }
                PlanckIgnore(actor, r, "decapitate handler");
            }
        }
        // ── HEAD-CREATION TRACKER (2026-07-27, for the race-swap design) ────────────────────
        // We run BEFORE NGD (it hooked this vtable slot first, so it is our downstream). Snapshot
        // the high-actor set, let the chain build the head, then diff: any NEW actor is the head
        // the engine/NGD just created. For each we log race + skeleton + whether its 3D exists
        // YET — a null Get3D() right here is the interception window where swapping the race
        // would make the engine load OUR head-only skeleton instead of the full body bake.
        const bool track = actor && DgOn() && ObjectHold::DgHeadTrackOn();
        std::vector<std::uint32_t> before;
        auto* pl = RE::ProcessLists::GetSingleton();
        if (track && pl) {
            before.reserve(pl->highActorHandles.size());
            for (auto& h : pl->highActorHandles)
                if (auto p = h.get()) before.push_back(p->GetFormID());
        }

        const bool ret = g_decapOrig ? g_decapOrig(self, actor, command) : false;

        if (track && pl) {
            for (auto& h : pl->highActorHandles) {
                auto p = h.get();
                if (!p) continue;
                const std::uint32_t fid = p->GetFormID();
                if (std::find(before.begin(), before.end(), fid) != before.end()) continue;
                auto* base = p->GetActorBase();
                auto* race = base ? base->GetRace() : nullptr;
                const char* rEd = (race && race->GetFormEditorID()) ? race->GetFormEditorID() : "?";
                const char* skF = race ? race->skeletonModels[RE::SEXES::kFemale].GetModel() : nullptr;
                auto* n3d = p->Get3D();
                logger::info("DG: ##### HEAD SPAWNED DURING DECAP #####");
                logger::info("DG:   victim={:08X} '{}'  ->  new actor {:08X}",
                             actor->GetFormID(), ActorName(actor), fid);
                logger::info("DG:   race={} ({:08X})  base={:08X}  IsHead={}",
                             rEd, race ? race->GetFormID() : 0, base ? base->GetFormID() : 0,
                             (g_ngd && g_ngd->IsHead(p.get())) ? 1 : 0);
                logger::info("DG:   skeletonF='{}'", skF ? skF : "<null>");
                logger::info("DG:   3D BUILT ALREADY? {}  <-- 'no' = we CAN swap the race here "
                             "and the engine will load our head-only skeleton",
                             n3d ? "YES (too late to swap cheaply)" : "NO (INTERCEPTION WINDOW)");
            }
        }
        return ret;
    }

    // ── DEATH-CONFIRMED DISMEMBERMENT (2026-07-27) ──────────────────────────────────────────
    // DF's own death-confirm needs its worker thread (bDeferredHitProcess=1), and that worker is
    // exactly what deadlocks in VR. So we run DF with bDeferredHitProcess=0 (no worker => the
    // deadlock is structurally impossible) and PPB supplies the missing confirmation: we watch
    // the kill, and once the actor is REALLY dead we ask DF to dismember through its PUBLIC API,
    // from the main thread. DF still applies all of its own logic (conditions, chance, armor
    // class, sounds, limb physics) — we only tell it "now, and on this actor".
    struct HitInfo {
        RE::ActorHandle      aggressor;
        RE::TESObjectWEAP*   weapon = nullptr;
        double               when   = 0.0;
        const char*          node   = nullptr;   // limb nearest the blow (hit-located choice)
        bool                 judged = false;     // we HAD the geometry: node==null then means
                                                 // "nothing was close enough" (accuracy gate)
    };
    std::unordered_map<std::uint32_t, HitInfo> g_lastHit;   // victim FormID -> last hit on them

    // The dismemberable humanoid nodes DF ships templates for (seen in its own logs). DF decides
    // whether any given one may be cut; we only choose which to offer.
    constexpr const char* kCutNodes[] = {
        "NPC Neck [Neck]",
        "NPC L Forearm [LLar]", "NPC R Forearm [RLar]",
        "NPC L Calf [LClf]",    "NPC R Calf [RClf]",
    };

    // World position of the first node found by name (nullptr if none resolve).
    bool NodePosOf(RE::Actor* a, const char* const* names, int count, RE::NiPoint3& out)
    {
        auto* root = a ? a->Get3D() : nullptr;
        if (!root) return false;
        for (int i = 0; i < count; ++i) {
            RE::BSFixedString bs(names[i]);
            if (auto* o = root->GetObjectByName(bs)) { out = o->world.translate; return true; }
        }
        return false;
    }

    // ── HIT-LOCATED LIMB CHOICE — LIMB GEOMETRY, NOT JOINT POINTS (2026-07-27) ──────────────
    // DF picks the limb from the HIT POSITION (ValidActorNodes takes a NiPoint3 + radius).
    // TESHitEvent has no position, so we reconstruct one AT HIT TIME, while the geometry still
    // means something: the aggressor's weapon node IS at the impact point.
    // We then measure against the LIMB SPAN, not the joint origin — the same geometry PPB's own
    // collision capsules are built on (bone A -> bone B, exactly how kSlotNode/kSlotNodeL pair
    // up). Nearest-joint-origin misjudges a mid-shin blow (the knee origin can sit farther than
    // a neighbouring joint); point-to-segment against the actual limb does not.
    // dgHitMaxDistU restores the ACCURACY GATE that DF's own radius provided: a blow that lands
    // near no dismemberable limb (a chest stab) severs nothing instead of picking "closest".
    struct LimbSeg { const char* cut; const char* distal; };
    constexpr LimbSeg kLimbSegs[] = {
        { "NPC Neck [Neck]",      "NPC Head [Head]"  },
        { "NPC L Forearm [LLar]", "NPC L Hand [LHnd]" },
        { "NPC R Forearm [RLar]", "NPC R Hand [RHnd]" },
        { "NPC L Calf [LClf]",    "NPC L Foot [Lft ]" },   // trailing space is real (PPB table)
        { "NPC R Calf [RClf]",    "NPC R Foot [Rft ]" },
    };

    float PointSegDist(const RE::NiPoint3& p, const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        const RE::NiPoint3 ab{ b.x - a.x, b.y - a.y, b.z - a.z };
        const RE::NiPoint3 ap{ p.x - a.x, p.y - a.y, p.z - a.z };
        const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
        float t = (len2 > 1e-6f) ? (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2 : 0.f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        const RE::NiPoint3 c{ a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
        const RE::NiPoint3 d{ p.x - c.x, p.y - c.y, p.z - c.z };
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }

    // Returns the limb nearest the blow. evaluatedOut = we had the geometry to judge; combined
    // with a null return that means "judged, and nothing was close enough" -> cut nothing.
    const char* PickHitNode(RE::Actor* victim, RE::Actor* agg, bool& evaluatedOut)
    {
        evaluatedOut = false;
        static const char* kAggNodes[] = { "WEAPON", "NPC R Hand [RHnd]", "NPC L Hand [LHnd]",
                                           "NPC R Forearm [RLar]" };
        RE::NiPoint3 blow;
        if (!agg || !NodePosOf(agg, kAggNodes, static_cast<int>(std::size(kAggNodes)), blow))
            return nullptr;
        auto* vroot = victim ? victim->Get3D() : nullptr;
        if (!vroot) return nullptr;

        const char* best = nullptr;
        float bestD = FLT_MAX;
        for (const auto& seg : kLimbSegs) {
            RE::BSFixedString bsA(seg.cut), bsB(seg.distal);
            auto* oA = vroot->GetObjectByName(bsA);
            if (!oA) continue;
            auto* oB = vroot->GetObjectByName(bsB);
            const float d = oB ? PointSegDist(blow, oA->world.translate, oB->world.translate)
                               : PointSegDist(blow, oA->world.translate, oA->world.translate);
            if (d < bestD) { bestD = d; best = seg.cut; }
        }
        if (!best) return nullptr;
        evaluatedOut = true;                       // we had geometry and formed a judgement
        const float gate = ObjectHold::DgHitMaxDistU();
        if (gate > 0.f && bestD > gate) {
            if (DgLog())
                logger::info("DG: hit-location {:.1f}u from the nearest limb ('{}') — beyond the "
                             "{:.0f}u accuracy gate, no limb offered", bestD, best, gate);
            return nullptr;                        // judged: nothing close enough to sever
        }
        return best;
    }

    class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* ev,
                                              RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (!ev || !ev->target || !DgOn()) return RE::BSEventNotifyControl::kContinue;
            auto* victim = ev->target->As<RE::Actor>();
            if (!victim) return RE::BSEventNotifyControl::kContinue;
            HitInfo hi;
            RE::Actor* agg = nullptr;
            if (ev->cause)
                if ((agg = ev->cause->As<RE::Actor>())) hi.aggressor = agg->GetHandle();
            if (ev->source)
                hi.weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(ev->source);
            hi.when = NowS();
            if (ObjectHold::DgHitLocatedOn())
                hi.node = PickHitNode(victim, agg, hi.judged);   // where the blow actually landed
            std::scoped_lock lk(g_lk);
            g_lastHit[victim->GetFormID()] = hi;
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    HitSink g_hitSink;

    // Ask DF to dismember a freshly-dead actor. MAIN THREAD ONLY (called from the Tick sweep).
    void RequestDeathDismember(RE::Actor* a, std::uint32_t id)
    {
        if (!g_df || !a) return;
        HitInfo hi;
        {
            std::scoped_lock lk(g_lk);
            if (auto it = g_lastHit.find(id); it != g_lastHit.end()) hi = it->second;
            g_lastHit.erase(id);
        }
        RE::Actor* agg = nullptr;
        if (auto p = hi.aggressor.get()) agg = p.get();

        // DF FIRST, ALWAYS. With bDeferredHitProcess = 0 DF often severs synchronously on the
        // killing hit and picks its own limb (in-game: "R Calf"/"L Calf" logged ~16 ms BEFORE
        // our death event). If DF already took a limb this death, adding ours would remove a
        // SECOND one. So we only step in when DF declined — PPB is the fallback, never a
        // duplicate.
        if (g_df->IsDismembered(a)) {
            if (DgLog())
                logger::info("DG: DEATH-CUT {:08X} '{}' skipped — DF already severed a limb "
                             "itself", id, ActorName(a));
            return;
        }

        // ACCURACY GATE: we had the geometry and judged no limb close enough to the blow — so
        // nothing is severed. This is the DF proximity requirement we must supply ourselves,
        // because its public API takes an explicit node and no hit position.
        if (ObjectHold::DgHitLocatedOn() && hi.judged && !hi.node) {
            if (DgLog())
                logger::info("DG: DEATH-CUT {:08X} '{}' — blow landed away from any limb, "
                             "nothing offered", id, ActorName(a));
            return;
        }

        // Offer nodes until DF actually takes one (it applies its own conditions/chance to each,
        // so this is "consider this limb", not "force a cut"). dgDeathNodeTries caps how many
        // are offered per death — 1 keeps DF's natural one-consideration-per-kill feel.
        const int tries = static_cast<int>(ObjectHold::DgDeathNodeTries());
        static std::uint32_t s_rot = 0;
        const int nNodes = static_cast<int>(std::size(kCutNodes));
        for (int t = 0; t < tries && t < nNodes; ++t) {
            // First offer the HIT-LOCATED limb (nearest the killing blow); later tries fall
            // back to the rotation so a declined limb still gives the next one a chance.
            const char* node = (t == 0 && hi.node) ? hi.node : kCutNodes[(s_rot++) % nNodes];
            RE::BSFixedString bs(node);
            if (g_df->IsDismemberedNode(a, &bs)) continue;   // already off — offer the next one
            g_df->Dismember(a, &bs, agg, hi.weapon, nullptr);
            const bool cut = g_df->IsDismemberedNode(a, &bs);
            if (DgLog())
                logger::info("DG: DEATH-CUT {:08X} '{}' offered '{}' ({}) to DF -> {}",
                             id, ActorName(a), node,
                             (t == 0 && hi.node) ? "hit-located" : "rotation",
                             cut ? "SEVERED" : "declined");
            if (cut) break;   // one limb per death
        }
    }

    // ── NGD HEAD-BIRTH TRACKER (2026-07-27) ─────────────────────────────────────────────────
    // The engine's DecapitateHandler is NOT in DF/NGD's path (0 fires, proven in-game), so the
    // tracker moves to NGD's OWN decap steps:
    //   OnDecapitate_Step01(victim, params, bool) -> RETURNS the head ref  (birth moment)
    //   OnDecapitate_Step02(victim, HEAD, u32, bool, params)               (head as a parameter)
    // Both log the head's race + skeleton + whether its 3D exists YET. A head with NO 3D at
    // Step01 is the window where swapping its race would make the engine load a PPB head-only
    // skeleton instead of the victim's full 130-capsule body bake — the user's race-swap design.
    using NgdStep01Fn = RE::TESObjectREFR*(*)(RE::Actor*, void*, bool);
    using NgdStep02Fn = void(*)(RE::Actor*, RE::Actor*, std::uint32_t, bool, void*);
    NgdStep01Fn g_ngdStep01Orig = nullptr;
    NgdStep02Fn g_ngdStep02Orig = nullptr;

    void LogHeadBirth(const char* where, RE::Actor* victim, RE::TESObjectREFR* headRef)
    {
        if (!headRef || !ObjectHold::DgHeadTrackOn()) return;
        auto* head = headRef->As<RE::Actor>();
        auto* base = head ? head->GetActorBase() : nullptr;
        auto* race = base ? base->GetRace() : nullptr;
        const char* rEd = (race && race->GetFormEditorID()) ? race->GetFormEditorID() : "?";
        const char* skF = race ? race->skeletonModels[RE::SEXES::kFemale].GetModel() : nullptr;
        auto* n3d = headRef->Get3D();
        int bodies = 0;
        if (n3d) {
            constexpr int kCap = 64; void* w[kCap]; const char* nm[kCap]; int n = 0;
            NpcFinger::CountActorBodies(n3d, w, nm, n, kCap);
            bodies = n;
        }
        logger::info("DG: ##### NGD HEAD @ {} #####", where);
        logger::info("DG:   victim={:08X} '{}' -> head ref {:08X} (isActor={})",
                     victim ? victim->GetFormID() : 0, victim ? ActorName(victim) : "?",
                     headRef->GetFormID(), head ? 1 : 0);
        logger::info("DG:   head race={} ({:08X})  base={:08X}", rEd,
                     race ? race->GetFormID() : 0, base ? base->GetFormID() : 0);
        logger::info("DG:   head skeletonF='{}'", skF ? skF : "<null>");
        logger::info("DG:   head 3D BUILT? {}   bodies={}", n3d ? "YES" : "NO", bodies);
        logger::info("DG:   {}", n3d
            ? "-> 3D already up: a race swap here needs a 3D rebuild (costly) — go earlier"
            : "-> ★ NO 3D YET: THIS is the race-swap window (swap race -> engine loads OUR "
              "head-only skeleton)");
    }


    // ── HAIR STRIP (2026-07-28, user-directed) ─────────────────────────────────────────────
    // HIGGS holds the body skinned to the mesh triangle nearest the palm; a wig-covered head
    // resolves to a kinematic SMP hair strand, so the grab holds hair and the head never moves.
    // v1 failed on BOTH counts the 17:37 log exposed: queued-mode unequip (a dead AI-less clone
    // never processes its equip queue) and the wig re-appearing because NGD replicates head gear
    // at Step02, seconds AFTER our classify-time strip. So: unequip in IMMEDIATE mode, then
    // REMOVE the item so a later NGD pass has nothing to re-equip — and the park loop re-calls
    // this for the first passes to mop up gear that arrives late.
    void StripHeadHair(RE::Actor* a, const char* when)
    {
        if (!a) return;
        auto* aem = RE::ActorEquipManager::GetSingleton();
        for (auto slot : { RE::BGSBipedObjectForm::BipedObjectSlot::kHair,
                           RE::BGSBipedObjectForm::BipedObjectSlot::kLongHair }) {
            auto* wig = a->GetWornArmor(slot);
            if (!wig) continue;
            if (aem)
                aem->UnequipObject(a, wig, nullptr, 1, nullptr,
                                   /*queueEquip*/ false, /*forceEquip*/ false,
                                   /*playSounds*/ false, /*applyNow*/ true);
            a->RemoveItem(wig, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            logger::info("DG: HEAD {:08X} hair-strip[{}]: unequipped+removed '{}' ({:08X})",
                         a->GetFormID(), when,
                         wig->GetName() ? wig->GetName() : "?", wig->GetFormID());
        }
    }

    // ── THE HEAD SKELETON SWAP (2026-07-27, the user's design) ──────────────────────────────
    // Proven in-game: at Step01's RETURN the head actor exists with NO 3D (bodies=0) — it loads
    // its skeleton AFTERWARDS, from ITS RACE. And its race is the victim's, which is why the
    // severed head inherits our full 130-capsule body bake.
    // So: for the duration of the head's 3D load we point that race's female skeleton at the
    // matching *_head.nif (PPB's bake with every non-head capsule collapsed and every non-head
    // body set to collide with nothing). The head then loads a skeleton whose ONLY collision is
    // the head — exactly "give the head its own Havok body" — and because it is a SEPARATE FILE
    // used only by heads, the shared-shape contamination that broke live NPCs cannot happen.
    // The path is restored the moment the head's 3D exists (polled in Tick) or on timeout.
    struct PendingSkelRestore {
        RE::TESRace*    race = nullptr;
        std::string     original;
        RE::ActorHandle head;
        double          deadline = 0.0;
        bool            active = false;
    };
    PendingSkelRestore g_skelSwap;

    void RestoreSwappedSkeleton(const char* why)
    {
        if (!g_skelSwap.active) return;
        if (g_skelSwap.race)
            g_skelSwap.race->skeletonModels[RE::SEXES::kFemale].SetModel(g_skelSwap.original.c_str());
        if (DgLog())
            logger::info("DG: head-skeleton restored -> '{}' ({})", g_skelSwap.original, why);
        g_skelSwap = PendingSkelRestore{};
    }

    RE::TESObjectREFR* NgdStep01Hook(RE::Actor* victim, void* params, bool b)
    {
        // Arm the swap BEFORE the engine creates the head (it loads its skeleton after this).
        bool armed = false;
        if (victim && DgOn() && ObjectHold::DgHeadSkelOn() && !g_skelSwap.active) {
            auto* base = victim->GetActorBase();
            auto* race = base ? base->GetRace() : nullptr;
            const char* cur = race ? race->skeletonModels[RE::SEXES::kFemale].GetModel() : nullptr;
            // Only ever touch a PPB skeleton we authored a head variant for, and never one that
            // is already a head file. "...\PPB\x.nif" -> "...\PPB\x_head.nif".
            if (cur && *cur && std::strstr(cur, "\\PPB\\") && !std::strstr(cur, "_head.nif")) {
                std::string s(cur);
                const auto dot = s.rfind(".nif");
                if (dot != std::string::npos) {
                    g_skelSwap.race     = race;
                    g_skelSwap.original = s;
                    g_skelSwap.deadline = NowS() + ObjectHold::DgHeadSkelHoldS();
                    g_skelSwap.active   = true;
                    const std::string headPath = s.substr(0, dot) + "_head.nif";
                    race->skeletonModels[RE::SEXES::kFemale].SetModel(headPath.c_str());
                    armed = true;
                    if (DgLog())
                        logger::info("DG: head-skeleton ARMED for '{}' -> '{}'",
                                     race->GetFormEditorID() ? race->GetFormEditorID() : "?",
                                     headPath);
                }
            }
        }

        auto* head = g_ngdStep01Orig ? g_ngdStep01Orig(victim, params, b) : nullptr;

        if (armed) {
            if (auto* ha = head ? head->As<RE::Actor>() : nullptr)
                g_skelSwap.head = ha->GetHandle();      // Tick restores once its 3D exists
            else
                RestoreSwappedSkeleton("no head actor returned");
        }
        LogHeadBirth("Step01 RETURN (birth)", victim, head);
        return head;
    }

    void NgdStep02Hook(RE::Actor* victim, RE::Actor* head, std::uint32_t a, bool b, void* params)
    {
        LogHeadBirth("Step02 ENTRY", victim, head);
        if (g_ngdStep02Orig) g_ngdStep02Orig(victim, head, a, b, params);
    }

    // ── death sink ──────────────────────────────────────────────────────────────────────────
    // Both stages (dying + dead) — the EARLIEST signal wins the race against DF's deferred
    // one-frame death-confirm. Idempotent per actor via the rec map.
    class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* ev,
                                              RE::BSTEventSource<RE::TESDeathEvent>*) override
        {
            if (!ev || !DgOn()) return RE::BSEventNotifyControl::kContinue;
            auto* actor = ev->actorDying ? ev->actorDying->As<RE::Actor>() : nullptr;
            if (!actor || actor == RE::PlayerCharacter::GetSingleton())
                return RE::BSEventNotifyControl::kContinue;

            const double now = NowS();
            std::scoped_lock lk(g_lk);
            auto& r = g_recs[actor->GetFormID()];
            if (r.enrollTime == 0.0) {           // fresh rec (existing actor dying)
                r.handle         = actor->GetHandle();
                r.enrollTime     = now;
                r.firstProbeDone = true;         // never block PPB on an already-built actor
                r.probeIdx       = 1;            // skip the 0 s probe; stumps land ~0.15 s in
            }
            if (r.deathTime == 0.0) {
                r.deathTime = now;
                if (DgLog())
                    logger::info("DG: death {:08X} '{}' (dead={}) — enrolled for gear probes{}",
                                 actor->GetFormID(), ActorName(actor), ev->dead,
                                 ObjectHold::DgDeathIgnoreOn() ? " + death-grace ignore" : "");
            }
            // Schedule the death-confirmed dismemberment request. ⚠ 2026-07-27: this used to
            // require ev->dead (the FINAL death stage) — but that event NEVER ARRIVES in this
            // setup (log: 4x "(dead=false)", 0x "(dead=true)"), so the request never armed and
            // our hit-located selection never ran once. Arm on ANY death event; the
            // dgDeathCutDelayS wait is what lets death actually finalise.
            if (ObjectHold::DgDeathCutOn() && r.cutAt == 0.0 && !r.cutDone)
                r.cutAt = now + ObjectHold::DgDeathCutDelayS();
            // v2 (2026-07-26): the death-INSTANT ignore is now OPT-IN (dgDeathIgnore, default
            // 0). Evidence: PLANCK's next frame-tick removed the dying victim's ragdoll from
            // the Havok world, and DF's one-frame deferred death-confirm then found no bodies
            // to sever — 3/3 NPC-vs-NPC kills got "Dismemberment request" but NO grant
            // (13:17-13:18 log), vs request->grant in 1 ms pre-guard. Post-hoc protection
            // (gear probes -> permanent ignore within ~1 s of a granted dismember) stays.
            if (ObjectHold::DgDeathIgnoreOn())
                PlanckIgnore(actor, r, "death grace");
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    DeathSink g_sink;
}

namespace DismemberGuard {

    // PLANCK runtime settings passthrough (used by the per-actor loosen scoping in Hooks.cpp).
    bool PlanckGetSetting(const char* name, double& out)
    {
        if (!g_planck) return false;
        if (g_planckBuild >= 80000) return g_planck->GetSettingDouble(name, out);
        return reinterpret_cast<IPlanck071*>(g_planck)->GetSettingDouble(name, out);
    }
    bool PlanckSetSetting(const char* name, double val)
    {
        if (!g_planck) return false;
        if (g_planckBuild >= 80000) return g_planck->SetSettingDouble(name, val);
        return reinterpret_cast<IPlanck071*>(g_planck)->SetSettingDouble(name, val);
    }


    void AcquirePlanck()
    {
        if (g_planck) return;   // idempotent (the AcquireHiggs pattern)
        if (!::GetModuleHandleA("activeragdoll.dll")) {
            logger::info("DG: PLANCK(activeragdoll.dll) not loaded — dismember guard detection-only.");
            return;
        }
        auto* msging = SKSE::GetMessagingInterface();
        if (!msging) { logger::info("DG: no SKSE messaging — PLANCK handshake unavailable."); return; }

        PlanckMessage pm;
        msging->Dispatch(PlanckMessage::kMessage_GetInterface, static_cast<void*>(&pm),
                         sizeof(PlanckMessage*), "PLANCK");
        if (!pm.GetApiFunction) { logger::info("DG: PLANCK did not answer the interface handshake."); return; }
        g_planck = static_cast<IPlanckInterface001*>(pm.GetApiFunction(1));
        if (g_planck) {
            g_planckBuild = g_planck->GetBuildNumber();   // slot 0: identical in every layout
            if (g_planckBuild < 70000) {
                logger::warn("DG: PLANCK build {} predates the 0.7.1 interface this plugin "
                             "knows -- interface calls DISABLED (detection-only). Update PLANCK.",
                             g_planckBuild);
                g_planck = nullptr;
            } else {
                logger::info("DG: PLANCK build {} -> {} vtable layout (settings@{}, ignores@{})",
                             g_planckBuild, g_planckBuild >= 80000 ? "0.8.x" : "0.7.x",
                             g_planckBuild >= 80000 ? "13/14" : "3/4",
                             g_planckBuild >= 80000 ? "3/4" : "5/6");
            }
        }
        if (!g_planck) { logger::info("DG: PLANCK GetApiFunction(1) returned null."); return; }
        logger::info("DG: PLANCK interface=OK build={} — dismember guard live (dgEnable={}, grace={:.0f}s).",
                     g_planck->GetBuildNumber(), DgOn() ? 1 : 0, ObjectHold::DgGraceDeadS());
    }

    void DrainQueuedCuts() { DrainQueuedCutsImpl(); }   // public entry (Tick + NpcFinger::OnFrame)

    void AcquireDfNgd()
    {
        auto* msging = SKSE::GetMessagingInterface();
        if (!msging) return;
        if (!g_ngd) {
            void* api = nullptr;
            msging->Dispatch(kNgdKey, &api, sizeof(void*), nullptr);
            g_ngd = static_cast<INGDecapitationsAPI*>(api);
            if (g_ngd) logger::info("DG: NGD API acquired (build {}) — IsHead() detection live.",
                                    g_ngd->GetVersion());
        }
        if (!g_df) {
            void* api = nullptr;
            msging->Dispatch(kDfKey, &api, sizeof(void*), nullptr);
            g_df = static_cast<IDismemberingFrameworkAPI*>(api);
            if (g_df) logger::info("DG: DF API acquired (build {}).", g_df->GetVersion());
        }
    }

    void Install()
    {
        if (g_sinkInstalled) return;
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESDeathEvent>(&g_sink);
            holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);   // aggressor/weapon for DF's API
            g_sinkInstalled = true;
            logger::info("DG: TESDeathEvent + TESHitEvent sinks registered (death-confirmed "
                         "dismemberment {}).", ObjectHold::DgDeathCutOn() ? "ON" : "off");
        }
        // NGD head-birth tracker: chain OnDecapitate_Step01 (RVA 0x49F00) + Step02 (0x4AFA0).
        // Both carry the SAME 14-byte position-independent prologue as DF's hook target
        // (48 89 5C 24 20 + push rbp/rsi/rdi/r12/r13/r14), verified byte-for-byte before any
        // patch — a mismatch (NGD update) disables the tracker instead of corrupting NGD.
        if (ObjectHold::DgHeadTrackOn() && !g_ngdStep01Orig) {
            if (HMODULE ngdMod = ::GetModuleHandleA("NextGenDecapitations.dll")) {
                // mov [rsp+XX],rbx ; push rbp,rsi,rdi,r12,r13,r14  = exactly 14 bytes of whole,
                // position-independent instructions. Byte 4 is the stack displacement and VARIES
                // (Step01 uses 0x18, Step02/DF use 0x20) — wildcard it, or the hook silently
                // refuses to install (that is the bug that skipped Step01 last run).
                static const std::uint8_t kProlog[14] = { 0x48,0x89,0x5C,0x24,0x00,0x55,0x56,0x57,
                                                          0x41,0x54,0x41,0x55,0x41,0x56 };
                auto prologOk = [](const std::uint8_t* p) {
                    for (int i = 0; i < 14; ++i) {
                        if (i == 4) continue;                     // displacement byte: any value
                        if (p[i] != kProlog[i]) return false;
                    }
                    return true;
                };
                struct { std::uintptr_t rva; const char* name; void* hook; void** orig; } kT[] = {
                    { 0x49F00, "OnDecapitate_Step01", reinterpret_cast<void*>(NgdStep01Hook),
                      reinterpret_cast<void**>(&g_ngdStep01Orig) },
                    { 0x4AFA0, "OnDecapitate_Step02", reinterpret_cast<void*>(NgdStep02Hook),
                      reinterpret_cast<void**>(&g_ngdStep02Orig) },
                };
                for (auto& t : kT) {
                    auto addr = reinterpret_cast<std::uintptr_t>(ngdMod) + t.rva;
                    const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
                    if (!prologOk(p)) {
                        logger::warn("DG: NGD {} prologue mismatch at NGD+0x{:X} — head tracker "
                                     "skipped for it (NGD updated?).", t.name, t.rva);
                        continue;
                    }
                    *t.orig = InstallAbsDetour14(addr, t.hook);
                    logger::info("DG: NGD {} tracker installed at NGD+0x{:X} ({}).", t.name, t.rva,
                                 *t.orig ? "ok" : "STUB ALLOC FAILED");
                }
            }
        }

        // Capture the MAIN thread id (Install runs at kDataLoaded = main thread). The
        // ProcessDismemberment hook compares against this to decide inline vs marshal.
        g_mainThreadId = ::GetCurrentThreadId();

        // DF FREEZE FIX — chain Events::MainEvent::ProcessDismemberment (DF+0xAD200) via the abs-
        // jump detour. On a worker-thread call it marshals the cut to the main thread (see the
        // hook). Byte-guarded so a DF update disables it instead of corrupting DF.
        if (ObjectHold::DgDeferDfOn() && !g_dfProcDismemberOrig) {
            if (HMODULE dfMod = ::GetModuleHandleA("DismemberingFramework.dll")) {
                constexpr std::uintptr_t kProcDismemberRVA = 0xAD200;
                auto addr = reinterpret_cast<std::uintptr_t>(dfMod) + kProcDismemberRVA;
                const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
                // Verify the 14-byte steal lands on the known prologue: mov [rsp+20],rbx (48 89
                // 5C 24 20) + push rbp (55) + push rsi (56) + push rdi (57) + push r12/r13/r14
                // (41 54 / 41 55 / 41 56). All position-independent -> a verbatim copy relocates.
                const bool ok =
                    p[0]==0x48 && p[1]==0x89 && p[2]==0x5C && p[3]==0x24 && p[4]==0x20 &&
                    p[5]==0x55 && p[6]==0x56 && p[7]==0x57 &&
                    p[8]==0x41 && p[9]==0x54 && p[10]==0x41 && p[11]==0x55 &&
                    p[12]==0x41 && p[13]==0x56;
                if (ok) {
                    g_dfProcDismemberOrig = reinterpret_cast<ProcDismemberFn>(
                        InstallAbsDetour14(addr,
                            reinterpret_cast<void*>(ProcessDismembermentHook)));
                    if (g_dfProcDismemberOrig)
                        logger::info("DG: DF ProcessDismemberment defer-hook installed at "
                                     "DF+0x{:X} (abs-jump detour, orig-stub={:X}).",
                                     kProcDismemberRVA,
                                     reinterpret_cast<std::uintptr_t>(g_dfProcDismemberOrig));
                    else
                        logger::warn("DG: DF defer-hook stub alloc FAILED — dismember-defer off.");
                } else {
                    logger::warn("DG: DF+0x{:X} prologue mismatch (first bytes {:02X} {:02X} "
                                 "{:02X} {:02X} {:02X} {:02X}) — DF updated? Dismember-defer "
                                 "DISABLED (no patch written).", kProcDismemberRVA,
                                 p[0], p[1], p[2], p[3], p[4], p[5]);
                }
            } else {
                logger::info("DG: DismemberingFramework.dll not loaded — dismember-defer skipped.");
            }
        }
        // Chain the engine DecapitateHandler (slot 1). Installed at kDataLoaded, AFTER NGD's
        // own hook (alphabetical DLL order) — so we wrap NGD and run first. One-shot.
        if (!g_decapOrig) {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_DecapitateHandler[0] };
            g_decapOrig = reinterpret_cast<DecapExecFn>(
                vtbl.write_vfunc(1, reinterpret_cast<std::uintptr_t>(DecapExecHook)));
            logger::info("DG: DecapitateHandler chained (downstream={:X} — expect NGD's hook).",
                         reinterpret_cast<std::uintptr_t>(g_decapOrig));
        }
    }

    bool IsExcluded(RE::Actor* a)
    {
        if (!a || !DgOn()) return false;
        std::scoped_lock lk(g_lk);
        auto it = g_recs.find(a->GetFormID());
        if (it == g_recs.end()) return false;
        // Confirmed dismember-touched, OR an FF spawn whose first gear-probe hasn't run yet
        // (that probe happens inside Tick on the same frame, so this window is ~one call).
        return it->second.exclPpb || !it->second.firstProbeDone;
    }

    void Tick(RE::Actor* driven)
    {
        if (!DgOn()) return;
        const double now = NowS();

        // Enrollment: a runtime-spawned (FF-ref) actor on its driven frames. First gear-probe
        // runs IMMEDIATELY so a clone is confirmed before any PPB rig can land on it, and a
        // legit spawn is cleared with zero delay.
        if (driven && driven != RE::PlayerCharacter::GetSingleton() &&
            driven->GetFormID() >= 0xFF000000) {
            std::scoped_lock lk(g_lk);
            auto& r = g_recs[driven->GetFormID()];
            if (r.enrollTime == 0.0) {
                r.handle     = driven->GetHandle();
                r.enrollTime = now;
                Probe(driven, r);
                if (!r.permanent) r.probeIdx = 1;   // continue background re-probes
            }
        }

        // NOTE (2026-07-27): the health<=0 "early kill detection" was REMOVED. It PLANCK-ignored
        // any driven actor reading <=0 health — which includes BLEEDOUT / ESSENTIAL / knocked-out
        // NPCs that recover; with corpses-ignored-forever they never got their ragdoll back
        // ("NPCs had no Havok bodies", user-reported). Death detection is now TESDeathEvent +
        // the decap handler ONLY (real, final deaths), which never fires on a recovering actor.

        // Execute any DF cuts captured off-thread (main thread here, no SKSE task lock involved).
        DrainQueuedCutsImpl();

        // Head-skeleton swap: hold the race pointed at the *_head.nif only until the head has
        // actually loaded its 3D, then put the real path back. The window must stay SHORT — any
        // other actor of that race loading 3D inside it would also get the head skeleton.
        if (g_skelSwap.active) {
            bool built = false;
            if (auto p = g_skelSwap.head.get()) built = (p->Get3D() != nullptr);
            if (built)                    RestoreSwappedSkeleton("head 3D built");
            else if (now > g_skelSwap.deadline) RestoreSwappedSkeleton("timeout");
        }

        // Throttled maintenance sweep (2 Hz): scheduled re-probes + grace restore + cleanup.
        if (now - g_lastSweep < 0.5) return;
        g_lastSweep = now;

        std::scoped_lock lk(g_lk);
        for (auto it = g_recs.begin(); it != g_recs.end();) {
            Rec& r = it->second;
            RE::Actor* a = nullptr;
            if (auto ptr = r.handle.get(); ptr) a = ptr.get();

            // Scheduled gear re-probes (relative to enrollment; death also re-anchors —
            // stump caps land fractions of a second after the kill).
            const double base = (r.deathTime > 0.0) ? r.deathTime : r.enrollTime;
            while (!r.permanent && r.probeIdx >= 0 && r.probeIdx < (int)std::size(kProbeAt) &&
                   kProbeAt[r.probeIdx] >= 0.f && now - base >= kProbeAt[r.probeIdx]) {
                ++r.probeIdx;
                if (Probe(a, r)) break;
            }
            const bool probing = !r.permanent && r.probeIdx < (int)std::size(kProbeAt) &&
                                 kProbeAt[r.probeIdx] >= 0.f;

            // DEATH-CONFIRMED DISMEMBERMENT: the actor is now really dead, so ask DF for the cut
            // it could not make at hit time (main thread, public API, DF keeps full authority).
            if (!r.cutDone && r.cutAt > 0.0 && now >= r.cutAt) {
                r.cutDone = true;
                // 2026-08-02: a draugr kill produced a scheduled cut and ZERO DEATH-CUT lines,
                // although every exit inside RequestDeathDismember logs. The one silent path is
                // this `if (a)` — a handle that fails to resolve for a just-dying actor — so the
                // skip now says so instead of vanishing. Same rule as the heel receipt: a branch
                // that declines to act must leave a trace, or its silence gets read as "it ran
                // and found nothing".
                if (a) {
                    RequestDeathDismember(a, it->first);
                } else if (DgLog()) {
                    logger::info("DG: DEATH-CUT {:08X} SKIPPED — actor handle did not resolve at "
                                 "the scheduled cut ({:.2f}s after death); no cut was offered.",
                                 it->first, now - r.deathTime);
                }
            }

            // Death-grace expiry: hand an ordinary corpse back to PLANCK. dgGraceDeadS <= 0 =
            // NEVER (the 22:46 freeze proof: corpse restored at death+20s, gibbed by Lydia at
            // death+2m41s on the once-again-PLANCK-managed body -> the identical freeze
            // signature. Corpse mutilation happens arbitrarily late, so a timed hand-back can
            // never cover it — corpses now stay ignored for good by default; the knob remains
            // for experiments).
            const float graceS = ObjectHold::DgGraceDeadS();
            // 2026-08-02: was `!r.permanent`, which no dismember-touched actor could ever
            // satisfy (permanent is set at detection and never cleared) — the restore was dead
            // code for exactly the actors that needed it. Now keyed on the PLANCK-specific flag,
            // so clone props stay ignored forever and victims come back after the grace.
            if (!r.planckPerm && r.planckIgnored && r.deathTime > 0.0 && graceS > 0.f &&
                now - r.deathTime > graceS) {
                PlanckRestore(a, r, "grace expired");
                r.graceDone = true;   // corpse handed back to PLANCK — never re-ignore it
            }

            // CLONE BODY STRIP (v3, 2026-07-26 user: "Not disable, REMOVE — as long as there is
            // a Havok body attached to that head, it won't behave like a head"): remove EVERY
            // ragdoll body from the clone, COM included. v2 kept the COM body and the head still
            // rolled like a torso-sized capsule — the head's physics belongs to NGD's own
            // mechanism (as on SE, where no live-actor ragdoll is ever in the world). By now the
            // clone is PLANCK-ignored + PPB-excluded and NGD's spawn storm has settled.
            // v5 (user: "shrink to 0.01 so everything is inside the head"; v3's removal left
            // nothing for HIGGS to grab): collapse every capsule to a point, COM anchor kept as
            // a head-sized graspable ball. RE-APPLIED ~1s x6 — NGD re-runs "Set Scales and
            // Collisions" on the clone periodically, which re-inflates a one-shot shrink (the
            // likely reason the earlier single-pass looked like it did nothing).
            // 2026-07-27: the SHAPE collapse is gone (it mutated SHARED shapes and stripped live
            // NPCs of collision — see the contamination note). Shapes are now handled entirely by
            // the head-only NIF. What remains is per-actor and safe: park every body ON the
            // anchor and cut the constraints, so nothing orbits or drags the head.
            // ⚠ 2026-07-28: re-assert FOREVER, not 6 passes. NGD's SetScalesAndCollisions runs on
            // a periodic timer for the head's whole life and re-writes motion/collision state —
            // a park that stops re-asserting is undone within seconds of its last pass (the
            // "grab still does nothing" hunt). 1 Hz for the first 6 s, then every 2 s.
            // GRAB FIX (Report 09 §6): the ONE thing that was never tried — make the anchor
            // reachable by HIGGS's hand. Independent of the (now-off) park machinery; re-asserted
            // a few times because NGD re-stamps collision state after Step02.
            if (r.isClone && r.stripAt > 0.0 && now >= r.stripAt &&
                ObjectHold::DgHeadGrabFixOn() && r.grabFixTries < 6) {
                ++r.grabFixTries;
                if (a && NpcFinger::FixHeadGrabFilter(a, "NPC COM [COM ]") == 1)
                    r.grabFixTries = 6;      // took effect — stop re-asserting
                if (!ObjectHold::DgHeadParkOn()) r.stripAt = now + 1.0;
            }
            if (r.isClone && r.stripAt > 0.0 && now >= r.stripAt && ObjectHold::DgHeadParkOn()) {
                ++r.stripCount;
                r.stripAt = now + (r.stripCount < 6 ? 1.0 : 2.0);
                if (a) {
                    NpcFinger::ParkBodiesAtAnchor(a, "NPC COM [COM ]", r.stripCount);
                    // NGD replicates head gear at Step02 (~1-3s after birth) — keep stripping
                    // through the early passes so late-arriving wigs are removed too.
                    if (r.stripCount <= 8 && ObjectHold::DgHeadStripHairOn())
                        StripHeadHair(a, "park");
                }
            }

            // Drop settled non-permanent recs (probes finished, no ignore held). Death recs
            // survive until the ACTOR is gone — erasing a graceDone corpse would let the
            // health<=0 path re-enroll and re-ignore it forever (the cycle guard).
            if (!r.permanent && !r.planckIgnored && !probing && r.firstProbeDone &&
                (r.deathTime == 0.0 || !a))
                it = g_recs.erase(it);
            else
                ++it;
        }
    }

    void ClearOnLoad()
    {
        std::scoped_lock lk(g_lk);
        // Actor pointers/handles do not survive a load; PLANCK's own ignore set keeps stale
        // raw pointers (harmless — pointer-compares only). Post-load, deaths re-enroll fresh.
        g_recs.clear();
        g_lastHit.clear();
        g_lastSweep = 0.0;
    }
}
