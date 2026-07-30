#include "PCH.h"
#include "Interop.h"
#include "HiggsInterface.h"
#include "SKEE.h"        // RaceMenu VR body-morph interface (Body-Scale, 2026-07-14)
#include "Tuning.h"      // ObjectHold::HiggsPokeFix (2026-07-30)

#include <Windows.h>  // GetModuleHandleA

namespace logger = SKSE::log;

namespace {
    Higgs::IHiggsInterface001* g_higgs = nullptr;
    SKEE::IBodyMorphInterface* g_skeeMorph = nullptr;   // null until AcquireSkee succeeds

    // ── Body-Scale region -> morph key tables (2026-07-14) ──────────────────────────────────
    // GetMorph(actor, name, "OBody") returns the slider value for one morph NAME under the NAKED
    // OBody key ONLY (see GetRegionMorph); here we combine the several NAMES that make up a body
    // region. Antonym ("Small"/"Slim") names are SUBTRACTED so every entry pushes the region's
    // signed deviation the same way (positive = bigger than the zero-slider capsule base).
    // Order matches BodyScale::Region 0..6 (chest,breasts,belly,waist,butt,thighs,upperArms).
    // 2026-07-18 EXTENSION (Report 21, from CBBE Petite's real XML): slim presets slim via sliders
    // the original tables never read — the petite waist/butt were INVISIBLE. Sign rule: sliders
    // whose POSITIVE value means SMALLER go in the INVERTED table; sliders that store negatives
    // for smaller (Belly, ChestWidth, ChestDepth) live in ADDITIVE and carry their own sign.
    // ★ 2026-07-18b VOCABULARY HARVEST: scanned all 117 installed SliderPresets XMLs — the MODERN
    // 3BA volume sliders (BreastsNewSH x194 presets, BreastsGone, ButtClassic, RoundAss, Thigh*
    // Thicc_v2...) were entirely missing, which is why Lydia's O-key preset read ALL-ZERO nets
    // (the auto re-fit pipeline fired correctly; the reads were blind). Shape/position sliders
    // (Height/Gravity/Perkiness/TopSlope/Together/Nipple*/Areola*...) stay excluded on purpose —
    // they move flesh, not volume. Unknown names now surface via the UNMAPPED log line at latch.
    constexpr const char* kRegionAdd[BodyScale::kMorphRegionCount][7] = {
        { "ChestWidth", "BigTorso",  "ChestDepth", nullptr, nullptr, nullptr, nullptr },  // chest
        { "Breasts",    "BreastWidth", "BreastsNewSH", nullptr, nullptr, nullptr, nullptr }, // breasts
        { "BigBelly",   "Belly",     nullptr, nullptr, nullptr, nullptr, nullptr },       // belly ("Belly" additive since 2026-07-16)
        { "Waist",      "ChubbyWaist", "WideWaistLine", "Hips", nullptr, nullptr, nullptr }, // waist (pelvis width rides spine0)
        { "Butt",       "BigButt",   "ButtClassic", "RoundAss", "AppleCheeks", "MuscleButt", nullptr }, // butt
        { "Thighs",     "ThighOutsideThicc_v2", "ThighInsideThicc_v2", "MuscleLegs", nullptr, nullptr, nullptr }, // thighs
        { "Arms",       "MuscleArms", nullptr, nullptr, nullptr, nullptr, nullptr },      // upperArms
    };
    constexpr const char* kRegionInv[BodyScale::kMorphRegionCount][5] = {
        { nullptr, nullptr, nullptr, nullptr, nullptr },   // chest
        { "BreastsSmall", "BreastsSmall2", "BreastsGone", "BreastFlatness2", "BreastFlatness" }, // breasts
        { nullptr, nullptr, nullptr, nullptr, nullptr },   // belly
        { "TummyTuck", "HipNarrow_v2", nullptr, nullptr, nullptr },  // waist
        { "ButtSmall", "ButtPressed_v2", nullptr, nullptr, nullptr },// butt
        { "SlimThighs", "LegsThin", nullptr, nullptr, nullptr },     // thighs
        { nullptr, nullptr, nullptr, nullptr, nullptr },   // upperArms
    };

