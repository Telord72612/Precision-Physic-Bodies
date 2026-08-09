// ─────────────────────────────────────────────────────────────────────────
// PivFix.cpp — Precision Physic Bodies' joint-pivot machinery (ported
// verbatim from AIHands' PalmCollider.cpp PPB half, 2026-07-07 split).
//
// The visualizer's blue balls are constraint pivots stored TWICE (once per
// connected body frame). This TU re-seats them deterministically (PivFixApply),
// rides each shoulder's chest-side anchor on the live clavicle chain
// (PivFollowShoulder, R + L since 2026-07-10), and exposes the stored pivots
// to the capsule auto-fit (PivReadJointLocal).
//
// This is the ONLY TU that includes the Havok 2010.2 SDK (global-namespace
// hkp* types). CMake scopes the SDK include path to this file; the PCH stays
// active so RE::hkp* (namespaced) coexist with the SDK's unqualified hkp*.
// The header exposes only void*/POD, so the rest of the plugin stays on RE::.
// ─────────────────────────────────────────────────────────────────────────

#include "PivFix.h"
#include "Tuning.h"
#include <set>
#include <map>
#include <mutex>
#include "Interop.h"
#include "DismemberGuard.h"  // PlanckGet/SetSetting — the PivGuard flag bracket (2026-07-29)   // Interop::IsActorGrabbedByPlayer / HasHiggs — the grab gate (2026-07-07)

// PPBHook.cpp's poseConformDump window epoch (0 = window closed) — gates the CLAVDUMP diagnostic.
namespace ArmIK { std::uint32_t PoseConformDumpEpoch(); }
// CapFix's latched measured TRUE scale (0 = unlatched -> leave joints alone). Phase-2 joint re-scale.
namespace GrabDiag { float MeasuredScaleOf(RE::Actor* actor); bool ActorCarriesBake(RE::Actor* actor); }

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
#include <Physics/Dynamics/Constraint/Bilateral/Ragdoll/hkpRagdollConstraintData.h>          // PivFix: joint pivots
#include <Physics/Dynamics/Constraint/Bilateral/LimitedHinge/hkpLimitedHingeConstraintData.h> // PivFix: elbow may be a hinge
#include <Physics/Collide/Shape/Convex/hkpConvexShape.h>
#include <Physics/Collide/Agent/Collidable/hkpCollidable.h>
#include <Physics/Dynamics/Motion/hkpMotion.h>               // MOTION_* types
#include <Physics/Dynamics/World/hkpWorld.h>

// CommonLibVR (RE::MemoryManager, REL, logger) comes from the force-included
// PCH (src/PCH.h → RE/Skyrim.h).
#include <atomic>
#include <chrono>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <xmmintrin.h>

namespace logger = SKSE::log;

// ═════════════════════════════════════════════════════════════════════════
// GLOBAL link-time stubs for SDK symbols whose real definitions live in the
// engine binary (not in any .lib we link). These bridge SDK inline methods to
// the engine impl via REL::Relocation, or provide a symbol the linker needs.
// Ported verbatim from the old GrabConstraint.cpp (proven). hkReferencedObject
// add/removeReference back hkRefPtr<T>::operator= (used by hkConstraintCinfo).
// ═════════════════════════════════════════════════════════════════════════
namespace {
    using _relVoidSelf = void(*)(const void* self);
    inline _relVoidSelf RelAddRef()    { static REL::Relocation<_relVoidSelf> fn{ REL::Offset(0xA01280) }; return fn.get(); }
    inline _relVoidSelf RelRemoveRef() { static REL::Relocation<_relVoidSelf> fn{ REL::Offset(0xA01340) }; return fn.get(); }
    using _relActivate = void(*)(hkpEntity*);
    inline _relActivate RelActivate()  { static REL::Relocation<_relActivate> fn{ REL::Offset(0xAA7130) }; return fn.get(); }
}
void hkReferencedObject::addReference() const    { RelAddRef()(this); }
void hkReferencedObject::removeReference() const { RelRemoveRef()(this); }
const hkClass* hkReferencedObject::getClassType() const { return nullptr; }
void hkReferencedObject::calcContentStatistics(hkStatisticsCollector*, const hkClass*) const {}
void hkpEntity::activate() { RelActivate()(this); }
// hkMemoryRouter::s_memoryRouter — declared in the SDK header, defined in the
// engine binary; hkReferencedObject's operator-delete macro references it. We
// never allocate SDK objects in this DLL, so it's never invoked; the symbol
// only needs to exist for the linker.
hkThreadLocalData<hkMemoryRouter*> hkMemoryRouter::s_memoryRouter{};
// hkpConstraintData base virtuals — MSVC may emit a thunk referencing the base
// impl; provide no-op definitions.
hkpSolverResults* hkpConstraintData::getSolverResults(hkpConstraintRuntime*) { return nullptr; }
void hkpConstraintData::addInstance(hkpConstraintInstance*, hkpConstraintRuntime*, int) const {}

namespace {

    // ── Constants (unit conversions) ────────────────────────────────────────
    constexpr float kSkyrimToHavok       = 0.0142875f;
    constexpr float kHavokToSkyrim       = 1.0f / 0.0142875f;

    inline bool IsLikelyPointer(const void* p) {
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v >= 0x10000 && (v & 0x7) == 0 && (v >> 32) != 0;
    }

    // ── Ported bhk wrapper layout (HIGGS RE/havok.h; byte-validated) ────────
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
    // Deferred post-correction audits (2026-08-01): actor id -> wall-clock ms at which to dump
    // the PIVTRACK ball-vs-bone audit, so every re-scale reports its OUTCOME and not just its
    // command. See the SELF-VERIFY note at the PIVRESCALE apply.
    static std::mutex g_rsvMx;
    static std::map<std::uint32_t, std::uint64_t> g_rescaleVerifyAt;
    inline std::uint64_t RsvNowMs() {   // same clock as PivNowMs, usable this early in the file
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
}

namespace ObjectHold {

    inline hkpRigidBody* HkOf(void* bhkWrapper) {
        if (!bhkWrapper || !IsLikelyPointer(bhkWrapper)) return nullptr;
        return reinterpret_cast<PortBhkRigidBody*>(bhkWrapper)->hkBody;
    }

    // ── PIV FIX: live joint-pivot re-seat (wrist/elbow/shoulder, RIGHT side) ─────────────────────────
    // The visualizer's blue balls are constraint pivots stored TWICE — one transform per connected body
    // frame (hkpSetLocalTransformsConstraintAtom m_transformA/B). Editing only one copy pre-stresses the
    // joint, so every write recomputes BOTH from the live body transforms: the knob gives the pivot in
    // the CHILD bone's local frame; the other body's copy = worldOther⁻¹ × worldChild × knob. Position
    // only — the rotation columns (twist/plane axes) are never touched. Same float-write safety class as
    // CapFix (official Havok setInBodySpace performs exactly these writes).
    namespace {
        struct hkpEntityConstraintAccess : hkpEntity {
            using hkpEntity::m_constraintsMaster;   // protected → accessible (the Impl getters aren't linked)
            using hkpEntity::m_constraintsSlave;
        };

        RE::hkpRigidBody* PivFindBodyRE(RE::Actor* actor, const char* nodeName) {
            auto* root = actor->Get3D();
            if (!root) return nullptr;
            auto* obj = root->GetObjectByName(nodeName);
            auto* node = obj ? obj->AsNode() : nullptr;
            if (!node) return nullptr;
            auto* colObj = node->collisionObject.get();
            if (!colObj) return nullptr;
            auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
            return body ? body->GetRigidBody() : nullptr;
        }

        // The pivot-bearing constraint (ragdoll ball or limited hinge) on `child`. If `other` is non-null
        // the joint must connect child↔other; if `exclude` is non-null the joint must NOT connect to it
        // (the shoulder path: the upper arm's non-elbow joint, whatever its parent bone turns out to be).
        hkpConstraintInstance* PivFindJoint(hkpEntity* child, hkpEntity* other, hkpEntity* exclude = nullptr) {
            auto matches = [&](hkpConstraintInstance* ci) -> bool {
                if (!ci || !ci->getData()) return false;
                hkpEntity* a = ci->getEntityA(); hkpEntity* b = ci->getEntityB();
                if (a != child && b != child) return false;
                hkpEntity* partner = (a == child) ? b : a;
                if (!partner) return false;
                if (other && partner != other) return false;
                if (exclude && partner == exclude) return false;
                const int t = ci->getData()->getType();
                return t == hkpConstraintData::CONSTRAINT_TYPE_RAGDOLL ||
                       t == hkpConstraintData::CONSTRAINT_TYPE_LIMITEDHINGE;
            };
            auto* acc = static_cast<hkpEntityConstraintAccess*>(child);
            for (int i = 0; i < acc->m_constraintsMaster.getSize(); ++i)
                if (matches(acc->m_constraintsMaster[i].m_constraint)) return acc->m_constraintsMaster[i].m_constraint;
            for (int i = 0; i < acc->m_constraintsSlave.getSize(); ++i)
                if (matches(acc->m_constraintsSlave[i])) return acc->m_constraintsSlave[i];
            return nullptr;
        }

        hkpSetLocalTransformsConstraintAtom* PivTransformsAtom(hkpConstraintData* data) {
            switch (data->getType()) {
            case hkpConstraintData::CONSTRAINT_TYPE_RAGDOLL:
                return &static_cast<hkpRagdollConstraintData*>(data)->m_atoms.m_transforms;
            case hkpConstraintData::CONSTRAINT_TYPE_LIMITEDHINGE:
                return &static_cast<hkpLimitedHingeConstraintData*>(data)->m_atoms.m_transforms;
            default: return nullptr;
            }
        }

        // DETERMINISTIC re-seat (2026-07-04 rewrite — the contamination-trap fix). Both stored copies get
        // knob-derived TARGETS: child target = the knob; partner target = the knob mapped through the RUNTIME
        // BIND relation (referencePose math, supplied by the hook pipeline — pose-independent). Every 1 Hz tick
        // simply compares stored vs target and writes on mismatch. No live-pose capture anywhere: the old transfer
        // read worldOther⁻¹·worldChild, and since the constraint DEFINES the equilibrium pose, healing at the
        // pose the bad data created just re-encoded the bad data (the self-consistent trap, 07-04). Dials,
        // heals and retries are all the same operation now; converged joints are silent.
        struct PivState {
            std::chrono::steady_clock::time_point lastTick{};
            unsigned lastGen = 0;
            bool     discovered = false;
            int      parity = 0;
            int      calmTicks[3] = {};
            int      driftTicks[3] = {};
            float    lastOther[3][3] = {};
            bool     haveOther[3] = {};
            std::uintptr_t lastCi[11] = {};
            int      lastType[11] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
            float    autoChild[11][3] = {};   // last statue-measured seat (STICKY targets)
            bool     haveAuto[11] = {};
            // GRAB GATE (2026-07-07): HIGGS-grab hold + post-release grace, plus the per-joint
            // drift-heuristic fallback for HIGGS-less installs (a converged joint suddenly >3u
            // off-target at sub-scramble speed with the SAME constraint = something is holding it).
            std::chrono::steady_clock::time_point lastGrab{};
            bool     wasConverged[11] = {};
            int      grabDrift[11] = {};
            int      grabCalm[11] = {};
            bool     grabLike[11] = {};
            std::chrono::steady_clock::time_point grabLikeSince[11]{};
        };
        // File-scope (not function-local) so kPreLoadGame can drop it before the Havok world rebuild.
        std::unordered_map<std::uint32_t, PivState> s_piv;
    }

    void PivFixClearAll() {
        s_piv.clear();
    }

