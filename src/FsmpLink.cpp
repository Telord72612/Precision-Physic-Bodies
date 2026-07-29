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

#include <chrono>

#include <atomic>
#include <cstring>

namespace logger = SKSE::log;

namespace {

    // -- STAGE 2: double-buffered push targets (main thread writes, physics thread reads) --
    struct TargetBuf {
        int                   n = 0;
        FsmpLink::PushTarget  t[14]{};   // 14 = NpcFingerTest kMaxSensors (2026-07-17: M'rissi's
                                         // foxtail publishes ALL 14 chords) — three sizes move in
                                         // lock-step: this, g_pubStage[], and the clamp below.
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
    std::atomic<std::uint64_t> g_steps{ 0 };
    std::atomic<int>           g_lastCount{ -1 };
    std::atomic<std::uint32_t> g_lastDtBits{ 0 };
    char                       g_sender[32] = {};   // written once pre-accept (main thread)

    // Deferred-log flags (set anywhere, consumed by OnFrame on the main thread)
    std::atomic<bool> g_logFirstStep{ false };

    struct PostSink : RE::BSTEventSink<hdt::PostStepEvent> {
        RE::BSEventNotifyControl ProcessEvent(const hdt::PostStepEvent* e,
                                              RE::BSTEventSource<hdt::PostStepEvent>*) override
        {
            // ⚠ TBB worker thread, engine world lock held: atomics only.
            if (e) {
                const int n = e->objects.size();
                if (n >= 0 && n < 1000000) g_lastCount.store(n, std::memory_order_relaxed);
                std::uint32_t bits;
                std::memcpy(&bits, &e->timeStep, 4);
                g_lastDtBits.store(bits, std::memory_order_relaxed);
            }
            if (g_steps.fetch_add(1, std::memory_order_relaxed) == 0)
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
            // TBB worker thread, engine world lock held: atomics only, NO logging.
            if (!e || !ObjectHold::FsmpPushEnabled()) return RE::BSEventNotifyControl::kContinue;
            const TargetBuf& buf = g_tbuf[g_tActive.load(std::memory_order_acquire)];
            if (buf.n <= 0) return RE::BSEventNotifyControl::kContinue;
            if (NowMs() - buf.stampMs > 250) return RE::BSEventNotifyControl::kContinue;   // stale rig

            // 2026-07-13 per-target feel: gain/clamp come from each PushTarget (the
            // publisher's per-table TuneOf numbers); fsmpPushMult is a GLOBAL multiplier
            // on both. fsmpPushForce/fsmpPushMaxForce are legacy — no longer read here.
            const float mult    = ObjectHold::FsmpPushMult();
            const float minDisp = ObjectHold::FsmpPushMinDispU();
            constexpr float kMatchU2 = 1.5f * 1.5f;

            const int n = e->objects.size();
            for (int i = 0; i < n; ++i) {
                btCollisionObject* o = e->objects[i];
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
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    PreSink g_preSink;

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

        if (vi.interfaceVersion.major != hdt::PluginInterface::INTERFACE_VERSION.major) {
            logger::warn("FSMPLINK: interface major mismatch — link stays OFF (stage-2 forces "
                         "would be unsafe against this engine).");
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
        iface->addListener(static_cast<hdt::IPostStepListener*>(&g_postSink));
        iface->addListener(static_cast<hdt::IPreStepListener*>(&g_preSink));
        g_accepted.store(true, std::memory_order_release);
        logger::info("FSMPLINK: ACCEPTED — PostStep census + PreStep force listeners attached "
                     "(stage 2; forces gated on fsmpPush).");
    }

}  // namespace

namespace FsmpLink {

    void Register()
    {
        auto* msg = SKSE::GetMessagingInterface();
        if (!msg) return;
        // Both known engine sender names: FSMP ("hdtsmp64", CMake project name) and
        // SMP Flex ("hdtSMP64", the classic name). Only the installed one dispatches.
        msg->RegisterListener("hdtsmp64", OnEngineMessage);
        msg->RegisterListener("hdtSMP64", OnEngineMessage);
        logger::info("FSMPLINK: listeners registered for senders 'hdtsmp64' + 'hdtSMP64' "
                     "(awaiting MSG_STARTUP at the engine's PostPostLoad).");
    }

    void OnFrame()
    {
        if (g_logFirstStep.exchange(false, std::memory_order_relaxed)) {
            float dt;
            std::uint32_t bits = g_lastDtBits.load(std::memory_order_relaxed);
            std::memcpy(&dt, &bits, 4);
            logger::info("FSMPLINK LIVE: first PostStep observed from '{}' — {} collision objects, "
                         "dt={:.4f}s. Handshake + event flow + threading PROVEN on this engine.",
                         g_sender, g_lastCount.load(std::memory_order_relaxed), dt);
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
        if (n > 14) n = 14;   // = TargetBuf::t[] capacity (lock-stepped with kMaxSensors)
        const int next = (g_tActive.load(std::memory_order_relaxed) + 1) & 3;
        TargetBuf& b = g_tbuf[next];
        b.n = n;
        for (int i = 0; i < n; ++i) b.t[i] = t[i];
        b.stampMs = NowMs();
        g_tActive.store(next, std::memory_order_release);
    }
}
