#pragma once
// ═══ OUTFIT TAG — "make the engine think this is her armour" (user, 2026-09-05/06) ══════════════
//
// Option A of the permanent-equip research (report 29 §6 + the 09-05 discussion). The engine marks
// every inventory entry that came from the NPC's outfit with ExtraOutfitItem (its `id` = the
// BGSOutfit). Those tags are what Actor::RemoveOutfitItems strips on an outfit change, and what
// the engine's DefaultOutfit re-dress keys on when it re-equips her after an unequip. Inventory
// extra data is SAVE state (a per-reference changeform), so a tag placed at runtime survives cell
// loads and reloads with no registry, no ESP record and no re-equip loop.
//
// What it does, per gesture-equipped ORDINARY piece on a non-player NPC, after "confirmed worn":
//   1. TAG the new piece's worn inventory entry as an outfit item of her CURRENT default outfit.
//   2. UNTAG the piece(s) it displaced in the same biped slots — so the engine no longer considers
//      the old piece part of her outfit and stops re-dressing her in it.
//   3. (knob equipRemoveDisplaced, default OFF) additionally REMOVE the displaced piece from her
//      inventory. Kept off until VR says whether the untag alone is enough.
//
// ⚠ HONEST CONFIDENCE (~70%, see report 29): the tag theory of the AI re-dress is inferred from
// SeverActions' as-built comments and the engine's RemoveOutfitItems, not read out of the exe. The
// project's own KNOWLEDGEBASE ("Outfit Cascade") shows at least ONE engine path that checks the
// OUTFIT RECORD'S ITEM LIST rather than the tags. Both levers are therefore knob-gated and logged;
// tomorrow's VR test decides which one is load-bearing. Nothing here touches a base record.
//
// What it does NOT do: survive `resetinventory` (rebuilds from the record — only a record can
// answer that, see the parked option B) or a cell RESPAWN on a non-persistent actor (HoldPool.h).
namespace OutfitTag
{
    // Tag (on=true) or untag (on=false) the WORN inventory entry of `base` on `actor` as belonging
    // to her current default outfit. Returns true if a list was changed. Main thread only.
    bool Set(RE::Actor* actor, RE::TESBoundObject* base, bool on);

    // Remove one instance of `base` from her inventory (the displaced piece), silently.
    bool RemoveDisplaced(RE::Actor* actor, RE::TESBoundObject* base);

    // Diagnostic: is this entry currently tagged? (-1 = no such entry / no worn list)
    int  IsTagged(RE::Actor* actor, RE::TESBoundObject* base);
}
