// ─────────────────────────────────────────────────────────────────────────
// FsmpLink.cpp — STAGE 1: SMP plugin-interface handshake + read-only census.
// Compiled against fsmp/PluginAPI.h pinned to the v4.0.1 tag (interface 2.0.0,
// bullet 3.24.0 declared; the installed 4.0.1 binary links bullet 3.24.4).
//
// Threading contract (PluginAPI.h + hdtSkyrimPhysicsWorld.cpp): Pre/PostStep
// listeners run on the engine's TBB worker thread while its world mutex is
// held — atomics only here, NO logging, NO blocking (the HIGGS-callback
// discipline; fmt on a foreign thread stack is the proven CTD class).
//
// Version safety: getVersionInfo() is virtual slot 1 (after the dtor) by API
// design — stable across the lineage precisely so clients can version-check
// before touching anything else. The call is still wrapped in SEH so a hostile
// vtable (unknown fork) downgrades to a logged failure, not a crash.
// ─────────────────────────────────────────────────────────────────────────

#include "FsmpLink.h"
#include "Tuning.h"    // ObjectHold::FsmpPush* knobs

// Bullet 3.24 (vendored headers, extern/bullet324 - matches the engine's declared
// BULLET_VERSION{3,24,0}). Included BEFORE PluginAPI.h so its forward declarations
// (btCollisionObject, btAlignedObjectArray) bind to the real definitions.
#include <BulletDynamics/Dynamics/btRigidBody.h>
#include "fsmp/PluginAPI.h"
#include "fsmp/PluginAPI_v1.h"   // HDT-SMP Flex (interface 1.0.0) — see that header's banner

#include <chrono>

#include <atomic>
#include <cstring>

namespace logger = SKSE::log;

namespace {

    // -- STAGE 2: double-buffered push targets (main thread writes, physics thread reads) --
    struct TargetBuf {
        int                   n = 0;
        FsmpLink::PushTarget  t[FsmpLink::kMaxPushTargets]{};   // sized by the ONE constant in
                                         // FsmpLink.h (28) — see its banner for why 28 and for the
                                         // history of the hand-synchronized 14s this replaces.
        std::uint64_t         stampMs = 0;
    };
    // 4-deep rotation (2026-07-13 review: the multi-rig merge legitimately publishes
    // several times per frame; 2 slots gave a same-frame rewrite window against the
    // TBB reader holding a reference — 4 slots puts 3 publishes between write and reuse).
    TargetBuf                  g_tbuf[4];
    std::atomic<int>           g_tActive{ 0 };
    std::atomic<std::uint64_t> g_pushApplied{ 0 };   // forces applied (physics thread) - OnFrame receipt

