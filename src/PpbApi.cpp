// ─────────────────────────────────────────────────────────────────────────────
// PpbApi.cpp — the PPB touch API engine (consumer contract: PpbTouchAPI.h).
//
// FIVE VARIABLES, per contact (user spec 2026-07-30):
//   WHO      the touched NPC (FormID → Actor)
//   WHERE    the named capsule (slot.child → the 107-name map, side-prefixed)
//   BY WHO   the toucher — revision 1: always the player (NPC touchers later)
//   WITH     FINGER / PALM / FIST / HAND / GRAB / WEAPON:<name> / OBJECT:<name>
//   DURATION seconds since the contact began
//
// DESIGN RULES
// * Pure geometry: point-to-capsule-surface distance on bodies PPB owns. No Havok
//   contact listeners, no physics cost, and the self-touch false positive (an NPC's
//   own hair capsules brushing her body) is STRUCTURALLY impossible — her garments
//   are never probe sources, only the player's hands/weapon/objects are.
// * Contact identity = (actor, wand, source class). The capsule under the probe
//   updates live while sliding, but the contact — and its duration — survives the
//   slide. Consumers who want per-part dwell time their own (VRTE does).
// * Everything runs on the MAIN thread (pre-drive roster + HIGGS frame callback);
//   Papyrus natives read a 4-deep snapshot rotation (the TargetBuf pattern).
// * Two-level culling keeps the scan cheap: actor cull (any probe within reach),
//   then slot cull (child 0 read stands proxy for the slot), then children.
// ─────────────────────────────────────────────────────────────────────────────

#include "PpbApi.h"
#include "PpbTouchAPI.h"
#include "CapFix.h"          // GrabDiag::ReadCapsuleWorldUSide / SlotHasLeftTwin / SlotLabel / SlotLiveChildren
#include "Diag.h"
#include "HandBox.h"         // HandBox::TipWorldU / BoxCenterWorldU
#include "NpcFingerTest.h"   // NpcFinger::PartName / WeaponPointU
#include "Interop.h"         // Interop::GetHiggs
#include "HiggsInterface.h"  // IsHoldingObject / GetGrabbedObject
#include "DismemberGuard.h"  // IsExcluded — dismember-touched actors are outside every per-actor system
#include "Tuning.h"          // ObjectHold::Api* knobs

#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace logger = SKSE::log;

namespace {

    using PPBAPI::PpbTouchContact;

    // ── constants ───────────────────────────────────────────────────────────
    constexpr int   kMaxRoster    = 8;    // driven actors considered per frame
    constexpr int   kMaxContacts  = 32;   // live contact table (≤ 6 sources × apiMaxActors)
    constexpr int   kMaxCallbacks = 8;
    constexpr int   kSlots        = 12;
    constexpr float kActorCullU   = 160.f;  // probe→actor-center reach gate (arm + weapon)
    constexpr float kSlotCullU    = 60.f;   // probe→slot-child0 gate before reading children

