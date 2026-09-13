Scriptname PPB_Invis extends ActiveMagicEffect
{PPB invisibility TOGGLE: cast on an NPC to hide her mesh (SetAlpha 0) so the collision/CBPC/bone
 visualizers are unobstructed; cast again to reveal. StorageUtil latch, per-actor.}

Event OnEffectStart(Actor akTarget, Actor akCaster)
    If akTarget == None
        Return
    EndIf
    Int cur = StorageUtil.GetIntValue(akTarget, "PPB_Invis")
    If cur == 0
        akTarget.SetAlpha(0.0, true)
        StorageUtil.SetIntValue(akTarget, "PPB_Invis", 1)
        Debug.Notification("PPB: NPC INVISIBLE (cast again to reveal)")
    Else
        akTarget.SetAlpha(1.0, true)
        StorageUtil.SetIntValue(akTarget, "PPB_Invis", 0)
        Debug.Notification("PPB: NPC visible")
    EndIf
EndEvent
