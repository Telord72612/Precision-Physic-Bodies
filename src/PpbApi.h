#pragma once
// PpbApi — the public touch API's engine (see PpbTouchAPI.h for the consumer contract).
// Player-as-toucher revision 1: probes = the player's HandBox boxes, HIGGS weapon bodies
// and HIGGS-held objects; targets = the 12 capsule slots of the nearest driven NPCs.
// Detection is pure geometry (point-to-capsule-surface); no Havok listeners.

#include "PpbTouchAPI.h"   // PPBAPI::PpbTouchContact — CopyContacts's element type

namespace PpbApi {
    // ── MOUTH GATE bridge (2026-07-31, VRTE report 16 §4.3 / R4) ────────────────────────
    // The mouth gate in NpcFingerTest.cpp is the AUTHORITATIVE mouth signal: a multi-capsule
    // AND (palate + both cheeks within gate), per-race child sets for beast heads, and a
    // finger-only test so a fist mashed into the face cannot open her. It was log-only. The
    // nearest-capsule race can never express that logic — it picks ONE capsule — so consumers
    // wanting "is something in her mouth" must read this, not a capsule name.
    // stage: 0 = LIPS, 1 = ENTER, 2 = THROAT REACHED.  entered = true on rise, false on fall.
    void EmitMouthStage(RE::Actor* actor, int stage, bool entered, int hand, float distU);

    // ── PROBE EXPORT (2026-08-19, for Orifice.cpp) ──────────────────────────────────────
    // A read-only copy of the probes CollectProbes already assembled this tick: the player's
    // hand boxes (with the held-hand suppression and the VRIK index exception already
    // applied), the HIGGS weapon blade segment, and the held-object bound-box segment. Any
    // consumer that needs "what is the player poking with, and where" reads THIS rather than
    // re-deriving it — one assembly, one set of fixes, no second source of truth to drift.
    // Main thread only. Returns the number copied (<= max, <= 15).
    // 2026-08-23: 14, not 12 — the PLAYER GENITAL WAND's 2 segments are appended after the
    // hand set (see CopyProbes). Pass a 14-slot buffer; a 12-slot one still works and simply
    // drops the wand.
    // 2026-09-03: 15 — the PLAYER HEAD BOX's single segment is appended after the wand. Every
    // existing 12/14-slot caller keeps working and simply drops the head, which is the correct
    // degradation and the reason it is appended LAST. ⚠ Deliberate today: the ORIFICE drive
    // passes 14, so the head can never open an orifice — it is a body-contact source, not an
    // insertion source. Raise that buffer only if that is ever actually wanted.
    // ⛔ SUPERSEDED 2026-09-12 — the paragraph above was FALSE in practice. CopyProbes counts only
    // LIVE probes, so the head (and the 09-06 mouth probe, making the real maximum 16, not 15) land
    // in any buffer with spare room whenever hand probes are dead — including Orifice's 14. The
    // "head never opens an orifice" rule is now enforced where it belongs: Orifice.cpp skips cls 4
    // explicitly. Consumers must filter on `cls`, never on buffer size.
    struct ProbeView {
        float p[3];     // probe point, or the segment start (world game units)
        float q[3];     // segment end (seg only)
        float pad;      // extra surface: object bound radius / weapon capsule radius. 0 for
                        // the bare hand boxes — they ARE the fingertip.
        int   seg;      // 1 = the probe is the SEGMENT p->q, 0 = the point p
        int   cls;      // 0 = hand box, 1 = weapon, 2 = held object, 3 = player genital wand,
                        // 4 = player head box
                        // (3 and 4 are not hand probes and never enter the per-hand set)
        int   wand;     // 0 = player's RIGHT hand, 1 = LEFT. Meaningless for cls 3/4 (reads 0).
        // The actor THIS hand is HIGGS-grabbing (0 = none). The held-hand suppression and the
        // sheathed-weapon gate are already baked into the probe set, but the GRAB MUTE is not:
        // it is per-TARGET (the weapon is muted against the actor being held and stays live
        // against everyone else), so it cannot be applied at export. Every consumer MUST drop a
        // cls==1 probe whose grabActorId equals the actor it is testing — exactly as
        // PpbApi::ScanActor does (PpbApi.cpp:891-892). Otherwise the recorded phantom (an axe
        // riding the grip of the hand holding her leg, "cervix -16.9u for 13.6s") is read as a
        // real contact.
        unsigned int grabActorId;
    };
    int CopyProbes(ProbeView* out, int max);

