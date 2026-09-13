#include "PCH.h"
#include "DeviceGesture.h"
#include "OutfitTag.h"
#include "HoldPool.h"
#include "PushStep.h"   // v29d: NoteEquipEvent — an equip changes her geometry; it must not read as a lift or a push
#include "PpbTouchAPI.h"
#include "PpbApi.h"   // PpbApi::CopyContacts — the snapshot, read internally now
#include "HiggsInterface.h"
#include "Interop.h"   // Interop::GetHiggs - PPB's ONE shared HIGGS handshake
#include "Ini.h"       // PPB.ini - the general settings file (born for bSlotOccupiedRefuse)
#include "VrteDdzGateAPI.h"   // the DD/ZaZ AddOn's clothing-gate vtable (green-lit 2026-08-30)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace logger = SKSE::log;

namespace {

// Defined further down, beside the rest of the event plumbing; declared here because the equip
// path calls it and sits earlier in the file.
void SendGestureEvent(const char* name, const std::string& strArg, float numArg, RE::TESForm* sender);

// ─────────────────────────────────────────────────────────────────────────────
// Config: SKSE/Plugins/PPB_tuning.txt (the [gesture] block), re-read ~1 Hz so the gesture can be
// dialled in VR without a restart.
// ─────────────────────────────────────────────────────────────────────────────
struct Cfg {
    float enabled        = 1.f;
    float equipEnabled   = 1.f;
    // Orifice insertion gate, used ONLY when PPB's Orifice module is off
    // (orificeKind == 0). When it is on, PPB's between-the-twins penetration
    // gate is the insertion proof and no distance test of ours is consulted.
    // d <= 0 means the device's surface has met a sensor that lives INSIDE the
    // body: that IS insertion. equipDwellS is what makes the gesture deliberate.
    float equipDepthU    = 0.0f;
    // Surface-site press gate (gag -> lips, cuff -> wrist/ankle). A PPB contact
    // exists from ~1u away (hover), so contact existence is NOT a press; this is
    // the "actually touching" line. distU <= 0 = surfaces met.
    float equipTouchU    = 0.0f;
    float equipDwellS    = 1.0f;   // held in place this long. "Really deliberate."
    // Dwell grace: the digest's reported capsule FLICKERS. Measured 2026-08-22
    // 09:52:31.939 — a gag genuinely in her mouth read 'Face(cheek L)' for one
    // frame and the hard reset zeroed 1.55s of earned dwell, so the release
    // found 0.5s and refused. A gap shorter than this PAUSES the dwell instead
    // of resetting it; only a real departure (or another actor) resets.
    float equipGraceS    = 0.4f;
    float equipMatchSite = 1.f;    // a vaginal plug only equips vaginally, etc.
    float equipNotify    = 0.f;   // 2.2.0: OFF — the floating "X equipped" / "She is already wearing X" lines were
                                  // debug feedback (user: "don't want to spam the player"). 1 = show them again.
    // Ordinary (non-device) clothing, armor and jewellery may be equipped by
    // the same press-and-release gesture. 0 = devices only.
    float equipGeneric   = 1.f;
    // Finger-in-orifice extraction: hold a bare finger inside an orifice that
    // has a plug in it for this long, and the plug comes out. 0 disables.
    float plugPullS      = 1.0f;
    // Where an extracted plug goes. 1 = into the extracting hand (pointing in
    // VR is grip-held/trigger-released, i.e. already a grab-ready hand, so
    // HIGGS can take it); 0 = let it fall where it was.
    float plugToHand     = 1.f;
    // ⛔ How long to wait for DD to finish taking its RENDERED half off before
    // we put the freed device in the world. DD DELETES a device dropped out of
    // an actor who still reads as wearing it (zadEquipScript.psc:389-391), so
    // dropping on the modevent frame destroys the item inside the player's
    // hand. See the banner over PendingSafeDrop. 0 = drop immediately (the old,
    // broken behaviour, kept only as an escape hatch).
    float ddSettleS      = 0.75f;
    // -- the two-hand undress --
    float undressEnabled = 1.f;
    // How much the two hands must SPREAD APART (game units, beyond their
    // distance when the pair formed) before the piece comes off. Measured
    // between the two hand positions, so dragging her around - which moves
    // both hands together - cannot false-fire it.
    float undressPullU   = 25.f;
    // ★ How close the two hands must be TO EACH OTHER at the moment the pair
    // forms (user, 2026-08-22). Both hands start together on the gear, then
    // separate - so the pull is a real separation and not a grab that was
    // already wide open. Without it, one hand on a cuff and the other across
    // the room on her hip would already be past the pull threshold.
    float undressGrabMaxU = 10.f;
    // ⛔⛔ THE SNAP-BACK IS THE TRIGGER (user, 2026-08-24, and this is the whole
    // gesture): "I hold grab on both hand, i pull my right hand WHILE holding
    // grab. My VR hand stay on the NPC's body while my controller is moving
    // away more. At a certain distance, the right hand, even if i keep
    // grabbing, will release and SNAP back at the controller WHILE i still hold
    // grab, and than the gears is in my right hand. THIS is the un-equip
    // trigger."
    //
    // HIGGS holds the VIRTUAL hand on her body while the controller walks away;
    // past its own stretch limit HIGGS breaks the hold and the hand snaps to the
    // controller. That break is the gesture COMPLETING - and the code treated it
    // as "a hand let go of the grab" and cancelled, every single time.
    //
    // This is the only guard that separates the two readings of a lost grip:
    // how far the pulling CONTROLLER travelled since the pair formed. A stretch
    // break is preceded by a long travel; letting go of the button is not.
    // Low on purpose - it only has to reject an immediate release, and setting
    // it near HIGGS's stretch limit would re-create the bug it fixes.
    float undressSnapMinU = 8.f;
    float undressNotify  = 0.f;   // 2.2.0: OFF — "X removed" / "locked over it" / refusal lines (same ruling). 1 = show.
    // Return a device to the world when another mod (Gift by Hand) takes it
    // after a failed gesture. OFF by default: the user runs GBH deliberately
    // and wants its give to work (2026-08-23).
    float returnFailedGifts = 0.f;
    // ★ FORCE GRADE (AddOn ask SS3, 2026-08-30): peak press depth (u INTO the capsule)
    // during the dwell -> 0 gentle / 1 firm / 2 forced, appended LAST to both equip payloads.
    float equipForce1U   = 1.0f;   // firm at this depth
    float equipForce2U   = 3.0f;   // forced at this depth
    // ★ OPTION A (2026-09-06): after a confirmed ORDINARY equip on an NPC, tag the new piece
    // as an item of her default outfit and untag the piece(s) it displaced (OutfitTag.h).
    // SHIPS OFF (user 2026-09-06): Tier 0 (preventUnequip) + the hold pool are the design; the tag
    // is parked until VR shows a generic re-dressing herself after rip -> dress. Report 31 §2.3.
    float equipOutfitTag      = 0.f;
    // ... and additionally REMOVE the displaced piece from her inventory. OFF until VR says
    // the untag alone does not stop the engine re-dressing her in it.
    float equipRemoveDisplaced = 0.f;
    // ★ HOLD POOL (2026-09-06): hold a GENERIC NPC in PPB_HoldPoolQuest so a cell respawn
    // cannot reset the gear (HoldPool.h). Inert until the ESP record exists.
    float equipHoldPool       = 1.f;
    // ★ SEVERACTIONS / NFF HANDOFF (user 2026-09-06: "if SA has control of that NPC, record to its
    // outfit system" + "do NFF probe"). After a confirmed ORDINARY equip or a plain rip, PPB_DeviceEquip.DoOutfitRecord
    // asks SeverActions whether it CONTROLS her outfit (registered follower, not outfit-excluded,
    // an active preset or the lock on) and if so re-saves her worn set into that preset / lock —
    // GiftByHandSN's proven pattern. Soft dependency: no SeverActions = no-op.
    float equipSaHandoff      = 1.f;
    // ★ v28 RELEASE REACH (2026-09-10, user: "If i hold a piece of gears on a body long enought to
    // equip, but than move away and drop that piece of gears, will it still equip? That's wrong, it
    // need to drop on the table"). An EARNED gesture used to equip wherever it was released within
    // kEarnedHoldS (5 s). Now, on the last frame the hand still holds it, the device is measured
    // against the capsule that EARNED the gesture - by PPB's own contact formula
    // (PpbApi::HeldObjectGapU) - and it equips only within this reach; farther, it simply falls
    // where you let go. Fails closed (no measurement = no equip). 0 = the check is off.
    float equipReleaseReachU  = 6.f;

    float logLevel       = 0.f;
} g_cfg;

// ─────────────────────────────────────────────────────────────────────────────
// Equip sites. A device carries a MASK, not a single site: measured on
// zbfCuffsIronBlack (003007:ZaZAnimationPack.esm), one ZaZ cuff set carries
// BOTH zbfWornWrist and zbfWornAnkles — it is a wrist+ankle rig and must equip
// from either zone.
// ─────────────────────────────────────────────────────────────────────────────
enum SiteBit : int {
    kSiteVaginal = 1 << 0,    // plugs, labia/vaginal piercings
    kSiteAnal    = 1 << 1,    // plugs
    kSiteMouth   = 1 << 2,    // gags
    kSiteWrist   = 1 << 3,    // arm cuffs, bracers, elbow binders
    kSiteAnkle   = 1 << 4,    // leg cuffs, shackles, greaves
    kSiteNeck    = 1 << 5,    // collars, chokers, amulets, necklaces
    kSiteHead    = 1 << 6,    // hoods, helmets, circlets, hair
    kSiteEyes    = 1 << 7,    // blindfolds
    kSiteChest   = 1 << 8,    // bras, corsets, harnesses, yokes, cuirass, dresses
    kSiteNipple  = 1 << 9,    // nipple piercings / clamps
    kSiteWaist   = 1 << 10,   // belts, navel piercings
    kSiteHands   = 1 << 11,   // gloves, gauntlets
    kSiteFinger  = 1 << 12,   // rings
    kSiteFeet    = 1 << 13,   // boots, shoes
    // ★ THE TORSO SPLIT (AddOn ask SS1, 2026-08-30). kSiteChest used to mean BOTH "wraps the
    // torso" (corset/harness/bra) and "involves torso AND arms" (armbinder/yoke) - and the zone
    // map reaches kSiteChest from the NECK and UPPER ARM (needed for binders), so a corset held
    // at the throat equipped onto the ribs. kSiteTorso is granted only by the chest/belly/waist
    // capsules; the binder classes keep kSiteChest and its wide reach.
    kSiteTorso   = 1 << 14,   // torso WRAPS: harness, corset, bra
    kSiteAll     = 0x7FFF,
};

const char* ZoneName(int bit)
{
    switch (bit) {
    case kSiteVaginal: return "vaginal";
    case kSiteAnal:    return "anal";
    case kSiteMouth:   return "mouth";
    case kSiteWrist:   return "wrist";
    case kSiteAnkle:   return "ankle";
    case kSiteNeck:    return "neck";
    case kSiteHead:    return "head";
    case kSiteEyes:    return "eyes";
    case kSiteChest:   return "chest";
    case kSiteNipple:  return "nipple";
    case kSiteWaist:   return "waist";
    case kSiteHands:   return "hand";
    case kSiteFinger:  return "finger";
    case kSiteFeet:    return "foot";
    case kSiteTorso:   return "torso";
    }
    return "none";
}

// The lowest set site bit - for naming a multi-site zone in one word.
int FirstBit(int m)
{
    for (int b = 1; b <= kSiteTorso; b <<= 1) if (m & b) return b;
    return 0;
}

// "vaginal|anal" etc., for the one-shot classification log.
void MaskStr(int mask, char* out, std::size_t cap)
{
    out[0] = '\0';
    if (!mask) { std::snprintf(out, cap, "none"); return; }
    for (int b = 1; b <= kSiteTorso; b <<= 1) {
        if (!(mask & b)) continue;
        if (out[0]) std::strncat(out, "|", cap - std::strlen(out) - 1);
        std::strncat(out, ZoneName(b), cap - std::strlen(out) - 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Device classification.
//
// DD: the device-type keyword is NOT on the item the player holds — the
// INVENTORY half carries only zad_InventoryDevice (measured on 014916, report
// 23 §11). The real class lives in the VMAD script property
// `zadequipscript.zad_DeviousDevice`, read off the held REFERENCE the same way
// DD's own zadLibs.GetDeviceKeyword does (zadLibs.psc:712) — minus its
// placeAtMe, because we already have a reference.
//
// ZaZ: plain armors, NO script (measured: zbfGagBall01 002007 / zbfCuffsIronBlack
// 003007, VMAD absent), classified by zbfWorn* keywords sitting directly on the
// base form. Equipping them is a plain equip — the user's call, 2026-08-21.
//
// Cached per base form: both sources are immutable per device type, and a VM
// lookup per frame would be indefensible.
// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-08-23 (report 23 §30). `cls` and `renderedFid` are captured at
// classify time, off the world reference's zadEquipScript, because BOTH become
// unreachable the moment the item is equipped — and reading them back off the
// held base form gives the WRONG ANSWER rather than no answer:
//
//   A DD device is a PAIR, and they carry different things. Verified in the
//   shipping records:
//     zad_armBinderHisec_Rendered  -> zad_DeviousHeavyBondage, zad_DeviousArmbinder,
//                                     zad_Lockable   ·  no FULL name, biped slots set
//     zad_armBinderHisec_Inventory -> zad_InventoryDevice ONLY
//                                     ·  Name "High Security Armbinder", NO slots
//
//   So the half the player picks up and holds — which is what `pv.base` is —
//   carries no class keyword and no lock keyword at all. And scanning the
//   RENDERED half's keywords is not enough either: it carries several class
//   keywords, so "first one wins" can answer "Armbinder" where DD's own
//   zad_DeviousDevice property says "HeavyBondage".
struct DevClass {
    int           mask        = 0;
    bool          dd          = false;
    bool          generic     = false;
    char          cls[64]     = {};   // suffix of zad_DeviousDevice — DD's OWN answer
    std::uint32_t renderedFid = 0;    // deviceRendered — the half she actually WEARS
};
std::unordered_map<std::uint32_t, DevClass> g_classCache;

bool IsDdInventoryDevice(RE::TESBoundObject* base)
{
    auto* kwf = base ? base->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return false;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (kw && *kw) {
            const char* id = (*kw)->GetFormEditorID();
            if (id && _stricmp(id, "zad_InventoryDevice") == 0) return true;
        }
    }
    return false;
}

// ★ 2026-08-23 — one pass over a device's keywords for everything VRTE needs to
// describe it: which DD CLASS it is (the suffix after "Devious", so zad_ /
// zadNG_ / any content mod's prefix all collapse to one row), whether it LOCKS,
// and whether it is quest-bound. Facts only — every word the LLM sees is chosen
// in VRTE's TriggerLib, so the phrasing can be retuned without a rebuild.
void DdDescribe(RE::TESBoundObject* base, char* clsOut, std::size_t cap,
                bool& locked, bool& quest)
{
    if (cap) clsOut[0] = '\0';
    locked = false;
    quest  = false;
    auto* kwf = base ? base->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (!kw.has_value() || !kw.value()) continue;
        const char* id = kw.value()->GetFormEditorID();
        if (!id) continue;
        if (_stricmp(id, "zad_Lockable") == 0)   { locked = true; continue; }
        if (_stricmp(id, "zad_QuestItem") == 0 ||
            _stricmp(id, "zad_BlockGeneric") == 0) { quest = true; continue; }
        // the CLASS keyword: "...DeviousGag" -> "Gag". Skip the generic marker
        // keywords that also contain the word.
        const char* p = std::strstr(id, "Devious");
        if (!p) continue;
        const char* k = p + 7;
        if (!k[0] || _stricmp(k, "Device") == 0) continue;
        if (clsOut[0]) continue;                 // first real class wins
        std::snprintf(clsOut, cap, "%s", k);
    }
}

// "zad_DeviousHeavyBondage" -> "HeavyBondage". Empty for a keyword that is not
// a class marker. Same rule DdMask and DdDescribe use, factored out so the
// class string and the site mask can never disagree about what a keyword means.
void ClsSuffix(const char* id, char* out, std::size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (!id) return;
    const char* p = std::strstr(id, "Devious");
    if (!p) return;
    const char* k = p + 7;
    if (!k[0] || _stricmp(k, "Device") == 0) return;
    std::snprintf(out, cap, "%s", k);
}

// The DD device-CLASS keyword (from zad_DeviousDevice) -> site mask.
// Is this device class a PLUG? The three rows DdMask maps to an orifice as an INSERTED object —
// deliberately NOT every device that happens to sit at an orifice site (a gag takes kSiteMouth, a
// vaginal piercing takes kSiteVaginal, and neither is a plug). Kept adjacent to DdMask so the two
// tables cannot drift apart.
bool IsPlugClass(const char* id)
{
    if (!id || !id[0]) return false;
    const char* p = std::strstr(id, "Devious");
    const char* k = p ? p + 7 : id;
    return _stricmp(k, "Plug") == 0 || _stricmp(k, "PlugVaginal") == 0 ||
           _stricmp(k, "PlugAnal") == 0;
}

int DdMask(const char* id)
{
    if (!id) return 0;
    // Match on the SUFFIX after "Devious", so zad_ / zadNG_ / a content mod's
    // own prefix all land on the same row.
    const char* p = std::strstr(id, "Devious");
    const char* k = p ? p + 7 : id;

    if (_stricmp(k, "PlugVaginal") == 0)      return kSiteVaginal;
    if (_stricmp(k, "PlugAnal") == 0)         return kSiteAnal;
    if (_stricmp(k, "Plug") == 0)             return kSiteVaginal | kSiteAnal;
    if (_strnicmp(k, "Gag", 3) == 0)          return kSiteMouth;    // + GagInflatable
    if (_stricmp(k, "ArmCuffs") == 0)         return kSiteWrist;
    if (_stricmp(k, "LegCuffs") == 0)         return kSiteAnkle;
    if (_stricmp(k, "AnkleShackles") == 0)    return kSiteAnkle;
    if (_stricmp(k, "Collar") == 0)           return kSiteNeck;
    if (_stricmp(k, "Blindfold") == 0)        return kSiteEyes;
    if (_stricmp(k, "Hood") == 0)             return kSiteHead;
    if (_stricmp(k, "Bra") == 0)              return kSiteTorso;
    if (_stricmp(k, "Belt") == 0)             return kSiteWaist;
    if (_stricmp(k, "Boots") == 0)            return kSiteFeet;
    if (_stricmp(k, "Gloves") == 0)           return kSiteHands;
    if (_stricmp(k, "PiercingsNipple") == 0)  return kSiteNipple;
    if (_stricmp(k, "PiercingsVaginal") == 0) return kSiteVaginal;
    // Torso rigs. A corset is wrapped rather than placed, but the user's call
    // (2026-08-22) is that anything wearable should work; they take the chest
    // site and the dwell keeps them deliberate.
    // ★ 2026-08-30 (AddOn asks SS2 items 2/3/4, user-ruled): a corset equips at the BELLY and
    // WAIST only; a harness/bra anywhere on the torso WRAP - never from the neck or an arm.
    if (_stricmp(k, "Corset") == 0)           return kSiteWaist;
    if (_stricmp(k, "Harness") == 0)          return kSiteTorso;
    // ★ ARM BINDING TAKES THE WHOLE ARM (2026-08-24, user-reported: "Armbinder,
    // they are not supported? I try to add one to Carmella, didn't work").
    // It was never unsupported - zad_DeviousDevice resolves to
    // zad_DeviousHeavyBondage and this row has always existed. The SITE was the
    // problem: kSiteChest alone is reachable from upper arm / chest / neck /
    // belly / waist, but NOT from the hand or the forearm - and the hands are
    // exactly where a player brings an arm restraint. The gesture failed the
    // site match and Gift by Hand picked the armbinder up instead (measured:
    // "[GBH_Gifts] Telord put Black Waxed Armbinder in Carmella's Left hand").
    // Adding kSiteWrist makes hand + forearm + upper arm all valid - the same
    // widening ZoneOfContact already applied for gags, collars and gloves after
    // the identical class of measured refusal.
    if (_stricmp(k, "HeavyBondage") == 0)     return kSiteChest | kSiteWrist;
    // ★ 2026-08-23: the rest of DD's real class list, read out of
    // "Devious Devices - Integration.esm" itself rather than recalled. Every
    // one of these previously returned 0 and fell through to the biped-slot
    // guess, which works but cannot tell a hobble skirt from a dress.
    if (_stricmp(k, "Armbinder") == 0)        return kSiteChest | kSiteWrist;
    if (_stricmp(k, "ArmbinderElbow") == 0)   return kSiteChest | kSiteWrist;
    if (_stricmp(k, "StraitJacket") == 0)     return kSiteChest | kSiteWrist;
    // ★ 2026-08-30 (asks 5/6): an elbow tie is not a torso garment - arms only. The
    // collared variants get +neck in ClassOfHeld (needs the rendered half's keywords).
    if (_stricmp(k, "ElbowTie") == 0)         return kSiteWrist;
    if (_stricmp(k, "PetSuit") == 0)          return kSiteChest;
    if (_stricmp(k, "PonyGear") == 0)         return kSiteChest;
    if (_stricmp(k, "BondageMittens") == 0)   return kSiteHands;
    if (_stricmp(k, "CuffsFront") == 0)       return kSiteWrist;
    if (_stricmp(k, "CuffsArms") == 0)        return kSiteWrist;
    if (_stricmp(k, "CuffsLegs") == 0)        return kSiteAnkle;
    if (_strnicmp(k, "HobbleSkirt", 11) == 0) return kSiteWaist;   // + ...Relaxed
    if (_stricmp(k, "Clamps") == 0)           return kSiteNipple;
    if (_stricmp(k, "Suit") == 0)             return kSiteChest;
    if (_strnicmp(k, "Yoke", 4) == 0)         return kSiteChest | kSiteWrist;    // NG YokeFront
    if (_stricmp(k, "Boxbinder") == 0)        return kSiteChest | kSiteWrist;    // NG
    // ★ 2026-08-30: DD's Butterfly is an ARM BINDER (arms folded behind, elbows out) - the
    // vaginal row was an NG guess. Safe whichever token zad_DeviousDevice holds: if it says
    // Armbinder this row never fires; if it says Butterfly it now answers like one.
    if (_stricmp(k, "Butterfly") == 0)        return kSiteChest | kSiteWrist;  // NG
    return 0;
}

// Class keywords sitting directly on the base form -> site mask (OR of all
// that match). Two families, same idea:
//   ZaZ:            zbfWorn*  (note the plural "Ankles")
//   Diary of Mine:  DOMWorn*  (note the singular "Ankle", and "PlugVaginal" —
//                   site AFTER "Plug", the reverse of ZaZ's order). Measured
//                   2026-08-22 on DOMCuffsRope (EB3554:DiaryOfMine.esm), the
//                   'Cuffs Rope' that classified as "not a device" in the
//                   first live run.
int PlainMask(RE::TESBoundObject* base)
{
    auto* kwf = base ? base->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return 0;
    int mask = 0;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (!kw || !*kw) continue;
        const char* id = (*kw)->GetFormEditorID();
        if (!id) continue;
        // Both families use a "<prefix>Worn<Part>" shape, so match on the tail.
        // ZaZ: zbfWorn*  (plural "Ankles")    DOM: DOMWorn*  (singular "Ankle",
        // and "PlugVaginal" - site AFTER Plug, the reverse of ZaZ's order).
        const char* w = std::strstr(id, "Worn");
        if (!w) continue;
        w += 4;
        if      (_stricmp(w, "VaginalPlug") == 0 ||
                 _stricmp(w, "PlugVaginal") == 0)     mask |= kSiteVaginal;
        else if (_stricmp(w, "AnalPlug") == 0 ||
                 _stricmp(w, "PlugAnal") == 0)        mask |= kSiteAnal;
        else if (_stricmp(w, "Plug") == 0 ||
                 _stricmp(w, "SlipperyPlug") == 0)    mask |= kSiteVaginal | kSiteAnal;
        else if (_stricmp(w, "Gag") == 0)             mask |= kSiteMouth;
        else if (_stricmp(w, "Wrist") == 0 ||
                 _strnicmp(w, "Cuffs", 5) == 0 ||     // DOM CuffsFront/Back/Crossed/Boxtied
                 _stricmp(w, "Elbows") == 0)          mask |= kSiteWrist;
        else if (_stricmp(w, "Ankles") == 0 ||
                 _stricmp(w, "Ankle") == 0)           mask |= kSiteAnkle;
        else if (_strnicmp(w, "Collar", 6) == 0)      mask |= kSiteNeck;    // + CollarLeash
        else if (_stricmp(w, "Blindfold") == 0)       mask |= kSiteEyes;
        else if (_stricmp(w, "Hood") == 0)            mask |= kSiteHead;
        else if (_stricmp(w, "Bra") == 0)             mask |= kSiteTorso;
        else if (_stricmp(w, "Belt") == 0 ||
                 _stricmp(w, "Waist") == 0 ||
                 _stricmp(w, "PiercingNavel") == 0)   mask |= kSiteWaist;
        else if (_stricmp(w, "NippleClamps") == 0 ||
                 _stricmp(w, "PiercingNipple") == 0)  mask |= kSiteNipple;
        else if (_stricmp(w, "PiercingLabia") == 0)   mask |= kSiteVaginal;
        else if (_stricmp(w, "PiercingNose") == 0)    mask |= kSiteHead;
        else if (_stricmp(w, "Yoke") == 0 ||
                 _stricmp(w, "Armbinder") == 0 ||
                 _stricmp(w, "CrossPole") == 0 ||
                 _stricmp(w, "HandsTiedToNeck") == 0) mask |= kSiteChest;
    }
    return mask;
}

// Ordinary gear - no device keywords at all. Classified purely by the biped
// slots the armor occupies, so a necklace goes on the neck, a ring on a finger
// and boots on the feet, with no per-mod table. User request 2026-08-22.
// Non-armor (weapons, food, potions, misc) has no slot mask and returns 0, so
// the gesture can never fire on them.
int GenericMask(RE::TESBoundObject* base)
{
    auto* armo = base ? base->As<RE::TESObjectARMO>() : nullptr;
    if (!armo) return 0;
    const std::uint32_t slots = static_cast<std::uint32_t>(armo->GetSlotMask().underlying());
    if (!slots) return 0;
    const auto has = [&](int sl) { return (slots & (1u << (sl - 30))) != 0; };
    int mask = 0;
    if (has(30) || has(31) || has(41) || has(42) || has(43)) mask |= kSiteHead;
    if (has(44))                                  mask |= kSiteMouth;   // gags, face masks
    if (has(55))                                  mask |= kSiteEyes;    // blindfolds
    if (has(35) || has(45))                       mask |= kSiteNeck;    // amulets, collars
    // ★ SLOT 58 MOVED HERE (2026-08-30, measured): 219 of its carriers are 185 Harness +
    // 28 Corset + 6 unclassified corsets/harnesses - DD convention uses 58 for the torso
    // SHELL. It was read as wrist (and the AddOn read it as neck); the data says torso.
    if (has(32) || has(46) || has(47) || has(56) || has(58))
        mask |= kSiteChest | kSiteTorso;
    if (has(51))                                  mask |= kSiteNipple;
    if (has(49) || has(52))                       mask |= kSiteWaist;
    if (has(33))                                  mask |= kSiteHands;
    if (has(36))                                  mask |= kSiteFinger;
    if (has(34) || has(59))                       mask |= kSiteWrist;
    if (has(37))                                  mask |= kSiteFeet;
    if (has(38) || has(53) || has(54))            mask |= kSiteAnkle;
    // * THE PLUG SLOTS (user, 2026-08-23: "look at which slot they equip on
    // instead of just their name, some DD equipment do not even have proper
    // name"). The DD convention is 57 vaginal / 48 anal, and content mods
    // follow it even carrying no DD keyword at all - measured on 'Carrot Anal
    // Plug' (09557D:zRavenous.esp), FirstPersonFlags 262144 = slot 48 alone,
    // which classified as "not wearable" before this.
    if (has(57))                                  mask |= kSiteVaginal;
    if (has(48))                                  mask |= kSiteAnal;
    return mask;
}

// Is this base actually WORN by her right now? Checked after every equip so the
// mod never claims a success it did not achieve - 2026-08-22 a Ball Strap Gag
// reported "equipped" and fell on the floor, because she already wore a gag and
// DD refuses two devices of one class.
bool IsWornNow(RE::Actor* a, RE::TESBoundObject* base)
{
    auto* armo = base ? base->As<RE::TESObjectARMO>() : nullptr;
    if (!a || !armo) return false;
    const std::uint32_t slots = static_cast<std::uint32_t>(armo->GetSlotMask().underlying());
    if (!slots) return true;       // nothing to check against - cannot disprove
    for (int b = 0; b < 32; ++b) {
        if (!(slots & (1u << b))) continue;
        if (a->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << b)) == armo)
            return true;
    }
    return false;
}

