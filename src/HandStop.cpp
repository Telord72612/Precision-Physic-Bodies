#include "HandStop.h"
#include "PpbTouchAPI.h"
#include "PpbApi.h"     // CopyContacts — the same snapshot the public API publishes
#include "Tuning.h"     // ObjectHold::HandStopMode()

#include <cmath>
#include <cstdio>

namespace logger = SKSE::log;

namespace {

// One penetration episode per hand: from the first frame inside a capsule to release.
struct HandEpisode {
    bool   inside      = false;
    double enteredS    = 0.0;
    float  maxDepthU   = 0.f;    // deepest penetration seen this episode (positive number)
    double lastLogS    = 0.0;
    char   part[48]    = {};     // the capsule currently deepest
    unsigned int actor = 0;
};
HandEpisode g_ep[2];             // [0]=right, [1]=left (PpbTouchContact::wand)

double NowS()
{
    using clk = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               clk::now().time_since_epoch()).count();
}

const char* SrcName(unsigned char k)
{
    switch (k) {
    case PPBAPI::kSourceFinger: return "FINGER";
    case PPBAPI::kSourcePalm:   return "PALM";
    case PPBAPI::kSourceFist:   return "FIST";
    case PPBAPI::kSourceHand:   return "HAND";
    default:                    return "?";
    }
}

}   // namespace

namespace HandStop {

void OnFrame()
{
    // ── knob + arm receipt ───────────────────────────────────────────────────────────────
    static int s_lastMode = -1;
    const int mode = (int)(ObjectHold::HandStopMode() + 0.5f);
    if (mode != s_lastMode) {
        s_lastMode = mode;
        logger::info("HANDSTOP mode -> {} ({})", mode,
                     mode == 0 ? "off" : mode == 1 ? "PROBE (log only, no writes)"
                                                   : "unknown - only 0/1 exist in stage 1");
        if (mode == 0) { g_ep[0] = HandEpisode{}; g_ep[1] = HandEpisode{}; }
    }
    if (mode != 1) return;

    // ── read this frame's contact snapshot (published a moment ago by PpbApi::OnFrame) ───
    PPBAPI::PpbTouchContact c[64];
    const int n = PpbApi::CopyContacts(c, 64);
    const double now = NowS();

    for (int h = 0; h < 2; ++h) {
        // Deepest HAND-CLASS penetration for this hand. Weapons are deliberately out of
        // stage 1 — the user's spec is the pushing hand; the weapon clamp is a later knob.
        float deepest = 0.f;
        const PPBAPI::PpbTouchContact* best = nullptr;
        for (int i = 0; i < n; ++i) {
            if (c[i].wand != h) continue;
            if (c[i].sourceKind > PPBAPI::kSourceHand) continue;   // finger/palm/fist/hand only
            if (c[i].distU >= 0.f) continue;                       // negative = inside
            const float d = -c[i].distU;
            if (d > deepest) { deepest = d; best = &c[i]; }
        }

        HandEpisode& e = g_ep[h];
        if (best) {
            if (!e.inside) {
                e = HandEpisode{};
                e.inside = true; e.enteredS = now; e.actor = best->actorFormId;
                std::snprintf(e.part, sizeof e.part, "%s", best->bodyPart);
                logger::info("HANDSTOP {} CONTACT: {:.2f}u into '{}' of {:08X} (src={}) "
                             "-- stage 2 would clamp the visible hand at this surface",
                             h ? "L" : "R", deepest, best->bodyPart, best->actorFormId,
                             SrcName(best->sourceKind));
                e.lastLogS = now;
            } else if (now - e.lastLogS >= 0.5) {
                e.lastLogS = now;
                logger::info("HANDSTOP {} holding: {:.2f}u into '{}' (episode max {:.2f}u, {:.1f}s)",
                             h ? "L" : "R", deepest, best->bodyPart,
                             deepest > e.maxDepthU ? deepest : e.maxDepthU, now - e.enteredS);
            }
            if (deepest > e.maxDepthU) e.maxDepthU = deepest;
            if (std::strcmp(e.part, best->bodyPart) != 0)
                std::snprintf(e.part, sizeof e.part, "%s", best->bodyPart);
        } else if (e.inside) {
            logger::info("HANDSTOP {} released: was {:.1f}s inside, max depth {:.2f}u (last '{}')",
                         h ? "L" : "R", now - e.enteredS, e.maxDepthU, e.part);
            e = HandEpisode{};
        }
    }
}

}   // namespace HandStop