    inline std::uint64_t NowMs() {
        return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    inline float SegPointDistU(const float a[3], const float b[3], const float p[3]) {
        const float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float ap[3] = { p[0]-a[0], p[1]-a[1], p[2]-a[2] };
        const float len2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
        float t = len2 > 1e-8f ? (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / len2 : 0.f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        const float dx = p[0]-(a[0]+ab[0]*t), dy = p[1]-(a[1]+ab[1]*t), dz = p[2]-(a[2]+ab[2]*t);
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    // Closest distance between two segments (Ericson, clamped) — the weapon-blade probe.
    inline float SegSegDistU(const float p1[3], const float q1[3],
                             const float p2[3], const float q2[3])
    {
        const float d1[3] = { q1[0]-p1[0], q1[1]-p1[1], q1[2]-p1[2] };
        const float d2[3] = { q2[0]-p2[0], q2[1]-p2[1], q2[2]-p2[2] };
        const float r[3]  = { p1[0]-p2[0], p1[1]-p2[1], p1[2]-p2[2] };
        const float A = d1[0]*d1[0] + d1[1]*d1[1] + d1[2]*d1[2];
        const float E = d2[0]*d2[0] + d2[1]*d2[1] + d2[2]*d2[2];
        const float F = d2[0]*r[0] + d2[1]*r[1] + d2[2]*r[2];
        float sN = 0.f, tN = 0.f;
        if (A <= 1e-8f && E <= 1e-8f) {
            // both degenerate: point-point
        } else if (A <= 1e-8f) {
            tN = F / E; tN = tN < 0.f ? 0.f : (tN > 1.f ? 1.f : tN);
        } else {
            const float C = d1[0]*r[0] + d1[1]*r[1] + d1[2]*r[2];
            if (E <= 1e-8f) {
                sN = -C / A; sN = sN < 0.f ? 0.f : (sN > 1.f ? 1.f : sN);
            } else {
                const float B = d1[0]*d2[0] + d1[1]*d2[1] + d1[2]*d2[2];
                const float den = A * E - B * B;
                sN = den > 1e-8f ? (B * F - C * E) / den : 0.f;
                sN = sN < 0.f ? 0.f : (sN > 1.f ? 1.f : sN);
                tN = (B * sN + F) / E;
                if (tN < 0.f)      { tN = 0.f; sN = -C / A; }
                else if (tN > 1.f) { tN = 1.f; sN = (B - C) / A; }
                sN = sN < 0.f ? 0.f : (sN > 1.f ? 1.f : sN);
            }
        }
        const float c1[3] = { p1[0]+d1[0]*sN, p1[1]+d1[1]*sN, p1[2]+d1[2]*sN };
        const float c2[3] = { p2[0]+d2[0]*tN, p2[1]+d2[1]*tN, p2[2]+d2[2]*tN };
        const float dx = c1[0]-c2[0], dy = c1[1]-c2[1], dz = c1[2]-c2[2];
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    // ── per-frame roster (OnPreDrive fills, OnFrame consumes — same frame only) ──
    struct RosterEntry { RE::Actor* actor; std::uint32_t id; float d2; };
    RosterEntry g_roster[kMaxRoster];
    int         g_rosterN = 0;

    // ── probe sources this tick ─────────────────────────────────────────────
    enum SourceClass : int { kClsHand = 0, kClsWeapon = 1, kClsObject = 2, kClsCount = 3 };
    struct Probe {
        bool  live = false;
        bool  seg  = false;  // true: the probe is the SEGMENT p->q (weapon blade axis)
        float p[3]{};        // probe point (or segment start), world game units
        float q[3]{};        // segment end (seg only)
        float pad  = 0.f;    // extra surface (object bound radius / weapon capsule radius)
        char  name[48]{};    // weapon/object base name
    };
    struct HandProbes {
        Probe boxes[4];      // 0/1 index proximal+distal TIPs, 2 fist slab, 3 palm plate (centers)
        Probe weapon;
        Probe object;
        bool  curled = false;   // index distal tip near the palm plate = fist
        float curlDistU = -1.f; // measured tip->palm distance (calibration receipt for apiLog)
        // VRIK live finger pose (0 = closed .. 1 = open); -1 = VRIK absent. THE authoritative
        // gesture source (user 2026-07-30: "my hand are in a fist or with the index sticking
        // out cause im pushing certain button on the controller — a check on that is all
        // that is needed") — geometry could not discriminate (constant curl, see below).
        float vrikIndex = -1.f;
        float vrikMiddle = -1.f;
        // The actor this hand is HIGGS-grabbing (0 = none). Set even though grabbing an
        // actor never fills hp.object — ScanActor uses it to MUTE THE WEAPON vs the grabbed
        // actor: a hand holding her leg cannot also be "stabbing" her with the axe riding
        // that same grip (2026-07-31, user-caught: leg held in the axe hand while the other
        // hand tested — the axe reported cervix -16.9u for 13.6s, pure phantom).
        std::uint32_t grabActorId = 0;
    };
    HandProbes g_hp[2];      // [0]=R, [1]=L (HandBox hand indexing)

    // ── the contact table ───────────────────────────────────────────────────
    struct Contact {
        bool          live = false;
        bool          seen = false;             // mark-sweep flag per tick
        std::uint32_t actorId = 0;
        std::uint8_t  wand = 0;                 // 0=R 1=L
        std::uint8_t  cls  = 0;                 // SourceClass
        std::uint64_t startMs = 0;
        // ── DWELL FILTER state (2026-07-30, user spec) ─────────────────────────────────
        // "if the player touch 20 capsule in one interaction, only the one he linger a
        // certain amount of time get sent thru the API. It's not so much that we don't
        // track them, it's more about having them sent." Tracking is full-rate; EMISSION
        // (events, callbacks, the Papyrus snapshot) only carries a body part after the
        // probe lingered on it past that part's dwell class. A contact that never
        // qualifies anywhere emits NOTHING — no Start, no End.
        int           candSlot = -1, candChild = -1;   // the part currently being lingered on
        std::uint8_t  candLeft = 0;
        std::uint64_t candSinceMs = 0;
        bool          emitted = false;          // a qualified Start has gone out
        PpbTouchContact pub{};                  // the published view (qualified parts only)
    };
    Contact g_contacts[kMaxContacts];

    // ── DIGEST LAYER (2026-07-30, user spec) ────────────────────────────────────────────
    // Identity is (actor, wand, REGION) — deliberately NOT the source class, because
    // "switching from index to fist should not restart the contact". Time is accumulated
    // per part and per source; the report names whichever of each was held LONGEST. So a
    // six-second face touch that crosses five capsules, half a second each, reads:
    //     R|FINGER|Face(cheek L)|human  dur=6.11s
    // The raw layer above is untouched and still publishes everything, on its own events.
    // Live GROUP-contacts. Was 12 when identity was the coarse region; groups are finer (33 vs
    // 13), so a hand spanning a boundary can hold two at once — 16 keeps headroom for both wands
    // plus a weapon/object class without ever dropping the newest.
    constexpr int kMaxDigest   = 16;
    constexpr int kMaxPartAcc  = 16;   // distinct capsules remembered per region visit
    struct DigestContact {
        bool          live = false, seen = false, emitted = false;
        std::uint8_t  unseen = 0;      // consecutive ticks the region was not the winner
        std::uint32_t actorId = 0;
        std::uint8_t  wand = 0;
        int           region = 0;   // for the report string + the dwell CLASS
        int           sub = 0;      // ★ the CAPSULE GROUP — this is the contact's IDENTITY
        std::uint64_t startMs = 0;
        float         inRegionS = 0.f;             // accumulated time, the dwell gate
        struct PartAcc { int slot, child; std::uint8_t left; float secs; };
        PartAcc       parts[kMaxPartAcc]{};
        int           nParts = 0;
        float         srcSecs[8]{};                // accumulated seconds per SourceKind
        float         deepestU = 1e9f;             // most-negative distU seen this visit
        PpbTouchContact pub{};
    };
    DigestContact g_digest[kMaxDigest];

    // ── Papyrus snapshot (main thread writes, VM threads read) ──────────────
    struct Snapshot { int n = 0; PpbTouchContact c[kMaxContacts]{}; };
    Snapshot         g_snap[4];        // DIGEST — what the Papyrus natives read
    std::atomic<int> g_snapActive{ 0 };
    Snapshot         g_snapRaw[4];     // RAW
    std::atomic<int> g_snapRawActive{ 0 };

    // ── consumer callbacks ──────────────────────────────────────────────────
    PPBAPI::PpbTouchCallback g_cbs[kMaxCallbacks]{};
    int g_cbN = 0;

    std::uint64_t g_lastTickMs = 0;
    int g_garmentChordN[2] = { 0, 0 };   // this-scan chord counts (tail, hair) for naming


    // ── skeleton classification ─────────────────────────────────────────────
    // Mirrors PPBHook's oursPPB check: the FEMALE skeleton model path of the actor's race.
    // Male/creature/child races never point into \PPB\, so they classify as not driven.
    const char* SkeletonOf(RE::Actor* a)
    {
        if (!a || DismemberGuard::IsExcluded(a)) return nullptr;
        auto* base = a->GetActorBase();
        if (!base || !base->IsFemale()) return nullptr;    // PPB drives females only (today)
        auto* race = base->GetRace();
        if (!race) return nullptr;
        // the proven PPBHook idiom (c.oursPPB): the FEMALE skeleton model path of her race
        const char* mdl = race->skeletonModels[RE::SEXES::kFemale].GetModel();
        if (!mdl || !*mdl) return nullptr;
        // case-insensitive contains
        auto has = [&](const char* needle) {
            const size_t nl = std::strlen(needle);
            for (const char* s = mdl; *s; ++s) {
                size_t i = 0;
                while (i < nl && s[i] && std::tolower((unsigned char)s[i]) == std::tolower((unsigned char)needle[i])) ++i;
                if (i == nl) return true;
            }
            return false;
        };
        if (!has("\\ppb\\") && !has("/ppb/")) return nullptr;
        if (has("khajiit"))  return "khajiit";
        if (has("draenei"))  return "draenei";
        if (has("beast"))    return "argonian";
        return "human";
    }

    // ── source naming ───────────────────────────────────────────────────────
    const char* SourceKindName(std::uint8_t k) {
        switch (k) {
        case PPBAPI::kSourceFinger: return "FINGER";
        case PPBAPI::kSourcePalm:   return "PALM";
        case PPBAPI::kSourceFist:   return "FIST";
        case PPBAPI::kSourceHand:   return "HAND";
        case PPBAPI::kSourceGrab:   return "GRAB";
        case PPBAPI::kSourceWeapon: return "WEAPON";
        case PPBAPI::kSourceObject: return "OBJECT";
        }
        return "?";
    }

    // ── REGION MAP (2026-07-30) ─────────────────────────────────────────────────────────
    // Which anatomical region a capsule belongs to. Drives the DIGEST stream: a contact is
    // (actor, wand, region), so a finger wandering five capsules across a face stays ONE
    // event instead of five (or, under a per-capsule dwell filter, none at all — the defect
    // this replaces). Intimate is split out of Pelvis deliberately: "touched her hip" and
    // "inserted" must not look alike to a consumer.
    int RegionOfPart(int slot, int child)
    {
        if (slot == PPBAPI::kSlotTail) return PPBAPI::kRegionTail;
        if (slot == PPBAPI::kSlotHair) return PPBAPI::kRegionHair;
        switch (slot) {
        case 0:  return PPBAPI::kRegionHand;
        case 1: case 2: return PPBAPI::kRegionArm;
        case 3:  return PPBAPI::kRegionFace;
        case 4:  return PPBAPI::kRegionWaist;
        case 5:  return PPBAPI::kRegionBelly;
        case 6:  return PPBAPI::kRegionChest;
        case 7:  return PPBAPI::kRegionNeck;
        case 8: case 9: return PPBAPI::kRegionLeg;
        case 10: return PPBAPI::kRegionFoot;
        case 11: return (child >= 21 && child <= 31) ? PPBAPI::kRegionIntimate
                                                     : PPBAPI::kRegionPelvis;
        }
        return PPBAPI::kRegionNone;
    }

    const char* RegionLabel(int region)
    {
        switch (region) {
        case PPBAPI::kRegionFace:     return "Face";
        case PPBAPI::kRegionNeck:     return "Neck";
        case PPBAPI::kRegionChest:    return "Chest";
        case PPBAPI::kRegionBelly:    return "Belly";
        case PPBAPI::kRegionWaist:    return "Waist";
        case PPBAPI::kRegionPelvis:   return "Pelvis";
        case PPBAPI::kRegionIntimate: return "Intimate";
        case PPBAPI::kRegionArm:      return "Arm";
        case PPBAPI::kRegionHand:     return "Hand";
        case PPBAPI::kRegionLeg:      return "Leg";
        case PPBAPI::kRegionFoot:     return "Foot";
        case PPBAPI::kRegionTail:     return "Tail";
        case PPBAPI::kRegionHair:     return "Hair";
        }
        return "?";
    }

    // ── SUB-REGIONS (2026-07-31) ────────────────────────────────────────────────────────
    // THE SOURCE OF TRUTH for the finer bucket AND for the depth ladder. The published
    // contact-list workbook is GENERATED by parsing this function — never hand-edit the
    // record, change the table here and regenerate (tools/ppb-repo-work/gen_contact_sheet.py).
    //
    // The head ladder is the user's model (2026-07-30): a cheek or a chin is an ordinary
    // FACE TOUCH on its own — those capsules only mean "mouth" in conjunction, because the
    // gate demands the palate AND both cheeks at once. The PALATE is the roof of the cavity,
    // so a touch there means something IS inside and it outranks any lip/cheek reading. The
    // THROAT WALL is the end of the cavity and outranks even the palate.
    int SubRegionOfPart(int slot, int child)
    {
        if (slot == PPBAPI::kSlotHair) return PPBAPI::kSubHair;
        if (slot == PPBAPI::kSlotTail) {
            // thirds of the LIVE chord chain, matching BodyPartName()'s base/mid/tip split
            const int n = g_garmentChordN[0] > 0 ? g_garmentChordN[0] : 1;
            return child * 3 < n       ? PPBAPI::kSubTailBase
                 : child * 3 < n * 2   ? PPBAPI::kSubTailMid
                                       : PPBAPI::kSubTailTip;
        }
        switch (slot) {
        case 0:  return PPBAPI::kSubPalm;
        case 1:  return PPBAPI::kSubForearm;
        case 2:  return child == 2 ? PPBAPI::kSubShoulder : PPBAPI::kSubUpperArm;
        case 3:
            switch (child) {
            case 0: case 14:          return PPBAPI::kSubHead;          // cranium, occiput
            case 12: case 13:         return PPBAPI::kSubHeadEar;       // temple / ear side
            case 1:                   return PPBAPI::kSubMouthOpening;  // upper lip = the ring
            case 9:                   return PPBAPI::kSubInMouth;       // palate = inside
            case 10:                  return PPBAPI::kSubMouthWall;     // throat = the far wall
            case 11:                  return PPBAPI::kSubInMouthDeep;   // deep floor
            default:                  return PPBAPI::kSubFaceSurface;   // cheeks/chins/nose/bones
            }
        case 4:  return PPBAPI::kSubWaist;
        case 5:  return PPBAPI::kSubBelly;
        case 6:
            if (child == 11 || child == 12) return PPBAPI::kSubBreast;
            if (child == 13 || child == 14) return PPBAPI::kSubShoulderCap;
            return PPBAPI::kSubRibCage;
        case 7:  return PPBAPI::kSubNeck;
        case 8:  return PPBAPI::kSubThigh;
        case 9:  return PPBAPI::kSubCalf;
        case 10: return PPBAPI::kSubFoot;
        case 11:
            switch (child) {
            case 0: case 1: case 2:   return PPBAPI::kSubOrificeRing;   // NOT the sensor chain
            case 16: case 17:         return PPBAPI::kSubGlute;
            case 21:                  return PPBAPI::kSubIntimateExternal;
            case 22: case 23:         return PPBAPI::kSubVaginalOpening;
            case 24: case 25:         return PPBAPI::kSubVaginalDeep;
            case 26: case 27:         return PPBAPI::kSubVaginalDeepest;
            case 28: case 29:         return PPBAPI::kSubAnalOpening;
            case 30: case 31:         return PPBAPI::kSubAnalDeep;
            default:                  return PPBAPI::kSubPelvis;
            }
        }
        return PPBAPI::kSubNone;
    }

    const char* SubRegionLabel(int sub)
    {
        switch (sub) {
        case PPBAPI::kSubHead:              return "Head";
        case PPBAPI::kSubHeadEar:           return "Head (temple / ear)";
        case PPBAPI::kSubFaceSurface:       return "Face surface";
        case PPBAPI::kSubMouthOpening:      return "Mouth opening";
        case PPBAPI::kSubInMouth:           return "In mouth";
        case PPBAPI::kSubInMouthDeep:       return "In mouth (deep floor)";
        case PPBAPI::kSubMouthWall:         return "Mouth wall";
        case PPBAPI::kSubNeck:              return "Neck";
        case PPBAPI::kSubShoulderCap:       return "Shoulder cap";
        case PPBAPI::kSubRibCage:           return "Rib cage / back";
        case PPBAPI::kSubBreast:            return "Breast";
        case PPBAPI::kSubBelly:             return "Belly / midriff";
        case PPBAPI::kSubWaist:             return "Waist band";
        case PPBAPI::kSubShoulder:          return "Shoulder";
        case PPBAPI::kSubUpperArm:          return "Upper arm";
        case PPBAPI::kSubForearm:           return "Forearm";
        case PPBAPI::kSubPalm:              return "Palm";
        case PPBAPI::kSubPelvis:            return "Pelvis / hip";
        case PPBAPI::kSubOrificeRing:       return "Pelvis - orifice ring";
        case PPBAPI::kSubGlute:             return "Glute";
        case PPBAPI::kSubIntimateExternal:  return "Intimate - external";
        case PPBAPI::kSubVaginalOpening:    return "Intimate - vaginal (opening)";
        case PPBAPI::kSubVaginalDeep:       return "Intimate - vaginal (deep)";
        case PPBAPI::kSubVaginalDeepest:    return "Intimate - vaginal (deepest)";
        case PPBAPI::kSubAnalOpening:       return "Intimate - anal (opening)";
        case PPBAPI::kSubAnalDeep:          return "Intimate - anal (deep)";
        case PPBAPI::kSubThigh:             return "Thigh";
        case PPBAPI::kSubCalf:              return "Calf";
        case PPBAPI::kSubFoot:              return "Foot";
        case PPBAPI::kSubTailBase:          return "Tail - base (root third)";
        case PPBAPI::kSubTailMid:           return "Tail - mid (middle third)";
        case PPBAPI::kSubTailTip:           return "Tail - tip (far third)";
        case PPBAPI::kSubHair:              return "Hair";
        }
        return "?";
    }

    // The ladder collapsed to a number: how far IN, ignoring where.
    int SubRegionDepthOf(int sub)
    {
        switch (sub) {
        case PPBAPI::kSubMouthOpening:
        case PPBAPI::kSubVaginalOpening:
        case PPBAPI::kSubAnalOpening:       return PPBAPI::kDepthOpening;
        case PPBAPI::kSubInMouth:
        case PPBAPI::kSubInMouthDeep:
        case PPBAPI::kSubVaginalDeep:
        case PPBAPI::kSubAnalDeep:          return PPBAPI::kDepthInside;
        case PPBAPI::kSubMouthWall:
        case PPBAPI::kSubVaginalDeepest:    return PPBAPI::kDepthDeepest;
        }
        return PPBAPI::kDepthSurface;
    }

    // Stamp the three classification bytes onto a contact. One call site per stream so the
    // published POD can never disagree with the accessors.
    int WeaponClassOf(bool left);          // defined with the engine-contact block below
    int WeaponEdgeOfClass(int c);

    inline void StampWeapon(PpbTouchContact& p, int wand, bool fromEngine)
    {
        if (p.sourceKind == PPBAPI::kSourceWeapon) {
            const int c = WeaponClassOf(wand != 0);
            if (c != PPBAPI::kWeapNone) {          // keep the last good class through a release tick
                p.weaponClass = (unsigned char)c;
                p.weaponEdge  = (unsigned char)WeaponEdgeOfClass(c);
            }
        } else { p.weaponClass = 0; p.weaponEdge = 0; }
        p.engineContact = fromEngine ? 1 : 0;
    }

    inline void StampClass(PpbTouchContact& p, int slot, int child, int regionOverride = -1)
    {
        const int sub = SubRegionOfPart(slot, child);
        p.region    = (unsigned char)(regionOverride >= 0 ? regionOverride
                                                          : RegionOfPart(slot, child));
        p.subRegion = (unsigned char)sub;
        p.depth     = (unsigned char)SubRegionDepthOf(sub);
    }

    // Dwell class per REGION for the digest stream (the raw stream keeps per-part dwell).
    float DwellSForRegion(int region)
    {
        switch (region) {
        case PPBAPI::kRegionIntimate: return ObjectHold::ApiDwellSensorS();
        case PPBAPI::kRegionPelvis:   return ObjectHold::ApiDwellComS();
        case PPBAPI::kRegionFace:     return ObjectHold::ApiDwellHeadS();
        case PPBAPI::kRegionTail:
        case PPBAPI::kRegionHair:     return ObjectHold::ApiDwellTailS();
        }
        return ObjectHold::ApiDwellS();
    }

    // Dwell class per body part — how long the probe must linger before that part is
    // SENT through the API. All live knobs; 0 = instant. Sensors get the shortest dwell
    // (an insertion is deliberate); the pelvis the longest (incidental brushes are common).
    float DwellSFor(int slot, int child)
    {
        if (slot == 11 && child >= 21 && child <= 31) return ObjectHold::ApiDwellSensorS();
        if (slot == 11)                               return ObjectHold::ApiDwellComS();
        if (slot == 3)                                return ObjectHold::ApiDwellHeadS();
        if (slot >= 100)                              return ObjectHold::ApiDwellTailS();
        return ObjectHold::ApiDwellS();
    }

    // BODYPART string: named children get the map name (side-prefixed on sided slots);
    // unnamed fall back to "<slot>.C<n>" so a touch is NEVER silently dropped.
    void BodyPartName(int slot, bool left, int child, char* out, size_t cap)
    {
        // Garment pseudo-slots: WHERE = the region along the chain, not a map name.
        if (slot == PPBAPI::kSlotTail || slot == PPBAPI::kSlotHair) {
            if (slot == PPBAPI::kSlotHair) { std::snprintf(out, cap, "hair"); return; }
            const int n = g_garmentChordN[0] > 0 ? g_garmentChordN[0] : 1;
            const char* seg = child * 3 < n ? "base" : (child * 3 < n * 2 ? "mid" : "tip");
            std::snprintf(out, cap, "tail (%s)", seg);
            return;
        }
        const char* nm = NpcFinger::PartName(slot, child);
        const bool sided = GrabDiag::SlotHasLeftTwin(slot);
        if (nm) {
            if (sided) std::snprintf(out, cap, "%s %s", left ? "L" : "R", nm);
            else       std::snprintf(out, cap, "%s", nm);
        } else {
            std::snprintf(out, cap, "%s%s.C%d", GrabDiag::SlotLabel(slot),
                          (sided && left) ? "L" : "", child);
        }
    }

    // ── probe collection (once per tick) ────────────────────────────────────
    void CollectProbes()
    {
        auto* hig = Interop::GetHiggs();
        // Publish HIGGS's weapon bodies so the contact listener can recognise them by pointer.
        // This is what turns Havok's own narrowphase into our weapon probe (Diag.h).
        // HIGGS hands back the bhk WRAPPER; contact events carry the hkpRigidBody. Step through
        // at +0x10 — the same PortBhkRigidBody layout WeaponSegmentU already relies on.
        auto hkpOf = [](RE::NiObject* ni) -> void* {
            if (!ni) return nullptr;
            auto* hk = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(ni) + 0x10);
            return (reinterpret_cast<std::uintptr_t>(hk) & 7) ? nullptr : hk;
        };
        if (hig) Diag::PublishWeaponBodies(hkpOf(hig->GetWeaponRigidBody(false)),
                                           hkpOf(hig->GetWeaponRigidBody(true)));
        else     Diag::PublishWeaponBodies(nullptr, nullptr);
        for (int hand = 0; hand < 2; ++hand) {
            HandProbes& hp = g_hp[hand];
            hp = HandProbes{};
            for (int b = 0; b < 4; ++b) {
                float* p = hp.boxes[b].p;
                const bool ok = (b <= 1) ? HandBox::TipWorldU(hand, b, p)
                                         : HandBox::BoxCenterWorldU(hand, b, p);
                hp.boxes[b].live = ok;
            }
            // curl: index DISTAL tip riding at the palm plate = fist. The distance is kept
            // as a CALIBRATION RECEIPT — apiLog prints it on every hand START/END so the
            // fist threshold gets set from measured open-vs-fist numbers, not guessed:
            // the first session classified an open hand as FIST on the guessed default (7).
            if (hp.boxes[1].live && hp.boxes[3].live) {
                const float dx = hp.boxes[1].p[0] - hp.boxes[3].p[0];
                const float dy = hp.boxes[1].p[1] - hp.boxes[3].p[1];
                const float dz = hp.boxes[1].p[2] - hp.boxes[3].p[2];
                hp.curlDistU = std::sqrt(dx*dx + dy*dy + dz*dz);
                hp.curled = hp.curlDistU < ObjectHold::ApiFistTipPalmU();
            }
            const bool isLeft = hand == 1;
            if (auto* vrik = Interop::GetVrik()) {
                hp.vrikIndex  = vrik->getFingerPos(isLeft, 1);
                hp.vrikMiddle = vrik->getFingerPos(isLeft, 2);
            }
            // Weapon = the blade SEGMENT (hilt->tip + radius) read off HIGGS's weapon body.
            // The first probe was the body POSITION, which sits at the HILT — prodding with
            // the blade tip never registered (user-verified miss, 2026-07-30). The point
            // fallback inside WeaponSegmentU covers unreadable shapes; its one-shot log
            // names the shape actually carried.
            if (GrabDiag::WeaponSegmentU(isLeft, hp.weapon.p, hp.weapon.q, &hp.weapon.pad)) {
                // ── BLADE-RADIUS CAP (2026-07-31, user-caught) ──────────────────────────
                // The form-bound radius is the weapon's second extent — for an axe that is
                // the blade PLANE's breadth (23u), making the probe a 46u-diameter barrel:
                // the axis can sit 6u off a capsule and still read -17u "deep". The cap
                // bounds it to something blade-THICKNESS-like. Swords/knives carry slim
                // bounds and are unaffected. 0 = uncapped.
                const float rCap = ObjectHold::ApiWeaponRMaxU();
                if (rCap > 0.f && hp.weapon.pad > rCap) hp.weapon.pad = rCap;
                hp.weapon.seg  = true;
                hp.weapon.live = true;
                // name = the equipped weapon in that hand (display name; may be empty)
                if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* obj = pl->GetEquippedObject(isLeft))
                        std::snprintf(hp.weapon.name, sizeof hp.weapon.name, "%s", obj->GetName());
                }
            }
            if (hig && hig->IsHoldingObject(isLeft)) {
                if (auto* refr = hig->GetGrabbedObject(isLeft)) {
                    if (auto* grabbed = refr->As<RE::Actor>())
                        hp.grabActorId = grabbed->GetFormID();   // mutes the weapon vs her
                    if (!refr->As<RE::Actor>()) {          // a grabbed ACTOR is GRAB, not OBJECT
                        if (auto* d3 = refr->Get3D()) {
                            const auto& wb = d3->worldBound;
                            hp.object.p[0] = wb.center.x; hp.object.p[1] = wb.center.y;
                            hp.object.p[2] = wb.center.z; hp.object.pad = wb.radius;
                        } else {
                            const auto pos = refr->GetPosition();
                            hp.object.p[0] = pos.x; hp.object.p[1] = pos.y; hp.object.p[2] = pos.z;
                        }
                        hp.object.live = true;
                        std::snprintf(hp.object.name, sizeof hp.object.name, "%s", refr->GetName());
                    }
                }
            }
            // ── HELD-HAND SUPPRESSION (2026-07-30, user spec) ────────────────────────────
            // If THIS hand holds a weapon or an object, its bare-hand boxes are noise: your
            // palm is on the grip, so every knife contact came with a phantom FIST contact
            // from the same wand (seen in the session-4 log and wrongly defended as a
            // feature). The OTHER hand is untouched — one-handed use keeps full hand data.
            // GRAB is unaffected: grabbing an ACTOR never sets hp.object (actors are skipped).
            //
            // ★ INDEX EXCEPTION (2026-07-31, user: "send a line for each hand IF both are in
            // contact"): full muting made a holding hand INVISIBLE — with a weapon carried
            // all session, that wand could never report a touch, so two-hand interactions
            // read one-handed. But the grip noise is the PALM and SLAB (they ride the grip);
            // a deliberately EXTENDED index is a poke. VRIK separates the two from the
            // controller itself: gripping keeps the index on the trigger (reads closed),
            // pointing it off-trigger reads open. So while holding: palm+slab always muted,
            // index boxes stay live only while VRIK says the finger is extended.
            // VRIK absent (-1) fails closed = the old full mute. apiSuppressHeldHand 2 =
            // strict full mute (the pre-exception behaviour) for consumers that want it.
            if (ObjectHold::ApiSuppressHeldHand() && (hp.weapon.live || hp.object.live)) {
                hp.boxes[2].live = hp.boxes[3].live = false;      // slab + palm = grip noise
                const bool idxOpen = hp.vrikIndex > 0.55f;
                if (ObjectHold::ApiSuppressHeldHandStrict() || !idxOpen)
                    hp.boxes[0].live = hp.boxes[1].live = false;  // index pair
            }
        }
    }

    // ── the per-(actor, hand, class) nearest-capsule find ───────────────────
    struct Hit {
        bool  found = false;
        float dist  = 1e9f;    // surface distance (probe pad already subtracted)
        int   slot = 0, child = 0;
        bool  left = false;
        int   viaBox = -1;     // which hand box made the nearest contact (kClsHand only)
    };

    // ── WEAPON CLASS / EDGE (2026-08-01) ────────────────────────────────────────────────
    // From the equipped record's animationType — the game's own classification, so it is right
    // for mod weapons too (an "Iron Rapier" is authored as kOneHandSword and reports Sword).
    // Name-matching would break on every translation and every mod's naming whim.
    int WeaponClassOf(bool left)
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* frm = pl ? pl->GetEquippedObject(left) : nullptr;
        auto* wp = frm ? frm->As<RE::TESObjectWEAP>() : nullptr;
        if (!wp) return PPBAPI::kWeapNone;
        switch (wp->GetWeaponType()) {
        case RE::WEAPON_TYPE::kHandToHandMelee: return PPBAPI::kWeapFist;
        case RE::WEAPON_TYPE::kOneHandSword:    return PPBAPI::kWeapSword;
        case RE::WEAPON_TYPE::kOneHandDagger:   return PPBAPI::kWeapDagger;
        case RE::WEAPON_TYPE::kOneHandAxe:      return PPBAPI::kWeapAxe;
        case RE::WEAPON_TYPE::kOneHandMace:     return PPBAPI::kWeapMace;
        case RE::WEAPON_TYPE::kTwoHandSword:    return PPBAPI::kWeapGreatsword;
        case RE::WEAPON_TYPE::kTwoHandAxe:      return PPBAPI::kWeapBattleaxe;
        case RE::WEAPON_TYPE::kBow:             return PPBAPI::kWeapBow;
        case RE::WEAPON_TYPE::kStaff:           return PPBAPI::kWeapStaff;
        case RE::WEAPON_TYPE::kCrossbow:        return PPBAPI::kWeapCrossbow;
        default:                                return PPBAPI::kWeapOther;
        }
    }

