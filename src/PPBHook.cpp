#include "PCH.h"
#include "PPBHook.h"
#include "RagFrame.h"   // the ragdoll-onset receipt (read-only)
#include "Tuning.h"    // ObjectHold::HeelFixEnabled / CapFixPollFile
#include "CapFix.h"    // GrabDiag::CapFixApply
#include "PivFix.h"    // ObjectHold::PivBindRel / PivFixApply / PivFollowShoulder
#include "Diag.h"      // Diag::OnPreDrive — read-only contact-listener registration + gated capdis spike
#include "PerfSys.h"   // PerfSys::OnPreDrive — the child-LOD tier tick (inert until perfPreset > 0)
#include "GenitalProbe.h"   // genProbe research instrument (2026-08-02)
#include "NpcFingerTest.h"  // NpcFinger::OnPreDrive — the NPC finger test collider (inert until npcFingerEnable/`nfing`)
#include "DismemberGuard.h" // DF/NGD ↔ PLANCK guard: head-clone/dismembered-actor exclusion (2026-07-26)
#include "Orifice.h"        // Orifice::OnPreDrive — native orifice drive (inert until orificeEnable)
#include "PpbApi.h"        // PpbApi::CopyProbes — the divergence probe's player-caused exclusion

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>   // std::sort — the head-trim median-of-10
#include <cstring>
#include <cfloat>      // FLT_MAX — the furniture probe's nearest-marker search (2026-09-12)
#include <set>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <xmmintrin.h>
#include <Windows.h>   // QueryPerformanceCounter — the pose-conform perf instrumentation

// Pose-track pipeline needs these havok types directly. CommonLibVR exposes
// layout info via individual includes; the PCH normally pulls them in, but
// we name them explicitly here to document the dependency surface.
#include "RE/H/hkbRagdollDriver.h"
#include "RE/H/hkbCharacter.h"
#include "RE/H/hkbBehaviorGraph.h"
#include "RE/H/hkbCharacterSetup.h"
#include "RE/H/hkaSkeleton.h"
#include "RE/H/hkaRagdollInstance.h"   // pose conform: ragdoll bone count / parentIndices / rigidBodies
#include "RE/H/hkpRigidBody.h"         // pose conform: bone->node matching by rigid-body pointer
#include "RE/B/bhkCollisionObject.h"   // pose conform: node -> bhkRigidBody -> hkpRigidBody (Diag's ResolveBody chain)
#include "RE/H/hkQsTransform.h"
#include "RE/H/hkQuaternion.h"
#include "RE/H/hkVector4.h"
#include "RE/B/BShkbAnimationGraph.h"

namespace logger = SKSE::log;

namespace ArmIK {

    static std::atomic<bool>          s_poseTrackFirstFire{ false };
    static std::atomic<std::uint64_t> s_poseTrackFires{ 0 };

    // ★ 2.2.0 SPINE BREADCRUMB: which NPC and which step of ApplyToPoseTrack was running when a C++ exception escaped it.
    // Hooks.cpp's catch reads it. The 2026-09-12 20:56:35 throw (a dying summoned Frost Atronach) named nothing — the
    // catch logged one line and suppressed the rest — so it could not be attributed. Pointer writes only; no cost.
    thread_local const char*   t_spineStage = "entry";
    thread_local std::uint32_t t_spineActor = 0;
    void SpineBreadcrumb(const char*& stage, std::uint32_t& actorFormId) { stage = t_spineStage; actorFormId = t_spineActor; }

    // Wall clock (s since first call) for throttling diagnostic logs.
    static float ArmNowSeconds() {
        using clock = std::chrono::steady_clock;
        static const auto start = clock::now();
        return std::chrono::duration<float>(clock::now() - start).count();
    }

    // ── hkbGeneratorOutput / Tracks mirror layout ────────────────────────────
    // CommonLibVR has only a forward declaration for hkbGeneratorOutput, so
    // we mirror PLANCK's include/RE/havok_behavior.h layout here. Field
    // offsets have been verified against both the PLANCK source
    // (https://github.com/adamhynek/activeragdoll/blob/master/include/RE/havok_behavior.h)
    // and the observed in-memory layout used by PLANCK's own drive hook.
    namespace Hkb {

        // Standard track IDs. We only use TRACK_POSE (index 2). The enum is
        // spelled out so it's easy to grep the indices if we need to touch
        // WORLD_FROM_MODEL or the ragdoll-control tracks later.
        enum StandardTrack : int {
            TRACK_WORLD_FROM_MODEL                    = 0,
            TRACK_EXTRACTED_MOTION                    = 1,
            TRACK_POSE                                = 2,
            TRACK_FLOAT_SLOTS                         = 3,
            TRACK_RIGID_BODY_RAGDOLL_CONTROLS         = 4,
            // 5 = TRACK_RIGID_BODY_RAGDOLL_BLEND_TIME
            // ⚠ 6 and 7 READ from the vendored havok_behavior.h (tools/_research/
            // ar_havok_behavior.h, the same header PLANCK ships), NOT recalled — the
            // RAGFRAME receipt needs both to reproduce PLANCK's watchdog predicate.
            TRACK_POWERED_RAGDOLL_CONTROLS            = 6,
            TRACK_POWERED_RAGDOLL_WFM_MODE            = 7,
            TRACK_KEYFRAMED_RAGDOLL_BONES             = 8,
            // indices 9..24 unused here
        };

        // 16-byte TrackHeader. One per standard track in the m_tracks block.
        // m_dataOffset is a BYTE offset measured from the Tracks* base
        // (i.e. from output.m_tracks), NOT from the header or from output.
        struct TrackHeader {
            std::int16_t capacity;          // 00
            std::int16_t numData;           // 02
            std::int16_t dataOffset;        // 04 — byte offset from Tracks* base
            std::int16_t elementSizeBytes;  // 06
            float        onFraction;        // 08
            std::int8_t  flags;             // 0C
            std::int8_t  type;              // 0D
            std::int16_t pad0E;             // 0E
        };
        static_assert(sizeof(TrackHeader) == 0x10);

        struct TrackMasterHeader {
            std::int32_t  numBytes;   // 00 — m_numBytes in PLANCK
            std::int32_t  numTracks;  // 04 — m_numTracks
            std::uint64_t unused;     // 08 — m_unused[8]
        };
        static_assert(sizeof(TrackMasterHeader) == 0x10);

        struct Tracks {
            TrackMasterHeader masterHeader;     // 00
            TrackHeader       trackHeaders[1];  // 10 — variable-length
        };

        // ── hkaKeyFrameHierarchyUtility::ControlData ─────────────────────────────
        // Layout read from the Havok 2010 SDK on disk:
        //   Source/Animation/Ragdoll/Controller/RigidBody/hkaKeyFrameHierarchyUtility.h
        // One element PER RAGDOLL BODY, living in TRACK_RIGID_BODY_RAGDOLL_CONTROLS (track 4).
        // Defaults from the SDK are in the comments; PLANCK overwrites the three marked (*) with
        // ONE global value for every bone, which is the capability ReDrive reclaims.
        // ── hkaKeyFrameHierarchyUtility::ControlData ──────────────────────────────────────
        // ⛔ DO NOT ADD A sizeof() ASSERTION HERE, AND DO NOT USE sizeof() AS THE STRIDE.
        //
        // This layout was guessed three times: 32 bytes (8 floats), then 48 (12), and the engine
        // reported 64. Each wrong guess disabled ReDrive completely and silently via the layout
        // guard, and the 32-byte version was locked in by a static_assert I wrote myself -- which
        // reads as "verified" while only restating the same guess.
        //
        // The fix is structural: the STRIDE now comes from hdr->elementSizeBytes at runtime, so
        // this struct only has to describe the LEADING fields we actually read and write. Fields
        // beyond positionGain are never touched and their existence is irrelevant to us.
        //
        // Field ORDER below is still an assumption. It is checked at runtime against the values
        // PLANCK writes every frame (hierarchyGain 0.6 / velocityGain 0.6 / positionGain 0.05) --
        // see the plausibility gate in ReDrive. If the check fails we dump and refuse to write.
        struct ControlData {
            float hierarchyGain;              // (*) 0.17 stock, 0.60 under PLANCK — target SPACE
                                              //     (0 = MODEL, 1 = LOCAL), NOT a strength, which
                                              //     is why ReDrive does not touch it.
            float velocityDamping;            //     0.00
            float accelerationGain;           //     1.00
            float velocityGain;               // (*) 0.60 — damps the position control
            float positionGain;               // (*) 0.05 — the authority dial ReDrive scales
            // ...and more. The engine says the real element is 64 bytes; we neither know nor need
            // the rest. Anything past here is reached only through the runtime stride.
        };

        // hkbGeneratorOutput is just a (Tracks*, bool) pair. We never
        // allocate one — we only reinterpret the pointer passed into
        // driveToPose by the animation graph.
        struct GeneratorOutput {
            Tracks* tracks;         // 00
            bool    deleteTracks;   // 08
        };

        static inline TrackHeader* GetHeader(GeneratorOutput* out, int trackId) {
            if (!out || !out->tracks) return nullptr;
            if (out->tracks->masterHeader.numTracks <= trackId) return nullptr;
            return &out->tracks->trackHeaders[trackId];
        }
        static inline void* GetData(GeneratorOutput* out, const TrackHeader* hdr) {
            return reinterpret_cast<std::uint8_t*>(out->tracks) + hdr->dataOffset;
        }
    }

    // ── HEEL FIX (2026-07-02 — toggle `heelFix` in the tuning file / console `hf`) ─────────────
    // RaceMenu heel height is an NiOverride transform on the scene-graph "NPC" node — invisible to the
    // whole Havok pipeline, so a heeled NPC's ragdoll (and with it every PLANCK touch/hit zone, HIGGS
    // contact, and any weld anchor body) sits exactly heelZ (~8u, measured) BELOW her visible body.
    // Bodies can't be moved directly (PLANCK's motors re-drive them toward worldFromModel × pose every
    // frame) — so we bias the TARGET: the inner-controller hook (0xA26C05) forwards a +heelZ-biased COPY
    // of worldFromModel, and PLANCK itself drives the entire ragdoll up to the raised visual skeleton.
    // Per-thread heel bias for the CURRENT driveToPose (our outer hook wraps the whole drive, so this
    // is live when the inner-controller call fires). Hooks::InnerDriveChainHook consumes it; zeroed at
    // the top of every ApplyToPoseTrack so one actor's bias can never leak into the next driver's drive.
    static thread_local float t_heelDriveBias = 0.f;
    float GetHeelDriveBias() { return t_heelDriveBias; }

    // ── HEEL AUTHORITY: Heels Fix (HeelsFix.esp) is the SOLE source of heel recognition ──────────
    // PPB no longer judges heel height itself. Heels Fix owns the heel: it lifts the XP32 "NPC" node via
    // NiOverride and, whenever it decides an actor is actively heeled (its own internal >0.5 test), flags
    // her with HeelsFixSpell — and removes it the moment the heel goes away (barefoot, or a "stuck" heel
    // it never applied). We simply mirror that decision onto the Havok ragdoll: flagged → raise the
    // ragdoll to the raised XP32 node; not flagged → do nothing, so the Havok tracks the un-raised XP32
    // and the two stay matched by construction (Heels Fix fails → BOTH sit on the ground → still matched).
    // HeelsFixSpell = HeelsFix.esp form 0x811 (ESL). Not installed → g_heelsFixSpell null → always no-op.
    static RE::SpellItem* g_heelsFixSpell = nullptr;
    void ResolveHeelsFix()
    {
        g_heelsFixSpell = nullptr;
        if (auto* dh = RE::TESDataHandler::GetSingleton())
            g_heelsFixSpell = dh->LookupForm<RE::SpellItem>(0x811, "HeelsFix.esp");
        // 2026-08-29 (offset-is-the-state): this lookup is INFORMATIONAL ONLY. The heel fix gates
        // purely on the node lift (npcNode->local.translate.z) and runs with or without this form.
        if (g_heelsFixSpell)
            logger::info("Heels Fix found (worker ability {:08X}) — note: the heel fix reads the NODE OFFSET, "
                         "never this spell (it is Papyrus-churned: cell-detach removes it).",
                         g_heelsFixSpell->GetFormID());
        else
            logger::info("Heels Fix (HeelsFix.esp) not detected — heel fix still ACTIVE: it mirrors any lift "
                         "found on the \"NPC\" node (0.01..40u), whoever wrote it. Barefoot reads 0 = no-op.");
    }
    bool HeelsFixHeeled(RE::Actor* actor) { return g_heelsFixSpell && actor && actor->HasSpell(g_heelsFixSpell); }

    // ── STICKY HEEL GATE (2026-07-31, user-diagnosed) ───────────────────────────────────
    // Heels Fix's periodic refresh (MCM: check for stuck heels every N minutes) REMOVES its
    // ability spell and re-adds it 0.5s later (HeelsFixRefreshEffect.psc: RemoveSpell ->
    // Wait(0.5) -> re-add). Gating on HasSpell per frame therefore inherits the flicker —
    // and when the Papyrus re-add loses its race, the spell stays OFF for minutes while the
    // NiOverride offset (the thing we actually read) persists: her visual stays heeled, PPB
    // dropped the bias, her Havok body sat 8u low. Measured live: offset 8->0->8->0 within
    // 175ms at 23:12:51, then stuck low until a 3D rebuild.
    // FIX: the spell only needs to be seen ONCE per actor — after that the NODE OFFSET is
    // the authority (it is Heels Fix's own persistent output; unequipping heels removes it,
    // so z reads 0 and the bias drops naturally with no spell involved).
    static std::mutex s_heeledOnceMx;
    static std::set<std::uint32_t> s_heeledOnce;
    bool HeeledSticky(RE::Actor* actor)
    {
        if (!actor) return false;
        const std::uint32_t id = actor->GetFormID();
        if (HeelsFixHeeled(actor)) {
            std::lock_guard<std::mutex> g(s_heeledOnceMx);
            s_heeledOnce.insert(id);
            return true;
        }
        std::lock_guard<std::mutex> g(s_heeledOnceMx);
        return s_heeledOnce.count(id) != 0;
    }
    void ClearHeeledSticky()
    {
        std::lock_guard<std::mutex> g(s_heeledOnceMx);
        s_heeledOnce.clear();
    }

    static void ApplyHeelFix(RE::Actor* actor, RE::hkbRagdollDriver* driver, void* generatorOutputRaw)
    {
        if (!ObjectHold::HeelFixEnabled()) return;
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return;
        auto* npcNode = root->GetObjectByName("NPC");
        if (!npcNode) return;
        // ★ THE OFFSET IS THE STATE (user, 2026-08-29): "if she is 7u off the ground on heels,
        // that means Heels Fix is ON." Heels Fix's own output IS the lift it wrote onto the XP32
        // "NPC" node — so we READ that state and mirror it, every drive. The old sticky gate
        // ("the ability must be seen reading ON once per session before the offset is trusted")
        // waited for a transient EVENT to re-occur instead of reading the state that exists; it
        // cleared on every load, and any session where the ability never happened to read ON left
        // a visibly-heeled NPC with her collision on the ground — silently. Heels Fix remains the
        // sole authority precisely BECAUSE the offset is its own output; the sane band below is
        // the only guard needed (0 = barefoot, >40u = not a heel).
        const float heelZ = npcNode->local.translate.z;
        if (heelZ <= 0.01f || heelZ > 40.f) return;            // flagged but node not (yet) raised, or a wild value → skip
        // A KNOCKED-DOWN NPC's bodies are free physics (authoritative) — biasing the drive or the pose
        // there would sink her visual under the real bodies. Stock behavior while ragdolled.
        if (actor->IsInRagdollState()) return;
        constexpr float kSkyrimToHavok = 0.0142875f;
        t_heelDriveBias = heelZ * kSkyrimToHavok;              // ← THE lever (v3, in-VR confirmed: ragdoll +8u):
                                                               //   consumed by the inner-controller chain hook
                                                               //   (0xA26C05); the earlier track/character writes
                                                               //   were decorative and are gone.
        static float s_lastLog = -10.f;
        const float now = ArmNowSeconds();
        if (now - s_lastLog > 2.f) {
            s_lastLog = now;
            logger::info("HEELFIX {:08X} heelZ={:.1f}u -> drive bias armed (+ postPhysics root compensation)",
                         actor->GetFormID(), heelZ);
        }
        (void)driver; (void)generatorOutputRaw;
    }

    static RE::Actor* GetActorFromDriver(RE::hkbRagdollDriver* driver);   // defined below


    // The COMPENSATION half (v4): the engine/PLANCK postPhysics maps the RAISED ragdoll back into the
    // output pose with the UNBIASED transform → the whole rendered body rose with the fix (in-VR 2026-07-02).
    // So after postPhysics has written the pose, subtract heelZ from the ROOT bone's model-space translation
    // — the visual drops back onto her heels while the ragdoll stays raised. Pose translations here are in
    // GAME units (the same TRACK_POSE buffer the arm IK writes). Self-contained actor resolve — postPhysics
    // fires from a different seam than our pre-drive wrapper, so the thread-local can't be trusted here.
    void ApplyHeelPostFix(RE::hkbRagdollDriver* driver, void* generatorOutputRaw)
    {
        if (!ObjectHold::HeelFixEnabled() || !driver || !generatorOutputRaw) return;
        auto* actor = GetActorFromDriver(driver);
        if (!actor || actor == RE::PlayerCharacter::GetSingleton()) return;
        auto* root = actor->Get3D();
        if (!root) return;
        auto* npcNode = root->GetObjectByName("NPC");
        if (!npcNode) return;
        // ⚠ 2026-08-01: converting only ONE half made her float 8u — the two halves are one
        // mechanism and MUST read the same gate. 2026-08-29: BOTH halves now read the OFFSET
        // ITSELF (user: "if she is 7u off the ground on heels, Heels Fix is ON") — the node lift
        // is Heels Fix's own output, the state, not a proxy for it. The sane band below is the
        // whole gate, identical to the drive half.
        const float heelZ = npcNode->local.translate.z;
        if (heelZ <= 0.01f || heelZ > 40.f) return;
        if (actor->IsInRagdollState()) return;
        auto* gen = reinterpret_cast<Hkb::GeneratorOutput*>(generatorOutputRaw);
        auto* hdr = Hkb::GetHeader(gen, Hkb::TRACK_POSE);
        if (!hdr || hdr->numData < 1 || hdr->onFraction <= 0.f) return;
        auto* pose = reinterpret_cast<RE::hkQsTransform*>(Hkb::GetData(gen, hdr));
        alignas(16) float t[4];
        _mm_store_ps(t, pose[0].translation.quad);
        t[2] -= heelZ;                                         // root bone, model space, game units
        pose[0].translation.quad = _mm_load_ps(t);
    }

    // ── hkbRagdollDriver → Actor* ────────────────────────────────────────────
    // PLANCK's pattern: driver.character.behaviorGraph.userData is a
    // BShkbAnimationGraph*, and BShkbAnimationGraph::holder is the Actor*.
    // See PLANCK src/RE/havok.cpp (GetActorFromRagdollDriver).
    static RE::Actor* GetActorFromDriver(RE::hkbRagdollDriver* driver)
    {
        if (!driver) return nullptr;
        auto* character = driver->character;                  // hkbCharacter* at 0x80
        if (!character) return nullptr;
        auto* behaviorGraph = character->behaviorGraph.get(); // hkRefPtr<hkbBehaviorGraph> at 0x58
        if (!behaviorGraph) return nullptr;
        // hkbBehaviorGraph inherits hkbNode (via hkbGenerator/hkbBindable chain).
        // hkbNode::userData is a uint64_t at offset 0x30; PLANCK stuffs a
        // BShkbAnimationGraph* there. Read raw bytes to avoid any inheritance-
        // access weirdness across CommonLibVR versions.
        const auto userData = *reinterpret_cast<std::uint64_t*>(
            reinterpret_cast<std::uintptr_t>(behaviorGraph) + 0x30);
        if (!userData) return nullptr;
        auto* graph = reinterpret_cast<RE::BShkbAnimationGraph*>(userData);
        return graph->holder;                                 // Actor* at 0x210
    }

    // ── Math helpers (stateless; duplicated from AIHands per the split) ──────
    static int FindBoneIndex(const RE::hkaSkeleton* skel, const char* name)
    {
        if (!skel) return -1;
        const auto& bones = skel->bones;
        for (std::int32_t i = 0; i < bones.size(); ++i) {
            const char* bn = bones[i].name.c_str();
            if (bn && std::strcmp(bn, name) == 0) return i;
        }
        return -1;
    }

    // ── Quaternion ↔ matrix conversions ──────────────────────────────────────
    // Havok stores quaternions as (x,y,z,w) inside an hkVector4::quad (__m128).
    [[maybe_unused]] static RE::hkQuaternion QuatFromMatrix(const RE::NiMatrix3& m)
    {
        const auto& e = m.entry;
        float qx, qy, qz, qw;
        float tr = e[0][0] + e[1][1] + e[2][2];
        if (tr > 0.f) {
            float s = std::sqrt(tr + 1.f) * 2.f;
            qw = 0.25f * s;
            qx = (e[2][1] - e[1][2]) / s;
            qy = (e[0][2] - e[2][0]) / s;
            qz = (e[1][0] - e[0][1]) / s;
        } else if (e[0][0] > e[1][1] && e[0][0] > e[2][2]) {
            float s = std::sqrt(1.f + e[0][0] - e[1][1] - e[2][2]) * 2.f;
            qw = (e[2][1] - e[1][2]) / s;
            qx = 0.25f * s;
            qy = (e[0][1] + e[1][0]) / s;
            qz = (e[0][2] + e[2][0]) / s;
        } else if (e[1][1] > e[2][2]) {
            float s = std::sqrt(1.f + e[1][1] - e[0][0] - e[2][2]) * 2.f;
            qw = (e[0][2] - e[2][0]) / s;
            qx = (e[0][1] + e[1][0]) / s;
            qy = 0.25f * s;
            qz = (e[1][2] + e[2][1]) / s;
        } else {
            float s = std::sqrt(1.f + e[2][2] - e[0][0] - e[1][1]) * 2.f;
            qw = (e[1][0] - e[0][1]) / s;
            qx = (e[0][2] + e[2][0]) / s;
            qy = (e[1][2] + e[2][1]) / s;
            qz = 0.25f * s;
        }
        // Normalize: Havok invariant is unit quaternion. Float drift from
        // repeated matrix round-trips denormalises by tiny epsilons per
        // frame, which Havok's constraint solver is intolerant of.
        float len2 = qx*qx + qy*qy + qz*qz + qw*qw;
        if (len2 > 1e-12f) {
            float inv = 1.f / std::sqrt(len2);
            qx *= inv; qy *= inv; qz *= inv; qw *= inv;
        }
        RE::hkQuaternion q;
        q.vec.quad = _mm_setr_ps(qx, qy, qz, qw);
        return q;
    }

    // q_out = q * RotX(ang): post-multiply an X-axis rotation, i.e. rotate in the BONE'S OWN
    // local frame (local X = the finger flexion axis on XP32; positive = into the palm —
    // AIHands ArmIK verified empirically). Hamilton product against b = (sin(a/2), 0, 0, cos(a/2)).
    static RE::hkQuaternion QuatMulRotX(const RE::hkQuaternion& q, float ang)
    {
        alignas(16) float a[4];
        _mm_store_ps(a, q.vec.quad);
        const float h = 0.5f * ang, bx = std::sin(h), bw = std::cos(h);
        RE::hkQuaternion out;
        out.vec.quad = _mm_setr_ps(
            a[3] * bx + a[0] * bw,      // x
            a[1] * bw + a[2] * bx,      // y
            a[2] * bw - a[1] * bx,      // z
            a[3] * bw - a[0] * bx);     // w
        return out;
    }

    static RE::NiMatrix3 MatrixFromQuat(const RE::hkQuaternion& q)
    {
        alignas(16) float tmp[4];
        _mm_store_ps(tmp, q.vec.quad);
        float x = tmp[0], y = tmp[1], z = tmp[2], w = tmp[3];
        RE::NiMatrix3 m{};
        m.entry[0][0] = 1.f - 2.f*(y*y + z*z);
        m.entry[0][1] = 2.f*(x*y - z*w);
        m.entry[0][2] = 2.f*(x*z + y*w);
        m.entry[1][0] = 2.f*(x*y + z*w);
        m.entry[1][1] = 1.f - 2.f*(x*x + z*z);
        m.entry[1][2] = 2.f*(y*z - x*w);
        m.entry[2][0] = 2.f*(x*z - y*w);
        m.entry[2][1] = 2.f*(y*z + x*w);
        m.entry[2][2] = 1.f - 2.f*(x*x + y*y);
        return m;
    }

    static RE::NiPoint3 GetTranslation(const RE::hkQsTransform& t)
    {
        alignas(16) float tmp[4];
        _mm_store_ps(tmp, t.translation.quad);
        return { tmp[0], tmp[1], tmp[2] };
    }

