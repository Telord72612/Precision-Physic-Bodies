#include "PCH.h"
#include "RagFrame.h"
#include "Tuning.h"
#include "PpbApi.h"           // CopyContacts — the self-sufficient subject fallback
#include "PpbTouchAPI.h"
#include "Interop.h"          // Interop::GetHiggs
#include "HiggsInterface.h"   // GetHandRigidBody / GetWeaponRigidBody — their speed IS the H2 reference

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>    // _mm_store_ps

namespace logger = SKSE::log;

// ============================================================================
//  RagFrame — read-only. RE:: types only (Probe.cpp's idiom); no Havok SDK TU,
//  so this file adds no link-stub surface and no CMake SDK scoping.
// ============================================================================
namespace {

    constexpr float kHavokToSkyrim = 1.0f / 0.0142875f;
    constexpr int   kPre   = 10;      // frames kept BEFORE the event
    constexpr int   kBodies = 18;     // the ragdoll set (the CharacterBumper is not one)

    // The 18 ragdoll bodies in Probe.cpp's order. Short tags keep one BODIES line
    // readable: a 18-body line with full node names is unusable in a log.
    constexpr const char* kNodes[kBodies] = {
        "NPC R Hand [RHnd]", "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]", "NPC Head [Head]",
        "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
        "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC COM [COM ]",
        "NPC L Hand [LHnd]", "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]",
        "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]",
    };
    constexpr const char* kTags[kBodies] = {
        "rHnd", "rLar", "rUar", "head", "spn0", "spn1", "spn2", "neck",
        "rThg", "rClf", "rFt", "com", "lHnd", "lLar", "lUar", "lThg", "lClf", "lFt",
    };

    const char* KnockStateName(int k) {
        switch (k) {
        case 0: return "Normal";  case 1: return "Explode"; case 2: return "ExplodeLeadIn";
        case 3: return "Out";     case 4: return "OutLeadIn"; case 5: return "Queued";
        case 6: return "GetUp";   case 7: return "Down";    case 8: return "WaitForTaskQueue";
        default: return "?";
        }
    }

    struct BodySample {
        std::uint8_t mt   = 0;        // hkpMotion::MotionType (1 dynamic .. 4 keyframed, 5 fixed)
        std::uint8_t layer = 0;       // filterInfo & 0x7F — 8 Biped / 32 DeadBip / 33 BipedNoCC
        float        v    = -1.f;     // |linear velocity|, GAME units/s. -1 = body unreadable
        float        w    = -1.f;     // |angular velocity|, rad/s
        // ⚠ "layer 8 = no floor" is only HALF the question (07 section 2.4): a body that is
        // collision-OFF or not in a world at all cannot touch anything either. All three are
        // needed together before "can she hit the ground yet" can be answered.
        bool         noCol = false;   // filter bit 14 — collision disabled
        bool         inWorld = false; // the body is actually in a Havok world
        std::uint16_t group = 0;      // filter >> 16 — a re-layer can carry a group change too
        // ⚠ MAGNITUDES ALONE CANNOT ANSWER THE QUESTION. The stage-1 pass criterion is "the
        // pelvis DESCENDING from F1", and H10's signature is motion "aligned with the push" —
        // both need a sign. |v| can never express either. z + vz are the cheapest pair that can.
        float        z    = 0.f;      // world height, game units
        float        vz   = 0.f;      // vertical velocity, game u/s (negative = falling)
        // H10 names "gravity 0 in-step" as one of its three terms: PLANCK zeroes gravityFactor
        // on all 18 bodies before every driven step and restores 1 after. Without this the
        // receipt cannot see the term it exists to test.
        float        grav = -1.f;
    };

