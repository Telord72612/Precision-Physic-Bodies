#include "PCH.h"
#include "Diag.h"
#include "Tuning.h"       // ObjectHold::LodSpikeEnabled
#include "Interop.h"      // Interop::GetHiggs (frame boundary)
#include "PPBHook.h"      // ArmIK::GetPoseConformStats / ResetPoseConformStats — the conform cost line
#include "HiggsInterface.h"

#include "RE/H/hkpWorld.h"
#include "RE/H/hkpContactListener.h"
#include "RE/H/hkpContactPointEvent.h"
#include "RE/H/hkpCollisionEvent.h"
#include "RE/H/hkpCollidable.h"
#include "RE/H/hkpListShape.h"
#include "RE/H/hkpCapsuleShape.h"
#include "RE/H/hkpCollisionInput.h"
#include "RE/H/hkpEntity.h"
#include "RE/H/hkpRigidBody.h"
#include "RE/B/bhkWorld.h"
#include "RE/B/bhkCollisionFilter.h"
#include "RE/B/bhkCollisionObject.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <xmmintrin.h>
#include <Windows.h>

namespace logger = SKSE::log;

// ============================================================================
//  RE:: types only — NO Havok SDK in this TU (same discipline as CapFix/Probe).
// ============================================================================
namespace {

    constexpr float kHavokToSkyrim = 1.0f / 0.0142875f;

