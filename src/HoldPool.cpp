#include "PCH.h"
#include "HoldPool.h"
#include <chrono>

namespace logger = SKSE::log;

namespace HoldPool
{
    namespace
    {
        constexpr const char*   kPlugin     = "Precision Physic Bodies.esp";
        constexpr RE::FormID    kQuestLocal = 0x806;
        constexpr int           kAliases    = 32;
        constexpr double        kPendingS   = 5.0;   // a dispatched DoHold that never lands is dropped after this

        RE::TESQuest* g_quest = nullptr;
        bool          g_installed = false;
        bool          g_saidAbsent = false;
        double        g_lastSweep = 0.0;

        // Per-alias receipts. The FILL itself is engine save state (refAliasMap); this only carries
        // the LRU age and the PENDING marker. A slot is PENDING while `actorFid != 0` and the engine
        // does not (yet) report that actor in the alias: either our DoHold is in flight, or a
        // DoRelease of the previous occupant must land first (eviction) before DoHold may be sent.
        struct Slot {
            std::uint32_t actorFid     = 0;      // who we intend to hold there (engine truth may lag)
            double        heldAt       = 0.0;    // LRU age
            double        pendingSince = 0.0;    // 0 = confirmed by the engine
            bool          dispatched   = false;  // DoHold already sent for `actorFid`
        };
        Slot g_slots[kAliases];

        double NowS()
        {
            using namespace std::chrono;
            static const auto t0 = steady_clock::now();
            return duration<double>(steady_clock::now() - t0).count();
        }

        void Dispatch(const char* fn, std::int32_t a, std::int32_t b, std::int32_t c, bool three)
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            RE::BSScript::IFunctionArguments* args = three
                ? RE::MakeFunctionArguments(std::int32_t(a), std::int32_t(b), std::int32_t(c))
                : RE::MakeFunctionArguments(std::int32_t(a), std::int32_t(b));
            vm->DispatchStaticCall("PPB_DeviceEquip", fn, args, cb);
            delete args;
        }

        // Who the ENGINE says is in alias i right now (survives loads; the source of truth).
        // GetAliasedRef takes the quest's aliasAccessLock — the VM thread's ForceRefTo/Clear mutate
        // the same hash map and a bare find() during a rehash reads freed memory.
        std::uint32_t EngineHeld(int i)
        {
            if (!g_quest) return 0;
            auto h = g_quest->GetAliasedRef(static_cast<std::uint32_t>(i));
            auto p = h.get();
            return p ? p->GetFormID() : 0;
        }

        bool IsPending(const Slot& s, std::uint32_t eng) { return s.actorFid != 0 && eng != s.actorFid; }

        // Another RUNNING quest holds her. Presence of the extra alone is not the answer: our own
        // Clear() may leave the array on the reference (empty or not), and po3's IsQuestItem
        // iterates the same way. Entries are (quest, alias, packages).
        bool HeldByAnotherQuest(RE::Actor* a)
        {
            if (!a) return false;
            auto* x = a->extraList.GetByType<RE::ExtraAliasInstanceArray>();
            if (!x) return false;
            RE::BSReadLockGuard guard(x->lock);
            for (auto* d : x->aliases) {
                if (d && d->quest && d->quest != g_quest && d->quest->IsRunning()) return true;
            }
            return false;
        }

        // The ONLY correct death test in this codebase (CapFix.cpp PpbTrulyDead): Actor::IsDead()
        // reports kRestrained / bleedout as dead, and a restrained generic NPC is exactly who the
        // pool exists for.
        bool TrulyDead(RE::Actor* a)
        {
            if (!a) return false;
            const auto l = a->GetLifeState();
            return l == RE::ACTOR_LIFE_STATE::kDead || l == RE::ACTOR_LIFE_STATE::kDying;
        }

