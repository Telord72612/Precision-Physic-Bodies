Scriptname PPB_Native Native Hidden
{Minimal Papyrus bridge for Precision Physic Bodies. Backed by PPB.dll -> src/PapyrusNatives.cpp.}

; STATUE MODE (A-pose spell v3): while on, the DLL overwrites every bone of this actor with the
; skeleton's reference (bind) pose each frame — the deliberate "animations-failed" statue used as
; the calibration reference (straight elbows, flat wrists, planted feet).
Function SetStatuePose(Actor npc, Bool on) Global Native

; HEEL FIX (console-driven): while ON, every PLANCK-driven NPC wearing heels gets her ragdoll driven
; up to match her heel-raised visible body (touch zones, weld anchors, hit boxes all align). From the
; in-game console:   cgf "PPB_Native.ToggleHeelFix"      (or SetHeelFix 1|0)
Bool Function SetHeelFix(Bool on) Global Native
Bool Function ToggleHeelFix() Global Native

; GESTURE PAUSE (2026-08-27, with the hand-gesture device layer's move into PPB): stand the layer
; down so a scripted scene placing an actor's hands is not read as a grab. While paused, no equip,
; undress or plug gesture is detected and any in-flight gesture state is cleared.
;   PPB_Native.SetGesturePaused(True)   before a scene poses hands
;   PPB_Native.SetGesturePaused(False)  after
; The add-on's legacy VRTE_DDZaZ_Native.SetScenePaused still works and forwards here, but new
; callers should use this directly.
Function SetGesturePaused(Bool abPaused) Global Native
