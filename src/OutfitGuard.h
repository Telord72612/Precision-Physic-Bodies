#pragma once
// ═══ OUTFIT GUARD — a PPB-managed generic NPC is never re-issued her outfit (2026-09-06) ═══
//
// User: "So if a NPC is naked, the engine gonna re-issue her clothes on cell load? Is there a
// way to fix that?" — Yes it does, and this is the fix.
//
// The engine's UpdateWornGear asks Actor::HasOutfitItems(defaultOutfit) — "does she own ANY
// inventory entry tagged as part of her outfit?" — and when the answer is no it rebuilds the
// whole outfit from the record (InventoryChanges::InitOutfitItems) and equips it
// (AddWornOutfit). Strip a generic naked and she is re-dressed on the next 3D load / AI
// evaluation. With SPID VR installed the identical decision runs inside SPID's AddWornOutfit
// hook, which calls the same engine function — so the function ENTRY is the one place both
// paths pass through.
//
// Hook: a 5-byte JMP at Actor::HasOutfitItems (VR: address-library ID 19265 -> 0x29F420) to a
// thunk that answers TRUE for any actor the hold pool holds (the marker of "PPB dresses /
// undresses this generic" — no new state, it is the alias fill) and otherwise runs the original
// through a trampoline that re-executes the stolen prologue. The prologue is READ AT RUNTIME
// (the exe's .text is DRM-encrypted on disk) and accepted only if it decodes as whole,
// position-independent instructions covering >= 5 bytes (mov/push/sub/xor forms MSVC emits);
// anything else logs the bytes and installs NOTHING — fail closed (report 31 §8).
//
// Unique NPCs on the engine path (no SeverActions preset) are NOT covered by this marker yet;
// SeverActions blanks a follower's outfit on its own undress. Knob `outfitGuard` (ships 1).
namespace OutfitGuard
{
    void Install();   // kDataLoaded, after HoldPool::Install
}
