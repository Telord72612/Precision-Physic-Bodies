#include "PCH.h"
#include "GenitalProbe.h"
#include "Tuning.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace logger = SKSE::log;

namespace {

    // The SOS chain as it exists in the XP32 superset PPB ships. Verified present by name in
    // meshes/actors/character/character assets female/PPB/skeleton_female.nif (2026-08-02) —
    // which is exactly why bone presence cannot be used as a male test.
    constexpr const char* kGenNodes[] = {
        "NPC GenitalsBase [GenBase]",
        "NPC Genitals01 [Gen01]", "NPC Genitals02 [Gen02]", "NPC Genitals03 [Gen03]",
        "NPC Genitals04 [Gen04]", "NPC Genitals05 [Gen05]", "NPC Genitals06 [Gen06]",
        "NPC GenitalsScrotum [GenScrot]",
        "NPC L GenitalsScrotum [LGenScrot]",
        "NPC R GenitalsScrotum [RGenScrot]",
    };
    constexpr int kGenNodeCount = static_cast<int>(std::size(kGenNodes));
    constexpr int kMaxKwChars   = 240;   // AND keyword accumulator, see the scan below

    // What we learned about one actor this tick. Packed into an integer signature so the log is
    // EDGE-triggered: identical state = no line. (Same discipline as the RESHAPEGATE receipt —
    // a once-per-actor print lands on the first tick, which is exactly when the state is least
    // settled, so key the dedup on the STATE, not the actor.)
    struct Signals {
        int  nodesFound   = 0;      // how many of the 10 chain nodes exist in the tree
        int  nodesHidden  = 0;      // ...of those, how many carry kHidden
        int  geomNear     = 0;      // geometry shapes parented under the chain
        int  geomHidden   = 0;      // ...of those, flagged kHidden
        bool bodyArmor    = false;  // something equipped in biped slot 32 (body)
        bool revealing    = false;  // that body armor carries a *revealing* keyword
        bool gen52        = false;  // ★ THE CANDIDATE: slot 52 on the actor's SKIN
        bool gen52Worn    = false;  // control: slot 52 as an EQUIPPED item (expected always 0)
        char andKw[kMaxKwChars] = "";   // every AND_* keyword found across all worn pieces
        int  andKwCount   = 0;
        int  wornPieces   = 0;          // how many distinct armors we scanned
        bool isFemale     = false;
        char geomName[64] = "";     // first geometry name found under the chain (identifies the
                                    // schlong mod: SOS / SAM / TRX / UBE all name theirs)
        char armorName[64] = "";
        char gen52Name[64] = "";
        char revealKw[64] = "";   // which keyword made revealing=1 (provenance)    // what is actually IN slot 52 (TNG's addon, or a bandolier)

        std::uint64_t sig() const {
            // geomName/armorName deliberately excluded: they do not change while worn, and
            // including them would re-fire the line on every unrelated re-equip.
            return (std::uint64_t)nodesFound
                 | ((std::uint64_t)nodesHidden << 8)
                 | ((std::uint64_t)geomNear   << 16)
                 | ((std::uint64_t)geomHidden << 24)
                 | ((std::uint64_t)(bodyArmor ? 1 : 0) << 32)
                 | ((std::uint64_t)(revealing ? 1 : 0) << 33)
                 | ((std::uint64_t)(isFemale  ? 1 : 0) << 34)
                 | ((std::uint64_t)(gen52     ? 1 : 0) << 35)
                 | ((std::uint64_t)(gen52Worn ? 1 : 0) << 36)
                 | ((std::uint64_t)(std::uint8_t)andKwCount << 40)
                 | ((std::uint64_t)(std::uint8_t)wornPieces << 48);
        }
    };

    bool IsHidden(RE::NiAVObject* o)
    {
        return o && o->flags.any(RE::NiAVObject::Flag::kHidden);
    }

