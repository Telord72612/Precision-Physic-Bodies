#pragma once
#include <cstdint>

// ============================================================================
//  RayTel — MEASURED raycast / query telemetry against PPB list bodies
//  (2026-08-22). Closes the [I] INFERRED gap in
//  Report/Precision Physic Bodies Module/08_Contact_And_Performance.md §3.5:
//  "the actual ray count per frame was never measured."
// ----------------------------------------------------------------------------
//  MECHANISM — five vtable slots, ZERO code bytes written.
//
//  bhkCollisionFilter derives from hkpCollisionFilter, which multiply-inherits
//  the four Havok filter interfaces (hkpCollisionFilter.h:27-31). MSVC therefore
//  emits FIVE vtables for it; CommonLibVR ships all five as
//  RE::bhkCollisionFilter::VTABLE[0..4] (VR 0x18292D8 / 0x1829300 / 0x1829318 /
//  0x1829340 / 0x1829358, RTTI-confirmed '.?AVbhkCollisionFilter@@', thisOff
//  0x00 / 0x10 / 0x18 / 0x20 / 0x28 — verified against the shipped exe's .rdata,
//  which is NOT DRM-encrypted).
//
//  Havok calls two of those interfaces ONCE PER CHILD of a shape collection:
//    * hkpRayShapeCollectionFilter::isCollisionEnabled — "called for every new
//      child-shape of a hkpShapeCollection to be considered for ray-casting"
//      (hkpRayShapeCollectionFilter.h:18). This is the castRayImpl child walk.
//    * hkpShapeCollectionFilter::isCollisionEnabled (5-arg) — "For each sub-shape
//      of the shape collection that needs to be tested against the shape the
//      filter is called" (hkpShapeCollectionFilter.h:21-22). This is the
//      hkpListAgent -> hkpShapeCollectionAgent walk that HIGGS's per-frame sphere
//      LinearCasts and PLANCK's player-proxy manifold rebuild go through.
//  Counting calls to those two IS counting children walked. The two per-BODY
//  filters (hkpRayCollidableFilter, hkpCollidableCollidableFilter) give the
//  denominators: rays x bodies, and broadphase pairs.
//
//  WHY NOT a code detour on hkpListShape::castRayImpl (0xA9C5F0): the shipped
//  SkyrimVR.exe is Steam-DRM packed and .text is CIPHERTEXT on disk, so the
//  prolog bytes a write_branch would clobber CANNOT be verified — rule 7. A
//  vtable slot is 8-byte aligned .rdata: arm/disarm is a naturally atomic store,
//  and DISARMED restores the original pointer, so "off" is byte-identical to
//  vanilla. Zero cost when off is literal, not approximate.
//
//  HARD RULE 1 COMPLIANCE: the thunks run on Havok collision threads. They do
//  pure integer work — relaxed uint64 atomics, integer pointer compares, and
//  __rdtsc() (an integer intrinsic, no XMM). No floats, no SIMD, no logging, no
//  allocation, no locks. Every conversion and every log line happens on the MAIN
//  thread in OnFrame().
// ============================================================================
namespace RayTel {

    // Main-thread frame tick: reads the `rayTel` knob, arms/disarms on the edge,
    // and emits the ~1 Hz report line. Called from PerfSys's HIGGS
    // PostVrikPostHiggs lambda. Instant no-op while the knob is 0 and we are
    // not armed (one float read + two compares).
    void OnFrame();

    // kPreLoadGame / kNewGame: zero the counters (the window is meaningless
    // across a load). The vtable patch itself lives in the EXE image and is
    // unaffected by a save load, so it is left alone.
    void ClearOnLoad();

    // TRUE while the five slots hold our thunks.
    bool Armed();
}
