Scriptname PPB_Touch Native Hidden
{Precision Physic Bodies - public touch API (polling side). Backed by PPB.dll -> src/PpbApi.cpp.

FIVE VARIABLES per contact:
  WHO       GetContactActor(i)      - the touched NPC
  WHERE     GetContactBodyPart(i)   - the named capsule ("BREAST L", "R upper thigh", ...)
  BY WHO    the player (always, in this revision - NPC touchers are planned)
  WITH WHAT GetContactSource(i)     - "FINGER" / "PALM" / "FIST" / "HAND" / "GRAB"
                                      / "WEAPON:<name>" / "OBJECT:<name>"
  DURATION  GetContactDuration(i)   - seconds since this contact began

POLLING: GetContactCount() then index 0..count-1. The list is a snapshot refreshed at the
apiHz rate (ships at 4/s; it is a host knob in PPB_tuning.txt, do not assume a
rate). Indices are NOT stable across polls - read what
you need in one pass.

EVENTS (push, no polling needed) - register a ModEvent handler:
  "PPB_TouchStart"  numArg = surface distance in units (negative = inside the capsule)
  "PPB_Touch"       numArg = distance, re-sent at apiHz while the contact holds
  "PPB_TouchEnd"    numArg = the contact's total DURATION in seconds
  sender = the touched NPC. strArg = "WAND|SOURCE|BODYPART|SKELETON" (split on "|").

COVERAGE: PPB drives female NPCs of mapped races (human catch-all, Argonian, Khajiit,
Draenei + PPB_Skeletons_Added_Race.ini). Males, children and creatures never appear here.}

; number of live contacts in the current snapshot
Int Function GetContactCount() Global Native

; the touched NPC (None if the index is stale or the actor unloaded)
Actor Function GetContactActor(Int i) Global Native

; named body part, side-prefixed on limb slots ("L thigh rod"); "<slot>.C<n>" if unnamed
String Function GetContactBodyPart(Int i) Global Native

; "FINGER" / "PALM" / "FIST" / "HAND" / "GRAB" / "WEAPON:<name>" / "OBJECT:<name>"
String Function GetContactSource(Int i) Global Native

; which of the player's hands: "L" or "R" (weapon/object contacts follow their hand)
String Function GetContactWand(Int i) Global Native

; "human" / "argonian" / "khajiit" / "draenei"
String Function GetContactSkeleton(Int i) Global Native

; seconds since this contact began (-1 if the index is stale)
Float Function GetContactDuration(Int i) Global Native

; current surface distance in game units; negative = penetrating
Float Function GetContactDistance(Int i) Global Native

; the full "WAND|SOURCE|BODYPART|SKELETON" string (same format as the mod events)
String Function GetContactPacked(Int i) Global Native

; -- the SUB-REGION layer (added 2026-07-31) -------------------------------------------
; Region is the coarse bucket the digest groups by: "Face" / "Neck" / "Chest" / "Belly" /
; "Waist" / "Pelvis" / "Intimate" / "Arm" / "Hand" / "Leg" / "Foot" / "Tail" / "Hair".
String Function GetContactRegion(Int i) Global Native

; The finer bucket: "Head", "Face surface", "Mouth opening", "In mouth", "Mouth wall",
; "Breast", "Glute", "Intimate - vaginal (deep)", "Tail - tip (far third)", ...
; Cheeks and chins report "Face surface" - they are ordinary face touches on their own and
; only signify the mouth in conjunction. The PALATE reports "In mouth": a touch there means
; something IS inside, and it outranks any lip reading. The THROAT reports "Mouth wall" and
; outranks even the palate.
String Function GetContactSubRegion(Int i) Global Native

; The ladder as a number, when you only care HOW FAR IN and not where:
;   0 = surface (skin, a face touch, a hip)
;   1 = opening (lips, vaginal/anal opening)
;   2 = inside  (palate, cervix, rectum)
;   3 = deepest (throat wall, uterus)
; -1 if the index is stale.
Int Function GetContactDepth(Int i) Global Native

; -- the fields a MOD EVENT cannot carry (added 2026-08-01) ----------------------------
; A Papyrus mod event gives one packed string and ONE number, so an event-only consumer
; cannot see depth, weapon class or provenance, and can never have distance AND duration
; at the same time. FindContact bridges that: from inside your event handler, resolve the
; contact and then read whatever you need, every value separate and typed.
;
;   Event OnPpbTouchStart(String eventName, String strArg, Float numArg, Form sender)
;       Int i = PPB_Touch.FindContact(sender as Actor, "")   ; "" = either hand
;       If i >= 0
;           If PPB_Touch.GetContactDepth(i) >= 2             ; inside
;               ; ... and duration, distance, weapon class are all available here
;           EndIf
;       EndIf
;   EndEvent
;
; asWand is "L" or "R"; pass "" to match either hand. Returns -1 if the contact has already
; ended -- which is normal in a PPB_TouchEnd handler, so read what you need on Start, or use
; the event's own numArg for the final duration.
Int Function FindContact(Actor akWho, String asWand) Global Native

; "Sword" / "Dagger" / "War axe" / "Mace" / "Greatsword" / "Battleaxe / warhammer" /
; "Bow" / "Staff" / "Crossbow" / "Fist"; "" when the source is not a weapon.
String Function GetContactWeaponClass(Int i) Global Native

; "Blade" / "Blunt" / "Pierce"; "" when not a weapon.
; NOTE: Skyrim has no damage-type field -- this is inferred from the weapon class, not read
; from the record. Battleaxes and warhammers share one animation type and both report Blade.
String Function GetContactWeaponEdge(Int i) Global Native

; True  = the game's own physics reported this collision (exact capsule, true depth).
; False = PPB measured proximity, which deliberately includes hover/near-misses.
; Only check this if you specifically want to ignore hover.
Bool Function GetContactIsEngine(Int i) Global Native