        const char* NameOf(RE::Actor* a)
        {
            const char* n = a ? a->GetDisplayFullName() : nullptr;
            return n ? n : "?";
        }
    }

    void Resync()
    {
        if (!g_quest) return;
        const double now = NowS();
        int n = 0;
        for (int i = 0; i < kAliases; ++i) {
            const std::uint32_t fid = EngineHeld(i);
            g_slots[i] = Slot{};
            g_slots[i].actorFid = fid;
            g_slots[i].heldAt   = fid ? now : 0.0;   // unknown age after a load: treat as fresh
            if (fid) ++n;
        }
        logger::info("HOLDPOOL: resync from the engine — {} of {} aliases holding an actor", n, kAliases);
    }

    void ClearOnLoad()
    {
        for (auto& s : g_slots) s = Slot{};
        g_lastSweep = 0.0;
    }

    void Install()
    {
        if (g_installed) return;
        g_installed = true;
        auto* dh = RE::TESDataHandler::GetSingleton();
        g_quest = dh ? dh->LookupForm<RE::TESQuest>(kQuestLocal, kPlugin) : nullptr;
        if (!g_quest) {
            logger::info("HOLDPOOL: quest 0x{:03X} in '{}' not found — the hold pool is INERT (the ESP record "
                         "has not been added yet; generic NPCs keep normal respawn behaviour)", kQuestLocal, kPlugin);
            return;
        }
        logger::info("HOLDPOOL: '{}' resolved ({} aliases in the record, running={})",
                     g_quest->GetFormEditorID() ? g_quest->GetFormEditorID() : "PPB_HoldPoolQuest",
                     g_quest->aliases.size(), g_quest->IsRunning() ? 1 : 0);
        Resync();
    }

    bool IsHeldByUs(RE::Actor* a)
    {
        if (!a || !g_quest) return false;
        const std::uint32_t fid = a->GetFormID();
        for (int i = 0; i < kAliases; ++i) {
            if (EngineHeld(i) == fid) return true;
            if (g_slots[i].actorFid == fid) return true;     // a hold of ours still in flight
        }
        return false;
    }

    int Count()
    {
        int n = 0;
        for (int i = 0; i < kAliases && g_quest; ++i) if (EngineHeld(i)) ++n;
        return n;
    }

    bool Hold(RE::Actor* a, const char* why)
    {
        if (!a) return false;
        if (!g_quest) {
            if (!g_saidAbsent) { g_saidAbsent = true; logger::info("HOLDPOOL: Hold requested but the pool is inert (no ESP record)"); }
            return false;
        }
        if (a->IsPlayerRef()) return false;
        const std::uint32_t fid = a->GetFormID();
        const char* nm = NameOf(a);
        if (auto* npc = a->GetActorBase(); npc && npc->IsUnique()) {
            logger::info("HOLDPOOL {:08X} '{}': UNIQUE — already persistent by nature, not pooled", fid, nm);
            return false;
        }
        const double now = NowS();
        if (IsHeldByUs(a)) {
            for (int i = 0; i < kAliases; ++i)
                if (EngineHeld(i) == fid || g_slots[i].actorFid == fid) g_slots[i].heldAt = now;   // refresh LRU
            return true;
        }
        if (HeldByAnotherQuest(a)) {
            logger::info("HOLDPOOL {:08X} '{}': another quest already holds her (alias present) — not pooled", fid, nm);
            return false;
        }
        // a truly free slot: the engine holds nobody AND no hold of ours is in flight there
        int slot = -1;
        for (int i = 0; i < kAliases; ++i)
            if (!EngineHeld(i) && g_slots[i].actorFid == 0) { slot = i; break; }
        if (slot >= 0) {
            Dispatch("DoHold", std::int32_t(g_quest->GetFormID()), slot, std::int32_t(fid), true);
            g_slots[slot] = Slot{ fid, now, now, true };
            logger::info("HOLDPOOL {:08X} '{}': HELD in alias {} ({}) — persistent once the engine confirms; "
                         "a cell respawn should no longer reset her", fid, nm, slot, why ? why : "");
            return true;
        }
        // pool full: release the least recently held CONFIRMED slot, and defer this hold until the
        // sweep sees that alias empty — never a Clear and a ForceRefTo on one alias in one frame.
        double oldest = 1e300; int oi = -1;
        for (int i = 0; i < kAliases; ++i) {
            if (IsPending(g_slots[i], EngineHeld(i))) continue;           // someone's hold is in flight
            if (g_slots[i].heldAt < oldest) { oldest = g_slots[i].heldAt; oi = i; }
        }
        if (oi < 0) {
            logger::info("HOLDPOOL {:08X} '{}': pool full and every alias is mid-transition — try again next equip", fid, nm);
            return false;
        }
        const std::uint32_t evicted = EngineHeld(oi);
        Dispatch("DoRelease", std::int32_t(g_quest->GetFormID()), oi, 0, false);
        g_slots[oi] = Slot{ fid, now, now, false };
        logger::info("HOLDPOOL: pool full — releasing alias {} (actor {:08X}, least recently held); hold of {:08X} '{}' "
                     "deferred until the alias reads empty ({})", oi, evicted, fid, nm, why ? why : "");
        return true;
    }

    void Release(RE::Actor* a, const char* why)
    {
        if (!a || !g_quest) return;
        const std::uint32_t fid = a->GetFormID();
        for (int i = 0; i < kAliases; ++i) {
            const std::uint32_t eng = EngineHeld(i);
            if (eng != fid && g_slots[i].actorFid != fid) continue;
            if (eng == fid) Dispatch("DoRelease", std::int32_t(g_quest->GetFormID()), i, 0, false);
            g_slots[i] = Slot{};
            logger::info("HOLDPOOL {:08X}: released alias {} ({})", fid, i, why ? why : "");
        }
    }

    void OnFrame()
    {
        if (!g_quest) return;
        const double now = NowS();
        if (now - g_lastSweep < 1.0) return;
        g_lastSweep = now;
        for (int i = 0; i < kAliases; ++i) {
            Slot& s = g_slots[i];
            const std::uint32_t eng = EngineHeld(i);

            if (IsPending(s, eng)) {
                if (!s.dispatched) {
                    if (eng == 0) {                                   // the eviction's Clear has landed
                        Dispatch("DoHold", std::int32_t(g_quest->GetFormID()), i, std::int32_t(s.actorFid), true);
                        s.dispatched = true; s.pendingSince = now;
                        logger::info("HOLDPOOL {:08X}: deferred hold dispatched into alias {}", s.actorFid, i);
                    } else if (now - s.pendingSince > kPendingS) {
                        logger::info("HOLDPOOL: alias {} still holds {:08X} {:.0f}s after its release was sent — "
                                     "dropping the deferred hold of {:08X}", i, eng, now - s.pendingSince, s.actorFid);
                        s = Slot{ eng, now, 0.0, false };            // keep the engine's truth as the receipt
                    }
                    continue;
                }
                if (now - s.pendingSince > kPendingS) {
                    logger::info("HOLDPOOL: alias {} expected {:08X} but the engine holds {:08X} — hold did not land "
                                 "(quest not running? alias not optional?); dropping the receipt", i, s.actorFid, eng);
                    s = eng ? Slot{ eng, now, 0.0, false } : Slot{};
                }
                continue;
            }

            if (eng == 0) { if (s.actorFid) s = Slot{}; continue; }   // released elsewhere (Clear landed)

            if (s.actorFid == 0) { s = Slot{ eng, now, 0.0, false }; }  // engine holds someone we had no receipt for
            else if (s.pendingSince != 0.0) {
                s.pendingSince = 0.0;                                 // confirmed by the engine
                logger::info("HOLDPOOL {:08X}: alias {} confirmed by the engine — she is persistent now", eng, i);
            }

            auto* f = RE::TESForm::LookupByID(eng);
            auto* a = f ? f->As<RE::Actor>() : nullptr;
            const char* reason = !a ? "actor gone" : (TrulyDead(a) ? "dead — let her respawn" : nullptr);
            if (!reason) continue;
            Dispatch("DoRelease", std::int32_t(g_quest->GetFormID()), i, 0, false);
            logger::info("HOLDPOOL {:08X}: released alias {} ({})", eng, i, reason);
            s = Slot{};
        }
    }
}
