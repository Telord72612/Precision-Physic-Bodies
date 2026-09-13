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
#include "Orifice.h"         // Orifice::Openness / NoteMouthStage — the orifice drive
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
    constexpr int   kMaxPartAcc   = 16;   // distinct capsules remembered per group visit
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

    // ══ CAPSULE-SEGMENT vs ORIENTED BOX (2026-09-03) ════════════════════════════════════════════
    // The distance a held item's REAL collision shape is from one of her capsules. Signed: negative
    // means the capsule axis is inside the box, and the magnitude is how deep — the same convention
    // every other probe here uses, so nothing downstream needs to know which probe answered.
    //
    // Method: push the capsule segment into the box's own frame and solve segment-vs-AABB there.
    // The box rotation is orthonormal, so its inverse is its transpose — six dot products, no
    // matrix inversion. Then minimise over the segment: the unsigned distance to a convex set is
    // convex along an affine path, so a ternary search converges on the GLOBAL minimum with no
    // local-minimum risk (a plain "sample the two endpoints" misses a mid-segment contact, which
    // is exactly the case that matters — an armour edge crossing her chest).
    inline float PtAabbSdU(const float p[3], const float h[3]) {
        float d[3], o[3];
        for (int i = 0; i < 3; ++i) { d[i] = std::fabs(p[i]) - h[i]; o[i] = d[i] > 0.f ? d[i] : 0.f; }
        const float ol = std::sqrt(o[0]*o[0] + o[1]*o[1] + o[2]*o[2]);
        // outside -> true euclidean distance; inside -> the least-negative face depth
        return ol > 0.f ? ol : (std::max)(d[0], (std::max)(d[1], d[2]));
    }
    inline float SegObbDistU(const float a[3], const float b[3],
                             const float c[3], const float R[9], const float h[3])
    {
        // world -> box-local. R's ROWS are the box axes, so the transpose is an axis-wise dot.
        float la[3], lb[3];
        const float da[3] = { a[0]-c[0], a[1]-c[1], a[2]-c[2] };
        const float db[3] = { b[0]-c[0], b[1]-c[1], b[2]-c[2] };
        for (int i = 0; i < 3; ++i) {
            la[i] = R[0*3+i]*da[0] + R[1*3+i]*da[1] + R[2*3+i]*da[2];
            lb[i] = R[0*3+i]*db[0] + R[1*3+i]*db[1] + R[2*3+i]*db[2];
        }
        const auto at = [&](float t, float o[3]) {
            for (int i = 0; i < 3; ++i) o[i] = la[i] + (lb[i] - la[i]) * t;
        };
        // unsigned distance, the quantity that is convex in t (the signed one is not)
        const auto gU = [&](float t) {
            float p[3]; at(t, p);
            float o[3];
            for (int i = 0; i < 3; ++i) { const float d = std::fabs(p[i]) - h[i]; o[i] = d > 0.f ? d : 0.f; }
            return std::sqrt(o[0]*o[0] + o[1]*o[1] + o[2]*o[2]);
        };
        float lo = 0.f, hi = 1.f;
        for (int k = 0; k < 24; ++k) {                 // ~1e-7 on [0,1]
            const float m1 = lo + (hi - lo) / 3.f, m2 = hi - (hi - lo) / 3.f;
            if (gU(m1) <= gU(m2)) hi = m2; else lo = m1;
        }
        float p[3]; at((lo + hi) * 0.5f, p);
        float best = PtAabbSdU(p, h);
        // ⚠ Penetration guard: once the axis is inside, gU is flat at 0 over an interval and the
        // ternary search lands anywhere in it, so the reported DEPTH would be arbitrary. Sampling
        // the ends and the middle makes the depth honest and monotone. Cheap: three evaluations.
        const float ts[3] = { 0.f, 0.5f, 1.f };
        for (float t : ts) { at(t, p); const float s = PtAabbSdU(p, h); if (s < best) best = s; }
        return best;
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
    // 2026-08-23: kClsGenital joins as a 4th source — the player's own genitals are simply
    // one more thing that can touch, scanned and reported exactly like hand/weapon/object.
    // 2026-09-03: kClsHead joins as a 5th source — the player's own head is simply one more
    // thing that can touch, scanned and reported exactly like hand/weapon/object/genital.
    enum SourceClass : int { kClsHand = 0, kClsWeapon = 1, kClsObject = 2, kClsGenital = 3,
                             kClsHead = 4, kClsCount = 5 };
    struct Probe {
        bool  live = false;
        bool  seg  = false;  // true: the probe is the SEGMENT p->q (weapon blade axis)
        float p[3]{};        // probe point (or segment start), world game units
        float q[3]{};        // segment end (seg only)
        float pad  = 0.f;    // extra surface (object bound radius / weapon capsule radius)
        char  name[48]{};    // weapon/object base name
        // ★ 2026-09-03: the held object's REAL collision box. When set, this OUTRANKS both seg
        // and point — it is the only description that is honest about a big item, and `pad` is
        // forced to 0 because a true box needs no inflation.
        bool  box  = false;
        float bc[3]{};       // box centre, world game units
        float bR[9]{};       // row-major world rotation of the box frame
        float bh[3]{};       // half extents along bR's rows
    };
    // ★ 2026-09-06: FIVE hand probes. 0/1 = index proximal + distal TIPs, 2 = the 3-finger base
    // slab, 3 = the 3-finger tip plate, 4 = HIGGS's OWN HAND BOX — the palm. User in VR: "HIGGS's
    // box IS the palm, it always has been; all our fingers are just extensions of it." It is the
    // collider that physically pushes her when the hand is open or fisted and it was never in this
    // list: the 19:56 log has her trunk bent 10–19u with ZERO contacts. The CLASS is still VRIK's
    // hand shape (ClassifyHand); the palm box only adds a LOCATION the class was blind to.
    static constexpr int kSlabBox  = 4;
    static constexpr int kHandBoxN = 5;
    struct HandProbes {
        Probe boxes[kHandBoxN];   // box 4 is a BOX probe (bc/bR/bh); 0..3 are points
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
        int lastViaBox = -1;    // diagnostic: which of the 5 boxes led this hand's latest hit (apiLog hv=)
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
        // ★ 2026-08-23 REWRITE — the raw stream now matches the DIGEST and the published
        // contract. It used to key the candidate on the EXACT capsule (slot+child+left) and
        // RESET candSinceMs whenever the nearest flipped. In a dense cluster (the intimate
        // ladder is 11 capsules; the chest carries ring + breast supports + shoulder caps)
        // the nearest flickers every tick, so the timer never survived its 0.25 s, `emitted`
        // never latched, and the contact was excluded from GetRawContacts() AND from every
        // raw event — silently, because the only log on this path is gated `!raw`. Two real
        // 7-second contacts (male chest ring, CLITORIS) reached VRTE as nothing at all.
        // INTEGRATION.md always described the right behaviour — "timed per CAPSULE GROUP,
        // not per capsule and not per coarse region" — it just was never implemented here;
        // the accumulate-and-pick-winner logic was written for the digest and never
        // back-ported. Now: the GROUP (sub-region) is the dwell identity, per-capsule
        // seconds accumulate, and the capsule with the most time is what gets reported.
        int           candSub = -1;             // capsule GROUP being lingered on (sub-region)
        std::uint64_t candSinceMs = 0;
        struct RawPart { int slot, child; std::uint8_t left; float secs; };
        RawPart       parts[kMaxPartAcc]{};     // per-capsule dwell inside the current group
        int           nParts = 0;
        std::uint64_t lastMs = 0;               // for this contact's own dt
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

    struct DigestContact {
        bool          live = false, seen = false, emitted = false;
        std::uint8_t  unseen = 0;      // consecutive ticks the region was not the winner
        std::uint32_t actorId = 0;
        std::uint8_t  wand = 0;
        // ★ THE BODY-PART SOURCES ARE THEIR OWN IDENTITY. `wand` is 0/1 for the two hands and
        // MEANINGLESS for GENITAL and HEAD, which both park in row 0 — so without this a kiss on
        // her cheek and the player's RIGHT HAND on her cheek folded into ONE digest contact and
        // the longest-held source won, silently hiding the other. Hands keep merging across
        // poses (finger -> fist must not restart a contact, which is the documented behaviour);
        // only the not-a-hand sources get their own lane.
        std::uint8_t  srcLane = 0;  // 0 = hands, else the SourceKind of a body-part source
        int           region = 0;   // for the report string + the dwell CLASS
        int           sub = 0;      // ★ the CAPSULE GROUP — this is the contact's IDENTITY
        std::uint64_t startMs = 0;
        float         inRegionS = 0.f;             // accumulated time, the dwell gate
        struct PartAcc { int slot, child; std::uint8_t left; float secs; };
        PartAcc       parts[kMaxPartAcc]{};
        int           nParts = 0;
        // ⛔ SIZED BY THE ENUM, NEVER BY A LITERAL. This was `srcSecs[8]` when kSourceHead was
        // appended as value 8: the `sk < 8` guard silently dropped every head contact, the
        // all-zero table then resolved to index 0, and the DIGEST published a face contact as
        // kSourceFinger on wand 0 — the wrong hand, the wrong body part, to every consumer.
        // Failure class 1 (a filter encoding a stale assumption turns a positive into a
        // confident WRONG answer). Any future source must extend kSourceCount, not this array.
        float         srcSecs[PPBAPI::kSourceCount]{};   // accumulated seconds per SourceKind
        char          genPart[8]{};                // "shaft"/"tip" (GENITAL) or "face"/"head"
                                                   // (HEAD): which part of the toucher touched
                                                   // of him. Unlike weapon/object there is no live
                                                   // name to re-read at digest time, so it rides here.
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
    // 2026-09-13: see PpbApi::IsExcludedActor (the public wrapper) — children and mannequins.
    bool ChildOrMannequin(RE::Actor* a)
    {
        auto* base = a ? a->GetActorBase() : nullptr;
        auto* race = base ? base->GetRace() : nullptr;
        if (!race) return false;
        if (race->IsChildRace()) return true;
        static RE::TESRace* s_manakin = nullptr;
        static bool         s_looked  = false;
        if (!s_looked) { s_looked = true; s_manakin = RE::TESForm::LookupByEditorID<RE::TESRace>("ManakinRace"); }
        return s_manakin && race == s_manakin;
    }

    const char* SkeletonOf(RE::Actor* a)
    {
        if (!a || DismemberGuard::IsExcluded(a)) return nullptr;
        if (ChildOrMannequin(a)) return nullptr;   // ★★★ 2026-09-13: never a touch-API actor, whatever its skeleton
        auto* base = a->GetActorBase();
        auto* race = base ? base->GetRace() : nullptr;
        if (!base || !race) return nullptr;
        // ★ 2026-08-22 MALE UNLOCK (user: "if I touch ANY capsule I want you to tell me exactly
        // what capsule it is"). The old gate was `!IsFemale() -> nullptr`, a leftover from before
        // male geometry existed; it made every male touch invisible to the API even while the
        // capsules collided fine and TOUCHPROBE saw them. Read the actor's OWN-SEX skeleton (the
        // SculptAllows lesson: a male's female-slot path is a different file, so the female slot
        // never matches him) and let anything living under \PPB\ through. Males only reach that
        // path when an ini section deliberately points them at a PPB skeleton, so no vanilla male
        // is affected. Part names already route per-sex via PartName(..., isMale).
        const char* mdl = race->skeletonModels[base->IsFemale() ? RE::SEXES::kFemale
                                                                : RE::SEXES::kMale].GetModel();
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
        case PPBAPI::kSourceGenital:return "GENITAL";
        case PPBAPI::kSourceHead:   return "HEAD";
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
        // GEN -> Intimate, the region the API already reserves for "not just a hip touch".
        if (slot == PPBAPI::kSlotGen)  return PPBAPI::kRegionIntimate;
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
    // isMale (2026-08-23, VRTE change request §3): the COM sensor ladder is SEX-SPECIFIC. The
    // male COM carries anal cover / anus / rectum at C21..C26 (MalePartNameOverride is the
    // authority and already named them right); the female map was being applied to it by index,
    // so a male anal touch published subRegion=kSubVaginal* and — the part that actually lies —
    // orificeKind = 1 (VAGINAL) on an actor with no vaginal chain. Only the NAME had been fixed.
    int SubRegionOfPart(int slot, int child, bool isMale = false)
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
            if (isMale) {
                switch (child) {
                case 0: case 1: case 2: return PPBAPI::kSubOrificeRing;  // NOT the sensor chain
                case 16: case 17:       return PPBAPI::kSubGlute;
                // C21/C22 "anal cover R/L" — external, NOT inside anything, so it must map to a
                // non-orifice sub (orificeKind 0). Mirrors the female C21 exactly, which is why
                // kSubIntimateExternal is used here rather than VRTE's suggested kSubGlute: the
                // glutes are C16/C17 on both sexes and the cover is not one.
                case 21: case 22:       return PPBAPI::kSubIntimateExternal;
                case 23: case 24:       return PPBAPI::kSubAnalOpening;   // "anus R/L"
                case 25: case 26:       return PPBAPI::kSubAnalDeep;      // "rectum R/L"
                default:                return PPBAPI::kSubPelvis;        // C27+ are deleted
                }
            }
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
        // ⛔ 2026-09-12: WEAPON contacts only. EngineHitFor keys on (actor, wand) alone, so a FINGER
        // or HEAD contact on wand 0 inherited engineContact = 1 whenever the right weapon had a
        // Havok hit on the same actor in the last 3 ticks — false provenance (master reference
        // defect A2/B2, and it now reached the kiss). Fixed at this one chokepoint for every source.
        // Removing a FALSE flag is a bug fix, not a contract change.
        p.engineContact = (fromEngine && p.sourceKind == PPBAPI::kSourceWeapon) ? 1 : 0;
    }

    // Which orifice a sub-region belongs to (the published orificeKind byte). Derived from the
    // SUB-REGION, not from raw child indices, so it stays right if the ladder is ever re-baked.
    inline int OrificeKindOfSub(int sub)
    {
        switch (sub) {
        case PPBAPI::kSubVaginalOpening:
        case PPBAPI::kSubVaginalDeep:
        case PPBAPI::kSubVaginalDeepest:  return 1;
        case PPBAPI::kSubAnalOpening:
        case PPBAPI::kSubAnalDeep:        return 2;
        case PPBAPI::kSubMouthOpening:
        case PPBAPI::kSubInMouth:
        case PPBAPI::kSubInMouthDeep:
        case PPBAPI::kSubMouthWall:       return 3;
        }
        return 0;
    }

    // ★ 2026-08-23 (VRTE change request §1) — ERECTION LEVEL on the contact, so a consumer can
    // narrate "his semi-hard penis" without guessing. Encoding is VRTE's, chosen so that the
    // reserved tail's contractual ZERO already means "absent":
    //     0 = absent (older PPB / female / no GEN rig)   ·   1 = level 0 flaccid   ·   N = level N-1
    // Reads what PPB is ASSERTING right now (FingerRig::genLevel), which is the same value that
    // drives SOSBend — not an arousal guess. Stamped on the TOUCHED actor, any source.
    inline void StampGenLevel(PpbTouchContact& p, RE::Actor* touched)
    {
        // ALWAYS write, including the 0. The digest's `pub` persists across frames, so a
        // write-only-when-present form would leave the last level frozen on the record after the
        // rig went away (he dressed, or the budget dropped it) — the stalest possible lie, and
        // exactly the keep-last-nonempty trap this file has already been bitten by twice.
        const int lv = touched ? NpcFinger::GenLevelOf(touched->GetFormID()) : -1;
        p._reserved[0] = (lv < 0) ? 0
                                  : (unsigned char)(lv + 1 > 255 ? 255 : lv + 1);
    }

    inline void StampClass(PpbTouchContact& p, int slot, int child, int regionOverride = -1,
                           bool isMale = false)
    {
        const int sub = SubRegionOfPart(slot, child, isMale);
        p.region    = (unsigned char)(regionOverride >= 0 ? regionOverride
                                                          : RegionOfPart(slot, child));
        p.subRegion = (unsigned char)sub;
        p.depth     = (unsigned char)SubRegionDepthOf(sub);
        // ORIFICE OPENNESS (2026-08-19). Both StampClass call sites set actorFormId first, so
        // this is the one place the openness can never disagree with the reported part.
        const int ok = OrificeKindOfSub(sub);
        p.orificeKind = (unsigned char)ok;
        if (ok) {
            // kind 1/2/3 -> Orifice::kVaginal/kAnal/kOral (0/1/2)
            float o = Orifice::Openness(p.actorFormId, ok - 1);
            if (!(o > 0.f)) o = 0.f;                 // NaN-safe before the cast (house rule)
            if (o > 1.f)    o = 1.f;
            p.orificeOpen = (unsigned char)(o * 255.f + 0.5f);
        } else {
            p.orificeOpen = 0;
        }
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

    // The male body carries a DIFFERENT capsule->meaning map on COM and spine1 (see
    // NpcFinger::MalePartNameOverride). Null actor = treat as female (the shared table).
    inline bool IsMaleActor(RE::Actor* a)
    {
        auto* base = a ? a->GetActorBase() : nullptr;
        return base && !base->IsFemale();
    }

    // BODYPART string: named children get the map name (side-prefixed on sided slots);
    // unnamed fall back to "<slot>.C<n>" so a touch is NEVER silently dropped.
    void BodyPartName(int slot, bool left, int child, char* out, size_t cap,
                      bool isMale = false, bool isBeast = false)
    {
        // Garment pseudo-slots: WHERE = the region along the chain, not a map name.
        if (slot == PPBAPI::kSlotGen) {
            // 4 chords over Gen01..Gen06, base -> tip.
            static const char* kSeg[4] = { "shaft (base)", "shaft (lower)",
                                           "shaft (upper)", "shaft (tip)" };
            std::snprintf(out, cap, "%s", (child >= 0 && child < 4) ? kSeg[child] : "shaft");
            return;
        }
        if (slot == PPBAPI::kSlotTail || slot == PPBAPI::kSlotHair) {
            if (slot == PPBAPI::kSlotHair) { std::snprintf(out, cap, "hair"); return; }
            const int n = g_garmentChordN[0] > 0 ? g_garmentChordN[0] : 1;
            const char* seg = child * 3 < n ? "base" : (child * 3 < n * 2 ? "mid" : "tip");
            std::snprintf(out, cap, "tail (%s)", seg);
            return;
        }
        const char* nm = NpcFinger::PartName(slot, child, isMale, isBeast);
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
        // -- SHEATHED-WEAPON GATE (2026-08-08, user-reported phantom) --------------------
        // HIGGS keeps its weapon rigid body ALIVE when the weapon is sheathed (it only turns
        // the body's collision off -- which is why the NPC never moved). Our probe had no
        // drawn-state check at all, so a VRIK-holstered weapon kept registering WEAPON
        // contacts, and the FSMP push channel visibly shoved wig chords from a weapon that
        // was on the player's hip. HIGGS's own weapon collision is gated on exactly this
        // state (hand.cpp:863) -- we copied the probe but not the gate.
        // -- WPNSTATE receipt (2026-08-08, the stance-change ghost) -----------------------
        // The stance-change reproduction proved a REAL Havok weapon contact (src=ENG) but the
        // log could not say what the drawn flag read AT that moment, nor where the weapon body
        // sat relative to the hand. Guessing has missed twice on this bug; measure instead.
        // ~2 Hz while apiLog is on: the drawn flag, HIGGS's own collision-disabled state, and
        // the weapon-body-to-palm distance per hand. One stance flip in front of the log picks
        // the correct gate (flag vs position vs HIGGS DisableWeaponCollision).
        if (ObjectHold::ApiLogEnabled() && hig) {
            static std::uint64_t s_wpnLog = 0;
            const std::uint64_t nowMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (nowMs - s_wpnLog > 500) {
                s_wpnLog = nowMs;
                auto* plS = RE::PlayerCharacter::GetSingleton();
                const bool drawnNow = plS && plS->AsActorState() && plS->AsActorState()->IsWeaponDrawn();
                float dR = -1.f, dL = -1.f;
                for (int h2 = 0; h2 < 2; ++h2) {
                    float wp[3], wq[3], pad;
                    if (!GrabDiag::WeaponSegmentU(h2 == 1, wp, wq, &pad)) continue;
                    float palm[3];
                    if (!HandBox::BoxCenterWorldU(h2, 3, palm)) continue;
                    const float dx = wp[0]-palm[0], dy = wp[1]-palm[1], dz = wp[2]-palm[2];
                    (h2 ? dL : dR) = std::sqrt(dx*dx + dy*dy + dz*dz);
                }
                logger::info("WPNSTATE drawn={} higgsColDis(R/L)={}/{} hilt-to-palm R={:.1f}u L={:.1f}u",
                             drawnNow ? 1 : 0,
                             hig->IsWeaponCollisionDisabled(false) ? 1 : 0,
                             hig->IsWeaponCollisionDisabled(true)  ? 1 : 0,
                             dR, dL);
            }
        }
        bool wpnDrawn = true;
        // -- SHEATHED-WEAPON COLLISION SHUTOFF (2026-08-08, measured) ---------------------
        // WPNSTATE telemetry through live stance flips proved all three facts at once:
        // the drawn flag tracks the stance HONESTLY (0/1 crisp on every toggle); HIGGS's
        // weapon collision body NEVER leaves the controller (hilt-to-palm ~13u while
        // sheathed -- not on the hip); and nothing ever disables its collision
        // (higgsColDis stayed 0/0 throughout). That body is the "ghost sword" physically
        // shoving hair while the hand is visibly empty. HIGGS's public API exists for
        // exactly this arbitration (slots 13-15, same calls Physical Collision VR uses),
        // so: drawn=0 -> DisableWeaponCollision, drawn=1 -> Enable. Paired with our own
        // flag so we never fight another mod's disable and only ever re-enable what WE
        // turned off.
        if (ObjectHold::ApiWeaponDrawnOnly()) {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            wpnDrawn = pl && pl->AsActorState() && pl->AsActorState()->IsWeaponDrawn();
        }
        {
            static bool s_weDisabled[2] = { false, false };
            for (int h = 0; h < 2; ++h) {
                const bool left = h == 1;
                if (!hig) break;
                if (!wpnDrawn && ObjectHold::WeaponSheathedColOff() && !s_weDisabled[h]) {
                    if (!hig->IsWeaponCollisionDisabled(left)) {
                        hig->DisableWeaponCollision(left);
                        s_weDisabled[h] = true;
                        logger::info("WPNSTATE {} weapon collision OFF (sheathed) via HIGGS API",
                                     left ? "L" : "R");
                    }
                } else if ((wpnDrawn || !ObjectHold::WeaponSheathedColOff()) && s_weDisabled[h]) {
                    hig->EnableWeaponCollision(left);
                    s_weDisabled[h] = false;
                    logger::info("WPNSTATE {} weapon collision restored (drawn) via HIGGS API",
                                 left ? "L" : "R");
                }
            }
        }
        // feed the pair-rejection filter EVERY tick (identity is needed precisely when
        // sheathed; the nullptr publish below only gates ENG attribution, not this)
        if (hig)
            NpcFinger::NoteWeaponBodies(hkpOf(hig->GetWeaponRigidBody(false)),
                                        hkpOf(hig->GetWeaponRigidBody(true)), !wpnDrawn);
        if (hig && wpnDrawn)
                 Diag::PublishWeaponBodies(hkpOf(hig->GetWeaponRigidBody(false)),
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
            // ★ PROBE 5 — HIGGS's OWN HAND BOX, THE PALM (2026-09-06). Read from HIGGS's body each
            // frame (no new body, nothing created) and ranked as a BOX exactly like the held object.
            {
                Probe& sl = hp.boxes[kSlabBox];
                GrabDiag::ObjBoxU ob{};
                if (ObjectHold::ApiPalmProbeOn() && GrabDiag::HandSlabBoxU(hand == 1, ob)) {
                    sl.box = true; sl.pad = 0.f;
                    for (int i = 0; i < 3; ++i) { sl.bc[i] = ob.c[i]; sl.bh[i] = ob.h[i]; sl.p[i] = ob.c[i]; }
                    for (int i = 0; i < 9; ++i) sl.bR[i] = ob.R[i];
                    sl.live = true;
                }
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
            // ★ A POKE IS THE TIP ONLY (user ruling 2026-09-06): in the FINGER pose the curled fingers'
            // boxes and the palm box are muted — "it's usually for orifice or stuff, other contact
            // could create issue". VRIK must POSITIVELY report the pose (index open, middle closed);
            // with VRIK absent nothing is muted here (the class falls back to geometry anyway).
            if (hp.vrikIndex > 0.55f && hp.vrikMiddle >= 0.f && hp.vrikMiddle < 0.45f)
                hp.boxes[2].live = hp.boxes[3].live = hp.boxes[kSlabBox].live = false;
            // Weapon = the blade SEGMENT (hilt->tip + radius) read off HIGGS's weapon body.
            // The first probe was the body POSITION, which sits at the HILT — prodding with
            // the blade tip never registered (user-verified miss, 2026-07-30). The point
            // fallback inside WeaponSegmentU covers unreadable shapes; its one-shot log
            // names the shape actually carried.
            if (wpnDrawn && GrabDiag::WeaponSegmentU(isLeft, hp.weapon.p, hp.weapon.q, &hp.weapon.pad)) {
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
                        // ── SEGMENT FIRST (2026-08-19) ──────────────────────────────────
                        // The sphere below cannot express an INSERTION DEPTH: for a 3x3x18u
                        // DD plug worldBound.radius is ~9u, so it reads "touching" from a
                        // hand's width away and every depth-consuming consumer is misled
                        // (VRTE report 23 s7.6). A long thin item must carry an AXIS. Round
                        // items fail the aspect test inside and keep the sphere, which is
                        // the honest description for them.
                        // ★★ BOX FIRST (2026-09-03, user ruling). The item's REAL Havok collision
                        // shape outranks both descriptions below it, because both of them place a
                        // big item by its ORIGIN and then argue about how much to inflate it. The
                        // box is where the item actually is. The segment is still filled in
                        // alongside it (long axis of the box) so `CopyProbes` consumers — the
                        // orifice drive reads p/q — keep working with no contract change.
                        GrabDiag::ObjBoxU ob{};
                        if (ObjectHold::ApiObjectBoxOn() && GrabDiag::ObjectBoxU(isLeft, ob)) {
                            hp.object.box = true;
                            hp.object.pad = 0.f;              // a true box needs no inflation
                            for (int i = 0; i < 3; ++i) { hp.object.bc[i] = ob.c[i]; hp.object.bh[i] = ob.h[i]; }
                            for (int i = 0; i < 9; ++i) hp.object.bR[i] = ob.R[i];
                            int ax = 0;
                            if (ob.h[1] > ob.h[ax]) ax = 1;
                            if (ob.h[2] > ob.h[ax]) ax = 2;
                            for (int i = 0; i < 3; ++i) {
                                const float d = ob.R[i*3+ax] * ob.h[ax];
                                hp.object.p[i] = ob.c[i] - d;
                                hp.object.q[i] = ob.c[i] + d;
                            }
                            hp.object.seg = true;
                        } else if (GrabDiag::ObjectSegmentU(refr, hp.object.p, hp.object.q,
                                                     &hp.object.pad)) {
                            hp.object.seg = true;
                        } else if (auto* d3 = refr->Get3D()) {
                            const auto& wb = d3->worldBound;
                            hp.object.p[0] = wb.center.x; hp.object.p[1] = wb.center.y;
                            hp.object.p[2] = wb.center.z; hp.object.pad = wb.radius;
                            // ★ v8.9 CAP THE INFLATION (2026-08-30, user-reported yoke + collar).
                            // The bound sphere of a big device is enormous - a Devious Heavy Steel
                            // Yoke measured "d=-30.59u vs head.C2". Uncapped, EVERY capsule on her
                            // body ties for closest, so the equip site is a lottery and the gesture
                            // never resolves. The weapon path already caps its radius; objects did
                            // not.
                            const float padCap = ObjectHold::ObjectPadMaxU();   // v9.2: reach only; ranking is by dRaw
                            if (padCap > 0.f && hp.object.pad > padCap) hp.object.pad = padCap;
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
                // 2026-09-06 (user): the code default is now STRICT (2) — "there is a reason the
                // player is holding the apple, it's deliberate": the held thing reports, the hand
                // does not. The 07-31 index exception survives only as knob value 1.
                hp.boxes[2].live = hp.boxes[3].live = hp.boxes[kSlabBox].live = false;   // 3-finger boxes + the palm box
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
        float rank = 1e9f;   // v9.2: TRUE surface distance, used only to pick the nearest capsule
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

    // ── ENGINE-TRUTH HAND TOUCHES (2026-09-07, report 33 §8.3) — resolved EVERY FRAME ──────────
    // The pointer -> (actor, slot, side) walk is the weapon path's, cached per body: bodies are
    // stable until a ragdoll rebuild, so one FindNode re-check per touched body per frame replaces
    // 24 x roster node walks. A cache hit that no longer matches falls back to the full search.
    struct EngTouchRec { PpbApi::EngineTouch t; bool live; };
    EngTouchRec g_engTouch[16]{};                    // v11.1b: one record per (actor, HAND) — two hands may touch one actor in a frame
    struct BodyResCache { void* body; std::uint32_t actorId; int slot; bool left; };
    BodyResCache g_bodyRes[32]{};
    int          g_bodyResN = 0;
    // v11.1b negative cache (review): a body the roster could NOT resolve — a corpse (DeadBip, never NoteDriven), an
    // excluded actor, the 9th+ NPC — is not re-walked 24 x roster times every frame; the miss holds 8 frames or until
    // the roster size changes.
    struct BodyMiss { void* body; std::uint32_t frame; int rosterN; };
    BodyMiss      g_bodyMiss[16]{};
    int           g_bodyMissN = 0;
    std::uint32_t g_engFrame  = 0;

    inline double NowSec() {
        return std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    inline bool TrunkSlotEng(int slot) { return slot == 3 || slot == 4 || slot == 5 || slot == 6 || slot == 7 || slot == 8 || slot == 11; }

    bool ResolveNpcBody(void* body, const RosterEntry* roster, int nRoster,
                        std::uint32_t& actorId, int& slot, bool& left)
    {
        for (int i = 0; i < g_bodyMissN; ++i) {                     // negative cache first
            if (g_bodyMiss[i].body != body) continue;
            if (g_engFrame - g_bodyMiss[i].frame < 8 && g_bodyMiss[i].rosterN == nRoster) return false;
            g_bodyMiss[i] = g_bodyMiss[--g_bodyMissN];              // expired or the roster changed — try again
            break;
        }
        // positive cache: validate with ONE node walk
        for (int i = 0; i < g_bodyResN; ++i) {
            if (g_bodyRes[i].body != body) continue;
            for (int ai = 0; ai < nRoster; ++ai) {
                RE::Actor* a = roster[ai].actor;
                if (!a || a->GetFormID() != g_bodyRes[i].actorId) continue;
                if (GrabDiag::SlotBodyRaw(a, g_bodyRes[i].slot, g_bodyRes[i].left) == body) {
                    actorId = g_bodyRes[i].actorId; slot = g_bodyRes[i].slot; left = g_bodyRes[i].left;
                    return true;
                }
            }
            g_bodyRes[i] = g_bodyRes[--g_bodyResN];   // stale (rebuilt ragdoll / actor gone) — drop, re-search
            break;
        }
        for (int ai = 0; ai < nRoster; ++ai) {
            RE::Actor* a = roster[ai].actor;
            if (!a) continue;
            for (int s = 0; s < 12; ++s)
                for (int side = 0; side < 2; ++side) {
                    void* b = GrabDiag::SlotBodyRaw(a, s, side != 0);
                    if (!b || b != body) continue;
                    actorId = a->GetFormID(); slot = s; left = side != 0;
                    if (g_bodyResN < 32) g_bodyRes[g_bodyResN++] = { body, actorId, slot, left };
                    else                 g_bodyRes[0] = { body, actorId, slot, left };
                    return true;
                }
        }
        if (g_bodyMissN < 16) g_bodyMiss[g_bodyMissN++] = { body, g_engFrame, nRoster };
        else                  g_bodyMiss[g_engFrame % 16] = { body, g_engFrame, nRoster };
        return false;
    }

    void CollectEngineHandTouches(const RosterEntry* roster, int nRoster)
    {
        ++g_engFrame;
        const double now = NowSec();
        for (auto& r : g_engTouch)                       // age out (2 s) — a stale entry must never anchor
            if (r.live && now - r.t.tS > 2.0) r.live = false;
        // ── v11.1b census receipt: prints when the collision thread's count moved, at most every 5 s. This is how a
        // classifier that has stopped matching is SEEN — the first build produced a whole session of `anchor api`.
        {
            static std::uint32_t s_lastPairs = 0; static double s_lastLog = -1e9;
            std::uint32_t pairs = 0, la = 0, lb = 0, stamps = 0;
            Diag::HandStampCensus(pairs, la, lb, stamps);
            if (pairs != s_lastPairs && now - s_lastLog > 5.0) {
                s_lastPairs = pairs; s_lastLog = now;
                logger::info("ENGSTAMP census: {} layer-56 contact events so far; last pair: 56-side 0x{:08X} (part {} bit15 {}) "
                             "vs 0x{:08X} (layer {} part {} bit15 {}); {} hand stamps written, roster {}",
                             pairs, la, (la >> 8) & 0x1Fu, (la & 0x8000u) ? 1 : 0,
                             lb, lb & 0x7Fu, (lb >> 8) & 0x1Fu, (lb & 0x8000u) ? 1 : 0, stamps, nRoster);
            }
        }
        Diag::HandContact raw[Diag::kHandContactRing];   // the whole ring, so max never truncates the newest
        const int n = Diag::DrainHandContacts(raw, Diag::kHandContactRing);
        if (n <= 0 || nRoster <= 0) return;
        // dedupe ACCEPTED (npcBody, hand) pairs and FAILED bodies separately (review): a stamp rejected for lack of a
        // hand must not eat a later valid stamp on the same body this frame.
        struct SeenOk { void* body; int hand; };
        SeenOk seenOk[Diag::kHandContactRing]; int nOk = 0;
        void*  seenFail[32]; int nFail = 0;
        static int s_receipts = 0;                       // the first 8 resolutions speak, then silence
        for (int i = 0; i < n; ++i) {
            if (!raw[i].npcBody) continue;
            bool skip = false;
            for (int k = 0; k < nFail; ++k) skip |= (seenFail[k] == raw[i].npcBody);
            if (skip) continue;
            int hand = -1;
            if      (raw[i].part == 3) hand = 0;
            else if (raw[i].part == 5) hand = 1;
            else                        hand = HandBox::HandOfBody(raw[i].playerBody);   // a PPB finger box; -1 = not ours
            if (hand < 0) {                                                               // fail closed: no hand, no anchor
                if (s_receipts < 8) { ++s_receipts;
                    logger::info("ENGSTAMP drop: player body 0x{:X} (part {}, src {}) is not one of our hand boxes — no hand, no anchor",
                                 (std::uintptr_t)raw[i].playerBody, raw[i].part, raw[i].src); }
                continue;
            }
            for (int k = 0; k < nOk; ++k) skip |= (seenOk[k].body == raw[i].npcBody && seenOk[k].hand == hand);
            if (skip) continue;
            std::uint32_t actorId = 0; int slot = -1; bool left = false;
            if (!ResolveNpcBody(raw[i].npcBody, roster, nRoster, actorId, slot, left)) {
                if (nFail < 32) seenFail[nFail++] = raw[i].npcBody;
                if (s_receipts < 8) { ++s_receipts;
                    logger::info("ENGSTAMP drop: NPC body 0x{:X} is not on the {}-actor roster (corpse / excluded / out of range) — no anchor",
                                 (std::uintptr_t)raw[i].npcBody, nRoster); }
                continue;
            }
            if (nOk < Diag::kHandContactRing) seenOk[nOk++] = { raw[i].npcBody, hand };
            EngTouchRec* rec = nullptr;
            for (auto& r : g_engTouch) if (r.live && r.t.actorId == actorId && r.t.hand == hand) { rec = &r; break; }
            if (!rec) for (auto& r : g_engTouch) if (!r.live) { rec = &r; break; }
            if (!rec) rec = &g_engTouch[0];
            // prefer a TRUNK stamp over a non-trunk one from the SAME frame (a hand cupping the shoulder while pressing
            // the chest must not hide the chest from pushStepAnchorTrunkOnly — review)
            if (rec->live && rec->t.tS == now && TrunkSlotEng(rec->t.slot) && !TrunkSlotEng(slot)) continue;
            rec->t = { actorId, slot, (int)raw[i].child, left, hand, raw[i].src, now };
            rec->live = true;
            if (s_receipts < 8) { ++s_receipts;
                logger::info("ENGSTAMP ok: actor {:08X} slot {}{} child {} <- hand {} ({}) — the engine saw the contact; PushStep anchors on it",
                             actorId, slot, left ? "L" : "", (int)raw[i].child, hand ? "L" : "R",
                             raw[i].src == 1 ? "weapon" : raw[i].src == 2 ? "finger box" : "HIGGS hand"); }
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
        // ── BREAST TOUCH PAD (2026-09-03, VRTE_API_Change_Request_BreastTouchReach) ──────────
        // The breast radius model saturates at lmBrCupSat 10.40 while the flesh does not: the
        // captured zero-slider neutrals are CBBE 9.97 / 3BA 9.63, so the clamp sits ~4% above the
        // BASE BODY and every preset larger than that gets the same ~3.15u capsule against a
        // mound that can be twice as deep. The finger therefore has to sink visibly into flesh
        // before the surface test fires — and on a light cupping touch it may never fire at all.
        //   This adds the missing reach on the CAPSULE side, keyed on the sub-region so it can
        // only ever affect the two breast children. ⛔ It is subtracted from the GATE distance
        // only, never from the RANK — see the branches below.
        // ⚠ FEMALES ONLY — ALL of them, and ONLY them. The defect this compensates for is a
        // property of the BREAST MODEL (r = lmBrRc + lmBrRm*cup, saturating at lmBrCupSat), and
        // that model only ever sizes a female's C11/C12. A MALE's spine2 C11/C12 are dialled
        // CHEST capsules — slot 6 has no male name override, so they are still called "BREAST
        // R/L", and without this gate every man in the game would have picked up 1.5u of extra
        // chest reach for a saturation defect that never applied to him.
        //   "All female skeletons" means exactly that: human, draenei, argonian and khajiit
        // females all resolve here. The BASE pad is unconditional for them (the capsule sits
        // ~1.5u inside the skin even on a zero-slider body — that is what ghostBreastPadU
        // measures); the SLOPE term needs a latched landmark measurement, so an unmeasured or
        // rejected actor degrades to the base pad rather than to a garbage extrapolation.
        const bool  brFemale  = !IsMaleActor(actor);
        const float brPadBase = brFemale ? ObjectHold::ApiBreastPadU() : 0.f;
        const float brCup     = brPadBase > 0.f ? GrabDiag::BreastCupOf(aid) : 0.f;
        // above the clamp the radius stopped tracking the body; restore that part outside the fit
        const float brPad     = (brPadBase <= 0.f) ? 0.f
                              : brPadBase + ObjectHold::ApiBreastPadSlope() *
                                            (std::max)(0.f, brCup - 10.40f);
        // ── PLAYER GENITAL WAND (2026-08-23): a 4th source, fetched once per actor. Empty
        // whenever he has no schlong or is dressed (the TNG slot-52 gate lives in the wand's
        // own lifecycle), so every loop below simply does nothing on those frames.
        float gwA[2][3], gwB[2][3], gwR = 0.f;
        const int nGw = HandBox::WandProbeSegments(gwA, gwB, &gwR);
        // Nearest of the wand's segments to one capsule, folded into `dst` like every other
        // source. Segment-vs-segment because both sides are rods.
        // `viaBox` carries WHICH segment won — seg 0 = shaft, seg 1 = tip — so the contact can
        // name the part of him that did the touching, the same way a weapon names itself.
        const auto wandVs = [&](const float a[3], const float b[3], float r,
                                int slotV, int chV, bool leftV, Hit& dst, float capPadV) {
            for (int s = 0; s < nGw; ++s) {
                const float dRank = SegSegDistU(a, b, gwA[s], gwB[s]) - r - gwR;
                if (dRank < dst.rank) { dst.found = true; dst.rank = dRank;
                                        dst.dist = dRank - capPadV; dst.slot = slotV;
                                        dst.child = chV; dst.left = leftV; dst.viaBox = s; }
            }
        };
        // ── PLAYER HEAD BOX (2026-09-03): a 5th source, fetched once per actor. Empty whenever
        // the head box is off, the player is beast-formed, or a scene is running (all gated in
        // the box's own lifecycle), so every loop below simply does nothing on those frames.
        // ONE segment, running back-of-skull -> face along the box's local +Y.
        float hdA[3], hdB[3], hdR = 0.f;
        const bool hasHead = HandBox::HeadProbeSegment(hdA, hdB, &hdR);
        // ★ 2026-09-06: the MOUTH — a second head-class segment (front face, below eye level). When it is
        // the nearer of the two the contact is named "mouth" (viaBox 2); the kiss finally has a name.
        float mA[3], mB[3], mR = 0.f;
        const bool hasMouth = hasHead && HandBox::MouthProbeSegment(mA, mB, &mR);
        // Segment-vs-segment for the same reason the wand uses it: both sides are rods, and the
        // FRONT of the head must be able to win on its own (that is the kiss). `viaBox` carries
        // which HALF of the segment won — 1 = the face end, 0 = the rest of the skull — so the
        // contact can name what touched without the consumer doing geometry.
        const auto headVs = [&](const float a[3], const float b[3], float r,
                                int slotV, int chV, bool leftV, Hit& dst, float capPadV) {
            if (!hasHead) return;
            // ★★ 2026-09-12: the head and mouth are CONTACT sources — they never go INSIDE her.
            // Both segments subtract their own radius (the head's ~5.5u), honest against her skin but it
            // meant a face pressed to hers could win the PALATE / THROAT race and publish "In mouth"
            // (depth 2, orificeKind 3; VRTE ranks it 75), and reach the deep pelvic chain the same way.
            // Rule = the published DEPTH LADDER, not child indices:
            //   * depth >= Inside (palate, throat wall, deep floor, cervix, uterus, rectum) — never, either segment.
            //   * pelvic OPENINGS (depth Opening on the COM: vaginal / anal opening) — the small MOUTH probe
            //     only. A mouth at her vulva is real oral contact; the fat 5.5u skull segment "reaching" an
            //     opening would be the same radius inflation this rule exists to stop.
            // ⛔ The first cut of this used `slot 11 && child >= 22` — reviewed as over-blocking: it is sex-blind
            //   (a MALE's C22 is the EXTERNAL "anal cover L", so a face on it reported his right cover), and it
            //   took the openings away from the mouth, so oral contact read as "CLITORIS" or the ring instead.
            //   SubRegionOfPart is already sex-routed, so the ladder gets both right for free.
            // ⚠ Beast heads: their C9-C11 are not palate/throat, but SubRegionOfPart is not beast-aware, so they
            //   still read as interior and are skipped — fails CLOSED (a crown touch reports the cranium).
            const int  depthV        = SubRegionDepthOf(SubRegionOfPart(slotV, chV, !brFemale));
            if (depthV >= PPBAPI::kDepthInside) return;
            const bool pelvicOpening = (slotV == 11 && depthV == PPBAPI::kDepthOpening);
            const float dRank = pelvicOpening ? 1e9f                      // the skull never scores an opening (Hit's own sentinel)
                                              : SegSegDistU(a, b, hdA, hdB) - r - hdR;
            if (dRank < dst.rank) {
                dst.found = true; dst.rank = dRank; dst.dist = dRank - capPadV; dst.slot = slotV;
                dst.child = chV; dst.left = leftV;
                // which end of OUR segment is nearer the capsule's midpoint decides the label
                const float mid[3] = { (a[0]+b[0])*0.5f, (a[1]+b[1])*0.5f, (a[2]+b[2])*0.5f };
                const float dA = (hdA[0]-mid[0])*(hdA[0]-mid[0]) + (hdA[1]-mid[1])*(hdA[1]-mid[1]) +
                                 (hdA[2]-mid[2])*(hdA[2]-mid[2]);
                const float dB = (hdB[0]-mid[0])*(hdB[0]-mid[0]) + (hdB[1]-mid[1])*(hdB[1]-mid[1]) +
                                 (hdB[2]-mid[2])*(hdB[2]-mid[2]);
                dst.viaBox = (dB < dA) ? 1 : 0;           // 1 = the face end
            }
            if (hasMouth) {
                const float dM = SegSegDistU(a, b, mA, mB) - r - mR;
                if (dM < dst.rank) {
                    dst.found = true; dst.rank = dM; dst.dist = dM - capPadV; dst.slot = slotV;
                    dst.child = chV; dst.left = leftV; dst.viaBox = 2;   // 2 = the mouth
                }
            }
        };
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
            const Probe* all[7] = { &hp.boxes[0], &hp.boxes[1], &hp.boxes[2], &hp.boxes[3],
                                    &hp.boxes[kSlabBox], &hp.weapon, &hp.object };
            for (const Probe* pr : all)
                if (pr->live && probeNear(pr, apf, kActorCullU)) { anyNear = true; break; }
        }
        // the wand is a probe too: without this the actor is culled before it can ever score
        for (int s = 0; s < nGw && !anyNear; ++s) {
            Probe pw{}; pw.live = true; pw.seg = true;
            std::memcpy(pw.p, gwA[s], sizeof pw.p); std::memcpy(pw.q, gwB[s], sizeof pw.q);
            if (probeNear(&pw, apf, kActorCullU)) anyNear = true;
        }
        // ...and so is the head. Same trap, same fix: a source absent from the cull is a source
        // that can never score, however real its collider is (the 2026-08-23 wand lesson —
        // "ask which LIST the consumer iterates").
        if (!anyNear && hasHead) {
            Probe ph{}; ph.live = true; ph.seg = true;
            std::memcpy(ph.p, hdA, sizeof ph.p); std::memcpy(ph.q, hdB, sizeof ph.q);
            if (probeNear(&ph, apf, kActorCullU)) anyNear = true;
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
                    const Probe* all[7] = { &hp.boxes[0], &hp.boxes[1], &hp.boxes[2],
                                            &hp.boxes[3], &hp.boxes[kSlabBox], &hp.weapon, &hp.object };
                    for (const Probe* pr : all) {
                        if (!pr->live) continue;
                        const float d = pr->seg ? SegSegDistU(a, b, pr->p, pr->q)
                                                : SegPointDistU(a, b, pr->p);
                        if (d < kSlotCullU) { slotNear = true; break; }
                    }
                }
                for (int s = 0; s < nGw && !slotNear; ++s) {   // wand keeps the slot alive too
                    if (SegSegDistU(a, b, gwA[s], gwB[s]) < kSlotCullU) slotNear = true;
                }
                if (!slotNear && hasHead &&                   // ...and so does the head box
                    SegSegDistU(a, b, hdA, hdB) < kSlotCullU) slotNear = true;
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
                    // ★ THE CAPSULE-SIDE PAD, GATE ONLY. kSubBreast is exactly slot 6 children
                    // 11/12 (SubRegionOfPart), so no new classification is needed and nothing
                    // else on the body can be affected. Interior capsules are excluded on
                    // principle — the v8.9 rule that a thing may only reach an orifice if its own
                    // point is genuinely in there — though breasts are slot 6 and outside the
                    // interior set by construction anyway.
                    const float capPad = (brPad > 0.f && slot == 6 && (ch == 11 || ch == 12))
                                         ? brPad : 0.f;
                    for (int hand = 0; hand < 2; ++hand) {
                        const HandProbes& hp = g_hp[hand];
                        for (int bx = 0; bx < kHandBoxN; ++bx) {
                            if (!hp.boxes[bx].live) continue;
                            // ⛔ RANK ON TRUE GEOMETRY, GATE ON REACH. The object branch has done
                            // this since v9.2; the hand branch never had a pad at all, so it had
                            // no split — and adding one naively would have let a padded breast
                            // WIN the nearest-capsule race against the sternum and rib capsules
                            // beside it, reporting "breast" for a touch on her chest. dRank is
                            // bit-identical to the old `d`, so with capPad 0 nothing changes.
                            const float dRank = (hp.boxes[bx].box      // 2026-09-06: the palm box is a BOX
                                                 ? SegObbDistU(a, b, hp.boxes[bx].bc, hp.boxes[bx].bR, hp.boxes[bx].bh)
                                                 : SegPointDistU(a, b, hp.boxes[bx].p)) - r;
                            const float dGate = dRank - capPad;
                            Hit& h = out[hand][kClsHand];
                            if (dRank < h.rank) { h.found = true; h.rank = dRank; h.dist = dGate;
                                                  h.slot = slot; h.child = ch; h.left = left;
                                                  h.viaBox = bx; }
                            if (isSensor && (!isThroat || nearPalate(hp.boxes[bx].p, nullptr))) {
                                Hit& sh = sens[hand][kClsHand];
                                if (dRank < sh.rank) { sh.found = true; sh.rank = dRank;
                                                       sh.dist = dGate; sh.slot = slot;
                                                       sh.child = ch; sh.left = left; sh.viaBox = bx; }
                            }
                        }
                        if (wpnOk[hand]) {
                            // the weapon's OWN pad stays in the rank exactly as it always has —
                            // that is shipped, tuned behaviour. Only capPad is gate-only.
                            const float dRank = (hp.weapon.seg
                                                ? SegSegDistU(a, b, hp.weapon.p, hp.weapon.q)
                                                : SegPointDistU(a, b, hp.weapon.p))
                                            - r - hp.weapon.pad;
                            const float dGate = dRank - capPad;
                            Hit& h = out[hand][kClsWeapon];
                            if (dRank < h.rank) { h.found = true; h.rank = dRank; h.dist = dGate;
                                                  h.slot = slot; h.child = ch; h.left = left; }
                            if (isSensor && (!isThroat ||
                                             nearPalate(hp.weapon.p, hp.weapon.seg ? hp.weapon.q : nullptr))) {
                                Hit& sh = sens[hand][kClsWeapon];
                                if (dRank < sh.rank) { sh.found = true; sh.rank = dRank;
                                                       sh.dist = dGate; sh.slot = slot;
                                                       sh.child = ch; sh.left = left; }
                            }
                        }
                        if (hp.object.live) {
                            // ★ v8.9 INTERIOR CAPSULES ARE NOT REACHABLE BY INFLATION (user:
                            // "i equipped a collar on her... it says it touched her palate", with
                            // the collar plainly outside her mouth). An object is inflated to its
                            // bound radius, and that sphere swallows capsules INSIDE the head. A
                            // thing can only touch the palate if its own point is genuinely in
                            // there - so interior sub-regions use the RAW point distance.
                            // Interior = past an orifice OPENING: mouth palate(9)/throat(10)/
                            // deep floor(11) on the head, and the vaginal/anal chains (child >= 22)
                            // on the COM. The lip ring (1) and the clitoris (21) are EXTERNAL
                            // landmarks and stay reachable normally.
                            const bool interior = (slot == 3 && (ch == 9 || ch == 10 || ch == 11)) ||
                                                  (slot == 11 && ch >= 22);
                            // ★ v9.2: RANK BY TRUE GEOMETRY, GATE BY REACH (2026-08-30).
                            // dRaw = the object's real surface distance; dGate = dRaw minus its
                            // reach (bound radius). Picking the nearest capsule by dGATE was the
                            // real yoke bug - a 30u sphere makes every capsule tie, so the equip
                            // site was a lottery - and capping the reach to fix THAT starved
                            // small/paired devices ('Copper Wrist Cuffs': 0 contacts for a
                            // minute). Ranking by dRaw picks the honest nearest capsule; the
                            // published distance stays dGate so a held device still reaches.
                            // ⛔ 2026-09-03: this used to be SegPointDistU ALWAYS, so the object's
                            // segment (filled since 2026-08-19) was computed and never read — a
                            // held item was ranked as a POINT AT ONE END of its own long axis.
                            // The weapon branch above always branched correctly; this one did not.
                            const float dRaw  = (hp.object.box
                                                ? SegObbDistU(a, b, hp.object.bc, hp.object.bR, hp.object.bh)
                                                : hp.object.seg
                                                ? SegSegDistU(a, b, hp.object.p, hp.object.q)
                                                : SegPointDistU(a, b, hp.object.p)) - r;
                            const float dGate = interior ? dRaw
                                                         : dRaw - hp.object.pad - capPad;
                            Hit& h = out[hand][kClsObject];
                            if (dRaw < h.rank) { h.found = true; h.rank = dRaw; h.dist = dGate;
                                                 h.slot = slot; h.child = ch; h.left = left; }
                            if (isSensor && (!isThroat || nearPalate(hp.object.p, nullptr))) {
                                Hit& sh = sens[hand][kClsObject];
                                if (dRaw < sh.rank) { sh.found = true; sh.rank = dRaw;
                                                      sh.dist = dGate; sh.slot = slot;
                                                      sh.child = ch; sh.left = left; }
                            }
                        }
                    }
                    // The wand is not per-hand, so it sits outside the hand loop and parks in
                    // row 0 (`wand` is published as 0 for this kind). Interior sensors matter
                    // more for this source than any other — it is the one that goes inside.
                    if (nGw > 0) {
                        wandVs(a, b, r, slot, ch, left, out[0][kClsGenital], capPad);
                        if (isSensor && (!isThroat || nearPalate(gwA[0], gwB[nGw - 1])))
                            wandVs(a, b, r, slot, ch, left, sens[0][kClsGenital], capPad);
                    }
                    // The head is not per-hand either, so it parks in row 0 alongside the wand
                    // (`wand` is published as 0 for this kind). It joins the SENSOR priority race
                    // for the same reason every source does — the palate/throat pair must be able
                    // to outrank the cranium — but it can never reach an interior pelvic sensor
                    // in practice, and the throat inside-test still gates it honestly.
                    if (hasHead) {
                        headVs(a, b, r, slot, ch, left, out[0][kClsHead], capPad);
                        if (isSensor && (!isThroat || nearPalate(hdA, hdB)))
                            headVs(a, b, r, slot, ch, left, sens[0][kClsHead], capPad);
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
        // kind 0 tail · 1 hair (knob-gated) · 2 GEN (always — a schlong touch is a body touch).
        // ★ 2026-08-23 FIX (found by the VRTE agent): this bound was
        //     ApiHairTarget() ? 3 : 1
        // which made `kind == 2` (GEN) UNREACHABLE at the shipped default apiHairTarget 0 —
        // the comment right above said "GEN (always)" while the loop could not reach it. Every
        // NPC schlong contact has been silently impossible since the GEN rig was added; VRTE's
        // whole male-genital path was dead code waiting on this one number. Hair is gated by the
        // `continue` on the next line and always was, so the bound only ever needed to be 3.
        const int kindMax = 3;
        for (int kind = 0; kind < kindMax; ++kind) {
            if (kind == 1 && !ObjectHold::ApiHairTarget()) continue;
            const int n = NpcFinger::GarmentChords(gid, kind);
            g_garmentChordN[kind] = n;
            for (int ch = 0; ch < n; ++ch) {
                float a[3], b[3], r;
                if (!NpcFinger::GarmentChordU(gid, kind, ch, a, b, &r)) continue;
                const int pseudo = kind == 0 ? PPBAPI::kSlotTail
                                 : kind == 1 ? PPBAPI::kSlotHair : PPBAPI::kSlotGen;
                for (int hand = 0; hand < 2; ++hand) {
                    const HandProbes& hp = g_hp[hand];
                    // ⚠ these share the SAME Hit objects as the body loop above, so they must use
                    // the same rank/gate discipline — otherwise a chord would be compared against
                    // the body loop's PADDED distance (rejecting a genuinely nearer chord) and a
                    // winning chord would leave a stale rank behind. No garment chord is a breast,
                    // so the gate equals the rank here.
                    for (int bx = 0; bx < kHandBoxN; ++bx) {
                        if (!hp.boxes[bx].live) continue;
                        const float dRank = (hp.boxes[bx].box
                                             ? SegObbDistU(a, b, hp.boxes[bx].bc, hp.boxes[bx].bR, hp.boxes[bx].bh)
                                             : SegPointDistU(a, b, hp.boxes[bx].p)) - r;
                        Hit& h = out[hand][kClsHand];
                        if (dRank < h.rank) { h.found = true; h.rank = dRank; h.dist = dRank;
                                              h.slot = pseudo; h.child = ch; h.left = false;
                                              h.viaBox = bx; }
                    }
                    if (wpnOk[hand]) {
                        const float dRank = (hp.weapon.seg
                                            ? SegSegDistU(a, b, hp.weapon.p, hp.weapon.q)
                                            : SegPointDistU(a, b, hp.weapon.p))
                                        - r - hp.weapon.pad;
                        Hit& h = out[hand][kClsWeapon];
                        if (dRank < h.rank) { h.found = true; h.rank = dRank; h.dist = dRank;
                                              h.slot = pseudo; h.child = ch; h.left = false; }
                    }
                    if (hp.object.live) {
                        // same correction as the sensor branch above — box, then segment, then point
                        const float dRaw  = (hp.object.box
                                            ? SegObbDistU(a, b, hp.object.bc, hp.object.bR, hp.object.bh)
                                            : hp.object.seg
                                            ? SegSegDistU(a, b, hp.object.p, hp.object.q)
                                            : SegPointDistU(a, b, hp.object.p)) - r;   // v9.2
                        const float dGate = dRaw - hp.object.pad;
                        Hit& h = out[hand][kClsObject];
                        if (dRaw < h.rank) { h.found = true; h.rank = dRaw; h.dist = dGate;
                                             h.slot = pseudo; h.child = ch; h.left = false; }
                    }
                }
                // tails / hair / another actor's GEN chords are touchable by the wand too
                if (nGw > 0) wandVs(a, b, r, pseudo, ch, false, out[0][kClsGenital], 0.f);
                // ...and by the head: resting your face against a tail or a wig is a real touch
                if (hasHead) headVs(a, b, r, pseudo, ch, false, out[0][kClsHead], 0.f);
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

        // ── ENGINE / GEOMETRY: BEST OF BOTH (2026-08-01, completed) ────────────────────
        // The two probes answer DIFFERENT questions and neither subsumes the other:
        //   GEO  "is the weapon within apiTouchU of this capsule?"  — proximity, computed by us
        //        from an APPROXIMATED weapon (segment + radius). Covers hover and near-misses,
        //        which is a deliberate feature, and works on capsules the engine never reports.
        //   ENG  "did Havok collide these two shapes?"              — the engine's narrowphase
        //        against the weapon's REAL geometry (hilt, blade, pommel), giving the exact
        //        capsule via the shape key and a true separating distance.
        // Policy: ENG owns IDENTITY whenever it exists (it cannot be wrong about which capsule
        // the solver touched), and its distance is preferred because it measures real shapes.
        // GEO fills every gap, so hover coverage is unchanged.
        // SANITY: the engine distance is cross-checked against the geometry of the SAME capsule.
        // A ring entry that survived a body swap could otherwise inject a wild depth (a -5.05u
        // outlier appeared once among neighbours under 1u). If they disagree beyond
        // kEngGeoSaneU the geometric value is used — identity still from the engine — and the
        // disagreement is logged, so this stays MEASURED rather than assumed.
        constexpr float kEngGeoSaneU = 6.f;
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
            // geometric distance to the SAME capsule the engine named — the cross-check
            float geoD = 1e9f;
            {
                float ca[3], cb[3], cr;
                if (GrabDiag::ReadCapsuleWorldUSide(actor, eh.slot, eh.left, eh.child, ca, cb, &cr)) {
                    const HandProbes& hp = g_hp[hand];
                    if (hp.weapon.live)
                        geoD = (hp.weapon.seg ? SegSegDistU(ca, cb, hp.weapon.p, hp.weapon.q)
                                              : SegPointDistU(ca, cb, hp.weapon.p))
                               - cr - hp.weapon.pad;
                }
            }
            // MEASURE the ENG-vs-GEO agreement instead of speculating about a "discontinuity".
            // If the two metrics track each other, mixing them is free and the flag is just
            // provenance; if they diverge, this says by how much and we can act on a number.
            if (geoD < 1e8f) {
                static std::atomic<int>   s_agrN{ 0 };
                static std::atomic<int>   s_agrSumMilli{ 0 };   // integer accumulator, |eng-geo| in 1/1000 u
                static std::atomic<int>   s_agrMaxMilli{ 0 };
                const int dMilli = (int)(std::fabs(eh.distU - geoD) * 1000.f);
                const int n = s_agrN.fetch_add(1, std::memory_order_relaxed) + 1;
                s_agrSumMilli.fetch_add(dMilli, std::memory_order_relaxed);
                int prevMax = s_agrMaxMilli.load(std::memory_order_relaxed);
                while (dMilli > prevMax &&
                       !s_agrMaxMilli.compare_exchange_weak(prevMax, dMilli, std::memory_order_relaxed)) {}
                if (ObjectHold::ApiLogEnabled() && (n % 64) == 0)
                    logger::info("API weapon ENG-vs-GEO agreement over {} samples: mean |diff| "
                                 "{:.2f}u, max {:.2f}u — small means the two metrics are "
                                 "interchangeable and mixing them costs nothing",
                                 n, s_agrSumMilli.load() / 1000.f / (float)n,
                                 s_agrMaxMilli.load() / 1000.f);
            }
            float useD = eh.distU;
            const bool sane = geoD < 1e8f && std::fabs(eh.distU - geoD) <= kEngGeoSaneU;
            if (geoD < 1e8f && !sane) {
                useD = geoD;                       // engine depth implausible -> trust geometry
                static std::atomic<int> s_disLogged{ 0 };
                if (ObjectHold::ApiLogEnabled() &&
                    s_disLogged.fetch_add(1, std::memory_order_relaxed) < 8)
                    logger::info("API weapon: ENG/GEO depth disagree on {:08X} slot={} child={} "
                                 "— eng={:.2f}u geo={:.2f}u (>{:.0f}u apart); using GEO depth, "
                                 "engine identity kept",
                                 eh.actorId, eh.slot, eh.child, eh.distU, geoD, kEngGeoSaneU);
            }
            Hit& h = out[hand][kClsWeapon];
            h.found = true; h.dist = useD;
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
        if (h.viaBox == kSlabBox)           return PPBAPI::kSourcePalm;   // HIGGS's box led = an open hand (2026-09-06)
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
        // ⚠ GENITAL and HEAD carry a sourceName too ("shaft"/"tip", "face"/"head") and were
        // both dropping it here — the discriminator that tells a kiss from a headbutt never
        // reached a single consumer. Gate on "is there a name", not on a list of kinds.
        if (c.sourceName[0])
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
        // ★ 2026-08-23: the RAW path used to log NOTHING (this gate was `!raw`), which is how
        // two 7-second contacts reached a consumer as silence with no trail to debug from.
        // Start/End only — Continue would be per-tick spam.
        if (ObjectHold::ApiLogEnabled() && phase != PPBAPI::kPhaseContinue && raw) {
            char rp[192];
            PackStr(c, rp, sizeof rp);
            logger::info("API RAW {} {:08X} {} d={:.2f}u dur={:.2f}s",
                         phase == PPBAPI::kPhaseStart ? "START" : "END",
                         c.actorFormId, rp, c.distU, c.durationS);
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
            static const char* const kViaName[kHandBoxN] = { "idx0", "idx1", "f3base", "f3tips", "PALM" };
            const int via = g_hp[c.wand & 1].lastViaBox;   // this hand's latest hand-class hit
            logger::info("API {} {:08X} {} d={:.2f}u dur={:.2f}s src={}{} curl={:.1f}u "
                         "vrik=I{:.2f}/M{:.2f} hv={}",
                         phase == PPBAPI::kPhaseStart ? "START" : "END",
                         c.actorFormId, packed, c.distU, c.durationS,
                         c.engineContact ? "ENG" : "GEO", wpn,
                         g_hp[c.wand & 1].curlDistU,
                         g_hp[c.wand & 1].vrikIndex, g_hp[c.wand & 1].vrikMiddle,
                         (via >= 0 && via < kHandBoxN) ? kViaName[via] : "-");
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

    bool IsExcludedActor(RE::Actor* actor) { return ChildOrMannequin(actor); }

    void NoteDriven(RE::Actor* actor)
    {
        if (!actor || g_rosterN >= kMaxRoster) return;
        if (ChildOrMannequin(actor)) return;   // 2026-09-13: not in the roster -> no engine touches, probes or contacts
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

        // ★ 2026-09-07 (report 33 §8.3): the engine's own hand contacts, resolved EVERY frame, BEFORE
        // the apiHz throttle below — this is the clock PushStep's hand-travel anchor lives on now.
        CollectEngineHandTouches(roster, rosterN);

        // 2026-08-19: the ORIFICE DRIVE consumes the probe set (CopyProbes) and nothing else
        // from this tick, so it must be able to run with the contact engine off. Assemble the
        // probes for it, then leave — none of the contact/digest/event machinery below runs.
        const bool apiOn = ObjectHold::ApiTouchEnabled();
        if (!apiOn && !ObjectHold::OrificeEnabled()) return;
        const std::uint64_t now = NowMs();
        const float hz = ObjectHold::ApiHz();
        if (now - g_lastTickMs < (std::uint64_t)(1000.f / (hz < 1.f ? 1.f : hz))) return;
        g_lastTickMs = now;
        if (!apiOn) { CollectProbes(); return; }

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
                    // ── DWELL on the capsule GROUP, reporting the most-touched capsule ──
                    // The group is the sub-region: sliding between neighbouring capsules that
                    // MEAN the same thing keeps one contact running, while cheek -> mouth
                    // genuinely changes the meaning and starts a new one (INTEGRATION.md's own
                    // example). Same identity the digest uses, so the two streams agree.
                    const bool maleT = IsMaleActor(actor);
                    const int  hSub  = SubRegionOfPart(h.slot, h.child, maleT);
                    if (hSub != ct->candSub) {                 // NEW GROUP: restart the visit
                        ct->candSub = hSub;
                        ct->candSinceMs = now;
                        ct->nParts = 0;
                        ct->lastMs = now;
                    }
                    // per-capsule seconds inside this group — the winner is what gets reported,
                    // so a wandering touch names the capsule it actually spent its time on
                    // rather than whichever one happened to be nearest on the emitting tick.
                    {
                        const float dtS = ct->lastMs ? (float)(now - ct->lastMs) / 1000.f : 0.f;
                        ct->lastMs = now;
                        int pi = -1;
                        for (int k = 0; k < ct->nParts; ++k)
                            if (ct->parts[k].slot == h.slot && ct->parts[k].child == h.child &&
                                ct->parts[k].left == (h.left ? 1 : 0)) { pi = k; break; }
                        if (pi < 0 && ct->nParts < kMaxPartAcc) {
                            pi = ct->nParts++;
                            ct->parts[pi] = { h.slot, h.child, (std::uint8_t)(h.left ? 1 : 0), 0.f };
                        }
                        if (pi >= 0) ct->parts[pi].secs += dtS;
                    }
                    // the reported capsule = most seconds held in this group
                    int bi = -1; float bs = -1.f;
                    for (int k = 0; k < ct->nParts; ++k)
                        if (ct->parts[k].secs > bs) { bs = ct->parts[k].secs; bi = k; }
                    const int  rSlot  = bi >= 0 ? ct->parts[bi].slot  : h.slot;
                    const int  rChild = bi >= 0 ? ct->parts[bi].child : h.child;
                    const bool rLeft  = bi >= 0 ? ct->parts[bi].left != 0 : h.left;
                    // the GROUP has been held long enough — dwell class comes from the reported
                    // capsule, so the intimate ladder keeps its own (tighter) threshold
                    const bool qualified =
                        (float)(now - ct->candSinceMs) / 1000.f >= DwellSFor(rSlot, rChild);

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
                    } else if (cls == kClsGenital) {
                        // Names the part of HIM that touched, exactly as a weapon names itself:
                        // seg 0 = shaft, seg 1 = tip (viaBox carries the winning segment).
                        p.sourceKind = PPBAPI::kSourceGenital;
                        std::snprintf(p.sourceName, sizeof p.sourceName, "%s",
                                      h.viaBox >= 1 ? "tip" : "shaft");
                    } else if (cls == kClsHead) {
                        // Same idiom: name the part of the HEAD that touched. "face" is the
                        // front end of the box — the half that makes a kiss a kiss — "head" is
                        // everything else (a forehead lean, the side of the skull, a headbutt).
                        p.sourceKind = PPBAPI::kSourceHead;
                        std::snprintf(p.sourceName, sizeof p.sourceName, "%s",
                                      h.viaBox == 2 ? "mouth" : h.viaBox == 1 ? "face" : "head");
                    } else {
                        p.sourceKind = ClassifyHand(actor, hand, h);
                        p.sourceName[0] = '\0';
                        g_hp[hand].lastViaBox = h.viaBox;      // apiLog hv= receipt
                    }
                    if (qualified) {
                        // the WHERE fields carry the MOST-TOUCHED capsule of this group
                        p.slot = rSlot; p.child = rChild;
                        p.leftTwin = rLeft ? 1 : 0;
                        BodyPartName(rSlot, rLeft, rChild, p.bodyPart, sizeof p.bodyPart,
                                     maleT, GrabDiag::IsBeastActorForNames(actor));
                        StampClass(p, rSlot, rChild, -1, maleT);
                        StampWeapon(p, hand, EngineHitFor(roster[ai].id, hand));
                        StampGenLevel(p, actor);
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
                        const int sub = SubRegionOfPart(h.slot, h.child, IsMaleActor(actor));
                        const std::uint8_t lane =
                            (p.sourceKind == PPBAPI::kSourceGenital ||
                             p.sourceKind == PPBAPI::kSourceHead) ? p.sourceKind : 0;
                        DigestContact* dc = nullptr;
                        for (DigestContact& d : g_digest)
                            if (d.live && d.actorId == roster[ai].id && d.wand == hand &&
                                d.sub == sub && d.srcLane == lane) { dc = &d; break; }
                        if (!dc) {
                            for (DigestContact& d : g_digest) if (!d.live) { dc = &d; break; }
                            if (dc) {
                                *dc = DigestContact{};
                                dc->live = true; dc->actorId = roster[ai].id;
                                dc->wand = (std::uint8_t)hand; dc->region = reg; dc->sub = sub;
                                dc->srcLane = lane;
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
                            if (sk < PPBAPI::kSourceCount) dc->srcSecs[sk] += dt;
                            // the sub-part name for the two nameless-at-emit kinds:
                            // GENITAL "shaft"/"tip" and HEAD "face"/"head". Both are resolved
                            // during the scan and would otherwise be lost by the digest fold.
                            if ((sk == PPBAPI::kSourceGenital || sk == PPBAPI::kSourceHead) &&
                                p.sourceName[0])
                                std::snprintf(dc->genPart, sizeof dc->genPart, "%s", p.sourceName);
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
            // ⚠ seed from what the contact ALREADY carries, not from a fixed kind: with a
            // strictly-greater test an all-zero table used to resolve to index 0 and publish
            // FINGER for a source that had never accumulated a single tick.
            int bk = dc.pub.sourceKind; float bks = 0.f;
            for (int k = 0; k < PPBAPI::kSourceCount; ++k)
                if (dc.srcSecs[k] > bks) { bks = dc.srcSecs[k]; bk = k; }
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
            } else if (bk == PPBAPI::kSourceGenital || bk == PPBAPI::kSourceHead) {
                // no live name to re-read for these kinds — carried in the digest as it folded.
                // ⛔ 2026-09-12: HEAD was folded into genPart (above, :2106) and then WIPED here by
                // the else branch, so digest events, callbacks and Papyrus GetContactSource never
                // said "face" / "head" / "mouth" — the one word that tells a consumer it was a kiss.
                if (dc.genPart[0])
                    std::snprintf(p.sourceName, sizeof p.sourceName, "%s", dc.genPart);
            } else p.sourceName[0] = '\0';
            if (bi >= 0) {
                p.slot = dc.parts[bi].slot; p.child = dc.parts[bi].child;
                p.leftTwin = dc.parts[bi].left;
                char part[48];
                {
                    auto* dcA = RE::TESForm::LookupByID<RE::Actor>(dc.actorId);
                    BodyPartName(p.slot, p.leftTwin != 0, p.child, part, sizeof part,
                                 IsMaleActor(dcA), GrabDiag::IsBeastActorForNames(dcA));
                }
                std::snprintf(p.bodyPart, sizeof p.bodyPart, "%s(%s)",
                              RegionLabel(dc.region), part);
                // the digest's REGION is the contact's identity, not the part's — a wandering
                // touch keeps one region while the reported part moves inside it.
                StampClass(p, p.slot, p.child, dc.region,
                           IsMaleActor(RE::TESForm::LookupByID<RE::Actor>(dc.actorId)));
                StampWeapon(p, dc.wand, EngineHitFor(dc.actorId, dc.wand));
                StampGenLevel(p, RE::TESForm::LookupByID<RE::Actor>(dc.actorId));
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
    // ── PROBE EXPORT (2026-08-19) — see PpbApi.h. Straight copy of what CollectProbes built
    // this tick; no filtering, so the consumer sees exactly what the contact engine sees.
    // Internal twin of the interface's GetContacts — same digest snapshot, no round trip.
    // The gesture layer (DeviceGesture.cpp) is inside PPB now and reads it directly.
    int CopyContacts(PPBAPI::PpbTouchContact* out, int max)
    {
        if (!out || max < 1) return 0;
        const Snapshot& s = g_snap[g_snapActive.load(std::memory_order_acquire)];
        const int n = s.n < max ? s.n : max;
        std::memcpy(out, s.c, sizeof(PpbTouchContact) * (size_t)n);
        return n;
    }

    int EngineTouchList(EngineTouch* out, int max)
    {
        if (!out || max < 1) return 0;
        int n = 0;
        for (const auto& r : g_engTouch)
            if (r.live && n < max) out[n++] = r.t;
        return n;
    }

    int CopyProbes(ProbeView* out, int max)
    {
        if (!out || max <= 0) return 0;
        int n = 0;
        for (int hand = 0; hand < 2; ++hand) {
            const HandProbes& hp = g_hp[hand];
            // ⚠ DELIBERATELY the four FINGER boxes only — HIGGS's palm box (boxes[kSlabBox]) is a
            // touch/push probe, NOT an orifice probe (user ruling 2026-09-06: orifice work is the
            // finger's; in the FINGER pose the palm box is muted anyway). Add it here only on a ruling.
            const Probe* all[6] = { &hp.boxes[0], &hp.boxes[1], &hp.boxes[2], &hp.boxes[3],
                                    &hp.weapon, &hp.object };
            const int   cls[6]  = { kClsHand, kClsHand, kClsHand, kClsHand,
                                    kClsWeapon, kClsObject };
            for (int i = 0; i < 6 && n < max; ++i) {
                const Probe* pr = all[i];
                if (!pr->live) continue;
                ProbeView& v = out[n++];
                v.p[0] = pr->p[0]; v.p[1] = pr->p[1]; v.p[2] = pr->p[2];
                v.q[0] = pr->q[0]; v.q[1] = pr->q[1]; v.q[2] = pr->q[2];
                v.pad  = pr->pad;
                v.seg  = pr->seg ? 1 : 0;
                v.cls  = cls[i];
                v.wand = hand;
                v.grabActorId = hp.grabActorId;   // per-TARGET grab mute — applied by the consumer
            }
        }
        // ── PLAYER GENITAL WAND (2026-08-23) ────────────────────────────────────────────────
        // The wand is a real Havok body and has been since it shipped — it pushes loose objects
        // through the ordinary narrowphase. But the ORIFICE drive is not a Havok-contact
        // consumer: it runs a geometric penetration gate over THIS list, so a source missing
        // here is invisible to it no matter how hard it is physically pushing. The wand was
        // file-local to HandBox, so the one thing a shaft most obviously ought to do, it could
        // not. It is a segment probe of exactly the same shape as the weapon blade — endpoints
        // plus a radius in `pad`, which the gate already uses as the intruder radius — so no new
        // consumer logic is needed anywhere.
        // APPENDED LAST on purpose: a caller passing the old max of 12 fills with hand probes
        // and simply drops the wand, which is the correct degradation, never a truncated hand.
        {
            float wa[2][3], wb[2][3], wr = 0.f;
            const int nw = HandBox::WandProbeSegments(wa, wb, &wr);
            for (int i = 0; i < nw && n < max; ++i) {
                ProbeView& v = out[n++];
                v.p[0] = wa[i][0]; v.p[1] = wa[i][1]; v.p[2] = wa[i][2];
                v.q[0] = wb[i][0]; v.q[1] = wb[i][1]; v.q[2] = wb[i][2];
                v.pad  = wr;                  // the shaft's own radius — a 1.3u intruder, not a fingertip
                v.seg  = 1;
                v.cls  = kClsGenital;
                v.wand = 0;                   // not a hand: no consumer reads .wand for this class
                v.grabActorId = 0;            // a shaft is never the thing HIGGS is holding
            }
        }
        // ── PLAYER HEAD BOX (2026-09-03) — same reasoning, appended after the wand ───────────
        // One segment, back-of-skull -> face, with the box's inscribed radius in `pad`. Appended
        // LAST so an older caller's smaller buffer degrades by dropping the head, never by
        // truncating a hand.
        {
            float ha[3], hb[3], hr = 0.f;
            if (n < max && HandBox::HeadProbeSegment(ha, hb, &hr)) {
                ProbeView& v = out[n++];
                v.p[0] = ha[0]; v.p[1] = ha[1]; v.p[2] = ha[2];
                v.q[0] = hb[0]; v.q[1] = hb[1]; v.q[2] = hb[2];
                v.pad  = hr;
                v.seg  = 1;
                v.cls  = kClsHead;
                v.wand = 0;                   // not a hand
                v.grabActorId = 0;            // a head is never the thing HIGGS is holding
            }
        }
        // ── THE MOUTH (2026-09-06) — appended after the head, same class, same reasoning ──────────
        {
            float ma[3], mb[3], mr = 0.f;
            if (n < max && HandBox::MouthProbeSegment(ma, mb, &mr)) {
                ProbeView& v = out[n++];
                v.p[0] = ma[0]; v.p[1] = ma[1]; v.p[2] = ma[2];
                v.q[0] = mb[0]; v.q[1] = mb[1]; v.q[2] = mb[2];
                v.pad  = mr;
                v.seg  = 1;
                v.cls  = kClsHead;
                v.wand = 0;
                v.grabActorId = 0;
            }
        }
        return n;
    }

    // ── v28 RELEASE REACH (2026-09-10) — see PpbApi.h ──────────────────────────────────────────────
    // A MIRROR of ScanActor's OBJECT branch: interior capsules (head C9/C10/C11, COM C22+) use the raw
    // distance; everything else subtracts the object's pad and the breast capsule pad. If that branch's
    // formula ever changes, change this with it - DeviceGesture's release decision is only honest while
    // the two agree.
    bool HeldObjectGapU(RE::Actor* actor, int hand, int slot, bool left, int child,
                        float* gapOut, float* rawOut, int* kindOut)
    {
        if (!actor || hand < 0 || hand > 1) return false;
        const HandProbes& hp = g_hp[hand];
        if (!hp.object.live) return false;
        float a[3], b[3], r = 0.f;
        if (!GrabDiag::ReadCapsuleWorldUSide(actor, slot, left, child, a, b, &r)) return false;
        const bool interior = (slot == 3 && (child == 9 || child == 10 || child == 11)) ||
                              (slot == 11 && child >= 22);
        float capPad = 0.f;
        if (slot == 6 && (child == 11 || child == 12) && !IsMaleActor(actor)) {
            const float brPadBase = ObjectHold::ApiBreastPadU();
            if (brPadBase > 0.f)
                capPad = brPadBase + ObjectHold::ApiBreastPadSlope() *
                                     (std::max)(0.f, GrabDiag::BreastCupOf(actor->GetFormID()) - 10.40f);
            if (capPad < 0.f) capPad = 0.f;           // the scan applies it only when brPad > 0
        }
        const float dRaw = (hp.object.box
                            ? SegObbDistU(a, b, hp.object.bc, hp.object.bR, hp.object.bh)
                            : hp.object.seg
                            ? SegSegDistU(a, b, hp.object.p, hp.object.q)
                            : SegPointDistU(a, b, hp.object.p)) - r;
        if (gapOut)  *gapOut  = interior ? dRaw : dRaw - hp.object.pad - capPad;
        if (rawOut)  *rawOut  = dRaw;
        if (kindOut) *kindOut = hp.object.box ? 2 : (hp.object.seg ? 1 : 0);
        return true;
    }

    void EmitMouthStage(RE::Actor* actor, int stage, bool entered, int hand, float distU)
    {
        // ORIFICE (2026-08-19): the mouth gate is the ONE mouth signal in PPB, so the oral
        // channel is fed from here — BEFORE the apiEvents gate, because the orifice drive is
        // not an event consumer and must not be silenced by an event knob.
        if (actor) Orifice::NoteMouthStage(actor->GetFormID(), stage, entered);
        if (!actor || !ObjectHold::ApiEventsEnabled()) return;
        static const char* kName[3]  = { "PPB_MouthLips", "PPB_MouthEnter", "PPB_MouthThroat" };
        static const char* kStage[3] = { "LIPS", "ENTER", "THROAT" };
        if (stage < 0 || stage > 2) return;
        char packed[32];
        // ★ 2026-09-12: hand == 2 is the PLAYER'S MOUTH (a kiss; LIPS only by construction). The
        // WAND token keeps its L/R contract and a third field is APPENDED rather than inventing an
        // 'H' token that an L/R parser would misread: "R|LIPS|HEAD".
        if (hand == 2) std::snprintf(packed, sizeof packed, "R|%s|HEAD", kStage[stage]);
        else           std::snprintf(packed, sizeof packed, "%s|%s", hand == 1 ? "L" : "R", kStage[stage]);
        SKSE::ModCallbackEvent ev{ kName[stage], packed, entered ? 1.f : 0.f, actor };
        SKSE::GetModCallbackEventSource()->SendEvent(&ev);
        if (ObjectHold::ApiLogEnabled())
            logger::info("API MOUTH {:08X} {} {} hand={} d={:.2f}u", actor->GetFormID(),
                         kStage[stage], entered ? "ON" : "OFF",
                         hand == 2 ? "HEAD" : (hand == 1 ? "L" : "R"), distU);
    }

    void ClearOnLoad()
    {
        for (auto& L : g_engLatch) L.live = false;   // FormIDs recycle across saves
        g_engN = 0;
        for (Contact& ct : g_contacts) ct.live = false;
        for (DigestContact& dc : g_digest) dc.live = false;
        for (auto& r : g_engTouch) r.live = false;   // 2026-09-07: body pointers + FormIDs recycle across loads
        g_bodyResN = 0;
        g_bodyMissN = 0;
        g_rosterN = 0;
        PublishSnapshot();
    }

    // ── the native interface object ─────────────────────────────────────────
    class TouchInterfaceImpl : public PPBAPI::IPpbTouchInterface1 {
    public:
        // 20102 (2026-09-03): bumped from 20101 so a consumer can FEATURE-DETECT kSourceHead.
        // Appending a source silently is exactly how a consumer ends up rendering "?" for a kind
        // it has no branch for — the reason the versioning contract exists. Consumers still gate
        // on >= 20000 for "males are driven"; >= 20102 means "source 8 (HEAD) can appear".
        // 20103 (2026-09-12): the kiss is real — HEAD digest contacts now carry their "face" / "head" /
        // "mouth" sourceName, a kiss can fire PPB_MouthLips as "R|LIPS|HEAD", the head never reaches
        // an interior capsule, and engineContact is set on weapon contacts only.
        // 20104 (2026-09-12): PPB_PushReaction — "push" / "shove" / "dropped" / "sweeped" per reaction (PushStep.cpp).
        // 20105 (2026-09-13): VRTE GearGestures request — PPB_GestureEquipRefused, PPB_GestureUndressGrip/GripEnd,
        // UndressEnd reason+sentence (and its paused/disabled/ripfailed Ends), GearEquipped <ordinary>,
        // PushReaction pusher fields.
        // 20106 (2026-09-13): VRTE integration review — children + mannequins excluded from every interaction layer,
        // PPB_PushPress, PushReaction <afterShove>, GearEquipped <locked>, verify 3.5 s, rip check 3.5 s.
        unsigned int GetBuildNumber() override { return 20106; }
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
        // NOTE (2026-08-23): the published 2-arg form answers from the FEMALE/reference map —
        // it is a static (slot,child) question with no actor, so it cannot know the sex. Live
        // contacts carry the sex-correct value in PpbTouchContact::subRegion; prefer that.
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
            if (c->sourceName[0]) {
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
        // ── appended 2026-08-01 ── the fields a MOD EVENT cannot carry.
        // A Papyrus event gives one packed string and ONE number, so an event-only consumer
        // could not see depth, weapon class, or provenance, and could never have distance AND
        // duration together. FindContact bridges it: the event handler resolves its own contact
        // and then reads whatever it likes, each value separate and typed.
        RE::BSFixedString N_GetContactWeaponClass(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c ? WeaponClassLabel(c->weaponClass) : "";
        }
        RE::BSFixedString N_GetContactWeaponEdge(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i);
            if (!c) return "";
            switch (c->weaponEdge) {
            case PPBAPI::kEdgeBlade:  return "Blade";
            case PPBAPI::kEdgeBlunt:  return "Blunt";
            case PPBAPI::kEdgePierce: return "Pierce";
            }
            return "";
        }
        // true = the game's own physics reported this collision; false = PPB proximity (hover)
        bool N_GetContactIsEngine(RE::StaticFunctionTag*, std::int32_t i) {
            const auto* c = At(i); return c && c->engineContact != 0;
        }
        // Resolve an event back to its live contact. asWand "L"/"R"; "" = either hand.
        // Returns -1 if the contact has already ended (normal on a TouchEnd handler).
        std::int32_t N_FindContact(RE::StaticFunctionTag*, RE::Actor* who, RE::BSFixedString wand) {
            if (!who) return -1;
            const Snapshot& s = Snap();
            const char* w = wand.c_str();
            const bool anyWand = !w || !*w;
            const std::uint8_t want = (w && (*w == 'L' || *w == 'l')) ? 1 : 0;
            for (int i = 0; i < s.n; ++i) {
                if (s.c[i].actorFormId != who->GetFormID()) continue;
                if (!anyWand && s.c[i].wand != want) continue;
                return i;
            }
            return -1;
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
        vm->RegisterFunction("GetContactDepth",       k, N_GetContactDepth);
        vm->RegisterFunction("GetContactWeaponClass", k, N_GetContactWeaponClass);
        vm->RegisterFunction("GetContactWeaponEdge",  k, N_GetContactWeaponEdge);
        vm->RegisterFunction("GetContactIsEngine",    k, N_GetContactIsEngine);
        vm->RegisterFunction("FindContact",           k, N_FindContact);
        logger::info("Papyrus natives registered: PPB_Touch (16 functions).");
        return true;
    }
}
