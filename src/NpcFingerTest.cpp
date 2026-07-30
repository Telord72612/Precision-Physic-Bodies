// ─────────────────────────────────────────────────────────────────────────
// NpcFingerTest.cpp — NPC FINGER TEST COLLIDER v2 (2026-07-09).
//
// 2026-07-13 MULTI-RIG AMENDMENT: the single global rig became g_rigs[kMaxRigs]
// — up to 8 (actor, table) rigs at once. Garment tables 1-4 are AUTO-PROBED on
// every driven NPC (knob npcFollower, ~2 s per-actor throttle); the legacy
// npcTailTest override and the tbl-0 finger test keep their old single-actor
// semantics and just occupy slots. Per-table feel (radius + FSMP push gain/
// clamp) comes from TuneOf; only the rig on the actor nearest the player
// publishes push targets each frame. Everything below this header that says
// "the rig" now means "each rig, slot-wise" — the per-rig lifecycle rules are
// unchanged.
//
// 4 runtime DYNAMIC capsules on the RIGHT hand of ONE selected/nearest driven
// NPC (Finger10→Finger12 chords per finger, index/middle/ring/pinky, NO
// thumb), each with a per-frame soft velocity drive onto the live bone chord,
// colliding ONLY with the player's HIGGS hand/wielded-weapon bodies (layer 56,
// parts 3/5). The deflection when the keyframed HIGGS hand pushes a dynamic
// finger — and the drive pulling it back — IS the test's observable.
// Built from ppb-scratch/spec2_npcfinger.md AS AMENDED BY review2_npcfinger.md.
//
// DELIBERATE DEVIATION (review2 Defect 2 — recorded here so the next reviewer
// sees it was on purpose): the round-1 reviewer mandated the Z65 motorized-
// constraint drive for a dynamic body; this TU uses applyHardKeyFrame as a
// plain velocity drive instead. PPB has ZERO constraint-creation bindings (the
// collviz Markers TU deliberately stripped all constraint machinery), and the
// stability math passes: K·dt = alpha = 0.6 < 1 (subcritical; Z65's failure
// mode was 1.11). Known residual: the velocity overwrite erases contact-
// response velocity each frame, so a hand held PRESSED into the chord gets
// ~90 Hz re-injection — if that buzzes in VR, drop npcFingerAlpha toward 0.3,
// then declare the Z65 port necessary.
//
// Body factory cloned from CollVizMarkers' Markers.cpp MarkerCreateCapsule
// (shipped, in-game-verified) with the DYNAMIC deltas of spec §2 — explicit
// m_inertiaTensor + m_centerOfMass (the engine ctor path does NOT run the
// inertia computer; a zero tensor on a dynamic body is a NaN/CTD).
//
// bhkWorld DISCIPLINE (2026-07-14, the HIGGS-precedent orphan fix): each rig holds
// a STRONG NiPointer<bhkWorld> (rig.heldWorld) to its creation world. That reference
// keeps the world (and its hkpWorld) ALIVE through any engine world-swap, so every
// destroy path can lock it and RemoveEntity deterministically, then release it — no
// orphan float, no use-after-free. (Replaced the old compare-only void* + the three
// failed deferred-removal attempts.) Filter-atomic ordering (review Defect 4): creation sets the
// "rig live" atomics BEFORE the first AddEntity; destruction clears them
// AFTER the last RemoveEntity — the vanilla fallback for finger-vs-own-
// ragdoll is COLLIDE, so the callback must cover every instant a body exists.
//
// This is the SECOND Havok-SDK TU in PPB.dll (after PivFix.cpp). CMake scopes
// the SDK include path to both; the namespace-scope hk link stubs
// (hkReferencedObject::addReference etc.) live in PivFix.cpp ONLY — repeating
// them here would be a duplicate-symbol link error.
// ─────────────────────────────────────────────────────────────────────────

#include "NpcFingerTest.h"
#include "Tuning.h"          // ObjectHold::NpcFinger* knob accessors
#include "Interop.h"         // Interop::HasHiggs / GetHiggs — creation gates
#include "HiggsInterface.h"  // GetHandRigidBody (the Defect-6 proximity gate)
#include "PerfSys.h"         // PerfSys::NearestDrivenId / HiggsRegistered
#include "FsmpLink.h"
#include "CapFix.h"          // GrabDiag::ReadCapsuleWorldU / SlotBodyPoseU — ReTouch geometry
#include "HandBox.h"         // HandBox::TipWorldU — the player's pointing-finger box        // stage-2 push publish (tail mode: capsule displacement -> SMP force)
#include "DismemberGuard.h"  // DrainQueuedCuts — DF off-thread cuts execute here (main thread)

// Windows.h (pulled via the PCH) defines NEAR/FAR as macros that collide with
// SDK identifiers. Undef before the SDK headers (min/max are already disarmed by
// the PCH's NOMINMAX).
#ifdef NEAR
#  undef NEAR
#endif
#ifdef FAR
#  undef FAR
#endif

// ── Havok 2010.2 SDK headers ────────────────────────────────────────────
#include <Common/Base/hkBase.h>
#include <Common/Base/Math/Vector/hkVector4.h>
#include <Common/Base/Math/Quaternion/hkQuaternion.h>
#include <Common/Base/Math/Matrix/hkMatrix3.h>
#include <Common/Base/Math/Matrix/hkTransform.h>
#include <Common/Base/Memory/Router/hkMemoryRouter.h>
#include <Physics/Dynamics/Entity/hkpRigidBody.h>
#include <Physics/Dynamics/Entity/hkpRigidBodyCinfo.h>
#include <Physics/Dynamics/Constraint/hkpConstraintData.h>
#include <Physics/Dynamics/Constraint/hkpConstraintInstance.h>
#include <Physics/ConstraintSolver/Constraint/Atom/hkpConstraintAtom.h>
#include <Physics/Collide/Shape/Convex/hkpConvexShape.h>
#include <Physics/Collide/Agent/Collidable/hkpCollidable.h>
#include <Physics/Collide/Agent/Collidable/hkpCollidableQualityType.h>
#include <Physics/Dynamics/Motion/hkpMotion.h>               // MOTION_* types
#include <Physics/Dynamics/World/hkpWorld.h>

// The SDK path drags in Windows headers WITHOUT the PCH's NOMINMAX guard: lowercase
// near/far (empty legacy keywords) and min/max return as macros AFTER the includes
// above — they mangle ordinary identifiers and every std::min/std::max below.
#ifdef near
#  undef near
#endif
#ifdef far
#  undef far
#endif
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

// CommonLibVR (RE::MemoryManager, RE::BSReadWriteLock, REL, logger) comes from
// the force-included PCH (src/PCH.h → RE/Skyrim.h).
// NO namespace-scope hk stubs here — PivFix.cpp:70-90 owns them for this DLL.
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string_view>
#include <xmmintrin.h>

namespace logger = SKSE::log;

namespace {

    // ── Body-creation engine bindings (VR offsets, verbatim from Markers.cpp) ──
    using _bhkRigidBodyCinfo_ctor = void(*)(void*);
    using _bhkRigidBody_ctor      = void(*)(void*, void*);
    using _bhkRigidBody_setActivated = void(*)(void*, bool);
    using _hkpWorld_AddEntity     = hkpEntity*(*)(void*, hkpEntity*, int);
    using _hkpWorld_RemoveEntity  = hkpEntity*(*)(void*, char*, hkpEntity*);
    using _hkpBody_setPosition    = void(*)(hkpEntity*, const hkVector4&);
    using _hkpBody_setRotation    = void(*)(hkpEntity*, const hkQuaternion&);
    using _applyHardKeyFrame      = void(*)(const hkVector4&, const hkQuaternion&, hkReal, hkpRigidBody*);

    inline _bhkRigidBodyCinfo_ctor fn_bhkRigidBodyCinfo_ctor() { static REL::Relocation<_bhkRigidBodyCinfo_ctor> f{ REL::Offset(0xE06110) }; return f.get(); }
    inline _bhkRigidBody_ctor      fn_bhkRigidBody_ctor()      { static REL::Relocation<_bhkRigidBody_ctor>      f{ REL::Offset(0x2AEC80) }; return f.get(); }
    inline _bhkRigidBody_setActivated fn_setActivated()        { static REL::Relocation<_bhkRigidBody_setActivated> f{ REL::Offset(0xE085D0) }; return f.get(); }
    inline _hkpWorld_AddEntity     fn_AddEntity()              { static REL::Relocation<_hkpWorld_AddEntity>     f{ REL::Offset(0xAB0CB0) }; return f.get(); }
    inline _hkpWorld_RemoveEntity  fn_RemoveEntity()           { static REL::Relocation<_hkpWorld_RemoveEntity>  f{ REL::Offset(0xAB0E50) }; return f.get(); }
    inline _hkpBody_setPosition    fn_setPosition()            { static REL::Relocation<_hkpBody_setPosition>    f{ REL::Offset(0xAA8FD0) }; return f.get(); }
    inline _hkpBody_setRotation    fn_setRotation()            { static REL::Relocation<_hkpBody_setRotation>    f{ REL::Offset(0xAA9000) }; return f.get(); }
    inline _applyHardKeyFrame      fn_applyHardKeyFrame()      { static REL::Relocation<_applyHardKeyFrame>      f{ REL::Offset(0xAF6DD0) }; return f.get(); }

    // ── Ported bhk wrapper layouts (HIGGS RE/havok.h; byte-validated in Markers) ──
    struct PortBhkShape {
        std::uint8_t  head[0x10];
        hkpShape*     shape;       // 0x10
        std::uint64_t unk18;
        std::uint32_t materialId;  // 0x20
        std::uint32_t pad24;
    };
    static_assert(sizeof(PortBhkShape) == 0x28);

    // ── Hand-built hkpCapsuleShape (the trg true-capsule marker recipe) ──────
    // No engine ctor binding exists (HIGGS only ever built boxes), so the capsule
    // is built by hand and stamped with the ENGINE's hkpCapsuleShape vtable —
    // mandatory for engine getAabb and collviz's DYNAMIC_CAST + HK_SHAPE_CAPSULE
    // tessellation. Layout byte-verified in Markers.cpp (size 0x50, radius @0x20,
    // vertexA @0x30, vertexB @0x40). No cached AABB — the floats fully define it.
    struct PortHkpCapsuleShape {
        void*         vptr;              // 0x00  engine VTABLE_hkpCapsuleShape
        std::uint16_t memSizeAndFlags;   // 0x08  0 = never-free convention (bodies leak by design)
        std::int16_t  referenceCount;    // 0x0A
        std::uint32_t pad0C;
        void*         userData;          // 0x10  bhk wrapper — collviz returns early if null
        std::int32_t  type;              // 0x18  HK_SHAPE_CAPSULE
        std::uint32_t pad1C;
        float         radius;            // 0x20  (hkpConvexShape)
        std::uint32_t pad24[3];
        float         vertexA[4];        // 0x30  .w = radius (SDK setVertex convention)
        float         vertexB[4];        // 0x40  .w = radius
    };
    static_assert(sizeof(PortHkpCapsuleShape) == 0x50);
    static_assert(offsetof(PortHkpCapsuleShape, userData) == 0x10);
    static_assert(offsetof(PortHkpCapsuleShape, radius)   == 0x20);
    static_assert(offsetof(PortHkpCapsuleShape, vertexA)  == 0x30);
    static_assert(offsetof(PortHkpCapsuleShape, vertexB)  == 0x40);

    // Engine vtable addresses (VR) — slot 0 of the real vftables, exactly what a
    // live engine object's vptr holds.
    inline void* VtblHkpCapsuleShape() { static REL::Relocation<void*> v{ REL::Offset(0x17A96B8) }; return reinterpret_cast<void*>(v.address()); }
    inline void* VtblBhkCapsuleShape() { static REL::Relocation<void*> v{ REL::Offset(0x182ADD8) }; return reinterpret_cast<void*>(v.address()); }

    struct PortBhkRigidBody {
        std::uint8_t  head[0x10];
        hkpRigidBody* hkBody;      // 0x10
        std::uint64_t unk18;
        std::uint8_t  flags;       // 0x20
        std::uint8_t  pad21[7];
        std::uint8_t  constraints[0x18];  // 0x28
    };
    static_assert(offsetof(PortBhkRigidBody, hkBody) == 0x10);
    static_assert(sizeof(PortBhkRigidBody) == 0x40);

    struct PortBhkRigidBodyCinfo {
        std::uint32_t collisionFilterInfo;  // 0x00
        std::uint32_t pad04;
        hkpShape*     shape;                 // 0x08
        std::uint8_t  unk10;
        std::uint8_t  pad11[7];
        std::uint64_t unk18;
        std::uint32_t unk20;
        float         unk24;
        std::uint8_t  unk28;
        std::uint8_t  pad29;
        std::uint16_t unk2A;
        std::uint32_t pad2C;
        alignas(16) std::uint8_t hkCinfoBuf[sizeof(hkpRigidBodyCinfo) > 0xE0 ? sizeof(hkpRigidBodyCinfo) : 0xE0];
        hkpRigidBodyCinfo& hkCinfo() { return *reinterpret_cast<hkpRigidBodyCinfo*>(hkCinfoBuf); }
    };
    static_assert(offsetof(PortBhkRigidBodyCinfo, shape)      == 0x08);
    static_assert(offsetof(PortBhkRigidBodyCinfo, hkCinfoBuf) == 0x30);

    // ── Constants ───────────────────────────────────────────────────────────
    constexpr std::uint32_t kSkinMaterialId = 0x233db702;
    // ── GARMENT touch-sound materials (2026-07-16, the "logs down the stairs" fix). The engine —
    // not HIGGS — plays a contact sound when impact speed crosses fMinSoundVel, picked by a
    // material-PAIR BGSImpactDataSet lookup on the two bhkShape materialIds. Skin's set maps nearly
    // every partner to PHYBodyMediumDirtImpact = the ragdoll BODY-FALL THUD, so skin-stamped hair/
    // tail capsules thud on every touch. Quiet choices (verified in THIS load order, ISC installed):
    // cloth -> PHYGenericClothImpact soft swish (if the lookup keys OUR set); snow/grass own sets are
    // NULL so the pair falls to skin's set's soft snow-whump/grass-rustle entries — DIRECTION-PROOF.
    // Selected by the npcGarmentMat knob (0 skin, 1 cloth, 2 snow, 3 grass, 4 none/0); applied at rig
    // CREATE — to re-dial live, change the knob then flip npcFollower 0 -> 1 (rigs rebuild).
    constexpr std::uint32_t kClothMaterialId = 0xE4D39CA3u;  // MaterialCloth 012F37
    constexpr std::uint32_t kSnowMaterialId  = 0x17C77AAFu;  // MaterialSnow  012F45
    constexpr std::uint32_t kGrassMaterialId = 0x6E2F68EEu;  // MaterialGrass 012F46
    inline std::uint32_t GarmentMaterialId() {
        switch (static_cast<int>(ObjectHold::NpcGarmentMat() + 0.5f)) {
        case 0:  return kSkinMaterialId;
        case 1:  return kClothMaterialId;
        case 2:  return kSnowMaterialId;
        case 4:  return 0u;              // no material — probe: does the pair lookup go silent or heavy?
        default: return kGrassMaterialId;
        }
    }
    constexpr float kSkyrimToHavok   = 0.0142875f;
    constexpr float kHavokToSkyrim   = 1.0f / 0.0142875f;
    constexpr int   kDoActivate      = 1;
    constexpr std::uint32_t kBit15   = 0x8000u;   // ragdoll-adjacency convention bit
    constexpr std::uint32_t kHiggsLayer  = 56u;   // L_HIGGSCOLLISION (only exists under HIGGS)
    constexpr unsigned      kFingerPart  = 30u;   // ragdoll part 30 = the FINGER SIGNATURE (unused band)
    constexpr float kChordMinU  = 1.5f;           // fist-at-creation floor (the v1 degenerate lesson)
    constexpr float kChordMaxU  = 5.0f;
    constexpr float kHandNearU  = 30.0f;          // review Defect 6: defer creation while a HIGGS hand is this close
    // REACT gates (review Defect 8): both-frames-small-gap + no-teleport + cooldown.
    // NOTE: thresholds are judgment values, not spec-sourced — the spec's example line shows a
    // 1.8u residual; steady tracking residual is ~(1-alpha)·gap ≈ 0.2u, so 1.0u has headroom.
    constexpr float kReactGapU      = 3.0f;
    constexpr float kReactResidualU = 1.0f;
    constexpr int   kTrackWin       = 90;         // ~1 s telemetry window per finger

    // ── bhkWorld field offsets + helpers (verbatim Markers.cpp) ─────────────
    constexpr std::size_t kBhkWorldLockOffset     = 0xC598;  // bhkWorld::worldLock
    constexpr std::size_t kBhkWorldHkpWorldOffset = 0x10;    // bhkWorld::world (hkpWorld*)

    inline RE::BSReadWriteLock* GetWorldLock(void* bhkWorld) {
        if (!bhkWorld) return nullptr;
        return reinterpret_cast<RE::BSReadWriteLock*>(reinterpret_cast<char*>(bhkWorld) + kBhkWorldLockOffset);
    }
    inline void* GetHkpWorld(void* bhkWorld) {
        if (!bhkWorld) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<char*>(bhkWorld) + kBhkWorldHkpWorldOffset);
    }
    inline bool IsLikelyPointer(const void* p) {
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v >= 0x10000 && (v & 0x7) == 0 && (v >> 32) != 0;
    }
    struct WorldWriteLock {
        RE::BSReadWriteLock* lock;
        explicit WorldWriteLock(void* bhkWorld) : lock(GetWorldLock(bhkWorld)) { if (lock) lock->LockForWrite(); }
        ~WorldWriteLock() { if (lock) lock->UnlockForWrite(); }
        WorldWriteLock(const WorldWriteLock&) = delete;
        WorldWriteLock& operator=(const WorldWriteLock&) = delete;
    };

    inline void* GameHeapAllocate(std::size_t bytes) {
        if (auto* mm = RE::MemoryManager::GetSingleton()) {
            void* p = mm->Allocate(bytes, 0, false);
            if (p) std::memset(p, 0, bytes);
            return p;
        }
        return nullptr;
    }
    // 16-aligned variant for hand-built hkp objects: hkVector4 members (capsule vertexA/B)
    // are read by the engine with aligned SIMD loads — an 8-aligned base would crash getAabb.
    inline void* GameHeapAllocateAligned16(std::size_t bytes) {
        if (auto* mm = RE::MemoryManager::GetSingleton()) {
            void* p = mm->Allocate(bytes, 0x10, true);
            if (p) std::memset(p, 0, bytes);
            return p;
        }
        return nullptr;
    }

    inline hkpRigidBody* HkOf(void* bhkWrapper) {
        if (!bhkWrapper || !IsLikelyPointer(bhkWrapper)) return nullptr;
        return reinterpret_cast<PortBhkRigidBody*>(bhkWrapper)->hkBody;
    }

    // Remove-from-world primitive (verbatim Markers.cpp mechanics). The CALLER is
    // responsible for the review-Defect-1 discipline: `bhkWorld` here must be the
    // FRESHLY-RESOLVED live world — never the rig's stored pointer. The
    // getWorld()==hkpWorld guard then makes a stale-body removal a clean no-op.
    void RemoveBody(void* bodyMem, void* bhkWorld) {
        hkpRigidBody* hk = HkOf(bodyMem);
        if (!hk) return;
        if (bhkWorld && IsLikelyPointer(bhkWorld)) {
            WorldWriteLock lock(bhkWorld);
            void* hkpWorld = GetHkpWorld(bhkWorld);
            if (IsLikelyPointer(hkpWorld)) {
                void* bodyWorld = static_cast<void*>(hk->getWorld());
                if (bodyWorld && bodyWorld == hkpWorld) {
                    char ret = 0;
                    fn_RemoveEntity()(hkpWorld, &ret, static_cast<hkpEntity*>(hk));
                }
            }
        }
        // (memory not freed — bhk wrapper refcount owns it; marker convention)
    }

    // ── CLONE BODY STRIP support (2026-07-26, DismemberGuard) ────────────────
    // Recursive skeleton walk collecting every node that carries a bhkRigidBody
    // wrapper. Used by NpcFinger::StripActorBodiesExcept to reduce an NGD head-
    // clone actor to a single physics body (the COM anchor the visible head
    // rides). Removal itself is the proven RemoveBody primitive above.
    void CollectBodiesRecursive(RE::NiAVObject* obj, const char* keepName,
                                void** outWrappers, const char** outNames, int& n, int cap)
    {
        if (!obj || n >= cap) return;
        if (auto* co = obj->GetCollisionObject()) {
            // Every skeleton-bone collision object derives from bhkNiCollisionObject (its
            // ragdoll bones are bhkBlendCollisionObjects) — static upcast-read of `body` is
            // layout-safe, and non-rigid worldobjects fail HkOf's pointer checks downstream.
            if (auto* bhkCo = static_cast<RE::bhkNiCollisionObject*>(co)) {
                if (auto* wrapper = bhkCo->body.get()) {
                    const char* nm = obj->name.c_str();
                    if (!keepName || !nm || _stricmp(nm, keepName) != 0) {
                        outWrappers[n] = static_cast<void*>(wrapper);
                        outNames[n]    = nm ? nm : "";
                        ++n;
                    }
                }
            }
        }
        if (auto* node = obj->AsNode())
            for (auto& child : node->GetChildren())
                CollectBodiesRecursive(child.get(), keepName, outWrappers, outNames, n, cap);
    }

    // ── Quaternion helpers (WXYZ, plain float math — no SDK calls) ───────────
    struct QuatW { float w, x, y, z; };

    inline void QuatNormalize(QuatW& q) {
        const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (n > 1e-12f) { const float inv = 1.f / n; q.w *= inv; q.x *= inv; q.y *= inv; q.z *= inv; }
        else            { q = { 1.f, 0.f, 0.f, 0.f }; }
    }
    // Shortest arc rotating local +Z (the capsule axis: vertexA=origin, vertexB=+Z·chord)
    // onto the unit chord direction d. A capsule is symmetric about its axis, so the twist
    // freedom is irrelevant (review Part 3: the spec's x-axis construction was unnecessary).
    inline QuatW QuatFromZTo(float dx, float dy, float dz) {
        const float w = 1.f + dz;                 // 1 + dot(+Z, d)
        if (w < 1e-6f) return { 0.f, 1.f, 0.f, 0.f };   // antiparallel: 180° about X
        QuatW q{ w, -dy, dx, 0.f };               // xyz = cross(+Z, d) = (-dy, dx, 0)
        QuatNormalize(q);
        return q;
    }
    inline QuatW QuatSlerp(const QuatW& a, QuatW b, float t) {
        float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (dot < 0.f) { dot = -dot; b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z; }
        float wa, wb;
        if (dot > 0.9995f) { wa = 1.f - t; wb = t; }               // nlerp fallback near-parallel
        else {
            const float th = std::acos(std::clamp(dot, -1.f, 1.f));
            const float s  = std::sin(th);
            wa = std::sin((1.f - t) * th) / s;
            wb = std::sin(t * th) / s;
        }
        QuatW q{ wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z };
        QuatNormalize(q);
        return q;
    }

    // ── Node tables (lifted verbatim from CapFix.cpp kFingerPairs rows 0-3) ──
    struct FingerPair { const char* host; const char* tip; const char* label; };
    constexpr int kFingerCount = 4;   // RIGHT hand only, no thumb (test scope); ALSO the
                                      // push-SENSOR count (only a table's first 4 chords publish)
    // 2026-07-12 CAPACITY RAISE (user: "we need more, way more"): rig bodies/arrays
    // sized for up to N chords; each table carries its own count (ChordTable.n).
    // 2026-07-15 raised 16 -> 24 for the FULL-COVERAGE fluffy-tail table (19 overlapped chords).
    // 2026-07-26 raised 24 -> 200 for full-coverage HAIR: DreadGirl (dH02_11) = 164 per-segment
    // chords, Jeeta (wing1111) = 52. Safe because hair strands are SHORT + INDEPENDENT (no long
    // single-chain servo-lag whip like the tail) and rig capsules never self-collide (Report 08).
    // All rig arrays are kMaxChords-sized, so this one bump scales bodies[]/a[]/c[]/target[] together.
    constexpr int kMaxChords = 200;
    constexpr FingerPair kFingerPairs[kFingerCount] = {
        { "NPC R Finger10 [RF10]", "NPC R Finger12 [RF12]", "R.index"  },
        { "NPC R Finger20 [RF20]", "NPC R Finger22 [RF22]", "R.middle" },
        { "NPC R Finger30 [RF30]", "NPC R Finger32 [RF32]", "R.ring"   },
        { "NPC R Finger40 [RF40]", "NPC R Finger42 [RF42]", "R.pinky"  },
    };
    constexpr const char* kFingerHandNode = "NPC R Hand [RHnd]";

    // ── TAIL MODE (2026-07-11, knob npcTailTest): the SAME rig machinery on the
    // fluffy-tail SMP bones instead of finger chords. `HDTS TailBone*` bones live in
    // the EQUIPPED TAIL MESH (FemaleTailKhajiit.nif — 21-bone chain, segments 3-6u)
    // and graft into the actor tree on equip, so bone presence IS the race/equip gate.
    // NIF-baked blend bodies on these graph-unknown bones = the finger Load3D CTD;
    // this runtime ride is the sanctioned path (06_BoneFollow research, 2026-07-11).
    // Chords span two SMP segments; resolved from the actor ROOT (no hand subtree).
    // OVERLAPPED chords (2026-07-11, user design): 3-segment spans staggered by 2, so
    // every interior region is covered by TWO capsules — the hand can trap one, its
    // neighbor still guards the fur behind it (the coverage-hole fix).
    // ★ 2026-07-15/16: the 19-chord one-per-bone experiment whipped badly (chain-amplified
    // servo lag to ~74u + the clamp/push feedback — Report 15 post-mortem) and was reverted;
    // the 4-chord base tested GREAT, so the tip is now covered by +4 MORE chords in the SAME
    // staggered style (user: "add 4 more to reach the tip") instead of one-per-bone.
    // 2026-07-17: the 8th chord (09.009 -> 09.012) REMOVED (user: "mostly past the mesh and it
    // gets caught on the hand") — the visible fur thins out before the last bones, so that
    // capsule floated beyond the tuft and snagged. The chain now ends at 09.008.
    constexpr int kTailChordCount = 7;
    constexpr FingerPair kTailPairs[kTailChordCount] = {
        { "HDTS TailBone02",     "HDTS TailBone05",     "tail.02-05" },
        { "HDTS TailBone04",     "HDTS TailBone07",     "tail.04-07" },
        { "HDTS TailBone06",     "HDTS TailBone09",     "tail.06-09" },
        { "HDTS TailBone08",     "HDTS TailBone09.002", "tail.08-09.002" },
        { "HDTS TailBone09.001", "HDTS TailBone09.004", "tail.09.001-004" },
        { "HDTS TailBone09.003", "HDTS TailBone09.006", "tail.09.003-006" },
        { "HDTS TailBone09.005", "HDTS TailBone09.008", "tail.09.005-008" },
    };
    // Variant B — the M'rissi-style foxtail convention (HDT TailBone001..0011; the
    // VRTouchEvents 01_CBPC_Touch_Nodes report's second name set — HER tail's bones
    // ARE in the live scene graph: CBPC tail touch is user-verified 100% on her).
    // 14 chords (2026-07-12 density raise, user design): the 4 original overlapped
    // SENSOR chords FIRST (indices 0-3 — the ONLY ones that publish push targets,
    // so the tuned force behavior is unchanged), then 10 adjacent GRIP chords
    // (001-002 .. 0010-0011) filling every segment so the hand always has
    // something to press ("her tail is so spaced out it's hard to push at all").
    constexpr FingerPair kTailPairsB[14] = {
        { "HDT TailBone002",  "HDT TailBone005",  "tailB.002-005" },
        { "HDT TailBone004",  "HDT TailBone007",  "tailB.004-007" },
        { "HDT TailBone006",  "HDT TailBone009",  "tailB.006-009" },
        { "HDT TailBone008",  "HDT TailBone0011", "tailB.008-0011" },
        { "HDT TailBone001",  "HDT TailBone002",  "tailB.g01-02" },
        { "HDT TailBone002",  "HDT TailBone003",  "tailB.g02-03" },
        { "HDT TailBone003",  "HDT TailBone004",  "tailB.g03-04" },
        { "HDT TailBone004",  "HDT TailBone005",  "tailB.g04-05" },
        { "HDT TailBone005",  "HDT TailBone006",  "tailB.g05-06" },
        { "HDT TailBone006",  "HDT TailBone007",  "tailB.g06-07" },
        { "HDT TailBone007",  "HDT TailBone008",  "tailB.g07-08" },
        { "HDT TailBone008",  "HDT TailBone009",  "tailB.g08-09" },
        { "HDT TailBone009",  "HDT TailBone0010", "tailB.g09-10" },
        { "HDT TailBone0010", "HDT TailBone0011", "tailB.g10-11" },
    };
    // Variant C — the VISIBLE vanilla-convention chain (TailBone01..05, XP32 CME-
    // wrapped, CBPC/animation-driven). 2026-07-12 M'rissi census finding: her visible
    // tail rides THESE (skin mesh MRTMrissiRNakedTail); the "HDT TailBone00X" chain
    // table B matches is a GHOST — an equipped invisible SMP tail item (renamed
    // Armor_00000004 at merge) dangling 5-40u under the real tail. FSMP creates NO
    // bone for plain TailBone0X (log-verified), so the push path finds no rigids on
    // this chain — table C delivers touch FEEL (HIGGS/weapon contact); motion response
    // stays with CBPC, which already reacts natively. Tried BEFORE table B in mode 1:
    // an NPC with only an equipped visible HDT tail has no TailBone01 -> falls to B.
    // OVERLAPPED chords (the user's design, same pattern as every other table:
    // staggered spans so every interior region is double-covered — the hand can
    // trap one capsule and its neighbor still guards the fur behind it).
    constexpr FingerPair kTailPairsC[kFingerCount] = {
        { "TailBone01", "TailBone03", "tailC.01-03" },
        { "TailBone02", "TailBone04", "tailC.02-04" },
        { "TailBone03", "TailBone05", "tailC.03-05" },
        { "TailBone04", "TailBone05", "tailC.04-05" },
    };

    // ── TABLE 6: Mal_Tail (2026-07-19, Yvanni the Draenei / Malignis-convention SMP tails) ──
    // 13 SMP bones "Mal_Tail 1".."Mal_Tail 13" (space-separated, no zero padding — verified from
    // her skin-carried Yvanni Tail_0.nif + Mal_Tail.xml: root kinematic, 2..13 dynamic ~1 kg,
    // chained generic constraints, <shared>public</shared>, tag MalTail). Armor-carried -> FSMP
    // AutoRenames at equip; the suffix matcher resolves plain names (exact-or-" <orig>" ends-with,
    // so "Mal_Tail 1" can never false-match "... Mal_Tail 13").
    // ★ TRIMMED 2026-07-20 (user: "twice the length of her tail; remove half"): the MESH only
    // reaches bone 7 — offline weight scan of Yvanni Tail_0.nif: bone 7 = last with strong verts,
    // 8 = 42 weak, 9..13 = ZERO verts (pure skeleton past the visible tail). The 1→13 table's
    // upper chords rode bare bones sticking straight out. 4 staggered span-2 chords ending ON
    // bone 7 (the 07-15 never-stretch-past-the-mesh law); ALL sensors (07-17 foxtail lesson).
    constexpr FingerPair kTailPairsD[4] = {
        { "Mal_Tail 1",  "Mal_Tail 3",  "tailD.01-03" },
        { "Mal_Tail 2",  "Mal_Tail 4",  "tailD.02-04" },
        { "Mal_Tail 4",  "Mal_Tail 6",  "tailD.04-06" },
        { "Mal_Tail 5",  "Mal_Tail 7",  "tailD.05-07" },
    };
    // WIG mode (2026-07-12): Carmella's dint999 "Amber Lights" wig — 193 strand bones
    // in 4 families (R x67 / L x66 / B x49 / F x11, "DKS_AmberL_R06 3" naming), carried
    // by the SKIN's wig ARMA -> renamed with the Armor prefix at merge (suffix match
    // handles it). Proof chords: the two LONGEST side strands + two back strands.
    // PERMANENT wig coverage (2026-07-13, user directive "Carmella's hair, permanently,
    // 50% capsule"): 15 of the 30 Amber Lights strands, longest-preferred + spatially
    // scattered (smp_strand_map.py run 2026-07-13; link 1 = scalp, chords span 2 -> last).
    // FIRST 4 ROWS = the push SENSORS (review 2026-07-13): the long grabbable SIDE
    // super-strands lead — front fringe and back fill follow as grip-only chords.
    constexpr FingerPair kWigPairs[15] = {
        { "DKS_AmberL_L19 2", "DKS_AmberL_L19 8", "wig.L19" },
        { "DKS_AmberL_R06 2", "DKS_AmberL_R06 8", "wig.R06" },
        { "DKS_AmberL_L11 2", "DKS_AmberL_L11 7", "wig.L11" },
        { "DKS_AmberL_R09 2", "DKS_AmberL_R09 7", "wig.R09" },
        { "DKS_AmberL_F1 2",  "DKS_AmberL_F1 4",  "wig.F1"  },
        { "DKS_AmberL_F3 2",  "DKS_AmberL_F3 4",  "wig.F3"  },
        { "DKS_AmberL_B1 2",  "DKS_AmberL_B1 7",  "wig.B1"  },
        { "DKS_AmberL_B3 2",  "DKS_AmberL_B3 7",  "wig.B3"  },
        { "DKS_AmberL_B5 2",  "DKS_AmberL_B5 7",  "wig.B5"  },
        { "DKS_AmberL_B7 2",  "DKS_AmberL_B7 7",  "wig.B7"  },
        { "DKS_AmberL_L14 2", "DKS_AmberL_L14 7", "wig.L14" },
        { "DKS_AmberL_L15 2", "DKS_AmberL_L15 7", "wig.L15" },
        { "DKS_AmberL_L20 2", "DKS_AmberL_L20 7", "wig.L20" },
        { "DKS_AmberL_R01 2", "DKS_AmberL_R01 7", "wig.R01" },
        { "DKS_AmberL_R02 2", "DKS_AmberL_R02 7", "wig.R02" },
    };
    // ── HAIR TABLES (2026-07-26) — ALL FSMP wigs across the 3 packs + the 2 Wammy followers (239),
    // generated offline by gen_all.py. Data-driven: ONE array, bound by a SIGNATURE-BONE lookup
    // (FindHairTable in the probe), not per-wig code. Table number = kHairBase + index into kHairTables.
    struct HairTable { const char* sig; const FingerPair* pairs; int n; int sensors; };
