#include "PCH.h"
#include "GenitalProbe.h"
#include "Tuning.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

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
        bool gen52        = false;  // ★ THE CANDIDATE THAT MATTERS: biped slot 52 occupied
        bool isFemale     = false;
        char geomName[64] = "";     // first geometry name found under the chain (identifies the
                                    // schlong mod: SOS / SAM / TRX / UBE all name theirs)
        char armorName[64] = "";
        char gen52Name[64] = "";    // what is actually IN slot 52 (TNG's addon, or a bandolier)

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
                 | ((std::uint64_t)(gen52     ? 1 : 0) << 35);
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
    bool LooksRevealing(RE::TESObjectARMO* armo)
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
            if (std::strstr(buf, "reveal")) return true;
        }
        return false;
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
            g_nextPollMs[id] = now + 1000 + (id % 331);
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
            s.revealing = LooksRevealing(armo);
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
        if (auto* g52 = actor->GetWornArmor(kGen)) {
            s.gen52 = true;
            const char* gn = g52->GetName();
            if (!gn || !*gn) gn = g52->GetFormEditorID();
            if (gn && *gn) std::snprintf(s.gen52Name, sizeof(s.gen52Name), "%s", gn);
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
            "GENPROBE {:08X} '{}' sex={} | SLOT52={} '{}' | nodes={}/{} hidden={} "
            "| geom={} geomHidden={} first='{}' | bodyArmor={} revealing={} armor='{}'",
            id, nm, s.isFemale ? "F" : "M",
            s.gen52 ? 1 : 0, s.gen52Name[0] ? s.gen52Name : "-",
            s.nodesFound, kGenNodeCount, s.nodesHidden,
            s.geomNear, s.geomHidden, s.geomName[0] ? s.geomName : "-",
            s.bodyArmor ? 1 : 0, s.revealing ? 1 : 0, s.armorName[0] ? s.armorName : "-");
    }
}
