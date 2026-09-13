Scriptname PPB_DeviceEquip Hidden
; ================================================================
; PPB - the Papyrus half of the hand-gesture device layer.
;
; MOVED HERE 2026-08-27 from the VRTE DD-ZaZ AddOn (VRTE_DDZaZ_Equip.psc lines
; 25-156), BEHAVIOUR VERBATIM. Only the identity changed: the script name, and
; the unlock return event VRTE_DDZaZ_Unlocked -> PPB_GestureUnlocked. Every
; guard, message and comment below is the original, because each one records a
; bug that was paid for once already.
;
; The gesture DETECTION moved into PPB.dll because PPB owns the contact data the
; gestures are built on. The add-on keeps narration, its restraint registry, and
; the DoVibrate / DoShock / DoCombatStun half of the old script.
;
; SOFT DEPENDENCY ON DEVIOUS DEVICES: only DoUnequipDevice touches zadlibs, and
; it resolves DD through GetFormFromFile, which returns None when DD is absent -
; the function then bails with a notification. Nothing else in PPB references
; DD, so a load order without Devious Devices runs this script harmlessly.
; ================================================================

; WHY THIS SCRIPT EXISTS (2026-08-22): the DLL cannot pass OBJECTS into a
; dispatched Papyrus call.  The VM types a packed reference as its most-derived
; ATTACHED script (a held DD plug arrived as `zadPlugScript`) and the external-
; dispatch type check refuses to upcast script types to Form:
;   "ObjectReference.AddItem(Form,...) received incompatible arguments!
;    Received types (zadPlugScript,int,bool)"  - Papyrus.0.log, 15:48:00
; Papyrus itself upcasts these fine - so the DLL passes three raw FormIDs as
; Ints and THIS script resolves and calls.  Plain-value static dispatch is the
; proven-working path (Debug.Notification uses it).
;
; The body is Gift by Hand's armor path, verbatim in spirit
; (BeVR_S_HiggsHook.psc:436-480):
;   AddItem(the held REFERENCE)  - moves it, never copies -> no duplication
;   EquipItemEx(base, 0, false, true) - DD's own zadLibs.LockDevice call;
;       fires the full OnEquipped chain, which equips the RENDERED half
;   QueueNiNodeUpdate() - the 3D refresh
;
; heldId may be 0 / dead (another mod consumed the world ref): fall back to
; a count-guarded AddItem of the base so she has exactly one, then equip.
; ================================================================
; preventUnequip (2026-09-03, Tier 0 of the permanent-equip research): 1 for ORDINARY gear,
; so the AI's own equipment evaluation and a plain UnequipItem cannot take it off. 0 for a
; Devious Devices item - DD equips its inventory half with false and its rendered half with
; true inside its own OnEquipped (zadEquipScript.psc:234), and forcing true here would fight
; that pairing. The DLL decides which; this script only obeys. Default 0 keeps every older
; caller's behaviour byte-identical.
Function DoEquip(Int actorId, Int heldId, Int baseId, Int preventUnequip = 0) Global
    Actor akTarget = Game.GetFormEx(actorId) as Actor
    Form  akBase   = Game.GetFormEx(baseId)
    if !akTarget || !akBase
        return
    endif
    ObjectReference akHeld = None
    if heldId != 0
        akHeld = Game.GetFormEx(heldId) as ObjectReference
    endif
    if akHeld
        akTarget.AddItem(akHeld, 1, true)
    elseif akTarget.GetItemCount(akBase) == 0
        akTarget.AddItem(akBase, 1, true)
    endif
    akTarget.EquipItemEx(akBase, 0, preventUnequip != 0, true)
    akTarget.QueueNiNodeUpdate()
EndFunction
; The failed-gesture eject: a give (e.g. Gift by Hand, if enabled) swallowed a
; device that did NOT equip - put it back into the world at her feet.
Function DoEject(Int actorId, Int baseId, Int count) Global
    Actor akTarget = Game.GetFormEx(actorId) as Actor
    Form  akBase   = Game.GetFormEx(baseId)
    if akTarget && akBase && count > 0
        akTarget.DropObject(akBase, count)
    endif
EndFunction

