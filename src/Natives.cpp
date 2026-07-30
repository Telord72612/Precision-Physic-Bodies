#include "Natives.h"
#include "PPBHook.h"   // ArmIK::SetStatuePose
#include "Tuning.h"    // ObjectHold::SetHeelFix / ToggleHeelFix
#include "PpbApi.h"    // PPB_Touch natives (the public touch API)

namespace logger = SKSE::log;

// ════════════════════════════════════════════════════════════════════════════
//  PPB_Native — minimal Papyrus bridge for the Precision Physic Bodies split.
//
//  These three natives moved off AIHands_Native (the split): the A-pose statue
//  spell calls PPB_Native.SetStatuePose now, and the legacy heel-fix cgf path
//  calls PPB_Native.SetHeelFix / ToggleHeelFix (console `hf` stays the primary
//  control). GetGrabArmed and the give natives stay on AIHands_Native.
// ════════════════════════════════════════════════════════════════════════════
namespace PapyrusNatives {
    constexpr const char* kClassName = "PPB_Native";

    // STATUE MODE — the A-pose spell v3 backend: bind-pose statue on/off per actor.
    static void SetStatuePose(RE::StaticFunctionTag*, RE::Actor* npc, bool on) {
        ArmIK::SetStatuePose(npc, on);
    }

    // HEEL FIX — console-driven (user rule: runtime switches are in-game, never file edits):
    //   cgf "PPB_Native.ToggleHeelFix"      flips it, with an on-screen confirmation.
    //   cgf "PPB_Native.SetHeelFix" 1|0     sets it explicitly.
    // While ON, every PLANCK-driven NPC wearing heels gets her ragdoll driven up to the visual skeleton.
    // The console prints cgf's RETURN VALUE ("... returned True/False") — that IS the on-screen confirmation.
    static bool ToggleHeelFix(RE::StaticFunctionTag*) {
        const bool on = ObjectHold::ToggleHeelFix();
        logger::info("HeelFix toggled {} (console)", on ? "ON" : "OFF");
        return on;
    }
    static bool SetHeelFix(RE::StaticFunctionTag*, bool on) {
        ObjectHold::SetHeelFix(on);
        logger::info("HeelFix set {} (console)", on ? "ON" : "OFF");
        return on;
    }

    bool Register(RE::BSScript::IVirtualMachine* vm) {
        if (!vm) return false;
        vm->RegisterFunction("SetStatuePose", kClassName, SetStatuePose);
        vm->RegisterFunction("ToggleHeelFix", kClassName, ToggleHeelFix);
        vm->RegisterFunction("SetHeelFix",    kClassName, SetHeelFix);
        logger::info("Papyrus natives registered: {}.SetStatuePose / ToggleHeelFix / SetHeelFix.", kClassName);
        PpbApi::RegisterNatives(vm);   // PPB_Touch.GetContact* (the public touch API)
        return true;
    }
}
