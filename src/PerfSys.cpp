#include "PCH.h"
#include "PerfSys.h"
#include "Tuning.h"        // ObjectHold::Perf* accessors
#include "DismemberGuard.h"
#include "Interop.h"       // Interop::GetHiggs / IsActorGrabbedByPlayer
#include "HiggsInterface.h"
#include "NpcFingerTest.h" // NpcFinger::FilterDecision/OnFrame — spliced into FilterCB + the frame lambda
#include "HandBox.h"
#include "FsmpLink.h"
#include "PpbApi.h"       // HandBox::FilterDecision/OnFrame/RegisterHiggs — same splice points

#include <atomic>
#include <iterator>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace logger = SKSE::log;

namespace {

    // ─────────────────────────────────────────────────────────────────────
    //  filterInfo bit layout (Diag.cpp conventions, Report 11 §5.2)
    // ─────────────────────────────────────────────────────────────────────
    inline unsigned      FilterPart(std::uint32_t f)  { return (f >> 8) & 0x1F; }
    inline std::uint32_t FilterGroup(std::uint32_t f) { return f >> 16; }
    constexpr std::uint32_t kBit15 = 0x8000u;   // ragdoll-adjacency convention bit

    // ─────────────────────────────────────────────────────────────────────
    //  hot-path caches for the comparison callback (updated OnFrame; the
    //  callback may run on a physics thread — atomics only, no locks)
    // ─────────────────────────────────────────────────────────────────────
    std::atomic<bool> g_cbSelfThigh{ false };
    std::atomic<bool> g_cbCrossPelvis{ false };
    std::atomic<bool> g_cbLoggedFirstSelf{ false };
    std::atomic<bool> g_cbLoggedFirstCross{ false };
    bool g_higgsRegistered = false;

    using CFRes = Higgs::IHiggsInterface001::CollisionFilterComparisonResult;