#include "PpbHairTablesAll.inc"
    constexpr int kHairBase = 1000;   // hair table numbers live at kHairBase + [0, kHairCount)

    // ── BUILD-TIME BOUND CHECK (2026-07-30) ──────────────────────────────────────────────
    // Every per-rig array is kMaxChords-sized (bodies[]/a[]/c[]/target[], and the sensor pick
    // set added with contact-ranked sensors), so a generated table carrying more chords than
    // that overflows ALL of them at once, silently. The largest table today is KS_Glow_3H_06
    // at exactly 200 = kMaxChords — i.e. we are sitting ON the limit with zero headroom, one
    // table regeneration away from a stack smash. Make that a build failure, not a runtime
    // corruption to be diagnosed from a crash log.
    constexpr int MaxGeneratedHairChords() {
        int m = 0;
        for (int i = 0; i < kHairCount; ++i)
            if (kHairTables[i].n > m) m = kHairTables[i].n;
        return m;
    }
    static_assert(MaxGeneratedHairChords() <= kMaxChords,
                  "a generated hair table exceeds kMaxChords — raise kMaxChords (EVERY per-rig "
                  "array scales with it) or regenerate the tables with a lower cap");
    // DRESS mode (DX Necromancer 3BA robes) — RETIRED 2026-07-13 (user directive "remove
    // the capsule on her clothes … back like the original"). The lower skirt chords hung at
    // the ankles/boots; a near-massless (0.05kg garment) dynamic body there, grabbed by
    // HIGGS, blew the Havok solver up and FROZE the engine on boot-touch. The DX Necromancer
    // HDT fix (the robes.xml/robes3BA.xml empty-float override shipped in the PPB mod folder)
    // is a SEPARATE artifact and STAYS — this only removes the PPB Havok capsule rig.
    // (kDressPairs table + tbl-4 tune/selector/probe all removed together.)
    constexpr float kTailChordMinU = 0.5f;    // 2026-07-15 lowered 2.0 -> 0.5: the full-coverage table's
                                              // TIP chords (09.010->09.012 etc.) span short segments; a 2.0u
                                              // floor STRETCHED them past the last bone (the "end sticking
                                              // out"). 0.5 is just a degenerate-guard floor, so every capsule
                                              // now spans EXACTLY node-to-node (endpoints sit on the XP32/SMP bones).
    constexpr float kTailChordMaxU = 12.0f;
    // SILENT-CONTACT velocity cap: now the npcSilentVelMS knob, DEFAULT OFF. The 0.15 m/s trial capped
    // a gated chord's drive to ~0.12u/frame while a hand was within 20u or a DRAWN WEAPON within 70u —
    // the same gate that opens the FSMP push publish — so tail motion built servo lag that was published
    // as displacement ×12000 and DRAGGED the SMP tail (the exact 2026-07-11 "lag reads as displacement"
    // failure the contact gate exists to prevent). See Report 15's regression post-mortem. The silence
    // goal (under HIGGS collisionMinHapticSpeed 0.2 m/s + fMinSoundVel) stays a live-dial experiment.
    // Table C's vanilla chain has ~12.9u segments (span-2 chord ~26u) — the 12u cap
    // would truncate every chord to under half. Applies to every table whose TuneOf
    // sets longCap (2/3/5); the fluffy tail (1) keeps the locked 12u cap.
    constexpr float kTailChordMaxLongU = 36.0f;   // wig super-strands 2->8 span ~33u

    // ── PER-TABLE FEEL (2026-07-13, the locked dial-session numbers): radius, FSMP push
    // gain and clamp per garment class. The fsmpPush* knobs become GLOBAL MULTIPLIERS
    // (default 1) on top of these — one dial scales everything proportionally.
    // Max push targets one actor can publish — sized by FsmpLink's target buffer (PushTarget t[14])
    // and the same-actor merge stage (g_pubStage[14]). Raising this REQUIRES raising both PLUS the
    // clamp in FsmpLink::PublishTargets — THREE sizes in lock-step. (8 -> 14 on 2026-07-17: the
    // foxtail's 10 grip chords collided but never drove the SMP bones — user: "not all capsules
    // were rigged to affect the XML bone".)
    // Single-truth: FsmpLink::kMaxPushTargets (28). Per-TABLE sensor counts stay <= 14,
    // so each rig publishes what it always did; only the same-actor MERGE stage grew —
    // the M'rissi tail+wig fix (her 14-sensor foxtail filled the old stage alone and
    // her wig published nothing).
    constexpr int kMaxSensors = FsmpLink::kMaxPushTargets;
    // `sensors` (2026-07-16) = how many of the table's LEADING chords publish FSMP push force; the
    // rest are grip-only (they collide, they don't drive the SMP sim). It lives HERE, per-table,
    // because it is a FEEL property: it was previously the global kFingerCount(4), which silently
    // made every table 4 no matter how many chords it had. That is what left the fluffy tail's 4 tip
    // chords inert (added 07-16: table grew 4 -> 8, sensor cap stayed 4 -> user: "at the tips it's
    // not even doing anything"). Per-table so raising the tail can NEVER re-tune the wig/foxtail.
    // ⚠ Rows 0..sensors-1 are the sensors — order every table so the grabbable/spread chords lead.
    struct TableTune { float r, force, maxF; bool longCap; int sensors; float massRef; };
    // massRef (2026-07-17): the bone-mass class each table's gain was DIALED on — rides with every
    // published target so FsmpLink's mass-scale normalizes around it (0.4 fluffy base bones; 0.1
    // foxtail mid bones — without this her 0.1/0.05 kg bones derate 4-8x and read dead).
    inline TableTune TuneOf(int tbl) {
        switch (tbl) {
        // fluffy Khajiit tail (HDTS): ALL 7 chords push (user directive 2026-07-16 "give power and
        // effect to all of them"; 8th chord removed 07-17 — past the fur). Force lands on each
        // chord's HOST bone, so this is the only way the chain past TailBone08 (09.001..09.008,
        // the visible tip) gets ANY force. ⚠ WATCH: the SMP masses taper 0.4 (base) -> 0.15 (tip), so the 12000 gain
        // dialed on the base delivers up to ~2.7x the ACCELERATION at the tip (a = F/m). If the tip
        // jitters, the fix is mass-scaled force (F = gain x disp x m/m_ref, Report 14 §9.3), NOT
        // dropping the gain — that would flatten the base's dialed feel.
        case 1:  return { 2.0f, 12000.f, 32000.f, false, 7, 0.4f };  // all 7 chords push (8th removed 07-17)
        case 2:  return { 2.0f,  2800.f,  7500.f, true, 14, 0.1f };  // M'rissi foxtail (2026-07-17, USER-CORRECTED):
                                                               // gain stays LOW (2400 dialed -> 2800 nudge) — her
                                                               // tail bones are LIGHT and 12000-class gain makes
                                                               // the tail "fly off violently". The real fix is
                                                               // sensors 4 -> 14: the 10 grip chords collided but
                                                               // never pushed, so mid/low-tail touches felt dead.
                                                               // Now every chord drives its host bone (dup hosts
                                                               // on 002/004/006/008 are harmless — PreSink first-
                                                               // match wins). NEVER raise this gain to 12000.
        case 3:  return { 0.3f,  3000.f, 16000.f, true,  4, 0.4f };  // wig: the 4 long side strands lead (untouched
                                                                     // incl. massRef 0.4 = today's live behavior)
        // case 4 (dress panels) RETIRED 2026-07-13 — see kDressPairs note.
        case 5:  return { 2.0f,     0.f,     0.f, true,  0, 0.4f };  // vanilla-chain tail (M'rissi): touch/grip ONLY —
                                                               // FSMP has NO rigids on plain TailBone0X (census
                                                               // 2026-07-12), motion response stays CBPC. Long
                                                               // cap: span-2 chords run ~26u.
        case 6:  return { 2.0f, 30000.f, 80000.f, true,  4, 1.0f };  // Mal_Tail (Yvanni): ~1.0 kg bones, heavily
                                                               // damped (XML 0.97/0.92). 2026-07-20 user: 2800
                                                               // was "almost nothing" (pushes FIRED — 327 in
                                                               // the log — so gain, not the gate). 30000/80000
                                                               // = the fluffy tail's PROVEN acceleration
                                                               // (12000/0.4kg ≡ 30000/1.0kg via mass-scale);
                                                               // trim live with fsmpPushMult if violent.
        // HAIR (2026-07-26): the proven wig feel (r0.3 / gain 3000 / clamp 16000 / longCap), massRef 0.4.
        // ALL 239 SMP wigs share this class (signature-bound, uniformly tuned); sensors = the wig's own
        // spread count. Dial the push live with fsmpPushMult (global); a hair-only gain knob can follow.
        default:
            if (tbl >= kHairBase && tbl - kHairBase < kHairCount)
                return { 0.3f, 3000.f, 16000.f, true, kHairTables[tbl - kHairBase].sensors, 0.4f };
            return { 0.5f, 0.f, 0.f, false, 0, 0.4f };  // fingers (no push publish)
        }
    }

    // ── FUR-CENTER OFFSETS for table B (2026-07-12, measured by
    // tools/ppb-scratch/tail_offset_measure.py on Fluffy M'rissi Replacer
    // Meshes/Fluffy/tailHDT.nif, shape 'Sippo', physics meshes\Fluffy\foxtail.xml):
    // the visible fur's weighted centroid hangs off each HDT TailBone's axis by
    // 4-9u (tip tuft 13u), CONSTANT in the bone's own bind frame — the mesh author
    // ran the bone chain along the tail's edge, not its core (the fluffy-Khajiit
    // HDTS chain runs through its fur center, which is why only M'rissi showed
    // capsules "5-6u under the tail"). Applying node.world.rotate · offset puts
    // each chord endpoint on the FUR LINE through every curl. Indexed
    // [pair][end 0=host 1=tip] to match kTailPairsB. Scaled live by
    // npcTailOffScale (0 = old bone-axis behavior) and the node's world scale.
    // Axis caveat: frame derived via pynifly skin-to-bone; if in-game placement
    // misses in a consistently ROTATED way, transpose is suspect #1.
    // Row-aligned with kTailPairsB (14 rows): per-bone measured fur-centroid
    // offsets for BOTH endpoints of every chord (sensors 0-3, grips 4-13).
    constexpr float kTailOffsB[14][2][3] = {
        { {  2.77f, -3.36f,  0.01f }, {  5.94f, -5.40f,  0.05f } },   // 002 -> 005
        { {  5.37f, -7.11f,  0.00f }, {  5.70f, -2.77f,  0.01f } },   // 004 -> 007
        { {  5.44f, -3.84f, -0.04f }, {  5.85f,  0.76f,  0.00f } },   // 006 -> 009
        { {  6.01f, -1.01f,  0.02f }, { 13.43f, -0.29f,  0.00f } },   // 008 -> 0011
        { {  2.38f, -4.14f, -0.01f }, {  2.77f, -3.36f,  0.01f } },   // 001 -> 002
        { {  2.77f, -3.36f,  0.01f }, {  4.01f, -4.27f, -0.03f } },   // 002 -> 003
        { {  4.01f, -4.27f, -0.03f }, {  5.37f, -7.11f,  0.00f } },   // 003 -> 004
        { {  5.37f, -7.11f,  0.00f }, {  5.94f, -5.40f,  0.05f } },   // 004 -> 005
        { {  5.94f, -5.40f,  0.05f }, {  5.44f, -3.84f, -0.04f } },   // 005 -> 006
        { {  5.44f, -3.84f, -0.04f }, {  5.70f, -2.77f,  0.01f } },   // 006 -> 007
        { {  5.70f, -2.77f,  0.01f }, {  6.01f, -1.01f,  0.02f } },   // 007 -> 008
        { {  6.01f, -1.01f,  0.02f }, {  5.85f,  0.76f,  0.00f } },   // 008 -> 009
        { {  5.85f,  0.76f,  0.00f }, {  6.45f,  2.10f, -0.01f } },   // 009 -> 0010
        { {  6.45f,  2.10f, -0.01f }, { 13.43f, -0.29f,  0.00f } },   // 0010 -> 0011
    };

    // Chord endpoint with the fur-center offset applied (table B only, knob-scaled).
    // `end`: 0 = host bone, 1 = tip bone of the pair.
    inline void ChordPointU(RE::NiAVObject* n, int tbl, int pair, int end, float out[3]) {
        const RE::NiPoint3& t = n->world.translate;
        out[0] = t.x; out[1] = t.y; out[2] = t.z;
        if (tbl != 2) return;
        const float k = ObjectHold::NpcTailOffScale() * n->world.scale;
        if (k <= 0.f) return;
        const float* o = kTailOffsB[pair][end];
        const RE::NiMatrix3& R = n->world.rotate;
        for (int r = 0; r < 3; ++r)
            out[r] += (R.entry[r][0] * o[0] + R.entry[r][1] * o[1] + R.entry[r][2] * o[2]) * k;
    }

    // Constructing a BSFixedString from a const char* takes the game's string-table
    // lock — intern the 9 names ONCE, at first use (the CapFix.cpp:599 idiom).
    // Finger-mode (tbl 0) name cache ONLY — tail/wig/dress modes resolve via
    // FindBoneSuffix on raw char*, never through here. Index kFingerCount*2 =
    // the group-harvest hand anchor (used by HarvestGroup for ALL modes).
    const std::array<RE::BSFixedString, kFingerCount * 2 + 1>& InternedNames(int) {
        static const std::array<RE::BSFixedString, kFingerCount * 2 + 1> s_names = [] {
            std::array<RE::BSFixedString, kFingerCount * 2 + 1> a{};
            for (int i = 0; i < kFingerCount; ++i) {
                a[i * 2 + 0] = RE::BSFixedString(kFingerPairs[i].host);
                a[i * 2 + 1] = RE::BSFixedString(kFingerPairs[i].tip);
            }
            a[kFingerCount * 2] = RE::BSFixedString(kFingerHandNode);
            return a;
        }();
        return s_names;
    }
    struct ChordTable { const FingerPair* pairs; int n; };
    ChordTable PairTable(int tbl) {
        switch (tbl) {
        case 1: return { kTailPairs,  kTailChordCount };  // fluffy tail (HDTS) — 7 chords (4 base + 3 tip), ALL sensors
        case 2: return { kTailPairsB, 14 };            // M'rissi HDT foxtail: 4 sensors + 10 grips
        case 3: return { kWigPairs,   15 };            // Amber Lights wig, permanent 50% coverage
        // case 4 (kDressPairs, DX Necromancer) RETIRED 2026-07-13 — never probed (auto-probe caps at 3).
        case 5: return { kTailPairsC, kFingerCount };  // RE-ACTIVATED 2026-07-17: skinned vanilla-chain tails
                                                       // (M'rissi post-skeleton-swap) — auto-probed via the
                                                       // cand==2 precedence + TailChainSkinned gate
        case 6: return { kTailPairsD, 4 };             // Mal_Tail SMP tails (Yvanni/Malignis convention; mesh ends at bone 7)
        default:
            if (tbl >= kHairBase && tbl - kHairBase < kHairCount) {   // any SMP wig (signature-bound)
                const HairTable& h = kHairTables[tbl - kHairBase];
                return { h.pairs, h.n };
            }
            return { kFingerPairs, kFingerCount };
        }
    }

    // Resolve the chord nodes by name (re-resolved EVERY use; no NiAVObject* is ever
    // cached — the FingerGate pooled-allocator rule). Finger mode resolves under the
    // R-hand subtree; tail mode resolves under the actor ROOT (the tail mesh grafts
    // its HDTS bones at tree level, not under a hand).
    struct FingerNodes {
        RE::NiAVObject* hand = nullptr;
        RE::NiAVObject* a[kMaxChords] = {};   // chord start
        RE::NiAVObject* c[kMaxChords] = {};   // chord end
        bool all = false;
    };
    // Suffix-aware bone finder (2026-07-11 FSMP source finding, DaymareOn/hdtSMP64
    // ActorManager::armorPrefix + doSkeletonMerge/cloneNodeTree): ARMOR-CARRIED SMP
    // bones are RENAMED at skeleton-merge to
    //   "hdtSSEPhysics_AutoRename_Armor_XXXXXXXX <orig>"   (8-hex per-armor counter + ' ')
    // so plain GetObjectByName misses them; SKELETON-carried bones keep plain names.
    // Match = exact OR ends-with-' <orig>'. First hit wins (multiple armors carrying
    // the same bone name = independent instances; fine for the test rig).
    // ── ENGINE-AGNOSTIC BONE-NAME UN-RENAMER (2026-07-29, the SMP Flex tail/hair fix) ─────
    // Every SMP engine renames armor-carried physics bones at the skeleton merge, and they
    // DO NOT agree on the scheme. Returns a pointer INTO nm at the original bone name.
    //
    //   Faster HDT-SMP : "hdtSSEPhysics_AutoRename_Armor_XXXXXXXX <orig>"   separator = SPACE
    //                    (also ..._Head_XXXXXXXX)
    //   HDT-SMP Flex   : "hdtA_XXXXXXXX_<orig>"  /  "hdtH_XXXXXXXX_<orig>" separator = UNDERSCORE
    //
    // Flex's format strings are literally `hdtA_%08X_` and `hdtH_%08X_` in its DLL (A=Armor,
    // H=Head), and its binary contains NO "AutoRename" string at all — so the old
    // ends-with-" <orig>" rule missed EVERY renamed bone under Flex. Measured consequence:
    // a Khajiit's live tail bones were present and simulating as
    // 'hdtA_1CF71480_HDTS TailBone09.003' and all four tail tables silently failed to
    // resolve, so no rig was ever created (and the same for every hair table).
    //
    // The Flex branch demands EXACTLY 8 hex digits then '_', so it cannot false-positive on
    // an ordinary bone that merely contains an underscore.
    inline const char* PlainBoneName(const char* nm) {
        if (!nm || !*nm) return nm;
        if (const char* ar = std::strstr(nm, "AutoRename_"))        // FSMP
            if (const char* sp = std::strchr(ar, ' '); sp && sp[1]) return sp + 1;
        if (nm[0] == 'h' && nm[1] == 'd' && nm[2] == 't' &&         // Flex
            (nm[3] == 'A' || nm[3] == 'H') && nm[4] == '_') {
            const char* p = nm + 5;
            int n = 0;
            while (n < 8 && ((p[n] >= '0' && p[n] <= '9') || (p[n] >= 'A' && p[n] <= 'F') ||
                             (p[n] >= 'a' && p[n] <= 'f'))) ++n;
            if (n == 8 && p[8] == '_' && p[9]) return p + 9;
        }
        return nm;
    }

    RE::NiAVObject* FindBoneSuffix(RE::NiAVObject* obj, const char* target, int tlen, int depth = 0) {
        if (!obj || depth > 40) return nullptr;
        if (const char* nm = obj->name.c_str(); nm && *nm) {
            // Compare the UN-RENAMED name: covers plain skeleton bones and both engines'
            // merge prefixes in one exact compare.
            const char* plain = PlainBoneName(nm);
            const int plen = (int)std::strlen(plain);
            if (plen == tlen && std::memcmp(plain, target, (size_t)tlen) == 0) return obj;
            // Retained fallback for any UNKNOWN renamer that uses a space separator.
            const int nlen = (int)std::strlen(nm);
            if (nlen > tlen && nm[nlen - tlen - 1] == ' ' &&
                std::memcmp(nm + nlen - tlen, target, (size_t)tlen) == 0) return obj;
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren())
                if (auto* hit = FindBoneSuffix(ch.get(), target, tlen, depth + 1)) return hit;
        return nullptr;
    }

    // Is the vanilla TailBone chain actually DRESSED — i.e. does any skinned mesh on the
    // actor bind this bone? Every PPB female skeleton CARRIES the dormant XP32 chain
    // (TailBone01-05 exist on plain humans too), so bone existence alone is NOT a tail:
    // creating table-C capsules from existence alone would put phantom tail rigs on every
    // woman in Skyrim. M'rissi's naked-skin tail (census 2026-07-12, MRTMrissiRNakedTail)
    // binds the chain; nothing on a plain human does (ponytail HAIRS are not skinned to
    // TailBone). Main thread, once per probe tick — bounded tree walk, no caching.
    bool TailChainSkinned(RE::NiAVObject* obj, const RE::NiAVObject* bone, int depth = 0) {
        if (!obj || !bone || depth > 40) return false;
        if (auto* geo = obj->AsGeometry()) {
            if (auto* si = geo->GetGeometryRuntimeData().skinInstance.get()) {
                // Bone COUNT comes from NiSkinData (the loader-populated truth — the same
                // field HIGGS IsSkinnedToNodes and PLANCK read, skinData+0x58). Do NOT use
                // NiSkinInstance::numMatrices: it sits in the renderer's lazy bone-matrix
                // upload block and is ZERO until first render, so a probe on a just-rebuilt
                // or culled actor would false-negative and stick the ghost rig (review
                // 2026-07-17 HIGH).
                const auto* sd = si->skinData.get();
                const std::uint32_t n = sd ? sd->bones : 0;
                if (si->bones && n > 0 && n < 512)
                    for (std::uint32_t i = 0; i < n; ++i)
                        if (si->bones[i] == bone) return true;
            }
            return false;
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren())
                if (TailChainSkinned(ch.get(), bone, depth + 1)) return true;
        return false;
    }

    // 2026-07-13 PERF fix (audit rank 1): ONE depth-first walk resolving EVERY chord
    // name of a table at once, replacing 2×n full-tree FindBoneSuffix walks per resolve
    // (the wig alone was 30 walks per frame — >85% of PPB's steady frame cost was this
    // repeated tree traversal, not physics). Match rule is IDENTICAL to FindBoneSuffix:
    // exact OR ends-with-" <name>" (the FSMP AutoRename armor prefix), depth cap 40,
    // and first DFS hit per name wins — so results are bit-identical to the old path.
    struct MultiResolve {
        const char*      target[kMaxChords * 2];
        int              tlen[kMaxChords * 2];
        RE::NiAVObject** out[kMaxChords * 2];
        int              n         = 0;
        int              remaining = 0;
    };
    void MultiWalk(RE::NiAVObject* obj, MultiResolve& mr, int depth = 0) {
        if (!obj || mr.remaining == 0 || depth > 40) return;
        if (const char* nm = obj->name.c_str(); nm && *nm) {
            const int nlen = (int)std::strlen(nm);
            // Un-rename ONCE per node, not once per target (this walk is the hot path — the
            // wig alone probes 30 names). Engine-agnostic: see PlainBoneName.
            const char* plain = PlainBoneName(nm);
            const int plen = (plain == nm) ? nlen : (int)std::strlen(plain);
            for (int i = 0; i < mr.n; ++i) {
                if (*mr.out[i]) continue;                     // first hit wins (matches FindBoneSuffix)
                const int tl = mr.tlen[i];
                if (plen == tl && std::memcmp(plain, mr.target[i], (size_t)tl) == 0) {
                    *mr.out[i] = obj; --mr.remaining;
                } else if (nlen > tl && nm[nlen - tl - 1] == ' ' &&
                           std::memcmp(nm + nlen - tl, mr.target[i], (size_t)tl) == 0) {
                    *mr.out[i] = obj; --mr.remaining;         // unknown space-separated renamer
                }
            }
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren()) {
                if (mr.remaining == 0) return;               // everything found — stop the walk
                MultiWalk(ch.get(), mr, depth + 1);
            }
    }

    // RETIRED (2026-07-13): this gate's only caller was the mode-1 table-5 (vanilla
    // TailBone) detour, which is retired — no code path creates a tbl-5 rig now, so
    // HasTailGeometry is DEAD. Kept (not deleted) as the reference implementation for
    // the "actor has a real tail MESH, not just the dormant XP32 bones" test, should a
    // future convention need it. Do NOT wire it back without re-reading the Carmella
    // lesson (04_Pitfall_Ledger): XP32 ships dormant TailBone01..05 on EVERY humanoid.
    //
    // Table-C gate (2026-07-12, the Carmella lesson): XP32 ships the dormant
    // TailBone01..05 rig in EVERY humanoid skeleton — bone presence alone says
    // nothing. Only bind the vanilla chain when the actor has an actual tail MESH:
    // a GEOMETRY leaf whose name contains "tail" (case-insensitive; the 4-char
    // match dodges "Nails" shapes that a bare "ail" scan would hit). M'rissi:
    // 'MRTMrissiRNakedTail...' qualifies; a tail-less follower has no such shape.
    [[maybe_unused]] bool HasTailGeometry(RE::NiAVObject* obj, int depth = 0) {
        if (!obj || depth > 40) return false;
        if (obj->AsGeometry()) {
            const char* nm = obj->name.c_str();
            if (nm)
                for (const char* p = nm; *p; ++p)
                    if ((p[0]=='t'||p[0]=='T') && (p[1]=='a'||p[1]=='A') &&
                        (p[2]=='i'||p[2]=='I') && (p[3]=='l'||p[3]=='L')) return true;
            return false;
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren())
                if (HasTailGeometry(ch.get(), depth + 1)) return true;
        return false;
    }

    FingerNodes ResolveNodes(RE::NiAVObject* root, int tbl) {
        FingerNodes n{};
        if (!root) return n;
        if (tbl > 0) {   // tail modes: ONE suffix-aware tree walk resolves every chord name
            const ChordTable tb = PairTable(tbl);
            n.hand = root;
            MultiResolve mr;
            for (int i = 0; i < tb.n; ++i) {
                mr.target[mr.n] = tb.pairs[i].host;
                mr.tlen[mr.n]   = (int)std::strlen(tb.pairs[i].host);
                mr.out[mr.n]    = &n.a[i]; ++mr.n;
                mr.target[mr.n] = tb.pairs[i].tip;
                mr.tlen[mr.n]   = (int)std::strlen(tb.pairs[i].tip);
                mr.out[mr.n]    = &n.c[i]; ++mr.n;
            }
            mr.remaining = mr.n;
            MultiWalk(root, mr);
            n.all = true;
            for (int i = 0; i < tb.n; ++i)
                if (!n.a[i] || !n.c[i]) { n.all = false; break; }
            return n;
        }
        const auto& names = InternedNames(0);
        n.hand = root->GetObjectByName(names[kFingerCount * 2]);
        if (!n.hand) return n;
        n.all = true;
        for (int i = 0; i < kFingerCount; ++i) {
            n.a[i] = n.hand->GetObjectByName(names[i * 2 + 0]);
            n.c[i] = n.hand->GetObjectByName(names[i * 2 + 1]);
            if (!n.a[i] || !n.c[i]) n.all = false;
        }
        return n;
    }

    // ── Internal state (main thread only, except the filter atomics) ────────
    struct FingerBody {                      // one per finger, 4 total
        void* bodyMem     = nullptr;         // PortBhkRigidBody* (opaque; HkOf() to reach hkpRigidBody)
        float chordU      = 0.f;             // shape length basis, solved once at creation
        float lastTargetU[3]{};              // last frame's drive target (world, game units) — REACT residual
        bool  hadSmallGap = false;           // last frame's pre-drive gap was small (REACT gate)
        std::uint32_t teleports = 0;
        // TRACK telemetry (fixed windows — no allocation in the per-frame path)
        float gapWin[kTrackWin]{};
        float velWin[kTrackWin]{};
        int   winN = 0;
        std::chrono::steady_clock::time_point lastReact{};
        // CONTACT-VERIFIED PUSH GATE state (2026-07-17, tail rigs only): hard-keyframe
        // prediction. The NEAR branch commands "land exactly on posCmd after the step",
        // so an unobstructed capsule arrives AT lastCmdPosU — any deviation = a real
        // physical obstruction. Stored as the POSITION (dt-free): reconstructing it from
        // velocity x this-frame-dt false-fired on every VR frame-time change (review
        // 2026-07-17, all three lenses — reprojection 90<->45 flips alone pierced the
        // threshold at ordinary tail-swish speeds).
        float         lastCmdPosU[3]{};  // commanded position at last drive (world, game units)
        std::uint64_t predFrame = 0;     // g_frame the prediction was written on (0 = invalid;
                                         // FAR-branch teleports and skipped frames reset it)
        std::chrono::steady_clock::time_point contactUntil{};   // contact latch expiry
    };
    struct FingerRig {
        bool          live    = false;
        bool          tailMode = false;      // any follower mode (vs finger test) — tbl > 0
        float         curlT[4]{};            // contact-curl integrator (target), per finger 0..cMax (tbl 0 only)
        float         curlA[4]{};            // EMA-smoothed applied curl (read by PPBHook's rotation writer)
        int           mode     = 0;          // npcTailTest value this rig serves (0 = auto-probe/finger;
                                             // 1/2/3 = created by the LEGACY OVERRIDE for that knob value)
        int           tbl      = 0;          // pair table this rig was built from (0..4)
        std::uint32_t actorId = 0;           // FormID — never an Actor*
        const void*   root    = nullptr;     // Get3D() pointer, COMPARE-ONLY (the FingerGate rule)
        // STRONG world reference (2026-07-14, the HIGGS-precedent orphan fix): a refcounted
        // NiPointer to the creation world. Holding it keeps the bhkWorld (and its hkpWorld)
        // ALIVE even after the engine swaps worlds on a cell change — so EVERY teardown can
        // deterministically lock it and RemoveEntity (no UAF: we own a reference), then
        // release it. This replaces the old compare-only void* whose "can't prove it's
        // alive" led to the orphan/float pile-up + three UAF-review attempts. Exactly HIGGS
        // main.cpp:562 (NiPointer<bhkWorld> world; per-frame compare; lock old; remove; reassign).
        RE::NiPointer<RE::bhkWorld> heldWorld;
        std::uint32_t group   = 0;           // owning NPC's collision group (from her R-hand body filter word)
        std::uint64_t lastSeenFrame = 0;
        // Squared player distance in GAME UNITS, refreshed every DriveRig. Used by the
        // garment-rig budget for the range destroy and to pick the farthest holder to evict.
        // ⚠ BOTH FIELDS MUST DEFAULT TO ZERO. FingerRig has no other non-zero initializer, so
        // g_rigs[kMaxRigs] lives in .bss and costs NOTHING in the image. A first attempt used
        // `float lastD2 = FLT_MAX` as the "never driven" sentinel — one non-zero default moved
        // the whole array (8 rigs x FingerBody bodies[200]) into .data and grew the DLL by
        // 1.28 MB (1,870,336 -> 3,154,944), all of it literal FLT_MAX-and-zero bytes on disk
        // and dirty pages at load. The validity flag keeps the sentinel without the cost.
        float         lastD2  = 0.f;     // meaningful only while d2Valid
        bool          d2Valid = false;   // false until this rig's first DriveRig
        int           missFrames = 0;        // consecutive DriveRig nodesMissing frames (unequip detector)
        std::chrono::steady_clock::time_point lastTrack{};   // per-rig 1 Hz TRACK throttle (a shared
                                             // static would let the first-driven rig starve the rest)
        std::chrono::steady_clock::time_point lastSkinCheck{};   // tbl-5 zombie guard: ~2 s skin-bind re-check
        int           skinMiss = 0;          // consecutive failed re-checks (3 = tail gone -> destroy)
        FingerBody    bodies[kMaxChords];    // per-rig capsule bodies (multi-rig 2026-07-13)
    };
    // 2026-07-13 MULTI-RIG (Report 15): a rig = (actor, table). Up to kMaxRigs live at
    // once — auto-probed garments (tables 1-4) on every driven NPC + the single-actor
    // finger test rig (tbl 0). Bodies are per-rig now; g_rigs[i].live gates everything.
    constexpr int kMaxRigs = 8;
    FingerRig g_rigs[kMaxRigs];
    // Hot-path cache for the comparison callback (it may run on a physics thread —
    // relaxed atomics only, no locks): the SET of live rig groups. Defect-4 ordering
    // per slot: stored BEFORE the rig's first AddEntity, cleared AFTER its last
    // RemoveEntity (the vanilla fallback for finger-vs-own-ragdoll is COLLIDE, so the
    // callback must cover every instant a body exists). 0 = empty slot.
    std::atomic<std::uint32_t> g_rigGroups[kMaxRigs] = {};
    std::atomic<std::uint32_t> g_fingerGroup{ 0 };   // the tbl-0 rig's group (own-body mode keys on THIS only)
    // per-actor probe throttle (2026-07-13 PERF fix, audit rank 2): FormID ->
    // {next-allowed probe frame, table cursor}. First sight SEEDS `next` with an
    // id-spread phase instead of probing immediately — a cell load no longer makes
    // every driven NPC resolve every garment table in the same frame (the audit's
    // 5-20ms single-frame cascade) — and each probe tick resolves at most ONE table.
    struct ProbeState { std::uint64_t next = 0; std::uint8_t cursor = 1; bool seeded = false; };
    std::unordered_map<std::uint32_t, ProbeState> g_probe;
    // 2026-07-14: the g_worldEpoch / g_worldspaceEpoch / g_pendingRemoval / ProcessPendingRemoval
    // scaffolding (the three failed deferred-removal attempts) is DELETED. A rig now holds a
    // strong NiPointer<bhkWorld> reference (FingerRig::heldWorld), so its world is always alive
    // at teardown and DestroyRig removes the bodies deterministically — no parking, no epochs.
    inline int FindFreeSlot() {
        for (int k = 0; k < kMaxRigs; ++k) if (!g_rigs[k].live) return k;
        return -1;
    }

    // Push-publish arbitration (main thread only): with several garment rigs live, only
    // the rig on the actor NEAREST the player publishes each frame — last-best-wins
    // within a frame; FsmpLink's 250 ms staleness covers ties/dropouts.
    float         g_pubBest  = FLT_MAX;
    std::uint32_t g_pubActor = 0;
    int g_pubN = 0;
    FsmpLink::PushTarget g_pubStage[kMaxSensors]{};   // derives from FsmpLink::kMaxPushTargets
    std::uint64_t g_pubFrame = 0;

    std::atomic<std::uint32_t> g_pinnedId{ 0 };   // console `nfing` pin (0 = knob/nearest mode)
    std::atomic<std::uint64_t> g_frame{ 0 };

    // First-pair filter logs (the g_cbLoggedFirstSelf.exchange idiom, once each)
    std::atomic<bool> g_logHand{ false };
    std::atomic<bool> g_logRagdoll{ false };
    std::atomic<bool> g_logCharCtrl{ false };

    // ── SKIP-reason throttle (1 line per reason per second) ─────────────────
    void LogSkip(const char* reason) {
        static std::chrono::steady_clock::time_point s_last[8]{};
        static const char* s_reasons[8] = { "noHiggs", "noWorld", "nodesMissing", "group0",
                                            "handNear", "fistChord", "allocFail", "?" };
        int idx = 7;
        for (int i = 0; i < 7; ++i) if (std::strcmp(reason, s_reasons[i]) == 0) { idx = i; break; }
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last[idx] < std::chrono::milliseconds(1000)) return;
        s_last[idx] = now;
        logger::info("NFING SKIP reason={}", reason);
    }

    // ── NODE CENSUS (2026-07-11): recursive dump of every node whose name contains
    // "ail" (Tail chains, any case) — the ground-truth instrument for whether SMP
    // tail bones exist in the live scene graph (fluffy-tail HDTS bones: the CBPC
    // visualizer says NO; M'rissi's HDT bones: touch-verified YES). Main thread only.
    void CensusWalk(RE::NiAVObject* obj, int depth, int& found) {
        if (!obj || depth > 40) return;
        const char* nm = obj->name.c_str();
        if (nm && *nm) {
            bool hit = false;
            for (const char* p = nm; *p; ++p)
                if ((p[0]=='a'||p[0]=='A') && (p[1]=='i'||p[1]=='I') && (p[2]=='l'||p[2]=='L')) { hit = true; break; }
            if (hit) {
                const auto& w = obj->world.translate;
                logger::info("CENSUS [{:2}] '{}' parent='{}' w=[{:.1f} {:.1f} {:.1f}]",
                             depth, nm, obj->parent ? obj->parent->name.c_str() : "<none>",
                             w.x, w.y, w.z);
                ++found;
            }
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren())
                CensusWalk(ch.get(), depth + 1, found);
    }
    std::atomic<bool> g_censusArmed{ false };   // nodeCensusNow edge -> one-shot dump

    // AIHANDS MODE own-arm belt (2026-07-12): exact filter WORDS of the owner's
    // hand/forearm/upperarm biped bodies, harvested at rig creation. In own-body
    // mode the fingers force-collide with the owner's flesh — but never with the
    // arm bodies they ride (constant interference). Word-match covers the L twins
    // too (same part+group word) — acceptable: no self-arm contact at all.
    std::atomic<std::uint32_t> g_ownArmWord[3] = { 0u, 0u, 0u };

    // ── FILTER KNOB CACHE (2026-07-16 — THE MISALIGNED-STACK CTD FIX) ────────────────
    // ⚠⚠ FilterDecision must contain ZERO float/SIMD work — not just zero logging.
    // HIGGS's comparator trampoline (higgs hooks.cpp:701) calls us with **RSP ≡ 8 mod 16**,
    // violating the x64 ABI's 16-byte alignment contract. MSVC's prologue trusts the ABI, so
    // the moment the register allocator needs a NONVOLATILE xmm it emits `movaps [rsp+N], xmm6`
    // — a 16-byte-ALIGNED spill onto a stack that isn't — which #GPs. CrashLogger reports that
    // as EXCEPTION_ACCESS_VIOLATION reading 0xFFFFFFFFFFFFFFFF (the -1 = "not a page fault"
    // tell). PROVEN TWICE: 2026-07-09 (fmt/spdlog's SIMD spills — hence the "never log" rule)
    // and 2026-07-16 (three plain `float` knob reads were enough register pressure to spill
    // xmm6 — CTD on load, both logs `movaps [rsp+0x20], xmm6`, RSP 0x…9F8 / 0x…238 = 8 mod 16).
    // So: the knobs are sampled on the MAIN thread (OnFrame, ~90 Hz — knobs stay hot-reloadable)
    // into ONE integer atomic; FilterDecision reads that and does pure integer math. This is the
    // SAME discipline HandBox::FilterDecision (g_boxPart) and PerfSys::FilterCB (g_cbSelfThigh)
    // already follow — NpcFinger was the lone violator.
    constexpr std::uint32_t kFKOwnBody      = 1u << 0;
    constexpr std::uint32_t kFKVsClutter    = 1u << 1;
    constexpr std::uint32_t kFKVsWorld      = 1u << 2;
    constexpr std::uint32_t kFKVsNpc        = 1u << 3;
    constexpr int           kFKBoxPartShift = 8;      // bits 8-12 = the HandBox part number
    // Seeded like HandBox's sibling `g_boxPart{4}`: if anything ever reads this before the first
    // refresh, it reads the DEFAULT part (4 = SkipBoth), never 0. (Currently unreachable — the
    // isFinger group scan bails first, and OnPreDrive refreshes before any TryCreate — but the
    // margin should be deliberate, not accidental.)
    std::atomic<std::uint32_t> g_filterKnobs{ 4u << kFKBoxPartShift };

    // MAIN THREAD ONLY (OnFrame + top of OnPreDrive). EVERY float read the collision callback's
    // decisions depend on happens HERE, never there. NOTE the accessors' types: NpcFingerOwnBody()
    // returns bool and HandBoxSubLayer() returns unsigned (both pre-thresholded/sanitised in
    // Tuning.cpp) — only the three VsClutter/VsWorld/VsNpc knobs are float, and those three are
    // exactly what the 07-16 CTD was: FilterDecision had been float-FREE until they were inlined
    // into it, and they alone tipped MSVC into spilling xmm6. Keep it that way.
    void RefreshFilterKnobs()
    {
        std::uint32_t w = 0;
        if (ObjectHold::NpcFingerOwnBody())            w |= kFKOwnBody;     // bool
        if (ObjectHold::NpcFingerVsClutter() >= 0.5f)  w |= kFKVsClutter;   // float
        if (ObjectHold::NpcFingerVsWorld()   >= 0.5f)  w |= kFKVsWorld;     // float
        if (ObjectHold::NpcFingerVsNpc()     >= 0.5f)  w |= kFKVsNpc;       // float
        w |= (ObjectHold::HandBoxSubLayer() & 0x1Fu) << kFKBoxPartShift;    // unsigned, already sanitised
        g_filterKnobs.store(w, std::memory_order_relaxed);
    }

    // ── Group harvest: the owning NPC's collision group off her R-hand body ──
    std::uint32_t HarvestGroup(RE::Actor* actor) {
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return 0;
        auto* obj = root->GetObjectByName(InternedNames(0)[kFingerCount * 2]);
        if (!obj) return 0;
        auto* colObj = obj->collisionObject.get();
        if (!colObj) return 0;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        auto* rb   = body ? body->GetRigidBody() : nullptr;
        auto* col  = rb ? rb->GetCollidableRW() : nullptr;
        if (!col) return 0;
        return col->broadPhaseHandle.collisionFilterInfo.filter >> 16;   // Diag's accessor idiom
    }

    // ── DESTROY (the ONE destroy path — review Defect 1 + Defect 4) ─────────
    // Fresh-resolves the actor's CURRENT world and pointer-compares it to the
    // recorded one. Equal -> RemoveBody with the FRESH pointer. Mismatch, null
    // cell, or unresolvable actor -> drop all pointers (the bodies died with
    // their world, or ride the old world to teardown — accepted residual).
    // The filter atomics are cleared AFTER removal: the vanilla fallback for
    // finger-vs-own-ragdoll is COLLIDE, so the callback must stay live for
    // every instant a body still exists (Defect 4 ordering).
    void DestroyRig(int slot, const char* reason) {
        FingerRig& rig = g_rigs[slot];
        if (!rig.live) return;
        // The rig holds a STRONG reference to its world (rig.heldWorld), so that world —
        // and its hkpWorld — is GUARANTEED still alive here even if the engine swapped
        // worlds on a cell change (HIGGS main.cpp:568 does exactly this). So we can
        // unconditionally lock it and RemoveEntity every body: no orphan, no UAF, no
        // parking. RemoveBody re-checks the body's own world membership as a belt.
        const bool removed = static_cast<bool>(rig.heldWorld);
        if (rig.heldWorld) {
            void* w = static_cast<void*>(rig.heldWorld.get());
            for (int i = 0; i < kMaxChords; ++i)
                if (rig.bodies[i].bodyMem) RemoveBody(rig.bodies[i].bodyMem, w);
        }
        // teleports list covers EVERY live chord (was hardcoded [0..3] — under-reported the 19-body
        // experiment's distal churn and contradicted TRACK telemetry; caught in the 07-15 post-mortem).
        char tps[kMaxChords * 8]; int tn = 0;
        for (int i = 0; i < kMaxChords && rig.bodies[i].bodyMem; ++i)
            tn += std::snprintf(tps + tn, sizeof(tps) - tn, "%s%d", i ? " " : "", rig.bodies[i].teleports);
        tps[tn] = '\0';
        logger::info("NFING DESTROY slot={} tbl={} reason={} actor={:08X} removed={} teleports=[{}]",
                     slot, rig.tbl, reason, rig.actorId, removed, tps);
        const bool wasFinger = rig.tbl == 0;
        rig = FingerRig{};   // default-construct resets heldWorld -> releases our world reference
        // Defect 4: this slot's group atomic clears LAST — after every RemoveEntity
        // completed. A same-actor sibling rig keeps its own slot's copy of the group,
        // so the comparator still covers ITS bodies.
        g_rigGroups[slot].store(0, std::memory_order_relaxed);
        if (wasFinger) g_fingerGroup.store(0, std::memory_order_relaxed);
    }

    // ── CREATE one capsule — MarkerCreateCapsule clone + the spec-§2 DYNAMIC deltas ──
    void* CreateFingerBody(void* bhkWorld, const float posU[3], const QuatW& rot,
                           float chordU, float radiusU, float massKg,
                           std::uint32_t filterInfo, float gravityFactor,
                           std::uint32_t materialId = kSkinMaterialId)
    {
        if (!bhkWorld) return nullptr;

        // 1) the hkpCapsuleShape itself (16-aligned: engine reads vertexA/B via aligned SIMD)
        auto* cap = static_cast<PortHkpCapsuleShape*>(GameHeapAllocateAligned16(sizeof(PortHkpCapsuleShape)));
        if (!cap) return nullptr;
        cap->vptr            = VtblHkpCapsuleShape();
        cap->memSizeAndFlags = 0;   // never-free convention
        cap->referenceCount  = 1;
        cap->type            = HK_SHAPE_CAPSULE;
        const float r = radiusU * kSkyrimToHavok;
        cap->radius = r;
        cap->vertexA[0] = 0.f; cap->vertexA[1] = 0.f; cap->vertexA[2] = 0.f;
        cap->vertexA[3] = r;        // .w = radius (SDK setVertex convention)
        cap->vertexB[0] = 0.f; cap->vertexB[1] = 0.f; cap->vertexB[2] = chordU * kSkyrimToHavok;
        cap->vertexB[3] = r;

        // 2) bhkCapsuleShape wrapper — hand-stamped mirror of the engine's 0x28 bhkShape
        //    (collviz null-checks shape->userData; no virtuals are called on it).
        void* shapeMem = GameHeapAllocate(sizeof(PortBhkShape));
        if (!shapeMem) return nullptr;
        auto* portShape = reinterpret_cast<PortBhkShape*>(shapeMem);
        *reinterpret_cast<void**>(portShape->head)                = VtblBhkCapsuleShape();
        *reinterpret_cast<std::uint32_t*>(portShape->head + 0x8)  = 1;   // NiRefObject refcount
        portShape->shape      = reinterpret_cast<hkpShape*>(cap);
        portShape->materialId = materialId;   // skin for fingers; quiet garment material for hair/tail (thud fix)
        cap->userData         = portShape;

        // 3) cinfo/body/AddEntity — MarkerCreateCapsule flow + the DYNAMIC deltas (spec §2):
        PortBhkRigidBodyCinfo cInfo;
        std::memset(&cInfo, 0, sizeof(cInfo));
        fn_bhkRigidBodyCinfo_ctor()(&cInfo);
        hkpRigidBodyCinfo& hkc = cInfo.hkCinfo();
        cInfo.collisionFilterInfo = filterInfo;             // delta 6: REAL colliding word, not a ghost layer
        cInfo.shape               = reinterpret_cast<hkpShape*>(cap);
        hkc.m_collisionFilterInfo = filterInfo;
        hkc.m_shape               = reinterpret_cast<hkpShape*>(cap);
        hkc.m_motionType          = hkpMotion::MOTION_DYNAMIC;          // delta 1
        hkc.m_qualityType         = HK_COLLIDABLE_QUALITY_MOVING;       // delta 2 (NOT KEYFRAMED_REPORTING — PLANCK branches on it)
        // delta 3: mass + EXPLICIT inertia — the engine ctor path does not run
        // hkpInertiaTensorComputer; a zero tensor on a dynamic body is NaN/CTD.
        // Solid-capsule diagonal about the CENTER, with m_centerOfMass moved to the
        // capsule center (review Defect 7 — shape-local origin is at vertexA/knuckle).
        const float L   = chordU * kSkyrimToHavok;                      // havok metres
        const float m   = massKg;
        const float izz = std::max(0.5f * m * r * r, 1e-5f);
        const float ixx = std::max(m * (L * L / 12.f + r * r / 4.f), 1e-5f);
        hkc.m_mass = m;
        hkc.m_inertiaTensor.setDiagonal(ixx, ixx, izz);
        hkc.m_centerOfMass.set(0.f, 0.f, L * 0.5f, 0.f);
        hkc.m_gravityFactor      = gravityFactor;                       // delta 4 (default 0; drive overwrites velocity anyway)
        // delta 5 — velocity ceiling. 100/500 are HIGGS's own HAND values; correct for a
        // ~4 kg finger body but wrong-sized for a ~0.05 kg garment push-capsule. Q2 guard
        // (2026-07-15): should a grab-motor ever reach a light body (belt BEHIND the
        // FilterDecision grab-phantom shield), a low ceiling bounds the per-substep
        // ejection (~1-2 u/frame at 90 Hz) so it stays inside snapU=20u and the FAR-branch
        // recovery zeroes it gracefully — instead of a 100 m/s ≈ 197 u/frame tunnel that
        // stresses CCD. Mass/inertia are UNTOUCHED (re-heavying re-drags the SMP hair);
        // only runaway ejection speed is capped, so contact hand-feel + push-sensing hold.
        const bool lightBody     = massKg < 1.f;                        // garment (~0.05) vs finger (~4)
        hkc.m_maxLinearVelocity  = lightBody ? 10.f  : 100.f;
        hkc.m_maxAngularVelocity = lightBody ? 50.f  : 500.f;
        hkc.m_linearDamping      = 0.05f;
        hkc.m_enableDeactivation = false;                               // a deactivated test body would freeze mid-air
        hkc.m_solverDeactivation = hkpRigidBodyCinfo::SOLVER_DEACTIVATION_OFF;
        hkVector4 posHavok;
        posHavok.set(posU[0] * kSkyrimToHavok, posU[1] * kSkyrimToHavok, posU[2] * kSkyrimToHavok, 0.f);
        hkc.m_position = posHavok;
        hkc.m_rotation.set(rot.x, rot.y, rot.z, rot.w);                 // hk stores (imag, real)

        void* bodyMem = GameHeapAllocate(sizeof(PortBhkRigidBody));
        if (!bodyMem) return nullptr;
        fn_bhkRigidBody_ctor()(bodyMem, &cInfo);
        auto* portBody = reinterpret_cast<PortBhkRigidBody*>(bodyMem);
        if (!portBody->hkBody) return nullptr;
        fn_setActivated()(bodyMem, true);
        {
            WorldWriteLock lock(bhkWorld);
            void* hkpW = GetHkpWorld(bhkWorld);
            if (!IsLikelyPointer(hkpW)) return nullptr;
            fn_AddEntity()(hkpW, portBody->hkBody, kDoActivate);
        }
        return bodyMem;
    }

    // ── MESH-BAND MARKERS (2026-07-18, Route B): 7 ghost capsules at the girth verts ──────────
    // Part-29 layer-56 words: FilterDecision ignores every pair involving them, so they are
    // pure visuals (the collision visualizer draws every world body regardless of filter).
    // Recreated per update via RemoveBody(freshWorld) — never the stored pointer (Defect 1).
    static void* g_meshMarker[11] = {};
    static void* g_meshMarkerWorld = nullptr;   // COMPARE-ONLY (world-change detection)

    void UpdateMeshMarkersImpl(RE::Actor* actor, const float posU[][3], int count)
    {
        if (count > 11) count = 11;   // 2026-07-23: 7 torso + 4 limb landmarks
        auto* cell = actor ? actor->GetParentCell() : nullptr;
        RE::bhkWorld* wp = cell ? cell->GetbhkWorld() : nullptr;
        void* world = static_cast<void*>(wp);
        if (!world) return;
        if (g_meshMarkerWorld && g_meshMarkerWorld != world)
            for (auto& b : g_meshMarker) b = nullptr;      // old world died with its bodies — forget
        for (auto& b : g_meshMarker)
            if (b) { RemoveBody(b, world); b = nullptr; }  // fresh live world, never the stored ptr
        g_meshMarkerWorld = world;
        // ── 2026-07-25 ROOT FIX (the haunted-floor seizure + the Papyrus playerImpact storm):
        // markers used to ride kHiggsLayer — the PLAYER-HAND layer — so an actor's ragdoll
        // fighting an embedded marker read as a stream of PLAYER IMPACTS: physics storm AND a
        // Papyrus event flood (the VM dumps the user watched). A visualization body must be
        // collidable by NOTHING: layer 15 = L_NONCOLLIDABLE. If collviz stops drawing them,
        // that is a cosmetic problem to solve later — never by putting them back on a live layer.
        constexpr std::uint32_t kMarkerWord =
            15u | (1u << 15) | (29u << 8) | (0xFFF0u << 16);
        const QuatW ident{ 1.f, 0.f, 0.f, 0.f };
        int made = 0;
        for (int i = 0; i < count; ++i) {
            const float* p = posU[i];
            if (!(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))) continue;
            if (p[2] < -1e8f) continue;                    // CapFix "band never fired" sentinel
            g_meshMarker[i] = CreateFingerBody(world, p, ident, 0.6f, 0.225f, 0.05f,   // halved 2026-07-22 for precise placement judging
                                               kMarkerWord, 0.f, kGrassMaterialId);
            if (g_meshMarker[i]) ++made;
        }
        logger::info("MESHMARK: {} band marker(s) placed (ghost capsules — toggle collviz to see)", made);
    }

    // ── CREATE one rig of the given table in the given slot (main thread, live
    // world in hand). The caller owns table selection (legacy mode mapping, auto-
    // probe, finger) and guarantees `slot` is free; rig.mode stays 0 here — the
    // LEGACY OVERRIDE caller stamps it after a successful create. ──────────────
    // Squared player distance in GAME UNITS. Lives in the anonymous namespace because BOTH
    // TryCreate (birth stamp) and the garment-rig budget in namespace NpcFinger call it.
    float GarmentPlayerD2(RE::Actor* actor)
    {
        if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
            const RE::NiPoint3 pp = pl->GetPosition(), ap = actor->GetPosition();
            const float dx = ap.x - pp.x, dy = ap.y - pp.y, dz = ap.z - pp.z;
            return dx * dx + dy * dy + dz * dz;
        }
        return FLT_MAX;
    }

    void TryCreate(RE::Actor* actor, int tbl, int slot)
    {
        // Preconditions (spec §2 + review Defects 5/6). Layer 56 and the filter
        // callback only exist when HIGGS installed them.
        if (!Interop::HasHiggs() || !PerfSys::HiggsRegistered()) { LogSkip("noHiggs"); return; }
        FingerRig& rig = g_rigs[slot];
        if (rig.live) return;   // caller bug belt — never re-dress a live slot
        auto* cell = actor->GetParentCell();
        RE::bhkWorld* worldPtr = cell ? cell->GetbhkWorld() : nullptr;
        void* world = static_cast<void*>(worldPtr);
        if (!world) { LogSkip("noWorld"); return; }
        auto* root = actor->Get3D();
        const bool tailMode = tbl > 0;                      // any garment table (vs finger test)
        FingerNodes nodes = ResolveNodes(root, tbl);
        if (!nodes.all) { LogSkip("nodesMissing"); return; }
        const std::uint32_t group = HarvestGroup(actor);
        if (group == 0) { LogSkip("group0"); return; }   // review Defect 5: systemGroup 0 collides with everything — retry next tick

        // AIHands own-body mode: harvest the owner's arm-body words for the belt (see
        // g_ownArmWord). FINGER-RIG creation only (multi-rig re-scope: the belt serves
        // npcFingerOwnBody, a finger feature — garment rigs must not re-stamp it with
        // some other NPC's arm words).
        if (tbl == 0) {
            static constexpr const char* kArmNodes[3] =
                { "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]" };
            auto* root3d = actor->Get3D();
            for (int k = 0; k < 3; ++k) {
                std::uint32_t w = 0;
                if (auto* nd = root3d ? root3d->GetObjectByName(kArmNodes[k]) : nullptr) {
                    if (auto* co = nd->collisionObject.get()) {
                        auto* body = static_cast<RE::bhkCollisionObject*>(co)->GetRigidBody();
                        auto* rb   = body ? body->GetRigidBody() : nullptr;
                        if (auto* col = rb ? rb->GetCollidableRW() : nullptr)
                            w = col->broadPhaseHandle.collisionFilterInfo.filter;   // CFilter -> raw word
                    }
                }
                g_ownArmWord[k].store(w, std::memory_order_relaxed);
            }
        }

        // Review Defect 6: defer creation while a HIGGS hand body is near the fingers —
        // spawning a dynamic capsule inside the infinite-mass keyframed hand is a
        // violent first-frame depenetration ejection that poisons the first REACT read.
        if (auto* h = Interop::GetHiggs()) {
            for (bool isLeft : { false, true }) {
                hkpRigidBody* hb = HkOf(h->GetHandRigidBody(isLeft));
                if (!hb || !IsLikelyPointer(hb)) continue;
                const hkVector4& hp = hb->getPosition();
                const float hx = hp(0) * kHavokToSkyrim, hy = hp(1) * kHavokToSkyrim, hz = hp(2) * kHavokToSkyrim;
                for (int i = 0; i < PairTable(tbl).n; ++i) {
                    const RE::NiPoint3& t = nodes.a[i]->world.translate;
                    const float dx = t.x - hx, dy = t.y - hy, dz = t.z - hz;
                    if (dx * dx + dy * dy + dz * dz < kHandNearU * kHandNearU) { LogSkip("handNear"); return; }
                }
            }
        }

        // Filter word (spec §4): 56 | part 30 | bit15 | owning group. The same-group +
        // bit-15 stamp is the proven charController coexistence mechanism.
        const std::uint32_t filterInfo = kHiggsLayer | (kFingerPart << 8) | kBit15 | (group << 16);
        // Per-table feel (2026-07-13): garment radii come from the locked TuneOf dial
        // numbers, not the npcTailR knob; the finger test keeps its own knob.
        const TableTune tune = TuneOf(tbl);
        const float rU      = tailMode ? tune.r : ObjectHold::NpcFingerR();
        const float tipU    = tailMode ? 0.f : ObjectHold::NpcFingerTipU();
        // 2026-07-14 GARMENT MASS FIX: the finger rig wants a heavy body (npcFingerMassKg,
        // ~4kg) for a solid own-body push; but a GARMENT rig has 11-15 capsules, and at 4kg
        // each that's 44-60kg riding the SMP hair/dress bones, dragging them down (user:
        // "15 capsules made her hair heavy, 4 fingers was fine"). Garment capsules are a
        // light touch-surface — give them a small mass so they never weigh down the cloth.
        // 2026-07-15 PER-GARMENT MASS: a fluffy/foxtail TAIL (tbl 1/2) needs a MEATY mass — at 0.05 kg
        // (hair weight) it can't hold a push and gets flung ("flies away", "can't move the tail"). Hair
        // (tbl 3) stays light so it doesn't drag the SMP strands. Fingers use their own knob.
        const float massKg  = (tbl == 1 || tbl == 2 || tbl == 5 || tbl == 6) ? ObjectHold::NpcTailMassKg()
                            : tailMode               ? ObjectHold::NpcGarmentMassKg()
                                                     : ObjectHold::NpcFingerMassKg();
        const float gravity = ObjectHold::NpcFingerGravity();

        // Defect 4 mirror: this slot's group atomic goes live BEFORE the first AddEntity
        // so the callback Ignores finger-vs-ragdoll from the instant a body enters the world.
        g_rigGroups[slot].store(group, std::memory_order_relaxed);
        if (tbl == 0) g_fingerGroup.store(group, std::memory_order_relaxed);

        // npcFingerCount (default 1) = the MANDATORY Risk-1 spike: dynamic creation
        // via the engine ctor is the one 80%-confidence step — verify ONE capsule
        // (index) in VR first, then raise the knob to 4. Applied at creation only;
        // a knob change while a rig is live takes effect on the next recreate.
        const int nCreate = tailMode ? PairTable(tbl).n
                                     : std::min((int)ObjectHold::NpcFingerCount(), kFingerCount);

        float chords[kMaxChords]{};
        bool  failed = false;
        for (int i = 0; i < nCreate; ++i) {
            float pA[3], pC[3];                     // fur-line endpoints (offset applies on tbl 2 only)
            ChordPointU(nodes.a[i], tbl, i, 0, pA);
            ChordPointU(nodes.c[i], tbl, i, 1, pC);
            const float dx = pC[0] - pA[0], dy = pC[1] - pA[1], dz = pC[2] - pA[2];
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            // fist-at-creation would bake a collapsed chord — clamp (the v1 degenerate lesson)
            // 2026-07-12: table B (M'rissi HDT) joined the 36u cap — her chords measure
            // 14-19u and the 12u cap was TRUNCATING them from the host end, eating the
            // overlap region (user report: "capsules end to end, hand passes through").
            // 2026-07-13: the long cap is now per-table data (TuneOf.longCap — tbls 2/3/4).
            // Fluffy (tbl 1) chords are 6-8u and never reach either cap.
            const float chordU = std::clamp(len + tipU,
                                            tailMode ? kTailChordMinU : kChordMinU,
                                            tune.longCap ? kTailChordMaxLongU
                                                     : (tailMode ? kTailChordMaxU : kChordMaxU));
            chords[i] = chordU;
            QuatW rot = (len > 1e-4f) ? QuatFromZTo(dx / len, dy / len, dz / len)
                                      : QuatW{ 1.f, 0.f, 0.f, 0.f };
            rig.bodies[i] = FingerBody{};
            rig.bodies[i].bodyMem = CreateFingerBody(world, pA, rot, chordU, rU, massKg, filterInfo, gravity,
                                                     tailMode ? GarmentMaterialId() : kSkinMaterialId);
            rig.bodies[i].chordU  = chordU;
            rig.bodies[i].lastTargetU[0] = pA[0]; rig.bodies[i].lastTargetU[1] = pA[1]; rig.bodies[i].lastTargetU[2] = pA[2];
            if (!rig.bodies[i].bodyMem) { failed = true; break; }
        }
        if (failed) {
            // roll back the partial set against the SAME (still live) creation world,
            // then clear the slot's group atomic last (same ordering as DestroyRig)
            for (int i = 0; i < kMaxChords; ++i) {
                if (rig.bodies[i].bodyMem) RemoveBody(rig.bodies[i].bodyMem, world);
                rig.bodies[i] = FingerBody{};
            }
            g_rigGroups[slot].store(0, std::memory_order_relaxed);
            LogSkip("allocFail");
            return;
        }

        rig.live     = true;
        rig.tailMode = tailMode;
        rig.mode     = 0;           // LEGACY OVERRIDE caller stamps its mode after the call
        rig.tbl      = tbl;
        rig.actorId  = actor->GetFormID();
        rig.root     = root;      // compare-only
        rig.heldWorld = RE::NiPointer<RE::bhkWorld>(worldPtr);   // STRONG ref — keeps the world alive for teardown
        rig.group    = group;
        rig.lastSeenFrame = g_frame.load(std::memory_order_relaxed);
        // Stamp the player distance AT BIRTH. Creation happens after this OnPreDrive's per-slot
        // drive loop, so without this a new rig would carry d2Valid == false until the actor's
        // NEXT frame — and the budget's farthest-holder scan would see an "unknown" holder.
        // Adversarial review 2026-07-30 showed that window let a rig born this frame be evicted
        // by any in-range actor, even a FARTHER one, before it ever drove.
        rig.lastD2  = GarmentPlayerD2(actor);
        rig.d2Valid = true;

        auto* base = actor->GetActorBase();
        // chordU list covers EVERY chord this rig built (was hardcoded [0..3]; 07-15 post-mortem).
        char cds[kMaxChords * 8]; int cn = 0;
        for (int i = 0; i < nCreate && i < kMaxChords; ++i)
            cn += std::snprintf(cds + cn, sizeof(cds) - cn, "%s%.1f", i ? " " : "", chords[i]);
        cds[cn] = '\0';
        logger::info("NFING CREATE slot={} {:08X} \"{}\" tbl={} group={} bodies={} chordU=[{}] r={:.2f} world={}",
                     slot, rig.actorId, (base && base->GetFullName()) ? base->GetFullName() : "<unnamed>",
                     tbl == 1 ? "TAIL(HDTS)" : tbl == 2 ? "TAIL(HDT equipped)"
                       : tbl == 3 ? "WIG(AmberLights)"
                       : tbl == 5 ? "TAIL(vanilla/CBPC)"
                       : tbl == 6 ? "TAIL(Mal_Tail)"
                       : tbl >= kHairBase ? "HAIR" : "finger",
                     group, nCreate, cds, rU, world);
    }

    // ── DRIVE (per finger, per frame — spec §3) ──────────────────────────────
    void DriveRig(int slot, RE::Actor* actor, void* freshWorld, float deltaTime)
    {
        FingerRig& rig = g_rigs[slot];
        FingerNodes nodes = ResolveNodes(actor->Get3D(), rig.tbl);
        if (!nodes.all) {
            // Transient (mid-reload) OR the garment was UNEQUIPPED: FSMP's skeleton
            // un-merge deletes the renamed armor bones WITHOUT rebuilding the actor
            // root, so the 3dReload guard never fires and the rig would zombie —
            // orphan capsules floating at their last pose (review 2026-07-12, HIGH).
            // ~60 driven frames (≈0.7 s) of continuous misses = the garment is gone.
            LogSkip("nodesMissing");
            if (++rig.missFrames > 60) DestroyRig(slot, "nodesLost");
            return;
        }
        rig.missFrames = 0;
        // Player distance, refreshed for EVERY rig every driven frame. Deliberately NOT folded
        // into the publish election below: that block is gated on `tune.sensors > 0`, and ~60 of
        // the 239 generated hair tables declare sensors 0 (touch-only, like tbl 5). Computing it
        // there would leave those rigs pinned at FLT_MAX and the range sweep would destroy them
        // on their first frame.
        if (auto* plD = RE::PlayerCharacter::GetSingleton()) {
            const RE::NiPoint3 pp = plD->GetPosition();
            const RE::NiPoint3 ap = actor->GetPosition();
            const float dx = ap.x - pp.x, dy = ap.y - pp.y, dz = ap.z - pp.z;
            rig.lastD2 = dx * dx + dy * dy + dz * dz;
            rig.d2Valid = true;
        }
        const ChordTable tb = PairTable(rig.tbl);
        const FingerPair* pairs = tb.pairs;
        const TableTune tune = TuneOf(rig.tbl);      // per-table push gain/clamp for the sensors

        FsmpLink::PushTarget push[kMaxSensors]{};    // sensors = the table's FIRST TuneOf(tbl).sensors chords
        int nPush = 0;

        // STAGE-2 CONTACT GATE (2026-07-11): during walking, servo lag reads as capsule
        // displacement and the push force turns it into a drag on the tail (user-diagnosed
        // "stiff where the capsules are"). Only publish real pushes: a chord's displacement
        // counts ONLY while a HIGGS hand (<20u) or wielded weapon (<70u — body sits at the
        // hilt, blade reaches far) is near that chord's host bone. No gate point near =
        // zero displacement published (bone position stays fresh for the staleness check).
        float gateP[4][3]; float gateR2[4]; int nGate = 0;
        // Gate points also collected for the FINGER rig when the contact-curl is on (2026-07-16):
        // a HIGGS hand near a finger chord bypasses the curl's servo-lag gate (deliberate touch
        // during her own motion must still charge the curl).
        if (rig.tailMode || (rig.tbl == 0 && ObjectHold::NpcFingerCurlGain() > 0.01f)) {
            if (auto* hig = Interop::GetHiggs()) {
                for (bool left : { false, true }) {
                    if (hkpRigidBody* hb = HkOf(hig->GetHandRigidBody(left)); hb && IsLikelyPointer(hb)) {
                        const hkVector4& hp = hb->getPosition();
                        gateP[nGate][0] = hp(0) * kHavokToSkyrim;
                        gateP[nGate][1] = hp(1) * kHavokToSkyrim;
                        gateP[nGate][2] = hp(2) * kHavokToSkyrim;
                        gateR2[nGate] = 20.f * 20.f;
                        ++nGate;
                    }
                    if (hkpRigidBody* wb = HkOf(hig->GetWeaponRigidBody(left)); wb && IsLikelyPointer(wb)) {
                        const hkVector4& wp = wb->getPosition();
                        gateP[nGate][0] = wp(0) * kHavokToSkyrim;
                        gateP[nGate][1] = wp(1) * kHavokToSkyrim;
                        gateP[nGate][2] = wp(2) * kHavokToSkyrim;
                        gateR2[nGate] = 70.f * 70.f;
                        ++nGate;
                    }
                }
            }
        }

        // ── SENSOR SELECTION BY CONTACT, NOT BY TABLE INDEX (2026-07-30) ──────────────────
        // Was: the publishable set is the FIRST TuneOf(tbl).sensors chords — a static prefix
        // of the AUTHORING order, which is root-first. On a 200-chord wig that meant chords
        // 14..199 could never push no matter where the player actually touched, and the same
        // on any long chain. Measured: 8 of the 239 generated hair tables carry 160-200 chords
        // against a 14-sensor cap (~7%).
        //
        // The COUNT is deliberately unchanged — each table's `sensors` is a dialed quantity AND
        // the cost bound (FsmpLink's PreSink is O(bodies x targets) per physics step, and the
        // 19-chord whip regression proved that RAISING the count is dangerous). Only WHICH
        // chords fill those slots changes: the ones nearest a gate point (hand / wielded weapon)
        // win. Everything downstream is untouched — same cap, same per-chord proximity gate,
        // same contact-verified deviation latch, same force maths.
        //
        // With no gate point in range the legacy prefix is kept verbatim: nothing can be gated
        // that frame anyway (every dispU would publish as zero), so this both preserves the
        // old publish-buffer freshness exactly and costs zero ranking work on the common frame.
        bool sensorPick[kMaxChords];
        {
            const int cap = tune.sensors < kMaxSensors ? tune.sensors : kMaxSensors;
            for (int i = 0; i < tb.n && i < kMaxChords; ++i) sensorPick[i] = false;
            if (rig.tailMode && cap > 0) {
                if (nGate == 0 || cap >= tb.n) {
                    for (int i = 0; i < tb.n && i < cap; ++i) sensorPick[i] = true;   // legacy prefix
                } else {
                    // Rank by distance from each chord's HOST BONE to the nearest gate point.
                    // Ranking uses the raw bone position; the GATE TEST inside the loop is left
                    // exactly as it was (fur-line anchor tA), so a selected chord behaves
                    // identically to before — only its eligibility changed.
                    float bestD[kMaxSensors]; int bestI[kMaxSensors]; int nb = 0;
                    for (int i = 0; i < tb.n; ++i) {
                        if (!nodes.a[i]) continue;
                        const RE::NiPoint3& p = nodes.a[i]->world.translate;
                        float d2 = FLT_MAX;
                        for (int g = 0; g < nGate; ++g) {
                            const float dx = p.x - gateP[g][0], dy = p.y - gateP[g][1],
                                        dz = p.z - gateP[g][2];
                            const float t = dx * dx + dy * dy + dz * dz;
                            if (t < d2) d2 = t;
                        }
                        // insertion into an ascending top-`cap` list (cap <= 14 — trivial)
                        if (nb < cap) {
                            int k = nb++;
                            while (k > 0 && bestD[k - 1] > d2) { bestD[k] = bestD[k - 1]; bestI[k] = bestI[k - 1]; --k; }
                            bestD[k] = d2; bestI[k] = i;
                        } else if (d2 < bestD[nb - 1]) {
                            int k = nb - 1;
                            while (k > 0 && bestD[k - 1] > d2) { bestD[k] = bestD[k - 1]; bestI[k] = bestI[k - 1]; --k; }
                            bestD[k] = d2; bestI[k] = i;
                        }
                    }
                    for (int k = 0; k < nb; ++k) sensorPick[bestI[k]] = true;
                }
            }
        }

        const float invDt  = 1.f / std::max(deltaTime, 1e-4f);
        const float alpha  = ObjectHold::NpcFingerAlpha();
        const float snapU  = ObjectHold::NpcFingerSnapU();
        const float maxVel = ObjectHold::NpcFingerMaxVel();
        const bool  logOn  = ObjectHold::NpcFingerLogEnabled();
        const auto  now    = std::chrono::steady_clock::now();

        // tbl-5 ZOMBIE GUARD (review 2026-07-17): the vanilla chain is SKELETON-carried, so
        // the nodesMissing unequip detector can never fire for table 5 — the bones outlive
        // the tail mesh. Re-verify the skin bind ~2 s; 3 consecutive misses = the tail was
        // unequipped/swapped without a root rebuild -> destroy (phantom capsules riding the
        // dormant chain are exactly what the TailChainSkinned gate exists to prevent).
        if (rig.tbl == 5 && now - rig.lastSkinCheck >= std::chrono::milliseconds(2000)) {
            rig.lastSkinCheck = now;
            if (!TailChainSkinned(actor->Get3D(), nodes.a[0])) {
                if (++rig.skinMiss >= 3) { DestroyRig(slot, "skinLost"); return; }
            } else {
                rig.skinMiss = 0;
            }
        }

        for (int i = 0; i < tb.n; ++i) {
            FingerBody& fb = rig.bodies[i];
            hkpRigidBody* hk = HkOf(fb.bodyMem);
            if (!hk) continue;

            // 1-2) live chord target (world, game units). tA/tC ride the FUR LINE
            // (fur-center offset, tbl 2); the SMP push-match position stays the RAW
            // bone-axis node position (that's where the SMP rigids live).
            float tAv[3], tCv[3];
            ChordPointU(nodes.a[i], rig.tbl, i, 0, tAv);
            ChordPointU(nodes.c[i], rig.tbl, i, 1, tCv);
            const struct { float x, y, z; } tA{ tAv[0], tAv[1], tAv[2] }, tC{ tCv[0], tCv[1], tCv[2] };
            const float ddx = tC.x - tA.x, ddy = tC.y - tA.y, ddz = tC.z - tA.z;
            const float dlen = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);

            const hkVector4& bp = hk->getPosition();
            const float bx = bp(0) * kHavokToSkyrim, by = bp(1) * kHavokToSkyrim, bz = bp(2) * kHavokToSkyrim;
            const hkQuaternion& bq = hk->getRotation();
            const QuatW bodyRot{ bq.m_vec(3), bq.m_vec(0), bq.m_vec(1), bq.m_vec(2) };

            QuatW targetRot = (dlen > 1e-3f) ? QuatFromZTo(ddx / dlen, ddy / dlen, ddz / dlen)
                                             : bodyRot;   // full-fist degenerate: hold current rotation

            // 3) gap
            const float gx = tA.x - bx, gy = tA.y - by, gz = tA.z - bz;
            const float gapU = std::sqrt(gx * gx + gy * gy + gz * gz);
            const float gapM = gapU * kSkyrimToHavok;

            // ── CONTACT-VERIFIED PUSH GATE (2026-07-17, the "floaty tail" fix): the hand/weapon
            // proximity gate stays open the whole time a hand is within 20u, and the NEAR branch
            // closes only `alpha` of the gap per frame — so a moving tail carries a speed-
            // proportional lag gap that the publish turned into a velocity-opposing drag (the
            // tail fell in slow motion). The NEAR branch SETS the capsule's velocity, so an
            // unobstructed capsule moves EXACTLY lastCmdVelU x dt; a predicted-vs-actual motion
            // deviation means something physically obstructed or shoved it. Latch + hold keeps
            // the force continuous through a moving push. Per-capsule, main thread only — no
            // collision-callback work (the pitfall ledger's pure-integer rule untouched). At
            // rest the command is ~0 and a merely-resting contact produces no deviation, so
            // own-body/world contact does not latch until it actually deflects the servo.
            bool contactLatched = false;
            if (rig.tailMode) {
                const std::uint64_t frNow = g_frame.load(std::memory_order_relaxed);
                if (fb.predFrame != 0 && frNow == fb.predFrame + 1) {
                    // dt-FREE deviation: an unobstructed hard-keyframed body lands exactly on
                    // the commanded position regardless of frame-time jitter / reprojection /
                    // sgtm (velocity x dt reconstruction false-fired on every dt change).
                    const float dvX = bx - fb.lastCmdPosU[0];
                    const float dvY = by - fb.lastCmdPosU[1];
                    const float dvZ = bz - fb.lastCmdPosU[2];
                    const float devU = ObjectHold::FsmpContactDevU();
                    if (dvX * dvX + dvY * dvY + dvZ * dvZ > devU * devU) {
                        contactLatched = true;   // the deviating frame itself always publishes
                        fb.contactUntil = now + std::chrono::milliseconds(
                            (int)ObjectHold::FsmpContactHoldMS());
                    }
                }
                contactLatched = contactLatched || now < fb.contactUntil;
            }

            // ── TOUCH WRITE-BACK (2026-07-13, finger rig only; RESTORED 2026-07-13 after the
            // multi-rig refactor dropped it — the transformation contract didn't list it):
            // the capsule's contact displacement moves the finger BONES — tip full share,
            // knuckle half, clamped 2u, written every frame ON TOP of the animation pose
            // (which re-poses fingers each frame — the heel-fix decoration pattern, no
            // accumulation/feedback). Skinned mesh follows the node worlds = the finger
            // visibly yields when its capsule is pressed.
            // ── CONTACT-CURL INTEGRATOR (2026-07-16): sustained capsule displacement charges a
            // per-finger curl factor; contact gone -> it relaxes. The factor is read by PPBHook's
            // rotation writer (bind/anim-relative RotX on all 3 finger segments, the AIHands
            // FP_Fist-share recipe) — the finger CONFORMS to what pressed it instead of only
            // shifting. Leaky integrator (not proportional) so the equilibrium is "curled until
            // the displacement nulls" — the same fixed point as AIHands' curl-until-touch, with
            // the physics capsule as the sensor. EMA 0.35 = the AIHands smoothing constant.
            if (rig.tbl == 0 && i < 4) {
                const float cGain = ObjectHold::NpcFingerCurlGain();
                if (cGain > 0.01f) {
                    const float dz = 0.15f;                              // the write-back deadzone
                    // ── SERVO-LAG GATE (the ledger's "displacement sensor reads MOTION" trap —
                    // the exact failure the tail's contact gate was built for): a swinging arm's
                    // finger chords move fast and the capsules LAG = displacement with no touch.
                    // Charge only while the chord itself is quasi-static (< lagGate u/s vs last
                    // frame's target) OR a HIGGS hand/weapon is deliberately near this chord.
                    bool nearHand = false;
                    for (int g = 0; g < nGate; ++g) {
                        const float qx = tA.x - gateP[g][0], qy = tA.y - gateP[g][1], qz = tA.z - gateP[g][2];
                        if (qx * qx + qy * qy + qz * qz < gateR2[g]) { nearHand = true; break; }
                    }
                    const float lagGate = ObjectHold::NpcFingerCurlLagGate();
                    bool chargeOk = true;
                    if (!nearHand && lagGate > 0.01f) {
                        const float mx = tA.x - fb.lastTargetU[0], my = tA.y - fb.lastTargetU[1],
                                    mz = tA.z - fb.lastTargetU[2];
                        const float chordSpdU = std::sqrt(mx * mx + my * my + mz * mz) * invDt;
                        chargeOk = chordSpdU < lagGate;                  // fast chord = lag, not contact
                    }
                    if (gapU > dz && gapU < 20.f && chargeOk)
                        rig.curlT[i] += cGain * (gapU - dz) * deltaTime;
                    else
                        rig.curlT[i] -= ObjectHold::NpcFingerCurlDecay() * deltaTime;
                    const float cMax = ObjectHold::NpcFingerCurlMax();
                    rig.curlT[i] = rig.curlT[i] < 0.f ? 0.f : (rig.curlT[i] > cMax ? cMax : rig.curlT[i]);
                } else {
                    rig.curlT[i] = 0.f;                                  // knob off -> drain immediately
                }
                rig.curlA[i] += (rig.curlT[i] - rig.curlA[i]) * 0.35f;
                if (rig.curlA[i] < 0.0005f) rig.curlA[i] = 0.f;          // snap tiny residue to a clean zero
            }
            if (rig.tbl == 0 && gapU > 0.15f && gapU < 20.f) {
                const float follow = ObjectHold::NpcFingerFollow();
                if (follow > 0.01f) {
                    const float lim = 2.f;
                    // As the curl takes over (the bones themselves conform), fade the translation
                    // shove out so the same displacement isn't applied twice (shift + curl).
                    const float fEff = follow * (1.f - (i < 4 ? rig.curlA[i] : 0.f));
                    float sx = -gx * fEff, sy = -gy * fEff, sz = -gz * fEff;
                    const float sm = std::sqrt(sx * sx + sy * sy + sz * sz);
                    if (sm > lim) { const float f = lim / sm; sx *= f; sy *= f; sz *= f; }
                    if (nodes.c[i]) {
                        nodes.c[i]->world.translate.x += sx;
                        nodes.c[i]->world.translate.y += sy;
                        nodes.c[i]->world.translate.z += sz;
                    }
                    if (nodes.a[i]) {
                        nodes.a[i]->world.translate.x += sx * 0.5f;
                        nodes.a[i]->world.translate.y += sy * 0.5f;
                        nodes.a[i]->world.translate.z += sz * 0.5f;
                    }
                }
            }

            // STAGE-2 PUSH SENSOR (tail mode): the capsule displacement off its chord
            // host = the push imparted by whatever HIGGS-side object is in contact.
            // Published every frame; FsmpLink deadzones + stales it.
            if (rig.tailMode && i < kMaxChords && sensorPick[i] && nPush < kMaxSensors) {
                bool gated = false;
                for (int g = 0; g < nGate; ++g) {
                    const float ddx = tA.x - gateP[g][0], ddy = tA.y - gateP[g][1], ddz = tA.z - gateP[g][2];
                    if (ddx * ddx + ddy * ddy + ddz * ddz < gateR2[g]) { gated = true; break; }
                }
                // match position = RAW bone-axis node position (SMP rigids sit on the
                // bone, not the fur line); displacement = capsule vs its OWN fur-line
                // anchor tA (measuring vs the raw bone would read the fur offset as a
                // permanent ~5u phantom push).
                const RE::NiPoint3& rawA = nodes.a[i]->world.translate;
                push[nPush].bonePosU[0] = rawA.x; push[nPush].bonePosU[1] = rawA.y; push[nPush].bonePosU[2] = rawA.z;
                // 2026-07-17: BOTH gates — proximity (a hand/weapon is around) AND verified
                // contact (the capsule is actually being obstructed). Proximity alone published
                // the servo's lag gap as drag on the tail's own motion (the floaty-tail bug).
                if (gated && (contactLatched || ObjectHold::FsmpContactGate() < 0.5f)) {
                    push[nPush].dispU[0] = bx - tA.x; push[nPush].dispU[1] = by - tA.y; push[nPush].dispU[2] = bz - tA.z;
                }   // else: dispU stays {0,0,0} — near-nothing / touch-nothing = push-nothing
                // per-target feel (2026-07-13): the locked per-table gain/clamp ride
                // WITH the target; FsmpLink scales both by the global fsmpPushMult.
                push[nPush].force   = tune.force;
                push[nPush].maxF    = tune.maxF;
                push[nPush].massRef = tune.massRef;
                ++nPush;
            }

            // ── SILENT-CONTACT gate (2026-07-15): is a HIGGS hand/weapon near THIS chord? (garment
            // rigs only). Reuses the same proximity points the push sensor uses; drives the NEAR-branch
            // velocity clamp below so a held/brushed garment capsule stays SILENT and doesn't kick.
            bool chordGated = false;
            if (rig.tailMode) {
                for (int g = 0; g < nGate; ++g) {
                    const float hx = tA.x - gateP[g][0], hy = tA.y - gateP[g][1], hz = tA.z - gateP[g][2];
                    if (hx * hx + hy * hy + hz * hz < gateR2[g]) { chordGated = true; break; }
                }
            }

            // 5-precheck) implied drive speed (m/s) — above the cap, take the far branch
            // instead (the mandatory ApplyHardKeyframeVelocityClamped lesson, Risk 5)
            const float impliedVel = gapM * alpha * invDt;
            const bool  far = (gapU > snapU) || (impliedVel > maxVel);

            // 6) REACT residual (review Defect 8: needs small gap LAST frame AND THIS
            // frame, no teleport this frame, and a 0.5 s per-finger cooldown)
            const float rx = bx - fb.lastTargetU[0], ry = by - fb.lastTargetU[1], rz = bz - fb.lastTargetU[2];
            const float residualU = std::sqrt(rx * rx + ry * ry + rz * rz);
            const bool  gapSmallNow = gapU < kReactGapU;
            if (logOn && fb.hadSmallGap && gapSmallNow && !far &&
                residualU > kReactResidualU && now - fb.lastReact > std::chrono::milliseconds(500)) {
                fb.lastReact = now;
                logger::info("NFING REACT {:08X} {}: residual={:.2f}u (external impulse — expected: player hand contact)",
                             rig.actorId, pairs[i].label, residualU);
            }

            hkVector4 targetPos;
            targetPos.set(tA.x * kSkyrimToHavok, tA.y * kSkyrimToHavok, tA.z * kSkyrimToHavok, 0.f);
            hkQuaternion targetRotHk;
            targetRotHk.set(targetRot.x, targetRot.y, targetRot.z, targetRot.w);

            if (far) {
                // FAR branch — direct set, then keyframe-to-self to ZERO both velocities
                // (the teleport-then-keyframe idiom; direct-set on world-resident bodies
                // has HIGGS precedent). freshWorld == the rig's live world (caller-verified).
                {
                    WorldWriteLock lock(freshWorld);
                    fn_setPosition()(static_cast<hkpEntity*>(hk), targetPos);
                    fn_setRotation()(static_cast<hkpEntity*>(hk), targetRotHk);
                }
                fn_applyHardKeyFrame()(targetPos, targetRotHk, invDt, hk);
                ++fb.teleports;
                fb.predFrame = 0;   // teleport = the motion prediction is void this frame
            } else {
                // NEAR branch — the soft velocity drive: close only `alpha` of the gap per
                // frame, so a contact with the infinite-effective-mass HIGGS hand visibly
                // wins for several frames before the finger settles back on the chord.
                float dcx = (tA.x - bx) * alpha, dcy = (tA.y - by) * alpha, dcz = (tA.z - bz) * alpha;
                // SILENT-CONTACT clamp (npcSilentVelMS, default 0 = OFF): when a hand is near a GARMENT
                // chord, cap the commanded displacement so the injected velocity (|disp|*invDt) stays
                // under the knob — below HIGGS's haptic gate (0.2 m/s) AND the engine's sound gate.
                // ⚠ Trade-off (why it defaults OFF): the cap creates servo LAG on a moving garment, and
                // the SAME hand/weapon proximity opens the FSMP push publish, so the lag is fed into the
                // SMP garment as push force — on the tail this dragged and whipped it (Report 15
                // post-mortem). Dial live only while watching for that feedback.
                const float silentVelMS = ObjectHold::NpcSilentVelMS();
                if (rig.tailMode && chordGated && silentVelMS > 0.01f) {
                    const float maxDispU = silentVelMS * kHavokToSkyrim * deltaTime;   // m/s -> u/frame
                    const float dmag = std::sqrt(dcx * dcx + dcy * dcy + dcz * dcz);
                    if (dmag > maxDispU && dmag > 1e-5f) {
                        const float f = maxDispU / dmag; dcx *= f; dcy *= f; dcz *= f;
                    }
                }
                const float cx = bx + dcx;
                const float cy = by + dcy;
                const float cz = bz + dcz;
                const QuatW rotCmd = QuatSlerp(bodyRot, targetRot, alpha);
                hkVector4 posCmd;
                posCmd.set(cx * kSkyrimToHavok, cy * kSkyrimToHavok, cz * kSkyrimToHavok, 0.f);
                hkQuaternion rotCmdHk;
                rotCmdHk.set(rotCmd.x, rotCmd.y, rotCmd.z, rotCmd.w);
                fn_applyHardKeyFrame()(posCmd, rotCmdHk, invDt, hk);
                if (rig.tailMode) {
                    // contact-gate prediction: (cx,cy,cz) is EXACTLY where the hard keyframe
                    // just commanded the body to land (post silent-clamp) — dt-free. Skipped
                    // when the command approaches the body's own m_maxLinearVelocity (10 m/s
                    // on light garment bodies): a solver-truncated command undershoots the
                    // prediction and would false-latch every fast frame (review 2026-07-17).
                    if (impliedVel < 9.5f) {
                        fb.lastCmdPosU[0] = cx; fb.lastCmdPosU[1] = cy; fb.lastCmdPosU[2] = cz;
                        fb.predFrame = g_frame.load(std::memory_order_relaxed);
                    } else {
                        fb.predFrame = 0;
                    }
                }
            }

            // telemetry window + bookkeeping
            fb.gapWin[fb.winN % kTrackWin] = gapU;
            fb.velWin[fb.winN % kTrackWin] = far ? 0.f : impliedVel;
            ++fb.winN;
            fb.hadSmallGap = gapSmallNow;
            fb.lastTargetU[0] = tA.x; fb.lastTargetU[1] = tA.y; fb.lastTargetU[2] = tA.z;
        }

        if (rig.tailMode && tune.sensors > 0) {
            // MULTI-RIG PUBLISH ARBITRATION (2026-07-13): FsmpLink holds ONE target
            // buffer, so only the rig on the actor NEAREST the player publishes each
            // frame (last-best-wins within the frame; FsmpLink's 250 ms staleness
            // covers ties/dropouts).
            // sensors==0 rigs (tbl 5, touch-only) are EXCLUDED from the election
            // (review 2026-07-17): a sensor-less winner publishes nothing, the buffer
            // goes stale, and every pushing actor's force dies while it stays nearest.
            const float d2 = rig.lastD2;   // computed once per drive above (all rig types)
            const std::uint64_t fr = g_frame.load(std::memory_order_relaxed);
            if (g_pubFrame != fr) { g_pubFrame = fr; g_pubBest = FLT_MAX; g_pubActor = 0; g_pubN = 0; }
            // 2026-07-13 review: same-actor garment rigs MERGE their sensors (up to
            // FsmpLink's kMaxSensors) instead of tying on identical d2 — wig + dress both push.
            // A strictly nearer ACTOR resets the stage and takes over.
            // ⚠ 2026-07-16 (07-17: stage now 14 slots): the fluffy tail claims 7, the wig's 4 fit
            // alongside — a tail+wig actor publishes BOTH in full. Only the 14-sensor foxtail can
            // fill the stage alone (same-actor wig then gets 0). Accepted: the tail is the
            // interaction the slots exist for. If a tail+wig NPC ever needs both pushing, raise
            // kMaxSensors AND FsmpLink's TargetBuf::t[] + g_pubStage[] together (the PreSink cost is
            // O(bodies x targets) per step — cheap, but the three sizes must move in lock-step).
            if (d2 < g_pubBest && rig.actorId != g_pubActor) {
                g_pubBest = d2; g_pubActor = rig.actorId; g_pubN = 0;
            }
            if (rig.actorId == g_pubActor && nPush > 0) {
                for (int t = 0; t < nPush && g_pubN < kMaxSensors; ++t) g_pubStage[g_pubN++] = push[t];
                FsmpLink::PublishTargets(g_pubStage, g_pubN);   // accumulated set, last write wins
            }
        }

        // 1 Hz TRACK lines ("does it track": p95 gap under ~0.5u with 0 teleports =
        // riding the chord). Per-rig throttle — a shared static would let the first-
        // driven rig starve every other rig's telemetry.
        if (logOn && now - rig.lastTrack >= std::chrono::milliseconds(1000)) {
            rig.lastTrack = now;
            for (int i = 0; i < tb.n; ++i) {
                FingerBody& fb = rig.bodies[i];
                const int n = std::min(fb.winN, kTrackWin);
                if (n < 1) continue;
                std::array<float, kTrackWin> tmp{};
                float sum = 0.f, mx = 0.f;
                for (int k = 0; k < n; ++k) { tmp[k] = fb.gapWin[k]; sum += tmp[k]; mx = std::max(mx, tmp[k]); }
                std::sort(tmp.begin(), tmp.begin() + n);
                const float p95g = tmp[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
                for (int k = 0; k < n; ++k) tmp[k] = fb.velWin[k];
                std::sort(tmp.begin(), tmp.begin() + n);
                const float p95v = tmp[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
                logger::info("NFING TRACK {:08X} {}: gap avg={:.2f}u p95={:.2f}u max={:.2f}u teleports={} driveVel p95={:.2f}m/s",
                             rig.actorId, pairs[i].label, sum / n, p95g, mx, fb.teleports, p95v);
            }
        }
    }

}  // namespace