    // ── module-name resolver (copied from Hooks.cpp; small, TU-local) ──
    std::string ResolveModuleName(std::uintptr_t addr)
    {
        if (!addr) return "<null>";
        HMODULE hmod = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
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

    // biped part number -> readable name (Report 11 §5.2, from the actual bake).
    // Ambiguity noted: part 2 is COM *and* Spine0 (aliased); we print the raw number too.
    const char* PartName(int p) {
        switch (p) {
        case 0:  return "Neck?";
        case 1:  return "Head";
        case 2:  return "COM/Spn0";
        case 3:  return "Spn1";
        case 4:  return "Spn2";
        case 5:  return "LUpArm";  case 6:  return "LForeA";  case 7:  return "LHand";
        case 8:  return "LThigh";  case 9:  return "LCalf";   case 10: return "LFoot";
        case 11: return "RUpArm";  case 12: return "RForeA";  case 13: return "RHand";
        case 14: return "RThigh";  case 15: return "RCalf";   case 16: return "RFoot";
        case 17: return "Tail";
        default: return "?";
        }
    }
    inline int   FilterPart(std::uint32_t f)  { return (int)((f >> 8) & 0x1F); }
    inline int   FilterLayer(std::uint32_t f) { return (int)(f & 0x7F); }
    inline bool  FilterNoColl(std::uint32_t f){ return (f & 0x4000u) != 0; }
    inline std::uint32_t FilterGroup(std::uint32_t f) { return f >> 16; }

    // ── node table (the 18 ragdoll bodies; same set Probe uses) ──
    constexpr const char* kNode18[18] = {
        "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Head [Head]",
        "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
        "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC COM [COM ]",
        "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]",
        "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]",
    };
    // slot table for the spike (CapFixSlot indexing; slot 8 = R Thigh)
    constexpr const char* kSlotNode[12] = {
        "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Head [Head]",
        "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
        "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC COM [COM ]",
    };
    constexpr const char* kSlotName[12] = { "hand", "forearm", "upperarm", "head",
        "spine0", "spine1", "spine2", "neck", "thighR", "calfR", "footR", "com" };

    // Resolve a named node's live hkpRigidBody / collidable / shape (RE:: chain, never allocates).
    struct BodyRes { RE::hkpRigidBody* rb{}; RE::hkpCollidable* col{}; const RE::hkpShape* shape{}; };
    BodyRes ResolveBody(RE::Actor* actor, const char* node) {
        BodyRes r{};
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return r;
        auto* obj = root->GetObjectByName(node);
        if (!obj) return r;
        auto* colObj = obj->collisionObject.get();
        if (!colObj) return r;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        r.rb  = body ? body->GetRigidBody() : nullptr;
        r.col = r.rb ? r.rb->GetCollidableRW() : nullptr;
        r.shape = r.col ? r.col->shape : nullptr;
        return r;
    }
    inline std::uint32_t FilterOf(RE::hkpRigidBody* rb) {
        if (!rb) return 0;
        auto* col = rb->GetCollidableRW();
        return col ? col->broadPhaseHandle.collisionFilterInfo.filter : 0u;
    }

    // ========================================================================
    //  ARM STATE
    // ========================================================================
    std::atomic<bool> g_armed{ false };

    // ========================================================================
    //  1) PHYSICS-STEP TIMER
    // ========================================================================
    constexpr int    kStepBuckets  = 400;      // 0.05 ms each -> 0..20 ms + overflow
    constexpr double kStepBucketMs = 0.05;
    std::atomic<std::uint64_t> g_stepHist[kStepBuckets + 1];
    std::atomic<std::uint64_t> g_stepCount{ 0 };
    std::atomic<std::uint64_t> g_stepSumMicros{ 0 };
    std::atomic<std::uint64_t> g_stepMaxMicros{ 0 };
    std::atomic<std::uint32_t> g_stepsThisFrame{ 0 };
    std::atomic<std::uint64_t> g_frameCount{ 0 };
    std::atomic<std::uint64_t> g_stepsPerFrame[6];   // 0,1,2,3,4,5+
    bool g_frameCbRegistered = false;

    // ========================================================================
    //  2) CONTACT COUNTER — fixed-table, lock-free, read-only
    // ========================================================================
    struct PairSlot {
        std::atomic<std::uint64_t> key{ 0 };        // 0 = free
        const void*   bodyA{ nullptr };
        const void*   bodyB{ nullptr };
        std::uint32_t filterA{ 0 };
        std::uint32_t filterB{ 0 };
        std::atomic<std::int32_t>  newPoints{ 0 };  // cumulative NEW points this manifold instance (upper bound)
        std::atomic<std::int32_t>  exact{ 0 };      // last full-manifold walk count (EXACT)
        std::atomic<std::int32_t>  walk{ 0 };
        std::atomic<std::int32_t>  maxLive{ 0 };    // session max instantaneous points (UB or exact)
        std::atomic<std::int32_t>  maxChildPairs{ 0 };
        std::atomic<bool>          maxFromExact{ false };
        std::atomic<std::uint64_t> childBits[9];    // 24*24 = 576 bits
    };
    constexpr int kNumSlots = 1024;
    constexpr int kProbe    = 8;
    PairSlot g_pairs[kNumSlots];

    std::atomic<std::uint64_t> g_contactPointEvents{ 0 };
    std::atomic<std::uint64_t> g_newPointEvents{ 0 };
    std::atomic<std::uint64_t> g_fullManifoldWalks{ 0 };
    std::atomic<std::uint64_t> g_collisionsAdded{ 0 };
    std::atomic<std::uint64_t> g_collisionsRemoved{ 0 };
    std::atomic<std::uint64_t> g_droppedEvents{ 0 };
    std::atomic<std::uint64_t> g_childKeyOverflow{ 0 };
    std::atomic<std::uint64_t> g_typeHist[5];
    std::atomic<std::uint32_t> g_threadIds[4];
    std::atomic<int>           g_threadCount{ 0 };
    std::atomic<int>           g_pairsSeen{ 0 };

    inline std::uint64_t MixKey(const void* a, const void* b) {
        std::uint64_t x = reinterpret_cast<std::uintptr_t>(a);
        std::uint64_t y = reinterpret_cast<std::uintptr_t>(b);
        std::uint64_t h = x * 0x9E3779B97F4A7C15ull ^ (y + 0x632BE59BD9B4E019ull + (x << 6) + (x >> 2));
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
        return h ? h : 1;   // 0 is the "free" sentinel
    }

    PairSlot* FindSlot(const void* a, const void* b, std::uint32_t fa, std::uint32_t fb) {
        const std::uint64_t myKey = MixKey(a, b);
        const std::size_t   h = (std::size_t)(myKey & (kNumSlots - 1));
        for (int i = 0; i < kProbe; ++i) {
            PairSlot& s = g_pairs[(h + i) & (kNumSlots - 1)];
            std::uint64_t cur = s.key.load(std::memory_order_acquire);
            if (cur == myKey && s.bodyA == a && s.bodyB == b) return &s;
            if (cur == 0) {
                std::uint64_t expected = 0;
                if (s.key.compare_exchange_strong(expected, myKey, std::memory_order_acq_rel)) {
                    s.bodyA = a; s.bodyB = b; s.filterA = fa; s.filterB = fb;
                    g_pairsSeen.fetch_add(1, std::memory_order_relaxed);
                    return &s;
                }
                cur = s.key.load(std::memory_order_acquire);
                if (cur == myKey && s.bodyA == a && s.bodyB == b) return &s;
            }
        }
        return nullptr;   // table saturated on this probe run
    }

    inline void BumpMax(PairSlot& s, std::int32_t v, bool fromExact) {
        std::int32_t prev = s.maxLive.load(std::memory_order_relaxed);
        while (v > prev && !s.maxLive.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {}
        if (v >= s.maxLive.load(std::memory_order_relaxed)) s.maxFromExact.store(fromExact, std::memory_order_relaxed);
    }

    inline void SetChildBit(PairSlot& s, std::uint32_t ci, std::uint32_t cj) {
        std::uint32_t a = (ci == 0xFFFFFFFFu) ? 0u : ci;
        std::uint32_t b = (cj == 0xFFFFFFFFu) ? 0u : cj;
        if (a >= 24 || b >= 24) { g_childKeyOverflow.fetch_add(1, std::memory_order_relaxed); if (a >= 24) a = 23; if (b >= 24) b = 23; }
        const std::uint32_t bit  = a * 24u + b;         // 0..575
        const std::uint64_t mask = 1ull << (bit & 63u);
        const std::uint64_t old  = s.childBits[bit >> 6].fetch_or(mask, std::memory_order_relaxed);
        if (!(old & mask)) {
            std::int32_t cnt = 0;
            for (int w = 0; w < 9; ++w) cnt += (std::int32_t)std::popcount(s.childBits[w].load(std::memory_order_relaxed));
            std::int32_t prev = s.maxChildPairs.load(std::memory_order_relaxed);
            while (cnt > prev && !s.maxChildPairs.compare_exchange_weak(prev, cnt, std::memory_order_relaxed)) {}
        }
    }

    inline void RecordThread() {
        const std::uint32_t tid = ::GetCurrentThreadId();
        const int n = g_threadCount.load(std::memory_order_relaxed);
        for (int i = 0; i < n && i < 4; ++i)
            if (g_threadIds[i].load(std::memory_order_relaxed) == tid) return;
        const int idx = g_threadCount.fetch_add(1, std::memory_order_relaxed);
        if (idx < 4) g_threadIds[idx].store(tid, std::memory_order_relaxed);
    }

    // Contact decimation counters (2026-07-10) — written from collision threads, read by the
    // `perf` report + PerfSys's main-thread change logger.
    std::atomic<std::uint64_t> g_decimSeen{ 0 }, g_decimKilled{ 0 };

    // The listener. Counting is read-only (g_armed-gated); the decimation branch (2026-07-10) is
    // the ONE write path: it sets kIsDisabled on new contact points when perfContactKeep is live.
    struct PpbContactCounter : RE::hkpContactListener {
        RE::hkpWorld* world = nullptr;

        void ContactPointCallback(const RE::hkpContactPointEvent& e) override {
            // ── PERF CONTACT DECIMATION (perfContactKeep / perfPreset 50|80|90 → keep 1-in-2|5|10).
            // Runs UNGATED (g_armed only gates the counters below). ⚠ collision threads: atomics
            // only, NO logging (proven CTD). Policy: decide once per NEW manifold point (kIsNew);
            // the pair must involve a bit-15 NPC ragdoll body and NO fixed body — world/ground
            // contacts stay full so ragdolled NPCs never sink through floors. Kill = kIsDisabled
            // on the point's properties (the solver skips it for the point's manifold lifetime).
            if (const int N = ObjectHold::PerfContactKeepN(); N > 1) {
                auto* props = e.contactPointProperties;
                if (props && props->flags.any(RE::hkContactPointMaterial::Flag::kIsNew) &&
                    !props->flags.any(RE::hkContactPointMaterial::Flag::kIsDisabled)) {
                    RE::hkpRigidBody* ba = e.bodies[0];
                    RE::hkpRigidBody* bb = e.bodies[1];
                    if (ba && bb &&
                        ba->motion.type.get() != RE::hkpMotion::MotionType::kFixed &&
                        bb->motion.type.get() != RE::hkpMotion::MotionType::kFixed &&
                        ((FilterOf(ba) | FilterOf(bb)) & 0x8000u)) {
                        const std::uint64_t n = g_decimSeen.fetch_add(1, std::memory_order_relaxed);
                        if (n % (std::uint64_t)N) {          // keep every N-th, disable the rest
                            props->flags.set(RE::hkContactPointMaterial::Flag::kIsDisabled);
                            g_decimKilled.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }

            if (!g_armed.load(std::memory_order_relaxed)) return;
            RecordThread();
            g_contactPointEvents.fetch_add(1, std::memory_order_relaxed);
            { const int t = (int)e.type; if (t >= 0 && t < 5) g_typeHist[t].fetch_add(1, std::memory_order_relaxed); }

            const void* a = e.bodies[0];
            const void* b = e.bodies[1];
            if (!a || !b) return;
            std::uint32_t fa = FilterOf(e.bodies[0]);
            std::uint32_t fb = FilterOf(e.bodies[1]);
            auto* ka = e.GetShapeKeys(0);
            auto* kb = e.GetShapeKeys(1);
            std::uint32_t ci = ka ? ka[0] : 0xFFFFFFFFu;   // leaf-to-root: keys[0] IS the list child index
            std::uint32_t cj = kb ? kb[0] : 0xFFFFFFFFu;
            if (a > b) { std::swap(a, b); std::swap(fa, fb); std::swap(ci, cj); }

            PairSlot* s = FindSlot(a, b, fa, fb);
            if (!s) { g_droppedEvents.fetch_add(1, std::memory_order_relaxed); return; }
            SetChildBit(*s, ci, cj);

            if (e.firingCallbacksForFullManifold) {
                // EXACT path: the manager delay hit zero, so the whole manifold is walked point-by-point.
                if (e.firstCallbackForFullManifold) s->walk.store(0, std::memory_order_relaxed);
                const std::int32_t w = s->walk.fetch_add(1, std::memory_order_relaxed) + 1;
                if (e.lastCallbackForFullManifold) {
                    s->exact.store(w, std::memory_order_relaxed);
                    g_fullManifoldWalks.fetch_add(1, std::memory_order_relaxed);
                    BumpMax(*s, w, /*fromExact*/true);
                }
            } else {
                // NEW-POINT path (delay throttled): cumulative -> UPPER BOUND on the live manifold
                // (over-counts by removed points -> false RED only, never false GREEN).
                const std::int32_t np = s->newPoints.fetch_add(1, std::memory_order_relaxed) + 1;
                g_newPointEvents.fetch_add(1, std::memory_order_relaxed);
                BumpMax(*s, np, /*fromExact*/false);
            }
        }

        void CollisionAddedCallback(const RE::hkpCollisionEvent& e) override {
            if (!g_armed.load(std::memory_order_relaxed)) return;
            g_collisionsAdded.fetch_add(1, std::memory_order_relaxed);
            const void* a = e.bodies[0]; const void* b = e.bodies[1];
            if (!a || !b) return;
            std::uint32_t fa = FilterOf(e.bodies[0]);
            std::uint32_t fb = FilterOf(e.bodies[1]);
            if (a > b) { std::swap(a, b); std::swap(fa, fb); }
            PairSlot* s = FindSlot(a, b, fa, fb);
            if (!s) { g_droppedEvents.fetch_add(1, std::memory_order_relaxed); return; }
            // new manifold instance -> reset the live counters (keep the session maxes)
            s->newPoints.store(0, std::memory_order_relaxed);
            s->walk.store(0, std::memory_order_relaxed);
            for (int w = 0; w < 9; ++w) s->childBits[w].store(0, std::memory_order_relaxed);
        }

        void CollisionRemovedCallback(const RE::hkpCollisionEvent& e) override {
            if (!g_armed.load(std::memory_order_relaxed)) return;
            g_collisionsRemoved.fetch_add(1, std::memory_order_relaxed);
            const void* a = e.bodies[0]; const void* b = e.bodies[1];
            if (!a || !b) return;
            if (a > b) std::swap(a, b);
            PairSlot* s = FindSlot(a, b, 0, 0);
            if (!s) return;
            s->newPoints.store(0, std::memory_order_relaxed);
            for (int w = 0; w < 9; ++w) s->childBits[w].store(0, std::memory_order_relaxed);
        }
    };
    PpbContactCounter g_ctc;
    RE::hkpWorld*     s_ctcWorld = nullptr;

    using AddContactListenerFn = void* (*)(RE::hkpWorld*, RE::hkpContactListener*);

    void EnsureContactListener(RE::Actor* actor) {
        if (!actor) return;
        auto* cell = actor->GetParentCell();
        auto* bhk  = cell ? cell->GetbhkWorld() : nullptr;
        if (!bhk) return;
        RE::hkpWorld* world = bhk->GetWorld1();
        if (!world || world == s_ctcWorld) return;    // idempotent per world; never re-register per frame

        // STRUCTURAL self-check (pure reads) BEFORE we call the reloc. If the array does not look like a
        // listener list, ABORT — do not call an offset we cannot statically verify against garbage state.
        auto& L = world->contactListeners;
        const std::int32_t before = L.size();
        if (before < 0 || before > 64) {
            logger::error("Diag CTC ABORT: contactListeners.size()={} implausible — NOT registering (world 0x{:X})",
                          before, (std::uintptr_t)world);
            s_ctcWorld = world;   // don't hammer a suspect world every frame
            return;
        }
        for (std::int32_t i = 0; i < before; ++i) {
            auto* lp = L[i];
            const std::uintptr_t vt = lp ? *reinterpret_cast<std::uintptr_t*>(lp) : 0;
            logger::info("Diag CTC listener[{}] = 0x{:X} vtable-module=[{}]",
                         i, (std::uintptr_t)lp, lp ? ResolveModuleName(vt) : "<null>");
        }
        // ALREADY-REGISTERED guard (2026-07-10): hkpWorld pointers get RECYCLED across interior/
        // exterior transitions, so s_ctcWorld tracking alone re-added us to a world we never left
        // (the 22:59:58 "contactListeners 5 -> 6" line) — doubled callbacks = doubled perf counters
        // AND double decimation rolls (50% behaved like 75%). Identity-scan the array; if we are
        // already in it, ADOPT the world instead of re-adding.
        for (std::int32_t i = 0; i < before; ++i) {
            if (L[i] == &g_ctc) {
                s_ctcWorld  = world;
                g_ctc.world = world;
                logger::info("Diag CTC: already registered on hkpWorld 0x{:X} (recycled pointer) — adopted, not re-added.",
                             (std::uintptr_t)world);
                return;
            }
        }
        {
            RE::BSWriteLockGuard lk(bhk->worldLock);       // PLANCK takes the same lock here
            static REL::Relocation<AddContactListenerFn> addContactListener{ REL::Offset(0xAB5580) };
            addContactListener(world, &g_ctc);             // THE ONE new reloc
        }
        const std::int32_t after = L.size();               // FUNCTIONAL verification of 0xAB5580
        if (after != before + 1 || L[after - 1] != &g_ctc) {
            logger::error("Diag CTC ABORT: addContactListener did NOT append us (before={} after={}) — "
                          "counters may be unreliable on this world", before, after);
            s_ctcWorld = world;
            return;
        }
        s_ctcWorld  = world;
        g_ctc.world = world;
        logger::info("Diag CTC armed on hkpWorld 0x{:X} (contactListeners {} -> {}). read-only; PLANCK reorders "
                     "us each frame (harmless).", (std::uintptr_t)world, before, after);
    }

    // ========================================================================
    //  3) COLLISION-FILTER DUMP (world route only — no Address Library dep)
    // ========================================================================
    void DumpCollisionFilter(RE::hkpWorld* world) {
        if (!world) { logger::info("Diag FILTER: no world"); return; }
        auto* f = static_cast<RE::bhkCollisionFilter*>(world->collisionFilter);   // hkpWorld+0xD0
        if (!f) { logger::info("Diag FILTER: world->collisionFilter is null"); return; }
        logger::info("---- Diag COLLISION-FILTER dump (bhkCollisionFilter 0x{:X}, world route) ----", (std::uintptr_t)f);
        for (int i = 0; i < 24; ++i)
            logger::info("Diag FILTER bipedBitfields[{}] = 0x{:08X}", i, f->bipedBitfields[i]);
        // collision tolerance: hkpWorld::collisionInput (0xC8) -> hkpCollisionInput::tolerance (0x10)
        if (world->collisionInput) {
            const float tol = reinterpret_cast<RE::hkpCollisionInput*>(world->collisionInput)->tolerance;
            if (tol > 0.f && tol < 10.f)
                logger::info("Diag FILTER: hkpCollisionInput::tolerance = {:.5f} havok m (~{:.2f} game u)", tol, tol * kHavokToSkyrim);
            else
                logger::info("Diag FILTER: hkpCollisionInput::tolerance = {:.5f} — OUT OF PLAUSIBLE RANGE, treat as unknown", tol);
        } else {
            logger::info("Diag FILTER: world->collisionInput null — tolerance unavailable");
        }
    }

    // ========================================================================
    //  4) RUNTIME CAPSULE CENSUS
    // ========================================================================
    void DumpCensus(RE::Actor* actor) {
        if (!actor) { logger::info("Diag CENSUS: no actor selected — census skipped"); return; }
        const std::uint32_t id = actor->GetFormID();
        auto* baseF = actor->GetActorBase();
        const char* nm = (baseF && baseF->GetFullName()) ? baseF->GetFullName() : "<unnamed>";
        logger::info("================ Diag CENSUS {:08X} \"{}\" ================", id, nm);

        int bodies = 0, lists = 0, singleCaps = 0, listChildCaps = 0, totalCaps = 0;
        for (const char* node : kNode18) {
            BodyRes r = ResolveBody(actor, node);
            if (!r.rb || !r.shape) { logger::info("Diag CENSUS {:08X} '{}': no rigid body/shape", id, node); continue; }
            ++bodies;
            const int   mt   = (int)r.rb->motion.type.get();
            const std::uint32_t filt = r.col ? r.col->broadPhaseHandle.collisionFilterInfo.filter : 0;
            const std::uint16_t cpcd = r.rb->contactPointCallbackDelay;              // hkpEntity+0xFE
            const std::uint8_t  nsk  = r.rb->numShapeKeysInContactPointProperties;   // hkpEntity+0x139
            const auto& bvd = r.col->boundingVolumeData;
            const int stype = (int)r.shape->type;

            if (r.shape->type == RE::hkpShapeType::kList) {
                ++lists;
                auto* list = static_cast<const RE::hkpListShape*>(r.shape);
                const std::int32_t n = list->childInfo.size();
                logger::info("Diag CENSUS {:08X} '{}': LIST 0x{:X} childInfo={} getNumChildShapes={} numDisabled={} "
                             "enabled[0..1]=0x{:08X},0x{:08X} filter=0x{:08X}(part {} {} grp {} layer {}{}) motion={} "
                             "cpCallbackDelay={} numShapeKeys={} bvd.num={} bvd.cap={}",
                             id, node, (std::uintptr_t)list, n, list->GetNumChildShapes(), list->numDisabledChildren,
                             list->enabledChildren[0], list->enabledChildren[1], filt,
                             FilterPart(filt), PartName(FilterPart(filt)), FilterGroup(filt), FilterLayer(filt),
                             FilterNoColl(filt) ? " NOCOLL" : "", mt, cpcd, nsk,
                             bvd.numChildShapeAabbs, bvd.capacityChildShapeAabbs);
                for (std::int32_t i = 0; i < n && i < 32; ++i) {
                    const RE::hkpShape* ch = list->childInfo[i].shape;
                    const std::uint32_t cfi = list->childInfo[i].collisionFilterInfo.filter;
                    if (ch && ch->type == RE::hkpShapeType::kCapsule) {
                        auto* cap = static_cast<const RE::hkpCapsuleShape*>(ch);
                        alignas(16) float va[4], vb[4];
                        _mm_store_ps(va, cap->vertexA.quad);
                        _mm_store_ps(vb, cap->vertexB.quad);
                        ++listChildCaps; ++totalCaps;
                        logger::info("Diag CENSUS {:08X}   '{}' child {}: CAPSULE A=[{:.2f} {:.2f} {:.2f}] "
                                     "B=[{:.2f} {:.2f} {:.2f}] r={:.2f}u childFilter=0x{:08X}",
                                     id, node, i,
                                     va[0]*kHavokToSkyrim, va[1]*kHavokToSkyrim, va[2]*kHavokToSkyrim,
                                     vb[0]*kHavokToSkyrim, vb[1]*kHavokToSkyrim, vb[2]*kHavokToSkyrim,
                                     cap->radius*kHavokToSkyrim, cfi);
                    } else {
                        logger::info("Diag CENSUS {:08X}   '{}' child {}: type {} (not a capsule) childFilter=0x{:08X}",
                                     id, node, i, ch ? (int)ch->type : -1, cfi);
                    }
                }
            } else if (r.shape->type == RE::hkpShapeType::kCapsule) {
                ++singleCaps; ++totalCaps;
                auto* cap = static_cast<const RE::hkpCapsuleShape*>(r.shape);
                alignas(16) float va[4], vb[4];
                _mm_store_ps(va, cap->vertexA.quad);
                _mm_store_ps(vb, cap->vertexB.quad);
                logger::info("Diag CENSUS {:08X} '{}': CAPSULE A=[{:.2f} {:.2f} {:.2f}] B=[{:.2f} {:.2f} {:.2f}] r={:.2f}u "
                             "filter=0x{:08X}(part {} {} grp {} layer {}{}) motion={} cpCallbackDelay={} numShapeKeys={}",
                             id, node,
                             va[0]*kHavokToSkyrim, va[1]*kHavokToSkyrim, va[2]*kHavokToSkyrim,
                             vb[0]*kHavokToSkyrim, vb[1]*kHavokToSkyrim, vb[2]*kHavokToSkyrim, cap->radius*kHavokToSkyrim,
                             filt, FilterPart(filt), PartName(FilterPart(filt)), FilterGroup(filt), FilterLayer(filt),
                             FilterNoColl(filt) ? " NOCOLL" : "", mt, cpcd, nsk);
            } else {
                logger::info("Diag CENSUS {:08X} '{}': shape type {} (not capsule/list) filter=0x{:08X} motion={} "
                             "cpCallbackDelay={} numShapeKeys={}", id, node, stype, filt, mt, cpcd, nsk);
            }
        }
        logger::info("Diag CENSUS {:08X} TOTALS: {} ragdoll bodies, {} lists, {} single capsules, {} list-child capsules, "
                     "{} total capsules on bodies. (Static NIF says 18 bodies / 11 lists / 7 single / 37 children.)",
                     id, bodies, lists, singleCaps, listChildCaps, totalCaps);
        logger::info("Diag CENSUS {:08X} NOTE: the cpCallbackDelay above settles R1 — if it is 65535 the contact "
                     "callback fires only on NEW points, so maxPointsInOnePair is an UPPER BOUND (safe direction). "
                     "Run `perf` on a SECOND NPC and compare list pointers to check per-actor shape ownership.", id);
        logger::info("================ Diag CENSUS {:08X} end ================", id);
    }

    // ========================================================================
    //  5) GATED disableChild SPIKE (deferred; world-locked; leaf bit-flips)
    // ========================================================================
    struct SpikeReq {
        std::uint32_t formId = 0;
        int           slot   = -1;
        std::uint32_t mask   = 0;
        bool          pending = false;
    };
    std::mutex g_spikeMx;
    SpikeReq   g_spike;

    // Byte-faithful reimplementation of hkpListShape::disableChild (VR 0xA9C3F0), including the cmp/je
    // idempotence guard. No calls, no allocation, no AABB touch. x64 offsets (enabledChildren@0x70,
    // numDisabledChildren@0x42) via CommonLibVR members — NOT the report's x86 disassembly offsets.
    bool DisableChildInline(RE::hkpListShape* list, std::uint32_t idx) {
        if (idx >= (std::uint32_t)RE::hkpListShape::kMaxDisabledChildren) return false;
        if (idx >= (std::uint32_t)list->childInfo.size())                 return false;
        const std::uint32_t word = idx >> 5, bit = 1u << (idx & 31);
        const std::uint32_t old = list->enabledChildren[word];
        const std::uint32_t nw  = old & ~bit;
        if (nw == old) return false;                    // idempotence guard
        list->enabledChildren[word] = nw;
        ++list->numDisabledChildren;
        return true;
    }
    bool EnableChildInline(RE::hkpListShape* list, RE::hkpCollidable* col, std::uint32_t idx) {
        if (idx >= (std::uint32_t)RE::hkpListShape::kMaxDisabledChildren) return false;
        if (idx >= (std::uint32_t)list->childInfo.size())                 return false;
        // Re-enabling is MORE dangerous than disabling: never enable past the AABB cache capacity that was
        // sized from getNumChildShapes() at world-add time (Report §4.5 rule 2).
        if (col->boundingVolumeData.capacityChildShapeAabbs < list->childInfo.size()) {
            logger::error("capdis: REFUSING enable child {} — capacityChildShapeAabbs {} < childInfo {}",
                          idx, col->boundingVolumeData.capacityChildShapeAabbs, (int)list->childInfo.size());
            return false;
        }
        const std::uint32_t word = idx >> 5, bit = 1u << (idx & 31);
        const std::uint32_t old = list->enabledChildren[word];
        const std::uint32_t nw  = old | bit;
        if (nw == old) return false;
        list->enabledChildren[word] = nw;
        --list->numDisabledChildren;
        return true;
    }
    inline void InvalidateBvd(RE::hkpCollidable* col) {   // min[0]=1, max[0]=0 -> consumer short-circuits to recalc
        col->boundingVolumeData.min[0] = 1;
        col->boundingVolumeData.max[0] = 0;
    }
    bool AuditList(const RE::hkpListShape* list) {        // numDisabledChildren must equal cleared-bit count
        const int n = list->childInfo.size();
        int cleared = 0;
        for (int i = 0; i < n && i < 256; ++i)
            if (!(list->enabledChildren[i >> 5] & (1u << (i & 31)))) ++cleared;
        return cleared == (int)list->numDisabledChildren;
    }

    void ApplyPendingSpike(RE::Actor* actor) {
        if (!ObjectHold::LodSpikeEnabled()) return;      // gate 1: the tuning knob
        std::uint32_t formId; int slot; std::uint32_t mask;
        {
            std::lock_guard<std::mutex> g(g_spikeMx);
            if (!g_spike.pending) return;                // gate 2: a queued console request
            if (!actor || actor->GetFormID() != g_spike.formId) return;
            formId = g_spike.formId; slot = g_spike.slot; mask = g_spike.mask;
            g_spike.pending = false;                     // one-shot (idempotent flips; re-type capdis to redo)
        }
        if (slot < 0 || slot > 11) return;
        BodyRes r = ResolveBody(actor, kSlotNode[slot]);
        if (!r.rb || !r.col || !r.shape || r.shape->type != RE::hkpShapeType::kList) {
            logger::error("capdis APPLY {:08X} slot {} ({}): body is not a bhkListShape (type {}) — spike aborted",
                          formId, slot, kSlotName[slot], r.shape ? (int)r.shape->type : -1);
            return;
        }
        auto* list = const_cast<RE::hkpListShape*>(static_cast<const RE::hkpListShape*>(r.shape));
        auto* col  = r.col;
        auto* cell = actor->GetParentCell();
        auto* bhk  = cell ? cell->GetbhkWorld() : nullptr;
        if (!bhk) { logger::error("capdis APPLY {:08X}: no bhkWorld — aborted", formId); return; }

        RE::BSWriteLockGuard lk(bhk->worldLock);
        const std::int32_t  n        = list->childInfo.size();
        const std::uint32_t enBefore = list->enabledChildren[0];
        const std::uint16_t ndBefore = list->numDisabledChildren;
        logger::info("capdis APPLY {:08X} slot {} ({}) mask=0x{:X}: BEFORE childInfo={} getNumChildShapes={} "
                     "numDisabled={} enabled[0]=0x{:08X} bvd.num={} bvd.cap={}",
                     formId, slot, kSlotName[slot], mask, n, list->GetNumChildShapes(), ndBefore, enBefore,
                     col->boundingVolumeData.numChildShapeAabbs, col->boundingVolumeData.capacityChildShapeAabbs);

        int changed = 0;
        for (std::int32_t i = 0; i < n && i < 256; ++i) {
            const bool wantDisabled = ((mask >> i) & 1u) != 0;    // mask = desired DISABLED set; 0 -> re-enable all
            if (wantDisabled) { if (DisableChildInline(list, (std::uint32_t)i)) ++changed; }
            else              { if (EnableChildInline(list, col, (std::uint32_t)i)) ++changed; }
        }
        if (!AuditList(list)) {   // only enabledChildren[0] can change for n<=32 -> a full restore
            list->enabledChildren[0]  = enBefore;
            list->numDisabledChildren = ndBefore;
            logger::error("capdis APPLY {:08X}: AUDIT FAILED (numDisabledChildren != cleared bits) — snapshot restored, spike aborted", formId);
            return;
        }
        InvalidateBvd(col);
        logger::info("capdis APPLY {:08X} slot {} ({}) mask=0x{:X}: AFTER getNumChildShapes={} numDisabled={} "
                     "enabled[0]=0x{:08X} (changed {} bits, bvd invalidated). With `perf` armed, no contact "
                     "event on this body should now carry a disabled shapeKey.",
                     formId, slot, kSlotName[slot], mask, list->GetNumChildShapes(), list->numDisabledChildren,
                     list->enabledChildren[0], changed);
    }

    // ========================================================================
    //  REPORT
    // ========================================================================
    void ResetCounters() {
        for (auto& s : g_pairs) {
            s.key.store(0, std::memory_order_relaxed);
            s.bodyA = nullptr; s.bodyB = nullptr; s.filterA = 0; s.filterB = 0;
            s.newPoints.store(0); s.exact.store(0); s.walk.store(0);
            s.maxLive.store(0); s.maxChildPairs.store(0); s.maxFromExact.store(false);
            for (int w = 0; w < 9; ++w) s.childBits[w].store(0);
        }
        g_contactPointEvents = 0; g_newPointEvents = 0; g_fullManifoldWalks = 0;
        g_collisionsAdded = 0; g_collisionsRemoved = 0; g_droppedEvents = 0; g_childKeyOverflow = 0;
        g_pairsSeen = 0;
        g_decimSeen = 0; g_decimKilled = 0;
        for (int i = 0; i < 5; ++i) g_typeHist[i] = 0;
        for (int i = 0; i <= kStepBuckets; ++i) g_stepHist[i] = 0;
        g_stepCount = 0; g_stepSumMicros = 0; g_stepMaxMicros = 0; g_stepsThisFrame = 0; g_frameCount = 0;
        for (int i = 0; i < 6; ++i) g_stepsPerFrame[i] = 0;
        for (int i = 0; i < 4; ++i) g_threadIds[i] = 0;
        g_threadCount = 0;
        ArmIK::ResetPoseConformStats();   // the conform cost line measures the armed window only
    }

    std::string PairLabel(std::uint32_t fa, std::uint32_t fb) {
        char buf[96];
        const std::uint32_t ga = FilterGroup(fa), gb = FilterGroup(fb);
        std::snprintf(buf, sizeof(buf), "%s(p%d,g%u) x %s(p%d,g%u) [%s]",
                      PartName(FilterPart(fa)), FilterPart(fa), ga,
                      PartName(FilterPart(fb)), FilterPart(fb), gb,
                      (ga == gb ? "intra" : "cross"));
        return buf;
    }

    void DumpReport(char* out, std::size_t outSz) {
        // ---- step timing ----
        const std::uint64_t stepN = g_stepCount.load();
        const double meanStep = stepN ? (double)g_stepSumMicros.load() / (double)stepN / 1000.0 : 0.0;
        const double maxStep  = (double)g_stepMaxMicros.load() / 1000.0;
        auto pct = [&](double p) -> double {
            if (!stepN) return 0.0;
            const std::uint64_t target = (std::uint64_t)((double)stepN * p);
            std::uint64_t acc = 0;
            for (int i = 0; i <= kStepBuckets; ++i) {
                acc += g_stepHist[i].load();
                if (acc >= target) return (i >= kStepBuckets) ? (kStepBuckets * kStepBucketMs) : ((i + 1) * kStepBucketMs);
            }
            return maxStep;
        };
        const double p95 = pct(0.95), p99 = pct(0.99);
        logger::info("==================== PPB perf REPORT ====================");
        if (stepN) {
            logger::info("Diag STEP (0xDFB722 hkpWorld::stepDeltaTime): n={} mean={:.4f}ms p95={:.4f}ms p99={:.4f}ms max={:.4f}ms",
                         stepN, meanStep, p95, p99, maxStep);
        } else {
            logger::info("Diag STEP: NO SAMPLES — physics-step hook not installed (see the InstallPhysicsStepHook log line) "
                         "OR no physics ran while armed. Contact gate below is independent of this.");
        }
        logger::info("Diag STEPS/FRAME: [0]={} [1]={} [2]={} [3]={} [4]={} [5+]={} (frames={}) — any [2+] attributable to us "
                     "is the 70 Hz cliff; investigate.",
                     g_stepsPerFrame[0].load(), g_stepsPerFrame[1].load(), g_stepsPerFrame[2].load(),
                     g_stepsPerFrame[3].load(), g_stepsPerFrame[4].load(), g_stepsPerFrame[5].load(), g_frameCount.load());

        // ---- XP32 pose-conform cost (PPBHook.cpp atomics; reset when perf was armed) ----
        {
            std::uint64_t cf = 0, cs = 0, cm = 0;
            ArmIK::GetPoseConformStats(cf, cs, cm);
            if (cf) {
                logger::info("Diag CONFORM: fires={} mean={:.2f}us max={:.2f}us per drive (poseConform prep + inner "
                             "write; budget 50us — mitigate with poseConformEveryN)",
                             cf, (double)cs / (double)cf / 1000.0, (double)cm / 1000.0);
            } else {
                logger::info("Diag CONFORM: no conform-timed drives while armed (poseConform/poseConformDump off, "
                             "no driven actors, or only one-off map-build fires).");
            }
        }

        // ---- contact decimation (perfContactKeep, 2026-07-10) ----
        {
            const int           N      = ObjectHold::PerfContactKeepN();
            const std::uint64_t seen   = g_decimSeen.load(std::memory_order_relaxed);
            const std::uint64_t killed = g_decimKilled.load(std::memory_order_relaxed);
            logger::info("Diag DECIMATION: keepN={} ({}) candidates={} disabled={} ({:.1f}%) since arm",
                         N, N > 1 ? "ON: keep 1-in-N NEW points on NPC pairs" : "OFF", seen, killed,
                         seen ? 100.0 * (double)killed / (double)seen : 0.0);
        }

        // ---- contacts: scan the slot table ----
        std::int32_t maxPts = 0; bool maxExact = false;
        std::int64_t sumMax = 0; int pairsWithData = 0;
        double rMax = 0.0;
        std::vector<int> live;
        live.reserve(256);
        for (int i = 0; i < kNumSlots; ++i) {
            PairSlot& s = g_pairs[i];
            if (s.key.load(std::memory_order_relaxed) == 0) continue;
            const std::int32_t ml = s.maxLive.load(std::memory_order_relaxed);
            live.push_back(i);
            if (ml > 0) { sumMax += ml; ++pairsWithData; }
            if (ml > maxPts) { maxPts = ml; maxExact = s.maxFromExact.load(std::memory_order_relaxed); }
            const std::int32_t cp = s.maxChildPairs.load(std::memory_order_relaxed);
            if (cp >= 1 && ml > 0) { const double r = (double)ml / (double)cp; if (r > rMax) rMax = r; }
        }
        const double meanPtsPerPair = pairsWithData ? (double)sumMax / (double)pairsWithData : 0.0;

        const bool collCbLive = g_collisionsAdded.load() > 0;
        logger::info("Diag CONTACTS: contactPointEvents={} newPointEvents={} fullManifoldWalks={} collisionsAdded={} "
                     "collisionsRemoved={} droppedEvents={} childKeyOverflow={} pairsSeen={}",
                     g_contactPointEvents.load(), g_newPointEvents.load(), g_fullManifoldWalks.load(),
                     g_collisionsAdded.load(), g_collisionsRemoved.load(), g_droppedEvents.load(),
                     g_childKeyOverflow.load(), g_pairsSeen.load());
        logger::info("Diag CONTACTS type hist: TOI={} ExpandManifold={} Manifold={} ManifoldEndOfStep={} SavedContactPoint={}",
                     g_typeHist[0].load(), g_typeHist[1].load(), g_typeHist[2].load(), g_typeHist[3].load(), g_typeHist[4].load());
        {
            std::string tids;
            const int tn = g_threadCount.load();
            for (int i = 0; i < tn && i < 4; ++i) { char b[16]; std::snprintf(b, sizeof(b), "%u ", g_threadIds[i].load()); tids += b; }
            logger::info("Diag CONTACTS callback thread ids: [{}] (main-thread frame tick id logged by HIGGS; differing ids "
                         "mean Havok fires the callback multi-threaded — the counter is lock-free either way).", tids);
        }
        if (!collCbLive && g_contactPointEvents.load() > 500) {
            logger::info("Diag CONTACTS WARNING: collisionsAdded==0 after {} contact events — the collision-callback world "
                         "extension is not installed (PLANCK absent?). newPoints never resets, so maxPointsInOnePair is a "
                         "MONOTONIC upper bound (still safe-direction; over-reports).", g_contactPointEvents.load());
        }

        const char* exactTag = maxExact ? "EXACT" : "UPPER-BOUND";
        const char* gateVerdict = (maxPts <= 128) ? "GREEN(<=128)" : (maxPts <= 160) ? "AMBER(<=160)" : (maxPts <= 200) ? "AMBER(<=200)" : "RED(>200)";
        logger::info("Diag SHIP GATE: maxPointsInOnePair = {} [{}]  ->  {}   (mean points/pair={:.2f} over {} pairs)",
                     maxPts, exactTag, gateVerdict, meanPtsPerPair, pairsWithData);

        // top-5 offending pairs
        std::sort(live.begin(), live.end(), [](int a, int b) {
            return g_pairs[a].maxLive.load(std::memory_order_relaxed) > g_pairs[b].maxLive.load(std::memory_order_relaxed);
        });
        const int topN = (int)std::min<std::size_t>(live.size(), 5);
        for (int i = 0; i < topN; ++i) {
            PairSlot& s = g_pairs[live[i]];
            const std::int32_t ml = s.maxLive.load(std::memory_order_relaxed);
            const std::int32_t cp = s.maxChildPairs.load(std::memory_order_relaxed);
            const double r = cp >= 1 ? (double)ml / (double)cp : 0.0;
            logger::info("Diag TOP{}: {}  maxPts={} [{}] childPairs={} r={:.2f}",
                         i + 1, PairLabel(s.filterA, s.filterB), ml,
                         s.maxFromExact.load() ? "exact" : "UB", cp, r);
        }

        // projection table — the actual deliverable
        const double rProj = (rMax > 0.0) ? rMax : 4.0;   // fall back to the structural bound if unmeasured
        const bool   rMeasured = (rMax > 0.0);
        logger::info("Diag PROJECTION (r = points-per-child-pair; using r_max={:.2f} [{}], structural bound r=4):",
                     rProj, rMeasured ? "measured" : "no multi-child pair seen -> structural 4");
        auto proj = [&](const char* name, int pairs) {
            const double pm = pairs * rProj;
            const double p4 = pairs * 4.0;
            const char* vm = pm <= 128 ? "GREEN" : pm <= 200 ? "AMBER" : "RED";
            const char* v4 = p4 <= 128 ? "GREEN" : p4 <= 200 ? "AMBER" : "RED";
            logger::info("Diag PROJECT {:<22} {:>3} child-pairs -> {:>4.0f}/256 [{}] @r_max ; {:>4.0f}/256 [{}] @r=4",
                         name, pairs, pm, vm, p4, v4);
        };
        proj("COM21 x Spine2(11)", 231);
        proj("COM21 x COM21 (cross)", 441);
        proj("Spine0(11) x Spine2(11)", 121);
        proj("COM21 x Calf(4)", 84);
        proj("COM21 x Hand(3)", 63);
        logger::info("Diag DECISION: COM@21 intra-actor needs r_max < {:.3f}; cross-actor COM x COM needs r_max < {:.3f}. "
                     "If maxPointsInOnePair > 200 in ANY scenario -> do NOT bake COM at 21 (cut per Report §7).",
                     256.0 / 231.0, 256.0 / 441.0);
        logger::info("==================== end PPB perf REPORT ====================");

        std::snprintf(out, outSz,
                      "PPB perf: maxPointsInOnePair=%d [%s] (RED>200) meanStep=%.3fms p95=%.3f max=%.3f frames=%llu "
                      "steps=%llu dropped=%llu r_max=%.2f%s -> COM21xSpn2 231=%.0f/256 [%s]; COMxCOM 441=%.0f/256 [%s]. "
                      "Full dump in PPB.log.",
                      maxPts, exactTag, meanStep, p95, maxStep,
                      (unsigned long long)g_frameCount.load(), (unsigned long long)stepN,
                      (unsigned long long)g_droppedEvents.load(), rProj, rMeasured ? "" : "(struct)",
                      231 * rProj, (231 * rProj <= 200 ? "OK" : "RED"),
                      441 * rProj, (441 * rProj <= 200 ? "OK" : "RED"));
    }

}  // anonymous namespace

// ============================================================================
//  PUBLIC API
// ============================================================================
namespace Diag {

    bool Armed() { return g_armed.load(std::memory_order_relaxed); }

    void OnPhysicsStep(double stepMs) {
        if (!g_armed.load(std::memory_order_relaxed)) return;
        g_stepsThisFrame.fetch_add(1, std::memory_order_relaxed);
        g_stepCount.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t micros = (std::uint64_t)(stepMs * 1000.0 + 0.5);
        g_stepSumMicros.fetch_add(micros, std::memory_order_relaxed);
        std::uint64_t prev = g_stepMaxMicros.load(std::memory_order_relaxed);
        while (micros > prev && !g_stepMaxMicros.compare_exchange_weak(prev, micros, std::memory_order_relaxed)) {}
        int b = (int)(stepMs / kStepBucketMs);
        if (b < 0) b = 0; if (b > kStepBuckets) b = kStepBuckets;
        g_stepHist[b].fetch_add(1, std::memory_order_relaxed);
    }

    void OnFrame() {
        if (!g_armed.load(std::memory_order_relaxed)) return;
        std::uint32_t s = g_stepsThisFrame.exchange(0, std::memory_order_relaxed);
        if (s > 5) s = 5;
        g_stepsPerFrame[s].fetch_add(1, std::memory_order_relaxed);
        g_frameCount.fetch_add(1, std::memory_order_relaxed);
    }

    void RegisterFrameCallback() {
        if (g_frameCbRegistered) return;
        auto* h = Interop::GetHiggs();
        if (!h) {
            logger::info("Diag: HIGGS interface absent — steps/frame histogram unavailable (perf still measures step ms + contacts).");
            return;
        }
        h->AddPostVrikPostHiggsCallback(&Diag::OnFrame);
        g_frameCbRegistered = true;
        logger::info("Diag: frame boundary wired to HIGGS AddPostVrikPostHiggsCallback (steps/frame live).");
    }

    void OnPreDrive(RE::Actor* actor) {
        // The listener must be live for the perf counters AND for contact decimation — either arms it.
        if (g_armed.load(std::memory_order_relaxed) || ObjectHold::PerfContactKeepN() > 1)
            EnsureContactListener(actor);
        ApplyPendingSpike(actor);   // internally gated (lodSpike + matching FormID request)
    }

    void TogglePerf(RE::Actor* selected, char* out, std::size_t outSz) {
        if (!g_armed.load(std::memory_order_relaxed)) {
            ResetCounters();
            RegisterFrameCallback();                       // retry: HIGGS may have arrived after kDataLoaded
            RE::hkpWorld* world = nullptr;
            if (selected) {
                DumpCensus(selected);
                if (auto* cell = selected->GetParentCell())
                    if (auto* bhk = cell->GetbhkWorld()) world = bhk->GetWorld1();
            }
            if (world) DumpCollisionFilter(world);
            g_armed.store(true, std::memory_order_relaxed);
            logger::info("========== PPB perf ARMED (contact counter + step timer live). Run the scenario, then 'perf' again. ==========");
            std::snprintf(out, outSz,
                          "PPB perf ARMED — counters reset%s. Run the scenario, then type 'perf' again to dump maxPointsInOnePair + step ms.",
                          selected ? ", census+filter dumped to PPB.log" : " (NO NPC selected: census/filter skipped — click one before arming)");
        } else {
            g_armed.store(false, std::memory_order_relaxed);
            DumpReport(out, outSz);
            logger::info("========== PPB perf DISARMED — full report dumped to PPB.log. ==========");
        }
    }

    void CapDisRequest(RE::Actor* actor, int slot, std::uint32_t mask, char* out, std::size_t outSz) {
        if (!actor) { std::snprintf(out, outSz, "capdis: click an NPC in the console first, then: capdis <slot> <mask>"); return; }
        if (slot < 0 || slot > 11) { std::snprintf(out, outSz, "capdis: slot must be 0..11 (8 = R Thigh, the deployed 6-child list)"); return; }
        if (!ObjectHold::LodSpikeEnabled()) {
            std::snprintf(out, outSz, "capdis: SAFETY GATE — set 'lodSpike 1' in PPB_tuning.txt first, then retry. Nothing recorded.");
            return;
        }
        {
            std::lock_guard<std::mutex> g(g_spikeMx);
            g_spike.formId = actor->GetFormID();
            g_spike.slot   = slot;
            g_spike.mask   = mask;
            g_spike.pending = true;
        }
        logger::info("capdis REQUEST recorded: actor {:08X} slot {} mask=0x{:X} — applies on the next pre-drive fire "
                     "(deferred, world-locked, main thread).", actor->GetFormID(), slot, mask);
        std::snprintf(out, outSz,
                      "capdis: queued slot %d mask 0x%X on %08X. Applies next frame (watch PPB.log). "
                      "mask bit i set = child i DISABLED; mask 0 = re-enable all.", slot, mask, actor->GetFormID());
    }

    void ClearOnLoad() {
        s_ctcWorld = nullptr;                              // the Havok world is rebuilt across a load
        g_ctc.world = nullptr;
        std::lock_guard<std::mutex> g(g_spikeMx);
        g_spike.pending = false;
    }
}