    CFRes FilterCB(void*, std::uint32_t infoA, std::uint32_t infoB)
    {
        // NPC FINGER TEST + HAND BOXES (2026-07-09): spliced AHEAD of the perf rules —
        // first non-Continue wins in HIGGS's dispatch, so a finger/box decision must
        // preempt the thigh/pelvis logic below. Both early-out on one relaxed "live"
        // atomic each, so the idle cost is 2 loads + compares.
        if (int d = NpcFinger::FilterDecision(infoA, infoB))
            return d == 1 ? CFRes::Collide : CFRes::Ignore;
        if (int d = HandBox::FilterDecision(infoA, infoB))
            return d == 1 ? CFRes::Collide : CFRes::Ignore;

        // ⚠ NEVER log from this callback: it runs on Havok's multithreaded collision
        // threads through HIGGS's trampoline — fmt/spdlog SIMD spills fault on the
        // misaligned stack there (PROVEN CTD 2026-07-09, NpcFinger::FilterDecision).
        // Flags only; OnFrame (main thread) reports them.
        if ((infoA ^ infoB) & 0xFFFF0000u) {
            // cross-group (two different actors / actor-vs-world)
            if (g_cbCrossPelvis.load(std::memory_order_relaxed)
                && (infoA & kBit15) && (infoB & kBit15)
                && FilterPart(infoA) == 2 && FilterPart(infoB) == 2) {   // COM/Spn0 alias
                g_cbLoggedFirstCross.store(true, std::memory_order_relaxed);
                return CFRes::Ignore;
            }
            return CFRes::Continue;
        }
        // same system group (one actor's own bodies). PLANCK already returned
        // Continue for these only while it has self-collision ON for the actor.
        if (!g_cbSelfThigh.load(std::memory_order_relaxed)) return CFRes::Continue;
        if (!(infoA & kBit15) || !(infoB & kBit15)) return CFRes::Continue;
        const unsigned pa = FilterPart(infoA), pb = FilterPart(infoB);
        if ((pa == 8 && pb == 14) || (pa == 14 && pb == 8)) {            // LThigh x RThigh
            g_cbLoggedFirstSelf.store(true, std::memory_order_relaxed);
            return CFRes::Ignore;
        }
        return CFRes::Continue;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  LOD: the 17 list bodies + per-tier child KEEP budgets (incl. child 0)
    //  REDUCED keeps the LOWEST child indices (build order ~ spread order).
    // ─────────────────────────────────────────────────────────────────────
    struct SlotBudget { const char* node; std::uint8_t keepP50, keepP80; };
    constexpr SlotBudget kLists[] = {
        { "NPC COM [COM ]",          8, 4 },
        { "NPC Spine [Spn0]",        5, 3 },
        { "NPC Spine1 [Spn1]",       5, 3 },
        { "NPC Spine2 [Spn2]",       5, 3 },
        { "NPC Head [Head]",         5, 3 },
        { "NPC R Thigh [RThg]",      4, 2 },
        { "NPC L Thigh [LThg]",      4, 2 },
        { "NPC R Calf [RClf]",       3, 2 },
        { "NPC L Calf [LClf]",       3, 2 },
        { "NPC R Foot [Rft ]",       3, 2 },
        { "NPC L Foot [Lft ]",       3, 2 },
        { "NPC R UpperArm [RUar]",   3, 2 },
        { "NPC L UpperArm [LUar]",   3, 2 },
        { "NPC R Forearm [RLar]",    2, 2 },
        { "NPC L Forearm [LLar]",    2, 2 },
        { "NPC R Hand [RHnd]",       2, 2 },
        { "NPC L Hand [LHnd]",       2, 2 },
    };
    constexpr int kNumLists = (int)std::size(kLists);

    enum class Tier : std::uint8_t { Full = 0, Reduced = 1, Core = 2 };
    const char* TierName(Tier t) { return t == Tier::Full ? "FULL" : t == Tier::Reduced ? "REDUCED" : "CORE"; }

    int KeepFor(Tier t, int slot, int preset, int nChildren) {
        if (t == Tier::Full) return nChildren;
        if (t == Tier::Core) return 1;
        const int k = (preset >= 80 && preset != 90) ? kLists[slot].keepP80 : kLists[slot].keepP50;
        return k < nChildren ? k : nChildren;   // P90 REDUCED uses the P50 budgets
    }

    // ─────────────────────────────────────────────────────────────────────
    //  per-actor state (main-thread maps; mutex only vs ClearOnLoad)
    // ─────────────────────────────────────────────────────────────────────
    struct ActorState {
        Tier          tier{ Tier::Full };       // last COMMITTED tier
        Tier          pendingTier{ Tier::Full };
        int           pendingFrames{ 0 };
        bool          transitionDone{ true };   // staged apply finished?
        bool          held{ false };
        float         dist{ 1.0e9f };
        std::uint64_t lastSeenFrame{ 0 };
        std::uint64_t lastAuditFrame{ 0 };
    };
    std::mutex g_mx;
    std::unordered_map<std::uint32_t, ActorState> g_state;
    std::atomic<std::uint64_t> g_frame{ 0 };
    // elected in OnFrame from last frame's stamps. Atomic (2026-07-09): the new
    // NearestDrivenId() accessor is read from NpcFingerTest outside g_mx.
    std::atomic<std::uint32_t> g_nearestId{ 0 };
    bool          g_twoHeld = false;
    bool          g_loggedPresetOnce = false;

    // ─────────────────────────────────────────────────────────────────────
    //  list-mutation primitives (byte-faithful to Diag.cpp's verified copies;
    //  see 11_ListShape_Performance.md — keep in sync with Diag)
    // ─────────────────────────────────────────────────────────────────────
    inline bool DisableChildInline(RE::hkpListShape* l, std::uint32_t i) {
        const std::uint32_t word = i >> 5, bit = 1u << (i & 31);
        const std::uint32_t old = l->enabledChildren[word], nw = old & ~bit;
        if (nw == old) return false;
        l->enabledChildren[word] = nw;
        ++l->numDisabledChildren;
        return true;
    }
    inline bool EnableChildInline(RE::hkpListShape* l, RE::hkpCollidable* col, std::uint32_t i, bool relaxed) {
        // capacity refusal (review F5): the world-add AABB cache was sized from
        // getNumChildShapes() — growing past it corrupts. Relaxed only for
        // out-of-world bodies (no live BVD consumer).
        if (!relaxed && col && col->boundingVolumeData.capacityChildShapeAabbs < (std::uint32_t)l->childInfo.size())
            return false;
        const std::uint32_t word = i >> 5, bit = 1u << (i & 31);
        const std::uint32_t old = l->enabledChildren[word], nw = old | bit;
        if (nw == old) return false;
        l->enabledChildren[word] = nw;
        --l->numDisabledChildren;
        return true;
    }
    inline void InvalidateBvd(RE::hkpCollidable* col) {
        col->boundingVolumeData.min[0] = 1;
        col->boundingVolumeData.max[0] = 0;
    }
    bool AuditList(const RE::hkpListShape* list) {
        const int n = list->childInfo.size();
        int cleared = 0;
        for (int i = 0; i < n && i < 256; ++i)
            if (!(list->enabledChildren[i >> 5] & (1u << (i & 31)))) ++cleared;
        return cleared == (int)list->numDisabledChildren;
    }

    struct BodyRes { RE::hkpRigidBody* rb{}; RE::hkpCollidable* col{}; const RE::hkpShape* shape{}; };
    BodyRes ResolveBody(RE::Actor* actor, const char* node) {   // Diag's chain, never allocates
        BodyRes r{};
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) return r;
        auto* obj = root->GetObjectByName(node);
        if (!obj) return r;
        auto* colObj = obj->collisionObject.get();
        if (!colObj) return r;
        auto* body = static_cast<RE::bhkCollisionObject*>(colObj)->GetRigidBody();
        r.rb  = body ? body->GetRigidBody() : nullptr;
        r.col = r.rb ? r.rb->GetCollidableRW() : nullptr;
        r.shape = r.col ? r.col->shape : nullptr;
        return r;
    }