    struct Sample {
        bool   used = false;
        // ⛔ WHOSE frame this is. The ring is flushed at F0 and every entry used to be printed
        // under the ARMED actor's FormID — but the subject can change between samples (the
        // player touches someone else), so the pre-event window could silently attribute
        // another actor's bodies to the one being knocked down. That is the receipt lying about
        // the only thing it exists to measure.
        std::uint32_t actorId = 0;
        int    rel  = 0;              // frame index relative to F0
        double tMs  = 0.0;            // ms since F0 (negative before it)
        // ⚠ stamped on EVERY sample, including the ring's pre-event frames, so the pre/post
        // timeline is ONE clock (07 section 2.7). The old nominal rel x 14.7 ms assumed a
        // 68 fps headset; a session at another rate would have mis-dated everything before F0,
        // and F0-relative timing is the whole point of the receipt.
        std::chrono::steady_clock::time_point stamp{};
        // actor state
        int    knock = -1, life = -1;
        bool   rag = false, animDrivenGraph = false;
        bool   ccPresent = false;
        bool   ccInWorld = false;     // the controller left the world on RemoveCharacterControllerFromWorld
        std::uint32_t ccFlags = 0;    // kFollowRagdoll 1<<12 / kNotPushable 1<<14 / kNoSim 1<<17
        int    ccState = -1;
        float  ccFallTime = -1.f;
        int    steps = -1;            // physics steps this frame (HIGGS runs 2)
        // drive state (from the pre-drive hook, this frame)
        bool   driveSeen = false;
        bool   rbOn = false, pwOn = false, planckBug = false, allKf = false;
        float  rbFrac = -1.f, pwMaxForce = -1.f;
        int    wfmMode = -1;
        std::uint32_t rwk = 0;
        // physics
        BodySample body[kBodies];
        float  handV[2] = { -1.f, -1.f };     // HIGGS hand bodies, game u/s
        float  handW[2] = { -1.f, -1.f };     // ...and their spin: H2's blade term is w x r
        float  wpnV[2]  = { -1.f, -1.f };     // HIGGS weapon bodies
        float  wpnW[2]  = { -1.f, -1.f };
        bool   wpnOff[2] = { false, false };  // IsWeaponCollisionDisabled — a disabled blade
                                              // cannot be the pusher, which rules H2 out for it
        float  playerSpeed = -1.f;            // the player's own locomotion, game u/s
        // deepest hand/weapon contact on this actor. 4 Hz and dwell-filtered, hence "snap".
        float  deepU = 1e9f;
        int    deepSlot = -1, deepChild = -1;
        int    deepKind = -1;                 // WHICH source was deepest (hand/weapon/head)
        bool   deepEng = false;
        // ⚠ the PREVIOUS frame's count. NoteSettle runs later inside the same PushStep::OnFrame
        // than RagFrame::OnFrame does, so the value this sample reads was produced one frame
        // ago. Labelled rather than silently mis-dated — a receipt that dates its own evidence
        // wrongly is worse than one that admits the offset.
        int    settleCapped = -1;
    };

    // ── ring + arm state (main thread only) ─────────────────────────────────
    Sample        g_ring[kPre];
    int           g_ringN   = 0;              // total pushes (index = g_ringN % kPre)
    std::uint32_t g_subject = 0;              // whose frames we are sampling
    std::uint32_t g_armed   = 0;              // 0 = idle; else the armed actor
    int           g_postLeft = 0;
    std::chrono::steady_clock::time_point g_t0{};
    // NoteDrive lands from the pre-drive hook at a different point in the frame than
    // OnFrame, so it parks here and the next OnFrame folds it into that frame's sample.
    struct PendingDrive {
        std::uint32_t id = 0;
        bool  seen = false, rbOn = false, pwOn = false, planckBug = false;
        float rbFrac = -1.f, pwMaxForce = -1.f;
        int   wfmMode = -1;
        bool  rag = false, allKf = false;
        std::uint32_t rwk = 0;
    };
    PendingDrive g_pend;
    // cheapest possible idle cost for the pre-drive hook, which runs per actor per frame
    std::atomic<bool> g_wantDrive{ false };
    // HIGGS steps physics TWICE per frame, so a launch delivered in one substep is invisible to
    // a once-per-frame sample unless the count is known (07 section 2.4). Bumped from the
    // 0xDFB722 step chain hook; exchanged to 0 each frame.
    std::atomic<int>  g_stepsThisFrame{ 0 };
    // ⛔ THE HIT SINK IS NOT MAIN-THREAD-ONLY. PLANCK's contactPointCallback -> DoHit takes an
    // ELSE branch (planck_080 main.cpp:1880) that calls DispatchHitEvents INLINE on the Havok
    // collision thread whenever the actor path is refused (ghost / !CanHit) — and the installed
    // activeragdoll.ini has hitAnyMoveableObjects = 1, so a ragdoll body qualifies. Float work
    // or spdlog on that thread is the documented CTD class (T3/T4; HandBox.cpp: "never log here
    // — collision-thread callback, fmt SIMD = CTD"). So the sink does INTEGER work only, parks
    // the hit, and OnFrame emits it on the main thread next tick.
    std::uint32_t     g_mainThreadId = 0;
    std::atomic<bool> g_ragOnAtomic{ false };   // Enabled() as an integer read for the sink
    struct PendingHit {
        std::atomic<bool> pending{ false };
        std::uint32_t id = 0;
        bool  planck = false, power = false, isLeft = false;
        float velMag = -1.f;
    };
    PendingHit g_hit;
    int               g_settleCapped = -1;   // set by NoteSettle, consumed by the next sample

    bool         g_flushPending = false;  // Arm latched; OnFrame owes the ring flush
    bool         s_prevKnocked = false;   // knock-state edge for the self-arm
    std::chrono::steady_clock::time_point g_prevSampleStamp{};
    RE::NiPoint3 g_prevPlayerPos{};
    bool         g_prevPlayerPosValid = false;