// ★ PER-DEVICE SITE OVERRIDES (2026-08-30): Data/SKSE/Plugins/PPB_deviceSites.txt,
// "formid~plugin = site|site" (names as in ZoneName, plus "all"), keyed on the RENDERED
// half's defining FormID (the AddOn's table key). Loaded once per launch on first use;
// resolved through TESDataHandler so load order cannot shift them.
std::unordered_map<std::uint32_t, int> g_siteOverride;
bool g_siteOverrideLoaded = false;

int SiteBitByName(const char* n)
{
    for (int b = 1; b <= kSiteTorso; b <<= 1)
        if (_stricmp(n, ZoneName(b)) == 0) return b;
    if (_stricmp(n, "all") == 0) return kSiteAll;
    return 0;
}

void LoadSiteOverrides()
{
    if (g_siteOverrideLoaded) return;
    g_siteOverrideLoaded = true;
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "Data/SKSE/Plugins/PPB_deviceSites.txt", "r") != 0 || !f) {
        logger::info("[EQUIP] no PPB_deviceSites.txt - per-device site overrides off");
        return;
    }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char fidHex[16] = {}, plugin[128] = {}, sites[128] = {};
        if (std::sscanf(line, " %8[0-9A-Fa-fxX] ~ %127[^=] = %127[^\r\n#]", fidHex, plugin, sites) != 3)
            continue;
        // trim trailing spaces off the plugin name
        for (int i = (int)std::strlen(plugin) - 1; i >= 0 && plugin[i] == ' '; --i) plugin[i] = '\0';
        const std::uint32_t local = (std::uint32_t)std::strtoul(fidHex, nullptr, 16);
        auto* form = dh->LookupForm(local, plugin);
        if (!form) {
            logger::info("[EQUIP] deviceSites: {}~{} not found in the load order - line skipped",
                         fidHex, plugin);
            continue;
        }
        int mask = 0;
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", sites);
        for (char* tok = std::strtok(buf, "| \t"); tok; tok = std::strtok(nullptr, "| \t"))
            mask |= SiteBitByName(tok);
        if (!mask) continue;
        g_siteOverride[form->GetFormID()] = mask;
        ++n;
    }
    fclose(f);
    logger::info("[EQUIP] deviceSites: {} per-device site override(s) loaded", n);
}

bool FormHasKeyword(RE::TESForm* f, const char* name)
{
    auto* kwf = f ? f->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return false;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (kw && *kw) {
            const char* id = (*kw)->GetFormEditorID();
            if (id && _stricmp(id, name) == 0) return true;
        }
    }
    return false;
}

// ═══ THE SLOT-OCCUPIED RULE (user, 2026-08-30) ══════════════════════════════════════════════
// Vanilla behaviour for equipping onto an occupied biped slot is a silent SWAP - the old piece
// teleports into her inventory. The user's rule: a gesture equip onto an occupied slot REFUSES
// exactly like a DD refusal - the piece falls from the hand, with the blocker named.
// Carve-out (established doctrine, AddOn spec §4.2): a framework DEVICE never blocks another
// framework device - belt-over-plug is DD's own mechanic and DD arbitrates device layering.
// Toggle: PPB.ini [Equip] bSlotOccupiedRefuse (default ON; hot, ~1s).
bool IsFrameworkDevice(RE::TESObjectARMO* armo)
{
    auto* kwf = armo ? armo->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return false;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (!kw || !*kw) continue;
        const char* id = (*kw)->GetFormEditorID();
        if (!id) continue;
        if (_strnicmp(id, "zad", 3) == 0 || _strnicmp(id, "zbf", 3) == 0 ||
            std::strncmp(id, "DOM", 3) == 0) return true;
    }
    return false;
}

// ═══ THE CLOTHING GATE (user, 2026-08-30: "i installed a plug while she was dressed, should
// not happen") ═══════════════════════════════════════════════════════════════════════════════
// PPB-side implementation of the AddOn's §4 model. ⚠ Their doc asks us to call their natives
// (VRTE_DDZaZ_Equip.CanEquipOn) and to WAIT for their green light; this is the local stand-in
// so the user is not blocked, and it is knob-gated so it can be swapped for their call later
// without touching the gesture flow.
//
// THE MODEL (their §4.1, the user's own rule): a device is refused when a NON-DEVICE garment
// already occupies ITS OWN REGION. "Clothed" is PER-REGION, not "wearing a body slot" — a
// cuirass on slot 32 must not block a wrist cuff, only gloves do.
// ⚠ A framework device NEVER blocks another (zad/zbf/DOM) — DD arbitrates its own layering.
// Toggle: PPB.ini [Equip] bClothingGate (default ON; hot).
const char* ClothingBlocker(std::uint32_t actorFid, int siteBit)
{
    if (!Ini::GetBool("Equip", "bClothingGate", true)) return nullptr;
    auto* form  = RE::TESForm::LookupByID(actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !siteBit) return nullptr;

    // region -> the biped slots whose garments block it (AddOn §4.2, verbatim)
    int slots[6] = { 0, 0, 0, 0, 0, 0 };
    int n = 0;
    const auto add = [&](int sl) { if (n < 6) slots[n++] = sl; };
    switch (siteBit) {
    case kSiteHead: case kSiteEyes:                 add(30); add(31); add(42); break;
    case kSiteMouth:                                add(44);                    break;
    case kSiteNeck:                                 add(45);                    break;
    case kSiteChest: case kSiteTorso: case kSiteNipple: add(32);                break;
    // ⛔ WRIST IS NOT BLOCKED BY A GLOVE (user ruling 2026-09-03, reversing the lumped row).
    // "I try put copper cuff on her, it told me it couldn't cause she have a glove, which
    // doesn't matter, copper cuff are outer, it should work."
    //   Measured: 'Copper Wrist Cuffs' (02431D:ZaZAnimationPack.esm, zbfWristSimpleCopper) is
    // FirstPersonFlags 536870912 = bit 29 = biped slot 59. The blocker was 'Necromancer Black
    // Claws', a HAND item on 33. Two different slots — so this was never the same-slot rule
    // (that one is bSlotOccupiedRefuse, and it is correct); it was this coarse region row
    // treating hands and forearms as one place. A cuff, bracer or shackle goes OVER a glove.
    //   Kept for Hands/Finger: a glove genuinely does cover the palm and the ring finger.
    case kSiteWrist:                                    add(34); add(59);       break;
    case kSiteHands: case kSiteFinger:                  add(33);                break;
    case kSiteAnkle: case kSiteFeet:                add(37); add(38);           break;
    // pelvis / intimate — the case the user hit: a plug under a dress
    case kSiteWaist: case kSiteVaginal: case kSiteAnal: add(32); add(49); add(52); break;
    default: return nullptr;
    }
    for (int i = 0; i < n; ++i) {
        auto* worn = actor->GetWornArmor(
            static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << (slots[i] - 30)));
        if (!worn) continue;
        if (IsFrameworkDevice(worn)) continue;          // devices never block devices
        return (worn->GetName() && worn->GetName()[0]) ? worn->GetName() : "her clothing";
    }
    return nullptr;
}

// Returns the blocking garment's name when the rule refuses this equip, nullptr to proceed.
const char* SlotOccupiedBlocker(std::uint32_t actorFid, RE::TESBoundObject* base)
{
    if (!Ini::GetBool("Equip", "bSlotOccupiedRefuse", true)) return nullptr;
    auto* form  = RE::TESForm::LookupByID(actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !base) return nullptr;
    // The slots that matter are the ones she will WEAR: the rendered half for a DD pair.
    const DevClass* dc = nullptr;
    if (auto it = g_classCache.find(base->GetFormID()); it != g_classCache.end()) dc = &it->second;
    RE::TESObjectARMO* armo = nullptr;
    if (dc && dc->dd && dc->renderedFid) {
        auto* rf = RE::TESForm::LookupByID(dc->renderedFid);
        armo = rf ? rf->As<RE::TESObjectARMO>() : nullptr;
    }
    if (!armo) armo = base->As<RE::TESObjectARMO>();
    if (!armo) return nullptr;
    const bool heldIsDevice = dc && (dc->dd || (dc->mask != 0 && !dc->generic));
    const std::uint32_t slots = static_cast<std::uint32_t>(armo->GetSlotMask().underlying());
    for (int b = 0; b < 32; ++b) {
        if (!(slots & (1u << b))) continue;
        auto* worn = actor->GetWornArmor(
            static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << b));
        if (!worn || worn == armo) continue;
        if (heldIsDevice && IsFrameworkDevice(worn)) continue;   // DD's business, not ours
        return (worn->GetName() && worn->GetName()[0]) ? worn->GetName() : "something";
    }
    return nullptr;
}

DevClass ClassOfHeld(RE::TESObjectREFR* refr, RE::TESBoundObject* base)
{
    if (!refr || !base) return {};
    const std::uint32_t baseId = base->GetFormID();
    if (auto it = g_classCache.find(baseId); it != g_classCache.end()) return it->second;

    DevClass dc{};
    if (IsDdInventoryDevice(base)) {
        dc.dd = true;
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
        if (policy) {
            const auto handle = policy->GetHandleForObject(
                static_cast<RE::VMTypeID>(RE::FormType::Reference), refr);
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (vm->FindBoundObject(handle, "zadequipscript", obj) && obj) {
                if (auto* v = obj->GetProperty("zad_DeviousDevice")) {
                    if (v->IsObject()) {
                        if (auto* f = RE::BSScript::UnpackValue<RE::TESForm*>(v)) {
                            if (auto* kw = f->As<RE::BGSKeyword>()) {
                                const char* id = kw->GetFormEditorID();
                                dc.mask = DdMask(id);
                                ClsSuffix(id, dc.cls, sizeof dc.cls);
                            }
                        }
                    }
                }
                // The half she will actually WEAR. Grabbed here because the
                // world reference (and with it this script instance) is
                // consumed by the equip — after that there is no route from
                // the inventory half back to the rendered one.
                if (auto* v = obj->GetProperty("deviceRendered")) {
                    if (v->IsObject()) {
                        if (auto* f = RE::BSScript::UnpackValue<RE::TESForm*>(v))
                            dc.renderedFid = f->GetFormID();
                    }
                }
            }
        }
    } else {
        dc.mask = PlainMask(base);   // ZaZ / Diary of Mine: keywords on the base itself
    }
    // * SLOTS ARE UNIONED WITH KEYWORDS, never merely a fallback. A keyword
    // table can only know the names it was written against; the biped slot is
    // what the GAME itself uses, so it catches every content mod whatever they
    // named things - and it costs nothing when the keyword already agreed.
    if (g_cfg.equipGeneric != 0.f || dc.mask == 0) {
        const int slotMask = GenericMask(base);
        if (dc.mask == 0 && slotMask != 0) dc.generic = true;
        dc.mask |= slotMask;
    }

    // ★ RENDERED-HALF POST-FIXES (2026-08-30, AddOn asks 5/6 + SS2.x). The class keyword
    // alone cannot see these; the rendered half's OTHER keywords disambiguate.
    if (dc.dd && dc.renderedFid) {
        auto* rf = RE::TESForm::LookupByID(dc.renderedFid);
        if (rf) {
            if (_stricmp(dc.cls, "ElbowTie") == 0 && FormHasKeyword(rf, "zad_DeviousCollar")) {
                dc.mask = kSiteNeck | kSiteWrist;   // collared elbow tie: neck + arms only
                logger::info("[EQUIP] '{}': collared ElbowTie -> sites neck|wrist", base->GetName());
            }
            if (_stricmp(dc.cls, "PonyGear") == 0 && FormHasKeyword(rf, "zad_DeviousBoots")) {
                dc.mask = kSiteFeet | kSiteAnkle;   // pony BOOTS, not the harness: legs
                logger::info("[EQUIP] '{}': PonyGear+Boots -> sites foot|ankle", base->GetName());
            }
        }
    }
    // ★ PER-DEVICE OVERRIDE, applied LAST and REPLACING the mask (user-ruled records).
    LoadSiteOverrides();
    {
        const std::uint32_t key = (dc.dd && dc.renderedFid) ? dc.renderedFid : baseId;
        if (auto it = g_siteOverride.find(key); it != g_siteOverride.end()) {
            dc.mask = it->second;
            char oms[64];
            MaskStr(dc.mask, oms, sizeof oms);
            logger::info("[EQUIP] '{}': per-device site OVERRIDE -> {}", base->GetName(), oms);
        }
    }

    g_classCache[baseId] = dc;
    char ms[64];
    MaskStr(dc.mask, ms, sizeof ms);
    // cls / rendered are logged because everything downstream depends on them:
    // an empty cls on a DD device means the narration will fall back to the
    // generic "restraint" row, and a missing rendered half means the equip
    // check degrades to the old cannot-disprove one. NEVER FAIL SILENTLY - both
    // used to be invisible, and both were wrong for months (report 23 §30).
    logger::info("[EQUIP] device 0x{:08X} '{}' -> sites {} ({}) cls='{}' rendered=0x{:08X}",
                 baseId, base->GetName(), ms,
                 dc.dd ? "DD" : (dc.generic ? "ordinary gear"
                                            : (dc.mask ? "ZaZ/plain" : "not wearable")),
                 dc.cls[0] ? dc.cls : "-", dc.renderedFid);
    if (dc.dd && (!dc.cls[0] || !dc.renderedFid)) {
        logger::info("[EQUIP] ⚠ DD device '{}' did not yield {}{}{} from its zadEquipScript - "
                     "narration and the worn-check will run degraded",
                     base->GetName(),
                     dc.cls[0] ? "" : "a class",
                     (!dc.cls[0] && !dc.renderedFid) ? " or " : "",
                     dc.renderedFid ? "" : "a rendered half");
    }
    return dc;
}


// ONE snapshot per frame, shared by every consumer. Before this the frame
// took up to five separate GetContacts calls (TickHand x2, TickPlugPull x2,
// TickUndress) for data that cannot change between them.
PPBAPI::PpbTouchContact g_snap[32];
int                     g_snapN = 0;

Higgs::IHiggsInterface001*   g_higgs = nullptr;
// ★ THE DD/ZaZ CLOTHING GATE (2026-08-30, their DD_Clothing_Gate_API_for_PPB.md). A synchronous
// C++ vtable, NOT the Papyrus natives: we decide at the release frame inside TickHand, and a VM
// round-trip there is slow, async, and cannot answer in the frame we need it — their words, and
// they withdrew the native ask themselves. ⚠ Null = the AddOn is absent: KEEP the local
// ClothingBlocker as the fallback and never hard-depend on them.
VRTEDDZ::IVrteGateInterface1* g_ddzGate = nullptr;

void AcquireDdzGate()
{
    if (g_ddzGate) return;
    auto* msg = SKSE::GetMessagingInterface();
    if (!msg) return;
    VRTEDDZ::GateMessage m{};
    msg->Dispatch(VRTEDDZ::GateMessage::kGetGateInterface, &m, sizeof(m), "VRTE_DDZaZ");
    if (m.GetApiFunction) {
        g_ddzGate = static_cast<VRTEDDZ::IVrteGateInterface1*>(m.GetApiFunction(1));
        if (g_ddzGate)
            logger::info("[EQUIP] DD/ZaZ clothing gate ACQUIRED (build {}) — their four-layer model "
                         "now decides; the local ClothingBlocker stays as the no-AddOn fallback.",
                         g_ddzGate->GetBuildNumber());
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logger::info("[EQUIP] DD/ZaZ clothing gate not available (AddOn absent or older) — "
                         "using PPB's own region gate.");
        }
    }
}
std::atomic<bool>            g_paused{ false };
std::atomic<bool>            g_armed { false };