; ================================================================
; The two-hand undress (2026-08-22, user design): both hands grab the SAME
; worn piece, the later hand pulls away, the piece comes off.
;
; This function is the DD branch only — generic armor is unequipped natively
; by the DLL. DD devices must go through zadlibs or their scripts re-equip
; them, and the user's rule stands: locked / quest devices RESIST with a
; message ("key matters"). zadlibs itself enforces the same contract via
; genericonly=true in UnlockDeviceByKeyword (zadlibs.psc:507-513).
;
; On success we tell the DLL which INVENTORY device to hand the player:
; SendModEvent("PPB_GestureUnlocked", <inventory FormID as string>, leftHand)
; with the ACTOR as sender. The DLL then drops it at the pulling hand and
; HIGGS-grabs it into that hand.
; ================================================================
Function DoUnequipDevice(Int actorId, Int wornId, Float leftHand, Float fromFinger = 0.0) Global
    Actor a = Game.GetFormEx(actorId) as Actor
    Armor rendered = Game.GetFormEx(wornId) as Armor
    if !a || !rendered
        return
    endif
    zadlibs libs = Game.GetFormFromFile(0x00F624, "Devious Devices - Integration.esm") as zadlibs
    if !libs
        Debug.Notification("Devious Devices is not responding.")
        return
    endif
    ; The user's difficulty rule, enforced FIRST and loudly.
    if rendered.HasKeyword(libs.zad_QuestItem) || rendered.HasKeyword(libs.zad_BlockGeneric)
        Debug.Notification("It is locked on tight - you don't have the key.")
        return
    endif
    ; ★ LOCKED DEVICES NEED THE KEY (user, 2026-08-24): "can we make a device
    ; that require a key not removable and just do a 'missing a key to remove
    ; this device' when i pull on it? Like a screen message saying that."
    ;
    ; zad_Lockable is DD's own marker for "this closed with a lock", and it
    ; sits on the RENDERED half - the half we are holding - so this costs one
    ; keyword test and needs nothing from the inventory side.
    ; Bare hands do not pick a lock. Use DD's own menu with the key.
    ; ⚠ A FINGER EXTRACTION IS EXEMPT (user, 2026-08-24): "you can lock a plug in
    ; the orifice, you need a chastity belt on top." A finger working a plug
    ; loose is not picking a lock, so the lock keyword is the wrong barrier for
    ; it - the DLL checks for a chastity belt instead, before it ever calls here.
    ; The two-hand pull still needs the key.
    if fromFinger == 0.0 && rendered.HasKeyword(libs.zad_Lockable)
        Debug.Notification("You are missing the key to remove this device.")
        return
    endif
    ; Which device CLASS is this? The rendered half carries the class keyword;
    ; zadDeviceTypes (088C6D) is DD's own authoritative 19-entry class list.
    FormList types = Game.GetFormFromFile(0x088C6D, "Devious Devices - Integration.esm") as FormList
    Keyword classKw = None
    Int n = 0
    if types
        n = types.GetSize()
    endif
    Int i = 0
    while i < n && !classKw
        Keyword k = types.GetAt(i) as Keyword
        if k && rendered.HasKeyword(k)
            classKw = k
        endif
        i += 1
    endwhile
    if !classKw
        Debug.Notification("The device will not come loose.")
        return
    endif
    ; Bank the inventory half BEFORE unlocking (after, it is no longer "worn").
    Armor idevice = libs.GetWornDevice(a, classKw)

    ; ⛔⛔ NEVER UNLOCK WITHOUT THE INVENTORY HALF (2026-08-24). This is why a
    ; blindfold and a ball gag VANISHED when they were pulled off.
    ;
    ; GetWornDevice can return None for a device she is visibly wearing - a
    ; second rip on an already-removed piece, or a device whose pairing DD
    ; cannot resolve. The old code went straight on to UnlockDeviceByKeyword
    ; anyway, and DD then ran its whole removal against a None:
    ;     Error: Cannot call GetName() on a None object    zadLibs.psc:413
    ;     Error: Cannot call HasKeyword() on a None object  zadLibs.psc:417
    ;     Error: Cannot check to for a None item to be equipped
    ; Measured four times in one session, and every one of them is a device the
    ; player never saw again: DD tore down half the pairing, and with idevice
    ; None we sent no unlock event, so nothing was ever handed back.
    ;
    ; If we cannot name the half we are meant to give back, we do not touch it.
    ; She keeps wearing it, which is recoverable; the alternative is not.
    if !idevice
        Debug.Notification("That device will not come loose.")
        Debug.Trace("[PPB] REFUSED to unlock on " + a.GetDisplayName() \
            + ": GetWornDevice returned None for the worn class - not calling " \
            + "UnlockDeviceByKeyword, because DD would run it against a None and " \
            + "the device would be lost.")
        return
    endif

    if libs.UnlockDeviceByKeyword(a, classKw, false)
        a.SendModEvent("PPB_GestureUnlocked", idevice.GetFormID() as String, leftHand)
    else
        Debug.Notification("It is locked on tight - you don't have the key.")
    endif