    bool Enabled() { return ObjectHold::RagFrameOn(); }

    // Per-body read — Probe.cpp's exact idiom, extended with the collision LAYER
    // (the DeadBip re-layer is what dates when a floor and walls start existing for
    // her: under PLANCK the L_BIPED row is zeroed, so a body still on layer 8 cannot
    // touch the ground at all) and angular speed (H3's signature is violent spin).
    void SampleBodies(RE::Actor* actor, Sample& s)
    {
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return;
        // ⚠ TAKE THE WORLD READ LOCK. The Havok step runs on another thread, and an unlocked
        // 128-bit velocity read can tear — which in this receipt would MANUFACTURE the exact
        // one-frame |v| spike that is H3's signature. The instrument must not be able to invent
        // its own answer. (PPB's precedent is split — Probe reads unlocked from the console —
        // but that is a user-triggered dump, not a per-frame sample feeding a verdict.) A READ
        // lock, never write: this function writes nothing.
        auto* cell   = actor->GetParentCell();
        auto* bworld = cell ? cell->GetbhkWorld() : nullptr;
        RE::BSReadWriteLock* wl = bworld ? std::addressof(bworld->worldLock) : nullptr;
        if (wl) wl->LockForRead();
        struct Unlock {
            RE::BSReadWriteLock* l;
            ~Unlock() { if (l) l->UnlockForRead(); }
        } unlock{ wl };
        // intern once: this runs EVERY frame while armed, and Probe.cpp's per-call construction
        // is fine for a one-shot console dump but not for a per-frame sampler.
        static RE::BSFixedString s_names[kBodies];
        static bool s_interned = false;
        if (!s_interned) { for (int i = 0; i < kBodies; ++i) s_names[i] = kNodes[i]; s_interned = true; }
        for (int i = 0; i < kBodies; ++i) {
            auto* obj = root->GetObjectByName(s_names[i]);
            if (!obj) continue;
            auto* colObj = obj->collisionObject.get();
            auto* body   = colObj ? static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody() : nullptr;
            auto* hkp    = body ? body->GetRigidBody() : nullptr;
            if (!hkp) continue;
            alignas(16) float lv[4], av[4];
            _mm_store_ps(lv, hkp->motion.linearVelocity.quad);
            _mm_store_ps(av, hkp->motion.angularVelocity.quad);
            BodySample& b = s.body[i];
            b.mt    = static_cast<std::uint8_t>(hkp->motion.type.get());
            // RE::CFilter, not a raw word — `.filter` is the 32-bit value; low 7 bits = layer.
            const std::uint32_t fw = hkp->collidable.broadPhaseHandle.collisionFilterInfo.filter;
            b.layer   = static_cast<std::uint8_t>(fw & 0x7Fu);
            b.noCol   = (fw & 0x4000u) != 0;
            b.group   = static_cast<std::uint16_t>(fw >> 16);
            b.inWorld = body->GetWorld1() != nullptr;
            b.v = std::sqrt(lv[0]*lv[0] + lv[1]*lv[1] + lv[2]*lv[2]) * kHavokToSkyrim;
            b.w = std::sqrt(av[0]*av[0] + av[1]*av[1] + av[2]*av[2]);
            b.vz = lv[2] * kHavokToSkyrim;
            alignas(16) float ps[4];
            _mm_store_ps(ps, hkp->motion.motionState.transform.translation.quad);
            b.z    = ps[2] * kHavokToSkyrim;
            b.grav = hkp->motion.gravityFactor;
        }
    }