double NowS()
{
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

// ── the held device ──────────────────────────────────────────────────────────
struct HeldDevice {
    RE::ObjectRefHandle refr;
    RE::TESBoundObject* base = nullptr;
    int   mask = 0;
    bool  dd   = false;
    bool  live = false;
};
HeldDevice g_held[2];

struct InsertState {
    std::uint32_t       actorFid = 0;
    int                 site  = 0;      // the zone bit the dwell is running on
    float               dwell = 0.f;
    float               peakPressU = 0.f;    // force grade: deepest press this gesture
    double              lastQualify = 0.0;   // realtime of the last qualifying frame
    RE::TESBoundObject* base  = nullptr;   // banked so a release can still act
    RE::ObjectRefHandle refr;
    // v28 RELEASE REACH: the capsule the latest qualifying frame was on (the best contact's own
    // slot/child/side) and the device's distance to it on the last frame the hand still held it.
    int                 capSlot  = -1;
    int                 capChild = 0;
    bool                capLeft  = false;
    bool                relOk    = false;   // did the last held-frame measurement read
    int                 relKind  = 0;       // PpbApi's object probe: 2 box / 1 segment / 0 point
    float               relGapU  = 0.f;     // gate distance to that capsule (<= 0 = surfaces meet)
    float               relRawU  = 0.f;     // true surface distance
    double              relS     = 0.0;     // realtime it was taken
};
InsertState g_insert[2];

// ★ v28 RELEASE REACH (2026-09-10) — asked ONLY for a release whose dwell was earned. true = equip
// exactly as before. false = the device falls where it was let go: TickHand's release path then
// treats it like any release without a valid gesture (the eject watch still arms if it touched her
// within the last second, so a Gift-by-Hand give cannot swallow it).
// Measured 2026-09-10 18:32:52: a gag earned at her mouth, carried away and dropped on a table,
// equipped anyway ~1.1 s after its last qualifying frame - ONCE EARNED never asked where the device
// WAS. A shorter time window cannot fix that (the digest keeps an unseen contact one extra 4 Hz
// tick); the geometry at the moment of release can. Fails CLOSED: no fresh measurement, no equip.
bool ReleaseReachOk(int h, const InsertState& ins, double now)
{
    const float       reach = g_cfg.equipReleaseReachU;
    const char* const name  = ins.base ? ins.base->GetName() : "?";
    if (reach <= 0.f) {
        logger::info("[EQUIP] RELEASE CHECK hand{} '{}' -> 0x{:08X}: off (equipReleaseReachU 0) - equipping as earned",
                     h, name, ins.actorFid);
        return true;
    }
    const double age = now - ins.relS;
    if (ins.capSlot < 0 || !ins.relOk || age > 0.5) {
        logger::info("[EQUIP] RELEASE CHECK hand{} '{}' -> 0x{:08X}: REFUSED - no fresh measurement against the "
                     "capsule that earned it (slot {} child {}{}, read {}, {:.2f}s old); it falls where you let go",
                     h, name, ins.actorFid, ins.capSlot, ins.capChild, ins.capLeft ? " L" : "",
                     ins.relOk ? "ok" : "FAILED", age);
        return false;
    }
    static const char* const kKind[3] = { "point", "segment", "box" };
    const bool ok = ins.relGapU <= reach;
    logger::info("[EQUIP] RELEASE CHECK hand{} '{}' -> 0x{:08X}: {} - on the last held frame it was {:.2f}u from the "
                 "{} capsule that earned it (slot {} child {}{}, true surface {:.2f}u, {} probe), reach {:.2f}u{}",
                 h, name, ins.actorFid, ok ? "IN REACH" : "REFUSED", ins.relGapU, ZoneName(ins.site),
                 ins.capSlot, ins.capChild, ins.capLeft ? " L" : "", ins.relRawU,
                 kKind[(ins.relKind >= 0 && ins.relKind <= 2) ? ins.relKind : 0], reach,
                 ok ? " - equipping" : " - it falls where you let go");
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ THE FAILED-GESTURE EJECT (user rule, 2026-08-22): a device that does NOT
// equip must STAY IN THE WORLD — a cuff pressed to her neck, a vaginal plug at
// the wrong orifice, a too-short press: none of these may end up in her
// inventory.
//
// The inventory transfer on release is NOT ours, and it is NOT HIGGS — HIGGS
// never stashes on drop (user statement, confirmed by the 2026-08-22 Papyrus
// log). It is Gift by Hand's single-grab give: GetCloseNode claims any drop
// whose PLAYER HAND is within HandRadius/HeadRadius of her HAND or HEAD
// nodes. Proven drop by drop in Papyrus.0.log: the gags claimed at Node:0
// (head), the wrist cuffs / rope cuffs / plug second-copies at Node:1/2 —
// because an NPC's idle hands hang at HIP height, a plug released at her
// pelvis is inside HandRadius of a hand node. The ankle-cuff drop (far from
// both) logged flagNode:999 and GBH correctly did nothing — it fell.
//
// The proper deconfliction is a device guard in the user's own GiftByHandSN
// fork (their call, their mod). This eject is the addon-side backstop that
// works regardless: while a classified device is held and touching an NPC we
// snapshot her count of that base; on a release that did NOT equip we check
// ~0.6s later — if the world reference is gone and her count rose, a give
// claimed it, and we DropObject it back out at her feet.
//
// Deliberately scoped to CLASSIFIED devices only: dropping food, weapons or
// any ordinary item into an NPC keeps GBH's behaviour untouched. Known
// cosmetic residue while GBH is unguarded: its gift LLM event has already
// fired by the time we eject.
// ─────────────────────────────────────────────────────────────────────────────
struct TouchTrack {
    std::uint32_t       actorFid  = 0;     // the NPC this held device is touching
    double              lastTouch = 0.0;
    int                 baseCount = 0;     // her count of the base BEFORE any stash
    RE::TESBoundObject* base      = nullptr;
    RE::ObjectRefHandle refr;
    // R1 "place" (2026-09-13): time this hold spent pressed against a body part the device does not go on
    std::uint32_t       wrongActor = 0;
    float               wrongS     = 0.f;
    int                 wrongZone  = 0;
};
TouchTrack g_touch[2];

struct PendingEject {
    bool                active    = false;
    double              at        = 0.0;
    std::uint32_t       actorFid  = 0;
    int                 baseCount = 0;
    RE::TESBoundObject* base      = nullptr;
    RE::ObjectRefHandle refr;
};
PendingEject g_eject[2];

// An equip that has been asked for but not yet confirmed. Papyrus runs the
// actual EquipItemEx asynchronously, so the answer is checked a beat later.
struct PendingVerify {
    bool                active   = false;
    double              at       = 0.0;
    std::uint32_t       actorFid = 0;
    RE::TESBoundObject* base     = nullptr;
    bool                left     = false;
    // ★ 2026-08-24: DD's OnEquipped chain is Papyrus and its length is not ours
    // to predict. Measured on a 'Black Bone Gag': DD logged OnEquipped, our
    // check at +1.2s still did not see the rendered half worn, and the refusal
    // path then UNEQUIPPED AND DROPPED a device that had gone on correctly.
    // A false "it did not go on" is destructive, so it now costs a second look
    // before anything is undone.
    int                 tries    = 0;
    float               forceU   = 0.f;   // peak press depth of the gesture (force grade)
    // ★ 2026-09-06 (option A): the worn pieces occupying the new item's biped slots at the
    // moment of the gesture — the ones the equip displaces. Captured BEFORE the move.
    std::uint32_t       displaced[8] = {};
    int                 nDisplaced   = 0;
};
PendingVerify g_verify[2];

struct PendingHandGrab {
    bool                active = false;
    double              timeout = 0.0;
    RE::ObjectRefHandle ref;
    bool                left = false;
    int                 tries = 0;      // GrabObject attempts made
    // false = just clear ownership and leave it lying there (the failed-gesture
    // eject and a refused equip); true = put it in the player's hand.
    bool                grabIt = true;
    // Ripped off a PLAYER TEAMMATE: clear the ownership the dropped ref
    // inherits from her inventory, so the piece is not "stolen" loot.
    // A NON-teammate's gear keeps her ownership on purpose - pulling a
    // stranger's necklace off IS theft, and the vanilla crime rule is the
    // roleplay-correct outcome (user call, 2026-08-22, verified on Carmella:
    // no follower faction anywhere in her data, flag was correct).
    bool                clearOwner = false;
};
PendingHandGrab g_handGrab[2];

// ⛔⛔ THE ANIMATED ARM IS NOT YOUR ARM (root-caused 2026-08-24, report 23 §34).
//
// This used to read "NPC L/R Hand [LHnd/RHnd]" off the player's 3D — the
// THIRD-PERSON ANIMATION SKELETON. In VR those bones are driven by the
// animation graph and VRIK, not by the controllers, and the moment HIGGS grabs
// an actor VRIK pins the arms into a hold pose. The bones then stop tracking
// entirely.
//
// Measured, user pulling their right hand a full arm's length away from a gag:
//     [UNDRESS] pulling 'unnamed piece': -4.2/25.0u
//     [UNDRESS] pulling 'unnamed piece': -4.2/25.0u
//     [UNDRESS] pulling 'unnamed piece': -4.2/25.0u      <- frozen, 2 seconds
// and at the arm itself, a 5.2u jump in 7 ms as the pose snapped shut. So the
// pull could never reach undressPullU except by luck in the instant before the
// pose locked — which is exactly how the handful of successful undresses got
// through, and why it looked intermittent rather than broken.
//
// The WAND nodes are the controllers. Nothing in the animation graph can pin
// them, so they keep tracking through any grab, hold or IK pose.
//
// ⚠ The two sources do NOT report the same absolute distance, so any knob
// calibrated against the old one (undressGrabMaxU, undressPullU) is calibrated
// against a lie and needs re-reading off the log. That is why TickUndress logs
// both for now.
//
// Falls back to the old bones if VR node data is unavailable — flat Skyrim, or
// a runtime where the offset does not resolve — because a frozen measurement
// is still better than no gesture at all.
bool PlayerHandPos(bool isLeft, RE::NiPoint3& out)
{
    auto* pl = RE::PlayerCharacter::GetSingleton();
    if (!pl) return false;
    if (auto* vr = pl->GetVRNodeData()) {
        auto& wand = isLeft ? vr->LeftWandNode : vr->RightWandNode;
        if (wand) { out = wand->world.translate; return true; }
    }
    auto* d3 = pl->Get3D();
    if (!d3) return false;
    auto* node = d3->GetObjectByName(isLeft ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]");
    if (!node) return false;
    out = node->world.translate;
    return true;
}

// The OLD source, kept for one calibration pass only: it is what every existing
// undress knob was dialled against, and the log prints both so the real numbers
// can be read off a real gesture instead of guessed at. Delete once the knobs
// are re-dialled.
bool PlayerHandPosBone(bool isLeft, RE::NiPoint3& out)
{
    auto* pl = RE::PlayerCharacter::GetSingleton();
    auto* d3 = pl ? pl->Get3D() : nullptr;
    if (!d3) return false;
    auto* node = d3->GetObjectByName(isLeft ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]");
    if (!node) return false;
    out = node->world.translate;
    return true;
}


void ReleaseToHand(RE::Actor* actor, RE::TESBoundObject* base, bool left, bool intoHand)
{
    if (!actor || !base) return;
    RE::NiPoint3 hp{};
    const bool haveHand = PlayerHandPos(left, hp);
    auto handle = actor->RemoveItem(base, 1, RE::ITEM_REMOVE_REASON::kDropping,
                                    nullptr, nullptr, haveHand ? &hp : nullptr, nullptr);
    PendingHandGrab& pg = g_handGrab[left ? 1 : 0];
    pg.active     = true;
    pg.timeout    = NowS() + 2.5;   // retried every frame until HIGGS confirms
    pg.ref        = handle;
    pg.left       = left;
    pg.grabIt     = intoHand;
    pg.tries      = 0;
    pg.clearOwner = actor->IsPlayerTeammate();
}

int CountOf(RE::Actor* a, RE::TESBoundObject* base)
{
    if (!a || !base) return 0;
    auto counts = a->GetInventoryCounts();
    auto it = counts.find(base);
    return it != counts.end() ? static_cast<int>(it->second) : 0;
}

void BuildHeld()
{
    for (int h = 0; h < 2; ++h) { g_held[h] = HeldDevice{}; }
    if (!g_higgs) return;
    for (int h = 0; h < 2; ++h) {
        const bool isLeft = (h == 1);
        if (!g_higgs->IsHoldingObject(isLeft)) continue;
        auto* refr = g_higgs->GetGrabbedObject(isLeft);
        if (!refr || refr->As<RE::Actor>()) continue;   // a held ACTOR is not a tool
        HeldDevice& hd = g_held[h];
        hd.base = refr->GetBaseObject() ? refr->GetBaseObject()->As<RE::TESBoundObject>() : nullptr;
        const DevClass dc = ClassOfHeld(refr, hd.base);
        hd.refr = refr->GetHandle();
        hd.mask = dc.mask;
        hd.dd   = dc.dd;
        hd.live = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Contact -> equip zone.
//
// ★ THE §14 RULE: the contact stream IS the detection. No actor enumeration, no
// range test, no IsDriven call, no geometry of our own — the actor comes from
// the contact, and PPB's coverage gate is implicit in a contact existing at
// all. The first build answered this question itself with a hand-rolled
// scanner and got "actors=1 inRange=0" in a furnished room while PPB was
// correctly opening her orifice from the same plug in the same frames. Two
// systems answered the same question that night; only one was right.
//
// `zone` = which equip site this contact's capsule belongs to (0 = none).
// `gate` = is the insertion / press requirement met.
//
// PRIMARY: PPB's orifice verdict. orificeKind != 0 means the between-the-twins
// ellipse gate already passed — real insertion, proven upstream, no distance
// test of ours needed (and ours would be worse).
// FALLBACK (orifice module off — it ships orificeEnable 0): raw (slot, child)
// against the COM sensor chain, gated on distU.
// SURFACE sites have no insertion; they gate on distU <= equipTouchU:
//   mouth = head C1 upper lip / C9 palate / C10 throat wall — the §11 log
//           proved a bone gag digests as 'palate', so lips alone would miss;
//   wrist = forearm C1 (wrist half);
//   ankle = foot C3 (ankle lock) + calf C3 (shin lower) — the user's words
//           were "lower arm and lower calf", and a cuff held at the ankle
//           genuinely lands on either capsule.
// ─────────────────────────────────────────────────────────────────────────────
// `accept` is the site mask the HELD DEVICE will take. It exists for one
// reason, measured 2026-08-24 on an armbinder and a blindfold:
//
// ⛔⛔ PPB'S ORIFICE VERDICT USED TO WIN UNCONDITIONALLY. A big object held
// against her overlaps interior sensors as well as the surface capsule it is
// really on, and when PPB called one of those an orifice the whole contact
// collapsed to vaginal-only / anal-only / mouth-only. Two measured failures,
// both of them CORRECT gestures:
//     'Black Leather Blindfold' (sites eyes)      placed at the mouth
//     'Black Leather Armbinder' (sites wrist|chest) placed at the anal
// The blindfold could never be equipped at all; the armbinder earned 2.05s of
// dwell at the wrist and then had it destroyed by the orifice frame.
//
// So the orifice verdict now applies only when the device ACTUALLY WANTS that
// orifice. A plug still cannot be equipped anywhere but its own opening -
// nothing about the strict half changed - but a blindfold brought to her face
// no longer becomes a mouth-only contact, and an armbinder held at her wrist
// stays at her wrist.
void ZoneOfContact(const PPBAPI::PpbTouchContact& c, int accept, int& zone, bool& gate)
{
    zone = 0;
    gate = false;
    // PPB own orifice verdict - its penetration gate has already passed.
    // Consulted ONLY for a device that wants that site; otherwise fall through
    // to the capsule switch, which places the contact by WHERE IT IS.
    if (c.orificeKind == 1 && (accept & kSiteVaginal)) { zone = kSiteVaginal; gate = true; return; }
    if (c.orificeKind == 2 && (accept & kSiteAnal))    { zone = kSiteAnal;    gate = true; return; }
    if (c.orificeKind == 3 && (accept & kSiteMouth))   { zone = kSiteMouth;   gate = true; return; }

    const bool press = (c.distU <= g_cfg.equipTouchU);
    const bool deep  = (c.distU <= g_cfg.equipDepthU);

    // * WIDE ON PURPOSE (2026-08-23). The first version demanded near-surgical
    // placement - a gag had to touch the lip/palate/throat capsules or it read
    // as "at the head" and was refused; a collar brought to the throat resolved
    // to the chin or the wrist; gloves read as "wrist". Measured refusals, all
    // of them CORRECT gestures by the player:
    //     'Black Leather Ball Strap Gag' (mouth) placed at the head
    //     'Iron Collar with Long Chain'  (neck)  placed at the mouth / wrist
    //     'Necromancer Black Claws'      (hand)  placed at the wrist
    // A player brings a thing ROUGHLY where it goes; the 1s dwell and the
    // explicit release are what make it deliberate, not pinpoint accuracy. So
    // every part of the body accepts anything worn near it, and the device own
    // site mask decides which of those it really is.
    // Orifices stay STRICT - vaginal and anal must not be interchangeable.
    switch (c.slot) {
    case 0:                                   // hand
        zone = kSiteHands | kSiteFinger | kSiteWrist;          gate = press; return;
    case 1:                                   // forearm
        zone = kSiteWrist | kSiteHands | kSiteFinger;          gate = press; return;
    case 2:                                   // upper arm - sleeves, binders, long gloves
        // + kSiteHands 2026-08-30 (ask SS2 item 1): "those can be held against the lower or
        // upper arms too" - long gloves run up the arm. NO kSiteTorso here, by design.
        zone = kSiteWrist | kSiteChest | kSiteHands;           gate = press; return;
    case 3:                                   // head: gags, hoods, blindfolds, collars
        zone = kSiteMouth | kSiteHead | kSiteEyes | kSiteNeck; gate = press; return;
    case 7:                                   // neck: collars, chokers, amulets
        zone = kSiteNeck | kSiteHead | kSiteMouth | kSiteChest;gate = press; return;
    case 6:                                   // chest / breasts
        zone = kSiteChest | kSiteTorso | kSiteNipple | kSiteNeck; gate = press; return;
    case 5:                                   // belly
    case 4:                                   // waist
        zone = kSiteWaist | kSiteChest | kSiteTorso;           gate = press; return;
    case 8:                                   // thigh
        zone = kSiteWaist | kSiteAnkle | kSiteFeet;            gate = press; return;
    case 9:                                   // calf
        zone = kSiteAnkle | kSiteFeet;                         gate = press; return;
    case 10:                                  // foot
        zone = kSiteFeet | kSiteAnkle;                         gate = press; return;
    case 11:                                  // COM
        // ⛔ FIXED 2026-08-23 (user: "I tried adding a clitoris ring once and it
        // didn't work. The nipple ring worked."). Child 21 IS THE CLITORIS —
        // kSubIntimateExternal, an EXTERNAL landmark. The whole 21..27 range was
        // gated on `deep`, i.e. real penetration, which a PLUG earns and a
        // PIERCING never can: you lay a ring ON the clitoris, you do not push it
        // inside her. So every vaginal piercing was unequippable, while nipple
        // rings worked fine — the breast zone gates on `press`, not depth.
        // C21 alone now gates on press; C22+ (the real vaginal chain) still
        // demand depth, so a plug still cannot be equipped by hovering outside.
        // ⛔ FIXED AGAIN 2026-08-24 (user: "the chastity belt didn't equip").
        // Measured: 'Black Metal Ceremonial Chastity Belt with Anal Slot'
        // (sites waist) held at her crotch resolved to `vaginal` and was
        // refused - "at the vaginal - not deep enough: d=0.91u".
        //
        // §39 stopped PPB's ORIFICE VERDICT from hijacking a device that wants
        // no orifice, but the COM CHILD MAP below did the same thing by a
        // different route and was left alone. A belt, a hobble skirt, a harness
        // - everything worn OVER the crotch - hits these same capsules, and
        // they offered the orifice and nothing else.
        //
        // ★ Fixing one of two paths to a bug leaves the bug. §39's own note
        //   said "the orifice verdict answers its own question, not yours" -
        //   and this map answers exactly the same question, so it needed the
        //   same treatment on the same day and did not get it.
        //
        // The orifice stays STRICT for anything that wants it: a plug still
        // needs real depth. A device that does NOT want the orifice is placed
        // by REGION instead - the pelvis is the waist - and only has to press.
        if (c.child == 21) {
            zone = kSiteVaginal | kSiteWaist; gate = press;
        } else if (c.child >= 22 && c.child <= 27) {
            if (accept & kSiteVaginal) { zone = kSiteVaginal; gate = deep;  }
            else                       { zone = kSiteWaist;   gate = press; }
        } else if (c.child >= 28 && c.child <= 31) {
            if (accept & kSiteAnal)    { zone = kSiteAnal;    gate = deep;  }
            else                       { zone = kSiteWaist;   gate = press; }
        } else {
            zone = kSiteWaist; gate = press;
        }
        return;
    }
}

// ── notification ─────────────────────────────────────────────────────────────
// This CommonLibVR has no RE::DebugNotification, so go through the VM the same
// way Papyrus's Debug.Notification does. Only fired on real state changes.
void Notify(const char* msg)
{
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm || !msg) return;
    auto* args = RE::MakeFunctionArguments(std::string(msg));
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
    vm->DispatchStaticCall("Debug", "Notification", args, cb);
    delete args;
}

// ── the equip ────────────────────────────────────────────────────────────────
// ★ REPLAYED FROM GIFT BY HAND (2026-08-22, after the first live run).
//
// The first build equipped through ActorEquipManager::EquipObject. Result: the
// inventory item FLAGGED as equipped but NO rendered device ever appeared —
// DD's zadEquipScript chain never did its work — while GBH's triple-grab
// gesture equipped the SAME devices VISIBLY in the same session. GBH's armor
// path (BeVR_S_HiggsHook.psc:436-480, the SN fork) is three Papyrus calls, so
// we make exactly those calls, through the VM, in the same order:
//
//   target.AddItem(heldReference)  — a REFERENCE is MOVED into the inventory,
//       never copied. This is also the duplication fix: the first build did
//       AddObjectToContainer(base) in a race with Gift by Hand's single-grab
//       give (both landed, she got two — log 09:50/09:51, every equip
//       "(added)" here + "AddAndMayEquip: adding item" in Papyrus.0.log).
//       One reference is one item wherever it ends up; whichever mod moves it
//       second simply no-ops.
//   target.EquipItemEx(base, 0, false, true)  — DD's OWN equip call
//       (zadLibs.LockDevice uses these exact arguments). Fires the full
//       OnEquipped chain, which is what equips the RENDERED half. DDNF keeps
//       it honest because the paired inventory item is present.
//   target.QueueNiNodeUpdate()  — GBH ends with this; the 3D refresh.
//
// ZaZ / DOM items are scriptless plain armors, so for them the same three
// calls are just "a normal equip" — the user's instruction verbatim.
//
// No AEM, no container math, no world-ref delete — the move consumes it.
void DoEquip(std::uint32_t actorFid, RE::TESBoundObject* base, RE::ObjectRefHandle refrH,
             bool left, float peakPressU)
{
    auto* form  = RE::TESForm::LookupByID(actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !base) return;

    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) {
        logger::info("[EQUIP] FAILED: no VM — cannot equip '{}'", base->GetName());
        return;
    }

    // ★ CLAIM THIS ACTOR BEFORE ANYTHING TOUCHES HER (2026-08-26). The
    // TESEquipEvent sink in WornDevices narrates every device change it sees,
    // which is how a MENU equip gets narrated at all - but it also sees the one
    // we are about to cause, and this gesture already narrates through
    // PPB_GestureDeviceEquipped. Claimed first, so the claim is standing before
    // EquipItemEx can possibly fire the engine event.
    // The add-on's WornDevices sink needs to know this removal was OUR gesture and not a menu,
    // so it can attribute it. PPB does not own that bookkeeping — it announces, the add-on claims.
    SendGestureEvent("PPB_GestureClaim", std::to_string(actorFid), 0.f,
                     RE::TESForm::LookupByID(actorFid));

    // ⛔ NO OBJECT ARGUMENTS ARE EVER DISPATCHED FROM C++ (2026-08-22).
    // The VM types a packed object as its MOST-DERIVED attached script and the
    // external-dispatch check refuses the upcast compiled Papyrus performs
    // routinely — measured on the second live run:
    //   "ObjectReference.AddItem(Form,int,bool) received incompatible
    //    arguments! Received types (zadPlugScript,int,bool)"
    // The same trap awaits ANY object arg: a DD ARMO base packs as its
    // zadEquipScript subtype too, so an EquipItemEx(base,...) method dispatch
    // is equally rejectable. (A parallel session's partial fix went native
    // with Actor::PickUpObject for the MOVE but kept the EquipItemEx object
    // dispatch — same wall, one call later.)
    //
    // So the DLL passes three raw FormIDs as INTS to our own shipped helper,
    // Scripts/PPB_DeviceEquip.pex, which resolves them with GetFormEx and
    // runs Gift by Hand's proven three lines in real, compiled Papyrus:
    // AddItem(heldRef) — the reference MOVE, no duplication — then
    // EquipItemEx(base, 0, false, true) — DD's own LockDevice call, fires the
    // OnEquipped chain that equips the RENDERED half — then
    // QueueNiNodeUpdate(). Plain-value static dispatch is the proven path:
    // Notify (Debug.Notification) has used it from this exact context all
    // along, and the user has SEEN those notifications.
    // ⛔ THE MOVE IS NATIVE (2026-08-22 night, the dog-bone zombie).
    // A ref WE dropped out of her inventory (undress) can come back with a
    // STALE VM binding — Papyrus.0.log 19:34:19 showed 0xFF000E55 resolving
    // as "[Item 3 in container (1301DE4F)].zadGagScript" with "no native
    // object bound": truthy in Papyrus, None at AddItem, so the move no-oped
    // and the bone fell while the toast said equipped. Re-equips of ripped
    // items ALWAYS hit this. Actor::PickUpObject moves the NATIVE reference
    // — no VM object involved, zombie-proof — and the helper's count-guard
    // then sees the item in her inventory and goes straight to EquipItemEx.
    // ★ 2026-09-06: what is she wearing in the slots this piece will take? Captured BEFORE
    // anything moves, so the confirm step can untag exactly those pieces (OutfitTag).
    std::uint32_t displaced[8] = {}; int nDisplaced = 0;
    if (auto* armo = base->As<RE::TESObjectARMO>()) {
        const std::uint32_t sm = static_cast<std::uint32_t>(armo->GetSlotMask().underlying());
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        for (int b = 0; b < 32 && nDisplaced < 8; ++b) {
            if ((sm & (1u << b)) == 0) continue;
            auto* worn = actor->GetWornArmor(static_cast<Slot>(1u << b));
            if (!worn || worn == armo) continue;
            bool dup = false;
            for (int k = 0; k < nDisplaced; ++k) if (displaced[k] == worn->GetFormID()) dup = true;
            if (!dup) displaced[nDisplaced++] = worn->GetFormID();
        }
    }
    bool moved = false;
    if (auto r = refrH.get(); r && !r->IsDeleted() && r->GetBaseObject() == base) {
        actor->PickUpObject(r.get(), 1, false, /*playSound*/ false);
        moved = true;
    }

    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
    // 4th arg (2026-09-03, Tier 0 of the permanent-equip research): preventUnequip for ORDINARY
    // gear only. DD deliberately equips its inventory half with false and its rendered half with
    // true inside its own OnEquipped; forcing true on the DD path would fight that pairing.
    int preventUnequip = 1;
    if (auto it = g_classCache.find(base->GetFormID()); it != g_classCache.end() && it->second.dd)
        preventUnequip = 0;
    auto* args = RE::MakeFunctionArguments(
        static_cast<std::int32_t>(actorFid),
        static_cast<std::int32_t>(0),      // heldId unused now — the move already happened
        static_cast<std::int32_t>(base->GetFormID()),
        static_cast<std::int32_t>(preventUnequip));
    vm->DispatchStaticCall("PPB_DeviceEquip", "DoEquip", args, cb);
    delete args;
    // ★ v29d (user 2026-09-11): tell PushStep an equip just happened on her. Heeled boots raise BOTH feet ~8 u, and
    // with a hand on her thigh that read as a lift and ragdolled her. No lift / stumble / ragdoll / walk for
    // pushStepEquipSettleS, then her height at that moment becomes the new floor.
    PushStep::NoteEquipEvent(actorFid);

    logger::info("[EQUIP] '{}' (0x{:08X}) -> actor 0x{:08X} via native PickUpObject + "
                 "PPB_DeviceEquip.DoEquip{} - verifying",
                 base->GetName(), base->GetFormID(), actorFid,
                 moved ? "" : " (world ref already consumed - count-guarded fallback)");

    // ★ NEVER CLAIM A SUCCESS WE DID NOT ACHIEVE (2026-08-22). A Ball Strap Gag
    // reported "equipped" and fell on the floor: she already wore a gag, and DD
    // allows one device per class, so its script refused and re-asserted the
    // old one. The toast fired anyway because it was unconditional. Now the
    // notification waits for proof, and a refusal is both explained and undone.
    PendingVerify& pv = g_verify[left ? 1 : 0];
    pv.active   = true;
    pv.at       = NowS() + 1.2;      // DD's OnEquipped chain needs a beat
    pv.actorFid = actorFid;
    pv.base     = base;
    pv.left     = left;
    pv.forceU   = peakPressU;
    pv.nDisplaced = nDisplaced;
    for (int k = 0; k < 8; ++k) pv.displaced[k] = displaced[k];
}

// A payload FIELD: never NULL, never contains '|' (a '|' in an item or NPC name would shift every later field).
static void FieldCopy(char* out, std::size_t cap, const char* s)
{
    if (!out || cap == 0) return;
    std::size_t i = 0;
    for (; s && s[i] && i + 1 < cap; ++i) out[i] = (s[i] == '|') ? '/' : s[i];
    out[i] = '\0';
}

// ★★ 2026-09-13 THE REFUSED EQUIP (VRTE change request GearGestures R1; user: "an equip event can fail due to the slot
// being already used or the wrong location. if that happen, a short-lived event should be sent to the LLM").
// Every refusal used to be log-only. One event, sender = the NPC the player tried to dress, numArg = hand (0 R / 1 L):
//   PPB_GestureEquipRefused  "<name>|<slotMask>|<reason>|<blocker>|<isDD>|<class>|<zone>"
//     reason  slot      bSlotOccupiedRefuse: a piece she wears holds a slot this one needs (blocker = that piece)
//             clothing  the equip gate: a garment over the site, or a device on one of its slots (blocker = it)
//             place     held against the WRONG body part for >= equipDwellS, then let go (see TickHand)
//             refused   PPB asked for the equip and it did not go on (TickVerify; blocker = the piece in its slot, or "")
//     slotMask the HELD item's biped mask (a DD device: its rendered half — same encoding as GearEquipped)
//     zone     where it was aimed (slot / clothing) or where it was held (place); "" for refused
static void SendEquipRefused(std::uint32_t actorFid, RE::TESBoundObject* base, int hand,
                             const char* reason, const char* blocker, int zone)
{
    auto* af    = RE::TESForm::LookupByID(actorFid);
    auto* actor = af ? af->As<RE::Actor>() : nullptr;
    if (!actor || !base || actor->GetFormID() == 0x14) return;
    const DevClass* dc = nullptr;
    if (auto it = g_classCache.find(base->GetFormID()); it != g_classCache.end()) dc = &it->second;
    RE::TESObjectARMO* armo = nullptr;
    if (dc && dc->dd && dc->renderedFid) {
        auto* rf = RE::TESForm::LookupByID(dc->renderedFid);
        armo = rf ? rf->As<RE::TESObjectARMO>() : nullptr;
    }
    if (!armo) armo = base->As<RE::TESObjectARMO>();
    const std::uint32_t slotMask = armo ? static_cast<std::uint32_t>(armo->GetSlotMask().underlying()) : 0u;
    char cls[64] = {};
    if (dc && dc->cls[0]) {
        std::snprintf(cls, sizeof cls, "%s", dc->cls);
    } else {
        bool l = false, q = false;
        DdDescribe(armo ? static_cast<RE::TESBoundObject*>(armo) : base, cls, sizeof cls, l, q);
    }
    const bool isDD = (dc && dc->dd) || IsDdInventoryDevice(base);
    char nm[96], bl[96], cl[64];
    FieldCopy(nm, sizeof nm, base->GetName());
    FieldCopy(bl, sizeof bl, blocker);
    FieldCopy(cl, sizeof cl, cls);
    char buf[384];
    std::snprintf(buf, sizeof buf, "%s|%u|%s|%s|%d|%s|%s", nm, slotMask, reason ? reason : "", bl, isDD ? 1 : 0, cl,
                  zone ? ZoneName(zone) : "");
    SendGestureEvent("PPB_GestureEquipRefused", buf, static_cast<float>(hand ? 1 : 0), actor);
    logger::info("[EQUIP->VRTE] PPB_GestureEquipRefused \"{}\" hand{} on 0x{:08X}", buf, hand, actorFid);
}

// ─────────────────────────────────────────────────────────────────────────────
// TickHand — pure consumer of PPB's digest snapshot (§14 rewrite, 2026-08-21).
//
// Per frame, for a hand holding a classified device: find this wand's OBJECT
// contacts in GetContacts(), resolve each to an equip zone, and run the dwell
// on the best qualifying one. On HIGGS release, the banked state decides.
//
// Dwell is accumulated HERE (+= dt while qualifying), never taken from
// c.durationS — PPB's duration starts at first contact, hover included, and
// the user's "really deliberate" rule means time IN PLACE, not time NEAR.
//
// ⚠ NEVER FAIL SILENTLY. Three separate bugs in this module hid behind a quiet
// `return` — the wrong HIGGS message id, the wrong half of the DD device pair,
// and a numerically impossible depth gate — and each cost a full in-game
// session. Every rejection an action can meet says why, at the DEFAULT log
// level, throttled. logLevel adds detail; it never gates a reason.
// ─────────────────────────────────────────────────────────────────────────────
void TickHand(int h, float dt, double now)
{
    InsertState& ins = g_insert[h];
    HeldDevice&  hd  = g_held[h];

    // ── released? decide on what was banked last frame, then clear ──────────
    if (!hd.live) {
        TouchTrack& tt = g_touch[h];
        // v28: an EARNED gesture must still be AT her when you let go (ReleaseReachOk, below InsertState).
        if (ins.actorFid && ins.base && ins.dwell >= g_cfg.equipDwellS && ReleaseReachOk(h, ins, now)) {
            // ★ THE SLOT-OCCUPIED RULE (user, 2026-08-30): an occupied slot refuses like a
            // DD refusal instead of the vanilla silent swap. The piece simply falls from the
            // hand; the eject watch still arms so a Gift-by-Hand give cannot swallow it.
            // ⛔ PASS THE RENDERED HALF (their §2): 0 of 1,221 DD INVENTORY halves carry a
            // biped slot or a class keyword, so asking about the held half fails OPEN — the
            // exact bug the gate exists to remove. We already resolve and log the rendered id.
            const char* clothes = nullptr;
            {
                const DevClass* dcg = nullptr;
                if (auto it = g_classCache.find(ins.base->GetFormID()); it != g_classCache.end())
                    dcg = &it->second;
                if (g_ddzGate) {
                    const std::uint32_t devFid = (dcg && dcg->renderedFid) ? dcg->renderedFid
                                                                          : ins.base->GetFormID();
                    clothes = g_ddzGate->BlockedBy(ins.actorFid, devFid);
                    if (clothes && !clothes[0]) clothes = nullptr;   // "" means ALLOW
                } else {
                    clothes = ClothingBlocker(ins.actorFid, ins.site);   // fallback, unchanged
                }
            }
            if (const char* blocker = clothes ? clothes
                                              : SlotOccupiedBlocker(ins.actorFid, ins.base)) {
                logger::info("[EQUIP] REFUSED: '{}' -> 0x{:08X} — {} '{}' ({})",
                             ins.base->GetName(), ins.actorFid,
                             clothes ? "the" : "a slot it needs is taken by", blocker,
                             // DD SN reply #3 M4 (2026-09-10): the AddOn gate (build 4) also refuses a device when a worn
                             // DEVICE already holds one of its biped slots - no longer only "dressed over that region".
                             clothes ? "equip gate: it is in the way - a garment over that region, or a device already "
                                       "holding one of its slots (the DD/ZaZ AddOn gate when it is present; without it "
                                       "PPB.ini [Equip] bClothingGate, garments only)"
                                     : "PPB.ini [Equip] bSlotOccupiedRefuse");
                if (g_cfg.equipNotify != 0.f) {
                    char nbuf[190];
                    std::snprintf(nbuf, sizeof nbuf, "She is already wearing %s.", blocker);
                    Notify(nbuf);
                }
                SendEquipRefused(ins.actorFid, ins.base, h, clothes ? "clothing" : "slot", blocker, ins.site);   // R1
                if (tt.actorFid && tt.base) {
                    PendingEject& pe = g_eject[h];
                    pe.active    = true;
                    pe.at        = now + 0.6;
                    pe.actorFid  = tt.actorFid;
                    pe.baseCount = tt.baseCount;
                    pe.base      = tt.base;
                    pe.refr      = tt.refr;
                }
            } else {
                DoEquip(ins.actorFid, ins.base, ins.refr, h == 1, ins.peakPressU);
            }
        } else if (tt.actorFid && tt.base && (now - tt.lastTouch) < 1.0) {
            // A classified device released ON her without a valid gesture:
            // arm the eject watch. If nobody stashes it, it just falls and
            // the watch no-ops.
            PendingEject& pe = g_eject[h];
            pe.active    = true;
            pe.at        = now + 0.6;
            pe.actorFid  = tt.actorFid;
            pe.baseCount = tt.baseCount;
            pe.base      = tt.base;
            pe.refr      = tt.refr;
            logger::info("[EQUIP] hand{} released '{}' at 0x{:08X} without a valid gesture — "
                         "watching for a stash to return it to the world",
                         h, tt.base->GetName(), tt.actorFid);
            // ★ R1 "place" (2026-09-13): only a DELIBERATE wrong placement is a refusal — the device was pressed
            // against a body part it does not go on (the `wrong` bucket: gate met, zone not accepted) for at least
            // equipDwellS in total during this hold, it touched her within the last second, and the right-site
            // dwell was NOT earned (an earned gesture carried away is not "it does not go there"). A casual drop,
            // a brush on the way past, or a too-light press never fires it.
            if (tt.wrongActor && tt.wrongActor == tt.actorFid && tt.wrongS >= g_cfg.equipDwellS &&
                !(ins.actorFid && ins.dwell >= g_cfg.equipDwellS))
                SendEquipRefused(tt.wrongActor, tt.base, h, "place", "", tt.wrongZone);
        }
        ins = InsertState{};
        tt  = TouchTrack{};
        return;
    }
    if (hd.mask == 0 || !hd.base) { ins = InsertState{}; g_touch[h] = TouchTrack{}; return; }

    // ★ v28 RELEASE REACH — while the hand still HOLDS the device, measure it against the capsule
    // that earned the bank, every frame. The release decision reads THIS measurement, never the
    // release frame's: by then HIGGS has let go and the item is already falling.
    if (ins.actorFid && ins.capSlot >= 0) {
        auto* rf = RE::TESForm::LookupByID(ins.actorFid);
        auto* ra = rf ? rf->As<RE::Actor>() : nullptr;
        float g = 0.f, raw = 0.f; int kind = 0;
        ins.relOk = ra && PpbApi::HeldObjectGapU(ra, h, ins.capSlot, ins.capLeft, ins.capChild,
                                                 &g, &raw, &kind);
        if (ins.relOk) { ins.relGapU = g; ins.relRawU = raw; ins.relKind = kind; }
        ins.relS = now;
    }

    // equipMatchSite 0 = any classified device equips from any zone.
    const int accept = (g_cfg.equipMatchSite != 0.f) ? hd.mask : kSiteAll;

    const PPBAPI::PpbTouchContact* buf = g_snap;
    const int n = g_snapN;

    const PPBAPI::PpbTouchContact* best  = nullptr;  int bestZone = 0;
    const PPBAPI::PpbTouchContact* wrong = nullptr;  int wrongZone = 0;   // gate met, zone not accepted
    const PPBAPI::PpbTouchContact* shal  = nullptr;  int shalZone = 0;    // zone hit, gate unmet
    const PPBAPI::PpbTouchContact* touch = nullptr;                       // touching her, no zone at all
    const PPBAPI::PpbTouchContact* nearest = nullptr;                     // any contact — the eject's actor
    int nWand = 0;

    for (int i = 0; i < n; ++i) {
        const auto& c = buf[i];
        if (c.wand != static_cast<unsigned char>(h)) continue;
        if (c.sourceKind != PPBAPI::kSourceObject) continue;
        if (c.toucherFormId != 0x14) continue;
        ++nWand;
        if (!nearest || c.distU < nearest->distU) nearest = &c;
        int zone = 0; bool gate = false;
        ZoneOfContact(c, accept, zone, gate);
        if (!zone) {
            if (!touch || c.distU < touch->distU) touch = &c;
            continue;
        }
        if (!gate) {
            if (!shal || c.distU < shal->distU) { shal = &c; shalZone = FirstBit(zone & accept ? zone & accept : zone); }
            continue;
        }
        if (!(accept & zone)) {
            if (!wrong || c.distU < wrong->distU) { wrong = &c; wrongZone = FirstBit(zone); }
            continue;
        }
        // Narrow a multi-site zone down to what THIS device actually wants.
        if (!best || c.distU < best->distU) { best = &c; bestZone = FirstBit(zone & accept); }
    }

    // ── touch tracking for the failed-gesture eject ─────────────────────────
    // Snapshot her count of this base the moment the device first touches her
    // (BEFORE any release, so before any stash can move it), and keep the
    // world-ref banked so the eject can tell "fell to the floor" from
    // "swallowed by a stash".
    {
        TouchTrack& tt = g_touch[h];
        tt.base = hd.base;
        tt.refr = hd.refr;
        if (nearest) {
            if (tt.actorFid != nearest->actorFormId) {
                tt.actorFid = nearest->actorFormId;
                auto* af = RE::TESForm::LookupByID(tt.actorFid);
                tt.baseCount = CountOf(af ? af->As<RE::Actor>() : nullptr, hd.base);
            }
            tt.lastTouch = now;
        }
    }

    if (!best) {
        // R1 "place": bank how long THIS hold has pressed the device against a body part it does not go on.
        if (wrong) {
            TouchTrack& tw = g_touch[h];
            if (tw.wrongActor != wrong->actorFormId) { tw.wrongActor = wrong->actorFormId; tw.wrongS = 0.f; }
            tw.wrongS    += dt;
            tw.wrongZone  = wrongZone;
        }
        // Say why, in order of how close the player got.
        if (wrong) {
            static double s_lastWrong = 0.0;
            if (now - s_lastWrong > 3.0) {
                s_lastWrong = now;
                char ms[64];
                MaskStr(hd.mask, ms, sizeof ms);
                // Logged, never shown. A wrong placement is not worth a message
                // on screen - it is troubleshooting, not gameplay (user, 2026-08-23).
                logger::info("[EQUIP] refused hand{}: '{}' (sites {}) placed at the {} on 0x{:08X}",
                             h, hd.base->GetName(), ms, ZoneName(wrongZone), wrong->actorFormId);
            }
        } else if (shal) {
            static double s_lastShal = 0.0;
            if (now - s_lastShal > 1.0) {
                s_lastShal = now;
                const bool orificeZone = (shalZone == kSiteVaginal || shalZone == kSiteAnal ||
                                          (shalZone == kSiteMouth && shal->slot == 11));
                logger::info("[EQUIP] hand{} '{}' at the {} — not {} enough: d={:.2f}u, need <= {:.2f}u",
                             h, hd.base->GetName(), ZoneName(shalZone),
                             orificeZone ? "deep" : "pressed",
                             shal->distU,
                             (shal->slot == 11) ? g_cfg.equipDepthU : g_cfg.equipTouchU);
            }
        } else if (touch) {
            static double s_lastTouch = 0.0;
            if (now - s_lastTouch > 1.0) {
                s_lastTouch = now;
                logger::info("[EQUIP] hand{} '{}' touching '{}' (d={:.2f}u) — not an equip site",
                             h, hd.base->GetName(), touch->bodyPart, touch->distU);
            }
        } else if (g_cfg.logLevel >= 1.f) {
            static double s_lastIdle = 0.0;
            if (now - s_lastIdle > 2.0) {
                s_lastIdle = now;
                logger::info("[EQUIP] hand{} '{}': no PPB OBJECT contacts ({} total in snapshot)",
                             h, hd.base->GetName(), n);
            }
        }
        // ★ GRACE — do not zero earned dwell on a flicker. Measured 2026-08-22
        // 09:52:31.939: a gag genuinely in her mouth read 'Face(cheek L)' for
        // ONE frame; the hard reset threw away 1.55s of dwell and the release
        // found 0.5s. A gap shorter than equipGraceS PAUSES the dwell (nothing
        // accumulates); only a real departure — or a different actor — resets.
        // This also covers the release race: the contact can drop a frame
        // before HIGGS reports the hand empty, and the bank must survive to
        // the release check.
        // ★★ ONCE EARNED, IT STAYS EARNED (2026-08-24). equipGraceS (0.4s) was
        // sized for a one-frame capsule flicker, and that is all it can absorb.
        // Measured on an armbinder: the dwell reached 2.05/1.00s at the wrist -
        // the gesture was COMPLETE - then one orifice-claimed frame made the
        // zone unacceptable, 0.4s of grace ran out, `ins` was zeroed, and 1.8s
        // later the release found nothing and reported "without a valid
        // gesture". The player had done everything right and held it for twice
        // as long as required.
        //
        // A satisfied dwell is a finished gesture. Nothing after it may take it
        // back except letting go, which is the release path's business. The
        // long window still expires so a bank cannot survive the player walking
        // off with the device and dropping it somewhere else entirely.
        constexpr double kEarnedHoldS = 5.0;
        if (ins.actorFid && ins.dwell >= g_cfg.equipDwellS &&
            (now - ins.lastQualify) <= kEarnedHoldS) {
            static double s_lastEarned = 0.0;
            if (now - s_lastEarned > 1.0) {
                s_lastEarned = now;
                logger::info("[EQUIP] hand{} '{}': gesture already EARNED ({:.2f}s at the {}) - "
                             "held until you release, whatever the contact does now",
                             h, hd.base->GetName(), ins.dwell, ZoneName(ins.site));
            }
            return;
        }
        if (ins.actorFid && (now - ins.lastQualify) <= g_cfg.equipGraceS) {
            return;   // bank kept, dwell paused
        }
        ins = InsertState{};
        return;
    }

    // ── qualifying: run the dwell ────────────────────────────────────────────
    if (ins.actorFid != best->actorFormId) {
        ins = InsertState{};
        ins.actorFid = best->actorFormId;
    }
    ins.site        = bestZone;
    ins.dwell      += dt;
    if (-best->distU > ins.peakPressU) ins.peakPressU = -best->distU;   // force grade peak
    ins.lastQualify = now;
    ins.base        = hd.base;
    ins.refr        = hd.refr;
    ins.capSlot     = best->slot;          // v28: the capsule this frame qualified on
    ins.capChild    = best->child;
    ins.capLeft     = best->leftTwin != 0;

    {
        static double s_lastLog = 0.0;
        if (now - s_lastLog > 0.5) {
            s_lastLog = now;
            logger::info("[EQUIP] hand{} '{}' -> {} on 0x{:08X} d={:.2f}u dwell={:.2f}/{:.2f}s | {}",
                         h, hd.base->GetName(), ZoneName(bestZone), best->actorFormId,
                         best->distU, ins.dwell, g_cfg.equipDwellS,
                         best->orificeKind
                             ? fmt::format("PPB orifice open={:.2f}", best->orificeOpen / 255.f)
                             : std::string("orifice module off — distU fallback"));
        }
    }
}

// ── the eject processor ──────────────────────────────────────────────────────
// Runs ~0.6s after a failed-gesture release, once per release. Order of tests:
// world ref still standing = it simply fell (correct outcome, no-op); ref gone
// AND her count rose above the pre-release snapshot = a give (GBH's head/hand
// radius) swallowed it — DropObject puts it back at her feet.
void TickEject(int h, double now)
{
    PendingEject& pe = g_eject[h];
    if (!pe.active || now < pe.at) return;
    pe.active = false;
    if (g_cfg.returnFailedGifts == 0.f) return;   // GBH give is wanted

    if (auto r = pe.refr.get(); r && !r->IsDeleted() && r->Get3D()) {
        return;   // still a world object - it fell, exactly as it should
    }
    auto* form  = RE::TESForm::LookupByID(pe.actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !pe.base) return;
    const int nowCount = CountOf(actor, pe.base);
    if (nowCount <= pe.baseCount) return;   // nobody took it - nothing to undo
    const int delta = nowCount - pe.baseCount;

    // Native, and OWNERSHIP CLEARED. The Papyrus DropObject used before left
    // the item owned by HER, so the player's own gear came back flagged stolen
    // (user-reported 2026-08-22). It was the player's to begin with - a failed
    // gesture must not cost them the item or a bounty.
    for (int i = 0; i < delta; ++i) {
        RE::NiPoint3 hp{};
        const bool haveHand = PlayerHandPos(h == 1, hp);
        auto handle = actor->RemoveItem(pe.base, 1, RE::ITEM_REMOVE_REASON::kDropping,
                                        nullptr, nullptr, haveHand ? &hp : nullptr, nullptr);
        if (auto r = handle.get()) r->extraList.SetOwner(nullptr);
    }
    logger::info("[EQUIP] returned {}x '{}' from 0x{:08X} to the world, unowned - a failed "
                 "gesture must not gift the item away",
                 delta, pe.base->GetName(), pe.actorFid);
}

// -----------------------------------------------------------------------------
// * THE TWO-HAND UNDRESS (user design, 2026-08-22)
//
// "Anything that is dressed there": both hands HIGGS-grab the SAME worn piece
// - resolved by which armor covers the capsules each hand is on, so one hand
// on each side of a collar still matches - and the LATER-gripping hand pulls
// away. When the spread between the hands has grown past undressPullU, the
// piece comes off and lands IN the pulling hand (HIGGS GrabObject).
//
// Why not HIGGS's own worn-item pull: it requires a non-empty FULL name on
// the worn ARMO and all 770 DD rendered halves are unnamed (higgs utils.cpp:
// 88-96) - DD gear can never even highlight. And its plain RemoveItem is
// exactly the removal DD's scripts fight. So: our gesture, two backends -
//   DD device (worn half carries zad_* keywords) -> Papyrus helper
//     DoUnequipDevice: locked/quest devices REFUSE with a message (the
//     user's "key matters" rule, enforced by DD's own genericonly contract),
//     else UnlockDeviceByKeyword; the helper reports the freed INVENTORY
//     device back via the "PPB_GestureUnlocked" mod event and the DLL drops
//     it at the pulling hand.
//   Anything else (ZaZ, DOM, regular clothing/armor) -> native UnequipObject
//     + RemoveItem(kDropping at the hand) - no scripts to fight.
// Both ends: PendingHandGrab waits for the dropped ref's 3D, then
// g_higgs->GrabObject(ref, pullingHand) puts it in the player's grip.
// -----------------------------------------------------------------------------

// PPB capsule slot -> the biped slots that can cover it, most specific first.
// Slot numbers are Skyrim biped slots (30..61); DD assignments per
// zadLibs.GetSlotMaskForDeviceType (report 22 s2).
const int* UndressCandidates(int ppbSlot, int& nOut)
{
    static const int hand[]     = { 33, 59, 58, 34 };            // gloves, DD arm cuffs, forearms
    static const int forearm[]  = { 59, 58, 34, 33, 32 };
    static const int upperarm[] = { 34, 58, 59, 32 };
    static const int head[]     = { 44, 55, 46, 30, 31, 43 };    // gag, blindfold, hood-ish, helmet, hair, ears
    static const int neck[]     = { 45, 35, 47, 32 };            // DD collar, amulet, back, body
    static const int chest[]    = { 56, 46, 32 };                // bra, chest-secondary, body
    // 57/48 (the plug slots) trail the belly/waist lists so a support hand on
    // her LOWER BACK or belly resolves a worn plug when nothing covers it —
    // the user's plug-extraction gesture: one hand at the plug (COM zone),
    // the other anywhere on the lower body, pull apart. Body armor still
    // wins when she is dressed, which is physically right: no pulling a
    // plug through a dress.
    static const int belly[]    = { 49, 56, 32, 52, 57, 48 };    // belt, bra, body, underwear, plugs
    static const int waist[]    = { 49, 52, 32, 57, 48 };
    static const int com[]      = { 52, 49, 57, 48, 32 };        // underwear, belt, plugs, body
    static const int thigh[]    = { 53, 54, 37, 32, 57, 48 };    // DD leg cuffs, boots, body, plugs (support hand)
    static const int calf[]     = { 37, 53, 54, 32 };
    static const int foot[]     = { 37, 53, 54 };
    switch (ppbSlot) {
    case 0:  nOut = 4; return hand;
    case 1:  nOut = 5; return forearm;
    case 2:  nOut = 4; return upperarm;
    case 3:  nOut = 6; return head;
    case 7:  nOut = 4; return neck;
    case 6:  nOut = 3; return chest;
    // ★ 2026-09-02 FIX: belly[] and thigh[] hold SIX entries; nOut was 4 for both, which
    // truncated exactly the two trailing plug slots (57/48) the comment above says were
    // appended so a support hand on her belly or thigh resolves a worn plug. The documented
    // two-hand plug pull from the belly/thigh could never resolve a plug.
    case 5:  nOut = 6; return belly;
    case 4:  nOut = 5; return waist;
    case 11: nOut = 5; return com;
    case 8:  nOut = 6; return thigh;
    case 9:  nOut = 4; return calf;
    case 10: nOut = 3; return foot;
    }
    nOut = 0; return nullptr;
}

RE::TESObjectARMO* WornAt(RE::Actor* a, int bipedSlot)
{
    if (!a || bipedSlot < 30 || bipedSlot > 61) return nullptr;
    return a->GetWornArmor(
        static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << (bipedSlot - 30)));
}

bool IsDdWorn(RE::TESObjectARMO* w)
{
    auto* kwf = w ? w->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return false;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (kw && *kw) {
            const char* id = (*kw)->GetFormEditorID();
            if (id && _strnicmp(id, "zad_", 4) == 0) return true;
        }
    }
    return false;
}