namespace NpcFinger {

    void ClearMeshMarkers(RE::Actor* anyActor)
    {
        // STOPPING IS NOT UNDOING (three incidents now): turning a marker knob off must
        // actively remove the parked bodies, not abandon them. Needs any live actor for the
        // world; if the stored world died (load), the bodies died with it — just forget.
        auto* cell = anyActor ? anyActor->GetParentCell() : nullptr;
        RE::bhkWorld* wp = cell ? cell->GetbhkWorld() : nullptr;
        void* world = static_cast<void*>(wp);
        if (!world || (g_meshMarkerWorld && g_meshMarkerWorld != world)) {
            for (auto& b : g_meshMarker) b = nullptr;
            g_meshMarkerWorld = nullptr;
            return;
        }
        int removed = 0;
        for (auto& b : g_meshMarker)
            if (b) { RemoveBody(b, world); b = nullptr; ++removed; }
        g_meshMarkerWorld = nullptr;
        if (removed) logger::info("MESHMARK: {} parked marker(s) actively removed (knob-off)", removed);
    }

    void UpdateMeshMarkers(RE::Actor* actor, const float posU[][3], int count)
    {
        UpdateMeshMarkersImpl(actor, posU, count);   // impl lives in the file-local namespace
    }

    // ════════════════ GHOST ZONES + MOUTH TOUCH (2026-07-24, ReTouch Path B) ════════════════
    // The first Havok-native touch feature: player finger inside an NPC's mouth -> her lips form
    // an O (facegen phoneme). CBPC is not involved anywhere — detection is PURE GEOMETRY computed
    // per frame from bodies we own:
    //   * the finger = our HandBox index box (fallback: the player's finger node),
    //   * the mouth  = a knob-defined GHOST capsule in the head BODY frame (no Havok object at
    //     all — a virtual capsule that therefore tracks the head EXACTLY and can never push,
    //     orphan, or cost physics time),
    //   * breast/butt ghost zones = the LIVE driven capsules (spine2 C11/12, com C16/17) plus a
    //     pad — so they inherit ReScale + ReShape + per-NPC fits automatically, which is the
    //     whole point of Path B.
    // Visual markers refresh at ~4 Hz for the dial session (they lag a fast-moving body a beat;
    // the DETECTION never lags — it is recomputed from live transforms every frame).

