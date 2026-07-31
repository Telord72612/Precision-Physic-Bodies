# PPB Capsule API Name Record

**Generated** from `ProposedPartName()` in `NpcFingerTest.cpp` by
`tools/ppb-scratch/gen_capsule_name_record.py`, then cross-checked against the
strings in the shipped `PPB.dll`. Do not hand-edit: change the code table and re-run.

`107` named capsules across `113` children in `12` slots (`6` buried seeds).

The stable address is **(slot, side, child)**. Slots marked *sided* have an identical
left twin; unsided slots are centreline and exist once.

## Two warnings an API consumer must read first

**1. Positions come from the live body, not the NIF.** The NIF holds BASE geometry only. Every dialled capsule - including all 11 COM internal sensors (C21..C31) and the two back capsules (spine1 C9, spine2 C17) - is positioned at RUNTIME from cap* knobs in PPB_tuning.txt. In the NIF those children still sit at the buried seed spec p1=(0,0,0) p2=(0,0,2u) r=0.5u. Read positions from the live Havok body, never from the NIF.

**2. Index alignment holds only inside the named range.**

```
Names are index-aligned across all four PPB skeletons ONLY for the NAMED range of each slot. Above that range the same index is DIFFERENT anatomy per race, which is why those indices are deliberately left unnamed. Verified 2026-07-29 by classify_unnamed_children.py:
  slot 3 (head) C15+ : buried seeds on human; argonian C15/C16 = horn/spine; khajiit C15/C16 = ears; draenei C15..C22 = horns + ears. Head child count is 23 on human/draenei but 17 on argonian/khajiit.
  slot 4 (spine) C8/C9 : exist ONLY on argonian (10 children vs 8) = dorsal ridge.
  slot 5 (spine1) C7/C8 and slot 6 (spine2) C15/C16 : buried seeds on human, argonian dorsal RIDGE on skeletonbeast_female.
Never resolve an unnamed index to a human name.
```

## Slot 0 - `hand` (`NPC R Hand [RHnd]`)

> *Sided* - identical left twin on `NPC L Hand [LHnd]`. Full address is (slot, side, child).

| Child | Name | Tuning knob |
|---|---|---|
| C0 | palm rod | `capHand` |
| C1 | thumb-side rod | `capHandC1` |
| C2 | pinky-side rod | `capHandC2` |
| C3 | palm centre (1) | `capHandC3` |
| C4 | palm centre (2) | `capHandC4` |

## Slot 1 - `forearm` (`NPC R Forearm [RLar]`)

> *Sided* - identical left twin on `NPC L Forearm [LLar]`. Full address is (slot, side, child).

| Child | Name | Tuning knob |
|---|---|---|
| C0 | forearm (elbow half) | `capFore` |
| C1 | forearm (wrist half) | `capForeC1` |

## Slot 2 - `upperarm` (`NPC R UpperArm [RUar]`)

> *Sided* - identical left twin on `NPC L UpperArm [LUar]`. Full address is (slot, side, child).

| Child | Name | Tuning knob |
|---|---|---|
| C0 | upper arm (shoulder half) | `capUpper` |
| C1 | upper arm (elbow half) | `capUpperC1` |
| C2 | deltoid / shoulder cap | `capUpperC2` |
| C3 | upper arm (elbow half, inner twin) | `capUpperC3` |

## Slot 3 - `head` (`NPC Head [Head]`)

> *Centreline* - exists once, no left twin.

> **Child count differs per race:** argonian 17, draenei 23, human 23, khajiit 17

| Child | Name | Tuning knob |
|---|---|---|
| C0 | cranium | `capHead` |
| C1 | upper lip | `capHeadC1` |
| C2 | chin R | `capHeadC2` |
| C3 | chin L | `capHeadC3` |
| C4 | cheek R | `capHeadC4` |
| C5 | cheek L | `capHeadC5` |
| C6 | cheekbone R | `capHeadC6` |
| C7 | cheekbone L | `capHeadC7` |
| C8 | nose | `capHeadC8` |
| C9 | palate | `capHeadC9` |
| C10 | throat wall | `capHeadC10` |
| C11 | under-jaw (deep) | `capHeadC11` |
| C12 | temple/ear R | `capHeadC12` |
| C13 | temple/ear L | `capHeadC13` |
| C14 | back of head | `capHeadC14` |

## Slot 4 - `spine0` (`NPC Spine [Spn0]`)

> *Centreline* - exists once, no left twin.

> **Child count differs per race:** argonian 10, draenei 8, human 8, khajiit 8

| Child | Name | Tuning knob |
|---|---|---|
| C0 | waist ring | `capSpine0` |
| C1 | lower belly R | `capSpine0C1` |
| C2 | lower belly L | `capSpine0C2` |
| C3 | front waist band | `capSpine0C3` |
| C4 | lower abdomen | `capSpine0C4` |
| C5 | lower back R | `capSpine0C5` |
| C6 | lower back L | `capSpine0C6` |
| C7 | flank R | `capSpine0C7` |

## Slot 5 - `spine1` (`NPC Spine1 [Spn1]`)

> *Centreline* - exists once, no left twin.