    void PivFixApply(RE::Actor* actor, const PivBindRel rel[11], bool isStatue) {
        const unsigned gen = CapFixGen();
        if (gen == 0 || !actor) return;

        const std::uint32_t id = actor->GetFormID();
        auto& st = s_piv[id];
        const auto now = std::chrono::steady_clock::now();
        if (st.lastTick.time_since_epoch().count() != 0 && now - st.lastTick < std::chrono::milliseconds(1000))
            return;
        st.lastTick = now;

        // ── GRAB GATE (2026-07-07, the in-VR bug fix): a player grab displaces limb bodies ~12u at
        // sub-scramble speeds — every existing guard missed it, so the 1 Hz heal snapped the pivots
        // back each second (bounce + runaway). While HIGGS reports the player holding THIS actor,
        // skip the heal AND the statue-autoseat measurement entirely; after release, hold off another
        // 1.5 s so PLANCK re-settles before the first heal. Sticky auto-seat targets stay untouched.
        if (PivGrabGateEnabled()) {
            if (Interop::IsActorGrabbedByPlayer(actor)) {
                st.lastGrab = now;
                logger::info("PIVFIX grab-gate HOLD {:08X} - player grab detected, pivot heal paused", id);
                return;
            }
            if (st.lastGrab.time_since_epoch().count() != 0 &&
                now - st.lastGrab < std::chrono::milliseconds(1500))
                return;   // post-release grace (silent)
        }

        const bool genChanged = (gen != st.lastGen);
        st.parity ^= 1;

        // joint = { child bone (the knob's reference frame), the bone on the other side of the joint }.
        // All partners EXPLICIT (the shoulder's chest-body partner was proven in the 07-04 sessions).
        static constexpr const char* kChild[11] = {
            "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]",
            "NPC Spine [Spn0]",  "NPC Spine1 [Spn1]",    "NPC Spine2 [Spn2]",
            "NPC Neck [Neck]",   "NPC Head [Head]",
            "NPC R Thigh [RThg]", "NPC R Calf [RClf]",   "NPC R Foot [Rft ]" };
        static constexpr const char* kOther[11] = {
            "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Spine2 [Spn2]",
            "NPC COM [COM ]",    "NPC Spine [Spn0]",     "NPC Spine1 [Spn1]",
            "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
            "NPC COM [COM ]",    "NPC R Thigh [RThg]",   "NPC R Calf [RClf]" };
        static constexpr const char* kName[11]  = {
            "wrist", "elbow", "shoulder", "spine0", "spine1", "spine2",
            "neck", "head", "hipR", "kneeR", "ankleR" };

        float strainU[11] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
        float pivotW[11][3];                               // child-copy pivot, world (havok m), for the ruler
        bool  havePivotW[11] = {};
        bool  anyEnabled = false;
        const bool autoSeat = (g_tune.pivAutoSeat > 0.5f) && isStatue;   // statue-gated: bind pose only
        auto* root3dSeat = actor->Get3D();

        for (int j = 0; j < 11; ++j) {
            auto* reChild = PivFindBodyRE(actor, kChild[j]);
            if (!reChild) continue;
            auto* child = reinterpret_cast<hkpEntity*>(reChild);
            auto* reOtherFind = PivFindBodyRE(actor, kOther[j]);
            if (!reOtherFind) {
                if (!st.discovered) logger::info("PivFix {:08X} {}: no body on {}", id, kName[j], kOther[j]);
                continue;
            }
            hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOtherFind));
            if (!ci) {
                if (!st.discovered)
                    logger::info("PivFix {:08X} {}: no ragdoll/hinge constraint found (child {})",
                                 id, kName[j], kChild[j]);
                continue;
            }
            // The partner body comes from the constraint itself (source of truth for all three joints).
            auto* reOtherEnt = (ci->getEntityA() == child) ? ci->getEntityB() : ci->getEntityA();
            if (!reOtherEnt) continue;
            auto* reOther = reinterpret_cast<RE::hkpRigidBody*>(reOtherEnt);
            auto* atom = PivTransformsAtom(ci->getDataRw());
            if (!atom) continue;
            const bool childIsA = (ci->getEntityA() == child);
            hkTransform& tChild = childIsA ? atom->m_transformA : atom->m_transformB;
            hkTransform& tOther = childIsA ? atom->m_transformB : atom->m_transformA;

            // Constraint-replacement visibility: the engine/PLANCK recreates instances (AI-warp rebuilds;
            // PLANCK converts hinges->ragdoll constraints at add per convertHingeConstraintsToRagdollConstraints).
            // A replaced instance carries factory pivots — the heal below fixes it; this line names the event.
            const int ctype = ci->getData()->getType();
            const auto ciPtr = reinterpret_cast<std::uintptr_t>(ci);
            // Same instance as last tick? A REPLACED instance (factory reset) must heal immediately —
            // the grab-like heuristic below only ever defers when the constraint is UNCHANGED.
            const bool ciSame = (st.lastCi[j] == ciPtr && st.lastType[j] == ctype);
            if (st.discovered && st.lastCi[j] != 0 && (st.lastCi[j] != ciPtr || st.lastType[j] != ctype))
                logger::info("PivFix {:08X} {} constraint REPLACED (0x{:X} type {} -> 0x{:X} type {}) - fresh factory pivots expected",
                             id, kName[j], st.lastCi[j], st.lastType[j], ciPtr, ctype);
            st.lastCi[j] = ciPtr; st.lastType[j] = ctype;

            const hkVector4 curLocal = tChild.getTranslation();
            if (genChanged || !st.discovered)
                logger::info("PivFix {:08X} gen={} {} ({}) BEFORE child-frame pivot=[{:.2f} {:.2f} {:.2f}] (game u)",
                             id, gen, kName[j],
                             ci->getData()->getType() == hkpConstraintData::CONSTRAINT_TYPE_RAGDOLL ? "ragdoll" : "hinge",
                             curLocal(0) * kHavokToSkyrim, curLocal(1) * kHavokToSkyrim, curLocal(2) * kHavokToSkyrim);

            auto* rbChild = reinterpret_cast<hkpRigidBody*>(reChild);
            auto* rbOther = reinterpret_cast<hkpRigidBody*>(reOther);
            const hkTransform& TC = rbChild->getTransform();
            const hkTransform& TO = rbOther->getTransform();
            const hkRotation& RC = TC.getRotation();
            const hkRotation& RO = TO.getRotation();

            // THE STRAIN METER: map each stored copy to world through its own body; a healthy joint reads ~0.
            // world = R·local + t (setTransformedPos isn't header-inline in this SDK drop).
            const hkVector4 oLocal = tOther.getTranslation();
            const float cwx = RC(0,0)*curLocal(0) + RC(0,1)*curLocal(1) + RC(0,2)*curLocal(2) + TC.getTranslation()(0);
            const float cwy = RC(1,0)*curLocal(0) + RC(1,1)*curLocal(1) + RC(1,2)*curLocal(2) + TC.getTranslation()(1);
            const float cwz = RC(2,0)*curLocal(0) + RC(2,1)*curLocal(1) + RC(2,2)*curLocal(2) + TC.getTranslation()(2);
            const float owx = RO(0,0)*oLocal(0) + RO(0,1)*oLocal(1) + RO(0,2)*oLocal(2) + TO.getTranslation()(0);
            const float owy = RO(1,0)*oLocal(0) + RO(1,1)*oLocal(1) + RO(1,2)*oLocal(2) + TO.getTranslation()(1);
            const float owz = RO(2,0)*oLocal(0) + RO(2,1)*oLocal(1) + RO(2,2)*oLocal(2) + TO.getTranslation()(2);
            strainU[j] = std::sqrt((cwx-owx)*(cwx-owx) + (cwy-owy)*(cwy-owy) + (cwz-owz)*(cwz-owz)) * kHavokToSkyrim;
            pivotW[j][0] = cwx; pivotW[j][1] = cwy; pivotW[j][2] = cwz;
            havePivotW[j] = true;

            float p[3];
            bool slotOn = PivFixSlot(j, p);
            // AUTO-SEAT: override the knob with the LIVE bone origin expressed in the child body frame.
            // Statue = bind pose = motors hold bodies at animation targets, so the node is the rig truth;
            // each tick's write shrinks ball-vs-bone until PIVTRACK reads ~0 (deterministic convergence).
            if (autoSeat && root3dSeat) {
                if (auto* nodeObj = root3dSeat->GetObjectByName(kChild[j])) {
                    const auto& nw = nodeObj->world.translate;
                    const float dxs = nw.x * kSkyrimToHavok - TC.getTranslation()(0);
                    const float dys = nw.y * kSkyrimToHavok - TC.getTranslation()(1);
                    const float dzs = nw.z * kSkyrimToHavok - TC.getTranslation()(2);
                    p[0] = (RC(0,0)*dxs + RC(1,0)*dys + RC(2,0)*dzs) * kHavokToSkyrim;
                    p[1] = (RC(0,1)*dxs + RC(1,1)*dys + RC(2,1)*dzs) * kHavokToSkyrim;
                    p[2] = (RC(0,2)*dxs + RC(1,2)*dys + RC(2,2)*dzs) * kHavokToSkyrim;
                    st.autoChild[j][0] = p[0]; st.autoChild[j][1] = p[1]; st.autoChild[j][2] = p[2];
                    st.haveAuto[j] = true;    // STICKY: this measurement outlives the statue
                    slotOn = true;
                }
            } else if (g_tune.pivAutoSeat > 0.5f && st.haveAuto[j]) {
                // Outside the statue the last MEASURED seat is the target — the hand-dialed knobs are
                // demoted to cold-start fallbacks (the 07-05 bug: dropping the statue restored the old
                // eyeball values over the better numeric ones).
                p[0] = st.autoChild[j][0]; p[1] = st.autoChild[j][1]; p[2] = st.autoChild[j][2];
                slotOn = true;
            }
            // NEW (PPB): per-joint WORLD +Z lift — applied to the SELECTED child target (manual knob and
            // sticky auto-seat alike). The world offset [0,0,upU] (game units) is expressed in the child
            // body frame via the body's live world rotation: p += R_bodyᵀ·[0,0,upU]. The partner copy
            // inherits automatically through the bind-relation mapping below.
            const float upU = PivFixUpU(j);
            if (slotOn && upU != 0.f) {
                p[0] += RC(2,0) * upU;
                p[1] += RC(2,1) * upU;
                p[2] += RC(2,2) * upU;
            }
            if (!slotOn) continue;
            anyEnabled = true;

            // Calm tracking: fast bodies or a wild copy-disagreement = transient pose (AI-warp ragdoll
            // rebuild, load-in snap). Writes during those bake warped joint pairs = the jitter poison.
            if (!rel[j].valid) {
                if (st.parity == 0 && !st.discovered)
                    logger::info("PivFix {:08X} {}: no runtime bind relation yet - skipped", id, kName[j]);
                continue;
            }

            // TARGETS (game units). Partner = R_rel·p + t_rel — the bind relation, never the live pose.
            const float tOx = rel[j].R[0]*p[0] + rel[j].R[1]*p[1] + rel[j].R[2]*p[2] + rel[j].t[0];
            const float tOy = rel[j].R[3]*p[0] + rel[j].R[4]*p[1] + rel[j].R[5]*p[2] + rel[j].t[1];
            const float tOz = rel[j].R[6]*p[0] + rel[j].R[7]*p[1] + rel[j].R[8]*p[2] + rel[j].t[2];

            // Stored-vs-target: converged joints are silent; any mismatch (dial, factory reset, PLANCK
            // constraint swap) gets written as soon as the bodies aren't outright flailing. The 40 u/s
            // guard is only about not touching constraints mid-ragdoll-scramble — correctness no longer
            // depends on pose, so there is no blackout, no dial gate, no chase, no HOLD.
            const float lx = p[0] * kSkyrimToHavok, ly = p[1] * kSkyrimToHavok, lz = p[2] * kSkyrimToHavok;
            const hkVector4 oStored = tOther.getTranslation();
            const float childOffU = std::sqrt((curLocal(0)-lx)*(curLocal(0)-lx) + (curLocal(1)-ly)*(curLocal(1)-ly) +
                                              (curLocal(2)-lz)*(curLocal(2)-lz)) * kHavokToSkyrim;
            const float oxT = tOx * kSkyrimToHavok, oyT = tOy * kSkyrimToHavok, ozT = tOz * kSkyrimToHavok;
            float otherOffU = std::sqrt((oStored(0)-oxT)*(oStored(0)-oxT) + (oStored(1)-oyT)*(oStored(1)-oyT) +
                                        (oStored(2)-ozT)*(oStored(2)-ozT)) * kHavokToSkyrim;
            // Shoulder under clavicle-follow: the partner anchor is owned by the per-frame follower —
            // the 1 Hz bind-math value would fight it (two writers, two targets). Child side only here.
            const bool followOther = (j == 2 && g_tune.pivClavFollow > 0.5f);
            if (followOther) otherOffU = 0.f;
            if (childOffU <= 0.05f && otherOffU <= 0.05f) {           // converged — silent
                st.wasConverged[j] = true;
                st.grabDrift[j] = 0;
                if (st.grabLike[j]) { st.grabLike[j] = false; st.grabCalm[j] = 0; }
                continue;
            }

