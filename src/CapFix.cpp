#include "PCH.h"
#include "CapFix.h"
#include "Tuning.h"    // ObjectHold::CapFixGen/CapFixSlot/CapFixChildSlot/CapFixSet/CapAutoFitEnabled/CapMirrorLEnabled
#include "PivFix.h"    // ObjectHold::PivReadJointLocal — the joint-ball reader for the capsule auto-fit
#include "Interop.h"   // Interop::IsActorGrabbedByPlayer — the auto-fit grab gate (2026-07-07)
#include "NpcFingerTest.h"   // NpcFinger::UpdateMeshMarkers — the Route B band markers (2026-07-18)
#include "DismemberGuard.h"  // IsExcluded — the dead/dismembered latch gate (2026-07-28)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <xmmintrin.h>   // _mm_store_ps

namespace logger = SKSE::log;

// ============================================================================
//  CapFix — the live capsule editor. RE:: types only (no Havok SDK in this TU).
//  Units: Havok stores positions in metres (1 Skyrim unit = 0.0142875 m). All
//  logged positions/lengths are converted to SKYRIM UNITS for readability.
// ============================================================================
namespace {

    constexpr float kHavokToSkyrim = 1.0f / 0.0142875f;   // havok m -> skyrim u
    constexpr float kSkyrimToHavok = 0.0142875f;          // skyrim u -> havok m

    // ---- node lookup --------------------------------------------------------
    RE::NiAVObject* FindNode(RE::Actor* actor, const char* name) {
        if (!actor || !name) return nullptr;
        auto* root = actor->Get3D();
        if (!root) return nullptr;
        return root->GetObjectByName(RE::BSFixedString(name));
    }
}

namespace GrabDiag {

    // Upper bound on bhkListShape children we will drill into. Raised from a hardcoded 8 (2026-07-08):
    // the wave-2 torso bake gives COM 21 children (spine0/1/2 get 11 each), so the old `n > 8` plausibility
    // gate would have silently SKIPPED the whole COM list — mains included. 24 leaves headroom and stays
    // far below Havok's own caps (hkpListShape::kMaxChildrenForSPUMidPhase = 252).
    static constexpr int kMaxListChildren = 24;

    // ---- slot tables (file-scope: the gen sweep, the list path and the L mirror all use them) ----
    static constexpr const char* kSlotNode[12] = {
        "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Head [Head]",
        "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
        "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC COM [COM ]",
    };
    static constexpr const char* kSlotName[12] = { "hand", "forearm", "upperarm", "head",
        "spine0", "spine1", "spine2", "neck", "thighR", "calfR", "footR", "com" };
    // LEFT twins (2026-07-07 live mirror; names verified in the deployed NIF — "Lft " keeps the
    // trailing space like "Rft "). Center slots (head/spine0-2/neck/com) have no twin -> nullptr.
    static constexpr const char* kSlotNodeL[12] = {
        "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]", nullptr,
        nullptr, nullptr, nullptr, nullptr,
        "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]", nullptr,
    };

