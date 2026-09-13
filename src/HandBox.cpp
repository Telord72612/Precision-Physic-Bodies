// ─────────────────────────────────────────────────────────────────────────
// HandBox.cpp — PLAYER HAND 4-BOX COLLIDER v2 ("the 4 rectangles", 2026-07-09).
//
// Track A (Phase 0): live SHRINK-ONLY dial of HIGGS's one-piece 10x3x18cm palm
// slab via a per-frame idempotent halfExtents write, clamped to the first-seen
// baseline — grow is the unverified broadphase direction (report 10 Risk 2);
// failure mode of shrink = missed contacts, never CTD.
//
// Track B (Phase 1): 4 KEYFRAMED follower boxes per hand riding the player's
// third-person finger bones — 2 index segments + one wide 3-finger slab + a
// fingertip plate, NO thumb (Finger00/01/02 never get bodies). Driven with
// applyHardKeyFrame (velocity, not teleport) so PLANCK's MOTION_DYNAMIC NPC
// flesh genuinely yields to a finger-width push. Built from ppb-scratch/
// spec2_handbox.md AS AMENDED BY review2_handbox.md:
//   N1  keyframe ONCE per frame snapshot (invDt = 1/frame-dt captured at
//       snapshot time) — never per physics substep. 2026-07-10 LAG FIX: the
//       bone snapshot itself moved from the PostVrikPostHiggs callback into
//       the FIRST PrePhysicsStep fire of each frame (TakeBoneSnapshot) — the
//       old snapshot trailed the hand by a frame whenever PostVrikPostHiggs
//       landed after the frame's physics step; OnFrame keeps only the frame
//       counter (the frame-edge signal) + deferred telemetry
//   N3  Mode 1 GetFingerValues curl REJECTED (stale when empty-handed) —
//       Mode 1 = fixed hand-frame offsets only
//   N4  create with bit 14 SET, clear after HIGGS's own 0.1 s enable delay
//   N5  bit 14 INHERITED from HIGGS's live hand word, then OR our conditions
//   N6  only the player's parent cell's bhkWorld is authoritative — the hook
//       fires for EVERY stepped world
//   N9  double arming gate (handBoxEnable AND handBoxArm)
//   N10 half-thickness floored at 0.010 m (accessor side)
//   N11 UpdateCollisionFilterOnEntity with HIGGS's exact enum constants
//   N13 quaternions normalized after Shepperd extraction
//   N14 create/destroy ONLY inside the PrePhysicsStep callback
//
// bhkWorld* DISCIPLINE (round-1 R3, same class as NpcFingerTest): the stored
// rig.bhkWorld is COMPARE-ONLY; RemoveBody ALWAYS receives the callback's
// freshly-resolved live world — the body-membership guard inside makes a
// stale-body removal a clean no-op (= forget). Accepted residual: an
// interior<->exterior world overlap can orphan 4 frozen boxes in the still-
// alive old world until its teardown removes them (clean fix later = eager
// removal on cell-detach).
//
// Honest framing (restated from the spec): these boxes PUSH flesh only. No
// HIGGS haptics/sound, no PLANCK hit/damage/aggro (both pointer-identity-gate
// on HIGGS's own bodies), zero CBPC response. NPCs outside PLANCK's active
// set have keyframed biped bodies -> zero response — parity with today's slab.
//
// Havok-SDK TU (CMake scopes the SDK include path to it). The namespace-scope
// hk link stubs live in PivFix.cpp ONLY — repeating them here would be a
// duplicate-symbol link error.
// ─────────────────────────────────────────────────────────────────────────

#include "HandBox.h"
#include "NpcFingerTest.h"  // NpcFinger::g_gbFingerAte — GRABBUG census cross-read
#include <string>
#include "GenitalProbe.h"   // GenitalProbe::IsExposed — the TNG slot-52 exposure gate
#include "Tuning.h"          // ObjectHold::HandBox*/HiggsSlab* knob accessors
#include "Interop.h"         // Interop::GetHiggs — interface + hand bodies
#include "HiggsInterface.h"  // vfuncs 21/24/28/12/16 — PrePhysicsStep, hand body, hold state

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
#include <Physics/Collide/Shape/Convex/Box/hkpBoxShape.h>
#include <Physics/Collide/Shape/Convex/hkpConvexShape.h>     // setRadiusUnchecked
#include <Physics/Collide/Agent/Collidable/hkpCollidable.h>
#include <Physics/Collide/Agent/Collidable/hkpCollidableQualityType.h>
#include <Physics/Dynamics/Motion/hkpMotion.h>               // MOTION_* types
#include <Physics/Dynamics/World/hkpWorld.h>                 // HK_UPDATE_FILTER_* enums

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
#include <chrono>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>

namespace logger = SKSE::log;

namespace {

    // ── Body-creation engine bindings (VR offsets, verbatim from Markers.cpp,
    //    plus the AIHands-proven UpdateCollisionFilterOnEntity — review N11) ──
    using _bhkBoxShape_ctor       = void(*)(void*, hkVector4*);
    using _bhkRigidBodyCinfo_ctor = void(*)(void*);
    using _bhkRigidBody_ctor      = void(*)(void*, void*);
    using _bhkRigidBody_setActivated = void(*)(void*, bool);
    using _hkpWorld_AddEntity     = hkpEntity*(*)(void*, hkpEntity*, int);
    using _hkpWorld_RemoveEntity  = hkpEntity*(*)(void*, char*, hkpEntity*);
    using _hkpBody_setPosition    = void(*)(hkpEntity*, const hkVector4&);
    using _hkpBody_setRotation    = void(*)(hkpEntity*, const hkQuaternion&);
    using _applyHardKeyFrame      = void(*)(const hkVector4&, const hkQuaternion&, hkReal, hkpRigidBody*);
    using _hkpWorld_UpdateFilterOnEntity = void(*)(void*, hkpEntity*, int, int);
    using _TESRace_IsBeast        = bool(*)(RE::TESRace*);

    inline _bhkBoxShape_ctor       fn_bhkBoxShape_ctor()       { static REL::Relocation<_bhkBoxShape_ctor>       f{ REL::Offset(0x2AEB70) }; return f.get(); }
    inline _bhkRigidBodyCinfo_ctor fn_bhkRigidBodyCinfo_ctor() { static REL::Relocation<_bhkRigidBodyCinfo_ctor> f{ REL::Offset(0xE06110) }; return f.get(); }
    inline _bhkRigidBody_ctor      fn_bhkRigidBody_ctor()      { static REL::Relocation<_bhkRigidBody_ctor>      f{ REL::Offset(0x2AEC80) }; return f.get(); }
    inline _bhkRigidBody_setActivated fn_setActivated()        { static REL::Relocation<_bhkRigidBody_setActivated> f{ REL::Offset(0xE085D0) }; return f.get(); }
    inline _hkpWorld_AddEntity     fn_AddEntity()              { static REL::Relocation<_hkpWorld_AddEntity>     f{ REL::Offset(0xAB0CB0) }; return f.get(); }
    inline _hkpWorld_RemoveEntity  fn_RemoveEntity()           { static REL::Relocation<_hkpWorld_RemoveEntity>  f{ REL::Offset(0xAB0E50) }; return f.get(); }
    inline _hkpBody_setPosition    fn_setPosition()            { static REL::Relocation<_hkpBody_setPosition>    f{ REL::Offset(0xAA8FD0) }; return f.get(); }
    inline _hkpBody_setRotation    fn_setRotation()            { static REL::Relocation<_hkpBody_setRotation>    f{ REL::Offset(0xAA9000) }; return f.get(); }
    inline _applyHardKeyFrame      fn_applyHardKeyFrame()      { static REL::Relocation<_applyHardKeyFrame>      f{ REL::Offset(0xAF6DD0) }; return f.get(); }
    inline _hkpWorld_UpdateFilterOnEntity fn_UpdateFilter()    { static REL::Relocation<_hkpWorld_UpdateFilterOnEntity> f{ REL::Offset(0xAB3110) }; return f.get(); }
    inline _TESRace_IsBeast        fn_TESRace_IsBeast()        { static REL::Relocation<_TESRace_IsBeast>        f{ REL::Offset(0x398940) }; return f.get(); }

    // ── Ported bhk wrapper layouts (HIGGS RE/havok.h; byte-validated in Markers) ──
    struct PortBhkShape {
        std::uint8_t  head[0x10];
        hkpShape*     shape;       // 0x10
        std::uint64_t unk18;
        std::uint32_t materialId;  // 0x20
        std::uint32_t pad24;
    };
    static_assert(sizeof(PortBhkShape) == 0x28);

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

    // Engine hkpBoxShape mirror for the Phase-0 slab write (CommonLibVR
    // RE/H/hkpBoxShape.h: sizeof 0x40, radius @0x20, halfExtents @0x30).
    struct PortHkpBoxShape {
        void*         vptr;              // 0x00
        std::uint16_t memSizeAndFlags;   // 0x08
        std::int16_t  referenceCount;    // 0x0A
        std::uint32_t pad0C;
        void*         userData;          // 0x10
        std::int32_t  type;              // 0x18  HK_SHAPE_BOX
        std::uint32_t pad1C;
        float         radius;            // 0x20  (hkpConvexShape convex radius)
        std::uint32_t pad24[3];
        float         halfExtents[4];    // 0x30
    };
    static_assert(sizeof(PortHkpBoxShape) == 0x40);
    static_assert(offsetof(PortHkpBoxShape, radius)      == 0x20);
    static_assert(offsetof(PortHkpBoxShape, halfExtents) == 0x30);

    // ── Constants ───────────────────────────────────────────────────────────
    constexpr std::uint32_t kSkinMaterialId = 0x233db702;
    constexpr float kSkyrimToHavok  = 0.0142875f;
    constexpr float kHavokToSkyrim  = 1.0f / 0.0142875f;
    constexpr int   kDoActivate     = 1;
    constexpr std::uint32_t kBit15  = 0x8000u;
    constexpr std::uint32_t kBit14  = 1u << 14;          // collision-OFF bit (HIGGS convention)
    constexpr unsigned kHiggsLayer  = 56u;               // L_HIGGSCOLLISION
    constexpr float kSlabFloorM     = 0.004f;            // NOTE: safety floor for slab dial writes (judgment value)
    constexpr float kHalfMinM       = 0.004f;            // sanity clamp on solved box halves
    constexpr float kHalfMaxM       = 0.15f;             // NOTE: a bad solve must never create a huge collider
    constexpr float kHalfEpsM       = 1e-5f;             // live half-extent write epsilon (0.01 mm): skip-if-unchanged (idempotent, slab-dial idiom)
    constexpr float kOpenHandCos    = 0.9659f;           // cos 15° — the open-hand creation gate
    constexpr int   kEnableDelayMs  = 100;               // review N4: HIGGS's own handWeaponCollisionEnableDelay
                                                         // default (higgs config.h:245 = 0.1 s), copied verbatim

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

    inline hkpRigidBody* HkOf(void* bhkWrapper) {
        if (!bhkWrapper || !IsLikelyPointer(bhkWrapper)) return nullptr;
        return reinterpret_cast<PortBhkRigidBody*>(bhkWrapper)->hkBody;
    }
    // FOREIGN (HIGGS-owned) wrapper walk: validate BOTH hops before dereferencing
    inline hkpRigidBody* HkOfValidated(void* bhkWrapper) {
        hkpRigidBody* hk = HkOf(bhkWrapper);
        return (hk && IsLikelyPointer(hk)) ? hk : nullptr;
    }

    // Remove-from-world primitive (verbatim Markers.cpp mechanics). The CALLER must
    // pass the FRESHLY-RESOLVED live world (the stored rig pointer is COMPARE-ONLY,
    // round-1 R3) — the getWorld()==hkpWorld guard turns a stale-body removal into
    // a clean no-op, which IS the forget path.
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

    // ── small matrix/quaternion helpers (plain float math, WXYZ quats) ───────
    struct QuatW { float w, x, y, z; };