    // 3x3 matrix multiply (column-vector convention; entry[row][col]).
    static RE::NiMatrix3 Mat3Mul(const RE::NiMatrix3& a, const RE::NiMatrix3& b)
    {
        RE::NiMatrix3 r{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.entry[i][j] =
                    a.entry[i][0] * b.entry[0][j] +
                    a.entry[i][1] * b.entry[1][j] +
                    a.entry[i][2] * b.entry[2][j];
            }
        }
        return r;
    }

    // ── XP32 POSE-CONFORM (2026-07-10 — knobs poseConform / poseConformDump / poseConformEveryN) ────
    // THE PROBLEM: the ragdoll-local drive pose the inner controller call receives (poseLocalSpace at
    // 0xA26C05 — ragdoll-skeleton parent-local, 18 bones, already actor-scaled + havok-scaled by the
    // engine's CopyAndApplyScaleToPose) is built from ANIMATION-DATA translations (TRACK_POSE mapped
    // through the anim->ragdoll hkaSkeletonMapper), while the RENDERED skeleton uses the XP32 NIF's
    // node-local translations. On XP32 the two disagree by units, and the delta ROTATES with pose —
    // so the driven bodies sit off the visible body in every pose, worse down the chain.
    //
    // THE CONFORM: for each ragdoll bone, recompute its ragdoll-parent-relative translation from the
    // LIVE NiAVObject chain (bone up through any non-ragdoll intermediates — clavicles, XPMSE CME
    // helpers — to the ragdoll-parent node). node->local is exactly the hybrid the rendered skeleton
    // shows: rotations are this frame's animation, translations are the NIF's. Overwrite ONLY the
    // TRANSLATIONS in poseLocalSpace (x kSkyrimToHavok x actorScale to match its units); rotations
    // stay animation-owned. The root bone (COM, parentIndices < 0) is NEVER written — its drive-pose
    // translation is model-space, anchored by worldFromModel (root motion + the heel fix's root-level
    // bias live there; the conform is bone-level, so both apply independently).
    //
    // ARCHITECTURE = the heel fix's: state is prepared per drive in the outer 0xB266AB hook
    // (PoseConformPrepare, thread-local, zeroed at the top of every ApplyToPoseTrack) and consumed
    // ONCE by Hooks::InnerDriveChainHook (0xA26C05) via ApplyPoseConform, which disarms on consume.
    namespace {

        constexpr float kConfHavokScale = 0.0142875f;   // game units -> havok m (matches the heel fix)
        constexpr int   kConfMaxBones   = 24;           // skeleton.hkx = 18 ragdoll bones (beasts share it); clamped headroom

        // The 18 ragdoll BODY nodes — PivFix's kDChild set plus COM (the same set Diag's kNode18 and
        // Probe use). Matching to ragdoll bone indices is by RIGID-BODY POINTER, never by ragdoll bone
        // name (Ragdoll_* naming differs per skeleton), so table order is irrelevant.
        constexpr const char* kConfNode18[18] = {
            "NPC COM [COM ]",
            "NPC Spine [Spn0]",  "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
            "NPC Neck [Neck]",   "NPC Head [Head]",
            "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]",
            "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
            "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]",
            "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]",
        };

        // The 17 constraint child->other pairs, verbatim from PivFix.cpp kDChild/kDOther — the ragdoll
        // hierarchy (COM is the root). NOTE: the WRITE uses the live ragdoll skeleton's parentIndices
        // (that is what hkbPoseLocalToPoseWorld composes the drive pose with, so it is the only correct
        // parent map); this table is the BUILD-TIME cross-check — a mismatch is logged loudly.
        struct ConfPair { const char* child; const char* other; };
        constexpr ConfPair kConfPairs[17] = {
            { "NPC R Hand [RHnd]",    "NPC R Forearm [RLar]"  },
            { "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]" },
            { "NPC R UpperArm [RUar]","NPC Spine2 [Spn2]"     },
            { "NPC Spine [Spn0]",     "NPC COM [COM ]"        },
            { "NPC Spine1 [Spn1]",    "NPC Spine [Spn0]"      },
            { "NPC Spine2 [Spn2]",    "NPC Spine1 [Spn1]"     },
            { "NPC Neck [Neck]",      "NPC Spine2 [Spn2]"     },
            { "NPC Head [Head]",      "NPC Neck [Neck]"       },
            { "NPC R Thigh [RThg]",   "NPC COM [COM ]"        },
            { "NPC R Calf [RClf]",    "NPC R Thigh [RThg]"    },
            { "NPC R Foot [Rft ]",    "NPC R Calf [RClf]"     },
            { "NPC L Hand [LHnd]",    "NPC L Forearm [LLar]"  },
            { "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]" },
            { "NPC L UpperArm [LUar]","NPC Spine2 [Spn2]"     },
            { "NPC L Thigh [LThg]",   "NPC COM [COM ]"        },
            { "NPC L Calf [LClf]",    "NPC L Thigh [LThg]"    },
            { "NPC L Foot [Lft ]",    "NPC L Calf [LClf]"     },
        };

        // Per-actor conform map, keyed by FormID. Node pointers are cached with the root-pointer-
        // compare-only idiom: `root` is a validity TOKEN compared against actor->Get3D() every fire
        // and never dereferenced before that check — any 3D reset changes the root pointer and forces
        // a rebuild, so no cached NiAVObject* is ever used across a tree swap. `ragdoll` re-keys on
        // PLANCK's hinge->ragdoll constraint replacement / AI-warp ragdoll rebuilds the same way.
        struct ConfCache {
            RE::NiAVObject* root    = nullptr;
            const void*     ragdoll = nullptr;
            int             n       = 0;
            RE::NiAVObject* node[kConfMaxBones]       = {};
            RE::NiAVObject* parentNode[kConfMaxBones] = {};   // ragdoll-parent bone node; the actor root for the root bone (dump chain)
            const char*     name[kConfMaxBones]       = {};   // static-table strings — safe to park in the TLS
            const char*     parentName[kConfMaxBones] = {};
            bool            isRoot[kConfMaxBones]     = {};
            bool            resolvable[kConfMaxBones] = {};
            // poseConformEveryN reuse: the composed translations (GAME units, ragdoll parent-local).
            float           t[kConfMaxBones][3]       = {};
            bool            haveDeltas = false;
            // poseConformRoot v2 (2026-07-13 sky-launch fix): the root correction is a CONSTANT
            // captured ONCE per ragdoll build, from the FIRST conform fire — before any root
            // write can leak into the measurement. Targeting the live node every frame fed back
            // (PLANCK writes the risen ragdoll into the nodes -> next target rises -> orbit).
            float           rootDelta[3]    = {};
            bool            rootDeltaValid  = false;
            // ── PPB-SKELETON GATE (2026-07-29, the feet-in-ground ship bug) ──────────────
            // The conform maps by BONE NAME, so it reached every XP32 humanoid — males and
            // unmapped NPCs included. Conform is PPB's fit layer: it may only act on actors
            // standing on a PPB skeleton. Resolved once per rebuild from the race's own-sex
            // skeleton model (the same field the runtime skeleton map repoints).
            bool            oursPPB    = false;
            bool            senseStamped = false;   // 2026-09-02: push-sense bones resolved for THIS build (any rebuild resets it)
            bool            oursLogged = false;
            // HEAD TRIM (2026-07-13, user design): after the root delta latches, sample the
            // head joint's pivot-vs-node Z (PivFix::PivHeadGapZ) ~1 Hz; the 10-sample MEDIAN
            // (Report 17A-26/27: a mean lets one scene-warp/stagger frame shift the trim),
            // if outside the 0.5u buffer, trims rootDelta.z ONCE (deadbanded one-shot per
            // capture — self-stabilizing, not a feedback loop).
            float           headTrimSamples[10] = {};
            int             headTrimN   = 0;
            bool            headTrimDone = false;
            std::uint32_t   headTrimTick = 0;
            bool            wasAttached = true;   // furniture gate edge (stand-up -> full re-capture)
            int             attachedFrames = 0;   // consecutive attached fires — capture debounce (17A-29)
            int             pushFrames = 0;       // 2026-09-12: consecutive PUSH-SENSABLE fires (six-state rule) — the sensor's own debounce
            float           lastHeelZ = -999.f;   // heel-state edge (shoe change / NiOverride arming)
            float           lastHeelBias = -999.f; // hf-toggle edge (drive bias armed/disarmed)
            // PUSH SENSE v5: ragdoll bone index per sensed trunk slot {4,5,6,11,8}; -1 = absent.
            std::int8_t     senseBone[8]  = { -1, -1, -1, -1, -1, -1, -1, -1 };
            std::int8_t     senseChild[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // v7.5 lever far-ends
            // v29 FOOT TRIP CHANNEL (2026-09-10): ragdoll bone index of each foot, resolved with the sense bones.
            // Published on its OWN map (s_footGap), never under a touch slot, so no push path can read it.
            std::int8_t     footBone[2]   = { -1, -1 };   // [0] = R foot, [1] = L foot
            std::uint32_t   phase      = 0;   // per-actor fire counter for the EveryN stride
            std::uint32_t   winEpoch   = 0;   // 60 s summary: counted-this-window latch
        };
        // Drive path is main-thread (the ApplyHeelFix/s_bindRelCache precedent) — no lock.
        std::unordered_map<std::uint32_t, ConfCache> s_confCache;

        // ── PUSH SENSE v5 (2026-08-29) ─────────────────────────────────────────────────────
        // Slots sensed and their ragdoll bone names (touch-API slot numbering).
        // v8.3: + neck (lever child = head) and head (leaf - the neck IS its lever already).
        // 108 = the LEFT thigh (pseudo-slot). The touch API reports both thighs as slot 8 with a
        // leftTwin flag; sensing only the right one meant a LEFT-hip push measured the WRONG BONE
        // (user-reported 2026-08-30: "for COM i think we hit the top of the leg").
        constexpr int         kSenseN = 8;
        constexpr int         kSenseSlot[kSenseN] = { 4, 5, 6, 11, 8, 7, 3, 108 };
        constexpr const char* kSenseName[kSenseN] = { "NPC Spine [Spn0]", "NPC Spine1 [Spn1]",
                                                "NPC Spine2 [Spn2]", "NPC COM [COM ]",
                                                "NPC R Thigh [RThg]", "NPC Neck [Neck]",
                                                "NPC Head [Head]",   "NPC L Thigh [LThg]" };
        // v7.5: each sensed bone's CHILD joint — the far end of the segment the hand pushes.
        // The lever converts BEND (rotation) into measured centimeters; without it a chest push
        // mostly rotates and the joint barely translates (user-observed: spine0 reacted, spine2
        // lagged — spine0 sits on the pelvis anchor and can only translate).
        constexpr const char* kSenseChild[kSenseN] = { "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
                                                 "NPC Neck [Neck]",   "NPC Spine [Spn0]",
                                                 "NPC R Calf [RClf]", "NPC Head [Head]",
                                                 "<none>",            "NPC L Calf [LClf]" };
        // Rest calibration: the constant offset between a bone's JOINT (what the pose drives)
        // and its BODY's origin, expressed BONE-LOCAL so it survives the actor turning. Latched
        // once from ~0.5 s of samples with the player far away; spread-gated so a moving actor
        // cannot latch garbage; invalidated with the conform cache. Drive-thread only, no lock.
        struct SenseCal { bool latched = false; float T[3] = {}; float mn[3] = {}, mx[3] = {};
                          int cnt = 0; float Roff[9] = { 1,0,0, 0,1,0, 0,0,1 };
                          // v8.8: her OWN idle lag, tracked while untouched and frozen on approach.
                          float bx = 0.f, by = 0.f; bool haveBase = false;
                          double triadS = 0.0;       // v11 PUSHTRIAD receipt throttle
                          // v11 sense rewrite: Z baseline + the last moment anything was near/reacting (hold-off clock).
                          // T/mn/mx/cnt/latched/Roff are RETIRED with the body-origin path (kept so old code paths compile).
                          float bz = 0.f; float lastBusyS = -1e9f; };
        std::unordered_map<std::uint64_t, SenseCal> s_senseCal;      // key = (formId<<8)|slot
        // The published gap (read by PushStep's 0x5E0885 hook — cross-thread, mutex-guarded).
        // v13: rx/ry/rz = the RAW gap (node - animation point), BEFORE the rest-lag baseline. The travel
        // sensor zeroes it at contact instead, which removes each joint's resting lag exactly without an EMA
        // that can eat the push it is supposed to measure (report 33 §1.2, the 37x defect).
        struct SenseGapV { float dx = 0.f, dy = 0.f, mag = 0.f; float tS = -1e9f;
                           float rx = 0.f, ry = 0.f, rz = 0.f; };
        std::unordered_map<std::uint64_t, SenseGapV> s_senseGap;
        std::mutex s_senseGapMx;
        // ★ v12 THE TRAVEL SENSOR (user's design, 2026-09-07 evening). The XP32 node's position expressed
        // in HER OWN FRAME: origin-relative and de-yawed, so walking and turning cancel and only a BEND
        // shows. PushStep captures this at first contact and measures |now - captured|. No animation
        // reference, no baseline, no lever, no rate — "just distance from origin on the XP32".
        struct SenseLocalV { float x = 0.f, y = 0.f, z = 0.f; float tS = -1e9f; };
        std::unordered_map<std::uint64_t, SenseLocalV> s_senseLocal;   // key = (formId<<8)|slot, same mutex
        // v11 STAND-DOWN: per actor, the time until which the push sensor publishes ZERO and learns nothing,
        // stamped by PushStep the moment it queues a stagger/ragdoll. Same mutex as the gap (cross-thread).
        std::unordered_map<std::uint32_t, double> s_senseStandDown;
        // ★ v29 FOOT TRIP CHANNEL (user 2026-09-10: "We still track the walk pose and where the node should be during a
        // walk ... we'll do the same for the feet"). Per actor, per foot: how far the foot NODE sits above the foot the
        // animation wants this frame. A stride raises both together (difference ~0); a trip or a lift raises only the
        // real foot. Same mutex as the gap. Keyed (formId << 1) | foot - deliberately NOT a touch-slot key.
        struct FootGapV { float rz = 0.f; float nodeZ = 0.f; float animZ = 0.f; float tS = -1e9f; };
        std::unordered_map<std::uint64_t, FootGapV> s_footGap;

        // Per-drive thread-local handoff to the inner hook (the heel-fix t_heelDriveBias pattern).
        struct ConfTls {
            bool          armed  = false;   // consumed + disarmed by ApplyPoseConform
            bool          write  = false;   // poseConform knob at prep time (dump can run write-free)
            bool          dump   = false;   // this drive logs the per-bone comparison
            bool          triad  = false;   // v11: this drive logs the PUSHTRIAD node/body/intent triad
            bool          knocked = false;  // v11: ragdolled / any knock state incl. get-up — read in PREP (main thread)
            float         originW[3] = {};  // v11: her origin this prep, so the triad can print ABSOLUTE node/intent positions
            float         yaw    = 0.f;    // v12: her facing this prep — the travel sensor measures in HER frame, so a TURN is not a push
            int           n      = 0;
            float         k      = 1.f;     // GAME units -> pose units: kConfHavokScale x actorScale
            float         scale  = 1.f;     // the actor scale used for k (dump header)
            std::uint32_t formId = 0;
            std::uint8_t  flags[kConfMaxBones] = {};   // 0 = write, 1 = root (skip), 2 = unresolved (skip)
            float         t[kConfMaxBones][3]  = {};
            const char*   name[kConfMaxBones]       = {};
            const char*   parentName[kConfMaxBones] = {};
            // FK-world dump support (dump fires only): live ragdoll parentIndices + XP32 node worlds.
            std::int16_t  par[kConfMaxBones]      = {};
            float         nodeW[kConfMaxBones][3] = {};
            bool          haveFk = false;
            // PUSH SENSE v5 handoff (bodies read at PREP - node/collision reads are main-thread
            // safe there, NOT in the inner hook; positions in GAME units, world).
            bool          senseArmed = false;
            std::int8_t   senseBone[8]      = { -1, -1, -1, -1, -1, -1, -1, -1 };
            std::int8_t   senseChild[8]     = { -1, -1, -1, -1, -1, -1, -1, -1 };
            bool          senseBodyValid[8] = {};
            float         senseBody[8][3]   = {};
            float         senseBodyR[8][9]  = {};   // v7.5: body world rotation (row-major)
            std::int8_t   footBone[2]       = { -1, -1 };   // v29: foot trip channel, [0] = R, [1] = L
            // ── ARM PROBE (armProbe knob): filled on the main thread in PoseConformPrepare,
            // consumed in ApplyPoseConform where the incoming drive pose is also in hand.
            // index 0 = R UpperArm, 1 = R Forearm, 2 = R Hand.
            int           armBone[3]     = { -1, -1, -1 };  // conform-map index, -1 = unmapped
            bool          armNodeOk[3]   = {};
            bool          armBodyOk[3]   = {};
            float         armNodeW[3][3] = {};   // visible XP32 node, WORLD, game units
            float         armBodyW[3][3] = {};   // Havok rigid body,  WORLD, game units
            bool          armOn          = false;
            // ── LEG PROBE (legProbe knob), v25. Same shape as the arm block above, four slots:
            // 0 = R Thigh, 1 = R Calf, 2 = R Foot, 3 = L Foot. The right leg is the chain, the left
            // foot is the symmetry control that separates a ROOT-level cause from a per-body one.
            int           legBone[4]     = { -1, -1, -1, -1 };  // conform-map index, -1 = unmapped
            bool          legNodeOk[4]   = {};
            bool          legBodyOk[4]   = {};
            float         legNodeW[4][3] = {};   // visible XP32 node, WORLD, game units
            float         legBodyW[4][3] = {};   // Havok rigid body,  WORLD, game units
            bool          legOn          = false;
            // poseConformRoot v2: 0 = off, 1 = capture-then-apply (first fire after build),
            // 2 = apply the cached constant. rootCache valid ONLY within this drive call.
            std::uint8_t  rootMode = 0;
            float         rootDelta[3] = {};
            ConfCache*    rootCache = nullptr;
        };
        thread_local ConfTls t_conf;

        // Steady-state perf counters for the Diag `perf` report (map-build spikes excluded).
        std::atomic<std::uint64_t> s_confFires{ 0 }, s_confSumNs{ 0 }, s_confMaxNs{ 0 };

        inline std::uint64_t ConfQpc() { LARGE_INTEGER t; ::QueryPerformanceCounter(&t); return (std::uint64_t)t.QuadPart; }
        inline double ConfNsPerCount() {
            static const double v = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return 1e9 / (double)f.QuadPart; }();
            return v;
        }

        // Translation of `bone`'s origin expressed in `stop`'s frame, composed from LIVE node locals
        // (bone's own local, then each intermediate's, stopping when we reach `stop` — its local is
        // NOT applied). node->local: rotations are this frame's animation, translations are the NIF's.
        // Point map per NiTransform convention: p' = R*p*s + t (the ComputePivBindRels form).
        bool ConfComposeChain(const RE::NiAVObject* bone, const RE::NiAVObject* stop, RE::NiPoint3& out)
        {
            RE::NiPoint3 p{ 0.f, 0.f, 0.f };
            const RE::NiAVObject* nd = bone;
            for (int depth = 0; nd && depth < 10; ++depth) {   // XP32 ragdoll gaps are 1-3 nodes; 10 = defensive cap
                if (nd == stop) { out = p; return true; }
                const auto& L = nd->local;
                p = (L.rotate * p) * L.scale + L.translate;
                nd = nd->parent;
            }
            return false;
        }

        // Rebuild the per-actor conform map: ragdoll bone index -> XP32 node (by rigid-body pointer)
        // -> ragdoll parent (by the live skeleton's parentIndices). ~18 GetObjectByName walks — a
        // one-off spike per 3D/ragdoll rebuild, logged and excluded from the steady-state stats.
        void ConfRebuild(ConfCache& c, RE::hkbRagdollDriver* driver, RE::NiAVObject* root)
        {
            c = ConfCache{};
            auto* rag = driver->ragdoll;
            if (!rag) return;
            const RE::hkaSkeleton* skel = rag->skeleton.get();
            if (!skel) return;
            const int           nBones = (int)skel->bones.size();
            const std::int16_t* par    = skel->parentIndices.data();
            if (nBones <= 0 || !par) return;
            const int n = nBones < kConfMaxBones ? nBones : kConfMaxBones;

            // Resolve the 18 candidate body nodes -> their live hkpRigidBody (Diag's ResolveBody chain).
            RE::hkpRigidBody* rbOf[18] = {};
            RE::NiAVObject*   ndOf[18] = {};
            for (int k = 0; k < 18; ++k) {
                auto* obj = root->GetObjectByName(kConfNode18[k]);
                if (!obj) continue;
                auto* colObj = obj->collisionObject.get();
                if (!colObj) continue;
                auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
                rbOf[k] = body ? body->GetRigidBody() : nullptr;
                ndOf[k] = obj;
            }

            // Pass 1: ragdoll bone i -> node via its rigid body (boneToRigidBodyMap when sane,
            // NOTE: identity fallback otherwise — Skyrim's instance is 1:1 in practice).
            int mapped = 0;
            for (int i = 0; i < n; ++i) {
                RE::hkpRigidBody* rb = nullptr;
                const auto& b2r = rag->boneToRigidBodyMap;
                if (b2r.size() == nBones) {
                    const int ri = b2r[i];
                    if (ri >= 0 && ri < rag->rigidBodies.size()) rb = rag->rigidBodies[ri];
                } else if (i < rag->rigidBodies.size()) {
                    rb = rag->rigidBodies[i];
                }
                if (!rb) continue;
                for (int k = 0; k < 18; ++k) {
                    if (rbOf[k] && rbOf[k] == rb) {
                        c.node[i] = ndOf[k];
                        c.name[i] = kConfNode18[k];
                        ++mapped;
                        break;
                    }
                }
            }

            // Pass 2: ragdoll parents from the live skeleton + resolvability + kConfPairs cross-check.
            int pairsOk = 0, pairsMismatch = 0, rootIdx = -1;
            for (int i = 0; i < n; ++i) {
                if (!c.node[i]) continue;
                const int p = par[i];
                if (p < 0) {
                    // worldFromModel-anchored root (COM). Dump-only chain: compose up to the actor
                    // root — model-frame-ish (includes the "NPC" node's heel offset, so a heeled NPC
                    // shows ~heelZ here by design). NEVER written.
                    c.isRoot[i]     = true;
                    c.parentNode[i] = root;
                    c.parentName[i] = "<worldFromModel>";
                    RE::NiPoint3 dummy;
                    c.resolvable[i] = ConfComposeChain(c.node[i], root, dummy);
                    rootIdx = i;
                    continue;
                }
                if (p >= n || !c.node[p]) continue;   // parent bone unmapped -> this bone stays unresolved
                c.parentNode[i] = c.node[p];
                c.parentName[i] = c.name[p];
                RE::NiPoint3 dummy;
                c.resolvable[i] = ConfComposeChain(c.node[i], c.node[p], dummy);
                for (const auto& pr : kConfPairs) {
                    if (std::strcmp(pr.child, c.name[i]) == 0) {
                        if (std::strcmp(pr.other, c.parentName[i]) == 0) ++pairsOk; else ++pairsMismatch;
                        break;
                    }
                }
            }
            c.root    = root;
            c.ragdoll = rag;
            c.n       = n;
            logger::info("PCONF map built: {} ragdoll bones{}, {} mapped to XP32 body nodes, parent pairs "
                         "{}/17 match PivFix kDChild/kDOther ({} MISMATCH), root bone = [{}] '{}'",
                         nBones, nBones > kConfMaxBones ? " (CLAMPED to 24)" : "", mapped,
                         pairsOk, pairsMismatch, rootIdx, rootIdx >= 0 ? c.name[rootIdx] : "<none>");
            if (pairsMismatch > 0)
                logger::warn("PCONF: live ragdoll hierarchy disagrees with the PivFix constraint tables on {} pair(s) — "
                             "the live skeleton's parentIndices win (they are what the drive pose composes with).", pairsMismatch);
        }

        // ★ 2026-09-02: THE PPB-SKELETON GATE, stamped on EVERY rebuild by EVERY rebuilder.
        // Before this, oursPPB was computed only inside PoseConformPrepare's rebuild branch:
        //  (1) ReDrive and DivergenceProbe rebuilt the cache themselves and never consulted it,
        //      so both acted on males and unmapped races while their comment claimed otherwise;
        //  (2) if either of them rebuilt FIRST (poseConform/pushStep off, reDrive/divProbe on),
        //      oursPPB stayed false and prep never re-evaluated it — conform + push-sense were
        //      silently dead for that actor until a 3D/ragdoll rebuild (the knob-flip workflow).
        // Own-sex model path must be one of OURS ("\PPB\" folder; forward slashes tolerated).
        void ConfStampOurs(ConfCache& c, RE::Actor* actor, std::uint32_t id)
        {
            c.oursPPB = false;
            if (auto* base = actor ? actor->GetActorBase() : nullptr) {
                if (auto* race = base->GetRace()) {
                    const auto sex = base->IsFemale() ? RE::SEXES::kFemale : RE::SEXES::kMale;
                    const char* mdl = race->skeletonModels[sex].GetModel();
                    c.oursPPB = mdl && std::strstr(mdl, "\\PPB\\") != nullptr;
                    if (!c.oursPPB && mdl)      // tolerate forward slashes in repointed paths
                        c.oursPPB = std::strstr(mdl, "/PPB/") != nullptr;
                }
            }
            if (!c.oursPPB && !c.oursLogged) {
                c.oursLogged = true;
                logger::info("PCONF skip {:08X}: not on a PPB skeleton (male/unmapped race) — "
                             "conform / ReDrive / divergence probe will never touch this actor", id);
            }
        }

        // Edge-triggered dump window (the jtrackNow idiom): poseConformDump 0 -> non-0 arms ~2 s in
        // which every driven actor dumps ONCE at the inner hook. Tick updates the window; Claim
        // consumes an actor's once-per-window slot only after a successful prep.
        bool  s_confDumpArmed = false;
        std::uint32_t s_confDumpEpoch = 0;   // increments per edge — the CLAVDUMP once-per-window key
        std::chrono::steady_clock::time_point s_confDumpEdgeAt{};
        std::unordered_set<std::uint32_t>     s_confDumpDone;

        bool ConfDumpTick()
        {
            static float s_lastVal = 0.f;
            const float cur  = ObjectHold::PoseConformDump();
            const bool  edge = cur > 0.5f && s_lastVal <= 0.5f;
            s_lastVal = cur;
            if (edge) {
                s_confDumpArmed  = true;
                ++s_confDumpEpoch;
                s_confDumpEdgeAt = std::chrono::steady_clock::now();
                s_confDumpDone.clear();
                logger::info("PCONF dump armed via poseConformDump knob — every driven actor dumps once in the "
                             "next ~2 s (set it back to 0, then non-0, to re-fire)");
            }
            if (s_confDumpArmed &&
                std::chrono::steady_clock::now() - s_confDumpEdgeAt > std::chrono::seconds(2)) {
                s_confDumpArmed = false;
                s_confDumpDone.clear();
            }
            return s_confDumpArmed;
        }
        bool ConfDumpClaim(std::uint32_t id) { return s_confDumpArmed && s_confDumpDone.insert(id).second; }

        // The outer-hook half: build/refresh the conform set for THIS drive and park it in the TLS.
        // ══════════════════════════════════════════════════════════════════════════════════
        //  ReDrive (2026-08-25) — PER-BONE DRIVE AUTHORITY
        //
        //  WHAT IT IS. Every ragdoll bone is driven toward the animated pose by a controller whose
        //  strength is `positionGain`. PLANCK stamps ONE global value into all 18 bones
        //  (planck_080 main.cpp:5241-5245) even though the palette is a PER-BONE ARRAY. ReDrive
        //  reclaims that: a multiplier per bone on how hard the animation pulls, versus how much
        //  the Havok result is allowed to stand.
        //
        //      multiplier < 1  ->  the bone YIELDS. Contacts win. It gives against a wall or
        //                          another actor's chest, and the mesh follows it (postPhysics).
        //      multiplier = 1  ->  stock. Exactly what PLANCK asked for. The safe default.
        //      multiplier > 1  ->  the bone HOLDS its animated pose harder.
        //
        //  THE INTENDED SHAPE (user's design): high authority on COM/spine so the actor stays where
        //  the animation puts her, decreasing DOWN each limb chain so the extremity gives first.
        //  A shoved arm should cost you an arm, not the whole torso. Suggested starting gradient is
        //  in PPB_tuning.txt; the shipped defaults are all 1.0 so enabling ReDrive changes NOTHING
        //  until dialled — the same discipline as every other PPB knob.
        //
        //  WHY positionGain AND velocityGain TOGETHER. `velocityGain` damps the position control.
        //  Scaling position alone would leave the damping over-strong for the new stiffness and the
        //  bone would crawl. Scaling both keeps the controller's damping ratio roughly constant,
        //  so the bone stays critically damped as authority moves. `hierarchyGain` is deliberately
        //  NOT touched: per the SDK it selects the target SPACE (model vs local), not a strength.
        //
        //  ORDERING. PLANCK writes this same palette in its own drive hook. Whoever writes LAST
        //  wins, so ReDrive logs the value it FINDS on its first fire per actor: read 0.6 and
        //  PLANCK went first (we win, correct); read the SDK default 0.17 and we are running ahead
        //  of PLANCK and are about to be overwritten. That one line settles the ordering question
        //  without guesswork.
        constexpr int kReDriveN = 18;   // ragdoll bones. CharacterBumper is layer 30, NOT drive-driven.
        std::unordered_map<std::uint32_t, std::uint8_t> s_reDriveSeen;   // ordering receipt, once per actor

        // ── WHY-NOT RECEIPTS (2026-08-28) ────────────────────────────────────────────────
        // The first VR test produced ZERO output with the knob armed and this function proven to
        // be called (DivergenceProbe runs on the line above and logged fine). Seven silent early
        // returns meant "disabled", "not reached" and "layout mismatch" were indistinguishable
        // from the log - so the session produced no verdict at all, only the absence of one.
        // Every exit now names itself, once per actor, latched in a bitmask so a per-drive guard
        // cannot flood the log. A guard that can silently disable a feature MUST announce itself.
        enum : std::uint32_t {
            kRdNullArg   = 1u << 0, kRdRagdoll  = 1u << 1, kRdNo3D    = 1u << 2,
            kRdNoTrack   = 1u << 3, kRdLayout   = 1u << 4, kRdNoData  = 1u << 5,
            kRdNoBones   = 1u << 6, kRdOK       = 1u << 7,
            kRdGrabbed   = 1u << 8,   // HIGGS holds this actor - weights stand down (Report 18 s5)
            kRdCtlBad    = 1u << 9,   // controller failed the plausibility check - refused to write
            kRdApplied   = 1u << 10,  // boneWeights actually SET on the controller (the success receipt)
            kRdClamped   = 1u << 11,  // at least one knob exceeded the hierarchy-extrapolation bound
            kRdNotPPB    = 1u << 12,  // 2026-09-02: not on a PPB skeleton (male/unmapped race) - weights stand down
        };
        std::unordered_map<std::uint32_t, std::uint32_t> s_rdWhy;   // actor -> reasons already logged

        // Log `bit` for `id` at most once. Returns true the first time only.
        bool RdSay(std::uint32_t id, std::uint32_t bit)
        {
            if (s_rdWhy.size() > 512) s_rdWhy.clear();
            std::uint32_t& seen = s_rdWhy[id];
            if (seen & bit) return false;
            seen |= bit;
            return true;
        }

        // ════════════════════════════════════════════════════════════════════════════════
        //  ReDrive v2 (2026-08-28) — per-bone authority via the ENGINE'S OWN channel.
        //
        //  v1 wrote the generatorOutput ControlData palette from the OUTER hook. Measured dead:
        //  PLANCK's PreDriveToPoseHook runs after us on that chain and re-stamps the palette
        //  every frame (hierarchyGain read 0.17 = the pre-PLANCK SDK default, proving order).
        //  The track is also ONE element + an index table, not the per-bone array doc 25 §7.3
        //  assumed — capacity=18 was measured, but expanding it means fighting PLANCK's
        //  TryForceRigidBodyControls forever.
        //
        //  v2 uses hkaKeyFrameHierarchyUtility::BodyData::m_boneWeights instead — Havok's own
        //  per-bone gain multiplier ("The gains are multiplied by these weights", SDK header),
        //  with a shipped setter whose doc says: set before driveToPose, null after. That seam
        //  is the CALL at 0xA26C05, which PPB already owns (InnerDriveChainHook). PLANCK only
        //  READS m_boneWeights (planck_080 main.cpp:6330) — no writer to race, and our weights
        //  MULTIPLY whatever PLANCK stamped instead of competing with it.
        //
        //  This function (outer hook, where actor/driver/bone-names live) only COMPUTES the
        //  weights into TLS. ReDriveInnerApply/Restore (called from InnerDriveChainHook) do the
        //  pointer swap at the exact moment Havok's own documentation prescribes.
        // ════════════════════════════════════════════════════════════════════════════════

        // The controller's leading members, mirrored from the AUTHORITATIVE header on disk:
        // tools/Havok 2010 Files/Source/Animation/Ragdoll/Controller/RigidBody/
        // hkaRagdollRigidBodyController.h — class has NO base and no vtable, so the palette is
        // at +0x00. hkArray = ptr+int+int (16B, hkArray.h:273-275). hkReal = float
        // (hkBaseTypes.h:89). ⛔ After the ControlData saga: this was READ, not guessed, and the
        // runtime plausibility check below still verifies it live before any write.
        struct HkArrayRaw { void* data; std::int32_t size; std::int32_t capacityAndFlags; };
        struct HkaRbCtlMirror {
            HkArrayRaw          palette;             // +0x00 hkArray<ControlData>
            HkArrayRaw          bodyIndexToPalette;  // +0x10 hkArray<int>
            std::int32_t        numRigidBodies;      // +0x20 BodyData begins
            std::int32_t        _pad24;
            void**              rigidBodies;         // +0x28
            const std::int16_t* parentIndices;       // +0x30
            std::int32_t*       controlDataIndices;  // +0x38
            const float*        boneWeights;         // +0x40 ← the channel
        };
        static_assert(offsetof(HkaRbCtlMirror, boneWeights) == 0x40, "boneWeights must sit at +0x40");

        // Parked by ReDrive (outer hook), consumed by the inner-drive seam, disarmed after the
        // chained call either way. Same TLS per-drive pattern as PoseConformPrepare.
        struct ReDriveTls {
            bool          armed   = false;
            bool          applied = false;
            std::uint32_t id      = 0;
            int           n       = 0;
            float         w[64];
            const float*  saved   = nullptr;   // previous m_boneWeights, restored verbatim
        };
        thread_local ReDriveTls s_rdW;

        void ReDrive(RE::Actor* actor, RE::hkbRagdollDriver* driver, void* generatorOutputRaw)
        {
            s_rdW.armed = false;               // never inherit a stale arm from a previous drive
            {
                static int s_lastEnabled = -1;
                const int en = ObjectHold::ReDriveEnabled() ? 1 : 0;
                if (en != s_lastEnabled) {
                    s_lastEnabled = en;
                    logger::info("REDRIVE knob -> {} (reDrive {})", en ? "ARMED" : "off", en);
                }
                if (!en) return;
            }
            if (!actor || !driver) return;
            const std::uint32_t id = actor->GetFormID();

            if (actor->IsInRagdollState()) {
                if (RdSay(id, kRdRagdoll))
                    logger::info("REDRIVE {:08X} SKIP: actor is in true ragdoll state "
                                 "(free physics - the drive is not in charge here)", id);
                return;
            }
            auto* root = actor->Get3D();
            auto* rag  = driver->ragdoll;
            if (!root || !rag) {
                if (RdSay(id, kRdNo3D))
                    logger::info("REDRIVE {:08X} SKIP: root3D={} ragdoll={}",
                                 id, root ? "ok" : "NULL", rag ? "ok" : "NULL");
                return;
            }

            // Bone index -> XP32 node name, via the conform cache. ★ 2026-09-02: the PPB-skeleton
            // gate is stamped HERE too (ConfStampOurs) — this path used to rebuild without it and
            // acted on males and unmapped races while its comment claimed "gated there".
            auto& c = s_confCache[id];
            if (c.root != root || c.ragdoll != rag || c.n == 0) {
                ConfRebuild(c, driver, root);
                if (c.n == 0) {
                    if (RdSay(id, kRdNoBones))
                        logger::info("REDRIVE {:08X} SKIP: conform cache built 0 bones "
                                     "(not a PPB skeleton, or the bone map did not resolve)", id);
                    return;
                }
                ConfStampOurs(c, actor, id);
            }
            if (!c.oursPPB) {
                if (RdSay(id, kRdNotPPB))
                    logger::info("REDRIVE {:08X} SKIP: not on a PPB skeleton (male/unmapped race) - "
                                 "per-bone drive authority is for baked bodies only", id);
                return;
            }

            // ★ THE HIGGS-GRAB GATE (Report 18 §5, doc 25 §7.5 — finally built). A HIGGS grab
            // bolts a motorized constraint onto the grabbed body; modulating the same chain's
            // drive at the same time is two controllers fighting one body, and the recorded
            // worst case is a solver NaN → engine freeze. While HIGGS holds this actor, the
            // weights stand down entirely. Same probe set the divergence probe's player-cause
            // exclusion uses.
            {
                PpbApi::ProbeView pv[16];
                const int np = PpbApi::CopyProbes(pv, 16);
                for (int i = 0; i < np; ++i) {
                    if (pv[i].grabActorId == id) {
                        if (RdSay(id, kRdGrabbed))
                            logger::info("REDRIVE {:08X} STAND-DOWN: HIGGS is holding this actor "
                                         "- weights disabled while the grab lasts (Report 18 §5)", id);
                        s_rdWhy[id] &= ~kRdApplied;   // re-announce when weights come back
                        return;
                    }
                }
            }

            // Fill the weight block from the knobs. Coarse sanity clamp only — the EXACT
            // hierarchy-extrapolation bound needs PLANCK's live hierarchyGain, which does not
            // exist yet at this point in the frame; ReDriveInnerApply clamps against it.
            const int nb = c.n < 64 ? c.n : 64;
            int off = 0;
            for (int i = 0; i < nb; ++i) {
                float m = c.name[i] ? ObjectHold::ReDriveFor(c.name[i]) : 1.f;
                if (m < 0.f)  m = 0.f;
                if (m > 4.f)  m = 4.f;
                s_rdW.w[i] = m;
                if (std::fabs(m - 1.0f) > 0.0005f) ++off;
            }
            {
                static std::unordered_map<std::uint32_t, int> s_lastOff;
                if (s_lastOff.size() > 512) s_lastOff.clear();
                auto it = s_lastOff.find(id);
                if (it == s_lastOff.end() || it->second != off) {
                    s_lastOff[id] = off;
                    logger::info("REDRIVE {:08X} weights computed: {} of {} bones off 1.0{}",
                                 id, off, nb,
                                 off == 0 ? " -- all knobs are 1.0, boneWeights stays NULL" : "");
                }
            }
            if (off == 0) return;                       // nothing to say: leave the engine alone

            s_rdW.n = nb; s_rdW.id = id; s_rdW.armed = true;
        }


        // ══════════════════════════════════════════════════════════════════════════════════
        //  DIVERGENCE PROBE (2026-08-25) — MEASURE ONLY. Writes nothing to the game.
        //
        //  Doc 25 section 7.7 step 1: before building any authority dial, confirm the cases show
        //  what we think they show. It answers exactly one question, per bone:
        //      "is this body sitting off its animated target, and is that gap PERSISTING?"
        //
        //  WHAT IS COMPARED. At the PRE-drive seam the ragdoll bodies still hold LAST frame's
        //  solved result while the nodes carry the pose the drive is chasing. So
        //      gap = |live body world - animated bone world|
        //  is "the error the drive is about to try to close". A gap that decays is the servo
        //  working. A gap that HOLDS across ticks is the drive losing to something else.
        //
        //  WHY NO NEW HOOK. PLANCK already Write5Calls hkaKeyFrameHierarchyUtility::
        //  CalculateApplyKeyframeData (VR 0xBA3B36); a second hook on one site is an ordering
        //  hazard this project has already been bitten by. Everything needed is in hand here.
        //
        //  COST. Reuses the conform cache (bone->node, already built and PPB-skeleton gated) and
        //  the same QPC accumulators, so the marginal work is one distance per bone per throttled
        //  tick. An idle actor exits on the first knob read.
        // TWO TIMESCALES, and the reason is doc 25 trap 3: a body's transform is NOT its bone's
        // transform — there is a constant per-bone offset (the rigid-body T local). So the raw gap
        // never settles at zero, it settles at a per-bone BASELINE, and a fixed threshold would
        // read that baseline as permanent divergence on every bone of every actor forever.
        // Fix: a SLOW ema learns the baseline (what "normal" looks like for this bone on this
        // actor), a FAST ema tracks now, and the detector keys on the DIFFERENCE. That cancels the
        // constant offset by construction and needs no T-transform maths, no calibration, and no
        // per-skeleton table. It also survives ReScale/ReShape changing the offset.
        struct DivBone {
            float fast     = -1.f;   // short-window gap, GAME units
            float slow     = -1.f;   // long-window baseline for THIS bone on THIS actor
            int   warm     = 0;      // ticks observed (baseline is not trustworthy until warm)
            int   holdN    = 0;      // consecutive throttled ticks with fast-slow over threshold
            int   peakHold = 0;      // worst run seen (kept for the summary)
        };
        struct DivActor {
            DivBone       b[kConfMaxBones];
            std::uint64_t lastMs = 0;
            int           tick   = 0;
        };
        constexpr float kDivSlowAlpha = 0.01f;   // baseline window ~100 ticks
        constexpr int   kDivWarmTicks = 24;      // do not judge a bone until its baseline settled
        std::unordered_map<std::uint32_t, DivActor> s_divCache;

        // Could the PLAYER be the cause? A detector that cannot answer this fights the player's own
        // hands: push an arm, the gap spikes, and it reads "the animation is losing". Two cheap
        // tests, both from data PPB already maintains every tick.
        static bool DivPlayerNear(std::uint32_t actorId, const float boneW[3])
        {
            PpbApi::ProbeView pv[16];
            const int n  = PpbApi::CopyProbes(pv, 16);
            const float r  = ObjectHold::DivProbePlayerU();
            const float r2 = r * r;
            for (int i = 0; i < n; ++i) {
                if (pv[i].grabActorId == actorId) return true;      // HIGGS holds her: always ours
                const float* pts[2] = { pv[i].p, pv[i].q };
                for (int e = 0; e < (pv[i].seg ? 2 : 1); ++e) {
                    const float dx = pts[e][0] - boneW[0], dy = pts[e][1] - boneW[1],
                                dz = pts[e][2] - boneW[2];
                    if (dx * dx + dy * dy + dz * dz < r2) return true;
                }
            }
            return false;
        }

        // ── RAGFRAME DRIVE READ (2026-09-03) — measure-only, Ragdoll Research Module 05 §2.
        // Runs INSIDE ApplyToPoseTrack, i.e. one call AHEAD of PLANCK on the same
        // hkbGeneratorOutput, which is the entire point: PLANCK's "bugged out while getting up"
        // watchdog — the thing that actually issues the engine knockdown ~13 frames after PPB
        // asks for one — carries no timestamp in its own log, so the only way to TIME it is to
        // evaluate its exact predicate ourselves, one call earlier, every frame.
        //   PLANCK's predicate, read verbatim from activeragdoll main.cpp:5171-5192:
        //     isPoweredOnly = !isRigidBodyOn && isPoweredOn
        //     allNoForce    = every powered element's m_maxForce <= 0   (vacuously true if none)
        //     usingRootBone = powered world-from-model mode == USE_ROOT_BONE (4)
        //     fire if isPoweredOnly && allNoForce && usingRootBone && !IsInRagdollState()
        // Track ids and the two data layouts are READ from the vendored havok_behavior.h
        // (tools/_research/ar_havok_behavior.h) — NOT recalled. STRIDE comes from the track
        // header's own elementSizeBytes, never sizeof(): that is the ReDrive lesson (three
        // wrong struct guesses, one of them locked in by a self-written static_assert).
        void RagFrameDrive(RE::Actor* actor, RE::hkbRagdollDriver* driver, void* generatorOutputRaw)
        {
            if (!ObjectHold::RagFrameOn() || !actor || !driver || !generatorOutputRaw) return;
            auto* gen = reinterpret_cast<Hkb::GeneratorOutput*>(generatorOutputRaw);
            auto* rbH = Hkb::GetHeader(gen, Hkb::TRACK_RIGID_BODY_RAGDOLL_CONTROLS);
            auto* pwH = Hkb::GetHeader(gen, Hkb::TRACK_POWERED_RAGDOLL_CONTROLS);
            auto* wmH = Hkb::GetHeader(gen, Hkb::TRACK_POWERED_RAGDOLL_WFM_MODE);
            const bool  rbOn   = rbH && rbH->onFraction > 0.f;
            const bool  pwOn   = pwH && pwH->onFraction > 0.f;
            const float rbFrac = rbH ? rbH->onFraction : -1.f;
            // the largest maxForce over the powered elements: PLANCK's allNoForce is
            // "no element above zero", so the max is the number that decides it.
            float pwMax = -1.f;
            if (pwH && pwH->numData > 0 && pwH->elementSizeBytes > 0) {
                auto* base = reinterpret_cast<std::uint8_t*>(Hkb::GetData(gen, pwH));
                pwMax = 0.f;
                for (int i = 0; i < pwH->numData; ++i) {
                    // hkbPoweredRagdollControlData: m_maxForce is the FIRST field (offset 0).
                    const float f = *reinterpret_cast<const float*>(base + (std::size_t)i * pwH->elementSizeBytes);
                    if (f > pwMax) pwMax = f;
                }
            }
            int wfmMode = -1;
            if (wmH && wmH->onFraction > 0.f && wmH->numData > 0) {
                // hkbWorldFromModelModeData: int16 poseMatchingBone0/1/2 then a UInt8 mode @ 0x06
                auto* d = reinterpret_cast<std::uint8_t*>(Hkb::GetData(gen, wmH));
                wfmMode = (int)d[6];
            }
            const bool ragFlag = actor->IsInRagdollState();
            const bool allNoForce = !(pwMax > 0.f);
            const bool planckBug  = (!rbOn && pwOn) && allNoForce && wfmMode == 4 && !ragFlag;
            // ── Q5 (06 section 3): does the engine hard-keyframe the eight lower bones under
            // PLANCK? If it does, the fading KeyframeLowerBody track hands pelvis and legs
            // dx/dt velocities toward a sweeping pose and the dynamic switch PRESERVES them —
            // a harder H10 on exactly the bones a knee-collapse needs. Two driver bytes plus
            // the per-body motion types in the BODIES line answer it together.
            //   reportingWhenKeyframed is hkArray<int32_t> at +0x58 (CommonLibVR declares it);
            //   its first word is the per-bone keyframe-reporting bitfield.
            std::uint32_t rwk = 0;
            if (!driver->reportingWhenKeyframed.empty())
                rwk = static_cast<std::uint32_t>(driver->reportingWhenKeyframed[0]);
            // the KEYFRAMED_RAGDOLL_BONES track: on = the graph is asking for bones to be
            // hard-keyframed at all (vanilla Root runs KeyframeLowerBody over bones 0 3 1 5 7 2 4 8)
            auto* kfH = Hkb::GetHeader(gen, Hkb::TRACK_KEYFRAMED_RAGDOLL_BONES);
            const bool allKf = kfH && kfH->onFraction > 0.f;
            RagFrame::NoteDrive(actor->GetFormID(), rbOn, rbFrac, pwOn, pwMax, wfmMode,
                                ragFlag, planckBug, rwk, allKf);
        }

        // ★★ FURNITURE PROBE (2026-09-12, READ-ONLY — report 39 §3's prerequisite; no furniture gate exists yet).
        // The first kiss VR session (17:26-17:37) showed the push SENSOR blind — "origin captured ... 0 of 8 joints
        // read" on every contact — both while Carmella SAT in a chair and while she CLIMBED STAIRS on her way to
        // furniture. The sensor arms only while ObjectHold::ActorRagdollAttached() is true (PoseConformPrepare), and
        // that is false for sit state != Normal OR an occupied-furniture handle OR knock/ragdoll/swim/killmove/AI-off;
        // PushStep's SEATED label printed only the union, so the log could not say WHICH signal was on while she
        // walked. This line prints every ingredient SEPARATELY:
        //   * on any change of the tuple below (at most one line per 0.25 s per actor), and
        //   * a 1 s heartbeat while furniture is involved (sit state not Normal, or a furniture handle), so the
        //     distance to the nearest seat marker can be watched shrinking during the approach.
        // IsMoving flickered between stair steps (M1/M0/M1 at 17:27:13-16): PRINTED, never a trigger (the v13a
        // lesson — a per-frame flicker in a change key buries every receipt that matters).
        // Main thread (the pre-drive seam, same as PoseConformPrepare's node reads). Reads only. Knob furnProbe.
        struct FurnProbeState {
            int  sit = -2, knock = -2;
            bool rag = false, ai = false, kill = false, attached = false, sensable = false;
            std::uint8_t verdict = 0xFF;
            std::uint32_t furn = 0, pkg = 0;
            int  nearIdx = -2;
            float lastLogS = -1.0e9f;
        };
        std::unordered_map<std::uint32_t, FurnProbeState> s_furnProbe;

        const char* FurnSitName(int s)
        {
            static const char* const k[] = { "Normal", "WantToSit", "WaitingForSitAnim", "IsSitting", "WantToStand",
                                             "WantToSleep", "WaitingForSleepAnim", "IsSleeping", "WantToWake" };
            return (s >= 0 && s < 9) ? k[s] : "?";
        }
        const char* FurnAnimName(std::uint16_t t)   // BSFurnitureMarker::AnimationType bits: sit 1, sleep 2, lean 4
        {
            static const char* const k[] = { "none", "sit", "sleep", "sit+sleep", "lean", "sit+lean", "sleep+lean", "sit+sleep+lean" };
            return k[t & 7];
        }

        // ── ★★ THE FURNITURE VERDICT (2026-09-12, user ruling: the six-state fix + the lean exception) ─────────────────
        // One answer to "is her BODY in furniture?" shared by the three push consumers: the push SENSOR arming
        // (PoseConformPrepare), the feet-lift seat guard and PushReactionsBlocked (both PushStep.cpp). Computed HERE on
        // the main thread every drive (FurnitureTick, called before PoseConformPrepare) because the lean exception walks
        // the furniture's 3D; PushStep reads the cached answer (PushFurnitureVerdict) and never touches 3D itself —
        // report 39 §3: "classify in OnFrame/pre-drive, cache per actor; never in OnMotionDrivenCheck".
        //   FREE — sit state Normal / WantToSit / WantToSleep, WHATEVER furniture handle she holds (the handle is a
        //          reservation: 251u from the seat en route, 932u after standing, 22,230u for a bedroll in another cell).
        //   BUSY — sit state 2 WaitingForSitAnim, 3 IsSitting, 4 WantToStand, 6 WaitingForSleepAnim, 7 IsSleeping,
        //          8 WantToWake — her body is in the interaction.
        //   LEAN — BUSY by state, but the nearest marker of her occupied furniture (within 150u) is lean-only. Pushable.
        // pushStepFurnSitState 0 = the legacy rule exactly (handle OR any non-Normal state = BUSY, no lean).
        struct FurnVerdictEntry { std::uint8_t verdict = kFurnFree; float tS = -1.0e9f; };
        std::unordered_map<std::uint32_t, FurnVerdictEntry> s_furnVerdict;       // written by the tick, read by PushStep
        std::mutex s_furnVerdictMx;
        std::unordered_map<std::uint32_t, std::uint8_t> s_furnVerdictLogged;     // main thread only: last PUSHFURN receipt

        constexpr float kFurnLeanMarkerMaxU = 150.f;   // a lean marker farther than this from her is not the one she uses

        bool FurnSitBusy(int s) { return s == 2 || s == 3 || s == 4 || s == 6 || s == 7 || s == 8; }
        const char* FurnVerdictName(std::uint8_t v)
        {
            return v == kFurnBusy ? "BUSY" : (v == kFurnLean ? "LEAN" : "free");
        }

        struct FurnMarkers {
            std::uint32_t furn = 0; const char* name = ""; float refDist = -1.f;
            int nMarkers = 0; std::uint16_t typeMask = 0, nearType = 0; int nearIdx = -2; float nearDist = -1.f;
            const char* leanKw = nullptr;   // v36: the lean keyword the furniture carries (sit-marker leans), or null
        };

        // ★ v36 THE SIT-MARKER LEANS (user ruling 2026-09-12, "all lean"). The census (39_research/furniture_census.txt) found
        // counter / bar-counter / lean-table / soldier-wall leans carry SIT markers, so the marker read cannot see them; they
        // are named by keyword. Every row verified in the live load order with houseCARL on 2026-09-12, including which FURN
        // records carry each (CounterLeanMarker + CounterBarLeanMarker; LeanTableMarker + DBM TCC_LeanTableMarker;
        // SoldierWallIdle + SoldierWallIdleSandbox; ZaZ zpfWallLeanMarker — a sit marker despite its name — and zpfRailLeanMarker).
        // Resolved once through the data handler (load-order independent); an absent plugin (no ZaZ) leaves its slot null.
        struct LeanKeyword { std::uint32_t localId; const char* plugin; const char* name; RE::BGSKeyword* kw; };
        LeanKeyword s_leanKw[] = {
            { 0x088106, "Skyrim.esm",           "FurnitureCounterLeanMarker", nullptr },
            { 0x0F5078, "Skyrim.esm",           "isBarCounter",               nullptr },
            { 0x0C4EF2, "Skyrim.esm",           "isLeanTable",                nullptr },
            { 0x0BFB07, "Skyrim.esm",           "isIdleSoldierWall",          nullptr },
            { 0x061EF5, "ZaZAnimationPack.esm", "zpfFurnitureWallLean",       nullptr },
            { 0x061F04, "ZaZAnimationPack.esm", "zpfFurnitureRailLean",       nullptr },
        };
        bool s_leanKwResolved = false;   // main thread only

        // MAIN THREAD ONLY. Returns the matching keyword's EditorID, or null.
        const char* FurnLeanKeyword(RE::TESObjectREFR* furn)
        {
            if (!s_leanKwResolved) {
                auto* dh = RE::TESDataHandler::GetSingleton();
                if (!dh) return nullptr;   // not ready yet: try again next time, do not latch a failure
                int n = 0;
                for (auto& k : s_leanKw) {
                    auto* form = dh->LookupForm(k.localId, k.plugin);
                    k.kw = form ? form->As<RE::BGSKeyword>() : nullptr;
                    if (k.kw) ++n;
                }
                s_leanKwResolved = true;
                logger::info("PUSHFURN lean keywords resolved: {} of {} (a missing one = its plugin is not loaded; "
                             "wall/rail LEAN markers are read from the furniture 3D regardless)", n, std::size(s_leanKw));
            }
            auto* base = furn ? furn->GetBaseObject() : nullptr;
            auto* kwf  = base ? base->As<RE::BGSKeywordForm>() : nullptr;
            if (!kwf) return nullptr;
            for (const auto& k : s_leanKw)
                if (k.kw && kwf->HasKeyword(k.kw)) return k.name;
            return nullptr;
        }
        // MAIN THREAD ONLY: resolves the occupied-furniture handle and walks its 3D markers.
        void FurnReadMarkers(RE::Actor* actor, const RE::NiPoint3& pos, FurnMarkers& m)
        {
            auto furn = actor->GetOccupiedFurniture().get();
            if (!furn) return;
            m.furn = furn->GetFormID();
            if (const char* nm = furn->GetName(); nm) m.name = nm;
            m.refDist = pos.GetDistance(furn->GetPosition());
            m.leanKw  = FurnLeanKeyword(furn.get());   // v36: record data — needs no 3D
            auto* froot = furn->Get3D();
            if (!froot) return;   // reserved furniture in an unloaded cell (the 22,230u bedroll): no markers to read
            // Same lookup Standpoint-plugin SlotSolver.cpp uses in VR (world = root.world * marker.offset).
            auto* fmn = RE::BSFurnitureMarkerNode::FindBSFurnitureMarkerNode(froot);
            if (!fmn) return;
            m.nMarkers = static_cast<int>(fmn->markers.size());
            float best = FLT_MAX;
            for (std::uint32_t i = 0; i < fmn->markers.size(); ++i) {
                const auto& mk = fmn->markers[i];
                const std::uint16_t t = mk.animationType.underlying();
                m.typeMask |= t;
                const float d = pos.GetDistance(froot->world * mk.offset);
                if (d < best) { best = d; m.nearIdx = static_cast<int>(i); m.nearType = t; }
            }
            if (best < FLT_MAX) m.nearDist = best;
        }

        // The rule. `m` = the marker read (null when it was not taken: the lean exception then cannot apply).
        std::uint8_t FurnVerdictOf(RE::Actor* actor, int sit, const FurnMarkers* m)
        {
            if (!ObjectHold::PushStepFurnSitState())   // legacy A/B lever: the reservation handle OR any non-Normal state
                return (sit != 0 || static_cast<bool>(actor->GetOccupiedFurniture())) ? kFurnBusy : kFurnFree;
            if (!FurnSitBusy(sit)) return kFurnFree;
            if (ObjectHold::PushStepFurnLean() && m) {
                const bool leanMarker = m->nMarkers > 0 && m->nearDist >= 0.f && m->nearDist <= kFurnLeanMarkerMaxU &&
                                        (m->nearType & 4) && !(m->nearType & 3);   // her own marker is lean-only
                if (leanMarker || m->leanKw) return kFurnLean;                    // v36: or a sit-marker lean by keyword
            }
            return kFurnBusy;
        }

        // ★ The push SENSOR's own "may I read her joints": ActorRagdollAttached minus the furniture reservation. The
        // shared ActorRagdollAttached stays the CALIBRATION gate (ReScale / conform must never measure mid-sit).
        bool PushSensable(RE::Actor* actor, std::uint8_t verdict)
        {
            if (!ObjectHold::PushStepFurnSitState()) return ObjectHold::ActorRagdollAttached(actor);
            if (!actor || actor->IsInRagdollState()) return false;
            if (auto* st = actor->AsActorState()) {
                if (st->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return false;   // mid get-up
                if (st->IsSwimming()) return false;
            }
            if (actor->IsInKillMove()) return false;
            if (!actor->IsAIEnabled()) return false;   // PLANCK 0.8 loosens a DisableAI'd standing NPC (17A-4)
            return verdict != kFurnBusy;
        }

        // Every drive, every driven actor (pre-drive, main thread): classify, cache, receipt on change, then the
        // optional FURNPROBE line. Returns this drive's verdict for the sensor arming.
        std::uint8_t FurnitureTick(RE::Actor* actor)
        {
            if (!actor) return kFurnFree;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const RE::NiPoint3 pos = actor->GetPosition();
            const float playerDist = player ? pos.GetDistance(player->GetPosition()) : FLT_MAX;

            FurnProbeState now{};
            if (auto* st = actor->AsActorState()) {
                now.sit   = static_cast<int>(st->GetSitSleepState());
                now.knock = static_cast<int>(st->GetKnockState());
            }
            const bool probe = ObjectHold::FurnProbeOn() && playerDist <= 1000.f;   // a push needs the player's hand on her
            // The marker walk runs only when it can change the answer (a busy state with the lean exception on) or the
            // probe wants to print it — never for a standing NPC with the probe off.
            const bool readM = probe ||
                               (ObjectHold::PushStepFurnSitState() && ObjectHold::PushStepFurnLean() && FurnSitBusy(now.sit));
            FurnMarkers fm{};
            if (readM) FurnReadMarkers(actor, pos, fm);
            const std::uint8_t verdict = FurnVerdictOf(actor, now.sit, readM ? &fm : nullptr);
            const std::uint32_t id = actor->GetFormID();
            const float nowS = ArmNowSeconds();
            {
                std::scoped_lock lk(s_furnVerdictMx);
                if (s_furnVerdict.size() > 128) s_furnVerdict.clear();
                s_furnVerdict[id] = FurnVerdictEntry{ verdict, nowS };
            }
            // Receipt on every verdict change near the player (a city of NPCs sitting down must not flood the log).
            if (playerDist <= 1500.f && ObjectHold::PushStepEnabled()) {
                if (s_furnVerdictLogged.size() > 128) s_furnVerdictLogged.clear();
                auto it = s_furnVerdictLogged.try_emplace(id, kFurnFree).first;
                if (it->second != verdict) {
                    // v36: what each verdict ALLOWS, spelled out (the lean splits push from the feet path)
                    const char* allows = verdict == kFurnBusy ? "push / shove / feet-lift all STAND DOWN"
                                       : verdict == kFurnLean ? (ObjectHold::PushStepFurnLeanPush()
                                                                     ? "push / shove / feet-lift allowed (pushStepFurnLean 2)"
                                                                     : "push / shove STAND DOWN, feet-lift (leg sweep) ALLOWED")
                                                              : "push / shove / feet-lift allowed";
                    logger::info("PUSHFURN {:08X} {} -> {} | sit={}({}) | furniture {:08X} '{}', nearest marker #{} at {:.1f}u ({}), "
                                 "lean keyword {} | rule {} | {}",
                                 id, FurnVerdictName(it->second), FurnVerdictName(verdict), now.sit, FurnSitName(now.sit),
                                 fm.furn, fm.name, fm.nearIdx, fm.nearDist, FurnAnimName(fm.nearType),
                                 fm.leanKw ? fm.leanKw : "none",
                                 ObjectHold::PushStepFurnSitState() ? (ObjectHold::PushStepFurnLean() ? "six-state + lean" : "six-state")
                                                                    : "LEGACY handle",
                                 allows);
                    it->second = verdict;
                }
            }
            if (!probe) return verdict;

            now.rag      = actor->IsInRagdollState();
            now.ai       = actor->IsAIEnabled();
            now.kill     = actor->IsInKillMove();
            now.attached = ObjectHold::ActorRagdollAttached(actor);   // the CALIBRATION gate (ReScale / conform)
            now.verdict  = verdict;
            now.sensable = PushSensable(actor, verdict);             // what the push sensor now uses
            const bool moving = actor->IsMoving();

            now.furn    = fm.furn;
            now.nearIdx = fm.nearIdx;
            const char* furnName = fm.name;
            const float refDist = fm.refDist, nearDist = fm.nearDist;
            const int nMarkers = fm.nMarkers;
            const std::uint16_t typeMask = fm.typeMask, nearType = fm.nearType;
            // ⚠ MiddleHighProcessData::currentFurnitureMarkerID @0x2E8 is NOT verified on VR (39_research/
            // lean-furniture.txt: the layout is proven only up to 0x250). Printed for cross-checking against the
            // nearest marker, never trusted.
            long long markerId = -1;
            if (auto* proc = actor->GetActorRuntimeData().currentProcess; proc && proc->middleHigh)
                markerId = static_cast<long long>(proc->middleHigh->currentFurnitureMarkerID);
            int pkgType = -1;
            if (auto* pkg = actor->GetCurrentPackage()) {
                now.pkg = pkg->GetFormID();
                pkgType = static_cast<int>(pkg->packData.packType.underlying());
            }

            if (s_furnProbe.size() > 128) s_furnProbe.clear();
            auto& prev = s_furnProbe[id];
            const bool changed = prev.sit != now.sit || prev.knock != now.knock || prev.rag != now.rag ||
                                 prev.ai != now.ai || prev.kill != now.kill || prev.attached != now.attached ||
                                 prev.sensable != now.sensable || prev.verdict != now.verdict ||
                                 prev.furn != now.furn || prev.pkg != now.pkg || prev.nearIdx != now.nearIdx;
            const bool involved = now.sit != 0 || now.furn != 0;
            const float since = nowS - prev.lastLogS;
            if (!(changed ? since >= 0.25f : (involved && since >= 1.0f))) return verdict;   // a throttled change logs next tick

            if (now.furn) {
                logger::info("FURNPROBE {:08X} {} | sit={}({}) knock={} rag={} ai={} kill={} moving={} => attached={} "
                             "| PUSH {} sensor={} | furniture {:08X} '{}' origin {:.0f}u, {} marker(s) [{}], nearest #{} at {:.1f}u ({}), "
                             "lean kw {} | markerID@0x2E8 {} (VR offset unverified) | package {:08X} type {}",
                             id, changed ? "CHANGE" : "beat", now.sit, FurnSitName(now.sit), now.knock,
                             now.rag ? 1 : 0, now.ai ? 1 : 0, now.kill ? 1 : 0, moving ? 1 : 0, now.attached ? 1 : 0,
                             FurnVerdictName(verdict), now.sensable ? 1 : 0,
                             now.furn, furnName, refDist, nMarkers, FurnAnimName(typeMask), now.nearIdx, nearDist,
                             FurnAnimName(nearType), fm.leanKw ? fm.leanKw : "none", markerId, now.pkg, pkgType);
            } else {
                logger::info("FURNPROBE {:08X} {} | sit={}({}) knock={} rag={} ai={} kill={} moving={} => attached={} "
                             "| PUSH {} sensor={} | furniture none | markerID@0x2E8 {} (VR offset unverified) | package {:08X} type {}",
                             id, changed ? "CHANGE" : "beat", now.sit, FurnSitName(now.sit), now.knock,
                             now.rag ? 1 : 0, now.ai ? 1 : 0, now.kill ? 1 : 0, moving ? 1 : 0, now.attached ? 1 : 0,
                             FurnVerdictName(verdict), now.sensable ? 1 : 0, markerId, now.pkg, pkgType);
            }
            now.lastLogS = nowS;
            prev = now;
            return verdict;
        }

        void DivergenceProbe(RE::Actor* actor, RE::hkbRagdollDriver* driver)
        {
            if (!ObjectHold::DivProbeEnabled() || !actor || !driver) return;
            if (actor->IsInRagdollState()) return;          // free physics: the gap is meaningless
            auto* root = actor->Get3D();
            auto* rag  = driver->ragdoll;
            if (!root || !rag) return;

            const std::uint64_t q0 = ConfQpc();
            const std::uint32_t id = actor->GetFormID();

            // Reuse the conform cache; build on demand so the probe works with poseConform OFF.
            auto& c = s_confCache[id];
            if (c.root != root || c.ragdoll != rag || c.n == 0) {
                ConfRebuild(c, driver, root);
                if (c.n == 0) return;
                ConfStampOurs(c, actor, id);
            }
            if (!c.oursPPB) return;   // 2026-09-02: PPB-skeleton gate (was missing — probed males/unmapped races)

            auto& d = s_divCache[id];
            if (++d.tick < (int)(ObjectHold::DivProbeEveryN() + 0.5f)) {
                s_confSumNs.fetch_add((std::uint64_t)((double)(ConfQpc() - q0) * ConfNsPerCount()),
                                      std::memory_order_relaxed);
                return;
            }
            d.tick = 0;

            const float gapU  = ObjectHold::DivProbeGapU();
            const float alpha = ObjectHold::DivProbeAlpha();
            const int   holdN = (int)(ObjectHold::DivProbeHoldN() + 0.5f);
            const auto& b2r   = rag->boneToRigidBodyMap;
            const int   nB    = (int)rag->rigidBodies.size();

            int   worstIdx = -1, nPersist = 0, nPlayer = 0;
            float worstGap = 0.f, worstBase = 0.f;

            for (int i = 0; i < c.n; ++i) {
                if (!c.resolvable[i] || !c.node[i]) continue;
                RE::hkpRigidBody* rb = nullptr;
                if ((int)b2r.size() == c.n) {
                    const int ri = b2r[i];
                    if (ri >= 0 && ri < nB) rb = rag->rigidBodies[ri];
                } else if (i < nB) {
                    rb = rag->rigidBodies[i];
                }
                if (!rb) continue;

                // live body world (HAVOK m) -> GAME u, against the animated bone the drive chases
                const auto& T = rb->motion.motionState.transform;
                alignas(16) float p4[4];
                _mm_store_ps(p4, T.translation.quad);
                const float toGame = 1.f / kConfHavokScale;
                const float bw[3] = { p4[0] * toGame, p4[1] * toGame, p4[2] * toGame };
                const RE::NiPoint3& nw = c.node[i]->world.translate;
                const float dx = bw[0] - nw.x, dy = bw[1] - nw.y, dz = bw[2] - nw.z;
                const float gap = std::sqrt(dx * dx + dy * dy + dz * dz);

                DivBone& db = d.b[i];
                if (db.fast < 0.f) { db.fast = db.slow = gap; }
                else {
                    db.fast += alpha * (gap - db.fast);
                    db.slow += kDivSlowAlpha * (gap - db.slow);   // learns this bone's baseline
                }
                if (db.warm < kDivWarmTicks) { ++db.warm; db.holdN = 0; continue; }

                const float excess = db.fast - db.slow;   // how far ABOVE its own normal
                if (excess > gapU) {
                    if (DivPlayerNear(id, bw)) { db.holdN = 0; ++nPlayer; continue; }
                    ++db.holdN;
                    if (db.holdN > db.peakHold) db.peakHold = db.holdN;
                    if (db.holdN >= holdN) {
                        ++nPersist;
                        if (excess > worstGap) { worstGap = excess; worstIdx = i; worstBase = db.slow; }
                    }
                } else {
                    db.holdN = 0;
                }
            }

            // ONE line per actor per report interval, never per bone per frame (the log-volume
            // lesson from the raw-stream work). Silence is a real answer here: no persistent gap.
            if (nPersist > 0) {
                const std::uint64_t now = (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch()).count();
                if (now - d.lastMs > (std::uint64_t)(ObjectHold::DivProbeReportS() * 1000.f)) {
                    d.lastMs = now;
                    logger::info("DIVPROBE {:08X} {} bone(s) PERSISTENTLY off target (excess>{:.2f}u for >={} ticks)"
                                 " | worst {} +{:.2f}u over its {:.2f}u baseline | {} excluded as player-caused",
                                 id, nPersist, gapU, holdN,
                                 (worstIdx >= 0 && c.name[worstIdx]) ? c.name[worstIdx] : "?",
                                 worstGap, worstBase, nPlayer);
                }
            }
            s_confSumNs.fetch_add((std::uint64_t)((double)(ConfQpc() - q0) * ConfNsPerCount()),
                                  std::memory_order_relaxed);
            s_confFires.fetch_add(1, std::memory_order_relaxed);
        }

        // Called from ApplyToPoseTrack for every driven non-player actor; cheap no-op when both knobs
        // are off. QPC-timed; steady-state cost accumulates into the perf atomics + the 60 s summary.
        // ── DRIVE VELOCITY DAMPING (knob driveDamping) ──────────────────────────────────
        // Writes ControlData::m_velocityDamping on every ragdoll body of this actor. Havok's own
        // header: "This gain dampens the velocities of the bodies. The current velocity of the body
        // is scaled by this parameter on every frame before the controller is applied." SDK default
        // 0.0 — and NOTHING in this stack writes it.
        // ORDERING: PPB is the CHAIN HEAD (0xB266AB), PLANCK is downstream, and PLANCK re-stamps ONLY
        // hierarchyGain / velocityGain / positionGain each frame — never damping — so this survives.
        // STRIDE: hdr->elementSizeBytes at runtime, NEVER sizeof(). That is the ReDrive lesson.
        // PLAUSIBILITY GATE: element 0 must look like a ControlData before we write a byte.
        static void ApplyDriveDamping(RE::Actor* actor, void* generatorOutputRaw)
        {
            const float want = ObjectHold::DriveDamping();
            if (want < -0.5f || !generatorOutputRaw) return;
            auto* gen = reinterpret_cast<Hkb::GeneratorOutput*>(generatorOutputRaw);
            auto* hdr = Hkb::GetHeader(gen, Hkb::TRACK_RIGID_BODY_RAGDOLL_CONTROLS);
            if (!hdr || hdr->numData <= 0 || hdr->elementSizeBytes <= 0 || hdr->onFraction <= 0.f) return;
            auto* base = reinterpret_cast<std::uint8_t*>(Hkb::GetData(gen, hdr));
            if (!base) return;
            auto* e0 = reinterpret_cast<const Hkb::ControlData*>(base);
            if (!(e0->hierarchyGain >= 0.f && e0->hierarchyGain <= 1.f &&
                  e0->velocityGain  >= 0.f && e0->velocityGain  <= 2.f)) {
                static bool s_warned = false;
                if (!s_warned) {
                    s_warned = true;
                    logger::warn("DRIVEDAMP: refusing to write — element 0 is not a ControlData "
                                 "(hier={:.3f} vel={:.3f} stride={}). Layout moved.",
                                 e0->hierarchyGain, e0->velocityGain, (int)hdr->elementSizeBytes);
                }
                return;
            }
            const float clamped = want < 0.f ? 0.f : (want > 1.f ? 1.f : want);
            for (int i = 0; i < hdr->numData; ++i)
                reinterpret_cast<Hkb::ControlData*>(base + (std::size_t)i * hdr->elementSizeBytes)
                    ->velocityDamping = clamped;
            static float s_lastDamp = -999.f;
            if (clamped != s_lastDamp) {
                s_lastDamp = clamped;
                logger::info("DRIVEDAMP {:08X}: velocityDamping -> {:.3f} on {} bodies (read back {:.3f}). "
                             "SDK default 0.0; nothing else in the stack writes it. This is the term "
                             "Havok provides to kill a per-frame overshoot/correct limit cycle.",
                             actor ? actor->GetFormID() : 0u, clamped, (int)hdr->numData,
                             reinterpret_cast<const Hkb::ControlData*>(base)->velocityDamping);
            }
        }

        void PoseConformPrepare(RE::Actor* actor, RE::hkbRagdollDriver* driver, void* generatorOutputRaw)
        {
            ApplyDriveDamping(actor, generatorOutputRaw);   // knob-gated; instant return at -1
            const bool on      = ObjectHold::PoseConformEnabled();
            const bool dumpArm = ConfDumpTick();
            // v9.0: the sensor prep also arms for the DIAGNOSTIC probe alone, so the body-jitter
            // measurement stays available while pushStep is switched off for a bisection.
            const bool sense   = ObjectHold::PushStepEnabled() ||
                                 ObjectHold::PushStepIdleProbe() != 0.f;
            if (!on && !dumpArm && !sense) return;
            // A knocked-down NPC's bodies are free physics (authoritative) — same gate as the heel fix.
            if (actor->IsInRagdollState()) return;
            auto* root = actor->Get3D();
            if (!root || !driver->ragdoll) return;

            const std::uint64_t q0 = ConfQpc();
            const std::uint32_t id = actor->GetFormID();
            auto& c = s_confCache[id];
            bool rebuilt = false;
            if (c.root != root || c.ragdoll != driver->ragdoll || c.n == 0) {
                ConfRebuild(c, driver, root);
                rebuilt = true;
                if (c.n == 0) return;   // unbuildable this fire (no skeleton/bodies yet) — retry next drive
                // PPB-skeleton gate: recompute on every rebuild (race/skeleton can change on a
                // 3D reload). Shared with ReDrive / DivergenceProbe since 2026-09-02.
                ConfStampOurs(c, actor, id);
            }
            // ★ 2026-09-02: stamp the push-sense bones whenever THIS build has not been stamped —
            // not only when prep itself rebuilt. ReDrive / DivergenceProbe can rebuild the cache
            // first (ConfRebuild resets senseStamped), and prep used to skip this block in that
            // case, leaving senseBone[] unresolved until the next 3D/ragdoll rebuild.
            if (!c.senseStamped) {
                c.senseStamped = true;
                if (!rebuilt) ConfStampOurs(c, actor, id);   // a foreign rebuild: re-evaluate the gate too
                // PUSH SENSE v5: resolve the sensed trunk bones + drop this actor's stale
                // calibration (a ragdoll rebuild can move the body-vs-joint offset).
                for (int s5 = 0; s5 < kSenseN; ++s5) {
                    c.senseBone[s5] = -1;
                    c.senseChild[s5] = -1;
                    for (int i = 0; i < c.n; ++i) {
                        if (c.name[i] && std::strcmp(c.name[i], kSenseName[s5]) == 0)
                            c.senseBone[s5] = (std::int8_t)i;
                        if (c.name[i] && std::strcmp(c.name[i], kSenseChild[s5]) == 0)
                            c.senseChild[s5] = (std::int8_t)i;
                    }
                    const std::uint64_t key = (std::uint64_t(id) << 8) | (unsigned)kSenseSlot[s5];
                    s_senseCal.erase(key);
                    std::scoped_lock lkg(s_senseGapMx);
                    s_senseGap.erase(key);
                }
                // ★ v29: the two FOOT bones for the trip channel (names from kConfNode18, trailing space load-bearing).
                c.footBone[0] = c.footBone[1] = -1;
                for (int i = 0; i < c.n; ++i) {
                    if (!c.name[i]) continue;
                    if (std::strcmp(c.name[i], "NPC R Foot [Rft ]") == 0) c.footBone[0] = (std::int8_t)i;
                    if (std::strcmp(c.name[i], "NPC L Foot [Lft ]") == 0) c.footBone[1] = (std::int8_t)i;
                }
                {
                    std::scoped_lock lkf(s_senseGapMx);
                    s_footGap.erase((std::uint64_t(id) << 1) | 0u);
                    s_footGap.erase((std::uint64_t(id) << 1) | 1u);
                }
            }
            if (!c.oursPPB) return;   // the gate: males + unmapped races are structurally unreachable

            // FURNITURE GATE (2026-07-13): never capture/trim while the ragdoll is
            // PLANCK-loosened (sit/lean/sleep/ragdoll — the "green" state); the stand-up
            // edge resets BOTH the root capture and the head trim for a clean re-measure.
            const bool attached = ObjectHold::ActorRagdollAttached(actor);
            // ★★ 2026-09-12 THE PUSH SENSOR'S OWN GATE (six-state rule + lean). `attached` above stays the CALIBRATION gate
            // for conform / head trim; the push sensor no longer borrows it, because a furniture RESERVATION (a handle 251u
            // from the seat, 932u after standing, 22,230u in another cell) made it false on a walking or standing NPC and
            // blinded the sensor ("0 of 8 joints read"). Counted BEFORE the stand-up return below; the same 10-frame
            // debounce then applies to the push-sensable edge (a stand-up re-attaches the ragdoll on those frames).
            const bool pushOk = PushSensable(actor, PushFurnitureVerdict(actor));   // FurnitureTick ran earlier this drive
            if (!pushOk) c.pushFrames = 0; else if (c.pushFrames < 1000000) ++c.pushFrames;
            if (!attached) { c.wasAttached = false; c.attachedFrames = 0; }
            else {
                if (c.attachedFrames < 1000000) ++c.attachedFrames;
                if (!c.wasAttached) {
                    c.wasAttached = true;
                    c.rootDeltaValid = false;
                    c.headTrimN = 0; c.headTrimDone = false;
                    logger::info("PCONF {:08X} stood up -> re-capturing root delta + head trim (furniture gate)", id);
                }
                // CTD HARDENING (2026-07-18 crash: engine AV one tick after 00013485's PCONF+CapFix
                // stand-up fired same-frame): the ragdoll is mid-re-attach on the stand-up frames —
                // skip conform for ~10 frames (~110 ms at 90 Hz) before touching her nodes again.
                if (c.attachedFrames < 10) return;
            }
            // HEEL-STATE EDGE (2026-07-13, the Carmella half-heel): the NiOverride heel
            // lift can arm late, fail ("stuck heels"), or change with shoes — any
            // calibration latched under one heel state is wrong under another. Read the
            // offset from its source node (order-independent of the heel-fix prep) and
            // treat a material change exactly like the stand-up edge: full re-measure.
            {
                float heelNow = 0.f;   // the offset IS the heel state (2026-08-29) — and it is
                                       // steadier than the spell: during a Heels Fix refresh the
                                       // SPELL flickers while the offset persists, so reading it
                                       // directly cannot fire the edge on a flicker.
                if (auto* npcNode = root->GetObjectByName("NPC")) {
                    const float z = npcNode->local.translate.z;
                    if (z > 0.01f && z <= 40.f) heelNow = z;
                }
                const float biasNow = GetHeelDriveBias();               // hf toggle / arming edge
                bool edge = false;
                if (c.lastHeelZ < -900.f) c.lastHeelZ = heelNow;          // first sight: baseline only
                else if (std::fabs(heelNow - c.lastHeelZ) > 0.25f) { c.lastHeelZ = heelNow; edge = true; }
                if (c.lastHeelBias < -900.f) c.lastHeelBias = biasNow;
                else if (std::fabs(biasNow - c.lastHeelBias) > 0.25f) { c.lastHeelBias = biasNow; edge = true; }
                if (edge) {
                    c.rootDeltaValid = false;
                    c.headTrimN = 0; c.headTrimDone = false;
                    logger::info("PCONF {:08X} heel state changed (offset {:+.1f}u bias {:+.1f}u) -> re-capturing root delta + head trim",
                                 id, heelNow, biasNow);
                }
            }
            // EveryN stride: recompute the chain compositions this fire, or reuse the cached
            // translations. NOTE: reused deltas lag the pose by up to N-1 frames — under the PD
            // drive's own servo lag that is invisible at N=2-4, which is why EveryN is the cheap
            // mitigation lever rather than a quality knob.
            const int  N         = ObjectHold::PoseConformEveryN();
            const bool recompute = rebuilt || !c.haveDeltas || (c.phase % (std::uint32_t)N) == 0 || dumpArm;
            ++c.phase;
            if (recompute) {
                for (int i = 0; i < c.n; ++i) {
                    if (!c.node[i] || !c.resolvable[i]) continue;
                    RE::NiPoint3 p;
                    if (ConfComposeChain(c.node[i], c.parentNode[i], p)) {
                        c.t[i][0] = p.x; c.t[i][1] = p.y; c.t[i][2] = p.z;
                    } else {
                        c.resolvable[i] = false;   // tree changed shape under us — degrade this bone only
                    }
                }
                c.haveDeltas = true;
            }

            // Actor scale: read the WORLD_FROM_MODEL track's hkQsTransform scale — the exact value the
            // engine's CopyAndApplyScaleToPose used on the low-res pose. NOTE: the inner hook's
            // worldFromModel PARAM may already carry scale 1 (PLANCK's reimplementation suggests the
            // engine flattens it before the controller call), so the track is the trustworthy source;
            // ref GetScale() is the fallback.
            float scale = actor->GetScale();
            if (auto* gen = reinterpret_cast<Hkb::GeneratorOutput*>(generatorOutputRaw)) {
                if (auto* hdr = Hkb::GetHeader(gen, Hkb::TRACK_WORLD_FROM_MODEL);
                    hdr && hdr->numData >= 1 && hdr->onFraction > 0.f) {
                    alignas(16) float s4[4];
                    _mm_store_ps(s4, reinterpret_cast<RE::hkQsTransform*>(Hkb::GetData(gen, hdr))->scale.quad);
                    if (s4[0] > 0.01f && s4[0] < 100.f) scale = s4[0];
                }
            }
            if (!(scale > 0.01f && scale < 100.f)) scale = 1.f;

            // ── ARM PROBE fill (main thread; node + body world positions for the RIGHT ARM) ──
        // The four channels answer the question every other instrument has dodged: is the BONE
        // moving, or only its picture? node = what you see, body = what physics holds.
        t_conf.armOn = (ObjectHold::ArmProbe() != 0.f);
        if (t_conf.armOn) {
            static const char* kArm[3] = { "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]" };
            for (int ai = 0; ai < 3; ++ai) {
                t_conf.armBone[ai] = -1; t_conf.armNodeOk[ai] = false; t_conf.armBodyOk[ai] = false;
                for (int i = 0; i < c.n; ++i) {
                    if (!c.name[i] || std::strcmp(c.name[i], kArm[ai]) != 0) continue;
                    t_conf.armBone[ai] = i;
                    if (auto* nd = c.node[i]) {
                        t_conf.armNodeW[ai][0] = nd->world.translate.x;
                        t_conf.armNodeW[ai][1] = nd->world.translate.y;
                        t_conf.armNodeW[ai][2] = nd->world.translate.z;
                        t_conf.armNodeOk[ai] = true;
                        if (auto* colObj = nd->collisionObject.get()) {
                            auto* rb  = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
                            auto* hkb = rb ? rb->GetRigidBody() : nullptr;
                            if (hkb) {
                                const RE::hkVector4& bp = hkb->motion.motionState.transform.translation;
                                alignas(16) float q[4]; _mm_store_ps(q, bp.quad);
                                constexpr float kHavokToSkyrim = 69.9915f;
                                t_conf.armBodyW[ai][0] = q[0] * kHavokToSkyrim;
                                t_conf.armBodyW[ai][1] = q[1] * kHavokToSkyrim;
                                t_conf.armBodyW[ai][2] = q[2] * kHavokToSkyrim;
                                t_conf.armBodyOk[ai] = true;
                            }
                        }
                    }
                    break;
                }
            }
        }
        // ── LEG PROBE fill (main thread; node + body world positions for the LEGS) ──
        // Verbatim the arm fill, four bones instead of three. All four are in kConfNode18, so the
        // conform-map lookup that the arm uses resolves them the same way.
        t_conf.legOn = (ObjectHold::LegProbe() != 0.f);
        if (t_conf.legOn) {
            static const char* kLeg[4] = { "NPC R Thigh [RThg]", "NPC R Calf [RClf]",
                                           "NPC R Foot [Rft ]",  "NPC L Foot [Lft ]" };
            for (int li = 0; li < 4; ++li) {
                t_conf.legBone[li] = -1; t_conf.legNodeOk[li] = false; t_conf.legBodyOk[li] = false;
                for (int i = 0; i < c.n; ++i) {
                    if (!c.name[i] || std::strcmp(c.name[i], kLeg[li]) != 0) continue;
                    t_conf.legBone[li] = i;
                    if (auto* nd = c.node[i]) {
                        t_conf.legNodeW[li][0] = nd->world.translate.x;
                        t_conf.legNodeW[li][1] = nd->world.translate.y;
                        t_conf.legNodeW[li][2] = nd->world.translate.z;
                        t_conf.legNodeOk[li] = true;
                        if (auto* colObj = nd->collisionObject.get()) {
                            auto* rb  = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
                            auto* hkb = rb ? rb->GetRigidBody() : nullptr;
                            if (hkb) {
                                const RE::hkVector4& bp = hkb->motion.motionState.transform.translation;
                                alignas(16) float q[4]; _mm_store_ps(q, bp.quad);
                                constexpr float kHavokToSkyrim = 69.9915f;
                                t_conf.legBodyW[li][0] = q[0] * kHavokToSkyrim;
                                t_conf.legBodyW[li][1] = q[1] * kHavokToSkyrim;
                                t_conf.legBodyW[li][2] = q[2] * kHavokToSkyrim;
                                t_conf.legBodyOk[li] = true;
                            }
                        }
                    }
                    break;
                }
            }
        }
        // Park the drive's conform set for the inner hook.
            t_conf.write  = on;
            t_conf.dump   = ConfDumpClaim(id);
            t_conf.triad  = ObjectHold::PushSenseTriad() != 0.f;   // v11 probe
            // v11: the engine's own reaction state, read here on the main thread so the inner hook stays pure math.
            // GetKnockState covers the Ragdoll->GetUp window where IsInRagdollState already reads FALSE.
            t_conf.knocked = actor->IsInRagdollState() ||
                             actor->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal;
            { const RE::NiPoint3 op = actor->GetPosition(); t_conf.originW[0] = op.x; t_conf.originW[1] = op.y; t_conf.originW[2] = op.z; }
            t_conf.yaw = actor->data.angle.z;   // v12 travel sensor
            t_conf.n      = c.n;
            t_conf.k      = kConfHavokScale * scale;
            t_conf.scale  = scale;
            t_conf.formId = id;
            t_conf.rootMode = 0;
            t_conf.rootCache = nullptr;
            if (ObjectHold::PoseConformRoot() && on) {
                if (c.rootDeltaValid) {
                    t_conf.rootMode = 2;
                    t_conf.rootDelta[0] = c.rootDelta[0];
                    t_conf.rootDelta[1] = c.rootDelta[1];
                    t_conf.rootDelta[2] = c.rootDelta[2];
                } else if (recompute && attached && c.attachedFrames >= 90) {
                    // Capture needs FRESH targets AND an attached ragdoll — held ~1 s
                    // (17A-29 debounce: the first attached frames after a get-up/warp
                    // often still carry displacement the state flags can't see).
                    t_conf.rootMode = 1;
                    t_conf.rootCache = &c;
                }
            }
            // HEAD TRIM sampling (main thread; ~1 Hz via the tick divider)
            if (attached && ObjectHold::PoseConformRoot() && c.rootDeltaValid && !c.headTrimDone) {
                if (++c.headTrimTick % 90 == 0) {
                    float gz = 0.f;
                    if (ObjectHold::PivHeadGapZ(actor, gz) && std::fabs(gz) < 10.f) {
                        c.headTrimSamples[c.headTrimN] = gz;
                        if (++c.headTrimN >= 10) {
                            // MEDIAN-of-10 (17A-26/27): one warped sample cannot move the trim.
                            float srt[10];
                            std::memcpy(srt, c.headTrimSamples, sizeof srt);
                            std::sort(srt, srt + 10);
                            const float med = 0.5f * (srt[4] + srt[5]);
                            c.headTrimDone = true;
                            if (std::fabs(med) > 0.5f) {             // the user's 0.5u buffer
                                c.rootDelta[2] -= med;               // pivot above node -> lower the root
                                logger::info("PCONF HEADTRIM {:08X}: head gapZ median {:+.2f}u -> rootDelta.z {:+.2f}u",
                                             id, med, c.rootDelta[2]);
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < c.n; ++i) {
                t_conf.flags[i]      = c.isRoot[i] ? 1 : ((c.node[i] && c.resolvable[i]) ? 0 : 2);
                t_conf.t[i][0]       = c.t[i][0];
                t_conf.t[i][1]       = c.t[i][1];
                t_conf.t[i][2]       = c.t[i][2];
                t_conf.name[i]       = c.name[i] ? c.name[i] : "<unmapped>";
                t_conf.parentName[i] = c.parentName[i] ? c.parentName[i] : "<none>";
            }
            // FK-world dump data (dump fires only; main-thread NiAVObject reads are safe HERE, not in
            // the inner hook): ragdoll parentIndices + the XP32 node worlds the FK compares against,
            // plus an arm-chain census — every live local from each upper arm up to spine2, exposing
            // any runtime-written CME/clavicle transform the follow math could be skipping.
            t_conf.haveFk = false;
            // par[] now fills EVERY armed drive (2026-08-29): PUSH SENSE v5's FK needs it, and
            // it is 18 int16 copies. The node worlds + arm census stay dump-only.
            if (const RE::hkaSkeleton* sk2 = driver->ragdoll->skeleton.get();
                sk2 && sk2->parentIndices.data() && (int)sk2->bones.size() >= c.n) {
                for (int i = 0; i < c.n; ++i) t_conf.par[i] = sk2->parentIndices.data()[i];
                t_conf.haveFk = true;
            }
            // PUSH SENSE v5: body positions read HERE (prep = main thread, node/collision reads
            // safe; the inner hook must stay pure math). GAME units, world. Sensing stands down
            // while furniture-loosened (free bodies = the gap would be garbage) and for ~10
            // frames after re-attach (the same CTD-hardening window the conform uses).
            t_conf.senseArmed = sense && pushOk && c.pushFrames >= 10;   // 2026-09-12: was `attached && attachedFrames >= 10`
            t_conf.footBone[0] = c.footBone[0]; t_conf.footBone[1] = c.footBone[1];   // v29 foot trip channel
            for (int s5 = 0; s5 < kSenseN; ++s5) {
                t_conf.senseBone[s5]      = c.senseBone[s5];
                t_conf.senseBodyValid[s5] = false;
                const int bi = c.senseBone[s5];
                if (!t_conf.senseArmed || bi < 0 || !c.node[bi]) continue;
                auto* colObj = c.node[bi]->collisionObject.get();
                auto* rb  = colObj ? static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody() : nullptr;
                auto* hkb = rb ? rb->GetRigidBody() : nullptr;
                if (!hkb) continue;
                // ★ 2026-09-12 (the lean exception): a body that is NOT in a physics world has a FROZEN transform — the
                // engine's enter-furniture path sets the graph's world to null unless PLANCK's seamlessFurnitureTransition
                // keeps an active actor's ragdoll in. Reading it would publish a garbage gap that could walk or knock her
                // down. Skip it: an out-of-world ragdoll reads as blind ("0 of N joints"), never as a push.
                if (!hkb->world) continue;
                alignas(16) float b4[4];
                _mm_store_ps(b4, hkb->motion.motionState.transform.translation.quad);
                const float toGame = 1.f / kConfHavokScale;
                t_conf.senseBody[s5][0] = b4[0] * toGame;
                t_conf.senseBody[s5][1] = b4[1] * toGame;
                t_conf.senseBody[s5][2] = b4[2] * toGame;
                // v7.5: the body's world ROTATION — hkTransform stores a 3x3 as three COLUMN
                // vectors; convert to row-major entry[r][c] = col_c[r]. (Any residual convention
                // error is absorbed by the rest-latched R_off, by construction.)
                {
                    const auto& rot = hkb->motion.motionState.transform.rotation;
                    alignas(16) float c0[4], c1[4], c2[4];
                    _mm_store_ps(c0, rot.col0.quad);
                    _mm_store_ps(c1, rot.col1.quad);
                    _mm_store_ps(c2, rot.col2.quad);
                    float* R = t_conf.senseBodyR[s5];
                    R[0] = c0[0]; R[1] = c1[0]; R[2] = c2[0];
                    R[3] = c0[1]; R[4] = c1[1]; R[5] = c2[1];
                    R[6] = c0[2]; R[7] = c1[2]; R[8] = c2[2];
                }
                t_conf.senseChild[s5] = c.senseChild[s5];
                t_conf.senseBodyValid[s5] = true;
            }
            {   // v11: the sense path READS THE NODE now (user's model, PUSHTRIAD-verified) — always fill
                for (int i = 0; i < c.n; ++i) {
                    const RE::NiPoint3 w = c.node[i] ? c.node[i]->world.translate : RE::NiPoint3{};
                    t_conf.nodeW[i][0] = w.x; t_conf.nodeW[i][1] = w.y; t_conf.nodeW[i][2] = w.z;
                }
            }
            if (t_conf.dump) {
                for (const char* ua : { "NPC R UpperArm [RUar]", "NPC L UpperArm [LUar]" }) {
                    const RE::NiAVObject* nd = root->GetObjectByName(ua);
                    for (int depth = 0; nd && depth < 6; ++depth) {
                        const auto& L = nd->local;
                        logger::info("PCONF ARMCHAIN {:08X} [{}] '{}' local.t=[{:.3f} {:.3f} {:.3f}] scale={:.4f}",
                                     id, depth, nd->name.c_str(), L.translate.x, L.translate.y, L.translate.z,
                                     L.scale);
                        if (std::strcmp(nd->name.c_str(), "NPC Spine2 [Spn2]") == 0) break;
                        nd = nd->parent;
                    }
                }
            }
            t_conf.armed = t_conf.write || t_conf.dump || t_conf.senseArmed;

            const std::uint64_t ns = (std::uint64_t)((double)(ConfQpc() - q0) * ConfNsPerCount());
            if (rebuilt) {
                // One-off resolve spike (18 GetObjectByName walks) — logged, kept OUT of the
                // steady-state stats so `perf`'s mean/max answer the per-frame budget question.
                logger::info("PCONF map+first-compute for {:08X} took {:.0f}us (one-off; excluded from perf stats)",
                             id, ns / 1000.0);
                return;
            }
            s_confFires.fetch_add(1, std::memory_order_relaxed);
            s_confSumNs.fetch_add(ns, std::memory_order_relaxed);
            std::uint64_t prevMax = s_confMaxNs.load(std::memory_order_relaxed);
            while (ns > prevMax && !s_confMaxNs.compare_exchange_weak(prevMax, ns, std::memory_order_relaxed)) {}
            if (ns > 50'000) {   // the user's stated budget: ~50us per actor-frame
                static float s_lastWarn = -10.f;
                const float nowS = ArmNowSeconds();
                if (nowS - s_lastWarn > 5.f) {
                    s_lastWarn = nowS;
                    logger::warn("PCONF WARN {:08X}: conform compute {:.1f}us > 50us budget (n={} everyN={}) — "
                                 "raise poseConformEveryN", id, ns / 1000.0, c.n, N);
                }
            }
            // 60 s summary while the write is live (main-thread drive path — statics are safe here,
            // the ApplyHeelFix precedent).
            if (on) {
                static auto          s_winStart  = std::chrono::steady_clock::now();
                static std::uint64_t s_winFires  = 0, s_winNs = 0, s_winMaxNs = 0;
                static std::uint32_t s_winActors = 0, s_winEpoch = 1;
                ++s_winFires;
                s_winNs += ns;
                if (ns > s_winMaxNs) s_winMaxNs = ns;
                if (c.winEpoch != s_winEpoch) { c.winEpoch = s_winEpoch; ++s_winActors; }
                const auto now = std::chrono::steady_clock::now();
                if (now - s_winStart >= std::chrono::seconds(60)) {
                    logger::info("PCONF perf 60s: {} drives / {} actor(s) conformed, mean {:.1f}us max {:.1f}us "
                                 "per drive (everyN={})",
                                 s_winFires, s_winActors,
                                 s_winFires ? (double)s_winNs / (double)s_winFires / 1000.0 : 0.0,
                                 s_winMaxNs / 1000.0, N);
                    s_winStart = now;
                    s_winFires = 0; s_winNs = 0; s_winMaxNs = 0; s_winActors = 0;
                    ++s_winEpoch;
                }
            }
        }
    }   // anonymous namespace (pose conform)

    // The inner-hook half (0xA26C05): dump the incoming-vs-XP32 comparison, then overwrite the
    // conformed bones' translations. Consumes + disarms the TLS so a driveToPose that ever entered
    // outside our outer hook can never see a stale set.
// ── ReDrive v2, the inner-drive seam (called from Hooks.cpp InnerDriveChainHook) ──────────
// Havok's own prescription for m_boneWeights (SDK, setBoneWeights): "set them before calling
// driveToPose() and then set them to HK_NULL again". The CALL at 0xA26C05 IS driveToPose, so
// Apply immediately before the chained call and Restore immediately after — all return paths.
void ReDriveInnerApply(void* controller)
{
    auto& t = s_rdW;
    t.applied = false;
    if (!t.armed || !controller) return;
    auto* rc = reinterpret_cast<HkaRbCtlMirror*>(controller);

    // ★ LIVE PLAUSIBILITY CHECK — the mirror layout was READ from the SDK header, but this
    // verifies it against the running engine before any write: the bone count must match the
    // ragdoll we measured in the outer hook, and the palette must hold at least one element
    // (measured: size 1, matching the track's numData=1). Wrong offsets fail this instantly.
    if (rc->numRigidBodies != t.n || rc->palette.size < 1 || !rc->palette.data) {
        if (RdSay(t.id, kRdCtlBad))
            logger::warn("REDRIVE {:08X} REFUSED at the controller: numRigidBodies={} (expected {}) "
                         "paletteSize={} paletteData={} -- mirror layout does not match the live "
                         "engine, NOT writing boneWeights",
                         t.id, rc->numRigidBodies, t.n, rc->palette.size,
                         rc->palette.data ? "ok" : "NULL");
        t.armed = false;
        return;
    }

    // ★ EXACT EXTRAPOLATION CLAMP, from the LIVE palette. At this point PLANCK's stamp has
    // already happened (its PreDriveToPoseHook precedes the engine's driveToPose), so palette[0]
    // holds the REAL hierarchyGain (0.6 under PLANCK). The weight multiplies hierarchyGain,
    // which is a slerp/lerp FRACTION — w*hg > 1 extrapolates PAST the target. Cap w at 1/hg.
    const float hg = *reinterpret_cast<const float*>(rc->palette.data);   // hierarchyGain @ +0
    if (hg > 0.01f && hg < 1.5f) {
        const float wmax = 1.0f / hg;
        int clamped = 0;
        for (int i = 0; i < t.n; ++i)
            if (t.w[i] > wmax) { t.w[i] = wmax; ++clamped; }
        if (clamped && RdSay(t.id, kRdClamped))
            logger::info("REDRIVE {:08X} clamped {} weight(s) to {:.3f} (= 1/hierarchyGain {:.3f}) "
                         "-- higher would extrapolate the drive target past the pose",
                         t.id, clamped, wmax, hg);
    }

    t.saved = rc->boneWeights;

    // ★ MULTIPLY INTO THE ENGINE'S OWN WEIGHTS, never replace them (2026-08-28, from the
    // "prev=non-null(!)" receipt). The driver sets its own per-bone array on the controller
    // before this call -- likely the behavior graph's ragdoll blend weights. Replacing the
    // pointer flattened whatever the engine put there on every bone we were NOT dialling.
    // Composing keeps engine semantics exact where our knob is 1.0: w = engineW * knob.
    if (t.saved) {
        for (int i = 0; i < t.n; ++i) t.w[i] *= t.saved[i];
    }
    rc->boneWeights = t.w;
    t.applied = true;
    if (RdSay(t.id, kRdApplied)) {
        char dump[360]; int off2 = 0;
        const int show = t.n < 18 ? t.n : 18;
        for (int k = 0; k < show && off2 < (int)sizeof(dump) - 16; ++k)
            off2 += std::snprintf(dump + off2, sizeof(dump) - off2, "%.2f ",
                                  t.saved ? t.saved[k] : 1.f);
        logger::info("REDRIVE {:08X} boneWeights SET (composed): n={} hierarchyGain={:.3f} "
                     "engine's own weights were [{}] -- ours multiply into them",
                     t.id, t.n, hg, dump);
    }
}

void ReDriveInnerRestore(void* controller)
{
    auto& t = s_rdW;
    if (t.applied && controller)
        reinterpret_cast<HkaRbCtlMirror*>(controller)->boneWeights = t.saved;
    t.applied = false;
    t.armed   = false;                         // consumed: one arm drives exactly one call
}

// Leak guard: the outer hook calls this after its chained call returns. If PLANCK's
// DriveToPoseHook early-returned (dead actor, no ragdoll entry), the inner site never ran and
// the arm would otherwise survive into the NEXT actor's drive on this thread.
void ReDriveDisarm()
{
    s_rdW.armed   = false;
    s_rdW.applied = false;
}

    void ApplyPoseConform(void* poseLocalSpace, const RE::hkQsTransform* worldFromModel)
    {
        if (!t_conf.armed) return;
        t_conf.armed = false;
        if (!poseLocalSpace) return;
        const std::uint64_t q0 = ConfQpc();
        auto* pose = reinterpret_cast<RE::hkQsTransform*>(poseLocalSpace);
        const float k = (t_conf.k > 1e-6f) ? t_conf.k : 1.f;

        const bool doDump = t_conf.dump;
        t_conf.dump = false;
        if (doDump) {
            float wfmS = 1.f;
            if (worldFromModel) {
                alignas(16) float s4[4];
                _mm_store_ps(s4, worldFromModel->scale.quad);
                wfmS = s4[0];
            }
            logger::info("PCONF DUMP {:08X}: n={} scale(track)={:.4f} scale(innerWFM)={:.4f} k={:.6f} write={} "
                         "(GAME u, ragdoll parent-local; in=incoming drive pose, xp=XP32 chain, d=xp-in)",
                         t_conf.formId, t_conf.n, t_conf.scale, wfmS, k, t_conf.write);
            for (int i = 0; i < t_conf.n; ++i) {
                alignas(16) float in4[4];
                _mm_store_ps(in4, pose[i].translation.quad);
                const float ix = in4[0] / k, iy = in4[1] / k, iz = in4[2] / k;
                const float dx = t_conf.t[i][0] - ix, dy = t_conf.t[i][1] - iy, dz = t_conf.t[i][2] - iz;
                const char* tag = t_conf.flags[i] == 1 ? " ROOT(skip)"
                                : t_conf.flags[i] == 2 ? " UNRESOLVED(skip)" : "";
                logger::info("PCONF DUMP {:08X} [{:2}] {:<22} <- {:<22} in=[{:8.3f} {:8.3f} {:8.3f}] "
                             "xp=[{:8.3f} {:8.3f} {:8.3f}] d=[{:7.3f} {:7.3f} {:7.3f}] |d|={:.3f}{}",
                             t_conf.formId, i, t_conf.name[i], t_conf.parentName[i], ix, iy, iz,
                             t_conf.t[i][0], t_conf.t[i][1], t_conf.t[i][2], dx, dy, dz,
                             std::sqrt(dx * dx + dy * dy + dz * dz), tag);
            }
        }

        if (t_conf.write) {
            for (int i = 0; i < t_conf.n; ++i) {
                if (t_conf.flags[i] == 2) continue;                 // unresolved: animation-owned
                if (t_conf.flags[i] == 1) {
                    // ── ROOT (poseConformRoot v2): apply a CONSTANT model-space delta, captured
                    // once per ragdoll build. NEVER target the live node here — PLANCK writes the
                    // driven ragdoll back into the nodes, so a live target integrates to orbit
                    // (the 2026-07-12 sky-launch). Capture fire: delta = XP32-implied target minus
                    // the incoming animation root, sanity-capped; then it is bind-vs-bind constant.
                    if (t_conf.rootMode == 0) continue;
                    alignas(16) float in4[4];
                    _mm_store_ps(in4, pose[i].translation.quad);
                    if (t_conf.rootMode == 1) {
                        float d[3] = { t_conf.t[i][0] - in4[0] / k,
                                       t_conf.t[i][1] - in4[1] / k,
                                       t_conf.t[i][2] - in4[2] / k };
                        // HEEL-AWARE CAPTURE (2026-07-13, the Roisin/Astanova double-count):
                        // our hook sees the PRE-bias pose while the live nodes carry the
                        // NiOverride heel lift — the raw delta = bind + heelZ, and the heel
                        // drive-bias then adds heelZ again every frame. Subtract it once here
                        // so each system counts the lift exactly once (hf toggles stay clean).
                        d[2] -= GetHeelDriveBias();
                        const float m2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
                        if (m2 > 100.f) continue;                   // >10u = bad frame; retry next fire
                        if (t_conf.rootCache) {
                            t_conf.rootCache->rootDelta[0] = d[0];
                            t_conf.rootCache->rootDelta[1] = d[1];
                            t_conf.rootCache->rootDelta[2] = d[2];
                            t_conf.rootCache->rootDeltaValid = true;
                        }
                        t_conf.rootDelta[0] = d[0]; t_conf.rootDelta[1] = d[1]; t_conf.rootDelta[2] = d[2];
                    }
                    in4[0] += t_conf.rootDelta[0] * k;
                    in4[1] += t_conf.rootDelta[1] * k;
                    in4[2] += t_conf.rootDelta[2] * k;
                    pose[i].translation.quad = _mm_load_ps(in4);
                    continue;
                }
                alignas(16) float v[4];
                _mm_store_ps(v, pose[i].translation.quad);   // preserve w
                v[0] = t_conf.t[i][0] * k;
                v[1] = t_conf.t[i][1] * k;
                v[2] = t_conf.t[i][2] * k;
                pose[i].translation.quad = _mm_load_ps(v);
            }
        }

        // FK-WORLD dump (2026-07-10): forward-kinematics of the FINAL drive pose (anim rotations +
        // conformed translations) through the ragdoll parentIndices, composed with worldFromModel —
        // the drive's actual world-space command per bone — vs the live XP32 node world (cached at
        // prep). The discriminator: arms off HERE = the drive itself commands the offset; ~0 here
        // with the arm visibly off = the constraint side (shoulder anchor) owns the displacement.
        if (doDump && t_conf.haveFk) {
            if (!worldFromModel) {
                logger::info("PCONF FKW {:08X}: skipped (no worldFromModel param)", t_conf.formId);
            } else {
                RE::NiMatrix3 mR[kConfMaxBones];
                RE::NiPoint3  mt[kConfMaxBones];
                const RE::NiMatrix3 Rw = MatrixFromQuat(worldFromModel->rotation);
                const RE::NiPoint3  tw = GetTranslation(*worldFromModel);
                alignas(16) float ws4[4];
                _mm_store_ps(ws4, worldFromModel->scale.quad);
                const float ws     = (ws4[0] > 0.01f && ws4[0] < 100.f) ? ws4[0] : 1.f;
                const float toGame = 1.f / kConfHavokScale;
                logger::info("PCONF FKW {:08X}: drive world command vs XP32 node world (GAME u; wfmScale={:.4f})",
                             t_conf.formId, ws);
                for (int i = 0; i < t_conf.n; ++i) {
                    const RE::NiMatrix3 Ri = MatrixFromQuat(pose[i].rotation);
                    alignas(16) float t4[4];
                    _mm_store_ps(t4, pose[i].translation.quad);
                    const RE::NiPoint3 ti{ t4[0], t4[1], t4[2] };
                    const int p = t_conf.par[i];
                    if (p >= 0 && p < i) { mR[i] = Mat3Mul(mR[p], Ri); mt[i] = mt[p] + mR[p] * ti; }
                    else                 { mR[i] = Ri; mt[i] = ti; }   // root (or out-of-order guard)
                    const RE::NiPoint3 fkH = tw + (Rw * mt[i]) * ws;
                    const float fx = fkH.x * toGame, fy = fkH.y * toGame, fz = fkH.z * toGame;
                    const float dx = fx - t_conf.nodeW[i][0], dy = fy - t_conf.nodeW[i][1],
                                dz = fz - t_conf.nodeW[i][2];
                    logger::info("PCONF FKW {:08X} [{:2}] {:<22} fkW=[{:8.2f} {:8.2f} {:8.2f}] "
                                 "boneW=[{:8.2f} {:8.2f} {:8.2f}] d=[{:6.2f} {:6.2f} {:6.2f}] |d|={:.2f}{}",
                                 t_conf.formId, i, t_conf.name[i], fx, fy, fz,
                                 t_conf.nodeW[i][0], t_conf.nodeW[i][1], t_conf.nodeW[i][2],
                                 dx, dy, dz, std::sqrt(dx * dx + dy * dy + dz * dz),
                                 t_conf.flags[i] == 1 ? " ROOT" : (t_conf.flags[i] == 2 ? " UNRESOLVED" : ""));
                }
            }
        }
        // ═══ PUSH SENSE v5 (2026-08-29): body vs THE ANIMATION'S INTENT ═══════════════════
        // The pose in hand here is the drive's true target — the animation the actor is
        // SUPPOSED to be in (PLANCK injects its clean BSLookAtModifier capture into this very
        // pose; conform translations are our corrected intent). FK it to world exactly as the
        // FKW dump does, subtract from the live body, correct by the rest-latched bone-local
        // offset: what remains is how far the player's push made physics lose. ~18 mat-muls.
        if (t_conf.senseArmed && t_conf.haveFk && worldFromModel) {
            RE::NiMatrix3 sR[kConfMaxBones];
            RE::NiPoint3  st[kConfMaxBones];
            const RE::NiMatrix3 Rw2 = MatrixFromQuat(worldFromModel->rotation);
            const RE::NiPoint3  tw2 = GetTranslation(*worldFromModel);
            alignas(16) float ws5[4];
            _mm_store_ps(ws5, worldFromModel->scale.quad);
            const float ws2     = (ws5[0] > 0.01f && ws5[0] < 100.f) ? ws5[0] : 1.f;
            const float toGame2 = 1.f / kConfHavokScale;
            for (int i = 0; i < t_conf.n; ++i) {
                const RE::NiMatrix3 Ri = MatrixFromQuat(pose[i].rotation);
                alignas(16) float t5[4];
                _mm_store_ps(t5, pose[i].translation.quad);
                const RE::NiPoint3 ti{ t5[0], t5[1], t5[2] };
                const int pp = t_conf.par[i];
                if (pp >= 0 && pp < i) { sR[i] = Mat3Mul(sR[pp], Ri); st[i] = st[pp] + sR[pp] * ti; }
                else                   { sR[i] = Ri; st[i] = ti; }
            }
            // ★★ v9.0 BODY SELF-MOTION — THE REFERENCE-FREE INSTRUMENT (2026-08-30).
            // Every other number in this file is measured against MY animation reference, so if
            // that reference is wrong my probe measures my own error (the ledger's healthy figure
            // is 0.21u mean joint-vs-node; I am reporting 1-6u, which is itself suspicious).
            // This measures nothing but the BODY: how far each rigid body actually TRAVELS,
            // frame to frame, summed over a second. A still NPC's bodies should travel almost
            // nothing. If the path length is large while she stands, the bodies are genuinely
            // vibrating - proof of the shiver that owes nothing to my math.
            if (ObjectHold::PushStepIdleProbe() != 0.f) {
                struct BodyJit { std::uint32_t id = 0; float prev[kSenseN][3] = {};
                                 bool have[kSenseN] = {}; float path[kSenseN] = {};
                                 float maxStep[kSenseN] = {}; int n = 0; float lastLogS = -1e9f;
                                 // v9.1: the window's START position - path/net separates a
                                 // VIBRATION (huge path, no net travel) from real motion.
                                 float start[kSenseN][3] = {}; bool haveStart[kSenseN] = {}; };
                static BodyJit bj;
                if (bj.id != t_conf.formId) { bj = BodyJit{}; bj.id = t_conf.formId; }
                for (int s9 = 0; s9 < kSenseN; ++s9) {
                    if (!t_conf.senseBodyValid[s9]) continue;
                    const float* b = t_conf.senseBody[s9];
                    if (bj.have[s9]) {
                        const float dx = b[0] - bj.prev[s9][0], dy = b[1] - bj.prev[s9][1],
                                    dz = b[2] - bj.prev[s9][2];
                        const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                        bj.path[s9] += step;
                        if (step > bj.maxStep[s9]) bj.maxStep[s9] = step;
                    }
                    bj.prev[s9][0] = b[0]; bj.prev[s9][1] = b[1]; bj.prev[s9][2] = b[2];
                    bj.have[s9] = true;
                    if (!bj.haveStart[s9]) {
                        bj.start[s9][0] = b[0]; bj.start[s9][1] = b[1]; bj.start[s9][2] = b[2];
                        bj.haveStart[s9] = true;
                    }
                }
                ++bj.n;
                const float nowP = ArmNowSeconds();
                if (nowP - bj.lastLogS > 1.f) {
                    bj.lastLogS = nowP;
                    char bbuf[224]; bbuf[0] = 0;
                    for (int s9 = 0; s9 < kSenseN; ++s9) {
                        const std::size_t len = std::strlen(bbuf);
                        float net = 0.f;
                        if (bj.haveStart[s9] && bj.have[s9]) {
                            const float nx = bj.prev[s9][0] - bj.start[s9][0];
                            const float ny = bj.prev[s9][1] - bj.start[s9][1];
                            const float nz = bj.prev[s9][2] - bj.start[s9][2];
                            net = std::sqrt(nx * nx + ny * ny + nz * nz);
                        }
                        // ratio = path / net. ~1 means she genuinely travelled; >>1 means the
                        // body went nowhere while shaking - a VIBRATION, measured.
                        const float ratio = net > 0.05f ? bj.path[s9] / net : 999.f;
                        std::snprintf(bbuf + len, sizeof bbuf - len, "%ss%d:%.1f/%.2f/x%.0f",
                                      s9 ? " " : "", kSenseSlot[s9], bj.path[s9], net, ratio);
                        bj.path[s9] = 0.f; bj.maxStep[s9] = 0.f; bj.haveStart[s9] = false;
                    }
                    logger::info("BODYJITTER {:08X} {} drives, per bone path/net/ratio [{}] "
                                 "(ratio ~1 = real travel; >>1 = vibrating in place)",
                                 t_conf.formId, bj.n, bbuf);
                    bj.n = 0;
                }
            }
            // ── ARMPROBE (knob armProbe) ────────────────────────────────────────────────
            // Four channels for the right arm, accumulated per drive, reported at 1 Hz.
            //   node = visible XP32 bone (WORLD)      — what the user actually SEES
            //   body = Havok ragdoll body (WORLD)     — what physics holds
            //   anim = incoming drive target          — where the ANIMATION wants it
            //   xp   = PPB's XP32 chain value         — where the CONFORM wants it
            // anim/xp are ragdoll-parent-local (that is the space the drive pose lives in); node
            // and body are world. Comparing PATH between them is still valid: a bone that is
            // vibrating shows path >> net in whichever layer is doing the vibrating.
            if (t_conf.armOn) {
                struct ArmAcc {
                    std::uint32_t id = 0; int n = 0; float lastLogS = -1e9f;
                    float prev[3][4][3] = {}; float start[3][4][3] = {};
                    bool  have[3][4] = {};    bool  haveStart[3][4] = {};
                    float path[3][4] = {};    float maxStep[3][4] = {};
                    float gapSum[3] = {};     float gapMax[3] = {}; int gapN[3] = {};
                };
                static ArmAcc aa;
                if (aa.id != t_conf.formId) { aa = ArmAcc{}; aa.id = t_conf.formId; }
                for (int ai = 0; ai < 3; ++ai) {
                    const int bi = t_conf.armBone[ai];
                    float ch[4][3]; bool ok[4] = { false, false, false, false };
                    if (t_conf.armNodeOk[ai]) { for (int k2 = 0; k2 < 3; ++k2) ch[0][k2] = t_conf.armNodeW[ai][k2]; ok[0] = true; }
                    if (t_conf.armBodyOk[ai]) { for (int k2 = 0; k2 < 3; ++k2) ch[1][k2] = t_conf.armBodyW[ai][k2]; ok[1] = true; }
                    if (bi >= 0 && bi < t_conf.n) {
                        alignas(16) float in4[4];
                        _mm_store_ps(in4, pose[bi].translation.quad);
                        ch[2][0] = in4[0] / k; ch[2][1] = in4[1] / k; ch[2][2] = in4[2] / k; ok[2] = true;
                        ch[3][0] = t_conf.t[bi][0]; ch[3][1] = t_conf.t[bi][1]; ch[3][2] = t_conf.t[bi][2]; ok[3] = true;
                    }
                    for (int cix = 0; cix < 4; ++cix) {
                        if (!ok[cix]) continue;
                        if (aa.have[ai][cix]) {
                            const float dx = ch[cix][0] - aa.prev[ai][cix][0];
                            const float dy = ch[cix][1] - aa.prev[ai][cix][1];
                            const float dz = ch[cix][2] - aa.prev[ai][cix][2];
                            const float st = std::sqrt(dx * dx + dy * dy + dz * dz);
                            aa.path[ai][cix] += st;
                            if (st > aa.maxStep[ai][cix]) aa.maxStep[ai][cix] = st;
                        }
                        for (int k2 = 0; k2 < 3; ++k2) aa.prev[ai][cix][k2] = ch[cix][k2];
                        aa.have[ai][cix] = true;
                        if (!aa.haveStart[ai][cix]) {
                            for (int k2 = 0; k2 < 3; ++k2) aa.start[ai][cix][k2] = ch[cix][k2];
                            aa.haveStart[ai][cix] = true;
                        }
                    }
                    if (ok[0] && ok[1]) {                       // node-vs-body separation
                        const float gx = ch[0][0] - ch[1][0], gy = ch[0][1] - ch[1][1], gz = ch[0][2] - ch[1][2];
                        const float g = std::sqrt(gx * gx + gy * gy + gz * gz);
                        aa.gapSum[ai] += g; ++aa.gapN[ai];
                        if (g > aa.gapMax[ai]) aa.gapMax[ai] = g;
                    }
                }
                ++aa.n;
                const float nowA = ArmNowSeconds();
                if (nowA - aa.lastLogS > 1.f) {
                    aa.lastLogS = nowA;
                    static const char* kTag[3]  = { "RUar", "RLar", "RHnd" };
                    static const char* kChan[4] = { "node", "body", "anim", "xp" };
                    for (int ai = 0; ai < 3; ++ai) {
                        char buf[288]; buf[0] = 0;
                        for (int cix = 0; cix < 4; ++cix) {
                            float net = 0.f;
                            if (aa.haveStart[ai][cix] && aa.have[ai][cix]) {
                                const float nx = aa.prev[ai][cix][0] - aa.start[ai][cix][0];
                                const float ny = aa.prev[ai][cix][1] - aa.start[ai][cix][1];
                                const float nz = aa.prev[ai][cix][2] - aa.start[ai][cix][2];
                                net = std::sqrt(nx * nx + ny * ny + nz * nz);
                            }
                            const float ratio = net > 0.05f ? aa.path[ai][cix] / net : 999.f;
                            const std::size_t len = std::strlen(buf);
                            std::snprintf(buf + len, sizeof buf - len, "%s%s:%.2f/%.2f/x%.0f(max%.3f)",
                                          cix ? " " : "", kChan[cix], aa.path[ai][cix], net, ratio,
                                          aa.maxStep[ai][cix]);
                            aa.path[ai][cix] = 0.f; aa.maxStep[ai][cix] = 0.f; aa.haveStart[ai][cix] = false;
                        }
                        const float gm = aa.gapN[ai] ? aa.gapSum[ai] / aa.gapN[ai] : -1.f;
                        logger::info("ARMPROBE {:08X} {} {} drives [{}] node-vs-body gap mean {:.2f}u max {:.2f}u "
                                     "(path/net/ratio/maxstep, game u; ratio>>1 = vibrating in place)",
                                     t_conf.formId, kTag[ai], aa.n, buf, gm, aa.gapMax[ai]);
                        aa.gapSum[ai] = 0.f; aa.gapMax[ai] = 0.f; aa.gapN[ai] = 0;
                    }
                    aa.n = 0;
                }
            }

            // ── LEGPROBE (knob legProbe), v25 ───────────────────────────────────────────
            // The leg twin of ARMPROBE. Built 2026-09-09 because the user reports jitter in the
            // hands AND feet, and only the arm had an instrument; ARMPROBE had already proved the
            // right hand vibrates (worst window: 19.07u of path to finish 0.23u away, ratio x85,
            // one 30.7u single-frame step) while the feet were pure argument.
            //   slots 0/1/2 = R Thigh -> R Calf -> R Foot, the CHAIN: read down it and the bone
            //                 where path/net first blows up is where the shake ENTERS.
            //   slot  3     = L Foot, the SYMMETRY CONTROL. HEELFIX's postPhysics root
            //                 compensation moves BOTH feet; a single-body problem moves one.
            // Channels are ARMPROBE's so the numbers compare directly: node = what you SEE,
            // body = what physics holds, anim = the incoming drive target, xp = the conform target.
            // ★ arot is the channel ARMPROBE does not have, and the reason this probe can answer
            // the question the arm one could not. anim/xp are parent-LOCAL TRANSLATIONS, which for
            // a child bone is a constant bone length - hence anim path 0.00u in 1220 of 1221 arm
            // windows. That is a blind probe, not a frozen animation: a limb's motion is ROTATION.
            // arot sums the drive target's angular step in degrees against its net angle, so
            // "is the animation pose itself shaking" finally has a number. Large arot path with a
            // small arot net convicts the drive; a calm arot under a wild node/body acquits it.
            if (t_conf.legOn) {
                struct LegAcc {
                    std::uint32_t id = 0; int n = 0; float lastLogS = -1e9f;
                    float prev[4][4][3] = {}; float start[4][4][3] = {};
                    bool  have[4][4] = {};    bool  haveStart[4][4] = {};
                    float path[4][4] = {};    float maxStep[4][4] = {};
                    float qPrev[4][4] = {};   float qStart[4][4] = {};
                    bool  qHave[4] = {};      bool  qHaveStart[4] = {};
                    float qPath[4] = {};      float qMaxStep[4] = {};
                    float gapSum[4] = {};     float gapMax[4] = {}; int gapN[4] = {};
                };
                static LegAcc la;
                if (la.id != t_conf.formId) { la = LegAcc{}; la.id = t_conf.formId; }
                for (int li = 0; li < 4; ++li) {
                    const int bi = t_conf.legBone[li];
                    float ch[4][3]; bool ok[4] = { false, false, false, false };
                    if (t_conf.legNodeOk[li]) { for (int k2 = 0; k2 < 3; ++k2) ch[0][k2] = t_conf.legNodeW[li][k2]; ok[0] = true; }
                    if (t_conf.legBodyOk[li]) { for (int k2 = 0; k2 < 3; ++k2) ch[1][k2] = t_conf.legBodyW[li][k2]; ok[1] = true; }
                    if (bi >= 0 && bi < t_conf.n) {
                        alignas(16) float in4[4];
                        _mm_store_ps(in4, pose[bi].translation.quad);
                        ch[2][0] = in4[0] / k; ch[2][1] = in4[1] / k; ch[2][2] = in4[2] / k; ok[2] = true;
                        ch[3][0] = t_conf.t[bi][0]; ch[3][1] = t_conf.t[bi][1]; ch[3][2] = t_conf.t[bi][2]; ok[3] = true;
                        // ── the rotation channel: the drive target's OWN angular path ──
                        // q and -q are the same rotation, so the dot product is taken absolute
                        // before the arc-cos - without that a sign flip reads as a 180 deg jump
                        // and one flip would swamp a whole second of real motion.
                        alignas(16) float q4[4];
                        _mm_store_ps(q4, pose[bi].rotation.vec.quad);
                        if (la.qHave[li]) {
                            float d = q4[0] * la.qPrev[li][0] + q4[1] * la.qPrev[li][1] +
                                      q4[2] * la.qPrev[li][2] + q4[3] * la.qPrev[li][3];
                            if (d < 0.f) d = -d;
                            if (d > 1.f) d = 1.f;
                            const float st = 2.f * std::acos(d) * 57.29578f;   // degrees
                            la.qPath[li] += st;
                            if (st > la.qMaxStep[li]) la.qMaxStep[li] = st;
                        }
                        for (int k2 = 0; k2 < 4; ++k2) la.qPrev[li][k2] = q4[k2];
                        la.qHave[li] = true;
                        if (!la.qHaveStart[li]) {
                            for (int k2 = 0; k2 < 4; ++k2) la.qStart[li][k2] = q4[k2];
                            la.qHaveStart[li] = true;
                        }
                    }
                    for (int cix = 0; cix < 4; ++cix) {
                        if (!ok[cix]) continue;
                        if (la.have[li][cix]) {
                            const float dx = ch[cix][0] - la.prev[li][cix][0];
                            const float dy = ch[cix][1] - la.prev[li][cix][1];
                            const float dz = ch[cix][2] - la.prev[li][cix][2];
                            const float st = std::sqrt(dx * dx + dy * dy + dz * dz);
                            la.path[li][cix] += st;
                            if (st > la.maxStep[li][cix]) la.maxStep[li][cix] = st;
                        }
                        for (int k2 = 0; k2 < 3; ++k2) la.prev[li][cix][k2] = ch[cix][k2];
                        la.have[li][cix] = true;
                        if (!la.haveStart[li][cix]) {
                            for (int k2 = 0; k2 < 3; ++k2) la.start[li][cix][k2] = ch[cix][k2];
                            la.haveStart[li][cix] = true;
                        }
                    }
                    if (ok[0] && ok[1]) {                       // node-vs-body separation
                        const float gx = ch[0][0] - ch[1][0], gy = ch[0][1] - ch[1][1], gz = ch[0][2] - ch[1][2];
                        const float g = std::sqrt(gx * gx + gy * gy + gz * gz);
                        la.gapSum[li] += g; ++la.gapN[li];
                        if (g > la.gapMax[li]) la.gapMax[li] = g;
                    }
                }
                ++la.n;
                const float nowL = ArmNowSeconds();
                if (nowL - la.lastLogS > 1.f) {
                    la.lastLogS = nowL;
                    static const char* kLTag[4] = { "RThg", "RClf", "Rft ", "Lft " };
                    static const char* kChanL[4] = { "node", "body", "anim", "xp" };
                    for (int li = 0; li < 4; ++li) {
                        char buf[288]; buf[0] = 0;
                        for (int cix = 0; cix < 4; ++cix) {
                            float net = 0.f;
                            if (la.haveStart[li][cix] && la.have[li][cix]) {
                                const float nx = la.prev[li][cix][0] - la.start[li][cix][0];
                                const float ny = la.prev[li][cix][1] - la.start[li][cix][1];
                                const float nz = la.prev[li][cix][2] - la.start[li][cix][2];
                                net = std::sqrt(nx * nx + ny * ny + nz * nz);
                            }
                            const float ratio = net > 0.05f ? la.path[li][cix] / net : 999.f;
                            const std::size_t len = std::strlen(buf);
                            std::snprintf(buf + len, sizeof buf - len, "%s%s:%.2f/%.2f/x%.0f(max%.3f)",
                                          cix ? " " : "", kChanL[cix], la.path[li][cix], net, ratio,
                                          la.maxStep[li][cix]);
                            la.path[li][cix] = 0.f; la.maxStep[li][cix] = 0.f; la.haveStart[li][cix] = false;
                        }
                        // net ANGLE start->end, same absolute-dot rule as the per-step measure
                        float qnet = 0.f;
                        if (la.qHaveStart[li] && la.qHave[li]) {
                            float d = la.qPrev[li][0] * la.qStart[li][0] + la.qPrev[li][1] * la.qStart[li][1] +
                                      la.qPrev[li][2] * la.qStart[li][2] + la.qPrev[li][3] * la.qStart[li][3];
                            if (d < 0.f) d = -d;
                            if (d > 1.f) d = 1.f;
                            qnet = 2.f * std::acos(d) * 57.29578f;
                        }
                        const float qratio = qnet > 0.05f ? la.qPath[li] / qnet : 999.f;
                        const float gm = la.gapN[li] ? la.gapSum[li] / la.gapN[li] : -1.f;
                        logger::info("LEGPROBE {:08X} {} {} drives [{}] arot {:.1f}/{:.1f}deg x{:.0f}(max{:.2f}deg) "
                                     "node-vs-body gap mean {:.2f}u max {:.2f}u "
                                     "(path/net/ratio/maxstep, game u; ratio>>1 = vibrating in place; "
                                     "arot = the DRIVE TARGET's own rotation - big path + small net there = the ANIMATION is the thing shaking)",
                                     t_conf.formId, kLTag[li], la.n, buf, la.qPath[li], qnet, qratio,
                                     la.qMaxStep[li], gm, la.gapMax[li]);
                        la.qPath[li] = 0.f; la.qMaxStep[li] = 0.f; la.qHaveStart[li] = false;
                        la.gapSum[li] = 0.f; la.gapMax[li] = 0.f; la.gapN[li] = 0;
                    }
                    la.n = 0;
                }
            }
            // Player proximity (attribution for the CALIBRATION only — a latch must never
            // absorb a push). Probe set is PPB's own snapshot; safe from any thread.
            // 15, not 14: the player HEAD BOX probe is appended LAST by CopyProbes, so a
            // 14-slot buffer silently drops it — and this is the rest-latch's "is the player
            // near this bone" test. Dropping the head here would let the baseline absorb a
            // head push as if it were her own idle lag, which is precisely the class of bug
            // v8.8 was built to eliminate ("a latch must never absorb a push").
            // v11: 16, not 15 â the MOUTH probe is appended LAST by CopyProbes (after the head); a 15-slot
            // buffer silently dropped it, so a mouth-only contact could never freeze the baseline.
            PpbApi::ProbeView pv[16];
            const int np = PpbApi::CopyProbes(pv, 16);
            // ★ v11 STAND-DOWN (user, 2026-09-07). While ANY reaction plays â ours (PushStep stamps
            // SenseStandDown when it queues a stagger/ragdoll) or the engine's (knock state ≠ normal, which
            // also covers the Ragdoll→GetUp window) â the gap is servo lag on a fast animation, not a push.
            // PUSHTRIAD 11:31:54: node<->intent climbed to 14.03 u with the player doing nothing, and pub hit
            // 24.1 u, above the ragdoll bar, from the stagger we had just fired. Publish ZERO, learn nothing.
            const float nowSense = ArmNowSeconds();
            bool reacting = t_conf.knocked;
            if (!reacting) {
                std::scoped_lock lkg(s_senseGapMx);
                if (auto it = s_senseStandDown.find(t_conf.formId); it != s_senseStandDown.end()) {
                    if ((double)nowSense < it->second) reacting = true;
                    else s_senseStandDown.erase(it);
                }
            }
            for (int s5 = 0; s5 < kSenseN; ++s5) {
                const int bi = t_conf.senseBone[s5];
                if (bi < 0 || bi >= t_conf.n) continue;
                const RE::NiPoint3 fkH = tw2 + (Rw2 * st[bi]) * ws2;
                const RE::NiPoint3 fkW{ fkH.x * toGame2, fkH.y * toGame2, fkH.z * toGame2 };
                // ★ v11 THE POINT WE READ IS THE XP32 NODE (the user's model, PUSHTRIAD-verified 11:31): the
                // constraint pivot is seated on the node, node<->body read 0.12–1.2 u horizontally at rest, and
                // the old rigid-body-origin path needed a rest-latched ~8 u offset carried by the intent rotation —
                // which the probe caught injecting 6 u of phantom gap once her posture left the calibration pose
                // (corr 7.30 vs node 1.06 at rest, 11:31:55). The node IS the anatomy. No offset, no calibration.
                const float nx = t_conf.nodeW[bi][0], ny = t_conf.nodeW[bi][1], nz = t_conf.nodeW[bi][2];
                if (nx == 0.f && ny == 0.f && nz == 0.f) continue;             // node not read this prep
                const RE::NiPoint3 gap{ nx - fkW.x, ny - fkW.y, nz - fkW.z };
                bool playerNear = false;
                for (int k5 = 0; k5 < np; ++k5) {
                    const float ddx = pv[k5].p[0] - nx, ddy = pv[k5].p[1] - ny, ddz = pv[k5].p[2] - nz;
                    if (ddx * ddx + ddy * ddy + ddz * ddz < 20.f * 20.f) { playerNear = true; break; }
                }
                const std::uint64_t key = (std::uint64_t(t_conf.formId) << 8) | (unsigned)kSenseSlot[s5];
                auto& cal = s_senseCal[key];
                if (playerNear || reacting) cal.lastBusyS = nowSense;
                // ★ v11 THE BASELINE FREEZE (PUSHTRIAD 11:31:42: pub 0.54 u on a 20 u bend; 11:31:54: pub 10.0 u
                // with her standing at rest). The old rule froze on "a probe within 20 u of the BODY" — a hard
                // push drives the body AWAY from a stationary hand, the freeze released mid-bend, the EMA ate the
                // push as idle lag and then subtracted it as a phantom. A baseline that models idle lag may only
                // learn from something that LOOKS like idle lag: gap small, nobody near, nothing reacting, and a
                // hold-off after the last contact so the rebound is not absorbed either.
                const float gxyRaw = std::sqrt(gap.x * gap.x + gap.y * gap.y);
                const bool quiet = !playerNear && !reacting &&
                                   gxyRaw < ObjectHold::PushSenseBaseFreezeU() &&
                                   (nowSense - cal.lastBusyS) > ObjectHold::PushSenseBaseHoldS();
                if (quiet) {
                    if (!cal.haveBase) { cal.bx = gap.x; cal.by = gap.y; cal.bz = gap.z; cal.haveBase = true; }
                    else {
                        const float a = ObjectHold::PushStepBaseAlpha();
                        cal.bx += (gap.x - cal.bx) * a;
                        cal.by += (gap.y - cal.by) * a;
                        cal.bz += (gap.z - cal.bz) * a;
                    }
                }
                const float gx = gap.x - (cal.haveBase ? cal.bx : 0.f);
                const float gy = gap.y - (cal.haveBase ? cal.by : 0.f);
                const float gz = gap.z - (cal.haveBase ? cal.bz : 0.f);
                // ★ v11 Z (PUSHTRIAD 11:31:42: in the 90° bend the chest node went 20 u back and 35 u DOWN; the
                // horizontal-only magnitude discarded the larger half of the bend). The DIRECTION stays horizontal
                // — the walk is horizontal — the MAGNITUDE the bars see is 3-D. pushSenseZ 0 = the old XY magnitude.
                const float magXY = std::sqrt(gx * gx + gy * gy);
                const float mag   = ObjectHold::PushSenseZ() != 0.f ? std::sqrt(gx * gx + gy * gy + gz * gz) : magXY;
                if (t_conf.triad) {
                    const double tnow = (double)nowSense;
                    if (tnow - cal.triadS >= (playerNear ? 0.05 : 1.0)) {
                        cal.triadS = tnow;
                        const bool  bv = t_conf.senseBodyValid[s5];
                        const float bxw = t_conf.senseBody[s5][0], byw = t_conf.senseBody[s5][1];
                        const float bodyRaw  = bv ? std::sqrt((bxw - fkW.x) * (bxw - fkW.x) + (byw - fkW.y) * (byw - fkW.y)) : -1.f;
                        const float nodeBody = bv ? std::sqrt((nx - bxw) * (nx - bxw) + (ny - byw) * (ny - byw)) : -1.f;
                        // v11 13:15 (user: "the body didn't move 10u, it moved 2u"): ABSOLUTE node and intent, relative to
                        // her origin, so "the body was thrown" and "the intent moved under a lagging body" stop looking alike.
                        const float ox = t_conf.originW[0], oy = t_conf.originW[1];
                        logger::info("PUSHTRIAD {:08X} s{:<3} node<->intent xy {:6.2f} z {:+6.2f} | base xy {:5.2f} z {:+5.1f} | PUB {:6.2f} (xy {:5.2f}) | body<->intent raw {:6.2f} node<->body {:5.2f} | node-org ({:+6.1f},{:+6.1f}) intent-org ({:+6.1f},{:+6.1f}) | {}{}",
                                     t_conf.formId, kSenseSlot[s5], gxyRaw, gap.z,
                                     std::sqrt(cal.bx * cal.bx + cal.by * cal.by), cal.bz, reacting ? 0.f : mag, magXY,
                                     bodyRaw, nodeBody, nx - ox, ny - oy, fkW.x - ox, fkW.y - oy, playerNear ? "NEAR" : "idle",
                                     reacting ? " REACTING" : (quiet ? " learning" : ""));
                    }
                }
                std::scoped_lock lkg(s_senseGapMx);
                if (s_senseGap.size() > 512) s_senseGap.clear();
                {   // ★ v12: publish the node IN HER FRAME (origin-relative, de-yawed). Unconditional — the
                    // travel sensor has no reference to be contaminated, so it keeps measuring through a
                    // reaction; PushStep decides what a reaction means.
                    const float ox2 = t_conf.originW[0], oy2 = t_conf.originW[1], oz2 = t_conf.originW[2];
                    const float cy2 = std::cos(-t_conf.yaw), sy2 = std::sin(-t_conf.yaw);
                    const float rx = nx - ox2, ry = ny - oy2, rz = nz - oz2;
                    if (s_senseLocal.size() > 512) s_senseLocal.clear();
                    s_senseLocal[key] = { rx * cy2 - ry * sy2, rx * sy2 + ry * cy2, rz, nowSense };
                }
                // stand-down publishes ZERO (a stale large value would fire the tier the instant the window lapsed)
                // v13: the RAW gap always rides along — the travel sensor needs it even through a reaction
                // stand-down (it does its own zeroing, so a stale baseline cannot mislead it).
                if (reacting) s_senseGap[key] = { 0.f, 0.f, 0.f, nowSense, gap.x, gap.y, gap.z };
                else          s_senseGap[key] = { gx, gy, mag, nowSense, gap.x, gap.y, gap.z };
            }
            // ★ v29 FOOT TRIP CHANNEL: each foot NODE's height above the animation's foot this drive. Unconditional, like
            // the raw gap - PushStep zeroes it at contact and decides what it means. nodeW / st are this drive's data.
            for (int f9 = 0; f9 < 2; ++f9) {
                const int fb = t_conf.footBone[f9];
                if (fb < 0 || fb >= t_conf.n) continue;
                const float fnx = t_conf.nodeW[fb][0], fny = t_conf.nodeW[fb][1], fnz = t_conf.nodeW[fb][2];
                if (fnx == 0.f && fny == 0.f && fnz == 0.f) continue;          // node not read this prep
                const RE::NiPoint3 fkF = tw2 + (Rw2 * st[fb]) * ws2;
                const float animZ = fkF.z * toGame2;
                std::scoped_lock lkf(s_senseGapMx);
                if (s_footGap.size() > 256) s_footGap.clear();
                s_footGap[(std::uint64_t(t_conf.formId) << 1) | (unsigned)f9] = { fnz - animZ, fnz, animZ, nowSense };
            }
        }

        // Fold the (tiny) inner-hook cost into the same sum — one drive's conform = prep + this.
        // fires/max stay prep-owned (prep dominates), so mean stays honest and max stays comparable.
        s_confSumNs.fetch_add((std::uint64_t)((double)(ConfQpc() - q0) * ConfNsPerCount()),
                              std::memory_order_relaxed);
    }

    void GetPoseConformStats(std::uint64_t& fires, std::uint64_t& sumNs, std::uint64_t& maxNs)
    {
        fires = s_confFires.load(std::memory_order_relaxed);
        sumNs = s_confSumNs.load(std::memory_order_relaxed);
        maxNs = s_confMaxNs.load(std::memory_order_relaxed);
    }

    void ResetPoseConformStats()
    {
        s_confFires.store(0, std::memory_order_relaxed);
        s_confSumNs.store(0, std::memory_order_relaxed);
        s_confMaxNs.store(0, std::memory_order_relaxed);
    }

    // PUSH SENSE v5 accessor — consumed by PushStep's 0x5E0885 hook (cross-thread).
    void SenseStandDown(std::uint32_t actorFormId, float seconds)
    {
        // v11 (user, 2026-09-07: "is the stumble itself contaminating the data?" — measured yes: 14 u of
        // node<->intent with no input during a stagger we fired, re-triggering the tier the instant the
        // 1.5 s cooldown expired). PushStep stamps this when it queues ANY reaction; the sense path publishes
        // zero and learns nothing until it lapses. Later of the two if stamped twice.
        std::scoped_lock lk(s_senseGapMx);
        if (s_senseStandDown.size() > 128) s_senseStandDown.clear();
        const double until = (double)ArmNowSeconds() + (double)seconds;
        auto& v = s_senseStandDown[actorFormId];
        if (until > v) v = until;
    }
    bool GetPushSenseGap(std::uint32_t actorFormId, int slot, float minMagU,
                         float& dxOut, float& dyOut, float& magOut)
    {
        const std::uint64_t key = (std::uint64_t(actorFormId) << 8) | (unsigned)slot;
        std::scoped_lock lkg(s_senseGapMx);
        auto it = s_senseGap.find(key);
        if (it == s_senseGap.end()) return false;
        if (ArmNowSeconds() - it->second.tS > 0.25f) return false;   // stale (actor gone / stood down)
        if (it->second.mag < minMagU) return false;
        dxOut = it->second.dx; dyOut = it->second.dy; magOut = it->second.mag;
        return true;
    }

    // ★ v12 TRAVEL SENSOR (see the publish site): the sensed node's position in HER frame.
    bool GetPushNodeLocal(std::uint32_t actorFormId, int slot, float outLocal[3])
    {
        const std::uint64_t key = (std::uint64_t(actorFormId) << 8) | (unsigned)slot;
        std::scoped_lock lkg(s_senseGapMx);
        auto it = s_senseLocal.find(key);
        if (it == s_senseLocal.end()) return false;
        if (ArmNowSeconds() - it->second.tS > 0.25f) return false;   // stale (actor gone / not driven)
        outLocal[0] = it->second.x; outLocal[1] = it->second.y; outLocal[2] = it->second.z;
        return true;
    }

    // ★★ v13 (the user's spec): how far this joint sits from ITS ANIMATION POINT — the drive pose FK'd to
    // world, i.e. where the animation says the joint should be this frame. WORLD units, RAW (no baseline).
    // PushStep zeroes it at the moment of contact, so what it reports afterwards is purely what the player did.
    // ★★ 2026-09-12 THE FURNITURE VERDICT, public face (see PPBHook.h). Any thread: the sit state is a plain field read;
    // the lean answer is taken ONLY from the pre-drive cache (FurnitureTick), never computed here — no handle lookup, no 3D.
    std::uint8_t PushFurnitureVerdict(RE::Actor* actor)
    {
        if (!actor) return kFurnFree;
        int sit = 0;
        if (auto* st = actor->AsActorState()) sit = static_cast<int>(st->GetSitSleepState());
        if (!ObjectHold::PushStepFurnSitState())   // legacy A/B lever — same answer the tick gives
            return (sit != 0 || static_cast<bool>(actor->GetOccupiedFurniture())) ? kFurnBusy : kFurnFree;
        if (!FurnSitBusy(sit)) return kFurnFree;
        if (ObjectHold::PushStepFurnLean()) {
            std::scoped_lock lk(s_furnVerdictMx);
            if (auto it = s_furnVerdict.find(actor->GetFormID());
                it != s_furnVerdict.end() && it->second.verdict == kFurnLean && ArmNowSeconds() - it->second.tS <= 0.5f)
                return kFurnLean;   // a stale or missing classification fails CLOSED to BUSY
        }
        return kFurnBusy;
    }

    bool GetPushGapRaw(std::uint32_t actorFormId, int slot, float outRaw[3])
    {
        const std::uint64_t key = (std::uint64_t(actorFormId) << 8) | (unsigned)slot;
        std::scoped_lock lkg(s_senseGapMx);
        auto it = s_senseGap.find(key);
        if (it == s_senseGap.end()) return false;
        if (ArmNowSeconds() - it->second.tS > 0.25f) return false;   // stale (actor gone / not driven)
        outRaw[0] = it->second.rx; outRaw[1] = it->second.ry; outRaw[2] = it->second.rz;
        return true;
    }

    // ★ v29 FOOT TRIP CHANNEL (see the publish site): foot NODE Z minus the animation's foot Z, world game units.
    // foot 0 = R, 1 = L. false = not published (not a PPB skeleton, loosened, not driven) or stale (> 0.25 s).
    bool GetFootAnimGap(std::uint32_t actorFormId, int foot, float& gapZ)
    {
        if (foot < 0 || foot > 1) return false;
        std::scoped_lock lkg(s_senseGapMx);
        auto it = s_footGap.find((std::uint64_t(actorFormId) << 1) | (unsigned)foot);
        if (it == s_footGap.end()) return false;
        if (ArmNowSeconds() - it->second.tS > 0.25f) return false;
        gapZ = it->second.rz;
        return true;
    }

    void ClearPoseConformCache()
    {
        s_confCache.clear();
        { std::scoped_lock lkg(s_senseGapMx); s_footGap.clear(); }   // v29
        s_senseCal.clear();                       // v5: calibration lives and dies with the cache
        { std::scoped_lock lkg(s_senseGapMx); s_senseLocal.clear(); }   // v12
        { std::scoped_lock lkg(s_senseGapMx); s_senseGap.clear(); }
        s_confDumpArmed = false;
        s_confDumpDone.clear();
        s_furnProbe.clear();                      // 2026-09-12: FormIDs recycle across loads
        { std::scoped_lock lkv(s_furnVerdictMx); s_furnVerdict.clear(); }   // 2026-09-12 furniture verdict cache
        s_furnVerdictLogged.clear();
    }

    // The dump window's epoch for out-of-TU diagnostics (PivFix's CLAVDUMP): 0 = window closed,
    // else a counter that changes per poseConformDump edge — "log once per window" key.
    std::uint32_t PoseConformDumpEpoch()
    {
        return s_confDumpArmed ? s_confDumpEpoch : 0;
    }

    // ── STATUE MODE (A-pose spell v3, 2026-07-04) ────────────────────────────
    // Flagged actors get EVERY bone overwritten with the skeleton's reference
    // (bind) pose each frame — the same stance the engine shows when animations
    // fail to load, produced deliberately and reversibly. Straight elbows, flat
    // wrists, planted feet: the calibration statue the arm-IK A-pose couldn't be.
    static std::mutex                        s_statueMx;
    static std::unordered_set<std::uint32_t> s_statueSet;

    void SetStatuePose(RE::Actor* npc, bool on)
    {
        if (!npc) return;
        const std::uint32_t id = npc->GetFormID();
        std::lock_guard<std::mutex> g(s_statueMx);
        if (on) s_statueSet.insert(id); else s_statueSet.erase(id);
        logger::info("STATUE {} for {:08X} ({} actor(s) flagged)", on ? "ON" : "OFF", id, s_statueSet.size());
    }

    static bool IsStatuePose(std::uint32_t id)
    {
        std::lock_guard<std::mutex> g(s_statueMx);
        return s_statueSet.count(id) != 0;
    }

    // Console `statue` toggle: flip the flag for this actor, return the NEW state. Same effect the
    // A-pose spell drives (SetStatuePose) — reachable without the ESP: select the NPC + type `statue`.
    bool ToggleStatuePose(RE::Actor* npc)
    {
        if (!npc) return false;
        const std::uint32_t id = npc->GetFormID();
        std::lock_guard<std::mutex> g(s_statueMx);
        bool nowOn;
        if (s_statueSet.count(id)) { s_statueSet.erase(id); nowOn = false; }
        else                       { s_statueSet.insert(id); nowOn = true; }
        logger::info("STATUE {} for {:08X} ({} actor(s) flagged) [console toggle]",
                     nowOn ? "ON" : "OFF", id, s_statueSet.size());
        return nowOn;
    }

    void ClearStatueSet()
    {
        std::lock_guard<std::mutex> g(s_statueMx);
        s_statueSet.clear();
    }

    // Read-only statue-flag query for the `probe` console command.
    bool IsStatueFlagged(std::uint32_t id)
    {
        return IsStatuePose(id);
    }

    // ── RUNTIME BIND RELATIONS for the pivot editor (2026-07-04) ─────────────
    // Compose each bone's model-space BIND transform from the animation skeleton's referencePose
    // (the SAME data the statue drives — the true runtime bind; the NIF nodes disagree with it by
    // 2-6u, measured). Then form child->other relative rotation+translation per joint. These make
    // the pivot writer's partner targets pose-independent — the fix for the self-consistent
    // contamination trap (healing at the pose the bad data created just re-encoded the bad data).
    // Rotations compose as pure quats; scale folds into translations only (the hand bone is 0.85).
    static void ComputePivBindRels(RE::hkbRagdollDriver* driver, ObjectHold::PivBindRel* out)
    {
        for (int j = 0; j < 17; ++j) out[j].valid = false;
        auto*                  character = driver ? driver->character : nullptr;
        const RE::hkaSkeleton* skel = (character && character->setup) ?
            character->setup->animationSkeleton.get() : nullptr;
        if (!skel) return;
        const auto&         refP    = skel->referencePose;
        const std::int16_t* parents = skel->parentIndices.data();
        const int           nBones  = (int)refP.size();
        if (!parents || nBones <= 0) return;

        struct MT { RE::NiMatrix3 R; RE::NiPoint3 t; bool ok; };
        auto modelOf = [&](int bone) -> MT {
            MT m{ {}, { 0.f, 0.f, 0.f }, false };
            if (bone < 0 || bone >= nBones) return m;
            RE::NiMatrix3 R = MatrixFromQuat(refP[bone].rotation);
            RE::NiPoint3  t = GetTranslation(refP[bone]);
            for (int b = parents[bone]; b >= 0 && b < nBones; b = parents[b]) {
                const RE::NiMatrix3 Rp = MatrixFromQuat(refP[b].rotation);
                const RE::NiPoint3  tp = GetTranslation(refP[b]);
                alignas(16) float sp4[4]; _mm_store_ps(sp4, refP[b].scale.quad);
                const float sp = (sp4[0] > 0.01f) ? sp4[0] : 1.f;
                t = tp + (Rp * t) * sp;
                R = Mat3Mul(Rp, R);
            }
            m.R = R; m.t = t; m.ok = true;
            return m;
        };

        // 17 joints: 0..10 = right/center (as PivFixApply), 11..16 = left limbs (wristL/elbowL/
        // shoulderL/hipL/kneeL/ankleL) — the SAME table PivDescaleApply/JTRACK sweep, so PivScaleCorrect
        // can seat the left side pair-consistently (never lopsided).
        static constexpr const char* kChildBone[17] = {
            "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]",
            "NPC Spine [Spn0]",  "NPC Spine1 [Spn1]",    "NPC Spine2 [Spn2]",
            "NPC Neck [Neck]",   "NPC Head [Head]",
            "NPC R Thigh [RThg]", "NPC R Calf [RClf]",   "NPC R Foot [Rft ]",
            "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]",
            "NPC L Thigh [LThg]", "NPC L Calf [LClf]",   "NPC L Foot [Lft ]" };
        static constexpr const char* kOtherBone[17] = {
            "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Spine2 [Spn2]",
            "NPC COM [COM ]",    "NPC Spine [Spn0]",     "NPC Spine1 [Spn1]",
            "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
            "NPC COM [COM ]",    "NPC R Thigh [RThg]",   "NPC R Calf [RClf]",
            "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]", "NPC Spine2 [Spn2]",
            "NPC COM [COM ]",    "NPC L Thigh [LThg]",   "NPC L Calf [LClf]" };
        int childB[17], otherB[17];
        for (int j = 0; j < 17; ++j) {
            childB[j] = FindBoneIndex(skel, kChildBone[j]);
            otherB[j] = FindBoneIndex(skel, kOtherBone[j]);
        }
        for (int j = 0; j < 17; ++j) {
            const MT c = modelOf(childB[j]);
            const MT o = modelOf(otherB[j]);
            if (!c.ok || !o.ok) continue;
            // R_rel = R_oᵀ·R_c ; t_rel = R_oᵀ·(t_c − t_o)  (rotations orthonormal — scale never enters R)
            RE::NiMatrix3 RoT;
            for (int r = 0; r < 3; ++r)
                for (int cc = 0; cc < 3; ++cc) RoT.entry[r][cc] = o.R.entry[cc][r];
            const RE::NiMatrix3 Rrel = Mat3Mul(RoT, c.R);
            const RE::NiPoint3  d{ c.t.x - o.t.x, c.t.y - o.t.y, c.t.z - o.t.z };
            const RE::NiPoint3  trel = RoT * d;
            out[j].R[0] = Rrel.entry[0][0]; out[j].R[1] = Rrel.entry[0][1]; out[j].R[2] = Rrel.entry[0][2];
            out[j].R[3] = Rrel.entry[1][0]; out[j].R[4] = Rrel.entry[1][1]; out[j].R[5] = Rrel.entry[1][2];
            out[j].R[6] = Rrel.entry[2][0]; out[j].R[7] = Rrel.entry[2][1]; out[j].R[8] = Rrel.entry[2][2];
            out[j].t[0] = trel.x; out[j].t[1] = trel.y; out[j].t[2] = trel.z;
            out[j].valid = true;
        }
        int ok = 0; for (int j = 0; j < 17; ++j) ok += out[j].valid ? 1 : 0;
        logger::info("PivFix bind relations computed (runtime referencePose): {}/17 joints valid "
                     "(R/C W={} E={} S={} s0={} s1={} s2={} N={} H={} hip={} knee={} ankle={} | "
                     "L wrist={} elbow={} shoulder={} hip={} knee={} ankle={})",
                     ok, out[0].valid, out[1].valid, out[2].valid, out[3].valid, out[4].valid,
                     out[5].valid, out[6].valid, out[7].valid, out[8].valid, out[9].valid, out[10].valid,
                     out[11].valid, out[12].valid, out[13].valid, out[14].valid, out[15].valid, out[16].valid);
    }

    // Per-driver bind-relation cache. The partner-side targets come from the RUNTIME BIND relations
    // (referencePose math), computed once per driver and cached: pose-independent, so the 07-04
    // contamination trap can't recur. Also caches the clavicle-follow data (each upper arm's parent
    // bone name + its parent-local bind transform; L twin added 2026-07-10 — the left arm displaced
    // ~4u in protracted poses without it). File-scope so kPreLoadGame can drop it.
    struct PivCache {
        std::array<ObjectHold::PivBindRel, 17> rels{};   // 0..10 R/center (PivFixApply), 11..16 L limbs (PivScaleCorrect)
        std::string armParent;          // R upper arm bone's parent node (clavicle-chain carrier)
        float       refR[9] = { 1,0,0, 0,1,0, 0,0,1 };
        float       refT[3] = { 0, 0, 0 };
        bool        followValid = false;
        std::string armParentL;         // L twin — same carrier pattern, mirrored chain
        float       refRL[9] = { 1,0,0, 0,1,0, 0,0,1 };
        float       refTL[3] = { 0, 0, 0 };
        bool        followValidL = false;
        // Contact-curl bone indices (2026-07-16): the 12 R-hand finger segments on the
        // ANIMATION skeleton, [finger*3 + seg], finger = index/middle/ring/pinky, seg =
        // 0 MCP / 1 PIP / 2 DIP. -1 = unresolved (creature/custom skeleton) -> skipped.
        std::array<int, 12> fingerBone{ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
    };
    static std::unordered_map<const void*, PivCache> s_bindRelCache;

    void ClearBindRelCache()
    {
        s_bindRelCache.clear();
    }

    void ApplyToPoseTrack(RE::hkbRagdollDriver* driver, float deltaTime, void* generatorOutputRaw)
    {
        t_heelDriveBias = 0.f;   // FIRST: never let one actor's heel bias leak into another's drive
        t_conf.armed = false;    // same discipline for the pose-conform set (ApplyPoseConform also
        t_conf.dump  = false;    // disarms on consume — belt and suspenders across early returns)
        t_spineStage = "entry"; t_spineActor = 0;   // 2.2.0 breadcrumb
        if (!driver || !generatorOutputRaw) return;

        s_poseTrackFires.fetch_add(1, std::memory_order_relaxed);

        // One-shot log on the very first hook fire where we can actually
        // resolve an actor — proves the chain is wired end-to-end.
        auto* actor = GetActorFromDriver(driver);
        if (!actor) return;
        if (actor == RE::PlayerCharacter::GetSingleton()) return;  // VRIK owns player arms

        const std::uint32_t id = actor->GetFormID();

        // DISMEMBER GUARD (2026-07-26): enroll/maintain, then hard-skip every PPB per-actor
        // system for NGD head-clone actors and dismembered corpses. A clone never gets a rig
        // (the arms-off artifact), GHOST/NFING never latch onto a severed head, and PIVRESCALE
        // never fights a detached ragdoll. See DismemberGuard.h.
        t_spineActor = id;
        t_spineStage = "DismemberGuard::Tick";
        DismemberGuard::Tick(actor);
        t_spineStage = "GenitalProbe::Tick";
        GenitalProbe::Tick(actor);   // no-op unless genProbe 1
        t_spineStage = "DismemberGuard::IsExcluded / Orifice::ReleaseActor";
        if (DismemberGuard::IsExcluded(actor)) {
            // ORIFICE: this gate is a POINT OF NO RETURN — once excluded, Orifice::OnPreDrive
            // (below) is never called for this actor again, so an armed bone ring would stay
            // pushed open for the life of the 3D (kill + dismember mid-touch, an NGD head clone,
            // an FF-spawn enrollment race). Give the module its one last frame here, while her
            // nodes are still live. No-op for an actor it was never holding.
            Orifice::ReleaseActor(actor);
            return;
        }

        // PIVGUARD (2026-07-29 v2): per-actor PLANCK pivot-collapse scoping + stranded-pivot
        // self-heal. Sets the flag 0 for THIS PPB-skeleton actor's drive; Hooks.cpp restores it
        // right after the chain. See PivFix.cpp for the full design + the v1 postmortem.
        t_spineStage = "PivGuardOnPreDrive";
        ObjectHold::PivGuardOnPreDrive(actor, driver->ragdoll);

        // DIAGNOSTIC (2026-07-08, read-only): register the perf contact listener on this actor's Havok
        // world when `perf` is armed (idempotent per world, from the main thread outside the step), and
        // apply any pending `capdis` disableChild spike for this actor (double-gated by lodSpike + a
        // matching FormID). No-op cost when disarmed and no spike is queued.
        t_spineStage = "Diag::OnPreDrive";
        Diag::OnPreDrive(actor);

        // PERF SYSTEM (2026-07-09): per-actor child-LOD tick — evaluates the FULL/REDUCED/CORE tier and
        // applies staged enabledChildren diffs under the world lock. Inert until perfPreset > 0.
        t_spineStage = "PerfSys::OnPreDrive";
        PerfSys::OnPreDrive(actor);

        // FURNITURE VERDICT + PROBE (2026-09-12): the six-state rule + lean exception, classified and cached for the push
        // sensor (PoseConformPrepare below) and PushStep; PUSHFURN receipt on change; FURNPROBE line when furnProbe is on.
        // MUST run before PoseConformPrepare — the sensor arming reads this drive's verdict from the cache.
        t_spineStage = "FurnitureTick";
        FurnitureTick(actor);

        // HEEL FIX: runs for EVERY driven non-player actor — a heeled NPC's ragdoll should match her
        // visible body regardless of what any other mod is doing to her (PLANCK/HIGGS touch zones are
        // global). Toggle-gated.
        t_spineStage = "ApplyHeelFix";
        ApplyHeelFix(actor, driver, generatorOutputRaw);
        // XP32 POSE-CONFORM (2026-07-10): park this drive's XP32-derived ragdoll-local translations in
        // the TLS; Hooks::InnerDriveChainHook (0xA26C05) writes them into the drive pose (and emits the
        // poseConformDump comparison). Root-level heel bias + bone-level conform are independent — both
        // apply. Knob-gated inside (poseConform / poseConformDump both 0 -> cheap no-op).
        t_spineStage = "PoseConformPrepare";
        PoseConformPrepare(actor, driver, generatorOutputRaw);
        t_spineStage = "DivergenceProbe";
        DivergenceProbe(actor, driver);   // measure-only (doc 25 section 7.7)
        t_spineStage = "RagFrameDrive";
        RagFrameDrive(actor, driver, generatorOutputRaw);   // measure-only (Ragdoll Research 05)
        t_spineStage = "ReDrive";
        ReDrive(actor, driver, generatorOutputRaw);   // per-bone drive authority
        // CAP FIX: live hand-capsule calibration — applies once per `capfix` console generation per actor.
        // The 1 Hz file poll auto-arms a generation when Claude edits capHand* in the tuning file.
        t_spineStage = "CapFixPollFile";
        ObjectHold::CapFixPollFile();
        t_spineStage = "CapFixApply";
        GrabDiag::CapFixApply(actor);
        // FINGER CAPS: per-frame (NOT gen-gated) far-endpoint rewrite — the FingerX0->FingerX2 span
        // crosses the PIP joint, so it must follow the live pose. Graceful no-op until the NIF bake lands.
        t_spineStage = "FingerCapTrack";
        GrabDiag::FingerCapTrack(actor);
        // NPC FINGER TEST v2 (2026-07-09): runtime DYNAMIC finger capsules on the one selected/nearest
        // rig — creation, per-frame guards, and the velocity drive all live at this pre-drive seam
        // (fresh node transforms, live world in hand). Inert until npcFingerEnable/`nfing`.
        t_spineStage = "NpcFinger::OnPreDrive (rigs / garment / GEN)";
        NpcFinger::OnPreDrive(actor, deltaTime);
        // ORIFICE DRIVE (2026-08-19): sense the pelvic sensor ladder against the touch API's
        // probes, ease, and write the vaginal/anal bone rings (absolute, rest + offset). Placed
        // AFTER NpcFinger so the finger rig's own node work is already done, and after the
        // dismember/PivGuard gates above so an excluded actor never reaches it. Inert (and
        // self-restoring) until orificeEnable. NPC-only — the player is excluded above.
        t_spineStage = "Orifice::OnPreDrive";
        Orifice::OnPreDrive(actor, deltaTime);
        t_spineStage = "PivFix (bind-relation cache / PivFixApply / ReScale / clavicle follow / finger curl)";
        // PIV FIX: live joint-pivot re-seat (wrist/elbow/shoulder, R side) — same gen/poll loop.
        // The partner-side targets come from the RUNTIME BIND relations (referencePose math), computed
        // once per driver and cached: pose-independent, so the 07-04 contamination trap can't recur.
        {
            auto itBR = s_bindRelCache.find(driver);
            if (itBR == s_bindRelCache.end()) {
                PivCache pc{};
                ComputePivBindRels(driver, pc.rels.data());
                // Clavicle-follow data: each upper arm's PARENT bone name + its parent-local bind
                // transform (R = NPC R Clavicle [RClv] chain, L = NPC L Clavicle [LClv] chain).
                auto*                  characterC = driver->character;
                const RE::hkaSkeleton* skelC = (characterC && characterC->setup) ?
                    characterC->setup->animationSkeleton.get() : nullptr;
                if (skelC) {
                    struct FollowSide { const char* bone; std::string* parent; float* R; float* t; bool* valid; const char* tag; };
                    const FollowSide sides[2] = {
                        { "NPC R UpperArm [RUar]", &pc.armParent,  pc.refR,  pc.refT,  &pc.followValid,  "R" },
                        { "NPC L UpperArm [LUar]", &pc.armParentL, pc.refRL, pc.refTL, &pc.followValidL, "L" },
                    };
                    const auto* parents = skelC->parentIndices.data();
                    const int   nB = (int)skelC->referencePose.size();
                    for (const auto& s : sides) {
                        const int ua = FindBoneIndex(skelC, s.bone);
                        if (ua >= 0 && ua < nB && parents && parents[ua] >= 0) {
                            const int pIdx = parents[ua];
                            *s.parent = skelC->bones[pIdx].name.c_str();
                            const RE::NiMatrix3 rl = MatrixFromQuat(skelC->referencePose[ua].rotation);
                            // TRANSLATION from the LIVE NIF node local, NOT referencePose (2026-07-10
                            // pose-conform alignment): referencePose carries the ANIM-DATA bind lengths
                            // (~19-20u clavicle->upperarm) while the conform rests the bodies at the
                            // XP32 NIF's (~22.8u). The node's local.translate is the NIF's constant
                            // (animations write rotations only) — the same source the conform composes.
                            // Rotation stays referencePose (bind orientation, matches the follow math).
                            RE::NiPoint3 tl = GetTranslation(skelC->referencePose[ua]);
                            if (auto* root3d = actor->Get3D()) {
                                if (auto* uaNode = root3d->GetObjectByName(s.bone))
                                    tl = uaNode->local.translate;
                            }
                            for (int r = 0; r < 3; ++r)
                                for (int c = 0; c < 3; ++c) s.R[r*3 + c] = rl.entry[r][c];
                            s.t[0] = tl.x; s.t[1] = tl.y; s.t[2] = tl.z;
                            *s.valid = true;
                            logger::info("PivFollow cache ({}): upper arm parent bone = '{}' refT=[{:.2f} {:.2f} {:.2f}] (NIF-local source)",
                                         s.tag, *s.parent, tl.x, tl.y, tl.z);
                        }
                    }
                }
                // Contact-curl bone indices (2026-07-16): resolved once per driver alongside
                // the bind relations. Bracketed XP32 names are REQUIRED — unbracketed names
                // silently resolve -1 (the AIHands lesson). Missing bones stay -1 = skipped.
                if (skelC) {
                    static constexpr const char* kCurlBones[12] = {
                        "NPC R Finger10 [RF10]", "NPC R Finger11 [RF11]", "NPC R Finger12 [RF12]",
                        "NPC R Finger20 [RF20]", "NPC R Finger21 [RF21]", "NPC R Finger22 [RF22]",
                        "NPC R Finger30 [RF30]", "NPC R Finger31 [RF31]", "NPC R Finger32 [RF32]",
                        "NPC R Finger40 [RF40]", "NPC R Finger41 [RF41]", "NPC R Finger42 [RF42]",
                    };
                    for (int b = 0; b < 12; ++b)
                        pc.fingerBone[b] = FindBoneIndex(skelC, kCurlBones[b]);
                }
                itBR = s_bindRelCache.emplace(driver, std::move(pc)).first;
            }
            // PIVOT DESCALE (2026-07-09): SUPERSEDED 2026-07-14 by the uniform re-scale system and no
            // longer called. It drove EVERY non-scale-1 NPC's pivots to baked scale-1, which is WRONG for
            // a correctly-scaled NPC (it pushed her joints off her own nodes). PivScaleCorrect below must
            // see the RAW engine pivots (baked × GetScale) so its factor = trueScale/GetScale lands them
            // at baked × trueScale. (Function + pivDescale knob kept as inert reference.)
            ObjectHold::PivFixApply(actor, itBR->second.rels.data(), IsStatuePose(id));
            // UNIFORM RE-SCALE (2026-07-14, user's authoritative spec): a 3-point check (anchor=COM, head,
            // calf — Havok joint distance vs XP32 node distance, 0.5u trigger) finds the ~2-3% of NPCs
            // whose baked Havok body was scaled to the WRONG size (record scale != real XP32 scale), and
            // scales the WHOLE ragdoll by ONE factor (trueScale/GetScale) to match. A correctly-scaled NPC
            // PASSES and is left byte-for-byte untouched. Reads the raw engine pivots (descale retired).
            ObjectHold::PivScaleCorrect(actor);
            // Per-frame (NOT 1 Hz): the shoulder anchors must ride clavicle animation at animation rate.
            if (itBR->second.followValid)
                ObjectHold::PivFollowShoulder(actor, false, itBR->second.armParent.c_str(),
                                              itBR->second.refR, itBR->second.refT);
            if (itBR->second.followValidL)
                ObjectHold::PivFollowShoulder(actor, true, itBR->second.armParentL.c_str(),
                                              itBR->second.refRL, itBR->second.refTL);
            // JOINT-TRACK knob trigger (2026-07-10): `jtrackNow` 0 -> non-0 arms a ~2 s window in
            // which every driven actor dumps the read-only joint-vs-bone audit ONCE (the `jtrack`
            // console donors were all taken — the knob is the working trigger). Placed AFTER the
            // descale/seat/heal/follow writes above so the dump measures the state this frame
            // actually ships with.
            ObjectHold::PivJointTrackTick(actor);

            // ── CONTACT-DRIVEN FINGER CURL (2026-07-16, the "fingers conform to what they touch"
            // writer). NpcFinger's DriveRig charges per-finger curl factors from capsule
            // displacement (leaky integrator + EMA); here — with TRACK_POSE + poseLocal in hand,
            // the same dual-write contract as the statue — each charged finger's 3 segments get
            // a RotX flexion of kFistShare[seg] × curl (the AIHands FP_Fist proportions,
            // in-VR-validated). npcFingerCurlMode 0 = ADDITIVE on the frame's animation local
            // (contact DEFLECTS whatever pose the animation holds); 1 = BIND-RELATIVE (grasp
            // override, the AIHands wrap style). Rotations ONLY; runs before the statue block so
            // a statued NPC still reads pure bind (diagnostics win). Cost: 12 quat muls, only on
            // the ONE rigged actor, only while a finger is actually charged.
            {
                float curl[4];
                if (NpcFinger::GetFingerCurl(id, curl)) {
                    auto* genF = reinterpret_cast<Hkb::GeneratorOutput*>(generatorOutputRaw);
                    auto* phF  = Hkb::GetHeader(genF, Hkb::TRACK_POSE);
                    auto* characterF = driver->character;
                    const RE::hkaSkeleton* skelF = (characterF && characterF->setup) ?
                        characterF->setup->animationSkeleton.get() : nullptr;
                    if (phF && phF->numData > 0 && skelF) {
                        auto* poseF = reinterpret_cast<RE::hkQsTransform*>(Hkb::GetData(genF, phF));
                        auto* poseLocalF = reinterpret_cast<RE::hkQsTransform*>(
                            characterF ? characterF->poseLocal : nullptr);
                        const int  nPL      = characterF ? characterF->numPoseLocal : 0;
                        const bool bindRel  = ObjectHold::NpcFingerCurlMode() >= 0.5f;
                        static constexpr float kFistShare[3] = { 1.40f, 1.50f, 1.30f };  // rad, MCP/PIP/DIP
                        if (poseF) {
                            for (int f = 0; f < 4; ++f) {
                                if (curl[f] < 0.001f) continue;
                                for (int s = 0; s < 3; ++s) {
                                    const int bi = itBR->second.fingerBone[f * 3 + s];
                                    if (bi < 0 || bi >= (int)phF->numData) continue;
                                    const float ang = kFistShare[s] * curl[f];
                                    const RE::hkQuaternion base =
                                        (bindRel && bi < (int)skelF->referencePose.size())
                                            ? skelF->referencePose[bi].rotation
                                            : poseF[bi].rotation;
                                    const RE::hkQuaternion q = QuatMulRotX(base, ang);
                                    poseF[bi].rotation = q;
                                    if (poseLocalF && bi < nPL) poseLocalF[bi].rotation = q;
                                }
                            }
                        }
                    }
                }
            }
        }

        t_spineStage = "first-fire receipt / statue";
        if (!s_poseTrackFirstFire.exchange(true, std::memory_order_relaxed)) {
            auto* base = actor->GetActorBase();
            const char* nm = (base && base->GetFullName()) ? base->GetFullName() : "<unnamed>";
            logger::info(
                "FIRST-FIRE ApplyToPoseTrack — actor {:08X} \"{}\" resolved from driver 0x{:X}. "
                "Pose-track pipeline live.",
                actor->GetFormID(), nm, reinterpret_cast<std::uintptr_t>(driver));
        }

        // ── STATUE MODE ──────────────────────────────────────────────────────
        // Flagged actor: stomp EVERY bone with the skeleton's reference (bind)
        // pose — TRACK_POSE + poseLocal dual-write, same contract as all our pose
        // work. Heel fix and the CapFix/PivFix calibration loops above have
        // already run.
        if (IsStatuePose(id)) {
            auto* genS = reinterpret_cast<Hkb::GeneratorOutput*>(generatorOutputRaw);
            auto* phS  = Hkb::GetHeader(genS, Hkb::TRACK_POSE);
            auto* characterS = driver->character;
            const RE::hkaSkeleton* skelS = (characterS && characterS->setup) ?
                characterS->setup->animationSkeleton.get() : nullptr;
            if (phS && phS->numData > 0 && skelS) {
                auto* poseS = reinterpret_cast<RE::hkQsTransform*>(Hkb::GetData(genS, phS));
                if (poseS) {
                    const auto& refP = skelS->referencePose;
                    auto* poseLocalS = reinterpret_cast<RE::hkQsTransform*>(
                        characterS ? characterS->poseLocal : nullptr);
                    const int nPL = characterS ? characterS->numPoseLocal : 0;
                    const int n   = (std::min)((int)phS->numData, (int)refP.size());
                    // ROTATIONS ONLY (07-04 fix): stomping full transforms also stomped bind
                    // translations/scales, which disagree with scaled hands (0.85) — fingers sank
                    // into the palm. Rotations define the pose; translations stay animation-owned.
                    for (int i = 0; i < n; ++i) {
                        poseS[i].rotation = refP[i].rotation;
                        if (poseLocalS && i < nPL) poseLocalS[i].rotation = refP[i].rotation;
                    }
                    static std::atomic<bool> s_statueFirst{ false };
                    if (!s_statueFirst.exchange(true, std::memory_order_relaxed))
                        logger::info("STATUE first apply — actor {:08X}, {} bones -> bind pose (the deliberate 'animations-failed' statue).", id, n);
                }
            }
        }

        (void)deltaTime;
    }
}