    // The player's own bodies. Never logged before, and H2 cannot be judged without
    // them: "bone |v| tracks the hand body's |v|" is the whole signature.
    void SamplePlayer(Sample& s)
    {
        if (auto* h = Interop::GetHiggs()) {
            for (int i = 0; i < 2; ++i) {
                const bool isLeft = (i == 1);
                if (auto* nb = h->GetHandRigidBody(isLeft)) {
                    if (auto* hkp = static_cast<RE::bhkRigidBody*>(nb)->GetRigidBody()) {
                        alignas(16) float lv[4], av[4];
                        _mm_store_ps(lv, hkp->motion.linearVelocity.quad);
                        _mm_store_ps(av, hkp->motion.angularVelocity.quad);
                        s.handV[i] = std::sqrt(lv[0]*lv[0] + lv[1]*lv[1] + lv[2]*lv[2]) * kHavokToSkyrim;
                        s.handW[i] = std::sqrt(av[0]*av[0] + av[1]*av[1] + av[2]*av[2]);
                    }
                }
                if (auto* nb = h->GetWeaponRigidBody(isLeft)) {
                    if (auto* hkp = static_cast<RE::bhkRigidBody*>(nb)->GetRigidBody()) {
                        alignas(16) float lv[4], av[4];
                        _mm_store_ps(lv, hkp->motion.linearVelocity.quad);
                        _mm_store_ps(av, hkp->motion.angularVelocity.quad);
                        s.wpnV[i] = std::sqrt(lv[0]*lv[0] + lv[1]*lv[1] + lv[2]*lv[2]) * kHavokToSkyrim;
                        s.wpnW[i] = std::sqrt(av[0]*av[0] + av[1]*av[1] + av[2]*av[2]);
                    }
                }
            }
        }
        // ⚠ the weapon body being collision-DISABLED is load-bearing: PLANCK turns it off while
        // sheathed and for 0.5 s after every hit, and a disabled blade cannot have pushed her.
        if (auto* h = Interop::GetHiggs())
            for (int i = 0; i < 2; ++i) s.wpnOff[i] = h->IsWeaponCollisionDisabled(i == 1);
        // locomotion: HIGGS bakes the room delta into its hand keyframe velocity on
        // non-warp frames, so "was the player moving" is load-bearing for reading handV.
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            const RE::NiPoint3 cur = p->GetPosition();
            if (g_prevPlayerPosValid) {
                const float dx = cur.x - g_prevPlayerPos.x, dy = cur.y - g_prevPlayerPos.y,
                            dz = cur.z - g_prevPlayerPos.z;
                // real dt, not a hardcoded 60 Hz — every other velocity on this line is a
                // measured u/s and a fabricated one beside them is worse than none.
                const double dtMs = (g_prevSampleStamp.time_since_epoch().count() != 0)
                    ? std::chrono::duration<double, std::milli>(s.stamp - g_prevSampleStamp).count()
                    : 0.0;
                s.playerSpeed = (dtMs > 0.5)
                    ? (float)(std::sqrt(dx*dx + dy*dy + dz*dz) * 1000.0 / dtMs) : -1.f;
            }
            g_prevPlayerPos = cur;
            g_prevPlayerPosValid = true;
            g_prevSampleStamp = s.stamp;
        }
    }

    void Emit(const Sample& s, std::uint32_t id)
    {
        if (!s.used) return;
        logger::info("RAGFRAME {:08X} f={:+d} t={:+.1f}ms knock={}({}) life={} rag={} animDrv={} "
                     "steps={} cc={}/world={} ccState={} ccFlags=0x{:08X}[{}{}{}] fallT={:.2f} "
                     "settleCappedPrev={}",
                     id, s.rel, s.tMs, s.knock, KnockStateName(s.knock), s.life,
                     s.rag ? 1 : 0, s.animDrivenGraph ? 1 : 0, s.steps,
                     s.ccPresent ? "SET" : "NONE", s.ccInWorld ? 1 : 0, s.ccState, s.ccFlags,
                     (s.ccFlags & (1u << 12)) ? "FollowRagdoll " : "",
                     (s.ccFlags & (1u << 14)) ? "NotPushable " : "",
                     (s.ccFlags & (1u << 17)) ? "NoSim" : "",
                     s.ccFallTime, s.settleCapped);
        if (s.driveSeen)
            logger::info("RAGFRAME {:08X} f={:+d} DRIVE rbOn={} rbFrac={:.3f} pwOn={} pwMaxForce={:.1f} "
                         "wfmMode={} allKf={} rwk=0x{:08X} planckBug={}",
                         id, s.rel, s.rbOn ? 1 : 0, s.rbFrac, s.pwOn ? 1 : 0, s.pwMaxForce,
                         s.wfmMode, s.allKf ? 1 : 0, s.rwk, s.planckBug ? 1 : 0);
        else
            logger::info("RAGFRAME {:08X} f={:+d} DRIVE (not driven this frame)", id, s.rel);
        char buf[512]; std::size_t len = 0;
        for (int i = 0; i < kBodies; ++i) {
            const BodySample& b = s.body[i];
            if (b.v < 0.f) continue;
            const int wrote = std::snprintf(buf + len, sizeof buf - len,
                                            "%s%s:%u/%u%s%s/%.0f/%+.0f/%.1f/z%.0f/g%.1f",
                                            len ? " " : "",
                                            kTags[i], b.mt, b.layer,
                                            b.noCol ? "N" : "", b.inWorld ? "" : "!",
                                            b.v, b.vz, b.w, b.z, b.grav);
            if (wrote <= 0 || len + wrote >= sizeof buf) break;
            len += wrote;
        }
        // tag:mt/layer[N=noCollision][!=not in a world]/|v|/|w| — all three are needed before
        // "can this body touch the floor" has an answer (07 section 2.4).
        // group: normally uniform across the 18 bodies — print it once, and flag a body that
        // differs (a re-layer can carry a group change with it).
        std::uint16_t g0 = 0; bool gMixed = false;
        for (const BodySample& b : s.body)
            if (b.v >= 0.f) { if (!g0) g0 = b.group; else if (b.group != g0) gMixed = true; }
        char deep[96] = "none";
        if (s.deepU < 1e8f)
            std::snprintf(deep, sizeof deep, "s%d.C%d d=%.2fu kind=%d src=%s(snap)",
                          s.deepSlot, s.deepChild, s.deepU, s.deepKind,
                          s.deepEng ? "ENG" : "GEO");
        logger::info("RAGFRAME {:08X} f={:+d} BODIES(tag:mt/layer/|v|/vz/|w|/z/grav) {} | "
                     "hand R:{:.0f}/{:.1f} L:{:.0f}/{:.1f} | wpn R:{:.0f}/{:.1f}{} "
                     "L:{:.0f}/{:.1f}{} | playerSpd={:.0f} | grp=0x{:04X}{} | deep={}",
                     id, s.rel, len ? buf : "(none readable)",
                     s.handV[0], s.handW[0], s.handV[1], s.handW[1],
                     s.wpnV[0], s.wpnW[0], s.wpnOff[0] ? "[colDis]" : "",
                     s.wpnV[1], s.wpnW[1], s.wpnOff[1] ? "[colDis]" : "",
                     s.playerSpeed, g0, gMixed ? "(MIXED)" : "", deep);
    }
}

