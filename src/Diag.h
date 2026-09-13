#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================================
//  Diag — the read-only performance/correctness instrument for the wave-2
//  torso-bake gating question (2026-07-08). Everything here observes the
//  CURRENTLY DEPLOYED skeleton (45 capsules, 11 list bodies, COM/spine single
//  capsules) — it bakes nothing and adds no ragdoll bodies. The one write it
//  can make is the DOUBLE-gated `capdis` disableChild spike (lodSpike + a
//  console request), which flips enable bits on the R-Thigh list already in
//  the NIF. See Report/Precision Physic Bodies Module/11_ListShape_Performance.md.
//
//  Instruments (all off until the `perf` console command arms them):
//   1. World hkpContactListener (read-only) — buckets contact points by
//      (bodyA,bodyB), tracks maxPointsInOnePair (THE SHIP GATE) + per-child rate.
//   2. Physics-step timer chained on 0xDFB722 (Hooks.cpp) — mean/p95/max step ms.
//   3. Collision-filter dump — bipedBitfields[0..23] + collision tolerance.
//   4. Runtime capsule census — per body: shape type + list-child count + geometry.
//   5. Gated disableChild spike (`capdis`).
//   6. `perf` console command — arm/reset, then dump everything + disarm.
// ============================================================================
namespace RE { class Actor; class hkpWorld; }

namespace Diag {

    // ── ENGINE-TRUTH WEAPON CONTACTS (2026-08-01) ──────────────────────────────────────
    // Havok's contact events already carry the exact weapon-vs-capsule collision the player
    // feels (haptics fire, the NPC is pushed). Reconstructing it geometrically failed on every
    // weapon whose shape is not a simple rod. So: the listener records the real contacts, and
    // the main thread resolves the body pointer to (actor, slot, side) and the shape key to the
    // capsule child. No segment, no radius, no bounding box.
    struct WeaponContact {
        void*        otherBody;   // the body the weapon hit (resolve to actor+slot by pointer)
        std::uint32_t child;      // list-child shape key = the exact capsule
        int          wand;        // 0 = right, 1 = left
        float        distHavok;   // separating distance in HAVOK metres (negative = penetrating)
    };
    void PublishWeaponBodies(void* rightBody, void* leftBody);
    int  DrainWeaponContacts(WeaponContact* out, int max);

    // ── ENGINE-TRUTH HAND CONTACTS (2026-09-07, user design — report 33 §8.3) ──────────────
    // "When my hand touch her havok capsule, ANY capsule, the engine react. It detect a contact.
    //  That's our trigger." The same listener that records weapon contacts stamps every contact
    // between a PLAYER-side collider (HIGGS hand or weapon body: layer 56 + bit 15 + part 3 = R /
    // 5 = L; a PPB finger box: part 4) and an NPC ragdoll body (bit 15 on a biped layer 8/32/33).
    // Pure integer work on the collision thread. The main thread (PpbApi::CollectEngineHandTouches,
    // EVERY frame — not apiHz) resolves the NPC body to (actor, slot, side) exactly as the weapon
    // path does and the player body to a hand; PushStep anchors hand travel on it.
    struct HandContact {
        void*         npcBody;     // the NPC ragdoll body (resolve to actor+slot+side by pointer)
        void*         playerBody;  // the player-side body (part 4 -> HandBox::HandOfBody)
        std::uint32_t child;       // the NPC list-child shape key = the exact capsule
        int           part;        // filter part of the player body: 3 R, 5 L (hand or weapon), 4 PPB box
        int           src;         // 0 HIGGS hand, 1 HIGGS weapon (pointer-matched), 2 PPB finger box
    };
    int  DrainHandContacts(HandContact* out, int max);
    // v11.1b: the ring size — pass a buffer THIS big to DrainHandContacts, or the NEWEST stamps are the ones dropped.
    constexpr int kHandContactRing = 128;
    // v11.1b census (integer-only on the collision thread): layer-56-vs-other contact events seen, the LAST such pair's
    // two filter words, and hand stamps written. The main thread prints them — the first build's classifier required
    // bit 15 on HIGGS's hand (it has none: hand.cpp:576) and produced a whole session of `anchor api` with no error line.
    void HandStampCensus(std::uint32_t& pairs, std::uint32_t& lastA56, std::uint32_t& lastB, std::uint32_t& stamps);


    // ── physics-step timing (called from Hooks::StepChainHook @0xDFB722) ──
    void OnPhysicsStep(double stepMs);   // accumulates only while armed
    bool Armed();                        // step hook gate (cheap atomic read)

    // ── frame boundary (HIGGS AddPostVrikPostHiggsCallback; NoArgCallback) ──
    void OnFrame();                      // snapshots steps-this-frame -> histogram
    void RegisterFrameCallback();        // wire OnFrame into HIGGS if present (idempotent)

    // ── per-driven-actor pre-drive tick (from ArmIK::ApplyToPoseTrack) ──
    //  When armed: registers the read-only contact listener on the actor's Havok
    //  world (idempotent per world). Always: applies any pending `capdis` spike
    //  for this actor (gated by lodSpike + a matching FormID request).
    void OnPreDrive(RE::Actor* actor);

    // ── console `perf` (zero-param toggle) ──
    //  1st press: reset counters, arm, dump census+filter for the selected NPC.
    //  2nd press: disarm, dump the full report to PPB.log + a one-line console summary.
    void TogglePerf(RE::Actor* selected, char* out, std::size_t outSz);

    // ── console `capdis <slot> <mask>` (referenceFunction=true) ──
    //  Records a deferred spike request; the flip happens on the next pre-drive
    //  fire under the world lock. mask = the DESIRED disabled-child set
    //  (bit i set -> child i disabled). mask 0 re-enables all. slot 8 = R Thigh.
    void CapDisRequest(RE::Actor* actor, int slot, std::uint32_t mask, char* out, std::size_t outSz);

    // kPreLoadGame teardown: the Havok world is rebuilt across a load — drop the
    // cached world pointer (forces re-registration) and any pending spike.
    void ClearOnLoad();
}