    const char* WeaponClassLabel(int c)
    {
        switch (c) {
        case PPBAPI::kWeapFist:       return "Fist";
        case PPBAPI::kWeapSword:      return "Sword";
        case PPBAPI::kWeapDagger:     return "Dagger";
        case PPBAPI::kWeapAxe:        return "War axe";
        case PPBAPI::kWeapMace:       return "Mace";
        case PPBAPI::kWeapGreatsword: return "Greatsword";
        case PPBAPI::kWeapBattleaxe:  return "Battleaxe / warhammer";
        case PPBAPI::kWeapBow:        return "Bow";
        case PPBAPI::kWeapStaff:      return "Staff";
        case PPBAPI::kWeapCrossbow:   return "Crossbow";
        case PPBAPI::kWeapOther:      return "Other";
        }
        return "";
    }

    // Does it cut, crush, or stab? ⚠ Skyrim's record model has NO damage-type field — every
    // melee weapon deals one generic physical damage, and "blunt vs blade" is a property of the
    // SHAPE, not of any game data. This is therefore an honest inference from the class, not a
    // value read out of the record. A battleaxe is grouped with warhammers by animationType, so
    // that one row is genuinely ambiguous and is called BLADE (the commoner case).
    int WeaponEdgeOfClass(int c)
    {
        switch (c) {
        case PPBAPI::kWeapSword:
        case PPBAPI::kWeapAxe:
        case PPBAPI::kWeapGreatsword:
        case PPBAPI::kWeapBattleaxe:  return PPBAPI::kEdgeBlade;
        case PPBAPI::kWeapDagger:     return PPBAPI::kEdgePierce;
        case PPBAPI::kWeapMace:
        case PPBAPI::kWeapStaff:
        case PPBAPI::kWeapFist:
        case PPBAPI::kWeapBow:
        case PPBAPI::kWeapCrossbow:   return PPBAPI::kEdgeBlunt;
        }
        return PPBAPI::kEdgeNone;
    }