    // Apply `tier` to every list body of `actor`. Enables are staged (≤ stageBits
    // per body per call). Returns true when the actor fully matches the tier.
    bool ApplyTier(RE::Actor* actor, Tier tier, int preset, int stageBits, bool relaxedEnable)
    {
        auto* cell = actor->GetParentCell();
        auto* bhk  = cell ? cell->GetbhkWorld() : nullptr;
        if (!bhk && !relaxedEnable) return false;             // no world to lock — try again later

        bool complete = true;
        // one lock for the whole actor (same guard class as Diag::ApplyPendingSpike)
        std::optional<RE::BSWriteLockGuard> lk;
        if (bhk) lk.emplace(bhk->worldLock);

        for (int s = 0; s < kNumLists; ++s) {
            BodyRes r = ResolveBody(actor, kLists[s].node);
            if (!r.rb || !r.col || !r.shape || r.shape->type != RE::hkpShapeType::kList)
                continue;                                     // e.g. body not built yet — skip quietly
            auto* list = const_cast<RE::hkpListShape*>(static_cast<const RE::hkpListShape*>(r.shape));
            const std::int32_t n = list->childInfo.size();
            if (n < 1 || n > 32) continue;
            const int keep = KeepFor(tier, s, preset, (int)n);

            const std::uint32_t enBefore = list->enabledChildren[0];
            const std::uint16_t ndBefore = list->numDisabledChildren;
            bool changed = false;
            int  enables = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                const bool want = (i < keep);
                if (want) {
                    // staged enables (review F6): never materialize a full envelope in one frame
                    if (!(list->enabledChildren[0] & (1u << i))) {
                        if (enables >= stageBits) { complete = false; continue; }
                        if (EnableChildInline(list, r.col, (std::uint32_t)i, relaxedEnable)) { ++enables; changed = true; }
                        else if (r.col->boundingVolumeData.capacityChildShapeAabbs < (std::uint32_t)n) {
                            // review F5(c): capacity got stamped while reduced — clamp + shout once per audit window
                            complete = true;   // nothing more we can do this pass
                            logger::error("PERF LOD {:08X} '{}': BVD capacity {} < {} children — enable clamped "
                                          "(will self-repair on next 3D reload)", actor->GetFormID(), kLists[s].node,
                                          r.col->boundingVolumeData.capacityChildShapeAabbs, n);
                            break;
                        }
                    }
                } else {
                    if (DisableChildInline(list, (std::uint32_t)i)) changed = true;
                }
            }
            if (changed) {
                if (!AuditList(list)) {                       // rollback (word[0] snapshot; all lists ≤ 32)
                    list->enabledChildren[0]  = enBefore;
                    list->numDisabledChildren = ndBefore;
                    logger::error("PERF LOD {:08X} '{}': AUDIT FAILED — restored, LOD skipped this body",
                                  actor->GetFormID(), kLists[s].node);
                    continue;
                }
                InvalidateBvd(r.col);
            }
        }
        return complete;
    }

}   // namespace