    // Shared accessor: a named node's live collision capsule (null if absent / not a capsule).
    static RE::hkpCapsuleShape* GetNodeCapsule(RE::Actor* actor, const char* nodeName, int* typeOut = nullptr)
    {
        auto* hn = FindNode(actor, nodeName);
        if (!hn) return nullptr;
        auto* colObj = hn->collisionObject.get();
        if (!colObj) return nullptr;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        auto* hkp  = body ? body->GetRigidBody() : nullptr;
        auto* col  = hkp ? hkp->GetCollidableRW() : nullptr;
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape) return nullptr;
        if (typeOut) *typeOut = static_cast<int>(shape->type);
        if (shape->type != RE::hkpShapeType::kCapsule) return nullptr;
        return const_cast<RE::hkpCapsuleShape*>(static_cast<const RE::hkpCapsuleShape*>(shape));
    }

    // Same chain from an ALREADY-RESOLVED node (no name search, no string interning) — the per-frame
    // finger writer calls this 8x/frame/actor. NEVER allocates.
    static RE::hkpCapsuleShape* GetCapsuleOnNode(RE::NiAVObject* node)
    {
        if (!node) return nullptr;
        auto* colObj = node->collisionObject.get();
        if (!colObj) return nullptr;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        auto* hkp  = body ? body->GetRigidBody() : nullptr;
        auto* col  = hkp ? hkp->GetCollidableRW() : nullptr;
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape || shape->type != RE::hkpShapeType::kCapsule) return nullptr;
        return const_cast<RE::hkpCapsuleShape*>(static_cast<const RE::hkpCapsuleShape*>(shape));
    }

    // Same chain, list-shape variant (null if absent / not a bhkListShape). NEVER allocates.
    static RE::hkpListShape* GetNodeListShape(RE::Actor* actor, const char* nodeName)
    {
        auto* hn = FindNode(actor, nodeName);
        if (!hn) return nullptr;
        auto* colObj = hn->collisionObject.get();
        if (!colObj) return nullptr;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        auto* hkp  = body ? body->GetRigidBody() : nullptr;
        auto* col  = hkp ? hkp->GetCollidableRW() : nullptr;
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape || shape->type != RE::hkpShapeType::kList) return nullptr;
        return const_cast<RE::hkpListShape*>(static_cast<const RE::hkpListShape*>(shape));
    }

    // Raw capsule float write (endpoints/radius in GAME units — converted here). Pure float edits,
    // the proven-safe class; shape TYPE is never touched.
    // ── BODY MATERIAL STAMP (2026-07-18, the "ground-hitting noise" fix — scope: every capsule
    // CapFix resolves, i.e. all BAKED-FEMALE bodies. Males keep NIF skin; mixed male-female
    // contacts still go quiet because the grass side of the pair falls to the soft entries.) ──
    // The engine plays contact sounds via a material-PAIR BGSImpactDataSet lookup on the two bhk
    // wrapper materialIds (NpcFingerTest.cpp:215 — the garment fix proved the mechanism in-game).
    // NIF-loaded capsules carry MaterialSkin -> the body-fall dirt THUD on every touch, absurd now
    // that ragdolls are always on. Offsets byte-validated in the Port structs: hkpShape userData
    // @0x10 -> bhk wrapper, wrapper materialId @0x20. npcBodyMat selector matches npcGarmentMat
    // (0 = leave the NIF material / skin, 1 cloth, 2 snow, 3 grass DEFAULT, 4 none).
    static void StampBodyMaterial(RE::hkpCapsuleShape* cap)
    {
        if (!cap) return;
        std::uint32_t mat;
        switch (static_cast<int>(ObjectHold::NpcBodyMat() + 0.5f)) {
        case 0:  return;                       // skin/NIF — leave untouched
        case 1:  mat = 0xE4D39CA3u; break;     // MaterialCloth
        case 2:  mat = 0x17C77AAFu; break;     // MaterialSnow
        case 4:  mat = 0u;          break;     // none
        default: mat = 0x6E2F68EEu; break;     // MaterialGrass (direction-proof near-silent)
        }
        void* wrap = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(cap) + 0x10);
        if (!wrap) return;
        auto* mid = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(wrap) + 0x20);
        if (*mid != mat) *mid = mat;
    }

    static void WriteCapsule(RE::hkpCapsuleShape* cap, const float a[3], const float b[3], float r)
    {
        StampBodyMaterial(cap);   // every written capsule (mains, children, left mirrors) goes quiet
        cap->vertexA.quad = _mm_set_ps(0.f, a[2] * kSkyrimToHavok, a[1] * kSkyrimToHavok, a[0] * kSkyrimToHavok);
        cap->vertexB.quad = _mm_set_ps(0.f, b[2] * kSkyrimToHavok, b[1] * kSkyrimToHavok, b[0] * kSkyrimToHavok);
        cap->radius = r * kSkyrimToHavok;
    }

    // The VERIFIED left-mirror transform (bake script + NIF dump agree): every L/R capsule pair is
    // an exact X-negation in body-local space — { -AX, AY, AZ }, { -BX, BY, BZ }, same radius.
    static void MirrorVals(const float a[3], const float b[3], float am[3], float bm[3])
    {
        am[0] = -a[0]; am[1] = a[1]; am[2] = a[2];
        bm[0] = -b[0]; bm[1] = b[1]; bm[2] = b[2];
    }

    // Union-AABB repatch (MANDATORY after any list-child write): hkpListShape caches
    // aabbCenter/aabbHalfExtents at construction and GetAabbImpl serves broadphase from the cache —
    // a child moved/grown past the baked envelope silently MISSES collisions at its far end.
    // Recompute over ALL live children (havok units, shape-local); pure float writes.
    static void RepatchListAabb(RE::hkpListShape* list)
    {
        const std::int32_t n = list->childInfo.size();
        float mn[3] = {  1e9f,  1e9f,  1e9f };
        float mx[3] = { -1e9f, -1e9f, -1e9f };
        std::int32_t counted = 0;
        for (std::int32_t i = 0; i < n; ++i) {
            const RE::hkpShape* ch = list->childInfo[i].shape;
            if (!ch || ch->type != RE::hkpShapeType::kCapsule) continue;
            auto* cap = static_cast<const RE::hkpCapsuleShape*>(ch);
            alignas(16) float va[4], vb[4];
            _mm_store_ps(va, cap->vertexA.quad);
            _mm_store_ps(vb, cap->vertexB.quad);
            const float cr = cap->radius;
            for (int k = 0; k < 3; ++k) {
                mn[k] = (std::min)({ mn[k], va[k] - cr, vb[k] - cr });
                mx[k] = (std::max)({ mx[k], va[k] + cr, vb[k] + cr });
            }
            ++counted;
        }
        if (counted > 0) {
            list->aabbCenter.quad      = _mm_set_ps(0.f, (mn[2]+mx[2])*0.5f, (mn[1]+mx[1])*0.5f, (mn[0]+mx[0])*0.5f);
            list->aabbHalfExtents.quad = _mm_set_ps(0.f, (mx[2]-mn[2])*0.5f, (mx[1]-mn[1])*0.5f, (mx[0]-mn[0])*0.5f);
        }
    }

    // ── LIVE LEFT-MIRROR (capMirrorL, 2026-07-07) — single-value flavor: mirror one RIGHT-side
    // slot write onto the LEFT twin body. Graceful for every live L shape: single capsule ->
    // mirrored write (radiusOnly for the auto-fit-owned radius branch); bhkListShape -> mirrored
    // write into child 0 (the MAIN capsule) + the L list's OWN union-AABB repatch; body/shape
    // missing -> log + skip. NEVER allocates.
    static void MirrorSlotToLeft(RE::Actor* actor, std::uint32_t id, int slot,
                                 const float a[3], const float b[3], float r, bool radiusOnly)
    {
        if (!ObjectHold::CapMirrorLEnabled()) return;
        const char* lNode = kSlotNodeL[slot];
        if (!lNode) return;                              // center slot — no twin
        int lType = -1;
        if (auto* lcap = GetNodeCapsule(actor, lNode, &lType)) {
            if (radiusOnly) {
                StampBodyMaterial(lcap);   // 2026-07-18: radius-only path bypasses WriteCapsule
                lcap->radius = r * kSkyrimToHavok;
                logger::debug("CapFix {:08X} MIRROR-L {} radius={:.1f}", id, kSlotName[slot], r);
            } else {
                float am[3], bm[3];
                MirrorVals(a, b, am, bm);
                WriteCapsule(lcap, am, bm, r);
                logger::debug("CapFix {:08X} MIRROR-L {} A=[{:.1f} {:.1f} {:.1f}] B=[{:.1f} {:.1f} {:.1f}] r={:.1f}",
                             id, kSlotName[slot], am[0], am[1], am[2], bm[0], bm[1], bm[2], r);
            }
            return;
        }
        if (lType == static_cast<int>(RE::hkpShapeType::kList)) {
            // L twin already a list (post-bake): the slot values belong to child 0 (the main line).
            auto* list = GetNodeListShape(actor, lNode);
            if (!list || list->childInfo.size() < 1) return;
            const RE::hkpShape* ch = list->childInfo[0].shape;
            if (!ch || ch->type != RE::hkpShapeType::kCapsule) {
                logger::info("CapFix {:08X} MIRROR-L {}: L list child 0 not a capsule — skipped", id, kSlotName[slot]);
                return;
            }
            auto* lcap = const_cast<RE::hkpCapsuleShape*>(static_cast<const RE::hkpCapsuleShape*>(ch));
            if (radiusOnly) {
                StampBodyMaterial(lcap);   // 2026-07-18: radius-only path bypasses WriteCapsule
                lcap->radius = r * kSkyrimToHavok;
            } else {
                float am[3], bm[3];
                MirrorVals(a, b, am, bm);
                WriteCapsule(lcap, am, bm, r);
            }
            RepatchListAabb(list);
            logger::debug("CapFix {:08X} MIRROR-L {} (into L list child 0){}", id, kSlotName[slot],
                         radiusOnly ? " radius-only" : "");
            return;
        }
        logger::info("CapFix {:08X} MIRROR-L {}: no L capsule (shape type {}) — skipped", id, kSlotName[slot], lType);
    }

    // ── LIVE LEFT-MIRROR — list flavor: replay the RIGHT list-child writes onto the LEFT twin.
    // L still a single capsule (pre-bake, e.g. today's L hand): the old dialed single capsule WAS
    // the main/center line, so it receives the mirrored child-0 values (the 07-03 hand history).
    // L a list: mirror per child (skipping children the L list doesn't have) + repatch ITS cached
    // union AABB. L missing: log + skip. NEVER allocates.
    static void MirrorListToLeft(RE::Actor* actor, std::uint32_t id, int slot,
                                 const bool  wroteChild[kMaxListChildren],
                                 const float wa[kMaxListChildren][3],
                                 const float wb[kMaxListChildren][3],
                                 const float wr[kMaxListChildren])
    {
        if (!ObjectHold::CapMirrorLEnabled()) return;
        const char* lNode = kSlotNodeL[slot];
        if (!lNode) return;
        int lType = -1;
        if (auto* lcap = GetNodeCapsule(actor, lNode, &lType)) {
            StampBodyMaterial(lcap);                     // 2026-07-18: stamp even when geometry is skipped
            if (!wroteChild[0]) return;                  // main line untouched — nothing to mirror
            float am[3], bm[3];
            MirrorVals(wa[0], wb[0], am, bm);
            WriteCapsule(lcap, am, bm, wr[0]);
            logger::debug("CapFix {:08X} MIRROR-L {}: L is a single capsule — mirrored the main line "
                         "A=[{:.1f} {:.1f} {:.1f}] B=[{:.1f} {:.1f} {:.1f}] r={:.1f}",
                         id, kSlotName[slot], am[0], am[1], am[2], bm[0], bm[1], bm[2], wr[0]);
            return;
        }
        auto* list = GetNodeListShape(actor, lNode);
        if (!list) {
            logger::info("CapFix {:08X} MIRROR-L {}: no L body/shape (type {}) — skipped", id, kSlotName[slot], lType);
            return;
        }
        const std::int32_t n = list->childInfo.size();
        // 2026-07-18 sound fix (review): stamp EVERY L child, incl. baked/disabled ones the
        // geometry replay skips — material is not geometry; the L twin must match the R's
        // stamp coverage (else the right hip goes quiet and the left still thuds).
        for (std::int32_t i2 = 0; i2 < n; ++i2) {
            const RE::hkpShape* ch2 = list->childInfo[i2].shape;
            if (ch2 && ch2->type == RE::hkpShapeType::kCapsule)
                StampBodyMaterial(const_cast<RE::hkpCapsuleShape*>(
                    static_cast<const RE::hkpCapsuleShape*>(ch2)));
        }
        bool wrote = false;
        for (int i = 0; i < kMaxListChildren; ++i) {
            if (!wroteChild[i]) continue;
            if (i >= n) {
                logger::info("CapFix {:08X} MIRROR-L {}: L list has only {} children — child {} skipped",
                             id, kSlotName[slot], n, i);
                continue;
            }
            const RE::hkpShape* ch = list->childInfo[i].shape;
            if (!ch || ch->type != RE::hkpShapeType::kCapsule) {
                logger::info("CapFix {:08X} MIRROR-L {}: L child {} not a capsule — skipped", id, kSlotName[slot], i);
                continue;
            }
            auto* lcap = const_cast<RE::hkpCapsuleShape*>(static_cast<const RE::hkpCapsuleShape*>(ch));
            float am[3], bm[3];
            MirrorVals(wa[i], wb[i], am, bm);
            WriteCapsule(lcap, am, bm, wr[i]);
            wrote = true;
        }
        if (!wrote) return;
        RepatchListAabb(list);                           // the L list's OWN cache
        logger::debug("CapFix {:08X} MIRROR-L {} (list children mirrored + L AABB repatched)", id, kSlotName[slot]);
    }

    // ── generalized bhkListShape per-child tuning (2026-07-07; grown from the 07-03 3-capsule hand
    // original). Child index mapping: hand (slot 0) keeps C1..C3 = children 0..2; the flesh-fit slots
    // (fore/upper/thigh/calf/foot) put the MAIN capsule at child 0 — still driven by the SLOT knobs —
    // and the ADDED capsules at children 1..N (cap<Slot>C1..CN). The children are ordinary
    // hkpCapsuleShapes — the proven float-edit class. TODAY only the R hand is a list in the NIF;
    // the flesh-fit bake (parallel task) converts the limb bodies. Until then those bodies are single
    // capsules and never reach this path. A list with fewer children than knobs logs + skips the
    // extras — no crash, NEVER allocate shapes at runtime.
    // 2026-07-12 SCALE TERM (the conformer's scale axis, proven on Lydia at 0.8): knob
    // geometry is authored for scale 1; the engine scales the NIF-baked shapes at ragdoll
    // build, so knob WRITES must match — multiply endpoints AND radius by the actor's live
    // scale. Guarded to a sane band; 1.0 outside it.
    // Latched MEASURED effective ragdoll scale per actor (2026-07-13): written by the
    // 1 Hz span sampler, read here. Replaces record-based guessing entirely once latched
    // (the Oakwood lesson: two same-record NPCs got ragdolls built at different scales).
    static std::unordered_map<std::uint32_t, float> s_effScale;

    // The RAW measured true visual scale (0.5*(medHead/g_refHeadSpan + medCalf/g_refCalfSpan)), latched
    // at the same 10-sample point as s_effScale but WITHOUT the capsule-resize buffer collapse. s_effScale
    // is a CAPSULE decision (a within-2%-buffer NPC is stored as GetScale to avoid resize churn); the PivFix
    // JOINT correction must instead see the actual measured scale, because a mis-scaled NPC can read
    // trueScale ~= GetScale yet still have joints far off the nodes. Read via GrabDiag::MeasuredScaleOf.
    static std::unordered_map<std::uint32_t, float> s_trueScale;

    // ── MEASURED-SCALE REFERENCE (2026-07-14, user's design — Lydia is the standard) ──
    // Lydia's XP32 node spacing COM->head and COM->calf, NORMALIZED to scale 1, is the
    // "scale 1" reference. Captured LIVE from the reference actor (measuredScaleRefId).
    // For any other NPC: their world span / the scale-1 reference = their TRUE visual
    // scale — which the engine can MISREPORT (GetScale "lies"; a base-0.95 NPC whose
    // skeleton is really at 1.0). We resize her Havok body to that true scale so the
    // joints land on the nodes. Using XP32 NODE distances (not ragdoll body ORIGINS,
    // which sit at the parent joint) is the fix that makes the scale-1 master read a
    // true 1.0 instead of the old body-origin 1.033 that inflated her.
    // ★ BAKED scale-1 reference (2026-07-14, user directive): Lydia's XP32 COM->head / COM->calf NODE
    // spacing at scale 1 — fixed skeleton geometry, measured ONCE and hardcoded, NEVER re-seeded per
    // session. The old design captured this live from Lydia every launch and silently no-op'd the WHOLE
    // measured-scale system on every NPC whenever she wasn't measured (her follower AI reads "not attached").
    // Flip `refCapNow` while standing next to Lydia to read her exact spans off the log (deterministic,
    // bypasses the flaky sampler), then hardcode the logged numbers here.
    // Estimate until refCapNow confirms: head ~51.6u. The calf value is UNCONFIRMED (do not trust 62.3 —
    // it may be the ankle, not the calf node) — run refCapNow and replace before relying on the scale.
    // ★ BAKED (2026-07-14): the CONFIRMED Lydia reference (weight-100 zero-slider scale-1, measured LIVE)
    // COM->head = 50.954u, COM->L Calf = 36.825u @scale1 — the fixed scale-1 skeleton spans. Baked ONLY
    // now that a correct, IF-gated (>1u), one-shot JOINT correction (PivScaleCorrect) rides alongside the
    // capsule resize, so the collision capsules and the Havok joints move together (the capsule-vs-joint
    // mismatch that broke the normal case is closed). Normal NPCs are left untouched by BOTH: their
    // trueScale sits inside the ~2% capsule buffer (no resize) AND their joints sit <1u off the nodes
    // (PivScaleCorrect passes them). Only the mis-scaled / engine-lies actors are corrected.
    // Re-baselined 2026-07-14: Lydia DRESSED + IDLE (was naked+A-pose 50.954/36.825). Verified 1:1 first —
    // JTRACK on her raw ragdoll (re-scale OFF, reloaded) showed all 17 joints on their nodes (mean 0.26u,
    // max 0.54u), so these spans are a clean scale-1 baseline. Sampler agreed (0.9701 vs JTRACK 0.9741).
    static float g_refHeadSpan = 49.914f;
    static float g_refCalfSpan = 35.673f;

    // Per-actor apply/identity state for CapFixApply (2026-07-13: hoisted from a
    // function-local static so a load screen can CLEAR it — the audit's save/load
    // latch leak: loading a DIFFERENT save in-session let a recycled FormID inherit
    // the previous save's latched scale/identity for ~10s).
    struct Applied {
        unsigned gen = 0;
        const void* bodyId[18] = {};
        std::chrono::steady_clock::time_point lastProbe{};
        // MEASURED-SCALE (2026-07-13, user design): 1 sample per identity probe (1 Hz),
        // ratio = bodySpan/nodeSpan averaged over torso+leg; at 10 samples the MEAN
        // ratio x the node world scale latches as the actor's effective ragdoll build
        // scale — it replaces every record-based guess (the engine can lie about an
        // NPC's scale; the tape measure can't). Reset on ragdoll rebuild (re-measure).
        float  headSamples[10]{}; // COM->head WORLD spans — MEDIAN-of-10 (17A-26/27: median kills
        float  calfSamples[10]{}; // COM->calf WORLD spans   one-frame warp/stagger outliers)
        int    ratioN   = 0;
        float  effScale = 0.f;    // 0 = not latched yet -> CapScaleOf falls back to GetScale
        float  scaleAtLatch = 0.f;// root world.scale at latch time — the live-SetScale drift edge
        float  sampleScale0 = 0.f;// root scale at sample 0 — discard buffer if scale drifts MID-collection
        int    attachedTicks = 0; // consecutive attached probe ticks — sampler debounce
        bool   wasAttached = true; // furniture gate edge: false->true = stood up -> RE-MEASURE
    };
    static std::unordered_map<std::uint32_t, Applied> s_applied;

    // ── BAKE GATE (2026-07-15) — "does this actor carry OUR female bake?" Our bake converts the COM
    // ragdoll body to a bhkListShape (21 children); every STOCK skeleton — male, undead/draugr, and
    // custom-skeleton females (Teen Dolls / Unslaad) that never load skeleton_female.nif — keeps a single
    // bhkCapsuleShape COM. List-shapes on ragdoll bodies are UNPRECEDENTED in vanilla (Report 07: 0 across
    // 19,057 NIFs + 127 loose skeletons), so a list-COM <=> our bake, essentially 100% in this load order.
    // This gate stops the GEOMETRY writers (the arc re-scale in PivFix + the capsule fitter here) from
    // deforming bodies we never baked — the male/skeleton "re-scaled + capsules shrunk" bug. Cached per
    // FormID; erased on ragdoll rebuild (werewolf/VL skeleton swap) and on load.
    static std::unordered_map<std::uint32_t, int> s_carriesBake;   // 0 = unknown/retry, 1 = yes, 2 = no

    static float CapScaleOf(RE::Actor* actor) {
        // ── ARC trueScale (2026-07-15, Option B — full scaling solution). The capsules ride the SAME
        // pose-invariant, reference-free scale the arc re-scale gives the JOINTS: nodeArc / .nif scale-1
        // base, latched by PivScaleCorrect. Untainted (no Lydia reference) and immune to the "engine lies
        // about scale" case — so joints and capsules stay in lock-step. Falls back to GetScale until the
        // arc is latched (a far/unsettled NPC PivScaleCorrect hasn't measured yet).
        if (actor) {
            const float arc = ObjectHold::PivArcTrueScaleOf(actor);
            if (arc > 0.3f && arc < 3.f) return arc;
        }
        // (2026-07-15) Legacy effScale tape RETIRED from the capsule fallback (user directive — "not
        // needed"). The pose-invariant arc supersedes it entirely; before the arc latches we fall back to
        // stable GetScale (the noisy tape was what caused the "too big on walk-in" transient + misleading
        // "resizing" logs). s_effScale is no longer read here; the sampler is knob-gated off (below).
        const float s = actor ? actor->GetScale() : 1.f;   // fallback (arc not latched yet)
        return (s > 0.05f && s < 20.f) ? s : 1.f;
    }

    // The LATCHED true scale for the PivFix JOINT correction (PivScaleCorrect). This is the RAW span
    // ratio (s_trueScale), NOT the capsule-buffered s_effScale: a within-buffer NPC is stored in
    // s_effScale as GetScale (a capsule decision), but the joint code must see the measured scale
    // regardless — a mis-scaled NPC can read trueScale ~= GetScale yet have joints far off the nodes.
    // Returns 0 when the actor is NOT measured yet — the constraint pivots are LEFT ALONE until the true
    // scale is known (never fall back to GetScale, the value we're correcting away from). Plausibility-
    // bounded 0.3..3. EXCEPTION: the reference actor is scale-1 BY DEFINITION (never trust a live
    // GetScale — mods could scale her), so her joints are judged against a clean 1.0.
    float MeasuredScaleOf(RE::Actor* actor) {
        if (!actor || !ObjectHold::MeasuredScaleEnabled()) return 0.f;
        const std::uint32_t refId = static_cast<std::uint32_t>(static_cast<std::int64_t>(ObjectHold::MeasuredScaleRefId()));
        if (refId != 0 && actor->GetFormID() == refId) return 1.f;
        auto it = s_trueScale.find(actor->GetFormID());
        if (it != s_trueScale.end() && it->second > 0.3f && it->second < 3.f) return it->second;
        return 0.f;
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════
    //  BODY SCALE (2026-07-14) — size the collision CAPSULES to an NPC's body SHAPE. A THIRD system,
    //  SEPARATE from the uniform re-scale (CapScaleOf/effScale) and from the JOINT re-scale
    //  (PivScaleCorrect). It NEVER touches a ragdoll joint/pivot: it only supplies a per-region
    //  MULTIPLIER that rides on top of CapScaleOf through the EXISTING scale-from-joint apply, so every
    //  capsule endpoint/radius edit still flows through the same `a*=cs; b*=cs; r*=cs; WriteCapsule`
    //  path (spec A). Two data sources: BODY = OBody/SKEE named-morph net deviations (Interop::
    //  GetRegionMorph, O(1)); HEAD = a cheap head-mesh bounding-extent measure (vertex fallback),
    //  once + cached. One-shot per NPC on 3D-load + settled; factors/clamps read live at apply.
    // ════════════════════════════════════════════════════════════════════════════════════════════

    // Reference head size, captured LIVE from the reference actor (Lydia, measuredScaleRefId) — the
    // head sculpt is NOT a morph, so it has no zero-slider base; her mesh IS the base. Two flavours,
    // compared like-with-like: g_refHeadDepth = isotropic morphed-vertex max-extent (PRIMARY path);
    // g_refHeadDiam = 2*modelBound.radius (bound FALLBACK). 0 = uncaptured -> head ratio 1.0 until she
    // is measured once.  (Reordered 2026-07-14: vertex is primary — see BodyScaleLatch.)
    // NOTE these are ORTHOGONAL to g_refHeadSpan (CapFix.cpp:311), which is a COM->head JOINT distance
    // (a body-height proxy for the re-scale) — NOT a head-shape measure. Do not conflate them.
    static float g_refHeadDiam  = 0.f;
    static float g_refHeadDepth = 0.f;

    // Per-NPC cache: RAW reads (not baked ratios) so the live factor/clamp knobs apply at read time.
    struct RegionData {
        float net[BodyScale::kMorphRegionCount] = {};  // signed morph deviations (chest..upperArms)
        float headRaw = 0.f, headRef = 0.f;            // head measure + the matching reference
        int   headSrc = 0;                             // 0 none / 1 bound(diam) / 2 verts(depth)
        bool  latched = false;
        // ROUTE B (2026-07-18, review-reworked): RAW sampled girths (0 = that band failed) —
        // ratios resolve at READ time against the live meshNeutral*/clamp/master knobs, honoring
        // the struct's own raw-reads doctrine. Per-REGION validity: one failed band no longer
        // drops the whole actor to slider mode.
        float meshGirth[BodyScale::kMorphRegionCount] = {};
        float meshBreastZ = 0.f;   // breast front-mass Z RELATIVE to the spine2 bind pos (2026-07-20;
                                   // negative = below the joint; 0 = invalid/uncaptured)
        float meshBreastCup = 0.f; // CUP: breast front z-extent (vertical size, u)
        float meshBreastAbsY = 0.f;// breast mound Y, SPINE2-local — the forward-shift driver
        float meshButtAbsY  = 0.f; // cheek  mound Y, COM-local    — the backward-shift driver
        int   meshVC = 0;          // sampled mesh vertex count = the body-type FINGERPRINT
        bool  meshSampled = false;
        // ★ UV LANDMARKS (2026-07-23): this actor's seven anatomical points, BONE-LOCAL, in
        // scale-free base-mesh units — the same space the capsule knobs live in BEFORE the es
        // multiply. THE measurement source for ReShape; nothing else feeds shape any more.
        float lmPos[11][3] = {};
        bool  lmOk[11]     = {};
        float lmButt[3]   = {};   // convenience alias of lmPos[kLmButt]
        bool  lmButtOk    = false;
        // derived breast drivers (see the neutral-shape record for the definitions)
        float lmBreastDist = 0.f, lmCup = 0.f, lmSag = 0.f;
        bool  lmBreastOk   = false;
        // HEAD landmarks, head-node-local (head bind is effectively unrotated)
        float lmNose[3] = {};
        bool  lmNoseOk  = false;
        float lmChin[3] = {};
        bool  lmChinOk  = false;
    };
    static std::unordered_map<std::uint32_t, RegionData> s_regionRatio;
    // OBody push-event queue (2026-07-18): the Obody_ApplyMorph sink (main.cpp) runs on the Papyrus
    // VM thread and must never touch s_regionRatio directly — it queues formIDs here; the main-
    // thread 1 Hz sweep drains them (erase -> re-latch -> freshLatch re-dress).
    static std::mutex g_bsInvalMx;
    static std::vector<std::uint32_t> g_bsInval;

    // ── HEAD-MESH measure (spec C), fully guarded (AIHands flagged dynamicData reads as CTD-prone) ──
    // ROOT-WALK finder (2026-07-14 fix): the FaceGen head BSDynamicTriShape is NOT parented under the
    // "NPC Head [Head]" BONE — it hangs off the actor-root facegen/skin node (skinned to head/jaw/eye
    // bones). The old head-bone-subtree recursion therefore returned null for EVERY actor (headSrc=0
    // everywhere in the log). Fix: walk the WHOLE Get3D() tree; read the head bone's world position as
    // an ANCHOR; pick the kDynamicTriShape whose worldBound.center is NEAREST the anchor, rejecting any
    // candidate > ~25u away (excludes the BODY dynamic trishape, centered at the torso/COM well below
    // the head). Immune to head-part parenting. Full-tree pattern: NpcFingerTest.cpp CensusWalk/HasTail.
    static RE::BSDynamicTriShape* FindHeadGeom(RE::Actor* actor)
    {
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return nullptr;
        auto* headBone = root->GetObjectByName(RE::BSFixedString("NPC Head [Head]"));
        if (!headBone) return nullptr;
        const RE::NiPoint3 anchor = headBone->world.translate;       // world-space head position
        constexpr float kMaxHeadDist2 = 25.f * 25.f;                 // reject anything > ~25u from the head bone
        RE::BSDynamicTriShape* best = nullptr;
        float bestD2 = kMaxHeadDist2;                                // start at the reject radius: any pick beats it
        // recurse the FULL scene graph; select by TYPE + nearest worldBound.center to the head anchor.
        struct Rec {
            static void Walk(RE::NiAVObject* obj, const RE::NiPoint3& anchor,
                             RE::BSDynamicTriShape*& best, float& bestD2, int depth) {
                if (!obj || depth > 40) return;
                if (auto* geom = obj->AsGeometry()) {
                    if (geom->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape) {
                        const RE::NiPoint3& c = geom->worldBound.center;
                        const float dx = c.x - anchor.x, dy = c.y - anchor.y, dz = c.z - anchor.z;
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < bestD2) { bestD2 = d2; best = static_cast<RE::BSDynamicTriShape*>(geom); }
                    }
                }
                if (auto* node = obj->AsNode()) {
                    for (auto& c : node->GetChildren())
                        if (auto* cp = c.get()) Walk(cp, anchor, best, bestD2, depth + 1);
                }
            }
        };
        Rec::Walk(root, anchor, best, bestD2, 0);
        return best;
    }

    // Bound-first: 2*modelBound.radius (isotropic size proxy). 0 if unusable. One pointer deref.
    static float HeadBoundDiam(RE::BSDynamicTriShape* g)
    {
        if (!g) return 0.f;
        const float r = g->GetModelData().modelBound.radius;
        return (std::isfinite(r) && r > 1.f) ? 2.f * r : 0.f;
    }
    // Morphed-vertex measure (now PRIMARY): ISOTROPIC size = the max box extent of the morphed CPU verts
    // (HIGGS precedent: 16-byte stride, first 3 floats = position). An isotropic max-extent (not a single
    // Y axis) is the only per-NPC-varying head number — modelBound.radius is the authored local bound,
    // near-constant per race. CAPPED + fully guarded — every index stays < n <= vertexCount.
    static float HeadVertDepthImpl(RE::BSDynamicTriShape* g)
    {
        if (!g) return 0.f;
        if (g->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape) return 0.f;   // never cast on type alone
        void* dd = g->GetDynamicTrishapeRuntimeData().dynamicData;
        const std::uint16_t vc = g->GetTrishapeRuntimeData().vertexCount;
        if (!dd || vc == 0) return 0.f;                                                 // pre-3D-load -> skip
        constexpr int kHeadVertCap = 2048;                                             // heads ~1-2k verts
        const int n = vc < kHeadVertCap ? static_cast<int>(vc) : kHeadVertCap;
        const float* v = static_cast<const float*>(dd);
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < n; ++i) {
            const float* p = v + static_cast<std::size_t>(i) * 4;                      // 4 floats/16 bytes
            for (int k = 0; k < 3; ++k) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
        }
        const float rx = mx[0] - mn[0], ry = mx[1] - mn[1], rz = mx[2] - mn[2];
        if (!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) return 0.f;
        float size = rx;                                                               // isotropic: largest box extent
        if (ry > size) size = ry;
        if (rz > size) size = rz;
        return size > 0.f ? size : 0.f;
    }
    // ★ SEH shield (2026-07-28 crash-2026-07-28-18-05-52): on the DECAP frame the engine destroys
    // the face geometry, so the BSDynamicTriShape found a moment earlier can be FREED-BUT-NONNULL —
    // every guard above then reads garbage that happens to pass (`dd` was 0x1'00003CAC). No pointer
    // check can win that race; only a hardware-fault net can. Both functions below are pure
    // float/pointer code (no C++ unwinding), which is what __try requires.
    static float HeadVertDepth(RE::BSDynamicTriShape* g)
    {
        __try { return HeadVertDepthImpl(g); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    }
    static float HeadBoundDiamSafe(RE::BSDynamicTriShape* g)
    {
        __try { return HeadBoundDiam(g); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    }

    // ══ ROUTE B: MEASURED trueShape (2026-07-18, Report 21 "ROUTE B GREENLIT") ═══════════════
    // Samples the LIVE morphed body mesh — BSDynamicTriShape dynamicData is the morphed PRE-SKIN
    // base mesh, so the measurement is pose-independent and race-scale-free (race scale lives in
    // the skeleton, never the base mesh). Girth = NEAR-MAX (8th-largest) radial distance from the
    // region's own axis — positions, never counts/averages (density-immune, Report 21 rule).
    // trueShape = girth / captured neutral: custom base meshes (Sofia/M'rissi), weight blend, and
    // OBody morphs are all measured in one read. Guarded like the head measure (same class).
    struct MeshGirths { float g[7]; int n[7]; float wpos[7][3]; float bz; float cup; };

    // ── ReShape v2 (bone-anchored) output + forward decl; implementation sits with the decode below.
    struct BoneBands {
        float g[BodyScale::kMorphRegionCount] = {};   // chest,breasts,belly,waist,butt,thighs,arms
        float cup  = 0.f;
        float sagZ = 0.f;   // mound mean Z relative to the spine2 bind — drives breast SAG
        float tipBrP[3]{}, tipBuP[3]{};  // the ACTUAL vertices called "nipple" / "cheek apex"
        bool  tipBrOk = false, tipBuOk = false;
        RE::BSTriShape* geo = nullptr;   // the sampled geometry — its world frame owns the verts
        float lmPos[11][3]{};            // UV landmarks, BONE-LOCAL (per the table's bone id)
        float lmMesh[11][3]{};           // same points in mesh space (for markers)
        float lmErr[11]{};               // UV lookup error; >0.02 = this body may not share the layout
        bool  lmOk[11]{};
        float brAbsY = 0.f; // breast mound Y in SPINE2-local space — drives the forward SHIFT
        float buAbsY = 0.f; // cheek  mound Y in COM-local space    — drives the backward SHIFT
        bool  ok   = false;
    };
    static bool SampleBoneBands(RE::Actor* actor, std::uint32_t id, BoneBands& out, bool verbose,
                                const RE::BSGeometry* exclude = nullptr);

    // ── v3 VERTEX SOURCE (2026-07-18c, the in-game FINDING #2 fix): VR bodies are STATIC
    // BSTriShapes — RaceMenu/OBody morphs regenerate the RAW RENDER VERTEX BUFFER; the
    // BSDynamicTriShape dynamicData surface is SE-facegen-only (the live candidate dump showed
    // only face parts as dynamic, all with null dynamicData). Read rawVertexData through the
    // vertexDesc layout: positions are HALF-FLOAT unless VF_FULLPREC (the HIGGS mesh-sampling
    // precedent). Both sources feed the same girth math through MeshPos().
    static inline float HalfToFloat(std::uint16_t h)
    {
        const std::uint32_t s = (h & 0x8000u) << 16;
        std::uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, f;
        if (e == 0) {
            if (!m) f = s;
            else { e = 113; while (!(m & 0x400u)) { m <<= 1; --e; } m &= 0x3FFu; f = s | (e << 23) | (m << 13); }
        } else if (e == 31) f = s | 0x7F800000u | (m << 13);
        else f = s | ((e + 112u) << 23) | (m << 13);
        float out; std::memcpy(&out, &f, 4); return out;
    }

    struct BodyMesh {
        RE::BSTriShape*     geo = nullptr;
        const std::uint8_t* data = nullptr;
        int  n = 0; int stride = 16; bool half = false; bool dynamic = false;
        RE::NiSkinInstance*  skinInst = nullptr;   // bones[] palette (bone-anchor test)
        RE::NiSkinPartition* skinPart = nullptr;   // partition[0].bones = local->global map
    };
    // SELF-CALIBRATING DECODE (2026-07-18e): the in-game probes showed vertexDesc cannot be
    // trusted blindly (declared half decode produced ±65k coordinate garbage). Decode 24 spread
    // verts under a candidate (stride, precision); accept only body-plausible output: finite,
    // |coord| < 500u, real z spread. The first interpretation that passes wins.
    static bool PlausibleDecode(const std::uint8_t* data, int n, int stride, bool half);
    static inline void MeshPos(const BodyMesh& bm, int i, float out[3])
    {
        const std::uint8_t* p = bm.data + static_cast<std::size_t>(i) * bm.stride;
        if (bm.half) {
            const std::uint16_t* h = reinterpret_cast<const std::uint16_t*>(p);
            out[0] = HalfToFloat(h[0]); out[1] = HalfToFloat(h[1]); out[2] = HalfToFloat(h[2]);
        } else {
            const float* f = reinterpret_cast<const float*>(p);
            out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
        }
    }
    static bool PlausibleDecode(const std::uint8_t* data, int n, int stride, bool half)
    {
        if (!data || n < 24 || stride < (half ? 8 : 12) || stride > 96) return false;
        float mnz = 1e9f, mxz = -1e9f;
        for (int k = 0; k < 24; ++k) {
            const int i = static_cast<int>(static_cast<std::int64_t>(k) * (n - 1) / 23);
            const std::uint8_t* p = data + static_cast<std::size_t>(i) * stride;
            float v[3];
            if (half) {
                const std::uint16_t* h = reinterpret_cast<const std::uint16_t*>(p);
                v[0] = HalfToFloat(h[0]); v[1] = HalfToFloat(h[1]); v[2] = HalfToFloat(h[2]);
            } else {
                const float* f = reinterpret_cast<const float*>(p);
                v[0] = f[0]; v[1] = f[1]; v[2] = f[2];
            }
            for (float c : v)
                if (!std::isfinite(c) || c < -500.f || c > 500.f) return false;
            if (v[2] < mnz) mnz = v[2]; if (v[2] > mxz) mxz = v[2];
        }
        return (mxz - mnz) > 5.f;                          // real geometry has vertical spread
    }

    // ★★ THE FULL-EXTENT GUARD (2026-07-21, CTD in Solitude — crash-2026-07-21-21-26-59).
    // Second access violation of the SAME class: movss xmm1,[r10+r9+0x04] deep inside the
    // FindBodyMesh recursion, reading a vertex off the end of a buffer. The 07-20 fix (ban
    // BSDynamicTriShape) was TOO NARROW — a plain STATIC BSTriShape can also report a
    // vertexCount larger than its real allocation, and PlausibleDecode only ever probes 24
    // spread verts near the head, which says NOTHING about the buffer's LENGTH (Ledger 10b,
    // written after the first crash and not fully applied — this is that lesson, enforced).
    // VirtualQuery is the only honest answer: ask the OS whether the ENTIRE n*stride extent is
    // committed and readable BEFORE anything walks it. No count from geometry data is trusted.
    static bool ExtentReadable(const void* base, std::size_t bytes)
    {
        if (!base || bytes == 0) return false;
        auto addr = reinterpret_cast<const std::uint8_t*>(base);
        std::size_t covered = 0;
        while (covered < bytes) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(addr + covered, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
            if (mbi.State != MEM_COMMIT) return false;
            constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
            if (mbi.Protect == 0 || (mbi.Protect & kNoRead)) return false;
            const auto regionEnd = reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            const std::size_t adv = static_cast<std::size_t>(regionEnd - (addr + covered));
            if (adv == 0) return false;
            covered += adv;
        }
        return true;
    }

    // Try every candidate interpretation of a raw buffer; first PLAUSIBLE one wins.
    static bool TryRawBuffer(const std::uint8_t* data, int n, int strideA, int strideB,
                             bool declaredHalf, BodyMesh& bm)
    {
        if (!data || n < 500) return false;
        const int  strides[2] = { strideA, strideB };
        const bool halves[2]  = { declaredHalf, !declaredHalf };
        for (int si = 0; si < 2; ++si) {
            const int st = strides[si];
            if (st <= 0 || (si == 1 && st == strides[0])) continue;
            // ★ FULL-EXTENT CHECK FIRST: the whole n*stride span must be committed+readable, or
            // the count is a lie and no amount of head-probing makes the walk safe (the Solitude
            // CTD). Cheap: one VirtualQuery per contiguous region, once per candidate mesh.
            if (!ExtentReadable(data, static_cast<std::size_t>(n) * static_cast<std::size_t>(st)))
                continue;
            for (bool hf : halves)
                if (PlausibleDecode(data, n, st, hf)) {
                    bm.data = data; bm.n = n; bm.stride = st; bm.half = hf; bm.dynamic = false;
                    return true;
                }
        }
        return false;
    }

    static bool ResolveMeshSource(RE::BSGeometry* geom, BodyMesh& bm, bool dynViaSkin = false)
    {
        const auto type = geom->GetType().get();
        if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kDynamicTriShape)
            return false;
        auto* tri = static_cast<RE::BSTriShape*>(geom);
        bm.geo = tri;
        const std::uint32_t vc = tri->GetTrishapeRuntimeData().vertexCount;
        // ★ DYNAMIC-TRISHAPE PATH DELETED (2026-07-20, CTD crash-2026-07-20-16-06-36): VR bodies
        // are NEVER BSDynamicTriShape (proven 2026-07-18 — morphs regenerate the STATIC buffer);
        // the only dynamic shapes are FACEGEN parts, and their vertexCount LIES (Auri's
        // "auriantlers" read 64,399) — PlausibleDecode probes only the buffer head, then the
        // full bbox pass walked ~1MB off the real allocation → EXCEPTION_ACCESS_VIOLATION at
        // PPB.dll+0x40914 inside the candidate scan. Excluding dynamic shapes entirely means the
        // scan can never touch a facegen buffer again.
        // ── 2026-07-23 HEAD EXCEPTION (dynViaSkin): the 19:08 dump showed every facegen part
        // (head, overlays, eyes, brows, mouth) rejected RIGHT HERE — they are dynamic trishapes,
        // and this guard fired before the SKINNED path below ever ran. The skinned path never
        // touches the lying vertexCount or the dynamic buffer: it uses the skin partition's OWN
        // count and OWN buffer, span-verified. So the head sampler may fall through to it, and
        // ONLY to it — the dynamic-buffer probe that caused the 07-20 CTD stays deleted.
        if (type == RE::BSGeometry::Type::kDynamicTriShape && !dynViaSkin)
            return false;
        const bool dynSkinOnly = (type == RE::BSGeometry::Type::kDynamicTriShape);
        auto& gr = geom->GetGeometryRuntimeData();
        if (auto* si0 = gr.skinInstance.get()) { bm.skinInst = si0; bm.skinPart = si0->skinPartition.get(); }
        if (auto* rd = gr.rendererData; !dynSkinOnly && rd && rd->rawVertexData && vc >= 500) {
            const int n = vc < 65535 ? static_cast<int>(vc) : 65535;
            auto vdG = gr.vertexDesc; auto vdR = rd->vertexDesc;
            if (TryRawBuffer(rd->rawVertexData, n,
                             static_cast<int>(vdR.GetSize()), static_cast<int>(vdG.GetSize()),
                             !vdR.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC), bm))
                return true;
        }
        // SKINNED path (2026-07-18d): skinned meshes (bodies, hair, outfits) keep the SHARED
        // full-mesh vertex buffer on partition[0].buffData; NiSkinPartition::vertexCount is the
        // true whole-mesh count (the geometry's own count is garbage for skinned shapes).
        if (auto* si = gr.skinInstance.get()) {
            if (auto* sp = si->skinPartition.get()) {
                if (sp->numPartitions > 0 && sp->vertexCount >= 500) {
                    auto& part = sp->partitions[0];
                    if (part.buffData && part.buffData->rawVertexData) {
                        const int n = sp->vertexCount < 65535 ? static_cast<int>(sp->vertexCount)
                                                              : 65535;
                        auto vdB = part.buffData->vertexDesc; auto vdP = part.vertexDesc;
                        if (TryRawBuffer(part.buffData->rawVertexData, n,
                                         static_cast<int>(vdB.GetSize()),
                                         static_cast<int>(vdP.GetSize()),
                                         !vdB.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC), bm))
                            return true;
                    }
                }
            }
        }
        return false;
    }

    // The body = the largest readable trishape with a BODY-LIKE HEIGHT (z-span ~100u vs a head's
    // ~25u vs origin-centered garment spaces). verbose=true logs every candidate + its source.
    static bool FindBodyMesh(RE::Actor* actor, BodyMesh& out, bool verbose = false,
                             const RE::BSGeometry* exclude = nullptr)
    {
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return false;
        out = BodyMesh{};
        int bestN = 4000;                                  // reject anything smaller than a body
        const RE::NiPoint3 anchor = root->world.translate; // the ACTOR's world position
        struct Rec {
            static void Walk(RE::NiAVObject* obj, const RE::NiPoint3& anchor, BodyMesh& best, int& bestN, bool verbose, int depth, const RE::BSGeometry* exclude) {
                if (!obj || depth > 40) return;
                if (auto* geom = obj->AsGeometry(); geom && geom != exclude) {
                    // 2026-07-19 WRONG-MESH fixes: (1) "*Overlay*"/"*Ovl*" shapes are RaceMenu
                    // proxies (FakeOverlay = shared template; Body [Ovl0] = overlay layer with a
                    // PARKED world transform — its marker landed 2000u away). (2) TRANSFORM gate:
                    // a real body RIDES its actor — any candidate parked >200u from the actor's
                    // root is a proxy regardless of name. Name filter + geometry gate together.
                    bool overlay = false;
                    if (const char* nm = geom->name.c_str())
                        for (const char* s = nm; *s; ++s)
                            if (_strnicmp(s, "overlay", 7) == 0 || _strnicmp(s, "ovl", 3) == 0) {
                                overlay = true; break;
                            }
                    if (!overlay) {
                        const RE::NiPoint3& gw = geom->world.translate;
                        const float ddx = gw.x - anchor.x, ddy = gw.y - anchor.y, ddz = gw.z - anchor.z;
                        if (ddx * ddx + ddy * ddy + ddz * ddz > 200.f * 200.f) overlay = true;
                    }
                    BodyMesh bm;
                    if (!overlay && ResolveMeshSource(geom, bm)) {
                        float lo = 1e9f, hi = -1e9f, p[3];
                        for (int i = 0; i < bm.n; ++i) {
                            MeshPos(bm, i, p);
                            if (p[2] < lo) lo = p[2]; if (p[2] > hi) hi = p[2];
                        }
                        const float zspan = hi - lo;
                        if (verbose)
                            logger::info("  CAND '{}' vc={} src={} stride={} zspan={:.1f} zmin={:.1f}",
                                         bm.geo->name.c_str(), bm.n,
                                         bm.dynamic ? "dyn" : (bm.half ? "raw-half" : "raw-full"),
                                         bm.stride, zspan, lo);
                        if (bm.n > bestN && zspan >= 55.f && zspan <= 145.f && lo > -10.f && lo < 45.f) {
                            bestN = bm.n; best = bm;
                        }
                    } else if (verbose && geom->AsTriShape()) {
                        auto* t = static_cast<RE::BSTriShape*>(geom);
                        const std::uint32_t vc = t->GetTrishapeRuntimeData().vertexCount;
                        if (vc >= 4000)
                            logger::info("  CAND '{}' vc={} UNREADABLE (no dyn/raw vertex data)",
                                         t->name.c_str(), vc);
                    }
                }
                if (auto* node = obj->AsNode())
                    for (auto& c : node->GetChildren())
                        if (auto* cp = c.get()) Walk(cp, anchor, best, bestN, verbose, depth + 1, exclude);
            }
        };
        Rec::Walk(root, anchor, out, bestN, verbose, 0, exclude);
        return out.geo != nullptr;
    }

    // Four passes over the verts (16-byte stride, pos = first 3 floats — the head-measure
    // contract): 1) bbox + plausibility, 2) centroids, 2b) protrusion anchors, 3) girths.
    // REVIEW-REWORKED 2026-07-18 (verified against the real 3BA meshes by the review agents):
    //  · breasts/butt = back/front-anchored PROTRUSION (pure Y), not radius-from-centroid — the
    //    density centroid CHASES the breast mass and the old measure read the arm-split gate
    //    (ZERO slim→curvy sensitivity on the real femalebody nifs).
    //  · arm split is z-AWARE (arms never reach below mid-torso; hips/thighs never above it) and
    //    the default MeshArmFrac widened 0.35→0.55 so wide chests/hips never clip at the gate.
    //  · arms = |Δy| front-back thickness (the bind pose hangs arms diagonally — a radial measure
    //    in (y,z) read the band's own z-length, not the arm).
    //  · mesh-space plausibility (zSpan 55..145u, zmin −10..45u) rejects foreign-space garments.
    // ── THE SPINE2 BIND CONSTANT (2026-07-20 v2) ──────────────────────────────────────────
    // breastZrel = mound front-mass Z − this constant. Body meshes are authored in the SKELETON's
    // bind space (the same space the raw verts live in), and every PPB female skeleton shares an
    // IDENTICAL chain (half-dragon verified 0.0000u; draenei/orc byte-copies) — so spine2's bind
    // Z is ONE number, derived offline from skeleton_female.nif by accumulating the parent chain
    // (tools/ppb-scratch/derive_neutrals.py). This replaces the v1 runtime NiSkinData skinToBone
    // read, which returned GARBAGE in-game (nan / ±1e37 / zeros — the engine does not populate
    // those legacy transforms for BSTriShape skinning; caught live 2026-07-20). Hoof-invariant
    // by construction: the mesh BOTTOM never enters the math.
    static constexpr float kSpine2BindZ = 91.2488f;
    // ★ ABSOLUTE-POSITION anchors (2026-07-21, the Clove finding). Wall-relative protrusion is the
    // right measure for SIZE but is BLIND TO POSITION by construction — two women whose breasts bulge
    // equally from their own chest wall read identical even when one's wall sits 1.5u deeper. The
    // capsule is placed in BONE-local space, so the SHIFT needs the mound's absolute offset from the
    // bone it hangs on. Derived offline from skeleton_female.nif by parent-chain accumulation
    // (same method as kSpine2BindZ): Spine2 bind=(0,-5.2570,91.3157) · COM bind=(0,0,68.9113).
    static constexpr float kSpine2BindY = -5.2570f;   // breasts hang off spine2
    // ★★ UV LANDMARK SYSTEM (2026-07-22) — THE reference solution. CBBE/3BA/Softbody share ONE UV
    // layout, so a UV is the SAME anatomical point on every body: look up the vertex nearest a UV,
    // read its position, subtract the joint. No neutral, no fingerprint, no protrusion heuristic,
    // and no base-mesh contamination — every one of which failed before this.
    // UVs read off the real femalebody_1.dds by the user; validated on CBBE + 3BA: the three
    // CENTRELINE landmarks land at X=0.00 exactly, heights order chest>nipple>navel>waist/butt,
    // and the butt lands at -Y while chest/nipple are +Y (which is what settled +Y = FORWARD).
    // CAPSULE-FRAME Z: the ragdoll body's origin sits at its PARENT joint, ~6.22u below the spine2
    // NODE bind (Ledger). Derived from the user's hand-dialled nipple (Z 11.07) vs measured (4.85).
    // ★ 2026-07-22 (user directive): measure from the XP32 NODE, not the ragdoll BODY frame. The
    // Havok body is a physics object — it drifts with servo lag, collisions and PLANCK's furniture
    // loosening (Ledger: "never measure a PLANCK-loosened ragdoll"). The node is the base everything
    // is built on AND is what ReScale's arc-sum already uses, so both systems share one reference.
    // The 6.22u body-origin offset belongs at capsule-PLACEMENT time, not measurement time.
    static constexpr float kSpine2FrameZ = 91.3157f;   // spine2 NODE bind (pure anatomy)
    static constexpr float kBodyFrameZOffset = 6.22f;  // node -> ragdoll-body frame, apply on PLACEMENT
    static constexpr float kComFrameZ    = 68.9113f;   // COM bind; offset NOT yet eye-verified
    // belly/waist must reference their OWN joint, not spine2 (user rule 2026-07-22)
    static constexpr float kSpine1FrameZ = 81.4516f, kSpine1BindY = -5.2398f;
    static constexpr float kSpine0FrameZ = 72.7029f, kSpine0BindY = -5.2398f;
    struct UvLandmark { const char* name; float u, v; int bone; };  // bone 0 = spine2, 1 = COM
    static constexpr UvLandmark kUvLandmarks[] = {
        { "R_nipple",     0.424242f, 0.698242f, 0 },   // 0  breast tip
        { "R_breast_up",  0.424242f, 0.662100f, 0 },   // 1  breast UPPER edge  }- thickness/cup
        { "R_breast_dn",  0.424242f, 0.740200f, 0 },   // 2  breast LOWER edge  }
        { "chest_center", 0.500000f, 0.705550f, 0 },   // 3  sternum BETWEEN the breasts = the true
                                                       //    chest reference: breast SIZE is the tip
                                                       //    MINUS this, so a broad chest with small
                                                       //    breasts cannot read as a big bust.
        { "belly",        0.500000f, 0.820310f, 2 },   // 4  -> SPINE1
        { "waist",        0.500000f, 0.878900f, 3 },   // 5  -> SPINE0
        { "butt_cheek",   0.048828f, 0.649902f, 1 },   // 6  COM
        // ── LIMBS (2026-07-23, user-supplied UVs). Two points per limb: the PAIR is the
        // measurement — the distance between them is the limb's girth chord at that band, which
        // is all a long-bone capsule needs (arms/legs grow in RADIUS ONLY, never in length).
        { "R_leg_a",      0.278320f, 0.781250f, 4 },   // 7  -> R Thigh  }- same V: one height,
        { "R_leg_b",      0.112300f, 0.781250f, 4 },   // 8  -> R Thigh  }  two points around it
        { "R_arm_a",      0.244140f, 0.561520f, 5 },   // 9  -> R UpperArm }- same U: sampled up
        { "R_arm_b",      0.244140f, 0.419920f, 5 },   // 10 -> R UpperArm }  the arm's length
    };
    static constexpr int kNumUvLandmarks = 11;
    enum : int { kLmNipple = 0, kLmBrUp, kLmBrDn, kLmChest, kLmBelly, kLmWaist, kLmButt,
             kLmLegA, kLmLegB, kLmArmA, kLmArmB };
    static constexpr float kComBindY    =  0.0000f;   // cheeks hang off COM
    // LIMB bind frames (2026-07-23, offline pynifly parent-chain accumulation on the
    // shipped PPB skeleton_female.nif — the same method that produced the spine values,
    // re-validated against them in the same run).
    static constexpr float kRThighFrameZ = 68.9113f, kRThighBindY = 0.0004f, kRThighBindX = 6.6151f;
    static constexpr float kRUArmFrameZ  = 106.5231f, kRUArmBindY  = -5.2289f, kRUArmBindX = 12.9690f;
    // ── LIMB BIND ROTATIONS (2026-07-23). Spine bones are unrotated, so 'bone-local' there is
    // just mesh-minus-bind. LIMB bones are ROTATED — the thigh's local +Z points DOWN the leg —
    // so the capsule frame is NOT the translation-only frame and the landmark must be turned
    // into it: bone_local = R^T * (vertex - bind). Without this the leg landmark reads Z ~-21
    // while the thigh capsule lives at Z -6..+27, and nothing lines up.
    static constexpr float kRThighRot[3][3] = {{-0.9943f, 0.0414f, 0.0985f},
                                               { 0.0379f, 0.9986f,-0.0368f},
                                               {-0.0999f,-0.0329f,-0.9945f}};
    static constexpr float kRUArmRot[3][3]  = {{-0.9073f, 0.0771f, 0.4133f},
                                               { 0.0874f, 0.9962f, 0.0059f},
                                               {-0.4113f, 0.0415f,-0.9106f}};
    static constexpr const char* kLmBoneName[6] =
        { "spine2", "COM", "spine1", "spine0", "RThigh", "RUpperArm" };

    // == HEAD NOSE SAMPLER (2026-07-23) -- DIAGNOSTIC-FIRST, the probe method that cracked the
    // body. The head is SEPARATE facegen geometry the body finder rejects by design (too few
    // verts, wrong height span), so it gets its own walk. Every head-plausible candidate is
    // logged with its nose-UV resolve so the in-game look tells us which mesh is the real head
    // and which space its vertices live in -- measured, never assumed.
    //   * facegen sculpts move VERTICES, never UVs -> one nose UV = the same anatomical point on
    //     every human head, custom sculpts and High Poly Head included (HPH shares the layout).
    //   * beast heads use different UV layouts -> uvErr rejects loudly -> extent fallback stands.
    //   * head bind (0, -1.5475, 120.3436), rotation ~identity -- an UNROTATED frame, no limb trap.
    static constexpr float kHeadBindX = 0.f, kHeadBindY = -1.5475f, kHeadBindZ = 120.3436f;
    // Head BODY-frame offset (2026-07-24, eye-measured): the chin->nose probe rod needed
    // +1.5u Z to sit on the face, so the head ragdoll body frame sits ~1.5u BELOW the head
    // node (same phenomenon as spine2's 6.22). Irrelevant for RADIUS scaling; required for
    // any future head capsule PLACEMENT work.
    static constexpr float kHeadBodyFrameZOffset = 1.5f;

    // ══ FACEGEN FROM DISK (2026-07-24, "no half measures") ══════════════════════════════════
    // Runtime facegen head positions are GPU-only in VR (measured: dynamicData null, no render
    // buffer, partition buffer = UV+skinning only). But the BAKED facegen NIF on disk carries
    // them: each BSDynamicTriShape block ends in [u32 dynSize][nv float4 positions] with
    // dynSize == nv*16 and the block ending exactly after — validated on real files, where the
    // arithmetic (112112 = 7007*16, +124 == block end) identified the layout beyond doubt.
    // Positions are BODY-SPACE (z ~93-133). UVs come from the RUNTIME partition buffer; the
    // pairing key is VERTEX COUNT EQUALITY (renderer pairs the same two streams by index — a
    // mismatched order would render every face scrambled, so index identity is load-bearing
    // for the game itself, not just for us).
    // Read through BSResourceNiBinaryStream = the game's own resource stack, so BSA-packed
    // facegen (vanilla NPCs) resolves exactly like loose files.
    struct FaceGenBlock { int nv = 0; std::vector<float> xyz; };   // tight 12B stride
    struct FaceGenFile  { bool tried = false; std::vector<FaceGenBlock> blocks; };
    static std::unordered_map<std::uint32_t, FaceGenFile> s_facegen;

    static const FaceGenFile& FaceGenLoad(RE::TESNPC* npc, std::uint32_t logId)
    {
        if (s_facegen.size() > 128) s_facegen.clear();      // simple cap; files are immutable
        auto& e = s_facegen[npc->GetFormID()];
        if (e.tried) return e;
        e.tried = true;
        // ── DEFINING-PLUGIN RESOLUTION (2026-07-24c): GetFile(0) returned NULL on Lydia and the
        // old code exited SILENTLY — cached as tried, invisible in the log, a whole pulse wasted
        // on a nothing. Two rules applied: resolve by LOAD INDEX via the data handler (the form
        // ID's top byte IS the defining plugin's load slot — no API indirection to go stale),
        // and LOG EVERY exit path. A loader that can fail quietly is a diagnostic that lies.
        const std::uint32_t fid = npc->GetFormID();
        const char* fname = nullptr;
        std::uint32_t local = 0;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if ((fid >> 24) == 0xFEu) {
            local = fid & 0xFFFu;
            if (auto* f = dh ? dh->LookupLoadedLightModByIndex(
                                   static_cast<std::uint16_t>((fid >> 12) & 0xFFFu)) : nullptr)
                fname = f->fileName;
        } else {
            local = fid & 0xFFFFFFu;
            if (auto* f = dh ? dh->LookupLoadedModByIndex(
                                   static_cast<std::uint8_t>(fid >> 24)) : nullptr)
                fname = f->fileName;
        }
        if (!fname) {                                       // fallback: the form's own file list
            if (auto* file0 = npc->GetFile(0)) fname = file0->fileName;
        }
        if (!fname) {
            logger::info("FACEGEN {:08X} npc {:08X}: defining plugin UNRESOLVED (dh={} light={})",
                         logId, fid, dh != nullptr, (fid >> 24) == 0xFEu);
            return e;
        }
        char path[280];
        snprintf(path, sizeof(path),
                 "meshes\\actors\\character\\facegendata\\facegeom\\%s\\%08X.NIF",
                 fname, local);
        RE::BSResourceNiBinaryStream in(path);
        if (!in.good()) {
            logger::info("FACEGEN {:08X} '{}' NOT FOUND (loose+BSA)", logId, path);
            return e;
        }
        // ── SLURP via tell() accounting. NiBinaryStream::read returns BOOL, not bytes —
        // the first draft treated true as "1 byte" and collapsed a 2MB file into ~32 junk
        // bytes, then the parser bailed silently. Read the header (CommonLibVR-4.14.0
        // NiBinaryStream.h), never guess an API twice.
        std::vector<std::uint8_t> buf;
        buf.reserve(1 << 21);
        std::uint8_t chunk[65536];
        for (;;) {
            const std::uint32_t before = in.tell();
            const bool ok = in.read(chunk, static_cast<std::uint32_t>(sizeof(chunk)));
            const std::uint32_t after = in.tell();
            const std::uint32_t got = after - before;
            if (got) buf.insert(buf.end(), chunk, chunk + got);
            if (!ok || got == 0) break;
            if (buf.size() > (16u << 20)) break;            // 16MB hard cap
        }
        const std::uint8_t* d = buf.data();
        const std::size_t   N = buf.size();
        auto rdU32 = [&](std::size_t o2) -> std::uint32_t {
            std::uint32_t v; std::memcpy(&v, d + o2, 4); return v; };
        auto rdU16 = [&](std::size_t o2) -> std::uint16_t {
            std::uint16_t v; std::memcpy(&v, d + o2, 2); return v; };
        // every exit names its stage — a parser that fails quietly burned a pulse already
        auto bail = [&](const char* stage, std::size_t o2) -> const FaceGenFile& {
            logger::info("FACEGEN {:08X} parse-fail@{} (N={} o={}) '{}'", logId, stage, N, o2, path);
            return e;
        };
        std::size_t o = 0;
        while (o < N && d[o] != '\n') ++o;
        if (o + 20 > N) return bail("magic", o);
        ++o;                                                // newline
        o += 4;                                             // version
        o += 1;                                             // endian
        o += 4;                                             // user version
        const std::uint32_t nblocks = rdU32(o); o += 4;
        const std::uint32_t bsver   = rdU32(o); o += 4;
        if (nblocks == 0 || nblocks > 4096) return bail("nblocks", o);
        for (int k = 0; k < (bsver > 130 ? 4 : 3); ++k) {   // export ShortStrings
            if (o >= N) return bail("exportStr", o);
            o += 1 + d[o];
        }
        if (o + 2 > N) return bail("ntypes", o);
        const std::uint16_t ntypes = rdU16(o); o += 2;
        std::vector<bool> isDyn(ntypes, false);
        for (int t = 0; t < ntypes; ++t) {
            if (o + 4 > N) return bail("typeLen", o);
            const std::uint32_t ln = rdU32(o); o += 4;
            if (o + ln > N || ln > 256) return bail("typeStr", o);
            isDyn[t] = (std::string_view(reinterpret_cast<const char*>(d + o), ln)
                            .find("BSDynamicTriShape") != std::string_view::npos);
            o += ln;
        }
        if (o + 2ull * nblocks + 4ull * nblocks > N) return bail("blockTables", o);
        const std::size_t tidxOff = o; o += 2ull * nblocks;
        const std::size_t bszOff  = o; o += 4ull * nblocks;
        if (o + 8 > N) return bail("nstr", o);
        const std::uint32_t nstr = rdU32(o); o += 8;        // numStrings + maxLen
        if (nstr > 4096) return bail("nstrBounds", o);
        for (std::uint32_t k = 0; k < nstr; ++k) {
            if (o + 4 > N) return bail("strLen", o);
            const std::uint32_t ln = rdU32(o); o += 4 + ln;
            if (o > N) return bail("strBody", o);
        }
        if (o + 4 > N) return bail("ngroups", o);
        const std::uint32_t ngroups = rdU32(o); o += 4 + 4ull * ngroups;
        if (o > N) return bail("groups", o);
        // block sweep: find every dynamic block's position tail
        std::size_t boff = o;
        for (std::uint32_t bi = 0; bi < nblocks; ++bi) {
            const std::uint32_t bsz = rdU32(bszOff + 4ull * bi);
            const std::size_t   b0  = boff;
            boff += bsz;
            if (boff > N) break;
            const std::uint16_t ti = rdU16(tidxOff + 2ull * bi);
            if (ti >= ntypes || !isDyn[ti]) continue;
            for (std::uint32_t so = 96; so + 4 < bsz && so < 160; ++so) {
                const std::uint32_t dynSz = rdU32(b0 + so);
                if (!dynSz || (dynSz & 15) || so + 4 + dynSz != bsz) continue;
                const int nv = static_cast<int>(dynSz / 16);
                if (nv < 300 || nv > 65000) continue;
                FaceGenBlock blk; blk.nv = nv;
                blk.xyz.resize(static_cast<std::size_t>(nv) * 3);
                for (int i = 0; i < nv; ++i)
                    std::memcpy(&blk.xyz[static_cast<std::size_t>(i) * 3],
                                d + b0 + so + 4 + static_cast<std::size_t>(i) * 16, 12);
                bool dup = false;                            // head blocks ship duplicated
                for (auto& x : e.blocks) if (x.nv == nv) { dup = true; break; }
                if (!dup) e.blocks.push_back(std::move(blk));
                break;
            }
        }
        {
            char lst[96]; int lo = 0; lst[0] = 0;
            for (auto& b : e.blocks)
                lo += snprintf(lst + lo, sizeof(lst) - lo, "%d ", b.nv);
            logger::info("FACEGEN {:08X} '{}' loaded: {} dyn block(s) nv=[{}]",
                         logId, path, e.blocks.size(), lst);
        }
        return e;
    }

    static void SampleHeadNose(RE::Actor* actor, std::uint32_t id, RegionData& d)
    {
        d.lmNoseOk = false;
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return;
        const float nu = ObjectHold::LmNoseUV(0), nv = ObjectHold::LmNoseUV(1);
        const float cu = ObjectHold::LmChinUV(0), cv = ObjectHold::LmChinUV(1);
        struct Best { float err = 1e9f; int vc = 0; };
        Best best;
        struct Rec {
            static void Walk(RE::NiAVObject* obj, std::uint32_t id, float nu, float nv,
                             float cu, float cv, RegionData& d, Best& best, int depth,
                             RE::Actor* actorFG) {
                if (!obj || depth > 40) return;
                if (auto* geom = obj->AsGeometry()) {
                    // ══ FACEGEN SPLIT-BUFFER PATH (2026-07-24, from the HEADHEX hand-decode) ══
                    // Facegen dynamic trishapes split their vertex data in two:
                    //   partition buffer (stride 16): UV half2 @0 | 4 half WEIGHTS @4 | 4 u8 bone ids @12
                    //     (proof: every sampled vertex's weights sum to exactly 1.000)
                    //   dynamicData: float4 POSITION per vertex — the morphed SCULPT itself, so a
                    //     custom-sculpted face (Faralda) measures correctly, not the base head.
                    // Same index = same vertex. The 07-20 CTD came from walking dynamicData with
                    // the shape's LYING vertexCount (51,986); the partition's own count (3,832) is
                    // the true one, and both buffers are extent-verified before any walk.
                    bool fgDone = false;
                    {
                        // 2026-07-24b: the type gate was the wrong assumption — no HEADDYN fired
                        // and the head fell through to the legacy path, so in VR the head may be a
                        // PLAIN BSTriShape whose own vertexCount lies (51,986 one session, 4,620
                        // the next). Its positions then live in the ordinary render buffer, and
                        // the ONLY reason the resolver failed there is that it walked it with the
                        // lying count. Position source, tried in order:
                        //   1. dynamicData        (if the shape IS dynamic — float4 per vertex)
                        //   2. render buffer      (walked with the PARTITION's true count)
                        // UVs always come from the partition buffer (half2 @0, the hand-decoded
                        // layout); same index = same vertex. Both buffers extent-verified.
                        auto& g3 = geom->GetGeometryRuntimeData();
                        auto* si3 = g3.skinInstance.get();
                        auto* sp3 = si3 ? si3->skinPartition.get() : nullptr;
                        if (sp3 && sp3->numPartitions > 0 && sp3->vertexCount >= 500) {
                            auto& pt3 = sp3->partitions[0];
                            const int n3 = static_cast<int>(sp3->vertexCount);
                            const int st3 = pt3.buffData
                                ? static_cast<int>(pt3.buffData->vertexDesc.GetSize()) : 0;
                            const std::uint8_t* uvb = pt3.buffData ? pt3.buffData->rawVertexData : nullptr;
                            const std::uint8_t* pos = nullptr; int pst = 0; bool phalf = false;
                            const char* psrc = "none";
                            if (geom->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape) {
                                auto* dts = static_cast<RE::BSDynamicTriShape*>(geom);
                                if (auto* dd = dts->GetDynamicTrishapeRuntimeData().dynamicData) {
                                    pos = reinterpret_cast<const std::uint8_t*>(dd);
                                    pst = 16; phalf = false; psrc = "dynamicData";
                                }
                            }
                            if (!pos && g3.rendererData && g3.rendererData->rawVertexData) {
                                pos = g3.rendererData->rawVertexData;
                                pst = static_cast<int>(g3.rendererData->vertexDesc.GetSize());
                                phalf = !g3.rendererData->vertexDesc.HasFlag(
                                            RE::BSGraphics::Vertex::VF_FULLPREC);
                                psrc = "renderBuf";
                            }
                            if (!pos) {
                                // ── DISK (the real path in VR): positions from the baked facegen
                                // NIF, matched to this candidate by VERTEX COUNT equality.
                                // 2026-07-24d: the guard below was SILENT and the disk source
                                // skipped two pulses in a row with no trace. Every branch speaks now.
                                auto* npc2 = actorFG ? actorFG->GetActorBase() : nullptr;
                                if (!npc2) {
                                    logger::info("FACEGEN {:08X} '{}' UNREACHABLE: actor={} base=null",
                                                 id, geom->name.c_str(),
                                                 static_cast<const void*>(actorFG));
                                } else {
                                    auto& fg = FaceGenLoad(npc2, id);
                                    for (auto& blk : fg.blocks)
                                        if (blk.nv == n3) {
                                            pos = reinterpret_cast<const std::uint8_t*>(blk.xyz.data());
                                            pst = 12; phalf = false; psrc = "diskNIF";
                                            break;
                                        }
                                    if (!pos)
                                        logger::info("FACEGEN {:08X} '{}' no dyn block matches n3={} "
                                                     "({} block(s) loaded)",
                                                     id, geom->name.c_str(), n3, fg.blocks.size());
                                }
                            }
                            if (uvb && pos && st3 >= 4 && pst >= (phalf ? 8 : 12) &&
                                ExtentReadable(uvb, static_cast<std::size_t>(n3) * st3) &&
                                ExtentReadable(pos, static_cast<std::size_t>(n3) * pst)) {
                                fgDone = true;
                                auto rdPos = [&](int i, float o[3]) {
                                    const std::uint8_t* q = pos + static_cast<std::size_t>(i) * pst;
                                    if (phalf) {
                                        const std::uint16_t* h = reinterpret_cast<const std::uint16_t*>(q);
                                        o[0] = HalfToFloat(h[0]); o[1] = HalfToFloat(h[1]); o[2] = HalfToFloat(h[2]);
                                    } else {
                                        const float* f = reinterpret_cast<const float*>(q);
                                        o[0] = f[0]; o[1] = f[1]; o[2] = f[2];
                                    }
                                };
                                bool okP = true; float lo3 = 1e9f, hi3 = -1e9f;
                                for (int k = 0; k < 24 && okP; ++k) {
                                    const int i = static_cast<int>(static_cast<std::int64_t>(k) * (n3 - 1) / 23);
                                    float v3[3]; rdPos(i, v3);
                                    for (int c3 = 0; c3 < 3; ++c3)
                                        if (!std::isfinite(v3[c3]) || std::fabs(v3[c3]) > 300.f) okP = false;
                                    if (okP) { if (v3[2] < lo3) lo3 = v3[2]; if (v3[2] > hi3) hi3 = v3[2]; }
                                }
                                if (okP && (hi3 - lo3) > 3.f) {
                                    const float tu[2] = { nu, cu }, tv[2] = { nv, cv };
                                    int bi3[2] = { -1, -1 }; float bd3[2] = { 1e9f, 1e9f };
                                    for (int i = 0; i < n3; ++i) {
                                        std::uint16_t ha, hb;
                                        const std::uint8_t* q = uvb + static_cast<std::size_t>(i) * st3;
                                        std::memcpy(&ha, q, 2); std::memcpy(&hb, q + 2, 2);
                                        const float u = HalfToFloat(ha), v = HalfToFloat(hb);
                                        if (!std::isfinite(u) || !std::isfinite(v)) continue;
                                        for (int t = 0; t < 2; ++t) {
                                            const float dd = (u - tu[t]) * (u - tu[t]) + (v - tv[t]) * (v - tv[t]);
                                            if (dd < bd3[t]) { bd3[t] = dd; bi3[t] = i; }
                                        }
                                    }
                                    if (bi3[0] >= 0 && bi3[1] >= 0) {
                                        const float e0 = std::sqrt(bd3[0]), e1 = std::sqrt(bd3[1]);
                                        float pn[3], pc[3]; rdPos(bi3[0], pn); rdPos(bi3[1], pc);
                                        const bool bodySp = (lo3 > 60.f);
                                        const float ln3[3] = { pn[0] - (bodySp ? kHeadBindX : 0.f),
                                                               pn[1] - (bodySp ? kHeadBindY : 0.f),
                                                               pn[2] - (bodySp ? kHeadBindZ : 0.f) };
                                        const float lc3[3] = { pc[0] - (bodySp ? kHeadBindX : 0.f),
                                                               pc[1] - (bodySp ? kHeadBindY : 0.f),
                                                               pc[2] - (bodySp ? kHeadBindZ : 0.f) };
                                        const float dxf = ln3[0]-lc3[0], dyf = ln3[1]-lc3[1], dzf = ln3[2]-lc3[2];
                                        const float fh = std::sqrt(dxf*dxf + dyf*dyf + dzf*dzf);
                                        logger::info("HEADDYN {:08X} '{}' src={} n={} pst={} half={} zspan={:.1f} [{}] "
                                                     "nose uvErr={:.4f} local=({:.2f},{:.2f},{:.2f}) "
                                                     "chin uvErr={:.4f} local=({:.2f},{:.2f},{:.2f}) faceH={:.2f}",
                                                     id, geom->name.c_str(), psrc, n3, pst, phalf, hi3 - lo3,
                                                     bodySp ? "body-space" : "local-space",
                                                     e0, ln3[0], ln3[1], ln3[2],
                                                     e1, lc3[0], lc3[1], lc3[2], fh);
                                        const bool anat = e0 < 0.02f && e1 < 0.02f &&
                                                          std::fabs(ln3[0]) < 2.5f &&
                                                          ln3[2] > lc3[2] && fh > 4.f && fh < 15.f;
                                        if (anat && (e0 < best.err - 1e-4f ||
                                            (e0 < best.err + 1e-4f && n3 > best.vc))) {
                                            best.err = e0; best.vc = n3;
                                            d.lmNose[0] = ln3[0]; d.lmNose[1] = ln3[1]; d.lmNose[2] = ln3[2];
                                            d.lmChin[0] = lc3[0]; d.lmChin[1] = lc3[1]; d.lmChin[2] = lc3[2];
                                            d.lmNoseOk = true; d.lmChinOk = true;
                                        } else if (e0 < 0.02f && e1 < 0.02f) {
                                            // honest label (2026-07-24): a duplicate losing the
                                            // tie-break is NOT an anatomy rejection — the old
                                            // single message lied about the reason.
                                            if (anat)
                                                logger::info("HEADDYN {:08X} '{}' duplicate of the "
                                                             "accepted best (err={:.4f})",
                                                             id, geom->name.c_str(), e0);
                                            else
                                                logger::info("HEADDYN {:08X} '{}' REJECTED by anatomy "
                                                             "(noseX={:.1f} noseZ={:.1f} chinZ={:.1f} faceH={:.1f})",
                                                             id, geom->name.c_str(), ln3[0], ln3[2], lc3[2], fh);
                                        }
                                    }
                                } else {
                                    logger::info("HEADDYN {:08X} '{}' src={} IMPLAUSIBLE (zspan={:.1f})",
                                                 id, geom->name.c_str(), psrc, hi3 - lo3);
                                }
                            } else if (uvb && st3 >= 4) {
                                logger::info("HEADDYN {:08X} '{}' NO POSITION SOURCE (dynType={} rdBuf={})",
                                             id, geom->name.c_str(),
                                             geom->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape,
                                             g3.rendererData && g3.rendererData->rawVertexData ? "yes" : "no");
                            }
                        }
                    }
                    BodyMesh bm;
                    const bool readable = !fgDone && ResolveMeshSource(geom, bm, true);
                    if (!fgDone && !readable) {
                        // the 17:57 candidate dump proved the REAL head never reaches the resolver
                        // (every accepted candidate was clothing/hair). Name the unreadable shapes
                        // so the next look tells us what the facegen head IS and why it fails.
                        if (auto* t = geom->AsTriShape()) {
                            const std::uint32_t vc = t->GetTrishapeRuntimeData().vertexCount;
                            if (vc >= 500) {
                                // 2026-07-23 PROBE: name every failing skinned-path condition so the
                                // next pulse says EXACTLY why the facegen head is unreadable —
                                // skinPart null / 0 partitions / no buffData / no rawVertexData.
                                auto& g2 = t->GetGeometryRuntimeData();
                                const char* why = "no skinInstance";
                                int nparts = -1, spvc = -1; const void* buf = nullptr; const void* raw = nullptr;
                                if (auto* si2 = g2.skinInstance.get()) {
                                    why = "no skinPartition";
                                    if (auto* sp2 = si2->skinPartition.get()) {
                                        nparts = static_cast<int>(sp2->numPartitions);
                                        spvc   = static_cast<int>(sp2->vertexCount);
                                        why = (nparts <= 0) ? "0 partitions"
                                            : (spvc < 500)   ? "partition vc<500" : "?";
                                        if (nparts > 0) {
                                            auto& pt = sp2->partitions[0];
                                            buf = pt.buffData;
                                            if (!pt.buffData) why = "no buffData";
                                            else { raw = pt.buffData->rawVertexData;
                                                   if (!raw) why = "no rawVertexData";
                                                   else if (spvc >= 500) why = "decode failed"; }
                                        }
                                    }
                                }
                                logger::info("HEADUV {:08X} cand='{}' vc={} UNREADABLE — WHY='{}' "
                                             "(skinPartVC={} nparts={} buffData={} rawVtx={})",
                                             id, t->name.c_str(), vc, why, spvc, nparts,
                                             buf ? "yes" : "no", raw ? "yes" : "no");
                                // ── HEX DUMP for a head-plausible facegen candidate (the decode
                                // is failing on a buffer that EXISTS — dump the raw layout so it
                                // can be hand-decoded like the body skin-weight format was).
                                if (raw && spvc >= 1000 && spvc <= 8000 && nparts >= 1) {
                                    auto* si3 = g2.skinInstance.get();
                                    auto* sp3 = si3 ? si3->skinPartition.get() : nullptr;
                                    if (sp3) {
                                        auto& pt3 = sp3->partitions[0];
                                        const int strA = static_cast<int>(pt3.buffData->vertexDesc.GetSize());
                                        const int strB = static_cast<int>(pt3.vertexDesc.GetSize());
                                        const bool full = pt3.buffData->vertexDesc.HasFlag(
                                                              RE::BSGraphics::Vertex::VF_FULLPREC);
                                        logger::info("HEADHEX {:08X} '{}' strideA={} strideB={} FULLPREC={}",
                                                     id, t->name.c_str(), strA, strB, full);
                                        // raw bytes for the first 3 verts (32 each) — hand-decode the
                                        // layout: position shows as small (~-10..+15) floats; whichever
                                        // offset holds three of those IS the position slot.
                                        const std::uint8_t* d = pt3.buffData->rawVertexData;
                                        const int st = strA > 0 ? strA : (strB > 0 ? strB : 32);
                                        for (int vi = 0; vi < 3; ++vi) {
                                            const std::uint8_t* q = d + static_cast<std::size_t>(vi) * st;
                                            char hex[160]; int o = 0;
                                            const int nb = st < 32 ? st : 32;
                                            for (int bx = 0; bx < nb && o < 150; ++bx)
                                                o += snprintf(hex + o, sizeof(hex) - o, "%02X ", q[bx]);
                                            const float* f = reinterpret_cast<const float*>(q);
                                            logger::info("HEADHEX {:08X} v{} f32@0=({:.2f},{:.2f},{:.2f}) [{}]",
                                                         id, vi, f[0], f[1], f[2], hex);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (readable && bm.n >= 500) {
                        float lo = 1e9f, hi = -1e9f, p[3];
                        bool fin = true;
                        for (int i = 0; i < bm.n; ++i) {
                            MeshPos(bm, i, p);
                            if (!std::isfinite(p[2])) { fin = false; break; }
                            if (p[2] < lo) lo = p[2]; if (p[2] > hi) hi = p[2];
                        }
                        const float span = hi - lo;
                        // head-plausible: small vertical span, sitting either at head height
                        // (body space, zmin ~100+) or near the origin (head-local space)
                        if (fin && span >= 5.f && span <= 45.f &&
                            ((lo > 60.f && hi < 160.f) || (lo > -30.f && hi < 40.f))) {
                            const int stride = bm.stride;
                            int uvOff = -1;
                            for (int off = 4; off + 4 <= stride && uvOff < 0; off += 2) {
                                int ok = 0; float umin = 9.f, umax = -9.f, vmin = 9.f, vmax = -9.f;
                                for (int k = 0; k < 24; ++k) {
                                    const int i = static_cast<int>(static_cast<std::int64_t>(k) * (bm.n - 1) / 23);
                                    const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + off;
                                    std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                                    const float u = HalfToFloat(a), v = HalfToFloat(b2);
                                    if (!std::isfinite(u) || !std::isfinite(v)) { umax = -9.f; break; }
                                    if (u > -0.1f && u < 1.1f && v > -0.1f && v < 1.1f) ++ok;
                                    if (u < umin) umin = u; if (u > umax) umax = u;
                                    if (v < vmin) vmin = v; if (v > vmax) vmax = v;
                                }
                                if (ok >= 22 && (umax - umin) > 0.05f && (vmax - vmin) > 0.05f) uvOff = off;
                            }
                            if (uvOff >= 0) {
                                // resolve BOTH head landmarks on this candidate in one pass
                                const float tu[2] = { nu, cu }, tv[2] = { nv, cv };
                                int   bi[2] = { -1, -1 }; float bd2[2] = { 1e9f, 1e9f };
                                for (int i = 0; i < bm.n; ++i) {
                                    const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + uvOff;
                                    std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                                    const float u = HalfToFloat(a), v = HalfToFloat(b2);
                                    if (!std::isfinite(u) || !std::isfinite(v)) continue;
                                    for (int t = 0; t < 2; ++t) {
                                        const float dd = (u - tu[t]) * (u - tu[t]) + (v - tv[t]) * (v - tv[t]);
                                        if (dd < bd2[t]) { bd2[t] = dd; bi[t] = i; }
                                    }
                                }
                                const bool bodySpace = (lo > 60.f);
                                float lp[2][3]; float errT[2] = { 9.f, 9.f };
                                for (int t = 0; t < 2; ++t) {
                                    if (bi[t] < 0) continue;
                                    errT[t] = std::sqrt(bd2[t]);
                                    MeshPos(bm, bi[t], p);
                                    lp[t][0] = p[0] - (bodySpace ? kHeadBindX : 0.f);
                                    lp[t][1] = p[1] - (bodySpace ? kHeadBindY : 0.f);
                                    lp[t][2] = p[2] - (bodySpace ? kHeadBindZ : 0.f);
                                    logger::info("HEADUV {:08X} cand='{}' vc={} span={:.1f} zmin={:.1f} [{}] {} "
                                                 "uvErr={:.4f} headLocal=({:.2f},{:.2f},{:.2f})",
                                                 id, bm.geo->name.c_str(), bm.n, span, lo,
                                                 bodySpace ? "body-space" : "local-space",
                                                 t == 0 ? "nose" : "chin", errT[t],
                                                 lp[t][0], lp[t][1], lp[t][2]);
                                }
                                // best candidate = lowest NOSE uvErr, ties to the bigger mesh (the
                                // real head outnumbers brows/eyes; hair fails the uvErr gate).
                                // The chin rides along from the SAME mesh - never mixed across two.
                                bool anat = true;
                                if (errT[0] < 0.02f && errT[1] < 0.02f) {
                                    const float dxc = lp[0][0]-lp[1][0], dyc = lp[0][1]-lp[1][1],
                                                dzc = lp[0][2]-lp[1][2];
                                    const float fh = std::sqrt(dxc*dxc + dyc*dyc + dzc*dzc);
                                    anat = std::fabs(lp[0][0]) < 2.5f      // nose on the centreline
                                        && lp[0][2] > lp[1][2]             // nose ABOVE the chin
                                        && fh > 4.f && fh < 15.f;          // a face is 4-15u tall
                                    if (!anat)
                                        logger::info("HEADUV {:08X} cand='{}' REJECTED by anatomy "
                                                     "(noseX={:.1f} noseZ={:.1f} chinZ={:.1f} faceH={:.1f})",
                                                     id, bm.geo->name.c_str(),
                                                     lp[0][0], lp[0][2], lp[1][2], fh);
                                } else anat = false;                       // demand BOTH points for the head
                                if (anat && errT[0] < 0.02f && (errT[0] < best.err - 1e-4f ||
                                    (errT[0] < best.err + 1e-4f && bm.n > best.vc))) {
                                    best.err = errT[0]; best.vc = bm.n;
                                    d.lmNose[0] = lp[0][0]; d.lmNose[1] = lp[0][1]; d.lmNose[2] = lp[0][2];
                                    d.lmNoseOk = true;
                                    d.lmChinOk = (errT[1] < 0.02f);
                                    if (d.lmChinOk) {
                                        d.lmChin[0] = lp[1][0]; d.lmChin[1] = lp[1][1]; d.lmChin[2] = lp[1][2];
                                    }
                                }
                            }
                        }
                    }
                }
                if (auto* node = obj->AsNode())
                    for (auto& c : node->GetChildren())
                        if (auto* cp = c.get()) Rec::Walk(cp, id, nu, nv, cu, cv, d, best, depth + 1, actorFG);
            }
        };
        Rec::Walk(root, id, nu, nv, cu, cv, d, best, 0, actor);
        if (d.lmNoseOk) {
            const float nd = std::sqrt(d.lmNose[0]*d.lmNose[0] + d.lmNose[1]*d.lmNose[1] + d.lmNose[2]*d.lmNose[2]);
            float fl = 0.f;
            if (d.lmChinOk) {
                const float dx = d.lmNose[0]-d.lmChin[0], dy = d.lmNose[1]-d.lmChin[1], dz = d.lmNose[2]-d.lmChin[2];
                fl = std::sqrt(dx*dx + dy*dy + dz*dz);
            }
            logger::info("HEADUV {:08X} RESOLVED nose=({:.2f},{:.2f},{:.2f}) chin=({:.2f},{:.2f},{:.2f}) "
                         "noseDepth={:.3f} chinDepth={:.3f} noseChin={:.3f}", id,
                         d.lmNose[0], d.lmNose[1], d.lmNose[2],
                         d.lmChin[0], d.lmChin[1], d.lmChin[2], nd,
                         d.lmChinOk ? std::sqrt(d.lmChin[0]*d.lmChin[0] + d.lmChin[1]*d.lmChin[1] + d.lmChin[2]*d.lmChin[2]) : 0.f,
                         fl);
        }
        else
            logger::info("HEADUV {:08X} no head candidate resolved the nose UV ({:.4f},{:.4f})", id, nu, nv);
    }

    static bool SampleBodyGirths(RE::Actor* actor, MeshGirths& out, const char** outName, int* outVC = nullptr)
    {
        for (int r = 0; r < 7; ++r) {
            out.g[r] = 0.f; out.n[r] = 0;
            out.wpos[r][0] = out.wpos[r][1] = 0.f; out.wpos[r][2] = -1e9f;   // sentinel: no marker
        }
        out.bz = 0.f; out.cup = 0.f;
        if (outName) *outName = "";
        BodyMesh bm;
        if (!FindBodyMesh(actor, bm)) return false;
        if (outName) *outName = bm.geo->name.c_str();
        if (outVC) *outVC = bm.n;
        const int n = bm.n;

        // pass 1 — bbox + global centroid (space plausibility already enforced by the finder)
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        double csum[3] = {};
        float p[3];
        for (int i = 0; i < n; ++i) {
            MeshPos(bm, i, p);
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(p[k])) return false;
                if (p[k] < mn[k]) mn[k] = p[k];
                if (p[k] > mx[k]) mx[k] = p[k];
                csum[k] += p[k];
            }
        }
        const float zSpan = mx[2] - mn[2];
        const float xHalf = (mx[0] - mn[0]) * 0.5f;
        if (zSpan < 10.f || xHalf < 8.f) return false;     // degenerate belt (finder should prevent)
        const float cxAll    = static_cast<float>(csum[0] / n);
        const float armSplit = ObjectHold::MeshArmFrac() * xHalf;
        constexpr float kArmZnFloor = 0.55f;               // arms exist only above mid-torso

        float lo[7], hi[7];
        for (int r = 0; r < 7; ++r) { lo[r] = ObjectHold::MeshBandLo(r); hi[r] = ObjectHold::MeshBandHi(r); }
        auto isArm = [&](float zn, float dx) { return zn > kArmZnFloor && std::fabs(dx) > armSplit; };

        // pass 2 — ring centroids (0..4) + per-side thigh/arm centroids
        double ring[5][3] = {}; int ringN[5] = {};
        double thC[2][3] = {};  int thN[2] = {};
        double arC[2][3] = {};  int arN[2] = {};
        float  arOuter[2] = {};                    // per-side outermost |dx| in the arm band
        for (int i = 0; i < n; ++i) {
            MeshPos(bm, i, p);
            const float zn = (p[2] - mn[2]) / zSpan;
            const float dx = p[0] - cxAll;
            const bool  arm = isArm(zn, dx);
            for (int r = 0; r < 5; ++r)
                if (!arm && zn >= lo[r] && zn < hi[r]) {
                    ring[r][0] += p[0]; ring[r][1] += p[1]; ring[r][2] += p[2]; ++ringN[r];
                }
            if (!arm && zn >= lo[5] && zn < hi[5]) {
                const int s = dx < 0.f ? 0 : 1;
                thC[s][0] += p[0]; thC[s][1] += p[1]; thC[s][2] += p[2]; ++thN[s];
            }
            if (arm && zn >= lo[6] && zn < hi[6]) {
                // pass 2 only finds each side's OUTER EDGE — the arm is always the outermost
                // geometry at its height. Centroids come in pass 2b, gated to the edge zone
                // (2026-07-19 fix: massive breasts crossed the x-split and the arm measure read
                // breast DEPTH — ~4x visual arm capsules. Breast flesh sits 8u+ inboard of the
                // arm's outer edge, so the edge-zone gate excludes it).
                const int s = dx < 0.f ? 0 : 1;
                const float ax = std::fabs(dx);
                if (ax > arOuter[s]) arOuter[s] = ax;
            }
        }
        const float cyBreast = ringN[1] >= 24 ? static_cast<float>(ring[1][1] / ringN[1]) : 0.f;
        const float cyButt   = ringN[4] >= 24 ? static_cast<float>(ring[4][1] / ringN[4]) : 0.f;

        // pass 2b — protrusion ANCHORS: the breast ring's REAR-half mean y (back of the torso,
        // breast-independent) and the butt ring's FRONT-half mean y. Girth then = how far the
        // front/rear surface protrudes from the opposite anchor — pure Y, gate-immune.
        double yRearSum = 0., yFrontSum = 0.; int yRearN = 0, yFrontN = 0;
        for (int i = 0; i < n; ++i) {
            MeshPos(bm, i, p);
            const float zn = (p[2] - mn[2]) / zSpan;
            const float dx = p[0] - cxAll;
            if (isArm(zn, dx)) {
                // arm centroids from EDGE-ZONE verts only (within 6u of the side's outer edge)
                if (zn >= lo[6] && zn < hi[6]) {
                    const int s = dx < 0.f ? 0 : 1;
                    if (arOuter[s] > 1.f && arOuter[s] - std::fabs(dx) <= 6.f) {
                        arC[s][0] += p[0]; arC[s][1] += p[1]; arC[s][2] += p[2]; ++arN[s];
                    }
                }
                continue;
            }
            if (ringN[1] >= 24 && zn >= lo[1] && zn < hi[1] && p[1] < cyBreast) { yRearSum += p[1]; ++yRearN; }
            if (ringN[4] >= 24 && zn >= lo[4] && zn < hi[4] && p[1] > cyButt)   { yFrontSum += p[1]; ++yFrontN; }
        }
        const float yRear  = yRearN  >= 12 ? static_cast<float>(yRearSum / yRearN)   : 0.f;
        const float yFront = yFrontN >= 12 ? static_cast<float>(yFrontSum / yFrontN) : 0.f;

        // pass 3 — girths: top-8 per accumulator (near-max, density rule). 2026-07-19: distances
        // AND positions ride the same sorted array — the girth value (the 8th largest) and the
        // marker vertex are now the SAME vertex: stable between pulses, junk-vert-proof (a junk
        // vert can occupy slots 1-7 without owning the reported girth or the marker).
        struct TopK { float d[8]; float p[8][3]; };
        TopK top[9] = {};   // 0..4 rings, 5/6 thigh R/L, 7/8 arm R/L
        auto pushK = [](TopK& t, float d, const float* pp) {
            if (d <= t.d[7]) return;
            int k = 7;
            while (k > 0 && d > t.d[k - 1]) {
                t.d[k] = t.d[k - 1];
                t.p[k][0] = t.p[k - 1][0]; t.p[k][1] = t.p[k - 1][1]; t.p[k][2] = t.p[k - 1][2];
                --k;
            }
            t.d[k] = d; t.p[k][0] = pp[0]; t.p[k][1] = pp[1]; t.p[k][2] = pp[2];
        };
        int elig1 = 0, elig4 = 0;                           // breast/butt push-eligible counts
        double bzSum = 0.;                                  // breast front-mass z accumulator
        auto push4hi = [](float v, float t[4]) {
            if (v <= t[3]) return; int k = 3;
            while (k > 0 && v > t[k - 1]) { t[k] = t[k - 1]; --k; } t[k] = v; };
        auto push4lo = [](float v, float t[4]) {
            if (v >= t[3]) return; int k = 3;
            while (k > 0 && v < t[k - 1]) { t[k] = t[k - 1]; --k; } t[k] = v; };
        for (int i = 0; i < n; ++i) {
            MeshPos(bm, i, p);
            const float zn = (p[2] - mn[2]) / zSpan;
            const float dx = p[0] - cxAll;
            const bool  arm = isArm(zn, dx);
            // chest / belly / waist: radial from the ring centroid (gate no longer clips: the
            // z-aware split keeps hips/ribcage inside the ring at MeshArmFrac 0.55)
            for (int r = 0; r < 5; ++r) {
                if (r == 1 || r == 4) continue;
                if (arm || zn < lo[r] || zn >= hi[r] || ringN[r] < 24) continue;
                const float cx = static_cast<float>(ring[r][0] / ringN[r]);
                const float cy = static_cast<float>(ring[r][1] / ringN[r]);
                // SECTOR RULES (2026-07-19): each region reads only its own flesh —
                if (r == 0 && p[1] > cy) continue;   // chest: FRONT excluded (breast-proof)
                if (r == 3 && p[1] < cy) continue;   // waist: REAR excluded (the glutes belong to the cheeks)
                const float ddx = p[0] - cx, ddy = p[1] - cy;
                const float d = std::sqrt(ddx * ddx + ddy * ddy);
                pushK(top[r], d, p);
            }
            // breasts: FRONT surface protrusion beyond the rear anchor
            if (!arm && yRearN >= 12 && zn >= lo[1] && zn < hi[1] && p[1] > cyBreast) {
                const float d = p[1] - yRear;
                pushK(top[1], d, p); ++elig1; bzSum += p[2];
            }
            // butt: REAR surface protrusion beyond the front anchor
            if (!arm && yFrontN >= 12 && zn >= lo[4] && zn < hi[4] && p[1] < cyButt) {
                const float d = yFront - p[1];
                pushK(top[4], d, p); ++elig4;
            }
            if (!arm && zn >= lo[5] && zn < hi[5]) {
                const int s = dx < 0.f ? 0 : 1;
                if (thN[s] >= 24) {
                    const float cx = static_cast<float>(thC[s][0] / thN[s]);
                    const float cy = static_cast<float>(thC[s][1] / thN[s]);
                    const float ddx = p[0] - cx, ddy = p[1] - cy;
                    const float d = std::sqrt(ddx * ddx + ddy * ddy);
                    pushK(top[5 + s], d, p);
                }
            }
            if (arm && zn >= lo[6] && zn < hi[6]) {
                const int s = dx < 0.f ? 0 : 1;
                // edge-zone gate: breast bleed sits 8u+ inboard of the arm's outer edge
                if (arN[s] >= 24 && arOuter[s] > 1.f && arOuter[s] - std::fabs(dx) <= 6.f) {
                    // front-back thickness only — pose-safe for the diagonally-hanging bind arms
                    const float cy = static_cast<float>(arC[s][1] / arN[s]);
                    const float d = std::fabs(p[1] - cy);
                    pushK(top[7 + s], d, p);
                }
            }
        }
        // JOINT-RELATIVE breastZ (2026-07-20 v2): mound front-mass Z minus the spine2 BIND
        // constant (see kSpine2BindZ above). Typically a few u POSITIVE (the mound sits ~5u
        // above the mid-back spine2 joint origin). 0 stays the "invalid → vertical shift
        // self-disables" sentinel (no eligible mound verts).
        if (elig1 > 0)
            out.bz = static_cast<float>(bzSum / elig1) - kSpine2BindZ;
        // pass 3b — MOUND-based CUP (2026-07-19d): z-extent of verts on the breast MOUND only
        // (protrusion >= meshCupFrac x near-max). The old all-front measure read the flat chest
        // WALL and saturated for everyone (Lydia 11.61 / Sofia 11.56 / M'rissi 11.42).
        if (elig1 >= 16 && top[1].d[7] > 0.5f && yRearN >= 12) {
            const float thr = ObjectHold::MeshCupFrac() * top[1].d[7];
            float hi4[4] = { -1e9f, -1e9f, -1e9f, -1e9f }, lo4[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
            int nMound = 0;
            for (int i = 0; i < n; ++i) {
                MeshPos(bm, i, p);
                const float zn = (p[2] - mn[2]) / zSpan;
                const float dx = p[0] - cxAll;
                if (isArm(zn, dx) || zn < lo[1] || zn >= hi[1] || p[1] <= cyBreast) continue;
                if (p[1] - yRear < thr) continue;          // chest wall, not mound
                push4hi(p[2], hi4); push4lo(p[2], lo4); ++nMound;
            }
            if (nMound >= 16 && hi4[3] > -1e8f && lo4[3] < 1e8f) out.cup = hi4[3] - lo4[3];
        }
        out.g[0] = top[0].d[7]; out.n[0] = ringN[0];
        out.g[1] = top[1].d[7]; out.n[1] = elig1;
        out.g[2] = top[2].d[7]; out.n[2] = ringN[2];
        out.g[3] = top[3].d[7]; out.n[3] = ringN[3];
        out.g[4] = top[4].d[7]; out.n[4] = elig4;
        out.g[5] = top[5].d[7] > top[6].d[7] ? top[5].d[7] : top[6].d[7]; out.n[5] = thN[0] + thN[1];
        out.g[6] = top[7].d[7] > top[8].d[7] ? top[7].d[7] : top[8].d[7]; out.n[6] = arN[0] + arN[1];
        // marker positions: the girth-defining vertex per region, MESH space -> WORLD space via
        // the geometry transform (skinned body geometry sits at the actor root, so this lands the
        // marker on the standing NPC; pose deviation from bind is acceptable for band-checking).
        {
            const RE::NiTransform& wt = bm.geo->world;
            const int src[7] = { 0, 1, 2, 3, 4,
                                 top[5].d[7] > top[6].d[7] ? 5 : 6,
                                 top[7].d[7] > top[8].d[7] ? 7 : 8 };
            for (int r = 0; r < 7; ++r) {
                const int q = src[r];
                if (top[q].d[7] <= 0.f) continue;          // band never filled 8 — no marker
                RE::NiPoint3 mp{ top[q].p[7][0], top[q].p[7][1], top[q].p[7][2] };
                RE::NiPoint3 w = wt.rotate * (mp * wt.scale) + wt.translate;
                out.wpos[r][0] = w.x; out.wpos[r][1] = w.y; out.wpos[r][2] = w.z;
            }
        }
        return true;
    }

    // slot(+child) -> BodyScale::Region, else -1 (hand 0 / forearm 1 / neck 7 / calf 9 / foot 10 = base).
    // spine2 (slot 6) splits per CHILD: the breast-support children (capSpine2C11/C12 = ApplyListSlot
    // child indices 11,12) take the breasts region; the main + every other child take chest.
    static int RegionForSlotChild(int slot, int child)
    {
        switch (slot) {
        case 2:  return BodyScale::kUpperArms;   // R UpperArm (L auto-mirrors)
        case 3:  return BodyScale::kHead;        // head list (main + children)
        // 2026-07-18 SWAP (user anatomy correction): spine0 sits UNDER spine1 — spine0 = WAIST
        // (top of pelvis / bottom of belly), spine1 = BELLY (navel band). The old wiring was crossed.
        case 4:  return BodyScale::kWaist;       // spine0 = waist
        case 5:  return BodyScale::kBelly;       // spine1 = belly
        case 6:  return (child == 11 || child == 12) ? BodyScale::kBreasts : BodyScale::kChest;  // spine2
        case 8:  return BodyScale::kThighs;      // R Thigh (L auto-mirrors)
        // 2026-07-19 (user correction): the two BUTT CHEEKS (C16/C17) DO radius-scale with the
        // butt measure. It's the ORIFICE RING + inner-pelvis children that must NEVER fatten —
        // a bigger ring radius would SHRINK the opening. The protection is STRUCTURAL now
        // (per-child), so the bodyScaleButt factor is safe to re-enable.
        case 11: return (child == 16 || child == 17) ? BodyScale::kButt : -1;
        default: return -1;                      // unmapped -> ratio 1.0
        }
    }

    // The per-region body-shape multiplier, layered on top of CapScaleOf. Reads the CACHED raw values
    // and applies the LIVE factor + clamp knobs, so a tuning edit re-dresses on the next gen sweep.
    // 1.0 (a no-op that reproduces today's behaviour byte-for-byte) whenever: the system is OFF, the
    // actor isn't latched yet, the slot is unmapped, or the head has no usable measure/reference.
    // ── PER-BODY-TYPE NEUTRALS (2026-07-19f): each base body is built DIFFERENT (3BA +15-19%
    // mid-torso vs Softbody; CBBE base bustier) — "shape" must be the deviation from HER OWN
    // base. Keyed by the sampled mesh's vertex-count fingerprint; derived OFFLINE from the base
    // nifs with the sampler's exact math (Softbody row validated digit-for-digit vs the in-game
    // Lydia capture). Unknown fingerprints fall back to the knob set (Softbody).
    //                        chest  breasts belly  waist  butt   thighs arms   breastZ cup
    // ⚠ breastZ column (idx 7) ZEROED 2026-07-20: semantics changed to SPINE2-RELATIVE (the old
    // 85.06/84.70 were absolute-above-bottom). 0 = uncaptured → vertical shift disabled for that
    // type until re-captured (read breastZrel= off a zero-slider anchor of the type, write here).
    static constexpr float k3baNeutral[9]  = { 13.33f, 8.65f, 8.73f, 11.67f, 14.22f, 7.28f, 3.27f, 0.f, 9.63f };
    static constexpr float kCbbeNeutral[9] = { 13.65f, 11.08f, 8.36f, 10.59f, 13.30f, 6.54f, 3.27f, 0.f, 9.97f };
    // Yvanni's custom Draenei CBBE build (2026-07-20, derive_neutrals.py on her femalebody_1.nif
    // — Weight 100 = the pure _1 file; her live girths matched within 2%, so her OBody morphs are
    // near-nil and her ratios collapse to ~1.0 = capsules at the dialed base). Her hoof-stretched
    // bbox shifts the bands vs a human body, but the SAME shifted bands produced this neutral, so
    // the shift cancels in the ratio. breastZrel derived with kSpine2BindZ (+5.32 = mound above
    // the joint); cup 8.16.
    static constexpr float kYvanniNeutral[9] = { 11.83f, 17.22f, 9.61f, 14.01f, 14.84f, 7.79f, 3.38f, 5.32f, 8.16f };
    static const float* NeutralTableOf(int vc)
    {
        if (vc == 18436) return k3baNeutral;
        if (vc == 13554) return kCbbeNeutral;
        if (vc == 12888) return kYvanniNeutral;            // Draenei custom CBBE (Yvanni)
        return nullptr;                                    // knob set (Softbody / user-captured)
    }
    static float NeutralOf(int region, int vc)             // region 0..6
    {
        // ★ ReShape v2: ONE neutral for EVERY body type — Lydia IS neutralShape (user directive).
        // No per-body fingerprint tables: bone-anchored measurement is already base-mesh
        // independent, so a 3BA/CBBE body that genuinely IS bustier should read bustier and get a
        // bigger capsule (that is the whole point of direct measurement). vc == -1 is the v2
        // sentinel stamped at latch. A 0 neutral = UNCAPTURED -> ratio resolve fails -> region inert.
        if (vc == -1) return ObjectHold::BoneNeutral(region);
        if (const float* t = NeutralTableOf(vc)) return t[region];
        return ObjectHold::MeshNeutral(region);
    }
    static float NeutralBreastZOf(int vc)
    {
        // v2.1: SAG re-enabled for the breast free-capsule only (user refinement — it must drop as
        // it grows). Every RING capsule still stays horizontal; this neutral feeds only the two
        // spine2 breast children via MeshShiftDeltas. 0 = uncaptured -> vertical stays off.
        if (vc == -1) return ObjectHold::BoneNeutralBreastZ();
        if (const float* t = NeutralTableOf(vc)) return t[7];
        return ObjectHold::MeshNeutralBreastZ();
    }
    static float NeutralCupOf(int vc)
    {
        if (vc == -1) return ObjectHold::BoneNeutralCup();
        if (const float* t = NeutralTableOf(vc)) return t[8];
        return ObjectHold::MeshNeutralBreastCup();
    }

    // ROUTE B read-time resolver (review 2026-07-18): measured ratio for ONE region, or false ->
    // slider path. Live at read time: the master toggle, neutral edits, and clamps all apply on
    // the next re-dress with no re-latch. Factor < 0.05 keeps the documented "factor 0 = region
    // OFF" semantics (and guards the net inversion against divide-by-tiny). The ratio is CLAMPED
    // HERE so radius and the translation net share the same safety band.
    // ── 2026-07-23: MeshRegionRatio() DELETED — girth/slider generation, replaced by UV landmarks.


    // ── DIRECT GEOMETRIC SHIFTS (2026-07-19b, the Sofia ladder lesson): the capsule moves by the
    // distance the measured SURFACE moved — u per u — never through slider-unit inversion (which
    // turned Sofia's real 1.77u of butt growth into an 8.2u shift). Vertical breast response is
    // MEASURED sag: the front-mass height delta vs the neutral (down when she sags — no more
    // guessed "up" constant). Bounded ±12u.
    // ══ UV ReShape (2026-07-23) — THE shape system ══════════════════════════════════════════════
    // Every channel is a straight line between one anatomical point and the XP32 node that owns
    // it, compared against Lydia's neutral. No girth statistic, no base-mesh fingerprint, no
    // slider fallback, and NO SECOND PATH that can silently take over (the failure that cost two
    // sessions). If the landmark is missing the channel simply does nothing.
    //
    //   chest -> spine2 ring    belly -> spine1 ring    waist -> spine0 ring
    //   butt  -> COM C16/C17    breast -> spine2 C11/C12 (its own model, below)
    //
    // RINGS scale RADIALLY only. Z is never touched here: height is ReScale's job, and the
    // apply path already multiplies Z by es alone.
    static int LmIndexForRegion(int region)
    {
        switch (region) {
        case BodyScale::kChest: return kLmChest;
        case BodyScale::kBelly: return kLmBelly;
        case BodyScale::kWaist: return kLmWaist;
        case BodyScale::kButt:  return kLmButt;
        default: return -1;               // thighs / arms / head — no UV point yet
        }
    }

    // Radial distance of a landmark from its own node, in the XY plane. This is "how far the
    // flesh sits from the spine axis at that band" — the only thing a ring capsule cares about.
    static inline float LmRadial(const float p[3]) { return std::sqrt(p[0]*p[0] + p[1]*p[1]); }

    static bool UvRegionRatio(const RegionData& d, int region, float* outRatio)
    {
        if (!ObjectHold::LmReShapeEnabled()) return false;
        // ── LIMBS: the driver is the CHORD between the pair, not a distance-from-node. A limb
        // capsule only ever grows in RADIUS (user spec), so girth across the band is the whole
        // measurement — and a chord is immune to any bind-frame error, since a constant offset
        // cancels in a difference. That is why the arm channel is trustworthy even while the
        // arm's bind constant was wrong.
        if (region == BodyScale::kThighs || region == BodyScale::kUpperArms) {
            const bool leg = (region == BodyScale::kThighs);
            const int ia = leg ? kLmLegA : kLmArmA, ib = leg ? kLmLegB : kLmArmB;
            if (!d.lmOk[ia] || !d.lmOk[ib]) return false;
            const float gain = ObjectHold::LmRegionGain(region);
            if (gain < 0.001f) return false;
            const float nC = ObjectHold::LmNeutLimbChord(region);
            if (nC < 1.f) return false;
            const float dx = d.lmPos[ia][0] - d.lmPos[ib][0];
            const float dy = d.lmPos[ia][1] - d.lmPos[ib][1];
            const float dz = d.lmPos[ia][2] - d.lmPos[ib][2];
            const float gC = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (gC < 1.f) return false;
            float r = 1.f + (gC / nC - 1.f) * gain;
            const float lo = ObjectHold::LmClampLo(), hi = ObjectHold::LmClampHi();
            if (r < lo) r = lo; else if (r > hi) r = hi;
            *outRatio = r;
            return true;
        }
        const int idx = LmIndexForRegion(region);
        if (idx < 0 || !d.lmOk[idx]) return false;
        const float gain = ObjectHold::LmRegionGain(region);
        if (gain < 0.001f) return false;                  // channel disabled
        float neut[3]; ObjectHold::LmNeutralPos(idx, neut);
        const float nR = LmRadial(neut), gR = LmRadial(d.lmPos[idx]);
        if (nR < 1.f || gR < 1.f) return false;           // degenerate — refuse rather than guess
        float r = 1.f + (gR / nR - 1.f) * gain;
        const float lo = ObjectHold::LmClampLo(), hi = ObjectHold::LmClampHi();
        if (r < lo) r = lo; else if (r > hi) r = hi;
        *outRatio = r;
        return true;
    }

    // ── BREAST MODEL ───────────────────────────────────────────────────────────────────────────
    // The one channel with no constant and no ring geometry, so it is COMPUTED OUTRIGHT rather
    // than nudged from a knob: both endpoints and the radius come straight from her measurements.
    // Fitted from the two bodyScale-0 anchors and validated on Lydia as a holdout (0.12u miss on
    // a body that was not in the fit). Values are scale-free — es still rides on top.
    static bool UvBreastCapsule(const RegionData& d, float a[3], float b[3], float* r)
    {
        if (!ObjectHold::LmReShapeEnabled() || !d.lmBreastOk) return false;
        const float bd = d.lmBreastDist;
        if (bd < 0.2f || bd > 20.f || d.lmCup < 1.f || d.lmCup > 40.f) return false;  // implausible
        float m[14]; ObjectHold::LmBreastModel(m);
        float ex[5]; ObjectHold::LmBreastModelEx(ex);
        // SATURATED drivers (2026-07-23): the linear fits hold through the calibrated range and
        // are CLAMPED beyond it. An extreme preset gets the boundary response instead of an
        // extrapolation — and can never again pull a correction that reshapes the middle.
        float cup = d.lmCup, sag = d.lmSag;
        if (cup > ex[2]) cup = ex[2];
        if (sag < ex[3]) sag = ex[3]; else if (sag > ex[4]) sag = ex[4];
        // HEIGHT term (the Imperial finding): the mound's own height on the chest, straight from
        // the up/dn landmarks. Sag = tilt (internal droop-shape); this = where the mass SITS.
        // The two are independent — the Imperial dropped 0.87u of height at ~zero tilt change.
        const float midZ = (d.lmPos[kLmBrUp][2] + d.lmPos[kLmBrDn][2]) * 0.5f;
        const float hOff = ex[0] * (midZ - ex[1]);                  // ~1:1, capsule follows flesh
        a[1] = m[0] + m[1] * bd;      b[1] = m[2] + m[3] * bd;      // forward  <- breastDistance
        a[2] = m[4] + m[5] * sag + hOff;                            // vertical <- tilt + height
        b[2] = m[6] + m[7] * sag + hOff;
        *r   = m[8] + m[9] * cup;                                   // radius   <- cup (saturated)
        a[0] = m[10];                                               // root X never moved
        b[0] = m[12] + m[11] * (b[1] - m[13]);                      // lateral = 25% of forward
        if (*r < 0.5f) *r = 0.5f;                                   // never degenerate
        return true;
    }

    // ══ LANDMARK BUTT FIT (2026-07-22) ══════════════════════════════════════════════════════════
    // Where this actor's cheek capsule must move relative to the neutral. Her butt_cheek landmark is
    // a REAL VERTEX ON HER SKIN found by UV, so there is no girth statistic, no neutral mesh and no
    // body-type fingerprint in the chain — just her flesh and the COM joint. Pushed lmButtOutU along
    // its own outward radial it becomes the point the capsule should occupy; the difference against
    // the Lydia anchor is the translation.
    //   * Full 3D on purpose. The legacy shift moved the cheeks on Y alone, so a butt that is wider
    //     or sits lower (Sofia: +0.4 out, -0.25 down vs Lydia) could never be reached.
    //   * Scale-free base-mesh units, added BEFORE the es multiply in ApplyScaleShape — same
    //     convention as the legacy shift, so the skeleton scale still rides on top exactly once.
    static bool ButtLandmarkDelta(RE::Actor* actor, float out[3])
    {
        out[0] = out[1] = out[2] = 0.f;
        if (!actor || !ObjectHold::LmButtFitEnabled()) return false;
        auto it = s_regionRatio.find(actor->GetFormID());
        if (it == s_regionRatio.end() || !it->second.latched || !it->second.lmButtOk) return false;
        const float* p   = it->second.lmButt;
        const float  rad = std::sqrt(p[0] * p[0] + p[1] * p[1]);
        if (rad < 1.f) return false;              // degenerate reading — refuse rather than guess
        const float push = ObjectHold::LmButtOutU();
        float neut[3]; ObjectHold::LmButtNeutral(neut);
        out[0] = p[0] + push * (p[0] / rad) - neut[0];
        out[1] = p[1] + push * (p[1] / rad) - neut[1];
        out[2] = p[2]                       - neut[2];   // push is radial (XY) — height is raw
        // PROPORTIONAL damp: scales the DISPLACEMENT, so a neutral-shaped body (delta ~0) is
        // untouched at any gain while extreme presets are reined in. 2026-07-23 user finding:
        // the overshoot is a high-end problem, so it must be fixed on the slope, never with a
        // flat push — a flat push drags zero-slider bodies off a placement already confirmed.
        const float pg = ObjectHold::LmButtPosGain();
        out[0] *= pg; out[1] *= pg; out[2] *= pg;
        auto bound = [](float v) { return v > 6.f ? 6.f : (v < -6.f ? -6.f : v); };
        out[0] = bound(out[0]); out[1] = bound(out[1]); out[2] = bound(out[2]);
        return true;
    }

    // ── 2026-07-23: MeshShiftDeltas() DELETED — girth/slider generation, replaced by UV landmarks.


    // ── ReShape == ReScale (2026-07-19c, USER ARCHITECTURAL DIRECTIVE): the measured shape
    // ratio applies EXACTLY like trueScale — endpoints AND radius, per region. Radius-only shape
    // (the 2026-07-15 rule) remains SLIDER-mode behavior; in MEASURED mode the ring must MOVE
    // with the flesh ("no amount of radius shrinking pulls a capsule ring inward" — M'rissi).
    // Long bones take shape on RADIUS only (shape never lengthens limbs); torso rings + the COM
    // cheeks scale their radial X/Y offsets too; Z (along-bone/height) never shape-scales. COM
    // ring/inner-pelvis children carry region -1 and take no shape at all.
    static float BodyScaleRegionRatio(RE::Actor* actor, int slot, int child);   // defined below

    static void ApplyScaleShape(RE::Actor* actor, int slot, int child,
                                float a[3], float b[3], float* r)
    {
        const float es = CapScaleOf(actor);
        // ── UV ReShape (2026-07-23): the ring ratio now comes from ONE source, the landmark
        // distance-from-node against Lydia's neutral. The girth sampler and the OBody-slider
        // fallback that used to feed this are gone — a second source that can quietly take over
        // is exactly what produced capsules nobody could account for.
        float q = 1.f;
        if (actor) {
            const int reg = RegionForSlotChild(slot, child);
            if (reg >= 0 && reg != BodyScale::kBreasts && reg != BodyScale::kButt) {
                auto it = s_regionRatio.find(actor->GetFormID());
                float uq = 1.f;
                if (it != s_regionRatio.end() && it->second.latched &&
                    UvRegionRatio(it->second, reg, &uq))
                    q = uq;
            }
        }
        // raceBias (2026-07-19e): flat per-race lateral+radius bias — fur-race measurement
        // correction (M'rissi's mesh reads wider than her judged silhouette). Height untouched.
        const float rb = actor ? ObjectHold::RaceBiasOf(actor->GetRace()) : 1.f;
        float qEnd = 1.f;
        // The four FREE capsules (breasts C11/12, cheeks C16/17) are placed by their own UV model
        // and must never be endpoint-scaled on top of it (user spec).
        const bool freeCap = (slot == 6 && (child == 11 || child == 12)) ||
                             (slot == 11 && (child == 16 || child == 17));
        // RINGS move radially by the same ratio that sets their radius — the capsule ring opens
        // outward to meet the flesh. LONG BONES (arms/legs) grow in RADIUS ONLY (user spec), so
        // their endpoints are never scaled: qEnd stays 1 and only *r takes the ratio.
        if (!freeCap && q != 1.f) {
            static constexpr bool kLongBone[12] = { false, true, true, false, false, false,
                                                    false, false, true, true, false, false };
            if (slot >= 0 && slot < 12 && !kLongBone[slot]) qEnd = q;
        }
        a[0] *= es * qEnd * rb; a[1] *= es * qEnd * rb; a[2] *= es;
        b[0] *= es * qEnd * rb; b[1] *= es * qEnd * rb; b[2] *= es;
        *r   *= es * q * rb;
    }

    // ── 2026-07-23: BodyScaleRegionNet() DELETED — girth/slider generation, replaced by UV landmarks.


    // ── BEAST HEAD GATE (2026-07-26, user: "No head scaling for them"): Khajiit/Argonian head
    // meshes use entirely different UV layouts than the human femalehead, so the nose/chin
    // landmarks land on random anatomy and the ratio wrecks the skull (eye-confirmed on spawned
    // Khajiit + Argonian). Any actor whose race FEMALE skeleton is one of our beast files gets
    // NO head channel at all — head capsules ride es x baked, the pre-head-channel behavior.
    // Detection = the race skeleton path (the sculpt-gate pattern), so every beast race the
    // ini maps is covered automatically, with zero race lists to maintain.
    static bool IsBeastSkeletonActor(RE::Actor* actor)
    {
        auto* base = actor ? actor->GetActorBase() : nullptr;
        auto* race = base ? base->GetRace() : nullptr;
        if (!race) return false;
        const char* mdl = race->skeletonModels[RE::SEXES::kFemale].GetModel();
        if (!mdl) return false;
        for (const char* p = mdl; *p; ++p)
            if ((*p == 'b' || *p == 'B') && _strnicmp(p, "beast", 5) == 0) return true;
        return false;
    }

    // ── 2026-07-23: REWRITTEN onto UV landmarks. The girth resolver and the OBody-SLIDER FALLBACK
    // that used to live here are DELETED, not disabled. The fallback is the specific thing that
    // burned two sessions: with the old two-knob gating it became the sole active path while every
    // log line described it as inactive, so capsules carried up to 1.6u of offset nobody could
    // account for. There is now ONE source, and a missing landmark yields 1.0 (do nothing) rather
    // than a guess from a different measurement system.
    static float BodyScaleRegionRatio(RE::Actor* actor, int slot, int child)
    {
        if (!actor) return 1.f;
        auto it = s_regionRatio.find(actor->GetFormID());
        if (it == s_regionRatio.end() || !it->second.latched) return 1.f;
        const int region = RegionForSlotChild(slot, child);
        if (region < 0) return 1.f;
        const RegionData& d = it->second;
        // HEAD keeps its own extent-based measure until the nose/chin UV landmarks land — it is
        // the one region with a working non-UV source, and dropping it would regress Faralda-class
        // NPCs for no gain. Everything else is UV or nothing.
        if (region == BodyScale::kHead) {
            if (IsBeastSkeletonActor(actor)) return 1.f;   // beast gate — no head scaling, period
            float lo = ObjectHold::LmClampLo(), hi = ObjectHold::LmClampHi();
            if (hi < lo) hi = lo;
            // ── UV HEAD CHANNEL (2026-07-24, the last ReShape piece): face height (nose->chin,
            // from the disk-facegen landmarks) vs Lydia's neutral. The Faralda case is WHY the
            // mesh ratio is the only honest driver: her sculpt (0.914) visually cancels her race
            // scale (1.08) — es alone oversized her head capsules ~9%, and the race record alone
            // would prescribe exactly the wrong correction. capsule = es x THIS ratio = her true
            // rendered face size. Same shape as every channel: one landmark pair, one neutral.
            if (d.lmNoseOk && d.lmChinOk) {
                const float nn = ObjectHold::LmNeutNoseChin();
                const float dx = d.lmNose[0] - d.lmChin[0], dy = d.lmNose[1] - d.lmChin[1],
                            dz = d.lmNose[2] - d.lmChin[2];
                const float fh = std::sqrt(dx*dx + dy*dy + dz*dz);
                const float g  = ObjectHold::LmRegionGain(BodyScale::kHead);
                if (nn > 1.f && fh > 1.f && g > 0.001f) {
                    float r = 1.f + (fh / nn - 1.f) * g;
                    return r < lo ? lo : (r > hi ? hi : r);
                }
            }
            // extent fallback — beast races and any head whose facegen failed the gates
            if (d.headSrc == 0 || d.headRef <= 1.f) return 1.f;
            const float raw = 1.f + (d.headRaw / d.headRef - 1.f) *
                                    ObjectHold::BodyScaleRegionFactor(BodyScale::kHead);
            return raw < lo ? lo : (raw > hi ? hi : raw);
        }
        float uq = 1.f;
        if (UvRegionRatio(d, region, &uq)) return uq;   // already clamped inside
        return 1.f;
    }

    // CapScaleOf (the uniform re-scale) x this region's body-shape ratio — the SINGLE seam Body-Scale
    // uses. Swapped in at the four CapScaleOf geometry sites so every capsule still moves ONLY through
    // the existing endpoint/radius x scalar -> WriteCapsule path (spec A). child = the ApplyListSlot
    // child index (0 = the main capsule for the single-capsule / gen-sweep / auto-fit sites).
    static float CapRegionScaleOf(RE::Actor* actor, int slot, int child)
    {
        return CapScaleOf(actor) * BodyScaleRegionRatio(actor, slot, child);
    }

    // One-shot latch: read the 7 morph nets + the head mesh extent, cache the RAW values. Gated by the
    // caller on attached + settled. Head reference is captured here (once) off the reference actor.
    static void BodyScaleLatch(RE::Actor* actor, std::uint32_t id, bool isRef)
    {
        // ★ 2026-07-28: NEVER measure a dead or dismember-touched actor. A generation bump
        // re-latches EVERYONE, including headless corpses whose face geometry the engine has
        // destroyed — that walk was crash-2026-07-28-18-05-52 (freed BSDynamicTriShape).
        // A previously-latched live shape stays cached, so corpses keep the shape they died with.
        if (actor && !isRef && (actor->IsDead() || DismemberGuard::IsExcluded(actor))) return;
        RE::BSDynamicTriShape* hg = FindHeadGeom(actor);
        // Reference head capture (Lydia): both flavours, once, so NPCs compare like-with-like.
        if (isRef) {
            if (g_refHeadDiam  <= 1.f) { const float d = HeadBoundDiamSafe(hg);  if (d > 1.f) g_refHeadDiam  = d; }
            if (g_refHeadDepth <= 1.f) { const float d = HeadVertDepth(hg);  if (d > 1.f) g_refHeadDepth = d; }
        }
        RegionData d;
        for (int reg = 0; reg < BodyScale::kMorphRegionCount; ++reg)
            d.net[reg] = Interop::GetRegionMorph(actor, reg);         // 0 if SKEE absent / no morphs
        // HEAD (reordered 2026-07-14): the morphed-vertex extent is the ONLY per-NPC-varying head number,
        // so try it FIRST (src=2); modelBound.radius is the authored local bound (near-constant per race,
        // ratio ~1.0 = no sizing) so it is only the FALLBACK (src=1). Both guarded; each needs its ref.
        if (hg) {
            const float dep = HeadVertDepth(hg);
            if (dep > 0.f && g_refHeadDepth > 1.f) { d.headRaw = dep; d.headRef = g_refHeadDepth; d.headSrc = 2; }
            else {
                const float diam = HeadBoundDiamSafe(hg);
                if (diam > 0.f && g_refHeadDiam > 1.f) { d.headRaw = diam; d.headRef = g_refHeadDiam; d.headSrc = 1; }
            }
        }
        // ONE stage diagnostic (so a future src=0 shows WHICH stage failed): head bone found? head geom
        // found? and each raw measure + its reference, independent of which one won above.
        {
            auto* r3d = actor ? actor->Get3D() : nullptr;
            const bool headBoneFound = r3d && r3d->GetObjectByName(RE::BSFixedString("NPC Head [Head]"));
            logger::info("BodyScale {:08X} HEADSTAGE headBoneFound={} hgFound={} boundDiam={:.2f} vertDepth={:.2f} "
                         "refDiam={:.2f} refDepth={:.2f}",
                         id, headBoneFound ? 1 : 0, hg ? 1 : 0,
                         HeadBoundDiamSafe(hg), HeadVertDepth(hg), g_refHeadDiam, g_refHeadDepth);
        }
        // ── ROUTE B measured trueShape (2026-07-18, review-reworked): sample RAW girths; ratios
        // resolve at read time per region (MeshRegionRatio). Log names the sampled SHAPE so a
        // wrong pick is visible, and names any failed band so the calibration session sees it.
        // ── ★ ReShape v2 (2026-07-21): BONE-ANCHORED measurement supersedes the bbox-fraction band
        // sampler when boneShape is on. It feeds the SAME downstream path (meshGirth[] ->
        // MeshRegionRatio -> ApplyScaleShape), so the response rules are unchanged and already
        // correct: xy *= es*q (horizontal only), z *= es (shape never touches height), endpoints
        // multiplied in BONE-LOCAL space (so torso rings move radially from the SPINE AXIS, not the
        // body centre), and RegionForSlotChild leaves COM main + C1..C6 at region -1 = untouched.
        // Legacy sampler stays compiled: set boneShape 0 to revert instantly.
        bool v2done = false;
        // ★ The sampler also produces the UV LANDMARKS, which the landmark butt fit needs even when
        // the girth-driven ReShape is off. Gating the MEASUREMENT on the RESPONSE knob is the trap
        // that has now cost two sessions — meshShape 0 froze the readings as well as the reaction.
        // lmButtFit therefore keeps the sampler alive on its own.
        if (ObjectHold::LmReShapeEnabled() && ObjectHold::BoneShapeEnabled()) {
            if (!IsBeastSkeletonActor(actor))               // beast gate: never run the facegen
                SampleHeadNose(actor, id, d);               // walker on a muzzle (garbage UVs)
            BoneBands bb;
            bool bbOk = SampleBoneBands(actor, id, bb, false);
            // ── DECOY DETECTION (2026-07-23, the Faralda 17:57 case): her outfit carried an
            // embedded reference body FROZEN AT BASE SHAPE, and biggest-mesh-wins picked it over
            // her real morphed skin. It passes every gate — genuine body, perfect UVs, clean
            // anatomy — the ONLY tell is that it measures EXACTLY the neutral while SKEE says
            // real morphs exist. In that case re-run the finder EXCLUDING it: if a second body
            // candidate reads differently, that one is her actual skin.
            if (bbOk && bb.geo) {
                float mx = 0.f;
                for (int rg = 0; rg < BodyScale::kMorphRegionCount; ++rg)
                    if (std::fabs(d.net[rg]) > mx) mx = std::fabs(d.net[rg]);
                bool baseFrozen = mx > 0.15f;
                if (baseFrozen) {
                    static constexpr int kChk[3] = { kLmNipple, kLmChest, kLmButt };
                    for (int c3 = 0; c3 < 3 && baseFrozen; ++c3) {
                        if (!bb.lmOk[kChk[c3]]) { baseFrozen = false; break; }
                        float nt[3]; ObjectHold::LmNeutralPos(kChk[c3], nt);
                        for (int ax = 0; ax < 3; ++ax)
                            if (std::fabs(bb.lmPos[kChk[c3]][ax] - nt[ax]) > 0.05f) baseFrozen = false;
                    }
                }
                if (baseFrozen) {
                    BoneBands bb2;
                    if (SampleBoneBands(actor, id, bb2, false, bb.geo) && bb2.lmOk[kLmNipple]) {
                        float nt[3]; ObjectHold::LmNeutralPos(kLmNipple, nt);
                        const bool moved = std::fabs(bb2.lmPos[kLmNipple][0] - nt[0]) > 0.05f ||
                                           std::fabs(bb2.lmPos[kLmNipple][1] - nt[1]) > 0.05f ||
                                           std::fabs(bb2.lmPos[kLmNipple][2] - nt[2]) > 0.05f;
                        if (moved) {
                            logger::info("DECOY {:08X} '{}' measured EXACTLY base while morphs exist "
                                         "(maxNet={:.2f}) — re-sampled '{}' instead", id,
                                         bb.geo->name.c_str(), mx,
                                         bb2.geo ? bb2.geo->name.c_str() : "?");
                            bb = bb2;
                        } else {
                            logger::info("DECOY-SUSPECT {:08X} '{}' base-exact with morphs (maxNet={:.2f}) "
                                         "but no better candidate — keeping it", id, bb.geo->name.c_str(), mx);
                        }
                    }
                }
            }
            if (bbOk) {
                // ── cache the whole landmark set: this is ReShape's only input ──────────────
                for (int m = 0; m < kNumUvLandmarks; ++m) {
                    d.lmOk[m] = bb.lmOk[m];
                    d.lmPos[m][0] = bb.lmPos[m][0];
                    d.lmPos[m][1] = bb.lmPos[m][1];
                    d.lmPos[m][2] = bb.lmPos[m][2];
                }
                // derived breast drivers — same definitions as the neutral-shape record
                d.lmBreastOk = bb.lmOk[kLmNipple] && bb.lmOk[kLmChest] &&
                               bb.lmOk[kLmBrUp]   && bb.lmOk[kLmBrDn];
                if (d.lmBreastOk) {
                    auto nrm = [](const float p[3]) {
                        return std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]); };
                    const float* nip = bb.lmPos[kLmNipple];
                    const float* up  = bb.lmPos[kLmBrUp];
                    const float* dn  = bb.lmPos[kLmBrDn];
                    d.lmBreastDist = nrm(nip) - nrm(bb.lmPos[kLmChest]);
                    d.lmCup = std::sqrt((up[0]-dn[0])*(up[0]-dn[0]) +
                                        (up[1]-dn[1])*(up[1]-dn[1]) +
                                        (up[2]-dn[2])*(up[2]-dn[2]));
                    d.lmSag = nip[2] - (up[2] + dn[2]) * 0.5f;
                    logger::info("LMDRIVE {:08X} breastDistance={:.3f} cup={:.3f} sag={:+.3f}",
                                 id, d.lmBreastDist, d.lmCup, d.lmSag);
                }
                d.lmButtOk = bb.lmOk[kLmButt];
                if (d.lmButtOk) {
                    d.lmButt[0] = bb.lmPos[kLmButt][0];
                    d.lmButt[1] = bb.lmPos[kLmButt][1];
                    d.lmButt[2] = bb.lmPos[kLmButt][2];
                    if (ObjectHold::LmButtFitEnabled()) {
                        const float* q = d.lmButt;
                        const float rr = std::sqrt(q[0] * q[0] + q[1] * q[1]);
                        float nt[3]; ObjectHold::LmButtNeutral(nt);
                        const float pu = ObjectHold::LmButtOutU();
                        logger::info("LMBUTT {:08X} landmark=({:6.2f},{:7.2f},{:5.2f}) radial={:5.2f} "
                                     "-> cheek delta=({:+.2f},{:+.2f},{:+.2f}) vs neutral ({:.2f},{:.2f},{:.2f})",
                                     id, q[0], q[1], q[2], rr,
                                     q[0] + pu * (q[0] / rr) - nt[0],
                                     q[1] + pu * (q[1] / rr) - nt[1],
                                     q[2] - nt[2], nt[0], nt[1], nt[2]);
                    }
                }
                for (int r = 0; r < BodyScale::kMorphRegionCount; ++r) d.meshGirth[r] = bb.g[r];
                d.meshBreastCup = bb.cup;
                d.meshBreastAbsY = bb.brAbsY;
                d.meshButtAbsY   = bb.buAbsY;
                // ★ EYE VERIFICATION: drop ghost capsules on the two vertices we CALL nipple/cheek
                // apex. mesh space -> world via the actor root, same transform the legacy markers use.
                // ★ UV LANDMARK MARKERS: one ghost per landmark, placed on the ACTUAL vertex the UV
                // resolved to. Visual ground truth — if these sit on nipple / sternum / navel /
                // waist / cheek on every body, the reference system is proven by eye, not argued.
                if (ObjectHold::MeshMarkersEnabled()) {
                    float lw[11][3]{}; int nlm = 0;
                    // ★ ride the BONE, not the mesh: the verts are PRE-SKIN (bind pose), so a marker
                    // placed from them sits where the vertex would be in bind pose — 2u high / 4u
                    // forward of her live skin, and it cannot sway with her. Composing the landmark's
                    // BONE-LOCAL offset against the LIVE node transform makes it track exactly.
                    auto* root4 = actor->Get3D();
                    auto* spn2  = root4 ? root4->GetObjectByName(RE::BSFixedString("NPC Spine2 [Spn2]")) : nullptr;
                    auto* comN  = root4 ? root4->GetObjectByName(RE::BSFixedString("NPC COM [COM ]")) : nullptr;
                    if (spn2 && comN) {
                        for (int m = 0; m < kNumUvLandmarks; ++m) {
                            if (!bb.lmOk[m]) continue;
                            const int bn = kUvLandmarks[m].bone;
                            RE::NiAVObject* nd = spn2;
                            if (bn == 1) nd = comN;
                            else if (bn == 2) {
                                auto* n1 = root4->GetObjectByName(RE::BSFixedString("NPC Spine1 [Spn1]"));
                                if (n1) nd = n1;
                            } else if (bn == 3) {
                                auto* n0 = root4->GetObjectByName(RE::BSFixedString("NPC Spine [Spn0]"));
                                if (n0) nd = n0;
                            } else if (bn == 4) {
                                auto* nt = root4->GetObjectByName(RE::BSFixedString("NPC R Thigh [RThg]"));
                                if (!nt) continue;      // limb missing -> skip, never mis-parent
                                nd = nt;
                            } else if (bn == 5) {
                                auto* nu = root4->GetObjectByName(RE::BSFixedString("NPC R UpperArm [RUar]"));
                                if (!nu) continue;
                                nd = nu;
                            }
                            const RE::NiPoint3 lp{ bb.lmPos[m][0], bb.lmPos[m][1], bb.lmPos[m][2] };
                            const RE::NiPoint3 w = nd->world * lp;
                            lw[nlm][0] = w.x; lw[nlm][1] = w.y; lw[nlm][2] = w.z; ++nlm;
                        }
                        if (nlm > 0) {
                            NpcFinger::UpdateMeshMarkers(actor, lw, nlm);
                            logger::info("UVLM {:08X} placed {} landmark marker(s)", id, nlm);
                        }
                    }
                }
                if (false && ObjectHold::MeshMarkersEnabled() && (bb.tipBrOk || bb.tipBuOk)) {
                    float wp[2][3]{};
                    // ★ 2026-07-21 FIX: the verts live in the GEOMETRY's space, not the actor root's.
                    // Using root->world put the ghost ~5u in front of her neck while the coordinates
                    // themselves were sane — a rendering bug masquerading as a measurement bug, which
                    // cost us a whole verification cycle. bm.geo->world is the correct frame.
                    if (auto* g3 = static_cast<RE::NiAVObject*>(bb.geo)) {
                        const auto& wt = g3->world;
                        auto toWorld = [&](const float p[3], float o[3]) {
                            const RE::NiPoint3 lp{ p[0], p[1], p[2] };
                            const RE::NiPoint3 w = wt * lp;
                            o[0] = w.x; o[1] = w.y; o[2] = w.z;
                        };
                        // live probe offset: user walks the ghost onto the real nipple
                        float probe[3] = { bb.tipBrP[0] + ObjectHold::TipProbe(0),
                                           bb.tipBrP[1] + ObjectHold::TipProbe(1),
                                           bb.tipBrP[2] + ObjectHold::TipProbe(2) };
                        if (bb.tipBrOk) toWorld(probe, wp[0]);
                        if (bb.tipBuOk) toWorld(bb.tipBuP, wp[1]);
                        NpcFinger::UpdateMeshMarkers(actor, wp, 2);
                        logger::info("BodyScale {:08X} TIPMARK nipple=({:.1f},{:.1f},{:.1f}) "
                                     "apex=({:.1f},{:.1f},{:.1f})  [mesh-space PROBED=({:.2f},{:.2f},{:.2f}) raw=({:.2f},{:.2f},{:.2f})]",
                                     id, wp[0][0], wp[0][1], wp[0][2], wp[1][0], wp[1][1], wp[1][2],
                                     probe[0], probe[1], probe[2],
                                     bb.tipBrP[0], bb.tipBrP[1], bb.tipBrP[2]);
                    }
                }
                // v2.1: the breast FREE capsule is the ONE vertical channel (user: it must move
                // down as it grows). Rings stay horizontal — joint spacing never changes.
                d.meshBreastZ   = bb.sagZ;
                d.meshVC        = -1;     // sentinel -> NeutralOf uses the single Lydia neutral
                d.meshSampled   = true;
                v2done          = true;
            } else {
                logger::info("BodyScale {:08X} BONEGIRTH failed — falling back to the legacy sampler", id);
            }
        }
        // ── 2026-07-23: the LEGACY GIRTH SAMPLER fallback (SampleBodyGirths, ~90 lines of
        // band statistics, dressed-mesh guards and body fingerprints) was DELETED here. UV
        // landmarks replaced it outright: a UV coordinate is the same anatomical point on
        // CBBE / 3BA / Softbody, so there is nothing left for a fingerprint to disambiguate
        // and no band that can collapse. If the landmarks fail, ReShape does nothing.
        d.latched = true;
        s_regionRatio[id] = d;
        // Log the raw reads + the currently-resolved ratios (factor/clamp live) so the user can verify + tune.
        static constexpr const char* kRegName[BodyScale::kRegionCount] =
            { "chest", "breasts", "belly", "waist", "butt", "thighs", "upperArms", "head" };
        logger::info("BodyScale {:08X} LATCHED skee={} morphNet[chest={:.2f} breasts={:.2f} belly={:.2f} waist={:.2f} "
                     "butt={:.2f} thighs={:.2f} arms={:.2f}] head(src={} raw={:.2f} ref={:.2f})",
                     id, Interop::HasSkee() ? "yes" : "no",
                     d.net[0], d.net[1], d.net[2], d.net[3], d.net[4], d.net[5], d.net[6],
                     d.headSrc, d.headRaw, d.headRef);
        // 2026-07-18b (the Lydia all-zero lesson): surface every real-valued OBody slider no table
        // knows — the exact list needed to extend the vocabulary when a preset reads blind.
        {
            char unm[420];
            const int nUnm = Interop::CollectUnmappedObodyMorphs(actor, unm, sizeof(unm));
            if (nUnm > 0)
                logger::info("BodyScale {:08X} UNMAPPED OBody sliders ({}): {}", id, nUnm, unm);
        }
        for (int reg = 0; reg < BodyScale::kRegionCount; ++reg) {
            // representative slot per region for the ratio readout (child 0, breasts uses child 11)
            int slot, child = 0;
            switch (reg) {
                case BodyScale::kChest: slot = 6; break;   case BodyScale::kBreasts: slot = 6; child = 11; break;
                // 2026-07-18 review: representatives track the SWAPPED wiring (waist=spine0, belly=spine1)
                case BodyScale::kBelly: slot = 5; break;   case BodyScale::kWaist:  slot = 4; break;
                case BodyScale::kButt:  continue;          // translation-driven — no radius line (see below)
                case BodyScale::kThighs: slot = 8; break;
                case BodyScale::kUpperArms: slot = 2; break; default: slot = 3; break;   // head
            }
            const float ratio = BodyScaleRegionRatio(actor, slot, child);
            if (std::fabs(ratio - 1.f) > 0.001f)
                logger::info("BodyScale {:08X}   region={} ratio={:.3f} (factor={:.2f})",
                             id, kRegName[reg], ratio, ObjectHold::BodyScaleRegionFactor(reg));
        }
        // ── 2026-07-23: the SLIDER 'fallback-shift' readout was DELETED with the path it
        // described. It reported a shift computed from OBody nets and labelled it
        // '(inactive)' based on a flag that did not govern the apply path — so it stated
        // the opposite of the truth for a whole session. A diagnostic for a code path
        // that no longer exists is worse than no diagnostic: see LMDRIVE / LMBUTT.

    }

    // ── SCULPT GATE (2026-07-17, user directive after the Argonian head dial visibly moved
    // Lydia's and the Khajiit's finished heads): when PPB_skeletons.txt declares
    // `sculpt <skeleton filename>`, GLOBAL cap* knobs apply ONLY to actors whose race FEMALE
    // skeleton is that NIF. Different skeleton = different finished geometry — a dial session
    // must never move baked work on other races. npcCap per-NPC overrides are exempt (targeted
    // by design). No sculpt line = legacy global knobs (normal body-tuning play).
    static bool SculptAllows(RE::Actor* actor) {
        const char* tgt = ObjectHold::SculptTarget();
        if (!*tgt) return true;                          // gate off -> legacy
        auto* race = actor ? actor->GetRace() : nullptr;
        if (!race) return false;
        const char* mdl = race->skeletonModels[RE::SEXES::kFemale].GetModel();
        if (!mdl || !*mdl) return false;
        const char* base = mdl;                          // basename of the race's skeleton path
        for (const char* p = mdl; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        return _stricmp(base, tgt) == 0;
    }
    // Slot-SCOPED variant (review 2026-07-17 MEDIUM): `sculpt <nif> head` gates ONLY that slot —
    // non-target actors keep their normal knob dressing (arc re-scale + body-shape ratios) on
    // every OTHER slot, instead of reverting to bare NIF state on their next ragdoll rebuild.
    // A slot-less sculpt line still gates every slot (documented big hammer).
    static bool SculptAllowsSlot(RE::Actor* actor, int slot) {
        if (!*ObjectHold::SculptTarget()) return true;    // gate off
        const int ss = ObjectHold::SculptTargetSlot();
        if (ss >= 0 && ss != slot) return true;           // slot outside the sculpt scope
        return SculptAllows(actor);
    }

    static void ApplyListSlot(RE::Actor* actor, std::uint32_t id, unsigned gen, int slot)
    {
        auto* list = GetNodeListShape(actor, kSlotNode[slot]);
        if (!list) return;
        const std::int32_t n = list->childInfo.size();
        if (n < 1 || n > kMaxListChildren) {
            logger::info("CapFix {:08X} {}-list: implausible childCount {} — skipped", id, kSlotName[slot], n);
            return;
        }
        const int knobs = ObjectHold::CapFixChildKnobs(slot);
        if (knobs > n)
            logger::info("CapFix {:08X} {}-list: {} children live but {} knob blocks — extra knobs skipped",
                         id, kSlotName[slot], n, knobs);

        // BEAST HEAD GATE part 2 (2026-07-26): the capHead knob blocks carry the eye-dialed
        // HUMAN face layout (lips/chins/cheeks/nose/palate). Beast head lists differ in child
        // LAYOUT and COUNT (khajiit 17 live vs the human 22), so index-writing human values
        // parks the whole head rig inside the skull ("all the bones are inside their head",
        // eye-confirmed on spawned Khajiit + Argonian). Beast actors keep their BAKED head
        // list untouched — knobs, npcCap and the knobless captured×ratio branch all skipped —
        // until per-skeleton beast dials land via a sculpt-gate session + bake.
        if (slot == 3 && IsBeastSkeletonActor(actor)) return;

        static constexpr const char* kHandChild[5] = { "c1-center", "c2-thumb", "c3-pinky", "c4-fill", "c5-fill" };
        const bool sculptOk = SculptAllowsSlot(actor, slot);   // once per slot, not per child
        // 2026-07-14: the scale term is now PER-CHILD (CapRegionScaleOf) — spine2's breast-support
        // children take the breasts ratio while its main + others take chest. Computed inside the loop.
        bool  wrote = false;
        bool  wroteChild[kMaxListChildren] = {};
        float wa[kMaxListChildren][3], wb[kMaxListChildren][3], wr[kMaxListChildren] = {};
        // ★ 2026-07-20: npcCap is keyed on the BASE NPC FormID (resolved via LookupForm at parse),
        // so it MUST be looked up by the actor's base — not `id`, which is the reference/placed-
        // instance FormID and never equals the base. Yvanni's hooves/breasts + Auri's antlers were
        // stored fine (SKELMAP receipt) but never matched in-game until this. Fall back to id if null.
        const std::uint32_t npcBaseId = actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : id;
        // UV ReShape input: her cached landmark set. Null until the sampler has latched her, in
        // which case every shape channel simply no-ops and the knob values stand.
        const RegionData* rd = nullptr;
        {
            auto itRd = s_regionRatio.find(actor->GetFormID());
            if (itRd != s_regionRatio.end() && itRd->second.latched) rd = &itRd->second;
        }
        // Loop over ALL live children, not just the knob-covered ones (review 2026-07-17 MEDIUM):
        // an npcCap override may target a child BEYOND the knob count (a personal NIF carrying
        // extra seed children) — CapFixChildSlot self-bounds, so haveKnob is simply false there.
        for (std::int32_t i = 0; i < n && i < kMaxListChildren; ++i) {
            char label[48];
            if (slot == 0 && i < 5) std::snprintf(label, sizeof(label), "%s.%s", kSlotName[slot], kHandChild[i]);
            else if (i == 0)        std::snprintf(label, sizeof(label), "%s.main", kSlotName[slot]);
            else                    std::snprintf(label, sizeof(label), "%s.C%d", kSlotName[slot], static_cast<int>(i));
            const RE::hkpShape* ch = list->childInfo[i].shape;
            if (!ch || ch->type != RE::hkpShapeType::kCapsule) {
                logger::info("CapFix {:08X} {}: child shape type {} not a capsule — skipped",
                             id, label, ch ? static_cast<int>(ch->type) : -1);
                continue;
            }
            auto* cap = const_cast<RE::hkpCapsuleShape*>(static_cast<const RE::hkpCapsuleShape*>(ch));
            alignas(16) float va[4], vb[4];
            _mm_store_ps(va, cap->vertexA.quad);
            _mm_store_ps(vb, cap->vertexB.quad);
            logger::debug("CapFix {:08X} gen={} {} BEFORE A=[{:.2f} {:.2f} {:.2f}] B=[{:.2f} {:.2f} {:.2f}] r={:.2f}",
                         id, gen, label,
                         va[0] * kHavokToSkyrim, va[1] * kHavokToSkyrim, va[2] * kHavokToSkyrim,
                         vb[0] * kHavokToSkyrim, vb[1] * kHavokToSkyrim, vb[2] * kHavokToSkyrim,
                         cap->radius * kHavokToSkyrim);
            float a[3], b[3], r;
            // PER-NPC OVERRIDE (2026-07-17): a PPB_skeletons.txt `npcCap` entry for (this actor,
            // slot, child) WINS over the global knob — and drives the child even when no knob block
            // covers it (Enable 0 / beyond the knob count). This is the "personal skeleton" layer:
            // seed children in the shared NIF stay buried for everyone except the named NPC.
            StampBodyMaterial(cap);   // 2026-07-18 sound fix: stamp EVERY live child, knobbed or baked
            const bool haveKnob = sculptOk && ObjectHold::CapFixChildSlot(slot, static_cast<int>(i), a, b, r);
            const bool haveNpc  = ObjectHold::NpcCapOverride(npcBaseId, slot, static_cast<int>(i), a, b, &r);
            if (!haveKnob && !haveNpc) {
                // ── KNOBLESS HEAD CHILDREN (2026-07-24, the missing ReShape wire): the head is
                // the ONE slot whose capsules are all baked (no knobs), so the head ratio computed
                // at latch never landed — "region=head ratio=0.914" printed while the capsules
                // stayed put. Rules (user spec):
                //   * FULL 3D scaling incl. Z — the head is the END of the chain, not a ring;
                //     a Z-frozen scale can never bring nose and chin closer together.
                //   * Scale about the XP32 HEAD NODE, not the capsule frame origin — the node
                //     sits kHeadBodyFrameZOffset (~1.5u, eye-measured) ABOVE the frame, so
                //     node-centred scaling keeps the capsules ON the face instead of drifting.
                //   * RATIO ONLY, no es: the engine bakes the actor's scale into the live shape
                //     at rig creation (measured: Faralda's baked C14 reads 1.059x Lydia's), so
                //     baked-live x meshRatio = her true rendered face by construction.
                //   * SELF-HEALING base: capture the baked geometry per shape pointer; if the
                //     live value is not what we last wrote, the rig was rebuilt -> re-capture.
                //     The ratio can never compound across re-latches.
                if (slot == 3) {
                    const float hr = BodyScaleRegionRatio(actor, 3, static_cast<int>(i));
                    if (std::fabs(hr - 1.f) > 0.005f) {
                        struct HeadBase { float base[7]; float last[7]; };
                        static std::unordered_map<const void*, HeadBase> s_headBase;
                        if (s_headBase.size() > 1024) s_headBase.clear();
                        const float cur[7] = {
                            va[0] * kHavokToSkyrim, va[1] * kHavokToSkyrim, va[2] * kHavokToSkyrim,
                            vb[0] * kHavokToSkyrim, vb[1] * kHavokToSkyrim, vb[2] * kHavokToSkyrim,
                            cap->radius * kHavokToSkyrim };
                        auto hbIt = s_headBase.find(cap);
                        bool fresh = (hbIt == s_headBase.end());
                        if (!fresh)
                            for (int c2 = 0; c2 < 7 && !fresh; ++c2)
                                if (std::fabs(cur[c2] - hbIt->second.last[c2]) > 0.05f) fresh = true;
                        auto& hb = s_headBase[cap];
                        if (fresh) std::memcpy(hb.base, cur, sizeof(cur));
                        // node position in the capsule frame: (0, 0, +offset x engine-ish scale);
                        // es approximates the engine scale here — worst-case error ~0.1u on the
                        // centre, second-order versus the drift node-centring prevents.
                        const float cz = kHeadBodyFrameZOffset * CapScaleOf(actor);
                        float na[3], nb[3];
                        for (int ax = 0; ax < 3; ++ax) {
                            const float cc = (ax == 2) ? cz : 0.f;
                            na[ax] = cc + (hb.base[ax]     - cc) * hr;
                            nb[ax] = cc + (hb.base[3 + ax] - cc) * hr;
                        }
                        const float nr = hb.base[6] * hr;
                        WriteCapsule(cap, na, nb, nr);
                        hb.last[0] = na[0]; hb.last[1] = na[1]; hb.last[2] = na[2];
                        hb.last[3] = nb[0]; hb.last[4] = nb[1]; hb.last[5] = nb[2];
                        hb.last[6] = nr;
                        wrote = true;
                        wroteChild[i] = true;
                        wa[i][0] = na[0]; wa[i][1] = na[1]; wa[i][2] = na[2];
                        wb[i][0] = nb[0]; wb[i][1] = nb[1]; wb[i][2] = nb[2];
                        wr[i] = nr;
                        logger::debug("CapFix {:08X} APPLIED {} headRatio={:.3f} node-centred "
                                      "A=[{:.2f} {:.2f} {:.2f}] B=[{:.2f} {:.2f} {:.2f}] r={:.2f}",
                                      id, label, hr, na[0], na[1], na[2], nb[0], nb[1], nb[2], nr);
                    }
                }
                continue;
            }
            // ── MORPH-DRIVEN TRANSLATION (2026-07-18, Report 21): displacement capsules MOVE with
            // their region net instead of inflating — breasts ride forward(+Y)/up(+Z), the butt
            // cheeks ride backward(−Y). Signed and linear through zero (petite pulls them inward).
            // Offsets are zero-scale bone-local u, added BEFORE the es multiply below so they scale
            // with the skeleton exactly like the base geometry.
            // ── UV ReShape, free capsules ──────────────────────────────────────────────────────
            // ONE path per channel. No fallback: if the landmark is absent the capsule keeps its
            // knob value untouched, which is the honest outcome — never a guess from another
            // measurement system that the log then describes incorrectly.
            if (slot == 6 && (i == 11 || i == 12)) {
                float ua[3], ub[3], ur;
                if (rd && UvBreastCapsule(*rd, ua, ub, &ur)) {
                    // fully computed from her own anatomy; mirror X for the left breast
                    const float sx = (a[0] + b[0]) < 0.f ? -1.f : 1.f;
                    a[0] = ua[0] * sx; a[1] = ua[1]; a[2] = ua[2];
                    b[0] = ub[0] * sx; b[1] = ub[1]; b[2] = ub[2];
                    r = ur;
                }
            } else if (slot == 11 && (i == 16 || i == 17)) {
                float lb[3];
                if (ButtLandmarkDelta(actor, lb)) {
                    // position: translate to HER cheek. X mirrors per side (landmark is sampled
                    // on the right, body is symmetric).
                    const float sx = (a[0] + b[0]) < 0.f ? -1.f : 1.f;
                    a[0] += lb[0] * sx; b[0] += lb[0] * sx;
                    a[1] += lb[1];      b[1] += lb[1];
                    a[2] += lb[2];      b[2] += lb[2];
                }
                // radius: same radial ratio the rings use (user spec — the cheeks scale too)
                float br;
                if (rd && UvRegionRatio(*rd, BodyScale::kButt, &br)) r *= br;
            }
            // ReShape == ReScale (2026-07-19c): measured shape scales radial endpoints + radius
            // exactly like es; slider mode stays radius-only (legacy). See ApplyScaleShape.
            ApplyScaleShape(actor, slot, static_cast<int>(i), a, b, &r);
            // DEGENERATE GUARD (the June lesson): A==B collides fine but is INVISIBLE to the visualizer.
            if (std::fabs(a[0]-b[0]) < 0.01f && std::fabs(a[1]-b[1]) < 0.01f && std::fabs(a[2]-b[2]) < 0.01f)
                b[2] += 0.15f;
            WriteCapsule(cap, a, b, r);
            wrote = true;
            wroteChild[i] = true;
            wa[i][0] = a[0]; wa[i][1] = a[1]; wa[i][2] = a[2];
            wb[i][0] = b[0]; wb[i][1] = b[1]; wb[i][2] = b[2];
            wr[i] = r;
            logger::debug("CapFix {:08X} APPLIED {} A=[{:.1f} {:.1f} {:.1f}] B=[{:.1f} {:.1f} {:.1f}] r={:.1f}",
                         id, label, a[0], a[1], a[2], b[0], b[1], b[2], r);
        }
        if (!wrote) return;
        RepatchListAabb(list);
        MirrorListToLeft(actor, id, slot, wroteChild, wa, wb, wr);
    }

    // ── CAP FIX console entry (2026-07-02 rework): EVERYTHING in-game, no file. Select the NPC in the
    // console, then:  capfix                      → prints her current R-hand capsule
    //                 capfix ax ay az bx by bz r  → writes it NOW + all other driven NPCs follow.
    // ══ ReTouch GEOMETRY EXPORTS (2026-07-24) ═══════════════════════════════════════════════
    // The ghost-zone/touch layer reads the LIVE capsule geometry — the same bodies the apply
    // path writes — so touch zones inherit ReScale + ReShape + the head channel by construction.
    static RE::hkpRigidBody* SlotHkBody(RE::Actor* actor, int slot)
    {
        if (!actor || slot < 0 || slot > 11) return nullptr;
        auto* hn = FindNode(actor, kSlotNode[slot]);
        if (!hn) return nullptr;
        auto* colObj = hn->collisionObject.get();
        if (!colObj) return nullptr;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        return body ? body->GetRigidBody() : nullptr;
    }
    bool SlotBodyPoseU(RE::Actor* actor, int slot, float posOutU[3], float rotOut[9])
    {
        auto* hkp = SlotHkBody(actor, slot);
        if (!hkp) return false;
        const auto& T = hkp->motion.motionState.transform;
        alignas(16) float t[4], c0[4], c1[4], c2[4];
        _mm_store_ps(t,  T.translation.quad);
        _mm_store_ps(c0, T.rotation.col0.quad);
        _mm_store_ps(c1, T.rotation.col1.quad);
        _mm_store_ps(c2, T.rotation.col2.quad);
        posOutU[0] = t[0] * kHavokToSkyrim; posOutU[1] = t[1] * kHavokToSkyrim; posOutU[2] = t[2] * kHavokToSkyrim;
        for (int r = 0; r < 3; ++r) {
            rotOut[r * 3 + 0] = (r == 0 ? c0[0] : (r == 1 ? c0[1] : c0[2]));
            rotOut[r * 3 + 1] = (r == 0 ? c1[0] : (r == 1 ? c1[1] : c1[2]));
            rotOut[r * 3 + 2] = (r == 0 ? c2[0] : (r == 1 ? c2[1] : c2[2]));
        }
        return true;
    }
    // Sided body getter (2026-07-29, the mapping-HUD session): slot geometry for the LEFT twin.
    // kSlotNodeL[slot] is null for center slots — callers skip those.
    static RE::hkpRigidBody* SlotHkBodySide(RE::Actor* actor, int slot, bool left)
    {
        if (!actor || slot < 0 || slot > 11) return nullptr;
        const char* nm = left ? kSlotNodeL[slot] : kSlotNode[slot];
        if (!nm) return nullptr;
        auto* hn = FindNode(actor, nm);
        if (!hn) return nullptr;
        auto* colObj = hn->collisionObject.get();
        if (!colObj) return nullptr;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        return body ? body->GetRigidBody() : nullptr;
    }
    bool SlotHasLeftTwin(int slot) { return slot >= 0 && slot < 12 && kSlotNodeL[slot] != nullptr; }

    bool ReadCapsuleWorldUSide(RE::Actor* actor, int slot, bool left, int child,
                               float aOut[3], float bOut[3], float* rOut)
    {
        auto* hkp = SlotHkBodySide(actor, slot, left);
        if (!hkp) return false;
        auto* col = hkp->GetCollidableRW();
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape) return false;
        const RE::hkpCapsuleShape* cap = nullptr;
        if (shape->type == RE::hkpShapeType::kCapsule) {
            if (child > 0) return false;
            cap = static_cast<const RE::hkpCapsuleShape*>(shape);
        } else if (shape->type == RE::hkpShapeType::kList) {
            auto* list = static_cast<const RE::hkpListShape*>(shape);
            if (child < 0 || child >= static_cast<int>(list->childInfo.size())) return false;
            const RE::hkpShape* ch = list->childInfo[child].shape;
            if (!ch || ch->type != RE::hkpShapeType::kCapsule) return false;
            cap = static_cast<const RE::hkpCapsuleShape*>(ch);
        } else return false;
        // world pose straight off THIS body (SlotBodyPoseU is right-side-bound)
        alignas(16) float t[4], c0[4], c1[4], c2[4];
        const auto& ms = hkp->motion.motionState.transform;
        _mm_store_ps(t,  ms.translation.quad);
        _mm_store_ps(c0, ms.rotation.col0.quad);
        _mm_store_ps(c1, ms.rotation.col1.quad);
        _mm_store_ps(c2, ms.rotation.col2.quad);
        float pos[3], R[9];
        for (int i = 0; i < 3; ++i) pos[i] = t[i] * kHavokToSkyrim;
        R[0]=c0[0]; R[1]=c1[0]; R[2]=c2[0];
        R[3]=c0[1]; R[4]=c1[1]; R[5]=c2[1];
        R[6]=c0[2]; R[7]=c1[2]; R[8]=c2[2];
        alignas(16) float va[4], vb[4];
        _mm_store_ps(va, cap->vertexA.quad);
        _mm_store_ps(vb, cap->vertexB.quad);
        for (int i = 0; i < 3; ++i) {
            aOut[i] = pos[i] + (R[i*3+0]*va[0] + R[i*3+1]*va[1] + R[i*3+2]*va[2]) * kHavokToSkyrim;
            bOut[i] = pos[i] + (R[i*3+0]*vb[0] + R[i*3+1]*vb[1] + R[i*3+2]*vb[2]) * kHavokToSkyrim;
        }
        if (rOut) *rOut = cap->radius * kHavokToSkyrim;
        return true;
    }
    bool ReadCapsuleWorldU(RE::Actor* actor, int slot, int child,
                           float aOut[3], float bOut[3], float* rOut)
    {
        auto* hkp = SlotHkBody(actor, slot);
        if (!hkp) return false;
        auto* col = hkp->GetCollidableRW();
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape) return false;
        const RE::hkpCapsuleShape* cap = nullptr;
        if (shape->type == RE::hkpShapeType::kCapsule) {
            if (child > 0) return false;
            cap = static_cast<const RE::hkpCapsuleShape*>(shape);
        } else if (shape->type == RE::hkpShapeType::kList) {
            auto* list = static_cast<const RE::hkpListShape*>(shape);
            if (child < 0 || child >= static_cast<int>(list->childInfo.size())) return false;
            const RE::hkpShape* ch = list->childInfo[child].shape;
            if (!ch || ch->type != RE::hkpShapeType::kCapsule) return false;
            cap = static_cast<const RE::hkpCapsuleShape*>(ch);
        } else return false;
        float pos[3], R[9];
        if (!SlotBodyPoseU(actor, slot, pos, R)) return false;
        alignas(16) float va[4], vb[4];
        _mm_store_ps(va, cap->vertexA.quad);
        _mm_store_ps(vb, cap->vertexB.quad);
        for (int i = 0; i < 3; ++i) {
            aOut[i] = pos[i] + (R[i*3+0]*va[0] + R[i*3+1]*va[1] + R[i*3+2]*va[2]) * kHavokToSkyrim;
            bOut[i] = pos[i] + (R[i*3+0]*vb[0] + R[i*3+1]*vb[1] + R[i*3+2]*vb[2]) * kHavokToSkyrim;
        }
        if (rOut) *rOut = cap->radius * kHavokToSkyrim;
        return true;
    }
    const char* SlotLabel(int slot) { return (slot >= 0 && slot < 12) ? kSlotName[slot] : "?"; }
    int SlotLiveChildren(RE::Actor* actor, int slot)
    {
        auto* hkp = SlotHkBody(actor, slot);
        auto* col = hkp ? hkp->GetCollidableRW() : nullptr;
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape) return 0;
        if (shape->type == RE::hkpShapeType::kList)
            return static_cast<int>(static_cast<const RE::hkpListShape*>(shape)->childInfo.size());
        return 0;
    }

    void CapFixConsole(RE::Actor* actor, bool apply,
                       float ax, float ay, float az, float bx, float by, float bz, float r,
                       char* out, std::size_t outSz)
    {
        if (!actor) {
            snprintf(out, outSz, "PPB CapFix: click an NPC in the console first, then: capfix [ax ay az bx by bz r]");
            return;
        }
        int shapeType = -1;
        auto* cap = GetNodeCapsule(actor, "NPC R Hand [RHnd]", &shapeType);
        if (!cap) {
            snprintf(out, outSz, "PPB CapFix: no R-hand capsule on this ref (shape type %d)", shapeType);
            return;
        }
        alignas(16) float va[4], vb[4];
        _mm_store_ps(va, cap->vertexA.quad);
        _mm_store_ps(vb, cap->vertexB.quad);
        if (!apply) {
            snprintf(out, outSz,
                     "R-hand capsule A=[%.1f %.1f %.1f] B=[%.1f %.1f %.1f] r=%.1f (body-local u). "
                     "Live loop: edit capHand*/capFore*/capUpper*/capHead* in the tuning file (1 Hz auto-apply).",
                     va[0] * kHavokToSkyrim, va[1] * kHavokToSkyrim, va[2] * kHavokToSkyrim,
                     vb[0] * kHavokToSkyrim, vb[1] * kHavokToSkyrim, vb[2] * kHavokToSkyrim,
                     cap->radius * kHavokToSkyrim);
            return;
        }
        constexpr float kS2H = 0.0142875f;
        StampBodyMaterial(cap);   // 2026-07-18: console path writes raw (bypasses WriteCapsule)
        cap->vertexA.quad = _mm_set_ps(0.f, az * kS2H, ay * kS2H, ax * kS2H);
        cap->vertexB.quad = _mm_set_ps(0.f, bz * kS2H, by * kS2H, bx * kS2H);
        cap->radius = r * kS2H;
        float a3[3] = { ax, ay, az }, b3[3] = { bx, by, bz };
        ObjectHold::CapFixSet(a3, b3, r);                    // store + arm: every other driven NPC follows
        logger::info("CapFix CONSOLE APPLIED {:08X} A=[{:.1f} {:.1f} {:.1f}] B=[{:.1f} {:.1f} {:.1f}] r={:.1f}",
                     actor->GetFormID(), ax, ay, az, bx, by, bz, r);
        snprintf(out, outSz,
                 "APPLIED: A=[%.1f %.1f %.1f] B=[%.1f %.1f %.1f] r=%.1f — was A=[%.1f %.1f %.1f] r=%.1f. "
                 "(All driven NPCs follow. Visualizer mods may NOT redraw live shape edits — verify by touch.)",
                 ax, ay, az, bx, by, bz, r,
                 va[0] * kHavokToSkyrim, va[1] * kHavokToSkyrim, va[2] * kHavokToSkyrim,
                 cap->radius * kHavokToSkyrim);
    }

    // ── CAP FIX: live capsule rewrite for the 4-slot sweep (hand/forearm/upper arm/head), once per
    // generation per actor. Slots dial the RIGHT side; capMirrorL replays every write on the LEFT. ──
    static const void* GetNodeBodyPtr(RE::Actor* actor, const char* nodeName)
    {
        auto* root = actor->Get3D();
        if (!root) return nullptr;
        auto* obj = root->GetObjectByName(nodeName);
        auto* node = obj ? obj->AsNode() : nullptr;
        if (!node || !node->collisionObject) return nullptr;
        auto* body = static_cast<RE::bhkCollisionObject*>(node->collisionObject.get())->GetRigidBody();
        return body ? body->GetRigidBody() : nullptr;
    }

    // MEASURED-SCALE sampler support (2026-07-13, the Oakwood build-scale bug): world
    // position of a node's hkp body ORIGIN, game units. nullptr body -> false.
    static bool GetNodeBodyPos(RE::Actor* actor, const char* nodeName, float out[3])
    {
        auto* hk = const_cast<RE::hkpRigidBody*>(
            static_cast<const RE::hkpRigidBody*>(GetNodeBodyPtr(actor, nodeName)));
        if (!hk) return false;
        const RE::hkVector4& p = hk->motion.motionState.transform.translation;
        alignas(16) float v[4];
        _mm_store_ps(v, p.quad);
        out[0] = v[0] * kHavokToSkyrim; out[1] = v[1] * kHavokToSkyrim; out[2] = v[2] * kHavokToSkyrim;
        return true;
    }
    static bool GetNodePos(RE::Actor* actor, const char* nodeName, float out[3])
    {
        auto* root = actor->Get3D();
        auto* obj  = root ? root->GetObjectByName(nodeName) : nullptr;
        if (!obj) return false;
        out[0] = obj->world.translate.x; out[1] = obj->world.translate.y; out[2] = obj->world.translate.z;
        return true;
    }

    // CAPSULE AUTO-FIT (2026-07-05): arm capsules span their SEATED joint balls — forearm runs
    // elbow-ball -> wrist-ball, upper arm runs shoulder-ball -> elbow-ball, both endpoints read from
    // the live constraint data in this bone's own frame (bury-the-ball doctrine made literal).
    // Drift-checked writes at the caller's 1 Hz cadence; radius stays the slot knob (flesh = eye's job).
    static void CapAutoFitArms(RE::Actor* actor, std::uint32_t id)
    {
        // GRAB GATE (2026-07-07): a player-grabbed limb's joint balls are displaced — fitting
        // capsules to them would bake the displacement. Same gate as PivFixApply.
        if (ObjectHold::PivGrabGateEnabled() && Interop::IsActorGrabbedByPlayer(actor)) return;
        // slot -> { capsule node, A = (joint, childSide), B = (joint, childSide) }
        struct Fit { int slot; const char* node; const char* name; int jA; bool cA; int jB; bool cB; };
        static constexpr Fit kFits[8] = {
            { 1, "NPC R Forearm [RLar]",  "forearm",  1, true,  0, false },   // elbow child / wrist other
            { 2, "NPC R UpperArm [RUar]", "upperarm", 2, true,  1, false },   // shoulder child / elbow other
            { 4, "NPC Spine [Spn0]",      "spine0",   3, true,  4, false },   // spine0 ball -> spine1 ball
            { 5, "NPC Spine1 [Spn1]",     "spine1",   4, true,  5, false },
            { 6, "NPC Spine2 [Spn2]",     "spine2",   5, true,  6, false },   // the chest span
            { 7, "NPC Neck [Neck]",       "neck",     6, true,  7, false },
            { 8, "NPC R Thigh [RThg]",    "thighR",   8, true,  9, false },   // hip -> knee
            { 9, "NPC R Calf [RClf]",     "calfR",    9, true, 10, false },   // knee -> ankle
        };
        for (const auto& f : kFits) {
            float dummyA[3], dummyB[3], r;
            if (!ObjectHold::CapFixSlot(f.slot, dummyA, dummyB, r)) continue;   // slot disabled
            float a[3], b[3];
            if (!ObjectHold::PivReadJointLocal(actor, f.jA, f.cA, a)) continue;
            if (!ObjectHold::PivReadJointLocal(actor, f.jB, f.cB, b)) continue;
            int shapeType = -1;
            auto* cap = GetNodeCapsule(actor, f.node, &shapeType);
            if (!cap) {
                // A BAKED body is a bhkListShape, not a capsule — the ball-to-ball fitter cannot reach
                // it (child 0 takes the static slot knobs instead). Already true for fore/upper/thigh/calf
                // since wave-1; wave-2 adds spine0/1/2. Harmless, but it must never surprise anyone.
                static bool s_loggedAutoFitList[12] = {};
                if (shapeType == static_cast<int>(RE::hkpShapeType::kList) && !s_loggedAutoFitList[f.slot]) {
                    s_loggedAutoFitList[f.slot] = true;
                    logger::info("CapFix auto-fit skipped for {}: shape is a list (child 0 takes the static slot knobs)",
                                 f.name);
                }
                continue;
            }
            r *= CapRegionScaleOf(actor, f.slot, 0);   // 2026-07-12 review fix x 2026-07-14 body-scale:
                                       // knob radius is scale-1 authored -> uniform re-scale x this limb/
                                       // torso region's body-shape ratio (RADIUS ONLY on the auto-fit main;
                                       // its endpoints a/b are joint-ball-derived live space — NOT scaled)
            alignas(16) float va[4], vb[4];
            _mm_store_ps(va, cap->vertexA.quad);
            _mm_store_ps(vb, cap->vertexB.quad);
            const float drift =
                std::fabs(va[0]*kHavokToSkyrim - a[0]) + std::fabs(va[1]*kHavokToSkyrim - a[1]) +
                std::fabs(va[2]*kHavokToSkyrim - a[2]) + std::fabs(vb[0]*kHavokToSkyrim - b[0]) +
                std::fabs(vb[1]*kHavokToSkyrim - b[1]) + std::fabs(vb[2]*kHavokToSkyrim - b[2]);
            if (drift < 0.10f) continue;                                        // already fitted
            if (std::fabs(a[0]-b[0]) < 0.01f && std::fabs(a[1]-b[1]) < 0.01f && std::fabs(a[2]-b[2]) < 0.01f)
                b[2] += 0.15f;                                                  // degenerate guard
            WriteCapsule(cap, a, b, r);
            logger::debug("CapFix {:08X} AUTOFIT {} A=[{:.1f} {:.1f} {:.1f}](joint {}) B=[{:.1f} {:.1f} {:.1f}](joint {}) r={:.1f}",
                         id, f.name, a[0], a[1], a[2], f.jA, b[0], b[1], b[2], f.jB, r);
            // L bodies are X-mirrored twins, so the ball-derived endpoints X-negate identically.
            MirrorSlotToLeft(actor, id, f.slot, a, b, r, false);
        }
    }

    // Resolve whether the actor carries our female bake (COM = bhkListShape). Distinguishes "3D not up
    // yet" (return false, DON'T cache -> retry next tick) from "single-capsule COM = stock skeleton"
    // (cache NO) and "list COM = our bake" (cache YES). Non-static: PivScaleCorrect (PivFix.cpp) forward-
    // declares + calls it as the SAME gate, so the re-scale and the capsule fitter agree on who to touch.
    bool ActorCarriesBake(RE::Actor* actor) {
        if (!actor) return false;
        const std::uint32_t id = actor->GetFormID();
        auto it = s_carriesBake.find(id);
        if (it != s_carriesBake.end() && it->second != 0) return it->second == 1;
        auto* hn = FindNode(actor, "NPC COM [COM ]");
        if (!hn) return false;
        auto* colObj = hn->collisionObject.get();
        if (!colObj) return false;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        auto* hkp  = body ? body->GetRigidBody() : nullptr;
        auto* col  = hkp ? hkp->GetCollidableRW() : nullptr;
        const RE::hkpShape* shape = col ? col->shape : nullptr;
        if (!shape) return false;                        // 3D mid-load — retry next tick, don't cache
        const bool isBake = (shape->type == RE::hkpShapeType::kList);
        s_carriesBake[id] = isBake ? 1 : 2;
        return isBake;
    }

    void InvalidateBodyScale(std::uint32_t id)   // any thread (the OBody VM event sink)
    {
        std::lock_guard<std::mutex> g(g_bsInvalMx);
        if (g_bsInval.size() < 256) g_bsInval.push_back(id);
    }

    // ══ PATH-B FOUNDATION TEST (2026-07-20): skin-weight bone anchoring + 12-angle girth ═══════
    // Proves the base-mesh-INDEPENDENT measurement for direct capsule placement. Every body vertex
    // is assigned to its DOMINANT skeleton bone via SKIN WEIGHTS (the anatomical anchor identical
    // across CBBE / 3BA / custom / high-poly — the game's own mesh->skeleton binding), then each
    // bone's flesh is measured as 12 evenly-spaced angular max-radii around its centroid. Pure
    // DIAGNOSTIC: logs decode-validation + a per-bone z-ladder + per-target sectors, MOVES NOTHING.
    // Gated by meshBoneTest; fires once per fresh latch, same pulse as MESHGIRTH.
    //
    // Vertex skin layout (VertexDesc.h::GetSize): VF_SKINNED = 4x half WEIGHTS then 4x u8 INDICES
    // (12 bytes) at GetAttributeOffset(VA_SKINNING). The u8 index is LOCAL to the partition palette:
    // part.bones[local] = global index into skinInstance->bones[] (the bone NiAVObject). We
    // SELF-CALIBRATE the skinning byte offset (find the offset whose 4 half-weights sum ~1.0 with
    // in-range indices) — descriptors have burned us before (PlausibleDecode, positions).
    static bool SampleBoneBands(RE::Actor* actor, std::uint32_t id, BoneBands& out, bool verbose,
                                const RE::BSGeometry* exclude)
    {
        const auto t0 = std::chrono::steady_clock::now();   // cost is logged per call (perf question)
        BodyMesh bm;
        if (!FindBodyMesh(actor, bm, false, exclude)) { if (verbose) logger::info("BONETEST {:08X} no body mesh", id); return false; }
        RE::NiSkinPartition* sp = bm.skinPart;
        RE::NiSkinInstance*  si = bm.skinInst;
        const char* mname = bm.geo ? bm.geo->name.c_str() : "?";
        out.geo = bm.geo;
        if (!sp || !si || sp->numPartitions == 0) {
            if (verbose) logger::info("BONETEST {:08X} '{}' NO SKIN DATA (sp={} si={})", id, mname, (void*)sp, (void*)si);
            return false;
        }
        auto& part = sp->partitions[0];
        const int numBones = part.numBones;
        const std::uint16_t* palette = part.bones;
        if (!palette || numBones <= 0) { if (verbose) logger::info("BONETEST {:08X} '{}' empty palette (numBones={})", id, mname, numBones); return false; }

        auto boneName = [&](int g) -> const char* {
            if (g < 0 || g > 511) return "?";
            RE::NiAVObject* nd = si->bones ? si->bones[g] : nullptr;
            const char* nm = nd ? nd->name.c_str() : nullptr;
            return (nm && *nm) ? nm : "?";
        };

        const int stride = bm.stride;
        // ── STRUCTURAL FACTS FIRST (these alone diagnose a failed decode)
        if (verbose) {
        logger::info("BONETEST {:08X} '{}' n={} stride={} half={} numParts={} spVerts={}",
                     id, mname, bm.n, stride, bm.half, sp->numPartitions, sp->vertexCount);
        for (std::uint32_t pi = 0; pi < sp->numPartitions && pi < 8; ++pi) {
            auto& pp = sp->partitions[pi];
            logger::info("BONETEST   part[{}] verts={} numBones={} bonesPerVert={} vmap={} bones={}",
                         pi, pp.vertices, pp.numBones, pp.bonesPerVertex,
                         pp.vertexMap ? "yes" : "NULL", pp.bones ? "yes" : "NULL");
        }
        // ── RAW HEX of 4 spread verts: if every interpretation fails, this is the ground truth we
        // read the real format off of by eye — never guess a binary layout twice.
        for (int s = 0; s < 4; ++s) {
            const int i = static_cast<int>(static_cast<std::int64_t>(s) * (bm.n - 1) / 3);
            const std::uint8_t* p = bm.data + static_cast<std::size_t>(i) * stride;
            std::string hex;
            for (int b = 0; b < stride && b < 64; ++b) {
                static const char* D = "0123456789ABCDEF";
                hex += D[(p[b] >> 4) & 0xF]; hex += D[p[b] & 0xF]; hex += ' ';
            }
            logger::info("BONETEST   raw vert{}: {}", i, hex);
        }
        }   // end if(verbose)

        // ── vertex -> partition map FIRST (the self-cal needs it): bone palettes are PER-PARTITION
        // (Lydia's Softbody = 17 partitions x 18 bones), so a vertex's local bone index must resolve
        // through ITS OWN partition's palette. v1 gated on partition[0] and failed everywhere.
        std::vector<std::uint8_t> vpart(bm.n, 0);
        if (sp->numPartitions > 1) {
            for (std::uint32_t pi = 0; pi < sp->numPartitions && pi < 255; ++pi) {
                auto& pp = sp->partitions[pi];
                if (!pp.vertexMap) continue;
                for (int j = 0; j < pp.vertices; ++j) {
                    const int gv = pp.vertexMap[j];
                    if (gv >= 0 && gv < bm.n) vpart[gv] = static_cast<std::uint8_t>(pi);
                }
            }
        }

        // ── SELF-CAL v3. v2 resolved the WEIGHTS perfectly (32/32) but read the INDICES from the
        // wrong 4 bytes: scoring on weight-sum ALONE cannot tell (off=X, wFirst=0) from
        // (off=X+4, wFirst=1) — both read weights at X+4 — and the earlier offset won the tie, so
        // the "indices" were really the UV block (g=[-1 -1 -1 -1] everywhere, bones owning ~30 verts,
        // scrambled z-ladder). IN-GAME HEX GROUND TRUTH (Lydia, stride 32, FULLPREC):
        //   pos 0-15 (float3+pad) | uv 16-19 (half2) | WEIGHTS 20-27 (4x half) | INDICES 28-31 (4x u8)
        // Fix: score the INDICES too, against each vertex's OWN partition numBones — at a wrong
        // offset those bytes are UV/normal data and blow the bound, so the tie cannot happen.
        // global bone bound (max palette value across ALL partitions) — the self-cal needs it
        int maxG = 0;
        for (std::uint32_t pi = 0; pi < sp->numPartitions; ++pi) {
            auto& pp = sp->partitions[pi];
            if (!pp.bones) continue;
            for (int k = 0; k < pp.numBones; ++k) if (pp.bones[k] > maxG) maxG = pp.bones[k];
        }
        if (maxG > 511) maxG = 511;

        // ── v4: the INDEX MODE is also self-calibrated. v3 fixed the block order (wFirst=1 confirmed
        // on every mesh) but still assumed PARTITION-LOCAL indices: single-partition meshes scored
        // 32/32 while the 17-partition Softbody scored 14/32. HAND-DECODED HEX SETTLED IT (Lydia,
        // indices @28-31): vert0 = 02 01 00 00, but vert10306 = 29 02 24 00 (41, 36) and
        // vert15459 = 42 25 00 00 (66, 37). Indices reach 66 — beyond ANY partition's 17-23 palette —
        // so here the per-vertex index is a GLOBAL index into skinInstance->bones[], not a palette
        // slot. Single-partition meshes cannot distinguish the two, which is why they "passed".
        int skinOff = -1, wFmt = 0, wFirst = 1, idxMode = 0, bestOk = 0;
        auto readW = [&](const std::uint8_t* q, int k, int fmt) -> float {
            std::uint16_t raw; std::memcpy(&raw, q + 2 * k, 2);
            return fmt == 0 ? HalfToFloat(raw) : (static_cast<float>(raw) / 65535.f);
        };
        auto idxValid = [&](int i, const std::uint8_t* ip, int mode) -> bool {
            for (int c = 0; c < 4; ++c) {
                const int v = ip[c];
                if (mode == 0) {                        // GLOBAL: in range AND resolvable to a node
                    if (v > maxG || !si->bones || !si->bones[v]) return false;
                } else {                                // partition-local via that vert's palette
                    const auto& pp = sp->partitions[vpart[i]];
                    if (!pp.bones || v >= pp.numBones) return false;
                }
            }
            return true;
        };
        int scoreOf[2] = { 0, 0 };                      // best score per index-mode, for the log
        for (int off = 0; off + 12 <= stride; off += 4)
            for (int fmt = 0; fmt < 2; ++fmt)
                for (int wf = 0; wf < 2; ++wf)
                    for (int im = 0; im < 2; ++im) {
                        int ok = 0;
                        for (int k = 0; k < 32; ++k) {
                            const int i = static_cast<int>(static_cast<std::int64_t>(k) * (bm.n - 1) / 31);
                            const std::uint8_t* p  = bm.data + static_cast<std::size_t>(i) * stride + off;
                            const std::uint8_t* wp = wf ? p : p + 4;      // weights block
                            const std::uint8_t* ip = wf ? p + 8 : p;      // indices block
                            float sum = 0.f; bool fin = true;
                            for (int c = 0; c < 4; ++c) {
                                const float v = readW(wp, c, fmt);
                                if (!std::isfinite(v) || v < -0.01f || v > 1.01f) fin = false;
                                sum += v;
                            }
                            if (fin && sum > 0.95f && sum < 1.05f && idxValid(i, ip, im)) ++ok;
                        }
                        if (ok > scoreOf[im]) scoreOf[im] = ok;
                        if (ok > bestOk) { bestOk = ok; skinOff = off; wFmt = fmt; wFirst = wf; idxMode = im; }
                    }
        if (verbose)
            logger::info("BONETEST   skin-cal: off={} wFmt={} wFirst={} idxMode={} score={}/32 (global={} local={}) maxG={}",
                         skinOff, wFmt == 0 ? "half" : "unorm16", wFirst,
                         idxMode == 0 ? "GLOBAL" : "part-local", bestOk, scoreOf[0], scoreOf[1], maxG);
        if (skinOff < 0 || bestOk < 20) {
            if (verbose) logger::info("BONETEST {:08X} decode NOT resolved (best {}/32) — read the raw hex above", id, bestOk);
            return false;
        }

        // decode -> gid[] are GLOBAL bone ids, resolved through the vertex's OWN partition palette
        auto decode = [&](int i, float pos[3], float wt[4], int gid[4], int& dom) {
            MeshPos(bm, i, pos);
            const std::uint8_t* p  = bm.data + static_cast<std::size_t>(i) * stride + skinOff;
            const std::uint8_t* wp = wFirst ? p : p + 4;
            const std::uint8_t* ip = wFirst ? p + 8 : p;
            const auto& pp = sp->partitions[vpart[i]];
            dom = 0; float best = -1.f;
            for (int k = 0; k < 4; ++k) {
                wt[k] = readW(wp, k, wFmt);
                const int loc = ip[k];
                if (idxMode == 0) gid[k] = (loc <= maxG) ? loc : -1;                          // GLOBAL
                else              gid[k] = (pp.bones && loc < pp.numBones) ? pp.bones[loc] : -1;
                if (wt[k] > best) { best = wt[k]; dom = k; }
            }
        };

        // decode validation — 6 spread verts (weights should sum ~1.0; dom bone name should be sane)
        if (verbose)
        for (int s = 0; s < 6; ++s) {
            const int i = static_cast<int>(static_cast<std::int64_t>(s) * (bm.n - 1) / 5);
            float pos[3], wt[4]; int gid[4], dom;
            decode(i, pos, wt, gid, dom);
            logger::info("BONETEST   vert{} z={:.1f} w=[{:.2f} {:.2f} {:.2f} {:.2f}] sum={:.2f} g=[{} {} {} {}] dom='{}'",
                         i, pos[2], wt[0], wt[1], wt[2], wt[3], wt[0] + wt[1] + wt[2] + wt[3],
                         gid[0], gid[1], gid[2], gid[3], boneName(gid[dom]));
        }

        // one decode pass: store {x,y,z,domGlobal}; census per global bone
        // ── MEMBERSHIP = WEIGHT THRESHOLD, not winner-takes-all. Dominant-only STARVED the waist
        // (Spn0 got 129 verts and 4 empty sectors on all three bodies, because Pelvis (3278) and
        // Spine1 (1165) out-voted it for the same flesh). Real flesh is SHARED between bones, so a
        // vertex counts toward EVERY bone it is >= kW weighted to. Fills every ring properly.
        constexpr float kW = 0.25f;
        struct Acc { int n = 0; double sx = 0, sy = 0, sz = 0; float zmin = 1e9f, zmax = -1e9f; };
        std::vector<Acc> acc(maxG + 1);
        struct Vtx { float x, y, z; int g[4]; float w[4]; };
        std::vector<Vtx> vtx(bm.n);
        for (int i = 0; i < bm.n; ++i) {
            float pos[3], wt[4]; int gid[4], dom;
            decode(i, pos, wt, gid, dom);
            Vtx v; v.x = pos[0]; v.y = pos[1]; v.z = pos[2];
            for (int k = 0; k < 4; ++k) { v.g[k] = gid[k]; v.w[k] = wt[k]; }
            vtx[i] = v;
            for (int k = 0; k < 4; ++k) {
                const int g = gid[k];
                if (g < 0 || g > maxG || wt[k] < kW) continue;
                Acc& a = acc[g];
                a.n++; a.sx += pos[0]; a.sy += pos[1]; a.sz += pos[2];
                if (pos[2] < a.zmin) a.zmin = pos[2];
                if (pos[2] > a.zmax) a.zmax = pos[2];
            }
        }

        // per-bone z-ladder (bones with real vert mass, sorted by zmean — the anatomical partition)
        if (verbose) {
        std::vector<int> order;
        for (int g = 0; g <= maxG; ++g) if (acc[g].n > 20) order.push_back(g);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return (acc[a].sz / acc[a].n) < (acc[b].sz / acc[b].n); });
        for (int g : order)
            logger::info("BONETEST   bone[{}] '{}' n={} zmean={:.1f} z=[{:.1f}..{:.1f}]",
                         g, boneName(g), acc[g].n, acc[g].sz / acc[g].n, acc[g].zmin, acc[g].zmax);

        // 12-angle girth for the spine + breast targets (waist=Spn0, belly=Spn1, chest=Spn2)
        // ── BAND PROFILES. Soft/breast/butt bones are BODY-TYPE SPECIFIC and cannot be relied on
        // (3BA: 'L Breast02/03' · Softbody: 'Soft_L101..R803' · plain CBBE (Yvanni): NONE — that
        // flesh binds straight to Spine2/Pelvis). Only the CORE skeleton bones are universal, so we
        // profile every band-relevant bone by SUBSTRING and derive the soft bands from sector
        // direction: sectors 0-2/9-11 ~ +X side, 3-5 ~ +Y (front = breast/belly), 6-8 ~ -Y (rear =
        // butt). Front-vs-rear max on the chest/pelvis ring IS the breast/butt protrusion.
        static const char* kWant[] = { "Spine", "Pelvis", "Thigh", "Calf",
                                       "UpperArm", "Upperarm", "Breast", "Butt", "Soft_" };
        for (int g = 0; g <= maxG; ++g) {
            if (acc[g].n < 60) continue;
            const char* bn = boneName(g);
            bool want = false;
            for (const char* kk : kWant) if (std::strstr(bn, kk)) { want = true; break; }
            if (!want) continue;
            const float cx = static_cast<float>(acc[g].sx / acc[g].n);
            const float cy = static_cast<float>(acc[g].sy / acc[g].n);
            float sect[12] = {};
            int filled = 0;
            for (int i = 0; i < bm.n; ++i) {
                bool in = false;
                for (int k = 0; k < 4; ++k) if (vtx[i].g[k] == g && vtx[i].w[k] >= kW) { in = true; break; }
                if (!in) continue;
                const float dx = vtx[i].x - cx, dy = vtx[i].y - cy;
                const float rad = std::sqrt(dx * dx + dy * dy);
                int bin = static_cast<int>((std::atan2(dy, dx) + 3.14159265f) / (2.f * 3.14159265f) * 12.f);
                if (bin < 0) bin = 0; if (bin > 11) bin = 11;
                if (rad > sect[bin]) sect[bin] = rad;
            }
            float mean = 0.f, mx = 0.f, mn = 1e9f;
            for (float s : sect) { mean += s; if (s > mx) mx = s; if (s < mn) mn = s; if (s > 0.01f) ++filled; }
            mean /= 12.f;
            logger::info("BONETEST   BAND '{}' n={} zmean={:.1f} z=[{:.1f}..{:.1f}] c=({:.1f},{:.1f}) fill={}/12 "
                         "sect=[{:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}] "
                         "mean={:.2f} min={:.2f} max={:.2f}",
                         bn, acc[g].n, acc[g].sz / acc[g].n, acc[g].zmin, acc[g].zmax, cx, cy, filled,
                         sect[0], sect[1], sect[2], sect[3], sect[4], sect[5], sect[6], sect[7],
                         sect[8], sect[9], sect[10], sect[11], mean, mn, mx);
        }
        }   // end if(verbose) diagnostics

        // ══ UV LANDMARK RESOLVE (2026-07-22) — the ReShape reference measurement ══════════════
        // For each landmark: find the vertex nearest its UV, then express it BONE-LOCAL. That is a
        // straight line from the anatomy to the Havok joint that owns it. Nothing is inferred.
        {
            int uvOff2 = -1;
            for (int off = 4; off + 4 <= stride && uvOff2 < 0; off += 2) {
                int ok = 0; float umin = 9.f, umax = -9.f, vmin = 9.f, vmax = -9.f;
                for (int k = 0; k < 24; ++k) {
                    const int i = static_cast<int>(static_cast<std::int64_t>(k) * (bm.n - 1) / 23);
                    const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + off;
                    std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                    const float u = HalfToFloat(a), v = HalfToFloat(b2);
                    if (!std::isfinite(u) || !std::isfinite(v)) { umax = -9.f; break; }
                    if (u > -0.1f && u < 1.1f && v > -0.1f && v < 1.1f) ++ok;
                    if (u < umin) umin = u; if (u > umax) umax = u;
                    if (v < vmin) vmin = v; if (v > vmax) vmax = v;
                }
                if (ok >= 22 && (umax - umin) > 0.05f && (vmax - vmin) > 0.05f) uvOff2 = off;
            }
            if (uvOff2 >= 0) {
                for (int m = 0; m < kNumUvLandmarks; ++m) {
                    const auto& Ldef = kUvLandmarks[m];
                    struct { const char* name; float u, v; int bone; } L{
                        Ldef.name, ObjectHold::LmUV(m, 0), ObjectHold::LmUV(m, 1), Ldef.bone };
                    int bi = -1; float bd = 1e9f;
                    for (int i = 0; i < bm.n; ++i) {
                        const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + uvOff2;
                        std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                        const float u = HalfToFloat(a), v = HalfToFloat(b2);
                        if (!std::isfinite(u) || !std::isfinite(v)) continue;
                        const float d = (u - L.u) * (u - L.u) + (v - L.v) * (v - L.v);
                        if (d < bd) { bd = d; bi = i; }
                    }
                    if (bi < 0) continue;
                    const float err = std::sqrt(bd);
                    // a body that does NOT share the CBBE UV layout fails LOUDLY here instead of
                    // silently measuring the wrong anatomy
                    if (err > 0.02f) {
                        logger::info("UVLM {:08X} '{}' {} REJECTED uvErr={:.4f} — body may not share the CBBE UV layout",
                                     id, mname, L.name, err);
                        continue;
                    }
                    float fz = kSpine2FrameZ, fy = kSpine2BindY, fx = 0.f;
                    if      (L.bone == 1) { fz = kComFrameZ;    fy = kComBindY; }
                    else if (L.bone == 2) { fz = kSpine1FrameZ; fy = kSpine1BindY; }
                    else if (L.bone == 3) { fz = kSpine0FrameZ; fy = kSpine0BindY; }
                    // LIMBS are the first OFF-CENTRE frames: unlike every spine bone, their bind
                    // X is non-zero, so the landmark must be de-biased on all THREE axes.
                    else if (L.bone == 4) { fz = kRThighFrameZ; fy = kRThighBindY; fx = kRThighBindX; }
                    else if (L.bone == 5) { fz = kRUArmFrameZ;  fy = kRUArmBindY;  fx = kRUArmBindX;  }
                    out.lmMesh[m][0] = vtx[bi].x; out.lmMesh[m][1] = vtx[bi].y; out.lmMesh[m][2] = vtx[bi].z;
                    float d3[3] = { vtx[bi].x - fx, vtx[bi].y - fy, vtx[bi].z - fz };
                    if (L.bone == 4 || L.bone == 5) {
                        // rotated bone -> turn the offset into the bone's OWN frame (R^T * d)
                        const auto& M = (L.bone == 4) ? kRThighRot : kRUArmRot;
                        float r3[3];
                        for (int c = 0; c < 3; ++c)
                            r3[c] = M[0][c]*d3[0] + M[1][c]*d3[1] + M[2][c]*d3[2];
                        d3[0] = r3[0]; d3[1] = r3[1]; d3[2] = r3[2];
                    }
                    out.lmPos[m][0]  = d3[0];
                    out.lmPos[m][1]  = d3[1];
                    out.lmPos[m][2]  = d3[2];
                    out.lmErr[m] = err; out.lmOk[m] = true;
                    logger::info("UVLM {:08X} '{}' {:<13} uvErr={:.4f} vert{} local=({:6.2f},{:6.2f},{:6.2f}) [{}] uv=({:.4f},{:.4f})",
                                 id, mname, L.name, err, bi,
                                 out.lmPos[m][0], out.lmPos[m][1], out.lmPos[m][2],
                                 kLmBoneName[L.bone], L.u, L.v);
                }
            } else {
                logger::info("UVLM {:08X} '{}' no UV channel found — landmarks unavailable", id, mname);
            }
        }

        // ══ ARMOR-CHECK (2026-07-23, user request): OBody refits fire on DRESSED NPCs all the
        // time, and the sampler measures whichever mesh won the finder contest. Classify every
        // sample into the three dressed scenarios so the log SAYS which one happened:
        //   1  outfit carries the reference body -> all landmarks resolve on real skin, valid
        //   2  covered skin is zapped            -> nearest-UV lands far away, uvErr rejects,
        //                                           those channels go INERT (capsules keep values)
        //   3  an ARMOR mesh won the contest and its own UV islands alias a target UV within
        //      0.02 -> passes the uvErr gate while measuring a pauldron. The uvErr gate CANNOT
        //      catch this — but anatomy can: the sternum/navel/waist are CENTRELINE points, the
        //      torso stacks chest>belly>waist in height, front points sit forward (+Y) and the
        //      cheek behind (−Y) in mesh space. A pauldron passing ALL of that is ~impossible.
        // Scenario 3 DROPS the whole landmark set (fail loud, never measure a pauldron).
        {
            int nAcc = 0, nRej = 0;
            for (int m = 0; m < kNumUvLandmarks; ++m) (out.lmOk[m] ? nAcc : nRej)++;
            bool bad = false; char why[96] = "";
            auto chk = [&](bool cond, const char* tag) {
                if (!cond && !bad) { bad = true; snprintf(why, sizeof(why), "%s", tag); }
            };
            if (out.lmOk[kLmChest] && out.lmOk[kLmBelly] && out.lmOk[kLmWaist]) {
                chk(std::fabs(out.lmMesh[kLmChest][0]) < 1.5f &&
                    std::fabs(out.lmMesh[kLmBelly][0]) < 1.5f &&
                    std::fabs(out.lmMesh[kLmWaist][0]) < 1.5f, "centreline point off X=0");
                chk(out.lmMesh[kLmChest][2] > out.lmMesh[kLmBelly][2] &&
                    out.lmMesh[kLmBelly][2] > out.lmMesh[kLmWaist][2], "chest/belly/waist height order");
            }
            if (out.lmOk[kLmNipple]) chk(out.lmMesh[kLmNipple][1] > 0.f, "nipple not forward");
            if (out.lmOk[kLmButt])   chk(out.lmMesh[kLmButt][1]   < 0.f, "butt cheek not behind");
            if (bad && nAcc >= 4) {
                logger::info("ARMOR-CHECK {:08X} mesh='{}' vc={} VERDICT=3 WRONG MESH ({}) — "
                             "landmark set DROPPED, capsules keep their values", id, mname, bm.n, why);
                for (int m = 0; m < kNumUvLandmarks; ++m) out.lmOk[m] = false;
            } else if (nRej > 0) {
                logger::info("ARMOR-CHECK {:08X} mesh='{}' vc={} VERDICT=2 skin partly absent — "
                             "{}/{} landmarks inert, those channels hold", id, mname, bm.n, nRej,
                             static_cast<int>(kNumUvLandmarks));
            } else {
                logger::info("ARMOR-CHECK {:08X} mesh='{}' vc={} VERDICT=1 full body under outfit — "
                             "all {} landmarks valid", id, mname, bm.n, static_cast<int>(kNumUvLandmarks));
            }
        }

        // ══ DERIVED BODY MEASURES from the UV landmarks (2026-07-22, user's design) ═══════════
        // Each is a straight line between two ANATOMICAL POINTS — no statistics, no neutral.
        //   BREAST SIZE  = nipple.Y - chest_center.Y   (the sternum is the true chest reference, so a
        //                  BROAD chest with small breasts can no longer read as a big bust — this is
        //                  the whole reason every earlier "protrusion" measure failed)
        //   CUP/THICKNESS= |breast_up - breast_dn|     (vertical extent of the breast)
        //   SAG          = nipple.Z - midpoint(up,dn).Z  (negative = tip hangs BELOW the mid-line)
        if (out.lmOk[kLmNipple] && out.lmOk[kLmChest]) {
            const float size = out.lmPos[kLmNipple][1] - out.lmPos[kLmChest][1];
            float thick = 0.f, sag = 0.f;
            if (out.lmOk[kLmBrUp] && out.lmOk[kLmBrDn]) {
                const float dx = out.lmPos[kLmBrUp][0] - out.lmPos[kLmBrDn][0];
                const float dy = out.lmPos[kLmBrUp][1] - out.lmPos[kLmBrDn][1];
                const float dz = out.lmPos[kLmBrUp][2] - out.lmPos[kLmBrDn][2];
                thick = std::sqrt(dx * dx + dy * dy + dz * dz);
                sag   = out.lmPos[kLmNipple][2] - 0.5f * (out.lmPos[kLmBrUp][2] + out.lmPos[kLmBrDn][2]);
            }
            float bellyR = 0.f, waistR = 0.f, buttR = 0.f;
            if (out.lmOk[kLmBelly]) bellyR = out.lmPos[kLmBelly][1];
            if (out.lmOk[kLmWaist]) waistR = out.lmPos[kLmWaist][1];
            if (out.lmOk[kLmButt])  buttR  = -out.lmPos[kLmButt][1];   // rear is -Y; report positive
            logger::info("UVMEAS {:08X} '{}' breastSize={:.2f} cup={:.2f} sag={:+.2f} | chest={:.2f} "
                         "belly={:.2f} waist={:.2f} buttOut={:.2f}",
                         id, mname, size, thick, sag,
                         out.lmPos[kLmChest][1], bellyR, waistR, buttR);
        }

        // ══ NIPPLE-UV SCAN (2026-07-21) ═══════════════════════════════════════════════════════
        // CBBE / 3BA / Softbody all share ONE UV LAYOUT (that is why body textures interchange), so
        // the nipple sits at a FIXED UV on every one of them even though vertex count, topology and
        // bone names all differ. Vertex indices are not portable; UVs are. tipProbe* holds a
        // SPINE2-LOCAL target (the user dialled the real nipple with the breast capsule); we report
        // the verts nearest that point and their UVs. If Softbody and 3BA agree on UV, that UV IS
        // the universal nipple key and the whole measure becomes: find the vert nearest nipple-UV.
        {
            const float tx = ObjectHold::TipProbe(0), ty = ObjectHold::TipProbe(1), tz = ObjectHold::TipProbe(2);
            if (std::fabs(tx) + std::fabs(ty) + std::fabs(tz) > 0.01f) {
                // spine2-local target -> mesh space
                const float mx = tx, my = ty + kSpine2BindY, mz = tz + kSpine2BindZ;
                // self-calibrate the UV offset: two consecutive halves, both plausibly in [-0.1,1.1]
                int uvOff = -1;
                for (int off = 4; off + 4 <= stride && uvOff < 0; off += 2) {
                    int ok = 0;
                    for (int k = 0; k < 24; ++k) {
                        const int i = static_cast<int>(static_cast<std::int64_t>(k) * (bm.n - 1) / 23);
                        const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + off;
                        std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                        const float u = HalfToFloat(a), v = HalfToFloat(b2);
                        if (std::isfinite(u) && std::isfinite(v) && u > -0.1f && u < 1.1f && v > -0.1f && v < 1.1f) ++ok;
                    }
                    // ★ a range test ALONE accepts the PAD (all zeros passed [-0.1,1.1] and won
                    // at off=12, giving UV=(0,0) on every body). A real UV set must also VARY and
                    // be non-degenerate — demand spread on both axes before believing it.
                    float u0 = 0.f, v0 = 0.f, umin = 9.f, umax = -9.f, vmin = 9.f, vmax = -9.f;
                    for (int k = 0; k < 24; ++k) {
                        const int i = static_cast<int>(static_cast<std::int64_t>(k) * (bm.n - 1) / 23);
                        const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + off;
                        std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                        const float u = HalfToFloat(a), v = HalfToFloat(b2);
                        if (!std::isfinite(u) || !std::isfinite(v)) { umax = -9.f; break; }
                        if (k == 0) { u0 = u; v0 = v; }
                        if (u < umin) umin = u; if (u > umax) umax = u;
                        if (v < vmin) vmin = v; if (v > vmax) vmax = v;
                    }
                    const bool varies = (umax - umin) > 0.05f && (vmax - vmin) > 0.05f;
                    if (ok >= 22 && varies) uvOff = off;
                }
                int best[3] = { -1, -1, -1 }; float bd[3] = { 1e9f, 1e9f, 1e9f };
                for (int i = 0; i < bm.n; ++i) {
                    const float dx = vtx[i].x - mx, dy = vtx[i].y - my, dz = vtx[i].z - mz;
                    const float d = dx * dx + dy * dy + dz * dz;
                    for (int r = 0; r < 3; ++r)
                        if (d < bd[r]) { for (int q2 = 2; q2 > r; --q2) { bd[q2] = bd[q2-1]; best[q2] = best[q2-1]; }
                                         bd[r] = d; best[r] = i; break; }
                }
                // ── UV LANDMARK LOOKUP: if a target UV is set, find the vertex nearest that UV
                // and hand it to the marker so the eye can confirm it on every body.
                const float tu = ObjectHold::NipUV(0), tv = ObjectHold::NipUV(1);
                if (uvOff >= 0 && (tu > 0.0001f || tv > 0.0001f)) {
                    int bu = -1; float bud = 1e9f;
                    for (int i = 0; i < bm.n; ++i) {
                        const std::uint8_t* q = bm.data + static_cast<std::size_t>(i) * stride + uvOff;
                        std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                        const float u = HalfToFloat(a), v = HalfToFloat(b2);
                        if (!std::isfinite(u) || !std::isfinite(v)) continue;
                        const float d = (u - tu) * (u - tu) + (v - tv) * (v - tv);
                        if (d < bud) { bud = d; bu = i; }
                    }
                    if (bu >= 0) {
                        out.tipBrP[0] = vtx[bu].x; out.tipBrP[1] = vtx[bu].y; out.tipBrP[2] = vtx[bu].z;
                        out.tipBrOk = true;
                        logger::info("NIPUV {:08X} '{}' target UV=({:.4f},{:.4f}) -> vert{} uvDist={:.4f} "
                                     "pos=({:.2f},{:.2f},{:.2f})  spine2-local=({:.2f},{:.2f},{:.2f})",
                                     id, mname, tu, tv, bu, std::sqrt(bud),
                                     vtx[bu].x, vtx[bu].y, vtx[bu].z,
                                     vtx[bu].x, vtx[bu].y - kSpine2BindY, vtx[bu].z - kSpine2BindZ);
                    }
                }
                logger::info("NIPSCAN {:08X} '{}' target(mesh)=({:.2f},{:.2f},{:.2f}) uvOff={} stride={}",
                             id, mname, mx, my, mz, uvOff, stride);
                for (int r = 0; r < 3; ++r) {
                    if (best[r] < 0) continue;
                    float u = -1.f, v = -1.f;
                    if (uvOff >= 0) {
                        const std::uint8_t* q = bm.data + static_cast<std::size_t>(best[r]) * stride + uvOff;
                        std::uint16_t a, b2; std::memcpy(&a, q, 2); std::memcpy(&b2, q + 2, 2);
                        u = HalfToFloat(a); v = HalfToFloat(b2);
                    }
                    logger::info("NIPSCAN   #{} vert{} d={:.2f}u pos=({:.2f},{:.2f},{:.2f}) UV=({:.4f},{:.4f})",
                                 r, best[r], std::sqrt(bd[r]), vtx[best[r]].x, vtx[best[r]].y, vtx[best[r]].z, u, v);
                }
            }
        }

        // ══ THE MEASUREMENT (ReShape v2 payload) ══════════════════════════════════════════════
        auto gather = [&](std::vector<int>& gs, const char* const* keys, int nk) {
            for (int g = 0; g <= maxG; ++g) {
                if (acc[g].n < 40) continue;
                const char* bn2 = boneName(g);
                for (int k = 0; k < nk; ++k) if (std::strstr(bn2, keys[k])) { gs.push_back(g); break; }
            }
        };
        auto inGroup = [&](int i, const std::vector<int>& gs) {
            for (int k = 0; k < 4; ++k) {
                if (vtx[i].w[k] < kW) continue;
                for (int g : gs) if (vtx[i].g[k] == g) return true;
            }
            return false;
        };
        // RING MEAN = 12 evenly-spaced angular max-radii about the group's own centroid. This is
        // the ring the capsule has to fill. Valid for the vertical torso/leg bands.
        auto ringMean = [&](const std::vector<int>& gs, int* outN) -> float {
            double sx = 0, sy = 0; int n = 0;
            for (int i = 0; i < bm.n; ++i) if (inGroup(i, gs)) { sx += vtx[i].x; sy += vtx[i].y; ++n; }
            if (outN) *outN = n;
            if (n < 40) return 0.f;
            const float cx = static_cast<float>(sx / n), cy = static_cast<float>(sy / n);
            float sect[12] = {};
            for (int i = 0; i < bm.n; ++i) {
                if (!inGroup(i, gs)) continue;
                const float dx = vtx[i].x - cx, dy = vtx[i].y - cy;
                const float rad = std::sqrt(dx * dx + dy * dy);
                int bin = static_cast<int>((std::atan2(dy, dx) + 3.14159265f) / (2.f * 3.14159265f) * 12.f);
                if (bin < 0) bin = 0; if (bin > 11) bin = 11;
                if (rad > sect[bin]) sect[bin] = rad;
            }
            float m = 0.f; for (float s : sect) m += s;
            return m / 12.f;
        };
        // ★★ BREAST = MOUND vs CHEST WALL — both taken from the FRONT of the SAME ring (v2.1).
        // v2.0 measured the breast off the REAR anchor, which silently included TORSO DEPTH and
        // ring-centroid placement. Result: M'rissi (petite, visibly SMALLER breasts than Lydia)
        // measured +3u over her, because her chest ring's centroid sits further back (her front
        // reads 13.8 / rear 5.5 vs Lydia's 9.6 / 9.1 — near-identical TOTAL depth 19.3 vs 18.7).
        // Wall-relative cancels torso depth AND centroid placement: both terms come from the same
        // front surface, so only the actual bulge survives.
        //   mound = mean Y of the front-most 15% of the front half
        //   wall  = mean Y of the 40..70th percentile of the front half (chest wall, mound-free)
        //   protrusion = wall - mound   (front is -Y, so wall > mound)
        // dir = -1: FRONT protrusion off the front chest wall (breast). dir = +1: REAR protrusion
        // off the rear pelvis wall (butt). v2.2: the butt got the SAME wall-relative fix as the
        // breast — its old cross-ring anchor was depth-dominated and read 12.78..12.91 on four
        // wildly different bodies (the curvy one BELOW Lydia). Wall-vs-bulge, same surface, so
        // only the cheek/mound sticks out of the subtraction.
        auto breastProt = [&](const std::vector<int>& gs, int dir, float* outMound, float* outWall) -> float {
            double sy = 0; int n = 0;
            for (int i = 0; i < bm.n; ++i) if (inGroup(i, gs)) { sy += vtx[i].y; ++n; }
            if (n < 60) return 0.f;
            const float cy = static_cast<float>(sy / n);
            std::vector<float> fy;                             // the relevant HALF, outward-signed
            for (int i = 0; i < bm.n; ++i) {
                if (!inGroup(i, gs)) continue;
                const float y = vtx[i].y;
                if (dir < 0 ? (y < cy) : (y > cy)) fy.push_back(dir < 0 ? y : -y);
            }
            if (fy.size() < 40) return 0.f;
            std::sort(fy.begin(), fy.end());                   // most-outward first (signed)
            const int m = static_cast<int>(fy.size());
            const int mEnd = m * 15 / 100, wLo = m * 40 / 100, wHi = m * 70 / 100;
            if (mEnd < 4 || wHi - wLo < 4) return 0.f;
            double sm = 0; for (int i = 0; i < mEnd; ++i) sm += fy[i];
            double sw = 0; for (int i = wLo; i < wHi; ++i) sw += fy[i];
            float mound = static_cast<float>(sm / mEnd);
            float wall  = static_cast<float>(sw / (wHi - wLo));
            if (dir > 0) { mound = -mound; wall = -wall; }      // un-sign back to real Y
            if (outMound) *outMound = mound;
            if (outWall)  *outWall  = wall;
            return dir < 0 ? (wall - mound) : (mound - wall);
        };
        // The mound vert set (past half the protrusion) drives BOTH cup and sag. v2.0's cup
        // SATURATED (every NPC 24-25u) because a 0.6x-of-tip threshold off the rear anchor let the
        // whole chest front in; anchoring on the WALL isolates real breast tissue.
        // MOUND STATS. The mound set (verts past 50% of the protrusion) is the ONLY breast/cheek
        // tissue in the ring — everything else is chest wall, shoulder or hip. cup + sag + TIP all
        // come from it. ★ 2026-07-21 THE TIP IS THE FIX: brAbsY used to be "mean Y of the front-most
        // 15% of the front half of everything Spine2 touches", and that SET is a different anatomical
        // region per mesh (3BA gives Spine2 6882 verts incl. shoulders/upper back; Softbody 2652), so
        // the number moved because the SAMPLE moved, not because the body did — Sofia read LESS
        // forward than Lydia, and Aela read differently on CBBE vs 3BA. The tip is a real point: the
        // near-max (8th) most-OUTWARD vertex of the mound only = the nipple / cheek apex. Near-max,
        // not the extreme, so one stray vert can never own it (the density rule).
        auto moundStats = [&](const std::vector<int>& gs, int dir, float mound, float wall,
                              float* outCup, float* outSagZ, float* outTipY, float outTipP[3]) {
            *outCup = 0.f; *outSagZ = 0.f; *outTipY = 0.f;
            const float prot = (dir < 0) ? (wall - mound) : (mound - wall);
            if (prot <= 0.05f) return;
            const float thresh = (dir < 0) ? (wall - 0.5f * prot) : (wall + 0.5f * prot);
            // ★★ THE Z GATE (2026-07-21, marker-confirmed). Protrusion alone does NOT isolate breast
            // or cheek: the COLLARBONE juts forward as much as a breast, and the upper hip as much as
            // a cheek. The eye check proved it — the "nipple" ghost capsule sat on Lydia's LEFT
            // COLLARBONE (z 104.3 vs breast bones 93-100, clavicle 106.6) and the "cheek apex" landed
            // in FRONT of her thigh. Both features live BELOW their ring's mean height; everything
            // above it is clavicle/shoulder or hip crest. Bone-name independent, so it holds on CBBE
            // too (no breast bones at all). This is why Sofia and Aela came out 0.01u apart — I was
            // comparing collarbones.
            double gz = 0; int gn = 0;
            for (int i = 0; i < bm.n; ++i) if (inGroup(i, gs)) { gz += vtx[i].z; ++gn; }
            const float zGate = gn ? static_cast<float>(gz / gn) : 1e9f;
            std::vector<float> zs, ys; double sz = 0;
            for (int i = 0; i < bm.n; ++i) {
                if (!inGroup(i, gs)) continue;
                if (vtx[i].z >= zGate) continue;          // reject clavicle / hip crest
                const float y = vtx[i].y;
                if ((dir < 0) ? (y >= thresh) : (y <= thresh)) continue;
                zs.push_back(vtx[i].z); sz += vtx[i].z;
                ys.push_back((dir < 0) ? y : -y);          // outward-signed for one nth_element
            }
            if (zs.size() < 12) return;
            std::nth_element(zs.begin(), zs.begin() + 3, zs.end());
            const float zlo = zs[3];
            std::nth_element(zs.begin(), zs.end() - 4, zs.end());
            const float zhi = *(zs.end() - 4);
            *outCup  = zhi - zlo;
            *outSagZ = static_cast<float>(sz / zs.size()) - kSpine2BindZ;
            std::nth_element(ys.begin(), ys.begin() + 7, ys.end());
            *outTipY = (dir < 0) ? ys[7] : -ys[7];         // un-sign back to real Y
            // ★ MARKER PROBE (2026-07-21): emit the ACTUAL vertex we are calling the tip, so the eye
            // can confirm it. The marker system existed all along (Report 04 §7 "numbers lie; the eye
            // is ground truth") but was fed only by the LEGACY sampler — v2 ran blind for its whole
            // life. If this lands on a sternum or an armpit, that explains every number in the thread.
            if (outTipP) {
                float best = 1e9f; int bi = -1;
                for (int i = 0; i < bm.n; ++i) {
                    if (!inGroup(i, gs)) continue;
                    if (vtx[i].z >= zGate) continue;      // same gate as the measure
                    const float y = vtx[i].y;
                    if ((dir < 0) ? (y >= thresh) : (y <= thresh)) continue;
                    const float sv = (dir < 0) ? y : -y;
                    const float d  = std::fabs(sv - *outTipY);
                    if (d < best) { best = d; bi = i; }
                }
                if (bi >= 0) { outTipP[0] = vtx[bi].x; outTipP[1] = vtx[bi].y; outTipP[2] = vtx[bi].z; }
            }
        };

        static const char* kChestK[] = { "Spine2" };
        static const char* kBellyK[] = { "Spine1" };
        static const char* kWaistK[] = { "NPC Spine [" };
        static const char* kThighK[] = { "Thigh" };
        static const char* kArmK[]   = { "UpperArm", "Upperarm" };
        static const char* kPelvK[]  = { "Pelvis" };
        std::vector<int> gChest, gBelly, gWaist, gThigh, gArm, gPelv;
        gather(gChest, kChestK, 1); gather(gBelly, kBellyK, 1); gather(gWaist, kWaistK, 1);
        gather(gThigh, kThighK, 1); gather(gArm,   kArmK,   2); gather(gPelv,  kPelvK,  1);

        int nc = 0, nb2 = 0, nw = 0, nt = 0, na2 = 0, np = 0;
        out.g[BodyScale::kChest]     = ringMean(gChest, &nc);
        out.g[BodyScale::kBelly]     = ringMean(gBelly, &nb2);
        out.g[BodyScale::kWaist]     = ringMean(gWaist, &nw);
        out.g[BodyScale::kThighs]    = ringMean(gThigh, &nt);
        out.g[BodyScale::kUpperArms] = ringMean(gArm,   &na2);
        ringMean(gPelv, &np);
        float mound = 0.f, wall = 0.f;
        out.g[BodyScale::kBreasts]   = breastProt(gChest, -1, &mound, &wall);
        float buMound = 0.f, buWall = 0.f;
        out.g[BodyScale::kButt]      = breastProt(gPelv,  +1, &buMound, &buWall);  // v2.2 wall-relative
        float tipBr = 0.f, tipBu = 0.f, dCup = 0.f, dSag = 0.f;
        moundStats(gChest, -1, mound,   wall,   &out.cup, &out.sagZ, &tipBr, out.tipBrP);
        moundStats(gPelv,  +1, buMound, buWall, &dCup,    &dSag,     &tipBu, out.tipBuP);
        out.tipBrOk = (out.tipBrP[0] != 0.f || out.tipBrP[1] != 0.f || out.tipBrP[2] != 0.f);
        out.tipBuOk = (out.tipBuP[0] != 0.f || out.tipBuP[1] != 0.f || out.tipBuP[2] != 0.f);
        // SIZE stays wall-relative (above). POSITION = a REAL POINT (nipple / cheek apex) minus the
        // joint — the same subtraction for every body, no regional averaging in between.
        out.brAbsY = tipBr - kSpine2BindY;
        out.buAbsY = tipBu - kComBindY;

        // ★ PLAUSIBILITY BAND (2026-07-21, the Solitude overshoot). A CLOTHED NPC's readable mesh is
        // her OUTFIT, and the mound-vs-wall split on a dress returns nonsense — the town run produced
        // breast protrusions of 0.02 and 14.19 against a known-good naked Lydia at 3.72, and the 14.19
        // became (14.19-3.72)*0.7 = 7.3u of forward shove. A real wall-relative female breast lives in
        // ~1.5..12u and a butt in ~1..12u; outside that the number is NOT the feature it claims to be.
        // 0 = the established "invalid" sentinel: MeshRegionRatio then fails for that region, it stays
        // ratio 1.0 (inert) and MeshShiftDeltas skips it — the capsule keeps its baked position rather
        // than being shoved by a measurement we know is wrong. Radius/sag ride the same verdict.
        auto band = [](float v, float lo, float hi) { return (v >= lo && v <= hi) ? v : 0.f; };
        const float rawBr = out.g[BodyScale::kBreasts], rawBu = out.g[BodyScale::kButt];
        out.g[BodyScale::kBreasts] = band(rawBr, 1.5f, 12.f);
        out.g[BodyScale::kButt]    = band(rawBu, 1.0f, 12.f);
        if (out.g[BodyScale::kBreasts] <= 0.f) { out.cup = 0.f; out.sagZ = 0.f; }
        if (out.g[BodyScale::kBreasts] <= 0.f || out.g[BodyScale::kButt] <= 0.f)
            logger::info("BONEGIRTH {:08X} '{}' IMPLAUSIBLE soft measure (breasts={:.2f} butt={:.2f}) — "
                         "likely a CLOTHED/outfit mesh; those regions left inert", id, mname, rawBr, rawBu);

        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - t0).count();
        logger::info("BONEGIRTH {:08X} '{}' chest={:.2f} breasts={:.2f} belly={:.2f} waist={:.2f} "
                     "butt={:.2f} thighs={:.2f} arms={:.2f} cup={:.2f} sagZ={:.2f} "
                     "brAbsY={:.2f} buAbsY={:.2f} (n {}/{}/{}/{}/{}) [{:.0f}us]",
                     id, mname, out.g[0], out.g[1], out.g[2], out.g[3], out.g[4], out.g[5], out.g[6],
                     out.cup, out.sagZ, out.brAbsY, out.buAbsY, nc, nb2, nw, nt, np, us);
        // ── DRESSED-MESH GUARD, v2 port (review finding 2026-07-21): v2 had NO protection against
        // measuring an OUTFIT mesh — the legacy guard lives in SampleBodyGirths, which v2 bypasses.
        // Same recipe as legacy: a KNOWN body vertex count is trusted outright (outfits never carry
        // a body VC); anything else must fill the torso rings like a real body (a naked body puts
        // 450-2600 verts in each; the Monk Skirt class scores sparse). Reject -> return false ->
        // the caller falls to the legacy sampler, whose own guard + slider fallback take over.
        const bool knownVC = (bm.n == 15460) || (bm.n == 18436) || (bm.n == 13554) || (bm.n == 12888);
        if (!knownVC && (nw < 150 || nb2 < 150 || nc < 150)) {
            logger::info("BONEGIRTH {:08X} '{}' torso rings too sparse for an unknown mesh "
                         "(waist/belly/chest n={}/{}/{}) — likely an OUTFIT, rejecting", id, mname, nw, nb2, nc);
            out.ok = false;
            return false;
        }
        out.ok = out.g[BodyScale::kChest] > 0.1f;
        return out.ok;
    }

    // The meshBoneTest diagnostic = a VERBOSE SampleBoneBands run (decode validation + per-bone
    // z-ladder + per-bone BAND profiles). Measures only; moves nothing.
    static void BoneAnchorTest(RE::Actor* actor, std::uint32_t id)
    {
        BoneBands bands;
        SampleBoneBands(actor, id, bands, true);
    }

    void CapFixApply(RE::Actor* actor)
    {
        const unsigned gen = ObjectHold::CapFixGen();
        if (gen == 0 || !actor) return;                       // never armed this session

        // ── BAKE GATE: only actors carrying OUR female bake (list-shape COM) get their Havok bodies
        // touched. Stock skeletons (male, undead, custom-skeleton female) are LEFT ENTIRELY ALONE — this
        // is the fix for males/skeletons getting female-calibrated re-scale + capsule shrink. A null COM
        // (3D mid-load) returns false without caching, so we simply retry next tick.
        if (!ActorCarriesBake(actor)) return;

        // The 6 mirror-targeted L twins ride the identity probe too (indices 12..17), so an
        // AI-warp/teleport ragdoll rebuild re-applies the LEFT side with everything else.
        static constexpr const char* kProbeNodeL[6] = {
            "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]",
            "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]" };

        // Per-actor guard: skip while the gen is unchanged AND the bodies are the same physics objects we
        // last dressed. An AI-warp/teleport REBUILDS the ragdoll (factory shapes, same gen) — the 1 Hz
        // identity probe catches that and re-applies everything automatically (the teleport self-heal).
        const std::uint32_t id = actor->GetFormID();
        auto& ap = s_applied[id];
        if (ap.gen == gen) {
            const auto now = std::chrono::steady_clock::now();
            // PERF fix (audit 3a): per-actor probe PERIOD, not a shared 1000ms — identical
            // periods started on the same cell-load frame stay aligned forever and stack
            // their 150-400µs probes into the same frame (~1.5-2.5ms spikes). Different
            // per-actor periods drift apart permanently.
            if (now - ap.lastProbe < std::chrono::milliseconds(1000 + (int)(id % 331))) return;
            ap.lastProbe = now;
            // ── FURNITURE GATE (2026-07-13): a seated/leaning/ragdolled NPC's bodies are
            // PLANCK-loosened — measurements there are garbage. Never sample unattached;
            // on the stand-up edge, RESET and re-measure (heals any poisoned latch).
            // Hoisted ABOVE the auto-fitter (review 2026-07-18 MEDIUM x2): the fitter reads live
            // constraint floats and writes capsules — it must obey both gates below.
            const bool attached = ObjectHold::ActorRagdollAttached(actor);
            if (!attached) { ap.wasAttached = false; ap.attachedTicks = 0; }
            else {
                ++ap.attachedTicks;
                if (!ap.wasAttached) {
                    ap.wasAttached = true;
                    ap.ratioN = 0; ap.effScale = 0.f;   // clean re-measure
                    logger::info("CapFix {:08X} stood up -> re-measuring effScale (furniture gate)", id);
                }
                // CTD HARDENING (2026-07-18 crash: PPB.dll in-stack at 00013485's stand-up tick,
                // engine AV reading a stale float): the ragdoll is mid-re-attach on the stand-up
                // tick — never probe or apply on it. One settled tick (~1s) before touching her.
                if (ap.attachedTicks < 2) return;
            }
            // Sculpt-gated (review 2026-07-17 MEDIUM) + attachment-gated (2026-07-18): the auto-
            // fitter never runs on a loosened (seated) or mid-re-attach ragdoll, and during a
            // sculpt session it must not touch non-target skeletons.
            if (attached && ObjectHold::CapAutoFitEnabled() && SculptAllows(actor)) CapAutoFitArms(actor, id);
            auto* root3d = actor->Get3D();
            const float rootScale = root3d ? root3d->world.scale : 1.f;
            // ── SCALE-DRIFT EDGE (Report 17A-24/34): a live SetScale (follower frameworks,
            // console) grows the nodes with NO ragdoll rebuild — the closed window would
            // never notice. >1% drift from the latch-time root scale re-opens the window
            // exactly like the stand-up edge (s_effScale serves the old value meanwhile).
            if (ap.effScale > 0.f && ap.scaleAtLatch > 0.f && rootScale > 0.05f && rootScale < 20.f &&
                std::fabs(rootScale / ap.scaleAtLatch - 1.f) > 0.01f) {
                ap.ratioN = 0; ap.effScale = 0.f;
                logger::info("CapFix {:08X} root scale drift {:.3f} -> {:.3f} — re-measuring effScale",
                             id, ap.scaleAtLatch, rootScale);
            }
            // ── MEASURED-SCALE sampler (2026-07-14, REFERENCE-ANCHORED — user's design) ──
            // Measure the actor's XP32 NODE spacing anchor(COM)->head and anchor(COM)->calf.
            // Compared to Lydia's scale-1 reference, the ratio IS the actor's TRUE visual
            // scale — which the engine can misreport (a base-0.95 NPC whose skeleton is
            // really 1.0). NODE distances (not ragdoll body ORIGINS, which sit at the parent
            // joint) — that fix makes the scale-1 master read a true 1.0, not the old 1.033
            // that inflated her. DEBOUNCE (17A-29): open only after 2 attached ticks.
            const std::uint32_t refId = static_cast<std::uint32_t>(static_cast<std::int64_t>(ObjectHold::MeasuredScaleRefId()));
            const bool isRef = (refId != 0 && id == refId);
            // ★ refCapNow (2026-07-14): DETERMINISTIC one-shot reference read — bypasses the attached-gate
            // and the 10-sample wait entirely. Flip refCapNow>0.5 in the tuning file while standing next to
            // the reference actor (Lydia); this reads her COM/head/calf nodes NOW and logs + bakes the
            // scale-1 spans, even when the normal sampler stalls (follower AI-disabled reads "not attached").
            // Hardcode the logged numbers into g_refHeadSpan/g_refCalfSpan for the ship build, then set it back.
            if (ObjectHold::MeasuredScaleEnabled() && isRef && ObjectHold::RefCapNow() && rootScale > 0.05f && rootScale < 20.f) {
                float rc[3], rh[3], rk[3];
                if (GetNodePos(actor, "NPC COM [COM ]", rc) && GetNodePos(actor, "NPC Head [Head]", rh) &&
                    GetNodePos(actor, "NPC L Calf [LClf]", rk)) {
                    auto dd = [](const float a[3], const float b[3]) {
                        const float x=a[0]-b[0], y=a[1]-b[1], z=a[2]-b[2]; return std::sqrt(x*x + y*y + z*z);
                    };
                    g_refHeadSpan = dd(rc, rh) / rootScale;
                    g_refCalfSpan = dd(rc, rk) / rootScale;
                    logger::info("CapFix {:08X} ★ REFERENCE (refCapNow) headSpan={:.3f} calfSpan={:.3f} @scale1 "
                                 "-- HARDCODE THESE into g_refHeadSpan/g_refCalfSpan", id, g_refHeadSpan, g_refCalfSpan);
                }
            }
            if (ObjectHold::MeasuredScaleEnabled() && attached && ap.attachedTicks >= 2 && ap.effScale <= 0.f && ap.ratioN < 10) {
                float com[3], hN[3], cN[3];
                if (rootScale > 0.05f && rootScale < 20.f &&
                    GetNodePos(actor, "NPC COM [COM ]", com) &&
                    GetNodePos(actor, "NPC Head [Head]", hN) &&
                    GetNodePos(actor, "NPC L Calf [LClf]", cN)) {
                    auto d = [](const float a[3], const float b[3]) {
                        const float x=a[0]-b[0], y=a[1]-b[1], z=a[2]-b[2];
                        return std::sqrt(x*x + y*y + z*z);
                    };
                    const float headSpan = d(com, hN), calfSpan = d(com, cN);
                    if (headSpan > 20.f && calfSpan > 20.f) {        // sane pose spans only
                        // DRIFT-DURING-SAMPLING guard (17A-31): a SetScale mid-collection would blend
                        // two scales into one median. Stamp the scale at sample 0; if it drifts >1%
                        // before the 10th sample, throw the buffer away and restart cleanly.
                        if (ap.ratioN == 0) {
                            ap.sampleScale0 = rootScale;
                        } else if (std::fabs(rootScale / ap.sampleScale0 - 1.f) > 0.01f) {
                            ap.ratioN = 0;
                            ap.sampleScale0 = rootScale;
                        }
                        ap.headSamples[ap.ratioN] = headSpan;
                        ap.calfSamples[ap.ratioN] = calfSpan;
                        ++ap.ratioN;
                    }
                }
            }
            // LATCH at 10 samples. The trueScale latch is THROTTLED so a heavy-population area
            // can't fire 100 resizes at once; the reference capture and the wait-for-reference
            // path are not throttled.
            if (ObjectHold::MeasuredScaleEnabled() && attached && ap.effScale <= 0.f && ap.ratioN >= 10) {
                auto median = [](const float* s){ float t[10]; std::memcpy(t,s,sizeof t); std::sort(t,t+10); return 0.5f*(t[4]+t[5]); };
                const float medHead = median(ap.headSamples), medCalf = median(ap.calfSamples);
                if (isRef) {
                    // CAPTURE the reference (Lydia): her spacing normalized to scale 1 (÷ her scale).
                    const float refHead = medHead / rootScale, refCalf = medCalf / rootScale;
                    // PLAUSIBILITY BAND (17A-32): the reference is the single point of failure — every
                    // other NPC's trueScale divides by it. Refuse a reference measured in a broken pose
                    // (ragdolled/T-posed/warped) so one bad capture can't poison the whole population.
                    // Lydia's real spans are ~51.6/62.3u @scale1; accept a wide human band, else retry.
                    if (refHead < 30.f || refHead > 80.f || refCalf < 30.f || refCalf > 95.f) {
                        ap.ratioN = 0; ap.effScale = 0.f;            // implausible reference — re-measure
                        logger::warn("CapFix {:08X} REFERENCE rejected (implausible spans head={:.2f} calf={:.2f}) — re-measuring",
                                     id, refHead, refCalf);
                    } else {
                        // NEVER re-seed a BAKED constant per session (the old flaw: a live capture every
                        // launch silently redefined the scale-1 standard). Only seed if unbaked (<=1).
                        if (g_refHeadSpan <= 1.f || g_refCalfSpan <= 1.f) {
                            g_refHeadSpan = refHead;
                            g_refCalfSpan = refCalf;
                            logger::info("CapFix {:08X} MEASURED-SCALE REFERENCE seeded live (no baked constant): headSpan={:.2f} calfSpan={:.2f} @scale1",
                                         id, g_refHeadSpan, g_refCalfSpan);
                        } else {
                            logger::info("CapFix {:08X} reference actor measured (headSpan={:.2f} calfSpan={:.2f}) — baked constant kept ({:.2f}/{:.2f})",
                                         id, refHead, refCalf, g_refHeadSpan, g_refCalfSpan);
                        }
                        ap.effScale = 1.f;
                        ap.scaleAtLatch = rootScale;
                        s_effScale[id] = actor->GetScale();          // the master herself: never corrected
                    }
                } else if (g_refHeadSpan > 1.f && g_refCalfSpan > 1.f) {
                    static std::chrono::steady_clock::time_point s_lastLatch{};
                    const auto nowL = std::chrono::steady_clock::now();
                    if (nowL - s_lastLatch < std::chrono::milliseconds(120)) {
                        ap.ratioN = 8;   // over the per-120ms latch budget — retry the latch in ~2 probe ticks
                    } else {
                        s_lastLatch = nowL;
                        const float trueScale = 0.5f * (medHead / g_refHeadSpan + medCalf / g_refCalfSpan);
                        const float cur = CapScaleOf(actor);
                        if (trueScale > 0.3f && trueScale < 3.f) {
                            ap.effScale = trueScale;
                            ap.scaleAtLatch = rootScale;
                            // RAW true scale for the JOINT correction — latched regardless of the capsule
                            // buffer branch below (req 5: the joint code must not depend on the capsule
                            // resize decision). s_effScale may collapse to GetScale; s_trueScale never does.
                            s_trueScale[id] = trueScale;
                            if (std::fabs(trueScale - cur) < 0.02f) {   // ~2% buffer — no churn on noise
                                s_effScale[id] = cur;
                                logger::info("CapFix {:08X} MEASURED trueScale={:.4f} within buffer of {:.4f} — no correction", id, trueScale, cur);
                            } else {
                                s_effScale[id] = trueScale;
                                ap.gen = 0;                            // resize the Havok body at the true scale
                                logger::info("CapFix {:08X} MEASURED trueScale={:.4f} (was {:.4f}) -> resizing (engine scale off)", id, trueScale, cur);
                            }
                        } else {
                            ap.effScale = cur; s_effScale[id] = cur;   // implausible reading — leave as-is
                            ap.scaleAtLatch = rootScale;               // still arm the drift edge for this actor
                        }
                    }
                }
                // else: reference not captured yet (stand by Lydia first) — leave effScale 0, retry next tick.
            }
            // ── BODY SCALE latch (2026-07-14) — SEPARATE from the measured-scale block above; it does
            // NOT depend on effScale/the reference-anchored joint math, only on a settled body + morphs.
            // One-shot per NPC, cached. bodyScaleDump edge clears the whole cache so everyone re-latches.
            {
                static float s_lastBSDump = 0.f, s_lastMeshDump = 0.f, s_lastBoneTest = 0.f;
                // ── UV-KNOB AUTO RE-LATCH (2026-07-23): editing any landmark UV used to need a
                // manual bodyScaleDump bump — forgotten more than once, and stale landmarks look
                // exactly like fresh ones. Hash every live UV knob; any change re-latches everyone.
                static float s_lastUvHash = -1e30f;
                float uvHash = 0.f;
                for (int li = 0; li < kNumUvLandmarks; ++li)
                    uvHash += ObjectHold::LmUV(li, 0) * 3.1f + ObjectHold::LmUV(li, 1) * 7.7f;
                uvHash += ObjectHold::LmNoseUV(0) * 13.3f + ObjectHold::LmNoseUV(1) * 17.9f
                        + ObjectHold::LmChinUV(0) * 23.1f + ObjectHold::LmChinUV(1) * 29.7f;
                if (s_lastUvHash < -1e29f) s_lastUvHash = uvHash;          // first sweep: arm only
                else if (std::fabs(uvHash - s_lastUvHash) > 1e-5f) {
                    s_lastUvHash = uvHash;
                    s_regionRatio.clear();
                    logger::info("UVLM landmark UV changed -> full re-latch");
                }
                const float bsDump = ObjectHold::BodyScaleDump();
                const float mDump  = ObjectHold::MeshShapeDump();
                const float bTest  = ObjectHold::MeshBoneTest();      // toggling it re-latches everyone -> fires BONETEST
                if (bsDump != s_lastBSDump || mDump != s_lastMeshDump || bTest != s_lastBoneTest) {
                    s_lastBSDump = bsDump; s_lastMeshDump = mDump; s_lastBoneTest = bTest;
                    s_regionRatio.clear();      // everyone re-latches (and re-SAMPLES in mesh mode)
                }
            }
            {   // OBody push events (2026-07-18): drain per-NPC invalidations queued from the VM
                // thread (Obody_ApplyMorph sink in main.cpp) — erased actors re-latch below and
                // the freshLatch path re-dresses them, no manual pulse needed.
                std::lock_guard<std::mutex> g(g_bsInvalMx);
                for (std::uint32_t iid : g_bsInval) s_regionRatio.erase(iid);
                g_bsInval.clear();
            }
            bool freshLatch = false;
            if (ObjectHold::LmReShapeEnabled() && attached) {
                auto& rr = s_regionRatio[id];
                // Morphs load a beat after 3D; if SKEE is present, wait for HasMorphs so we never latch
                // body regions at a spurious base. A no-morph NPC (or SKEE absent) latches after a short
                // settle grace (head can still scale; body stays at base — a safe under-scale).
                const bool ready = !Interop::HasSkee() || Interop::SkeeHasMorphs(actor) || ap.attachedTicks >= 6;
                if (!rr.latched && ready) {
                    BodyScaleLatch(actor, id, isRef);
                    // 2026-07-18 RE-DRESS FIX (the "everything sticking out" gotcha): a fresh latch
                    // must re-APPLY, not just re-measure — otherwise the previous preset's geometry
                    // stays frozen on her until some capsule knob happens to change.
                    freshLatch = rr.latched;
                    if (freshLatch)
                        logger::info("CapFix {:08X} fresh BodyScale latch -> re-dressing all slots", id);
                    if (freshLatch && ObjectHold::MeshBoneTest() > 0.5f)
                        BoneAnchorTest(actor, id);      // Path-B skin-weight bone-anchor diagnostic (logs only)
                }
            }
            bool rebuilt = false;
            // 2026-07-12 review hardening: no null-guard — a stored-null that GAINS a body
            // (staggered collision attach) must also count as rebuilt. Permanently absent
            // slots stay null==null and never spin.
            for (int s = 0; s < 12 && !rebuilt; ++s)
                if (ap.bodyId[s] != GetNodeBodyPtr(actor, kSlotNode[s])) rebuilt = true;
            for (int s = 0; s < 6 && !rebuilt; ++s)
                if (ap.bodyId[12 + s] != GetNodeBodyPtr(actor, kProbeNodeL[s])) rebuilt = true;
            if (!rebuilt && !freshLatch) return;
            if (rebuilt) {
                logger::info("CapFix {:08X} body identity changed (ragdoll rebuilt) -> re-applying all slots", id);
                s_carriesBake.erase(id);            // skeleton may have swapped (werewolf/VL) -> re-detect the bake
                ap.ratioN = 0; ap.effScale = 0.f;   // re-measure the rebuilt ragdoll
                                                    // (s_effScale keeps the old value live meanwhile)
            }
            // freshLatch-only: fall through to the apply loop WITHOUT resetting the scale latch —
            // the geometry re-dresses with the new ratios; the measured scale is still valid.
        }
        // 2026-07-12 NULL-WINDOW FIX (the Lydia disable/enable saga): if the probe fires while
        // the 3D is mid-rebuild, GetNodeBodyPtr returns nulls; storing them would blind the
        // identity probe forever (stored-null entries are skipped by the `ap.bodyId[s] &&`
        // guard). Don't consume the event — retry next tick once the bodies exist.
        if (!GetNodeBodyPtr(actor, kSlotNode[0]) && !GetNodeBodyPtr(actor, kSlotNode[11]))
            return;
        ap.gen = gen;
        for (int s = 0; s < 12; ++s) ap.bodyId[s] = GetNodeBodyPtr(actor, kSlotNode[s]);
        for (int s = 0; s < 6; ++s) ap.bodyId[12 + s] = GetNodeBodyPtr(actor, kProbeNodeL[s]);

        for (int slot = 0; slot < 12; ++slot) {
            bool enable = false; float a[3], b[3], r;
            enable = SculptAllowsSlot(actor, slot) && ObjectHold::CapFixSlot(slot, a, b, r);
            int shapeType = -1;
            auto* cap = GetNodeCapsule(actor, kSlotNode[slot], &shapeType);
            if (!cap) {
                // Any list-capable slot + LIST shape (hand since 07-03; fore/upper/thigh/calf/foot
                // once the flesh-fit bake lands) → the per-child tuning path.
                if (shapeType == static_cast<int>(RE::hkpShapeType::kList) &&
                    ObjectHold::CapFixChildKnobs(slot) > 0) {
                    ApplyListSlot(actor, id, gen, slot);
                    continue;
                }
                if (enable)
                    logger::info("CapFix {:08X} {}: shape type {} is NOT a capsule / no body — needs the NIF path",
                                 id, kSlotName[slot], shapeType);
                continue;
            }
            alignas(16) float va[4], vb[4];
            _mm_store_ps(va, cap->vertexA.quad);
            _mm_store_ps(vb, cap->vertexB.quad);
            logger::debug("CapFix {:08X} gen={} {} BEFORE A=[{:.2f} {:.2f} {:.2f}] B=[{:.2f} {:.2f} {:.2f}] r={:.2f}",
                         id, gen, kSlotName[slot],
                         va[0] * kHavokToSkyrim, va[1] * kHavokToSkyrim, va[2] * kHavokToSkyrim,
                         vb[0] * kHavokToSkyrim, vb[1] * kHavokToSkyrim, vb[2] * kHavokToSkyrim,
                         cap->radius * kHavokToSkyrim);
            StampBodyMaterial(cap);   // 2026-07-18 review: stamp on RESOLVE, not on knob-pass —
                                      // a disabled slot (or sculpt-gated non-target) must still go quiet
            if (!enable) continue;
            // ReShape == ReScale (2026-07-19c): measured shape scales radial endpoints + radius
            // exactly like es; slider mode stays radius-only (legacy). See ApplyScaleShape.
            ApplyScaleShape(actor, slot, 0, a, b, &r);
            // Auto-fit-owned slots: endpoints belong to the joint-derived fitter; the gen sweep only
            // applies the radius knob (writing zero A/B knobs here would squash the span for ~1s).
            static constexpr bool kAutoFitOwned[12] = { false, true, true, false,
                                                        true, true, true, true, true, true, false, false };
            if (ObjectHold::CapAutoFitEnabled() && kAutoFitOwned[slot]) {
                StampBodyMaterial(cap);   // 2026-07-18: radius-only path bypasses WriteCapsule
                cap->radius = r * 0.0142875f;
                logger::debug("CapFix {:08X} {} radius={:.1f} (endpoints -> auto-fit)", id, kSlotName[slot], r);
                MirrorSlotToLeft(actor, id, slot, a, b, r, true);   // radius-only mirror
                continue;
            }
            // DEGENERATE GUARD (the June lesson): identical endpoints collide fine but are INVISIBLE to
            // the visualizer (zero axis → NaN mesh). Never write A==B — nudge B along Z by 0.15u.
            if (std::fabs(a[0]-b[0]) < 0.01f && std::fabs(a[1]-b[1]) < 0.01f && std::fabs(a[2]-b[2]) < 0.01f)
                b[2] += 0.15f;
            WriteCapsule(cap, a, b, r);
            logger::debug("CapFix {:08X} APPLIED {} A=[{:.1f} {:.1f} {:.1f}] B=[{:.1f} {:.1f} {:.1f}] r={:.1f}",
                         id, kSlotName[slot], a[0], a[1], a[2], b[0], b[1], b[2], r);
            MirrorSlotToLeft(actor, id, slot, a, b, r, false);
        }
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════
    //  FINGER ENDPOINT TRACKING (2026-07-08)
    //
    //  The finger capsules are NIF-authored FOLLOWER bodies hung off the FingerX0 nodes (the vanilla
    //  beast-tail recipe) — they are NOT members of the 18-body ragdoll instance, so pitfall rule #3
    //  is untouched. What IS ours to maintain is their far endpoint.
    //
    //  Each capsule spans FingerX0 -> FingerX2, i.e. ACROSS the PIP joint. That span is NOT static:
    //  the far node's position in the host's frame depends on FingerX1's rotation. At bind it is
    //  already [0, -0.36, 3.11] rather than the naive [0, 0, 3.71] (18% too long, off-axis), and in a
    //  fist it swings ~90° and collapses. So we rewrite point2 EVERY FRAME:
    //
    //      localB = R_Aᵀ · (t_C − t_A)          A = [0,0,0]  (the host node's own origin)
    //
    //  R_A is the host node's WORLD rotation. NiTransform keeps rotate (orthonormal) and scale apart,
    //  and the rigid body's frame is exactly the node's world rotation+translation with scale DROPPED
    //  (verified: R/L Hand body translations equal their nodes' world bind translations to 3dp). So
    //  the transpose IS the inverse, and the accumulated world scale (0.8517 under the hand, plus any
    //  actor/RaceMenu scale) rides along in the world translations. Nothing is frozen in — this is
    //  strictly better than a static bake, which would encode one actor's scale.
    //
    //  Pure float edits (pitfall rule #1). No AABB repatch: a bare hkpCapsuleShape has no cached AABB
    //  (unlike hkpListShape, which serves the broadphase from aabbCenter/aabbHalfExtents).
    //
    //  GRACEFUL SKIP: the NIF bake is a parallel task and may not have landed — and male / beast /
    //  vanilla skeletons will never carry these bodies at all. A missing node or a node without a
    //  capsule body is skipped silently after one log line. NEVER allocates.
    // ════════════════════════════════════════════════════════════════════════════════════════════
    struct FingerPair { const char* host; const char* tip; const char* label; };
    static constexpr int kFingerCount = 8;
    // Host = FingerX0 (carries the body). Tip = FingerX2 (the SECOND node of the pair). Thumb (Finger0x)
    // is excluded. NOTE: fingers are NOT an X-mirror between hands (L Finger22 bind differs from R's) —
    // each side reads its OWN live nodes, so capMirrorL is irrelevant here and is never consulted.
    static constexpr FingerPair kFingerPairs[kFingerCount] = {
        { "NPC R Finger10 [RF10]", "NPC R Finger12 [RF12]", "R.index"  },
        { "NPC R Finger20 [RF20]", "NPC R Finger22 [RF22]", "R.middle" },
        { "NPC R Finger30 [RF30]", "NPC R Finger32 [RF32]", "R.ring"   },
        { "NPC R Finger40 [RF40]", "NPC R Finger42 [RF42]", "R.pinky"  },
        { "NPC L Finger10 [LF10]", "NPC L Finger12 [LF12]", "L.index"  },
        { "NPC L Finger20 [LF20]", "NPC L Finger22 [LF22]", "L.middle" },
        { "NPC L Finger30 [LF30]", "NPC L Finger32 [LF32]", "L.ring"   },
        { "NPC L Finger40 [LF40]", "NPC L Finger42 [LF42]", "L.pinky"  },
    };

    // Fingers 0..3 hang off the R hand, 4..7 off the L hand. Resolving each finger node from the HAND
    // subtree (~16 nodes) instead of the 749-node skeleton root turns 16 full-tree walks per frame into
    // 2 full walks + 16 tiny ones.
    static constexpr const char* kFingerHandNode[2] = { "NPC R Hand [RHnd]", "NPC L Hand [LHnd]" };

    // Per-actor gate. Deliberately stores NO NiAVObject*: a 3D reload frees the whole tree, and the
    // game's pooled allocator can hand the same addresses back — so a cached node pointer is a dangling
    // deref waiting to happen. (The existing bodyId probe only ever COMPARES pointers, never derefs; we
    // keep that property.) `root` here is compare-only; the nodes are re-resolved by name every frame.
    // `any` exists so an un-baked / male / beast skeleton costs one probe per SECOND rather than per
    // frame — the collision objects attach AFTER the 3D does, so a one-shot probe would miss them forever.
    struct FingerGate {
        const void*                           root = nullptr;   // compared, never dereferenced
        std::chrono::steady_clock::time_point lastProbe{};
        bool                                  have[kFingerCount] = {};
        bool                                  any = false;
    };
    static std::unordered_map<std::uint32_t, FingerGate> s_gate;   // hoisted (see s_applied)

    // kPreLoadGame (audit save/load latch leak): FormID-keyed latches must not survive
    // into a DIFFERENT save — recycled FormIDs (dynamic FF followers/clones especially)
    // would inherit the previous save's measured scale and body identities. Main thread,
    // same threading as the other ClearOnLoad teardowns.
    void CapFixClearOnLoad() {
        s_effScale.clear();
        s_trueScale.clear();
        s_applied.clear();
        s_carriesBake.clear();
        s_gate.clear();
        // Body-Scale per-NPC raw reads are FormID-keyed identity data (morph/head shape) — a recycled
        // FormID in a different save must not inherit them. The g_refHead* references are fixed skeleton
        // geometry (re-captured off the reference actor next session) so they are left as-is.
        s_regionRatio.clear();
    }

    void FingerCapTrack(RE::Actor* actor)
    {
        if (!ObjectHold::FingerCapTrackEnabled()) return;
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return;

        // Constructing a BSFixedString from a const char* takes the game's string-table lock. Intern the
        // 18 names ONCE, at first use.
        static const std::array<RE::BSFixedString, kFingerCount * 2 + 2> s_names = [] {
            std::array<RE::BSFixedString, kFingerCount * 2 + 2> a{};
            for (int i = 0; i < kFingerCount; ++i) {
                a[i * 2 + 0] = RE::BSFixedString(kFingerPairs[i].host);
                a[i * 2 + 1] = RE::BSFixedString(kFingerPairs[i].tip);
            }
            a[kFingerCount * 2 + 0] = RE::BSFixedString(kFingerHandNode[0]);
            a[kFingerCount * 2 + 1] = RE::BSFixedString(kFingerHandNode[1]);
            return a;
        }();

        RE::NiAVObject* hand[2] = { root->GetObjectByName(s_names[kFingerCount * 2 + 0]),
                                    root->GetObjectByName(s_names[kFingerCount * 2 + 1]) };

        auto&      fg  = s_gate[actor->GetFormID()];
        const auto now = std::chrono::steady_clock::now();

        // Re-probe on a 3D reload, and — until a finger body has been seen — once per second.
        if (fg.root != root || (!fg.any && now - fg.lastProbe >= std::chrono::milliseconds(1000))) {
            fg = FingerGate{};
            fg.root      = root;
            fg.lastProbe = now;
            std::uint32_t missMask = 0;
            for (int i = 0; i < kFingerCount; ++i) {
                auto* h     = hand[i / 4];
                auto* nodeA = h ? h->GetObjectByName(s_names[i * 2 + 0]) : nullptr;
                auto* nodeC = h ? h->GetObjectByName(s_names[i * 2 + 1]) : nullptr;
                fg.have[i]  = nodeA && nodeC && GetCapsuleOnNode(nodeA);
                if (fg.have[i]) fg.any = true; else missMask |= (1u << i);
            }
            // one line per missing finger, ONCE for the whole session (bake absent / wrong skeleton)
            static std::uint32_t s_loggedMiss = 0;
            const std::uint32_t  fresh = missMask & ~s_loggedMiss;
            if (fresh) {
                s_loggedMiss |= missMask;
                for (int i = 0; i < kFingerCount; ++i)
                    if (fresh & (1u << i))
                        logger::info("FingerCapTrack: no capsule body on '{}' ({}) — NIF bake absent for "
                                     "this skeleton; skipped (logged once)",
                                     kFingerPairs[i].host, kFingerPairs[i].label);
            }
            static bool s_loggedLive = false;
            if (fg.any && !s_loggedLive) {
                s_loggedLive = true;
                int n = 0; for (bool h : fg.have) n += h ? 1 : 0;
                logger::info("FingerCapTrack LIVE on {:08X}: {}/{} finger capsules tracked "
                             "(per-frame FingerX0->FingerX2 endpoint rewrite, r={:.2f}u)",
                             actor->GetFormID(), n, kFingerCount, ObjectHold::FingerCapR());
            }
        }
        if (!fg.any) return;                                           // nothing to drive on this skeleton

        const float rKnob = ObjectHold::FingerCapR();
        for (int i = 0; i < kFingerCount; ++i) {
            if (!fg.have[i]) continue;                                 // gate only — the nodes are re-resolved
            auto* h = hand[i / 4];
            if (!h) continue;
            auto* nodeA = h->GetObjectByName(s_names[i * 2 + 0]);
            auto* nodeC = h->GetObjectByName(s_names[i * 2 + 1]);
            auto* cap   = GetCapsuleOnNode(nodeA);                     // survives ragdoll rebuilds
            if (!cap || !nodeC) continue;

            const RE::NiMatrix3& RA = nodeA->world.rotate;             // orthonormal (scale is separate)
            const RE::NiPoint3&  tA = nodeA->world.translate;          // game units
            const RE::NiPoint3&  tC = nodeC->world.translate;
            const RE::NiPoint3   d{ tC.x - tA.x, tC.y - tA.y, tC.z - tA.z };
            float b[3] = {                                             // R_Aᵀ · d  (transpose = inverse)
                RA.entry[0][0] * d.x + RA.entry[1][0] * d.y + RA.entry[2][0] * d.z,
                RA.entry[0][1] * d.x + RA.entry[1][1] * d.y + RA.entry[2][1] * d.z,
                RA.entry[0][2] * d.x + RA.entry[1][2] * d.y + RA.entry[2][2] * d.z };
            // DEGENERATE GUARD (the June lesson, and a real case here: a full fist folds the X0->X2
            // chord back on itself). A==B collides fine but is INVISIBLE to the visualizer.
            if (std::fabs(b[0]) < 0.01f && std::fabs(b[1]) < 0.01f && std::fabs(b[2]) < 0.01f)
                b[2] = 0.15f;
            static constexpr float kOrigin[3] = { 0.f, 0.f, 0.f };     // endpoint A = the host node's origin
            WriteCapsule(cap, kOrigin, b, rKnob);                      // pure float edit; no AABB cache to repatch
        }
    }
}