    inline void QuatNormalize(QuatW& q) {
        const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (n > 1e-12f) { const float inv = 1.f / n; q.w *= inv; q.x *= inv; q.y *= inv; q.z *= inv; }
        else            { q = { 1.f, 0.f, 0.f, 0.f }; }
    }
    inline void Mat3Mul(const float a[9], const float b[9], float o[9]) {          // o = a·b
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                o[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
    }
    inline void Mat3TMul(const float a[9], const float b[9], float o[9]) {         // o = aᵀ·b
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                o[r * 3 + c] = a[0 * 3 + r] * b[0 * 3 + c] + a[1 * 3 + r] * b[1 * 3 + c] + a[2 * 3 + r] * b[2 * 3 + c];
    }
    // Shepperd extraction, row-major m[r*3+c], NORMALIZED before use (review N13 —
    // MarkerKeyframeRot's caller must never feed a drifting quat to applyHardKeyFrame).
    inline QuatW Mat3ToQuat(const float m[9]) {
        QuatW q;
        const float tr = m[0] + m[4] + m[8];
        if (tr > 0.f) {
            const float s = std::sqrt(tr + 1.f) * 2.f;
            q.w = 0.25f * s;
            q.x = (m[7] - m[5]) / s;
            q.y = (m[2] - m[6]) / s;
            q.z = (m[3] - m[1]) / s;
        } else if (m[0] > m[4] && m[0] > m[8]) {
            const float s = std::sqrt(1.f + m[0] - m[4] - m[8]) * 2.f;
            q.w = (m[7] - m[5]) / s;
            q.x = 0.25f * s;
            q.y = (m[1] + m[3]) / s;
            q.z = (m[2] + m[6]) / s;
        } else if (m[4] > m[8]) {
            const float s = std::sqrt(1.f + m[4] - m[0] - m[8]) * 2.f;
            q.w = (m[2] - m[6]) / s;
            q.x = (m[1] + m[3]) / s;
            q.y = 0.25f * s;
            q.z = (m[5] + m[7]) / s;
        } else {
            const float s = std::sqrt(1.f + m[8] - m[0] - m[4]) * 2.f;
            q.w = (m[3] - m[1]) / s;
            q.x = (m[2] + m[6]) / s;
            q.y = (m[5] + m[7]) / s;
            q.z = 0.25f * s;
        }
        QuatNormalize(q);
        return q;
    }

    // Spin a box's rider-frame axes about a chosen rider axis (rotOff := Rt·rotOff), angle in
    // degrees. Axis 0=X 1=Y 2=Z(down-the-bone). Extracted 2026-07-10 so the index boxes (0/1)
    // and the tip plate (3) share ONE transcription — byte-identical to the original inline
    // plate spin. Caller gates on |deg|>0.01 (a zero tilt is an identity multiply, skip it).
    inline void ApplyRiderTilt(float rotOff[9], float deg, int axis) {
        const float th = deg * 0.01745329252f;               // deg -> rad
        const float c = std::cos(th), s = std::sin(th);
        float Rt[9] = { 1,0,0, 0,1,0, 0,0,1 };
        switch (axis) {
        case 0: Rt[4] = c; Rt[5] = -s; Rt[7] = s; Rt[8] = c; break;   // about X
        case 1: Rt[0] = c; Rt[2] = s;  Rt[6] = -s; Rt[8] = c; break;  // about Y
        default: Rt[0] = c; Rt[1] = -s; Rt[3] = s; Rt[4] = c; break;  // about Z (down-the-bone)
        }
        float out9[9];                                        // rotOff := Rt * rotOff (rider frame)
        for (int r = 0; r < 3; ++r)
            for (int cc = 0; cc < 3; ++cc)
                out9[r * 3 + cc] = Rt[r * 3 + 0] * rotOff[0 * 3 + cc]
                                 + Rt[r * 3 + 1] * rotOff[1 * 3 + cc]
                                 + Rt[r * 3 + 2] * rotOff[2 * 3 + cc];
        std::memcpy(rotOff, out9, sizeof(out9));
    }

    // ── Node tables (names ★-verified vs higgs FingerAnimator + skeleton_female.nif) ──
    enum : int { kF10 = 0, kF11, kF12, kF20, kF21, kF22, kF30, kF31, kF32, kF40, kF41, kF42, kHand, kNodeCount };
    constexpr const char* kNodeNames[2][kNodeCount] = {
        { "NPC R Finger10 [RF10]", "NPC R Finger11 [RF11]", "NPC R Finger12 [RF12]",
          "NPC R Finger20 [RF20]", "NPC R Finger21 [RF21]", "NPC R Finger22 [RF22]",
          "NPC R Finger30 [RF30]", "NPC R Finger31 [RF31]", "NPC R Finger32 [RF32]",
          "NPC R Finger40 [RF40]", "NPC R Finger41 [RF41]", "NPC R Finger42 [RF42]",
          "NPC R Hand [RHnd]" },
        { "NPC L Finger10 [LF10]", "NPC L Finger11 [LF11]", "NPC L Finger12 [LF12]",
          "NPC L Finger20 [LF20]", "NPC L Finger21 [LF21]", "NPC L Finger22 [LF22]",
          "NPC L Finger30 [LF30]", "NPC L Finger31 [LF31]", "NPC L Finger32 [LF32]",
          "NPC L Finger40 [LF40]", "NPC L Finger41 [LF41]", "NPC L Finger42 [LF42]",
          "NPC L Hand [LHnd]" },
    };

    // Intern the 26 names ONCE at first use (BSFixedString construction takes the
    // game's string-table lock — the CapFix idiom).
    const std::array<RE::BSFixedString, 2 * kNodeCount>& InternedNames() {
        static const std::array<RE::BSFixedString, 2 * kNodeCount> s_names = [] {
            std::array<RE::BSFixedString, 2 * kNodeCount> a{};
            for (int h = 0; h < 2; ++h)
                for (int i = 0; i < kNodeCount; ++i)
                    a[h * kNodeCount + i] = RE::BSFixedString(kNodeNames[h][i]);
            return a;
        }();
        return s_names;
    }

    // ── State (main thread only, except the filter atomics) ─────────────────
    // Snapshot double-duty (2026-07-10 lag fix): `id` is advanced at PostVrikPostHiggs
    // (OnFrame — the frame-edge signal); the node poses land here at the FIRST
    // PrePhysicsStep fire of each frame (TakeBoneSnapshot), and the same fire
    // keyframes against them ONCE per id (review N1). Both callbacks run on the
    // game's main/update thread (PLANCK precedent at the same seams) — a plain
    // struct suffices, no double-buffer.
    struct Snapshot {
        std::uint64_t id = 0;
        float dtOwnMs  = 0.f;   // wall-clock interval (legacy source), diagnostic
        float dtUsedMs = 0.f;   // what actually fed invDt this frame
        float invDt = 90.f;                    // 1/frame-dt captured at snapshot time
        bool  valid[2] = { false, false };     // [0]=right, [1]=left
        float pos[2][kNodeCount][3]{};
        float rot[2][kNodeCount][9]{};         // row-major world rotations
        float scale[2] = { 1.f, 1.f };         // hand node world scale
    };
    Snapshot g_snap;
    std::uint64_t g_lastConsumedSnap = 0;

    // ── HIGGS-ANCHORED MODE 2 state (2026-07-12, the constant-pressure judder fix) ──
    // Mode 2 composes each box as  T_higgsBody · R_rel_smoothed · off_hand :
    //   T_higgsBody  = HIGGS's own hand body pose (predicted controller data — SOLID),
    //   R_rel        = the skeleton hand node's pose IN the HIGGS body's frame
    //                  (anatomically near-constant; heavily smoothed),
    //   off_hand     = the boxes' hand-frame offsets from SolveGeometry's mode-1
    //                  re-anchor (RELATIVE finger-in-hand pose — world noise cancels).
    // Riding the skeleton bones directly (modes 0/1) feeds a feedback loop under
    // sustained press: flesh pushback nudges the player's hand/body → the finger
    // BONES move → the boxes chase → new contacts → the charController is shoved
    // every frame (VR view judder, user-repro 2026-07-12). Mode 2 breaks the loop
    // by never letting world-space skeleton motion into the box pose.
    struct EffFrame {
        bool  valid = false;
        float pos[3]{};
        float rot[9] = { 1,0,0, 0,1,0, 0,0,1 };
    };
    EffFrame g_eff[2];                       // per-hand effective hand frame (mode 2)
    struct RelState {
        bool  init = false;
        float pos[3]{};
        QuatW rot{ 1.f, 0.f, 0.f, 0.f };
    };
    RelState g_rel[2];                       // smoothed hand-node-in-HIGGS-body relation
    // kRelAlpha was a compile-time 0.05. It is now the live knob handBoxRelAlpha (2026-08-02):
    // the residual (EMA(R) - R) displaces the box from the hand every frame, and at alpha 1 the
    // algebra collapses to E == T_handnode (byte-identical to mode 1's target). Ships at 0.05.
    constexpr float kRelAlphaDefault = 0.05f;

    // ── JITTER DIAGNOSTIC (2026-08-02) ────────────────────────────────────────────────────
    // The boxes are MOTION_KEYFRAMED with infinite mass, so contacts CANNOT move them: their
    // position is 100% commanded. Any shake therefore lives in the command, and these channels
    // measure the command's two halves separately — how far the TARGET strays from the hand
    // bone (relErr/dRel), and whether the body actually LANDS where it was told (gain).
    std::atomic<std::uint32_t> g_stepDtBits{ 0 };   // last real hkpWorld step dt, raw float bits
    // Physics time accumulated across every substep since the last keyframe. THIS is the interval
    // the previous keyframe's velocity was actually integrated over, and therefore the correct
    // basis for the next one. Integer microseconds so the physics-thread add stays trivial.
    // MEASURED 2026-08-02, 764 samples in a live shaking session:
    //   dtStep (real)  mean 13.92 ms  stdev  6.82
    //   dtOwn  (clock) mean 19.81 ms  stdev 23.08   <- 3.4x the variance, saturates its 100 ms clamp
    //   gain = dtStep x invDt: range 0.11..1.87, stdev 0.26, 37% of frames outside 0.80..1.25.
    // A gain of 0.11 executes 11% of the commanded motion; 1.87 executes 187%. That multiplicative
    // per-frame noise on position IS the shake.
    std::atomic<bool> g_sceneSuspend{ false };   // an OStim/SexLab scene is running
    std::atomic<std::uint32_t> g_stepAccumUs{ 0 };
    float g_prevRelErr[2][3]{};
    bool  g_prevRelErrOk[2]{};
    float g_dRelU[2]{};                            // per-frame movement of the box RELATIVE to
                                                   // the hand = the shake, in game units
    float g_relErrU[2]{};
    float g_prevRelPos[2][3]{};                    // last frame's RAW (unfiltered) relation
    bool  g_prevRelPosOk[2]{};
    float g_dRU[2]{};                              // per-frame movement of the RAW relation R.
                                                   // This is the DRIVER of the whole effect and
                                                   // stays meaningful at every alpha — unlike
                                                   // dRel, which is identically 0 at alpha 1 by
                                                   // construction. If the two sources were truly
                                                   // in phase, dR would be ~0; whatever dR reads
                                                   // is the frame-of-motion the anchor is stale by.

    inline void QuatToMat3(const QuatW& q, float m[9]) {
        const float xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z;
        const float xy=q.x*q.y, xz=q.x*q.z, yz=q.y*q.z;
        const float wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
        m[0]=1-2*(yy+zz); m[1]=2*(xy-wz);   m[2]=2*(xz+wy);
        m[3]=2*(xy+wz);   m[4]=1-2*(xx+zz); m[5]=2*(yz-wx);
        m[6]=2*(xz-wy);   m[7]=2*(yz+wx);   m[8]=1-2*(xx+yy);
    }
    inline QuatW QuatNlerp(const QuatW& a, const QuatW& b, float t) {
        float d = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
        const float s = d < 0.f ? -1.f : 1.f;          // shortest arc
        QuatW o{ a.w + (s*b.w - a.w)*t, a.x + (s*b.x - a.x)*t,
                 a.y + (s*b.y - a.y)*t, a.z + (s*b.z - a.z)*t };
        QuatNormalize(o);
        return o;
    }

    // Compute the per-hand effective frame for this snapshot. Mode 2 with a live
    // HIGGS body: E = T_higgs · R_rel_smoothed. Anything else (mode 0/1, HIGGS body
    // missing): E = the snapshot hand node (byte-identical to the old behavior).
    void UpdateEffFrames()
    {
        auto* h = Interop::GetHiggs();
        const int mode = ObjectHold::HandBoxFollowMode();
        for (int hand = 0; hand < 2; ++hand) {
            EffFrame& e = g_eff[hand];
            e.valid = g_snap.valid[hand];
            if (!e.valid) continue;
            const float* hp = g_snap.pos[hand][kHand];
            const float* hr = g_snap.rot[hand][kHand];
            hkpRigidBody* hb = (mode == 2 && h) ? HkOfValidated(h->GetHandRigidBody(hand == 1)) : nullptr;
            // ── LEASH, PART 1 (2026-07-29): REJECT A RUNAWAY ANCHOR ─────────────────────────
            // Mode 2 rides HIGGS's hand body. If that body is ever stale/displaced (hand
            // disabled, two-handing, beast recreation, a tracking glitch), our boxes would
            // follow it PERFECTLY to the wrong place — and because the target moves smoothly
            // the velocity clamp in KeyframeAll never fires, so they'd stay wrong forever.
            // That is the "collision far away" report. Ground truth is the skeleton hand node,
            // which is always valid: if the anchor is further than the leash from it, drop to
            // the node for this frame.
            if (hb) {
                const hkVector4& t = hb->getPosition();
                const float dx = t(0) * kHavokToSkyrim - hp[0];
                const float dy = t(1) * kHavokToSkyrim - hp[1];
                const float dz = t(2) * kHavokToSkyrim - hp[2];
                const float leash = ObjectHold::HandBoxLeashU();
                if (leash > 0.f && (dx*dx + dy*dy + dz*dz) > leash * leash) {
                    // Log the first 3 in full, then a running TOTAL every 256. Without the total
                    // a capped log cannot distinguish "fired 6 times" (a real glitch, fixed) from
                    // "fires every frame" (a false positive silently degrading mode 2 to mode 0).
                    static std::uint32_t s_warned[2] = { 0, 0 };
                    static std::uint64_t s_total[2]  = { 0, 0 };
                    ++s_total[hand];
                    if (s_warned[hand] < 3) {
                        ++s_warned[hand];
                        logger::info("HBOX leash: {} HIGGS hand anchor is {:.1f}u from the hand node "
                                     "(> {:.0f}u) — falling back to the skeleton node this frame",
                                     hand == 1 ? "L" : "R",
                                     std::sqrt(dx*dx + dy*dy + dz*dz), leash);
                    } else if ((s_total[hand] & 0xFF) == 0) {
                        logger::info("HBOX leash: {} anchor rejected {} times so far (latest {:.1f}u) "
                                     "— a HIGH count here means the leash is firing routinely, not "
                                     "catching a one-off glitch",
                                     hand == 1 ? "L" : "R", s_total[hand],
                                     std::sqrt(dx*dx + dy*dy + dz*dz));
                    }
                    hb = nullptr;                        // treat exactly like "HIGGS body absent"
                }
            }
            if (!hb) {                                   // modes 0/1, or HIGGS body absent/rejected
                std::memcpy(e.pos, hp, sizeof(e.pos));
                std::memcpy(e.rot, hr, sizeof(e.rot));
                g_rel[hand].init = false;                // re-init the relation on return
                continue;
            }
            const hkVector4& bp = hb->getPosition();
            const hkQuaternion& bq = hb->getRotation();
            const float gp[3] = { bp(0) * kHavokToSkyrim, bp(1) * kHavokToSkyrim, bp(2) * kHavokToSkyrim };
            QuatW gq{ bq.m_vec(3), bq.m_vec(0), bq.m_vec(1), bq.m_vec(2) };
            QuatNormalize(gq);
            float gR[9];
            QuatToMat3(gq, gR);
            // fresh relation: R_rel = T_higgs^-1 · T_handnode
            float relRot[9];
            Mat3TMul(gR, hr, relRot);                    // gR^T · hr
            const float d[3] = { hp[0] - gp[0], hp[1] - gp[1], hp[2] - gp[2] };
            float relPos[3];
            for (int c = 0; c < 3; ++c)
                relPos[c] = gR[0*3+c]*d[0] + gR[1*3+c]*d[1] + gR[2*3+c]*d[2];
            QuatW relQ = Mat3ToQuat(relRot);
            RelState& rs = g_rel[hand];
            const float relAlpha = ObjectHold::HandBoxRelAlpha();
            if (!rs.init || relAlpha >= 0.999f) {
                // alpha 1 -> S == R -> E == T_handnode exactly: no filter, no phase residual.
                rs.init = true;
                std::memcpy(rs.pos, relPos, sizeof(relPos));
                rs.rot = relQ;
            } else {
                for (int c = 0; c < 3; ++c) rs.pos[c] += (relPos[c] - rs.pos[c]) * relAlpha;
                rs.rot = QuatNlerp(rs.rot, relQ, relAlpha);
            }
            // ── CHANNEL A: the shake, measured. Because gR is orthonormal, |E.pos - hp| is
            // EXACTLY |rs.pos - relPos| in game units — not a proxy for the displacement, it IS
            // the displacement of the box target from the hand bone it should sit on.
            {
                const float ex = rs.pos[0]-relPos[0], ey = rs.pos[1]-relPos[1], ez = rs.pos[2]-relPos[2];
                g_relErrU[hand] = std::sqrt(ex*ex + ey*ey + ez*ez);
                if (g_prevRelErrOk[hand]) {
                    const float dx = ex-g_prevRelErr[hand][0], dy = ey-g_prevRelErr[hand][1],
                                dz = ez-g_prevRelErr[hand][2];
                    g_dRelU[hand] = std::sqrt(dx*dx + dy*dy + dz*dz);
                } else g_dRelU[hand] = 0.f;
                g_prevRelErr[hand][0]=ex; g_prevRelErr[hand][1]=ey; g_prevRelErr[hand][2]=ez;
                g_prevRelErrOk[hand] = true;
            }
            // ── CHANNEL A2: the RAW relation's per-frame movement. Survives alpha = 1 (where the
            // filtered residual is identically zero), so the instrument still reports something
            // in the SHIPPED configuration — the failure mode this project has now hit twice.
            {
                if (g_prevRelPosOk[hand]) {
                    const float rx = relPos[0]-g_prevRelPos[hand][0],
                                ry = relPos[1]-g_prevRelPos[hand][1],
                                rz = relPos[2]-g_prevRelPos[hand][2];
                    g_dRU[hand] = std::sqrt(rx*rx + ry*ry + rz*rz);
                } else g_dRU[hand] = 0.f;
                std::memcpy(g_prevRelPos[hand], relPos, sizeof(relPos));
                g_prevRelPosOk[hand] = true;
            }
            // E = T_higgs · R_rel_smoothed
            float sR[9];
            QuatToMat3(rs.rot, sR);
            Mat3Mul(gR, sR, e.rot);
            for (int r = 0; r < 3; ++r)
                e.pos[r] = gp[r] + gR[r*3+0]*rs.pos[0] + gR[r*3+1]*rs.pos[1] + gR[r*3+2]*rs.pos[2];
        }
    }

    // Player-space warp state (2026-07-12 stability fix): the player's world position
    // at the previous consumed snapshot. Boxes are TELEPORTED by this frame's player
    // delta before keyframing (HIGGS's g_playerSpaceBodies technique) so the keyframe
    // velocity carries only hand-relative motion — locomotion never becomes velocity.
    float g_prevPlayerPos[3]{};
    bool  g_prevPlayerPosValid = false;

    struct BoxDef {
        int   rider = 0;                       // snapshot node index the box rides
        float off[3]{};                        // box center in the rider's frame (game units)
        float rotOff[9] = { 1,0,0, 0,1,0, 0,0,1 };   // box axes in the rider's frame
        float half[3]{};                       // half extents, METRES
    };                                         // (2026-07-10: ALL fields re-solved every snapshot — off/rotOff feed
                                               //  BoxWorldPose live; half is written to the engine shape by
                                               //  ResolveGeometryLive, same idempotent idiom as the HIGGS slab dial)
    struct HandRig {
        bool   live = false;
        void*  bodyMem[4]{};                   // PortBhkRigidBody* (opaque)
        void*  bhkWorld = nullptr;             // creation world (raw, for compares/logging)
        // ★ 2026-07-21 (player-hand orphan capsules): the 2026-07-14 orphan fix gave the NPC rigs a
        // STRONG NiPointer<bhkWorld> (NpcFingerTest FingerRig::heldWorld) but was NEVER applied here.
        // A compare-only void* forces the orphan-vs-UAF dilemma: DestroyHand removed from the CURRENT
        // world, so after a cell change the bodies — which live in the OLD world — hit the membership
        // guard and were silently FORGOTTEN, left floating in the swapped-away world. bhkWorld is
        // NiRefObject-derived, so holding a strong ref keeps the creation world ALIVE and lets
        // teardown deterministically remove from the world the bodies are ACTUALLY in.
        RE::NiPointer<RE::bhkWorld> heldWorld;
        BoxDef def[4];
        float  scaleAtCreate = 1.f;
        std::uint32_t lastWord = 0;
        std::chrono::steady_clock::time_point createdAt{};
    };
    HandRig g_rig[2];                          // [0]=right, [1]=left

    // ── PLAYER GENITAL WAND state (2026-08-18) — declared THIS early because
    //    AnythingActive and PlayerSpaceWarp read g_wand and both precede the wand
    //    machinery (which lives just above OnPrePhysicsStep, with the full rationale).
    constexpr int kWandSegs = 2;
    // full SOS chain, base→tip; we resolve whichever subset this skeleton actually has
    constexpr const char* kWandChain[] = {
        "NPC GenitalsBase [GenBase]",
        "NPC Genitals01 [Gen01]", "NPC Genitals02 [Gen02]", "NPC Genitals03 [Gen03]",
        "NPC Genitals04 [Gen04]", "NPC Genitals05 [Gen05]", "NPC Genitals06 [Gen06]",
    };
    constexpr int   kWandChainN = (int)std::size(kWandChain);
    constexpr float kWandTipPadU = 0.7f;     // tip flesh extends past the last bone

    struct WandSnap {
        bool  valid = false;
        float p[3][3]{};                     // base / mid / tip, world game units
    };
    WandSnap g_wandSnap;

    struct WandRig {
        bool   live = false;
        void*  bodyMem[kWandSegs]{};
        void*  bhkWorld = nullptr;           // compare-only
        RE::NiPointer<RE::bhkWorld> heldWorld;   // STRONG — same orphan-vs-UAF fix as HandRig
        std::uint32_t lastWord = 0;
        std::chrono::steady_clock::time_point createdAt{};
    };
    WandRig g_wand;

    // ── PLAYER HEAD BOX state (2026-09-03) — declared beside the wand for the same reason:
    //    AnythingActive and PlayerSpaceWarp both read it and both precede the machinery.
    //    ONE keyframed box riding the VRIK-posed 3rd-person head node. See Tuning.h for the
    //    full rationale (why it cannot stop the player, and the H2 velocity risk).
    constexpr const char* kHeadNode = "NPC Head [Head]";
    // rad/s. A head turn is a few rad/s; anything past this is a tracking glitch or a load
    // spike, and on a box this size the corner speed (w x r) is what reaches an NPC bone.
    constexpr float kHeadMaxAngVel = 12.f;

    struct HeadSnap {
        bool  valid = false;
        float pos[3]{};                      // head node world translate, game units
        float rot[9]{};                       // row-major; COLUMNS are the axes (BoxWorldPose idiom)
        float scale = 1.f;                    // node world scale (RaceMenu / race / VRIK)
        bool  hmdValid = false;               // is the headset node resolvable this frame?
        float hmdPos[3]{};
        bool  rodeHmd  = false;               // this snapshot's pose IS the headset, not the bone
        // ★ KISS (2026-09-12): the FULL-ROTATION headset (+0x570) sampled in this same pass, so the
        // mouth probe can pitch and roll with the player's head while the box keeps its rider.
        bool  fullRotValid = false;           // +0x570 resolved AND passed the position sanity check
        float fullRot[9]{};                   // row-major, columns are the axes (same idiom as rot)
        float fwdZUpright = 0.f;              // forward.z of +0x580 — diagnostic: ~0 always if yaw-only
        float fwdZFull    = 0.f;              // forward.z of +0x570 — goes negative when looking down
    };
    HeadSnap g_headSnap;

    struct HeadRig {
        bool   live = false;
        void*  bodyMem = nullptr;
        void*  bhkWorld = nullptr;           // compare-only
        RE::NiPointer<RE::bhkWorld> heldWorld;   // STRONG — same orphan-vs-UAF fix as HandRig
        std::uint32_t lastWord = 0;
        std::chrono::steady_clock::time_point createdAt{};
    };
    HeadRig g_head;



    bool g_registered = false;

    // Hot-path caches for the comparison callback (physics thread — atomics only)
    std::atomic<bool>          g_boxLive{ false };
    // g_boxGroup = the group OUR bodies actually carry (may be the private group).
    // g_playerGroup = the player's REAL group, always straight from HIGGS's hand word. The belts
    // need both: ours to recognize our own bodies, the player's to recognize HIGGS's hands and the
    // player's own biped once we are no longer sharing a group with them.
    std::atomic<std::uint32_t> g_boxGroup{ 0 };
    std::atomic<std::uint32_t> g_playerGroup{ 0 };
    std::atomic<std::uint32_t> g_wandPart{ 9 };
    std::atomic<bool>          g_wandLive{ false };
    std::atomic<std::uint32_t> g_headPart{ 10 };
    std::atomic<bool>          g_headLive{ false };
    std::atomic<std::uint32_t> g_boxPart{ 4 };     // our sub-layer (handBoxSubLayer, sanitized)
    std::atomic<bool>          g_logFirstIgnore{ false };

    // Phase-0 slab baselines (first-seen per HIGGS body identity; review N7 keeps
    // the convex radius too — a user-configured HandCollisionBoxRadius inflates the
    // effective slab past the dialed extents)
    struct SlabBase {
        const void* hkBody = nullptr;          // identity key — COMPARED, never dereferenced after capture
        float half[3]{};
        float radius = 0.f;
        bool  captured = false;
    };
    SlabBase g_slabBase[2];

    // ── SKIP-reason throttle (1 line per reason per second) ─────────────────
    void LogSkip(const char* reason) {
        static std::chrono::steady_clock::time_point s_last[8]{};
        static const char* s_reasons[8] = { "noHiggs", "noHiggsHand", "fist", "degenerate",
                                            "allocFail", "?", "?", "?" };
        int idx = 5;
        for (int i = 0; i < 5; ++i) if (std::strcmp(reason, s_reasons[i]) == 0) { idx = i; break; }
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last[idx] < std::chrono::milliseconds(1000)) return;
        s_last[idx] = now;
        logger::info("HBOX SKIP reason={}", reason);
    }

    bool IsPlayerBeast(RE::PlayerCharacter* player) {
        // review N8: read the race beast flag directly (the same engine check HIGGS
        // uses, TESRace_IsBeast @0x398940) — the extents heuristic broke whenever the
        // user customized higgs_vr.ini. The heuristic survives as a log-only cross-check.
        auto* race = player ? player->GetRace() : nullptr;
        return race && fn_TESRace_IsBeast()(race);
    }

    bool AnythingActive() {
        if (g_rig[0].live || g_rig[1].live || g_wand.live || g_head.live) return true;
        if (ObjectHold::PlayerWandOn()) return true;
        if (ObjectHold::HeadBoxOn()) return true;
        if (ObjectHold::HandBoxEnabled()) return true;
        if (ObjectHold::HiggsSlabHalfX() >= 0.f || ObjectHold::HiggsSlabHalfY() >= 0.f ||
            ObjectHold::HiggsSlabHalfZ() >= 0.f) return true;
        if (ObjectHold::HandBoxDump() > 0.5f) return true;
        if (ObjectHold::HandBoxDumpNow() > 0.5f) return true;
        return false;
    }

    // The effective group for PPB's own player-attached bodies. Mode 2 swaps only while HIGGS
    // holds something — that is exactly the window where its contact rule fires, so adjacency
    // stays native the rest of the time.
    std::uint32_t EffectiveGroup(std::uint32_t playerGroup, bool higgsHolding)
    {
        const int mode = ObjectHold::HandBoxPrivGroupMode();
        if (mode == 0) return playerGroup;
        if (mode == 2 && !higgsHolding) return playerGroup;
        return ObjectHold::HandBoxPrivGroupId();
    }

    bool HiggsHoldingAnything()
    {
        auto* h = Interop::GetHiggs();
        if (!h) return false;
        return h->IsHoldingObject(false) || h->IsHoldingObject(true) || h->IsTwoHanding();
    }

    // ── DESTROY (fresh live world in, stored pointer compare-only) ──────────
    void DestroyHand(int hand, void* liveWorld, const char* reason)
    {
        auto& rig = g_rig[hand];
        if (!rig.live) return;
        const bool inLive = rig.bhkWorld == liveWorld;
        // Remove from the world the bodies were CREATED in (held alive by rig.heldWorld), NOT from
        // whatever world is live now — that is precisely how they used to be orphaned on a cell change.
        void* owner = rig.heldWorld ? static_cast<void*>(rig.heldWorld.get()) : liveWorld;
        if (owner)
            for (void* mem : rig.bodyMem)
                if (mem) RemoveBody(mem, owner);
        logger::info("HBOX DESTROY hand={} reason={} removedInWorld={} ownerHeld={}",
                     hand == 1 ? "L" : "R", reason, inLive, rig.heldWorld != nullptr);
        rig = HandRig{};
        // filter atomic cleared AFTER removal, and only when NEITHER hand still has
        // live bodies (the NpcFinger Defect-4 ordering class)
        if (!g_rig[0].live && !g_rig[1].live)
            g_boxLive.store(false, std::memory_order_relaxed);
    }

    // ── geometry ─────────────────────────────────────────────────────────────
    // World pose of one box from the current snapshot: pos = rider + R_rider·off,
    // rot = R_rider·rotOff. Offsets were solved from live world positions, so any
    // node scale is already folded in (rigs are re-solved on >1% hand-scale drift).
    void BoxWorldPose(int hand, const BoxDef& d, float posU[3], QuatW& rot)
    {
        // Mode-2 (HIGGS-anchored): hand-riding boxes compose against the effective
        // frame, not the raw skeleton hand node (see UpdateEffFrames).
        const bool eff = d.rider == kHand && g_eff[hand].valid;
        const float* rp = eff ? g_eff[hand].pos : g_snap.pos[hand][d.rider];
        const float* rr = eff ? g_eff[hand].rot : g_snap.rot[hand][d.rider];
        for (int r = 0; r < 3; ++r)
            posU[r] = rp[r] + rr[r * 3 + 0] * d.off[0] + rr[r * 3 + 1] * d.off[1] + rr[r * 3 + 2] * d.off[2];
        float Rw[9];
        Mat3Mul(rr, d.rotOff, Rw);
        rot = Mat3ToQuat(Rw);
    }