// ★ THE BODY-PART RULE (user, 2026-08-22):
//   "so long the one holding is on the gear itself, and the other one is
//    pulling from the gear or from the same body part that the gear is
//    equipped on, then it will remove it."
// So one hand must be ON the piece; the other may be on the piece OR anywhere
// on the same part of the body. These are the parts. Sides are deliberately
// NOT distinguished - a cuff SET covers both wrists, and grabbing one of each
// and pulling apart is the natural way to take it off.
enum BodyRegion : int {
    kRegNone = 0, kRegArm, kRegHead, kRegTorso, kRegPelvis, kRegLeg,
};

BodyRegion RegionOfSlot(int ppbSlot)
{
    switch (ppbSlot) {
    case 0: case 1: case 2:  return kRegArm;     // hand, forearm, upper arm
    case 3: case 7:          return kRegHead;    // head + neck (a collar spans both)
    case 4: case 5: case 6:  return kRegTorso;   // waist, belly, chest
    case 11:                 return kRegPelvis;
    case 8: case 9: case 10: return kRegLeg;     // thigh, calf, foot
    }
    return kRegNone;
}

const char* RegionName(BodyRegion r)
{
    switch (r) {
    case kRegArm:    return "arm";
    case kRegHead:   return "head/neck";
    case kRegTorso:  return "torso";
    case kRegPelvis: return "pelvis";
    case kRegLeg:    return "leg";
    default:         return "-";
    }
}

