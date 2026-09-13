#pragma once
// ═══ HOLD POOL — make a GENERIC NPC persistent so a cell respawn cannot reset her (2026-09-06) ═══
//
// User: "if the issue is that a generic NPC will get their inventory reset eventually, but
// persistent NPCs don't, can we give an NPC we edited the inventory of a persistent flag?"
//
// Persistence in Skyrim is a RELATIONSHIP, not a flag: a reference becomes persistent while a
// quest alias holds it (that is how followers, quest targets and DoM's captives survive cell
// resets — DoM's own line is `EssentialRef.ForceRefTo(clone)`). The kPersistent form flag exists
// but the cell keeps SEPARATE lists of persistent and temporary references built at load, and no
// mod in this load order flips the flag at runtime — every one of them uses an alias.
//
// So: `Precision Physic Bodies.esp` carries `PPB_HoldPoolQuest` (FormID 0x806, start-game-enabled,
// no scripts) with 32 empty reference aliases PPB_Hold00..31 (alias IDs 0..31, each OPTIONAL — an
// empty script-forced alias must be Optional or the quest never starts and Clear() errors).
// When the gesture equips ORDINARY gear on a NON-unique NPC that nobody else already holds, PPB
// fills a free alias with her through the Papyrus helper (`ReferenceAlias.ForceRefTo` — plain-int
// dispatch, the object-free path the VM accepts). From then on the cell reset skips her and her
// inventory is save state. The alias fill itself is save state (TESQuest::refAliasMap), so there
// is NO co-save: the engine remembers who is held; Resync() re-reads it after a load.
//
// Skipped on purpose: the player; UNIQUE actors (already persistent by nature — every named
// follower and townsperson); anyone ANOTHER running quest already holds (ExtraAliasInstanceArray
// entries whose quest is not ours — DoM/YtM captives, SeverActions-managed followers).
// Bounded: pool size = the alias count (32). When full, the LEAST RECENTLY held is released and
// the new hold is DEFERRED to the next sweep, once the engine reports that alias empty (the two
// Papyrus calls are independent stacks; never race a Clear against a ForceRefTo on one alias).
// A held actor who truly dies (life state kDead/kDying — NOT Actor::IsDead(), which reads TRUE for
// a restrained or bleeding-out live NPC) is released on the next sweep; one who is gone is too.
// Cost: one tiny changeform per held actor; the sweep is 1 Hz and touches only the pool.
//
// Inert by construction until the ESP record exists: LookupForm(0x806) null -> one log line,
// every Hold() returns false, nothing else changes.
namespace HoldPool
{
    void Install();                                  // kDataLoaded: resolve the quest, log the verdict
    void Resync();                                   // kPostLoadGame: rebuild the receipts from the engine's alias map
    void ClearOnLoad();                              // kPreLoadGame / kNewGame: drop every receipt (FormIDs recycle)
    bool Hold(RE::Actor* actor, const char* why);    // main thread; false = not eligible / pool absent
    void Release(RE::Actor* actor, const char* why); // main thread; no-op if not held by us
    void OnFrame();                                  // 1 Hz sweep: deferred holds, confirmations, dead / gone -> release
    bool IsHeldByUs(RE::Actor* actor);               // engine-held OR a hold of ours still in flight
    int  Count();
}