    // ── THE CAPSULE NAME TABLE (2026-07-29, Report 15 + capsule_body_part_map.json) ─────────
    // Proposed body-part name per slot.child on the HUMAN female skeleton. GEO-classified names
    // are the hypothesis the mapping HUD exists to confirm — the user touches, reads, corrects.
    // nullptr = seed/duplicate (filtered; never shown).
    static const char* ProposedPartName(int slot, int child)
    {
        switch (slot) {
        // C3/C4 are NOT seeds — user-confirmed 2026-07-29: both sit at the CENTRE of the palm,
        // co-located with the palm rod. Named so the API never reports a live capsule as unnamed.
        case 0: { static const char* n[] = {"palm rod","thumb-side rod","pinky-side rod",
                  "palm centre (1)","palm centre (2)"};
                  return child < 5 ? n[child] : nullptr; }
        case 1: { static const char* n[] = {"forearm (elbow half)","forearm (wrist half)"};
                  return child < 2 ? n[child] : nullptr; }
        // C3 is LIVE on all four skeletons (not a seed): a thinner twin co-located with C1
        // (same endpoints, r 2.20 vs 2.60). Named so the API never reports it as unknown.
        case 2: { static const char* n[] = {"upper arm (shoulder half)","upper arm (elbow half)",
                  "deltoid / shoulder cap","upper arm (elbow half, inner twin)"};
                  return child < 4 ? n[child] : nullptr; }
        case 3: { static const char* n[] = {"cranium","upper lip","chin R","chin L","cheek R","cheek L",
                  "cheekbone R","cheekbone L","nose","palate","throat wall","under-jaw (deep)",
                  "temple/ear R","temple/ear L","back of head"};
                  return child < 15 ? n[child] : nullptr; }          // C15..C22 = buried seeds
        case 4: { static const char* n[] = {"waist ring","lower belly R","lower belly L","front waist band",
                  "lower abdomen","lower back R","lower back L","flank R"};
                  return child < 8 ? n[child] : nullptr; }
        case 5: { static const char* n[] = {"belly / navel (FRONT)","belly side R","belly side L","midriff R",
                  "midriff L","flank R","flank L",nullptr,nullptr,"BACK (lower)",nullptr};
                  return child < 11 ? n[child] : nullptr; }
        case 6: { static const char* n[] = {"chest ring","lat R","lat L","front rib R","front rib L",
                  "lower chest","trapezius R","trapezius L","collarbone R","collarbone L",
                  "sternum / cleavage","BREAST R","BREAST L","shoulder cap R","shoulder cap L",
                  nullptr,nullptr,"BACK (upper)",nullptr};
                  return child < 19 ? n[child] : nullptr; }
        case 7: return child == 0 ? "neck / throat" : nullptr;
        case 8: { static const char* n[] = {"thigh rod","upper thigh / groin","hip-glute fold","upper thigh",
                  "mid thigh","lower thigh","knee"};
                  return child < 7 ? n[child] : nullptr; }
        case 9: { static const char* n[] = {"calf rod","upper calf","calf belly","shin (lower)","knee rear"};
                  return child < 5 ? n[child] : nullptr; }
        case 10:{ static const char* n[] = {"sole (inner)","sole (outer)","arch","ankle lock"};
                  return child < 4 ? n[child] : nullptr; }
        case 11:{ static const char* n[] = {"orifice ring (base)","orifice ring (mid)","orifice ring (upper)",
                  "inner pelvis wall R","inner pelvis wall L","pubic mound","groin crease R","groin crease L",
                  "front hip R","front hip L","rear centreline R","rear centreline L","upper glute R",
                  "upper glute L","inner rail R","inner rail L","BUTT CHEEK R","BUTT CHEEK L",
                  "hip R","hip L",
                  // C20 is LIVE on all four skeletons: a byte-exact duplicate of C10.
                  "rear centreline R (twin)",
                  "CLITORIS",
                  "vaginal opening R","vaginal opening L",
                  "cervix R","cervix L",
                  "uterus R","uterus L",
                  "anus R","anus L",
                  "rectum R","rectum L"};
                  return child < 32 ? n[child] : nullptr; }
        }
        return nullptr;
    }