    // Count geometry under a node, and note whether it is hidden. Depth-bounded like every other
    // tree walk in this plugin — a malformed rig must never spin us.
    void WalkGeom(RE::NiAVObject* obj, int depth, Signals& s)
    {
        if (!obj || depth > 8) return;
        if (auto* geom = obj->AsGeometry()) {
            ++s.geomNear;
            if (IsHidden(geom)) ++s.geomHidden;
            if (s.geomName[0] == '\0') {
                const char* n = geom->name.c_str();
                if (n && *n) std::snprintf(s.geomName, sizeof(s.geomName), "%s", n);
            }
        }
        if (auto* node = obj->AsNode())
            for (auto& ch : node->GetChildren())
                WalkGeom(ch.get(), depth + 1, s);
    }

    // A body-slot armor whose keywords say it leaves the crotch visible. SOS uses "SOS_Revealing";
    // other frameworks use their own. Substring match, case-insensitive, so we catch the family
    // without hard-coding one mod's exact EditorID.
    // ⚠ This matches SOS/TNG's "…Revealing…" keyword family, NOT Advanced Nudity Detection —
    // AND ships 132 keywords and not one contains "reveal". Kept because it is the signal that
    // says "this garment leaves the schlong visible", which is exactly the MALE question. The
    // matched EditorID is reported so its provenance is never guessed at again.
    bool LooksRevealing(RE::TESObjectARMO* armo, char* outKw = nullptr, int outSz = 0)
    {
        if (!armo) return false;
        const auto n = armo->GetNumKeywords();
        for (std::uint32_t i = 0; i < n; ++i) {
            auto kw = armo->GetKeywordAt(i);
            if (!kw || !*kw) continue;
            const char* e = (*kw)->GetFormEditorID();
            if (!e) continue;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", e);
            for (char* p = buf; *p; ++p) *p = static_cast<char>(::tolower(*p));
            if (std::strstr(buf, "reveal")) {
                if (outKw && outSz > 1) std::snprintf(outKw, outSz, "%s", e);
                return true;
            }
        }
        return false;
    }

    // ── ADVANCED NUDITY DETECTION — READ THE GARMENT, NOT THE ACTOR (2026-08-17, corrected) ──
    // The first attempt read AND's six state FACTIONS off the actor. Measured on Carmella they
    // never moved: all 1 dressed AND naked, including AND_NudeActorFaction while she wore full
    // robes. Dead end, and the wrong model besides.
    //
    // AND actually classifies GARMENTS. The plugin ships 132 KYWD records (AND_ArmorBottom,
    // AND_PelvicCurtain, AND_PelvicFlashRisk{,Low,High,Extreme,Ultra}, AND_CoversAll,
    // AND_EffectivelyNaked, AND_Thong, AND_Microskirt, ... plus a parallel _Male set) and tags
    // worn items with them. So the exposure question is answered by the ITEM she has on, not by
    // any per-actor state — which is also why it survives the 100k custom armours problem only as
    // well as the tagging does, hence slot 32 stays the failsafe for untagged gear.
    //
    // ⚠ We do NOT hardcode which keyword means "open" yet. Nobody has measured which AND keywords
    // real gear actually carries in this load order, and guessing is exactly what produced the
    // faction detour. This DUMPS every AND_* keyword on every worn piece; the mapping gets built
    // from what a dress cycle actually prints. (PPB session method: log the unknown, then build
    // against it.)
    // Case-insensitive prefix test; no allocation, safe to call per worn item.
    bool HasPrefix(const char* s, const char* pre)
    {
        if (!s || !pre) return false;
        for (; *pre; ++s, ++pre)
            if (!*s || ::tolower((unsigned char)*s) != ::tolower((unsigned char)*pre)) return false;
        return true;
    }

    std::mutex                              g_mx;
    std::map<std::uint32_t, std::uint64_t>  g_lastSig;    // actor -> last logged signature
    std::map<std::uint32_t, std::uint64_t>  g_nextPollMs; // actor -> next allowed poll

    std::uint64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
}

namespace GenitalProbe {

