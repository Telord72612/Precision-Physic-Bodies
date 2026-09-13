#pragma once

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  HandStop — the player's visible hand STOPS at an NPC's body surface (PCVR-style clamp,
//  aimed at PPB's capsules).
//
//  THE GOAL (user, 2026-08-28): "Me pushing a NPC with my hand, my hand moving 2u or 3u, not
//  going further cause the NPC IS an obstacle, even if my controller IS going further...
//  exactly what Physical Collision VR do, but on our Precision Physic Bodies." Resistance in
//  VR = the visual hand holding at the surface while the controller continues.
//
//  PCVR already ships this clamp (settings.ini: fHandRadiusCm=4.0, bSlide=1, and
//  bBodyStopsHands=1 for its OWN 19 player parts) but its filter ignores biped layers, so it
//  stops on tables and never on people. PPB owns the NPC-side geometry PCVR can't see.
//  Longer term PCVR becomes a FOMOD-optional soft dependency: its physics, our obstacles.
//
//  STAGE 1 (this file, `handStop 1`): PROBE ONLY. Log per hand the deepest penetration into
//  any driven NPC's capsule — entry edge, 2 Hz while inside, exit edge with the episode max.
//  Zero writes to the game. Validates the depth/part numbers stage 2 will clamp against.
//  STAGE 2 (`handStop 2`, not yet built): the positional clamp with slide.
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace HandStop {
    // Per-frame tick. Called from PerfSys's frame lambda AFTER PpbApi::OnFrame so the contact
    // snapshot it reads is this frame's. Inert unless the `handStop` knob is non-zero.
    void OnFrame();
}