    // TOUCHPROBE = the mapping tool: logs the nearest solid capsule to each fingertip at 2 Hz.
    namespace {
        struct GhostTouchState {
            RE::Actor*    best   = nullptr;   // this-frame nearest driven candidate (same-frame use only)
            float         bestD2 = FLT_MAX;
            RE::Actor*    actor  = nullptr;   // tracked NPC (compare-only across frames)
            float         phon   = 0.f;       // current phoneme value 0..1
            bool          inMouth = false;
            bool          inThroat = false;   // tip at the end-of-mouth wall (only reachable while inMouth)
            bool          lipsLogged = false;
            bool          warnedPhon = false;
            std::uint64_t lastViz = 0, lastProbe = 0;
        };
        GhostTouchState g_gt;

        inline float SegPointDistU(const float a[3], const float b[3], const float pt[3])
        {
            const float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
            const float ap[3] = { pt[0]-a[0], pt[1]-a[1], pt[2]-a[2] };
            const float L2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
            float t = L2 > 1e-6f ? (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / L2 : 0.f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const float dx = pt[0]-(a[0]+ab[0]*t), dy = pt[1]-(a[1]+ab[1]*t), dz = pt[2]-(a[2]+ab[2]*t);
            return std::sqrt(dx*dx + dy*dy + dz*dz);
        }
        bool PlayerTipU(int hand, float out[3])
        {
            if (HandBox::TipWorldU(hand, static_cast<int>(ObjectHold::MouthFingerBox()), out))
                return true;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* root = pl ? pl->Get3D() : nullptr;
            if (!root) return false;
            const char* nm = hand == 0 ? "NPC R Finger12 [RF12]" : "NPC L Finger12 [LF12]";
            auto* nd = root->GetObjectByName(RE::BSFixedString(nm));
            if (!nd) return false;
            out[0] = nd->world.translate.x; out[1] = nd->world.translate.y; out[2] = nd->world.translate.z;
            return true;
        }
        // ── TOUCH-SOURCE CLASSIFIER (2026-07-25, user question answered YES): every hand box
        // is OUR object with a known identity, so which body part of the PLAYER did the touching
        // is a per-box distance comparison, free. Box roles in the current rig: 0/1 = INDEX
        // finger pair, 2 = slab (fist mass), 3 = palm plate.
        //   FINGER = an index box clearly nearest;  PALM = the plate;  HAND = slab or a tie.
        // Returns a tag string + fills the winning box's distance.
        const char* ClassifySource(int hand, const float segA[3], const float segB[3],
                                   float segR, float* dOut)
        {
            float best = FLT_MAX, second = FLT_MAX; int bestBox = -1;
            for (int b = 0; b < 4; ++b) {
                float c[3];
                if (!HandBox::BoxCenterWorldU(hand, b, c)) { if (dOut) *dOut = FLT_MAX; return "?"; }
                const float d = SegPointDistU(segA, segB, c) - segR;
                if (d < best) { second = best; best = d; bestBox = b; }
                else if (d < second) second = d;
            }
            if (dOut) *dOut = best;
            const float margin = 1.0f;                       // "clearly nearest" separation
            if ((bestBox == 0 || bestBox == 1) && second - best > margin) return "FINGER";
            if (bestBox == 3 && second - best > margin)                    return "PALM";
            return "HAND";
        }

        // ── HEAD FAMILY (2026-07-26, the beast mouth): which face layout does this actor's
        // head list carry? Resolved from the race FEMALE skeleton filename (the same signal
        // CapFix's beast gates use): "khajiit" -> the khajiit bake, else "beast" -> the
        // argonian/shared beast bake, else human. 0 human / 1 khajiit / 2 beast-other.
        int HeadFamilyOf(RE::Actor* a)
        {
            auto* base = a ? a->GetActorBase() : nullptr;
            auto* race = base ? base->GetRace() : nullptr;
            if (!race) return 0;
            const char* mdl = race->skeletonModels[RE::SEXES::kFemale].GetModel();
            if (!mdl) return 0;
            bool kha = false, beast = false;
            for (const char* p = mdl; *p; ++p) {
                if ((*p == 'k' || *p == 'K') && _strnicmp(p, "khajiit", 7) == 0) kha = true;
                if ((*p == 'b' || *p == 'B') && _strnicmp(p, "beast", 5) == 0)   beast = true;
            }
            return kha ? 1 : (beast ? 2 : 0);
        }
    }
    static void GhostTouchFrame()
    {
        if (!ObjectHold::GhostZonesEnabled()) { g_gt = GhostTouchState{}; return; }
        RE::Actor* a = g_gt.best;  g_gt.best = nullptr;
        const float d2 = g_gt.bestD2; g_gt.bestD2 = FLT_MAX;
        const float rng = ObjectHold::GhostRangeU();
        if (!a || d2 > rng * rng) { g_gt.actor = nullptr; g_gt.inMouth = false; g_gt.inThroat = false; g_gt.phon = 0.f; return; }
        if (a != g_gt.actor) {
            g_gt.actor = a; g_gt.inMouth = false; g_gt.inThroat = false; g_gt.phon = 0.f;
            logger::info("GHOST tracking {:08X}", a->GetFormID());
        }
        // marker knob falling edge -> ACTIVELY remove parked bodies (the seizure lesson)
        {
            static bool s_prevMk = true;
            const bool mk = ObjectHold::MeshMarkersEnabled() || ObjectHold::GhostVizEnabled();
            if (s_prevMk && !mk) ClearMeshMarkers(a);
            s_prevMk = mk;
        }
        // ── THE MOUTH GATE — REAL CAPSULES ONLY (2026-07-25, user architecture + user map).
        // The virtual mouth ghost is DELETED. Detection = the finger's distance to the NPC's
        // actual welded head capsules, eye-mapped by flash-probe:
        //   C9 = PALATE, C4/C5 = CHEEKS, C1 = UPPER LIP, C2/C3 = CHIN.
        //   IN-MOUTH  = within gate of C9 AND C4 AND C5   (user: "definitely inside")
        //   AT-LIPS   = within gate of C1 AND C2 AND C3   (logged stage)
        // Nothing placed, nothing tracked — a capsule can never be where the body is not.
        float tip[2][3]; bool tipOk[2];
        for (int h = 0; h < 2; ++h) tipOk[h] = PlayerTipU(h, tip[h]);
        float ca[3], cb[3], cr;
        auto childDist = [&](int child, const float pt[3]) -> float {
            if (!GrabDiag::ReadCapsuleWorldU(a, 3, child, ca, cb, &cr)) return FLT_MAX;
            return SegPointDistU(ca, cb, pt) - cr;
        };
        // Per-family child sets (2026-07-26): human = the eye-mapped C9/C4/C5 entry + C2/C3
        // chins + palate loose-hold + throat wall; beasts = their own TOUCHPROBE-mapped BAKED
        // capsules (user design: "if all three detect the finger box, open the mouth") —
        // Khajiit muzzle trio 6/7/8 + chins 2/3, Argonian lip+muzzle 1/6/8 + chins 2/4. No
        // palate condition and no throat event on beasts (their C9/C10 are other anatomy).
        const int  fam     = HeadFamilyOf(a);
        const bool isBeast = fam != 0;
        const int  eA  = isBeast ? ObjectHold::MouthBeastChild(fam == 1, 0) : 9;
        const int  eB  = isBeast ? ObjectHold::MouthBeastChild(fam == 1, 1) : 4;
        const int  eC  = isBeast ? ObjectHold::MouthBeastChild(fam == 1, 2) : 5;
        const int  ch1 = isBeast ? ObjectHold::MouthBeastChild(fam == 1, 3) : 2;
        const int  ch2 = isBeast ? ObjectHold::MouthBeastChild(fam == 1, 4) : 3;
        const float gate = isBeast ? ObjectHold::MouthBeastEnterU() : ObjectHold::MouthEnterU();
        float bestIn = FLT_MAX; int bestHand = -1; bool anyLips = false;
        for (int h = 0; h < 2; ++h) {
            if (!tipOk[h]) continue;
            const float dEA = childDist(eA, tip[h]);
            const float dEB = childDist(eB, tip[h]);
            const float dEC = childDist(eC, tip[h]);
            const float dIn = std::max(dEA, std::max(dEB, dEC));   // ALL three must be close
            if (dIn < bestIn) { bestIn = dIn; bestHand = h; }
            if (!isBeast) {   // AT-LIPS logged stage — human lip/chin indices only
                const float dLip  = childDist(1, tip[h]);
                const float dJawR = childDist(2, tip[h]);
                const float dJawL = childDist(3, tip[h]);
                anyLips = anyLips || std::max(dLip, std::max(dJawR, dJawL)) < gate;
            }
        }
        // finger-only requirement (user: "make sure it's detecting the finger, not the palm"):
        // the INDEX boxes must be the clearly-nearest part of the hand to the PRIMARY entry
        // capsule (human palate C9 / the family's first entry child).
        bool fingerOk = true;
        if (bestHand >= 0 && ObjectHold::MouthNeedFinger() &&
            GrabDiag::ReadCapsuleWorldU(a, 3, eA, ca, cb, &cr)) {
            float dSrc;
            const char* tag = ClassifySource(bestHand, ca, cb, cr, &dSrc);
            fingerOk = (tag[0] == 'F') || dSrc == FLT_MAX;   // FLT_MAX = rig not live -> node
        }                                                     // fallback, no boxes to classify
        // ── DEEP HOLD (2026-07-25, the user's palate+chin insight): the entry capsules sit
        // FORWARD in the mouth — a finger pushed all the way in leaves their range and the O
        // used to snap shut. The chin lines C2/C3 are the capsules that span the deep region
        // (their far ends reach Y~2), so: ENTER only through the front (strict palate+cheeks —
        // a hand cupping the jaw still can't open her), but once in, STAY while both chins are
        // close AND the palate is within a loose range (from under the jaw, outside, the palate
        // is far — that asymmetry is what separates deep-inside from below-the-jaw).
        // THROAT WALL (2026-07-26): C10 (knob mouthThroatChild) repositioned as the END of the
        // mouth cavity. Near it = unambiguously deep — it holds the mouth open BY ITSELF (the
        // user's back-of-mouth close bug: chins+palate lost the tip at the very back), and it
        // raises the THROAT event. Only evaluated while inMouth (enter-through-the-front-only).
        bool deepHold = false;
        float throatBest = FLT_MAX;
        if (g_gt.inMouth) {
            const int thCh = ObjectHold::MouthThroatChild();
            for (int h = 0; h < 2; ++h) {
                if (!tipOk[h]) continue;
                const float dJr = childDist(ch1, tip[h]);
                const float dJl = childDist(ch2, tip[h]);
                if (isBeast) {   // beasts: chins alone hold — no palate capsule, no throat wall
                    if (std::max(dJr, dJl) < ObjectHold::MouthDeepChinU()) deepHold = true;
                    continue;
                }
                const float dPl = childDist(9, tip[h]);
                const float dTh = childDist(thCh, tip[h]);
                if (dTh < throatBest) throatBest = dTh;
                if ((std::max(dJr, dJl) < ObjectHold::MouthDeepChinU() &&
                     dPl < ObjectHold::MouthDeepPalU()) ||
                    dTh < ObjectHold::MouthThroatU()) deepHold = true;
            }
        }
        const bool wasIn = g_gt.inMouth;
        if (!g_gt.inMouth) {
            if (bestHand >= 0 && fingerOk && bestIn < gate) g_gt.inMouth = true;
        } else {
            const bool stayFront = bestHand >= 0 && bestIn < ObjectHold::MouthExitU();
            if (!stayFront && !deepHold) g_gt.inMouth = false;
        }
        if (g_gt.inMouth != wasIn)
            logger::info("MOUTHTOUCH {:08X} {} hand={} worstOf3={:.2f}u", a->GetFormID(),
                         g_gt.inMouth ? "ENTER" : "EXIT", bestHand == 0 ? "R" : "L", bestIn);
        // THROAT event — "something reached the end of her throat". Future reaction hook
        // (gag phoneme, VRTouchEvents_PPBTouch stage code); log-only for now.
        {
            const bool wasThroat = g_gt.inThroat;
            g_gt.inThroat = g_gt.inMouth && throatBest < ObjectHold::MouthThroatU();
            if (g_gt.inThroat != wasThroat)
                logger::info("MOUTHTOUCH {:08X} THROAT {} d={:.2f}u", a->GetFormID(),
                             g_gt.inThroat ? "REACHED" : "LEFT",
                             throatBest == FLT_MAX ? -1.f : throatBest);
        }
        if (anyLips && !g_gt.inMouth && !g_gt.lipsLogged) {
            g_gt.lipsLogged = true;
            logger::info("MOUTHTOUCH {:08X} LIPS (C1+C2+C3 all within {:.2f}u)", a->GetFormID(), gate);
        } else if (!anyLips) g_gt.lipsLogged = false;
        // ── WIDE-O BLEND (2026-07-25): "Oh" rounds the lips but barely drops the jaw. A real
        // open mouth is Oh + BigAah underneath. g_gt.phon is a NORMALIZED 0..1 ramp; each
        // channel applies its own max — one motion, two morphs.
        const float dt   = 1.f / 90.f;
        const float tgt  = g_gt.inMouth ? 1.f : 0.f;
        const float rate = g_gt.inMouth ? ObjectHold::MouthRampIn() : ObjectHold::MouthRampOut();
        if (g_gt.phon < tgt)      { g_gt.phon += rate * dt; if (g_gt.phon > tgt) g_gt.phon = tgt; }
        else if (g_gt.phon > tgt) { g_gt.phon -= rate * dt; if (g_gt.phon < tgt) g_gt.phon = tgt; }
        if (auto* fad = a->GetFaceGenAnimationData()) {
            const auto i1 = static_cast<std::uint32_t>(ObjectHold::MouthPhonemeIdx());
            const auto i2 = static_cast<std::uint32_t>(ObjectHold::MouthPhoneme2Idx());
            const auto cnt = fad->phenomeKeyFrame.count;
            if (i1 < cnt) fad->phenomeKeyFrame.SetValue(i1, g_gt.phon * ObjectHold::MouthPhonemeMax());
            if (i2 < cnt && i2 != i1)
                fad->phenomeKeyFrame.SetValue(i2, g_gt.phon * ObjectHold::MouthPhoneme2Max());
            if (i1 >= cnt && !g_gt.warnedPhon) {
                g_gt.warnedPhon = true;
                logger::info("MOUTHTOUCH {:08X} phoneme keyframe count={} — idx {} unreachable",
                             a->GetFormID(), cnt, i1);
            }
        }
        const std::uint64_t fr = g_frame.load(std::memory_order_relaxed);
        // ── viz markers @ ~4 Hz: mouth ghost A/B + the four body ghost-zone centers
        if (ObjectHold::GhostVizEnabled() && fr - g_gt.lastViz >= 22) {
            g_gt.lastViz = fr;
            float pts[8][3]; int n = 0;
            static constexpr struct { int slot, child; } kGz[4] = { {6,11},{6,12},{11,16},{11,17} };
            for (auto& z : kGz)
                if (GrabDiag::ReadCapsuleWorldU(a, z.slot, z.child, ca, cb, &cr)) {
                    for (int i = 0; i < 3; ++i) pts[n][i] = (ca[i] + cb[i]) * 0.5f;
                    ++n;
                }
            if (n) UpdateMeshMarkersImpl(a, pts, n);
        }
        // ── TOUCHPROBE @ ~2 Hz: the CC-mapping tool — nearest solid capsule per fingertip
        if (ObjectHold::TouchProbeEnabled() && fr - g_gt.lastProbe >= 45) {
            g_gt.lastProbe = fr;
            for (int h = 0; h < 2; ++h) {
                if (!tipOk[h]) continue;
                float bd = FLT_MAX; int bs = -1, bc = -1; bool bl = false;
                for (int sl = 0; sl < 12; ++sl) {
                    const int nch  = GrabDiag::SlotLiveChildren(a, sl);
                    const int cmax = nch > 0 ? nch : 1;
                    const int sides = GrabDiag::SlotHasLeftTwin(sl) ? 2 : 1;   // 2026-07-29: L twins probed too
                    for (int sd = 0; sd < sides; ++sd)
                        for (int c = 0; c < cmax; ++c) {
                            if (!GrabDiag::ReadCapsuleWorldUSide(a, sl, sd == 1, nch > 0 ? c : 0, ca, cb, &cr)) continue;
                            const float d = SegPointDistU(ca, cb, tip[h]) - cr;
                            if (d < bd) { bd = d; bs = sl; bc = c; bl = (sd == 1); }
                        }
                }
                if (bs >= 0 && bd < ObjectHold::TouchProbeRangeU()) {
                    const char* tag = "?";
                    if (GrabDiag::ReadCapsuleWorldUSide(a, bs, bl, bc, ca, cb, &cr))
                        tag = ClassifySource(h, ca, cb, cr, nullptr);
                    const char* sideTag = GrabDiag::SlotHasLeftTwin(bs) ? (bl ? "[L]" : "[R]") : "";
                    logger::info("TOUCHPROBE {}[{}] {}{}.C{} d={:.2f}u", h == 0 ? "R" : "L",
                                 tag, GrabDiag::SlotLabel(bs), sideTag, bc, bd);

                    // ── MAPPING HUD (2026-07-29): the confirmation session. On actual TOUCH
                    // (deep proximity), pop the slot.child + proposed name in the player's view.
                    // "spine2 C13 - shoulder cap R" — the user confirms or corrects by voice.
                    if (ObjectHold::TouchProbeHudOn() && bd < ObjectHold::TouchProbeHudU()) {
                        static int  s_lastSlot = -1, s_lastChild = -1;
                        static bool s_lastLeft = false;
                        if (bs != s_lastSlot || bc != s_lastChild || bl != s_lastLeft) {
                            s_lastSlot = bs; s_lastChild = bc; s_lastLeft = bl;
                            const char* nm = ProposedPartName(bs, bc);
                            char msg[160];
                            std::snprintf(msg, sizeof msg, "PPB: %s%s C%d = %s  (%.1fu)",
                                          GrabDiag::SlotLabel(bs), sideTag, bc,
                                          nm ? nm : "?? (unnamed/seed)", bd);
                            RE::SendHUDMessage::ShowHUDMessage(msg);   // in-view HUD line (VR)
                            logger::info("TOUCHHUD {}", msg);
                        }
                    }
                }
            }
            // ── OBJTOUCH (2026-07-25): what is the HELD OBJECT pressed against? Identity from
            // HIGGS (GetGrabbedObject — the equip/hold system, never the mesh), position from
            // the object's own 3D root, target = nearest of HER capsules. "Sweet Roll vs
            // spine1.C1" — the answer to the food test.
            if (auto* hg = Interop::GetHiggs()) {
                for (int h = 0; h < 2; ++h) {
                    const bool isLeft = (h == 1);
                    if (!hg->IsHoldingObject(isLeft)) continue;
                    auto* ref = hg->GetGrabbedObject(isLeft);
                    if (!ref || ref->As<RE::Actor>()) continue;      // held NPCs are not objects
                    auto* o3d = ref->Get3D();
                    if (!o3d) continue;
                    // surface distance (2026-07-25): measure from the object's BOUND — centre
                    // instead of root, minus its radius — so d ~ 0 means actually touching
                    // (the dumpling read "5u at contact" measured from its root).
                    const float op[3] = { o3d->worldBound.center.x, o3d->worldBound.center.y,
                                          o3d->worldBound.center.z };
                    const float oRad = o3d->worldBound.radius;
                    float bd = FLT_MAX; int bs = -1, bc = -1;
                    for (int sl = 0; sl < 12; ++sl) {
                        const int nch  = GrabDiag::SlotLiveChildren(a, sl);
                        const int cmax = nch > 0 ? nch : 1;
                        for (int c = 0; c < cmax; ++c) {
                            if (!GrabDiag::ReadCapsuleWorldU(a, sl, nch > 0 ? c : 0, ca, cb, &cr)) continue;
                            const float d = SegPointDistU(ca, cb, op) - cr - oRad;
                            if (d < bd) { bd = d; bs = sl; bc = c; }
                        }
                    }
                    const char* nm = ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?";
                    if (bs >= 0 && bd < ObjectHold::TouchProbeRangeU() + 3.f)
                        logger::info("OBJTOUCH {} '{}' vs {}.C{} d={:.2f}u", isLeft ? "L" : "R",
                                     nm, GrabDiag::SlotLabel(bs), bc, bd);
                }
            }
        }
    }