// Same part, or a directly neighbouring one. The neighbours matter: the user's
// plug gesture explicitly allows the support hand on "the hips, butt cheek,
// lower back or somewhere close" - lower back is TORSO, the plug is PELVIS.
bool RegionsCompatible(BodyRegion a, BodyRegion b)
{
    // ★ A hand PPB cannot see is NOT a failure (2026-08-22). While HIGGS holds
    // an actor, the contact stream can go quiet for the hand doing the holding
    // - measured: "R: '-' @-" on every frame of a two-hand grab while the left
    // hand reported normally. We cannot ask where that hand is, so we let the
    // CLOSENESS GATE answer instead: two hands within a hand's width of each
    // other are on the same body part by construction. Proximity is the proof.
    if (a == kRegNone || b == kRegNone) return true;
    if (a == b) return true;
    const auto pair = [&](BodyRegion x, BodyRegion y) {
        return (a == x && b == y) || (a == y && b == x);
    };
    return pair(kRegHead,  kRegTorso) ||    // collar  <-> chest / shoulder
           pair(kRegTorso, kRegPelvis) ||   // belt    <-> lower back, plug <-> lower back
           pair(kRegPelvis, kRegLeg)   ||   // plug    <-> hip / thigh
           pair(kRegTorso, kRegArm);        // harness <-> shoulder / upper arm
}

struct HandGrip {
    RE::Actor*         actor   = nullptr;   // HIGGS-grabbed actor this hand holds
    double             since   = 0.0;       // when the grip started
    RE::TESObjectARMO* piece   = nullptr;   // worn piece under this hand
    double             pieceAt = 0.0;       // when the piece was last resolved
    BodyRegion         region  = kRegNone;  // where on her this hand is
};
HandGrip g_gripH[2];

struct UndressPair {
    bool               active   = false;
    RE::TESObjectARMO* piece    = nullptr;
    std::uint32_t      actorFid = 0;
    int                puller   = 0;        // the LATER-gripping hand
    float              d0       = 0.f;      // inter-hand distance at pair start
    // ★ 2026-08-23: the PPB capsule the hands armed ON. VRTE needs it to decide
    // the delivery tier — an undress performed over the BREAST interrupts, the
    // same garment taken off the belly does not. Captured at arming because by
    // the time the piece comes off the hands have travelled 25u and are nowhere
    // near where the gesture started.
    char               part[48] = {};
    // Where the PULLING controller was when the pair formed. The snap-back gate
    // measures against this, not against the hand-to-hand spread: the spread is
    // between two VIRTUAL hands, and a virtual hand held on her body does not
    // move no matter how far you drag the controller.
    RE::NiPoint3       pullStart{};
    // Passed undressPullU. ARMED, not fired - see the banner in TickUndress.
    bool               pulledFar = false;
};
UndressPair g_pair;

// ═══════════════════════════════════════════════════════════════════════════
// ★ THE PPB GESTURE EVENT BUS  (born in the DD-ZaZ add-on 2026-08-23, moved into PPB 2026-08-27)
//
// The user's architecture, stated plainly: "the AddOn is doing to VRTE exactly what PPB does to
// VRTE — it PUSHES information according to what its own .dll is doing in game, and VRTE is just
// exposing those events to the LLM."  The move makes that literal: PPB is the .dll that knows,
// so PPB is the .dll that pushes.
//
// Detection lives HERE (this file owns the whole two-hand undress state machine: arming, the 10u
// closeness gate, the grab-lifeline pull) and consumers own only narration policy. A consumer must
// never re-derive the gesture — two detectors for one gesture WILL drift, and silently.
//
// The names are PPB_Gesture*, NOT VRTE_DDZaZ_*. The DD/ZaZ vocabulary stays in the add-on, which
// listens for these and translates, so VRTouchEvents never learns DD concepts and the SFW/NSFW
// split the user set survives the move.
//
// Events, sender = the NPC (the wearer). ⚠ PpbTouchAPI.h is the CONTRACT — the exact field lists live there
// (corrected 2026-09-13: this banner had drifted). In short:
//   PPB_GestureUndressGrip / GripEnd   one hand's grab on a worn piece, and its end (R5, build 20105)
//   PPB_GestureUndressArm      strArg "<capsule>"            -> consumer SUPPRESSES grab narration
//   PPB_GestureUndressEnd      8 fields, ends "|<reason>|<sentence>"  -> un-suppress; narrate if done=1
//   PPB_GestureDeviceEquipped  7 fields, ends "|<force>"     -> wearer + onlooker lines
//   PPB_GestureGearEquipped    "<name>|<slotMask>|<force>|<ordinary>"  -> one persistent event, wearer only
//   PPB_GestureEquipRefused    7 fields, numArg = hand       -> a hand equip that did not happen (R1, build 20105)
//   PPB_GestureClaim           strArg "<actor FormID>"       -> bookkeeping: an equip or removal about to be
//                                                               seen on TESEquipEvent is ours, not a menu
//
// ⚠ The Arm/End PAIR is load-bearing: End fires on CANCEL too (a hand let go, the actor changed,
// the piece stopped being worn). Without it one aborted grab would silence that NPC's grab
// narration for the rest of the session.
//
// Inbound, the one event this file LISTENS for:
//   PPB_GestureUnlocked        strArg "<inventory FormID>"   numArg = leftHand
//     Sent by PPB_DeviceEquip.psc when a DD unlock succeeded, so the DLL can drop the freed device
//     at the pulling hand and HIGGS-grab it in. See UnlockSink.
// ═══════════════════════════════════════════════════════════════════════════
// ★ PPB emits GENERIC gesture events. The VRTE_DDZaZ_* vocabulary stays in the add-on, which
// listens for these and translates — so VRTouchEvents never learns DD concepts and the SFW/NSFW
// split the user set survives the move. Names are declared in PpbTouchAPI.h alongside the touch
// events, because they are part of the same published contract.
void SendGestureEvent(const char* name, const std::string& strArg, float numArg,
                      RE::TESForm* sender)
{
    auto* src = SKSE::GetModCallbackEventSource();
    if (!src) return;
    SKSE::ModCallbackEvent ev{};
    ev.eventName = name;
    ev.strArg    = strArg.c_str();
    ev.numArg    = numArg;
    ev.sender    = sender;
    src->SendEvent(&ev);
}

// The PPB capsule this hand is currently on for `actor`, or "". Mirrors
// PieceUnderHand's pick rule (a GRAB contact wins outright, else the nearest).
const char* HandCapsuleName(int h, RE::Actor* actor)
{
    if (!actor) return "";
    const PPBAPI::PpbTouchContact* pick = nullptr;
    for (int i = 0; i < g_snapN; ++i) {
        const auto& c = g_snap[i];
        if (c.wand != static_cast<unsigned char>(h)) continue;
        if (c.actorFormId != actor->GetFormID())     continue;
        if (c.toucherFormId != 0x14)                 continue;
        // ⛔ 2026-09-12: GENITAL and HEAD contacts publish wand 0 (they are not per-hand), so without
        // this a kiss or the wand could be taken as the RIGHT HAND's capsule / piece for the undress.
        if (c.sourceKind == PPBAPI::kSourceGenital || c.sourceKind == PPBAPI::kSourceHead) continue;
        if (c.sourceKind == PPBAPI::kSourceGrab) { pick = &c; break; }
        if (!pick || std::fabs(c.distU) < std::fabs(pick->distU)) pick = &c;
    }
    return pick ? pick->bodyPart : "";
}

// Close the undress out to VRTE. done=false is a CANCEL (un-suppress only).
// ★ 2026-09-13 (VRTE GearGestures R3): two fields APPENDED, "<reason>|<sentence>":
//   done       done=1, the pull completed (the rip is queued; a failed rip sends a corrective End, R4)
//   letgo      a hand released the grab without the snap-back
//   actor      the grabbed actor changed mid-pull
//   gone       the actor or the piece went away, or the piece stopped being worn before it came off
//   gate       the DD/ZaZ removal gate refused (sentence = the gate's own words)
//   paused     PPB_Native.SetGesturePaused(True) / PPB_GestureSetPaused while armed (R2)
//   disabled   undressEnabled / equipEnabled / enabled turned off while armed (R2)
//   ripfailed  a SECOND End, after a done=1, when the promised removal did not happen (R4)
// ⛔ No End is sent across a save load: kPreLoadGame resets the layer silently (the VM is not a safe place to be).
void SendUndressEndFor(std::uint32_t actorFid, RE::TESObjectARMO* piece, const char* part, bool done,
                       const char* reason, const char* sentence);
void SendUndressEnd(bool done, const char* reason = "", const char* sentence = "")
{
    if (!g_pair.actorFid) return;
    SendUndressEndFor(g_pair.actorFid, g_pair.piece, g_pair.part, done, reason, sentence);
}
void SendUndressEndFor(std::uint32_t actorFid, RE::TESObjectARMO* piece, const char* part, bool done,
                       const char* reason, const char* sentence)
{
    if (!actorFid) return;
    auto* form  = RE::TESForm::LookupByID(actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    const char* nm = (piece && piece->GetName() && piece->GetName()[0])
                         ? piece->GetName() : "";
    std::uint32_t slotMask = 0;
    if (piece) slotMask = static_cast<std::uint32_t>(piece->GetSlotMask().underlying());

    // ★ THE CLASS, because the NAME is on the other half (2026-08-24, user:
    // "The undress not having the right name is weird, can that be fixed? It
    // help with history context").
    //
    // What we pull off is the RENDERED half, and a DD rendered half has NO FULL
    // record - the name lives on the inventory half (report 23 §30). So the
    // gag came off as `piece='-'` and VRTE narrated "a piece of their gear".
    //
    // The rendered half DOES carry the class keywords, and that is the half we
    // are holding, so the class costs one keyword scan and needs nothing from
    // zadlibs. VRTE turns it into "the gag" / "the armbinder" through the
    // device dictionary it already has. Not the device's proper name, but the
    // right THING - which is what the history actually needs.
    char cls[64] = {};
    if (piece) {
        bool l = false, q = false;
        DdDescribe(piece, cls, sizeof cls, l, q);
    }
    const char* pt = part ? part : "";
    const char* rs = (reason && reason[0]) ? reason : (done ? "done" : "");

    // ⚠ SIXTH FIELD APPENDED, and that is safe here: VRTE's handler tests
    // `f.Length < 5`, so a longer payload still passes. (This is NOT the
    // 16-field VRTE_Contact payload, where f[15] is the escalation flag and
    // appending anything silently kills escalation mod-wide - report 22.)
    // 2026-09-13: fields 7-8 (reason, sentence) appended the same way; every text field is '|'-free.
    char nmF[96], ptF[64], clF[64], snF[192];
    FieldCopy(nmF, sizeof nmF, nm);
    FieldCopy(ptF, sizeof ptF, pt);
    FieldCopy(clF, sizeof clF, cls);
    FieldCopy(snF, sizeof snF, sentence);
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s|%u|%d|%d|%s|%s|%s|%s",
                  nmF, slotMask, done ? 1 : 0,
                  (piece && IsDdWorn(piece)) ? 1 : 0, ptF, clF, rs, snF);
    SendGestureEvent("PPB_GestureUndressEnd", buf, 0.f, actor);
    logger::info("[UNDRESS->VRTE] End done={} reason={} piece='{}' cls='{}' slot={} part='{}'{}{}",
                 done ? 1 : 0, rs[0] ? rs : "-", nm[0] ? nm : "-", cls[0] ? cls : "-", slotMask,
                 pt[0] ? pt : "-", snF[0] ? " | " : "", snF);
}

// piece -> not-before time; one rip must not chain-fire on the next frame
std::unordered_map<std::uint32_t, double> g_undressCd;



// Queued from the mod-event sink (VM thread) -> consumed on the frame hook.
struct PendingUnlockDrop {
    std::atomic<bool> active{ false };
    std::uint32_t     actorFid = 0;
    std::uint32_t     baseFid  = 0;
    bool              left     = false;
};
PendingUnlockDrop g_unlockDrop;

// ⛔ The DD removal is a ROUND TRIP - we dispatch to Papyrus, DD unlocks, and
// the helper answers with a mod event. The DLL's intent for the freed item
// ("into the pulling hand" for a two-hand pull, "just let it fall" for a
// finger extraction) cannot ride along in that event, so it is parked here and
// read when the answer arrives. Without this, a plug pulled out by a finger
// was force-grabbed into the very hand that was still inside her - it
// materialised inside her body mesh and looked like it had vanished
// (user-reported 2026-08-22).
struct RipIntent {
    std::uint32_t actorFid = 0;
    bool          intoHand = true;
    bool          left     = false;
    // The RENDERED (worn) armor we ripped. DD's delete gate reads its inventory
    // count, so this is what PendingSafeDrop watches. See its banner.
    std::uint32_t renderedFid = 0;
    double        at       = 0.0;
};
RipIntent g_ripIntent;

// ⛔⛔ DD DELETES THE DEVICE WE HAND OVER (root-caused 2026-08-23, report 23 §27).
// `zadEquipScript::OnContainerChanged` (zadEquipScript.psc:389-391 — the file's
// ONLY self.Delete()) destroys any device reference dropped out of an actor who
// still reads as wearing it:
//     elseif akOldContainer && !akNewContainer          ; Item is being dropped.
//         if libs.IsWearingDevice(akOldContainer as actor, DeviceRendered, ...) == 1
//             libs.Log(deviceName+" dropped.")
//             self.Delete()
// `ReleaseToHand`'s RemoveItem(..., kDropping, ...) IS "out of an actor, into no
// container", and DD's rendered-half removal is asynchronous — so dropping on
// the modevent frame (~38 ms after the unlock) loses that race by construction
// and the plug is destroyed inside the player's closed hand. It leaves no error,
// only `[Zad-NG]: <name> dropped.` in Papyrus.
//
// The half of DD's gate we can observe is `GetItemCount(deviceRendered) != 0`, so
// we hold the drop until that count reaches zero (or `ddSettleS` expires) and
// only then put the item in the world.
struct PendingSafeDrop {
    bool          active   = false;
    std::uint32_t actorFid = 0;
    std::uint32_t baseFid  = 0;
    std::uint32_t rendFid  = 0;
    bool          left     = false;
    bool          intoHand = true;
    double        deadline = 0.0;
    int           waits    = 0;
};
PendingSafeDrop g_safeDrop;



// Drop `base` out of `actor` at the given hand and queue the HIGGS grab into it.

void TickHandGrabs(double now)
{
    for (int i = 0; i < 2; ++i) {
        PendingHandGrab& pg = g_handGrab[i];
        if (!pg.active) continue;
        auto r = pg.ref.get();
        if (!r || now > pg.timeout) {
            if (now > pg.timeout) {
                logger::info("[UNDRESS] hand-over gave up after {} attempt(s) - the item is on the "
                             "ground where it came off, not in your hand", pg.tries);
            } else {
                // ⛔ The handle went invalid BEFORE the timeout - something
                // DESTROYED the reference we were handing over. This path used
                // to clear silently, which is exactly why DD's
                // OnContainerChanged self.Delete() stayed invisible for a whole
                // session (report 23 §27). Never fail silently.
                logger::info("[UNDRESS] the item was DELETED out of the {} hand before the hand-over "
                             "completed, after {} attempt(s). If Papyrus shows "
                             "'[Zad-NG]: <name> dropped.' this is DD's OnContainerChanged delete - "
                             "raise ddSettleS.", pg.left ? "left" : "right", pg.tries);
            }
            pg.active = false;
            continue;
        }
        if (!r->Get3D()) continue;              // not spawned yet - wait
        if (pg.clearOwner) {
            r->extraList.SetOwner(nullptr);     // not stolen
            pg.clearOwner = false;              // once is enough
        }
        const char* nm = r->GetBaseObject() ? r->GetBaseObject()->GetName() : "?";
        if (!pg.grabIt) {
            logger::info("[UNDRESS] '{}' dropped at the {} hand", nm, pg.left ? "left" : "right");
            pg.active = false;
            continue;
        }
        // ★ VERIFY THE HAND-OVER (2026-08-22). The old code called GrabObject
        // ONCE and logged success unconditionally - so an extracted plug HIGGS
        // declined to take was reported as "grabbed into the right hand" while
        // it actually sat inside her, and read to the user as vanished. Now it
        // is retried every frame until HIGGS confirms it is holding it, and a
        // give-up says so plainly.
        // ★ IDENTITY, not occupancy. `IsHoldingObject(hand)` only answers "is
        // that hand holding ANYTHING" - taking that as proof of OUR hand-over is
        // the same unsound-verification class as the three in §21 addendum 6. A
        // hand already full of something else would have reported success.
        if (g_higgs->IsHoldingObject(pg.left) && g_higgs->GetGrabbedObject(pg.left) == r.get()) {
            logger::info("[UNDRESS] '{}' is in the {} hand (confirmed, {} attempt(s))",
                         nm, pg.left ? "left" : "right", pg.tries);
            pg.active = false;
            continue;
        }
        if (!g_higgs->IsHandInGrabbableState(pg.left)) continue;   // hand busy - keep waiting
        g_higgs->GrabObject(r.get(), pg.left);
        ++pg.tries;
    }
}

// Resolve which worn piece hand `h` is on: the hand's PPB contact (prefer the
// GRAB contact - it names the grabbed capsule) -> candidate biped slots ->
// first worn armor.
RE::TESObjectARMO* PieceUnderHand(int h, RE::Actor* actor,
                                  const PPBAPI::PpbTouchContact* buf, int n,
                                  BodyRegion* regOut)
{
    if (regOut) *regOut = kRegNone;
    const PPBAPI::PpbTouchContact* pick = nullptr;
    for (int i = 0; i < n; ++i) {
        const auto& c = buf[i];
        if (c.wand != static_cast<unsigned char>(h)) continue;
        if (c.actorFormId != actor->GetFormID()) continue;
        if (c.toucherFormId != 0x14) continue;
        // ⛔ 2026-09-12: same rule as HandCapsuleName — wand 0 is not "the right hand" for these kinds.
        if (c.sourceKind == PPBAPI::kSourceGenital || c.sourceKind == PPBAPI::kSourceHead) continue;
        if (c.sourceKind == PPBAPI::kSourceGrab) { pick = &c; break; }
        if (!pick || std::fabs(c.distU) < std::fabs(pick->distU)) pick = &c;
    }
    if (!pick) return nullptr;
    if (regOut) *regOut = RegionOfSlot(pick->slot);
    int nc = 0;
    const int* cand = UndressCandidates(pick->slot, nc);
    for (int i = 0; i < nc; ++i) {
        if (auto* w = WornAt(actor, cand[i])) return w;
    }
    return nullptr;
}

// The class mask of a piece ALREADY WORN. The rendered DD half carries its
// class keyword directly, so DdMask applies per keyword; ZaZ/DOM fall through
// to their own table. Used to find which worn plug a finger is behind.
int WornMask(RE::TESObjectARMO* w)
{
    auto* kwf = w ? w->As<RE::BGSKeywordForm>() : nullptr;
    if (!kwf) return 0;
    int mask = 0;
    for (std::uint32_t i = 0; i < kwf->GetNumKeywords(); ++i) {
        auto kw = kwf->GetKeywordAt(i);
        if (kw && *kw) mask |= DdMask((*kw)->GetFormEditorID());
    }
    if (!mask) mask = PlainMask(w);
    return mask;
}

// ★ 2026-09-06: tell SeverActions (if it controls her outfit) that her worn set changed.
// mode 1 = a confirmed ordinary equip, 2 = a plain rip. Plain ints; the Papyrus side owns the
// SeverActions soft dependency (PPB_DeviceEquip.DoOutfitRecord).
void DispatchOutfitRecord(std::uint32_t actorFid, int mode, std::uint32_t baseFid)
{
    if (g_cfg.equipSaHandoff == 0.f) return;
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) return;
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
    // baseFid: the piece itself — the NFF branch moves it into her outfit chest (2026-09-06)
    auto* args = RE::MakeFunctionArguments(static_cast<std::int32_t>(actorFid), static_cast<std::int32_t>(mode),
                                           static_cast<std::int32_t>(baseFid));
    vm->DispatchStaticCall("PPB_DeviceEquip", "DoOutfitRecord", args, cb);
    delete args;
}

// ★★ v15 THE ADDON'S REMOVAL GATE (their change request 2026-09-08).
// Returns the refusal sentence, or nullptr when removal is ALLOWED or the gate cannot answer.
// `gateAllowed` is set only on an explicit "" (allow) — the caller uses it to bypass the Papyrus
// lock rule, and must NOT bypass it on a nullptr return.
//
// WHY THE GATE OWNS THIS. DD's unlock rule is per DEVICE, not per keyword: the key is a Papyrus
// property on the INVENTORY half (zadEquipScript.deviceKey + NumberOfKeysNeeded) which C++ cannot
// read, and at removal time only the RENDERED half is in hand. The AddOn captures the pair when DD
// announces an equip. PPB's own rule refused every zad_Lockable device without ever reading an
// inventory, which is why a skeleton key in the player's pack did nothing.
//
// ⚠ THREE-STATE, and the third is the point: refused / allowed / CANNOT ANSWER. A device the AddOn
// never saw equipped has no key on record. Today (their build 2) that is reported as a refusal and
// is indistinguishable from a genuine one, so PPB cannot yet tell them apart — we have asked for an
// appended RemovalStateOf(). Until it lands, an unanswerable device simply keeps PPB's own local
// rule, which is exactly today's behaviour, so this change can only ever ADD removals, never remove
// one that works now.
static const char* RemovalGateRefusal(RE::Actor* actor, std::uint32_t renderedFid,
                                      bool byHand, bool& gateAllowed)
{
    gateAllowed = false;
    if (!actor || !renderedFid) return nullptr;
    if (!g_ddzGate) return nullptr;
    const std::uint32_t gb = g_ddzGate->GetBuildNumber();
    if (gb < 2) return nullptr;                     // absent/old: PPB's own lock rule, unchanged
    const std::uint32_t aFid = actor->GetFormID();
    // ★★ v18a BUILD 3 — the third state, which is the whole reason we asked for it. The AddOn learns a
    // device's key when DD announces its EQUIP, so anything worn before their build went in has no key
    // on record. Build 2 reported that as BLOCKED, indistinguishable from a real refusal, so PPB showed
    // THEIR sentence about a missing key while the player was holding it. UNKNOWN is not a refusal and
    // it is NOT fail-open: PPB falls back to its own lock rule, which still refuses the device — for
    // PPB's reason, in PPB's words. That is exactly the behaviour that existed before any of this.
    if (gb >= 3) {
        const int st = g_ddzGate->RemovalStateOf(aFid, renderedFid, byHand);
        if (st == 0) { gateAllowed = true; return nullptr; }            // ALLOW
        if (st != 1) {                                                  // UNKNOWN, or any later state
            logger::info("[UNDRESS] DD/ZaZ gate holds no key record for 0x{:08X} (state {}) — not a "
                         "refusal; falling back to PPB's own lock rule", renderedFid, st);
            return nullptr;
        }
        const char* w3 = g_ddzGate->RemovalBlockedBy(aFid, renderedFid, byHand);   // BLOCKED
        return (w3 && w3[0]) ? w3 : nullptr;
    }
    const char* why = g_ddzGate->RemovalBlockedBy(aFid, renderedFid, byHand);
    if (!why) return nullptr;                       // defensive: null = cannot answer
    if (why[0] == '\0') { gateAllowed = true; return nullptr; }         // "" = ALLOW
    return why;                                     // a finished sentence, ready to show
}