    // Solve the 4 boxes from the current snapshot. 2026-07-10: called EVERY consumed snapshot
    // (ResolveGeometryLive) — anchors, tilt, length AND half-extents are all live now (no
    // recreate needed; the half write is a separate idempotent engine shape edit). Box basis:
    // colX=across, colY=dorsal, colZ=down-the-bone (children's local translation is (0,0,L)).
    bool SolveGeometry(int hand, BoxDef out[4])
    {
        const float tipFrac = ObjectHold::HandBoxTipFrac();
        const float plateTipFrac = ObjectHold::HandBoxPlateTipFrac();   // box 3 only (2026-07-10: index long, plate short)
        const float plateLenFrac = ObjectHold::HandBoxPlateLenFrac();   // box 3 only; accessor-clamped 0.2..1.0
        const float slabLenFrac  = ObjectHold::HandBoxSlabLenFrac();    // box 2 only; accessor-clamped 0.2..1.0 (2026-07-10)
        const float plateTiltDeg  = ObjectHold::HandBoxPlateTiltDeg();  // box 3 only; +deg = toward the index
        const int   plateTiltAxis = ObjectHold::HandBoxPlateTiltAxis(); // 0=X 1=Y 2=Z(down-the-bone, default)
        const float plateTilt2Deg  = ObjectHold::HandBoxPlateTilt2Deg();  // box 3 only; 2nd composed stage (2026-07-10)
        const int   plateTilt2Axis = ObjectHold::HandBoxPlateTilt2Axis(); // 0=X 1=Y(default) 2=Z(down-the-bone)
        const float padM    = ObjectHold::HandBoxPad();
        const float idxW    = ObjectHold::HandBoxIdxHalfW();
        const float idxT    = ObjectHold::HandBoxIdxHalfT();
        const float slabT   = ObjectHold::HandBoxSlabHalfT();
        // ── per-box LIVE DIALS (2026-07-10) — offsets GAME UNITS in the rider frame, tilt degrees,
        //    plate half overrides GAME UNITS (-1 = keep solved). All read live (this runs per frame). ──
        const float plateOffX = ObjectHold::HandBoxPlateOffX();
        const float plateOffY = ObjectHold::HandBoxPlateOffY();
        const float plateOffZ = ObjectHold::HandBoxPlateOffZ();
        const float plateHalfW = ObjectHold::HandBoxPlateHalfW();       // -1 = solved; else override half[0] (across)
        const float plateHalfT = ObjectHold::HandBoxPlateHalfT();       // -1 = solved; else override half[1] (dorsal)
        const float idxOffX = ObjectHold::HandBoxIdxOffX();
        const float idxOffY = ObjectHold::HandBoxIdxOffY();
        const float idxOffZ = ObjectHold::HandBoxIdxOffZ();
        const float idxTiltDeg  = ObjectHold::HandBoxIdxTiltDeg();      // boxes 0/1; +deg = about the tilt axis
        const int   idxTiltAxis = ObjectHold::HandBoxIdxTiltAxis();     // 0=X 1=Y 2=Z(down-the-bone, default)

        const auto P = [&](int i) -> const float* { return g_snap.pos[hand][i]; };
        const auto R = [&](int i) -> const float* { return g_snap.rot[hand][i]; };
        const auto localOf = [&](int rider, const float w[3], float o[3]) {
            const float* rp = P(rider); const float* rr = R(rider);
            const float d[3] = { w[0] - rp[0], w[1] - rp[1], w[2] - rp[2] };
            for (int c = 0; c < 3; ++c)   // Rᵀ·d (transpose = inverse; rotations orthonormal)
                o[c] = rr[0 * 3 + c] * d[0] + rr[1 * 3 + c] * d[1] + rr[2 * 3 + c] * d[2];
        };
        const auto dist = [&](int a, int b) {
            const float* pa = P(a); const float* pb = P(b);
            const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        // fingertip extrapolated (no tip node): tip = F_k2 + zAxis(F_k2)·plateTipFrac·|F_k2−F_k1|.
        // tipOf feeds ONLY box 3 — the index boxes keep tipFrac; handBoxPlateTipFrac decouples them.
        const auto tipOf = [&](int k2, int k1, float w[3]) {
            const float* p2 = P(k2); const float* r2 = R(k2);
            const float  L  = dist(k2, k1);
            for (int r = 0; r < 3; ++r) w[r] = p2[r] + r2[r * 3 + 2] * plateTipFrac * L;
        };

        // Box 0 — index proximal: rides F10, spans F10->F11
        {
            float v[3]; localOf(kF10, P(kF11), v);
            const float L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (L < 0.3f) return false;                      // sub-4mm knuckle span = broken snapshot
            out[0].rider = kF10;
            for (int c = 0; c < 3; ++c) out[0].off[c] = v[c] * 0.5f;
            out[0].half[0] = idxW; out[0].half[1] = idxT; out[0].half[2] = 0.5f * L * kSkyrimToHavok;
        }
        // Box 1 — index distal (tip incl., ×0.90 taper): rides F11
        {
            float v[3]; localOf(kF11, P(kF12), v);
            const float L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (L < 0.3f) return false;
            const float halfLenU = 0.5f * L * (1.f + tipFrac);
            out[1].rider = kF11;
            for (int c = 0; c < 3; ++c) out[1].off[c] = v[c] / L * halfLenU;
            out[1].half[0] = 0.90f * idxW; out[1].half[1] = 0.90f * idxT;
            out[1].half[2] = halfLenU * kSkyrimToHavok;
        }
        // Index-box LIVE DIALS (2026-07-10): nudge + spin boxes 0/1 in their own rider frames
        // (F10 / F11). Applied BEFORE the Mode-1 re-anchor so the offset flows through into the
        // hand-frame center (the tilt is discarded by Mode 1 exactly as the plate tilt is —
        // pre-existing limitation, Mode 1 rebuilds rotOff from R_handᵀ·R_rider).
        for (int b = 0; b < 2; ++b) {
            out[b].off[0] += idxOffX; out[b].off[1] += idxOffY; out[b].off[2] += idxOffZ;
            if (std::fabs(idxTiltDeg) > 0.01f) ApplyRiderTilt(out[b].rotOff, idxTiltDeg, idxTiltAxis);
        }
        // Box 2 — 3-finger slab, proximal: rides F20; min/max projection of
        // {F20,F30,F40,F21,F31,F41} in F20's frame
        // Box 3 — 3-finger tip plate: rides F21; projection of {F21,F31,F41, tip2,tip3,tip4}
        for (int box = 2; box <= 3; ++box) {
            const int rider = (box == 2) ? kF20 : kF21;
            float pts[6][3];
            int   n = 0;
            if (box == 2) {
                const int src[6] = { kF20, kF30, kF40, kF21, kF31, kF41 };
                for (int s : src) { std::memcpy(pts[n++], P(s), sizeof(float) * 3); }
            } else {
                const int src[3] = { kF21, kF31, kF41 };
                for (int s : src) { std::memcpy(pts[n++], P(s), sizeof(float) * 3); }
                tipOf(kF22, kF21, pts[n++]);
                tipOf(kF32, kF31, pts[n++]);
                tipOf(kF42, kF41, pts[n++]);
            }
            float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
            for (int p = 0; p < n; ++p) {
                float l[3]; localOf(rider, pts[p], l);
                for (int c = 0; c < 3; ++c) { mn[c] = std::min(mn[c], l[c]); mx[c] = std::max(mx[c], l[c]); }
            }
            out[box].rider = rider;
            for (int c = 0; c < 3; ++c) out[box].off[c] = 0.5f * (mn[c] + mx[c]);
            out[box].half[0] = 0.5f * (mx[0] - mn[0]) * kSkyrimToHavok + padM;   // across
            out[box].half[1] = slabT;                                            // dorsal thickness = knob
            out[box].half[2] = 0.5f * (mx[2] - mn[2]) * kSkyrimToHavok + padM;   // down-the-bone
            // Slab length scale (2026-07-10, box 2 only): the plate's dial mirrored — shrink the
            // down-the-bone extent while PINNING the BASE face (the hand/knuckle-ward mn[2] edge in
            // the rider frame), so the cut comes off the FINGERTIP-ward end (box 3 covers the tips).
            // Same placement rules as the plate's: before the Mode-1 re-anchor (off[2] must still be
            // rider-frame) and before the kHalfMinM sanity clamp (a floored half would nudge the base
            // face out by <4 mm — accepted, same as the plate).
            if (box == 2 && slabLenFrac < 1.f) {
                const float baseU = out[box].off[2] - out[box].half[2] * kHavokToSkyrim;
                out[box].half[2] *= slabLenFrac;
                out[box].off[2]   = baseU + out[box].half[2] * kHavokToSkyrim;
            }
            // Plate length scale (2026-07-10, box 3 only): shrink the down-the-bone extent while
            // PINNING the front face — the fingertip-ward mx[2] edge in the rider frame — so the
            // plate recedes toward the knuckles and the extrapolated tips stay covered.
            // NOTE: runs before the Mode-1 re-anchor (off[2] must still be rider-frame) and before
            // the kHalfMinM sanity clamp (a floored half would nudge the front face out by <4 mm — accepted).
            if (box == 3 && plateLenFrac < 1.f) {
                const float frontU = out[box].off[2] + out[box].half[2] * kHavokToSkyrim;
                out[box].half[2] *= plateLenFrac;
                out[box].off[2]   = frontU - out[box].half[2] * kHavokToSkyrim;
            }
            // Plate TILT (2026-07-10, box 3 only): spin the plate in the rider's frame about a chosen
            // axis. Default axis 2 = down-the-bone (spins the plate flat-on-the-fingers, toward the
            // index for +deg). Axis/sign are knobs so the orientation can be corrected live without a
            // rebuild if 15 deg lands on the wrong axis or the wrong way round.
            if (box == 3 && std::fabs(plateTiltDeg) > 0.01f)
                ApplyRiderTilt(out[box].rotOff, plateTiltDeg, plateTiltAxis);
            // Plate TILT STAGE 2 (2026-07-10): a SECOND rider-frame spin composed ON TOP of stage 1.
            // ApplyRiderTilt is rotOff := Rt·rotOff, so calling it again here yields rotOff := R2·R1·rotOff
            // (order fixed: stage 1 first). Lets the plate be oriented on two axes without a rebuild.
            if (box == 3 && std::fabs(plateTilt2Deg) > 0.01f)
                ApplyRiderTilt(out[box].rotOff, plateTilt2Deg, plateTilt2Axis);
            // Plate LIVE DIALS (2026-07-10): offset nudge (game units, rider frame, added on top of
            // the len-frac recede + before the Mode-1 re-anchor so it survives into the hand frame),
            // and the half OVERRIDES — plateHalfW/T replace the SOLVED half[0]/half[1] in game units
            // (-1 = keep solved). half[2] (down-the-bone) stays solved+len-fraced. The end-of-solve
            // sanity clamp still floors any override below kHalfMinM.
            if (box == 3) {
                out[box].off[0] += plateOffX; out[box].off[1] += plateOffY; out[box].off[2] += plateOffZ;
                if (plateHalfW >= 0.f) out[box].half[0] = plateHalfW * kSkyrimToHavok;   // game u -> metres
                if (plateHalfT >= 0.f) out[box].half[1] = plateHalfT * kSkyrimToHavok;
            }
        }

        // Mode 1 — fixed hand frame: same solved boxes re-anchored on the HAND node.
        // Review N3: the GetFingerValues curl drive is REJECTED (it returns stale
        // last-grab values whenever the hand is EMPTY — exactly when the boxes are
        // collidable), so Mode 1 is fixed open-hand offsets only, no curl.
        if (ObjectHold::HandBoxFollowMode() >= 1) {   // mode 2 needs the same hand-frame offsets
            const float* hp = P(kHand); const float* hr = R(kHand);
            for (int b = 0; b < 4; ++b) {
                const float* rp = P(out[b].rider);
                const float* rr = R(out[b].rider);
                float center[3];
                for (int r = 0; r < 3; ++r)
                    center[r] = rp[r] + rr[r * 3 + 0] * out[b].off[0] + rr[r * 3 + 1] * out[b].off[1] + rr[r * 3 + 2] * out[b].off[2];
                float offH[3];
                const float d[3] = { center[0] - hp[0], center[1] - hp[1], center[2] - hp[2] };
                for (int c = 0; c < 3; ++c)
                    offH[c] = hr[0 * 3 + c] * d[0] + hr[1 * 3 + c] * d[1] + hr[2 * 3 + c] * d[2];
                float rotH[9];
                Mat3TMul(hr, rr, rotH);                       // R_handᵀ·R_rider
                out[b].rider = kHand;
                std::memcpy(out[b].off, offH, sizeof(offH));
                std::memcpy(out[b].rotOff, rotH, sizeof(rotH));
            }
        }

        // sanity clamps — a bad solve must never bake a paper or building-sized box
        // (the T axes were already floored at 0.010 m by the accessors, review N10)
        for (int b = 0; b < 4; ++b)
            for (int c = 0; c < 3; ++c)
                out[b].half[c] = std::clamp(out[b].half[c], kHalfMinM, kHalfMaxM);
        return true;
    }

    // ── body factory — MarkerCreate clone + the spec-§1.5 deltas ─────────────
    void* CreateBoxBody(void* bhkWorld, const float posU[3], const QuatW& rot,
                        const float halfM[3], std::uint32_t filterInfo)
    {
        if (!bhkWorld) return nullptr;
        void* shapeMem = GameHeapAllocate(sizeof(PortBhkShape));
        if (!shapeMem) return nullptr;
        hkVector4 halfExtents;
        halfExtents.set(halfM[0], halfM[1], halfM[2], 0.f);   // delta 1: non-uniform (HIGGS does exactly this)
        fn_bhkBoxShape_ctor()(shapeMem, &halfExtents);
        auto* portShape = reinterpret_cast<PortBhkShape*>(shapeMem);
        portShape->materialId = kSkinMaterialId;
        if (portShape->shape)                                  // delta 3: crisp box, matches HIGGS
            static_cast<hkpConvexShape*>(portShape->shape)->setRadiusUnchecked(0.f);

        PortBhkRigidBodyCinfo cInfo;
        std::memset(&cInfo, 0, sizeof(cInfo));
        fn_bhkRigidBodyCinfo_ctor()(&cInfo);
        hkpRigidBodyCinfo& hkc = cInfo.hkCinfo();
        cInfo.collisionFilterInfo = filterInfo;                // delta 5: REAL colliding word, not collviz's ghost layers
        cInfo.shape               = portShape->shape;
        hkc.m_collisionFilterInfo = filterInfo;
        hkc.m_shape               = portShape->shape;
        hkc.m_motionType          = hkpMotion::MOTION_KEYFRAMED;
        // quality stays plain KEYFRAMED — NEVER KEYFRAMED_REPORTING: PLANCK upgrades
        // HIGGS's own bodies to it and branches on it; plain keeps us out of those paths
        hkc.m_qualityType         = HK_COLLIDABLE_QUALITY_KEYFRAMED;
        hkc.m_enableDeactivation  = false;                     // load-bearing broadphase-freshness fields
        hkc.m_solverDeactivation  = hkpRigidBodyCinfo::SOLVER_DEACTIVATION_OFF;
        hkc.m_maxAngularVelocity  = 500.f;                     // delta 4 (HIGGS hand.cpp:589)
        hkVector4 posHavok;
        posHavok.set(posU[0] * kSkyrimToHavok, posU[1] * kSkyrimToHavok, posU[2] * kSkyrimToHavok, 0.f);
        hkc.m_position = posHavok;
        hkc.m_rotation.set(rot.x, rot.y, rot.z, rot.w);        // delta 2 (hk quaternion = imag, real)

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

    // ── CREATE one hand's rig (inside the PrePhysicsStep callback only, N14) ──
    void TryCreateHand(int hand, void* authWorld)
    {
        auto* h = Interop::GetHiggs();
        if (!h) { LogSkip("noHiggs"); return; }
        const bool isLeft = hand == 1;
        // base word from HIGGS's live hand body — inherits playerGroup + bit 15
        // (charController protection by construction) and every re-stamp HIGGS does
        hkpRigidBody* handBody = HkOfValidated(h->GetHandRigidBody(isLeft));
        if (!handBody) { LogSkip("noHiggsHand"); return; }
        const std::uint32_t base = handBody->getCollidable()->getCollisionFilterInfo();

        // 2026-07-12: the open-hand creation gate (angle zAxis(F20)<->zAxis(F21) < 15°)
        // is REMOVED. It predates ResolveGeometryLive — geometry re-solves every frame
        // now, so a curled-hand creation self-corrects within a frame. In VR the
        // controller grip curls the fingers almost constantly, so the gate starved
        // creation for MINUTES (log 15:10: 8+ s of SKIP fist before the session's
        // FIRST boxes appeared — the user's "my finger boxes went away").
        // SolveGeometry's degenerate check (knuckle span < 0.3u) remains the guard.

        BoxDef def[4];
        if (!SolveGeometry(hand, def)) { LogSkip("degenerate"); return; }

        const unsigned subLayer = ObjectHold::HandBoxSubLayer();
        // creation word: our sub-layer spliced in, bit 14 FORCED ON (review N4 —
        // HIGGS's own "initially, turn collision off" recipe; FilterRefresh clears it
        // after the enable delay). Bit 14 from base rides along too (review N5).
        const std::uint32_t pgrpC = base >> 16;
        const std::uint32_t egrpC = EffectiveGroup(pgrpC, HiggsHoldingAnything());
        const std::uint32_t word = (((base & ~0x00001F00u) & 0x0000FFFFu)
                                    | (subLayer << 8) | (egrpC << 16)) | kBit14;

        // filter atomics live BEFORE the first AddEntity (the Defect-4 ordering class)
        g_playerGroup.store(pgrpC, std::memory_order_relaxed);
        g_boxGroup.store(egrpC, std::memory_order_relaxed);
        g_boxPart.store(subLayer, std::memory_order_relaxed);
        g_boxLive.store(true, std::memory_order_relaxed);

        auto& rig = g_rig[hand];
        bool failed = false;
        for (int b = 0; b < 4; ++b) {
            float posU[3]; QuatW rot{ 1.f, 0.f, 0.f, 0.f };
            BoxWorldPose(hand, def[b], posU, rot);           // world pose from the CURRENT snapshot
            rig.bodyMem[b] = CreateBoxBody(authWorld, posU, rot, def[b].half, word);
            if (!rig.bodyMem[b]) { failed = true; break; }
        }
        if (failed) {
            // roll back the partial set against the SAME (still live) creation world
            for (void*& mem : rig.bodyMem) { if (mem) RemoveBody(mem, authWorld); mem = nullptr; }
            if (!g_rig[0].live && !g_rig[1].live)
                g_boxLive.store(false, std::memory_order_relaxed);
            LogSkip("allocFail");
            return;
        }
        for (int b = 0; b < 4; ++b) rig.def[b] = def[b];
        rig.live          = true;
        rig.bhkWorld      = authWorld;                 // raw, for compares/logging
        rig.heldWorld.reset(static_cast<RE::bhkWorld*>(authWorld));   // STRONG: keeps it alive to teardown
        rig.scaleAtCreate = g_snap.scale[hand];
        rig.lastWord      = word;
        rig.createdAt     = std::chrono::steady_clock::now();
        logger::info("HBOX privGroup mode={} id=0x{:04X} playerGrp=0x{:04X} -> ourGrp=0x{:04X}",
                     ObjectHold::HandBoxPrivGroupMode(), ObjectHold::HandBoxPrivGroupId(),
                     pgrpC, egrpC);
        logger::info("HBOX CREATE hand={} mode={} word=0x{:08X} (bit14 ON, {} ms enable delay) "
                     "half=[idxP {:.4f}/{:.4f}/{:.4f} idxD {:.4f}/{:.4f}/{:.4f} slab {:.4f}/{:.4f}/{:.4f} tip {:.4f}/{:.4f}/{:.4f}]m scale={:.4f}",
                     isLeft ? "L" : "R", ObjectHold::HandBoxFollowMode(), word, kEnableDelayMs,
                     def[0].half[0], def[0].half[1], def[0].half[2],
                     def[1].half[0], def[1].half[1], def[1].half[2],
                     def[2].half[0], def[2].half[1], def[2].half[2],
                     def[3].half[0], def[3].half[1], def[3].half[2],
                     rig.scaleAtCreate);
    }

    // ── Phase 0: the HIGGS palm-slab shrink (per frame, idempotent) ──────────
    void SlabWrite(bool beast)
    {
        const float kx = ObjectHold::HiggsSlabHalfX();
        const float ky = ObjectHold::HiggsSlabHalfY();
        const float kz = ObjectHold::HiggsSlabHalfZ();
        if (kx < 0.f && ky < 0.f && kz < 0.f) return;          // -1 -1 -1 = leave HIGGS alone
        auto* h = Interop::GetHiggs();
        if (!h) return;
        for (int hand = 0; hand < 2; ++hand) {
            hkpRigidBody* hk = HkOfValidated(h->GetHandRigidBody(hand == 1));
            if (!hk) continue;
            const hkpShape* shape = hk->getCollidable()->getShape();
            auto* box = reinterpret_cast<PortHkpBoxShape*>(const_cast<hkpShape*>(shape));
            if (!box || !IsLikelyPointer(box) || box->type != HK_SHAPE_BOX) continue;
            auto& sb = g_slabBase[hand];
            if (sb.hkBody != static_cast<const void*>(hk)) {
                // first sight of THIS body (HIGGS recreates it on world change / beast
                // toggle): capture baseline extents + convex radius (review N7)
                sb.hkBody   = hk;
                sb.half[0]  = box->halfExtents[0];
                sb.half[1]  = box->halfExtents[1];
                sb.half[2]  = box->halfExtents[2];
                sb.radius   = box->radius;
                sb.captured = true;
                logger::info("HBOX slab baseline {}: half=[{:.4f} {:.4f} {:.4f}]m r={:.4f} body={}",
                             hand == 1 ? "L" : "R", sb.half[0], sb.half[1], sb.half[2], sb.radius,
                             static_cast<const void*>(hk));
                // review N8 cross-check (LOG-ONLY): the beast box is {0.1, 0.015, 0.2}
                if (!beast && std::fabs(sb.half[0] - 0.1f) < 0.005f && std::fabs(sb.half[2] - 0.2f) < 0.01f)
                    logger::info("HBOX slab baseline {} matches the BEAST box but the race flag says non-beast "
                                 "— customized higgs_vr.ini?", hand == 1 ? "L" : "R");
            }
            if (beast && !ObjectHold::HandBoxBeast()) continue;   // never touch the beast box unless opted in
            // SHRINK-ONLY (report 10 Risk 2): every write clamped to the first-seen
            // baseline — grow is the unverified broadphase-AABB direction. Knob -1 =
            // leave that axis at baseline. The per-frame idempotent rewrite self-heals
            // HIGGS's body recreation. ROLLBACK (review N7): knob back to -1 does NOT
            // restore mid-session — extents recover at the next body recreation (cell
            // change / beast toggle); we never write upward.
            // X (width): shrink-only by default. NOTE: higgsSlabAllowGrow 1 is a DELIBERATE, user-
            // requested deviation from report 10's shrink-only rule (Risk 2) — it lets X GROW past the
            // captured baseline, HARD-CLAMPED to 2.0x baseline X. HIGGS keyframes its slab every frame
            // (applyHardKeyFrame), so the integrator re-derives its broadphase AABB each step and picks
            // the widened extent up — same reasoning as ResolveGeometryLive's grown boxes. Y and Z stay
            // shrink-only REGARDLESS (only X may ever exceed baseline); if a grown slab still misses
            // contacts, the fallback is the same as elsewhere (recreate at the next HIGGS body rebuild).
            const float xCapHi = ObjectHold::HiggsSlabAllowGrow() ? (2.0f * sb.half[0]) : sb.half[0];
            const float wantX = (kx >= 0.f) ? std::min(std::max(kx, kSlabFloorM), xCapHi)     : sb.half[0];
            const float wantY = (ky >= 0.f) ? std::min(std::max(ky, kSlabFloorM), sb.half[1]) : sb.half[1];
            const float wantZ = (kz >= 0.f) ? std::min(std::max(kz, kSlabFloorM), sb.half[2]) : sb.half[2];
            // ── IDEMPOTENT SKIP (2026-08-02, user-reproduced hand shake) ─────────────────────
            // These three lines used to be written UNCONDITIONALLY, every frame, into a LIVE
            // hkpBoxShape that HIGGS keyframes and Havok is actively deriving contacts from.
            // Our OWN boxes never did that — ResolveGeometryLive writes "only when a knob
            // actually moved" — and a comment there even claims this function is the source of
            // that idiom ("the SlabWrite idiom ... idempotent skip"). It never was.
            // In-VR, user-reproduced: hand shake that vanished the moment these writes stopped,
            // and which was ABSENT WITH A WEAPON EQUIPPED — because HIGGS swaps to the weapon
            // collision body then, so the body being rewritten is no longer the active one.
            // That discriminator is what identified this site.
            // Writing only on change means one write per HIGGS body recreation instead of ~90/s.
            if (box->halfExtents[0] != wantX ||
                box->halfExtents[1] != wantY ||
                box->halfExtents[2] != wantZ) {
                box->halfExtents[0] = wantX;
                box->halfExtents[1] = wantY;
                box->halfExtents[2] = wantZ;
                logger::info("HBOX slab {} extents -> [{:.4f} {:.4f} {:.4f}]m (was [{:.4f} {:.4f} "
                             "{:.4f}]) — one write per body, not per frame",
                             hand == 1 ? "L" : "R", wantX, wantY, wantZ,
                             sb.half[0], sb.half[1], sb.half[2]);
            }
        }
    }

    // ── Track B live geometry (2026-07-10): re-solve OUR 4 boxes every consumed snapshot and
    //    write the half-extents to the engine shape — the twin of SlabWrite for our own bodies.
    //    off/rotOff are consumed live by BoxWorldPose, so re-solving alone makes size/tilt/length/
    //    offset knobs live; the half-extents need the SlabWrite idiom (mirror hkpBoxShape, clamp,
    //    idempotent skip). OUR boxes ONLY — never HIGGS's bodies (those are SlabWrite's, shrink-only).
    //    Runs INSIDE the once-per-snapshot consumer (never per substep). No world lock and no
    //    broadphase refresh: identical to the proven SlabWrite float write.
    // NOTE: unlike HIGGS's near-static slab (report 10 Risk 2 kept it shrink-only), our boxes are
    //    hard-keyframed every frame, so the integrator re-derives their broadphase AABB from the
    //    live motion each step — a GROWN extent is picked up on the next step through that path.
    //    If a grown box ever misses contacts, the same fallback as the slab applies: toggle
    //    handBoxEnable off/on to REBUILD the shape at the new size (RigLifecycle, review N14).
    void ResolveGeometryLive()
    {
        for (int hand = 0; hand < 2; ++hand) {
            auto& rig = g_rig[hand];
            if (!rig.live || !g_snap.valid[hand]) continue;   // keep the last good def on an absent snapshot
            BoxDef def[4];
            if (!SolveGeometry(hand, def)) continue;          // degenerate snapshot -> keep the last good def
            for (int b = 0; b < 4; ++b) {
                rig.def[b] = def[b];                          // off/rotOff go live through BoxWorldPose
                hkpRigidBody* hk = HkOf(rig.bodyMem[b]);
                if (!hk) continue;
                const hkpShape* shape = hk->getCollidable()->getShape();
                auto* box = reinterpret_cast<PortHkpBoxShape*>(const_cast<hkpShape*>(shape));
                if (!box || !IsLikelyPointer(box) || box->type != HK_SHAPE_BOX) continue;
                const float hx = std::clamp(def[b].half[0], kHalfMinM, kHalfMaxM);   // solve already clamped;
                const float hy = std::clamp(def[b].half[1], kHalfMinM, kHalfMaxM);   // re-clamp = SlabWrite safety idiom
                const float hz = std::clamp(def[b].half[2], kHalfMinM, kHalfMaxM);
                if (std::fabs(box->halfExtents[0] - hx) > kHalfEpsM ||
                    std::fabs(box->halfExtents[1] - hy) > kHalfEpsM ||
                    std::fabs(box->halfExtents[2] - hz) > kHalfEpsM) {
                    box->halfExtents[0] = hx;                 // idempotent: only when a knob actually moved
                    box->halfExtents[1] = hy;
                    box->halfExtents[2] = hz;
                }
            }
        }
    }

    // ── lifecycle (create/destroy/recreate — ONLY here, review N14) ─────────
    void RigLifecycle(void* authWorld, bool beast)
    {
        for (int hand = 0; hand < 2; ++hand) {
            auto& rig = g_rig[hand];
            // Scene gate (2026-08-03): during an OStim/SexLab scene the player's hands stay
            // CONTROLLER-TRACKED (OStimVR TrackHands=1) and sit inside the partner, so our boxes
            // are live colliders in the middle of the animation. Nothing else in the stack
            // suppresses them — HIGGS gates only held/two-handing, and PLANCK's own scene
            // behaviour is incidental (its SetPosition hook temp-ignores repositioned actors).
            // Reuses the handBoxEnable-0 teardown path exactly; no new lifecycle.
            // mode 2 = suspend ONLY in third person. In OStim VR first person the hands stay
            // CONTROLLER-tracked (TrackHands=1) and sit where the player actually reaches, so
            // the boxes are wanted there. It is third person that hands them to the ANIMATION,
            // which is what buried them inside the partner. Camera is polled every frame, so a
            // mid-scene wheel switch flips the rigs within a frame (recreate carries the usual
            // 100 ms collision-off grace).
            bool sceneOff = false;
            if (g_sceneSuspend.load(std::memory_order_relaxed)) {
                const int mode = ObjectHold::SceneSuspendHandsMode();
                if (mode == 1) sceneOff = true;
                else if (mode >= 2) {
                    // IsInFirstPerson() never flips in VR (in-game verified 2026-08-07: one
                    // THIRD line at scene start, none on wheel switches, boxes gone in both
                    // views). Measure the mode instead: HMD-to-scene-head distance. First
                    // person rides the head; the third-person ghost floats away.
                    bool first = false;
                    auto* cam = RE::PlayerCamera::GetSingleton();
                    RE::NiAVObject* camRoot = cam ? cam->cameraRoot.get() : nullptr;
                    auto* pl = RE::PlayerCharacter::GetSingleton();
                    auto* r3 = pl ? pl->Get3D(false) : nullptr;
                    RE::NiAVObject* head = r3 ? r3->GetObjectByName(RE::BSFixedString("NPC Head [Head]")) : nullptr;
                    if (camRoot && head) {
                        const auto& c = camRoot->world.translate;
                        const auto& hd = head->world.translate;
                        const float dx = c.x-hd.x, dy = c.y-hd.y, dz = c.z-hd.z;
                        const float lim = ObjectHold::SceneFirstDistU();
                        first = (dx*dx + dy*dy + dz*dz) < lim * lim;
                    }
                    sceneOff = !first;
                    static int s_lastFirst = -1;
                    if (s_lastFirst != (int)first) {
                        s_lastFirst = (int)first;
                        logger::info("SCENE camera -> {} person: hand colliders {}",
                                     first ? "FIRST" : "THIRD", first ? "RESTORED" : "suspended");
                    }
                }
            }
            const bool want = ObjectHold::HandBoxEnabled() && g_snap.valid[hand] && !sceneOff &&
                              !(beast && !ObjectHold::HandBoxBeast());
            if (rig.live) {
                if (rig.bhkWorld != authWorld) {
                    // world changed: RemoveBody receives the LIVE world (stored pointer is
                    // compare-only, round-1 R3); the membership guard no-ops for bodies in
                    // the old world = forget. Accepted residual: a still-alive old world
                    // keeps 4 frozen boxes until its teardown (documented in the plan).
                    DestroyHand(hand, authWorld, "worldChange");
                } else if (!want) {
                    DestroyHand(hand, authWorld, "off");           // knob/beast/snapshot loss — full rollback
                } else if (std::fabs(g_snap.scale[hand] - rig.scaleAtCreate)
                           > ObjectHold::HandBoxRebuildFrac() * rig.scaleAtCreate) {
                    // ⚠ 2026-07-29: this used to fire at 1% and was OBSOLETE — since the
                    // 07-10 rewrite, anchors, tilt, length AND half-extents are all re-solved
                    // LIVE every consumed snapshot (see SolveGeometry's header), so a scale
                    // drift needs no teardown at all. A user log showed 60 destroy/create
                    // cycles in one session on ordinary hand-scale jitter (0.8005..0.8755).
                    // Every recreate re-derives the filter word from HIGGS's hand body and
                    // starts collision-OFF for 100 ms, so the churn both flickered the
                    // colliders and gave a stale anchor repeated chances to be sampled —
                    // the "finger collision stuck somewhere" report. Threshold is now a knob
                    // defaulting to 15%, which only a real form change (beast/werewolf) hits.
                    DestroyHand(hand, authWorld, "scaleChange");
                }
            }
            if (!rig.live && want)
                TryCreateHand(hand, authWorld);
        }
    }

    // ── filter refresh (derive from HIGGS's LIVE hand word every frame) ──────
    void FilterRefresh(void* authWorld)
    {
        auto* h = Interop::GetHiggs();
        if (!h) return;
        const auto now = std::chrono::steady_clock::now();
        for (int hand = 0; hand < 2; ++hand) {
            auto& rig = g_rig[hand];
            if (!rig.live) continue;
            const bool isLeft = hand == 1;
            hkpRigidBody* handBody = HkOfValidated(h->GetHandRigidBody(isLeft));
            if (!handBody) continue;                       // keep the last word until HIGGS's body is back
            const std::uint32_t base = handBody->getCollidable()->getCollisionFilterInfo();
            // ours = HIGGS's word with only the sub-layer bits replaced. Bit 14 is
            // INHERITED from the live word (review N5 — HIGGS's own disable set covers
            // its creation delay + version drift), then our own conditions OR on top:
            const std::uint32_t pgrp = base >> 16;
            const std::uint32_t egrp = EffectiveGroup(pgrp, HiggsHoldingAnything());
            std::uint32_t ours = ((base & ~0x00001F00u) & 0x0000FFFFu)
                               | (g_boxPart.load(std::memory_order_relaxed) << 8)
                               | (egrp << 16);
            if (now - rig.createdAt < std::chrono::milliseconds(kEnableDelayMs))
                ours |= kBit14;                            // review N4: creation enable delay
            if (h->IsHoldingObject(isLeft) || h->IsTwoHanding() || h->IsDisabled(isLeft))
                ours |= kBit14;                            // held objects are sub-layer 2/6 (|4-2|=2 would collide)
            if (ours == rig.lastWord) continue;
            void* hkpW = GetHkpWorld(authWorld);
            if (!IsLikelyPointer(hkpW)) continue;
            {
                WorldWriteLock lock(authWorld);
                for (void* mem : rig.bodyMem) {
                    hkpRigidBody* hk = HkOf(mem);
                    if (!hk) continue;
                    hk->getCollidableRw()->setCollisionFilterInfo(ours);
                    // HIGGS's exact call (hand.cpp:665; review N11 — NOT the digest's (0,0))
                    fn_UpdateFilter()(hkpW, static_cast<hkpEntity*>(hk),
                                      HK_UPDATE_FILTER_ON_ENTITY_FULL_CHECK,
                                      HK_UPDATE_COLLECTION_FILTER_PROCESS_SHAPE_COLLECTIONS);
                }
            }
            g_playerGroup.store(pgrp, std::memory_order_relaxed);      // horse mount re-stamps the group
            g_boxGroup.store(egrp, std::memory_order_relaxed);         // what OUR bodies carry
            // ★ 2026-08-17: print `base` and ATTRIBUTE bit14. The old line printed only
            // lastWord -> ours, which makes an INHERITED bit14 (HIGGS's word, or any foreign
            // mod that poked it) indistinguishable from PPB's own OR below. That ambiguity cost
            // a full investigation to resolve by hand; the attribution is one line.
            const bool b14Base  = (base & kBit14) != 0;
            const bool b14Delay = (now - rig.createdAt) < std::chrono::milliseconds(kEnableDelayMs);
            const bool b14Hold  = h->IsHoldingObject(isLeft) || h->IsTwoHanding();
            const bool b14Dis   = h->IsDisabled(isLeft);
            logger::info("HBOX filter hand={}: 0x{:08X} -> 0x{:08X} (bit14={}) | base=0x{:08X} "
                         "why[inherited={} createDelay={} hold={} higgsDisabled={}]{}",
                         isLeft ? "L" : "R", rig.lastWord, ours, (ours & kBit14) != 0, base,
                         b14Base ? 1 : 0, b14Delay ? 1 : 0, b14Hold ? 1 : 0, b14Dis ? 1 : 0,
                         (b14Base && !b14Delay && !b14Hold && !b14Dis)
                             ? "  <-- INHERITED ONLY: something outside PPB set collision-off on "
                               "HIGGS's hand body. HIGGS re-asserts bit14 both ways every frame "
                               "(hand.cpp:670), so a PERSISTENT one here means a foreign writer."
                             : "");
            rig.lastWord = ours;
        }
    }

    // ── player-space warp (2026-07-12 stability fix — the HIGGS technique) ───
    // Teleport every live box by the player's world delta since the previous
    // consumed snapshot, BEFORE the keyframes are computed. HIGGS does exactly
    // this for its hand/weapon bodies (g_playerSpaceBodies, higgs main.cpp
    // SimulatePlayerSpace): locomotion moves the bodies POSITIONALLY, so the
    // keyframe velocity only ever carries hand-relative motion. Without it,
    // walking at low FPS starved the boxes (velocity × clamped sim-time covers
    // only part of a frame's player motion → visible trail/flying), and every
    // spike carried player speed into contact impulses.
    // Deltas > 50u (fast travel, teleport, load) are NOT warped — the spike
    // path in KeyframeAll recovers those cleanly with a zero-velocity snap.
    void PlayerSpaceWarp(void* authWorld, RE::PlayerCharacter* player)
    {
        // ⛔ handBoxWarp is a HAND-jitter instrument knob that this file's own header calls
        // "phased wrong" and invites the user to set to 0 — but for the HEAD box the warp is the
        // FIRST line of anti-launch defence (without it the player's locomotion becomes the
        // box's keyframe velocity and walking into an NPC hands her bone that speed: mechanism
        // H2). The two must not share a switch. When the knob is off we still warp the head, and
        // say so once, rather than silently dropping a defence the head box's design depends on.
        const bool warpHands = ObjectHold::HandBoxWarpOn();
        if (!warpHands && !g_head.live) { g_prevPlayerPosValid = false; return; }
        if (!warpHands) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                logger::info("HEADBOX: handBoxWarp is 0 — warping the HEAD box anyway (it is the "
                             "head's anti-launch defence; the knob only governs the hand boxes)");
            }
        }
        if (!player) { g_prevPlayerPosValid = false; return; }
        const RE::NiPoint3 pp = player->GetPosition();
        const float cur[3] = { pp.x, pp.y, pp.z };
        float delta[3] = { 0.f, 0.f, 0.f };
        const bool hadPrev = g_prevPlayerPosValid;
        if (hadPrev)
            for (int c = 0; c < 3; ++c) delta[c] = cur[c] - g_prevPlayerPos[c];
        std::memcpy(g_prevPlayerPos, cur, sizeof(cur));
        g_prevPlayerPosValid = true;
        if (!hadPrev) return;
        const float d2 = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
        if (d2 < 1e-6f || d2 > 50.f * 50.f) return;          // idle / teleport-class jump
        if (!g_rig[0].live && !g_rig[1].live && !g_wand.live && !g_head.live) return;
        hkVector4 dH;
        dH.set(delta[0] * kSkyrimToHavok, delta[1] * kSkyrimToHavok, delta[2] * kSkyrimToHavok, 0.f);
        WorldWriteLock lock(authWorld);                      // ONE lock for all 8 warps
        for (int hand = 0; hand < 2 && warpHands; ++hand) {
            auto& rig = g_rig[hand];
            if (!rig.live) continue;
            for (int b = 0; b < 4; ++b) {
                hkpRigidBody* hk = HkOf(rig.bodyMem[b]);
                if (!hk) continue;
                const hkVector4& p = hk->getPosition();
                hkVector4 np;
                np.set(p(0) + dH(0), p(1) + dH(1), p(2) + dH(2), 0.f);
                fn_setPosition()(static_cast<hkpEntity*>(hk), np);
            }
        }
        if (g_wand.live && warpHands) {
            for (void* mem : g_wand.bodyMem) {
                hkpRigidBody* hk = HkOf(mem);
                if (!hk) continue;
                const hkVector4& p = hk->getPosition();
                hkVector4 np;
                np.set(p(0) + dH(0), p(1) + dH(1), p(2) + dH(2), 0.f);
                fn_setPosition()(static_cast<hkpEntity*>(hk), np);
            }
        }
        // ★ THE HEAD BOX MUST BE IN THIS LOOP. Without it, walking forward makes the whole
        // locomotion delta the box's keyframe VELOCITY, and an infinite-mass body hands that
        // velocity straight to her dynamic bone through the contact's velocity constraint —
        // mechanism H2 in Report/Ragdoll Research Module/02, i.e. a run-in would LAUNCH her
        // instead of pushing her. headBoxMaxVel is the second line of defence, not the first.
        if (g_head.live) {
            if (hkpRigidBody* hk = HkOf(g_head.bodyMem)) {
                const hkVector4& p = hk->getPosition();
                hkVector4 np;
                np.set(p(0) + dH(0), p(1) + dH(1), p(2) + dH(2), 0.f);
                fn_setPosition()(static_cast<hkpEntity*>(hk), np);
            }
        }
    }

