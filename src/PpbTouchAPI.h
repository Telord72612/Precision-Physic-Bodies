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
//  PPB rebuilds female NPC Havok bodies as 12 body slots of named collision capsules
//  (~148 per NPC), live-fitted to each NPC's actual mesh. This interface reports CONTACT:
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
//  PPB drives FEMALE NPCs of mapped races: the human catch-all (covers elf/orc/etc on a
//  typical load order), Argonian, Khajiit, Draenei, plus anything the user adds to
//  PPB_Skeletons_Added_Race.ini. Males, children and creatures are NOT covered and
//  answer IsDriven() = false — route those to your fallback (e.g. CBPC), never assume.
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
        unsigned char _reserved[18]; // future fields; zero today
    };
    static_assert(sizeof(PpbTouchContact) == 160, "PpbTouchContact layout is frozen");

    // phase = Phase above. The pointer is valid only for the duration of the call — copy it.
    using PpbTouchCallback = void (*)(const PpbTouchContact* contact, int phase);

    // ── the interface ── vtable is append-only, slot numbers in comments are the contract.
    class IPpbTouchInterface1 {
    public:
        virtual unsigned int GetBuildNumber() = 0;                                       // 00
        // Is this actor carrying a PPB-driven body right now? false = not covered
        // (male/creature/child/unmapped race) — use your fallback path.
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
