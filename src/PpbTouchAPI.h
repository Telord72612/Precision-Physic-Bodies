#pragma once
// ═══════════════════════════════════════════════════════════════════════════════════════
//  PpbTouchAPI.h — the PUBLIC touch interface of Precision Physic Bodies (PPB).
//
//  Copy this single header into your project. It is deliberately self-contained: no
//  CommonLib types, no SKSE types beyond the messaging call you already make, actors are
//  addressed by FormID. Works from any SKSE library (CommonLibSSE, CommonLibVR, classic
//  skse64) because nothing here depends on one.
//
//  ── WHAT YOU GET ─────────────────────────────────────────────────────────────────────
//  PPB rebuilds NPC Havok bodies (female AND male since 2.0) as 12 body slots of named
//  collision capsules (~148 per NPC), live-fitted to each NPC's actual mesh. This interface reports CONTACT:
//  who was touched, where (named body part), by which hand, with what (finger / open palm
//  / fist / HIGGS grab / weapon / held object), how close/deep, and for how long.
//
//  Detection is pure geometry against bodies PPB owns (point-to-capsule-surface distance).
//  No Havok contact listeners are involved, so there is no physics cost to consuming this.
//
//  ── ACQUIRING THE INTERFACE ──────────────────────────────────────────────────────────
//  The same request/reply pattern HIGGS and PLANCK use. Any time at or after kPostLoad:
//
//      PPBAPI::PpbMessage msg{};
//      SKSE::GetMessagingInterface()->Dispatch(PPBAPI::PpbMessage::kGetTouchInterface,
//                                              &msg, sizeof(msg), "PPB");
//      if (msg.GetApiFunction)
//          g_ppb = static_cast<PPBAPI::IPpbTouchInterface1*>(msg.GetApiFunction(1));
//
//  A null GetApiFunction means PPB is not installed (or too old). A null return from
//  GetApiFunction(N) means PPB does not speak revision N — ask for a lower one.
//
//  ── VERSIONING CONTRACT (read this before extending) ─────────────────────────────────
//  * The vtable below is APPEND-ONLY. Methods are never reordered, removed, or changed
//    in signature. A revision bump adds methods at the END, or a new IPpbTouchInterface2.
//  * There is deliberately NO virtual destructor: slot 0 is GetBuildNumber, forever.
//    (The HDT-SMP v1/v2 split taught us what a destructor-at-slot-0 mismatch does: the
//    engine calls your destructor once per event. Not here.)
//  * PpbTouchContact is a fixed-size POD and is also append-only via the reserved tail.
//
//  ── COVERAGE CONTRACT (important) ────────────────────────────────────────────────────
//  PPB drives NPCs of mapped races, BOTH SEXES since 2.0: the human catch-alls (covering
//  elf/orc/etc on a typical load order), Argonian, Khajiit, Draenei (female), plus anything
//  the user adds to PPB_Skeletons_Added_Race.ini. Children and creatures are NOT covered —
//  and neither is anyone at maleGeometry 0 or on an unmapped custom skeleton — all of whom
//  answer IsDriven() = false: route those to your fallback (e.g. CBPC), never assume.
//  Part names are sex- and skeleton-routed (male tables, beast-head tables); consume the
//  name strings, don't assume the female reference map on every actor.
//
//  ── THREADING CONTRACT ───────────────────────────────────────────────────────────────
//  * All interface methods are MAIN-THREAD ONLY, and cheap (snapshot reads).
//  * Touch callbacks fire on the MAIN thread, at most at the configured event rate
//    (apiHz, which SHIPS AT 4/s - it is a host knob, do not assume a rate). Do not
//    block in them.
//  * Papyrus: PPB also fires mod events (below) and exposes polling natives
//    (script "PPB_Touch"), both safe from any Papyrus context.
//
//  ── PAPYRUS MOD EVENTS — TWO STREAMS, pick the one that suits you ────────────────────
//  DIGEST (recommended — grouped, dwell-filtered, one event per region visit):
//      "PPB_TouchStart"   numArg = surface distance in game units (negative = inside)
//      "PPB_Touch"        numArg = distance, re-sent at apiHz while contact holds
//      "PPB_TouchEnd"     numArg = the contact's total DURATION in seconds
//    BODYPART reads "Region(longest part)" e.g. "Face(cheek L)". One contact per
//    (actor, wand, REGION): wandering across five face capsules is ONE event, and the
//    part reported is whichever you spent longest on. Changing your hand pose mid-touch
//    (finger -> fist) does NOT restart it either; SOURCE reports the pose you held longest.
//
//  RAW (verbose — every capsule, every source class, no grouping):
//      "PPB_TouchRawStart" / "PPB_TouchRaw" / "PPB_TouchRawEnd"
//    Same strArg shape, BODYPART is the bare capsule name. Use this if you are building
//    your own aggregation. Host can disable either stream (apiEvents / apiRawEvents).
//
//  sender = the touched NPC (Actor). strArg is a '|'-packed string, split on '|':
//      "WAND|SOURCE|BODYPART|SKELETON"
//       WAND     ∈ L / R
//       SOURCE   ∈ FINGER / PALM / FIST / HAND / GRAB / WEAPON:<name> / OBJECT:<name>
//       BODYPART = the named capsule, side-prefixed on sided slots ("R BREAST R" never
//                  happens — centreline names carry their own side; limb slots get
//                  "L thigh rod" style prefixes). Unnamed children fall back to
//                  "<slot>.C<n>" so a touch is never silently dropped.
//       SKELETON ∈ human / argonian / khajiit / draenei
//  ('|' cannot appear in item names in practice; split on the FIRST three '|' if you
//   want to be bulletproof against exotic OBJECT names.)
// ═══════════════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════════════
//  THE GESTURE EVENT BUS  (added 2026-08-27 with the gesture layer's move into PPB)
// ═══════════════════════════════════════════════════════════════════════════════════════
//  The touch API above answers "what is touching what, right now". These SKSE mod events
//  answer a different question: "a hand just DID something to a worn item." They are part
//  of the same published contract and change under the same rules.
//
//  All are SKSE ModCallbackEvents. sender = the ACTOR the gesture happened to (never the
//  player's hand, even when the player's hands did it). Fields are '|'-separated.
//  Consumers must tolerate EXTRA trailing fields: this bus appends, it does not renumber.
//
//  ── OUTBOUND, the five PPB emits ─────────────────────────────────────────────────────
//   PPB_GestureUndressArm      "<capsule>"
//        Both hands have taken the same worn piece and a pull is armed. A consumer that
//        narrates touches should SUPPRESS grab narration on this actor until End.
//
//   PPB_GestureUndressEnd      "<name>|<slotMask>|<done>|<isDD>|<capsule>|<class>"
//        The pull resolved. ⚠ FIRES ON CANCEL TOO (done=0): a hand let go, the actor
//        changed, the piece stopped being worn. The Arm/End PAIR is load-bearing — a
//        consumer that only handles done=1 will silence that actor for the session.
//
//   PPB_GesturePlug            "<in|out>|<name>|<class>|<siteMask>|<leftHand>"
//        THE PLUG GESTURE, both edges, and only the gesture:
//          "in"  a plug was worked into an orifice by hand and equipped
//          "out" a fingertip worked a plug loose (fires just BEFORE the removal)
//        PLUGS ONLY, across every device family. A device qualifies when its DD class is
//        one of the three plug rows (…DeviousPlug / …DeviousPlugVaginal / …DeviousPlugAnal),
//        OR when it carries no DD class at all but its site lands on an orifice — that second
//        arm is what covers ZaZ, Diary of Mine and zRavenous plugs, which have no DD keyword.
//        A gag or a vaginal piercing sits at an orifice SITE but is not a plug and does NOT
//        appear here; use PPB_GestureDeviceEquipped's siteMask for those.
//        <class> is the empty string for a non-DD plug — it genuinely has no class name.
//        <siteMask> is always real: both edges resolve it from the same source the gesture
//        used to find the plug in the first place.
//        A plug leaving by menu, key, or another mod's script does NOT appear here — by
//        design. This event means a HAND did it.
//        ⚠ "out" is emitted just before the removal, and Devious Devices can still refuse it
//        after that point for two reasons PPB cannot see without zadlibs (no class keyword in
//        zadDeviceTypes; GetWornDevice returning None). The common refusal — a quest or
//        block-generic device — is caught in PPB and emits nothing. So a rare "out" with no
//        removal behind it is possible; pair it against the removal, do not assume it.
//
//   PPB_GestureDeviceEquipped  "<name>|<class>|<locked>|<quest>|<siteMask>|<slotMask>"
//        A DD/ZaZ device was put on by the equip gesture.
//
//   PPB_GestureGearEquipped    "<name>|<slotMask>"
//        Plain (non-DD) armor was put on by the equip gesture.
//
//   PPB_GestureClaim           "<actor FormID>"
//        Bookkeeping: the removal a consumer is about to see on TESEquipEvent was OUR
//        gesture, not a menu. Sent immediately before the unequip.
//
//  ── INBOUND, the one event PPB listens for ───────────────────────────────────────────
//   PPB_GestureSetPaused       numArg 1/0
//        Stand the gesture layer down (a scripted scene placing hands must not read as a
//        grab). Equivalent to the Papyrus native PPB_Native.SetGesturePaused(bool), which
//        new callers should prefer.
//
//  ⚠ NOT a gesture event, but it travels this bus: PPB_GestureUnlocked, sent by
//    PPB_DeviceEquip.psc back to PPB with the freed device's FormID. It is an internal
//    round trip. Do not consume it and do not send it.
// ═══════════════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════════════
//  THE PUSH EVENT  (added 2026-09-12, build 20104)
// ═══════════════════════════════════════════════════════════════════════════════════════
//  "The player just pushed / shoved / knocked down / swept this NPC." One SKSE mod event per
//  reaction, separate from the touch contacts: a contact says a hand is ON her, this says
//  what the push DID to her. Same rules as the gesture bus above.
//
//   PPB_PushReaction           "<kind>|<NPC display name>"      numArg 0 (reserved)
//        sender = the NPC the reaction happened to. The pusher is always the player.
//        <kind>:
//          push     she started walking back from a push. Once per push walk, when it
//                   engages — not per frame, and not again while the same walk continues.
//          shove    a push made her STUMBLE (the stagger played).
//          dropped  a SHOVE knocked her down (ragdoll).
//          sweeped  her LEGS were swept — both feet lifted off the floor — and she went down.
//        Split on the FIRST '|': everything after it is the name (a name could contain '|').
//        Consumers must tolerate extra trailing fields; this event appends, it never renumbers.
//
//        Sent on the MAIN thread, and only after the game ACCEPTED the reaction (a refused
//        knockdown sends nothing), so every event matches something the player saw.
//        A knockdown is ONE event — "dropped" or "sweeped", never also "shove". A push that
//        walks her back and then escalates sends "push" then "shove"/"dropped".
//        Nothing fires while the matching PPB.ini [Features] switch is off (bPushShove,
//        bPushWalk, bPushStumble, bShoveRagdoll, bFeetLift), nor for NPCs the push system
//        leaves alone (combat, kill move, busy in furniture; a leaner can only be "sweeped").
//        Requires GetBuildNumber() >= 20104.
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace PPBAPI {

    // Source classification for a contact. Values are frozen; new kinds append.
    enum SourceKind : unsigned char {
        kSourceFinger = 0,   // index fingertip, clearly nearest, hand not curled
        kSourcePalm   = 1,   // open-hand palm plate clearly nearest
        kSourceFist   = 2,   // hand curled (fingertips at the palm) — knuckle/back contact
        kSourceHand   = 3,   // hand contact, no clear finer classification
        kSourceGrab   = 4,   // HIGGS is actively grabbing THIS actor with that hand
        kSourceWeapon = 5,   // the wielded weapon's collision body (sourceName = weapon)
        kSourceObject = 6,   // a HIGGS-held object (sourceName = the object's base name)
        // appended 2026-08-23: the player's own genitals — one more thing that can touch.
        // `wand` is meaningless here and reads 0 (not a hand); `sourceName` is empty. Only
        // live while he is actually exposed (TNG slot 52), so trousers stop these contacts.
        kSourceGenital = 7,
        // ── appended 2026-09-03: the player's HEAD ────────────────────────────────────────
        // The player's own head/face as a toucher: one keyframed box riding the VRIK-posed
        // head node, published here the moment it touches any capsule. This is what makes
        // leaning your face against her a first-class event — a kiss on the lips (head C1),
        // a cheek, a forehead to her shoulder, a nuzzle into her neck.
        //   `wand` is MEANINGLESS here and reads 0 (the head is not a hand).
        //   `sourceName` is "face" when the FRONT of the head made the contact and "head"
        //   otherwise, so a consumer can tell a kiss from a headbutt without geometry.
        //   ★ Since 2026-09-06 it can also be "mouth": a small probe on the lower front of the
        //   face won the contact. Build >= 20103 (2026-09-12) guarantees all three names reach
        //   the DIGEST stream too (they were dropped there before), a HEAD contact never
        //   reports an interior sub-region (depth >= Inside), and the vaginal / anal OPENING
        //   capsules can be reached by the "mouth" only, never the whole head.
        //   ★ A kiss on her upper lip also raises the mouth gate's LIPS stage:
        //   PPB_MouthLips strArg "R|LIPS|HEAD" — a third field APPENDED to the usual
        //   "WAND|STAGE" (split on '|' and tolerate extra fields). A kiss is LIPS only: it
        //   never raises ENTER or THROAT.
        //   Host knob `headBox` gates it; it is DESTROYED during an OStim/SexLab scene (the
        //   same rule the genital wand follows), so do not expect these inside a scene.
        //   Like every other source it is pure geometry — no Havok listener is consulted.
        //   Requires GetBuildNumber() >= 20102 — gate on that if you branch on this kind.
        kSourceHead = 8,
        // ⛔ EVERY per-source array in the engine MUST be sized by this, never by a literal.
        // kSourceHead was appended as 8 into a `float srcSecs[8]` and the resulting `sk < 8`
        // guard silently published every head contact as kSourceFinger. Append a source =
        // bump this.
        kSourceCount = 9,
    };

    // GARMENT pseudo-slots (2026-07-30): garment contacts report through the same contact
    // stream with these values in PpbTouchContact::slot. child = the chord index along the
    // chain (0 = root). BODYPART strings: "tail (base)" / "tail (mid)" / "tail (tip)", "hair".
    // Real body slots stay 0..11; these are additive.
    // ⚠ kSlotHair is DECLARED but NOT EMITTED by default: hair strands drape the face and
    // head, so they would win the nearest-capsule race against cheeks and shadow face
    // touches. The host can enable it (apiHairTarget knob); consumers must tolerate never
    // seeing it.
    enum PseudoSlot : int {
        kSlotTail = 100,
        kSlotHair = 101,
        kSlotGen  = 102,   // 2026-08-22: male genital chain (GEN rig, tbl 7) — 4 chords
                           // base->tip along Gen01..Gen06. Reported like any other body part.
    };

    enum Phase : int {
        kPhaseEnd      = 0,
        kPhaseStart    = 1,
        kPhaseContinue = 2,
    };

    // ── REGIONS (2026-07-30) ─────────────────────────────────────────────────────────────
    // The DIGEST stream groups capsules into anatomical regions, because a real touch wanders:
    // a finger on someone's face crosses five capsules in six seconds without resting a half
    // second on any one of them. Per-capsule reporting either floods you or (with a dwell
    // filter) says nothing at all. A digest contact is therefore identified by
    // (actor, wand, REGION) and reports the part it spent the LONGEST on:
    //     "R|FINGER|Face(cheek L)|human"
    // Intimate is deliberately its own region, not part of Pelvis: "touched her hip" and
    // "inserted" are categorically different events for a consumer.
    enum Region : int {
        kRegionNone = 0,
        kRegionFace, kRegionNeck, kRegionChest, kRegionBelly, kRegionWaist,
        kRegionPelvis, kRegionIntimate, kRegionArm, kRegionHand, kRegionLeg,
        kRegionFoot, kRegionTail, kRegionHair,
    };

    // ── SUB-REGIONS (2026-07-31) ─────────────────────────────────────────────────────────
    // A finer bucket than Region, and the one that answers "how far in did it get". Region
    // says Face; SubRegion distinguishes a cheek from a fingertip on the palate.
    //
    // ★ THE DEPTH LADDER — this is the point of the enum, not a nicety.
    // Within the mouth and within the intimate chain, values are ordered SHALLOW -> DEEP and
    // each level OVERRIDES the ones below it:
    //     Face surface  <  Mouth opening  <  In mouth  <  Mouth wall
    //     external      <  opening        <  deep      <  deepest
    // A cheek or a chin is an ordinary FACE TOUCH on its own — those capsules only signify
    // "mouth" in conjunction (the mouth gate needs the palate AND both cheeks at once). But a
    // touch on the PALATE means something IS inside her mouth, full stop, and outranks any
    // simultaneous lip/cheek reading. The THROAT WALL outranks even that. SubRegionDepth()
    // collapses this to a 0..3 number if you only care "how deep", not "where".
    //
    // Values are frozen; new sub-regions APPEND. Do not assume the numeric spacing between
    // families is stable across revisions — switch on the names, or use SubRegionDepth().
    enum SubRegion : int {
        kSubNone = 0,
        // head — depth ladder runs kSubFaceSurface -> kSubMouthOpening -> kSubInMouth -> kSubMouthWall
        kSubHead,             // cranium, occiput
        kSubHeadEar,          // temple / ear side (the EAR capsules on beast heads)
        kSubFaceSurface,      // cheekbone, nose, cheeks, chins — a face touch on its own
        kSubMouthOpening,     // the lip ring: at the mouth, not inside it
        kSubInMouth,          // palate — a touch here means something IS inside
        kSubInMouthDeep,      // under-jaw / deep floor of the cavity
        kSubMouthWall,        // throat wall: the end of the cavity, outranks everything
        kSubNeck,
        // torso
        kSubShoulderCap, kSubRibCage, kSubBreast, kSubBelly, kSubWaist,
        // arms
        kSubShoulder, kSubUpperArm, kSubForearm, kSubPalm,
        // pelvis — kSubOrificeRing is OUTSIDE the intimate chain (region Pelvis, not Intimate)
        kSubPelvis, kSubOrificeRing, kSubGlute,
        // intimate chain — ordered by depth within each tract
        kSubIntimateExternal,                       // clitoris
        kSubVaginalOpening, kSubVaginalDeep, kSubVaginalDeepest,
        kSubAnalOpening, kSubAnalDeep,
        // legs
        kSubThigh, kSubCalf, kSubFoot,
        // garments — tail reports thirds of the chord chain, so "tip" means the same place
        // on a 4-chord foxtail and a 14-chord fluffy tail
        kSubTailBase, kSubTailMid, kSubTailTip, kSubHair,
    };

    // SubRegionDepth() buckets. Only meaningful on the mouth and intimate chains; every
    // other sub-region answers kDepthSurface.
    enum SubRegionDepthLevel : int {
        kDepthSurface = 0,   // outside: skin, a face touch, a hip
        kDepthOpening = 1,   // at the entrance: lips, vaginal/anal opening
        kDepthInside  = 2,   // unambiguously inside: palate, cervix, rectum
        kDepthDeepest = 3,   // the far wall: throat, uterus
    };

    // ── WEAPON CLASS + EDGE (2026-08-01) ────────────────────────────────────────────────
    // `sourceName` already carries the weapon's display name ("Iron Rapier"). These two add
    // what it IS, straight from the equipped record's animation type — so a consumer can tell a
    // mace from a dagger without string-matching a name that varies by mod and by language.
    enum WeaponClass : int {
        kWeapNone = 0,
        kWeapFist, kWeapSword, kWeapDagger, kWeapAxe, kWeapMace,
        kWeapGreatsword, kWeapBattleaxe,        // two-handed; battleaxe covers warhammers
        kWeapBow, kWeapStaff, kWeapCrossbow, kWeapOther,
    };

    // The coarse question most consumers actually ask: does this cut, crush, or stab?
    // Derived from the class, so it stays right when new classes are appended.
    enum WeaponEdge : int {
        kEdgeNone = 0,
        kEdgeBlade,    // sword, greatsword, axe, battleaxe — cutting
        kEdgeBlunt,    // mace, warhammer, staff, fist, a bow used as a club
        kEdgePierce,   // dagger, and thrusting blades
    };

    // One live contact. Fixed 160-byte POD; the reserved tail lets future revisions add
    // fields without moving anything.
    struct PpbTouchContact {
        unsigned int actorFormId;    // the touched NPC
        unsigned int toucherFormId;  // 0x14 = the player (always, in interface revision 1)
        int           slot;          // 0..11 (hand,forearm,upperarm,head,spine0,spine1,spine2,
                                     //        neck,thigh,calf,foot,com)
        int           child;         // capsule child index within the slot
        unsigned char leftTwin;      // 1 = the capsule is on the LEFT twin body (limb slots)
        unsigned char wand;          // 0 = player's RIGHT hand/weapon, 1 = LEFT
        unsigned char sourceKind;    // SourceKind
        unsigned char _pad0;
        float         distU;         // current surface distance, game units; negative = inside
        float         durationS;     // seconds since this contact began
        char          bodyPart[48];  // named body part (or "<slot>.C<n>")
        char          skeleton[12];  // "human" / "argonian" / "khajiit" / "draenei"
        char          sourceName[48];// weapon/object base name for kSourceWeapon/Object, else ""
        // ── appended 2026-07-31 out of the reserved tail (the sanctioned growth path) ──
        // Both are self-describing so you never need a second call to classify a contact.
        unsigned char region;        // Region of the reported part (digest: the contact's own)
        unsigned char subRegion;     // SubRegion of the reported part
        unsigned char depth;         // SubRegionDepthLevel — 0 surface .. 3 deepest
        // ── appended 2026-08-01 ── zero for non-weapon sources.
        unsigned char weaponClass;   // WeaponClass — what the weapon IS
        unsigned char weaponEdge;    // WeaponEdge — blade / blunt / pierce
        unsigned char engineContact; // 1 = this came from Havok's OWN narrowphase (exact capsule
                                     // + real separating distance), 0 = the geometric fallback
        // ── appended 2026-08-19 (ORIFICE DRIVE) ── out of the reserved tail, sizeof unchanged.
        // How far open the orifice this contact is INSIDE actually is, right now — PPB drives
        // the bone rings natively on any contact, outside scenes, so this is a real deformation
        // state and not a guess. Zero unless the reported part is an orifice sensor.
        unsigned char orificeKind;   // 0 = none, 1 = vaginal, 2 = anal, 3 = oral
        unsigned char orificeOpen;   // 0..255 = 0..1 of that orifice's full gape (ScaleMax)
        unsigned char _reserved[16]; // future fields; zero today
    };
    static_assert(sizeof(PpbTouchContact) == 160, "PpbTouchContact layout is frozen");

    // phase = Phase above. The pointer is valid only for the duration of the call — copy it.
    using PpbTouchCallback = void (*)(const PpbTouchContact* contact, int phase);

    // ── the interface ── vtable is append-only, slot numbers in comments are the contract.
    class IPpbTouchInterface1 {
    public:
        virtual unsigned int GetBuildNumber() = 0;                                       // 00
        // Is this actor carrying a PPB-driven body right now? false = not covered
        // (creature/child/unmapped race/maleGeometry 0) — use your fallback path.
        virtual bool IsDriven(unsigned int actorFormId) = 0;                             // 01
        // Writes the skeleton id ("human"...) into out, returns its length; 0 = not driven.
        virtual int  GetSkeleton(unsigned int actorFormId, char* out, int cap) = 0;      // 02
        // Snapshot of all live contacts (player-vs-NPC in revision 1). Returns the count
        // copied (<= max). Cheap: copies from a double-buffered snapshot.
        virtual int  GetContacts(PpbTouchContact* out, int max) = 0;                     // 03
        // Live world geometry of one capsule: endpoints A/B and radius, game units.
        // leftTwin selects the L twin body on limb slots. False = no such capsule.
        virtual bool ReadCapsule(unsigned int actorFormId, int slot, bool leftTwin,
                                 int child, float aOutU[3], float bOutU[3], float* rOutU) = 0;  // 04
        // The shipped name for (slot, child) on the human-female reference map, or null
        // for unnamed/race-specific children. Static strings — never freed.
        virtual const char* CapsuleName(int slot, int child) = 0;                        // 05
        // Live child count of a slot's list shape; 0 = single plain capsule (address it
        // as child 0); -1 = actor not driven / no body.
        virtual int  ChildCount(unsigned int actorFormId, int slot) = 0;                 // 06
        // Register a touch callback (main thread; fires alongside the mod events, same
        // rate limit). Returns false if the callback table is full.
        virtual bool AddTouchCallback(PpbTouchCallback cb) = 0;                          // 07
        // ── appended 2026-07-30 (revision 1 stays revision 1: append-only, never reordered) ──
        // The RAW stream: one contact per (actor, wand, source class), every capsule, no
        // region grouping. GetContacts() above is the DIGEST. Most consumers want the digest.
        virtual int  GetRawContacts(PpbTouchContact* out, int max) = 0;                  // 08
        // Region of a (slot, child) — see Region. 0 = unknown.
        virtual int  RegionOf(int slot, int child) = 0;                                  // 09
        // Human-readable region name ("Face", "Intimate", ...); static string, never freed.
        virtual const char* RegionName(int region) = 0;                                  // 10
        // ── appended 2026-07-31 ──────────────────────────────────────────────────────────
        // SubRegion of a (slot, child) — the finer bucket, and the depth ladder. 0 = unknown.
        // Garment chords: pass kSlotTail/kSlotHair with the chord index; tail resolves to
        // base/mid/tip using the LIVE chord count of the nearest rig.
        virtual int  SubRegionOf(int slot, int child) = 0;                               // 11
        // Human-readable sub-region name ("Face surface", "In mouth", "Mouth wall",
        // "Intimate - vaginal (deep)", "Tail - tip", ...); static string, never freed.
        virtual const char* SubRegionName(int subRegion) = 0;                            // 12
        // How far in: SubRegionDepthLevel, 0 surface .. 3 deepest. Use this when you only
        // care whether something went inside, not exactly where.
        virtual int  SubRegionDepth(int subRegion) = 0;                                  // 13
        // ── appended 2026-08-01 ──────────────────────────────────────────────────────────
        // "Iron Rapier" / "Steel Mace" is in sourceName; these say what it IS.
        virtual const char* WeaponClassName(int weaponClass) = 0;                        // 14
        // WeaponEdge for a class: blade / blunt / pierce. Use this rather than switching on
        // every class, so appended classes keep working.
        virtual int  WeaponEdgeOf(int weaponClass) = 0;                                  // 15
    };

    // The messaging request. Dispatch to sender "PPB" with this struct as data; PPB fills
    // GetApiFunction synchronously. GetApiFunction(1) -> IPpbTouchInterface1*.
    struct PpbMessage {
        enum : unsigned int { kGetTouchInterface = 0x50504254 };   // 'PPBT'
        void* (*GetApiFunction)(unsigned int revision) = nullptr;
    };
}