    void Tick(RE::Actor* actor)
    {
        if (!ObjectHold::GenProbeOn() || !actor) return;
        auto* root = actor->Get3D();
        if (!root) return;

        const std::uint32_t id = actor->GetFormID();
        {   // ~1 Hz per actor, staggered by id so a crowd never lands on one frame
            std::lock_guard<std::mutex> lk(g_mx);
            const std::uint64_t now = NowMs();
            auto it = g_nextPollMs.find(id);
            if (it != g_nextPollMs.end() && now < it->second) return;
            // genProbe 2 = FAST MODE (~10 Hz): the equip-latency measurement. TNG queues its
            // addon through ActorEquipManager, so the state flips some time AFTER the visual;
            // at the 1 Hz default that lag is unmeasurable (resolution ~1.3 s). Use 2 for a
            // dress/undress timing session, 1 for ordinary observation.
            const bool fast = ObjectHold::GenProbeFast();
            g_nextPollMs[id] = now + (fast ? 100 + (id % 37) : 1000 + (id % 331));
        }

        Signals s;
        if (auto* base = actor->GetActorBase())
            s.isFemale = base->GetSex() == RE::SEX::kFemale;

        for (int i = 0; i < kGenNodeCount; ++i) {
            auto* n = root->GetObjectByName(RE::BSFixedString(kGenNodes[i]));
            if (!n) continue;
            ++s.nodesFound;
            if (IsHidden(n)) ++s.nodesHidden;
            WalkGeom(n, 0, s);
        }
        // Nothing of the chain present at all -> not an actor this research is about. Stay silent
        // so the log is not flooded by every guard in the hold.
        if (s.nodesFound == 0) return;

        if (auto* armo = actor->GetWornArmor(RE::BIPED_MODEL::BipedObjectSlot::kBody)) {
            s.bodyArmor = true;
            s.revealing = LooksRevealing(armo, s.revealKw, sizeof(s.revealKw));
            const char* an = armo->GetName();
            if (an && *an) std::snprintf(s.armorName, sizeof(s.armorName), "%s", an);
        }

        // ── ★ SLOT 52 (kModPelvisSecondary, 1<<22) — the candidate this probe now exists to test ──
        // The installed framework is The New Gentleman, and TNG decides "covered vs revealing" by
        // its own ini records + biped slot 52, user-swappable at runtime. It is NOT keyword-derived,
        // so LooksRevealing() above cannot see it — that column is kept only as the control.
        // TNG parks its per-actor genital addon in slot 52, so "slot 52 occupied by TNG's addon"
        // is exposure BY CONSTRUCTION, and it inherits TNG's own race/child/creature exclusions and
        // its TNG_Gentlewoman gate for free. Bone presence cannot do any of that: all ten Genitals*
        // nodes ship on every PPB female skeleton, on _1stperson, and on a bear.
        // NOTE the cost asymmetry: GetWornArmor deep-copies the whole worn inventory into a std::map
        // (one make_unique per entry). Acceptable at this probe's ~1 Hz, NOT acceptable for the
        // eventual gate — that must walk GetInventoryChanges(false)->entryList and test
        // armo->HasPartOf(kGen) directly.
        constexpr auto kGen = RE::BIPED_MODEL::BipedObjectSlot::kModPelvisSecondary;
        // ⚠ 2026-08-17 CORRECTION: GetWornArmor(52) is the WRONG read and returns null even on a
        // naked male (measured: SLOT52=0 on a nude Imperial with all 10 chain bones present).
        // TNG's genital addon is part of the actor's SKIN, not an equipped item, so the skin is
        // what carries the slot-52 part. GetWornArmor is kept as a second column purely to show
        // that it never fires — if it ever does, some mod really is EQUIPPING something there.
        if (auto* skin = actor->GetSkin()) {
            if (skin->HasPartOf(kGen)) {
                s.gen52 = true;
                const char* gn = skin->GetName();
                if (!gn || !*gn) gn = skin->GetFormEditorID();
                if (gn && *gn) std::snprintf(s.gen52Name, sizeof(s.gen52Name), "%s", gn);
            }
        }
        if (auto* g52 = actor->GetWornArmor(kGen)) {
            s.gen52Worn = true;
            if (!s.gen52Name[0]) {
                const char* gn = g52->GetName();
                if (gn && *gn) std::snprintf(s.gen52Name, sizeof(s.gen52Name), "%s", gn);
            }
        }

        // ── AND keyword scan across every worn biped slot ───────────────────────────────
        // Slots 30..61 is the whole biped range. Dedup by form so a multi-slot item is scanned
        // once. Keywords are read from the LIVE form, which is what AND's runtime analysis
        // writes to — a static esp read would miss anything it adds on the fly.
        {
            RE::TESObjectARMO* seen[24] = {};
            int nSeen = 0;
            for (int slot = 30; slot <= 61 && nSeen < 24; ++slot) {
                auto* armo = actor->GetWornArmor(static_cast<RE::BIPED_MODEL::BipedObjectSlot>(
                                 1u << (slot - 30)));
                if (!armo) continue;
                bool dup = false;
                for (int k = 0; k < nSeen; ++k) if (seen[k] == armo) { dup = true; break; }
                if (dup) continue;
                seen[nSeen++] = armo;
                ++s.wornPieces;

                const auto nkw = armo->GetNumKeywords();
                for (std::uint32_t i = 0; i < nkw; ++i) {
                    auto kw = armo->GetKeywordAt(i);
                    if (!kw || !*kw) continue;
                    const char* e = (*kw)->GetFormEditorID();
                    if (!HasPrefix(e, "AND_")) continue;
                    ++s.andKwCount;
                    const int len = (int)std::strlen(s.andKw);
                    const int rem = kMaxKwChars - len - 2;
                    if (rem > 4)
                        std::snprintf(s.andKw + len, rem, "%s%s", len ? " " : "", e);
                }
            }
        }

        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            auto it = g_lastSig.find(id);
            if (it == g_lastSig.end() || it->second != s.sig()) {
                g_lastSig[id] = s.sig();
                changed = true;
            }
        }
        if (!changed) return;