    // HIGGS hands its interface to other plugins via an SKSE messaging handshake
    // (NOT a DLL export). Dispatch the magic message to "HIGGS"; HIGGS writes a
    // GetApiFunction pointer into our struct, which we call to fetch the iface.
    // Layout matches HiggsPluginAPI::GetHiggsInterface001 (higgsinterface001.cpp):
    //   struct HiggsMessage { void* (*GetApiFunction)(unsigned int); };
    struct HiggsMessage { void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr; };
    constexpr std::uint32_t kHiggsGetInterface = 0xF9279A57u;  // HiggsMessage::kMessage_GetInterface
}

namespace Interop {

    // ── HIGGS POKE FIX (2026-07-30, knob higgsPokeFix) ───────────────────────────────────
    // HIGGS plays a finger-CLOSE animation whenever a hand is near a grabbable object. That
    // is exactly the hand pose PPB's finger colliders need OPEN — with the fingers curled you
    // cannot poke anything, which is the single most common "PPB doesn't do anything for me"
    // report. HIGGS exposes the switch as `SelectedCloseFingerAnimMaxHandSpeed`, documented in
    // higgs_vr.ini as "Roomspace hand speed (m/s) below which to open the fingers when the hand
    // is next to a valid item to grab"; -1 disables the close animation so the hand stays open.
    //
    // We set it through HIGGS's OWN settings API (SetSettingDouble, vfunc 39) rather than
    // shipping an ini override. That matters: an ini override would clobber the user's whole
    // HIGGS config file and lose every other setting they had tuned, and it would fight the
    // load order. A runtime call touches ONE value, leaves their file untouched, and is
    // reversible by a knob.
    //
    // ⚠ Re-applied on every game load: HIGGS re-reads higgs_vr.ini when settings reload, which
    // would silently restore the default mid-session. Idempotent — it reads first and only
    // writes when the value actually differs, so a user who deliberately set something else
    // sees exactly one log line explaining what changed and how to opt out.
    void ApplyHiggsPokeFix(const char* whenTag)
    {
        if (ObjectHold::HiggsPokeFix() < 0.5f) return;
        auto* h = GetHiggs();
        if (!h) return;
        constexpr const char* kKey = "SelectedCloseFingerAnimMaxHandSpeed";
        const double want = -1.0;
        double cur = 0.0;
        const bool gotOk = h->GetSettingDouble(kKey, cur);
        if (gotOk && cur == want) return;                       // already correct — silent
        const bool setOk = h->SetSettingDouble(kKey, want);
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            if (setOk) {
                logger::info("HIGGS poke fix ({}): {} {} -> {:.1f} via HIGGS's own settings API. "
                             "HIGGS closes the fingers near a grabbable, which defeats PPB's finger "
                             "colliders; -1 keeps the hand open so poking works. Your higgs_vr.ini is "
                             "NOT modified. Set higgsPokeFix 0 in PPB_tuning.txt to leave it alone.",
                             whenTag, kKey,
                             gotOk ? std::to_string(cur) : std::string("<unreadable>"), want);
            } else {
                logger::warn("HIGGS poke fix ({}): SetSettingDouble('{}') FAILED — HIGGS build may not "
                             "expose it. Poking will need the ini set by hand.", whenTag, kKey);
            }
        }
    }


    void AcquireHiggs()
    {
        if (g_higgs) return;  // idempotent

        if (!::GetModuleHandleA("higgs_vr.dll")) {
            logger::info("PPB HIGGS interface=ABSENT (higgs_vr.dll not loaded) - grab gate falls back to the drift heuristic");
            return;
        }
        auto* msging = SKSE::GetMessagingInterface();
        if (!msging) { logger::info("PPB HIGGS interface=FAILED (no SKSE messaging)"); return; }

        HiggsMessage hm;
        msging->Dispatch(kHiggsGetInterface, static_cast<void*>(&hm), sizeof(HiggsMessage*), "HIGGS");
        if (!hm.GetApiFunction) { logger::info("PPB HIGGS interface=FAILED (HIGGS did not respond)"); return; }
        g_higgs = static_cast<Higgs::IHiggsInterface001*>(hm.GetApiFunction(1));
        if (!g_higgs) { logger::info("PPB HIGGS interface=FAILED (GetApiFunction(1) null)"); return; }

        logger::info("PPB HIGGS interface=OK build={} - grab gate live", g_higgs->GetBuildNumber());

        // ── ACTOR-GRAB DIAGNOSTIC (2026-07-28, the severed-head "grab does nothing" hunt) ──
        // Log every HIGGS grab/drop of an ACTOR ref (quiet: object grabs are ignored). This
        // gives a comparison signal: dragging a normal corpse logs GRAB — so if the severed
        // head never logs while corpses do, HIGGS is refusing the head BEFORE the held state
        // (selection/transition), not failing to move it after.
        g_higgs->AddGrabbedCallback([](bool isLeft, RE::TESObjectREFR* refr) {
            if (!refr || !refr->As<RE::Actor>()) return;
            logger::info("HIGGS GRAB [{}] actor {:08X} '{}'", isLeft ? "L" : "R",
                         refr->GetFormID(), refr->GetName());
        });
        g_higgs->AddDroppedCallback([](bool isLeft, RE::TESObjectREFR* refr) {
            if (!refr || !refr->As<RE::Actor>()) return;
            logger::info("HIGGS DROP [{}] actor {:08X} '{}'", isLeft ? "L" : "R",
                         refr->GetFormID(), refr->GetName());
        });
    }

    bool HasHiggs() { return g_higgs != nullptr; }
    Higgs::IHiggsInterface001* GetHiggs() { return g_higgs; }

    bool IsActorGrabbedByPlayer(RE::Actor* actor)
    {
        auto* h = g_higgs;
        if (!h || !actor) return false;
        for (bool isLeft : { false, true }) {
            if (!h->IsHoldingObject(isLeft)) continue;
            auto* ref = h->GetGrabbedObject(isLeft);
            if (ref && ref->GetFormID() == actor->GetFormID()) return true;
        }
        return false;
    }

    // ── SKEE (RaceMenu VR) body-morph handshake — structurally identical to AcquireHiggs above,
    // verbatim OBody-NG precedent (dispatch kExchangeInterface to "skee"; SKEE writes its
    // IInterfaceMap* into msg.interfaceMap; QueryInterface("BodyMorph"); version-guard + log). ──
    void AcquireSkee()
    {
        if (g_skeeMorph) return;  // idempotent (mirrors AcquireHiggs)

        // VR SKEE ships as skeevr.dll; the flat/SE build is skee64.dll — check both so the build
        // is correct whichever name this RaceMenu VR install carries.
        if (!::GetModuleHandleA("skee64.dll") && !::GetModuleHandleA("skeevr.dll")) {
            logger::info("PPB SKEE interface=ABSENT (skee not loaded) - Body-Scale falls back to the capsule base");
            return;
        }
        auto* msging = SKSE::GetMessagingInterface();
        if (!msging) { logger::info("PPB SKEE interface=FAILED (no SKSE messaging)"); return; }

        SKEE::InterfaceExchangeMessage msg;
        msging->Dispatch(SKEE::InterfaceExchangeMessage::kExchangeInterface,   // 0x9E3779B9
                         static_cast<void*>(&msg),
                         sizeof(SKEE::InterfaceExchangeMessage*),              // ptr-size, matches AcquireHiggs
                         "skee");
        if (!msg.interfaceMap) { logger::info("PPB SKEE interface=FAILED (skee did not respond)"); return; }

        g_skeeMorph = static_cast<SKEE::IBodyMorphInterface*>(
                          msg.interfaceMap->QueryInterface("BodyMorph"));
        if (!g_skeeMorph) { logger::info("PPB SKEE interface=FAILED (QueryInterface(\"BodyMorph\") null)"); return; }

        logger::info("PPB SKEE interface=OK version={} - Body-Scale morph reads live", g_skeeMorph->GetVersion());
    }

    bool HasSkee() { return g_skeeMorph != nullptr; }
    SKEE::IBodyMorphInterface* GetSkee() { return g_skeeMorph; }

    bool SkeeHasMorphs(RE::Actor* actor)
    {
        return g_skeeMorph && actor && g_skeeMorph->HasMorphs(actor);
    }

    // Net signed morph deviation for one BodyScale::Region (0..6). Morph values exist only AFTER
    // the 3D/skin is applied — tolerate HasMorphs==false by returning the base (0), so the caller
    // skips the region and retries on a later frame rather than baking a wrong ratio.
    float GetRegionMorph(RE::Actor* actor, int region)
    {
        auto* m = g_skeeMorph;
        if (!m || !actor || region < 0 || region >= BodyScale::kMorphRegionCount) return 0.0f;
        if (!m->HasMorphs(actor)) return 0.0f;                       // SKEE.h HasMorphs
        float net = 0.0f;
        // Read the NAKED OBody preset only (morphKey "OBody"), NOT the summed value: GetBodyMorphs sums
        // across ALL SKEE keys, folding OBody's clothed "OClothe" refit into the number, so a dressed NPC
        // reads her CLOTHED shape. OBody writes the naked preset under key "OBody" and the clothed refit
        // under "OClothe"; keying on "OBody" gives the naked body regardless of what she is wearing. A
        // slider with no OBody value returns 0 = at base (correct). Arg order: (actor, morphName, morphKey).
        for (const char* key : kRegionAdd[region]) if (key) net += m->GetMorph(actor, key, "OBody");  // additive names
        for (const char* key : kRegionInv[region]) if (key) net -= m->GetMorph(actor, key, "OBody");  // antonym names
        return net;   // ~0 => at base => caller skips the region
    }

    // ── UNMAPPED-SLIDER DETECTOR (2026-07-18b, the Lydia all-zero lesson) ────────────────────
    // Enumerates every "OBody"-keyed morph on the actor via VisitMorphValues and reports the ones
    // with real values that NO region table knows — the log line that turns "preset reads zero"
    // from a mystery into a copy-paste table extension. Main thread only (called at latch).
    int CollectUnmappedObodyMorphs(RE::Actor* actor, char* out, std::size_t outSz)
    {
        auto* m = g_skeeMorph;
        if (!m || !actor || !out || outSz < 8) return 0;
        struct V : SKEE::IBodyMorphInterface::MorphValueVisitor {
            char*       buf; std::size_t sz, len = 0; int count = 0;
            void Visit(RE::TESObjectREFR*, const char* name, const char* key, float val) override
            {
                if (!name || !key || _stricmp(key, "OBody") != 0) return;
                if (val > -0.05f && val < 0.05f) return;
                for (auto& row : kRegionAdd) for (const char* k : row)
                    if (k && _stricmp(k, name) == 0) return;
                for (auto& row : kRegionInv) for (const char* k : row)
                    if (k && _stricmp(k, name) == 0) return;
                ++count;
                if (len + 40 < sz) {
                    const int n = std::snprintf(buf + len, sz - len, "%s=%.2f ", name, val);
                    if (n > 0) len += static_cast<std::size_t>(n);
                }
            }
        } v;
        v.buf = out; v.sz = outSz; out[0] = 0;
        m->VisitMorphValues(actor, v);
        return v.count;
    }

}