namespace PerfSys {

    void RegisterHiggs()
    {
        if (g_higgsRegistered) return;
        auto* h = Interop::GetHiggs();
        if (!h) return;                                        // retried at kDataLoaded
        h->AddCollisionFilterComparisonCallback(&FilterCB);
        h->AddPostVrikPostHiggsCallback([]() {
            PerfSys::OnFrame();
            NpcFinger::OnFrame();   // finger-rig frame counter + stale/retarget/knob-off destroys
            FsmpLink::OnFrame();    // SMP-link deferred logging (stage 1)
            PpbApi::OnFrame();      // touch API tick: scan, contact table, events, snapshot
            HandBox::OnFrame();     // frame counter + deferred telemetry (2026-07-10 lag fix: the
                                    // pose snapshot moved into HandBox's first PrePhysicsStep fire)
        });
        HandBox::RegisterHiggs();   // PrePhysicsStep callback (vfunc 21) — same handshake site, idempotent
        g_higgsRegistered = true;
        logger::info("PERF: HIGGS comparison callback + frame callback registered "
                     "(preset resolves per-frame from PPB_tuning.txt).");
    }

    void OnFrame()
    {
        g_frame.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);

        // ── LIVE A/B SWITCH for PLANCK's GLOBAL loosenRagdollConstraintPivots (2026-07-31) ──
        // PLANCK has no MCM and no per-actor settings, so its band-aid can only be judged by
        // toggling it and looking. -1 = leave PLANCK alone (default). 0/1 = force. Applied on
        // CHANGE only, from the main thread, with the value read back so the log records what
        // PLANCK actually holds rather than what we asked for.
        {
            static float s_lastWant = -999.f;
            const float want = ObjectHold::PlanckLoosenGlobal();
            if (want != s_lastWant) {
                s_lastWant = want;
                if (want >= -0.5f) {
                    double before = -1.0;
                    const bool gotOk = DismemberGuard::PlanckGetSetting("loosenRagdollConstraintPivots", before);
                    const bool setOk = DismemberGuard::PlanckSetSetting("loosenRagdollConstraintPivots",
                                                                        want > 0.5f ? 1.0 : 0.0);
                    double after = -1.0;
                    DismemberGuard::PlanckGetSetting("loosenRagdollConstraintPivots", after);
                    if (setOk && gotOk)
                        logger::info("PLANCKLOOSEN: global loosenRagdollConstraintPivots {:.0f} -> {:.0f} "
                                     "(requested {:.0f}, read back {:.0f})", before, after, want, after);
                    else
                        logger::warn("PLANCKLOOSEN: could not set global loosenRagdollConstraintPivots "
                                     "(getOk={} setOk={}) — PLANCK absent, or its interface vtable moved "
                                     "(we call Get/SetSettingDouble at slots 13/14).",
                                     gotOk ? 1 : 0, setOk ? 1 : 0);
                } else {
                    logger::info("PLANCKLOOSEN: releasing global override — PLANCK keeps whatever it holds.");
                }
            }
        }

