#include "PCH.h"
#include "OutfitTag.h"

namespace logger = SKSE::log;

namespace OutfitTag
{
    namespace
    {
        // The WORN instance's extra list for `base`, or nullptr. A stack can carry several lists;
        // the worn one is the list with kWorn / kWornLeft. If the piece is not worn there is no
        // honest list to tag — the engine only tags worn outfit items.
        RE::ExtraDataList* WornList(RE::Actor* actor, RE::TESBoundObject* base, RE::InventoryEntryData** entryOut)
        {
            if (entryOut) *entryOut = nullptr;
            auto* inv = actor ? actor->GetInventoryChanges() : nullptr;
            if (!inv || !inv->entryList) return nullptr;
            for (auto* entry : *inv->entryList) {
                if (!entry || entry->object != base) continue;
                if (entryOut) *entryOut = entry;
                if (!entry->extraLists) return nullptr;
                for (auto* xl : *entry->extraLists) {
                    if (!xl) continue;
                    if (xl->HasType(RE::ExtraDataType::kWorn) || xl->HasType(RE::ExtraDataType::kWornLeft))
                        return xl;
                }
                return nullptr;
            }
            return nullptr;
        }
    }

    bool Set(RE::Actor* actor, RE::TESBoundObject* base, bool on)
    {
        if (!actor || !base) return false;
        auto* npc = actor->GetActorBase();
        auto* outfit = npc ? npc->defaultOutfit : nullptr;
        const char* nm = base->GetName() ? base->GetName() : "?";
        if (on && !outfit) {
            // No default outfit = nothing for the engine to re-dress her from; the tag would point
            // at nothing. preventUnequip carries this case on its own. Say so once per actor.
            logger::info("OUTFITTAG {:08X}: '{}' — actor has NO default outfit; tag skipped (nothing re-dresses her)",
                         actor->GetFormID(), nm);
            return false;
        }
        RE::InventoryEntryData* entry = nullptr;
        auto* xl = WornList(actor, base, &entry);
        if (!xl) {
            logger::info("OUTFITTAG {:08X}: '{}' has no WORN extra list (entry {}) — cannot {}",
                         actor->GetFormID(), nm, entry ? "present" : "absent", on ? "tag" : "untag");
            return false;
        }
        if (on) {
            if (auto* have = xl->GetByType<RE::ExtraOutfitItem>()) {
                if (have->id == outfit->GetFormID()) return false;      // already exactly right
                have->id = outfit->GetFormID();                          // re-point a stale tag
                logger::info("OUTFITTAG {:08X}: '{}' re-pointed to outfit 0x{:08X}", actor->GetFormID(), nm, outfit->GetFormID());
                return true;
            }
            auto* tag = RE::BSExtraData::Create<RE::ExtraOutfitItem>();
            if (!tag) return false;
            tag->id = outfit->GetFormID();
            xl->Add(tag);
            logger::info("OUTFITTAG {:08X}: '{}' TAGGED as an item of outfit 0x{:08X} — the engine now treats it as her outfit",
                         actor->GetFormID(), nm, outfit->GetFormID());
            return true;
        } else {
            if (!xl->HasType(RE::ExtraDataType::kOutfitItem)) return false;
            xl->RemoveByType(RE::ExtraDataType::kOutfitItem);
            logger::info("OUTFITTAG {:08X}: '{}' UNTAGGED — no longer part of her outfit", actor->GetFormID(), nm);
            return true;
        }
    }

    bool RemoveDisplaced(RE::Actor* actor, RE::TESBoundObject* base)
    {
        if (!actor || !base) return false;
        actor->RemoveItem(base, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        logger::info("OUTFITTAG {:08X}: displaced '{}' REMOVED from her inventory (equipRemoveDisplaced)",
                     actor->GetFormID(), base->GetName() ? base->GetName() : "?");
        return true;
    }

    int IsTagged(RE::Actor* actor, RE::TESBoundObject* base)
    {
        auto* xl = WornList(actor, base, nullptr);
        if (!xl) return -1;
        return xl->HasType(RE::ExtraDataType::kOutfitItem) ? 1 : 0;
    }
}