    // ── HAIR SIGNATURE LOOKUP (2026-07-26): bind any of the 239 SMP wigs in ONE tree walk instead of a
    // 239-table linear probe. Map each wig's signature bone -> its index; DFS the actor collecting each
    // node's PLAIN name (AutoRename armor prefix stripped), and on a signature hit VERIFY the full table
    // resolves (ResolveNodes) before binding — that verify is what makes the KS shared-name signature
    // collisions safe (only the wig whose whole bone set is present binds). Returns the hair index or -1.
    const std::unordered_map<std::string_view, std::vector<int>>& HairSigMap() {
        static const std::unordered_map<std::string_view, std::vector<int>> m = [] {
            std::unordered_map<std::string_view, std::vector<int>> mm;
            for (int i = 0; i < kHairCount; ++i) mm[std::string_view(kHairTables[i].sig)].push_back(i);
            return mm;
        }();
        return m;
    }
    int FindHairDFS(RE::NiAVObject* obj, RE::NiAVObject* root, int* sigHits, int depth = 0) {
        if (!obj || depth > 40) return -1;
        if (const char* nm = obj->name.c_str(); nm && *nm) {
            // Engine-agnostic un-rename (FSMP's "AutoRename_…XXXXXXXX <orig>" AND Flex's
            // "hdtA_XXXXXXXX_<orig>"): without the Flex branch every wig signature bone
            // missed under Flex and no hair table could ever bind. See PlainBoneName.
            const char* plain = PlainBoneName(nm);
            auto it = HairSigMap().find(std::string_view(plain));
            if (it != HairSigMap().end()) {
                if (sigHits) *sigHits += (int)it->second.size();
                for (int idx : it->second)
                    if (ResolveNodes(root, kHairBase + idx).all) return idx;
            }
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren())
                if (int h = FindHairDFS(ch.get(), root, sigHits, depth + 1); h >= 0) return h;
        return -1;
    }
    inline int FindHairTable(RE::NiAVObject* root, int* sigHits = nullptr) { return FindHairDFS(root, root, sigHits); }

    // ── GARMENT-RIG BUDGET (2026-07-30, knobs npcRigRangeU / npcRigMaxActors) ────────────
    // Split deliberately into a PURE predicate and a COMMITTING acquire.
    //
    // ⚠ The first version was one function used as the auto-probe branch gate, and it evicted
    // as a SIDE EFFECT of being asked. OnPreDrive runs for EVERY driven actor EVERY frame, and
    // the probe behind the gate usually creates NOTHING (it is throttled, and most NPCs match
    // no table — that is what the HAIRSCAN "no bind" receipt records). So a plain guard with no
    // hair or tail at all would destroy a real 200-chord wig rig it could never use, the victim
    // would rebuild ~200 frames later, and the guard would kill it again — a permanent
    // create/destroy cycle. DestroyRig frees nothing (never-free convention), so each cycle
    // leaked the whole rig. Found by adversarial review 2026-07-30 before it ever shipped.
    //
    // RULE: never let a query mutate lifecycle. Range gates the probe (pure); the budget is
    // acquired only once a table has actually resolved and we are about to TryCreate.
    // PURE. Cheap enough to gate the per-frame probe with.
    bool GarmentInRange(RE::Actor* actor)
    {
        const float range = ObjectHold::NpcRigRangeU();
        if (range <= 0.f) return true;                  // 0 = unlimited
        return GarmentPlayerD2(actor) <= range * range;
    }

    // COMMITTING: may evict. Call ONLY immediately before a TryCreate that will definitely run.
    bool GarmentBudgetAcquire(RE::Actor* actor, std::uint32_t id)
    {
        if (!GarmentInRange(actor)) return false;
        const int maxA = ObjectHold::NpcRigMaxActors();
        if (maxA <= 0) return true;                     // 0 = unlimited (kMaxRigs still caps)
        const float d2 = GarmentPlayerD2(actor);

        std::uint32_t holders[kMaxRigs]; int nH = 0;
        for (int k = 0; k < kMaxRigs; ++k) {
            const auto& r = g_rigs[k];
            if (!r.live || r.tbl == 0) continue;        // finger rig is not on this budget
            if (r.actorId == id) return true;           // same actor, 2nd table (tail+wig) — free
            bool seen = false;
            for (int h = 0; h < nH; ++h) if (holders[h] == r.actorId) { seen = true; break; }
            if (!seen && nH < kMaxRigs) holders[nH++] = r.actorId;
        }
        if (nH < maxA) return true;

        // Farthest holder among those with a known distance. TryCreate stamps lastD2/d2Valid at
        // birth, so "unknown" should be unreachable — if it ever happens we REFUSE rather than
        // evict blind. (The first version treated unknown as "farthest, evict freely, skip the
        // hysteresis", which made a rig born this frame the preferred victim of any in-range
        // actor — even one FARTHER away. Second review finding, same root cause: don't guess.)
        std::uint32_t worstId = 0; float worstD2 = -1.f;
        for (int h = 0; h < nH; ++h) {
            float hd2 = -1.f;
            for (int k = 0; k < kMaxRigs; ++k) {
                const auto& r = g_rigs[k];
                if (!r.live || r.tbl == 0 || r.actorId != holders[h] || !r.d2Valid) continue;
                if (r.lastD2 > hd2) hd2 = r.lastD2;
            }
            if (hd2 >= 0.f && hd2 > worstD2) { worstD2 = hd2; worstId = holders[h]; }
        }
        if (worstId == 0) return false;                 // no comparable holder — do not evict

        const float hyst = ObjectHold::NpcRigRangeHystU();
        if (std::sqrt(worstD2) - std::sqrt(d2) <= hyst) return false;   // not CLEARLY nearer
        for (int k = 0; k < kMaxRigs; ++k)
            if (g_rigs[k].live && g_rigs[k].tbl != 0 && g_rigs[k].actorId == worstId)
                DestroyRig(k, "budget");
        return true;
    }

    void OnPreDrive(RE::Actor* actor, float deltaTime)
    {
        if (!actor) return;
        // ReTouch: nearest-driven-actor election for the ghost-zone tracker (runs BEFORE the
        // idle early-return below — ghost zones are independent of the finger-test knobs).
        if (ObjectHold::GhostZonesEnabled()) {
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                const RE::NiPoint3 pp = pl->GetPosition(), ap = actor->GetPosition();
                const float dx = ap.x-pp.x, dy = ap.y-pp.y, dz = ap.z-pp.z;
                const float dd = dx*dx + dy*dy + dz*dz;
                if (dd < g_gt.bestD2) { g_gt.bestD2 = dd; g_gt.best = actor; }
            }
        }
        const std::uint32_t pin = g_pinnedId.load(std::memory_order_relaxed);
        bool anyLive = false;
        for (int k = 0; k < kMaxRigs; ++k) if (g_rigs[k].live) { anyLive = true; break; }
        // cheap idle path: no rig, no pin, all master knobs off, no census armed
        if (!anyLive && pin == 0 && !ObjectHold::NpcFingerEnabled() && !ObjectHold::NpcTailTest() &&
            !ObjectHold::NpcFollowerEnabled() && !g_censusArmed.load(std::memory_order_relaxed)) return;

        // Knob cache refresh #2 (main thread): OnFrame rides a HIGGS callback, but rig CREATION
        // happens right below — the cache must be hot BEFORE a body enters the world and the
        // collision callback starts asking. Cheap (a few float reads + one store) and it makes
        // the filter correct on the very first frame a rig exists, not one frame later.
        RefreshFilterKnobs();

        const std::uint32_t id = actor->GetFormID();

        // NODE CENSUS one-shot: fires on the pinned (else nearest driven) actor.
        if (g_censusArmed.load(std::memory_order_relaxed)) {
            const std::uint32_t want = pin ? pin : PerfSys::NearestDrivenId();
            if (want != 0 && id == want) {
                g_censusArmed.store(false, std::memory_order_relaxed);
                int found = 0;
                CensusWalk(actor->Get3D(), 0, found);
                logger::info("CENSUS {:08X} done: {} node(s) matching *ail*", id, found);
            }
        }