    // ── the 8 keyframes (+ the mandatory velocity clamp, spec §1.6) ──────────
    void KeyframeAll(void* authWorld)
    {
        const float maxVel = ObjectHold::HandBoxMaxVel();
        for (int hand = 0; hand < 2; ++hand) {
            auto& rig = g_rig[hand];
            if (!rig.live || !g_snap.valid[hand]) continue;   // lifecycle destroyed !valid rigs already
            for (int b = 0; b < 4; ++b) {
                hkpRigidBody* hk = HkOf(rig.bodyMem[b]);
                if (!hk) continue;
                float posU[3]; QuatW rot;
                BoxWorldPose(hand, rig.def[b], posU, rot);
                hkVector4 pos;
                pos.set(posU[0] * kSkyrimToHavok, posU[1] * kSkyrimToHavok, posU[2] * kSkyrimToHavok, 0.f);
                hkQuaternion q;
                q.set(rot.x, rot.y, rot.z, rot.w);
                // Keyframe, then spike-check (2026-07-12 REORDER of the old apply-then-
                // teleport): the old path teleported on a spike but LEFT the huge
                // computed velocity on the body — the box overshot the target on the
                // same pre-step it was snapped to (integration runs after us), got
                // yanked back next frame (visible jitter), and any contact during
                // those frames received the uncapped velocity as an impulse (NPC
                // flesh kicked violently → PLANCK shoves the charController back →
                // the user's "screen jerk"). NOW: on a spike, teleport pos+rot FIRST,
                // then RE-apply the keyframe against the new pose — the recomputed
                // error is ~0, so the residual velocity is ~0. Normal (sub-spike)
                // frames keep the plain keyframe velocity, which is what makes
                // keyframed-vs-dynamic contacts resolve. PlayerSpaceWarp upstream
                // keeps locomotion out of the error, so spikes are now genuinely
                // exceptional (real teleports, load-door arrivals, tracking jumps).
                fn_applyHardKeyFrame()(pos, q, g_snap.invDt, hk);
                const hkVector4& lv = hk->getLinearVelocity();
                const hkVector4& av = hk->getAngularVelocity();
                const bool linSpike = lv(0) * lv(0) + lv(1) * lv(1) + lv(2) * lv(2) > maxVel * maxVel;
                const bool angSpike = av(0) * av(0) + av(1) * av(1) + av(2) * av(2) > maxVel * maxVel;
                // ── LEASH, PART 2 (2026-07-29): POSITION check, not just velocity ───────────
                // HIGGS's own clamp (physics.cpp ApplyHardKeyframeVelocityClamped) triggers on
                // VELOCITY only — it catches "can't keep up", never "ended up somewhere else and
                // stopped". A body displaced and left sitting there has zero velocity and would
                // never be recovered. Ground truth = the skeleton hand node.
                bool posLeash = false;
                {
                    const float leash = ObjectHold::HandBoxLeashU();
                    if (leash > 0.f) {
                        const float* hp = g_snap.pos[hand][kHand];
                        const hkVector4& bp = hk->getPosition();
                        const float dx = bp(0) * kHavokToSkyrim - hp[0];
                        const float dy = bp(1) * kHavokToSkyrim - hp[1];
                        const float dz = bp(2) * kHavokToSkyrim - hp[2];
                        if (dx*dx + dy*dy + dz*dz > leash * leash) {
                            posLeash = true;
                            static std::uint32_t s_snapLog[2] = { 0, 0 };
                            static std::uint64_t s_snapTot[2] = { 0, 0 };
                            ++s_snapTot[hand];
                            if (s_snapLog[hand] < 3) {
                                ++s_snapLog[hand];
                                logger::info("HBOX leash: {} box {} was {:.1f}u from the hand node "
                                             "(> {:.0f}u) — snapped home",
                                             hand == 1 ? "L" : "R", b,
                                             std::sqrt(dx*dx + dy*dy + dz*dz), leash);
                            } else if ((s_snapTot[hand] & 0xFF) == 0) {
                                logger::info("HBOX leash: {} boxes snapped home {} times so far "
                                             "(latest {:.1f}u) — a HIGH count means the boxes are "
                                             "being displaced repeatedly, not once",
                                             hand == 1 ? "L" : "R", s_snapTot[hand],
                                             std::sqrt(dx*dx + dy*dy + dz*dz));
                            }
                        }
                    }
                }
                if (linSpike || angSpike || posLeash) {
                    {
                        WorldWriteLock lock(authWorld);
                        fn_setPosition()(static_cast<hkpEntity*>(hk), pos);
                        fn_setRotation()(static_cast<hkpEntity*>(hk), q);
                    }
                    // error is now ~0 → this rewrites the body velocity to ~0
                    fn_applyHardKeyFrame()(pos, q, g_snap.invDt, hk);
                }
            }
        }
    }

    // ── one-shot diagnostic dump (edge-triggered on handBoxDump 0 -> 1) ──────
    void DumpIfRequested(void* authWorld)
    {
        static float s_lastVal = 0.f;
        const float cur = ObjectHold::HandBoxDump();
        const bool fire = cur > 0.5f && s_lastVal <= 0.5f;
        s_lastVal = cur;
        if (!fire) return;
        logger::info("HBOX DUMP: authWorld={} snapId={} invDt={:.1f} enabled={} mode={} subLayer={}",
                     authWorld, g_snap.id, g_snap.invDt, ObjectHold::HandBoxEnabled(),
                     ObjectHold::HandBoxFollowMode(), ObjectHold::HandBoxSubLayer());
        auto* h = Interop::GetHiggs();
        for (int hand = 0; hand < 2; ++hand) {
            const char* hn = hand == 1 ? "L" : "R";
            const auto& sb = g_slabBase[hand];
            if (sb.captured) {
                float cx = -1.f, cy = -1.f, cz = -1.f;
                if (h) {
                    if (hkpRigidBody* hk = HkOfValidated(h->GetHandRigidBody(hand == 1))) {
                        auto* box = reinterpret_cast<const PortHkpBoxShape*>(hk->getCollidable()->getShape());
                        if (box && IsLikelyPointer(box) && box->type == HK_SHAPE_BOX) {
                            cx = box->halfExtents[0]; cy = box->halfExtents[1]; cz = box->halfExtents[2];
                        }
                    }
                }
                logger::info("HBOX DUMP slab {}: baseline=[{:.4f} {:.4f} {:.4f}] r={:.4f} written=[{:.4f} {:.4f} {:.4f}]",
                             hn, sb.half[0], sb.half[1], sb.half[2], sb.radius, cx, cy, cz);
            }
            const auto& rig = g_rig[hand];
            if (rig.live) {
                logger::info("HBOX DUMP rig {}: word=0x{:08X} world={} scaleAtCreate={:.4f}",
                             hn, rig.lastWord, rig.bhkWorld, rig.scaleAtCreate);
                for (int b = 0; b < 4; ++b) {
                    hkpRigidBody* hk = HkOf(rig.bodyMem[b]);
                    if (!hk) continue;
                    const hkVector4& p = hk->getPosition();
                    logger::info("HBOX DUMP rig {} box{}: pos=[{:.1f} {:.1f} {:.1f}]u half=[{:.4f} {:.4f} {:.4f}]m rider={}",
                                 hn, b, p(0) * kHavokToSkyrim, p(1) * kHavokToSkyrim, p(2) * kHavokToSkyrim,
                                 rig.def[b].half[0], rig.def[b].half[1], rig.def[b].half[2], rig.def[b].rider);
                }
            }
        }
        // review N2: the broken-oracle warning, in the log where the dial session lives
        logger::info("HBOX DUMP note: Collision Visualizer caches tessellation per shape identity — after a "
                     "slab dial, toggle trd off/on (or change cell) before judging the render; the physical "
                     "oracle is the hand sinking closer to flesh before contact");
    }