// Take one worn piece off her: DD through its own framework (which enforces the
// lock rule), anything else natively. `intoHand` puts it in the player's grip.
// ★ 2026-09-13 (VRTE GearGestures R4): returns false when the removal was NOT dispatched (no actor/piece, or the
// removal gate refused — `refusedOut` then carries the gate's sentence). True means the unequip was handed to the
// engine / DD's helper; whether it LANDED is checked a beat later by the rip check (TickRipCheck).
bool RipPiece(RE::Actor* actor, RE::TESObjectARMO* piece, bool left, bool intoHand,
              const char* why, const char** refusedOut = nullptr)
{
    if (refusedOut) *refusedOut = nullptr;
    if (!actor || !piece) return false;
    // ⛔ v15: ASK THE GATE BEFORE THE CLAIM BELOW. Every removal funnels through here, so this one
    // test covers the two-hand undress and the finger extraction alike — and it sits above
    // SendGestureEvent so a refused removal never announces itself (the 2026-08-27 rule: an event
    // that promises a removal which never arrives is worse than no event).
    const bool  byHandGate  = (why && _stricmp(why, "finger") == 0);
    bool        gateAllowed = false;
    if (const char* refused = RemovalGateRefusal(actor, piece->GetFormID(), byHandGate, gateAllowed)) {
        if (g_cfg.undressNotify != 0.f) Notify(refused);
        logger::info("[UNDRESS] RIP refused by the DD/ZaZ removal gate ({}): {}",
                     why ? why : "?", refused);
        if (refusedOut) *refusedOut = refused;
        return false;
    }
    // ★ Same claim as DoEquip, for the removal direction. Every removal this
    // AddOn performs by hand - a two-hand undress and a finger plug extraction
    // alike - funnels through here, so one claim covers both gestures.
    SendGestureEvent("PPB_GestureClaim", std::to_string(actor->GetFormID()), 0.f, actor);
    // ★ v29d: a REMOVAL moves her geometry too — taking heeled boots off drops both feet ~8 u — so the same settle
    // applies to every removal PPB performs (the gate has already allowed it at this point).
    PushStep::NoteEquipEvent(actor->GetFormID());
    const std::uint32_t pieceFid = piece->GetFormID();
    const char* nm = (piece->GetName() && piece->GetName()[0]) ? piece->GetName()
                                                               : "unnamed piece";
    if (IsDdWorn(piece)) {
        // The helper enforces the lock rule and reports the freed INVENTORY
        // device back through the PPB_GestureUnlocked mod event. Park what we
        // want done with it - the event cannot carry that.
        g_ripIntent.actorFid    = actor->GetFormID();
        g_ripIntent.intoHand    = intoHand;
        g_ripIntent.left        = left;
        g_ripIntent.renderedFid = pieceFid;
        g_ripIntent.at          = NowS();
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (vm) {
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            // 4th arg: 1 = a FINGER extraction, which is exempt from the
            // zad_Lockable refusal (the belt gate above is its lock instead).
            // A two-hand pull passes 0 and still needs the key.
            const bool fromFinger = (why && _stricmp(why, "finger") == 0);
            auto* args = RE::MakeFunctionArguments(
                static_cast<std::int32_t>(actor->GetFormID()),
                static_cast<std::int32_t>(pieceFid),
                left ? 1.0f : 0.0f,
                // v15: 2.0 = the AddOn's gate already approved this removal (the actor holds the
                // right key, and nothing is worn over it). The psc tests `fromFinger == 0.0`, so any
                // non-zero exempts the lock rule — 2.0 therefore bypasses it with NO psc edit and no
                // pex rebuild, while staying distinguishable from a finger extraction in the log.
                gateAllowed ? 2.0f : (fromFinger ? 1.0f : 0.0f));
            vm->DispatchStaticCall("PPB_DeviceEquip", "DoUnequipDevice", args, cb);
            delete args;
        }
        logger::info("[UNDRESS] RIP ({}, DD): '{}' -> DoUnequipDevice ({})", why, nm,
                     gateAllowed ? "DD/ZaZ gate APPROVED - lock rule bypassed"
                                 : "lock rule applies (gate absent or could not answer)");
    } else {
        if (auto* aem = RE::ActorEquipManager::GetSingleton()) {
            // forceEquip=true (7th arg): the piece may have been equipped by PPB with
            // preventUnequip, and without force our OWN undress would refuse to take it off.
            if (g_cfg.equipOutfitTag != 0.f) OutfitTag::Set(actor, piece, false);   // a rip ends the outfit claim
            aem->UnequipObject(actor, piece, nullptr, 1, nullptr,
                               true, /*forceEquip*/ true, false, false, nullptr);
        }
        ReleaseToHand(actor, piece, left, intoHand);
        logger::info("[UNDRESS] RIP ({}, plain): '{}' off {}", why, nm,
                     actor->GetDisplayFullName());
        DispatchOutfitRecord(actor->GetFormID(), 2, piece ? piece->GetFormID() : 0);   // SA/NFF-controlled? then her outfit loses the piece
        if (g_cfg.undressNotify != 0.f) {
            char nb[160];
            std::snprintf(nb, sizeof nb, "%s removed", nm);
            Notify(nb);
        }
    }
    return true;
}

// ★ FINGER EXTRACTION (user design, 2026-08-22): an orifice with a plug in it
// is still reachable, so putting a BARE finger in and holding it there is the
// removal gesture. No second hand, no collision on the plug's stem, no way to
// do it by accident - you have to be inside her.
struct PlugPull {
    std::uint32_t actorFid = 0;
    int           site     = 0;
    float         dwell    = 0.f;
    double        last     = 0.0;
};
PlugPull g_plugPull[2];

// The worn plug for an orifice, if she has one. DD uses slot 57 vaginal /
// 48 anal; other frameworks vary, so the candidate slots are checked in order
// and the piece must actually classify as a plug for THAT orifice.
// ★ A PLUG UNDER A BELT DOES NOT COME OUT (user, 2026-08-24): "you can lock a
// plug in the orifice, you need a chastity belt on top."
//
// That is the real rule, and it is better than the lock keyword: a plug is not
// held in by its own lock, it is held in by the thing covering it. So finger
// extraction ignores zad_Lockable entirely (a finger working a plug loose is
// not picking a lock) and is stopped by the BELT instead - which is both what
// DD models and what a body does.
//
// Matched by DD's own class rather than by slot, because slot 49 carries belts,
// corsets and underwear alike and only one of them is a barrier.
RE::TESObjectARMO* WornChastityBelt(RE::Actor* a)
{
    static const int slots[] = { 49, 52, 32, 48, 57 };
    for (int i = 0; i < 5; ++i) {
        auto* w = WornAt(a, slots[i]);
        if (!w) continue;
        char cls[64] = {};
        bool l = false, q = false;
        DdDescribe(w, cls, sizeof cls, l, q);
        if (cls[0] && _stricmp(cls, "Belt") == 0) return w;
    }
    return nullptr;
}

RE::TESObjectARMO* WornPlug(RE::Actor* a, int site)
{
    static const int vag[] = { 57, 48, 52, 49 };
    static const int anl[] = { 48, 57, 52, 49 };
    const int* order = (site == kSiteVaginal) ? vag : anl;
    for (int i = 0; i < 4; ++i) {
        auto* w = WornAt(a, order[i]);
        if (w && (WornMask(w) & site)) return w;
    }
    return nullptr;
}

void TickPlugPull(int h, float dt, double now)
{
    PlugPull& pp = g_plugPull[h];
    if (g_cfg.plugPullS <= 0.f) { pp = PlugPull{}; return; }
    // A hand holding a device is doing an EQUIP - never an extraction.
    if (g_held[h].live) {
        if (pp.dwell > 0.15f)
            logger::info("[UNDRESS] {} finger extraction cancelled at {:.2f}s - that hand is holding "
                         "an item, so it is doing an equip", h == 1 ? "LEFT" : "RIGHT", pp.dwell);
        pp = PlugPull{};
        return;
    }

    // ★ DEEPEST ORIFICE CONTACT WINS - NOT THE FIRST ONE IN THE BUFFER (2026-08-23).
    // This used to take the first contact carrying an orifice bit, with
    // `if (vaginal) ... else if (anal)`, so VAGINAL ALWAYS WON any frame that
    // carried both - and the digest gives no stable ordering. A finger held in
    // the ANUS therefore had its site stolen by any vaginal contact from the
    // same hand; `pp.site != site` then reset the dwell to zero every time it
    // flipped, so an anal extraction could never reach plugPullS while a
    // vaginal one always did. That is exactly the reported symptom: the butt
    // plug tail would not come out, the vaginal plug always would.
    const PPBAPI::PpbTouchContact* buf = g_snap;
    const int n = g_snapN;
    std::uint32_t bestFid[2] = { 0, 0 };          // [0] = vaginal, [1] = anal
    float         bestD[2]   = { 1.0e9f, 1.0e9f };
    for (int i = 0; i < n; ++i) {
        const auto& c = buf[i];
        if (c.wand != static_cast<unsigned char>(h) || c.toucherFormId != 0x14) continue;
        if (c.sourceKind != PPBAPI::kSourceFinger && c.sourceKind != PPBAPI::kSourceHand)
            continue;
        int z = 0; bool g = false;
        // The plug pull IS an orifice gesture, so it asks for the orifice
        // verdict explicitly - this is the one caller that wants it.
        ZoneOfContact(c, kSiteVaginal | kSiteAnal | kSiteMouth, z, g);
        if (!g) continue;
        const int k = (z & kSiteVaginal) ? 0 : ((z & kSiteAnal) ? 1 : -1);
        if (k < 0) continue;
        if (c.distU < bestD[k]) { bestD[k] = c.distU; bestFid[k] = c.actorFormId; }
    }
    // ★ STICKINESS: a dwell already running KEEPS its orifice for as long as
    // that orifice is still being touched. The user's rule is "hold the finger
    // in and it comes out", so nothing about the OTHER orifice may restart the
    // count - only an empty hand or a genuinely different actor does.
    std::uint32_t fid = 0;
    int site = 0;
    const int cur = (pp.site == kSiteVaginal) ? 0 : ((pp.site == kSiteAnal) ? 1 : -1);
    if (cur >= 0 && bestFid[cur] && bestFid[cur] == pp.actorFid) {
        fid = pp.actorFid; site = pp.site;
    } else if (bestFid[0] && (!bestFid[1] || bestD[0] <= bestD[1])) {
        fid = bestFid[0]; site = kSiteVaginal;
    } else if (bestFid[1]) {
        fid = bestFid[1]; site = kSiteAnal;
    }
    if (!fid) {
        if (pp.actorFid && now - pp.last > 0.4) {                  // flicker grace
            if (pp.dwell > 0.15f)
                logger::info("[UNDRESS] {} finger extraction lost contact at {:.2f}/{:.2f}s - the "
                             "finger left the orifice", h == 1 ? "LEFT" : "RIGHT",
                             pp.dwell, g_cfg.plugPullS);
            pp = PlugPull{};
        }
        return;
    }
    if (pp.actorFid != fid || pp.site != site) {
        if (pp.dwell > 0.15f)
            logger::info("[UNDRESS] {} finger extraction restarted at {:.2f}s - moved to a different {}",
                         h == 1 ? "LEFT" : "RIGHT", pp.dwell,
                         pp.actorFid != fid ? "actor" : "orifice");
        pp = PlugPull{};
        pp.actorFid = fid;
        pp.site     = site;
    }
    pp.dwell += dt;
    pp.last   = now;

    auto* form  = RE::TESForm::LookupByID(fid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor) { pp = PlugPull{}; return; }
    auto* plug = WornPlug(actor, site);
    if (!plug) {
        // Finger is inside, but nothing in there resolves as a plug. Silent
        // until now, which made "it just does not work" undiagnosable.
        static double s_nop[2] = { 0.0, 0.0 };
        if (now - s_nop[h] > 2.0) {
            s_nop[h] = now;
            logger::info("[UNDRESS] {} finger is in the {} of 0x{:08X} but nothing there resolves as "
                         "a plug (checked DD slots 57/48/52/49)",
                         h == 1 ? "LEFT" : "RIGHT", ZoneName(site), fid);
        }
        return;
    }

    {
        // ⚠ PER HAND. This throttle used to be one function-scope static shared
        // by both calls, so one hand could swallow the other's log slot and the
        // line never named the hand at all - which is exactly why a left-hand
        // extraction delivering into the right hand was invisible in the log
        // (2026-08-23). Name the hand, and never let one hand mute the other.
        static double s_last[2] = { 0.0, 0.0 };
        if (now - s_last[h] > 0.5) {
            s_last[h] = now;
            logger::info("[UNDRESS] {} finger in the {} of 0x{:08X} on '{}': {:.2f}/{:.2f}s",
                         h == 1 ? "LEFT" : "RIGHT", ZoneName(site), fid,
                         (plug->GetName() && plug->GetName()[0]) ? plug->GetName() : "a plug",
                         pp.dwell, g_cfg.plugPullS);
        }
    }
    if (pp.dwell < g_cfg.plugPullS) return;

    // The belt is the lock. Checked HERE rather than in the Papyrus half so the
    // player is told why on the frame it fails, and so the cooldown below is
    // never spent on an attempt that cannot succeed.
    if (auto* belt = WornChastityBelt(actor)) {
        static double s_belt[2] = { 0.0, 0.0 };
        if (now - s_belt[h] > 3.0) {
            s_belt[h] = now;
            const char* bn = (belt->GetName() && belt->GetName()[0]) ? belt->GetName()
                                                                    : "a chastity belt";
            logger::info("[UNDRESS] {} finger extraction blocked - '{}' is locked over it",
                         h == 1 ? "LEFT" : "RIGHT", bn);
            if (g_cfg.undressNotify != 0.f) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "%s is locked over it.", bn);
                Notify(msg);
            }
        }
        return;
    }

    const std::uint32_t plugFid = plug->GetFormID();
    if (auto it = g_undressCd.find(plugFid); it != g_undressCd.end() && now < it->second) return;
    g_undressCd[plugFid] = now + 2.0;
    // Into the extracting hand by default: pointing in VR means grip held with
    // the trigger released, so the hand is already in a grab-ready pose and
    // HIGGS can take the plug straight off the fingertip (user, 2026-08-22).
    logger::info("[UNDRESS] extraction fired on the {} hand - '{}' goes to that hand",
                 h == 1 ? "LEFT" : "RIGHT",
                 (plug->GetName() && plug->GetName()[0]) ? plug->GetName() : "a plug");
    // ⛔⛔ DO NOT ADD A COOLDOWN HERE. There is already one, fifteen lines up at
    // `g_undressCd[plugFid] = now + 2.0`, and it runs BEFORE this point.
    // A second copy added 2026-08-24 tested the very entry the first had just
    // written, so it was true every single time and `RipPiece` was never
    // reached: the log said "extraction fired" and no plug ever came out.
    // ★ Read the whole function before adding a guard to it - the anchor you
    //   are patching is not the whole story, and a duplicate guard is worse
    //   than a missing one because it looks correct in the diff.
    // ⚠ THE PlugRemoved EMIT THAT USED TO BE HERE IS GONE, and deliberately.
    //
    // It was added on 2026-08-26 because the finger extraction told VRTE
    // nothing at all. That was true, but fixing it HERE fixed exactly one of
    // the ways a plug can leave a body - a menu unequip, a key unlock, DD's own
    // RemoveDevice and any third-party script all still said nothing.
    //
    // ★ The TESEquipEvent sink in WornDevices now emits PlugRemoved for EVERY
    // path, this one included, and RipPiece claims the actor a line later so the
    // sink knows the act was ours rather than a menu. One emitter, every route,
    // and no window in which two of them could both fire.
    // ★ THE PLUG GESTURE, ON THE API (2026-08-27, user: "make sure that the gesture of the plug
    // is properly exposed to the API which is what VRTE use"). Emitted BEFORE the rip so the
    // consumer knows the removal that is about to arrive was a fingertip working it loose, not
    // a menu, a key, or DD's own RemoveDevice. The add-on's PlugRemoved still fires for EVERY
    // route (that is its job); this one fires only for the gesture.
    {
        const char* pn = (plug->GetName() && plug->GetName()[0]) ? plug->GetName() : "a plug";
        char pcls[64] = {}; bool pl = false, pq = false;
        DdDescribe(plug, pcls, sizeof pcls, pl, pq);

        // ⛔ REFUSE BEFORE ANNOUNCING (2026-08-27). PPB_DeviceEquip.DoUnequipDevice's FIRST guard
        // returns without removing anything when the rendered half carries zad_QuestItem or
        // zad_BlockGeneric. Emitting "out" before the round trip meant a quest-locked plug
        // reported an extraction on every single pull and never came out — an event that promises
        // a removal which never arrives is worse than no event, because a consumer cannot tell the
        // two apart. DdDescribe already reports exactly those two keywords as `quest`, so the
        // refusal is free here, and answering in the DLL also gets the player the message a frame
        // earlier instead of after a VM round trip.
        // ⚠ This does NOT cover DD's two remaining internal refusals (no class keyword in
        // zadDeviceTypes; GetWornDevice returning None). Those are unknowable without zadlibs and
        // are rare; the script still refuses them, and they still emit. Documented in the header.
        if (pq) {
            logger::info("[UNDRESS] {} finger extraction refused - '{}' is a quest/blocked device",
                         h == 1 ? "LEFT" : "RIGHT", pn);
            if (g_cfg.undressNotify != 0.f)
                Notify("It is locked on tight - you don't have the key.");
            pp = PlugPull{};
            return;
        }

        // ★ THE SITE COMES FROM WornMask, NOT DdMask(class) (2026-08-27). WornMask is literally
        // the predicate WornPlug() used to SELECT this plug (`WornMask(w) & site`), so the site we
        // publish and the site we searched agree by construction — and it falls back to PlainMask,
        // so a ZaZ / DoM / zRavenous plug with no DD keyword reports its real orifice instead of
        // the 0 that DdMask("") returns. `class` stays empty for those, honestly: they have none.
        // ⛔ v15: the gate decides BEFORE "out" is announced. RipPiece re-asks and would refuse
        // anyway, but by then this event has already promised an extraction (2026-08-27).
        {
            bool ga = false;
            if (const char* refused = RemovalGateRefusal(actor, plug->GetFormID(), true, ga)) {
                logger::info("[PLUG] {} extraction refused by the DD/ZaZ removal gate: {}",
                             h == 1 ? "LEFT" : "RIGHT", refused);
                if (g_cfg.undressNotify != 0.f) Notify(refused);
                pp = PlugPull{};
                return;
            }
        }
        char pbuf[192];
        std::snprintf(pbuf, sizeof pbuf, "out|%s|%s|%d|%d",
                      pn, pcls, WornMask(plug), h == 1 ? 1 : 0);
        SendGestureEvent("PPB_GesturePlug", pbuf, 0.f, actor);
    }
    RipPiece(actor, plug, h == 1, g_cfg.plugToHand != 0.f, "finger");
    pp = PlugPull{};
}

// ⛔⛔ THE RIP IS DEFERRED BY ONE FRAME (2026-08-24, after a hard freeze).
//
// The AddOn log stops mid-gesture, on the line after `RIP (snap, plain)`, with
// no crash log and Papyrus already quiet: a hang, not a crash. What changed
// just before it is that the rip moved into the GRIP-LOSS branch - so
// RipPiece -> UnequipObject -> RemoveItem -> queue a HIGGS GrabObject now all
// run inside HIGGS's frame callback, at the exact moment HIGGS is tearing down
// its own grab on that same hand. Re-entering a callback owner while it is
// mid-teardown is the shape of this freeze.
//
// ⚠ NOT PROVEN - a freeze with no crash log names no culprit, and this module
// is not the only thing in the frame. But it is the right structure anyway:
// this file already defers every other engine-touching action out of the
// callback (PendingVerify, PendingSafeDrop, PendingHandGrab, PendingEject) for
// exactly this reason, and the rip was the one that did not.
// ⚠ AddTask is NOT the answer - it is banned from this callback (KNOWLEDGEBASE:
// it hard-freezes the game). A pending struct picked up next tick is.
//
// The VRTE event still goes out immediately: SendEvent from the callback is
// documented safe, and the narration should not wait on a frame.
struct PendingRip {
    bool                active   = false;
    double              at       = 0.0;
    std::uint32_t       actorFid = 0;
    RE::TESObjectARMO*  piece    = nullptr;
    bool                left     = false;
    char                why[16]  = {};
    char                part[48] = {};   // R4: the capsule the pair armed on, for a corrective End
};
PendingRip g_pendingRip;

// ★ 2026-09-13 (VRTE GearGestures R4): UndressEnd(done=1) is announced BEFORE the rip, so a rip that never lands
// must be corrected. The rip check looks once, after the removal had time to finish — a plain UnequipObject is
// QUEUED (queueEquip=true) and DD's removal is a Papyrus round trip plus the ddSettleS drop, so an immediate look
// would read "still worn" for removals that are fine. 2.5 s is the same budget the equip verify uses.
struct RipCheck {
    bool                active   = false;
    double              at       = 0.0;
    std::uint32_t       actorFid = 0;
    RE::TESObjectARMO*  piece    = nullptr;
    char                part[48] = {};
};
RipCheck g_ripCheck;

void TickRipCheck(double now)
{
    RipCheck& rc = g_ripCheck;
    if (!rc.active || now < rc.at) return;
    rc.active = false;
    auto* form  = RE::TESForm::LookupByID(rc.actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !rc.piece) return;                 // nothing left to check against — say nothing
    if (!IsWornNow(actor, rc.piece)) return;          // it came off: the done=1 was true
    logger::info("[UNDRESS] RIP CHECK: '{}' is STILL WORN on 0x{:08X} 2.5 s after the pull - sending the corrective End",
                 rc.piece->GetName()[0] ? rc.piece->GetName() : "unnamed piece", rc.actorFid);
    SendUndressEndFor(rc.actorFid, rc.piece, rc.part, false, "ripfailed", "it is still worn");
}

void TickPendingRip(double now)
{
    PendingRip& pr = g_pendingRip;
    if (!pr.active || now < pr.at) return;
    pr.active = false;
    auto* form  = RE::TESForm::LookupByID(pr.actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !pr.piece) {
        logger::info("[UNDRESS] deferred rip dropped - the actor or the piece went away");
        SendUndressEndFor(pr.actorFid, pr.piece, pr.part, false, "ripfailed",
                          "the actor or the piece went away");   // R4
        return;
    }
    if (!IsWornNow(actor, pr.piece)) {
        // Not a broken promise: the piece is off, which is what done=1 said. No corrective End.
        logger::info("[UNDRESS] deferred rip dropped - '{}' is no longer worn",
                     pr.piece->GetName()[0] ? pr.piece->GetName() : "unnamed piece");
        return;
    }
    const char* refused = nullptr;
    if (!RipPiece(actor, pr.piece, pr.left, true, pr.why, &refused)) {
        SendUndressEndFor(pr.actorFid, pr.piece, pr.part, false, "ripfailed",
                          refused ? refused : "the removal was refused");   // R4: the gate re-ask said no
        return;
    }
    RipCheck& rc = g_ripCheck;                      // R4: did it actually come off?
    rc.active   = true;
    rc.at       = now + 2.5;
    rc.actorFid = pr.actorFid;
    rc.piece    = pr.piece;
    std::snprintf(rc.part, sizeof rc.part, "%s", pr.part);
}

// Take the piece off and tell VRTE. Shared by the two ways the gesture can
// complete: the HIGGS snap-back (the real one, see undressSnapMinU) and the
// raw-distance fallback.
void FireUndress(double now, const char* why)
{
    auto* form  = RE::TESForm::LookupByID(g_pair.actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !g_pair.piece) {
        logger::info("[UNDRESS] cannot complete ({}) - the actor or the piece is gone", why);
        SendUndressEnd(false, "gone");
        g_pair = UndressPair{};
        return;
    }
    if (!IsWornNow(actor, g_pair.piece)) {
        logger::info("[UNDRESS] '{}' is no longer worn - sequence dropped",
                     g_pair.piece->GetName());
        SendUndressEnd(false, "gone");
        g_pair = UndressPair{};
        return;
    }
    // ★ v29 (DD SN reply #3 M1, 2026-09-10; user "sure"): ASK THE REMOVAL GATE BEFORE ANNOUNCING. The rip is queued a frame
    // out and RipPiece asks the gate there, but this function announced UndressEnd(done=1) first - the 18:32:20 refused pull
    // narrated "pulled The Gag off Carmella" while she stayed gagged. The plug path already asks first (TickPlugPull). Same
    // vtable call, main thread, no VM: safe in this callback. RipPiece still re-asks when the deferred rip lands.
    {
        bool gaFire = false;
        if (const char* refused = RemovalGateRefusal(actor, g_pair.piece->GetFormID(), false, gaFire)) {
            g_undressCd[g_pair.piece->GetFormID()] = now + 2.0;   // the same 2 s cooldown a completed pull gets
            if (g_cfg.undressNotify != 0.f) Notify(refused);
            logger::info("[UNDRESS] pull refused by the DD/ZaZ removal gate before announcing ({}): {}",
                         why ? why : "?", refused);
            SendUndressEnd(false, "gate", refused);   // nothing came off - VRTE un-mutes (SendUndressEnd reads g_pair: reset AFTER)
            g_pair = UndressPair{};
            return;
        }
    }
    const bool left = (g_pair.puller == 1);   // the SECOND hand takes it
    g_undressCd[g_pair.piece->GetFormID()] = now + 2.0;

    // QUEUED, not done here - see the banner over PendingRip. 0.05s is one
    // frame at any sane rate, long enough for HIGGS to finish letting go.
    PendingRip& pr = g_pendingRip;
    pr.active   = true;
    pr.at       = now + 0.05;
    pr.actorFid = g_pair.actorFid;
    pr.piece    = g_pair.piece;
    pr.left     = left;
    std::snprintf(pr.why, sizeof pr.why, "%s", why ? why : "pull");
    std::snprintf(pr.part, sizeof pr.part, "%s", g_pair.part);

    SendUndressEnd(true);                     // VRTE narrates THIS one, now
    g_pair = UndressPair{};
}

