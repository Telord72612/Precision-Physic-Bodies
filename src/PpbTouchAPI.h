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
//    (apiHz, default 20/s). Do not block in them.
//  * Papyrus: PPB also fires mod events (below) and exposes polling natives
//    (script "PPB_Touch"), both safe from any Papyrus context.
//
//  ── PAPYRUS MOD EVENTS ───────────────────────────────────────────────────────────────
//      "PPB_TouchStart"   numArg = surface distance in game units (negative = inside)
//      "PPB_Touch"        numArg = distance, re-sent at apiHz while contact holds
//      "PPB_TouchEnd"     numArg = the contact's total DURATION in seconds
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

    enum Phase : int {
        kPhaseEnd      = 0,
        kPhaseStart    = 1,
        kPhaseContinue = 2,
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
        unsigned char _reserved[24]; // future fields; zero today
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
    };

    // The messaging request. Dispatch to sender "PPB" with this struct as data; PPB fills
    // GetApiFunction synchronously. GetApiFunction(1) -> IPpbTouchInterface1*.
    struct PpbMessage {
        enum : unsigned int { kGetTouchInterface = 0x50504254 };   // 'PPBT'
        void* (*GetApiFunction)(unsigned int revision) = nullptr;
    };
}