    // ══ ENGINE-TRUTH WEAPON CONTACTS (2026-08-01) ══════════════════════════════════════
    // Havok's narrowphase already computed the weapon-vs-capsule contact the player feels.
    // Diag's contact listener records (otherBody, shapeKey, wand); here we resolve the body
    // POINTER to (actor, slot, side) by matching against the slots of the actors we are already
    // scanning. Result: exact slot AND exact capsule child, with no segment, radius or bounding
    // box anywhere in the path — so it is correct for a rapier's swept hilt, an axe head and a
    // club alike, which no geometric approximation was.
    struct EngineHit { std::uint32_t actorId; int slot; int child; bool left; int wand; float distU; };
    EngineHit g_engHits[24];
    int       g_engN = 0;

    // ── ENGINE-HIT LATCH (2026-08-01, caught by the per-line src= tag) ──────────────────
    // Havok does not regenerate a contact point on EVERY step of a held touch, so a contact
    // that STARTED as src=ENG could END as src=GEO — and distU then jumps between two
    // different measurement systems mid-contact, which is worse than either alone. Hold the
    // last engine result briefly: the touch is still live, the engine simply had nothing new
    // to say this tick. One latch per (actor, wand); newest always wins, so a stale entry can
    // never accumulate. kEngHoldTicks 3 ~= 0.75s at apiHz 4.
    struct EngineLatch { std::uint32_t actorId; int wand; EngineHit hit; std::uint64_t tick; bool live; };
    EngineLatch g_engLatch[8]{};
    std::uint64_t g_engTick = 0;
    constexpr std::uint64_t kEngHoldTicks = 3;

    void LatchEngineHit(const EngineHit& h)
    {
        EngineLatch* slot = nullptr;
        for (auto& L : g_engLatch)
            if (L.live && L.actorId == h.actorId && L.wand == h.wand) { slot = &L; break; }
        if (!slot) for (auto& L : g_engLatch) if (!L.live) { slot = &L; break; }
        if (!slot) slot = &g_engLatch[0];                       // table full: recycle the first
        *slot = { h.actorId, h.wand, h, g_engTick, true };
    }

    bool EngineHitFor(std::uint32_t actorId, int wand)
    {
        for (const auto& L : g_engLatch)
            if (L.live && L.actorId == actorId && (L.wand & 1) == (wand & 1)) return true;
        return false;
    }

    void CollectEngineWeaponHits(int nRoster)
    {
        ++g_engTick;
        g_engN = 0;
        // age the latches out first, so a weapon that left never keeps answering
        for (auto& L : g_engLatch)
            if (L.live && g_engTick - L.tick > kEngHoldTicks) L.live = false;
        Diag::WeaponContact raw[24];
        const int n = Diag::DrainWeaponContacts(raw, 24);
        if (n <= 0) return;
        for (int i = 0; i < n && g_engN < 24; ++i) {
            if (!raw[i].otherBody) continue;
            for (int ai = 0; ai < nRoster; ++ai) {
                RE::Actor* a = g_roster[ai].actor;
                if (!a) continue;
                bool done = false;
                for (int slot = 0; slot < 12 && !done; ++slot)
                    for (int side = 0; side < 2; ++side) {
                        void* b = GrabDiag::SlotBodyRaw(a, slot, side != 0);
                        if (!b || b != raw[i].otherBody) continue;
                        // Havok metres -> game units; this is the engine's REAL separating
                        // distance, so a graze and a deep press no longer look identical.
                        const EngineHit eh = { a->GetFormID(), slot, (int)raw[i].child,
                                               side != 0, raw[i].wand,
                                               raw[i].distHavok * 69.9422f };   // havok m -> game u
                        g_engHits[g_engN++] = eh;
                        LatchEngineHit(eh);
                        done = true; break;
                    }
                if (done) break;
            }
        }
    }