// ═══════════════════════════════════════════════════════════════════════════
// ★★ 2026-09-13 THE EARLY GRIP SIGNAL (VRTE GearGestures R5; user: wait for PPB's early signal rather than delay
// every clothed grab in VRTE). UndressArm needs BOTH hands, so the FIRST hand's grab reached VRTE as a grope
// 0.32-0.87 s before PPB said anything (n=17). Now, per hand, the moment a HIGGS grab on an NPC lands on a worn
// piece that PieceUnderHand resolves:
//   PPB_GestureUndressGrip     "<hand R|L>|<name>|<slotMask>|<isDD>|<class>|<capsule>"   numArg = hand 0 R / 1 L
// and EXACTLY ONE matching end per grip:
//   PPB_GestureUndressGripEnd  "<hand R|L>|<name>|<slotMask>|<isDD>|<class>|<capsule>|<armed>|<reason>"
//     armed   1 = this grip became part of an armed two-hand pull (UndressArm fired while it was held; UndressEnd
//             is then the gesture's own outcome), 0 = it never did
//     reason  letgo (the hand let go / grabbed someone else) · moved (the hand is no longer on that piece for 0.6 s,
//             never while armed) · gone (the piece is no longer worn) · paused · disabled
// Timing: computed from the same digest snapshot the touch API publishes, in the SAME frame, right after that frame's
// PPB_TouchStart for the GRAB contact — never later than the contact itself. sender = the NPC.
// ⛔ No GripEnd across a save load (the layer resets silently at kPreLoadGame).
// ═══════════════════════════════════════════════════════════════════════════
struct GripSignal {
    std::uint32_t      actorFid = 0;       // 0 = no Grip announced on this hand
    RE::TESObjectARMO* piece    = nullptr;
    char               part[48] = {};
    double             lostAt   = 0.0;     // first frame the hand was no longer on the piece (0 = on it)
    bool               armed    = false;
};
GripSignal g_gripSig[2];

static void SendGripEdge(int h, const GripSignal& gs, bool start, const char* reason)
{
    auto* form  = RE::TESForm::LookupByID(gs.actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    char nm[96], cl[64], pt[48];
    FieldCopy(nm, sizeof nm, gs.piece ? gs.piece->GetName() : "");
    char cls[64] = {};
    if (gs.piece) { bool l = false, q = false; DdDescribe(gs.piece, cls, sizeof cls, l, q); }
    FieldCopy(cl, sizeof cl, cls);
    FieldCopy(pt, sizeof pt, gs.part);
    const std::uint32_t slotMask = gs.piece ? static_cast<std::uint32_t>(gs.piece->GetSlotMask().underlying()) : 0u;
    const int isDD = (gs.piece && IsDdWorn(gs.piece)) ? 1 : 0;
    char buf[384];
    if (start)
        std::snprintf(buf, sizeof buf, "%s|%s|%u|%d|%s|%s", h ? "L" : "R", nm, slotMask, isDD, cl, pt);
    else
        std::snprintf(buf, sizeof buf, "%s|%s|%u|%d|%s|%s|%d|%s", h ? "L" : "R", nm, slotMask, isDD, cl, pt,
                      gs.armed ? 1 : 0, reason ? reason : "");
    SendGestureEvent(start ? "PPB_GestureUndressGrip" : "PPB_GestureUndressGripEnd", buf,
                     static_cast<float>(h ? 1 : 0), actor);
    logger::info("[UNDRESS->VRTE] {} \"{}\" on 0x{:08X}", start ? "Grip" : "GripEnd", buf, gs.actorFid);
}

// End every announced grip (pause / disable paths). Safe to call when none are live.
static void EndAllGripSignals(const char* reason)
{
    for (int h = 0; h < 2; ++h) {
        if (g_gripSig[h].actorFid) SendGripEdge(h, g_gripSig[h], false, reason);
        g_gripSig[h] = GripSignal{};
    }
}

// R2: an armed pull torn down by a pause or a disable gets its End (done=0) before the state is dropped.
static void AbortUndressFor(const char* reason)
{
    if (g_pair.active) SendUndressEnd(false, reason);
    g_pair = UndressPair{};
    EndAllGripSignals(reason);
}

// Runs every frame from TickUndress, after g_gripH[] learned which actor each hand grabs.
static void TickGripSignals(double now)
{
    for (int h = 0; h < 2; ++h) {
        GripSignal& gs  = g_gripSig[h];
        RE::Actor*  act = g_gripH[h].actor;
        if (gs.actorFid && (!act || act->GetFormID() != gs.actorFid)) {   // the hand let go (or grabbed someone else)
            SendGripEdge(h, gs, false, "letgo");
            gs = GripSignal{};
        }
        if (!act || act->GetFormID() == 0x14) continue;
        auto* w = PieceUnderHand(h, act, g_snap, g_snapN, nullptr);
        if (!gs.actorFid) {
            if (!w) continue;
            gs.actorFid = act->GetFormID();
            gs.piece    = w;
            std::snprintf(gs.part, sizeof gs.part, "%s", HandCapsuleName(h, act));
            gs.armed    = (g_pair.active && g_pair.actorFid == gs.actorFid);
            SendGripEdge(h, gs, true, "");
            continue;
        }
        if (gs.piece && !IsWornNow(act, gs.piece)) {                       // it came off (or was taken off)
            SendGripEdge(h, gs, false, "gone");
            gs = GripSignal{};
            continue;
        }
        if (w == gs.piece) { gs.lostAt = 0.0; continue; }
        // While the pull is armed the hands MUST slide off the gear — that is not the grip ending.
        if (gs.armed && g_pair.active && g_pair.actorFid == gs.actorFid) { gs.lostAt = 0.0; continue; }
        if (w) {                                                           // now on a DIFFERENT worn piece
            SendGripEdge(h, gs, false, "moved");
            const bool wasArmed = gs.armed;
            gs          = GripSignal{};
            gs.actorFid = act->GetFormID();
            gs.piece    = w;
            std::snprintf(gs.part, sizeof gs.part, "%s", HandCapsuleName(h, act));
            gs.armed    = wasArmed && g_pair.active && g_pair.actorFid == gs.actorFid;
            SendGripEdge(h, gs, true, "");
            continue;
        }
        if (gs.lostAt == 0.0) gs.lostAt = now;                             // same 0.6 s hold the undress uses
        else if (now - gs.lostAt > 0.6) { SendGripEdge(h, gs, false, "moved"); gs = GripSignal{}; }
    }
}

void TickUndress(double now)
{
    if (g_cfg.undressEnabled == 0.f) { AbortUndressFor("disabled"); return; }   // R2: End + GripEnd, then drop

    // ── grip state: which actor each hand is HIGGS-grabbing ─────────────────
    for (int h = 0; h < 2; ++h) {
        auto* obj = g_higgs->GetGrabbedObject(h == 1);
        auto* act = obj ? obj->As<RE::Actor>() : nullptr;
        if (act != g_gripH[h].actor) {
            g_gripH[h] = HandGrip{};
            g_gripH[h].actor = act;
            g_gripH[h].since = now;
        }
    }
    TickGripSignals(now);   // R5: one hand is enough for the early signal
    RE::Actor* actor = g_gripH[0].actor;
    if (!actor || g_gripH[1].actor != actor) {
        // ⛔⛔ THIS BLOCK USED TO CANCEL UNCONDITIONALLY, AND THAT WAS THE BUG.
        // The gesture ENDS by losing a grip: HIGGS holds the virtual hand on
        // her body, the controller walks away, and past HIGGS's stretch limit
        // the hold breaks and the hand snaps back to the controller - with the
        // grab button still down. Reading that as "a hand let go" threw the
        // completed gesture away, every time, and the log said so in the very
        // line that hid it:  "sequence ended - a hand let go of the grab".
        //
        // A deliberate button release looks identical from GetGrabbedObject().
        // What separates them is TRAVEL: a stretch break is the end of a long
        // controller pull; letting go is not.
        if (g_pair.active) {
            const int  puller = g_pair.puller;
            const int  other  = 1 - puller;
            const bool pullerGone = (g_gripH[puller].actor == nullptr);
            const bool otherHeld  = (g_gripH[other].actor != nullptr &&
                                     g_gripH[other].actor->GetFormID() == g_pair.actorFid);
            float travel = 0.f;
            RE::NiPoint3 pw{};
            if (PlayerHandPos(puller == 1, pw)) {
                const float tx = pw.x - g_pair.pullStart.x;
                const float ty = pw.y - g_pair.pullStart.y;
                const float tz = pw.z - g_pair.pullStart.z;
                travel = std::sqrt(tx*tx + ty*ty + tz*tz);
            }
            if (pullerGone && otherHeld &&
                (g_pair.pulledFar || travel >= g_cfg.undressSnapMinU)) {
                logger::info("[UNDRESS] SNAP-BACK on the {} hand after {:.1f}u of pull "
                             "(need >= {:.1f}u) - taking '{}' off",
                             puller ? "left" : "right", travel, g_cfg.undressSnapMinU,
                             g_pair.piece && g_pair.piece->GetName()[0]
                                 ? g_pair.piece->GetName() : "unnamed piece");
                FireUndress(now, "snap");
                return;
            }
            // NEVER FAIL SILENTLY: say which hand went and how far it had come,
            // so a near-miss is one log line away from the right knob value.
            logger::info("[UNDRESS] sequence ended - {} ({} hand travelled {:.1f}u, "
                         "snap needs >= {:.1f}u)",
                         !pullerGone   ? "the holding hand let go"
                         : !otherHeld  ? "both hands let go at once"
                                       : "the pulling hand let go too early",
                         puller ? "left" : "right", travel, g_cfg.undressSnapMinU);
            SendUndressEnd(false, "letgo");  // un-suppress: nothing came off
        }
        g_pair = UndressPair{};
        return;
    }

    RE::NiPoint3 pR{}, pL{};
    if (!PlayerHandPos(false, pR) || !PlayerHandPos(true, pL)) return;
    const float dx = pR.x - pL.x, dy = pR.y - pL.y, dz = pR.z - pL.z;
    const float spread = std::sqrt(dx*dx + dy*dy + dz*dz);

    // ═══════════════════════════════════════════════════════════════════════
    // ARMED - the pull is running.
    //
    // ★ THE GRAB IS THE LIFELINE (user, 2026-08-22): "so long as the grab
    // button is held, the sequence is still a go." Once the sequence starts,
    // NOTHING about the contact stream can end it - not a capsule flicker, not
    // the holding hand going dark, not the hands sliding off the mesh as they
    // separate (which they must, since the gear stays on her body while the
    // hands travel). The only ways out are: a hand releases the grab (handled
    // above), the actor changes, or the piece stops being worn.
    //
    // This replaced a flicker-tolerance scheme of grace windows and a
    // remembered baseline, all of which existed only because the pair used to
    // die whenever PPB blinked. It cannot die that way any more, so they are
    // gone.
    // ═══════════════════════════════════════════════════════════════════════
    if (g_pair.active) {
        if (g_pair.actorFid != actor->GetFormID() || !g_pair.piece) {
            SendUndressEnd(false, g_pair.piece ? "actor" : "gone");
            g_pair = UndressPair{};
            return;
        }
        const float pulled = spread - g_pair.d0;
        {
            static double s_lastProg = 0.0;
            if (now - s_lastProg > 0.5) {
                s_lastProg = now;
                // Both sources, for one calibration pass. `wand` is the live
                // one; `bone` is the old animated-arm reading that froze under
                // a HIGGS grab. If bone stops moving while wand climbs, this
                // line is the proof — and the wand numbers are what the knobs
                // must be re-dialled against.
                RE::NiPoint3 bR{}, bL{};
                float boneSpread = -1.f;
                if (PlayerHandPosBone(false, bR) && PlayerHandPosBone(true, bL)) {
                    const float bx = bR.x - bL.x, by = bR.y - bL.y, bz = bR.z - bL.z;
                    boneSpread = std::sqrt(bx*bx + by*by + bz*bz);
                }
                logger::info("[UNDRESS] pulling '{}': {:.1f}/{:.1f}u  (wand spread {:.1f}u, "
                             "started {:.1f}u | old bone source {:.1f}u)",
                             g_pair.piece->GetName()[0] ? g_pair.piece->GetName()
                                                        : "unnamed piece",
                             pulled, g_cfg.undressPullU, spread, g_pair.d0, boneSpread);
            }
        }
        // ★★ THE DISTANCE ARMS. IT DOES NOT FIRE. (user, 2026-08-24)
        //
        //   "i noticed some gear falling to the floor before getting into my
        //    hand on Un-equip... The un-equip trigger is not the distance, it's
        //    the 'hand fly back to controller' moment. If my hand is still
        //    attached to the body and the controller is at 30u distance, wait
        //    for the release and make the equipment show up in the hand still
        //    holding."
        //
        // ⛔ Firing at undressPullU ripped the piece while HIGGS was STILL
        // holding her, so the hand meant to receive it was not free - and
        // ReleaseToHand put the item on the floor instead. Eight of nine
        // removals in the 14:xx session took this path, which is why "it works,
        // but it falls" was the shape of the complaint.
        //
        // The snap-back is not merely the nicer trigger: it is the moment the
        // hand BECOMES FREE, and therefore the only moment it can be handed
        // anything. Passing the distance just means the pull is genuine.
        if (pulled < g_cfg.undressPullU) return;
        if (!g_pair.pulledFar) {
            g_pair.pulledFar = true;
            logger::info("[UNDRESS] pulled past {:.0f}u ({:.1f}u) - ARMED; waiting for the hand to "
                         "snap back to the controller, which is when it can take the piece",
                         g_cfg.undressPullU, pulled);
        }
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // NOT ARMED - can the sequence start?
    //   1. a hand is ON the gear (its mesh, or right beside it)
    //   2. the other hand is grabbing too, within undressGrabMaxU of it
    //   3. and on the same gear or the same body part
    // ═══════════════════════════════════════════════════════════════════════
    const PPBAPI::PpbTouchContact* buf = g_snap;
    const int n = g_snapN;
    for (int h = 0; h < 2; ++h) {
        BodyRegion reg = kRegNone;
        auto* w = PieceUnderHand(h, actor, buf, n, &reg);
        if (reg != kRegNone) g_gripH[h].region = reg;   // where the hand is, gear or not
        if (w) {
            g_gripH[h].piece   = w;
            g_gripH[h].pieceAt = now;
        } else if (now - g_gripH[h].pieceAt > 0.6) {
            g_gripH[h].piece = nullptr;
        }
    }

    // The SECOND hand to grab is the one that pulls, and the one the gear ends
    // up in. Either hand may be the one resting on the gear.
    const int pullerHand = (g_gripH[1].since > g_gripH[0].since) ? 1 : 0;
    const int otherHand  = 1 - pullerHand;
    RE::TESObjectARMO* piece = nullptr;
    bool ok = false;
    const char* pairWhy = "";
    if (g_gripH[0].piece && g_gripH[0].piece == g_gripH[1].piece) {
        piece = g_gripH[0].piece;  ok = true;  pairWhy = "both hands on it";
    } else if (g_gripH[pullerHand].piece) {
        piece = g_gripH[pullerHand].piece;
        ok    = RegionsCompatible(g_gripH[pullerHand].region, g_gripH[otherHand].region);
        pairWhy = "pulling hand on it";
    } else if (g_gripH[otherHand].piece) {
        piece = g_gripH[otherHand].piece;
        ok    = RegionsCompatible(g_gripH[otherHand].region, g_gripH[pullerHand].region);
        pairWhy = "holding hand on it";
    }

    if (!ok || !piece) {
        static double s_lastWhy = 0.0;
        if (now - s_lastWhy > 2.0) {
            s_lastWhy = now;
            const bool anyPiece = (g_gripH[0].piece || g_gripH[1].piece);
            logger::info("[UNDRESS] both hands hold {} | R: '{}' @{} | L: '{}' @{} - {}",
                         actor->GetDisplayFullName(),
                         g_gripH[0].piece ? g_gripH[0].piece->GetName() : "-",
                         RegionName(g_gripH[0].region),
                         g_gripH[1].piece ? g_gripH[1].piece->GetName() : "-",
                         RegionName(g_gripH[1].region),
                         anyPiece ? "the other hand is not on that gear or its body part"
                                  : "no worn gear under either hand");
        }
        return;
    }

    const std::uint32_t pieceFid = piece->GetFormID();
    if (auto it = g_undressCd.find(pieceFid); it != g_undressCd.end() && now < it->second) return;

    if (spread > g_cfg.undressGrabMaxU) {
        static double s_lastFar = 0.0;
        if (now - s_lastFar > 2.0) {
            s_lastFar = now;
            logger::info("[UNDRESS] '{}' not started - hands are {:.1f}u apart, need <= {:.1f}u. "
                         "Take hold of it with both hands, then pull.",
                         piece->GetName()[0] ? piece->GetName() : "unnamed piece",
                         spread, g_cfg.undressGrabMaxU);
        }
        return;
    }

    g_pair.active   = true;
    g_pair.piece    = piece;
    g_pair.actorFid = actor->GetFormID();
    g_pair.puller   = pullerHand;
    g_pair.d0       = spread;
    // ⛔⛔ MEASURED 2026-08-24, 16:56:03.096 - a sequence that had armed 30 ms
    // earlier reported "SNAP-BACK ... after 14234.1u of pull". Two hundred
    // metres. The boot came off instantly and the player never pulled anything.
    //
    // My own fallback did it: on a failed read this used to set pullStart to
    // RE::NiPoint3{} - the WORLD ORIGIN - so `travel` became the magnitude of
    // the player's world coordinates, which is thousands of units anywhere but
    // Helgen. The snap gate was then trivially true for the rest of the pair.
    //
    // ★ A FALLBACK MUST FAIL CLOSED. Origin is not "no value", it is a real
    //   point very far away, and every comparison against it succeeds.
    // If we cannot read where the pull began we do not arm: no baseline, no
    // gesture. Refusing to start is recoverable; a phantom 14 km pull is not.
    if (!PlayerHandPos(pullerHand == 1, g_pair.pullStart)) {
        logger::info("[UNDRESS] '{}' not started - could not read the {} controller position, so "
                     "there is no baseline to measure the pull from",
                     piece->GetName()[0] ? piece->GetName() : "unnamed piece",
                     pullerHand ? "left" : "right");
        g_pair = UndressPair{};
        return;
    }
    // Capture WHERE the gesture armed. Prefer the pulling hand; fall back to the
    // other, because PPB goes quiet for whichever hand HIGGS treats as holding
    // her (report 21 addendum 3) and that hand can be either one.
    {
        const char* cap = HandCapsuleName(pullerHand, actor);
        if (!cap[0]) cap = HandCapsuleName(1 - pullerHand, actor);
        std::snprintf(g_pair.part, sizeof g_pair.part, "%s", cap);
    }
    // Tell VRTE to stop narrating grabs on her NOW — not when the piece comes
    // off. A bare-breast grab has a ZERO-second dwell, so it would already have
    // been spoken by the time the pull completes.
    SendGestureEvent("PPB_GestureUndressArm", g_pair.part, 0.f, actor);
    for (int gh = 0; gh < 2; ++gh)                         // R5: these grips are now part of an armed pull
        if (g_gripSig[gh].actorFid == g_pair.actorFid) g_gripSig[gh].armed = true;
    logger::info("[UNDRESS] SEQUENCE STARTED on '{}' ({}, {}) - hands {:.1f}u apart; keep both "
                 "grabs held and pull the {} hand +{:.0f}u",
                 piece->GetName()[0] ? piece->GetName() : "unnamed piece",
                 IsDdWorn(piece) ? "DD device" : "plain", pairWhy, spread,
                 pullerHand ? "left" : "right", g_cfg.undressPullU);
}

// Put a worn piece back into the world at a hand, ownership cleared if she is
// a teammate. Shared by the undress, the plug extraction and the equip revert.

// Did the equip actually land? If not, say WHY - naming the piece already in
// that slot when there is one - and return the item to the world so a refused
// equip never silently eats it.
void TickVerify(int h, double now)
{
    PendingVerify& pv = g_verify[h];
    if (!pv.active || now < pv.at) return;
    pv.active = false;

    auto* form  = RE::TESForm::LookupByID(pv.actorFid);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor || !pv.base) return;

    // ★ THE WORN HALF, NOT THE HELD ONE (2026-08-23, report 23 §30).
    // What we hold is the INVENTORY half; what she wears is the RENDERED one,
    // and every fact worth reporting lives on the latter. Two bugs died here:
    //
    //  1. `DdDescribe(pv.base, ...)` read the held half, which carries no class
    //     keyword — so cls was ALWAYS empty, locked ALWAYS false, slotMask
    //     ALWAYS 0, and the whole 19-class device dictionary on the VRTE side
    //     was unreachable. Every device narrated as a generic "restraint".
    //  2. `IsWornNow(actor, pv.base)` early-returns true when the base has no
    //     biped slots — and a DD inventory half has none. So the equip-success
    //     check was VACUOUSLY TRUE for every DD device, which is exactly the
    //     case it was written to catch (§22, the Ball Strap Gag that reported
    //     "equipped" and fell on the floor).
    //
    // Testing the rendered half against her worn slots fixes both, and is
    // exact rather than approximate: if she already wore a gag and DD refused
    // this one, THIS device's rendered half is not on her and we say so.
    const DevClass* dc = nullptr;
    if (auto it = g_classCache.find(pv.base->GetFormID()); it != g_classCache.end())
        dc = &it->second;

    RE::TESObjectARMO* rendered = nullptr;
    if (dc && dc->dd && dc->renderedFid) {
        auto* rf = RE::TESForm::LookupByID(dc->renderedFid);
        rendered = rf ? rf->As<RE::TESObjectARMO>() : nullptr;
    }

    if (rendered ? IsWornNow(actor, rendered) : IsWornNow(actor, pv.base)) {
        logger::info("[EQUIP] confirmed worn: '{}' on 0x{:08X}{}{}", pv.base->GetName(),
                     pv.actorFid, rendered ? " (rendered half checked)" : "",
                     pv.tries ? " (on the second look)" : "");
        {
            auto* aform  = RE::TESForm::LookupByID(pv.actorFid);
            auto* wearer = aform ? aform->As<RE::Actor>() : nullptr;

            // locked / quest: read BOTH halves and OR them. zad_Lockable sits on
            // the rendered half of every pair inspected, but nothing guarantees
            // a content mod puts zad_QuestItem in the same place, and a false
            // "not locked" is a worse answer than a redundant read.
            char c1[64] = {}; bool l1 = false, q1 = false;
            DdDescribe(pv.base, c1, sizeof c1, l1, q1);
            char c2[64] = {}; bool l2 = false, q2 = false;
            if (rendered) DdDescribe(rendered, c2, sizeof c2, l2, q2);
            const bool locked = l1 || l2;
            const bool quest  = q1 || q2;

            // CLASS, in order of authority: DD's own zad_DeviousDevice property,
            // then the rendered half's keywords, then the held half's (which for
            // a DD device carries none — this arm only fires for ZaZ/DoM gear).
            char cls[64] = {};
            const char* pick = (dc && dc->cls[0]) ? dc->cls : (c2[0] ? c2 : c1);
            std::snprintf(cls, sizeof cls, "%s", pick);

            auto* armo = rendered ? rendered : pv.base->As<RE::TESObjectARMO>();
            const std::uint32_t slotMask =
                armo ? static_cast<std::uint32_t>(armo->GetSlotMask().underlying()) : 0u;

            const char* nm = pv.base->GetName() ? pv.base->GetName() : "";
            char buf[256];

            // ★ FORCE GRADE (ask SS3): 0 gentle / 1 firm / 2 forced, from the gesture's peak
            // press depth. APPENDED LAST - both payloads are read positionally and
            // length-checked, so old readers ignore it. Never insert mid-payload.
            const int force = pv.forceU >= g_cfg.equipForce2U ? 2
                            : (pv.forceU >= g_cfg.equipForce1U ? 1 : 0);
            if (cls[0]) {
                // A DD/ZaZ device: the full description, wearer + onlookers.
                // strArg = "<name>|<classSuffix>|<locked>|<quest>|<siteMask>|<slotMask>|<force>"
                std::snprintf(buf, sizeof buf, "%s|%s|%d|%d|%d|%u|%d",
                              nm, cls, locked ? 1 : 0, quest ? 1 : 0,
                              dc ? dc->mask : DdMask(cls), slotMask, force);
                SendGestureEvent("PPB_GestureDeviceEquipped", buf, 0.f, wearer);
                logger::info("[EQUIP] force grade {} (peak press {:.2f}u; firm>={:.1f} forced>={:.1f})",
                             force, pv.forceU, g_cfg.equipForce1U, g_cfg.equipForce2U);

                logger::info("[DEVICE->VRTE] equipped '{}' cls='{}' locked={} slot={}",
                             nm, cls, locked ? 1 : 0, slotMask);
            } else if (wearer && wearer->GetFormID() != 0x14) {
                // ★ ORDINARY GEAR (user, 2026-08-23): "A normal persistentEvent
                // to the NPC we give it to." One event, one recipient — no
                // onlooker line, because someone else being handed a tunic is
                // not news. Neutral statement of fact only; how she takes it is
                // hers to decide (report 19 §1).
                // ORDINARY means ordinary: a DD inventory half whose class lookup failed reaches this arm
                // with an empty cls (report 31 O4), and zad/zbf/DOM restraints carry no "Devious" class
                // keyword at all. Neither is outfit material, and neither should pool or hand off.
                const bool ordinary = wearer && !(dc && dc->dd) && !IsDdInventoryDevice(pv.base) &&
                                      !(armo && IsFrameworkDevice(armo));
                // strArg = "<name>|<slotMask>|<force>|<ordinary>"
                // ★ 2026-09-13 (VRTE GearGestures R7): <ordinary> APPENDED - 1 = plain clothing/armour, 0 = a
                // ZaZ / Diary of Mine / other framework restraint that has no Devious class keyword.
                char nmF[96];
                FieldCopy(nmF, sizeof nmF, nm);
                std::snprintf(buf, sizeof buf, "%s|%u|%d|%d", nmF, slotMask, force, ordinary ? 1 : 0);
                SendGestureEvent("PPB_GestureGearEquipped", buf, 0.f, wearer);
                logger::info("[GEAR->VRTE] equipped '{}' slot={} ordinary={} on 0x{:08X}",
                             nm, slotMask, ordinary ? 1 : 0, pv.actorFid);
                // ★ OPTION A + HOLD POOL (2026-09-06, user: "make it so their regular armor is
                // the one we just equipped on them"). Ordinary gear only — DD devices persist
                // themselves. Nothing here writes a base record.
                if (ordinary) {
                    if (g_cfg.equipOutfitTag != 0.f) {
                        OutfitTag::Set(wearer, pv.base, true);
                        for (int k = 0; k < pv.nDisplaced; ++k) {
                            auto* df = RE::TESForm::LookupByID(pv.displaced[k]);
                            auto* db = df ? df->As<RE::TESBoundObject>() : nullptr;
                            if (!db) continue;
                            OutfitTag::Set(wearer, db, false);
                            if (g_cfg.equipRemoveDisplaced != 0.f) OutfitTag::RemoveDisplaced(wearer, db);
                        }
                    }
                    if (g_cfg.equipHoldPool != 0.f) HoldPool::Hold(wearer, "gesture equip");
                    DispatchOutfitRecord(pv.actorFid, 1, pv.base->GetFormID());   // SA/NFF-controlled? then her outfit gains the piece
                }
            }

            // ★ THE INSERTION HALF OF THE PLUG GESTURE — OUTSIDE BOTH ARMS, DELIBERATELY.
            //
            // It used to live inside the `if (cls[0])` DD arm, and that made it invisible for
            // every plug that is not a Devious Devices device: a ZaZ, Diary of Mine or zRavenous
            // plug carries no "...Devious<Class>" keyword, so cls is empty, control takes the
            // plain-gear arm, and no "in" edge ever existed. PPB's gesture layer supports those
            // plugs on purpose — GenericMask maps slot 57 -> vaginal and 48 -> anal precisely so
            // zRavenous's Carrot Anal Plug works — so the event has to see them too.
            //
            // TWO WAYS TO BE A PLUG, and the second is what the DD-only arm was missing:
            //   * a DD class name that IS a plug class (Plug / PlugVaginal / PlugAnal), or
            //   * no DD class at all, but a site mask that lands on an orifice.
            // Both read the SAME site source as the "out" edge's WornMask: dc->mask is built from
            // DdMask for DD keywords, PlainMask for ZaZ/DoM keywords, and GenericMask for bare
            // slots. A gag (kSiteMouth) and a vaginal piercing are still excluded — a piercing is
            // not an insertion, and a gag has its own site on DeviceEquipped.
            {
                const int siteMask = dc ? dc->mask : (cls[0] ? DdMask(cls) : 0);
                const bool isPlug  = IsPlugClass(cls) ||
                                     (!cls[0] && (siteMask & (kSiteVaginal | kSiteAnal)) != 0);
                if (isPlug) {
                    char pbuf[192];
                    std::snprintf(pbuf, sizeof pbuf, "in|%s|%s|%d|%d",
                                  nm, cls, siteMask, pv.left ? 1 : 0);
                    SendGestureEvent("PPB_GesturePlug", pbuf, 0.f, wearer);
                }
            }
        }
        if (g_cfg.equipNotify != 0.f) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s equipped", pv.base->GetName());
            Notify(buf);
        }
        return;
    }

    // ── not worn YET. Look once more before undoing anything. ───────────────
    if (pv.tries == 0) {
        pv.active = true;          // RE-ARM rather than mutate a cleared record
        pv.tries  = 1;
        pv.at     = now + 1.3;     // ~2.5s total; DD's chain has always beaten this
        logger::info("[EQUIP] '{}' not worn at the first check - looking again in 1.3s before "
                     "calling it refused", pv.base->GetName());
        return;
    }

    // Refused. Find what is occupying a slot this item needs - almost always a
    // same-class device DD will not double up.
    const char* blocker = nullptr;
    // ⚠ Use the RENDERED half's slots. A DD INVENTORY half has none at all
    // (report 23 §30), which is why every DD refusal has said "no blocking
    // piece found" - it was searching zero slots, not finding nothing.
    if (auto* armo = rendered ? rendered : pv.base->As<RE::TESObjectARMO>()) {
        const std::uint32_t slots = static_cast<std::uint32_t>(armo->GetSlotMask().underlying());
        for (int b = 0; b < 32 && !blocker; ++b) {
            if (!(slots & (1u << b))) continue;
            auto* w = actor->GetWornArmor(
                static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << b));
            if (w && w != armo && w->GetName() && w->GetName()[0]) blocker = w->GetName();
        }
    }
    logger::info("[EQUIP] REFUSED: '{}' did not go on 0x{:08X}{}{} - returning it to the world",
                 pv.base->GetName(), pv.actorFid,
                 blocker ? " - blocked by " : " (no blocking piece found)",
                 blocker ? blocker : "");
    if (g_cfg.equipNotify != 0.f) {
        char buf[190];
        if (blocker)
            std::snprintf(buf, sizeof buf, "She is already wearing %s.", blocker);
        else
            std::snprintf(buf, sizeof buf, "%s will not go on.", pv.base->GetName());
        Notify(buf);
    }
    SendEquipRefused(pv.actorFid, pv.base, pv.left ? 1 : 0, "refused", blocker ? blocker : "", 0);   // R1
    ReleaseToHand(actor, pv.base, pv.left, false);   // back on the ground, not stolen
}