EndFunction

; ================================================================
; HOLD POOL (2026-09-06): make a GENERIC NPC persistent by holding her in one of
; PPB_HoldPoolQuest's 32 empty reference aliases (0x806 in Precision Physic Bodies.esp).
; A quest alias is the engine's own persistence mechanism (DoM: EssentialRef.ForceRefTo);
; a held reference is skipped by the cell respawn and its inventory is save state.
; Plain ints in, the DLL never passes objects (the VM refuses the upcast).
; ================================================================
Function DoHold(Int questId, Int aliasIdx, Int actorId) Global
    Quest q = Game.GetFormEx(questId) as Quest
    Actor a = Game.GetFormEx(actorId) as Actor
    if !q || !a
        return
    endif
    ReferenceAlias ra = q.GetAlias(aliasIdx) as ReferenceAlias
    if ra
        ra.ForceRefTo(a)
    endif
EndFunction

Function DoRelease(Int questId, Int aliasIdx) Global
    Quest q = Game.GetFormEx(questId) as Quest
    if !q
        return
    endif
    ReferenceAlias ra = q.GetAlias(aliasIdx) as ReferenceAlias
    if ra
        ra.Clear()
    endif
EndFunction

; ================================================================
; SEVERACTIONS / NFF HANDOFF (2026-09-06, user: "if SA has control of that NPC, record to
; its outfit system" + "do NFF probe"). Called by the DLL after a confirmed ORDINARY equip
; (mode 1) or a plain rip (mode 2), with the piece's base FormID. ONE OWNER PER NPC:
;   1. SeverActions CONTROLS an outfit when the actor is a registered follower, is not
;      outfit-excluded, and has an active preset or the outfit lock on - its alias re-applies
;      that preset/lock on every load and would strip our piece, so the worn set is re-saved
;      INTO it (GiftByHandSN's GBH_Outfit pattern). SeverActions.esp 0xD62 = its god quest.
;   2. Nether's Follower Framework controls an outfit when the follower carries an outfit
;      SLOT and currently wears one of its custom outfit types (see NffRecord).
;   3. Anyone else stays on the engine path (preventUnequip + the hold pool).
; Soft dependencies: an absent framework is skipped by Game.GetModByName before any of its
; script types is touched.
; ================================================================
Function DoOutfitRecord(Int actorId, Int mode, Int baseId = 0) Global
    Actor a = Game.GetFormEx(actorId) as Actor
    if !a
        return
    endif
    if Game.GetModByName("SeverActions.esp") != 255 && SaControlsOutfit(a)
        SaRecord(a, mode)
        return
    endif
    if Game.GetModByName("nwsFollowerFramework.esp") != 255
        if NffRecord(a, mode, baseId)
            return
        endif
    endif
    ; no framework owns her outfit: the C++ side already did the engine path (preventUnequip + hold pool)
    Debug.Trace("[PPB] DoOutfitRecord mode=" + mode + ": no framework controls " + a.GetDisplayName() + " - engine path (preventUnequip + hold pool)")
EndFunction

Bool Function SaControlsOutfit(Actor a) Global
    if !SeverActionsNativeExt.Native_GetIsFollower(a)
        return false
    endif
    if SeverActionsNative.Native_GetOutfitExcluded(a)
        return false
    endif
    String preset = SeverActionsNative.Native_Outfit_GetActivePreset(a)
    Bool presetOn = (preset != "") || SeverActionsNative.Native_OutfitSlot_IsPresetActive(a)
    Bool lockOn = SeverActionsNativeExt.Native_Outfit_IsLockActive(a)
    return presetOn || lockOn
EndFunction

Function SaRecord(Actor a, Int mode) Global
    String preset = SeverActionsNative.Native_Outfit_GetActivePreset(a)
    Bool presetOn = (preset != "") || SeverActionsNative.Native_OutfitSlot_IsPresetActive(a)
    Bool lockOn = SeverActionsNativeExt.Native_Outfit_IsLockActive(a)
    SeverActions_Outfit sev = Game.GetFormFromFile(0x000D62, "SeverActions.esp") as SeverActions_Outfit
    if !sev
        return
    endif
    if presetOn
        if preset != ""
            sev.SaveOutfitPreset_Execute(a, preset)
        else
            Debug.Trace("[PPB] DoOutfitRecord: preset active but unnamed on " + a.GetDisplayName() + " - not re-saved")
        endif
    endif
    if lockOn
        sev.SnapshotLockedOutfit(a)
    endif
    Debug.Trace("[PPB] DoOutfitRecord mode=" + mode + ": SeverActions preset/lock re-saved for " + a.GetDisplayName() + " (preset='" + preset + "' lock=" + lockOn + ")")
EndFunction

; ================================================================
; NETHER'S FOLLOWER FRAMEWORK (2026-09-06, user: "do NFF probe"). NFF's outfit system is a
; WARDROBE: a managed follower carries an outfit SLOT (rank in nwsFF_storedFac) and, per outfit
; type (rank in nwsFF_OutTypeFac: 0 default / 1 town / 2 home; >= 3 = her base outfit), a chest
; whose contents NFF copies into a custom LeveledItem + Outfit record and applies with SetOutfit
; (nwsFollowerSetsScript.switchOutfit) - so she wears ENGINE-GENERATED copies of what sits in
; the chest, re-issued on every load. Recording INTO NFF therefore means: move the physical
; piece into the chest of her current outfit type (equip) or take its copy out of that chest
; (rip), then let NFF re-apply. UNTESTED in VR while NFF is disabled in this load order.
; nwsFollowerSets quest = 0x0487C1 in nwsFollowerFramework.esp (carries nwsFollowerSetsScript).
; ================================================================
Bool Function NffRecord(Actor a, Int mode, Int baseId) Global
    if baseId == 0
        return false
    endif
    nwsFollowerSetsScript sets = Game.GetFormFromFile(0x0487C1, "nwsFollowerFramework.esp") as nwsFollowerSetsScript
    if !sets || !sets.nwsFF_storedFac || !sets.nwsFF_OutTypeFac
        return false
    endif
    if !a.IsInFaction(sets.nwsFF_storedFac)
        return false
    endif
    Int slot = a.GetFactionRank(sets.nwsFF_storedFac)
    Int wornType = a.GetFactionRank(sets.nwsFF_OutTypeFac)
    if slot < 0 || wornType < 0 || wornType > 2
        return false
    endif
    ObjectReference chest = None
    if wornType == 2
        chest = sets.nwsFFHomeChestList.GetAt(slot) as ObjectReference
    elseif wornType == 1
        chest = sets.nwsFFTownChestList.GetAt(slot) as ObjectReference
    else
        chest = sets.nwsFFOutfitChestList.GetAt(slot) as ObjectReference
    endif
    Form base = Game.GetFormEx(baseId)
    if !chest || !base
        return false
    endif
    if mode == 1
        a.RemoveItem(base, 1, true, chest)
    else
        chest.RemoveItem(base, 1, true, None)
    endif
    sets.switchOutfit(a, wornType)
    Debug.Trace("[PPB] DoOutfitRecord mode=" + mode + ": NFF outfit chest (slot " + slot + " type " + wornType + ") updated for " + a.GetDisplayName() + " and re-applied")
    return true
EndFunction