| Child | Name | Tuning knob |
|---|---|---|
| C0 | belly / navel (FRONT) | `capSpine1` |
| C1 | belly side R | `capSpine1C1` |
| C2 | belly side L | `capSpine1C2` |
| C3 | midriff R | `capSpine1C3` |
| C4 | midriff L | `capSpine1C4` |
| C5 | flank R | `capSpine1C5` |
| C6 | flank L | `capSpine1C6` |
| C7 | _(buried seed - unnamed; may be race-specific anatomy)_ | `capSpine1C7` |
| C8 | _(buried seed - unnamed; may be race-specific anatomy)_ | `capSpine1C8` |
| C9 | BACK (lower) | `capSpine1C9` |
| C10 | _(buried seed - unnamed; may be race-specific anatomy)_ | `capSpine1C10` |

## Slot 6 - `spine2` (`NPC Spine2 [Spn2]`)

> *Centreline* - exists once, no left twin.

| Child | Name | Tuning knob |
|---|---|---|
| C0 | chest ring | `capSpine2` |
| C1 | lat R | `capSpine2C1` |
| C2 | lat L | `capSpine2C2` |
| C3 | front rib R | `capSpine2C3` |
| C4 | front rib L | `capSpine2C4` |
| C5 | lower chest | `capSpine2C5` |
| C6 | trapezius R | `capSpine2C6` |
| C7 | trapezius L | `capSpine2C7` |
| C8 | collarbone R | `capSpine2C8` |
| C9 | collarbone L | `capSpine2C9` |
| C10 | sternum / cleavage | `capSpine2C10` |
| C11 | BREAST R | `capSpine2C11` |
| C12 | BREAST L | `capSpine2C12` |
| C13 | shoulder cap R | `capSpine2C13` |
| C14 | shoulder cap L | `capSpine2C14` |
| C15 | _(buried seed - unnamed; may be race-specific anatomy)_ | `capSpine2C15` |
| C16 | _(buried seed - unnamed; may be race-specific anatomy)_ | `capSpine2C16` |
| C17 | BACK (upper) | `capSpine2C17` |
| C18 | _(buried seed - unnamed; may be race-specific anatomy)_ | `capSpine2C18` |

## Slot 7 - `neck` (`NPC Neck [Neck]`)

> *Centreline* - exists once, no left twin.

> Shape: **bhkCapsuleShape (single capsule, NOT a list)**. Addressed as child 0 by convention; the body has no list children at all.

| Child | Name | Tuning knob |
|---|---|---|
| C0 | neck / throat | `capNeck` |

## Slot 8 - `thighR` (`NPC R Thigh [RThg]`)

> *Sided* - identical left twin on `NPC L Thigh [LThg]`. Full address is (slot, side, child).

| Child | Name | Tuning knob |
|---|---|---|
| C0 | thigh rod | `capThigh` |
| C1 | upper thigh / groin | `capThighC1` |
| C2 | hip-glute fold | `capThighC2` |
| C3 | upper thigh | `capThighC3` |
| C4 | mid thigh | `capThighC4` |
| C5 | lower thigh | `capThighC5` |
| C6 | knee | `capThighC6` |

## Slot 9 - `calfR` (`NPC R Calf [RClf]`)

> *Sided* - identical left twin on `NPC L Calf [LClf]`. Full address is (slot, side, child).

| Child | Name | Tuning knob |
|---|---|---|
| C0 | calf rod | `capCalf` |
| C1 | upper calf | `capCalfC1` |
| C2 | calf belly | `capCalfC2` |
| C3 | shin (lower) | `capCalfC3` |
| C4 | knee rear | `capCalfC4` |

## Slot 10 - `footR` (`NPC R Foot [Rft ]`)

> *Sided* - identical left twin on `NPC L Foot [Lft ]`. Full address is (slot, side, child).

| Child | Name | Tuning knob |
|---|---|---|
| C0 | sole (inner) | `capFoot` |
| C1 | sole (outer) | `capFootC1` |
| C2 | arch | `capFootC2` |
| C3 | ankle lock | `capFootC3` |

## Slot 11 - `com` (`NPC COM [COM ]`)

> *Centreline* - exists once, no left twin.

| Child | Name | Tuning knob |
|---|---|---|
| C0 | orifice ring (base) | `capCom` |
| C1 | orifice ring (mid) | `capComC1` |
| C2 | orifice ring (upper) | `capComC2` |
| C3 | inner pelvis wall R | `capComC3` |
| C4 | inner pelvis wall L | `capComC4` |
| C5 | pubic mound | `capComC5` |
| C6 | groin crease R | `capComC6` |
| C7 | groin crease L | `capComC7` |
| C8 | front hip R | `capComC8` |
| C9 | front hip L | `capComC9` |
| C10 | rear centreline R | `capComC10` |
| C11 | rear centreline L | `capComC11` |
| C12 | upper glute R | `capComC12` |
| C13 | upper glute L | `capComC13` |
| C14 | inner rail R | `capComC14` |
| C15 | inner rail L | `capComC15` |
| C16 | BUTT CHEEK R | `capComC16` |
| C17 | BUTT CHEEK L | `capComC17` |
| C18 | hip R | `capComC18` |
| C19 | hip L | `capComC19` |
| C20 | rear centreline R (twin) | `capComC20` |
| C21 | CLITORIS | `capComC21` |
| C22 | vaginal opening R | `capComC22` |
| C23 | vaginal opening L | `capComC23` |
| C24 | cervix R | `capComC24` |
| C25 | cervix L | `capComC25` |
| C26 | uterus R | `capComC26` |
| C27 | uterus L | `capComC27` |
| C28 | anus R | `capComC28` |
| C29 | anus L | `capComC29` |
| C30 | rectum R | `capComC30` |
| C31 | rectum L | `capComC31` |
