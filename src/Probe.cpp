#include "PCH.h"
#include "Probe.h"
#include "Tuning.h"    // ObjectHold::HeelFixEnabled — heel context
#include "PPBHook.h"   // ArmIK::IsStatueFlagged — statue context
#include "PivFix.h"    // ObjectHold::PivReadJointLocal — the loaded-pivot dump (bake-mapping diagnosis)

#include <cmath>
#include <xmmintrin.h>   // _mm_store_ps

namespace logger = SKSE::log;

// ============================================================================
//  Probe — RE:: types only (no Havok SDK). All positions/velocities converted
//  to SKYRIM units for readability, matching every other PPB log line.
// ============================================================================
namespace {

    constexpr float kHavokToSkyrim = 1.0f / 0.0142875f;   // havok m -> skyrim u

    // hkpMotion::MotionType (RE/H/hkpMotion.h)
    const char* MotionTypeName(int t) {
        switch (t) {
        case 0: return "Invalid";
        case 1: return "Dynamic";
        case 2: return "SphereInertia";
        case 3: return "BoxInertia";
        case 4: return "Keyframed";
        case 5: return "Fixed";
        case 6: return "ThinBoxInertia";
        case 7: return "Character";
        default: return "?";
        }
    }
    // RE::KNOCK_STATE_ENUM (ActorState.h) — kGetUp=6 is the floating-NPC money value.
    const char* KnockStateName(int k) {
        switch (k) {
        case 0: return "Normal";
        case 1: return "Explode";
        case 2: return "ExplodeLeadIn";
        case 3: return "Out";
        case 4: return "OutLeadIn";
        case 5: return "Queued";
        case 6: return "GetUp";
        case 7: return "Down";
        case 8: return "WaitForTaskQueue";
        default: return "?";
        }
    }
    // RE::ACTOR_LIFE_STATE (ActorState.h) — remember kRestrained reads as "dead" via IsDead (ledger).
    const char* LifeStateName(int l) {
        switch (l) {
        case 0: return "Alive";
        case 1: return "Dying";
        case 2: return "Dead";
        case 3: return "Unconscious";
        case 4: return "Reanimate";
        case 5: return "Recycle";
        case 6: return "Restrained";
        case 7: return "EssentialDown";
        case 8: return "Bleedout";
        default: return "?";
        }
    }
    // RE::SIT_SLEEP_STATE (ActorState.h)
    const char* SitSleepName(int s) {
        switch (s) {
        case 0: return "Normal";
        case 1: return "WantToSit";
        case 2: return "WaitingForSitAnim";
        case 3: return "IsSitting/RidingMount";
        case 4: return "WantToStand";
        case 5: return "WantToSleep";
        case 6: return "WaitingForSleepAnim";
        case 7: return "IsSleeping";
        case 8: return "WantToWake";
        default: return "?";
        }
    }
    // RE::hkpCharacterStateType (hkpCharacterState.h; 5 = kUserState0 = Swimming)
    const char* CharStateName(int s) {
        switch (s) {
        case 0: return "OnGround";
        case 1: return "Jumping";
        case 2: return "InAir";
        case 3: return "Climbing";
        case 4: return "Flying";
        case 5: return "Swimming";
        default: return "UserState";
        }
    }
    // RE::hkpSurfaceInfo::SupportedState (hkpCharacterControl.h)
    const char* SupportedName(std::uint32_t s) {
        switch (s) {
        case 0: return "Unsupported";
        case 1: return "Sliding";
        case 2: return "Supported";
        default: return "?";
        }
    }
}

namespace Probe {