namespace RagFrame {

    // defined below; OnFrame drains the deferred ring flush (see Arm)
    void FlushRing(std::uint32_t actorId);

    void NoteSubject(std::uint32_t actorId) { if (actorId) g_subject = actorId; }

    void NoteDrive(std::uint32_t actorId, bool rbOn, float rbFrac, bool pwOn,
                   float pwMaxForce, int wfmMode, bool ragFlag, bool planckBug,
                   std::uint32_t rwk, bool allKf)
    {
        if (!g_wantDrive.load(std::memory_order_relaxed)) return;
        if (!actorId || (actorId != g_subject && actorId != g_armed)) return;
        g_pend.id = actorId; g_pend.seen = true;
        g_pend.rbOn = rbOn; g_pend.rbFrac = rbFrac;
        g_pend.pwOn = pwOn; g_pend.pwMaxForce = pwMaxForce;
        g_pend.wfmMode = wfmMode; g_pend.rag = ragFlag; g_pend.planckBug = planckBug;
        g_pend.rwk = rwk; g_pend.allKf = allKf;
    }

    void OnFrame()
    {
        const bool on = Enabled();
        g_wantDrive.store(on, std::memory_order_relaxed);
        g_ragOnAtomic.store(on, std::memory_order_relaxed);
        // drain a hit the sink parked from a collision thread (integer-only over there)
        if (g_hit.pending.load(std::memory_order_acquire)) {
            logger::info("RAGFRAME {:08X} HIT planck={} power={} vel={:.1f}u/s hand={} armed={} "
                         "postLeft={}{}",
                         g_hit.id, g_hit.planck ? 1 : 0, g_hit.power ? 1 : 0, g_hit.velMag,
                         g_hit.isLeft ? "L" : "R", g_armed ? 1 : 0, g_postLeft,
                         g_hit.planck ? "  <-- FULL-STRENGTH while the engine flag is still false"
                                      : "  (not a PLANCK hit)");
            g_hit.pending.store(false, std::memory_order_release);
        }
        if (!on) { g_armed = 0; g_postLeft = 0; return; }
        // Self-sufficient subject: PushStep names its last-touched actor, but the receipt must
        // also work with `pushStep 0` (the obvious way to isolate a knockdown from PPB's own
        // walk). Fall back to whoever the player is currently touching — the digest snapshot is
        // already assembled this frame, so this costs one copy and no scanning.
        if (!g_armed && !g_subject) {
            PPBAPI::PpbTouchContact c[8];
            const int n = PpbApi::CopyContacts(c, 8);
            if (n > 0) g_subject = c[0].actorFormId;
        }
        // ⚠ DRAIN THE STEP COUNTER UNCONDITIONALLY. It is incremented from the physics hook on
        // every frame, but used to be exchanged only on the fully-successful sampling path — so
        // the first sample after any gap (no subject, actor unresolvable) reported the ACCUMULATED
        // count of every skipped frame as if it were this frame's substeps.
        const int stepsThisFrame = g_stepsThisFrame.exchange(0, std::memory_order_relaxed);
        const std::uint32_t id = g_armed ? g_armed : g_subject;
        if (!id) return;
        auto* form = RE::TESForm::LookupByID(id);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) return;