    // ── per-box readback dump (edge-triggered on handBoxDumpNow 0 -> non-0) ──────────
    // One line per box per hand in GAME UNITS: the values the user just dialed, so they can be
    // BAKED into the compiled defaults later. Reads the LIVE-solved rig.def (post ResolveGeometryLive),
    // so off/half already include every offset/tilt/half-override/len-frac knob. tiltDeg is the applied
    // rider-frame spin (plate box 3 -> plateTilt; index boxes 0/1 -> idxTilt; slab box 2 -> 0).
    void BoxDumpIfRequested()
    {
        static float s_lastVal = 0.f;
        const float cur = ObjectHold::HandBoxDumpNow();
        const bool fire = cur > 0.5f && s_lastVal <= 0.5f;
        s_lastVal = cur;
        if (!fire) return;
        const float plateTiltDeg = ObjectHold::HandBoxPlateTiltDeg();
        const float idxTiltDeg   = ObjectHold::HandBoxIdxTiltDeg();
        const float plateTilt2Deg = ObjectHold::HandBoxPlateTilt2Deg();   // 2026-07-10: 2nd composed plate stage
        const int   plateTilt2Ax  = ObjectHold::HandBoxPlateTilt2Axis();
        for (int hand = 0; hand < 2; ++hand) {
            const char* hn = hand == 1 ? "L" : "R";
            const auto& rig = g_rig[hand];
            if (!rig.live) { logger::info("HBOX BOX {} (no live rig)", hn); continue; }
            for (int b = 0; b < 4; ++b) {
                const BoxDef& d = rig.def[b];
                const float tilt  = (b == 3) ? plateTiltDeg : (b <= 1 ? idxTiltDeg : 0.f);
                const float tilt2 = (b == 3) ? plateTilt2Deg : 0.f;     // stage-2 tilt applies to the plate (box 3) only
                const int   tilt2Ax = (b == 3) ? plateTilt2Ax : -1;
                const char* rider = (d.rider >= 0 && d.rider < kNodeCount) ? kNodeNames[hand][d.rider] : "?";
                logger::info("HBOX BOX {} {} off=[{:.3f} {:.3f} {:.3f}]u halfU=[{:.3f} {:.3f} {:.3f}]u tiltDeg={:.1f} tilt2Deg={:.1f} tilt2Axis={} rider={}",
                             hn, b, d.off[0], d.off[1], d.off[2],
                             d.half[0] * kHavokToSkyrim, d.half[1] * kHavokToSkyrim, d.half[2] * kHavokToSkyrim,
                             tilt, tilt2, tilt2Ax, rider);
            }
        }
    }

    // ── bone snapshot at the physics-step boundary (2026-07-10 LAG FIX) ──────
    // Taken inside the FIRST PrePhysicsStep fire of each frame instead of at the
    // PostVrikPostHiggs OnFrame callback: whenever PostVrikPostHiggs landed AFTER
    // the frame's physics step, the old snapshot keyframed the boxes with LAST
    // frame's hand pose — 1 frame of visible trail during fast movement. HIGGS
    // itself moves its palm slab from CURRENT-frame predicted room-space data
    // immediately before stepping (higgs main.cpp SimulatePlayerSpace ~483-496,
    // MoveHandAndWeaponCollision, invoked from PlayerPostApplyMovementDeltaUpdate
    // — its pre-step path), which is why its slab never lagged.
    // Reading actor->Get3D() node world transforms HERE is the same class of read
    // the call site already performs: the PrePhysicsStep callback runs at the
    // hkpWorld::stepDeltaTime call site on the main thread — the same site PPB's
    // own 0xDFB722 hook chains, whose pipeline walks these scene-graph nodes every
    // fire (CapFix/PivFix/heel fix).
    // invDt semantics preserved (review N1): dt is measured between SNAPSHOTS (the
    // first PrePhysicsStep fire of successive frames), so the keyframe velocity is
    // still pose-delta / snapshot-interval; substep re-fires in the same frame are
    // skipped by the caller's once-per-id gate.
    // NOTE: if VRIK writes the hand pose AFTER physics in the frame pipeline, this
    // changes nothing vs the old OnFrame snapshot (both would read the same
    // last-written pose) — safe either way; it only helps when the pose is
    // written pre-step.
    void TakeBoneSnapshot()
    {
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        float dt = 1.f / 90.f;
        if (s_last.time_since_epoch().count() != 0)
            dt = std::chrono::duration<float>(now - s_last).count();
        s_last = now;

        // ── invDt SOURCE (2026-08-02, measurement-driven) ────────────────────────────────────
        // applyHardKeyFrame sets v = (target - current) * invDt and Havok then integrates that
        // velocity for however long it actually steps. So invDt must be the INVERSE OF THE
        // PHYSICS TIME the body will be carried for. It never was: PPB measured wall-clock
        // between snapshots instead, and a live 764-sample capture of a shaking session showed
        // that clock has 3.4x the variance of the real step (stdev 23.08 ms vs 6.82 ms), pegs its
        // own 100 ms clamp, and drives the executed fraction (gain = dtStep * invDt) across
        // 0.11..1.87 with 37% of frames outside 0.80..1.25. Executing 11% of the commanded motion
        // one frame and 187% the next IS the visible shake.
        // Now: use the physics time actually accumulated across every substep since the previous
        // keyframe — the exact interval the last command was integrated over, and the best
        // estimate of the next. Falls back to the old clock if the step hook has not reported
        // yet (first frames, or the hook failed to install).
        const std::uint32_t accUs = g_stepAccumUs.exchange(0, std::memory_order_relaxed);
        const float stepDt = static_cast<float>(accUs) * 1.0e-6f;
        const bool  useStep = ObjectHold::HandBoxStepDtOn() && stepDt > 1e-4f && stepDt < 0.5f;
        // ── EXPLICIT DAMPING (2026-08-02) ────────────────────────────────────────────────────
        // applyHardKeyFrame drives v = (target - here) * invDt, so scaling invDt by k makes the
        // box close k of the remaining error per frame: a first-order low-pass with a CONSTANT,
        // known coefficient. k = 1 is deadbeat and reproduces the target's own noise, which is
        // the jitter users reported once the dt was corrected. The previous stability came from
        // the broken clock under-shooting by a random 0.11..1.87 — same averaged effect, but
        // noisy and able to OVERSHOOT. This cannot exceed 1, so the box only ever approaches.
        // Error decays geometrically (no ringing) and steady-state lag is v*dt*(1-k)/k ~ 1.6 mm
        // at k=0.88 / 12 ms / 1 m/s.
        const float track = ObjectHold::HandBoxTrack();
        g_snap.invDt = track / std::clamp(useStep ? stepDt : dt, 1e-4f, 0.1f);
        g_snap.dtOwnMs  = dt * 1000.f;          // diagnostic only
        g_snap.dtUsedMs = (useStep ? stepDt : dt) * 1000.f;
        g_snap.valid[0] = g_snap.valid[1] = false;

        // slab-only mode needs no node snapshot — the poses are only for the boxes
        if (!ObjectHold::HandBoxEnabled() && !g_rig[0].live && !g_rig[1].live) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root   = player ? player->Get3D(false) : nullptr;   // THIRD person: whoever posed the
        if (!root) return;                                        // fingers (anim/VRIK/knuckles/HIGGS)
        const auto& names = InternedNames();                      // lands here, VRIK offsets folded in
        for (int hand = 0; hand < 2; ++hand) {
            auto* hn = root->GetObjectByName(names[hand * kNodeCount + kHand]);
            if (!hn) continue;
            bool all = true;
            for (int i = 0; i < kNodeCount && all; ++i) {
                RE::NiAVObject* n = (i == kHand) ? hn : hn->GetObjectByName(names[hand * kNodeCount + i]);
                if (!n) { all = false; break; }
                const RE::NiPoint3&  t = n->world.translate;
                const RE::NiMatrix3& R = n->world.rotate;
                g_snap.pos[hand][i][0] = t.x;
                g_snap.pos[hand][i][1] = t.y;
                g_snap.pos[hand][i][2] = t.z;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        g_snap.rot[hand][i][r * 3 + c] = R.entry[r][c];
            }
            if (all) {
                g_snap.scale[hand] = hn->world.scale;
                g_snap.valid[hand] = true;
            }
        }
        // No valid bones this frame (menu, teleport): valid stays false — the
        // consumers keep the last good def / skip keyframing (existing behavior).
    }

    // ── the PrePhysicsStep consumer (registered on HIGGS vfunc 21) ───────────
    // ── HBOXPH (2026-08-02) — the jitter instrument ───────────────────────────────────────
    // Emitted from the once-per-frame consumer, AFTER the keyframe command, so it always runs
    // whenever boxes exist. ~5 Hz, both hands on one line.
    //   dtStep = the REAL delta hkpWorld is integrating (from our own chain hook, which has
    //            always received it and thrown it away). dtOwn = the steady_clock interval PPB
    //            actually feeds applyHardKeyFrame. gain = dtStep x invDt; 1.0 = deadbeat.
    //   relErr = how far the box TARGET sits from the hand bone (game units).
    //   dRel   = how far that moved THIS frame. THIS IS THE SHAKE, measured directly.
    // READING IT: dRel ~0 standing still and rising while running outdoors, with gain staying
    // near 1.0, means the TARGET is being displaced and the keyframe executes it faithfully ->
    // the relation filter is the cause, and handBoxRelAlpha 1 is the fix. If dRel stays ~0
    // through a visibly shaking session, the filter is innocent and gain is the next suspect.
    void PhaseLog()
    {
        if (!ObjectHold::HandBoxPhaseLogOn()) return;
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (s_last.time_since_epoch().count() != 0 &&
            now - s_last < std::chrono::milliseconds(200)) return;   // ~5 Hz
        s_last = now;
        const std::uint32_t bits = g_stepDtBits.load(std::memory_order_relaxed);
        float dtStep = 0.f;
        std::memcpy(&dtStep, &bits, sizeof(dtStep));
        const float invDt  = g_snap.invDt;
        const float dtOwn  = (invDt > 1e-6f) ? (1.f / invDt) : 0.f;
        const float gain   = dtStep * invDt;
        logger::info("HBOXPH dtStep={:.2f}ms dtUsed={:.2f}ms dtOwn={:.2f}ms gain={:.3f} | L relErr={:.3f}u "
                     "dRel={:.3f}u dR={:.3f}u | R relErr={:.3f}u dRel={:.3f}u dR={:.3f}u | alpha={:.2f} track={:.2f}",
                     dtStep * 1000.f, g_snap.dtUsedMs, g_snap.dtOwnMs, gain,
                     g_relErrU[1], g_dRelU[1], g_dRU[1],
                     g_relErrU[0], g_dRelU[0], g_dRU[0],
                     ObjectHold::HandBoxRelAlpha(), ObjectHold::HandBoxTrack());
    }