    void DumpActorState(RE::Actor* actor)
    {
        if (!actor) {
            logger::info("PROBE: no actor selected");
            return;
        }
        const std::uint32_t id = actor->GetFormID();
        auto* base = actor->GetActorBase();
        const char* nm = (base && base->GetFullName()) ? base->GetFullName() : "<unnamed>";

        logger::info("================ PROBE {:08X} \"{}\" ================", id, nm);

        // ── actor scale + LOADED constraint pivots, BOTH stored copies (2026-07-09:
        // the bake-mapping diagnosis — run with pivAutoSeat 0 BEFORE any statue so
        // these are the raw NIF-loaded values, not healed ones) ──
        logger::info("PROBE {:08X} GetScale={:.4f} refScale={}", id, actor->GetScale(),
                     actor->GetReferenceRuntimeData().refScale);
        {
            static constexpr const char* kJN[11] = { "wrist", "elbow", "shoulder", "spine0", "spine1",
                                                     "spine2", "neck", "head", "hipR", "kneeR", "ankleR" };
            for (int j = 0; j < 11; ++j) {
                float c[3]{}, o[3]{};
                const bool okC = ObjectHold::PivReadJointLocal(actor, j, true, c);
                const bool okO = ObjectHold::PivReadJointLocal(actor, j, false, o);
                if (okC || okO)
                    logger::info("PROBE PIVOT {:08X} {:<8} child=[{:.4f} {:.4f} {:.4f}]{} other=[{:.4f} {:.4f} {:.4f}]{} (game u)",
                                 id, kJN[j], c[0], c[1], c[2], okC ? "" : "!",
                                 o[0], o[1], o[2], okO ? "" : "!");
                else
                    logger::info("PROBE PIVOT {:08X} {:<8} unreadable (no constraint found)", id, kJN[j]);
            }
        }

        // ── Actor-level states ───────────────────────────────────────────────
        const int knock = static_cast<int>(actor->GetKnockState());
        const int life  = static_cast<int>(actor->GetLifeState());
        const int sit   = static_cast<int>(actor->GetSitSleepState());
        logger::info("PROBE {:08X} states: IsInRagdollState={} knock={}({}) life={}({}) sitSleep={}({}) killMove={}",
                     id, actor->IsInRagdollState(),
                     knock, KnockStateName(knock), life, LifeStateName(life), sit, SitSleepName(sit),
                     actor->IsInKillMove());

        // bAnimationDriven graph bool (ok=false means the variable is absent on this graph)
        {
            bool animDriven = false;
            const bool ok = actor->GetGraphVariableBool(RE::BSFixedString("bAnimationDriven"), animDriven);
            logger::info("PROBE {:08X} graph: bAnimationDriven={} (read {})", id, animDriven, ok ? "OK" : "FAILED/absent");
        }

        // Occupied furniture
        {
            auto handle = actor->GetOccupiedFurniture();
            std::uint32_t furnId = 0;
            const char* furnName = "";
            if (auto furn = handle.get()) {
                furnId = furn->GetFormID();
                furnName = furn->GetName() ? furn->GetName() : "";
            }
            logger::info("PROBE {:08X} furniture: occupied={:08X} \"{}\"", id, furnId, furnName);
        }

        // ── charController ground/support state ──────────────────────────────
        if (auto* cc = actor->GetCharController()) {
            const int state = static_cast<int>(cc->context.currentState);
            const auto sup = cc->surfaceInfo.supportedState.underlying();
            alignas(16) float sn[4];
            _mm_store_ps(sn, cc->surfaceInfo.surfaceNormal.quad);
            logger::info("PROBE {:08X} charCtrl: state={}({}) supported={}({}) surfNormal=[{:.2f} {:.2f} {:.2f}] "
                         "surfDynamic={} fallStartH={:.1f} fallTime={:.2f} supportBody={} flags=0x{:08X}",
                         id, state, CharStateName(state), sup, SupportedName(sup),
                         sn[0], sn[1], sn[2],
                         cc->surfaceInfo.surfaceIsDynamic,
                         cc->fallStartHeight, cc->fallTime,
                         cc->supportBody.get() ? "SET" : "null",
                         cc->flags.underlying());
        } else {
            logger::info("PROBE {:08X} charCtrl: NONE", id);
        }

        // ── Heel / statue context (the floating bug is heel-fix-adjacent) ────
        {
            float heelZ = 0.f;
            bool haveNode = false;
            if (auto* root = actor->Get3D()) {
                if (auto* npcNode = root->GetObjectByName("NPC")) {
                    heelZ = npcNode->local.translate.z;   // the RaceMenu heel offset lives HERE
                    haveNode = true;
                }
            }
            logger::info("PROBE {:08X} heel: NPC-node z={:.2f}u{} heelFixEnabled={} statue={}",
                         id, heelZ, haveNode ? "" : " (node MISSING)",
                         ObjectHold::HeelFixEnabled(), ArmIK::IsStatueFlagged(id));
        }

        // ── Per-ragdoll-body dump (12 R/center + 6 L twins) ──────────────────
        static constexpr const char* kProbeNodes[18] = {
            "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Head [Head]",
            "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
            "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC COM [COM ]",
            "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]",
            "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]",
        };
        auto* root = actor->Get3D();
        if (!root) {
            logger::info("PROBE {:08X} bodies: no 3D loaded", id);
            logger::info("================ PROBE {:08X} end ================", id);
            return;
        }
        int found = 0;
        for (const char* node : kProbeNodes) {
            auto* obj = root->GetObjectByName(RE::BSFixedString(node));
            if (!obj) { logger::info("PROBE {:08X} body '{}': node MISSING", id, node); continue; }
            auto* colObj = obj->collisionObject.get();
            auto* body = colObj ? static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody() : nullptr;
            auto* hkp  = body ? body->GetRigidBody() : nullptr;
            if (!hkp) { logger::info("PROBE {:08X} body '{}': no rigid body", id, node); continue; }
            ++found;
            const int mt = static_cast<int>(hkp->motion.type.get());
            alignas(16) float pos[4], lv[4];
            _mm_store_ps(pos, hkp->motion.motionState.transform.translation.quad);
            _mm_store_ps(lv, hkp->motion.linearVelocity.quad);
            const float px = pos[0] * kHavokToSkyrim, py = pos[1] * kHavokToSkyrim, pz = pos[2] * kHavokToSkyrim;
            const float vx = lv[0] * kHavokToSkyrim, vy = lv[1] * kHavokToSkyrim, vz = lv[2] * kHavokToSkyrim;
            const float spd = std::sqrt(vx*vx + vy*vy + vz*vz);
            logger::info("PROBE {:08X} body '{}': motion={}({}) vel=[{:.1f} {:.1f} {:.1f}] |v|={:.1f}u/s pos=[{:.1f} {:.1f} {:.1f}] worldZ={:.1f}",
                         id, node, mt, MotionTypeName(mt), vx, vy, vz, spd, px, py, pz, pz);
        }
        logger::info("PROBE {:08X} bodies: {}/18 with rigid bodies", id, found);
        logger::info("================ PROBE {:08X} end ================", id);
    }
}