    void ScanActor(RE::Actor* actor, Hit out[2][kClsCount])
    {
        // Interior-sensor race — filled by the body-slot loop, applied as an override at
        // the bottom of this function. See the isSensor comment in the loop.
        Hit sens[2][kClsCount]{};
        // ── GRAB MUTES THE WEAPON vs the grabbed actor (2026-07-31, user-caught) ────────
        // A hand HIGGS-grabbing THIS actor is holding her, not wielding at her — the weapon
        // rides the grip wherever the grab goes, so its contacts on HER are pure phantoms
        // (the axe-in-the-leg-holding-hand reported "cervix -16.9u dur=13.6s" while the
        // OTHER hand did the actual touching). Weapon stays live vs every other actor.
        const std::uint32_t aid = actor->GetFormID();
        const bool wpnOk[2] = { g_hp[0].weapon.live && g_hp[0].grabActorId != aid,
                                g_hp[1].weapon.live && g_hp[1].grabActorId != aid };
        // actor-level cull: any live probe within reach of the actor's center?  Segment
        // probes (the weapon blade) test BOTH endpoints and the midpoint — a blade tip can
        // be in range while the hilt is not.
        const auto ap = actor->GetPosition();
        const float apf[3] = { ap.x, ap.y, ap.z };
        auto probeNear = [](const Probe* pr, const float t[3], float rangeU) {
            auto near1 = [&](const float* pt) {
                const float dx = pt[0]-t[0], dy = pt[1]-t[1], dz = pt[2]-t[2];
                return dx*dx + dy*dy + dz*dz < rangeU * rangeU;
            };
            if (near1(pr->p)) return true;
            if (!pr->seg) return false;
            if (near1(pr->q)) return true;
            const float mid[3] = { (pr->p[0]+pr->q[0])*0.5f, (pr->p[1]+pr->q[1])*0.5f,
                                   (pr->p[2]+pr->q[2])*0.5f };
            return near1(mid);
        };
        bool anyNear = false;
        for (int hand = 0; hand < 2 && !anyNear; ++hand) {
            const HandProbes& hp = g_hp[hand];
            const Probe* all[6] = { &hp.boxes[0], &hp.boxes[1], &hp.boxes[2], &hp.boxes[3],
                                    &hp.weapon, &hp.object };
            for (const Probe* pr : all)
                if (pr->live && probeNear(pr, apf, kActorCullU)) { anyNear = true; break; }
        }
        if (!anyNear) return;

        for (int slot = 0; slot < kSlots; ++slot) {
            const int sides = GrabDiag::SlotHasLeftTwin(slot) ? 2 : 1;
            const int listN = GrabDiag::SlotLiveChildren(actor, slot);
            const int nCh   = listN > 0 ? listN : 1;       // 0 = single plain capsule → child 0
            for (int side = 0; side < sides; ++side) {
                const bool left = side == 1;
                float a[3], b[3], r;
                // slot cull: child 0 stands proxy for the whole slot
                if (!GrabDiag::ReadCapsuleWorldUSide(actor, slot, left, 0, a, b, &r)) continue;
                bool slotNear = false;
                for (int hand = 0; hand < 2 && !slotNear; ++hand) {
                    const HandProbes& hp = g_hp[hand];
                    const Probe* all[6] = { &hp.boxes[0], &hp.boxes[1], &hp.boxes[2],
                                            &hp.boxes[3], &hp.weapon, &hp.object };
                    for (const Probe* pr : all) {
                        if (!pr->live) continue;
                        const float d = pr->seg ? SegSegDistU(a, b, pr->p, pr->q)
                                                : SegPointDistU(a, b, pr->p);
                        if (d < kSlotCullU) { slotNear = true; break; }
                    }
                }
                if (!slotNear) continue;

                // palate cache for the throat wall's inside-test (C9 is read before C10)
                float c9a[3]{}, c9b[3]{}, c9r = 0.f; bool c9ok = false;
                for (int ch = 0; ch < nCh; ++ch) {
                    if (ch > 0 && !GrabDiag::ReadCapsuleWorldUSide(actor, slot, left, ch, a, b, &r))
                        continue;
                    if (slot == 3 && ch == 9) {
                        c9a[0]=a[0]; c9a[1]=a[1]; c9a[2]=a[2];
                        c9b[0]=b[0]; c9b[1]=b[1]; c9b[2]=b[2];
                        c9r = r; c9ok = true;
                    }
                    // PRIORITY (interior) capsules compete in a SEPARATE race that overrides the
                    // general one below. Without this they can never be reported: the general
                    // race keeps the most-penetrated capsule, and a BIG capsule always beats a
                    // small one, because a small capsule's distance floors at -r while a large
                    // one keeps going. User-verified miss: a 5s knife insertion reported
                    // "L upper thigh d=-11.43" and named zero sensors. Among priority capsules
                    // the deepest wins — so WHERE literally answers "how far it reached".
                    //
                    // ★ 2026-07-31 — THE MOUTH CHAIN WAS MISSING FROM THIS SET (VRTE report 16
                    // §4.3, verified). The palate (r 1.32) and throat (r 0.88) sit INSIDE the
                    // cranium capsule (r 5.52) and raced it unprotected — byte-for-byte the same
                    // masking bug, one region over. A finger in the mouth could therefore report
                    // "cranium", and the sub-region layer shipped 2026-07-31 would have called
                    // that "Head" while the user's own ladder says a palate touch means IN MOUTH.
                    // Declaring a depth ladder is worthless if the deep capsule never wins.
                    //
                    // Head C11 (under-jaw / deep floor) is deliberately NOT here: it is GEO-tagged
                    // (never eye-verified, doc 15 §1) and reachable from OUTSIDE under the jaw, so
                    // promoting it would let a chin scritch outrank the face.
                    const bool isSensor = (slot == 11 && ch >= 21 && ch <= 31)   // pelvic chain
                                       || (slot == 3  && (ch == 9 || ch == 10)); // palate, throat
                    // ★ THROAT INSIDE-TEST (2026-07-31, user-caught same day the throat joined
                    // the priority set): C10 sits close behind the EXTERIOR throat surface, so
                    // an axe pressed on her neck from outside reached it (d 0.21 -> -0.84) and
                    // priority made it WIN — "Face(throat wall)" for an outside neck press, the
                    // exact false positive C11 was excluded for. The discriminator is the mouth
                    // gate's own deep-hold asymmetry: from INSIDE the cavity the palate is near;
                    // from outside the throat it is far. So the throat only counts as a priority
                    // capsule when the probe is ALSO within mouthDeepPalU of the palate. One
                    // capsule is never proof — the user's original mouth-gate architecture.
                    const bool  isThroat = slot == 3 && ch == 10;
                    const float deepPal  = ObjectHold::MouthDeepPalU();
                    auto nearPalate = [&](const float* pt, const float* q2) {
                        if (!c9ok) return false;
                        const float dp = (q2 ? SegSegDistU(c9a, c9b, pt, q2)
                                             : SegPointDistU(c9a, c9b, pt)) - c9r;
                        return dp < deepPal;
                    };
                    for (int hand = 0; hand < 2; ++hand) {
                        const HandProbes& hp = g_hp[hand];
                        for (int bx = 0; bx < 4; ++bx) {
                            if (!hp.boxes[bx].live) continue;
                            const float d = SegPointDistU(a, b, hp.boxes[bx].p) - r;
                            Hit& h = out[hand][kClsHand];
                            if (d < h.dist) { h.found = true; h.dist = d; h.slot = slot;
                                              h.child = ch; h.left = left; h.viaBox = bx; }
                            if (isSensor && (!isThroat || nearPalate(hp.boxes[bx].p, nullptr))) {
                                Hit& sh = sens[hand][kClsHand];
                                if (d < sh.dist) { sh.found = true; sh.dist = d; sh.slot = slot;
                                                   sh.child = ch; sh.left = left; sh.viaBox = bx; }
                            }
                        }
                        if (wpnOk[hand]) {
                            const float d = (hp.weapon.seg
                                                ? SegSegDistU(a, b, hp.weapon.p, hp.weapon.q)
                                                : SegPointDistU(a, b, hp.weapon.p))
                                            - r - hp.weapon.pad;
                            Hit& h = out[hand][kClsWeapon];
                            if (d < h.dist) { h.found = true; h.dist = d; h.slot = slot;
                                              h.child = ch; h.left = left; }
                            if (isSensor && (!isThroat ||
                                             nearPalate(hp.weapon.p, hp.weapon.seg ? hp.weapon.q : nullptr))) {
                                Hit& sh = sens[hand][kClsWeapon];
                                if (d < sh.dist) { sh.found = true; sh.dist = d; sh.slot = slot;
                                                   sh.child = ch; sh.left = left; }
                            }
                        }
                        if (hp.object.live) {
                            const float d = SegPointDistU(a, b, hp.object.p) - r - hp.object.pad;
                            Hit& h = out[hand][kClsObject];
                            if (d < h.dist) { h.found = true; h.dist = d; h.slot = slot;
                                              h.child = ch; h.left = left; }
                            if (isSensor && (!isThroat || nearPalate(hp.object.p, nullptr))) {
                                Hit& sh = sens[hand][kClsObject];
                                if (d < sh.dist) { sh.found = true; sh.dist = d; sh.slot = slot;
                                                   sh.child = ch; sh.left = left; }
                            }
                        }
                    }
                }
            }
        }

        // ── GARMENT TARGETS (2026-07-30): TAIL chords compete in the same nearest-capsule
        // race as the body slots (pseudo-slot 100). HAIR (101) is knob-gated OFF by default
        // — user decision same day: hair chords DRAPE the head and shoulders, so in a
        // nearest-surface race a finger aimed at a cheek lands on a hair chord and face
        // touches misreport. apiHairTarget 1 adds hair anyway (live knob).
        // GRAB never classifies here unless HIGGS holds the ACTOR itself.
        const std::uint32_t gid = actor->GetFormID();
        const int kindMax = ObjectHold::ApiHairTarget() ? 2 : 1;
        for (int kind = 0; kind < kindMax; ++kind) {
            const int n = NpcFinger::GarmentChords(gid, kind);
            g_garmentChordN[kind] = n;
            for (int ch = 0; ch < n; ++ch) {
                float a[3], b[3], r;
                if (!NpcFinger::GarmentChordU(gid, kind, ch, a, b, &r)) continue;
                const int pseudo = kind == 0 ? PPBAPI::kSlotTail : PPBAPI::kSlotHair;
                for (int hand = 0; hand < 2; ++hand) {
                    const HandProbes& hp = g_hp[hand];
                    for (int bx = 0; bx < 4; ++bx) {
                        if (!hp.boxes[bx].live) continue;
                        const float d = SegPointDistU(a, b, hp.boxes[bx].p) - r;
                        Hit& h = out[hand][kClsHand];
                        if (d < h.dist) { h.found = true; h.dist = d; h.slot = pseudo;
                                          h.child = ch; h.left = false; h.viaBox = bx; }
                    }
                    if (wpnOk[hand]) {
                        const float d = (hp.weapon.seg
                                            ? SegSegDistU(a, b, hp.weapon.p, hp.weapon.q)
                                            : SegPointDistU(a, b, hp.weapon.p))
                                        - r - hp.weapon.pad;
                        Hit& h = out[hand][kClsWeapon];
                        if (d < h.dist) { h.found = true; h.dist = d; h.slot = pseudo;
                                          h.child = ch; h.left = false; }
                    }
                    if (hp.object.live) {
                        const float d = SegPointDistU(a, b, hp.object.p) - r - hp.object.pad;
                        Hit& h = out[hand][kClsObject];
                        if (d < h.dist) { h.found = true; h.dist = d; h.slot = pseudo;
                                          h.child = ch; h.left = false; }
                    }
                }
            }
        }

        // Interior-sensor override: a probe that reached an orifice sensor reports the
        // SENSOR, not whatever big capsule it is also buried in. Enter-level threshold —
        // once inserted, the sensor distance is far below it anyway.
        {
            const float touchU = ObjectHold::ApiTouchU();
            for (int hand = 0; hand < 2; ++hand)
                for (int cls = 0; cls < kClsCount; ++cls)
                    if (sens[hand][cls].found && sens[hand][cls].dist <= touchU)
                        out[hand][cls] = sens[hand][cls];
        }

        // ── ENGINE-TRUTH WEAPON OVERRIDE (2026-08-01) ──────────────────────────────────
        // A recorded Havok contact is not an estimate — the solver moved her because of it.
        // So it OUTRANKS the geometric weapon probe entirely: exact slot, exact capsule child,
        // and a distance of 0 (touching by definition; the probe's signed depth is a
        // reconstruction, this is the real event). Sensor priority still applies afterwards for
        // interior capsules, so an insertion keeps naming how far it reached.
        // ⚠ FOURTH instance of the capped-diagnostic trap in this project, and I wrote the rule.
        // A 4-line cap burns on the first burst, so "which slots does the engine actually
        // report?" stayed unanswerable exactly when it mattered. Per-slot RUNNING TOTALS, dumped
        // every 64 hits: a histogram cannot exhaust, and it answers coverage questions directly.
        static std::atomic<int> s_engLogged{ 0 };
        static std::atomic<int> s_engBySlot[12]{};
        static std::atomic<int> s_engTotal{ 0 };
        for (const auto& L : g_engLatch) {
            if (!L.live) continue;
            const EngineHit& eh = L.hit;
            if (eh.actorId != aid) continue;
            if (eh.slot >= 0 && eh.slot < 12) s_engBySlot[eh.slot].fetch_add(1, std::memory_order_relaxed);
            const int tot = s_engTotal.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ObjectHold::ApiLogEnabled()) {
                if (s_engLogged.fetch_add(1, std::memory_order_relaxed) < 4)
                    logger::info("API weapon: ENGINE CONTACT {:08X} slot={} child={} side={} wand={} "
                                 "— from Havok's own narrowphase (shape key), no geometry involved",
                                 eh.actorId, eh.slot, eh.child, eh.left ? "L" : "R",
                                 eh.wand ? "L" : "R");
                if ((tot % 64) == 0)
                    logger::info("API weapon ENGINE-CONTACT coverage after {}: hand={} fore={} "
                                 "uarm={} head={} spn0={} spn1={} spn2={} neck={} thigh={} "
                                 "calf={} foot={} com={}  (zeros = slots Havok never reports a "
                                 "weapon contact on — that is the coverage gap, not a bug in the "
                                 "override)",
                                 tot,
                                 s_engBySlot[0].load(), s_engBySlot[1].load(), s_engBySlot[2].load(),
                                 s_engBySlot[3].load(), s_engBySlot[4].load(), s_engBySlot[5].load(),
                                 s_engBySlot[6].load(), s_engBySlot[7].load(), s_engBySlot[8].load(),
                                 s_engBySlot[9].load(), s_engBySlot[10].load(), s_engBySlot[11].load());
            }
            const int hand = eh.wand & 1;
            Hit& h = out[hand][kClsWeapon];
            h.found = true; h.dist = eh.distU;
            h.slot = eh.slot; h.child = eh.child; h.left = eh.left;
        }
    }

    // ── source classification for a hand-class contact ──────────────────────
    std::uint8_t ClassifyHand(RE::Actor* touched, int hand, const Hit& h)
    {
        const bool isLeft = hand == 1;
        if (auto* hig = Interop::GetHiggs()) {
            if (hig->IsHoldingObject(isLeft)) {
                auto* refr = hig->GetGrabbedObject(isLeft);
                if (refr && refr->GetFormID() == touched->GetFormID())
                    return PPBAPI::kSourceGrab;
            }
        }
        const HandProbes& hp = g_hp[hand];
        // VRIK pose first — the live controller-driven hand state (0=closed..1=open).
        // Index open + middle closed = pointing; both closed = fist; both open = palm.
        // The 0.45/0.55 band is deliberate hysteresis dead space: mid-transition poses
        // fall through to the geometric classifiers below rather than flickering.
        if (hp.vrikIndex >= 0.f && hp.vrikMiddle >= 0.f) {
            const bool idxOpen    = hp.vrikIndex  > 0.55f, idxClosed = hp.vrikIndex  < 0.45f;
            const bool midOpen    = hp.vrikMiddle > 0.55f, midClosed = hp.vrikMiddle < 0.45f;
            if (idxOpen  && midClosed) return PPBAPI::kSourceFinger;
            if (idxClosed && midClosed) return PPBAPI::kSourceFist;
            if (idxOpen  && midOpen)   return PPBAPI::kSourcePalm;
        }
        if (hp.curled) return PPBAPI::kSourceFist;
        // Which box led the contact IS the gesture (2026-07-30, measured): on this rig the
        // curl distance is a CONSTANT ~3.7-4.3u for every gesture — the game hand's fingers
        // do not articulate — so tip-to-palm geometry cannot tell fist from open hand. But
        // the leading box can: a poke leads with an index box (0/1), an open-palm press
        // leads with the palm plate (3), and a knuckle press leads with the FIST SLAB (2),
        // which spans the middle/ring/pinky proximal row — exactly the surface a fist
        // presses with. The curl path above stays for rigs that DO articulate.
        if (h.viaBox == 0 || h.viaBox == 1) return PPBAPI::kSourceFinger;
        if (h.viaBox == 3)                  return PPBAPI::kSourcePalm;
        if (h.viaBox == 2)                  return PPBAPI::kSourceFist;
        return PPBAPI::kSourceHand;
    }

    // ── event + callback emission ───────────────────────────────────────────
    void PackStr(const PpbTouchContact& c, char* out, size_t cap)
    {
        const char* kind = SourceKindName(c.sourceKind);
        // The shipped format is FOUR fields — consumers are told they may split on the first
        // three '|'. The sub-region is therefore an OPT-IN fifth field, off by default, so
        // nobody's parser changes under them. Native/Papyrus consumers get it regardless.
        const char* sub = ObjectHold::ApiSubRegionInEvent() ? SubRegionLabel(c.subRegion) : nullptr;
        if (c.sourceKind == PPBAPI::kSourceWeapon || c.sourceKind == PPBAPI::kSourceObject)
            std::snprintf(out, cap, "%s|%s:%s|%s|%s%s%s", c.wand ? "L" : "R", kind,
                          c.sourceName, c.bodyPart, c.skeleton,
                          sub ? "|" : "", sub ? sub : "");
        else
            std::snprintf(out, cap, "%s|%s|%s|%s%s%s", c.wand ? "L" : "R", kind,
                          c.bodyPart, c.skeleton, sub ? "|" : "", sub ? sub : "");
    }

    // raw = the verbose per-capsule stream; !raw = the grouped digest stream.
    void Emit(const PpbTouchContact& c, int phase, bool raw)
    {
        // Native callbacks receive the DIGEST only — a C++ consumer wanting everything can
        // poll GetRawContacts(). Keeps callback traffic to the meaningful events.
        if (!raw)
            for (int i = 0; i < g_cbN; ++i)
                if (g_cbs[i]) g_cbs[i](&c, phase);
        const bool evOn = raw ? ObjectHold::ApiRawEventsEnabled() : ObjectHold::ApiEventsEnabled();
        if (evOn) {
            RE::TESForm* sender = RE::TESForm::LookupByID(c.actorFormId);
            char packed[192];
            PackStr(c, packed, sizeof packed);
            const char* name =
                raw ? (phase == PPBAPI::kPhaseStart ? "PPB_TouchRawStart"
                     : phase == PPBAPI::kPhaseEnd   ? "PPB_TouchRawEnd" : "PPB_TouchRaw")
                    : (phase == PPBAPI::kPhaseStart ? "PPB_TouchStart"
                     : phase == PPBAPI::kPhaseEnd   ? "PPB_TouchEnd" : "PPB_Touch");
            const float num = phase == PPBAPI::kPhaseEnd ? c.durationS : c.distU;
            SKSE::ModCallbackEvent ev{ name, packed, num, sender };
            SKSE::GetModCallbackEventSource()->SendEvent(&ev);
        }
        if (ObjectHold::ApiLogEnabled() && phase != PPBAPI::kPhaseContinue && !raw) {
            char packed[192];
            PackStr(c, packed, sizeof packed);
            // curl = the FIST calibration receipt: index-tip-to-palm distance for the hand
            // this contact rode. Do one open-hand touch and one fist touch, read the two
            // numbers, set apiFistTipPalmU between them. -1 = boxes not live.
            // src= says WHICH PATH produced this contact — ENG (Havok's own narrowphase: exact
            // capsule + real separating distance) or GEO (the geometric fallback). Printed on
            // EVERY line, not as a capped one-shot: the receipt cap exhausted on the first burst
            // and left "did the engine path drive this?" unanswerable. Per-line beats capped.
            // wpn= is the weapon CLASS and EDGE for weapon contacts (blank otherwise).
            char wpn[48] = "";
            if (c.sourceKind == PPBAPI::kSourceWeapon && c.weaponClass)
                std::snprintf(wpn, sizeof wpn, " wpn=%s/%s",
                              WeaponClassLabel(c.weaponClass),
                              c.weaponEdge == PPBAPI::kEdgeBlade  ? "Blade"  :
                              c.weaponEdge == PPBAPI::kEdgeBlunt  ? "Blunt"  :
                              c.weaponEdge == PPBAPI::kEdgePierce ? "Pierce" : "?");
            logger::info("API {} {:08X} {} d={:.2f}u dur={:.2f}s src={}{} curl={:.1f}u "
                         "vrik=I{:.2f}/M{:.2f}",
                         phase == PPBAPI::kPhaseStart ? "START" : "END",
                         c.actorFormId, packed, c.distU, c.durationS,
                         c.engineContact ? "ENG" : "GEO", wpn,
                         g_hp[c.wand & 1].curlDistU,
                         g_hp[c.wand & 1].vrikIndex, g_hp[c.wand & 1].vrikMiddle);
        }
    }

    void PublishSnapshot()
    {
        const int rn = (g_snapRawActive.load(std::memory_order_relaxed) + 1) & 3;
        Snapshot& r = g_snapRaw[rn];
        r.n = 0;
        for (const Contact& ct : g_contacts)
            if (ct.live && ct.emitted && r.n < kMaxContacts) r.c[r.n++] = ct.pub;
        g_snapRawActive.store(rn, std::memory_order_release);

        const int dn = (g_snapActive.load(std::memory_order_relaxed) + 1) & 3;
        Snapshot& d = g_snap[dn];
        d.n = 0;
        for (const DigestContact& dc : g_digest)
            if (dc.live && dc.emitted && d.n < kMaxContacts) d.c[d.n++] = dc.pub;
        g_snapActive.store(dn, std::memory_order_release);
    }

}  // namespace