    // ═════════════════════════════════════════════════════════════════════════════════════════
    //  PLAYER GENITAL WAND (2026-08-18) — doc 20 step 7, v1 TEST INSTRUMENT
    //  Two keyframed segment boxes riding the player's OWN SOS chain. HandBox-family on
    //  purpose: the player is excluded from the per-actor seam (PPBHook.cpp:1282), and this
    //  file already owns every player-attached-body mechanism (CreateBoxBody, the snapshot
    //  cadence, PlayerSpaceWarp, applyHardKeyFrame, the strong heldWorld teardown).
    //  What v1 proves: the wand EXISTS, tracks the SMP/CBPC-posed chain, and PUSHES what the
    //  hand boxes push (clutter, NPC flesh) while colliding with the player's own hands.
    //  What v1 is NOT: a touch-API source (no part naming, no VRTE events) and not
    //  contact-reactive (keyframed follower — reaction comes later via FsmpLink push into the
    //  SMP bodies, the same coupling fsmpPush already does for hair).
    //  Word: composed from HIGGS's LIVE hand word like the boxes (group churn + bit15 ride
    //  along) with TWO deliberate divergences from FilterRefresh:
    //    · inherited bit14 is MASKED OUT — HIGGS disables its HAND while holding a sword;
    //      that reason must not switch off the crotch. Only our own 100 ms creation delay
    //      sets bit14 here.
    //    · no IsHoldingObject/IsTwoHanding OR — same rationale.
    //  Part comes from playerWandPart (default 9, sanitized in the accessor — see Tuning.h
    //  for the full why-not-each-other-part table).
    void WandSnapshot()
    {
        g_wandSnap.valid = false;
        if (!ObjectHold::PlayerWandOn() && !g_wand.live) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root   = player ? player->Get3D(false) : nullptr;   // 3rd person: SMP/CBPC pose lands here
        if (!root) return;
        static RE::BSFixedString s_names[kWandChainN] = {};
        static bool s_interned = false;
        if (!s_interned) {
            for (int i = 0; i < kWandChainN; ++i) s_names[i] = kWandChain[i];
            s_interned = true;
        }
        RE::NiAVObject* nodes[kWandChainN]{};
        int first = -1, last = -1;
        for (int i = 0; i < kWandChainN; ++i) {
            nodes[i] = root->GetObjectByName(s_names[i]);
            if (nodes[i]) { if (first < 0) first = i; last = i; }
        }
        if (first < 0 || last <= first) {                          // no chain / single node: no segment
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                logger::info("WAND: player SOS chain not found ({} of {} nodes) — is a schlong "
                             "installed on the player? Wand stays down.",
                             (first >= 0) ? 1 : 0, kWandChainN);
            }
            return;
        }
        int mid = -1, want = (first + last) / 2;
        for (int i = first + 1; i < last; ++i)                     // present node nearest the middle
            if (nodes[i] && (mid < 0 || std::abs(i - want) < std::abs(mid - want))) mid = i;
        if (mid < 0) mid = first;                                  // 2-node chain: seg A degenerates, B carries
        const RE::NiAVObject* pick[3] = { nodes[first], nodes[mid], nodes[last] };
        for (int k = 0; k < 3; ++k) {
            const RE::NiPoint3& t = pick[k]->world.translate;
            g_wandSnap.p[k][0] = t.x; g_wandSnap.p[k][1] = t.y; g_wandSnap.p[k][2] = t.z;
        }
        g_wandSnap.valid = true;
        static bool s_resolvedOnce = false;
        if (!s_resolvedOnce) {
            s_resolvedOnce = true;
            logger::info("WAND: chain resolved base='{}' mid='{}' tip='{}'",
                         kWandChain[first], kWandChain[mid], kWandChain[last]);
        }
    }

    // segment i pose: box local +Z runs base→tip along the segment
    void WandSegPose(int seg, float posU[3], QuatW& rot, float halfM[3])
    {
        const float* a = g_wandSnap.p[seg];          // seg 0: base→mid, seg 1: mid→tip
        const float* b = g_wandSnap.p[seg + 1];
        float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        float len  = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        float tip[3] = { b[0], b[1], b[2] };
        if (seg == kWandSegs - 1 && len > 1e-3f) {   // extend the last segment past the bone tip
            const float inv = kWandTipPadU / len;
            tip[0] += d[0] * inv; tip[1] += d[1] * inv; tip[2] += d[2] * inv;
            d[0] = tip[0] - a[0]; d[1] = tip[1] - a[1]; d[2] = tip[2] - a[2];
            len += kWandTipPadU;
        }
        posU[0] = (a[0] + tip[0]) * 0.5f; posU[1] = (a[1] + tip[1]) * 0.5f; posU[2] = (a[2] + tip[2]) * 0.5f;
        float z[3] = { 0.f, 1.f, 0.f };              // degenerate fallback: forward
        if (len > 1e-3f) { z[0] = d[0]/len; z[1] = d[1]/len; z[2] = d[2]/len; }
        float up[3] = { 0.f, 0.f, 1.f };
        if (std::fabs(z[0]*up[0] + z[1]*up[1] + z[2]*up[2]) > 0.99f) { up[0] = 1.f; up[1] = 0.f; up[2] = 0.f; }
        float x[3] = { up[1]*z[2] - up[2]*z[1], up[2]*z[0] - up[0]*z[2], up[0]*z[1] - up[1]*z[0] };
        float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
        if (xl < 1e-4f) { x[0] = 1.f; x[1] = 0.f; x[2] = 0.f; xl = 1.f; }
        x[0] /= xl; x[1] /= xl; x[2] /= xl;
        float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
        const float m[9] = { x[0], y[0], z[0],  x[1], y[1], z[1],  x[2], y[2], z[2] };  // columns = axes
        rot = Mat3ToQuat(m);
        const float r = ObjectHold::PlayerWandR() * kSkyrimToHavok;
        halfM[0] = std::max(r, 0.010f);
        halfM[1] = std::max(r, 0.010f);
        halfM[2] = std::max(len * 0.5f * kSkyrimToHavok, 0.010f);
    }

    void DestroyWand(void* liveWorld, const char* reason)
    {
        if (!g_wand.live) return;
        void* owner = g_wand.heldWorld ? static_cast<void*>(g_wand.heldWorld.get()) : liveWorld;
        if (owner)
            for (void* mem : g_wand.bodyMem)
                if (mem) RemoveBody(mem, owner);
        logger::info("WAND DESTROY reason={}", reason);
        g_wand = WandRig{};
        g_wandLive.store(false, std::memory_order_relaxed);
    }

    std::uint32_t WandComposeWord(bool creating)
    {
        auto* h = Interop::GetHiggs();
        hkpRigidBody* handBody = h ? HkOfValidated(h->GetHandRigidBody(false)) : nullptr;
        if (!handBody && h) handBody = HkOfValidated(h->GetHandRigidBody(true));
        if (!handBody) return 0;
        const std::uint32_t base = handBody->getCollidable()->getCollisionFilterInfo();
        const std::uint32_t pgrpW = base >> 16;
        const std::uint32_t egrpW = EffectiveGroup(pgrpW, HiggsHoldingAnything());
        g_playerGroup.store(pgrpW, std::memory_order_relaxed);
        // boxes and wand share ONE group by design, and the wand may be the ONLY live rig —
        // FilterRefresh's store is early-outed when the box word is unchanged (or absent), so
        // publish it here too or isOurs() reads a stale/zero group and matches nothing.
        g_boxGroup.store(egrpW, std::memory_order_relaxed);
        g_wandPart.store(ObjectHold::PlayerWandPart(), std::memory_order_relaxed);
        std::uint32_t w = ((base & ~(0x00001F00u | kBit14)) & 0x0000FFFFu)
                        | (ObjectHold::PlayerWandPart() << 8) | (egrpW << 16);
        if (creating ||
            std::chrono::steady_clock::now() - g_wand.createdAt < std::chrono::milliseconds(kEnableDelayMs))
            w |= kBit14;
        return w;
    }

    void WandLifecycle(void* authWorld)
    {
        // ── EXPOSURE GATE (2026-08-23). The chain resolving is NOT proof of a schlong: PPB ships
        // the dormant SOS bones on every skeleton, so before this a player with no TNG/SOS at all
        // carried an invisible ~bind-length collider at the crotch — one that pushes loose objects
        // and, since the probe export landed, could open an NPC's orifice with nothing there.
        // Gating the LIFECYCLE (not just creation) means putting trousers on tears the wand down
        // within a frame, and WandProbeSegments — which keys off g_wand.live — goes quiet with it.
        const bool exposed = GenitalProbe::IsExposed(RE::PlayerCharacter::GetSingleton());
        // 2026-08-23: no player Havok physics during a scene either — the wand is a real collider
        // and would keep shoving the partner while the animation tries to place them.
        const bool sceneOff = g_sceneSuspend.load(std::memory_order_relaxed);
        const bool want = ObjectHold::PlayerWandOn() && g_wandSnap.valid && exposed && !sceneOff;
        if (g_wand.live) {
            if (g_wand.bhkWorld != authWorld)      DestroyWand(authWorld, "worldChange");
            else if (!want)                        DestroyWand(authWorld, "off");
        }
        if (g_wand.live || !want) return;
        const std::uint32_t word = WandComposeWord(true);
        if (!word) { LogSkip("noHiggsHand"); return; }
        bool failed = false;
        for (int i = 0; i < kWandSegs; ++i) {
            float posU[3]; QuatW rot; float halfM[3];
            WandSegPose(i, posU, rot, halfM);
            g_wand.bodyMem[i] = CreateBoxBody(authWorld, posU, rot, halfM, word);
            if (!g_wand.bodyMem[i]) { failed = true; break; }
        }
        if (failed) {
            for (void*& mem : g_wand.bodyMem) { if (mem) RemoveBody(mem, authWorld); mem = nullptr; }
            LogSkip("allocFail");
            return;
        }
        g_wand.live      = true;
        g_wandLive.store(true, std::memory_order_relaxed);
        g_wand.bhkWorld  = authWorld;
        g_wand.heldWorld.reset(static_cast<RE::bhkWorld*>(authWorld));
        g_wand.lastWord  = word;
        g_wand.createdAt = std::chrono::steady_clock::now();
        logger::info("WAND CREATE segs={} word=0x{:08X} part={} r={:.2f}u (bit14 ON, {} ms delay)",
                     kWandSegs, word, ObjectHold::PlayerWandPart(), ObjectHold::PlayerWandR(),
                     kEnableDelayMs);
    }

    void WandFilterRefresh(void* authWorld)
    {
        if (!g_wand.live) return;
        const std::uint32_t w = WandComposeWord(false);
        if (!w || w == g_wand.lastWord) return;
        void* hkpW = GetHkpWorld(authWorld);
        if (!IsLikelyPointer(hkpW)) return;
        {
            WorldWriteLock lock(authWorld);
            for (void* mem : g_wand.bodyMem) {
                hkpRigidBody* hk = HkOf(mem);
                if (!hk) continue;
                hk->getCollidableRw()->setCollisionFilterInfo(w);
                fn_UpdateFilter()(hkpW, static_cast<hkpEntity*>(hk),
                                  HK_UPDATE_FILTER_ON_ENTITY_FULL_CHECK,
                                  HK_UPDATE_COLLECTION_FILTER_PROCESS_SHAPE_COLLECTIONS);
            }
        }
        logger::info("WAND filter: 0x{:08X} -> 0x{:08X} (bit14={})",
                     g_wand.lastWord, w, (w & kBit14) != 0);
        g_wand.lastWord = w;
    }

    void WandKeyframe(void* authWorld)
    {
        if (!g_wand.live || !g_wandSnap.valid) return;
        const float maxVel = ObjectHold::HandBoxMaxVel();
        WorldWriteLock lock(authWorld);
        for (int i = 0; i < kWandSegs; ++i) {
            hkpRigidBody* hk = HkOf(g_wand.bodyMem[i]);
            if (!hk) continue;
            float posU[3]; QuatW rot; float halfM[3];
            WandSegPose(i, posU, rot, halfM);
            // live half-extent write, same idempotent idiom as ResolveGeometryLive: the SMP sim
            // changes segment length every frame (sway/erection), so the shape must follow
            const hkpShape* shape = hk->getCollidable()->getShape();
            auto* box = reinterpret_cast<PortHkpBoxShape*>(const_cast<hkpShape*>(shape));
            if (box && box->type == HK_SHAPE_BOX) {
                const float dx = std::fabs(box->halfExtents[0] - halfM[0]) +
                                 std::fabs(box->halfExtents[1] - halfM[1]) +
                                 std::fabs(box->halfExtents[2] - halfM[2]);
                if (dx > 1e-4f) {
                    box->halfExtents[0] = halfM[0]; box->halfExtents[1] = halfM[1];
                    box->halfExtents[2] = halfM[2];
                }
            }
            hkVector4 pos;
            pos.set(posU[0] * kSkyrimToHavok, posU[1] * kSkyrimToHavok, posU[2] * kSkyrimToHavok, 0.f);
            hkQuaternion q;
            q.set(rot.x, rot.y, rot.z, rot.w);
            fn_applyHardKeyFrame()(pos, q, g_snap.invDt, hk);
            const hkVector4& lv = hk->getLinearVelocity();
            if (lv(0)*lv(0) + lv(1)*lv(1) + lv(2)*lv(2) > maxVel * maxVel) {
                fn_setPosition()(static_cast<hkpEntity*>(hk), pos);        // spike: snap, then
                fn_applyHardKeyFrame()(pos, q, g_snap.invDt, hk);          // re-key -> ~0 residual
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════
    //  PLAYER HEAD BOX (2026-09-03) — one keyframed box riding the VRIK-posed head node.
    //  A wand clone by construction: same body factory, same word composition, same once-per-
    //  frame keyframe + clamp, same strong-heldWorld teardown, same null-userData treatment.
    //  Only the rider and the belts differ. USER SPEC: "i don't want my head to be stopped, i
    //  want the NPC's body to move when my head contact with it and push the NPC with it, like
    //  the hand does. which will make me not get inside their body anymore."
    //  Rider = the THIRD-PERSON "NPC Head [Head]" node (Get3D(false)) — i.e. VRIK's output, the
    //  head the player actually sees, with every VRIK offset and scale already folded in. That
    //  is the same deliberate divergence from HIGGS the finger boxes make (doc 09 §5).
    void HeadSnapshot()
    {
        g_headSnap.valid = false;
        g_headSnap.hmdValid = false;
        g_headSnap.rodeHmd = false;
        g_headSnap.fullRotValid = false;   // ★ kiss: never let last frame's headset pose survive a miss
        if (!ObjectHold::HeadBoxOn() && !g_head.live) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root   = player ? player->Get3D(false) : nullptr;   // 3rd person: VRIK poses this
        if (!root) return;
        static RE::BSFixedString s_name;
        static bool s_interned = false;
        if (!s_interned) { s_name = kHeadNode; s_interned = true; }
        RE::NiAVObject* n = root->GetObjectByName(s_name);
        if (!n) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                logger::info("HEADBOX: '{}' not found on the player's 3rd-person skeleton — "
                             "head box stays down.", kHeadNode);
            }
            return;
        }
        const RE::NiPoint3&  t = n->world.translate;
        const RE::NiMatrix3& R = n->world.rotate;
        g_headSnap.pos[0] = t.x; g_headSnap.pos[1] = t.y; g_headSnap.pos[2] = t.z;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                g_headSnap.rot[r * 3 + c] = R.entry[r][c];
        g_headSnap.scale = (n->world.scale > 0.01f) ? n->world.scale : 1.f;
        g_headSnap.valid = true;
        // ── DIAGNOSTIC ONLY (never a driver): is the node we ride actually where the player's
        // face is? If VRIK's head node does NOT track the HMD, this one number says so and we
        // change riders.
        // ⛔ THE RAW OFFSET WAS WRONG AND WOULD HAVE LIED CONFIDENTLY. This first read
        // PlayerCharacter+0x570 as "HmdNode" on the strength of a sibling module's note
        // (Report/Follower Bump Guard Module/01 §B5) — but the CommonLibVR this plugin actually
        // compiles against names 0x570 `GamepadNode` (PlayerCharacter.h:146), with LastSyncPos
        // at 0x578 and `UprightHmdNode` at 0x580. A dial session steered by a gamepad-node
        // distance is worse than no diagnostic at all. Use the TYPED accessor — GetVRNodeData()
        // at +0x3F0 — so the field is resolved by name and a CommonLib update cannot silently
        // re-point it. The name is printed with the number so the log says what it measured.
        // ⚠ CORRECTION (2026-09-12, research — this note is appended, not a rewrite): the warning
        // above was right to demand the typed accessor and WRONG about what +0x570 is. CommonLibVR
        // 4.14.0 mislabels it `GamepadNode`; it IS the HMD node. VRIK's own PDB enum lists
        // kNode_HmdNode = 48 (0x3F0 + 48*8 = 0x570), HIGGS computes its mouth zone from it, VRIK
        // and PLANCK read the headset there, and CommonLibSSE-NG 6.3.2 has since renamed the field
        // `HmdNode`. +0x580 (UprightHmdNode) is a DIFFERENT node, assumed yaw-only. The box keeps
        // riding +0x580; the KISS mouth probe now poses on +0x570 (see MouthProbeSegment).
        if (auto* vr = player->GetVRNodeData()) {
            // ★ KISS (2026-09-12): the full-rotation headset. Typed field, then a POSITION sanity
            // check before trusting it — a node that resolves but sits nowhere near the player is
            // worse than no node, because the probe would confidently sense the wrong place.
            if (auto* full = vr->GamepadNode.get()) {   // CommonLibVR's name for +0x570 = HmdNode
                const RE::NiPoint3& fp = full->world.translate;
                const RE::NiPoint3  pp = player->GetPosition();
                const float dx = fp.x - pp.x, dy = fp.y - pp.y, dz = fp.z - pp.z;
                // ★ HEADING check (round-2 review): position alone cannot prove this node is the headset — tracking
                // loss or a not-yet-initialised node can sit in the right place with the wrong rotation. Its
                // HORIZONTAL forward must agree with the upright headset's within 45 deg. Skipped when looking
                // nearly straight up/down, where a horizontal heading is undefined (and the upright node's is kept).
                bool headingOk = true;
                float headingDot = 1.f;
                if (auto* upr = vr->UprightHmdNode.get()) {
                    const RE::NiMatrix3& FRh = full->world.rotate;
                    const RE::NiMatrix3& URh = upr->world.rotate;
                    const float fx0 = FRh.entry[0][1], fy0 = FRh.entry[1][1];
                    const float ux0 = URh.entry[0][1], uy0 = URh.entry[1][1];
                    const float fl = std::sqrt(fx0 * fx0 + fy0 * fy0), ul = std::sqrt(ux0 * ux0 + uy0 * uy0);
                    if (fl > 0.31f && ul > 0.31f) {                     // |forward.z| < ~0.95 on both
                        headingDot = (fx0 * ux0 + fy0 * uy0) / (fl * ul);
                        headingOk  = headingDot > 0.7071f;              // cos 45
                    }
                }
                if (dx * dx + dy * dy < 300.f * 300.f && dz > -50.f && dz < 250.f && headingOk) {
                    const RE::NiMatrix3& FR = full->world.rotate;
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            g_headSnap.fullRot[r * 3 + c] = FR.entry[r][c];
                    g_headSnap.fwdZFull     = FR.entry[2][1];   // column 1 = forward; row 2 = world z
                    g_headSnap.fullRotValid = true;
                } else if (!headingOk) {
                    static std::chrono::steady_clock::time_point s_hdgAt{};
                    const auto nowH = std::chrono::steady_clock::now();
                    if (s_hdgAt.time_since_epoch().count() == 0 || nowH - s_hdgAt > std::chrono::seconds(60)) {
                        s_hdgAt = nowH;
                        logger::info("HEADBOX: +0x570 headset REJECTED on heading - its horizontal forward is {:.0f} deg "
                                     "from the upright headset's (want < 45) - the kiss probe uses the old pose this frame",
                                     std::acos(std::clamp(headingDot, -1.f, 1.f)) * 57.29578f);
                    }
                } else {
                    // ⛔ every exit speaks: a rejected headset silently drops the kiss probe back to the old pose,
                    // which would read in VR as "the fix did nothing". Say so, rate-limited to once a minute.
                    static std::chrono::steady_clock::time_point s_rejAt{};
                    const auto nowR = std::chrono::steady_clock::now();
                    if (s_rejAt.time_since_epoch().count() == 0 || nowR - s_rejAt > std::chrono::seconds(60)) {
                        s_rejAt = nowR;
                        logger::info("HEADBOX: +0x570 headset FAILED its sanity check (dxy {:.0f}u, dz {:+.0f}u from the "
                                     "player's feet; want <300 / -50..250) - the kiss probe is using the old pose this frame",
                                     std::sqrt(dx * dx + dy * dy), dz);
                    }
                }
            }
            if (auto* hmd = vr->UprightHmdNode.get()) {
                g_headSnap.fwdZUpright = hmd->world.rotate.entry[2][1];
                const RE::NiPoint3& h = hmd->world.translate;
                g_headSnap.hmdPos[0] = h.x; g_headSnap.hmdPos[1] = h.y; g_headSnap.hmdPos[2] = h.z;
                g_headSnap.hmdValid = true;
                // ★★ THE RIDER SWAP (2026-09-03). Everything above resolved the VRIK head BONE;
                // if we are riding the HEADSET, that was only the diagnostic and the headset now
                // becomes the pose outright. Deliberately done HERE, after the bone resolved, so
                // that a missing/!hmdValid headset degrades to the bone instead of dropping the
                // box — a fallback that fails CLOSED (the box keeps existing) rather than open.
                //   Scale is NOT taken from the HMD node: it is a VR tracking node with no
                // relation to the character's render scale, and the box's size must keep scaling
                // with the body. The bone's scale, already read above, stays.
                if (ObjectHold::HeadBoxRideHmd()) {
                    g_headSnap.pos[0] = h.x; g_headSnap.pos[1] = h.y; g_headSnap.pos[2] = h.z;
                    const RE::NiMatrix3& HR = hmd->world.rotate;
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            g_headSnap.rot[r * 3 + c] = HR.entry[r][c];
                    g_headSnap.rodeHmd = true;
                }
            }
        }
        // Riding was ASKED FOR but the headset did not resolve — say so once, because a silent
        // fall back to the bone reproduces the exact posture bug the rider exists to fix.
        if (ObjectHold::HeadBoxRideHmd() && !g_headSnap.hmdValid) {
            static bool s_hmdWarned = false;
            if (!s_hmdWarned) {
                s_hmdWarned = true;
                logger::info("HEADBOX: headBoxRider 1 but UprightHmdNode did not resolve — "
                             "falling back to the VRIK head bone (the box will sit at chin level "
                             "when you stand). GetVRNodeData() null or the node is not built yet.");
            }
        }
        static bool s_resolvedOnce = false;
        if (!s_resolvedOnce) {
            s_resolvedOnce = true;
            logger::info("HEADBOX: rider resolved '{}' scale={:.3f}", kHeadNode, g_headSnap.scale);
        }
    }

    // VRIK's own head-hide distance, read through its interface (slot 1 getSettingDouble) and
    // cached — VRIK re-reads its ini on its own schedule, so we refresh at ~1 Hz rather than
    // per frame. Returns 0 when VRIK is absent or the key is unknown, which degrades to "no
    // compensation" and leaves headBoxOffYU doing the whole job, exactly as before.
    float VrikHeadHideU()
    {
        if (!ObjectHold::HeadBoxVrikCompOn()) return 0.f;
        // ⛔ INERT WHILE RIDING THE HEADSET. The hide is a displacement VRIK applies to the 3P
        // head BONE; the headset was never moved, so adding it back would push the box a further
        // 12u out in front of the player's face. Keyed on what the snapshot ACTUALLY rode this
        // frame, not on the knob, so the fallback path (rider asked for, headset unresolved)
        // still gets its compensation.
        if (g_headSnap.rodeHmd) return 0.f;
        static float s_cached = 0.f;
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (s_last.time_since_epoch().count() == 0 || now - s_last > std::chrono::milliseconds(1000)) {
            s_last = now;
            float v = 0.f;
            if (auto* vr = Interop::GetVrik()) v = (float)vr->getSettingDouble("hidePlayerHeadDistance");
            // sane band: the setting's own documented range is 0..20. A wild value means we
            // read the wrong key or VRIK changed its schema — compensate nothing rather than
            // fling the box across the room.
            s_cached = (v > 0.f && v < 40.f) ? v : 0.f;
            static bool s_said = false;
            if (!s_said && s_cached > 0.f) {
                s_said = true;
                logger::info("HEADBOX: VRIK hidePlayerHeadDistance = {:.2f}u — compensating "
                             "forward (VRIK pushes the 3P head node BACK to hide your face; the "
                             "box rode that hide and registered late)", s_cached);
            }
        }
        return s_cached;
    }

    // Box pose from the snapshot: centre = node + R * (offset * scale); axes = the node's own.
    void HeadPose(float posU[3], QuatW& rot, float halfM[3])
    {
        const float s = g_headSnap.scale;
        // +Y is the head's FORWARD (anterior) axis, so undoing VRIK's backward hide is a
        // straight addition on Y. headBoxOffYU is then the residual dial to the NOSE.
        const float fwd = ObjectHold::HeadBoxOffYU() + VrikHeadHideU();
        const float off[3] = { ObjectHold::HeadBoxOffXU() * s,
                               fwd * s,
                               ObjectHold::HeadBoxOffZU() * s };
        const float* rr = g_headSnap.rot;
        for (int r = 0; r < 3; ++r)
            posU[r] = g_headSnap.pos[r] +
                      rr[r * 3 + 0] * off[0] + rr[r * 3 + 1] * off[1] + rr[r * 3 + 2] * off[2];
        rot = Mat3ToQuat(rr);
        halfM[0] = (std::max)(ObjectHold::HeadBoxHalfXU() * s * kSkyrimToHavok, 0.010f);
        halfM[1] = (std::max)(ObjectHold::HeadBoxHalfYU() * s * kSkyrimToHavok, 0.010f);
        halfM[2] = (std::max)(ObjectHold::HeadBoxHalfZU() * s * kSkyrimToHavok, 0.010f);
    }

    void DestroyHead(void* liveWorld, const char* reason)
    {
        if (!g_head.live) return;
        void* owner = g_head.heldWorld ? static_cast<void*>(g_head.heldWorld.get()) : liveWorld;
        if (owner && g_head.bodyMem) RemoveBody(g_head.bodyMem, owner);
        logger::info("HEADBOX DESTROY reason={}", reason);
        g_head = HeadRig{};
        g_headLive.store(false, std::memory_order_relaxed);
    }

    // Same two deliberate divergences from FilterRefresh the wand makes: the inherited bit 14 is
    // MASKED OUT (HIGGS disabling its HAND while holding a sword must not switch off your head),
    // and there is no IsHoldingObject/IsTwoHanding OR.
    std::uint32_t HeadComposeWord(bool creating)
    {
        auto* h = Interop::GetHiggs();
        hkpRigidBody* handBody = h ? HkOfValidated(h->GetHandRigidBody(false)) : nullptr;
        if (!handBody && h) handBody = HkOfValidated(h->GetHandRigidBody(true));
        if (!handBody) return 0;
        const std::uint32_t base  = handBody->getCollidable()->getCollisionFilterInfo();
        const std::uint32_t pgrpW = base >> 16;
        const std::uint32_t egrpW = EffectiveGroup(pgrpW, HiggsHoldingAnything());
        g_playerGroup.store(pgrpW, std::memory_order_relaxed);
        // the head box may be the ONLY live rig — publish the group here too or isOurs() reads a
        // stale/zero group and matches nothing (the wand's own lesson, same comment).
        g_boxGroup.store(egrpW, std::memory_order_relaxed);
        g_headPart.store(ObjectHold::HeadBoxPart(), std::memory_order_relaxed);
        std::uint32_t w = ((base & ~(0x00001F00u | kBit14)) & 0x0000FFFFu)
                        | (ObjectHold::HeadBoxPart() << 8) | (egrpW << 16);
        if (creating ||
            std::chrono::steady_clock::now() - g_head.createdAt < std::chrono::milliseconds(kEnableDelayMs))
            w |= kBit14;
        return w;
    }

    void HeadLifecycle(void* authWorld, bool beast)
    {
        // Scene gate: same reason as the wand's — a real collider in the middle of an
        // OStim/SexLab animation shoves the partner the animation is placing. ⚠ but honour
        // sceneSuspendHands like the hand rigs do rather than dropping unconditionally: mode 0
        // means "the user does not want scene suspension", and PPB's scene flag is a global
        // COUNT of any scene anywhere, so an unconditional drop kills the head box for two NPCs
        // in another room. (Mode 2, third-person-only, is a hand-rig heuristic that does not
        // apply to a head collider, so anything non-zero suspends here.)
        const bool sceneOff = g_sceneSuspend.load(std::memory_order_relaxed) &&
                              ObjectHold::SceneSuspendHandsMode() != 0;
        // Beast forms (werewolf / vampire lord) have a head node in a wholly different place and
        // proportion; the dialled offsets would be wrong. Reuse the boxes' own beast gate rather
        // than adding a second knob for a rare state.
        const bool beastOff = beast && !ObjectHold::HandBoxBeast();
        const bool want = ObjectHold::HeadBoxOn() && g_headSnap.valid && !sceneOff && !beastOff;
        if (g_head.live) {
            if (g_head.bhkWorld != authWorld) DestroyHead(authWorld, "worldChange");
            else if (!want)                   DestroyHead(authWorld, "off");
        }
        if (g_head.live || !want) return;
        const std::uint32_t word = HeadComposeWord(true);
        if (!word) { LogSkip("noHiggsHand"); return; }
        float posU[3]; QuatW rot; float halfM[3];
        HeadPose(posU, rot, halfM);
        g_head.bodyMem = CreateBoxBody(authWorld, posU, rot, halfM, word);
        if (!g_head.bodyMem) { LogSkip("allocFail"); return; }
        g_head.live      = true;
        g_headLive.store(true, std::memory_order_relaxed);
        g_head.bhkWorld  = authWorld;
        g_head.heldWorld.reset(static_cast<RE::bhkWorld*>(authWorld));
        g_head.lastWord  = word;
        g_head.createdAt = std::chrono::steady_clock::now();
        logger::info("HEADBOX CREATE word=0x{:08X} part={} half=({:.2f},{:.2f},{:.2f})u "
                     "off=({:.2f},{:.2f},{:.2f})u (bit14 ON, {} ms delay)",
                     word, ObjectHold::HeadBoxPart(),
                     ObjectHold::HeadBoxHalfXU(), ObjectHold::HeadBoxHalfYU(),
                     ObjectHold::HeadBoxHalfZU(), ObjectHold::HeadBoxOffXU(),
                     ObjectHold::HeadBoxOffYU(), ObjectHold::HeadBoxOffZU(), kEnableDelayMs);
    }

    void HeadFilterRefresh(void* authWorld)
    {
        if (!g_head.live) return;
        const std::uint32_t w = HeadComposeWord(false);
        if (!w || w == g_head.lastWord) return;
        void* hkpW = GetHkpWorld(authWorld);
        if (!IsLikelyPointer(hkpW)) return;
        {
            WorldWriteLock lock(authWorld);
            hkpRigidBody* hk = HkOf(g_head.bodyMem);
            if (hk) {
                hk->getCollidableRw()->setCollisionFilterInfo(w);
                fn_UpdateFilter()(hkpW, static_cast<hkpEntity*>(hk),
                                  HK_UPDATE_FILTER_ON_ENTITY_FULL_CHECK,
                                  HK_UPDATE_COLLECTION_FILTER_PROCESS_SHAPE_COLLECTIONS);
            }
        }
        logger::info("HEADBOX filter: 0x{:08X} -> 0x{:08X} (bit14={})",
                     g_head.lastWord, w, (w & kBit14) != 0);
        g_head.lastWord = w;
    }

    void HeadKeyframe(void* authWorld)
    {
        if (!g_head.live || !g_headSnap.valid) return;
        // ⚠ NOT HandBoxMaxVel. See Tuning.h: this clamp is the anti-launch defence, and it is
        // deliberately tight enough that a run-in teleports instead of delivering an impulse.
        const float maxVel = ObjectHold::HeadBoxMaxVel();
        WorldWriteLock lock(authWorld);
        hkpRigidBody* hk = HkOf(g_head.bodyMem);
        if (!hk) return;
        float posU[3]; QuatW rot; float halfM[3];
        HeadPose(posU, rot, halfM);
        // live half-extent write, the same idempotent idiom the wand and the slab dial use: the
        // knobs are hot, so the shape follows a dial without a rebuild. Only ever on a change.
        const hkpShape* shape = hk->getCollidable()->getShape();
        auto* box = reinterpret_cast<PortHkpBoxShape*>(const_cast<hkpShape*>(shape));
        if (box && box->type == HK_SHAPE_BOX) {
            const float dx = std::fabs(box->halfExtents[0] - halfM[0]) +
                             std::fabs(box->halfExtents[1] - halfM[1]) +
                             std::fabs(box->halfExtents[2] - halfM[2]);
            if (dx > 1e-4f) {
                box->halfExtents[0] = halfM[0]; box->halfExtents[1] = halfM[1];
                box->halfExtents[2] = halfM[2];
            }
        }
        hkVector4 pos;
        pos.set(posU[0] * kSkyrimToHavok, posU[1] * kSkyrimToHavok, posU[2] * kSkyrimToHavok, 0.f);
        hkQuaternion q;
        q.set(rot.x, rot.y, rot.z, rot.w);
        fn_applyHardKeyFrame()(pos, q, g_snap.invDt, hk);
        // ⚠ BOTH velocities, and BOTH parts of the pose on recovery. The clamp exists to stop
        // this body handing an NPC bone an impulse, and H2's blade term is w x r — an unbounded
        // ANGULAR velocity on a head-sized box delivers exactly that at the corners. Snapping
        // position alone also leaves the spun orientation live, so the re-key computes its
        // angular velocity from a stale rotation and the spike survives the guard.
        const hkVector4& lv = hk->getLinearVelocity();
        const hkVector4& av = hk->getAngularVelocity();
        const bool linSpike = lv(0)*lv(0) + lv(1)*lv(1) + lv(2)*lv(2) > maxVel * maxVel;
        const bool angSpike = av(0)*av(0) + av(1)*av(1) + av(2)*av(2) > kHeadMaxAngVel * kHeadMaxAngVel;
        if (linSpike || angSpike) {
            fn_setPosition()(static_cast<hkpEntity*>(hk), pos);        // spike: snap BOTH, then
            fn_setRotation()(static_cast<hkpEntity*>(hk), q);
            fn_applyHardKeyFrame()(pos, q, g_snap.invDt, hk);          // re-key -> ~0 residual
        }
    }

    void HeadLog()
    {
        if (!g_head.live || !ObjectHold::HeadBoxLogOn()) return;
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (s_last.time_since_epoch().count() != 0 && now - s_last < std::chrono::milliseconds(1000)) return;
        s_last = now;
        float posU[3]; QuatW rot; float halfM[3];
        HeadPose(posU, rot, halfM);
        // ★ THE DIAL READOUT. A scalar distance says "you are 9u off" but not WHICH WAY, so it
        // cannot be typed into a knob. Resolve the box->HMD delta into the head node's own axes
        // — the same frame headBoxOff*U live in — and the correction becomes readable: add
        // these three numbers to the current offsets and the box lands on the headset.
        float hmdD = -1.f, dl[3] = { 0.f, 0.f, 0.f };
        if (g_headSnap.hmdValid) {
            const float d[3] = { g_headSnap.hmdPos[0] - posU[0],
                                 g_headSnap.hmdPos[1] - posU[1],
                                 g_headSnap.hmdPos[2] - posU[2] };
            hmdD = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            const float* rr = g_headSnap.rot;          // columns are the axes -> transpose to invert
            for (int c = 0; c < 3; ++c)
                dl[c] = rr[0*3 + c] * d[0] + rr[1*3 + c] * d[1] + rr[2*3 + c] * d[2];
            if (g_headSnap.scale > 0.01f)
                for (float& v : dl) v /= g_headSnap.scale;
        }
        logger::info("HEADBOX rider={} centre=({:.1f},{:.1f},{:.1f}) node=({:.1f},{:.1f},{:.1f}) "
                     "half=({:.2f},{:.2f},{:.2f})u scale={:.3f} word=0x{:08X} vrikHide={:.1f}u "
                     "uprightHmdDist={:.1f}u  TO-HMD-LOCAL=({:+.2f},{:+.2f},{:+.2f})u "
                     "<- add to headBoxOffX/Y/ZU to sit ON the headset",
                     g_headSnap.rodeHmd ? "HMD" : "bone",
                     posU[0], posU[1], posU[2],
                     g_headSnap.pos[0], g_headSnap.pos[1], g_headSnap.pos[2],
                     halfM[0] * kHavokToSkyrim, halfM[1] * kHavokToSkyrim,
                     halfM[2] * kHavokToSkyrim, g_headSnap.scale, g_head.lastWord,
                     VrikHeadHideU(), hmdD, dl[0], dl[1], dl[2]);
    }

    void WandLog()
    {
        if (!g_wand.live || !ObjectHold::PlayerWandLogOn()) return;
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (s_last.time_since_epoch().count() != 0 && now - s_last < std::chrono::milliseconds(1000)) return;
        s_last = now;
        const float* a = g_wandSnap.p[0]; const float* t = g_wandSnap.p[2];
        const float d[3] = { t[0]-a[0], t[1]-a[1], t[2]-a[2] };
        logger::info("WAND base=({:.1f},{:.1f},{:.1f}) len={:.2f}u word=0x{:08X} valid={}",
                     a[0], a[1], a[2], std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]) +
                     kWandTipPadU, g_wand.lastWord, g_wandSnap.valid ? 1 : 0);
    }


    // ═════════════════════════════════════════════════════════════════════════════════════════
    //  NULL USERDATA (2026-08-18) — the PLANCK-hit / IWP-stab fix.
    //  ROOT CAUSE (verified in source): while HIGGS holds an NPC, our finger box touching the held
    //  limb enters PLANCK's hit logic; because our box carries a valid hkpRigidBody userData
    //  back-pointer (the PortBhkRigidBody wrapper), PLANCK does NOT early-return at main.cpp:2081
    //  and instead runs Character_HitTarget -> a REAL TESHitEvent -> Immersive Weapon Penetration
    //  VR's sink dramatises it into a stab + downed-crit ragdoll.
    //  FIX: our boxes are SYNTHETIC SENSORS with no Skyrim ref, so a null userData is the honest
    //  value. Nulled, PLANCK bails at :2081 BEFORE any hit; and because our bodies are plain
    //  KEYFRAMED (not KEYFRAMED_REPORTING) the :2082 CONTACT_IS_DISABLED branch is skipped, so the
    //  finger STILL TOUCHES — it just stops registering as a strike. HIGGS is unaffected (it already
    //  resolves our box to "not mine"; GetRefFromCollidable simply returns null, which its listeners
    //  already handle).
    //  ⚠ SELF-VERIFYING: hkpWorldObject::userData is at 0x18 (CommonLib-NG: world@0x10 userData@0x18
    //  collidable@0x20). After the game ctor the field holds OUR OWN wrapper pointer (== bodyMem),
    //  so we only write when *field == bodyMem. A wrong offset or unexpected state can therefore
    //  NEVER corrupt a live body — we refuse and log instead. Main thread (OnPrePhysicsStep), so
    //  logging is legal; the write is one aligned pointer store, idempotent (skips once nulled).
    constexpr std::size_t kUserDataOff = 0x18;
    // AUTO mode (2) safe-boot: stays false until the first kPostLoadGame of the session, so the
    // userData write never happens during the fragile initial cell load — only once a save is up.
    std::atomic<bool> g_sessionReloaded{ false };

    void NullUserDataOne(void* bodyMem, void* authWorld, const char* tag, int idx)
    {
        if (!bodyMem) return;
        hkpRigidBody* hk = HkOf(bodyMem);
        if (!hk) return;
        void** ud = reinterpret_cast<void**>(reinterpret_cast<char*>(hk) + kUserDataOff);
        void* cur = *ud;
        if (cur == nullptr) return;                          // already nulled — idempotent skip
        if (cur != bodyMem) {                                // NOT our wrapper: refuse, log ONCE
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                logger::info("NULLUD REFUSED on {}[{}]: userData@0x18 = {} but our wrapper = {} — "
                             "offset unverified, NOTHING written (safe).", tag, idx,
                             fmt::ptr(cur), fmt::ptr(bodyMem));
            }
            return;
        }
        {
            WorldWriteLock lock(authWorld);
            *ud = nullptr;                                    // verified: it was OUR back-pointer
        }
        logger::info("NULLUD {}[{}]: userData verified (== our wrapper) and nulled — PLANCK will "
                     "no longer read this box as a weapon hit.", tag, idx);
    }

    void NullUserDataTick(void* authWorld)
    {
        const int mode = ObjectHold::HandBoxNullUserDataMode();
        if (mode == 0) return;                                        // off
        if (mode == 2 && !g_sessionReloaded.load(std::memory_order_relaxed))
            return;                                                   // auto: wait for the first load
        for (int h = 0; h < 2; ++h)
            if (g_rig[h].live)
                for (int b = 0; b < 4; ++b)
                    NullUserDataOne(g_rig[h].bodyMem[b], authWorld, h == 1 ? "L-box" : "R-box", b);
        if (g_wand.live)
            for (int i = 0; i < kWandSegs; ++i)
                NullUserDataOne(g_wand.bodyMem[i], authWorld, "wand", i);
        // ★ The head box needs this MORE than the boxes do: a headbutt is exactly the contact
        // PLANCK's hit classifier would dramatise into a strike (and IWP-VR into a stab). Nulled,
        // PLANCK early-returns before Character_HitTarget; because we are plain KEYFRAMED (never
        // KEYFRAMED_REPORTING) the CONTACT_IS_DISABLED branch is skipped too, so the head still
        // TOUCHES and still PUSHES — it just never registers as a weapon hit.
        if (g_head.live)
            NullUserDataOne(g_head.bodyMem, authWorld, "head", 0);
    }

        void OnPrePhysicsStep(void* worldArg)
    {
        if (!AnythingActive()) return;                       // zero-knob cost: a few float reads

        // review N6: only the PLAYER's parent cell's world is authoritative — the
        // stepDeltaTime hook fires for EVERY stepped world, and a transition-overlap
        // fire from another world must not thrash create/destroy.
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell   = player ? player->GetParentCell() : nullptr;
        void* authWorld = cell ? static_cast<void*>(cell->GetbhkWorld()) : nullptr;
        if (!authWorld || worldArg != authWorld) return;

    // review N1: act exactly ONCE per frame (the OnFrame counter is the frame
        // edge) — the 2nd physics substep of a slow frame has a zero transform
        // delta and would re-keyframe the boxes into momentum-less ghosts mid-frame.
        if (g_snap.id == g_lastConsumedSnap) return;
        g_lastConsumedSnap = g_snap.id;

        // 2026-07-10 LAG FIX: snapshot the bones NOW, at the step boundary —
        // fresh hand pose for this frame's keyframes (see TakeBoneSnapshot).
        TakeBoneSnapshot();
        WandSnapshot();                   // player SOS chain pose (wand rider)
        HeadSnapshot();                   // player head node pose (head box rider)
        UpdateEffFrames();                // mode 2: HIGGS-anchored effective hand frames

        const bool beast = IsPlayerBeast(player);
        SlabWrite(beast);                 // Phase 0 (Track A)
        RigLifecycle(authWorld, beast);   // create/destroy/recreate (N14: only here)
        WandLifecycle(authWorld);         // player genital wand (same N14 discipline)
        HeadLifecycle(authWorld, beast);  // player head box (same N14 discipline)
        ResolveGeometryLive();            // Track B: per-snapshot re-solve + live half-extents (before keyframing)
        FilterRefresh(authWorld);         // on-change word write + UpdateCollisionFilterOnEntity
        WandFilterRefresh(authWorld);     // wand word (bit14 deliberately NOT inherited)
        HeadFilterRefresh(authWorld);     // head word (same divergences as the wand)
        PlayerSpaceWarp(authWorld, player);  // 2026-07-12: locomotion delta moved POSITIONALLY (HIGGS parity)
        KeyframeAll(authWorld);           // 8× applyHardKeyFrame + clamp (consumes the re-solved def)
        WandKeyframe(authWorld);          // +2 wand segments, same servo
        HeadKeyframe(authWorld);          // +1 head box, tighter clamp (anti-launch)
        NullUserDataTick(authWorld);      // PLANCK-hit fix (self-verifying; boxes + wand + head)
        WandLog();                        // ~1 Hz when playerWandLog
        HeadLog();                        // ~1 Hz when headBoxLog
        PhaseLog();                       // HBOXPH jitter diagnostic (no-op unless handBoxPhaseLog)
        DumpIfRequested(authWorld);
        BoxDumpIfRequested();             // per-box readback (edge on handBoxDumpNow)
    }

}  // namespace

