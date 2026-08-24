#pragma once
#include <cstdint>

// ============================================================================
//  Orifice — NATIVE ORIFICE DRIVE (2026-08-19).
//
//  WHAT IT IS. PPB already KNOWS when something is inside an NPC (the pelvic
//  sensor ladder, slot 11 children 22..31, and the mouth gate). Until now that
//  knowledge was only ever REPORTED. This module makes the body answer: the
//  vaginal / anal bone rings are pushed open by whatever is actually in them —
//  finger, held object, weapon, wand — OUTSIDE any scene. Today that only
//  happens during a scene, driven by another mod (Penetration Physics / PPA).
//
//  WHY A WRITE IS SAFE HERE, AND WHY IT MUST BE ABSOLUTE. The winning
//  skeleton_female.hkx (Pandora Behaviour Engine Plus) contains NEITHER "Anus"
//  NOR "Pussy": the behaviour graph cannot pose these bones, so nothing ever
//  overwrites what we write and nothing ever restores it either. That cuts both
//  ways —
//    * a write PERSISTS, so it only has to be made once per frame, and
//    * a write is FOREVER unless WE undo it.
//  Therefore every write is ABSOLUTE (rest + offset), NEVER accumulated: the
//  bone's rest translation is captured on the first frame of a contact and
//  restored on release, on knob-off, on scene handoff, on kPreLoadGame and on
//  kNewGame. An actor is never left deformed. This is the single hard rule of
//  the module.
//
//  BONES + DIRECTIONS come from the live PPA toml (accurate-penetration.toml)
//  so the two mods deform the same rings the same way; see the tables in the
//  .cpp. All 8 exist in PPB/skeleton_female.nif. The MALE sculpt carries the 4
//  Anus2 bones and NONE of the 4 vaginal ones, so the male body gets anal +
//  oral by EXISTENCE — no gender gate, no race gate, just a node lookup that
//  either resolves or does not.
//
//  Header exposes POD only (the Markers.h discipline).
// ============================================================================
namespace RE { class Actor; }

namespace Orifice {

    // Which orifice. Also the index into the published API byte.
    enum Kind : int { kVaginal = 0, kAnal = 1, kOral = 2, kKindCount = 3 };

    // Per driven actor from the 0xB266AB pre-drive chain (PPBHook::ApplyToPoseTrack),
    // main thread, before the chained driveToPose — the same seam NpcFinger::OnPreDrive
    // uses, where node world transforms are proven fresh. Senses (probe vs the pelvic
    // sensor ladder), eases, and writes the bone rings. Inert until orificeEnable.
    // deltaTime = the drive's frame dt.
    // ⚠ The PLAYER never reaches this seam (PPBHook.cpp excludes her above the chain),
    // so revision 1 is NPC-only by construction.
    void OnPreDrive(RE::Actor* actor, float deltaTime);

    // ── THE TWO TEARDOWNS THAT DO NOT DEPEND ON THE DRIVE SEAM ──────────────────
    // OnPreDrive is reached through per-actor gates (DismemberGuard's exclusion, the
    // driven set itself). An actor that stops arriving while a ring is armed would
    // keep her offset for the life of the 3D — the module's one hard rule broken by
    // a gate it does not own. So:
    //
    //  ReleaseActor — call it at the MOMENT an actor becomes excluded (PPBHook calls
    //    it from the DismemberGuard gate), while her 3D is still live. One last frame
    //    to put rest back. Safe on an actor we never touched.
    void ReleaseActor(RE::Actor* actor);
    //  Sweep — the general form: from the HIGGS frame callback (main thread, behind no
    //    per-actor gate), restore and drop anything not seen at the drive seam for ~0.5s.
    void Sweep();

    // The mouth gate's edge, funnelled from PpbApi::EmitMouthStage — the ONE mouth
    // signal in PPB (multi-capsule AND, per-race child sets, finger-only). This module
    // does NOT run a second mouth test and does NOT write MFG (GhostTouchFrame already
    // owns the phoneme ramp); it only turns the gate's stages into a published openness.
    // stage: 0 = LIPS, 1 = IN MOUTH, 2 = THROAT REACHED.
    void NoteMouthStage(std::uint32_t actorId, int stage, bool entered);

    // Published openness of one orifice, 0..1 (0 = at rest). 0 for an unknown actor.
    // Main-thread read (the touch API's publish path).
    float Openness(std::uint32_t actorId, int kind);

    // kPreLoadGame AND kNewGame: restore every actor we are still holding open (the
    // world is alive at PRE-load, so the nodes are still there), then drop the whole
    // FormID-keyed map — a recycled FormID in a different save must never inherit a
    // previous save's captured rest pose.
    void ClearOnLoad();
}