namespace PpbApi {

    void NoteDriven(RE::Actor* actor)
    {
        if (!actor || g_rosterN >= kMaxRoster) return;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return;
        const auto pp = pl->GetPosition(), ap = actor->GetPosition();
        const float dx = ap.x-pp.x, dy = ap.y-pp.y, dz = ap.z-pp.z;
        const float d2 = dx*dx + dy*dy + dz*dz;
        const float range = ObjectHold::ApiRangeU();
        if (range > 0.f && d2 > range * range) return;
        g_roster[g_rosterN++] = { actor, actor->GetFormID(), d2 };
    }

    void OnFrame()
    {
        // consume-and-clear the roster even when throttled/off — the pointers are
        // same-frame-only and must never survive into a later frame.
        RosterEntry roster[kMaxRoster];
        int rosterN = g_rosterN;
        std::memcpy(roster, g_roster, sizeof(RosterEntry) * (size_t)rosterN);
        g_rosterN = 0;

        if (!ObjectHold::ApiTouchEnabled()) return;
        const std::uint64_t now = NowMs();
        const float hz = ObjectHold::ApiHz();
        if (now - g_lastTickMs < (std::uint64_t)(1000.f / (hz < 1.f ? 1.f : hz))) return;
        g_lastTickMs = now;

        // nearest-first, capped at apiMaxActors
        const int maxA = ObjectHold::ApiMaxActors();
        for (int i = 1; i < rosterN; ++i)                          // insertion sort, n ≤ 8
            for (int j = i; j > 0 && roster[j].d2 < roster[j-1].d2; --j)
                std::swap(roster[j], roster[j-1]);
        if (maxA > 0 && rosterN > maxA) rosterN = maxA;

        CollectProbes();
        CollectEngineWeaponHits(rosterN);   // real Havok weapon contacts, resolved to slot+child

        for (Contact& ct : g_contacts) ct.seen = false;
        const float touchU = ObjectHold::ApiTouchU();
        const float exitU  = touchU + ObjectHold::ApiExitPadU();

        for (int ai = 0; ai < rosterN; ++ai) {
            RE::Actor* actor = roster[ai].actor;
            const char* skel = SkeletonOf(actor);
            if (!skel) continue;                                   // not driven — not covered
            Hit hits[2][kClsCount]{};
            ScanActor(actor, hits);

            for (int hand = 0; hand < 2; ++hand) {
                for (int cls = 0; cls < kClsCount; ++cls) {
                    const Hit& h = hits[hand][cls];
                    // find the existing contact for this identity
                    Contact* ex = nullptr;
                    for (Contact& ct : g_contacts)
                        if (ct.live && ct.actorId == roster[ai].id &&
                            ct.wand == hand && ct.cls == cls) { ex = &ct; break; }
                    const bool touching = h.found && h.dist <= (ex ? exitU : touchU);
                    if (!touching) continue;                       // sweep will close ex if any
                    Contact* ct = ex;
                    if (!ct) {                                     // ENTER
                        for (Contact& c : g_contacts) if (!c.live) { ct = &c; break; }
                        if (!ct) continue;                         // table full — drop newest
                        *ct = Contact{};
                        ct->live = true; ct->actorId = roster[ai].id;
                        ct->wand = (std::uint8_t)hand; ct->cls = (std::uint8_t)cls;
                        ct->startMs = now;
                    }
                    ct->seen = true;
                    // ── DWELL: the candidate part must be lingered on before it is sent ──
                    if (h.slot != ct->candSlot || h.child != ct->candChild ||
                        (h.left ? 1 : 0) != ct->candLeft) {
                        ct->candSlot = h.slot; ct->candChild = h.child;
                        ct->candLeft = h.left ? 1 : 0;
                        ct->candSinceMs = now;
                    }
                    const bool qualified =
                        (float)(now - ct->candSinceMs) / 1000.f >= DwellSFor(h.slot, h.child);

                    PpbTouchContact& p = ct->pub;
                    p.actorFormId   = roster[ai].id;
                    p.toucherFormId = 0x14;
                    p.wand = (std::uint8_t)hand;
                    p.distU = h.dist;
                    p.durationS = (float)(now - ct->startMs) / 1000.f;
                    std::snprintf(p.skeleton, sizeof p.skeleton, "%s", skel);
                    const HandProbes& hp = g_hp[hand];
                    // keep-last-nonempty on the names — same release-tick race as the digest
                    if (cls == kClsWeapon) {
                        p.sourceKind = PPBAPI::kSourceWeapon;
                        if (hp.weapon.name[0])
                            std::snprintf(p.sourceName, sizeof p.sourceName, "%s", hp.weapon.name);
                    } else if (cls == kClsObject) {
                        p.sourceKind = PPBAPI::kSourceObject;
                        if (hp.object.name[0])
                            std::snprintf(p.sourceName, sizeof p.sourceName, "%s", hp.object.name);
                    } else {
                        p.sourceKind = ClassifyHand(actor, hand, h);
                        p.sourceName[0] = '\0';
                    }
                    if (qualified) {
                        // the WHERE fields advance only to qualified parts
                        p.slot = h.slot; p.child = h.child;
                        p.leftTwin = h.left ? 1 : 0;
                        BodyPartName(h.slot, h.left, h.child, p.bodyPart, sizeof p.bodyPart);
                        StampClass(p, h.slot, h.child);
                        StampWeapon(p, hand, EngineHitFor(roster[ai].id, hand));
                        if (!ct->emitted) { ct->emitted = true; Emit(p, PPBAPI::kPhaseStart, true); }
                        else              Emit(p, PPBAPI::kPhaseContinue, true);
                    } else if (ct->emitted) {
                        // still touching, sliding over an unqualified part: keep the stream
                        // alive with the LAST QUALIFIED part in the WHERE fields
                        Emit(p, PPBAPI::kPhaseContinue, true);
                    }
                    // never emitted + never qualified = silence, by design

                    // ── DIGEST: accumulate into the (actor, wand, region) contact ──────
                    {
                        // ★ 2026-08-01 (user spec): a contact's DURATION is timed per CAPSULE
                        // GROUP (sub-region), not per capsule and not per coarse region. So a
                        // finger wandering cheek->chin->nose stays ONE contact (all "Face
                        // surface"), while sliding from the cheek INTO the mouth correctly starts
                        // a new one — the groups differ, and so does the meaning.
                        // Identity is (actor, wand, GROUP). Because the wand is part of it, two
                        // hands on the same group are two independent contacts that qualify and
                        // emit in the SAME TICK — both hands reported together, per-hand detail
                        // preserved, exactly as asked.
                        const int reg = RegionOfPart(h.slot, h.child);
                        const int sub = SubRegionOfPart(h.slot, h.child);
                        DigestContact* dc = nullptr;
                        for (DigestContact& d : g_digest)
                            if (d.live && d.actorId == roster[ai].id && d.wand == hand &&
                                d.sub == sub) { dc = &d; break; }
                        if (!dc) {
                            for (DigestContact& d : g_digest) if (!d.live) { dc = &d; break; }
                            if (dc) {
                                *dc = DigestContact{};
                                dc->live = true; dc->actorId = roster[ai].id;
                                dc->wand = (std::uint8_t)hand; dc->region = reg; dc->sub = sub;
                                dc->startMs = now;
                            }
                        }
                        if (dc) {
                            dc->seen = true;
                            const float dt = 1.f / (hz < 1.f ? 1.f : hz);   // one tick of time
                            dc->inRegionS += dt;
                            if (h.dist < dc->deepestU) dc->deepestU = h.dist;
                            // per-part accumulation — the longest-held part is the report
                            int pi = -1;
                            for (int k = 0; k < dc->nParts; ++k)
                                if (dc->parts[k].slot == h.slot && dc->parts[k].child == h.child &&
                                    dc->parts[k].left == (h.left ? 1 : 0)) { pi = k; break; }
                            if (pi < 0 && dc->nParts < kMaxPartAcc) {
                                pi = dc->nParts++;
                                dc->parts[pi] = { h.slot, h.child, (std::uint8_t)(h.left ? 1 : 0), 0.f };
                            }
                            if (pi >= 0) dc->parts[pi].secs += dt;
                            // per-source accumulation — the longest-held pose is the report,
                            // so finger->fist mid-touch does NOT restart or flip-flop
                            const std::uint8_t sk = p.sourceKind;
                            if (sk < 8) dc->srcSecs[sk] += dt;
                        }
                    }
                }
            }
        }

        // ── DIGEST emit + sweep ─────────────────────────────────────────────────────
        for (DigestContact& dc : g_digest) {
            if (!dc.live) continue;
            // resolve the report: longest-held part, longest-held source
            int bi = -1; float bs = -1.f;
            for (int k = 0; k < dc.nParts; ++k)
                if (dc.parts[k].secs > bs) { bs = dc.parts[k].secs; bi = k; }
            int bk = PPBAPI::kSourceHand; float bks = -1.f;
            for (int k = 0; k < 8; ++k) if (dc.srcSecs[k] > bks) { bks = dc.srcSecs[k]; bk = k; }
            PpbTouchContact& p = dc.pub;
            p.actorFormId = dc.actorId; p.toucherFormId = 0x14;
            p.wand = dc.wand;
            p.sourceKind = (std::uint8_t)bk;
            p.durationS = (float)(now - dc.startMs) / 1000.f;
            // skeleton: resolved from the actor — this sweep runs OUTSIDE the roster
            // loop, so the per-actor `skel` string is not in scope here.
            if (auto* da = RE::TESForm::LookupByID<RE::Actor>(dc.actorId))
                if (const char* sk2 = SkeletonOf(da))
                    std::snprintf(p.skeleton, sizeof p.skeleton, "%s", sk2);
            p.distU = dc.deepestU;              // digest reports how FAR it got, not this frame
            // carry the live source name for WEAPON/OBJECT reports.
            // KEEP-LAST-NONEMPTY (2026-07-31, re-test log): the name is read LIVE, and on the
            // release/handoff tick it is already cleared — so the End event, the one carrying
            // the final duration, printed "OBJECT:" with no name (3 instances, e.g.
            // "R|OBJECT:|Intimate(vaginal opening L) dur=4.06s"). p persists in dc.pub, so an
            // empty live name keeps the last real one instead of overwriting it.
            if (bk == PPBAPI::kSourceWeapon) {
                const char* nm = g_hp[dc.wand & 1].weapon.name;
                if (nm[0]) std::snprintf(p.sourceName, sizeof p.sourceName, "%s", nm);
            } else if (bk == PPBAPI::kSourceObject) {
                const char* nm = g_hp[dc.wand & 1].object.name;
                if (nm[0]) std::snprintf(p.sourceName, sizeof p.sourceName, "%s", nm);
            } else p.sourceName[0] = '\0';
            if (bi >= 0) {
                p.slot = dc.parts[bi].slot; p.child = dc.parts[bi].child;
                p.leftTwin = dc.parts[bi].left;
                char part[48];
                BodyPartName(p.slot, p.leftTwin != 0, p.child, part, sizeof part);
                std::snprintf(p.bodyPart, sizeof p.bodyPart, "%s(%s)",
                              RegionLabel(dc.region), part);
                // the digest's REGION is the contact's identity, not the part's — a wandering
                // touch keeps one region while the reported part moves inside it.
                StampClass(p, p.slot, p.child, dc.region);
                StampWeapon(p, dc.wand, EngineHitFor(dc.actorId, dc.wand));
            }
            const bool qual = dc.inRegionS >= DwellSForRegion(dc.region);
            if (dc.seen) {
                dc.unseen = 0;
                if (qual) {
                    if (!dc.emitted) { dc.emitted = true; Emit(p, PPBAPI::kPhaseStart, false); }
                    else              Emit(p, PPBAPI::kPhaseContinue, false);
                }
            } else if (++dc.unseen >= 2) {
                // ── EXIT GRACE (2026-07-31, measured in the re-test log): only ONE region can
                // be the winner each tick, so a hand straddling a region boundary (the crotch
                // touches Pelvis/Intimate/Leg/Waist capsules within a radius smaller than a
                // fingertip) flip-flops the winner and — at one-tick dwell — emitted an
                // End/Start ping-pong at 4 Hz (20+ events in 9 s, 14:54:15-24). Two consecutive
                // unseen ticks (~0.5 s) must pass before a region visit ENDS; a one-tick flip
                // now just re-seens the surviving contact. Cost: a genuine exit reports ~0.5 s
                // late and its End duration includes the grace. VRTE's report 16 §3.2(b)
                // predicted exactly this from the code; the log confirmed it.
                if (dc.emitted) Emit(p, PPBAPI::kPhaseEnd, false);
                dc.live = false;
            }
            dc.seen = false;
        }

        // sweep: contacts not refreshed this tick have ended (source left, actor left
        // range/roster, or actor stopped being driven)
        for (Contact& ct : g_contacts) {
            if (!ct.live || ct.seen) continue;
            ct.pub.durationS = (float)(now - ct.startMs) / 1000.f;
            if (ct.emitted) Emit(ct.pub, PPBAPI::kPhaseEnd, true);   // dwell filter: silent contacts
            ct.live = false;                                   // end silently too
        }

        PublishSnapshot();
    }