namespace HandBox {

    void SetSceneSuspended(bool on) { g_sceneSuspend.store(on, std::memory_order_relaxed); }
    void MarkSessionReloaded()      { g_sessionReloaded.store(true, std::memory_order_relaxed); }
    bool IsSceneSuspended()         { return g_sceneSuspend.load(std::memory_order_relaxed); }


    // Relaxed store only — physics thread, no allocation, no float math (the collision-callback
    // discipline: this seam has produced a movaps CTD before).
    void NotePhysicsStepDt(float dt)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &dt, sizeof(bits));
        g_stepDtBits.store(bits, std::memory_order_relaxed);
        if (dt > 0.f && dt < 1.f)
            g_stepAccumUs.fetch_add(static_cast<std::uint32_t>(dt * 1.0e6f),
                                    std::memory_order_relaxed);
    }


    // ── ReTouch fingertip export (2026-07-24): the mouth-touch gate wants "where is the
    // player's pointing finger" — that is one of OUR boxes. Returns the box CENTER (the boxes
    // are small; the detection thresholds absorb the half-extent).
    bool TipWorldU(int hand, int box, float outU[3])
    {
        if (hand < 0 || hand > 1 || !g_rig[hand].live) return false;
        if (box < 0) box = 0; else if (box > 3) box = 3;
        const BoxDef& d = g_rig[hand].def[box];
        QuatW q;
        BoxWorldPose(hand, d, outU, q);
        // extend to the TIP: half-length down the box's local +Z (down-the-bone), half[] is
        // METRES -> Skyrim units. z-axis from the quat directly.
        const float zx = 2.f * (q.x * q.z + q.w * q.y);
        const float zy = 2.f * (q.y * q.z - q.w * q.x);
        const float zz = 1.f - 2.f * (q.x * q.x + q.y * q.y);
        const float ext = d.half[2] * kHavokToSkyrim;
        outU[0] += zx * ext; outU[1] += zy * ext; outU[2] += zz * ext;
        return true;
    }
    bool BoxCenterWorldU(int hand, int box, float outU[3])
    {
        if (hand < 0 || hand > 1 || !g_rig[hand].live) return false;
        if (box < 0 || box > 3) return false;
        QuatW q;
        BoxWorldPose(hand, g_rig[hand].def[box], outU, q);
        return true;
    }

    // 2026-09-07 (report 33 §8.3): pointer -> hand for the engine-contact stamp. See HandBox.h.
    int HandOfBody(const void* hkpBody)
    {
        if (!hkpBody) return -1;
        for (int hand = 0; hand < 2; ++hand) {
            if (!g_rig[hand].live) continue;
            for (int b = 0; b < 4; ++b)
                if (static_cast<const void*>(HkOf(g_rig[hand].bodyMem[b])) == hkpBody) return hand;
        }
        return -1;
    }
    std::uint32_t BoxPartLive() { return g_boxPart.load(std::memory_order_relaxed); }   // v11.1b, see HandBox.h

    // ── 2026-08-23: publish the wand for the PROBE EXPORT. See HandBox.h.
    // Deliberately re-derives the endpoints the same way WandSegPose does — INCLUDING the
    // kWandTipPadU extension on the last segment — rather than reading the snapshot raw, so
    // the probe and the collider describe one object. Diverging here would be the worst kind
    // of bug: a sensor that reports contact where nothing can actually touch.
    // Main thread, like every other accessor here: g_wandSnap/g_wand live in the main-thread
    // state block (:385) and WandSnapshot writes them from OnPrePhysicsStep, which is the
    // main-thread stepDeltaTime site (:1577) — not the filter callback's physics thread. Reads
    // floats and a bool only, never a Havok pointer, so even a mis-sequenced call can be stale,
    // never unsafe.
    // ── HEAD BOX probe export (2026-09-03) — the same discipline as WandProbeSegments: publish
    // the SAME geometry the body occupies, gated on the BODY being live, so the geometric touch
    // view and the Havok collider can never disagree. A probe must never claim reach the collider
    // does not have.
    //   The box is modelled to the touch scan as a SEGMENT + RADIUS (the weapon-blade idiom): the
    // segment runs along the box's local +Y — anterior, i.e. back-of-skull -> face — so the
    // NEAREST point of the head is what gets measured, and the segment's far end is the face
    // itself. Radius is the mean of the two lateral half-extents, which inscribes the box.
    bool HeadProbeSegment(float aOutU[3], float bOutU[3], float* rOutU)
    {
        if (!aOutU || !bOutU || !rOutU) return false;
        if (!g_head.live || !g_headSnap.valid) return false;
        float posU[3]; QuatW rot; float halfM[3];
        HeadPose(posU, rot, halfM);
        const float halfU[3] = { halfM[0] * kHavokToSkyrim,
                                 halfM[1] * kHavokToSkyrim,
                                 halfM[2] * kHavokToSkyrim };
        const float* rr = g_headSnap.rot;                    // columns are the axes
        const float yAxis[3] = { rr[1], rr[4], rr[7] };      // local +Y in world
        // ⛔ THE CAPSULE MUST BE INSCRIBED IN THE BOX, not circumscribed around it. The first
        // version ran the segment the box's FULL half-depth and then added a radius on top, so
        // the published capsule reached ~4u past the face at ship defaults — the touch scan
        // would have reported contact from outside the collider, breaking this file's own law
        // that "a probe must never claim reach the collider does not have". Inscribed radius =
        // the SMALLER lateral half-extent (a mean can still exceed the thinner axis), and the
        // segment is shortened by that radius so the caps land ON the box faces, not past them.
        const float r = (std::min)(halfU[0], halfU[2]);
        const float halfLen = (std::max)(0.f, halfU[1] - r);
        for (int c = 0; c < 3; ++c) {
            aOutU[c] = posU[c] - yAxis[c] * halfLen;         // back of the skull
            bOutU[c] = posU[c] + yAxis[c] * halfLen;         // the face
        }
        *rOutU = r;
        return true;
    }

    // ★ MOUTH PROBE (2026-09-06, user in VR: "a smaller collision box at the bottom front of the head
    // collider … simulating that the player's mouth is colliding with the NPC"). Built as a PROBE on the
    // head box (the box is the collider): a short segment across the front face, mouthProbeOffZU below
    // the box centre (eye level under the rider), mouthProbeOutU proud of the face, radius mouthProbeR.
    // The touch scan names a mouth-led contact "mouth" (against "face" / "head"). Same gate as the head
    // probe: the head BODY must be live.
    bool MouthProbeSegment(float aOutU[3], float bOutU[3], float* rOutU)
    {
        if (!aOutU || !bOutU || !rOutU) return false;
        if (!ObjectHold::MouthProbeOn()) return false;
        if (!g_head.live || !g_headSnap.valid) return false;
        float posU[3]; QuatW rot; float halfM[3];
        HeadPose(posU, rot, halfM);
        const float halfXU = halfM[0] * kHavokToSkyrim;
        const float halfYU = halfM[1] * kHavokToSkyrim;
        const float halfZU = halfM[2] * kHavokToSkyrim;
        // The COLLIDER's axes. HeadPose poses the body on exactly g_headSnap.rot (no extra tilt), so
        // these columns ARE the box — its front face is the plane local y = +halfYU.
        const float* br = g_headSnap.rot;
        const float bx[3] = { br[0], br[3], br[6] };
        const float by[3] = { br[1], br[4], br[7] };
        const float bz[3] = { br[2], br[5], br[8] };
        const bool  full = ObjectHold::MouthProbeSource() == 1 && g_headSnap.fullRotValid;
        if (!full) {
            // mouthProbeSource 0, or +0x570 rejected this frame: the original 2026-09-06 pose, unchanged.
            const float s   = g_headSnap.scale;
            const float fwd = halfYU + ObjectHold::MouthProbeOutU() * s;
            const float up  = ObjectHold::MouthProbeOffZU() * s;
            const float lat = ObjectHold::MouthProbeOffXU() * s;
            const float hw  = ObjectHold::MouthProbeHalfWU() * s;
            for (int c = 0; c < 3; ++c) {
                const float m = posU[c] + by[c] * fwd + bz[c] * up + bx[c] * lat;
                aOutU[c] = m - bx[c] * hw;
                bOutU[c] = m + bx[c] * hw;
            }
            *rOutU = ObjectHold::MouthProbeR() * s;
            return true;
        }
        // ★★ KISS (2026-09-12) — mouthProbeSource 1: the headset STEERS the mouth, the COLLIDER owns the face.
        // ⛔ The first version of this (reviewed 2026-09-12, a MAJOR finding) pushed the probe out along the
        //    headset's PITCHED forward while the box it rides stays upright. The probe then left the
        //    collider: fully inside the box from ~15 deg of downward gaze, past the 2.2u kiss gate from ~33 deg
        //    (a kiss on a shorter NPC never fired), and 2.4-2.7u PROUD of the face at 20-30 deg of upward gaze
        //    (LIPS fired before any contact). A probe must never claim reach the collider does not have, and
        //    must never hide where the collider cannot reach.
        // So: build the anatomical mouth point on the full-rotation headset (+0x570), express it in the BOX's
        // axes, then pin it ONTO the box's front face and clamp it inside that face. The headset still
        // decides WHERE on the face the mouth is — looking down lowers it toward the chin edge, looking up
        // raises it, rolling your head slides it sideways and tilts the lip segment — but the probe can never
        // leave the surface that actually meets her. Verified numerically (scratch kiss_projection_sim.py):
        // centre on the face plane, the WHOLE capsule (end caps + radius) inside the face, segment in-plane,
        // for pitch -60..90 x roll -45..45; the old pose's numbers reproduce the reviewer's to 0.01u.
        // Offsets are REAL headset distances (no 3rd-person bone scale — it reads 1.000 on this rig anyway).
        // ⚠ Why this probe had never been seen firing is still UNPROVEN: the yaw-only rider is the leading
        // suspect, not an established cause. mouthProbeLog 1 at level / looking down / looking up, A/B'd
        // against mouthProbeSource 0, is what settles it.
        const float* fr = g_headSnap.fullRot;
        const float fx[3] = { fr[0], fr[3], fr[6] };
        const float fy[3] = { fr[1], fr[4], fr[7] };
        const float fz[3] = { fr[2], fr[5], fr[8] };
        const float out   = ObjectHold::MouthProbeOutU();
        const float hw    = ObjectHold::MouthProbeHalfWU();
        const float r     = ObjectHold::MouthProbeR();
        const float offZ  = ObjectHold::MouthProbeOffZU();
        const float offX  = ObjectHold::MouthProbeOffXU();
        const float lever = ObjectHold::MouthProbeLeverU();
        // ★ THE LEVER (round-2 review). The steering arm is ANATOMY — how far the mouth sits in FRONT of the eyes
        // (~2u: 2-3 cm at this rig's 76.5 u/m) — NOT the box's half-depth. Using halfYU (6u) as the arm raised the
        // probe to eye level by 30 deg of upward gaze, so a kiss on a TALLER NPC could miss. At level gaze the arm
        // contributes nothing (forward has no vertical part), so the user's dialled mouthProbeOffZU is unchanged.
        float d[3];                                              // anatomical mouth, relative to the box centre
        for (int c = 0; c < 3; ++c)
            d[c] = fy[c] * lever + fz[c] * offZ + fx[c] * offX;
        // lip segment direction FIRST: the clamps below need its spread. Headset lateral flattened into the face plane.
        const float fxDotBy = fx[0] * by[0] + fx[1] * by[1] + fx[2] * by[2];
        float sx[3] = { fx[0] - by[0] * fxDotBy, fx[1] - by[1] * fxDotBy, fx[2] - by[2] * fxDotBy };
        const float sn = std::sqrt(sx[0] * sx[0] + sx[1] * sx[1] + sx[2] * sx[2]);
        if (sn > 1e-4f) { sx[0] /= sn; sx[1] /= sn; sx[2] /= sn; }
        else            { sx[0] = bx[0]; sx[1] = bx[1]; sx[2] = bx[2]; }   // headset lateral ~ face normal
        float       lx = d[0] * bx[0] + d[1] * bx[1] + d[2] * bx[2];   // in the collider's frame...
        float       lz = d[0] * bz[0] + d[1] * bz[1] + d[2] * bz[2];
        const float ly = halfYU + out;                                 // ...pinned ONTO its front face
        // ⛔ round-2 review: the first clamp bounded only the CENTRE, so under roll the segment's END CAPS overhung the
        // box edge by ~1u. Bound the whole capsule: centre + half-width spread along that axis + the radius.
        const float sxZ = std::fabs(sx[0] * bz[0] + sx[1] * bz[1] + sx[2] * bz[2]);
        const float sxX = std::fabs(sx[0] * bx[0] + sx[1] * bx[1] + sx[2] * bx[2]);
        const float zLim = (std::max)(halfZU - r - hw * sxZ, 0.f);
        const float xLim = (std::max)(halfXU - r - hw * sxX, 0.f);
        lz = lz < -zLim ? -zLim : (lz > zLim ? zLim : lz);
        lx = lx < -xLim ? -xLim : (lx > xLim ? xLim : lx);
        for (int c = 0; c < 3; ++c) {
            const float m = posU[c] + bx[c] * lx + by[c] * ly + bz[c] * lz;
            aOutU[c] = m - sx[c] * hw;
            bOutU[c] = m + sx[c] * hw;
        }
        *rOutU = r;
        return true;
    }

    bool HeadForwardZ(float* uprightFwdZ, float* fullFwdZ, bool* posedOnFull)
    {
        if (uprightFwdZ) *uprightFwdZ = g_headSnap.fwdZUpright;
        if (fullFwdZ)    *fullFwdZ    = g_headSnap.fwdZFull;
        if (posedOnFull) *posedOnFull = ObjectHold::MouthProbeSource() == 1 && g_headSnap.fullRotValid;
        return g_headSnap.valid;
    }

    int WandProbeSegments(float aOutU[2][3], float bOutU[2][3], float* rOutU)
    {
        if (!aOutU || !bOutU || !rOutU) return 0;
        if (!g_wand.live || !g_wandSnap.valid) return 0;
        *rOutU = ObjectHold::PlayerWandR();
        int n = 0;
        for (int seg = 0; seg < kWandSegs; ++seg) {
            const float* a = g_wandSnap.p[seg];
            const float* b = g_wandSnap.p[seg + 1];
            float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (!(len > 1e-3f)) continue;              // degenerate link — NaN-safe form
            float tip[3] = { b[0], b[1], b[2] };
            if (seg == kWandSegs - 1) {                // same tip pad the body carries
                const float inv = kWandTipPadU / len;
                tip[0] += d[0] * inv; tip[1] += d[1] * inv; tip[2] += d[2] * inv;
            }
            aOutU[n][0] = a[0];   aOutU[n][1] = a[1];   aOutU[n][2] = a[2];
            bOutU[n][0] = tip[0]; bOutU[n][1] = tip[1]; bOutU[n][2] = tip[2];
            ++n;
        }
        return n;
    }

    void RegisterHiggs()
    {
        if (g_registered) return;
        auto* h = Interop::GetHiggs();
        if (!h) return;                                      // retried at kDataLoaded
        // NOTE: review risk 8 asked for a GetBuildNumber() hard gate before touching
        // vfunc 21 — the build→vtable mapping is unpublished, and PPB already
        // exercises the HIGHER vfunc 33 ungated (PerfSys, shipped), so we log the
        // build instead of guessing a refusal threshold.
        h->AddPrePhysicsStepCallback(&OnPrePhysicsStep);
        g_registered = true;
        logger::info("HBOX: HIGGS PrePhysicsStep callback registered (build {}) — inert until "
                     "handBoxEnable+handBoxArm or a higgsSlab* dial.", h->GetBuildNumber());
    }

    // ── GRAB-BUG CENSUS v2 (2026-08-17) ─────────────────────────────────────────────────────
    // v1 recorded only the LAST pair and, across 44,613 pairs, caught a benign one (our box x
    // HIGGS's own hand, which the belt Ignores by design). It did prove our boxes are consulted
    // during a grab. v2 targets the pair that actually matters: our box vs a BIPED-layer body
    // (8/32/33) = her ragdoll. Counters are split so one grab distinguishes three worlds:
    //   bipedSeen == 0        -> we are never asked; the pair dies upstream (broadphase/PLANCK)
    //   bipedIgnored > 0      -> PPB is the culprit, and 'branch' names which rule
    //   bipedContinue only    -> PPB passes it through; the Ignore is vanilla's or another mod's
    // PURE INTEGER, relaxed atomics, no logging (the movaps rule).
    std::atomic<std::uint32_t> g_gbA{ 0 }, g_gbB{ 0 };     // last (ourBox, biped) pair
    std::atomic<std::uint32_t> g_gbBranch{ 0 };            // 1 = fell through, 2 = belt-Ignore
    std::atomic<std::uint32_t> g_gbSeen{ 0 };              // biped pairs seen
    std::atomic<std::uint32_t> g_gbIgnored{ 0 };           // ...of those, WE returned Ignore
    std::atomic<std::uint32_t> g_gbOther{ 0 };             // non-biped pairs (context only)
    std::atomic<bool>          g_gbArmed{ false };
    // Layer histogram of the OTHER side, 128 bits. We must not depend on a GUESS about which
    // layer her arm capsule lives on: CapFix reshapes capsules on bodies it did not always
    // create, so "biped 8/32/33" is an inference, not a measurement. One grab now REPORTS the
    // layers our boxes actually met, and the counters stay meaningful either way.
    std::atomic<std::uint32_t> g_gbLayers[4] = {};
    // ── v3 (2026-08-17, after the first real run) ───────────────────────────────────────────
    // v2's single "last foreign pair" slot was SWAMPED: HIGGS fires grab-candidate casts on
    // layer 44 continuously while holding, so the slot always ended up holding a phantom
    // (0x0000002C) and never her arm — the one word the test exists to read. Dedicated slot,
    // biped layers only, sampled FIRST-per-grab (not last) so a late phantom cannot evict it.
    //
    // THE HYPOTHESIS THIS TESTS: our HandBox sits in the PLAYER's collision group with bit15
    // set. Skyrim's comparator treats same-group + bit15 on both sides as "same ragdoll -> do
    // not collide". If HIGGS re-groups the grabbed limb into the player's group for the
    // duration of the hold, her arm becomes ragdoll-adjacent to our box and VANILLA rejects the
    // pair — which matches every symptom: that limb only, other limbs fine, and PPB Ignoring
    // nothing. Reading her GROUP settles it: 01D9-like (== ourGroup) confirms, anything else
    // refutes.
    // ⚠ v3 also SKIPS our own dead hand. Reading the 2026-08-17 log against the census output
    // showed v2 captured `ourBox=0x01D9C438` — bit14 SET — in 5 of 6 grabs. That is the HOLDING
    // hand, which PPB itself switches off at :1239 while it holds something. Its pairs are
    // meaningless here: of course a collision-disabled box does not collide. The bug is on the
    // POKING hand (logged `0x…8438`, bit14 clear), so the census must ignore any of our boxes
    // carrying bit14 or it just re-measures our own intentional disable.
    std::atomic<std::uint32_t> g_gbBipOur{ 0 }, g_gbBipHer{ 0 };
    std::atomic<std::uint32_t> g_gbDeadHandSkipped{ 0 };
    std::atomic<std::uint32_t> g_gbBipBranch{ 0 };
    std::atomic<std::uint32_t> g_gbBipCount{ 0 };
    // NpcFinger runs BEFORE us in PerfSys::FilterCB, so anything it decides we never see.
    // Its counter (NpcFinger::g_gbFingerAte) tells "nobody asked us" apart from "answered first".

    void OnFrame()
    {
        // Deferred filter telemetry (FilterDecision must never log — collision thread).
        static bool s_repIgnore = false;
        if (!s_repIgnore && g_logFirstIgnore.load(std::memory_order_relaxed)) {
            s_repIgnore = true;
            logger::info("HBOX filter: box x HIGGS-own-body IGNORE pair seen (deferred report)");
        }

        // ── GRAB-BUG CENSUS: arm while the player is HIGGS-grabbing an actor, and report the
        // last pair our boxes saw against her bodies. Main thread — logging is legal here.
        {
            bool grabbing = false;
            if (auto* hg = Interop::GetHiggs())
                grabbing = (hg->IsHoldingObject(false) && hg->GetGrabbedObject(false)) ||
                           (hg->IsHoldingObject(true)  && hg->GetGrabbedObject(true));
            g_gbArmed.store(grabbing, std::memory_order_relaxed);
            static bool s_wasGrabbing = false;
            if (s_wasGrabbing && !grabbing) {          // report once, on RELEASE
                const std::uint32_t seen = g_gbSeen.exchange(0, std::memory_order_relaxed);
                const std::uint32_t ign  = g_gbIgnored.exchange(0, std::memory_order_relaxed);
                const std::uint32_t oth  = g_gbOther.exchange(0, std::memory_order_relaxed);
                const std::uint32_t a    = g_gbA.load(std::memory_order_relaxed);
                const std::uint32_t b    = g_gbB.load(std::memory_order_relaxed);
                const std::uint32_t br   = g_gbBranch.exchange(0, std::memory_order_relaxed);
                const std::uint32_t ate  = NpcFinger::g_gbFingerAte.exchange(0, std::memory_order_relaxed);
                std::string layers;
                for (unsigned L = 0; L < 128u; ++L) {
                    if (g_gbLayers[L >> 5].load(std::memory_order_relaxed) & (1u << (L & 31u))) {
                        if (!layers.empty()) layers += ',';
                        layers += std::to_string(L);
                    }
                }
                for (auto& w : g_gbLayers) w.store(0, std::memory_order_relaxed);
                const std::uint32_t bc  = g_gbBipCount.exchange(0, std::memory_order_relaxed);
                const std::uint32_t bo  = g_gbBipOur.exchange(0, std::memory_order_relaxed);
                const std::uint32_t bh  = g_gbBipHer.exchange(0, std::memory_order_relaxed);
                const std::uint32_t bbr  = g_gbBipBranch.exchange(0, std::memory_order_relaxed);
                const std::uint32_t dead = g_gbDeadHandSkipped.exchange(0, std::memory_order_relaxed);
                if (bc) {
                    logger::info("GRABBUG v3 BIPED: {} pairs vs her ragdoll (LIVE hand; {} dead-hand pairs "
                                 "skipped) | ourBox=0x{:08X} "
                                 "(grp {:04X} part {} bit15 {}) | herLimb=0x{:08X} (layer {} "
                                 "part {} grp {:04X} bit15 {}) | PPB {} | GROUPS {} -> {}",
                                 bc, dead, bo, bo >> 16, (bo >> 8) & 0x1Fu, (bo & 0x8000u) ? 1 : 0,
                                 bh, bh & 0x7Fu, (bh >> 8) & 0x1Fu, bh >> 16,
                                 (bh & 0x8000u) ? 1 : 0,
                                 bbr == 2 ? "IGNORED it" : "passed it through",
                                 (bo >> 16) == (bh >> 16) ? "MATCH" : "differ",
                                 ((bo >> 16) == (bh >> 16) && (bo & 0x8000u) && (bh & 0x8000u))
                                     ? "CONFIRMED: same group + bit15 both sides = vanilla treats "
                                       "them as ONE ragdoll and rejects the pair. Not our bug; fix "
                                       "by clearing bit15 on the HandBox or re-grouping it."
                                     : "REFUTED: the group/bit15 theory does not explain it — look "
                                       "at PLANCK's callback and the vanilla 56-vs-biped row.");
                } else {
                    logger::info("GRABBUG v3 BIPED: the LIVE hand's boxes met NO biped-layer body at "
                                 "all during this grab ({} dead-hand pairs skipped) -> her limb "
                                 "is not on 8/32/33, or the pair never reached any filter.", dead);
                }
                if (!seen) {
                    logger::info("GRABBUG v2: {} HIGGS-own pairs, but our boxes were NEVER asked "
                                 "about any FOREIGN body during the grab | layers seen: [{}] | "
                                 "NpcFinger decided {} pairs before us -> {}",
                                 oth, layers.empty() ? "none" : layers.c_str(), ate,
                                 ate ? "IT swallowed them; look at NpcFinger::FilterDecision"
                                     : "rejected UPSTREAM of PPB (broadphase, or an earlier callback)");
                } else {
                    logger::info("GRABBUG v2: foreign pairs={} of which WE Ignored={} (HIGGS-own={}) "
                                 "| layers seen: [{}] | NpcFinger ate {} "
                                 "| ourBox=0x{:08X} (layer {} part {} grp {:04X} bit15 {}) "
                                 "| herBody=0x{:08X} (layer {} part {} grp {:04X} bit15 {}) "
                                 "| last verdict: {}{}",
                                 seen, ign, oth, layers.empty() ? "none" : layers.c_str(), ate,
                                 a, a & 0x7Fu, (a >> 8) & 0x1Fu, a >> 16, (a & 0x8000u) ? 1 : 0,
                                 b, b & 0x7Fu, (b >> 8) & 0x1Fu, b >> 16, (b & 0x8000u) ? 1 : 0,
                                 br == 2 ? "PPB returned Ignore -- OURS" : "PPB passed it through",
                                 (br == 1 && ign == 0)
                                     ? " -> the Ignore is NOT ours (vanilla or another mod)" : "");
                }
            }
            s_wasGrabbing = grabbing;
        }

        // Frame counter ONLY (2026-07-10 lag fix): the bone snapshot + the invDt
        // capture moved into the FIRST PrePhysicsStep fire of each frame
        // (TakeBoneSnapshot) — this id edge is what marks the frame boundary for
        // that consumer, preserving the once-per-frame keyframe discipline (N1).
        ++g_snap.id;
    }

    int FilterDecision(std::uint32_t infoA, std::uint32_t infoB)
    {
        // gated on relaxed atomics — idle cost is a couple of loads + compares
        const bool boxLive  = g_boxLive.load(std::memory_order_relaxed);
        const bool wandLive = g_wandLive.load(std::memory_order_relaxed);
        const bool headLive = g_headLive.load(std::memory_order_relaxed);
        if (!boxLive && !wandLive && !headLive) return 0;
        const std::uint32_t grp  = g_boxGroup.load(std::memory_order_relaxed);     // OUR group
        const std::uint32_t pgrp = g_playerGroup.load(std::memory_order_relaxed);  // player's real group
        const unsigned      part = g_boxPart.load(std::memory_order_relaxed);
        const unsigned      wpart = g_wandPart.load(std::memory_order_relaxed);
        const unsigned      hpart = g_headPart.load(std::memory_order_relaxed);

        // "ours" = layer 56 + bit15 + OUR group. Under a private group this no longer overlaps
        // HIGGS's hands, which is exactly the point.
        const auto isOurs = [grp](std::uint32_t f) {
            return (f & 0x7Fu) == kHiggsLayer && (f & kBit15) != 0 && (f >> 16) == grp;
        };
        const auto isBox  = [&](std::uint32_t f) { return isOurs(f) && ((f >> 8) & 0x1Fu) == part; };
        const auto isWand = [&](std::uint32_t f) { return isOurs(f) && ((f >> 8) & 0x1Fu) == wpart; };
        const auto isHead = [&](std::uint32_t f) { return isOurs(f) && ((f >> 8) & 0x1Fu) == hpart; };

        const bool bA = isBox(infoA),  bB = isBox(infoB);
        const bool wA = isWand(infoA), wB = isWand(infoB);
        const bool hA = isHead(infoA), hB = isHead(infoB);
        if (!bA && !bB && !wA && !wB && !hA && !hB) return 0;   // existing PerfSys rules unchanged
        if (bA && bB) return 2;                           // box x box: never useful
        if (wA && wB) return 2;                           // wand seg x wand seg: adjacent segments

        // ── BELT 0 (head): every OTHER player-attached body. Unlike the wand, the head box has
        // NO useful self-contact: touching your own face with your own hand is two keyframed
        // (infinite-mass) bodies producing zero impulse, so the pair can only waste contact
        // points and muddy attribution. Adjacency covers |head 10 - wand 9| == 1 for free, but
        // ONLY while we share the player's group — under handBoxPrivGroup it is gone, so state
        // every exclusion explicitly. HIGGS's own hands/weapons are parts 2/3/5/6.
        if (hA != hB) {
            const std::uint32_t oh = hA ? infoB : infoA;
            if (isBox(oh) || isWand(oh)) return 2;        // our boxes / our wand
            const unsigned ohl = oh & 0x7Fu;
            const unsigned ohp = (oh >> 8) & 0x1Fu;
            if (ohl == kHiggsLayer && (oh & kBit15) && (oh >> 16) == pgrp &&
                (ohp == 2u || ohp == 3u || ohp == 5u || ohp == 6u))
                return 2;                                 // HIGGS's own hand / weapon clone
            if ((ohl == 8u || ohl == 32u || ohl == 33u) && (oh >> 16) == pgrp)
                return 2;                                 // the player's OWN ragdoll — never
            // Everything else falls through to Continue -> the vanilla layer-56 row, which
            // COLLIDES with Biped(8)/BipedNoCC(33)/DeadBip(32). That pair IS the feature: an
            // infinite-mass head box against her PLANCK-driven dynamic bones = she yields.
            // CharController(30) is absent from layer 56's bitfield, so neither her capsule nor
            // the player's can ever be touched here — by construction, not by a rule.
        }
        if (hA && hB) return 2;                           // (single body today; future-proof)

        // ── BELT 1 (wand): the player's OWN ragdoll. Adjacency used to do this for free
        // (|wandPart 9 - Lthigh 8| == 1 -> skip). Under a private group the pair becomes a plain
        // layer-56-vs-biped matrix COLLIDE, which would have the wand fighting the leg it hangs
        // beside. Key on the PLAYER's group so NPC bipeds are untouched (they must still collide).
        if (wA != wB) {
            const std::uint32_t ow = wA ? infoB : infoA;
            const unsigned      ol = ow & 0x7Fu;
            if ((ol == 8u || ol == 32u || ol == 33u) && (ow >> 16) == pgrp)
                return 2;                                 // our own body — never
            // wand vs HIGGS's own hand/weapon stays COLLIDE by design: that is the player
            // touching himself with his own hand, which is the whole point of the wand.
        }

        // ── BELT 2 (boxes): the player's OWN ragdoll, same reasoning as BELT 1.
        if (bA != bB) {
            const std::uint32_t ob = bA ? infoB : infoA;
            const unsigned      ol = ob & 0x7Fu;
            if ((ol == 8u || ol == 32u || ol == 33u) && (ob >> 16) == pgrp)
                return 2;
        }
        if (!bA && !bB) return 0;                         // wand-only pair: nothing below applies

        // Belt vs HIGGS's own bodies (hand 3/5, weapon clone 3/5, declared-but-dead
        // 2/6): the |4-3|=|4-5|=1 adjacency rule already skips them in the vanilla
        // comparator, but CompareFilterInfo @0xE2BA10 was never disassembled (spec
        // Risk 3) — Ignore here closes the misread case for free.
        const std::uint32_t o  = bA ? infoB : infoA;
        const unsigned      op = (o >> 8) & 0x1Fu;
        // ⚠ keyed on the PLAYER's group, not ours: once we take a private group we no longer
        // share one with HIGGS's hands, and the same-group adjacency that used to skip them
        // (|4-3| = |4-5| = 1) is gone — 56-vs-56 would COLLIDE through the matrix instead.
        if ((o & 0x7Fu) == kHiggsLayer && (o & kBit15) && (o >> 16) == pgrp &&
            (op == 2u || op == 3u || op == 5u || op == 6u)) {
            // ⚠ never log here — collision-thread callback, fmt SIMD = CTD (see NpcFingerTest)
            g_logFirstIgnore.store(true, std::memory_order_relaxed);
            if (g_gbArmed.load(std::memory_order_relaxed)) {
                const unsigned ol = o & 0x7Fu;
                g_gbLayers[ol >> 5].fetch_or(1u << (ol & 31u), std::memory_order_relaxed);
                if ((ol == 8u || ol == 32u || ol == 33u) &&
                    !((bA ? infoA : infoB) & kBit14)) {              // LIVE hand only
                    g_gbBipCount.fetch_add(1, std::memory_order_relaxed);
                    std::uint32_t expect = 0;
                    if (g_gbBipHer.compare_exchange_strong(expect, o, std::memory_order_relaxed)) {
                        g_gbBipOur.store(bA ? infoA : infoB, std::memory_order_relaxed);
                        g_gbBipBranch.store(2, std::memory_order_relaxed);
                    }
                }
                if (ol != kHiggsLayer) {                               // not HIGGS's own kit
                    g_gbA.store(bA ? infoA : infoB, std::memory_order_relaxed);
                    g_gbB.store(o, std::memory_order_relaxed);
                    g_gbBranch.store(2, std::memory_order_relaxed);    // WE Ignored it
                    g_gbSeen.fetch_add(1, std::memory_order_relaxed);
                    g_gbIgnored.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_gbOther.fetch_add(1, std::memory_order_relaxed);
                }
                if ((bA ? infoA : infoB) & kBit14)
                    g_gbDeadHandSkipped.fetch_add(1, std::memory_order_relaxed);
            }
            return 2;
        }
        if (g_gbArmed.load(std::memory_order_relaxed)) {
            const unsigned ol = o & 0x7Fu;
            g_gbLayers[ol >> 5].fetch_or(1u << (ol & 31u), std::memory_order_relaxed);
                if ((ol == 8u || ol == 32u || ol == 33u) &&
                !((bA ? infoA : infoB) & kBit14)) {                  // LIVE hand only
                g_gbBipCount.fetch_add(1, std::memory_order_relaxed);
                std::uint32_t expect = 0;
                if (g_gbBipHer.compare_exchange_strong(expect, o, std::memory_order_relaxed)) {
                    g_gbBipOur.store(bA ? infoA : infoB, std::memory_order_relaxed);
                    g_gbBipBranch.store(1, std::memory_order_relaxed);
                }
            }
            if (ol != kHiggsLayer) {                                   // not HIGGS's own kit
                g_gbA.store(bA ? infoA : infoB, std::memory_order_relaxed);
                g_gbB.store(o, std::memory_order_relaxed);
                g_gbBranch.store(1, std::memory_order_relaxed);        // we passed it through
                g_gbSeen.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_gbOther.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Everything else falls through to the vanilla table — layer 56's row gives
        // Biped(8)/BipedNoCC(33)/DeadBip(32) the payoff Collide and excludes
        // CharController(30); PLANCK's callback Continues for 56 x biped.
        return 0;
    }

    void ClearOnLoad()
    {
        // kPreLoadGame: the player's world is still alive — fresh-resolve it and let
        // DestroyHand's membership guard remove-or-forget per body (stored pointers
        // stay compare-only, round-1 R3).
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell   = player ? player->GetParentCell() : nullptr;
        void* fresh  = cell ? static_cast<void*>(cell->GetbhkWorld()) : nullptr;
        DestroyHand(0, fresh, "load");
        DestroyHand(1, fresh, "load");
        DestroyWand(fresh, "load");
        DestroyHead(fresh, "load");
        g_wandSnap = WandSnap{};
        g_headSnap = HeadSnap{};
        g_slabBase[0] = SlabBase{};    // HIGGS rebuilds its bodies across a load —
        g_slabBase[1] = SlabBase{};    // baselines recaptured on first sight of the new ones
        g_snap = Snapshot{};
        g_lastConsumedSnap = 0;
        g_eff[0] = EffFrame{}; g_eff[1] = EffFrame{};
        g_rel[0] = RelState{}; g_rel[1] = RelState{};   // relation re-learns on the new HIGGS bodies
        g_prevPlayerPosValid = false;      // never warp across a load's position jump
        g_logFirstIgnore.store(false, std::memory_order_relaxed);
    }
}
