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

    // ── per-frame roster (OnPreDrive fills, OnFrame consumes — same frame only) ──
    struct RosterEntry { RE::Actor* actor; std::uint32_t id; float d2; };
    RosterEntry g_roster[kMaxRoster];
    int         g_rosterN = 0;

    // ── probe sources this tick ─────────────────────────────────────────────
    enum SourceClass : int { kClsHand = 0, kClsWeapon = 1, kClsObject = 2, kClsCount = 3 };
    struct Probe {
        bool  live = false;
        float p[3]{};        // probe point, world game units
        float pad  = 0.f;    // extra surface (object bound radius)
        char  name[48]{};    // weapon/object base name
    };
    struct HandProbes {
        Probe boxes[4];      // 0/1 index proximal+distal TIPs, 2 fist slab, 3 palm plate (centers)
        Probe weapon;
        Probe object;
        bool  curled = false;   // index distal tip near the palm plate = fist
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
        PpbTouchContact pub{};                  // the published view (refreshed every tick)
    };
    Contact g_contacts[kMaxContacts];

    // ── Papyrus snapshot (main thread writes, VM threads read) ──────────────
    struct Snapshot { int n = 0; PpbTouchContact c[kMaxContacts]{}; };
    Snapshot         g_snap[4];
    std::atomic<int> g_snapActive{ 0 };

    // ── consumer callbacks ──────────────────────────────────────────────────
    PPBAPI::PpbTouchCallback g_cbs[kMaxCallbacks]{};
    int g_cbN = 0;

    std::uint64_t g_lastTickMs = 0;

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

    // BODYPART string: named children get the map name (side-prefixed on sided slots);
    // unnamed fall back to "<slot>.C<n>" so a touch is NEVER silently dropped.
    void BodyPartName(int slot, bool left, int child, char* out, size_t cap)
    {
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
        for (int hand = 0; hand < 2; ++hand) {
            HandProbes& hp = g_hp[hand];
            hp = HandProbes{};
            for (int b = 0; b < 4; ++b) {
                float* p = hp.boxes[b].p;
                const bool ok = (b <= 1) ? HandBox::TipWorldU(hand, b, p)
                                         : HandBox::BoxCenterWorldU(hand, b, p);
                hp.boxes[b].live = ok;
            }
            // curl: index DISTAL tip riding at the palm plate = fist
            if (hp.boxes[1].live && hp.boxes[3].live) {
                const float dx = hp.boxes[1].p[0] - hp.boxes[3].p[0];
                const float dy = hp.boxes[1].p[1] - hp.boxes[3].p[1];
                const float dz = hp.boxes[1].p[2] - hp.boxes[3].p[2];
                hp.curled = (dx*dx + dy*dy + dz*dz)
                          < ObjectHold::ApiFistTipPalmU() * ObjectHold::ApiFistTipPalmU();
            }
            const bool isLeft = hand == 1;
            if (NpcFinger::WeaponPointU(isLeft, hp.weapon.p)) {
                hp.weapon.live = true;
                // name = the equipped weapon in that hand (display name; may be empty)
                if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* obj = pl->GetEquippedObject(isLeft))
                        std::snprintf(hp.weapon.name, sizeof hp.weapon.name, "%s", obj->GetName());
                }
            }
            if (hig && hig->IsHoldingObject(isLeft)) {
                if (auto* refr = hig->GetGrabbedObject(isLeft)) {
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

    void ScanActor(RE::Actor* actor, Hit out[2][kClsCount])
    {
        // actor-level cull: any live probe within reach of the actor's center?
        const auto ap = actor->GetPosition();
        bool anyNear = false;
        for (int hand = 0; hand < 2 && !anyNear; ++hand) {
            const HandProbes& hp = g_hp[hand];
            const Probe* all[6] = { &hp.boxes[0], &hp.boxes[1], &hp.boxes[2], &hp.boxes[3],
                                    &hp.weapon, &hp.object };
            for (const Probe* pr : all) {
                if (!pr->live) continue;
                const float dx = pr->p[0]-ap.x, dy = pr->p[1]-ap.y, dz = pr->p[2]-ap.z;
                if (dx*dx + dy*dy + dz*dz < kActorCullU * kActorCullU) { anyNear = true; break; }
            }
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
                    for (const Probe* pr : all)
                        if (pr->live && SegPointDistU(a, b, pr->p) < kSlotCullU) { slotNear = true; break; }
                }
                if (!slotNear) continue;

                for (int ch = 0; ch < nCh; ++ch) {
                    if (ch > 0 && !GrabDiag::ReadCapsuleWorldUSide(actor, slot, left, ch, a, b, &r))
                        continue;
                    for (int hand = 0; hand < 2; ++hand) {
                        const HandProbes& hp = g_hp[hand];
                        for (int bx = 0; bx < 4; ++bx) {
                            if (!hp.boxes[bx].live) continue;
                            const float d = SegPointDistU(a, b, hp.boxes[bx].p) - r;
                            Hit& h = out[hand][kClsHand];
                            if (d < h.dist) { h.found = true; h.dist = d; h.slot = slot;
                                              h.child = ch; h.left = left; h.viaBox = bx; }
                        }
                        if (hp.weapon.live) {
                            const float d = SegPointDistU(a, b, hp.weapon.p) - r;
                            Hit& h = out[hand][kClsWeapon];
                            if (d < h.dist) { h.found = true; h.dist = d; h.slot = slot;
                                              h.child = ch; h.left = left; }
                        }
                        if (hp.object.live) {
                            const float d = SegPointDistU(a, b, hp.object.p) - r - hp.object.pad;
                            Hit& h = out[hand][kClsObject];
                            if (d < h.dist) { h.found = true; h.dist = d; h.slot = slot;
                                              h.child = ch; h.left = left; }
                        }
                    }
                }
            }
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
        if (hp.curled) return PPBAPI::kSourceFist;
        // "clearly nearest" = the winning box beats every other box class by 1u (the
        // handbook margin). viaBox 0/1 = index → FINGER; 3 = palm plate → PALM; else HAND.
        if (h.viaBox == 0 || h.viaBox == 1) return PPBAPI::kSourceFinger;
        if (h.viaBox == 3)                  return PPBAPI::kSourcePalm;
        return PPBAPI::kSourceHand;
    }

    // ── event + callback emission ───────────────────────────────────────────
    void PackStr(const PpbTouchContact& c, char* out, size_t cap)
    {
        const char* kind = SourceKindName(c.sourceKind);
        if (c.sourceKind == PPBAPI::kSourceWeapon || c.sourceKind == PPBAPI::kSourceObject)
            std::snprintf(out, cap, "%s|%s:%s|%s|%s", c.wand ? "L" : "R", kind,
                          c.sourceName, c.bodyPart, c.skeleton);
        else
            std::snprintf(out, cap, "%s|%s|%s|%s", c.wand ? "L" : "R", kind,
                          c.bodyPart, c.skeleton);
    }

    void Emit(const PpbTouchContact& c, int phase)
    {
        for (int i = 0; i < g_cbN; ++i)
            if (g_cbs[i]) g_cbs[i](&c, phase);
        if (ObjectHold::ApiEventsEnabled()) {
            RE::TESForm* sender = RE::TESForm::LookupByID(c.actorFormId);
            char packed[192];
            PackStr(c, packed, sizeof packed);
            const char* name = phase == PPBAPI::kPhaseStart ? "PPB_TouchStart"
                             : phase == PPBAPI::kPhaseEnd   ? "PPB_TouchEnd" : "PPB_Touch";
            const float num = phase == PPBAPI::kPhaseEnd ? c.durationS : c.distU;
            SKSE::ModCallbackEvent ev{ name, packed, num, sender };
            SKSE::GetModCallbackEventSource()->SendEvent(&ev);
        }
        if (ObjectHold::ApiLogEnabled() && phase != PPBAPI::kPhaseContinue) {
            char packed[192];
            PackStr(c, packed, sizeof packed);
            logger::info("API {} {:08X} {} d={:.2f}u dur={:.2f}s",
                         phase == PPBAPI::kPhaseStart ? "START" : "END",
                         c.actorFormId, packed, c.distU, c.durationS);
        }
    }

    void PublishSnapshot()
    {
        const int next = (g_snapActive.load(std::memory_order_relaxed) + 1) & 3;
        Snapshot& s = g_snap[next];
        s.n = 0;
        for (const Contact& ct : g_contacts)
            if (ct.live && s.n < kMaxContacts) s.c[s.n++] = ct.pub;
        g_snapActive.store(next, std::memory_order_release);
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
                    PpbTouchContact& p = ct->pub;
                    p.actorFormId   = roster[ai].id;
                    p.toucherFormId = 0x14;
                    p.slot = h.slot; p.child = h.child;
                    p.leftTwin = h.left ? 1 : 0;
                    p.wand = (std::uint8_t)hand;
                    p.distU = h.dist;
                    p.durationS = (float)(now - ct->startMs) / 1000.f;
                    std::snprintf(p.skeleton, sizeof p.skeleton, "%s", skel);
                    BodyPartName(h.slot, h.left, h.child, p.bodyPart, sizeof p.bodyPart);
                    const HandProbes& hp = g_hp[hand];
                    if (cls == kClsWeapon) {
                        p.sourceKind = PPBAPI::kSourceWeapon;
                        std::snprintf(p.sourceName, sizeof p.sourceName, "%s", hp.weapon.name);
                    } else if (cls == kClsObject) {
                        p.sourceKind = PPBAPI::kSourceObject;
                        std::snprintf(p.sourceName, sizeof p.sourceName, "%s", hp.object.name);
                    } else {
                        p.sourceKind = ClassifyHand(actor, hand, h);
                        p.sourceName[0] = '\0';
                    }
                    Emit(p, ex ? PPBAPI::kPhaseContinue : PPBAPI::kPhaseStart);
                }
            }
        }

        // sweep: contacts not refreshed this tick have ended (source left, actor left
        // range/roster, or actor stopped being driven)
        for (Contact& ct : g_contacts) {
            if (!ct.live || ct.seen) continue;
            ct.pub.durationS = (float)(now - ct.startMs) / 1000.f;
            Emit(ct.pub, PPBAPI::kPhaseEnd);
            ct.live = false;
        }

        PublishSnapshot();
    }

    void ClearOnLoad()
    {
        for (Contact& ct : g_contacts) ct.live = false;
        g_rosterN = 0;
        PublishSnapshot();
    }

    // ── the native interface object ─────────────────────────────────────────
    class TouchInterfaceImpl : public PPBAPI::IPpbTouchInterface1 {
    public:
        unsigned int GetBuildNumber() override { return 10300; }   // 1.3.0
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
        logger::info("Papyrus natives registered: PPB_Touch.GetContact* (9 functions).");
        return true;
    }
}
