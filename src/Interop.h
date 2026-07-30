#pragma once

namespace RE { class Actor; }
namespace Higgs { struct IHiggsInterface001; }
namespace SKEE { class IBodyMorphInterface; }   // fwd-decl (like Higgs::IHiggsInterface001) — full def in SKEE.h

// ── BODY SCALE regions (2026-07-14) ─────────────────────────────────────────
// The body-shape regions the Body-Scale system sizes capsule groups to. 0..6 are
// OBody/SKEE MORPH regions (read via Interop::GetRegionMorph); kHead (7) is a MESH
// measure (CapFix owns it) — listed here only so the shared cache is sized to cover it.
namespace BodyScale {
    enum Region : int {
        kChest = 0, kBreasts, kBelly, kWaist, kButt, kThighs, kUpperArms,  // 0..6 morph regions
        kHead,                                                             // 7   mesh-measured
        kRegionCount
    };
    static constexpr int kMorphRegionCount = 7;   // regions 0..6 are morph-driven
}

// Minimal HIGGS interop (ported from AIHands 2026-07-07) for the PivFix grab
// gate: acquire the HIGGS interface via the SKSE messaging handshake, then
// POLL IsHoldingObject/GetGrabbedObject per driven actor (the proven pattern —
// AIHands' callback-based flag was dead code; polling is what shipped).
namespace Interop {
    void AcquireHiggs();                        // idempotent; call at kPostPostLoad + kDataLoaded
    bool HasHiggs();
    Higgs::IHiggsInterface001* GetHiggs();      // nullptr until acquired
    // Sets HIGGS's SelectedCloseFingerAnimMaxHandSpeed to -1 through HIGGS's own settings API
    // so the hand stays OPEN near grabbables and PPB's finger colliders can actually poke.
    // Idempotent; knob higgsPokeFix. Call at kDataLoaded AND after every game load (HIGGS
    // re-reads its ini on a settings reload). whenTag only labels the one-time log line.
    void ApplyHiggsPokeFix(const char* whenTag);

    // TRUE while either player hand HIGGS-holds this actor (a grabbed NPC limb reports the NPC
    // refr through GetGrabbedObject — HIGGS State::HeldBody covers held bodies). Two virtual
    // calls per hand — negligible at the 1 Hz PivFix cadence. false when HIGGS is absent.
    bool IsActorGrabbedByPlayer(RE::Actor* actor);

    // ── SKEE (RaceMenu VR) body-morph interface (2026-07-14, Body-Scale) ─────────────────────
    // Acquired via the SAME idempotent SKSE messaging handshake as HIGGS (OBody-NG precedent);
    // call at kPostPostLoad + kDataLoaded. GetRegionMorph returns the net SIGNED morph deviation
    // for a BodyScale::Region (0..6): sum(additive keys) - sum(antonym keys), 0 = at the zero-
    // slider capsule base. 0.0f whenever SKEE is absent or the actor has no morphs yet (skip).
    void AcquireSkee();
    bool HasSkee();
    SKEE::IBodyMorphInterface* GetSkee();
    bool  SkeeHasMorphs(RE::Actor* actor);          // true once the actor's OBody/skin morphs are applied
    float GetRegionMorph(RE::Actor* actor, int region);   // region = BodyScale::Region 0..6
    int   CollectUnmappedObodyMorphs(RE::Actor* actor, char* out, std::size_t outSz);  // 07-18b: names no table knows
}