            const auto& lvC = reChild->motion.linearVelocity.quad;
            const auto& lvO = reOther->motion.linearVelocity.quad;
            const float spdC = std::sqrt(lvC.m128_f32[0]*lvC.m128_f32[0] + lvC.m128_f32[1]*lvC.m128_f32[1] + lvC.m128_f32[2]*lvC.m128_f32[2]) * kHavokToSkyrim;
            const float spdO = std::sqrt(lvO.m128_f32[0]*lvO.m128_f32[0] + lvO.m128_f32[1]*lvO.m128_f32[1] + lvO.m128_f32[2]*lvO.m128_f32[2]) * kHavokToSkyrim;

            // ── GRAB-LIKE fallback heuristic (2026-07-07; HIGGS ABSENT only — the HIGGS poll above is
            // authoritative when present). Classify "something is physically holding this limb": the
            // SAME constraint instance (a replaced one = factory reset, heal immediately), the joint
            // WAS converged, and now childOff is suddenly >3u at sub-scramble speed for ≥2 consecutive
            // ticks. A dial does the same to childOff but bumps the gen — genChanged ticks are exempt.
            // While classified: defer writes. Resume when childOff calms <1u for 2 ticks, or after a
            // 10 s hard cap (never wedge the heal forever on a false positive).
            if (PivGrabGateEnabled() && !Interop::HasHiggs() && !genChanged) {
                if (st.grabLike[j]) {
                    if (childOffU < 1.f) {
                        if (++st.grabCalm[j] >= 2) { st.grabLike[j] = false; st.grabDrift[j] = 0; st.grabCalm[j] = 0; }
                    } else {
                        st.grabCalm[j] = 0;
                    }
                    if (st.grabLike[j]) {
                        if (now - st.grabLikeSince[j] < std::chrono::seconds(10)) {
                            logger::info("PivFix {:08X} {} GRAB-LIKE DEFER (childOff {:.2f}u)", id, kName[j], childOffU);
                            continue;
                        }
                        st.grabLike[j] = false;   // hard cap hit — resume healing
                        st.grabDrift[j] = 0;
                    }
                } else if (ciSame && st.wasConverged[j] && childOffU > 3.f && spdC < 40.f && spdO < 40.f) {
                    // Defer while classifying too — a heal write would reset childOff and blind the count.
                    if (++st.grabDrift[j] >= 2) {
                        st.grabLike[j] = true; st.grabLikeSince[j] = now; st.grabCalm[j] = 0;
                        logger::info("PIVFIX grab-gate HOLD {:08X} {} (heuristic: converged joint displaced {:.1f}u at grab speed)",
                                     id, kName[j], childOffU);
                    }
                    continue;
                } else {
                    st.grabDrift[j] = 0;
                }
            }

            if (spdC > 40.f || spdO > 40.f) {
                logger::info("PivFix {:08X} {} write DEFERRED (speeds {:.0f}/{:.0f} u/s) - mid-scramble, retrying",
                             id, kName[j], spdC, spdO);
                continue;
            }

            hkVector4 newLocal;   newLocal.set(lx, ly, lz);
            hkVector4 otherLocal; otherLocal.set(oxT, oyT, ozT);
            tChild.setTranslation(newLocal);
            if (!followOther) tOther.setTranslation(otherLocal);
            st.wasConverged[j] = false;   // wrote this tick — convergence re-detected next tick

            // Bake-ready record: BOTH stored copies at full precision, exactly as the NIF wants them.
            logger::info("PIVBAKE {:08X} {} {} childIsA={} child=[{:.6f} {:.6f} {:.6f}] other=[{:.6f} {:.6f} {:.6f}] (havok m)",
                         id, kName[j],
                         ci->getData()->getType() == hkpConstraintData::CONSTRAINT_TYPE_RAGDOLL ? "ragdoll" : "hinge",
                         childIsA ? 1 : 0, lx, ly, lz, oxT, oyT, ozT);
            char upBuf[24] = "";
            if (upU != 0.f) std::snprintf(upBuf, sizeof(upBuf), " upU=%.1f", upU);
            logger::info("PivFix {:08X} {} {} child=[{:.1f} {:.1f} {:.1f}] (childOff {:.2f}u otherOff {:.2f}u){}",
                         id, genChanged ? "APPLIED" : "HEAL", kName[j], p[0], p[1], p[2], childOffU, otherOffU, upBuf);
        }