        // Per-actor facts shared by every rig guard below.
        // NOT IsDead(): the pitfall ledger's engine trap — Actor::IsDead() is TRUE for
        // kRestrained/bleedout-class states (proven live 2026-07-09: NFING DESTROY
        // reason=death 12 ms after CREATE on a perfectly alive Carmella). Only true
        // death states end a rig.
        const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
        const auto life = actor->GetLifeState();
        const bool dead = (life == RE::ACTOR_LIFE_STATE::kDead || life == RE::ACTOR_LIFE_STATE::kDying);
        auto* root3d = actor->Get3D();
        auto* cell = actor->GetParentCell();
        void* freshWorld = cell ? static_cast<void*>(cell->GetbhkWorld()) : nullptr;

        // ── ROUTE: drive every live rig owned by this actor (per-frame guards
        // per rig — spec §5 as REWRITTEN by review Defect 1, applied slot-wise) ──
        bool garmentDestroyed = false;   // a mode-0 tbl1-5 rig died to a reload/world edge -> fast re-probe
        for (int slot = 0; slot < kMaxRigs; ++slot) {
            FingerRig& rig = g_rigs[slot];
            if (!rig.live || rig.actorId != id) continue;
            const bool wasGarment = rig.mode == 0 && rig.tbl >= 1 && rig.tbl <= 6;   // 5/6 included: a door-
                                                                                     // transition must fast-
                                                                                     // reprobe M'rissi's and
                                                                                     // Yvanni's tails too
            if (dead) {
                if (pin == id)                                    // a dead pin must not resurrect a rig
                    g_pinnedId.store(0, std::memory_order_relaxed);
                DestroyRig(slot, "death");
                continue;
            }
            rig.lastSeenFrame = frame;
            if (root3d != rig.root) {                             // compare-only, never deref'd
                DestroyRig(slot, "3dReload");                     // re-create next tick on the new tree
                if (wasGarment) garmentDestroyed = true;
                continue;
            }
            if (!freshWorld) {
                DestroyRig(slot, "noWorld");                      // mismatch path inside -> forget
                if (wasGarment) garmentDestroyed = true;
                continue;
            }
            if (freshWorld != static_cast<void*>(rig.heldWorld.get())) {
                // Cell/world transition. DestroyRig removes the bodies from rig.heldWorld
                // (kept alive by our strong reference — safe even if the engine already
                // swapped the actor's world), then releases the reference. Recreate in the
                // new world next tick.
                DestroyRig(slot, "worldChange");
                if (wasGarment) garmentDestroyed = true;
                continue;
            }
            DriveRig(slot, actor, freshWorld, deltaTime);
        }
        // Fast garment re-probe (2026-07-13 review LOW): a 3dReload/worldChange destroyed
        // this actor's auto-probed garment rig(s); without resetting her probe cursor the
        // recreate waits out the rest of the ~200-frame probe cycle (dead tail/hair/dress
        // physics for seconds), contradicting the destroy-site "re-create next tick"
        // comment. Reset to an immediate table-1 sweep.
        if (garmentDestroyed) {
            auto it = g_probe.find(id);
            if (it != g_probe.end()) { it->second.cursor = 1; it->second.next = frame; }
        }

        if (dead || !root3d) return;   // no creation on a dying/undressed actor

        const int mode = ObjectHold::NpcTailMode();

        if (mode > 0) {
            // ── LEGACY OVERRIDE (npcTailTest > 0): auto-probe is BYPASSED; the old
            // single-actor behavior applies for that one table — pinned (else nearest
            // driven) actor, mode -> table mapping 1->1 (fallback 2), 2->3, 3->4. ──
            const std::uint32_t desired = pin ? pin : PerfSys::NearestDrivenId();
            if (desired != 0 && id == desired) {
                bool satisfied = false;   // ONE override rig total (old single-rig semantics)
                for (int k = 0; k < kMaxRigs; ++k)
                    if (g_rigs[k].live && g_rigs[k].mode == mode) { satisfied = true; break; }
                if (!satisfied) {
                    // mode 3 (DX Necromancer dress) RETIRED 2026-07-13 -> tbl 0 = skip.
                    int tbl = mode == 2 ? 3 : (mode == 3 ? 0 : 1);
                    if (mode == 1 && !ResolveNodes(root3d, 1).all) {  // fluffy (HDTS) absent ->
                        tbl = 2;                                      // HDT-equipped chain
                        // (tbl-5 promote RETIRED 2026-07-20 — see the auto-probe site.)
                    }
                    bool dup = false;     // an auto-probed rig may already dress this (actor, tbl)
                    for (int k = 0; k < kMaxRigs; ++k)
                        if (g_rigs[k].live && g_rigs[k].actorId == id && g_rigs[k].tbl == tbl) { dup = true; break; }
                    const int freeSlot = FindFreeSlot();
                    if (tbl != 0 && !dup && freeSlot >= 0) {
                        TryCreate(actor, tbl, freeSlot);
                        if (g_rigs[freeSlot].live)
                            g_rigs[freeSlot].mode = mode;   // stamp: OnFrame's mode-flip destroy targets THIS rig
                    }
                }
            }
        } else if (ObjectHold::NpcFollowerEnabled() && GarmentInRange(actor)) {
            // ── AUTO-PROBE (2026-07-13, knob npcFollower; PERF fix audit rank 2):
            // ONE table resolve per probe tick, per-actor phase-seeded on first sight.
            // The cursor walks tables 1→3 at a 40-frame gap, then rests a full cycle
            // (~200 frames + id spread). Probe order 1 then 2: both are tails — if the
            // fluffy HDTS chain owns the actor, the HDT set is skipped. ──
            ProbeState& ps = g_probe[id];
            // Reseed a LONG-expired entry (2026-07-13 review LOW): while mode>0 (legacy
            // override bypasses this branch) or npcFollower was off, ps.next ages into the
            // deep past; when auto-probe resumes for many actors at once they would all
            // probe the same frame (thundering herd). A stale entry whose FormID was
            // RECYCLED onto a new actor also lands here. Re-stagger instead of probing.
            if (ps.seeded && frame > ps.next && (frame - ps.next) > 600) {
                ps.cursor = 1;
                ps.next   = frame + 20 + (id % 240);
            }
            if (!ps.seeded) {                      // first sight after load/spawn: stagger, don't probe
                ps.seeded = true;
                ps.cursor = 1;
                ps.next   = frame + 20 + (id % 240);
            } else if (ps.next < frame) {
                const int freeSlot = FindFreeSlot();
                if (freeSlot < 0) {                // all slots dressed — rest a full cycle
                    ps.next = frame + 200 + (id % 80);
                } else {
                    // Walk tail/wig candidates 1,2,3 (fluffy / HDT foxtail / Amber wig), 6 (Mal_Tail),
                    // then step 5 = HAIR (all 239 SMP wigs, one signature DFS). Table 4 (dress) RETIRED.
                    // ★ 2026-07-26 FIX: hair MUST be a probe STEP. The first version ran the lookup only
                    // when the walk left tbl==0 — but an unrigged actor matches a candidate every tick,
                    // and the end-of-cycle reset rewinds the cursor before a tbl==0 tick can ever occur,
                    // so the hair path was DEAD CODE (receipt: zero tbl=HAIR in the session log).
                    bool have[7] = {};
                    for (int k = 0; k < kMaxRigs; ++k)
                        if (g_rigs[k].live && g_rigs[k].actorId == id && g_rigs[k].tbl >= 1 && g_rigs[k].tbl <= 6)
                            have[g_rigs[k].tbl] = true;
                    static constexpr int kProbeOrder[6] = { 0, 1, 2, 3, 6, -1 };  // [step] 1..5; -1 = HAIR
                    constexpr int kProbeSteps = 5;
                    int tbl = 0;
                    while (ps.cursor <= kProbeSteps) {
                        const int step = ps.cursor++;
                        const int cand = kProbeOrder[step];
                        if (cand < 0) {               // HAIR step: signature DFS over the 239 wig tables
                            bool haveHair = false;
                            for (int k = 0; k < kMaxRigs; ++k)
                                if (g_rigs[k].live && g_rigs[k].actorId == id && g_rigs[k].tbl >= kHairBase) { haveHair = true; break; }
                            if (!haveHair) {
                                int sigHits = 0;
                                const int hidx = FindHairTable(root3d, &sigHits);
                                if (hidx >= 0) { tbl = kHairBase + hidx; break; }
                                // Receipt, 1x per actor per session — makes every future "why no capsules?"
                                // answerable from the log alone: sigHits=0 → none of the 239 signature bones
                                // exist in her tree (hair from an uncovered mod, or facegen-baked static);
                                // sigHits>0 → a signature matched but the full bone set failed to resolve
                                // (a table/collision defect — report it).
                                static std::unordered_set<std::uint32_t> s_hairMissLogged;
                                if (s_hairMissLogged.insert(id).second) {
                                    auto* b = actor->GetActorBase();
                                    logger::info("NFING HAIRSCAN {:08X} \"{}\": no bind (sigHits={}) — {}",
                                                 id, (b && b->GetFullName()) ? b->GetFullName() : "<unnamed>",
                                                 sigHits,
                                                 sigHits ? "signature matched but full table failed to resolve"
                                                         : "no signature bone in tree (uncovered hair mod, or facegen-baked)");
                                }
                            }
                            continue;                 // dressed already / no match — cursor moves past the step
                        }
                        if (have[cand]) continue;
                        if (cand == 2 && (have[1] || have[5])) continue;   // HDTS or dressed vanilla chain
                                                                           // owns the tail: table 2 would
                                                                           // dress its GHOST
                        tbl = cand;
                        break;
                    }
                    // ── VANILLA-CHAIN PRECEDENCE (2026-07-17, the M'rissi hand-through-tail fix):
                    // before dressing the HDT-equipped chain, check whether the actor's VISIBLE
                    // tail is skinned to the vanilla TailBone chain. M'rissi post-skeleton-swap:
                    // her naked-skin tail binds TailBone01-05, while the "HDT TailBone00X" chain
                    // table 2 matches is an invisible equipped SMP item dangling 5-40u UNDER the
                    // real tail (the 2026-07-12 census) — her old rig tracked the ghost perfectly
                    // and her visible tail had nothing. The skin-bind gate keeps plain humans
                    // (who all carry the dormant chain in the PPB skeleton) rig-free.
                    // ★ WIDENED 2026-07-19 (Ulliss the half-dragon): the old scope guard also
                    // required the HDT ghost chain to resolve — but the Ulliss class (equipable
                    // CrbTail.nif skinned to the vanilla chain, NO HDT item anywhere) then never
                    // rigged at all. TailChainSkinned IS the real ghost protection: an unskinned
                    // chain never rigs (plain humans stay rig-free), and a skinned chain is by
                    // definition visible flesh riding those bones. Consequence: any female whose
                    // worn/body mesh skins TailBone01-05 without an SMP chain (incl. plain beast
                    // tails if their body binds the chain) now gets the grip-only tbl-5 rig —
                    // WATCH the 8-slot budget in beast-heavy scenes (receipts: NFING CREATE tbl=5).
                    // ★★ TIP-GATE 2026-07-20 (the Yvanni PHANTOM tail): root-bone skinning alone
                    // is NOT proof of a tail — her "Tail Ring" jewelry (draenei_rings_silver.nif)
                    // weights vanilla TailBone01/02 only, which spawned a straight-out tbl-5 rig
                    // on her DORMANT chain beside the real Mal_Tail rig. A real tail (Ulliss's
                    // CrbTail: 330-595 verts per bone, 01..05) weights the WHOLE chain — so gate
                    // on the TIP (TailBone05 = vn.c[3]) too. M'rissi is unaffected either way:
                    // her fluffy tail (Sippo, tailHDT.nif) binds ONLY the HDT chain (offline-
                    // verified 2026-07-20 — the old "her naked tail skins the vanilla chain"
                    // memory predates the Fluffy replacer winning her model) — she rides table 2.
                    // ★★★ tbl-5 PROMOTE RETIRED (2026-07-20, user directive): the vanilla-chain
                    // touch rig is "useless now" — a bone-ANIMATED tail cannot be pushed (Ulliss),
                    // and no NPC in this load order needs the M'rissi-class promote anymore (her
                    // fluffy tail binds the HDT chain = table 2). Table C stays defined for a
                    // future re-activation; the tip-gate lesson (jewelry binds chain ROOTS) is in
                    // the Ledger.
                    // if (tbl == 2) {
                    //     FingerNodes vn = ResolveNodes(root3d, 5);
                    //     if (vn.all && TailChainSkinned(root3d, vn.a[0]) &&
                    //         TailChainSkinned(root3d, vn.c[3])) tbl = 5;
                    // }
                    if (tbl != 0) {
                        FingerNodes nodes = ResolveNodes(root3d, tbl);
                        // Budget is acquired HERE — the table resolved, so this create is real.
                        // Evicting from the branch gate above would destroy a live rig on behalf
                        // of an actor that turns out to create nothing (see GarmentBudgetAcquire).
                        if (nodes.all && GarmentBudgetAcquire(actor, id)) TryCreate(actor, tbl, freeSlot);
                    }
                    if (ps.cursor > kProbeSteps) { ps.cursor = 1; ps.next = frame + 200 + (id % 80); }
                    else                ps.next = frame + 40;
                }
            }
        }

        // ── FINGER RIG (tbl 0): the existing single-actor logic — the console pin
        // wins; otherwise the knob attaches to the nearest driven NPC (elected inside
        // PerfSys's lodNearDist band). Only while the legacy override is off: the old
        // file's mode mapping made pin/knob serve the garment table when mode > 0. ──
        if (mode == 0 && (pin != 0 || ObjectHold::NpcFingerEnabled())) {
            const std::uint32_t desired = pin ? pin : PerfSys::NearestDrivenId();
            if (desired != 0 && id == desired) {
                bool haveFinger = false;   // ONE finger rig total, wherever it lives
                for (int k = 0; k < kMaxRigs; ++k)
                    if (g_rigs[k].live && g_rigs[k].tbl == 0) { haveFinger = true; break; }
                const int freeSlot = FindFreeSlot();
                if (!haveFinger && freeSlot >= 0)
                    TryCreate(actor, 0, freeSlot);
                // bodies were just placed on the chord — drive starts next tick
            }
        }
    }

    void OnFrame()
    {
        const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

        // Sample every knob the collision callback needs, HERE on the main thread (see
        // g_filterKnobs: float reads inside FilterDecision = the misaligned-stack CTD).
        RefreshFilterKnobs();

        // ReTouch ghost zones + mouth touch (main thread, after the drives of this frame)
        GhostTouchFrame();

        // DF freeze fix (2026-07-27): execute any dismemberment cut captured on DF's worker
        // thread. Also drained from DismemberGuard::Tick, but Tick only runs while some actor
        // is being driven — a victim can stop driving the instant it dies, so this unconditional
        // per-frame drain guarantees a queued cut always lands. Both callers are the main thread.
        DismemberGuard::DrainQueuedCuts();

        // (World-epoch / worldspace-epoch / deferred-removal machinery deleted 2026-07-14 —
        // rigs now hold a strong NiPointer<bhkWorld> so DestroyRig removes deterministically.)

        // Deferred filter telemetry: FilterDecision runs on collision threads and must
        // never touch fmt/spdlog (proven CTD) — it sets flags, we report them here.
        static bool s_repHand = false, s_repRag = false, s_repCC = false;
        if (!s_repHand && g_logHand.load(std::memory_order_relaxed)) {
            s_repHand = true;
            logger::info("NFING filter: finger x HIGGS-hand COLLIDE pair seen (deferred report)");
        }
        if (!s_repRag && g_logRagdoll.load(std::memory_order_relaxed)) {
            s_repRag = true;
            logger::info("NFING filter: finger x own-ragdoll pair seen (Ignored; own-body mode: Collided) (deferred report)");
        }
        if (!s_repCC && g_logCharCtrl.load(std::memory_order_relaxed)) {
            s_repCC = true;
            logger::info("NFING filter: finger x charController IGNORE pair seen (deferred report)");
        }

        // NODE CENSUS trigger: nodeCensusNow 0 -> non-0 arms the one-shot dump.
        {
            static float s_lastCensus = 0.f;
            const float v = ObjectHold::NodeCensusNow();
            if (v > 0.5f && s_lastCensus <= 0.5f) {
                g_censusArmed.store(true, std::memory_order_relaxed);
                logger::info("CENSUS armed — the pinned/nearest driven actor dumps every *ail* node once");
            }
            s_lastCensus = v;
        }

        const int mode = ObjectHold::NpcTailMode();
        const std::uint32_t pin = g_pinnedId.load(std::memory_order_relaxed);
        const bool follower = ObjectHold::NpcFollowerEnabled();
        const bool fingerKnob = ObjectHold::NpcFingerEnabled();

        // Per-slot lifecycle sweeps (the old single-rig rules applied slot-wise).
        for (int slot = 0; slot < kMaxRigs; ++slot) {
            FingerRig& rig = g_rigs[slot];
            if (!rig.live) continue;

            // Mode flip while live (npcTailTest changed): destroys ONLY override-created
            // rigs (rig.mode != 0); OnPreDrive recreates from the new table on the
            // target's next driven tick. Auto-probed/finger rigs (mode 0) are untouched.
            if (rig.mode != 0 && rig.mode != mode) {
                DestroyRig(slot, "mode");
                continue;
            }

            if (rig.tbl == 0) {
                // FINGER rig: knob-off / unpin / retarget-to-a-new-pin destroys (spec §5
                // "next OnFrame"). NOTE: in knob(nearest) mode the rig deliberately STAYS
                // on its actor while she is still driven — re-electing the nearest every
                // frame would thrash the rig between two NPCs at similar distances.
                if (pin != 0) {
                    if (rig.actorId != pin) { DestroyRig(slot, "retarget"); continue; }
                } else if (!fingerKnob) {
                    DestroyRig(slot, "knob");
                    continue;
                }
            } else if (rig.mode != 0) {
                // OVERRIDE garment rig: pin retarget applies (old single-rig behavior);
                // knob-off is the mode-flip destroy above (mode reads 0 when off).
                if (pin != 0 && rig.actorId != pin) { DestroyRig(slot, "retarget"); continue; }
            } else {
                // AUTO-PROBED garment rig: npcFollower 0 sweeps all of them.
                if (!follower) { DestroyRig(slot, "follower"); continue; }
            }

            // ── GARMENT-RIG RANGE SWEEP (2026-07-30) ──────────────────────────────────────
            // AUTO-PROBED garment rigs only — tbl != 0 AND mode == 0.
            //  * tbl == 0 (finger rig) is already single-target via the pin/nearest election;
            //    gating it here would fight that.
            //  * mode != 0 is a LEGACY OVERRIDE rig (npcTailTest), created deliberately on the
            //    pinned-else-nearest actor by a path that has NO range gate. Sweeping it here
            //    while its creator ignores range is a guaranteed per-frame create/destroy loop,
            //    and every destroy leaks the rig (never-free convention). Either both ends gate
            //    or neither does; an explicitly requested test rig should obey the knob, not a
            //    distance budget meant for the ambient auto-probe population.
            //    (Adversarial review 2026-07-30 — two independent reviewers found this.)
            // Hysteresis keeps an actor hovering exactly at the boundary from thrashing.
            if (rig.tbl != 0 && rig.mode == 0) {
                const float range = ObjectHold::NpcRigRangeU();
                if (range > 0.f && rig.d2Valid) {
                    const float out = range + ObjectHold::NpcRigRangeHystU();
                    if (rig.lastD2 > out * out) { DestroyRig(slot, "range"); continue; }
                }
            }

            // stale sweep: the actor left the driven set (the PerfSys F5 idiom)
            // 2026-07-13 review: 3 was tuned for one 4-body rig; menus/reprojection can
            // starve the pre-drive seam while g_frame keeps ticking, and a false sweep
            // now leaks 8 rigs x 16 never-freed shapes per cycle. ~300 ticks ≈ 3 s.
            if (frame - rig.lastSeenFrame > 300)
                DestroyRig(slot, "stale");
        }

        // g_probe map hygiene (2026-07-13 review LOW): entries accrue one per distinct
        // driven FormID and are otherwise dropped only at load. Tiny (~16 B each) but
        // unbounded across a marathon no-load session. A high-water clear is the cheap
        // safety valve — every live actor simply re-seeds (re-staggers) on her next
        // driven frame; the recycled-FormID case is already covered by the 600-frame
        // reseed in OnPreDrive. Sized well above any plausible nearby-actor working set.
        if (g_probe.size() > 4096) {
            g_probe.clear();
            logger::info("NFING g_probe high-water clear (>4096 entries) — actors re-seed on next drive");
        }
    }

    bool RequestToggle(RE::Actor* sel)
    {
        if (!sel) return g_pinnedId.load(std::memory_order_relaxed) != 0;
        const std::uint32_t id = sel->GetFormID();
        if (g_pinnedId.load(std::memory_order_relaxed) == id) {
            g_pinnedId.store(0, std::memory_order_relaxed);   // unpin — OnFrame destroys the rig
            logger::info("NFING unpin {:08X} (console)", id);
            return false;
        }
        g_pinnedId.store(id, std::memory_order_relaxed);      // pin — OnPreDrive creates on her next driven frame
        logger::info("NFING pin {:08X} (console)", id);
        return true;
    }

    // ── CONTACT-CURL QUERY (2026-07-16): PPBHook's rotation writer asks, per driven actor,
    // for the finger rig's EMA-smoothed curl factors. Main-thread only (both the integrator
    // in DriveRig and this reader run inside the same 0xB266AB pre-drive hook body). Returns
    // false for every actor that isn't carrying the live tbl-0 finger rig — the cheap path.
    bool GetFingerCurl(std::uint32_t actorId, float out[4])
    {
        for (int k = 0; k < kMaxRigs; ++k) {
            FingerRig& rig = g_rigs[k];
            if (!rig.live || rig.tbl != 0 || rig.actorId != actorId) continue;
            bool any = false;
            for (int f = 0; f < 4; ++f) { out[f] = rig.curlA[f]; any = any || rig.curlA[f] > 0.f; }
            return any;                       // rig present but fully open -> skip the writer
        }
        return false;
    }

    int FilterDecision(std::uint32_t infoA, std::uint32_t infoB)
    {
        // ── GRAB-PHANTOM SHIELD (2026-07-15, Report 18 §5 wig-freeze fix) ──────────
        // The wig-grab HANG root cause: HIGGS's grab-candidate cast forces its phantom
        // to layer 44 (near, hand.cpp:333) or 40 (far, hand.cpp:434) and only selects a
        // hit that is moveable + resolves to an actor ref — BOTH true of our ~0.05 kg
        // garment capsules. HIGGS then bolts a stiff motorized grab constraint onto a
        // near-massless body PPB is already servo-driving → a = F/m diverges → Havok
        // solver NaN → engine freeze. The Ignore that was meant to keep us out of the
        // grab cast lived AFTER the g_rigGroups scan (isFinger), so any live GROUP DRIFT
        // (3D re-merge/re-group on equip, rescope, slot past kMaxRigs, teardown residual)
        // made isFinger miss, fell through to Continue, and the VANILLA table let the
        // phantom hit us. This shield keys ONLY on the immutable signature (layer 56 +
        // part 30 + bit15 — unique to PPB rigs on this build: HIGGS hand = parts 2/3/5/6,
        // HandBox = part 4, layer 56 is HIGGS-exclusive) and NEVER consults g_rigGroups,
        // so it fires even when the group has drifted. Ordering-proof w.r.t. the group
        // scan (the actual failure); PLANCK returns Continue for (56,40/44) so PPB's
        // callback is always reached. The real HIGGS hand is layer 56 (not 40/44), so
        // the (capsule,hand) pair skips this block and still reaches the finger-vs-hand
        // Collide below — hand-feel is preserved. O(1), no logging (collision-thread safe).
        const auto ppbSig = [](std::uint32_t f) {
            return (f & 0x7Fu) == kHiggsLayer && ((f >> 8) & 0x1Fu) == kFingerPart &&
                   (f & kBit15) != 0;
        };
        const bool sA = ppbSig(infoA), sB = ppbSig(infoB);
        if (sA != sB) {                                  // exactly one side is a PPB rig capsule
            const unsigned othLayer = (sA ? infoB : infoA) & 0x7Fu;
            if (othLayer == 40u || othLayer == 44u)      // HIGGS grab/pick phantom (0x28 / 0x2C)
                return 2;                                // never a grab candidate → no massless-body constraint
        }

        // MULTI-RIG (2026-07-13): a word is one of OURS if it carries the finger
        // signature (layer 56 + part 30 + bit15) AND its group matches ANY live slot
        // in g_rigGroups. The signature checks are pure bit math and short-circuit
        // before the scan for every foreign word, so the idle cost stays trivial.
        // Relaxed loads only — this runs on Havok's multithreaded collision threads.
        const auto isFinger = [](std::uint32_t f) {
            if ((f & 0x7Fu) != kHiggsLayer || ((f >> 8) & 0x1Fu) != kFingerPart || !(f & kBit15))
                return false;
            const std::uint32_t g = f >> 16;
            if (g == 0) return false;
            for (int k = 0; k < kMaxRigs; ++k)
                if (g_rigGroups[k].load(std::memory_order_relaxed) == g) return true;
            return false;
        };
        // MESH MARKERS (2026-07-18): part-29 layer-56 bodies are visual GHOSTS — never collide.
        if ((((infoA & 0x7Fu) == kHiggsLayer) && (((infoA >> 8) & 0x1Fu) == 29u)) ||
            (((infoB & 0x7Fu) == kHiggsLayer) && (((infoB >> 8) & 0x1Fu) == 29u)))
            return 2;
        const bool fA = isFinger(infoA), fB = isFinger(infoB);
        if (!fA && !fB) return 0;                    // existing PerfSys rules run unchanged
        if (fA && fB) return 2;                      // finger x finger (any rigs) — never useful

        // Own-arm belt + own-body logic key on the group that MATCHED — the finger
        // word's own group, never a single global.
        const std::uint32_t grp = (fA ? infoA : infoB) >> 16;
        const std::uint32_t o  = fA ? infoB : infoA;
        const unsigned      op = (o >> 8) & 0x1Fu;

        // The one pair the test exists for: HIGGS's hand (parts 3/5) + the wielded-weapon
        // clone (same parts). Parts 2/6 are declared-but-never-stamped in this HIGGS
        // version (review Defect 3) — kept in the mask as dead-but-harmless. NOTE:
        // HIGGS-HELD objects keep their ORIGINAL layer (not 56) and are therefore
        // Ignored below — the §7 test presses the HAND or the WIELDED weapon, not a
        // gravity-held prop.
        // ⚠ NEVER log from this function: it runs on Havok's MULTITHREADED collision
        // threads through HIGGS's comparator trampoline — fmt/spdlog's SIMD spills
        // fault on the misaligned stack there (PROVEN CTD 2026-07-09 21:33, movaps
        // [rbp-0x40]). Flags only; OnFrame (main thread) reports them.
        // Parts 2/3/5/6 = HIGGS native hand + wielded weapon; ALSO our own HandBox
        // 4-box set (part = handBoxSubLayer, default 4) — 2026-07-11 fix: the finger
        // boxes previously slid through tail capsules because their part was unlisted.
        // Knobs come from the main-thread cache — NEVER call a float accessor here (g_filterKnobs).
        const std::uint32_t knobs   = g_filterKnobs.load(std::memory_order_relaxed);
        const unsigned      boxPart = (knobs >> kFKBoxPartShift) & 0x1Fu;
        if ((o & 0x7Fu) == kHiggsLayer && (o & kBit15) && (o >> 16) != grp &&
            (op == 2u || op == 3u || op == 5u || op == 6u || op == boxPart)) {
            g_logHand.store(true, std::memory_order_relaxed);
            return 1;
        }

        const unsigned olayer = o & 0x7Fu;

        // ── KNOB-GATED COLLISION WIDENING (2026-07-16, finger rig only): live A/B dials for
        // letting the fingers meet the WORLD so the contact-curl has something to conform to.
        // return 0 = fall through to the vanilla layer-56 row (which collides with all of
        // these) — we only lift PPB's own Ignore, we never force-Collide past vanilla.
        // ⚠ scope caveat (same as own-body): g_fingerGroup is the ACTOR's group, so if she
        // also carries a garment rig its capsules share the group and widen too — acceptable
        // for the single-NPC test scenario these default-OFF knobs exist for.
        const std::uint32_t fGrpW = fA ? (infoA >> 16) : (infoB >> 16);
        if (fGrpW == g_fingerGroup.load(std::memory_order_relaxed) && fGrpW != 0) {
            if ((knobs & kFKVsClutter) &&
                (olayer == 4u || olayer == 5u || olayer == 6u || olayer == 19u || olayer == 20u))
                return 0;                     // clutter / weapons / projectiles / debris
            if ((knobs & kFKVsWorld) &&
                (olayer == 1u || olayer == 2u || olayer == 10u || olayer == 14u || olayer == 17u))
                return 0;                     // statics / animstatics / props / trap / ground (infinite mass!)
            if ((knobs & kFKVsNpc) &&
                (olayer == 8u || olayer == 32u || olayer == 33u) && (o >> 16) != grp)
                return 0;                     // OTHER NPCs' ragdolls (vanilla row collides)
        }

        // ── AIHANDS OWN-BODY MODE (2026-07-12, npcFingerOwnBody 1): the fingers become
        // contact SENSORS for AIHands' contact-driven curl — they force-collide with the
        // owner's OWN flesh (vanilla rejects same-group+bit15 pairs; we override) and let
        // the vanilla table rule statics/clutter (layer 56 behaves like HIGGS's hands),
        // while the arm bodies they ride stay excluded via the exact-word belt.
        // Own-body mode applies ONLY to the FINGER rig's capsules (2026-07-13 review:
        // garment capsules share the part-30 signature — without this gate a tail
        // would force-collide with its own thigh, fighting the drive constantly).
        const std::uint32_t fGrp = fA ? (infoA >> 16) : (infoB >> 16);
        if ((knobs & kFKOwnBody) &&
            fGrp == g_fingerGroup.load(std::memory_order_relaxed) && fGrp != 0) {
            // Arm belt matches on GROUP+PART, layer-agnostic within {8,33} (2026-07-12 review:
            // the layer field flips 8<->33 with ragdoll/PLANCK state and HIGGS re-filters what
            // it grabs — an exact-word snapshot would leak the very bodies the belt excludes).
            if ((olayer == 8u || olayer == 33u) && (o >> 16) == grp) {
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t w = g_ownArmWord[k].load(std::memory_order_relaxed);
                    if (w && ((w >> 8) & 0x1Fu) == op) return 2; // own arm parts (L twins too): never
                }
                g_logRagdoll.store(true, std::memory_order_relaxed);
                return 1;                                        // own flesh: fingers REST on it
            }
            if (olayer == 5u || olayer == 6u) return 0;          // clutter/weapon (held objects): vanilla = collide
            return 2;                                            // statics, other NPCs, CC, phantom — Ignore
            // (review: statics are infinite-mass — a leaned-on table would flicker-oscillate;
            //  other NPCs stay excluded until AIHands needs them. Widen deliberately, not by default.)
        }