        // refresh the callback's hot caches from the tuning file (1 Hz reload)
        g_cbSelfThigh.store(ObjectHold::PerfFilterSelfThigh(), std::memory_order_relaxed);
        g_cbCrossPelvis.store(ObjectHold::PerfFilterCrossPelvis(), std::memory_order_relaxed);

        // deferred filter telemetry (the callback must never log — collision thread)
        static bool s_repSelf = false, s_repCross = false;
        if (!s_repSelf && g_cbLoggedFirstSelf.load(std::memory_order_relaxed)) {
            s_repSelf = true;
            logger::info("PERF filter: self thigh-x-thigh pair suppressed (bit15 CONFIRMED live; deferred report)");
        }
        if (!s_repCross && g_cbLoggedFirstCross.load(std::memory_order_relaxed)) {
            s_repCross = true;
            logger::info("PERF filter: cross-actor pelvis pair (part2 x part2) suppressed (deferred report)");
        }

        const int preset = ObjectHold::PerfPreset();
        if (preset > 0 && !g_loggedPresetOnce) {
            g_loggedPresetOnce = true;
            logger::info("PERF: preset {} live (selfThigh={} crossPelvis={} lod={})", preset,
                         ObjectHold::PerfFilterSelfThigh(), ObjectHold::PerfFilterCrossPelvis(),
                         ObjectHold::PerfLodEnabled());
        }

        // Contact-decimation change logger (2026-07-10): the decimation itself runs in Diag's
        // contact listener on collision threads (no logging there) — this is the visible receipt
        // that a preset/knob flip took, hot-swappable mid-scene via the tuning reload.
        static int s_lastKeepN = -1;
        const int  keepN = ObjectHold::PerfContactKeepN();
        if (keepN != s_lastKeepN) {
            logger::info("PERF decimation: {} (preset {} / perfContactKeep knob)",
                         keepN > 1 ? fmt::format("ON — keep 1 new contact point in {}", keepN) : "OFF",
                         preset);
            s_lastKeepN = keepN;
        }

        std::lock_guard<std::mutex> g(g_mx);
        // nearest election + two-held clamp from last frame's stamps
        std::uint32_t nearest = 0; float best = ObjectHold::PerfLodNearDist();
        int heldCount = 0;
        for (auto& [id, st] : g_state) {
            if (frame - st.lastSeenFrame > 3) continue;
            if (st.held) ++heldCount;
            if (st.dist < best) { best = st.dist; nearest = id; }
        }
        g_nearestId.store(nearest, std::memory_order_relaxed);
        g_twoHeld = heldCount >= 2;

