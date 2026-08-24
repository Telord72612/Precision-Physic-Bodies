#include "PCH.h"
#include "RayTel.h"
#include "Tuning.h"     // ObjectHold::RayTelMode
#include "PerfSys.h"    // PerfSys::TierCensus — the LOD-tier split

#include <atomic>
#include <chrono>
#include <cstdio>
#include <intrin.h>     // __rdtsc — INTEGER intrinsic (no XMM; see hard rule 1)

namespace logger = SKSE::log;

namespace {

    // ─────────────────────────────────────────────────────────────────────
    //  filterInfo bit layout (same convention as PerfSys.cpp:26-28 / Diag)
    // ─────────────────────────────────────────────────────────────────────
    constexpr std::uint32_t kBit15 = 0x8000u;                       // ragdoll-adjacency bit PPB stamps
    inline unsigned FilterPart(std::uint32_t f) { return (f >> 8) & 0x1F; }

    const char* kPartName[32] = {
        "Other","Head","COM","Spn1","Spn2","LUar","LLar","LHnd","LThg","LClf","LFt",
        "RUar","RLar","RHnd","RThg","RClf","RFt","Tail","Shld","Quiv","Wpn","Pony",
        "Wing","Pack","Chain","AddHead","AddChest","AddLeg","AddArm","p29","p30","p31"
    };

    // ─────────────────────────────────────────────────────────────────────
    //  ARM STATE (main thread writes, collision threads read relaxed)
    // ─────────────────────────────────────────────────────────────────────
    std::atomic<bool>          g_on{ false };      // thunks count (belt-and-braces vs in-flight calls)
    std::atomic<bool>          g_timing{ false };  // rayTel 2: also accumulate __rdtsc deltas
    std::atomic<std::uintptr_t> g_listCtnVt{ 0 };  // hkpListShape's hkpShapeContainer sub-object vtable
    bool                       g_armed = false;    // main-thread mirror (slots hold our thunks)

    // ─────────────────────────────────────────────────────────────────────
    //  COUNTERS — relaxed integer atomics ONLY (hard rule 1)
    // ─────────────────────────────────────────────────────────────────────
    // Every hot counter gets its OWN 64-byte cache line. Four Havok collision
    // threads hammering these concurrently would otherwise false-share one line
    // and the instrument would distort the thing it is measuring.
    using Ctr = std::atomic<std::uint64_t>;

    // RAY path: hkpRayCollidableFilter (per body per ray) + hkpRayShapeCollectionFilter (per child key per ray)
    alignas(64) Ctr g_rayBody{ 0 };       // ray x collidable tests
    alignas(64) Ctr g_rayBodyList{ 0 };   //   ... where the body's shape is an hkpListShape
    alignas(64) Ctr g_rayBodyPpb{ 0 };    //   ... where the body carries bit15 (PPB/ragdoll body)
    alignas(64) Ctr g_rayChild{ 0 };      // ray x child-key tests (ALL shape collections incl. terrain)
    alignas(64) Ctr g_rayChildList{ 0 };  //   ... where the container IS an hkpListShape

    // NARROWPHASE / linearCast / getClosestPoints path
    alignas(64) Ctr g_ccPair{ 0 };        // collidable x collidable (broadphase pair) — POSITIVE CONTROL
    alignas(64) Ctr g_ccPairNpc{ 0 };     //   ... where either side carries bit15
    alignas(64) Ctr g_npChild{ 0 };       // 5-arg shape-collection: shape x child key
    alignas(64) Ctr g_npChildList{ 0 };   //   ... where containerB IS an hkpListShape
    alignas(64) Ctr g_npPair{ 0 };        // 7-arg shape-collection: child x child (list vs list)
    alignas(64) Ctr g_npPairList{ 0 };    //   ... where BOTH containers are hkpListShapes