        // Everything else — own ragdoll (8/33), DeadBip (32), other NPCs, charControllers
        // (30), statics, clutter, the HIGGS near-cast phantom (44: keeps the fingers out
        // of the grab candidate cast entirely) — Ignore.
        if ((olayer == 8u || olayer == 33u) && (o >> 16) == grp) {
            g_logRagdoll.store(true, std::memory_order_relaxed);
        } else if (olayer == 30u) {
            g_logCharCtrl.store(true, std::memory_order_relaxed);
        }
        return 2;
    }

    int StripActorBodiesExcept(RE::Actor* a, const char* keepNodeName)
    {
        // CLONE BODY STRIP (2026-07-26, DismemberGuard): reduce an NGD head-clone actor to a
        // single physics body — the COM anchor the visible severed head rides. Caller
        // guarantees: main thread, actor PLANCK-ignored + PPB-excluded (nothing drives these
        // bodies), NGD's spawn-time scale/collision storm settled (dgStripDelayS). Removal =
        // the proven RemoveBody primitive (world write-lock + membership guard); bodies are
        // removed from the WORLD, never freed — their wrappers stay valid for NGD's later
        // maintenance writes.
        if (!a) return -1;
        RE::NiAVObject* root = a->Get3D();
        auto* cell = a->GetParentCell();
        RE::bhkWorld* world = cell ? cell->GetbhkWorld() : nullptr;
        if (!root || !world) return -1;
        constexpr int kCap = 64;
        void* wrappers[kCap]; const char* names[kCap]; int n = 0;
        CollectBodiesRecursive(root, keepNodeName, wrappers, names, n, kCap);
        int removed = 0;
        for (int i = 0; i < n; ++i) {
            hkpRigidBody* hk = HkOf(wrappers[i]);
            if (hk && hk->getWorld()) { RemoveBody(wrappers[i], static_cast<void*>(world)); ++removed; }
        }
        logger::info("NFING STRIP {:08X}: {} collision bodies found (keeping '{}'), {} removed from world",
                     a->GetFormID(), n, keepNodeName ? keepNodeName : "<none>", removed);
        return removed;
    }

    // Engine hkpListShape field mirror (offsets lifted from CommonLibVR hkpListShape.h —
    // childInfo hkArray @0x30, ChildInfo stride 0x20 with shape* at +0, cached AABB @0x50/0x60).
    namespace {
        struct PortHkpListShape {
            std::uint8_t  head[0x30];
            void*         childData;            // 0x30 hkArray<ChildInfo>.data
            std::int32_t  childSize;            // 0x38
            std::int32_t  childCapFlags;        // 0x3C
            std::uint16_t lsFlags;              // 0x40
            std::uint16_t numDisabledChildren;  // 0x42
            std::uint32_t pad44[3];
            float         aabbHalfExtents[4];   // 0x50
            float         aabbCenter[4];        // 0x60
        };
    }

    void CountActorBodies(RE::NiAVObject* root, void** outWrappers, const char** outNames,
                          int& n, int cap)
    {
        CollectBodiesRecursive(root, nullptr, outWrappers, outNames, n, cap);
    }

    // ── PARK EVERY BODY AT THE ANCHOR (2026-07-27, user: "every Havok joint and collision
    // capsule need to be AT the anchor point") ──────────────────────────────────────────────
    // The head-only NIF fixed the SHAPES (only the head/COM collide), but each ragdoll BODY
    // still sits at its own bone, so the user sees the joints orbiting 10-50u around the head
    // and dragging it. Shapes are SHARED between actors (the contamination lesson) — BODIES ARE
    // NOT: transforms, motion type and constraints are per-actor instances, so writing them for
    // one severed head touches nothing else.
    // So for a confirmed head: disable its ragdoll constraints (nothing left to pull the parts
    // back into a body shape), then teleport every non-anchor body onto the anchor and keyframe
    // it there. Result: one collidable head ball at the COM with every other part parked inside
    // it — no orbiting, no drag, nothing to catch on the world.
    struct EntityConstraintAccess : hkpEntity {
        using hkpEntity::m_constraintsMaster;
        using hkpEntity::m_constraintsSlave;
    };

    // Engine functions bound by address (the SDK symbols are not linked into this DLL) — all
    // three are PLANCK's proven bindings.
    inline void ConstraintSetEnabled(hkpConstraintInstance* ci, bool on)
    {
        using Fn = void(*)(hkpConstraintInstance*, bool);
        static REL::Relocation<Fn> fn{ REL::Offset(0xAC06A0) };
        if (ci) fn(ci, on);
    }
    // bhkRigidBody::SetMotionType — the WRAPPER-level call. It does the full job (motion type +
    // activation + collision-filter update) where a raw field poke does not.
    inline void BhkSetMotionType(void* bhkWrapper, std::uint64_t motionType)
    {
        using Fn = void(*)(void*, std::uint64_t);
        static REL::Relocation<Fn> fn{ REL::Offset(0xE08040) };
        if (bhkWrapper) fn(bhkWrapper, motionType);
    }
    inline void BhkSetActivated(void* bhkWrapper, bool activate)
    {
        using Fn = void(*)(void*, bool);
        static REL::Relocation<Fn> fn{ REL::Offset(0xE085D0) };
        if (bhkWrapper) fn(bhkWrapper, activate);
    }


    // ── SEVERED-HEAD GRAB FIX (2026-07-28) ───────────────────────────────────────────────────
    // ★ THE ANSWER WAS IN OUR OWN REPORT 09 §6 THE WHOLE TIME.
    // HIGGS's hand box carries a 5-bit RAGDOLL SUB-LAYER in filterInfo bits 8..12:
    // RightHand = 3, LeftHand = 5. Its documented rule (higgs physics.h:210-222):
    //     "If bit 15 is set, then if layer differs by 1 we don't collide."
    // So a body whose sub-layer is 2, 4 or 6 is one the hand PASSES THROUGH BY DEFINITION.
    // A ragdoll body carries its BIPED PART NUMBER in exactly those bits, and bit 15 is set on
    // biped bodies — so if the severed head's one reachable body (the COM anchor) happens to
    // hold 2/4/6, the player's hand can NEVER contact it. That explains every observation:
    // the grab callback still fires (selection uses a separate cast phantom), a WEAPON still
    // shoves the head (different body, different comparison), and a CORPSE still works (you
    // grab an arm/torso whose part number isn't 2/4/6, and its constraints drag the rest).
    // Fix: force the anchor's sub-layer to 0 (SkipNone) so |0-3| and |0-5| are both != 1, then
    // re-run the broadphase filter update (without it the change is not observed).
    int FixHeadGrabFilter(RE::Actor* a, const char* anchorNode)
    {
        if (!a || !anchorNode) return -1;
        RE::NiAVObject* root = a->Get3D();
        auto* cell = a->GetParentCell();
        RE::bhkWorld* bworld = cell ? cell->GetbhkWorld() : nullptr;
        if (!root || !bworld) return -1;
        constexpr int kCap = 64;
        void* wrappers[kCap]; const char* names[kCap]; int n = 0;
        CollectBodiesRecursive(root, nullptr, wrappers, names, n, kCap);
        for (int i = 0; i < n; ++i) {
            if (!names[i] || _stricmp(names[i], anchorNode) != 0) continue;
            hkpRigidBody* hk = HkOf(wrappers[i]);
            if (!hk) return -1;
            auto* col = hk->getCollidableRw();
            const std::uint32_t before = col->getCollisionFilterInfo();
            const std::uint32_t sub    = (before >> 8) & 0x1Fu;
            const bool          bit15  = (before & 0x8000u) != 0;
            const bool skipsRight = bit15 && (sub == 2u || sub == 4u);
            const bool skipsLeft  = bit15 && (sub == 4u || sub == 6u);
            if (sub == 0u && !skipsRight && !skipsLeft) {
                logger::info("NFING HEADGRAB {:08X}: filter=0x{:08X} layer={} sub={} bit15={} "
                             "-> already hand-reachable, no change",
                             a->GetFormID(), before, before & 0x7Fu, sub, bit15 ? 1 : 0);
                return 0;
            }
            const std::uint32_t after = before & ~0x00001F00u;      // sub-layer -> 0 (SkipNone)
            col->setCollisionFilterInfo(after);
            // Broadphase must be told, or the new filter is never observed (Report 09 §6).
            using _UpdFilter = void(*)(void*, hkpEntity*, int, int);
            static REL::Relocation<_UpdFilter> updFilter{ REL::Offset(0xAB3110) };
            if (void* hkpW = GetHkpWorld(static_cast<void*>(bworld))) {
                WorldWriteLock lock(static_cast<void*>(bworld));
                updFilter(hkpW, static_cast<hkpEntity*>(hk), 0, 0);
            }
            logger::info("NFING HEADGRAB {:08X}: filter 0x{:08X} -> 0x{:08X} (sub {} -> 0). "
                         "HIGGS hand skipped it: right={} left={} — now reachable.",
                         a->GetFormID(), before, after, sub,
                         skipsRight ? "YES" : "no", skipsLeft ? "YES" : "no");
            return 1;
        }
        return -1;
    }

    int ParkBodiesAtAnchor(RE::Actor* a, const char* anchorNode, int passNum)
    {
        if (!a || !anchorNode) return -1;
        RE::NiAVObject* root = a->Get3D();
        if (!root) return -1;
        constexpr int kCap = 64;
        void* wrappers[kCap]; const char* names[kCap]; int n = 0;
        CollectBodiesRecursive(root, nullptr, wrappers, names, n, kCap);

        // the anchor body (the one the visible head rides)
        hkpRigidBody* anchor = nullptr; void* anchorWrap = nullptr;
        for (int i = 0; i < n; ++i)
            if (names[i] && _stricmp(names[i], anchorNode) == 0) {
                anchor = HkOf(wrappers[i]); anchorWrap = wrappers[i]; break;
            }
        if (!anchor) return -1;
        const hkVector4 anchorPos = anchor->getPosition();

        // Reset detector: if a previous pass made the anchor dynamic/clutter and it is now
        // something else, an outside system (NGD's periodic SetScalesAndCollisions) undid us
        // between passes — the direct evidence the "grab stopped working" hunt needs.
        if (passNum > 1) {   // pass 1 legitimately starts keyframed/biped — only later passes count
            const auto prevType  = anchor->m_motion.m_type;
            const std::uint32_t prevLayer =
                anchor->getCollidable()->getCollisionFilterInfo() & 0x7Fu;
            // hkpMotion 1..4 are all dynamic flavours — the engine materializes our
            // SetMotionType(DYNAMIC) as BOX_INERTIA (3), which is fine (17:22 false alarm).
            const bool motionOk = prevType >= hkpMotion::MOTION_DYNAMIC &&
                                  prevType <= hkpMotion::MOTION_THIN_BOX_INERTIA;
            if (!motionOk || prevLayer != 4u)
                logger::info("NFING PARK {:08X}: pass {} found the anchor RESET (motion={} "
                             "layer={}) — an external pass (NGD?) is undoing us between passes",
                             a->GetFormID(), passNum, static_cast<int>(prevType), prevLayer);
        }

        // ── MAKE THE HEAD ACTUALLY LIFTABLE (2026-07-28) ─────────────────────────────────────
        // An actor's ragdoll bodies are KEYFRAMED (animation-driven) until something makes them
        // dynamic — normally PLANCK's AddRagdollToWorld does it. We PLANCK-ignore the head clone
        // permanently, so nothing ever did: the head stayed keyframed, i.e. INFINITE MASS, and
        // HIGGS could not lift it no matter what the NIF said its mass was (user: "it's like the
        // head is the infinite mass I can't move it"). Drive it DYNAMIC through the wrapper call
        // so the motion type, activation and collision filter are all updated together.
        BhkSetMotionType(anchorWrap, 1 /* MOTION_DYNAMIC */);
        BhkSetActivated(anchorWrap, true);
        // ...and make it a CLUTTER-layer object (2026-07-28). Verified in the HIGGS source: its
        // grab cast selects any MOVEABLE body (utils.cpp IsObjectSelectable), but a BIPED-layer
        // body on an ACTOR ref falls into the looting/armor special cases, and the clone is
        // PLANCK-ignored so PLANCK never registers its group as grabbable either. Layer 4
        // (L_CLUTTER) routes the head down HIGGS's plain object path — pickable, grabbable,
        // throwable — while still colliding with ground/statics/weapons. Group bits kept.
        {
            auto* colA = anchor->getCollidableRw();
            const std::uint32_t w = colA->getCollisionFilterInfo();
            colA->setCollisionFilterInfo((w & 0xFFFFFF80u) | 4u);
        }

        // ★ RAGDOLL BODIES ONLY (2026-07-28). The scenegraph walk returns EVERY body under the
        // actor's root — the previous pass parked 44 (a ragdoll has 18): the extra ~26 were the
        // SMP wig / gear bodies, which we keyframed into infinite-mass anchors welded in space
        // and whose 80 constraints we cut. Never touch physics we don't own. Every ragdoll bone
        // is named "NPC ..." — that prefix IS the ownership test. And a constraint is only cut
        // when BOTH of its ends are ours, so a wig chain attached to the head bone keeps its
        // root (the hair stays hair).
        bool ours[kCap] = {};
        hkpEntity* set[kCap + 1]; int ns = 0;
        set[ns++] = static_cast<hkpEntity*>(anchor);
        for (int i = 0; i < n; ++i) {
            hkpRigidBody* hk = HkOf(wrappers[i]);
            if (!hk || hk == anchor) continue;
            if (names[i] && std::strncmp(names[i], "NPC ", 4) == 0) {
                ours[i] = true;
                set[ns++] = static_cast<hkpEntity*>(hk);
            }
        }
        auto inSet = [&](hkpEntity* e) {
            for (int k = 0; k < ns; ++k) if (set[k] == e) return true;
            return false;
        };

        int parked = 0, cut = 0, foreign = 0, removedNow = 0;   // removedNow stays 0 (see below)
        for (int i = 0; i < n; ++i) {
            hkpRigidBody* hk = HkOf(wrappers[i]);
            if (!hk || hk == anchor) continue;
            if (!ours[i]) { ++foreign; continue; }

            // 1) sever the constraint chain so nothing drags the head or restores the spread.
            //    (The SDK setters aren't linked in this build — the engine's own
            //    hkpConstraintInstance::setEnabled is bound by address, PLANCK's proven idiom.)
            auto* acc = static_cast<EntityConstraintAccess*>(static_cast<hkpEntity*>(hk));
            for (int c = 0; c < acc->m_constraintsMaster.getSize(); ++c)
                if (auto* ci = acc->m_constraintsMaster[c].m_constraint)
                    if (inSet(ci->getEntityA()) && inSet(ci->getEntityB())) { ConstraintSetEnabled(ci, false); ++cut; }
            for (int c = 0; c < acc->m_constraintsSlave.getSize(); ++c)
                if (auto* ci = acc->m_constraintsSlave[c])
                    if (inSet(ci->getEntityA()) && inSet(ci->getEntityB())) { ConstraintSetEnabled(ci, false); ++cut; }

            // 2) teleport it onto the anchor, stop it dead — then REMOVE IT FROM THE WORLD.
            //    ★ 2026-07-28 (the 17:53 probe): keyframe+sleep did NOT hold — NGD's periodic
            //    pass restores dynamic motion types faster than our re-assert, and a dynamic
            //    satellite is "moveable" to HIGGS, so the palm-triangle walk grabbed the
            //    invisible head-bone GHOST instead of the anchor and the visible head never
            //    moved. A body with NO WORLD fails HIGGS's walk unconditionally (it checks
            //    m_world) and nothing NGD flips on it matters — removal ends the war. The
            //    wrapper refcount owns the memory (RemoveBody never frees); NGD's later
            //    maintenance writes on the out-of-world body are harmless.
            const_cast<hkTransform&>(hk->getTransform()).setTranslation(anchorPos);
            hk->m_motion.m_linearVelocity.setZero4();
            hk->m_motion.m_angularVelocity.setZero4();
            // ⛔ WORLD-REMOVAL REVERTED (crash-2026-07-28-22-15-55). Removing a satellite from
            // the world while its CONSTRAINT still references it leaves that constraint pointing
            // at a body with no simulation island — the engine's constraint pass then derefs
            // null (RCX=0, RDX=hkpRigidBody "NPC L Thigh [LThg]", RSI=hkpConstraintInstance on
            // that same body). DISABLING a constraint is not REMOVING it. Keyframe+sleep instead:
            // it never crashed, and with NGD's bAdvancedNPCMaintenance=0 nothing restores the
            // motion types any more, so the gentle path should now actually hold.
            BhkSetMotionType(wrappers[i], 4 /* MOTION_KEYFRAMED */);
            BhkSetActivated(wrappers[i], false);
            ++parked;
        }
        if (passNum <= 1 || removedNow > 0 || passNum % 30 == 0)
            logger::info("NFING PARK {:08X}: pass {} anchor '{}' -> DYNAMIC+active+clutter; {} ragdoll "
                         "satellites ({} REMOVED from world this pass), {} constraints disabled, {} "
                         "foreign bodies (SMP/gear) untouched",
                         a->GetFormID(), passNum, anchorNode, parked, removedNow, cut, foreign);
        return parked;
    }

    int ShrinkActorBodiesIntoHead(RE::Actor* a, const char* headAnchorNode)
    {
        // CLONE SHRINK (2026-07-26, v4 of the head-clone treatment — user: "shrink it to 0.01
        // scale... the whole body inside the head"): v3's full removal left NOTHING for HIGGS
        // to grab, so the severed head could not be picked up or thrown. Instead every capsule
        // on the clone collapses to a 0.01u point at its own bone — no collision footprint,
        // nothing to catch or roll on, constraints untouched (no skeleton surgery) — EXCEPT
        // the head anchor (the COM body the visible head rides): its first capsule becomes a
        // HEAD-SIZED ball, the one graspable, throwable body. List AABBs are re-cached to
        // match so the broadphase shrinks with the shapes.
        if (!a) return -1;
        RE::NiAVObject* root = a->Get3D();
        if (!root) return -1;
        constexpr int kCap = 64;
        void* wrappers[kCap]; const char* names[kCap]; int n = 0;
        CollectBodiesRecursive(root, nullptr, wrappers, names, n, kCap);
        const float kPointR = 0.01f * kSkyrimToHavok;
        const float kHeadR  = 4.8f  * kSkyrimToHavok;   // head.main-class ball
        auto shrinkCap = [](PortHkpCapsuleShape* cap, float r) {
            cap->radius = r;
            cap->vertexA[0] = 0.f; cap->vertexA[1] = 0.f; cap->vertexA[2] = 0.f;      cap->vertexA[3] = r;
            cap->vertexB[0] = 0.f; cap->vertexB[1] = 0.f; cap->vertexB[2] = 0.001f;   cap->vertexB[3] = r;
        };
        int done = 0;
        for (int i = 0; i < n; ++i) {
            hkpRigidBody* hk = HkOf(wrappers[i]);
            if (!hk) continue;
            auto* s = const_cast<hkpShape*>(hk->getCollidable()->getShape());
            if (!s) continue;
            const bool  anchor = headAnchorNode && names[i] && _stricmp(names[i], headAnchorNode) == 0;
            // ── NEUTRALIZE THE NON-HEAD BODIES (2026-07-27) ─────────────────────────────────
            // Collapsing the capsule SHAPES was not enough: the user saw the joints keep their
            // full-body spread, so limb bodies still sat in the floor and blocked the head from
            // being picked up. Shapes alone can't fix that — the BODIES are still where the
            // skeleton puts them. So every non-head body also becomes:
            //   · L_NONCOLLIDABLE (layer 15) — collides with NOTHING, so it cannot catch on the
            //     floor, block HIGGS, or push the head around (the proven marker-layer trick);
            //   · KEYFRAMED — stops simulating, so it neither falls nor drags the head.
            // The head/COM anchor keeps its dynamic, head-sized, fully collidable body: that is
            // the one object the player grabs and throws. NGD re-inflates periodically, which is
            // why DismemberGuard re-applies this pass.
            if (!anchor) {
                const std::uint32_t w = hk->getCollidable()->getCollisionFilterInfo();
                hk->getCollidableRw()->setCollisionFilterInfo((w & 0xFFFFFF80u) | 15u);
            }
            auto* asCap = reinterpret_cast<PortHkpCapsuleShape*>(s);
            if (asCap->type == HK_SHAPE_CAPSULE) {
                shrinkCap(asCap, anchor ? kHeadR : kPointR);
                ++done;
            } else if (asCap->type == HK_SHAPE_LIST) {
                auto* ls = reinterpret_cast<PortHkpListShape*>(s);
                auto* ci = static_cast<std::uint8_t*>(ls->childData);
                for (std::int32_t c = 0; ci && c < ls->childSize; ++c) {
                    auto* ch = *reinterpret_cast<PortHkpCapsuleShape**>(ci + c * 0x20);
                    if (!ch || ch->type != HK_SHAPE_CAPSULE) continue;
                    shrinkCap(ch, (anchor && c == 0) ? kHeadR : kPointR);
                    ++done;
                }
                const float he = (anchor ? kHeadR : kPointR) + 0.01f;
                ls->aabbHalfExtents[0] = he; ls->aabbHalfExtents[1] = he; ls->aabbHalfExtents[2] = he;
                ls->aabbCenter[0] = 0.f; ls->aabbCenter[1] = 0.f; ls->aabbCenter[2] = 0.f;
            }
        }
        logger::info("NFING SHRINK {:08X}: {} capsules collapsed to points across {} bodies "
                     "(anchor '{}' kept as a head-sized ball)",
                     a->GetFormID(), done, n, headAnchorNode ? headAnchorNode : "<none>");
        return done;
    }

    void ClearOnLoad()
    {
        // kPreLoadGame: each rig holds a strong reference to its world, so DestroyRig
        // removes its bodies deterministically (the world is alive because we own it),
        // then releases the reference so the load's world teardown proceeds normally.
        for (int slot = 0; slot < kMaxRigs; ++slot) DestroyRig(slot, "load");
        for (int slot = 0; slot < kMaxRigs; ++slot)                    // belt: non-live slots too
            g_rigGroups[slot].store(0, std::memory_order_relaxed);
        g_probe.clear();
        for (auto& w : g_ownArmWord) w.store(0, std::memory_order_relaxed);
        g_fingerGroup.store(0, std::memory_order_relaxed);
        g_pinnedId.store(0, std::memory_order_relaxed);
        g_logHand.store(false, std::memory_order_relaxed);
        g_logRagdoll.store(false, std::memory_order_relaxed);
        g_logCharCtrl.store(false, std::memory_order_relaxed);
        // mesh markers: drop pointers at the load boundary (leak-by-design, never-free convention)
        for (auto& b : g_meshMarker) b = nullptr;
        g_meshMarkerWorld = nullptr;
    }
}
