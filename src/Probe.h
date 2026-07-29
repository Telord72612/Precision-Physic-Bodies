#pragma once

// ============================================================================
//  Probe — the `probe` console command's read-only state dump (2026-07-07).
//  Select an NPC, type `probe`: her full physics/animation state goes to
//  PPB.log as one clearly-labeled multi-line block. RE:: types only; zero
//  writes anywhere — pure diagnosis for the floating-NPC / heel-fix class
//  of bugs (kGetUp, bAnimationDriven, charController support, body motion).
// ============================================================================
namespace Probe {
    void DumpActorState(RE::Actor* actor);
}