    inline std::uint64_t NowMs() {
        return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    hdt::PluginInterface*      g_iface{ nullptr };
    std::atomic<bool>          g_accepted{ false };
    std::atomic<int>           g_ifaceMajor{ 0 };   // 2 = Faster HDT-SMP, 1 = SMP Flex compat
    std::atomic<std::uint64_t> g_steps{ 0 };
    std::atomic<int>           g_lastCount{ -1 };
    std::atomic<std::uint32_t> g_lastDtBits{ 0 };
    char                       g_sender[32] = {};   // written once pre-accept (main thread)

    // Deferred-log flags (set anywhere, consumed by OnFrame on the main thread)
    std::atomic<bool> g_logFirstStep{ false };

    // ── ABI-NEUTRAL STEP BODIES (2026-07-29, SMP Flex support) ──────────────────
    // The interface-1 and interface-2 event STRUCTS are byte-identical; only the
    // listener base class differs (see fsmp/PluginAPI_v1.h). So the actual work
    // lives in these two free functions, taking the unpacked fields, and BOTH the
    // v2 BSTEventSink sinks and the v1 IEventListener sinks call straight into
    // them. One implementation, no duplicated force maths, no way for the two
    // engines to drift apart.
    using ObjArray = btAlignedObjectArray<btCollisionObject*>;

    void CensusStep(const ObjArray& objects, float timeStep)
    {
        // ⚠ TBB worker thread, engine world lock held: atomics only.
        const int n = objects.size();
        if (n >= 0 && n < 1000000) g_lastCount.store(n, std::memory_order_relaxed);
        std::uint32_t bits;
        std::memcpy(&bits, &timeStep, 4);
        g_lastDtBits.store(bits, std::memory_order_relaxed);
        if (g_steps.fetch_add(1, std::memory_order_relaxed) == 0)
            g_logFirstStep.store(true, std::memory_order_relaxed);
    }

    void PushStep(const ObjArray& objects);   // defined below, after the knob reads

    struct PostSink : RE::BSTEventSink<hdt::PostStepEvent> {
        RE::BSEventNotifyControl ProcessEvent(const hdt::PostStepEvent* e,
                                              RE::BSTEventSource<hdt::PostStepEvent>*) override
        {
            if (e) CensusStep(e->objects, e->timeStep);
            else if (g_steps.fetch_add(1, std::memory_order_relaxed) == 0)
                g_logFirstStep.store(true, std::memory_order_relaxed);
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    PostSink g_postSink;

    // -- STAGE 2: the force actuator. PreStep is the API's sanctioned window for
    // forces/torques; everything else stays read-only. Match = world-position
    // proximity to a published chord-host bone (<1.5u; tail bones sit 3-6u apart,
    // and SMP's world runs in plain game units - 06_BoneFollow + report 14).
    struct PreSink : RE::BSTEventSink<hdt::PreStepEvent> {
        RE::BSEventNotifyControl ProcessEvent(const hdt::PreStepEvent* e,
                                              RE::BSTEventSource<hdt::PreStepEvent>*) override
        {
            if (e) PushStep(e->objects);
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    PreSink g_preSink;

    void PushStep(const ObjArray& objects)
    {
        {
            // TBB worker thread, engine world lock held: atomics only, NO logging.
            if (!ObjectHold::FsmpPushEnabled()) return;
            const TargetBuf& buf = g_tbuf[g_tActive.load(std::memory_order_acquire)];
            if (buf.n <= 0) return;
            if (NowMs() - buf.stampMs > 250) return;   // stale rig

            // 2026-07-13 per-target feel: gain/clamp come from each PushTarget (the
            // publisher's per-table TuneOf numbers); fsmpPushMult is a GLOBAL multiplier
            // on both. fsmpPushForce/fsmpPushMaxForce are legacy — no longer read here.
            const float mult    = ObjectHold::FsmpPushMult();
            const float minDisp = ObjectHold::FsmpPushMinDispU();
            constexpr float kMatchU2 = 1.5f * 1.5f;

            const int n = objects.size();
            for (int i = 0; i < n; ++i) {
                btCollisionObject* o = objects[i];
                if (!o || o->isStaticOrKinematicObject()) continue;
                btRigidBody* rb = btRigidBody::upcast(o);
                if (!rb) continue;
                const btVector3& p = o->getWorldTransform().getOrigin();
                for (int t = 0; t < buf.n; ++t) {
                    const auto& tg = buf.t[t];
                    const float dx = p.x() - tg.bonePosU[0];
                    const float dy = p.y() - tg.bonePosU[1];
                    const float dz = p.z() - tg.bonePosU[2];
                    if (dx * dx + dy * dy + dz * dz > kMatchU2) continue;
                    const float mag2 = tg.dispU[0] * tg.dispU[0] + tg.dispU[1] * tg.dispU[1]
                                     + tg.dispU[2] * tg.dispU[2];
                    if (mag2 < minDisp * minDisp) continue;   // below deadzone — but a CO-LOCATED
                                          // duplicate-host target later in the buffer may carry
                                          // real displacement (foxtail curled pose: the long
                                          // sensor chord leaves the fur and publishes zero while
                                          // the short grip chord is pressed — review 2026-07-17
                                          // HIGH). Keep scanning; the post-force `break` still
                                          // caps at ONE force per body per step.
                    // ── MASS-SCALED FORCE (2026-07-17, the tip-jitter fix): the locked gains were
                    // dialed on the fluffy tail's 0.4 kg BASE bones; the same force on a 0.15 kg TIP
                    // bone = ~2.7x the acceleration -> sustained oscillation (log: 2-4u standing
                    // displacement gradient + 23k forces, the a=F/m loop). F = gain x disp x m/m_ref
                    // (Report 14 §9.3's designed endgame): the base keeps its exact dialed feel
                    // (0.4/0.4 = 1), light tip bones self-derate (0.15/0.4 = 0.375 -> ~4500 eff,
                    // inside the light-bone band the foxtail proved). Clamp scales too. Capped at
                    // 1.5 so heavier-than-ref bones can't be over-forced. fsmpMassScale 0 = off.
                    float mScale = 1.f;
                    if (ObjectHold::FsmpMassScale() >= 0.5f) {
                        const float invM = rb->getInvMass();
                        if (invM > 1e-6f) {
                            // m_ref rides WITH the target (per-table, 2026-07-17): 0.4 for the
                            // fluffy tail, 0.1 for the foxtail — each table normalizes around
                            // the bone class its gain was DIALED on, so "gain 2800" means the
                            // same felt response on the bones the user actually touches.
                            const float mRef = tg.massRef > 0.01f ? tg.massRef : 0.4f;
                            mScale = (1.f / invM) / mRef;
                            if (mScale > 1.5f) mScale = 1.5f;
                            else if (mScale < 0.05f) mScale = 0.05f;
                        }
                    }
                    const float effForce = tg.force * mult * mScale;
                    const float effMax   = tg.maxF  * mult * mScale;
                    btVector3 F(tg.dispU[0] * effForce, tg.dispU[1] * effForce, tg.dispU[2] * effForce);
                    const float fLen = F.length();
                    if (fLen > effMax && fLen > 1e-6f) F *= effMax / fLen;
                    rb->applyCentralForce(F);
                    g_pushApplied.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }

    // ── INTERFACE-1 SINKS (SMP Flex) ────────────────────────────────────────────
    // Same bodies, v1 ABI. `onEvent` is vtable SLOT 0 because hdtv1::IEventListener
    // has no virtual destructor — do NOT declare a destructor or any other virtual
    // in these structs, and do not reorder. They are file-static and never deleted,
    // so the missing virtual dtor is correct, not an oversight.
    struct PostSinkV1 : hdtv1::IEventListener<hdtv1::PostStepEvent> {
        void onEvent(const hdtv1::PostStepEvent& e) override { CensusStep(e.objects, e.timeStep); }
    };
    struct PreSinkV1 : hdtv1::IEventListener<hdtv1::PreStepEvent> {
        void onEvent(const hdtv1::PreStepEvent& e) override { PushStep(e.objects); }
    };
    PostSinkV1 g_postSinkV1;
    PreSinkV1  g_preSinkV1;

    // SEH-isolated version read: no C++ objects in this frame (SEH + unwinding
    // don't mix), returns false instead of crashing on a hostile/foreign vtable.
    bool ReadVersion(hdt::PluginInterface* iface, hdt::PluginInterface::VersionInfo* out)
    {
        __try {
            *out = iface->getVersionInfo();
            return true;
        } __except (1 /*EXCEPTION_EXECUTE_HANDLER*/) {
            return false;
        }
    }

    void OnEngineMessage(SKSE::MessagingInterface::Message* msg)
    {
        // MESSAGE CENSUS (2026-07-31): the handler used to return SILENTLY on any message that
        // was not MSG_STARTUP, so "the engine never dispatched" and "it dispatched something we
        // did not recognise" were indistinguishable in the log. Name what actually arrives.
        if (msg) {
            static std::atomic<int> s_census{ 8 };
            if (s_census.fetch_sub(1, std::memory_order_relaxed) > 0)
                logger::info("FSMPLINK census: message from '{}' type={} data={} "
                             "(MSG_STARTUP is type {})",
                             msg->sender ? msg->sender : "<null>", msg->type,
                             msg->data ? "present" : "NULL",
                             (int)hdt::PluginInterface::MSG_STARTUP);
        }
        if (!msg || msg->type != hdt::PluginInterface::MSG_STARTUP || !msg->data) return;
        if (g_accepted.load(std::memory_order_relaxed)) return;   // first accepted engine wins

        auto* iface = static_cast<hdt::PluginInterface*>(msg->data);
        const char* from = msg->sender ? msg->sender : "<unknown>";

        hdt::PluginInterface::VersionInfo vi{};
        if (!ReadVersion(iface, &vi)) {
            logger::warn("FSMPLINK: MSG_STARTUP from '{}' but getVersionInfo() faulted — foreign "
                         "interface layout; link stays OFF for this engine.", from);
            return;
        }
        logger::info("FSMPLINK: MSG_STARTUP from '{}' — interface {}.{}.{}, bullet {}.{}.{} "
                     "(compiled against interface {}.{}.{}, bullet {}.{}.{})",
                     from,
                     vi.interfaceVersion.major, vi.interfaceVersion.minor, vi.interfaceVersion.patch,
                     vi.bulletVersion.major, vi.bulletVersion.minor, vi.bulletVersion.patch,
                     hdt::PluginInterface::INTERFACE_VERSION.major,
                     hdt::PluginInterface::INTERFACE_VERSION.minor,
                     hdt::PluginInterface::INTERFACE_VERSION.patch,
                     hdt::PluginInterface::BULLET_VERSION.major,
                     hdt::PluginInterface::BULLET_VERSION.minor,
                     hdt::PluginInterface::BULLET_VERSION.patch);

        // ── INTERFACE TIER (2026-07-29): exact v2, or the v1 compat path for SMP Flex ──
        // The major gate must NEVER be merely loosened: v1 and v2 differ in the LISTENER
        // base class, so attaching a v2-shaped BSTEventSink to a v1 engine would have the
        // engine call our destructor once per physics step (fsmp/PluginAPI_v1.h explains
        // in full). The safe answer is a separate v1-shaped listener, below.
        const int major = vi.interfaceVersion.major;
        const bool exactV2 = (major == hdt::PluginInterface::INTERFACE_VERSION.major);
        const bool compatV1 = (major == hdtv1::PluginInterface::INTERFACE_VERSION.major);
        if (!exactV2 && !compatV1) {
            logger::warn("FSMPLINK: interface major {} is neither 2 (Faster HDT-SMP) nor 1 "
                         "(HDT-SMP Flex) — link stays OFF. PPB has no listener ABI for this "
                         "engine; forces and even the census would ride an unknown vtable.",
                         major);
            return;
        }
        // ⚠ Read the knob EARLY, off disk. This runs at the engine's kPostPostLoad, long
        // before the pre-drive hook (kDataLoaded) has ever polled PPB_tuning.txt, so
        // ObjectHold::FsmpFlexCompat() would still hold the compiled default here and the
        // user's "fsmpFlexCompat 0" would be silently ignored.
        if (compatV1 && ObjectHold::EarlyReadKnob("fsmpFlexCompat", 1.f) < 0.5f) {
            logger::warn("FSMPLINK: interface 1.x engine detected ('{}') but fsmpFlexCompat is 0 "
                         "— link stays OFF by request. Set fsmpFlexCompat 1 in PPB_tuning.txt to "
                         "enable the SMP Flex path.", from);
            return;
        }
        // BULLET ABI GATE (Report 17A-31): both sinks touch btRigidBody/btCollisionObject
        // through the COMPILED bullet layout — an engine built on a different bullet can
        // keep interface major 2 (the maintainer must remember to bump it) while moving
        // the fields applyCentralForce and the census reads land on. Wrong-offset reads
        // deref garbage; forces write into the wrong fields. Patch-level differences
        // (3.24.x) are ABI-stable; major/minor changes are not.
        if (vi.bulletVersion.major != hdt::PluginInterface::BULLET_VERSION.major ||
            vi.bulletVersion.minor != hdt::PluginInterface::BULLET_VERSION.minor) {
            logger::warn("FSMPLINK: bullet version mismatch (engine {}.{}.{} vs compiled {}.{}.{}) "
                         "— link stays OFF (census reads + push forces both ride the compiled "
                         "bullet ABI).",
                         vi.bulletVersion.major, vi.bulletVersion.minor, vi.bulletVersion.patch,
                         hdt::PluginInterface::BULLET_VERSION.major,
                         hdt::PluginInterface::BULLET_VERSION.minor,
                         hdt::PluginInterface::BULLET_VERSION.patch);
            return;
        }

        std::snprintf(g_sender, sizeof(g_sender), "%s", from);
        g_iface = iface;
        if (exactV2) {
            iface->addListener(static_cast<hdt::IPostStepListener*>(&g_postSink));
            iface->addListener(static_cast<hdt::IPreStepListener*>(&g_preSink));
            g_ifaceMajor.store(2, std::memory_order_relaxed);
            g_accepted.store(true, std::memory_order_release);
            logger::info("FSMPLINK: ACCEPTED (interface 2 — Faster HDT-SMP) — PostStep census + "
                         "PreStep force listeners attached (stage 2; forces gated on fsmpPush).");
        } else {
            // The PluginInterface vtable is identical across v1/v2 (same 6 slots, same
            // declaration order — verified against upstream source AND Flex's shipped PDB),
            // so re-typing the SAME pointer is legitimate: it changes only the ARGUMENT type
            // we pass, not which slot we call.
            auto* v1 = reinterpret_cast<hdtv1::PluginInterface*>(msg->data);
            v1->addListener(static_cast<hdtv1::IPostStepListener*>(&g_postSinkV1));
            v1->addListener(static_cast<hdtv1::IPreStepListener*>(&g_preSinkV1));
            g_ifaceMajor.store(1, std::memory_order_relaxed);
            g_accepted.store(true, std::memory_order_release);
            logger::info("FSMPLINK: ACCEPTED (interface 1 COMPAT — HDT-SMP Flex) — v1-shaped "
                         "onEvent listeners attached. Bullet matches at {}.{}.x so the census "
                         "reads and applyCentralForce are ABI-identical to the v2 path; only the "
                         "listener vtable differs. Watch for 'FSMPLINK LIVE' next — if it never "
                         "appears, the listeners are not being called and fsmpFlexCompat should "
                         "go to 0.",
                         vi.bulletVersion.major, vi.bulletVersion.minor);
        }
    }

}  // namespace

namespace FsmpLink {

    // Public forwarder — the anonymous-namespace handler is not linkable from main.cpp.
    // See FsmpLink.h for why the handshake no longer owns its own SKSE registration.
    void HandleEngineMessage(SKSE::MessagingInterface::Message* msg) { OnEngineMessage(msg); }


    void Register()
    {
        auto* msg = SKSE::GetMessagingInterface();
        if (!msg) return;
        // Both known engine sender names: FSMP ("hdtsmp64", CMake project name) and
        // SMP Flex ("hdtSMP64", the classic name). Only the installed one dispatches.
        // ⚠ 2026-07-31: these return values were DISCARDED and the success line printed
        // unconditionally — so "listeners registered" was a CLAIM, not a receipt, and a
        // session where the handshake never happened looked identical to one where it did.
        // SKSE resolves the sender by plugin NAME; a name that does not resolve returns false.
        // ⚠ NO once-only guard. FSMP's dispatch site says "Send ourselves to any plugin that
        // registered during the PostLoad event" (PluginInterfaceImpl::onPostPostLoad, v4.0.1),
        // so kPostLoad is the CONTRACTED registration point. A guard here would let an earlier
        // SKSEPluginLoad call suppress the compliant one — which is exactly what happened when
        // I moved registration earlier. Register at BOTH; a duplicate listener is harmless
        // (g_accepted makes the handler idempotent), a MISSING one is fatal and silent.
        // ⚠ 2026-08-01: we used to register BOTH spellings unconditionally. SKSE resolves
        // sender names case-INSENSITIVELY (_stricmp in LookupHandleFromName), so on any single
        // engine BOTH names resolve to the SAME plugin handle and we registered the same
        // handler twice for one sender. Whether SKSE appends or replaces on a duplicate
        // (listener, sender) pair is not documented — and we are currently registered, the
        // engine dispatches, and our handler is never called. Register the SECOND spelling
        // ONLY if the first did not resolve, so exactly one listener exists per engine.
        const bool okLower = msg->RegisterListener("hdtsmp64", OnEngineMessage);
        const bool okUpper = okLower ? false
                                     : msg->RegisterListener("hdtSMP64", OnEngineMessage);
        logger::info("FSMPLINK: sender resolution — 'hdtsmp64'={}{}",
                     okLower ? "RESOLVED (listening)" : "not a loaded plugin",
                     okLower ? "" : (okUpper ? ", 'hdtSMP64'=RESOLVED (listening)"
                                             : ", 'hdtSMP64'=not a loaded plugin"));
        if (okLower || okUpper)
            logger::info("FSMPLINK: listener registered — 'hdtsmp64'={} 'hdtSMP64'={} "
                         "(awaiting MSG_STARTUP at the engine's PostPostLoad).",
                         okLower ? "OK" : "no such plugin", okUpper ? "OK" : "no such plugin");
        else
            logger::warn("FSMPLINK: BOTH listener registrations FAILED — SKSE resolved neither "
                         "'hdtsmp64' nor 'hdtSMP64' to a loaded plugin. No SMP handshake is "
                         "possible this session: hair/tail capsules will collide but never PUSH. "
                         "Check the engine's declared plugin name.");
    }

    void OnFrame()
    {
        if (g_logFirstStep.exchange(false, std::memory_order_relaxed)) {
            float dt;
            std::uint32_t bits = g_lastDtBits.load(std::memory_order_relaxed);
            std::memcpy(&dt, &bits, 4);
            logger::info("FSMPLINK LIVE: first PostStep observed from '{}' (interface {}) — {} "
                         "collision objects, dt={:.4f}s. Handshake + event flow + threading "
                         "PROVEN on this engine.",
                         g_sender, g_ifaceMajor.load(std::memory_order_relaxed),
                         g_lastCount.load(std::memory_order_relaxed), dt);
        }
        // 60 s heartbeat while connected (dev receipt; cheap)
        static std::uint64_t s_lastSteps = 0;
        static int s_tick = 0;
        if (g_accepted.load(std::memory_order_relaxed) && ++s_tick >= 5400) {   // ~60s at 90fps
            s_tick = 0;
            const std::uint64_t st = g_steps.load(std::memory_order_relaxed);
            logger::info("FSMPLINK heartbeat: {} physics steps observed (+{}), {} objects in world, "
                         "{} push forces applied total.",
                         st, st - s_lastSteps, g_lastCount.load(std::memory_order_relaxed),
                         g_pushApplied.load(std::memory_order_relaxed));
            s_lastSteps = st;
        }
    }

    bool Connected() { return g_accepted.load(std::memory_order_acquire); }

    void PublishTargets(const PushTarget* t, int n)
    {
        if (n < 0) n = 0;
        if (n > kMaxPushTargets) n = kMaxPushTargets;   // = TargetBuf::t[] capacity (single truth)
        const int next = (g_tActive.load(std::memory_order_relaxed) + 1) & 3;
        TargetBuf& b = g_tbuf[next];
        b.n = n;
        for (int i = 0; i < n; ++i) b.t[i] = t[i];
        b.stampMs = NowMs();
        g_tActive.store(next, std::memory_order_release);
    }
}
