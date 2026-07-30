#pragma once
#include <cstdint>

// ============================================================================
//  NpcFingerTest — NPC FINGER TEST COLLIDER v2 (2026-07-09).
//  4 runtime DYNAMIC capsules on the RIGHT hand of ONE selected/nearest driven
//  NPC (Finger10→Finger12 chords, index/middle/ring/pinky, no thumb), soft
//  velocity-driven onto the live bone chord, colliding ONLY with the player's
//  HIGGS hand / wielded-weapon bodies. Built from ppb-scratch/
//  spec2_npcfinger.md AS AMENDED BY review2_npcfinger.md (FINGER_BUILD_PLAN).
//  Inert until npcFingerEnable > 0 or the `nfing` console command pins an NPC.
//
//  Header exposes only void*/POD (the Markers.h discipline) — the .cpp is the
//  SECOND Havok-SDK TU in PPB.dll (CMake scopes the SDK include path to it).
// ============================================================================
namespace RE { class Actor; }

namespace NpcFinger {

    // Per driven actor from the 0xB266AB pre-drive chain (PPBHook), main
    // thread, before the chained driveToPose — the same seam v1's
    // FingerCapTrack reads finger world transforms at (proven fresh).
    // Creation, per-frame guards, and the velocity drive all live here.
    // deltaTime = ApplyToPoseTrack's frame dt (the drive's invDt source).
    void OnPreDrive(RE::Actor* actor, float deltaTime);

    // Frame boundary (HIGGS PostVrikPostHiggs, via PerfSys's lambda): frame
    // counter + stale-actor sweep + retarget/knob-off destroys. All destroys
    // fresh-resolve the actor's CURRENT world — the stored bhkWorld* is
    // COMPARE-ONLY and is never passed to any remove/lock (review Defect 1).
    void OnFrame();

    // 2026-07-25: actively remove parked visualization markers (knob-off = undo, not stop)
    void ClearMeshMarkers(RE::Actor* anyActor);

    // Console `nfing`: pin/unpin the SELECTED actor; returns the new state
    // (true = pinned). Only flips the pin — actual create/destroy happens at
    // the hook seams above (the N14 discipline).
    bool RequestToggle(RE::Actor* sel);

    // 0=Continue 1=Collide 2=Ignore; pure bit math, physics-thread-safe
    // (relaxed atomics only — 2 loads + compares when no rig is live).
    // Spliced at the TOP of PerfSys::FilterCB.
    int  FilterDecision(std::uint32_t infoA, std::uint32_t infoB);

    // Contact-driven finger CURL (2026-07-16): fills out[4] with the finger rig's
    // EMA-smoothed per-finger curl factors (index/middle/ring/pinky, 0..1) for the
    // actor carrying the live tbl-0 rig. Returns false (out untouched past the copy)
    // for every other actor, or when all four fingers are fully open. MAIN THREAD
    // ONLY (the 0xB266AB pre-drive seam — same thread that runs the integrator).
    bool GetFingerCurl(std::uint32_t actorId, float out[4]);

    // ── PPB TOUCH API exports (2026-07-30) ──────────────────────────────────────────────
    // PartName: the shipped body-part name for (slot, child) on the human-female reference
    // map — wraps the file-local ProposedPartName table. nullptr = unnamed/race-specific.
    const char* PartName(int slot, int child);
    // WeaponPointU: world position (game units) of HIGGS's wielded-weapon collision body
    // for that hand. false = no weapon body (nothing wielded / HIGGS absent).
    bool WeaponPointU(bool left, float outU[3]);

    // MESH-BAND MARKERS (2026-07-18, Route B calibration): up to 7 visible GHOST capsules at
    // the girth-defining vertices so the user verifies band placement by eye. Part-29 layer-56
    // words — FilterDecision ignores every pair; keyframe-still via zero gravity + tiny mass.
    // Main thread; the live world is resolved fresh from the actor each call.
    void UpdateMeshMarkers(RE::Actor* actor, const float posU[][3], int count);

    // kPreLoadGame: the world is still alive at PRE-load — remove properly
    // (fresh-resolve + pointer-compare gate), then drop all pointers.
    void ClearOnLoad();
    // CLONE BODY STRIP (2026-07-26): remove every collision body of this actor from the
    // Havok world EXCEPT the named node's (e.g. "NPC COM [COM ]"). Main thread only; the
    // actor must already be PLANCK-ignored + PPB-excluded. Returns bodies removed, -1 = no
    // 3D/world. Bodies leave the world but are never freed (wrapper refcounts own them).
    int StripActorBodiesExcept(RE::Actor* a, const char* keepNodeName);
    // v4 clone treatment: collapse every capsule to a 0.01u point at its bone; the anchor
    // node's first capsule becomes a head-sized ball (the graspable severed head).
    int ShrinkActorBodiesIntoHead(RE::Actor* a, const char* headAnchorNode);
    // Severed head: disable its constraints and park every non-anchor body ON the anchor, so the
    // head is a single ball with nothing orbiting it. Bodies are per-actor — safe to write.
    int ParkBodiesAtAnchor(RE::Actor* a, const char* anchorNode, int passNum = 1);
    // Severed head: clear the anchor's ragdoll sub-layer so HIGGS's hand box can contact it
    // (Report 09 §6 — hand sub-layer 3/5 skips any body whose sub-layer differs by exactly 1).
    // Returns 1 = fixed, 0 = already reachable, -1 = anchor/world not found.
    int FixHeadGrabFilter(RE::Actor* a, const char* anchorNode);
    // Diagnostic: fill outWrappers/outNames with every collision body under a 3D root (n = count).
    void CountActorBodies(RE::NiAVObject* root, void** outWrappers, const char** outNames,
                          int& n, int cap);
}