    alignas(64) Ctr g_part[32];           // per-BIPED_PART histogram of ray x PPB-body tests (low rate)
    alignas(64) Ctr g_cycles{ 0 };        // summed __rdtsc deltas across all five thunks (rayTel 2 only)
    alignas(64) Ctr g_cycMax{ 0 };        // worst single call
    alignas(64) Ctr g_calls{ 0 };         // thunk entries while timing (mean denominator; rayTel 2 only)

    inline void Bump(std::atomic<std::uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

    inline void AddCycles(std::uint64_t t0)
    {
        const std::uint64_t t1 = __rdtsc();
        if (t1 <= t0) return;                        // core migration mid-call — drop the sample
        const std::uint64_t d = t1 - t0;
        g_cycles.fetch_add(d, std::memory_order_relaxed);
        std::uint64_t mx = g_cycMax.load(std::memory_order_relaxed);
        while (d > mx && !g_cycMax.compare_exchange_weak(mx, d, std::memory_order_relaxed)) {}
    }

    // Identity test: is this hkpShapeContainer sub-object the one baked into an
    // hkpListShape?  ONE aligned pointer load + one compare, no dereference of
    // anything we have not proven. Terrain (MOPP/bvTree) and every other shape
    // collection fail this and land only in the "all" counters.
    inline bool IsListContainer(const void* c)
    {
        if (!c) return false;
        return *reinterpret_cast<const std::uintptr_t*>(c) == g_listCtnVt.load(std::memory_order_relaxed);
    }

    // Per-body classification (pure integer reads through CommonLibVR members).
    inline void ClassifyBody(const void* colv, std::atomic<std::uint64_t>& listCnt,
                             std::atomic<std::uint64_t>& ppbCnt, bool histogram)
    {
        auto* col = reinterpret_cast<const RE::hkpCollidable*>(colv);
        if (!col) return;
        const std::uint32_t fi = col->broadPhaseHandle.collisionFilterInfo.filter;
        if (fi & kBit15) {
            Bump(ppbCnt);
            if (histogram) g_part[FilterPart(fi)].fetch_add(1, std::memory_order_relaxed);
        }
        const RE::hkpShape* sh = col->shape;
        if (sh && sh->type == RE::hkpShapeType::kList) Bump(listCnt);
    }

    // ─────────────────────────────────────────────────────────────────────
    //  SIGNATURES — LIVE-VERIFIED against the running SkyrimVR.exe, 2026-08-22.
    //  ⚠⚠ hkBool is NOT returned in AL in Bethesda's build. hkBool has user-
    //  defined constructors, so the MS x64 ABI returns it through a HIDDEN
    //  RETURN SLOT: RDX carries the slot pointer (this stays in RCX), every
    //  declared argument shifts one register right, AL is written THROUGH the
    //  slot, and RAX returns the slot pointer. Read from live decrypted code:
    //    0xE2B740:  mov rsi,rdx ... mov byte ptr [rsi],al ... mov rax,rsi ; ret
    //    0xE2B710:  mov rbx,rdx   (same convention, CC filter)
    //  The 5-arg overload reads caller-stack homes 0x28/0x30/0x38 = physical
    //  args #5/6/7 of SEVEN (this, sret, input, bodyA, bodyB, container, key)
    //  — measured frame-adjusted in the live process. The previous
    //  transcription omitted the sret slot; every argument was off by one.
    //  Declared (payload) forms, from the vendored 2010 headers:
    //    hkpCollidableCollidableFilter.h:28   isCollisionEnabled(colA, colB)
    //    hkpShapeCollectionFilter.h:41        isCollisionEnabled(input, bodyA, collectionBodyB, containerB, keyB)
    //    hkpShapeCollectionFilter.h:46        isCollisionEnabled(input, cbA, cbB, containerA, containerB, keyA, keyB)
    //    hkpRayShapeCollectionFilter.h:26     isCollisionEnabled(rayInput, bShape, container, bKey)
    //    hkpRayCollidableFilter.h:29          isCollisionEnabled(worldRayInput, colB)
    //  Thunks forward the original's RAX (the slot pointer) unchanged.
    // ─────────────────────────────────────────────────────────────────────
    using FnCC  = void* (*)(const void*, void*, const void*, const void*);
    using FnSC5 = void* (*)(const void*, void*, const void*, const void*, const void*, const void*, std::uint32_t);
    using FnSC7 = void* (*)(const void*, void*, const void*, const void*, const void*, const void*, const void*,
                            std::uint32_t, std::uint32_t);
    using FnRSC = void* (*)(const void*, void*, const void*, const void*, const void*, std::uint32_t);
    using FnRC  = void* (*)(const void*, void*, const void*, const void*);

    FnCC  g_origCC  = nullptr;
    FnSC5 g_origSC5 = nullptr;
    FnSC7 g_origSC7 = nullptr;
    FnRSC g_origRSC = nullptr;
    FnRC  g_origRC  = nullptr;

    // ── THUNKS. Collision threads. Integer only. Never log, never allocate. ──

    void* T_CC(const void* self, void* ret, const void* a, const void* b)
    {
        if (!g_on.load(std::memory_order_relaxed)) return g_origCC(self, ret, a, b);
        const bool tm = g_timing.load(std::memory_order_relaxed);
        const std::uint64_t t0 = tm ? __rdtsc() : 0ull;
        Bump(g_ccPair); if (tm) Bump(g_calls);
        {
            auto* ca = reinterpret_cast<const RE::hkpCollidable*>(a);
            auto* cb = reinterpret_cast<const RE::hkpCollidable*>(b);
            const std::uint32_t fa = ca ? ca->broadPhaseHandle.collisionFilterInfo.filter : 0u;
            const std::uint32_t fb = cb ? cb->broadPhaseHandle.collisionFilterInfo.filter : 0u;
            if ((fa | fb) & kBit15) Bump(g_ccPairNpc);
        }
        void* const r = g_origCC(self, ret, a, b);
        if (tm) AddCycles(t0);
        return r;
    }

    void* T_SC5(const void* self, void* ret, const void* input, const void* bodyA, const void* colBodyB,
                const void* containerB, std::uint32_t keyB)
    {
        if (!g_on.load(std::memory_order_relaxed)) return g_origSC5(self, ret, input, bodyA, colBodyB, containerB, keyB);
        const bool tm = g_timing.load(std::memory_order_relaxed);
        const std::uint64_t t0 = tm ? __rdtsc() : 0ull;
        Bump(g_npChild); if (tm) Bump(g_calls);
        if (IsListContainer(containerB)) Bump(g_npChildList);
        void* const r = g_origSC5(self, ret, input, bodyA, colBodyB, containerB, keyB);
        if (tm) AddCycles(t0);
        return r;
    }

    void* T_SC7(const void* self, void* ret, const void* input, const void* cbA, const void* cbB,
                const void* containerA, const void* containerB, std::uint32_t keyA, std::uint32_t keyB)
    {
        if (!g_on.load(std::memory_order_relaxed))
            return g_origSC7(self, ret, input, cbA, cbB, containerA, containerB, keyA, keyB);
        const bool tm = g_timing.load(std::memory_order_relaxed);
        const std::uint64_t t0 = tm ? __rdtsc() : 0ull;
        Bump(g_npPair); if (tm) Bump(g_calls);
        if (IsListContainer(containerA) && IsListContainer(containerB)) Bump(g_npPairList);
        void* const r = g_origSC7(self, ret, input, cbA, cbB, containerA, containerB, keyA, keyB);
        if (tm) AddCycles(t0);
        return r;
    }

    void* T_RSC(const void* self, void* ret, const void* rayInput, const void* bShape, const void* container, std::uint32_t bKey)
    {
        if (!g_on.load(std::memory_order_relaxed)) return g_origRSC(self, ret, rayInput, bShape, container, bKey);
        const bool tm = g_timing.load(std::memory_order_relaxed);
        const std::uint64_t t0 = tm ? __rdtsc() : 0ull;
        Bump(g_rayChild); if (tm) Bump(g_calls);
        if (IsListContainer(container)) Bump(g_rayChildList);
        void* const r = g_origRSC(self, ret, rayInput, bShape, container, bKey);
        if (tm) AddCycles(t0);
        return r;
    }

    void* T_RC(const void* self, void* ret, const void* worldRayInput, const void* colB)
    {
        if (!g_on.load(std::memory_order_relaxed)) return g_origRC(self, ret, worldRayInput, colB);
        const bool tm = g_timing.load(std::memory_order_relaxed);
        const std::uint64_t t0 = tm ? __rdtsc() : 0ull;
        Bump(g_rayBody); if (tm) Bump(g_calls);
        ClassifyBody(colB, g_rayBodyList, g_rayBodyPpb, true);
        void* const r = g_origRC(self, ret, worldRayInput, colB);
        if (tm) AddCycles(t0);
        return r;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  SLOT TABLE. Addresses come from CommonLibVR's Address-Library-backed
    //  VTABLE ids — nothing is hand-typed. `expectRva` is a RECEIPT (what the
    //  slot held on the shipped binary when this was written), not a gate:
    //  a different value means another plugin got there first and we chain it.
    // ─────────────────────────────────────────────────────────────────────
    struct SlotDef {
        const char*    label;
        std::size_t    vtIdx;      // index into RE::bhkCollisionFilter::VTABLE
        std::size_t    slot;       // vfunc index inside that vtable
        std::uintptr_t expectRva;  // VR 1.4.15 receipt
    };
    constexpr SlotDef kSlots[5] = {
        { "hkpCollidableCollidableFilter::isCollisionEnabled (body pair)",       1, 1, 0xE2B710 },
        // ⚠ LIVE-VERIFIED 2026-08-22 (frame-adjusted disasm of the running exe): slot 0 =
        // 0xE2B8C0 is the SEVEN-arg overload (reads caller homes to arg #9, two keys), slot 1 =
        // 0xE2B740 is the FIVE-arg (homes #5/6/7: one container, one key). MSVC emits same-name
        // overloads in REVERSE declaration order. The first cut had these two swapped.
        { "hkpShapeCollectionFilter::isCollisionEnabled 5-arg (child key)",      2, 1, 0xE2B740 },
        { "hkpShapeCollectionFilter::isCollisionEnabled 7-arg (child pair)",     2, 0, 0xE2B8C0 },
        { "hkpRayShapeCollectionFilter::isCollisionEnabled (child key per ray)", 3, 0, 0xE2B830 },
        { "hkpRayCollidableFilter::isCollisionEnabled (body per ray)",           4, 1, 0xE2B890 },
    };
    std::uintptr_t g_slotAddr[5]{};
    std::uintptr_t g_slotOrig[5]{};

    void ZeroCounters()
    {
        g_rayBody = 0; g_rayBodyList = 0; g_rayBodyPpb = 0; g_rayChild = 0; g_rayChildList = 0;
        g_ccPair = 0; g_ccPairNpc = 0; g_npChild = 0; g_npChildList = 0; g_npPair = 0; g_npPairList = 0;
        g_cycles = 0; g_cycMax = 0; g_calls = 0;
        for (int i = 0; i < 32; ++i) g_part[i] = 0;
    }

    // The live-world positive control: the object the ENGINE dispatches through
    // must be the bhkCollisionFilter whose vtables we are about to patch.
    RE::hkpWorld* PlayerWorld()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        auto* cell = pc ? pc->GetParentCell() : nullptr;
        auto* bhk = cell ? cell->GetbhkWorld() : nullptr;
        return bhk ? bhk->GetWorld1() : nullptr;
    }

    // One-shot latch: an abort must SHOUT ONCE, not once per frame at 90 Hz.
    // Cleared by a knob 1 -> 0 -> 1 cycle so a retry is always available.
    bool g_abortShouted = false;

    bool Arm()
    {
        RE::hkpWorld* world = PlayerWorld();
        if (!world) return false;                       // no world yet — retry next frame, silently
        auto* filt = world->collisionFilter;
        if (!filt) {
            if (!g_abortShouted) {
                g_abortShouted = true;
                logger::error("RAYTEL ABORT: world 0x{:X} has a null collisionFilter — nothing patched. "
                              "(Shouted once; set rayTel 0 then 1 to retry.)", (std::uintptr_t)world);
            }
            return false;
        }

        // ── POSITIVE CONTROL 1: the live filter's PRIMARY vptr must be the
        //    bhkCollisionFilter vtable CommonLibVR names. If some other plugin
        //    swapped the whole filter object, we must NOT patch a vtable the
        //    engine is not using.
        const std::uintptr_t liveVt = *reinterpret_cast<std::uintptr_t*>(filt);
        REL::Relocation<std::uintptr_t> primary{ RE::bhkCollisionFilter::VTABLE[0] };
        if (liveVt != primary.address()) {
            if (!g_abortShouted) {
                g_abortShouted = true;
                logger::error("RAYTEL ABORT: live world->collisionFilter vptr 0x{:X} != "
                              "VTABLE_bhkCollisionFilter[0] 0x{:X}. Another plugin replaced the filter object, "
                              "or this is not the expected runtime. NOTHING PATCHED. (Shouted once.)",
                              liveVt, primary.address());
            }
            return false;
        }
        g_abortShouted = false;

        // ── POSITIVE CONTROL 2: hkpListShape's SECOND vtable is its
        //    hkpShapeContainer sub-object (RTTI thisOff 0x20, verified in
        //    .rdata). That pointer is our per-child "is this a PPB list body"
        //    identity test — without it every counter would also be counting
        //    terrain MOPP children.
        REL::Relocation<std::uintptr_t> listCtn{ RE::hkpListShape::VTABLE[1] };
        g_listCtnVt.store(listCtn.address(), std::memory_order_relaxed);

        const std::uintptr_t base = REL::Module::get().base();

        // Capture originals FIRST (a thunk that fires the instant the slot is
        // written must find a valid forward pointer).
        for (int i = 0; i < 5; ++i) {
            REL::Relocation<std::uintptr_t> vt{ RE::bhkCollisionFilter::VTABLE[kSlots[i].vtIdx] };
            g_slotAddr[i] = vt.address() + kSlots[i].slot * sizeof(void*);
            g_slotOrig[i] = *reinterpret_cast<std::uintptr_t*>(g_slotAddr[i]);
        }
        g_origCC  = reinterpret_cast<FnCC>(g_slotOrig[0]);
        g_origSC5 = reinterpret_cast<FnSC5>(g_slotOrig[1]);
        g_origSC7 = reinterpret_cast<FnSC7>(g_slotOrig[2]);
        g_origRSC = reinterpret_cast<FnRSC>(g_slotOrig[3]);
        g_origRC  = reinterpret_cast<FnRC>(g_slotOrig[4]);

        void* const thunks[5] = { (void*)&T_CC, (void*)&T_SC5, (void*)&T_SC7, (void*)&T_RSC, (void*)&T_RC };

        ZeroCounters();
        g_on.store(true, std::memory_order_relaxed);    // arm the counters BEFORE the first thunk can run

        logger::info("================ RAYTEL ARM RECEIPT ================");
        logger::info("RAYTEL verified: live world 0x{:X} -> collisionFilter 0x{:X}, vptr 0x{:X} == "
                     "VTABLE_bhkCollisionFilter[0] (RTTI '.?AVbhkCollisionFilter@@', .rdata-confirmed). "
                     "imagebase 0x{:X}.", (std::uintptr_t)world, (std::uintptr_t)filt, liveVt, base);
        logger::info("RAYTEL verified: hkpListShape container vtable = 0x{:X} (VTABLE_hkpListShape[1], "
                     "RTTI thisOff 0x20 = the hkpShapeContainer sub-object) — the per-child identity test.",
                     listCtn.address());

        for (int i = 0; i < 5; ++i) {
            REL::Relocation<std::uintptr_t> vt{ RE::bhkCollisionFilter::VTABLE[kSlots[i].vtIdx] };
            const std::uintptr_t before = g_slotOrig[i];
            const std::uintptr_t rva = before - base;
            vt.write_vfunc(kSlots[i].slot, reinterpret_cast<std::uintptr_t>(thunks[i]));
            const std::uintptr_t after = *reinterpret_cast<std::uintptr_t*>(g_slotAddr[i]);
            logger::info("RAYTEL hooked slot @0x{:X} (VTABLE[{}] +{}) : {}  orig rva 0x{:X} {} -> thunk 0x{:X} {}",
                         g_slotAddr[i], kSlots[i].vtIdx, kSlots[i].slot, kSlots[i].label, rva,
                         rva == kSlots[i].expectRva ? "[matches the VR 1.4.15 receipt]"
                                                    : "[DIFFERS from the receipt — chaining another plugin]",
                         (std::uintptr_t)thunks[i],
                         after == reinterpret_cast<std::uintptr_t>(thunks[i]) ? "OK" : "WRITE FAILED");
            if (after != reinterpret_cast<std::uintptr_t>(thunks[i])) {
                logger::error("RAYTEL: slot write did NOT take — rolling back everything armed so far.");
                for (int j = 0; j <= i; ++j) {
                    REL::Relocation<std::uintptr_t> v2{ RE::bhkCollisionFilter::VTABLE[kSlots[j].vtIdx] };
                    v2.write_vfunc(kSlots[j].slot, g_slotOrig[j]);
                }
                g_on.store(false, std::memory_order_relaxed);
                return false;
            }
        }
        logger::info("RAYTEL: NO CODE BYTES WRITTEN. Five 8-byte .rdata vtable slots only — disarm restores "
                     "them byte-for-byte, so `rayTel 0` is literally vanilla. Counters are relaxed uint64 "
                     "atomics; all formatting is on the main thread.");
        logger::info("RAYTEL POSITIVE CONTROL: ccPairs/s, childKeys/s and rayBodyTests/s are three INDEPENDENT "
                     "Havok entry points. At least one must be non-zero in any populated cell; all three zero "
                     "means the harness is void and the line says so.");
        logger::info("RAYTEL HOW TO READ A ZERO: childWalks/s (the hkpRayShapeCollectionFilter counter) is "
                     "DOUBLY GATED and both gates default OFF — hkpWorldRayCastInput::enableShapeCollectionFilter "
                     "is false and castRayImpl takes a completely filter-free branch when the input's filter "
                     "pointer is null (SDK disasm 2026-08-22; nothing in PPB/HIGGS/PLANCK sets either gate). "
                     "A zero here means NOBODY OPTED IN — it says NOTHING about how castRayImpl walks children. "
                     "childKeys/s (the 5-arg shape-collection filter) is the unconditional per-child counter "
                     "for linearCast / getClosestPoints / getPenetrations — that is the load-bearing number.");
        logger::info("================ RAYTEL ARM RECEIPT end ================");
        g_armed = true;
        return true;
    }

    void Disarm(const char* why)
    {
        if (!g_armed) return;
        g_on.store(false, std::memory_order_relaxed);   // stop counting first
        int restored = 0;
        for (int i = 0; i < 5; ++i) {
            // Restore ONLY if the slot still holds our thunk — never clobber a
            // plugin that hooked on top of us after we armed.
            const std::uintptr_t cur = *reinterpret_cast<std::uintptr_t*>(g_slotAddr[i]);
            const void* const mine[5] = { (void*)&T_CC, (void*)&T_SC5, (void*)&T_SC7, (void*)&T_RSC, (void*)&T_RC };
            if (cur != reinterpret_cast<std::uintptr_t>(mine[i])) {
                logger::warn("RAYTEL disarm: slot @0x{:X} no longer holds our thunk (holds 0x{:X}) — LEFT ALONE. "
                             "Our thunk stays resident and keeps forwarding.", g_slotAddr[i], cur);
                continue;
            }
            REL::Relocation<std::uintptr_t> vt{ RE::bhkCollisionFilter::VTABLE[kSlots[i].vtIdx] };
            vt.write_vfunc(kSlots[i].slot, g_slotOrig[i]);
            ++restored;
        }
        g_armed = false;
        logger::info("RAYTEL DISARMED ({}) — {}/5 vtable slots restored to the original engine pointers. "
                     "The off state is byte-identical to vanilla.", why, restored);
    }

    // ─────────────────────────────────────────────────────────────────────
    //  MAIN-THREAD REPORT (floats live here and ONLY here)
    // ─────────────────────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point g_winStart{};
    std::uint64_t                         g_winTsc = 0;
    std::uint64_t                         g_frames = 0;

    void OpenWindow()
    {
        g_winStart = std::chrono::steady_clock::now();
        g_winTsc = __rdtsc();
        g_frames = 0;
        ZeroCounters();
    }

    void Report()
    {
        const auto   now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - g_winStart).count();
        if (sec < 0.05) { OpenWindow(); return; }
        const std::uint64_t tscNow = __rdtsc();

        const double rayBody     = (double)g_rayBody.load(std::memory_order_relaxed)     / sec;
        const double rayBodyList = (double)g_rayBodyList.load(std::memory_order_relaxed) / sec;
        const double rayBodyPpb  = (double)g_rayBodyPpb.load(std::memory_order_relaxed)  / sec;
        const double rayChild    = (double)g_rayChild.load(std::memory_order_relaxed)    / sec;
        const double rayChildLst = (double)g_rayChildList.load(std::memory_order_relaxed)/ sec;
        const double ccPair      = (double)g_ccPair.load(std::memory_order_relaxed)      / sec;
        const double ccPairNpc   = (double)g_ccPairNpc.load(std::memory_order_relaxed)   / sec;
        const double npChild     = (double)g_npChild.load(std::memory_order_relaxed)     / sec;
        const double npChildLst  = (double)g_npChildList.load(std::memory_order_relaxed) / sec;
        const double npPair      = (double)g_npPair.load(std::memory_order_relaxed)      / sec;
        const double npPairLst   = (double)g_npPairList.load(std::memory_order_relaxed)  / sec;

        const double avgChildPerRayBody = rayBodyList > 0.0 ? rayChildLst / rayBodyList : 0.0;

        // top-3 parts by ray x PPB-body tests
        char parts[96]; parts[0] = 0;
        {
            int idx[3] = { -1,-1,-1 }; std::uint64_t val[3] = { 0,0,0 };
            for (int i = 0; i < 32; ++i) {
                const std::uint64_t v = g_part[i].load(std::memory_order_relaxed);
                if (v <= val[2]) continue;
                if (v > val[0])      { val[2]=val[1]; idx[2]=idx[1]; val[1]=val[0]; idx[1]=idx[0]; val[0]=v; idx[0]=i; }
                else if (v > val[1]) { val[2]=val[1]; idx[2]=idx[1]; val[1]=v; idx[1]=i; }
                else                 { val[2]=v; idx[2]=i; }
            }
            int off = 0;
            for (int k = 0; k < 3 && idx[k] >= 0; ++k)
                off += std::snprintf(parts + off, sizeof(parts) - off, "%s%s %.0f",
                                     off ? " " : "", kPartName[idx[k]], (double)val[k] / sec);
            if (!off) std::snprintf(parts, sizeof(parts), "none");
        }

        // cost: cycles -> ms, calibrated from THIS window (no assumed TSC frequency)
        char cost[128];
        if (g_timing.load(std::memory_order_relaxed) && tscNow > g_winTsc) {
            const double tscPerSec = (double)(tscNow - g_winTsc) / sec;
            const double msPerSec  = (double)g_cycles.load(std::memory_order_relaxed) / tscPerSec * 1000.0;
            const double usMax     = (double)g_cycMax.load(std::memory_order_relaxed) / tscPerSec * 1e6;
            const double msFrame   = g_frames ? (msPerSec * sec / (double)g_frames) : 0.0;
            std::snprintf(cost, sizeof(cost), "filterCPU=%.3fms/s (%.4fms/frame, worst call %.2fus, %llu calls)",
                          msPerSec, msFrame, usMax,
                          (unsigned long long)g_calls.load(std::memory_order_relaxed));
        } else {
            std::snprintf(cost, sizeof(cost), "filterCPU=off (set rayTel 2 to add __rdtsc timing)");
        }

        int tf = 0, tr = 0, tc = 0;
        PerfSys::TierCensus(tf, tr, tc);
        const bool lodOn = ObjectHold::PerfLodEnabled();

        logger::info("RAYTEL {:.2f}s fps~{:.0f} | RAY bodyTests/s={:.0f} (list {:.0f}, ppbBit15 {:.0f}) "
                     "childWalks/s={:.0f} (onLists {:.0f}) avgChildren/rayBody={:.1f} | "
                     "NARROW ccPairs/s={:.0f} (npc {:.0f}) childKeys/s={:.0f} (onLists {:.0f}) "
                     "pairKeys/s={:.0f} (listXlist {:.0f}) | topParts/s: {} | "
                     "LOD {} tiers F/R/C={}/{}/{} | {}",
                     sec, g_frames / (sec > 0 ? sec : 1.0),
                     rayBody, rayBodyList, rayBodyPpb, rayChild, rayChildLst, avgChildPerRayBody,
                     ccPair, ccPairNpc, npChild, npChildLst, npPair, npPairLst,
                     (const char*)parts, lodOn ? "ON" : "OFF", tf, tr, tc, (const char*)cost);

        // POSITIVE CONTROL: at least ONE of the three independent entry points must be live.
        // All three zero = the vtable we patched is not the one the engine dispatches through.
        if (ccPair <= 0.0 && npChild <= 0.0 && rayBody <= 0.0)
            logger::error("RAYTEL POSITIVE CONTROL FAILED: ccPairs/s, childKeys/s AND rayBodyTests/s are ALL zero. "
                          "Those are three independent Havok filter entry points; all three silent means the "
                          "vtable we patched is NOT the one the engine dispatches through. Treat every number "
                          "on the line above as VOID — do not report it as a measurement.");
        else if (rayChild <= 0.0 && rayBody > 0.0)
            logger::warn("RAYTEL NOTE: rayBodyTests/s={:.0f} but childWalks/s=0 — rays ARE reaching bodies, yet "
                         "hkpListShape::castRayImpl never consulted the per-child ray filter. That is a REAL "
                         "finding, not a broken probe: it would mean the ray path does not filter per child on "
                         "this build. Cross-check with childKeys/s (the linearCast/narrowphase child walk).",
                         rayBody);

        OpenWindow();
        g_winTsc = tscNow;
    }

}   // namespace

namespace RayTel {

    bool Armed() { return g_armed; }

    void ClearOnLoad() { ZeroCounters(); OpenWindow(); }

    void OnFrame()
    {
        const int mode = ObjectHold::RayTelMode();     // 0 off / 1 counts / 2 counts + __rdtsc timing
        if (mode <= 0) {
            if (g_armed) Disarm("rayTel 0");
            g_abortShouted = false;                    // knob 1 -> 0 -> 1 re-enables the abort shout
            return;
        }
        g_timing.store(mode >= 2, std::memory_order_relaxed);
        if (!g_armed) {
            if (!Arm()) return;                        // no world yet (main menu / loading) — retry next frame
            OpenWindow();
            return;
        }
        ++g_frames;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - g_winStart).count() >= 1.0)
            Report();
    }
}