// -- the mod-event sink - the helper's answer for a DD unlock -----------------
// Fires on the VM thread; queue only, the frame hook does the engine work.
class UnlockSink : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev,
                                          RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
    {
        if (!ev || !ev->eventName.c_str()) return RE::BSEventNotifyControl::kContinue;

        // ── SCENE PAUSE, FORWARDED (2026-08-27) ──────────────────────────────────────────
        // The gesture layer moved into PPB, but the add-on still publishes the Papyrus native
        // VRTE_DDZaZ_Native.SetScenePaused that scene managers already call. It forwards here.
        // New callers should use PPB_Native.SetGesturePaused directly.
        if (_stricmp(ev->eventName.c_str(), "PPB_GestureSetPaused") == 0) {
            DeviceGesture::SetPaused(ev->numArg != 0.f);
            return RE::BSEventNotifyControl::kContinue;
        }

        // ── THE UNLOCK ANSWER ────────────────────────────────────────────────────────────
        // PPB_DeviceEquip.DoUnequipDevice sends this when a DD unlock succeeded, carrying the
        // INVENTORY half's FormID. Queue only — the drop happens on the frame tick, this
        // file's convention, because we are on the VM's thread here.
        if (_stricmp(ev->eventName.c_str(), "PPB_GestureUnlocked") != 0)
            return RE::BSEventNotifyControl::kContinue;
        const long long fid = ev->strArg.empty() ? 0 : std::atoll(ev->strArg.c_str());
        auto* sender = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
        if (fid && sender) {
            g_unlockDrop.actorFid = sender->GetFormID();
            g_unlockDrop.baseFid  = static_cast<std::uint32_t>(fid);
            g_unlockDrop.left     = (ev->numArg != 0.f);
            g_unlockDrop.active.store(true, std::memory_order_release);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
UnlockSink g_unlockSink;

// Deliver the device DD just unlocked into the hand that pulled it. Runs on the frame tick,
// one frame after the VM answered, because the sink above only parks the intent.
void TickUnlockDrop()
{
    if (!g_unlockDrop.active.load(std::memory_order_acquire)) return;
    g_unlockDrop.active.store(false, std::memory_order_relaxed);
    auto* aform = RE::TESForm::LookupByID(g_unlockDrop.actorFid);
    auto* bform = RE::TESForm::LookupByID(g_unlockDrop.baseFid);
    auto* actor = aform ? aform->As<RE::Actor>() : nullptr;
    auto* base  = bform ? bform->As<RE::TESBoundObject>() : nullptr;
    if (!actor || !base) return;
    // ★ THE HAND COMES FROM THE PARKED INTENT, NOT FROM THE ROUND TRIP
    // (2026-08-23). `TickPlugPull` already knows the wand PPB reported the
    // finger on; that is the hand the plug must land in. This used to read
    // `g_unlockDrop.left` — the value that travels out to Papyrus as a Float
    // and back through the mod event's numArg — and an extraction done with the
    // LEFT hand delivered into the RIGHT one. The event value now survives only
    // as the fallback for a stale intent.
    // Same lesson as §21 addendum 5, which parked `intoHand` and left `left`
    // riding the round trip: EVERY parameter the return leg needs must be
    // parked, not just the one that broke last time.
    const bool fresh = (g_ripIntent.actorFid == g_unlockDrop.actorFid &&
                        (NowS() - g_ripIntent.at) < 3.0);
    const bool intoHand = fresh ? g_ripIntent.intoHand : true;
    const bool left     = fresh ? g_ripIntent.left     : g_unlockDrop.left;
    logger::info("[UNDRESS] DD unlock confirmed: '{}' freed from 0x{:08X} - {} {} hand{}",
                 base->GetName(), g_unlockDrop.actorFid,
                 intoHand ? "into the" : "letting it fall by the",
                 left ? "left" : "right",
                 fresh ? "" : " (intent stale - hand taken from the mod event)");
    // Do NOT drop it yet - see the PendingSafeDrop banner. DD deletes a device
    // dropped while it still lists the rendered half.
    g_safeDrop.active   = true;
    g_safeDrop.actorFid = g_unlockDrop.actorFid;
    g_safeDrop.baseFid  = g_unlockDrop.baseFid;
    g_safeDrop.rendFid  = fresh ? g_ripIntent.renderedFid : 0;
    g_safeDrop.left     = left;
    g_safeDrop.intoHand = intoHand;
    g_safeDrop.deadline = NowS() + (g_cfg.ddSettleS > 0.f ? g_cfg.ddSettleS : 0.f);
    g_safeDrop.waits    = 0;
    if (g_cfg.undressNotify != 0.f) {
        char nb[160];
        std::snprintf(nb, sizeof nb, "%s removed", base->GetName());
        Notify(nb);
    }
}

// Release the freed device only once DD has finished taking its rendered half
// off - otherwise DD's OnContainerChanged deletes it out of the player's hand.
// See the PendingSafeDrop banner.
void TickSafeDrop(double now)
{
    if (!g_safeDrop.active) return;
    auto* aform = RE::TESForm::LookupByID(g_safeDrop.actorFid);
    auto* bform = RE::TESForm::LookupByID(g_safeDrop.baseFid);
    auto* actor = aform ? aform->As<RE::Actor>() : nullptr;
    auto* base  = bform ? bform->As<RE::TESBoundObject>() : nullptr;
    if (!actor || !base) {
        logger::info("[UNDRESS] deferred drop abandoned - actor or device no longer resolvable");
        g_safeDrop.active = false;
        return;
    }
    auto* rform = g_safeDrop.rendFid ? RE::TESForm::LookupByID(g_safeDrop.rendFid) : nullptr;
    auto* rend  = rform ? rform->As<RE::TESBoundObject>() : nullptr;
    const int  rc      = rend ? CountOf(actor, rend) : 0;
    const bool expired = now >= g_safeDrop.deadline;
    if (rc != 0 && !expired) { ++g_safeDrop.waits; return; }   // DD still holds it - dropping now = delete

    g_safeDrop.active = false;
    if (rc != 0) {
        logger::info("[UNDRESS] DD still lists the rendered half after {:.2f}s ({} frame(s) waited) - "
                     "dropping anyway. If '{}' vanishes, that is DD's OnContainerChanged delete; "
                     "raise ddSettleS.",
                     g_cfg.ddSettleS, g_safeDrop.waits, base->GetName());
    } else if (g_safeDrop.waits > 0) {
        logger::info("[UNDRESS] DD released the rendered half after {} frame(s) - safe to hand '{}' over",
                     g_safeDrop.waits, base->GetName());
    }
    ReleaseToHand(actor, base, g_safeDrop.left, g_safeDrop.intoHand);
}

// ── config ───────────────────────────────────────────────────────────────────
void LoadCfg()
{
    const char* path = "Data/SKSE/Plugins/PPB_tuning.txt";   // the gesture block; shares PPB's file
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[96]; float val = 0.f;
        // ★ 2026-09-02 FIX: the key class was `%[A-Za-z]` (letters only). A key with a digit —
        // equipForce1U / equipForce2U, the only two in this block — parsed as "equipForce" + the
        // digit as the VALUE, matched no branch, and was silently discarded: both force-grade
        // thresholds were pinned at their compiled defaults forever. Digits are now accepted.
        if (std::sscanf(line, " %95[A-Za-z0-9] %f", key, &val) != 2) continue;
        const auto eq = [&](const char* k) { return _stricmp(key, k) == 0; };
        if      (eq("enabled"))        g_cfg.enabled        = val;
        else if (eq("equipEnabled"))   g_cfg.equipEnabled   = val;
        else if (eq("equipDepthU"))    g_cfg.equipDepthU    = val;
        else if (eq("equipTouchU"))    g_cfg.equipTouchU    = val;
        else if (eq("equipDwellS"))    g_cfg.equipDwellS    = val;
        else if (eq("equipGraceS"))    g_cfg.equipGraceS    = val;
        else if (eq("equipMatchSite")) g_cfg.equipMatchSite = val;
        else if (eq("equipNotify"))    g_cfg.equipNotify    = val;
        else if (eq("equipGeneric"))   g_cfg.equipGeneric   = val;
        else if (eq("plugPullS"))      g_cfg.plugPullS      = val;
        else if (eq("plugToHand"))     g_cfg.plugToHand     = val;
        else if (eq("ddSettleS"))      g_cfg.ddSettleS      = val;
        else if (eq("undressEnabled")) g_cfg.undressEnabled = val;
        else if (eq("undressPullU"))   g_cfg.undressPullU   = val;
        else if (eq("undressGrabMaxU")) g_cfg.undressGrabMaxU = val;
        else if (eq("undressSnapMinU")) g_cfg.undressSnapMinU = val;
        else if (eq("undressNotify")) g_cfg.undressNotify  = val;
        else if (eq("returnFailedGifts")) g_cfg.returnFailedGifts = val;
        else if (eq("equipForce1U"))   g_cfg.equipForce1U   = val;
        else if (eq("equipForce2U"))   g_cfg.equipForce2U   = val;
        else if (eq("equipOutfitTag"))       g_cfg.equipOutfitTag       = val;
        else if (eq("equipRemoveDisplaced")) g_cfg.equipRemoveDisplaced = val;
        else if (eq("equipHoldPool"))        g_cfg.equipHoldPool        = val;
        else if (eq("equipSaHandoff"))       g_cfg.equipSaHandoff       = val;
        else if (eq("equipReleaseReachU"))   g_cfg.equipReleaseReachU   = val;   // v28
        else if (eq("logLevel"))       g_cfg.logLevel       = val;
    }
    fclose(f);
}

}   // namespace (anonymous) -- everything above is internal to this translation unit

// ════════════════════════════════════════════════════════════════════════════════════════════
//  PUBLIC SURFACE
// ════════════════════════════════════════════════════════════════════════════════════════════
namespace DeviceGesture {

void OnFrame()
{
    if (!g_armed.load(std::memory_order_relaxed)) return;

    // The AddOn answers the handshake at any time, so retry cheaply until it does.
    if (!g_ddzGate) {
        static double s_lastTry = 0.0;
        const double nowTry = NowS();
        if (nowTry - s_lastTry > 5.0) { s_lastTry = nowTry; AcquireDdzGate(); }
    }
    static double s_lastCfg = 0.0, s_last = 0.0;
    const double now = NowS();
    if (now - s_lastCfg > 1.0) { s_lastCfg = now; LoadCfg(); }

    float dt = (float)(now - s_last);
    s_last = now;
    if (dt <= 0.f || dt > 0.5f) dt = 1.f / 90.f;      // first frame / hitch guard

    // Pending ejects run even while the gesture side is disabled — they are
    // cleanup for a release that already happened. (SetPaused/Reset clears
    // them: a scene start must not fight scene inventory management.)
    for (int h = 0; h < 2; ++h) { TickEject(h, now); TickVerify(h, now); }
    TickRipCheck(now);        // R4: cleanup for a pull that already completed — runs paused or not

    if (g_paused.load(std::memory_order_relaxed) ||
        g_cfg.enabled == 0.f || g_cfg.equipEnabled == 0.f) {
        for (int h = 0; h < 2; ++h) g_insert[h] = InsertState{};
        // R2 (2026-09-13): an armed pull or an announced grip that this stand-down strands gets its End now.
        if (g_pair.active || g_gripSig[0].actorFid || g_gripSig[1].actorFid)
            AbortUndressFor(g_paused.load(std::memory_order_relaxed) ? "paused" : "disabled");
        return;
    }

    // ONE contact snapshot for the whole frame - every consumer reads g_snap.
    // Inside PPB now: read the same snapshot the API publishes, without the round trip.
    g_snapN = PpbApi::CopyContacts(g_snap, 32);

    // NO SCENE GATE HERE, BY DESIGN (user, 2026-08-23): "if I want to be able
    // to remove clothes or device during a scene, I should be able to do so
    // with my hands." Scene suppression belongs to VRTouchEvents, whose job is
    // narrating touches; a physical gesture is the player's to make whenever
    // they can reach. The Papyrus native VRTE_DDZaZ_Native.SetScenePaused
    // still exists for a scene manager that wants to silence this mod.

    BuildHeld();
    for (int h = 0; h < 2; ++h) TickHand(h, dt, now);
    for (int h = 0; h < 2; ++h) TickPlugPull(h, dt, now);
    TickUndress(now);
    TickPendingRip(now);      // one frame AFTER the gesture, never inside it
    TickUnlockDrop();
    TickSafeDrop(now);
    TickHandGrabs(now);
}


// ═══════════════════════════════════════════════════════════════════════════
void Install()
{
    if (g_armed.load(std::memory_order_relaxed)) return;   // idempotent
    LoadCfg();
    // ★ 2026-09-11 THE FEATURE MASTER (PPB.ini [Features] bEquipGestures), ANDed with the cfg
    // master exactly as the push side is. Returning BEFORE g_armed is set is what makes "off"
    // complete: OnFrame bails on !g_armed, so press-to-equip, two-hand undress, fingertip plug,
    // the clothing gate and every PPB_Gesture* event never arm — not merely "do nothing".
    // ⛔ NOT `equipEnabled` — that one silently disables undress and plug extraction too
    // (defect D3); `enabled` is the clean master.
    if (!Ini::FeatureEquipGestures()) {
        logger::info("[GESTURE] disabled by PPB.ini [Features] bEquipGestures=0 - inert.");
        return;
    }
    if (g_cfg.enabled == 0.f) { logger::info("[GESTURE] disabled by config - inert."); return; }

    // ★ 2026-08-27, THE SPLIT. This module used to be a separate plugin that had to ASK PPB for
    // its touch interface over SKSE messaging. It is inside PPB now, so it calls PpbApi directly
    // (see OnFrame's PpbApi::CopyContacts). The old handshake was not merely redundant - it
    // dispatched kGetTouchInterface with receiver "PPB" from inside PPB and treated a null answer
    // as "PPB not present - inert", so if SKSE does not hand a plugin its own message the whole
    // gesture layer would have armed never, silently.
    //
    // HIGGS is still required and still real: it is what tells us what the player is HOLDING and
    // what takes a freed device back into the hand (IsHoldingObject / GetGrabbedObject /
    // GrabObject / IsHandInGrabbableState). Use PPB's own shared handshake rather than a second
    // private one - Interop::AcquireHiggs is idempotent and already retried at kDataLoaded.
    g_higgs = Interop::GetHiggs();
    if (!g_higgs) {
        logger::info("[GESTURE] HIGGS not acquired yet - inert this pass "
                     "(Install is retried at kDataLoaded).");
        return;
    }

    // ⛔ NO FRAME CALLBACK REGISTERED HERE, AND THAT IS DELIBERATE (2026-08-27).
    // PerfSys::RegisterHiggs already owns PPB's single AddPostVrikPostHiggsCallback and calls
    // DeviceGesture::OnFrame from it, immediately after PpbApi::OnFrame. Registering our own
    // second callback ran OnFrame TWICE per frame: dt is derived from a shared s_last, so the
    // second call fell through to the `dt = 1/90` hitch guard and every dwell advanced at 2x -
    // equipDwellS 1.0 behaving like 0.5. It also left the two callbacks in an undefined order
    // relative to each other, where PerfSys's slot guarantees a snapshot from THIS frame.
    // If a tick is ever needed elsewhere, add it to PerfSys's lambda - never a second registration.

    // The Papyrus helper answers DD unlocks through a mod event (see UnlockSink), and the same
    // sink carries the scene-pause forward from the add-on's legacy native.
    if (auto* src = SKSE::GetModCallbackEventSource()) src->AddEventSink(&g_unlockSink);
    g_armed.store(true, std::memory_order_relaxed);
    logger::info("[GESTURE] armed (contact-driven). depth<={:.2f}u touch<={:.2f}u dwell>={:.2f}s "
                 "matchSite={} | tick owned by PerfSys, contacts read in-process",
                 g_cfg.equipDepthU, g_cfg.equipTouchU, g_cfg.equipDwellS,
                 g_cfg.equipMatchSite != 0.f);
    logger::info("[GESTURE] sites: plugs -> orifices (PPB orifice verdict, distU fallback), "
                 "gag -> lips/palate, cuffs -> wrist (forearm C1) + ankle (foot C3 / calf C3). "
                 "Orifice OPENING is the orifice module's job (knob 'orificeEnable').");
}

void Reset()
{
    for (int h = 0; h < 2; ++h) {
        g_insert[h]   = InsertState{};
        g_held[h]     = HeldDevice{};
        g_touch[h]    = TouchTrack{};
        g_eject[h]    = PendingEject{};
        g_gripH[h]    = HandGrip{};
        g_handGrab[h] = PendingHandGrab{};
        g_verify[h]   = PendingVerify{};
        g_plugPull[h] = PlugPull{};
    }
    g_pair = UndressPair{};
    g_gripSig[0] = GripSignal{};   // R5: silent here — Reset is the load path (no events across a load)
    g_gripSig[1] = GripSignal{};
    g_pendingRip = PendingRip{};
    g_ripCheck   = RipCheck{};
    g_unlockDrop.active.store(false, std::memory_order_relaxed);
    g_safeDrop = PendingSafeDrop{};
    g_ripIntent = RipIntent{};
    g_undressCd.clear();
    g_classCache.clear();   // FormIDs are per-session
}

// ─────────────────────────────────────────────────────────────────────────────
// SkyrimNet decorators.
//
// Nothing in the load order ever told an NPC she was gagged — verified 08-23:
// zero decorators anywhere feed DD state to SkyrimNet, and no prompt component
// reads a zad_* keyword. SkyrimNet's own `get_worn_equipment` cannot substitute:
// it renders only inside dynamic_character_bio.prompt, never in the live
// dialogue prompt, and it reports item NAMES — which are empty for every DD
// rendered device (report 23 §6.2).
//
// The Papyrus side does the DD lookup and SkyrimNet registration; the wording
// lives in the shipped drop-in prompt so it can be tuned without a rebuild:
//   SKSE/Plugins/SkyrimNet/prompts/submodules/user_final_instructions/
//       0900_vrtedd_gag.prompt
// ─────────────────────────────────────────────────────────────────────────────
void SetPaused(bool paused)
{
    g_paused.store(paused, std::memory_order_relaxed);
    if (!paused) return;

    // ⛔ RESET IS QUEUED, NOT CALLED (2026-08-27). Every caller of SetPaused arrives on the VM
    // thread - it is reached from a Papyrus native (PPB_Native.SetGesturePaused) and from the
    // add-on's legacy SetScenePaused via a mod event. Reset() clears g_undressCd and
    // g_classCache, which OnFrame iterates on the render thread: clearing an unordered_map out
    // from under an in-flight iteration is undefined behaviour, not a stale read.
    // The paused flag itself is atomic and takes effect immediately; only the teardown waits a
    // frame, and a frame of stale gesture state while pausing is harmless.
    // Same lesson as ledger P-2026-08-25-A: queue on the edge, act on the main thread.
    // R2 (2026-09-13): the queued teardown runs on the main thread, where SendEvent is safe — close an armed pull
    // and any announced grip BEFORE Reset() drops them silently. (OnFrame's paused branch usually gets there first;
    // whichever runs first clears the state, so exactly one End is sent.)
    if (auto* task = SKSE::GetTaskInterface()) task->AddTask([]() { AbortUndressFor("paused"); Reset(); });
    else                                       Reset();   // no task interface: better than leaking state
}

}   // namespace DeviceGesture
