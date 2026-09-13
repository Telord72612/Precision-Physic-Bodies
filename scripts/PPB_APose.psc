Scriptname PPB_APose extends ActiveMagicEffect
{ PPB STATUE TOGGLE (v3, 2026-07-04): cast -> the NPC becomes a bind-pose statue — the exact
  stance the engine shows when animations fail to load (straight elbows, flat wrists, planted feet),
  produced deliberately by the DLL overwriting every bone with the skeleton's reference pose each
  frame. RECAST on the same NPC -> released. The old v2 drove an "A-pose" through the arm IK, which
  bent elbows/wrists — unusable as a calibration reference. Naked test spell (no watchdogs). }

Event OnEffectStart(Actor akTarget, Actor akCaster)
    If akTarget == None
        Debug.Notification("PPB Statue: no target")
        Return
    EndIf
    If StorageUtil.GetIntValue(akTarget, "PPB_APose", 0) == 1
        ; second cast = toggle OFF: the running instance sees the flag drop and releases her
        StorageUtil.SetIntValue(akTarget, "PPB_APose", 0)
        Return
    EndIf
    StorageUtil.SetIntValue(akTarget, "PPB_APose", 1)
    ; Kill the MOTIVE to move (AI-warp teleport defense): top-priority vanilla MQ105DoNothing package
    ; override (runnable, unconditional, InterruptFlags=0) + SetDontMove as the second lock.
    Package doNothing = Game.GetFormFromFile(0x0006AF41, "Skyrim.esm") as Package
    If doNothing != None
        ActorUtil.AddPackageOverride(akTarget, doNothing, 100, 1)
        akTarget.EvaluatePackage()
    EndIf
    akTarget.SetDontMove(True)
    PPB_Native.SetStatuePose(akTarget, True)
    Debug.Notification("PPB Statue ON (recast to release)")
    While StorageUtil.GetIntValue(akTarget, "PPB_APose", 0) == 1
        Utility.Wait(0.5)
    EndWhile
    PPB_Native.SetStatuePose(akTarget, False)
    Package doNothing2 = Game.GetFormFromFile(0x0006AF41, "Skyrim.esm") as Package
    If doNothing2 != None
        ActorUtil.RemovePackageOverride(akTarget, doNothing2)
    EndIf
    akTarget.SetDontMove(False)
    akTarget.EvaluatePackage()
    Debug.Notification("PPB Statue OFF")
EndEvent