        Sample s{};
        s.used    = true;
        s.actorId = id;
        s.stamp   = std::chrono::steady_clock::now();
        s.knock = static_cast<int>(actor->GetKnockState());
        s.life  = static_cast<int>(actor->GetLifeState());
        s.rag   = actor->IsInRagdollState();
        actor->GetGraphVariableBool(RE::BSFixedString("bAnimationDriven"), s.animDrivenGraph);
        // ── THE CONTROLLER'S FATE (06 section 3 Q10). FullyRagdoll's enter sends event 814
        // RemoveCharacterControllerFromWorld, and the get-up clip sends 811 to put it back — so
        // "present" is not the question, "in the Havok world" is. It also decides whether the
        // capsule can still drag her, which is exactly how the walk+ragdoll warp bug happened.
        if (auto* cc = actor->GetCharController()) {
            s.ccPresent  = true;
            s.ccInWorld  = cc->GetHavokWorld() != nullptr;
            s.ccFlags    = cc->flags.underlying();
            s.ccState    = static_cast<int>(cc->context.currentState);
            s.ccFallTime = cc->fallTime;
        }
        s.steps = stepsThisFrame;
        s.settleCapped = g_settleCapped; g_settleCapped = -1;
        // ── DEEPEST HAND/WEAPON CONTACT. H1 and H2 both need "was anything inside her, and how
        // deep". Tagged `snap` on the line because the touch scan runs at apiHz (4/s) and is
        // dwell-filtered — it is the freshest honest answer, not a per-frame measurement.
        {
            PPBAPI::PpbTouchContact c[32];
            const int n = PpbApi::CopyContacts(c, 32);
            for (int i = 0; i < n; ++i) {
                if (c[i].actorFormId != id) continue;
                // ⚠ INCLUDE THE HEAD. It ships as a push source in this same build, so a
                // head-caused knockdown must not produce a confident "deep=none" — that is
                // failure class 1 (a diagnostic's own filter turning a positive into a
                // confident null) in the receipt written to avoid exactly that.
                const bool pusher = c[i].sourceKind <= 3 ||
                                    c[i].sourceKind == PPBAPI::kSourceWeapon ||
                                    c[i].sourceKind == PPBAPI::kSourceHead;
                if (!pusher) continue;
                if (c[i].distU < s.deepU) {
                    s.deepU = c[i].distU; s.deepSlot = c[i].slot; s.deepChild = c[i].child;
                    s.deepEng = c[i].engineContact != 0;
                    s.deepKind = c[i].sourceKind;
                }
            }
        }
        if (g_pend.seen && g_pend.id == id) {
            s.driveSeen = true;
            s.rbOn = g_pend.rbOn; s.rbFrac = g_pend.rbFrac;
            s.pwOn = g_pend.pwOn; s.pwMaxForce = g_pend.pwMaxForce;
            s.wfmMode = g_pend.wfmMode; s.planckBug = g_pend.planckBug;
            s.rwk = g_pend.rwk; s.allKf = g_pend.allKf;
        }
        g_pend.seen = false;
        SampleBodies(actor, s);
        SamplePlayer(s);

        // ⛔ THE INSTRUMENT MUST BE ABLE TO FIRE WITHOUT PUSHSTEP. RagFrame::Arm has exactly one
        // caller — PushStep's reaction tier — and that whole path is unreachable while
        // `pushStep 0`, because the WalkState map it reads is only populated by PushStep::OnFrame
        // (which early-returns on the master knob). So "ragFrame 1 + pushStep 0", the documented
        // way to isolate a knockdown from PPB's own walk, produced a live sampler that could
        // never flush: total silence from an armed instrument. Watch the subject's own knock
        // state for a rise and arm on it. The ring is already fresh at that point.
        if (!g_armed && !s_prevKnocked && (s.rag || s.knock == 1 || s.knock == 2)) {
            logger::info("RAGFRAME {:08X} self-armed on an OBSERVED knockdown (knock={} rag={}) "
                         "— no PushStep trigger; mmag/slot unknown",
                         id, s.knock, s.rag ? 1 : 0);
            Arm(id, -1.f, -1, "observed", 0.f, true);
        }
        s_prevKnocked = (s.rag || s.knock == 1 || s.knock == 2);

