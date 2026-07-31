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
