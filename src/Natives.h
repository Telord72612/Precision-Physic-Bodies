#pragma once
#include "PCH.h"

// PPB_Native — the Papyrus bridge for Precision Physic Bodies. Ships the three natives
// the split moves off AIHands_Native: SetStatuePose (the A-pose statue spell backend),
// SetHeelFix, ToggleHeelFix. Backed by PPB_Native.psc/.pex in the mod's Scripts folder.
namespace PapyrusNatives {
    bool Register(RE::BSScript::IVirtualMachine* vm);
}