        if (g_armed && g_flushPending) {      // pay the deferred flush on the main thread
            g_flushPending = false;
            FlushRing(g_armed);
        }
        if (g_armed) {
            const auto now = std::chrono::steady_clock::now();
            s.rel  = (int)ObjectHold::RagFramePost() - g_postLeft + 1;   // F0+1, F0+2, ...
            s.tMs  = std::chrono::duration<double, std::milli>(now - g_t0).count();
            Emit(s, id);
            if (--g_postLeft <= 0) {
                logger::info("RAGFRAME {:08X} window closed — read 05 §3: rbFrac decaying + "
                             "pwMaxForce 500 = H10 · a one-frame |v| step = H3 · |v| tracking the "
                             "hand body = H2", id);
                g_armed = 0;
            }
            return;
        }
        // idle: keep the last kPre frames so the window BEFORE the event survives
        g_ring[g_ringN % kPre] = s;
        ++g_ringN;
    }

    void Arm(std::uint32_t actorId, float mmag, int slot, const char* magSrc,
             float bearingDeg, bool ragdoll)
    {
        if (!Enabled() || !actorId) return;
        g_armed   = actorId;
        g_postLeft = (std::max)(1, (int)ObjectHold::RagFramePost());
        g_t0      = std::chrono::steady_clock::now();
        // ⛔ THE RING FLUSH IS DEFERRED, NOT DONE HERE. Arm's caller is PushStep's reaction tier,
        // which runs INSIDE the 0x5E0885 movement hook — the exact seam where doing real work
        // froze the game twice in this project's history (EvaluatePackage inline, and the bump
        // fallback). Up to 30 synchronous log lines with flush_on(info) is real work: it would
        // stall the movement evaluation between F0 and the first post-event sample, i.e. distort
        // the very timeline the receipt exists to measure. The next OnFrame (main thread, frame
        // lambda) drains it. Only the two-line header is written here.
        g_flushPending = true;
        logger::info("================ RAGFRAME {:08X} F0 — {} requested ================",
                     actorId, ragdoll ? "RAGDOLL" : "STAGGER");
        // ★ mmag AND ITS SOURCE, never logged before: the 2026-08-30 per-frame idle probe
        // read per-bone maxima BELOW the 20u trigger around K1, so the magnitude that
        // actually fired a knockdown is, to this day, unknown (Ragdoll Research 06 §3 item 8).
        logger::info("RAGFRAME {:08X} TRIGGER mmag={:.2f}u slot={} src={} bearing={:+.0f}° "
                     "(ragdollU={:.1f} staggerU={:.1f})",
                     actorId, mmag, slot, magSrc ? magSrc : "?", bearingDeg,
                     ObjectHold::PushStepRagdollU(), ObjectHold::PushStepStaggerU());
    }

    // Drained from OnFrame — see the deferral note in Arm.
    void FlushRing(std::uint32_t actorId)
    {
        // flush the ring oldest-first: these are the ~10 frames BEFORE the event, where H2
        // and the hit-as-trigger case both live (a stab can trip the 20u trigger by itself).
        const int n = (g_ringN < kPre) ? g_ringN : kPre;
        int skipped = 0;
        for (int k = 0; k < n; ++k) {
            const int idx = (g_ringN - n + k) % kPre;
            Sample& s = g_ring[idx];
            // ⛔ never relabel another actor's frame with the armed FormID (see Sample::actorId)
            if (s.actorId != actorId) { ++skipped; continue; }
            s.rel = -(n - k);
            // real elapsed time back from F0 — one clock across the whole window
            s.tMs = (s.stamp.time_since_epoch().count() != 0)
                      ? -std::chrono::duration<double, std::milli>(g_t0 - s.stamp).count()
                      : (double)s.rel * 14.7;
            Emit(s, actorId);
        }
        // ★ no silent caps: say what was dropped, or the window reads as complete when it is not
        if (skipped)
            logger::info("RAGFRAME {:08X} ring: {} of {} pre-event frame(s) belonged to another "
                         "actor and were NOT printed", actorId, skipped, n);
        g_ringN = 0;
        for (Sample& s : g_ring) s.used = false;
    }

    void NoteHit(std::uint32_t actorId, bool planck, bool power, float velMag, bool isLeft)
    {
        if (!Enabled() || !actorId) return;
        if (actorId != g_armed && actorId != g_subject) return;
        // ★ THE H3 DISCRIMINATOR. A PLANCK hit inside the window is applied at FULL strength
        // (stab x5 -> ~1,022 u/s on a 7 kg spine bone; x1.75 more on a power attack), because
        // its x0.3 reduction keys on IsInRagdollState() and that flag is still false. If this
        // line lands near F0 AND the BODIES line shows a one-frame |v| step on one bone with
        // its neighbours at ~35/25/15%, H3 is the launch and the fix is 04 stage 1b.
        logger::info("RAGFRAME {:08X} HIT planck={} power={} vel={:.1f}u/s hand={} armed={} "
                     "postLeft={}{}",
                     actorId, planck ? 1 : 0, power ? 1 : 0, velMag, isLeft ? "L" : "R",
                     g_armed ? 1 : 0, g_postLeft,
                     planck ? "  <-- FULL-STRENGTH while the engine flag is still false"
                            : "  (not a PLANCK hit)");
    }


    // ── THE HIT SINK (2026-09-03) — the H3 discriminator ────────────────────────────────────
    // H3 says the launch is PLANCK's melee impulse applied at FULL strength: its x0.3 reduction
    // for a ragdolled actor keys on IsInRagdollState(), which stays FALSE for ~13 frames after
    // PPB asks for the knockdown. A 2.5 m/s stab is ~1,022 u/s on a 7 kg spine bone, and 1,789
    // with the trigger held. If a PLANCK hit lands anywhere inside the window, that impulse IS
    // the launch — and a one-frame |v| step in the BODIES line will corroborate it.
    //   PPB's existing DismemberGuard::HitSink cannot answer this: it only records while the
    // guard is on, and it never reads PLANCK's extended data. Hence a dedicated sink.
    //   LAYOUT, READ from planck_080/include/planckinterface001.h (NOT recalled):
    //     struct PlanckHitEvent : TESHitEvent { void* hitData; PlanckHitData extendedHitData; }
    //     PlanckHitData { NiPoint3 position; NiPoint3 velocity; NiPointer node;
    //                     BSFixedString nodeName; bool isLeft; }
    //   CommonLibVR's TESHitEvent is exactly 0x20 (static_assert in the header), so hitData is
    //   at +0x20 and the extended block at +0x28: position +0x28, velocity +0x34, isLeft +0x50.
    //   ⚠ THE MAGIC TEST reads flags as a DWORD. CommonLibVR declares `flags` as a one-byte
    //   EnumSet at +0x18; PLANCK writes a full word there and tests
    //   (flags & 0xFFFFFF00) == 0x59914000 — so reading the byte can never see the magic.
    class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* ev,
                                              RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            // ⚠ integer read only — Enabled() is a FLOAT compare and this can be a Havok thread
            if (!ev || !ev->target || !g_ragOnAtomic.load(std::memory_order_relaxed))
                return RE::BSEventNotifyControl::kContinue;
            auto* victim = ev->target->As<RE::Actor>();
            if (!victim) return RE::BSEventNotifyControl::kContinue;
            const std::uint32_t vid = victim->GetFormID();
            if (vid != g_armed && vid != g_subject) return RE::BSEventNotifyControl::kContinue;
            const auto base = reinterpret_cast<std::uintptr_t>(ev);
            const std::uint32_t flagsWord = *reinterpret_cast<const std::uint32_t*>(base + 0x18);
            const bool planck = (flagsWord & 0xFFFFFF00u) == 0x59914000u;
            const bool power  = (flagsWord & 0x1u) != 0;      // kPowerAttack — PLANCK's x1.75
            float velMag = -1.f; bool isLeft = false;
            if (planck) {
                const auto* vel = reinterpret_cast<const float*>(base + 0x34);
                velMag = std::sqrt(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);
                isLeft = *reinterpret_cast<const bool*>(base + 0x50);
            }
            if (::GetCurrentThreadId() == g_mainThreadId) {
                NoteHit(vid, planck, power, velMag, isLeft);           // main thread: log now
            } else {
                // collision thread: park it. Plain stores under a release flag — no logging,
                // no allocation, no lock. OnFrame drains it next main-thread tick.
                g_hit.id = vid; g_hit.planck = planck; g_hit.power = power;
                g_hit.velMag = velMag; g_hit.isLeft = isLeft;
                g_hit.pending.store(true, std::memory_order_release);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    HitSink g_hitSink;
    bool    g_sinkInstalled = false;

    void InstallHitSink()
    {
        if (g_sinkInstalled) return;
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
            g_mainThreadId = ::GetCurrentThreadId();   // Install runs at kDataLoaded = main
            g_sinkInstalled = true;
            logger::info("RAGFRAME: TESHitEvent sink registered (PLANCK hit detection for H3)");
        }
    }

    void Install() { InstallHitSink(); }

    // Physics thread. Relaxed atomic only — no logging, no floats, no allocation (T4).
    void NoteStep() { g_stepsThisFrame.fetch_add(1, std::memory_order_relaxed); }

    void NoteSettle(std::uint32_t actorId, int capped)
    {
        if (!Enabled() || !actorId) return;
        if (actorId != g_armed && actorId != g_subject) return;
        g_settleCapped = capped;
    }

    void ClearOnLoad()
    {
        for (Sample& s : g_ring) s = Sample{};
        g_ringN = 0; g_subject = 0; g_armed = 0; g_postLeft = 0;
        g_pend = PendingDrive{};
        g_stepsThisFrame.store(0, std::memory_order_relaxed);
        g_settleCapped = -1;
        g_hit.pending.store(false, std::memory_order_release);
        g_prevPlayerPosValid = false;
        s_prevKnocked = false;
        g_flushPending = false;
        g_wantDrive.store(false, std::memory_order_relaxed);
    }
}