        const char* nm = "<unnamed>";
        if (auto* b = actor->GetActorBase()) if (const char* f = b->GetFullName(); f && *f) nm = f;

        logger::info(
            "GENPROBE {:08X} '{}' sex={} | SLOT52(skin)={} worn={} '{}' | nodes={}/{} hidden={} "
            "| geom={} geomHidden={} first='{}' | bodyArmor={} revealing={}({}) armor='{}' | worn={} ANDkw={} [{}]",
            id, nm, s.isFemale ? "F" : "M",
            s.gen52 ? 1 : 0, s.gen52Worn ? 1 : 0, s.gen52Name[0] ? s.gen52Name : "-",
            s.nodesFound, kGenNodeCount, s.nodesHidden,
            s.geomNear, s.geomHidden, s.geomName[0] ? s.geomName : "-",
            s.bodyArmor ? 1 : 0, s.revealing ? 1 : 0, s.revealKw[0] ? s.revealKw : "-",
            s.armorName[0] ? s.armorName : "-",
            s.wornPieces, s.andKwCount, s.andKw[0] ? s.andKw : "none");
    }

    // ── ★ THE EXPOSURE GATE. See GenitalProbe.h for the measurement and the why. ──────────
    bool IsExposed(RE::Actor* actor)
    {
        if (!actor) return false;
        constexpr auto kGen = RE::BIPED_MODEL::BipedObjectSlot::kModPelvisSecondary;

        // ── half 1: CAPABILITY. Cheap (one pointer + a bitmask), so it is never cached: it is
        // also the early-out that keeps every non-TNG actor off the expensive half entirely.
        auto* skin = actor->GetSkin();
        if (!skin || !skin->HasPartOf(kGen)) return false;

        // ── half 2: STATE (dressed?). GetWornArmor deep-copies the worn inventory, so it is
        // cached per actor behind a short TTL. The TTL is the ONLY imprecision in this gate:
        // for up to kTtlMs after a dress/undress the answer is stale. That is bounded and
        // one-sided in practice — a rig that lingers ~half a second after trousers go on is an
        // annoyance; the alternative (this call every frame, per actor) is a measurable cost
        // for a state that changes seconds apart at most.
        constexpr std::uint64_t kTtlMs = 500;
        struct Cached { std::uint64_t ms; bool covered; };
        static std::map<std::uint32_t, Cached> s_cache;
        const auto nowMs = (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        const std::uint32_t id = actor->GetFormID();
        auto it = s_cache.find(id);
        if (it == s_cache.end() || nowMs - it->second.ms > kTtlMs) {
            if (s_cache.size() > 512) s_cache.clear();      // unbounded-growth guard (cell churn)
            const bool covered = actor->GetWornArmor(kGen) != nullptr;
            it = s_cache.insert_or_assign(id, Cached{ nowMs, covered }).first;
        }
        return !it->second.covered;
    }
}