    // ── MOUTH GATE bridge (2026-07-31) ──────────────────────────────────────────────────
    // Re-emits the gate's three stages as mod events so a consumer gets the SAME verdict the
    // gate acts on, instead of inferring it from whichever capsule won the nearest race.
    //   "PPB_MouthLips"   numArg = 1 at the lips, 0 on leaving
    //   "PPB_MouthEnter"  numArg = 1 entered, 0 exited
    //   "PPB_MouthThroat" numArg = 1 reached, 0 left
    // sender = the NPC. strArg = "WAND|STAGE" (WAND = L/R, STAGE = LIPS/ENTER/THROAT).
    // Edge-triggered: exactly one event per transition, so there is no dwell and no spam.
    void EmitMouthStage(RE::Actor* actor, int stage, bool entered, int hand, float distU)
    {
        if (!actor || !ObjectHold::ApiEventsEnabled()) return;
        static const char* kName[3]  = { "PPB_MouthLips", "PPB_MouthEnter", "PPB_MouthThroat" };
        static const char* kStage[3] = { "LIPS", "ENTER", "THROAT" };
        if (stage < 0 || stage > 2) return;
        char packed[32];
        std::snprintf(packed, sizeof packed, "%s|%s", hand == 1 ? "L" : "R", kStage[stage]);
        SKSE::ModCallbackEvent ev{ kName[stage], packed, entered ? 1.f : 0.f, actor };
        SKSE::GetModCallbackEventSource()->SendEvent(&ev);
        if (ObjectHold::ApiLogEnabled())
            logger::info("API MOUTH {:08X} {} {} hand={} d={:.2f}u", actor->GetFormID(),
                         kStage[stage], entered ? "ON" : "OFF", hand == 1 ? "L" : "R", distU);
    }