    // ── v28 RELEASE REACH (2026-09-10, for DeviceGesture) ───────────────────────────────────────
    // The HELD OBJECT's distance to ONE of her capsules, by the same formula ScanActor gates an
    // object contact on (collision box > segment > bound sphere; interior capsules raw, the rest
    // minus the object's pad and the breast pad), from this hand's CURRENT probe (refreshed at
    // apiHz). gapOut = the gate distance (<= 0 = the surfaces meet), rawOut = the true surface
    // distance, kindOut = 2 box / 1 segment / 0 point. false = no live object probe on that hand
    // (nothing held) or the capsule does not read. Main thread only.
    bool HeldObjectGapU(RE::Actor* actor, int hand, int slot, bool left, int child,
                        float* gapOut, float* rawOut, int* kindOut);

    // The same DIGEST snapshot the published GetContacts() hands consumers, readable from inside
    // PPB without the interface round trip. Added 2026-08-26 for the gesture layer, which used to
    // live in the DD-ZaZ add-on and reached this data across the API.
    int CopyContacts(PPBAPI::PpbTouchContact* out, int max);

    // ── ENGINE-TRUTH HAND TOUCH (2026-09-07, user design — report 33 §8.3) ───────────────────
    // Havok's own record that a player collider (HIGGS hand / weapon, PPB finger box) touched an
    // NPC ragdoll body, resolved to (actor, slot, side, hand) EVERY FRAME — not at apiHz. The
    // 4 Hz contact snapshot planted PushStep's hand-travel anchor up to 250 ms late; this is
    // the clock the user asked for: "when the hand contact the collision capsule … start
    // tracking". tS = steady_clock seconds (PushStep's NowS() clock). Main thread only.
    struct EngineTouch {
        std::uint32_t actorId;
        int    slot;      // 0..11 touch-API slot; PushStep maps (8, left) to its 108
        int    child;     // list-child index = the capsule
        bool   left;
        int    hand;      // 0 = player's RIGHT, 1 = LEFT
        int    src;       // 0 HIGGS hand, 1 weapon, 2 PPB finger box
        double tS;        // when the main thread resolved it (<= 1 frame after the physics step)
    };
    // Every actor the engine saw a player collider touch, newest per actor, aged out after 2 s.
    // Returns the count copied (<= max).
    int EngineTouchList(EngineTouch* out, int max);

    // Per-frame roster: OnPreDrive announces every driven actor (same-frame use only —
    // the pointer is consumed by OnFrame later in the SAME frame, the g_gt.best pattern).
    void NoteDriven(RE::Actor* actor);

    // ★★★ 2026-09-13: children (race Child flag) and mannequins (ManakinRace) are OUT of every interaction layer —
    // no touch contacts, no push reactions, no gestures — whatever skeleton they ride. Cheap: one race flag read and,
    // for non-child races, one cached EditorID compare.
    bool IsExcludedActor(RE::Actor* actor);

    // The tick: runs on the HIGGS PostVrikPostHiggs frame callback (main thread), throttled
    // to the apiHz knob. Scans, updates the contact table, fires mod events + callbacks,
    // publishes the Papyrus snapshot.
    void OnFrame();

    // SKSE plugin-message handler (RegisterListener(nullptr, ...) in SKSEPlugin_Load):
    // answers PPBAPI::PpbMessage::kGetTouchInterface from any plugin.
    void OnPluginMessage(SKSE::MessagingInterface::Message* msg);

    // Papyrus natives (class "PPB_Touch") — registered from Natives.cpp.
    bool RegisterNatives(RE::BSScript::IVirtualMachine* vm);

    // Save/load hygiene: drop every live contact (no events fired for the dead ones).
    void ClearOnLoad();
}
