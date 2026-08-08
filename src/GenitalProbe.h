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
}
