#pragma once
// PpbApi — the public touch API's engine (see PpbTouchAPI.h for the consumer contract).
// Player-as-toucher revision 1: probes = the player's HandBox boxes, HIGGS weapon bodies
// and HIGGS-held objects; targets = the 12 capsule slots of the nearest driven NPCs.
// Detection is pure geometry (point-to-capsule-surface); no Havok listeners.

namespace PpbApi {

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