        // review F5(a): restore actors that stopped being driven while reduced
        for (auto it = g_state.begin(); it != g_state.end();) {
            auto& st = it->second;
            const bool stale = frame - st.lastSeenFrame > 3;
            if (stale && (st.tier != Tier::Full || !st.transitionDone)) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->first);
                if (actor && actor->Get3D()) {
                    // full restore, unstaged (actor left the active set; no live grind),
                    // relaxed enables (its body may already be out of the world)
                    ApplyTier(actor, Tier::Full, preset, 64, true);
                    logger::info("PERF LOD {:08X}: undriven -> restored FULL (F5 guard)", it->first);
                } else {
                    logger::info("PERF LOD {:08X}: undriven + 3D gone — state dropped (NIF reload restores)", it->first);
                }
                it = g_state.erase(it);
                continue;
            }
            if (stale && st.tier == Tier::Full && st.transitionDone) { it = g_state.erase(it); continue; }
            ++it;
        }
    }

    void OnPreDrive(RE::Actor* actor)
    {
        if (!actor || !g_higgsRegistered) return;
        const int preset = ObjectHold::PerfPreset();
        const bool lodOn = preset > 0 && ObjectHold::PerfLodEnabled();
        const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
        const std::uint32_t id = actor->GetFormID();

        std::lock_guard<std::mutex> g(g_mx);
        auto& st = g_state[id];
        st.lastSeenFrame = frame;

        // dist is stamped for EVERY driven actor, LOD on or off (2026-07-09): the
        // OnFrame nearest-election feeds NearestDrivenId(), which NpcFingerTest's
        // knob-attach consumes even at perfPreset 0. One distance per driven actor
        // per frame — no behavior change for the LOD itself.
        auto* player = RE::PlayerCharacter::GetSingleton();
        st.dist = player ? actor->GetPosition().GetDistance(player->GetPosition()) : 1.0e9f;

        if (!lodOn) {
            if (st.tier != Tier::Full || !st.transitionDone) {   // preset turned off — restore
                if (ApplyTier(actor, Tier::Full, preset, (int)ObjectHold::PerfLodStageBits(), false)) {
                    st.tier = Tier::Full; st.transitionDone = true;
                    logger::info("PERF LOD {:08X}: preset off -> FULL restored", id);
                }
            }
            return;
        }

        // ---- policy inputs ----
        st.held = Interop::IsActorGrabbedByPlayer(actor);

        // ---- desired tier (review F2: proximity alone never grants crowd-FULL) ----
        const float h = ObjectHold::PerfLodHystPct();
        const float nearT = ObjectHold::PerfLodNearDist() * (st.tier == Tier::Full    ? 1.f + h : 1.f - h);
        const float farT  = ObjectHold::PerfLodFarDist()  * (st.tier <= Tier::Reduced ? 1.f + h : 1.f - h);
        Tier want;
        if (st.held)
            want = (preset == 90 || g_twoHeld) ? Tier::Reduced : Tier::Full;
        else if (id == g_nearestId.load(std::memory_order_relaxed) && st.dist <= nearT)
            want = (preset == 90) ? Tier::Reduced : Tier::Full;
        else if (st.dist <= farT)
            want = (preset == 90) ? Tier::Core : Tier::Reduced;
        else
            want = Tier::Core;

        // ---- settle gate on downgrades (review F3: pure counters, no contact events) ----
        if (want > st.tier) {
            if (want != st.pendingTier) { st.pendingTier = want; st.pendingFrames = 0; }
            if (++st.pendingFrames < (int)ObjectHold::PerfLodSettleFrames()) want = st.tier;
        } else {
            st.pendingTier = want; st.pendingFrames = 0;
        }

        if (want == st.tier && st.transitionDone) {
            // periodic drift audit (cheap: every ~4 s re-assert the tier)
            if (frame - st.lastAuditFrame < 240) return;
        }
        st.lastAuditFrame = frame;

        const bool prevDone = st.transitionDone;
        const Tier prev = st.tier;
        st.transitionDone = ApplyTier(actor, want, preset, (int)ObjectHold::PerfLodStageBits(), false);
        st.tier = want;
        if (prev != want || (prevDone != st.transitionDone && st.transitionDone))
            logger::info("PERF LOD {:08X}: {} -> {}{} (d={:.0f}u held={} preset={})", id,
                         TierName(prev), TierName(want), st.transitionDone ? "" : " (staged...)",
                         st.dist, st.held, preset);
    }

    void ClearOnLoad()
    {
        std::lock_guard<std::mutex> g(g_mx);
        g_state.clear();
        g_nearestId.store(0, std::memory_order_relaxed);
        g_twoHeld = false;
    }

    // Nearest driven actor elected in OnFrame (0 = none) — NpcFingerTest's
    // knob-attach target. Relaxed atomic; one frame of staleness is fine.
    std::uint32_t NearestDrivenId()
    {
        return g_nearestId.load(std::memory_order_relaxed);
    }

    // TRUE once the HIGGS comparison + frame callbacks are live — NpcFingerTest's
    // creation gate (a finger body without the callback would fall through to the
    // vanilla comparator, whose finger-vs-own-ragdoll fallback is COLLIDE).
    bool HiggsRegistered()
    {
        return g_higgsRegistered;
    }
}
