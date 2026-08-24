#pragma once
// PpbApi — the public touch API's engine (see PpbTouchAPI.h for the consumer contract).
// Player-as-toucher revision 1: probes = the player's HandBox boxes, HIGGS weapon bodies
// and HIGGS-held objects; targets = the 12 capsule slots of the nearest driven NPCs.
// Detection is pure geometry (point-to-capsule-surface); no Havok listeners.

namespace PpbApi {
    // ── MOUTH GATE bridge (2026-07-31, VRTE report 16 §4.3 / R4) ────────────────────────
    // The mouth gate in NpcFingerTest.cpp is the AUTHORITATIVE mouth signal: a multi-capsule
    // AND (palate + both cheeks within gate), per-race child sets for beast heads, and a
    // finger-only test so a fist mashed into the face cannot open her. It was log-only. The
    // nearest-capsule race can never express that logic — it picks ONE capsule — so consumers
    // wanting "is something in her mouth" must read this, not a capsule name.
    // stage: 0 = LIPS, 1 = ENTER, 2 = THROAT REACHED.  entered = true on rise, false on fall.
    void EmitMouthStage(RE::Actor* actor, int stage, bool entered, int hand, float distU);

    // ── PROBE EXPORT (2026-08-19, for Orifice.cpp) ──────────────────────────────────────
    // A read-only copy of the probes CollectProbes already assembled this tick: the player's
    // hand boxes (with the held-hand suppression and the VRIK index exception already
    // applied), the HIGGS weapon blade segment, and the held-object bound-box segment. Any
    // consumer that needs "what is the player poking with, and where" reads THIS rather than
    // re-deriving it — one assembly, one set of fixes, no second source of truth to drift.
    // Main thread only. Returns the number copied (<= max, <= 14).
    // 2026-08-23: 14, not 12 — the PLAYER GENITAL WAND's 2 segments are appended after the
    // hand set (see CopyProbes). Pass a 14-slot buffer; a 12-slot one still works and simply
    // drops the wand.
    struct ProbeView {
        float p[3];     // probe point, or the segment start (world game units)
        float q[3];     // segment end (seg only)
        float pad;      // extra surface: object bound radius / weapon capsule radius. 0 for
                        // the bare hand boxes — they ARE the fingertip.
        int   seg;      // 1 = the probe is the SEGMENT p->q, 0 = the point p
        int   cls;      // 0 = hand box, 1 = weapon, 2 = held object, 3 = player genital wand
                        // (3 is export-only: it is not a hand probe, never enters the per-hand
                        //  set, and so never appears in the published touch-API contact list)
        int   wand;     // 0 = player's RIGHT hand, 1 = LEFT. Meaningless for cls 3 (reads 0).
        // The actor THIS hand is HIGGS-grabbing (0 = none). The held-hand suppression and the
        // sheathed-weapon gate are already baked into the probe set, but the GRAB MUTE is not:
        // it is per-TARGET (the weapon is muted against the actor being held and stays live
        // against everyone else), so it cannot be applied at export. Every consumer MUST drop a
        // cls==1 probe whose grabActorId equals the actor it is testing — exactly as
        // PpbApi::ScanActor does (PpbApi.cpp:891-892). Otherwise the recorded phantom (an axe
        // riding the grip of the hand holding her leg, "cervix -16.9u for 13.6s") is read as a
        // real contact.
        unsigned int grabActorId;
    };
    int CopyProbes(ProbeView* out, int max);

    // Per-frame roster: OnPreDrive announces every driven actor (same-frame use only —
    // the pointer is consumed by OnFrame later in the SAME frame, the g_gt.best pattern).
    void NoteDriven(RE::Actor* actor);

    // The tick: runs on the HIGGS PostVrikPostHiggs frame callback (main thread), throttled
    // to the apiHz knob. Scans, updates the contact table, fires mod events + callbacks,
    // publishes the Papyrus snapshot.
    void OnFrame();

    // SKSE plugin-message handler (RegisterListener(nullptr, ...) in SKSEPlugin_Load):
    // answers PPBAPI::PpbMessage::kGetTouchInterface from any plugin.
    void OnPluginMessage(SKSE::MessagingInterface::Message* msg);

    // Papyrus natives (class "PPB_Touch") — registered from Natives.cpp.
    bool RegisterNatives(RE::BSScript::IVirtualMachine* vm);

    // Save/load hygiene: drop every live contact (no events fired for the dead ones).
    void ClearOnLoad();
}
