#include "PCH.h"
#include "PPBHook.h"
#include "Tuning.h"    // ObjectHold::HeelFixEnabled / CapFixPollFile
#include "CapFix.h"    // GrabDiag::CapFixApply
#include "PivFix.h"    // ObjectHold::PivBindRel / PivFixApply / PivFollowShoulder
#include "Diag.h"      // Diag::OnPreDrive — read-only contact-listener registration + gated capdis spike
#include "PerfSys.h"   // PerfSys::OnPreDrive — the child-LOD tier tick (inert until perfPreset > 0)
#include "NpcFingerTest.h"  // NpcFinger::OnPreDrive — the NPC finger test collider (inert until npcFingerEnable/`nfing`)
#include "DismemberGuard.h" // DF/NGD ↔ PLANCK guard: head-clone/dismembered-actor exclusion (2026-07-26)

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>   // std::sort — the head-trim median-of-10
#include <cstring>
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
            // indices 5..24 unused here
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
        if (g_heelsFixSpell)
            logger::info("Heels Fix found (HeelsFixSpell {:08X}) — PPB heel offset is slaved to it (no own height logic).",
                         g_heelsFixSpell->GetFormID());
        else
            logger::warn("Heels Fix (HeelsFix.esp) NOT installed — PPB heel offset DISABLED (no-op); heeled NPCs "
                         "keep barefoot-height Havok bodies. Heels Fix is a required dependency of PPB.");
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
        const bool spellNow = HeelsFixHeeled(actor);
        if (!HeeledSticky(actor)) return;              // Heels Fix is the sole heel authority — no own height guess
        // carrying a refresh flicker: spell off but she was heeled and the offset persists
        const float heelZ = npcNode->local.translate.z;        // amount = the lift Heels Fix put on the XP32 "NPC" node
        // ⚠ 2026-08-01: this receipt used to print ABOVE the heelZ read, so it asserted "the node
        // offset persists" without ever having looked at the offset — and printed that every 10 s
        // while heelZ was 0.0 and NO bias was armed. A log line must never claim a fact the code
        // has not yet checked. Report the measured value and say which of the two cases it is:
        //   offset alive -> we bridge the Heels Fix refresh flicker (the sticky gate working)
        //   offset gone  -> Heels Fix genuinely dropped it; PPB holds NO bias (no own height logic),
        //                   so a still-heeled NPC's collision sits ~heelZ BELOW her visual body.
        static float s_flickerLog = -10.f;
        if (!spellNow) {
            const float nowS = ArmNowSeconds();
            if (nowS - s_flickerLog > 10.f) {
                s_flickerLog = nowS;
                if (heelZ > 0.01f)
                    logger::info("HEELFIX {:08X}: Heels Fix ability is OFF (refresh window) but the node "
                                 "offset PERSISTS at {:.2f}u — bias held from the offset itself.",
                                 actor->GetFormID(), heelZ);
                else
                    logger::info("HEELFIX {:08X}: Heels Fix ability is OFF *and* the node offset is GONE "
                                 "({:.2f}u) — NO bias armed. If she is still wearing heels her collision "
                                 "is now sitting below her visual body; Heels Fix has not re-added.",
                                 actor->GetFormID(), heelZ);
            }
        }
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
        // ⚠ 2026-08-01: this said "same authority as the drive half — must stay balanced" and I
        // then converted ONLY the drive half to the sticky gate. During a Heels Fix refresh the
        // bias applied while this compensation did not, so the whole rendered body rose 8u and
        // floated (user-observed). The two halves are one mechanism: they MUST read the same
        // gate, always. Any future change to one is a change to both.
        if (!HeeledSticky(actor)) return;
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
            float           lastHeelZ = -999.f;   // heel-state edge (shoe change / NiOverride arming)
            float           lastHeelBias = -999.f; // hf-toggle edge (drive bias armed/disarmed)
            std::uint32_t   phase      = 0;   // per-actor fire counter for the EveryN stride
            std::uint32_t   winEpoch   = 0;   // 60 s summary: counted-this-window latch
        };
        // Drive path is main-thread (the ApplyHeelFix/s_bindRelCache precedent) — no lock.
        std::unordered_map<std::uint32_t, ConfCache> s_confCache;

        // Per-drive thread-local handoff to the inner hook (the heel-fix t_heelDriveBias pattern).
        struct ConfTls {
            bool          armed  = false;   // consumed + disarmed by ApplyPoseConform
            bool          write  = false;   // poseConform knob at prep time (dump can run write-free)
            bool          dump   = false;   // this drive logs the per-bone comparison
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
        // Called from ApplyToPoseTrack for every driven non-player actor; cheap no-op when both knobs
        // are off. QPC-timed; steady-state cost accumulates into the perf atomics + the 60 s summary.
        void PoseConformPrepare(RE::Actor* actor, RE::hkbRagdollDriver* driver, void* generatorOutputRaw)
        {
            const bool on      = ObjectHold::PoseConformEnabled();
            const bool dumpArm = ConfDumpTick();
            if (!on && !dumpArm) return;
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
                // 3D reload). Own-sex model path must be one of OURS ("\PPB\" folder).
                c.oursPPB = false;
                if (auto* base = actor->GetActorBase()) {
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
                                 "conform will never touch this actor", id);
                }
            }
            if (!c.oursPPB) return;   // the gate: males + unmapped races are structurally unreachable

            // FURNITURE GATE (2026-07-13): never capture/trim while the ragdoll is
            // PLANCK-loosened (sit/lean/sleep/ragdoll — the "green" state); the stand-up
            // edge resets BOTH the root capture and the head trim for a clean re-measure.
            const bool attached = ObjectHold::ActorRagdollAttached(actor);
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
                float heelNow = 0.f;                                    // 0 unless Heels Fix flags her (heel recognition is 100% from Heels Fix)
                if (HeeledSticky(actor))   // sticky: the refresh flicker must not fire the edge
                    if (auto* npcNode = root->GetObjectByName("NPC"))
                        heelNow = npcNode->local.translate.z;
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

            // Park the drive's conform set for the inner hook.
            t_conf.write  = on;
            t_conf.dump   = ConfDumpClaim(id);
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
            if (t_conf.dump) {
                if (const RE::hkaSkeleton* sk2 = driver->ragdoll->skeleton.get();
                    sk2 && sk2->parentIndices.data() && (int)sk2->bones.size() >= c.n) {
                    for (int i = 0; i < c.n; ++i) t_conf.par[i] = sk2->parentIndices.data()[i];
                    t_conf.haveFk = true;
                }
                for (int i = 0; i < c.n; ++i) {
                    const RE::NiPoint3 w = c.node[i] ? c.node[i]->world.translate : RE::NiPoint3{};
                    t_conf.nodeW[i][0] = w.x; t_conf.nodeW[i][1] = w.y; t_conf.nodeW[i][2] = w.z;
                }
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
            t_conf.armed = t_conf.write || t_conf.dump;

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

    void ClearPoseConformCache()
    {
        s_confCache.clear();
        s_confDumpArmed = false;
        s_confDumpDone.clear();
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
        DismemberGuard::Tick(actor);
        if (DismemberGuard::IsExcluded(actor)) return;

        // PIVGUARD (2026-07-29 v2): per-actor PLANCK pivot-collapse scoping + stranded-pivot
        // self-heal. Sets the flag 0 for THIS PPB-skeleton actor's drive; Hooks.cpp restores it
        // right after the chain. See PivFix.cpp for the full design + the v1 postmortem.
        ObjectHold::PivGuardOnPreDrive(actor, driver->ragdoll);

        // DIAGNOSTIC (2026-07-08, read-only): register the perf contact listener on this actor's Havok
        // world when `perf` is armed (idempotent per world, from the main thread outside the step), and
        // apply any pending `capdis` disableChild spike for this actor (double-gated by lodSpike + a
        // matching FormID). No-op cost when disarmed and no spike is queued.
        Diag::OnPreDrive(actor);

        // PERF SYSTEM (2026-07-09): per-actor child-LOD tick — evaluates the FULL/REDUCED/CORE tier and
        // applies staged enabledChildren diffs under the world lock. Inert until perfPreset > 0.
        PerfSys::OnPreDrive(actor);

        // HEEL FIX: runs for EVERY driven non-player actor — a heeled NPC's ragdoll should match her
        // visible body regardless of what any other mod is doing to her (PLANCK/HIGGS touch zones are
        // global). Toggle-gated.
        ApplyHeelFix(actor, driver, generatorOutputRaw);
        // XP32 POSE-CONFORM (2026-07-10): park this drive's XP32-derived ragdoll-local translations in
        // the TLS; Hooks::InnerDriveChainHook (0xA26C05) writes them into the drive pose (and emits the
        // poseConformDump comparison). Root-level heel bias + bone-level conform are independent — both
        // apply. Knob-gated inside (poseConform / poseConformDump both 0 -> cheap no-op).
        PoseConformPrepare(actor, driver, generatorOutputRaw);
        // CAP FIX: live hand-capsule calibration — applies once per `capfix` console generation per actor.
        // The 1 Hz file poll auto-arms a generation when Claude edits capHand* in the tuning file.
        ObjectHold::CapFixPollFile();
        GrabDiag::CapFixApply(actor);
        // FINGER CAPS: per-frame (NOT gen-gated) far-endpoint rewrite — the FingerX0->FingerX2 span
        // crosses the PIP joint, so it must follow the live pose. Graceful no-op until the NIF bake lands.
        GrabDiag::FingerCapTrack(actor);
        // NPC FINGER TEST v2 (2026-07-09): runtime DYNAMIC finger capsules on the one selected/nearest
        // rig — creation, per-frame guards, and the velocity drive all live at this pre-drive seam
        // (fresh node transforms, live world in hand). Inert until npcFingerEnable/`nfing`.
        NpcFinger::OnPreDrive(actor, deltaTime);
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
