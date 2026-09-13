#pragma once

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  PushStep — sustained hand pressure on an NPC's trunk makes them STEP AWAY, engine-native.
//
//  THE USER'S SPEC (2026-08-29, verbatim): "FBG stops the trigger, but it's what after the
//  trigger that I want — the motion of the NPC to back away... I want it to do that until the
//  pressure is 0 from the player hand for 1 sec." And: grabbing+dragging an NPC makes her do
//  "little steps toward wherever she got pulled — THIS is what I want, BUT pushed instead."
//
//  THE MECHANISM (PushWalk, PLANCK's grabbed-drag repurposed): trunk pressure + a measured
//  bone displacement ≥ pushStepDispU engages planner-direct-control (MoveToHigh /
//  SetPlannerDirectControl / SetMotionDriven at MovementControllerNPC+0x128/+0x140), feeds
//  SetTargetSpeed/Direction/Angle per frame from the 0x5E0885 chain hook, and walks a distance
//  PROPORTIONAL to the displacement (× pushStepDispGain, capped per region). Exit clears the
//  planner and defers EvaluatePackage via task.
//
//  ⛔ THE BUMP FALLBACK IS GONE (2026-08-29). The hand-built type-32 package (PLANCK's
//  GetBumpedEx replica, 13 raw engine calls) froze the game the first time it fired on an
//  animation-driven actor (22:27:38, last line before silence; the task's first receipt never
//  printed). Animation-driven actors now RESIST instead of stepping. Do not rebuild it without
//  a root cause for that freeze. FBG's 'PSHB' receiver still exists (dormant, harmless).
// ═════════════════════════════════════════════════════════════════════════════════════════

namespace PushStep {
    // Per-frame detection tick. Called from PerfSys's frame lambda AFTER PpbApi::OnFrame.
    // Inert unless the `pushStep` knob is non-zero.
    void OnFrame();

    // Called from the 0x5E0885 chain hook (Hooks.cpp): the engine's per-actor motion-vs-
    // animation re-evaluation. Returns true when PushWalk drove this actor — the chained
    // original (and PLANCK's hook) must then be skipped for this call, exactly as PLANCK
    // skips the original for its grabbed actors.
    bool OnMotionDrivenCheck(RE::Actor* actor);

    // v7: is PushWalk driving this actor RIGHT NOW? Consumed by Hooks' movement-params
    // override (the PLANCK OverwriteMovementParameters replica) to scope the substitution.
    bool IsDrivingActor(std::uint32_t actorFormId);

    // ★ v29d EQUIP SETTLE (user 2026-09-11: "An equip event prevent ragdoll or stumble"). An equip changes her
    // GEOMETRY — high heels raise both feet ~8 u — and the lift/push sensors would read that as her leaving the
    // floor or being shoved. For pushStepEquipSettleS after one, no lift, stagger, ragdoll or walk may fire; at
    // the END of that window her current height becomes the new floor, for the feet AND every sensed trunk node.
    // Call it from every equip and every removal PPB itself performs.
    void NoteEquipEvent(std::uint32_t actorFormId);

    // v11.1b: kPreLoadGame teardown — drop the engine hand-travel anchors (FormID-keyed; the review's major finding).
    void ClearOnLoad();
}