        // Meter + ruler line, every 2s while dialing. True XP32 bind spacings (measured from the live
        // skeleton NIF 2026-07-03): forearm=16.05u (wrist-elbow), upper arm=22.83u (elbow-shoulder).
        if (anyEnabled && st.parity == 0) {
            float dWE = -1.f, dES = -1.f;
            if (havePivotW[0] && havePivotW[1]) {
                const float a = pivotW[0][0]-pivotW[1][0], b = pivotW[0][1]-pivotW[1][1], c = pivotW[0][2]-pivotW[1][2];
                dWE = std::sqrt(a*a + b*b + c*c) * kHavokToSkyrim;
            }
            if (havePivotW[1] && havePivotW[2]) {
                const float a = pivotW[1][0]-pivotW[2][0], b = pivotW[1][1]-pivotW[2][1], c = pivotW[1][2]-pivotW[2][2];
                dES = std::sqrt(a*a + b*b + c*c) * kHavokToSkyrim;
            }
            logger::info("PivFix STRAIN {:08X} arm W={:.2f} E={:.2f} S={:.2f} | spacing W-E={:.1f}u(true 16.0) E-S={:.1f}u(true 22.8)",
                         id, strainU[0], strainU[1], strainU[2], dWE, dES);
            logger::info("PivFix STRAIN {:08X} core s0={:.2f} s1={:.2f} s2={:.2f} neck={:.2f} head={:.2f} | legR hip={:.2f} knee={:.2f} ankle={:.2f}",
                         id, strainU[3], strainU[4], strainU[5], strainU[6], strainU[7],
                         strainU[8], strainU[9], strainU[10]);

        // PIVTRACK (2026-07-05): the Havok ball vs the XP32 bone origin, per joint, LIVE — the
            // instrument for "the ball left the flesh when she bent her arm". Ball = child-copy world
            // (pivotW); bone = the child node's rendered world position. delta ~0 in the statue and
            // spiking in bent poses WITH low strain = the body is displaced (limits/motors), not the data.
            if (auto* root3d = actor->Get3D()) {
                float trk[11];
                for (int j = 0; j < 11; ++j) {
                    trk[j] = -1.f;
                    if (!havePivotW[j]) continue;
                    auto* nodeObj = root3d->GetObjectByName(kChild[j]);
                    if (!nodeObj) continue;
                    const auto& bw = nodeObj->world.translate;
                    const float bx = pivotW[j][0] * kHavokToSkyrim - bw.x;
                    const float by = pivotW[j][1] * kHavokToSkyrim - bw.y;
                    const float bz = pivotW[j][2] * kHavokToSkyrim - bw.z;
                    trk[j] = std::sqrt(bx*bx + by*by + bz*bz);
                }
                logger::info("PIVTRACK {:08X} ball-vs-bone(u): W={:.1f} E={:.1f} S={:.1f} s0={:.1f} s1={:.1f} s2={:.1f} N={:.1f} H={:.1f} hip={:.1f} knee={:.1f} ankle={:.1f}",
                             id, trk[0], trk[1], trk[2], trk[3], trk[4], trk[5], trk[6], trk[7], trk[8], trk[9], trk[10]);
            }
        }
        st.lastGen = gen;
        st.discovered = true;
    }

    bool PivReadJointLocal(RE::Actor* actor, int joint, bool childSide, float outU[3]) {
        static constexpr const char* kChildJ[11] = {
            "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]",
            "NPC Spine [Spn0]",  "NPC Spine1 [Spn1]",    "NPC Spine2 [Spn2]",
            "NPC Neck [Neck]",   "NPC Head [Head]",
            "NPC R Thigh [RThg]", "NPC R Calf [RClf]",   "NPC R Foot [Rft ]" };
        static constexpr const char* kOtherJ[11] = {
            "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Spine2 [Spn2]",
            "NPC COM [COM ]",    "NPC Spine [Spn0]",     "NPC Spine1 [Spn1]",
            "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
            "NPC COM [COM ]",    "NPC R Thigh [RThg]",   "NPC R Calf [RClf]" };
        if (!actor || joint < 0 || joint > 10) return false;
        auto* reChild = PivFindBodyRE(actor, kChildJ[joint]);
        auto* reOther = PivFindBodyRE(actor, kOtherJ[joint]);
        if (!reChild || !reOther) return false;
        auto* ci = PivFindJoint(reinterpret_cast<hkpEntity*>(reChild), reinterpret_cast<hkpEntity*>(reOther));
        if (!ci) return false;
        auto* atom = PivTransformsAtom(ci->getDataRw());
        if (!atom) return false;
        const bool childIsA = (ci->getEntityA() == reinterpret_cast<hkpEntity*>(reChild));
        const hkTransform& t = (childSide == childIsA) ? atom->m_transformA : atom->m_transformB;
        const hkVector4 v = t.getTranslation();
        outU[0] = v(0) * kHavokToSkyrim; outU[1] = v(1) * kHavokToSkyrim; outU[2] = v(2) * kHavokToSkyrim;
        return true;
    }

    // ── CLAVICLE-FOLLOW: per-frame chest-side shoulder anchor, either side (see PivFix.h) ──
    void PivFollowShoulder(RE::Actor* actor, bool left, const char* armParentNode, const float refR[9], const float refT[3]) {
        if (!actor || g_tune.pivClavFollow <= 0.5f) return;
        // pivClavFollow gates ALONE (2026-07-10 fix). The old extra gate (pivShoulderEnable ||
        // pivAutoSeat) silently KILLED the follower when the ship bake retired PivFix (all enables
        // + autoseat -> 0): the "unbakeable, must-stay" feature never ran again, so each arm hung
        // from its STATIC baked anchor — the whole-chain ~3-4.5u zero-strain arm offset.
        auto* root = actor->Get3D();
        if (!root) return;
        auto* parentObj = root->GetObjectByName(armParentNode);
        if (!parentObj) return;
        auto* reChild = PivFindBodyRE(actor, left ? "NPC L UpperArm [LUar]" : "NPC R UpperArm [RUar]");
        auto* reOther = PivFindBodyRE(actor, "NPC Spine2 [Spn2]");
        if (!reChild || !reOther) return;
        auto* ci = PivFindJoint(reinterpret_cast<hkpEntity*>(reChild), reinterpret_cast<hkpEntity*>(reOther));
        if (!ci) return;
        auto* atom = PivTransformsAtom(ci->getDataRw());
        if (!atom) return;
        const bool childIsA = (ci->getEntityA() == reinterpret_cast<hkpEntity*>(reChild));
        hkTransform& tChild = childIsA ? atom->m_transformA : atom->m_transformB;
        hkTransform& tOther = childIsA ? atom->m_transformB : atom->m_transformA;

        // Stored child pivot (the seated ball) in game units — the point the anchor must meet.
        const hkVector4 cl = tChild.getTranslation();
        const float cu[3] = { cl(0) * kHavokToSkyrim, cl(1) * kHavokToSkyrim, cl(2) * kHavokToSkyrim };
        // ANIMATION-SIDE world position of that point: parentNode.world ∘ (refLocal ∘ childPivot).
        // The parent (clavicle chain) carries no ragdoll body — a pose-pure carrier, no feedback trap.
        const float lx = refR[0]*cu[0] + refR[1]*cu[1] + refR[2]*cu[2] + refT[0];
        const float ly = refR[3]*cu[0] + refR[4]*cu[1] + refR[5]*cu[2] + refT[1];
        const float lz = refR[6]*cu[0] + refR[7]*cu[1] + refR[8]*cu[2] + refT[2];
        const auto& w = parentObj->world;
        const RE::NiPoint3 local{ lx, ly, lz };
        const RE::NiPoint3 P = w.translate + (w.rotate * local) * w.scale;
        // Write the chest-side anchor so it lands on P given where the chest body is RIGHT NOW.
        auto* rbOther = reinterpret_cast<hkpRigidBody*>(reOther);
        const hkTransform& TO = rbOther->getTransform();
        const hkRotation&  RO = TO.getRotation();
        const float px = P.x * kSkyrimToHavok - TO.getTranslation()(0);
        const float py = P.y * kSkyrimToHavok - TO.getTranslation()(1);
        const float pz = P.z * kSkyrimToHavok - TO.getTranslation()(2);
        hkVector4 anchor;
        anchor.set(RO(0,0)*px + RO(1,0)*py + RO(2,0)*pz,
                   RO(0,1)*px + RO(1,1)*py + RO(2,1)*pz,
                   RO(0,2)*px + RO(1,2)*py + RO(2,2)*pz);
        tOther.setTranslation(anchor);
        static std::atomic<bool> s_followFirst[2]{};   // [0]=R, [1]=L — one FIRST-APPLY line per side
        if (!s_followFirst[left ? 1 : 0].exchange(true, std::memory_order_relaxed))
            logger::info("PivFollowShoulder FIRST APPLY ({}): anchor riding '{}' (clavicle-chain carrier)",
                         left ? "L" : "R", armParentNode);
        // Dump-window diagnostic (rides the poseConformDump edge): where the follower puts the
        // anchor vs where the live upper-arm bone actually is — once per window per side.
        if (const std::uint32_t ep = ArmIK::PoseConformDumpEpoch(); ep) {
            static std::uint32_t s_dumpEp[2] = {};
            if (s_dumpEp[left ? 1 : 0] != ep) {
                s_dumpEp[left ? 1 : 0] = ep;
                RE::NiPoint3 uaW{};
                if (auto* uaObj = root->GetObjectByName(left ? "NPC L UpperArm [LUar]" : "NPC R UpperArm [RUar]"))
                    uaW = uaObj->world.translate;
                const float dx = P.x - uaW.x, dy = P.y - uaW.y, dz = P.z - uaW.z;
                logger::info("PCONF CLAVDUMP {}: P=[{:.2f} {:.2f} {:.2f}] uaBoneW=[{:.2f} {:.2f} {:.2f}] "
                             "d=[{:.2f} {:.2f} {:.2f}] |d|={:.2f} carrier='{}' cu=[{:.2f} {:.2f} {:.2f}] (game u)",
                             left ? "L" : "R", P.x, P.y, P.z, uaW.x, uaW.y, uaW.z,
                             dx, dy, dz, std::sqrt(dx*dx + dy*dy + dz*dz), armParentNode, cu[0], cu[1], cu[2]);
            }
        }
    }

    // The held object's Havok body position in SKYRIM units (the 3-point heel tracker's ragdoll read).
    bool BodyPosU(void* wrapper, float out[3]) {
        hkpRigidBody* hk = HkOf(wrapper);
        if (!hk) return false;
        alignas(16) float t[4]; _mm_store_ps(t, hk->getTransform().getTranslation().getQuad());
        out[0] = t[0] * kHavokToSkyrim; out[1] = t[1] * kHavokToSkyrim; out[2] = t[2] * kHavokToSkyrim;
        return true;
    }

    // ── PIVOT DESCALE (2026-07-09 — the probe-proven engine-scaling fix) ─────────────────────────
    // The engine multiplies every NIF constraint pivot by the actor's GetScale at load (probe 23:00:
    // all 22 loaded values = baked × 0.9514 to four digits on scale-0.9514 Carmella), but the ragdoll
    // wants the UNSCALED values (auto-seat's raw targets are what fits — user-verified in VR). Stock
    // skeletons never showed this in 14 years: their child pivots are all (0,0,0) (scale-immune) and
    // PLANCK's loosenRagdollConstraintPivots band-aided the parent side. This pass undoes the engine
    // scaling ONCE per constraint INSTANCE — instance-pointer tracked, so PLANCK's hinge->ragdoll
    // replacement and AI-warp ragdoll rebuilds re-fire it naturally (their rebuilt instances carry
    // freshly-scaled values again). Covers ALL 17 joints (left side included — the side no healer
    // ever reached), any actor scale, no statue required. Same float-write class as the 1 Hz heal.
    namespace {
        constexpr const char* kDChild[17] = {
            "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]",
            "NPC Spine [Spn0]",  "NPC Spine1 [Spn1]",    "NPC Spine2 [Spn2]",
            "NPC Neck [Neck]",   "NPC Head [Head]",
            "NPC R Thigh [RThg]", "NPC R Calf [RClf]",   "NPC R Foot [Rft ]",
            "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]",
            "NPC L Thigh [LThg]", "NPC L Calf [LClf]",   "NPC L Foot [Lft ]" };
        constexpr const char* kDOther[17] = {
            "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Spine2 [Spn2]",
            "NPC COM [COM ]",    "NPC Spine [Spn0]",     "NPC Spine1 [Spn1]",
            "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
            "NPC COM [COM ]",    "NPC R Thigh [RThg]",   "NPC R Calf [RClf]",
            "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]", "NPC Spine2 [Spn2]",
            "NPC COM [COM ]",    "NPC L Thigh [LThg]",   "NPC L Calf [LClf]" };
        constexpr const char* kDName[17] = {           // shared by PivJointTrackDump (the `jtrack` audit)
            "wrist", "elbow", "shoulder", "spine0", "spine1", "spine2",
            "neck", "head", "hipR", "kneeR", "ankleR",
            "wristL", "elbowL", "shoulderL", "hipL", "kneeL", "ankleL" };
        std::unordered_map<std::uint32_t, std::array<hkpConstraintInstance*, 17>> g_descaled;
    }


    // ══ PIVGUARD (2026-07-29 v2 — the cycle-aware per-actor loosen split) ═══════════════════
    // GOAL: PLANCK's loosenRagdollConstraintPivots stays 1 GLOBALLY (the band-aid every
    // non-PPB skeleton needs), but PPB-skeleton actors run their drives under 0 — their
    // baked joints must never be collapsed to the anim pose (user-measured: ankle +6u/-3u,
    // shoulders 2u inward under the collapse).
    // v1 failed because PLANCK's restore is gated on the SAME flag: one collapsed frame +
    // scoped 0 forever = pivots stranded. v2 makes stranding impossible by OWNING the truth:
    //  - capture the 17 constraint pivot pairs per RAGDOLL INSTANCE on first sight (a fresh
    //    instance is baked-true by construction — the engine builds it from our NIF, and the
    //    collapse can only happen inside drives, all of which pass through our bracket first);
    //  - verify at ~2 Hz; any pivot that deviates gets the captured value written back (both
    //    copies, the dual-write discipline). A stray collapse survives <500 ms, once.
    //  - our own pivot writers (PIVRESCALE / descale) INVALIDATE the capture so the guard
    //    never fights them; shoulders are excluded entirely (clavicle-follow owns them live).
    namespace {
        struct PivCap {
            const void* ragdoll = nullptr;
            bool  have[17] = {};
            float a[17][4] = {}, b[17][4] = {};
            std::uint64_t lastCheckMs = 0;
            bool  announced = false;
        };
        std::unordered_map<std::uint32_t, PivCap> g_pivCap;
        std::mutex g_pivCapMx;

        bool PivIsOnPPBSkeleton(RE::Actor* actor)
        {
            auto* base = actor ? actor->GetActorBase() : nullptr;
            auto* race = base ? base->GetRace() : nullptr;
            if (!race) return false;
            const auto  sex = base->IsFemale() ? RE::SEXES::kFemale : RE::SEXES::kMale;
            const char* mdl = race->skeletonModels[sex].GetModel();
            if (!mdl) return false;
            return std::strstr(mdl, "\\PPB\\") != nullptr || std::strstr(mdl, "/PPB/") != nullptr;
        }
        std::uint64_t PivNowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }
        thread_local bool   t_pgScoped  = false;
        thread_local double t_pgRestore = 1.0;
    }

    bool PivGuardScopeActive()   { return t_pgScoped; }
    double PivGuardRestoreValue(){ return t_pgRestore; }
    void PivGuardClearScope()    { t_pgScoped = false; }
    void PivGuardInvalidate(std::uint32_t id)
    {
        std::lock_guard<std::mutex> g(g_pivCapMx);
        g_pivCap.erase(id);   // our own writer moved pivots — next drive re-captures the new truth
    }
    void PivGuardClearOnLoad()
    {
        std::lock_guard<std::mutex> g(g_pivCapMx);
        g_pivCap.clear();
        t_pgScoped = false;
    }

    void PivGuardOnPreDrive(RE::Actor* actor, const void* ragdollInstance)
    {
        // FIRST-FIRE DIAGNOSTIC (2026-07-29): the v2 field test failed SILENTLY — log the whole
        // decision chain for the first few drives so the log names the dead stage.
        // ⚠ 2026-07-31: this budget was 4 lines SHARED ACROSS ALL ACTORS, and the two reject
        // paths below consume it too — so on a normal street a generic NPC and a draugr burned
        // all four before the first PPB female ever drove, and the getOk/setOk receipt (the one
        // line that says whether our PLANCK calls actually land) was structurally unreachable.
        // A capped diagnostic that reaches its cap has stopped being a measurement. The REJECT
        // paths now have their own small budget; the ACCEPT path reports once PER ACTOR.
        static std::atomic<int> s_diagLeft{ 6 };
        const bool diag = s_diagLeft.load(std::memory_order_relaxed) > 0;

        if (!actor) return;
        if (!ObjectHold::PlanckLoosenOursOn()) {
            if (diag && s_diagLeft.fetch_sub(1) > 0)
                logger::info("PIVGUARD diag {:08X}: KNOB OFF (planckLoosenOurs)", actor->GetFormID());
            return;
        }
        if (!PivIsOnPPBSkeleton(actor)) {
            if (diag && s_diagLeft.fetch_sub(1) > 0) {
                auto* base = actor->GetActorBase();
                auto* race = base ? base->GetRace() : nullptr;
                const char* mdl = race ? race->skeletonModels[base->IsFemale() ? RE::SEXES::kFemale
                                                                               : RE::SEXES::kMale].GetModel() : nullptr;
                logger::info("PIVGUARD diag {:08X}: NOT PPB SKELETON — model='{}'",
                             actor->GetFormID(), mdl ? mdl : "<null>");
            }
            return;
        }

        // ── COMBAT GATE (2026-08-03, pivGuardCombatLoose) ────────────────────────────────────
        // PLANCK's loosen exists so the ragdoll CAN MATCH THE ANIM POSE (verified in its source,
        // PreDriveToPoseHook -> hkpEaseConstraintsAction + pivot move; see
        // 18_PLANCK_Internals_Reference). Scoping it to 0 keeps our baked joints from being
        // collapsed — correct for ordinary movement, but it also FORBIDS the ragdoll from
        // reaching poses our joints cannot express. Extreme combat animations are exactly that,
        // and a body that cannot follow its animation reads as stretched (user-reported).
        // With the knob on, leave PLANCK's global loosen alone while the actor is in combat.
        // 2026-08-08 refinement (user: a surrendered NPC must keep full-fit touch): the loosen
        // exists for EXTREME COMBAT ANIMS, and an in-combat NPC with her weapon away is not
        // performing any -- so require the weapon DRAWN too. A surrendered NPC keeps precise
        // capsule fit; the moment she re-draws, the loosen returns.
        if (ObjectHold::PivGuardCombatLooseOn() && actor->IsInCombat() &&
            actor->AsActorState() && actor->AsActorState()->IsWeaponDrawn()) {
            static std::mutex s_cmbMx;
            static std::set<std::uint32_t> s_cmbSeen;
            bool first = false;
            { std::lock_guard<std::mutex> g(s_cmbMx); first = s_cmbSeen.insert(actor->GetFormID()).second; }
            if (first)
                logger::info("PIVGUARD {:08X}: IN COMBAT -> leaving PLANCK's loosen at its global "
                             "value so extreme combat anims can be matched (pivGuardCombatLoose 1). "
                             "Capsule fit is intentionally traded away for the duration.",
                             actor->GetFormID());
            return;   // no scoping this drive — stock PLANCK behaviour
        }

        // 1) the flag bracket (Hooks.cpp restores right after the chained call returns)
        double prev = 1.0;
        const bool gotOk = DismemberGuard::PlanckGetSetting("loosenRagdollConstraintPivots", prev);
        const bool setOk = gotOk && DismemberGuard::PlanckSetSetting("loosenRagdollConstraintPivots", 0.0);
        // ONE receipt per actor, on its own budget — this is the line that proves our PLANCK
        // vtable calls still land after an upstream update. A FAILURE here means the whole
        // per-actor scoping is a silent no-op, so it logs as a WARNING, always.
        {
            static std::mutex s_seenMx;
            static std::set<std::uint32_t> s_seen;
            bool first = false;
            { std::lock_guard<std::mutex> g(s_seenMx); first = s_seen.insert(actor->GetFormID()).second; }
            if (first) {
                if (gotOk && setOk)
                    logger::info("PIVGUARD {:08X}: PLANCK setting calls OK (prev loosen={:.0f} -> 0 for this drive)",
                                 actor->GetFormID(), prev);
                else
                    logger::warn("PIVGUARD {:08X}: PLANCK SETTING CALL FAILED (getOk={} setOk={}) — per-actor "
                                 "pivot scoping is a NO-OP for her. If PLANCK was updated, its interface "
                                 "vtable may have moved (we call Get/SetSettingDouble at slots 13/14).",
                                 actor->GetFormID(), gotOk ? 1 : 0, setOk ? 1 : 0);
            }
        }
        if (setOk) {
            t_pgRestore = prev;
            t_pgScoped  = true;
        } else {
            return;   // PLANCK absent / API failed — nothing scoped
        }

        // 2) capture / verify at ~2 Hz per actor (joint lookup is 17 node walks — not per-frame)
        const std::uint32_t id = actor->GetFormID();
        PivCap* cp;
        {
            std::lock_guard<std::mutex> g(g_pivCapMx);
            cp = &g_pivCap[id];
        }
        const std::uint64_t now = PivNowMs();
        const bool fresh = (cp->ragdoll != ragdollInstance);
        if (!fresh && now - cp->lastCheckMs < 500) return;
        cp->lastCheckMs = now;
        if (fresh) { *cp = PivCap{}; cp->ragdoll = ragdollInstance; cp->lastCheckMs = now; }

        int healed = 0;
        for (int j = 0; j < 17; ++j) {
            if (j == 2 || j == 13) continue;                 // shoulders: clavicle-follow owns them
            auto* reChild = PivFindBodyRE(actor, kDChild[j]);
            auto* reOther = PivFindBodyRE(actor, kDOther[j]);
            if (!reChild || !reOther) continue;
            auto* child = reinterpret_cast<hkpEntity*>(reChild);
            hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOther));
            if (!ci || !ci->getDataRw()) continue;
            auto* atom = PivTransformsAtom(ci->getDataRw());
            if (!atom) continue;
            const hkVector4 ta = atom->m_transformA.getTranslation();
            const hkVector4 tb = atom->m_transformB.getTranslation();
            if (!cp->have[j]) {
                for (int k = 0; k < 3; ++k) { cp->a[j][k] = ta(k); cp->b[j][k] = tb(k); }
                cp->have[j] = true;
                continue;
            }
            bool off = false;
            for (int k = 0; k < 3; ++k)
                if (std::fabs(ta(k) - cp->a[j][k]) > 0.0015f ||
                    std::fabs(tb(k) - cp->b[j][k]) > 0.0015f) { off = true; break; }
            if (off) {
                hkVector4 ra; ra.set(cp->a[j][0], cp->a[j][1], cp->a[j][2], 0.f);
                hkVector4 rb; rb.set(cp->b[j][0], cp->b[j][1], cp->b[j][2], 0.f);
                atom->m_transformA.setTranslation(ra);
                atom->m_transformB.setTranslation(rb);
                ++healed;
            }
        }
        if (healed && !cp->announced) {
            cp->announced = true;
            logger::info("PIVGUARD {:08X}: healed {} stranded pivot pair(s) back to captured baked "
                         "values (collapse leak caught)", id, healed);
        }
    }

    void PivDescaleApply(RE::Actor* actor)
    {
        if (!actor || g_tune.pivDescale < 0.5f) return;
        const float scale = actor->GetScale();
        if (!(scale > 0.01f) || std::fabs(scale - 1.f) < 0.002f) return;   // unit scale: nothing to undo
        const float inv = 1.f / scale;
        const std::uint32_t id = actor->GetFormID();

        auto& seen = g_descaled[id];                       // value-init on first touch (all nullptr)
        int fixedNow = 0;
        for (int j = 0; j < 17; ++j) {
            auto* reChild = PivFindBodyRE(actor, kDChild[j]);
            if (!reChild) continue;
            auto* child = reinterpret_cast<hkpEntity*>(reChild);
            auto* reOther = PivFindBodyRE(actor, kDOther[j]);
            if (!reOther) continue;
            hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOther));
            if (!ci || seen[j] == ci) continue;            // already descaled THIS instance
            auto* atom = PivTransformsAtom(ci->getDataRw());
            if (!atom) continue;
            hkVector4 ta = atom->m_transformA.getTranslation();
            hkVector4 tb = atom->m_transformB.getTranslation();
            ta.mul4(inv);
            tb.mul4(inv);
            PivGuardInvalidate(actor->GetFormID());                  // pivots moved by US -> recapture
            atom->m_transformA.setTranslation(ta);
            atom->m_transformB.setTranslation(tb);
            seen[j] = ci;
            ++fixedNow;
        }
        if (fixedNow)
            logger::info("PIVDESCALE {:08X} scale={:.4f} -> {} constraint(s) descaled (x{:.4f}, left side included)",
                         id, scale, fixedNow, inv);
    }

    void PivDescaleClearAll() { g_descaled.clear(); }

    // ── UNIFORM RE-SCALE (2026-07-15, ARC-SUM — replaces the pose-tainted straight-line X/H check) ─────
    // REPLACES the old 17-joint per-joint node-seat entirely. The correction: the POSE-INVARIANT XP32 node
    // ARC-SUM (Spn0->Spn1->Spn2->Neck->Head, a sum of rigid bone lengths that a bending spine cannot move)
    // is compared to the Havok child-pivot arc-sum along the SAME chain; their ratio nodeArc/havokArc
    // (== trueScale/build_scale) is ONE uniform factor that scales the WHOLE ragdoll's constraint pivots
    // (both stored copies of every joint, L+R) so the Havok arc equals the XP32 arc (== baked x trueScale,
    // the STAMP result). A correctly-built NPC at ANY scale reads havokArc == nodeArc, passes the 0.5u
    // check, and is left byte-for-byte untouched. The base (48.045u @scale1) comes from the XPMSSE .nif.
    namespace {
        // Reads a joint (childNode<->otherNode): the CHILD-copy and OTHER-copy stored pivots mapped to
        // WORLD (game units), plus optional body speeds (game u/s) for the mid-scramble defer. false if
        // any body/constraint/atom is missing. World = R_body·local + t_body (the JTRACK math).
        bool ReadJointWorlds(RE::Actor* actor, const char* childNode, const char* otherNode,
                             float childW[3], float otherW[3], hkpConstraintInstance** ciOut,
                             float* spdChildU = nullptr, float* spdOtherU = nullptr)
        {
            auto* reChild = PivFindBodyRE(actor, childNode);
            auto* reOtherFind = PivFindBodyRE(actor, otherNode);
            if (!reChild || !reOtherFind) return false;
            auto* child = reinterpret_cast<hkpEntity*>(reChild);
            hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOtherFind));
            if (!ci) return false;
            auto* reOtherEnt = (ci->getEntityA() == child) ? ci->getEntityB() : ci->getEntityA();
            if (!reOtherEnt) return false;
            auto* atom = PivTransformsAtom(ci->getDataRw());
            if (!atom) return false;
            const bool childIsA = (ci->getEntityA() == child);
            const hkTransform& tChild = childIsA ? atom->m_transformA : atom->m_transformB;
            const hkTransform& tOther = childIsA ? atom->m_transformB : atom->m_transformA;
            auto* rbChild = reinterpret_cast<hkpRigidBody*>(reChild);
            auto* rbOther = reinterpret_cast<hkpRigidBody*>(reOtherEnt);
            const hkTransform& TC = rbChild->getTransform();
            const hkTransform& TO = rbOther->getTransform();
            const hkRotation& RC = TC.getRotation();
            const hkRotation& RO = TO.getRotation();
            const hkVector4 cl = tChild.getTranslation();
            const hkVector4 ol = tOther.getTranslation();
            // 2026-08-01: childW/otherW used to be written unconditionally while ciOut/spdChildU/
            // spdOtherU were null-guarded. That asymmetry is invisible at the call site and cost a
            // CTD the same day it was first hit. Guard them the same way as everything else.
            if (!childW || !otherW) return false;
            childW[0] = (RC(0,0)*cl(0) + RC(0,1)*cl(1) + RC(0,2)*cl(2) + TC.getTranslation()(0)) * kHavokToSkyrim;
            childW[1] = (RC(1,0)*cl(0) + RC(1,1)*cl(1) + RC(1,2)*cl(2) + TC.getTranslation()(1)) * kHavokToSkyrim;
            childW[2] = (RC(2,0)*cl(0) + RC(2,1)*cl(1) + RC(2,2)*cl(2) + TC.getTranslation()(2)) * kHavokToSkyrim;
            otherW[0] = (RO(0,0)*ol(0) + RO(0,1)*ol(1) + RO(0,2)*ol(2) + TO.getTranslation()(0)) * kHavokToSkyrim;
            otherW[1] = (RO(1,0)*ol(0) + RO(1,1)*ol(1) + RO(1,2)*ol(2) + TO.getTranslation()(1)) * kHavokToSkyrim;
            otherW[2] = (RO(2,0)*ol(0) + RO(2,1)*ol(1) + RO(2,2)*ol(2) + TO.getTranslation()(2)) * kHavokToSkyrim;
            if (ciOut) *ciOut = ci;
            if (spdChildU) { const auto& v = reChild->motion.linearVelocity.quad;
                *spdChildU = std::sqrt(v.m128_f32[0]*v.m128_f32[0] + v.m128_f32[1]*v.m128_f32[1] + v.m128_f32[2]*v.m128_f32[2]) * kHavokToSkyrim; }
            if (spdOtherU) { const auto& v = reinterpret_cast<RE::hkpRigidBody*>(reOtherEnt)->motion.linearVelocity.quad;
                *spdOtherU = std::sqrt(v.m128_f32[0]*v.m128_f32[0] + v.m128_f32[1]*v.m128_f32[1] + v.m128_f32[2]*v.m128_f32[2]) * kHavokToSkyrim; }
            return true;
        }

        // ── EVENT-DRIVEN RE-SCALE STATE MACHINE (2026-07-15, user-designed failsafe) ──────────────────
        // Not a continuous scan. A per-actor state machine armed by a Havok-joint REBUILD (the spine0<->COM
        // constraint-instance pointer changing — what PPB already sees) or a stand-up (ragdoll re-attach),
        // then: SETTLE 1-2s -> ANCHOR gate (the spine0<->COM joint's two copies coincide, ie connected/settled,
        // 0.5u; heel-safe unlike a joint-vs-node check) -> STABILITY gate (5 arc reads @0.5s, spread <=0.5u)
        // -> APPLY the median -> IDLE. Any gate that fails waits 5s and retries, up to 12x, then parks until
        // the NEXT rebuild. Idle cost is one instance-pointer compare per ~1s. Phases: 0 IDLE, 1 SETTLE, 2 MEASURE.
        struct ReScaleState {
            hkpConstraintInstance* anchorInst = nullptr;             // spine0<->COM instance — the rebuild signal
            bool  wasAttached = false;                               // ActorRagdollAttached edge (stand-up re-arm)
            int   phase = 0;                                         // 0 IDLE(done/unarmed) · 1 SETTLE · 2 MEASURE
            std::chrono::steady_clock::time_point idlePoll{};        // cheap ~1 Hz instance-watch while idle
            std::chrono::steady_clock::time_point armedAt{};         // when the sequence armed (settle timer)
            std::chrono::steady_clock::time_point lastRead{};        // last arc read (0.5 s cadence)
            std::chrono::steady_clock::time_point lastRetry{};       // last retry (5 s cadence)
            std::chrono::steady_clock::time_point stallLog{};        // last "why am I parked" receipt (5 s cadence)
            bool  farAbandoned = false;   // mid-sequence distance abandon -> re-arm on APPROACH,
                                          // not only on the next rebuild (2026-08-07 skull-2u-low)
            int   retryCount = 0;                                    // bounded 12 retries, then wait for next rebuild
            float reads[5]{};                                        // the 5 havokArc samples
            int   readN = 0;                                         // how many collected
            std::chrono::steady_clock::time_point lastOk{};          // last SUCCESSFUL arc read — the 90 s
                                                                     // unreadable give-up counts from here,
                                                                     // NOT armedAt (review 2026-07-17: a
                                                                     // walking/long-grabbed follower keeps
                                                                     // making progress past 90 s of wall time)
        };
        std::unordered_map<std::uint32_t, ReScaleState> s_rescale;
        // Arc trueScale (nodeArc / .nif scale-1 base) per NPC — pose-invariant, reference-free, untainted.
        // Published by PivScaleCorrect so CapFix sizes the collision CAPSULES to the SAME scale the arc
        // re-scale gives the joints (Option B, full scaling solution — joints + capsules in lock-step).
        std::unordered_map<std::uint32_t, float> s_arcTrueScale;

        inline float Dist3(const float a[3], const float b[3]) {
            const float x = a[0]-b[0], y = a[1]-b[1], z = a[2]-b[2];
            return std::sqrt(x*x + y*y + z*z);
        }
    }

    void PivReScaleClearAll() { s_rescale.clear(); s_arcTrueScale.clear(); }

    // The arc trueScale (nodeArc / .nif scale-1 base) latched by PivScaleCorrect for this NPC — the
    // pose-invariant, reference-free true scale. Returns 0 until measured (a settled, in-range read).
    // CapFix reads this so the collision capsules ride the SAME scale as the arc-corrected joints.
    float PivArcTrueScaleOf(RE::Actor* actor) {
        if (!actor) return 0.f;
        auto it = s_arcTrueScale.find(actor->GetFormID());
        return (it != s_arcTrueScale.end() && it->second > 0.3f && it->second < 3.f) ? it->second : 0.f;
    }

    void PivScaleCorrect(RE::Actor* actor)
    {
        if (CapFixGen() == 0 || !actor) return;
        if (g_tune.pivReScale < 0.5f) return;                        // master switch (off -> everyone untouched)
        // ── BAKE GATE (2026-07-15): only re-scale actors that carry OUR female bake (COM = bhkListShape).
        // Males, undead/draugr skeletons, and custom-skeleton females keep STOCK single-capsule bodies we
        // never built — re-scaling their pivots off the FEMALE .nif base (48.045) is meaningless and
        // deformed them. Shared gate with CapFix so both geometry writers touch exactly the same actors.
        if (!GrabDiag::ActorCarriesBake(actor)) return;
        // (2026-07-15) SELF-CONTAINED: the arc re-scale reads the XP32 nodes + Havok pivots directly, so it
        // no longer gates on CapFix's legacy tape measure — the whole arc scaling solution (joints + the
        // published capsule scale) is decoupled from measuredScaleEnable. The settle/anchor/speed gates below
        // are the only preconditions; the arc factor is plausibility-clamped (0.3..3.0) so it never writes garbage.
        const float getScale = actor->GetScale();
        if (!(getScale > 0.01f)) return;
        auto* root3d = actor->Get3D();
        if (!root3d) return;
        const std::uint32_t id = actor->GetFormID();
        auto& st = s_rescale[id];
        const auto now = std::chrono::steady_clock::now();

        // ── DISTANCE GATE + ABANDON (10 m ~= 700 game u): never process a far NPC (never the whole town);
        // if the player leaves mid-sequence, drop back to IDLE (re-arms on the next rebuild once back in range).
        constexpr float kMaxDistU = 10.0f * kHavokToSkyrim;
        if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
            const RE::NiPoint3 pa = actor->GetPosition();
            const RE::NiPoint3 pp = pc->GetPosition();
            const float dx = pa.x-pp.x, dy = pa.y-pp.y, dz = pa.z-pp.z;
            if (dx*dx + dy*dy + dz*dz > kMaxDistU*kMaxDistU) {
                // 2026-08-07 (Carmella skull 2u low, 8 min parked): this abandon was SILENT and
                // the only re-arm was "the next rebuild" -- which for a standing NPC may simply
                // never come, so a mid-sequence walk-away stranded her mis-scaled FOREVER with
                // zero receipts. Now: say so once, and remember to re-arm on APPROACH.
                if (st.phase != 0) {
                    st.farAbandoned = true;
                    logger::info("PIVRESCALE {:08X} ABANDONED mid-sequence (>10 m) -- will re-ARM "
                                 "when the player is back in range (no rebuild needed)", id);
                }
                st.phase = 0; return;
            }
            if (st.farAbandoned && st.phase == 0) {
                st.farAbandoned = false;
                st.phase = 1; st.armedAt = now; st.retryCount = 0; st.readN = 0; st.lastRetry = {};
                logger::info("PIVRESCALE {:08X} back in range after abandon -> re-ARM re-scale "
                             "sequence", id);
            }
        }

        // ── IDLE POLL THROTTLE: while parked (phase 0) the ONLY work is watching the rebuild signal, and a
        // rebuild is rare — poll it ~1 Hz (staggered by id), not every frame. Once armed (phase != 0) this is
        // skipped so the settle/read timers run at frame resolution.
        if (st.phase == 0) {
            const std::chrono::milliseconds idleInterval{ 1000 + static_cast<int>(id % 331) };
            if (st.idlePoll.time_since_epoch().count() != 0 && now - st.idlePoll < idleInterval) return;
            st.idlePoll = now;
        }

        // ── XP32 NODE reader (live world position). Used by the arc-sum measure below.
        auto nodePos = [&](const char* n, float o[3]) -> bool {
            auto* obj = root3d->GetObjectByName(n);
            if (!obj) return false;
            o[0] = obj->world.translate.x; o[1] = obj->world.translate.y; o[2] = obj->world.translate.z;
            return true;
        };

        // ── POST-CORRECTION SELF-VERIFY (2026-08-01, user-caught) ──────────────────────────────
        // The PIVRESCALE receipt below reports the drive TARGET, not the outcome. The user had a
        // head sitting ~3u low while that line read a perfectly clean x1.0521 / trueScale 0.9514 —
        // i.e. the correction announced success and nobody ever measured the result. That is the
        // ledger's oldest rule (measure the SETTLED bodies, never the command) being broken by the
        // correction's own logging. So: ~1.5s after every re-scale, walk the head chain and report
        // where each Havok pivot ACTUALLY landed relative to its XP32 bone, in game units.
        //   H= is the head. ~0 = the ball is on the bone. A few units = the correction did not land,
        //   and the size of H is the size of the user's visible error.
        {
            bool due = false;
            { std::lock_guard<std::mutex> gv(g_rsvMx);
              auto it = g_rescaleVerifyAt.find(id);
              if (it != g_rescaleVerifyAt.end() && RsvNowMs() >= it->second) { due = true; g_rescaleVerifyAt.erase(it); } }
            if (due) {
                static const char* kVN[5] = { "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
                                              "NPC Neck [Neck]",  "NPC Head [Head]" };
                static const char* kVO[5] = { "NPC COM [COM ]",   "NPC Spine [Spn0]", "NPC Spine1 [Spn1]",
                                              "NPC Spine2 [Spn2]", "NPC Neck [Neck]" };
                float d[5] = { -1.f, -1.f, -1.f, -1.f, -1.f };
                float nodeArcNow = 0.f, havokArcNow = 0.f;
                float nP[5][3], pP[5][3], oDmy[3]; bool okAll = true;
                for (int i = 0; i < 5; ++i) {
                    // oDmy, NOT nullptr — ReadJointWorlds writes otherW[0..2] UNCONDITIONALLY
                    // (only ciOut/spdChildU/spdOtherU are null-guarded). Passing nullptr here was
                    // crash-2026-08-01-22-06-06: EXCEPTION_ACCESS_VIOLATION, movss [rax],xmm1, rax=0.
                    if (!nodePos(kVN[i], nP[i]) ||
                        !ReadJointWorlds(actor, kVN[i], kVO[i], pP[i], oDmy, nullptr)) { okAll = false; continue; }
                    d[i] = Dist3(pP[i], nP[i]);
                }
                if (okAll)
                    for (int i = 0; i < 4; ++i) { nodeArcNow += Dist3(nP[i], nP[i+1]); havokArcNow += Dist3(pP[i], pP[i+1]); }
                const float resid = (havokArcNow > 1e-4f) ? (nodeArcNow / havokArcNow) : -1.f;
                logger::info("PIVVERIFY {:08X} 1.5s AFTER re-scale — ball-vs-bone(u): s0={:.2f} s1={:.2f} "
                             "s2={:.2f} neck={:.2f} HEAD={:.2f} | arc now node={:.2f} havok={:.2f} "
                             "residual factor x{:.4f} (want x1.0000; HEAD is the user-visible error)",
                             id, d[0], d[1], d[2], d[3], d[4], nodeArcNow, havokArcNow, resid);
            }
        }

        // ── ARC-SUM measure (pose-invariant): the XP32 node arc Spn0->Spn1->Spn2->Neck->Head vs the SAME
        // chain of Havok child-side pivots. nodeArc is a sum of RIGID bone lengths (a joint rotating never
        // changes it); the Havok pivot arc is likewise pose-invariant once the ragdoll is connected. Their
        // ratio nodeArc/havokArc == trueScale/build_scale == the exact uniform factor that drives the Havok
        // to baked×trueScale. Returns false if the 3D isn't ready or a joint is mid-scramble (>40 u/s).
        // Failure receipt for the give-up logs (2026-07-17): M'rissi sat in the stability
        // phase for 15 MINUTES with measureArc failing silently every tick — zero receipts,
        // no give-up. Record WHAT failed so the log can say so.
        const char* arcWhyKind = "?"; const char* arcWhyBone = "";
        auto measureArc = [&](float& nodeArcOut, float& havokArcOut) -> bool {
            static const char* kArcNode[5]  = { "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
                                                "NPC Neck [Neck]",  "NPC Head [Head]" };
            static const char* kArcOther[5] = { "NPC COM [COM ]",   "NPC Spine [Spn0]", "NPC Spine1 [Spn1]",
                                                "NPC Spine2 [Spn2]", "NPC Neck [Neck]" };
            float nodeP[5][3], pivP[5][3];
            for (int i = 0; i < 5; ++i) {
                if (!nodePos(kArcNode[i], nodeP[i])) { arcWhyKind = "node"; arcWhyBone = kArcNode[i]; return false; }
                float dmy[3], spdC = 0.f, spdO = 0.f;
                if (!ReadJointWorlds(actor, kArcNode[i], kArcOther[i], pivP[i], dmy, nullptr, &spdC, &spdO)) {
                    arcWhyKind = "joint"; arcWhyBone = kArcNode[i]; return false;
                }
                if (spdC > 40.f || spdO > 40.f) { arcWhyKind = "speed"; arcWhyBone = kArcNode[i]; return false; }
            }
            float na = 0.f, ha = 0.f;
            for (int i = 0; i < 4; ++i) { na += Dist3(nodeP[i], nodeP[i + 1]); ha += Dist3(pivP[i], pivP[i + 1]); }
            if (!(na > 1.f && ha > 1e-4f)) { arcWhyKind = "degenerate"; arcWhyBone = ""; return false; }
            nodeArcOut = na; havokArcOut = ha; return true;
        };

        // ── (A) TRIGGER: the spine0<->COM constraint INSTANCE pointer IS the rebuild signal. When it changes
        // (engine rebuilt the ragdoll: warp / 3D reload / PLANCK re-activate) OR the actor stands up
        // (ActorRagdollAttached false->true edge), ARM the sequence. This is the only work done while idle.
        auto* comB  = PivFindBodyRE(actor, "NPC COM [COM ]");
        auto* spn0B = PivFindBodyRE(actor, "NPC Spine [Spn0]");
        hkpConstraintInstance* anchorInst = (comB && spn0B)
            ? PivFindJoint(reinterpret_cast<hkpEntity*>(spn0B), reinterpret_cast<hkpEntity*>(comB)) : nullptr;
        const bool attached = ObjectHold::ActorRagdollAttached(actor);
        if (anchorInst && (anchorInst != st.anchorInst || (attached && !st.wasAttached))) {
            st.anchorInst = anchorInst;
            st.phase = 1; st.armedAt = now; st.retryCount = 0; st.readN = 0; st.lastRetry = {};
            logger::info("PIVRESCALE {:08X} rebuild/stand-up detected -> ARM re-scale sequence", id);
        } else if (anchorInst) {
            st.anchorInst = anchorInst;                              // same instance -> keep the pointer fresh
        }
        st.wasAttached = attached;
        if (st.phase == 0) return;                                  // IDLE/done -> nothing but the compare above

        // ── retry helper: park the sequence for 5 s, give up to IDLE after 12 tries (re-arms on next rebuild).
        auto retry = [&](const char* why) {
            st.readN = 0;
            if (st.lastRetry.time_since_epoch().count() == 0 || now - st.lastRetry >= std::chrono::seconds(5)) {
                st.lastRetry = now;
                if (++st.retryCount >= 12) {
                    st.phase = 0;
                    logger::info("PIVRESCALE {:08X} gave up ({}) after 12 retries -> idle until next rebuild", id, why);
                }
            }
        };

        // A grab displaces limbs ~12u — never measure through one (transient, no retry clock).
        // 2026-08-01: these three returns were SILENT — an armed sequence could sit forever with no
        // receipt, which is exactly what happened (ARM at 18:02:16, no PIVARC 2 minutes later, and
        // no "gave up" either because retry() is never reached from here). Name the stall, throttled.
        if (PivGrabGateEnabled() && Interop::IsActorGrabbedByPlayer(actor)) {
            if (st.stallLog.time_since_epoch().count() == 0 || now - st.stallLog >= std::chrono::seconds(5)) {
                st.stallLog = now;
                logger::info("PIVRESCALE {:08X} STALLED: player is grabbing her (HIGGS hold) — a grab "
                             "displaces limbs ~12u, so the measure is parked until you let go.", id);
            }
            return;
        }

        // ── (B) SETTLE: 1.5 s after arming, before anything is measured (ragdoll still building/dropping).
        if (now - st.armedAt < std::chrono::milliseconds(1500)) return;

        // ── (C) ANCHOR gate: the ragdoll must be CONNECTED + SETTLED. The spine0<->COM joint's two stored
        // pivot COPIES coincide in world only when the constraint is satisfied — a RELATIVE check, so the
        // heel-fix's COM lift never trips it (unlike a joint-vs-node position check would). Loosened / mid-drop
        // -> copies apart -> retry (wait 5 s, up to 12x). This is the user's ".5u anchor" failsafe, heel-safe.
        if (!attached) { retry("ragdoll detached"); return; }
        float s0Child[3], s0Other[3];
        if (!ReadJointWorlds(actor, "NPC Spine [Spn0]", "NPC COM [COM ]", s0Child, s0Other, nullptr)) { retry("no anchor joint"); return; }
        if (Dist3(s0Child, s0Other) > 0.5f) { retry("anchor not settled"); return; }

        // ── (D) STABILITY: 5 arc reads at 0.5 s. A settling ragdoll passes through TRANSIENT arcs (Lydia read
        // 45.56 mid-settle, then her real 50.51) — writing on a transient was exactly the bug this replaces.
        // The anchor gate above re-checks every tick, so if the body loosens mid-measure the reads reset.
        if (st.phase == 1) { st.phase = 2; st.readN = 0; st.lastRead = {}; st.lastOk = now; }   // enter MEASURE
        if (st.readN < 5) {
            if (st.readN == 0 || now - st.lastRead >= std::chrono::milliseconds(500)) {
                float na = 0.f, ha = 0.f;
                if (!measureArc(na, ha)) {
                    // Transiently not ready -> try again next frame — but NEVER silently forever:
                    // a PERSISTENT fail (2026-07-17: M'rissi hung here 15 min, zero receipts)
                    // gives up LOUDLY with the reason after 90 s WITHOUT A SINGLE successful
                    // read (lastOk-keyed: locomotion/grab windows that still read now and then
                    // keep the sequence alive — review 2026-07-17). Re-arms on the next rebuild.
                    if (now - st.lastOk > std::chrono::seconds(90)) {
                        st.phase = 0;
                        logger::info("PIVRESCALE {:08X} arc unreadable for 90 s ({} {}) -> idle until next rebuild",
                                     id, arcWhyKind, arcWhyBone);
                    }
                    return;
                }
                st.reads[st.readN++] = ha;
                st.lastRead = now;
                st.lastOk   = now;
            }
            return;                                                 // keep collecting
        }
        // 5 collected -> median + spread (insertion sort, 5 elements; no <algorithm>/<cstring> dependency).
        float tmp[5];
        for (int k = 0; k < 5; ++k) tmp[k] = st.reads[k];
        for (int a = 1; a < 5; ++a) { const float v = tmp[a]; int b = a - 1; while (b >= 0 && tmp[b] > v) { tmp[b + 1] = tmp[b]; --b; } tmp[b + 1] = v; }
        const float spread = tmp[4] - tmp[0];
        const float medHavokArc = tmp[2];
        if (spread > 0.5f) { logger::info("PIVRESCALE {:08X} arc UNSTABLE (spread={:.2f}u) -> retry", id, spread); retry("unstable arc"); return; }

        // ── (E) APPLY the median. Publish trueScale (capsules ride it), then park to IDLE regardless of
        // whether a write was needed — the next rebuild re-arms. factor == trueScale / build_scale.
        float nodeArc = 0.f, dummyH = 0.f;
        if (!measureArc(nodeArc, dummyH)) {
            st.phase = 0;
            logger::info("PIVRESCALE {:08X} arc re-read failed at APPLY ({} {}) -> idle until next rebuild",
                         id, arcWhyKind, arcWhyBone);
            return;
        }
        constexpr float kBaseArc = 48.045f;                          // Spn0->Head arc @scale1 (XPMSSE .nif)
        const float arcScale = nodeArc / kBaseArc;                   // the NPC's true scale (nodeArc / .nif base)
        s_arcTrueScale[id] = arcScale;                               // publish for CapFix -> capsules ride the SAME scale
        const float factor  = nodeArc / medHavokArc;                 // the correction (== trueScale/build_scale)
        logger::info("PIVARC {:08X} nodeArc={:.3f} medHavokArc={:.3f} (spread={:.2f}) -> factor x{:.4f} trueScale={:.4f} (GetScale {:.4f})",
                     id, nodeArc, medHavokArc, spread, factor, arcScale, getScale);
        st.phase = 0;                                               // DONE -> idle until the next rebuild
        if (std::fabs(nodeArc - medHavokArc) <= 0.5f) return;       // already correct (any scale) -> zero writes
        if (!(factor > 0.3f && factor < 3.f)) return;               // implausible -> never write garbage
        if (std::fabs(factor - 1.f) < 0.002f) return;               // nothing meaningful to scale
        if (g_tune.pivReScaleApply < 0.5f) return;                  // diagnostic mode: measure + log, don't write

        int scaledNow = 0;
        for (int j = 0; j < 17; ++j) {                               // all 12 joints, L+R (the 17-entry table)
            auto* reChild = PivFindBodyRE(actor, kDChild[j]);
            if (!reChild) continue;
            auto* child = reinterpret_cast<hkpEntity*>(reChild);
            auto* reOtherFind = PivFindBodyRE(actor, kDOther[j]);
            if (!reOtherFind) continue;
            hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOtherFind));
            if (!ci) continue;
            auto* atom = PivTransformsAtom(ci->getDataRw());
            if (!atom) continue;
            hkVector4 ta = atom->m_transformA.getTranslation();
            hkVector4 tb = atom->m_transformB.getTranslation();
            ta.mul4(factor);                                         // ONE scalar, BOTH copies, in lock-step
            tb.mul4(factor);
            PivGuardInvalidate(actor->GetFormID());                  // pivots moved by US -> recapture
            atom->m_transformA.setTranslation(ta);
            atom->m_transformB.setTranslation(tb);
            ++scaledNow;
        }
        if (scaledNow)
            logger::info("PIVRESCALE {:08X} MIS-SCALED (nodeArc={:.2f} medHavokArc={:.2f}) -> UNIFORM x{:.4f} "
                         "trueScale={:.4f} on {} constraint(s), both copies, L+R",
                         id, nodeArc, medHavokArc, factor, arcScale, scaledNow);
            // ── SELF-VERIFY (2026-08-01, user-caught) ───────────────────────────────────
            // "ReScale fired" is NOT "the result is right". This receipt reports the drive
            // TARGET; the user had a head 3u low with this exact line reading a clean
            // x1.0521 / trueScale 0.9514. That is the ledger's oldest rule (measure the
            // SETTLED bodies, never the command) being violated by the correction's own
            // logging. Arm a PIVTRACK audit ~1.5s later — long enough for the PD drive to
            // settle — so every re-scale states its OUTCOME, per joint, in game units.
            // H= is the head. Non-zero after a correction means the correction did not land.
            { std::lock_guard<std::mutex> gv(g_rsvMx); g_rescaleVerifyAt[id] = RsvNowMs() + 1500; }
    }

    // ── JOINT TRACK (2026-07-10 — the actor-scale audit, console `jtrack`, READ-ONLY) ─────────────
    // For EVERY ragdoll joint, measure where the Havok constraint pivot actually sits in the world
    // versus where its XP32 bone node is. Writes NOTHING to any constraint (no heal, no descale, no
    // arming) — pure measurement to settle whether a scaled actor's pivots drift with distance from
    // the root (the engine pivot×GetScale bug PivDescaleApply targets). Same 17 joints (left side
    // included) PivDescaleApply sweeps, via the shared kDChild/kDOther/kDName tables.
    // HEAD-GAP SAMPLER (2026-07-13, the user's head-trim design): the head joint's
    // pivot-vs-node Z gap, game units — one joint's worth of the jtrack math, exported
    // so the root conform can trim its captured constant by the MEASURED head error.
    // RAGDOLL-ATTACHED gate (2026-07-13, the furniture-poisoning fix): FALSE while the
    // actor sits/sleeps/uses furniture or is in a ragdoll state — PLANCK loosens the
    // ragdoll from the skeleton there ("green" bodies), so ANY measurement taken is
    // garbage and must not latch. All samplers (measured-scale, root capture, head
    // trim) gate on this and RE-MEASURE on the stand-up transition.
    bool ActorRagdollAttached(RE::Actor* actor)
    {
        if (!actor) return false;
        if (actor->IsInRagdollState()) return false;
        if (auto* st = actor->AsActorState()) {
            if (st->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return false;
            // 2026-07-13 conflict-forecast additions (Report 17A-20/21, same flags word):
            // kGetUp flips IsInRagdollState false while the body is still mid-get-up —
            // without this the stood-up edge fires and captures get-up garbage; swimming
            // holds a horizontal pose that can fill a whole 10-sample window.
            if (st->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return false;
            if (st->IsSwimming()) return false;
        }
        // 17A-22: killmove/paired sync warps hold bodies displaced against the partner.
        if (actor->IsInKillMove()) return false;
        // 17A-4: PLANCK 0.8.0 loosens the ragdoll of a DisableAI'd NPC while she STANDS
        // (paralysis/capture/down-state mods do this) — engine bookkeeping still reads
        // attached, so the samplers would latch poisoned constants.
        if (!actor->IsAIEnabled()) return false;
        if (actor->GetOccupiedFurniture()) return false;
        return true;
    }

    bool PivHeadGapZ(RE::Actor* actor, float& outZ)
    {
        if (!actor) return false;
        auto* root3d = actor->Get3D();
        if (!root3d) return false;
        auto* reChild = PivFindBodyRE(actor, "NPC Head [Head]");
        auto* reOther = PivFindBodyRE(actor, "NPC Neck [Neck]");
        if (!reChild || !reOther) return false;
        auto* child = reinterpret_cast<hkpEntity*>(reChild);
        hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOther));
        if (!ci) return false;
        auto* atom = PivTransformsAtom(ci->getDataRw());
        if (!atom) return false;
        const bool childIsA = (ci->getEntityA() == child);
        const hkTransform& tChild = childIsA ? atom->m_transformA : atom->m_transformB;
        const hkVector4 curLocal = tChild.getTranslation();
        auto* rbChild = reinterpret_cast<hkpRigidBody*>(reChild);
        const hkTransform& TC = rbChild->getTransform();
        const hkRotation& RC = TC.getRotation();
        const float cwz = (RC(2,0)*curLocal(0) + RC(2,1)*curLocal(1) + RC(2,2)*curLocal(2)
                           + TC.getTranslation()(2)) * kHavokToSkyrim;
        auto* nodeObj = root3d->GetObjectByName("NPC Head [Head]");
        if (!nodeObj) return false;
        outZ = cwz - nodeObj->world.translate.z;   // + = pivot ABOVE the node
        return true;
    }

    void PivJointTrackDump(RE::Actor* actor)
    {
        if (!actor) return;
        auto* root3d = actor->Get3D();
        const std::uint32_t id = actor->GetFormID();
        if (!root3d) {
            logger::info("JTRACK {:08X} - actor has no 3D loaded; nothing to measure", id);
            return;
        }
        auto* base = actor->GetActorBase();
        const char* nm = (base && base->GetFullName()) ? base->GetFullName() : "<unnamed>";
        logger::info("JTRACK {:08X} '{}' scale={:.4f} - Havok joint pivot vs XP32 node, 17 joints (READ-ONLY, no writes)",
                     id, nm, actor->GetScale());

        float gcMag[17] = {};                     // |gapChild| per joint (game u) — the summary set
        float bwX[17] = {}, bwY[17] = {}, bwZ[17] = {};   // child bone-node world position (game u) — scale-law ratio
        bool  jvalid[17] = {};
        for (int j = 0; j < 17; ++j) {
            auto* reChild = PivFindBodyRE(actor, kDChild[j]);
            if (!reChild) continue;
            auto* child = reinterpret_cast<hkpEntity*>(reChild);
            auto* reOtherFind = PivFindBodyRE(actor, kDOther[j]);
            if (!reOtherFind) continue;
            hkpConstraintInstance* ci = PivFindJoint(child, reinterpret_cast<hkpEntity*>(reOtherFind));
            if (!ci) continue;
            // Partner comes from the constraint itself (source of truth), exactly like PivFixApply.
            auto* reOtherEnt = (ci->getEntityA() == child) ? ci->getEntityB() : ci->getEntityA();
            if (!reOtherEnt) continue;
            auto* atom = PivTransformsAtom(ci->getDataRw());   // READ-ONLY use — only getTranslation() is called
            if (!atom) continue;
            const bool childIsA = (ci->getEntityA() == child);
            const hkTransform& tChild = childIsA ? atom->m_transformA : atom->m_transformB;
            const hkTransform& tOther = childIsA ? atom->m_transformB : atom->m_transformA;
            const hkVector4 curLocal = tChild.getTranslation();   // child-side pivot, child body frame
            const hkVector4 oLocal   = tOther.getTranslation();   // partner-side pivot, partner body frame

            auto* rbChild = reinterpret_cast<hkpRigidBody*>(reChild);
            auto* rbOther = reinterpret_cast<hkpRigidBody*>(reOtherEnt);
            const hkTransform& TC = rbChild->getTransform();
            const hkTransform& TO = rbOther->getTransform();
            const hkRotation& RC = TC.getRotation();
            const hkRotation& RO = TO.getRotation();

            // Stored pivots -> world (havok m): world = R·local + t. Then ×kHavokToSkyrim to game units.
            const float cwx = (RC(0,0)*curLocal(0) + RC(0,1)*curLocal(1) + RC(0,2)*curLocal(2) + TC.getTranslation()(0)) * kHavokToSkyrim;
            const float cwy = (RC(1,0)*curLocal(0) + RC(1,1)*curLocal(1) + RC(1,2)*curLocal(2) + TC.getTranslation()(1)) * kHavokToSkyrim;
            const float cwz = (RC(2,0)*curLocal(0) + RC(2,1)*curLocal(1) + RC(2,2)*curLocal(2) + TC.getTranslation()(2)) * kHavokToSkyrim;
            const float owx = (RO(0,0)*oLocal(0) + RO(0,1)*oLocal(1) + RO(0,2)*oLocal(2) + TO.getTranslation()(0)) * kHavokToSkyrim;
            const float owy = (RO(1,0)*oLocal(0) + RO(1,1)*oLocal(1) + RO(1,2)*oLocal(2) + TO.getTranslation()(1)) * kHavokToSkyrim;
            const float owz = (RO(2,0)*oLocal(0) + RO(2,1)*oLocal(1) + RO(2,2)*oLocal(2) + TO.getTranslation()(2)) * kHavokToSkyrim;

            // The child bone origin IS the joint position (a child bone's origin sits on its parent joint).
            auto* nodeObj = root3d->GetObjectByName(kDChild[j]);
            if (!nodeObj) continue;
            const auto& bw = nodeObj->world.translate;   // game units

            const float gcx = cwx - bw.x, gcy = cwy - bw.y, gcz = cwz - bw.z;
            const float gox = owx - bw.x, goy = owy - bw.y, goz = owz - bw.z;
            const float gcM = std::sqrt(gcx*gcx + gcy*gcy + gcz*gcz);
            const float goM = std::sqrt(gox*gox + goy*goy + goz*goz);
            const float copiesApart = std::sqrt((cwx-owx)*(cwx-owx) + (cwy-owy)*(cwy-owy) + (cwz-owz)*(cwz-owz));
            // Child body's own world position (BodyPosU-style), game units. NOTE: appended past the
            // spec's example line — an informative extra field; the dump stays strictly read-only.
            const float bodyx = TC.getTranslation()(0) * kHavokToSkyrim;
            const float bodyy = TC.getTranslation()(1) * kHavokToSkyrim;
            const float bodyz = TC.getTranslation()(2) * kHavokToSkyrim;

            gcMag[j] = gcM; bwX[j] = bw.x; bwY[j] = bw.y; bwZ[j] = bw.z; jvalid[j] = true;
            logger::info("JTRACK {:08X} {:>8} gapChild=[{:.2f} {:.2f} {:.2f}] |d|={:.2f}  gapOther=[{:.2f} {:.2f} {:.2f}] |d|={:.2f}  copiesApart={:.2f}  boneW=[{:.2f} {:.2f} {:.2f}] bodyW=[{:.2f} {:.2f} {:.2f}] (game u)",
                         id, kDName[j], gcx, gcy, gcz, gcM, gox, goy, goz, goM, copiesApart,
                         bw.x, bw.y, bw.z, bodyx, bodyy, bodyz);
        }

        // SUMMARY: mean/max |gapChild| across the discovered joints, plus the scale-law readout —
        // |gapChild| / (bone-to-root distance) for the 4 most-distal joints (wrist/ankle, both sides).
        // Under the engine pivot×GetScale bug the gap grows ~linearly with distance from the root, so
        // this ratio ~ |1 - scale| when the bug is present and ~ 0 when the pivots are correct.
        float sum = 0.f, maxg = 0.f; int nValid = 0;
        for (int j = 0; j < 17; ++j)
            if (jvalid[j]) { sum += gcMag[j]; if (gcMag[j] > maxg) maxg = gcMag[j]; ++nValid; }
        const float mean = nValid ? sum / nValid : 0.f;

        float rootX = 0.f, rootY = 0.f, rootZ = 0.f; bool rootFound = false;
        if (auto* rootNode = root3d->GetObjectByName("NPC Root [Root]")) {
            const auto& rw = rootNode->world.translate;
            rootX = rw.x; rootY = rw.y; rootZ = rw.z; rootFound = true;
        }
        auto ratioOf = [&](int j) -> float {
            if (!jvalid[j] || !rootFound) return -1.f;
            const float dx = bwX[j]-rootX, dy = bwY[j]-rootY, dz = bwZ[j]-rootZ;
            const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
            return d > 1e-3f ? gcMag[j] / d : -1.f;
        };
        logger::info("JTRACK {:08X} SUMMARY meanGapChild={:.2f} maxGapChild={:.2f} ({} joints) | scale-law gap/root-dist: wrist={:.3f} ankleR={:.3f} wristL={:.3f} ankleL={:.3f} (root {})",
                     id, mean, maxg, nValid, ratioOf(0), ratioOf(10), ratioOf(11), ratioOf(16),
                     rootFound ? "found" : "MISSING");
        // ★ REFERENCE spans: the exact scale-1 measured-scale reference (COM->head / COM->calf NODE
        // distances), dumped in the JTRACK path (runs even on stalled followers like the reference actor).
        // Fire jtrackNow next to Lydia (scale 1) and hardcode these into g_refHeadSpan / g_refCalfSpan.
        if (auto* comN = root3d->GetObjectByName("NPC COM [COM ]"))
        if (auto* headN = root3d->GetObjectByName("NPC Head [Head]"))
        if (auto* calfN = root3d->GetObjectByName("NPC L Calf [LClf]")) {
            const auto& c = comN->world.translate; const auto& h = headN->world.translate; const auto& k = calfN->world.translate;
            const float sc = root3d->world.scale > 0.05f ? root3d->world.scale : 1.f;
            const float dh = std::sqrt((h.x-c.x)*(h.x-c.x) + (h.y-c.y)*(h.y-c.y) + (h.z-c.z)*(h.z-c.z)) / sc;
            const float dk = std::sqrt((k.x-c.x)*(k.x-c.x) + (k.y-c.y)*(k.y-c.y) + (k.z-c.z)*(k.z-c.z)) / sc;
            logger::info("JTRACK {:08X} ★REF COM->head={:.3f} COM->calf={:.3f} @scale1 (scale {:.4f}) -- hardcode if this is Lydia",
                         id, dh, dk, sc);
        }
    }

    // ── JOINT-TRACK tuning-knob trigger (2026-07-10) ──────────────────────────────────────────────
    // The console registration for `jtrack` found no free donor slot in this load order (PPB.log:
    // "Console-command donors for 'jtrack' all missing"), so the tuning knob `jtrackNow` is the
    // working trigger (the registration code in main.cpp stays — harmless). Edge-triggered like
    // handBoxDumpNow: on the knob's 0 -> non-0 transition, arm a ~2 s window; each driven actor's
    // pre-drive tick inside the window dumps ONCE (so nearby NPCs all dump), then the latch disarms
    // until the next edge. Main thread only (the pre-drive pipeline calls this) — no locks needed.
    void PivJointTrackTick(RE::Actor* actor)
    {
        static float s_lastVal = 0.f;
        static bool  s_armed   = false;
        static std::chrono::steady_clock::time_point s_edgeAt{};
        static std::unordered_set<std::uint32_t> s_dumped;   // per-window once-per-actor latch

        const float cur  = JTrackNow();
        const bool  edge = cur > 0.5f && s_lastVal <= 0.5f;   // the handBoxDumpNow edge idiom
        s_lastVal = cur;
        if (edge) {
            s_armed  = true;
            s_edgeAt = std::chrono::steady_clock::now();
            s_dumped.clear();
            logger::info("JTRACK armed via jtrackNow knob — dumping every driven actor seen in the next ~2 s "
                         "(set jtrackNow back to 0, then non-0, to re-fire)");
        }
        if (!s_armed) return;
        if (std::chrono::steady_clock::now() - s_edgeAt > std::chrono::seconds(2)) {
            s_armed = false;                                  // window over — disarm until the next edge
            s_dumped.clear();
            return;
        }
        if (!actor) return;
        if (s_dumped.insert(actor->GetFormID()).second)       // once per actor per window
            PivJointTrackDump(actor);
    }
}
