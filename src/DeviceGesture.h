#pragma once
#include "PCH.h"     // RE::Actor for the restraint registry below
#include <string>

// =============================================================================
// DeviceGesture — DD device gestures. Insert a plug, release, it equips.
//
// HISTORY: this file was `OrificeOpen.{h,cpp}` and used to drive the gape bones
// itself. As of 2026-08-20 **PPB owns orifice opening for all three orifices**
// (`Orifice.{h,cpp}` in PPB.dll, report 23 final section), so the entire drive
// path — bone tables, rest capture, easing, PPA arbitration — has been deleted.
// What survives is the half that was always ours: DD semantics.
//
// THE BOUNDARY (the user's, and it is right)
//   PPB owns bodies, capsules, collision and now the orifices.
//   This addon owns Devious Devices: what a held item IS, where it may go, and
//   what equipping or removing it means. PPB is consumed through its published
//   API and is never modified.
//
// WHAT THIS DOES
//   * Identifies the device in the player's hand by KEYWORD EDITOR ID — DD via
//     the zadequipscript.zad_DeviousDevice property on the held reference, ZaZ
//     via zbfWorn* keywords on the base — so no FormID and no load-order index
//     is ever baked in. A device carries a site MASK (a ZaZ cuff set is
//     wrist AND ankle).
//   * Detection is PPB's CONTACT STREAM (§14 rewrite, 2026-08-21): the actor,
//     capsule, depth and orifice verdict all come from GetContacts(). No actor
//     enumeration, no geometry of our own — the first build re-derived both and
//     was wrong while PPB was right, in the same frames.
//   * Requires a deliberate gesture — inserted / pressed, held for a real
//     dwell, then released — because the user's rule is that a missed equip is
//     far better than an accidental one.
//   * Equips via ActorEquipManager on DD's INVENTORY device, which is what DD's
//     own zadLibs.LockDevice equips; DD's zadEquipScript::OnEquipped then does
//     all the real work. ZaZ items are plain armors and take the same call as
//     a plain equip.
//
// COVERAGE follows PPB: a contact only exists for actors PPB drives, so the
// coverage gate is implicit. Anyone else is a silent no-op, never a guess.
// =============================================================================

namespace DeviceGesture {
    // Acquire PPB + HIGGS and arm the frame hook. Idempotent — called at
    // kPostPostLoad and retried at kDataLoaded. Missing PPB or HIGGS logs one
    // line and leaves the module permanently inert.
    void Install();

    // Per-frame tick. Called from PerfSys's frame callback AFTER PpbApi::OnFrame, so the contact
    // snapshot it reads is this frame's.
    void OnFrame();

    // Forget all in-flight gestures. Call on kPreLoadGame and kNewGame.
    void Reset();

    // Scene suppression, mirroring PpbBridge::SetPaused.
    void SetPaused(bool paused);
}
