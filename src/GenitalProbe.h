#pragma once

// ═══════════════════════════════════════════════════════════════════════════════════════════
// GENITAL PROBE (2026-08-02) — RESEARCH INSTRUMENT ONLY. Reads nothing, writes nothing,
// creates no collision. It exists to answer ONE question before any male-body capsule code
// is written:
//
//     WHICH SIGNAL TRUTHFULLY REPORTS "this male's schlong is currently exposed"?
//
// Why it is needed: the XP32 superset PPB ships carries the full SOS bone chain
// (NPC GenitalsBase [GenBase], NPC Genitals01..06, NPC GenitalsScrotum, L/R GenitalsScrotum)
// on the FEMALE skeleton as well as the male, so bone presence is NOT a sex test and NOT a
// dress test. Candidate signals, all logged side by side so one VR session settles it:
//
//   nodes    — are the Genitals* nodes in the actor's 3D tree at all?
//   geom     — is there GEOMETRY skinned under/near that chain (the actual schlong mesh)?
//   hidden   — is that geometry flagged NiAVObject::Flag::kHidden (SOS's likely hide path)?
//   bodyArmo — is a body-slot (32) armor equipped, and does it carry SOS_Revealing?
//
// The user dresses/undresses a male NPC in VR; whichever column flips in lockstep with what
// they SEE is the signal the future rig may trust. Ships OFF (genProbe 0).
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace RE { class Actor; }

namespace GenitalProbe {
    // Per driven actor, from the main-thread pre-drive seam. Cheap no-op unless genProbe is on.
    // Throttled internally (~1 Hz per actor) and edge-logged: a line is emitted on the FIRST
    // sight of an actor and thereafter ONLY when the signal vector CHANGES — so a dress/undress
    // shows up as exactly one line, and standing still produces none.
    void Tick(RE::Actor* actor);

    // ── ★ THE EXPOSURE GATE (2026-08-23) — the measured signal, finally WIRED ─────────────
    //     exposed == skin->HasPartOf(52) && !GetWornArmor(52)
    // Doc 22 §5, measured 2026-08-17 on a live Imperial. The two halves mean different things:
    //   * skin->HasPartOf(52) = CAPABILITY — "TNG assigned this actor a schlong" (skin
    //     TNG_Skin_B07). Reads 1 dressed AND naked, so it is NOT an exposure test on its own.
    //   * GetWornArmor(52)    = STATE — 1 dressed, 0 naked, because TNG tags *covering*
    //     garments with slot 52. (The inline comment at the probe's own slot-52 block calls
    //     this column one "expected never to fire"; the measurement disproved that.)
    // Why this gate and no other: gate on the same authority that CREATES the thing. If TNG
    // fails there is no schlong, so our capsule failing identically is correct by construction.
    //
    // ⚠ WITHOUT this, bone existence was the de-facto gate — and PPB ships the dormant SOS
    // chain on EVERY skeleton (female, _1stperson, "and on a bear"), so a load order with no
    // TNG/SOS at all got a full GEN rig on every male NPC and an invisible wand on the player.
    // Exactly the phantom-rig class the tbl-5 skin gate exists to prevent.
    //
    // MALES ONLY — females read slot 52 FLAT in both states (TNG gates them behind
    // TNG_Gentlewoman), and their orifice side uses AND + slot 32 instead. A female actor
    // therefore answers false here, which is correct for every caller (GEN rig and the player
    // wand are both male-only by construction).
    // Main thread only. Cached per actor with a short TTL: GetWornArmor deep-copies the whole
    // worn inventory into a std::map, which is fine at the probe's ~1 Hz and NOT fine per frame.
    bool IsExposed(RE::Actor* actor);
}