    void ClearOnLoad()
    {
        for (auto& L : g_engLatch) L.live = false;   // FormIDs recycle across saves
        g_engN = 0;
        for (Contact& ct : g_contacts) ct.live = false;
        for (DigestContact& dc : g_digest) dc.live = false;
        g_rosterN = 0;
        PublishSnapshot();
    }

    // ── the native interface object ─────────────────────────────────────────
    class TouchInterfaceImpl : public PPBAPI::IPpbTouchInterface1 {
    public:
        unsigned int GetBuildNumber() override { return 10400; }   // 1.4.0
        bool IsDriven(unsigned int id) override {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            return a && SkeletonOf(a) != nullptr;
        }
        int GetSkeleton(unsigned int id, char* out, int cap) override {
            if (!out || cap < 1) return 0;
            out[0] = '\0';
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            const char* s = a ? SkeletonOf(a) : nullptr;
            if (!s) return 0;
            std::snprintf(out, (size_t)cap, "%s", s);
            return (int)std::strlen(out);
        }
        int GetContacts(PpbTouchContact* out, int max) override {
            if (!out || max < 1) return 0;
            const Snapshot& s = g_snap[g_snapActive.load(std::memory_order_acquire)];
            const int n = s.n < max ? s.n : max;
            std::memcpy(out, s.c, sizeof(PpbTouchContact) * (size_t)n);
            return n;
        }
        bool ReadCapsule(unsigned int id, int slot, bool left, int child,
                         float a[3], float b[3], float* r) override {
            auto* ac = RE::TESForm::LookupByID<RE::Actor>(id);
            return ac && GrabDiag::ReadCapsuleWorldUSide(ac, slot, left, child, a, b, r);
        }
        const char* CapsuleName(int slot, int child) override {
            return NpcFinger::PartName(slot, child);
        }
        int ChildCount(unsigned int id, int slot) override {
            auto* ac = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!ac) return -1;
            return GrabDiag::SlotLiveChildren(ac, slot);
        }
        bool AddTouchCallback(PPBAPI::PpbTouchCallback cb) override {
            if (!cb || g_cbN >= kMaxCallbacks) return false;
            g_cbs[g_cbN++] = cb;
            return true;
        }
        int GetRawContacts(PpbTouchContact* out, int max) override {
            if (!out || max < 1) return 0;
            const Snapshot& s = g_snapRaw[g_snapRawActive.load(std::memory_order_acquire)];
            const int n = s.n < max ? s.n : max;
            std::memcpy(out, s.c, sizeof(PpbTouchContact) * (size_t)n);
            return n;
        }
        int RegionOf(int slot, int child) override { return RegionOfPart(slot, child); }
        const char* RegionName(int region) override { return RegionLabel(region); }
        int SubRegionOf(int slot, int child) override { return SubRegionOfPart(slot, child); }
        const char* SubRegionName(int sub) override { return SubRegionLabel(sub); }
        int SubRegionDepth(int sub) override { return SubRegionDepthOf(sub); }
        const char* WeaponClassName(int c) override { return WeaponClassLabel(c); }
        int WeaponEdgeOf(int c) override { return WeaponEdgeOfClass(c); }
    };
    static TouchInterfaceImpl g_iface;

    static void* GetApiFunction(unsigned int revision)
    {
        return revision == 1 ? static_cast<PPBAPI::IPpbTouchInterface1*>(&g_iface) : nullptr;
    }

    void OnPluginMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg || msg->type != PPBAPI::PpbMessage::kGetTouchInterface || !msg->data) return;
        if (msg->dataLen < sizeof(PPBAPI::PpbMessage)) return;
        auto* req = static_cast<PPBAPI::PpbMessage*>(msg->data);
        req->GetApiFunction = &GetApiFunction;
        logger::info("API: touch interface handed to '{}' (revision query pattern).",
                     msg->sender ? msg->sender : "<unknown>");
    }

    // ── Papyrus natives (class PPB_Touch) — VM threads, snapshot reads only ─
    namespace {
        inline const Snapshot& Snap() { return g_snap[g_snapActive.load(std::memory_order_acquire)]; }
        inline const PpbTouchContact* At(int i) {
            const Snapshot& s = Snap();
            return (i >= 0 && i < s.n) ? &s.c[i] : nullptr;
        }
        std::int32_t N_GetContactCount(RE::StaticFunctionTag*) { return Snap().n; }
        RE::Actor* N_GetContactActor(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i);
            return c ? RE::TESForm::LookupByID<RE::Actor>(c->actorFormId) : nullptr;
        }
        RE::BSFixedString N_GetContactBodyPart(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? c->bodyPart : "";
        }
        RE::BSFixedString N_GetContactSource(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i);
            if (!c) return "";
            if (c->sourceKind == PPBAPI::kSourceWeapon || c->sourceKind == PPBAPI::kSourceObject) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%s:%s", SourceKindName(c->sourceKind), c->sourceName);
                return buf;
            }
            return SourceKindName(c->sourceKind);
        }
        RE::BSFixedString N_GetContactWand(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? (c->wand ? "L" : "R") : "";
        }
        RE::BSFixedString N_GetContactSkeleton(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? c->skeleton : "";
        }
        float N_GetContactDuration(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? c->durationS : -1.f;
        }
        float N_GetContactDistance(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? c->distU : 1e9f;
        }
        RE::BSFixedString N_GetContactPacked(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i);
            if (!c) return "";
            char packed[192];
            PackStr(*c, packed, sizeof packed);
            return packed;
        }
        // ── appended 2026-07-31: the sub-region layer ──────────────────────────────────
        RE::BSFixedString N_GetContactRegion(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? RegionLabel(c->region) : "";
        }
        RE::BSFixedString N_GetContactSubRegion(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? SubRegionLabel(c->subRegion) : "";
        }
        // 0 surface / 1 opening / 2 inside / 3 deepest — "did it go in, and how far"
        std::int32_t N_GetContactDepth(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? (std::int32_t)c->depth : -1;
        }
    }

    bool RegisterNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) return false;
        constexpr const char* k = "PPB_Touch";
        vm->RegisterFunction("GetContactCount",    k, N_GetContactCount);
        vm->RegisterFunction("GetContactActor",    k, N_GetContactActor);
        vm->RegisterFunction("GetContactBodyPart", k, N_GetContactBodyPart);
        vm->RegisterFunction("GetContactSource",   k, N_GetContactSource);
        vm->RegisterFunction("GetContactWand",     k, N_GetContactWand);
        vm->RegisterFunction("GetContactSkeleton", k, N_GetContactSkeleton);
        vm->RegisterFunction("GetContactDuration", k, N_GetContactDuration);
        vm->RegisterFunction("GetContactDistance", k, N_GetContactDistance);
        vm->RegisterFunction("GetContactPacked",   k, N_GetContactPacked);
        vm->RegisterFunction("GetContactRegion",    k, N_GetContactRegion);
        vm->RegisterFunction("GetContactSubRegion", k, N_GetContactSubRegion);
        vm->RegisterFunction("GetContactDepth",     k, N_GetContactDepth);
        logger::info("Papyrus natives registered: PPB_Touch.GetContact* (12 functions).");
        return true;
    }
}
