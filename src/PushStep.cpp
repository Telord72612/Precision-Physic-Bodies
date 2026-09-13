#include "PushStep.h"
#include "PpbTouchAPI.h"
#include "PpbApi.h"      // CopyContacts — the trunk-pressure scan
#include "Tuning.h"      // knobs
#include "Interop.h"     // IsActorGrabbedByPlayer — while grabbed, PLANCK's drag machinery owns movement
#include "HandBox.h"
#include "CapFix.h"      // GrabDiag::GetSlotPushDisplacement — the legacy body-vs-node sensor (v4)
#include "PPBHook.h"     // ArmIK::GetPushSenseGap — the v5 intent-gap sensor (body vs FK'd drive pose)
#include "RagFrame.h"    // the ragdoll-onset receipt (read-only)
#include "Ini.h"         // 2.2.0: PPB.ini [Features] bPushWalk / bPushStumble / bShoveRagdoll — the install choices

#include <chrono>
#include <cmath>
#include <cstring>   // v27: _strnicmp / _stricmp for the get-up probe
#include <unordered_map>
#include <mutex>
#include <vector>

namespace logger = SKSE::log;

namespace {

// ── the four engine calls, PLANCK 0.7.1's shipped VR offsets (see PushStep.h) ────────────
using Fn_HeadingFromVec   = float (*)(const RE::NiPoint3&);

REL::Relocation<Fn_HeadingFromVec>   GetHeadingFromVector{          REL::Offset(0xC97030) };
// ★ v7.2 THE UNIT FIX: SetTargetSpeed speaks NORMALIZED speed (the actor's own gait scale),
// not game units — PLANCK normalizes right before the feed (main.cpp:6743-6747). Feeding raw
// u/s (18..90) into the ~0..1 slot was gait roulette: every sprint/ignored-fade since v1.
using Fn_NormalizeSpeed = float (*)(void* actorState, float uPerS);
REL::Relocation<Fn_NormalizeSpeed>   ActorState_NormalizeSpeed{     REL::Offset(0x1120AA0) };
static float NormSpeedFor(RE::Actor* actor, float uPerS)
{
    void* st = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(actor) + 0xB8);
    return ActorState_NormalizeSpeed(st, uPerS);
}

// ── PLANCK's movement-controller mirrors (github.com/adamhynek/activeragdoll, RE/misc.h) ──
// Plain virtual interfaces; the compiler does the dispatch. Declaration ORDER is the vtable
// contract — copied verbatim from the repo the user pointed at (2026-08-29).
struct IMoveIface { virtual ~IMoveIface() = default; };
struct IMotionDrivenCtl : IMoveIface {
    virtual void MoveToHigh() = 0;                                   // 01
    virtual void MoveFromHigh() = 0;                                 // 02
    virtual void SetAnimationDriven() = 0;                           // 03
    virtual void SetMotionDriven() = 0;                              // 04
    virtual void SetAllowRotation() = 0;                             // 05
    virtual bool IsMotionDriven() = 0;                               // 06
    virtual bool IsAnimationDriven() = 0;                            // 07
    virtual bool IsAllowRotation() = 0;                              // 08
};
struct IPlannerDirectCtl : IMoveIface {
    virtual void SetPlannerDirectControl() = 0;                      // 01
    virtual void SetTargetDirection(const RE::NiPoint3& euler) = 0;  // 02
    virtual void SetTargetSpeed(float speed) = 0;                    // 03
    virtual void SetTargetAngle(const RE::NiPoint3& angle) = 0;      // 04
    virtual void ClearPlannerDirectControl() = 0;                    // 05
};
// MovementControllerNPC sub-object / flag offsets (repo misc.h; VR — same file PLANCK VR ships).
constexpr std::ptrdiff_t kMcMotionDriven   = 0x128;
constexpr std::ptrdiff_t kMcPlannerDirect  = 0x140;
constexpr std::ptrdiff_t kMcFlagPlanner    = 0x1C7;   // isPlannerDirectControl byte
constexpr std::ptrdiff_t kActorMoveCtl     = 0x148;   // planck utils.cpp:1051 (refcounted; transient read)

inline void* McOf(RE::Actor* a)
{
    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(a) + kActorMoveCtl);
}
inline IMotionDrivenCtl* MotionCtl(void* mc)
{
    return reinterpret_cast<IMotionDrivenCtl*>(reinterpret_cast<std::uintptr_t>(mc) + kMcMotionDriven);
}
inline IPlannerDirectCtl* PlannerCtl(void* mc)
{
    return reinterpret_cast<IPlannerDirectCtl*>(reinterpret_cast<std::uintptr_t>(mc) + kMcPlannerDirect);
}
inline bool PlannerActive(void* mc)
{
    return *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(mc) + kMcFlagPlanner) != 0;
}

// The pushed set: written by OnFrame's contact scan, consumed by the 0x5E0885 hook.
struct WalkState {
    int    fallFrames = 0;   // consecutive frames the char controller reported UNSUPPORTED
                             // while a player contact was recent (the fall-ragdoll trigger)
    float  dirX = 0.f, dirY = 0.f;    // push direction; v17 STEERS it toward the live push while driving
    // v17: which way she was told to face at engage, so the steering keeps the SAME rule as it turns.
    // true  = rear sector: face the retreat and walk FORWARD.
    // false = front sector: face where the push comes FROM and walk BACKWARD.
    bool   walkFaceAway = false;
    double lastDirS     = 0.0;        // v17 steering clock
    double lastPressureS = 0.0;
    int    slot = 6;
    bool   driving = false;           // we entered planner-direct-control for this actor
    double lastFallbackS = 0.0;       // throttle for the bump fallback
    // v2 episode controls (2026-08-29, after the first working walk went 16 steps + sideways):
    float  startX = 0.f, startY = 0.f;   // her position at engage — walked distance is measured
    float  angleZ = 0.f;                 // her facing at engage — fed unchanged (no wander)
    float  budgetU = 60.f;               // decel starts here (sustained contact may extend it)
    double reengageAfterS = 0.0;         // refractory: continued pressure starts the NEXT episode
    double lastEpisodeEndS = 0.0;        // v3.2: for the settle gate on the direction sensor
    // v6 envelope (2026-08-30): speed is a SHAPE, not a constant.
    double engageS      = 0.0;           // ramp-in reference
    float  targetSpeedU = 42.f;          // from push magnitude, locked at engage
    int    phase        = 0;             // 0 = walk (ramp/cruise/extend), 1 = decel fade
    double decelStartS  = 0.0;
    float  decelSpeedU  = 0.f;           // speed frozen at fade start
    bool   extended     = false;         // sustained-contact extension receipt latch
    bool   capped       = false;         // v6.1: fade caused by the HARD CAP = final, no resume
    // v21: the distance she should still cover once the push stops, LATCHED at the fade's first frame.
    // Latched rather than recomputed, because `walked` keeps growing during the fade - a live
    // recompute would chase its own tail and stretch the stop to roughly half the walk.
    float  stopDistU    = 0.f;
    // v21a: the CONSTANT deceleration that covers stopDistU, solved ONCE from the speed she carried when
    // the push ended. Re-solving it each frame from the CURRENT speed collapses it quadratically
    // (90 u/s -> a 122, but 10 u/s -> a 1.5), so she asymptotes into a crawl and only ever stops when the
    // 3 s fade backstop cuts her off - measured: every fade ended at exactly 3.00-3.02 s.
    float  stopAccelU   = 0.f;
    float  lastGapMag   = 0.f;           // v7.3: previous gap reading (for the growth rate)
    double lastGapS     = 0.0;
    bool   sawHard      = false;         // v8.0: any qualifying contact BEYOND breast capsules this hold
    float  pressDistU   = 99.f;          // v8.4: this frame's closest qualifying contact (press gate)
    // ★ v11 rule 7: which KIND of thing is pressing this frame. A held object gets a raised bar, but
    // only when nothing else is pressing — a hand and an object together read as the hand.
    bool   pressHand    = false;
    bool   pressObj     = false;
    bool   pressWand[2] = { false, false };       // v11.1b: which HANDS press this frame (wand 0 R / 1 L), re-decided per frame
    double lastPressWandS[2] = { -1e9, -1e9 };    // v11.1b: per-hand API pressure clock — the engine anchor's lapse partner
    // 2026-09-13 (VRTE GearGestures R6): the capsule each hand last pressed on, for PPB_PushReaction's pusher fields
    int    pressSlot[2]  = { -1, -1 };
    int    pressChild[2] = { 0, 0 };
    bool   pressLeft[2]  = { false, false };
    double waitLogS    = 0.0;                     // v11.1b: the `waiting` receipt's OWN throttle (it shared lastFallbackS and silenced every other refusal receipt)
    char   contactsDbg[96] = {};         // v8.5 forensics: this frame's qualifying contacts
    int    contactsN    = 0;
    // v8.7 IMPULSE MEMORY: the strongest PRESSED moment of this push, so a fast impulse that
    // spikes and collapses between gate openings is not lost.
    float  peakMag      = 0.f;
    float  peakDx       = 0.f, peakDy = 0.f;
    float  peakRate     = 0.f;
    int    peakSlot     = -1;
    double peakS        = -1e9;
    double lastReactS   = -1e9;         // v9.5: stagger/ragdoll cooldown
    // v9.9 WINDOWED RATE (user in VR 2026-09-06: "recognises a normal push as a shove"): the
    // tiers' speed witness. The one-frame delta above (lastGapMag) turns 0.6u of sensor wobble
    // at 75 Hz into +45 u/s — the log fired staggers 13 ms after engages that reported rate 0.0.
    // The ring answers "how fast over the last pushStepRateWinS" (0 until it spans half of it).
    static constexpr int kRing = 16;
    float  ringMag[kRing] = {};
    double ringS[kRing]   = {};
    int    ringN = 0, ringHead = 0;
    float  rateW        = 0.f;
    float  peakRateW    = 0.f;           // the windowed rate at the moment the peak was latched
    // ★ v10.0 LIVE SPEED (user in VR 2026-09-06: "smoother ... ramping up the speed as the push
    // continues ... ramping down as the push stops"): the commanded speed is a rate-limited
    // FOLLOWER of a target recomputed every walking frame from the live push depth — no longer
    // locked at engage. pushStepSpeedTrack 0 restores the v6 envelope (targetSpeedU + ramps).
    float  liveTargetU  = 0.f;           // this frame's target from the live gap (0 in the fade)
    float  cmdSpeedU    = 0.f;           // what we command; slews toward the target
    double lastSpeedS   = 0.0;           // follower clock
    double lastSpeedLogS= 0.0;           // 1 Hz receipt throttle
    float  liveRatio    = 1.f;           // v10.1: (depth×lat/ref)^exp this frame — scales the up-slew
    // ★ v11 TIER HOLD (user 13:15: a palm at d=-0.10u spiked the chest node to 12 u and it was rebounding 60 ms
    // later — a JOLT, not the sustained bend rule 3 pictures). The gap must sit above a bar for pushStepTierHoldS
    // before that tier fires. Reset the moment it drops below (or the sensor stands down and publishes zero).
    double stagAboveS   = 0.0;           // when the lateral-scaled gap first exceeded the stumble bar (0 = not above)
    double ragAboveS    = 0.0;           // same for the raw knockdown bar
    // ★ v11 HAND TRAVEL (user 14:20): where the pushing hand was when it FIRST touched her this episode, and how
    // far it has moved since. The walk-start gate reads this; the body bar alone is crossed after ~1 u of hand
    // motion because the keyframed hand throws her chest ahead of itself.
    float  handP0[3]    = {};
    bool   handP0Valid  = false;
    float  handTravelU  = 0.f;
    double handP0S      = 0.0;           // v11.1: when the API anchor was planted (receipt cadence)
    float  rampTS       = 1.f;           // v8.0: this episode's ramp-in time (aggro-scaled at engage)
    float  rampFloorE   = 0.15f;         // v8.0: this episode's ramp floor
    // *** v14 THE LEVER: the crossing rate this episode engaged with, and its normalised 0..1 form. Set ONCE
    // at engage, from the origin clock as it stood BEFORE the walk-engage re-capture. The live follower reads
    // leverNorm as a FLOOR, so easing off never drops the speed below what the shove earned, while a push that
    // keeps deepening can still raise it.
    float  crossU       = 0.f;           // u/s the bar was crossed at (receipt + diagnosis)
    float  leverNorm    = 0.f;           // 0..1 normalised lever
};
std::mutex g_walkMx;
std::unordered_map<std::uint32_t, WalkState> g_walk;
// ★ v11.1 ENGINE ANCHOR (2026-09-07, user design — report 33 §8.3): where the pushing hand was when the ENGINE
// first saw it touch her this episode, and how far it has moved since. Lives OUTSIDE WalkState on purpose: the
// hook erases a WalkState the moment it has no pressure, and a contact stamp arrives up to 250 ms before the
// 4 Hz snapshot's first contact does — an anchor inside WalkState would be erased before it could be used.
// Under g_walkMx. The episode starts on the first stamp and ends when BOTH clocks have been quiet for
// kAnchorLapseS: the engine (Havok may report only NEW points, so a resting hand can go quiet) AND the API
// snapshot's pressure (which does see a resting hand).
struct EngAnchor {
    bool   valid = false;
    float  p0[3] = {};
    float  travelU = 0.f;
    int    slot = -1;        // the capsule slot of the latest stamp (8 + left = 108)
    int    src = -1;         // 0 HIGGS hand, 1 weapon, 2 PPB finger box (latest stamp)
    double firstS = 0.0;     // when the anchor was planted
    double lastEngS = -1e9;  // last engine stamp from THIS hand
};
// v11.1b (review): one anchor PER HAND per actor. The first build kept one per actor and locked it onto whichever hand
// touched first — a resting left hand then judged the right hand's push (gate never passed), or a withdrawn hand's
// travel passed a later push by the other hand instantly. Each hand has its own clock; the PRESSING hand decides.
struct EngAnchors { EngAnchor h[2]; };   // [0] = R, [1] = L
std::unordered_map<std::uint32_t, EngAnchors> g_engAnchor;
constexpr double kAnchorLapseS = 0.5;

// ══ ★ v12 THE TRAVEL SENSOR (user's design, 2026-09-07 evening) ═══════════════════════════════════════
// "Player's hand contact, start calculating distance of push. Pass 3u, start walking sequence. Pass 10u,
//  do stumble. No complicated gate, swing, state, just distance from origin on the XP32."
// At the first player contact we capture every sensed node's position IN HER FRAME (PPBHook publishes it
// origin-relative and de-yawed, so her walking and turning cancel). Every frame after, the push IS
// |now - captured| on the touched bone. Nothing else: no animation reference, no baseline EMA, no lever,
// no growth rate, no peak latch, no hemisphere test, no press gate, no hand-travel gate.
// Two re-captures, both mechanical rather than tuned: the contact lapsing (a new push is a new origin),
// and the walk starting (the walk's own gait moves her bones, and that is not the player pushing).
// ★★ v13 (user): "We don't just track ONE havok joint, we track all of them, so we know what is going on in
// all the body ... it doesn't matter where the contact is originating." The six he named decide; the thighs
// are still measured and printed (a hip press lands there) but do not vote, because a leg swings on its own.
constexpr int kTravelSlots = 8;
constexpr int kTravelSlot[kTravelSlots] = { 11, 4, 5, 6, 7, 3, 8, 108 };   // COM, Spine0, Spine1 | Spine2, Neck, Head | thighs
// ★★ v13a (MEASURED 00:16:49, one slight chest push, all eight joints from the same contact):
//     s11 COM 1.4 | s4 spine0 2.4 | s5 spine1 5.2 | s6 spine2 8.4 | s7 neck 9.5 | s3 head 9.8
// The head reads SEVEN TIMES the COM for the same push. That is a LEVER, not a bigger push: a chest press
// pivots her about her hips, and the joints furthest from the pivot travel furthest. Taking the maximum over
// all six therefore let the HEAD decide every push, and the head crosses the 10 u stumble bar while her COM
// has barely moved 1.4 u — "a slight push on the chest and she stumbled".
// The magnitude that means "she is actually being displaced" is the BASE of the trunk. The user's own two
// examples both read on the base: "spine0 2u and spine1 4u -> walk" and "spine0 20u but head 3u -> ragdoll".
// So COM/Spine0/Spine1 vote; Spine2/Neck/Head and the thighs stay measured and printed (they are the gradient
// that will separate a bend from a sweep) but do not set the number. pushStepVoteN raises it hot.
constexpr int kTravelVoteMax = 6;
inline int TravelIdx(int slot) {
    for (int i = 0; i < kTravelSlots; ++i) if (kTravelSlot[i] == slot) return i;
    return -1;
}
struct TravelOrigin {
    bool   have[kTravelSlots] = {};
    float  loc0[kTravelSlots][3] = {};
    double capturedS = 0.0;
    double lastContactS = -1e9;
    float  lastMag = 0.f;      // receipt only
    int    lastSlot = -1;
    double lastLogS = 0.0;
    // ★ v12c: the bone this push is measured on, latched with the origin and held for the whole episode.
    // -1 = not yet set (the first qualifying contact after the capture sets it).
    int    lockSlot = -1;
    // v14a THE CROSSING CLOCK: the last time she was still sitting on her animation point. The lever divides
    // the bar by (now - max(capturedS, riseS)), so gate delay and a resting hand are no longer charged to it.
    double riseS = 0.0;
};
std::unordered_map<std::uint32_t, TravelOrigin> g_travel;

// Capture (or re-capture) the origin for every sensed bone. Caller holds g_walkMx.
// `force` re-captures even when one is already live (the walk-start re-anchor).
static void CaptureTravelOriginLocked(std::uint32_t id, double nowS, bool force, const char* why)
{
    if (g_travel.size() > 64) g_travel.clear();
    auto& t = g_travel[id];
    const bool lapsed = (nowS - t.lastContactS) > (double)ObjectHold::PushStepOriginHoldS();
    bool any = false;
    for (int i = 0; i < kTravelSlots; ++i) any |= t.have[i];
    if (!force && any && !lapsed) { t.lastContactS = nowS; return; }
    int got = 0;
    for (int i = 0; i < kTravelSlots; ++i) {
        float L[3];
        // ★ v13: capture each joint's offset from ITS ANIMATION POINT, not its position. The reference is the
        // animation from here on, so her walking, turning and posing are all tracked out for free; zeroing it
        // here removes each joint's resting servo lag without a baseline EMA.
        t.have[i] = ArmIK::GetPushGapRaw(id, kTravelSlot[i], L);
        if (t.have[i]) { t.loc0[i][0] = L[0]; t.loc0[i][1] = L[1]; t.loc0[i][2] = L[2]; ++got; }
    }
    t.capturedS = nowS; t.lastContactS = nowS; t.lastMag = 0.f;
    t.riseS = nowS;   // v14a: displacement is zero by definition at a capture
    t.lockSlot = -1;   // v12c: the next qualifying contact latches the bone for this episode
    if (why)           // v13a: a null reason = a silent re-capture (the per-frame self-move follow)
        logger::info("PUSHTRAVEL {:08X} origin captured ({}) — {} of {} joints read; the push is measured from HERE",
                     id, why, got, kTravelSlots);
}

// How far the touched bone has travelled from its captured origin, and in which WORLD direction.
// mag is the full 3-D distance (a backward bend is mostly DOWN — report 33 §1.3); the direction the
// walk uses is horizontal.
static bool TravelOf(std::uint32_t id, int slot, float yaw, float& dxW, float& dyW, float& mag);   // v12c fwd
// ★ v12c: the bone this push is measured on — the latched one, falling back to the live touched slot only
// before the latch is set. Caller must NOT hold g_walkMx.
static int TravelSlotOf(std::uint32_t id, int touched)
{
    std::scoped_lock lk(g_walkMx);
    auto it = g_travel.find(id);
    if (it != g_travel.end() && it->second.lockSlot >= 0) return it->second.lockSlot;
    return touched;
}
// *** v14d PER-NODE BARS. Returns the amount ADDED to all three rungs for this node. Every node is still
// judged 1:1 on its own displacement; this only says which threshold that displacement is measured against.
// *** v24: is this actor off-limits to the push system right now? Combat is arbitrated by the game and
// a shove reaction there both reads wrong and steps on it; a killmove or paired animation drives BOTH
// participants from one synchronised clip, so knocking one down mid-scene desyncs the pair.
// Covers every push output: the walk, the stumble, the knockdown and the lift ragdoll.
// ★★ v29d THE EQUIP SETTLE (user 2026-09-11: "An equip event prevent ragdoll or stumble"). An equip moves her
// GEOMETRY, not her body: heeled boots raise both feet ~8 u in one step, and on 2026-09-11 20:46:00.461 that fired
// LIFTED — BOTH FEET (+8.5 / +8.1) because the rest had been latched barefoot. While the window is open nothing
// fires; when it closes, the height at that moment becomes the new floor (see the lift block).
double NowS();   // defined below; the settle helpers are needed up here, by PushReactionsBlocked
struct EquipSettle { double stampS = 0.0; bool rebased = false; };
std::unordered_map<std::uint32_t, EquipSettle> g_equipSettle;
std::mutex g_equipMx;   // its OWN mutex: PushReactionsBlocked is called both inside and outside g_walkMx scopes

static bool EquipSettleActive(std::uint32_t id, double nowS)
{
    const float w = ObjectHold::PushStepEquipSettleS();
    if (w <= 0.f || !id) return false;
    std::scoped_lock lk(g_equipMx);
    auto it = g_equipSettle.find(id);
    return it != g_equipSettle.end() && (nowS - it->second.stampS) < (double)w;
}

// ★★ v36 (user ruling 2026-09-12): the FURNITURE half of the gate, and WHY — so a stand-down receipt names its real
// reason. `feetPath` = the caller is the leg-sweep lift (or the unsupported fall), not a push:
//   BUSY (sitting / sleeping / working / getting up) blocks BOTH paths.
//   LEAN blocks the PUSH path only — "someone leaning against something is really stable, but will still fall if their
//   legs are swept" — unless pushStepFurnLean 2 (fully free, v35's behaviour).
// A cache read of ArmIK::PushFurnitureVerdict — no 3D, safe in the 0x5E0885 movement hook. Legacy lever: no furniture gate.
// (Dialogue / conversation is deliberately NOT a gate — user ruling 2026-09-12; report 39 §3's dialogue design is dropped.)
static const char* PushFurnitureBlock(RE::Actor* a, bool feetPath)
{
    if (!a || !ObjectHold::PushStepFurnSitState()) return nullptr;
    const std::uint8_t v = ArmIK::PushFurnitureVerdict(a);
    if (v == ArmIK::kFurnBusy) return "she is IN FURNITURE (sitting / sleeping / working)";
    if (v == ArmIK::kFurnLean && !feetPath && !ObjectHold::PushStepFurnLeanPush())
        return "she is LEANING (stable against a push; a leg sweep still takes her)";
    return nullptr;
}

static bool PushReactionsBlocked(RE::Actor* a, bool feetPath = false)
{
    if (!a) return false;
    if (ObjectHold::PushStepCombatGate()   != 0.f && a->IsInCombat())   return true;
    if (ObjectHold::PushStepKillMoveGate() != 0.f && a->IsInKillMove()) return true;
    if (EquipSettleActive(a->GetFormID(), NowS())) return true;   // v29d: an equip is not a push
    // ★★ 2026-09-12 v35/v36 (user rulings): furniture = push/shove off; a LEAN = no push but the legs can still be swept.
    // Before v35 the only thing holding a seated NPC was the push sensor going blind; now it is a stated rule, with the
    // same verdict the sensor and the lift use.
    if (PushFurnitureBlock(a, feetPath)) return true;
    return false;
}

static float TravelBarOffsetFor(int slot, bool driving)
{
    float o = 0.f;
    switch (slot) {
        case 11: o = ObjectHold::PushStepOffCom();    break;   // COM
        case 4:  o = ObjectHold::PushStepOffSpine0(); break;
        case 5:  o = ObjectHold::PushStepOffSpine1(); break;
        case 6:  o = ObjectHold::PushStepOffSpine2(); break;
        case 7:  o = ObjectHold::PushStepOffNeck();   break;
        case 3:  o = ObjectHold::PushStepOffHead();   break;
        default: return 0.f;                                   // thighs etc: shared ladder, no walk term
    }
    // *** v20: while she DRIVES, her own gait lifts every node's reading by an amount that scales with
    // height up the spine (measured p90: COM +3.5 ... Head +47.1). Compensate per node so the gait alone
    // cannot cross any bar, and the node the PUSH loads is the one that decides.
    if (driving) {
        o += ObjectHold::PushStepOffWalkAdd();                 // the flat all-node term (v16), still honoured
        switch (slot) {
            case 11: o += ObjectHold::PushStepWalkOffCom();    break;
            case 4:  o += ObjectHold::PushStepWalkOffSpine0(); break;
            case 5:  o += ObjectHold::PushStepWalkOffSpine1(); break;
            case 6:  o += ObjectHold::PushStepWalkOffSpine2(); break;
            case 7:  o += ObjectHold::PushStepWalkOffNeck();   break;
            case 3:  o += ObjectHold::PushStepWalkOffHead();   break;
            default: break;
        }
    }
    return o;
}
// ★★ v13 THE WHOLE-BODY READING (user's spec). The decision is the LARGEST movement-from-animation-point
// across the six voting joints since the hand touched her — the contact location is irrelevant to it, exactly
// as the user specified ("it doesn't matter where the contact is originating"). Returns false until at least
// ★ v29c THE THREE RUNGS OF ONE NODE (user 2026-09-11: "the ladder is 10/20/35, let's make it 10/30/40 for head and
// neck. keep everything else as is"). TravelBarOffsetFor shifts all three rungs together, so it cannot express that.
// An override is the ABSOLUTE bar for that node's rung; 0 keeps the shared rung + the node's all-rung offset. The
// driving-only terms are re-added on top of an override (they cancel her own gait, not the player's push), and the
// ladder is clamped so no knob can invert it.
static void TravelBarsFor(int slot, bool driving, float& walk, float& stumble, float& rag)
{
    const float off = TravelBarOffsetFor(slot, driving);
    walk    = ObjectHold::PushStepTravelWalkU()    + off;
    stumble = ObjectHold::PushStepTravelStumbleU() + off;
    rag     = ObjectHold::PushStepTravelRagdollU() + off;
    float ovS = 0.f, ovR = 0.f;
    switch (slot) {
        case 11: ovS = ObjectHold::PushStepBarStumbleCom();    ovR = ObjectHold::PushStepBarRagCom();    break;
        case 4:  ovS = ObjectHold::PushStepBarStumbleSpine0(); ovR = ObjectHold::PushStepBarRagSpine0(); break;
        case 5:  ovS = ObjectHold::PushStepBarStumbleSpine1(); ovR = ObjectHold::PushStepBarRagSpine1(); break;
        case 6:  ovS = ObjectHold::PushStepBarStumbleSpine2(); ovR = ObjectHold::PushStepBarRagSpine2(); break;
        case 7:  ovS = ObjectHold::PushStepBarStumbleNeck();   ovR = ObjectHold::PushStepBarRagNeck();   break;
        case 3:  ovS = ObjectHold::PushStepBarStumbleHead();   ovR = ObjectHold::PushStepBarRagHead();   break;
        default: break;
    }
    if (ovS > 0.f || ovR > 0.f) {
        const float drive = driving ? (off - TravelBarOffsetFor(slot, false)) : 0.f;   // the gait term only
        if (ovS > 0.f) stumble = ovS + drive;
        if (ovR > 0.f) rag     = ovR + drive;
    }
    if (stumble < walk)    stumble = walk;
    if (rag     < stumble) rag     = stumble;
}

// one voting joint reads.
static bool TravelMax(std::uint32_t id, float yaw, float& dxW, float& dyW, float& mag, int& slotOut)
{
    mag = -1.f; slotOut = -1;
    // ★ v29d THE EQUIP SETTLE, first half: an equip moved her GEOMETRY, not her body. Publish NO reading while the
    // window is open, so neither a walk nor a stumble/ragdoll tier can engage off it ("Don't want her start to walk
    // back or something"). Every sensed node's origin is re-based when the window closes (the lift block).
    if (EquipSettleActive(id, NowS())) { mag = 0.f; return false; }
    int nVote = (int)ObjectHold::PushStepVoteN();
    if (nVote < 1) nVote = 1; else if (nVote > kTravelVoteMax) nVote = kTravelVoteMax;
    // v20: the RANKING must use the same walk-adjusted offsets, or the head (reading 50 while walking)
    // would win the vote over the COM (reading 8) even though both sit below their own bars.
    // ⛔ Read `driving` in its OWN scope and release: TravelOf below takes g_walkMx and std::mutex is
    // not recursive.
    bool drivingNow = false;
    {
        std::scoped_lock lk(g_walkMx);
        if (auto iw = g_walk.find(id); iw != g_walk.end()) drivingNow = iw->second.driving;
    }
    float best = -1e9f;
    for (int i = 0; i < nVote; ++i) {
        float dx, dy, m;
        if (!TravelOf(id, kTravelSlot[i], yaw, dx, dy, m)) continue;
        // v14d: rank by how far each node is past ITS OWN bar, so a node held to a higher bar must travel
        // that much further to win the vote. `mag` returned is the node's RAW displacement — every logged
        // number stays true; only the comparison point moves.
        const float adj = m - TravelBarOffsetFor(kTravelSlot[i], drivingNow);
        if (adj > best) { best = adj; mag = m; dxW = dx; dyW = dy; slotOut = kTravelSlot[i]; }
    }
    if (slotOut < 0) { mag = 0.f; return false; }
    return true;
}
// v14a THE CROSSING CLOCK. While she is still within pushStepCrossFloorU of her animation point the push has
// not started, so the lever's stopwatch keeps re-arming; it starts the frame she actually begins to move. This
// is what makes "how fast it goes from 0 to 5" true even when the hand has been resting on her for seconds.
// Must NOT be called while g_walkMx is held.
static void TravelNoteRise(std::uint32_t id, float mag, double nowS)
{
    if (mag >= ObjectHold::PushStepCrossFloorU()) return;
    std::scoped_lock lk(g_walkMx);
    if (auto it = g_travel.find(id); it != g_travel.end()) it->second.riseS = nowS;
}
// ★ v12c: every bone's travel in one string, so an attribution mistake is VISIBLE rather than inferred.
static void TravelDump(std::uint32_t id, float yaw, char* out, std::size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < kTravelSlots; ++i) {
        float dx, dy, m;
        const std::size_t len = std::strlen(out);
        if (TravelOf(id, kTravelSlot[i], yaw, dx, dy, m))
            std::snprintf(out + len, cap - len, "%ss%d:%.1f", i ? " " : "", kTravelSlot[i], m);
        else
            std::snprintf(out + len, cap - len, "%ss%d:-", i ? " " : "", kTravelSlot[i]);
    }
}

static bool TravelOf(std::uint32_t id, int slot, float yaw, float& dxW, float& dyW, float& mag)
{
    // (see TravelSlotOf below — callers pass the LATCHED bone, never the live touched slot)
    const int idx = TravelIdx(slot);
    if (idx < 0) return false;
    float L[3];
    if (!ArmIK::GetPushGapRaw(id, slot, L)) return false;   // v13: gap from the ANIMATION POINT
    float lx, ly, lz;
    {
        std::scoped_lock lk(g_walkMx);
        auto it = g_travel.find(id);
        if (it == g_travel.end() || !it->second.have[idx]) return false;
        lx = L[0] - it->second.loc0[idx][0];
        ly = L[1] - it->second.loc0[idx][1];
        lz = L[2] - it->second.loc0[idx][2];
    }
    // v13: the gap is already WORLD-space (node minus the FK'd animation point), so there is nothing to
    // un-rotate — and the delta since contact is world too. yaw is unused now; kept in the signature so the
    // call sites stay readable.
    (void)yaw;
    mag = std::sqrt(lx * lx + ly * ly + lz * lz);
    dxW = lx; dyW = ly;
    return true;
}

// v10.2 LIFT SENSOR state (under g_walkMx): a leg/trunk player contact stamps lastTouchS; frames counts
// consecutive both-feet-up frames. Separate from WalkState because a leg-only touch never creates one.
// ★ v11 (report 32 §2.2): the lift is measured from each NPC's OWN rest height. The rest is latched
// PER FOOT while she is UNTOUCHED and not walking, as a MEDIAN — measured 2026-09-07: while touched the
// feet band spans 3–6 u and its MINIMUM is the planted foot, which would put the bar under every step.
// Until it converges the heel lift (the "NPC" node's local Z, the same value HEELFIX reads) is the
// fallback, so the feature works from the first frame on a heeled NPC.
struct LiftState {
    double lastTouchS = 0.0; int frames = 0; double lastLogS = 0.0;
    static constexpr int kRestN = 45;
    float  restBuf[2][kRestN] = {};
    int    restN     = 0;
    double restLastS = 0.0;          // 4 Hz sampler — 45 samples span ~11 s of idle, several breath cycles
    bool   restOk    = false;
    float  restZ[2]  = { 0.f, 0.f }; // per-foot rest height above her ORIGIN
    float  restHeelU = 0.f;          // the heelZ the latch was taken at (re-latch if her shoes change)
    // v23: the LOWEST lift seen since this episode began, per foot. The rise gate measures against it,
    // so a foot that is merely parked high (the sitting false positive: R +27.9 frozen for 3.5 s) can
    // never qualify - only one that actually travelled upward can.
    float  minD[2]   = { 0.f, 0.f };
    bool   minOk     = false;
    // ---- v26 (2026-09-10) DIAGNOSTIC ONLY, report 35 sect L.6 build (a). Neither field is
    // read by any gate. ----------------------------------------------------------------
    // Her world origin Z at the instant the rest latched. The lift is measured RELATIVE to
    // her origin (foot_z - GetPosition().z), so a whole-body lift cancels out and reads ~0 —
    // identical to "she never left the ground". This one number separates them.
    float  restOriginZ = 0.f;
    // The lowest lift seen since the CURRENT contact episode began, tracked regardless of
    // symmetry or height (minD[] is gated on both, so it cannot show a lift that has not
    // qualified yet). This makes a SLOW or one-footed lift visible while it is happening.
    float  epMin[2]  = { 0.f, 0.f };
    bool   epOk      = false;
    // ---- v28 (2026-09-10) THE LIFT ZONE. With pushStepLiftZoneRule on, lastTouchS above means "a LIFT-ZONE
    // contact" (calf, foot, pelvis/COM, lower thigh); ANY contact still freezes the rest latch, exactly as
    // before, through lastAnyTouchS. The att* fields name the contact that attributed the lift (receipts).
    // (v28: epMin[] above is no longer display-only - the rise gate reads it when pushStepLiftRiseFromContact 1.)
    double lastAnyTouchS = 0.0;
    int    attWand  = -1;      // 2026-09-13 (R6): the player's hand (0 R / 1 L) behind the attributing contact
    int    attSlot  = -1;
    int    attChild = 0;
    bool   attLeft  = false;
    int    attSrc   = 0;
    int    attHow   = 0;       // thigh only: 1 probe point, 2 contacted capsule's centre, 3 rod centre
    float  attFrac  = -1.f;    // thigh only: 0 = hip .. 1 = knee
    float  attDistU = 0.f;
    char   attPart[48] = {};
    // ---- v29 (2026-09-10) THE COLLISION CAPSULE, ONE LEG, THE TRIP, THE FLOOR -----------------------------------
    int    attEngine = 0;                  // 1 = the attributing contact came from the Havok collision capsule (engine stamp)
    double legTouchS[2] = { 0.0, 0.0 };    // last lift-zone contact on her RIGHT [0] / LEFT [1] LEG (thigh mask / calf / foot)
    float  a0[2]   = { 0.f, 0.f };         // foot node-minus-animation Z captured with the rise floor (the trip reference)
    bool   a0Ok[2] = { false, false };
    bool   needGround   = false;           // after a knockdown or a seat: wait for both feet at floor level
    double groundSinceS = 0.0;             // when both feet first sat inside the floor band (0 = not yet)
    double quietLogS    = 0.0;             // throttle for the not-a-lift-zone watch line
    double comLogS      = 0.0;             // v29b: throttle for the pelvis-bar receipt
    double needSinceS   = 0.0;             // v29e: last frame she was DOWN or SEATED — the floor wait's deadline runs from here
};
std::unordered_map<std::uint32_t, LiftState> g_lift;

// ── v28 LIFT ZONE geometry (2026-09-10) ──────────────────────────────────────────────────────────────
// User: "Higher thigh contact is for a push/shove, anything lower than mid thigh is for a leg lift, and if
// there is lift from com, it's a leg lift." The digest names the CAPSULE a contact is on, not the point,
// and thigh child 0 is the whole bone - so the point comes from the probe set the scan used. MEASURED
// first: the one real between-the-legs lift (12:09) was on "mid thigh" (C4), ~0.6 of the way hip->knee -
// a split by capsule NAME would have lost it. Main thread only; never called under g_walkMx.
static float V3Dist2(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}
// t in [0,1] of the point on segment a->b nearest p
static float SegParamOf(const float a[3], const float b[3], const float p[3])
{
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float len2  = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 1e-8f ? ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] + (p[2] - a[2]) * ab[2]) / len2 : 0.f;
    return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}
// Closest approach of segments p1->q1 and p2->q2 (Ericson, clamped - PpbApi.cpp's SegSegDistU math).
// Returns the distance; *s1 = the parameter on the FIRST segment.
static float SegSegClosest(const float p1[3], const float q1[3], const float p2[3], const float q2[3], float* s1)
{
    const float d1[3] = { q1[0] - p1[0], q1[1] - p1[1], q1[2] - p1[2] };
    const float d2[3] = { q2[0] - p2[0], q2[1] - p2[1], q2[2] - p2[2] };
    const float r[3]  = { p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2] };
    const float A = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2];
    const float E = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    const float F = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
    float sN = 0.f, tN = 0.f;
    if (A <= 1e-8f && E <= 1e-8f) {
        // both degenerate: point-point
    } else if (A <= 1e-8f) {
        tN = F / E; tN = tN < 0.f ? 0.f : (tN > 1.f ? 1.f : tN);
    } else {
        const float C = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
        if (E <= 1e-8f) {
            sN = -C / A; sN = sN < 0.f ? 0.f : (sN > 1.f ? 1.f : sN);
        } else {
            const float B   = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2];
            const float den = A * E - B * B;
            sN = den > 1e-8f ? (B * F - C * E) / den : 0.f;
            sN = sN < 0.f ? 0.f : (sN > 1.f ? 1.f : sN);
            tN = (B * sN + F) / E;
            if (tN < 0.f)      { tN = 0.f; sN = -C / A; }
            else if (tN > 1.f) { tN = 1.f; sN = (B - C) / A; }
            sN = sN < 0.f ? 0.f : (sN > 1.f ? 1.f : sN);
        }
    }
    const float c1[3] = { p1[0] + d1[0] * sN, p1[1] + d1[1] * sN, p1[2] + d1[2] * sN };
    const float c2[3] = { p2[0] + d2[0] * tN, p2[1] + d2[1] * tN, p2[2] + d2[2] * tN };
    if (s1) *s1 = sN;
    return std::sqrt(V3Dist2(c1, c2));
}
// Where along her thigh a slot-8 contact sits: *fracOut = 0 at the hip .. 1 at the knee. Returns how the
// point was found: 1 = the player's probe (its nearest point on the contacted capsule), 2 = the contacted
// capsule's centre, 3 = the whole-bone rod's centre (child 0 and no probe), 0 = her thigh did not read.
// The palm is not in CopyProbes (orifice ruling 2026-09-06), so a HAND contact also tries HIGGS's live
// palm box.
// ★ v29 THE COLLISION CAPSULE DECIDES THE THIGH (user 2026-09-10: "thigh split is different capsule ... Use the collision
// capsule, we own them"). Every ring child counts by its IDENTITY: bit N of pushStepLiftThighMask = child N credits a lift
// (default 112 = C4 mid thigh, C5 lower thigh, C6 knee; C1 groin, C2 hip-glute fold, C3 upper thigh push). Only the ROD
// (child 0, the bone core inside the rings) is placed by the hand's position along it.
// ⚠ v28's "C4 sits ~0.60 hip->knee" was the COMPILED default capsule; the live dial puts C4 at 0.41-0.56 (report 37 §2.1),
// and clamping the probe to a capsule made v28 a name rule anyway.
static bool ThighChildLifts(int child)
{
    if (child <= 0 || child > 30) return false;
    const int m = (int)ObjectHold::PushStepLiftThighMask();
    return ((m >> child) & 1) != 0;
}
static int ThighContactFrac(RE::Actor* actor, const PPBAPI::PpbTouchContact& ct,
                            const PpbApi::ProbeView* pv, int np, float* fracOut)
{
    const bool left = ct.leftTwin != 0;
    float ra[3], rb[3], rr = 0.f;
    if (!GrabDiag::ReadCapsuleWorldUSide(actor, 8, left, 0, ra, rb, &rr)) return 0;
    {   // orient the rod hip -> knee: the knee is the end nearer her calf (fallback: the hip is nearer her pelvis)
        float ka[3], kb[3], kr = 0.f;
        bool swapEnds = false;
        if (GrabDiag::ReadCapsuleWorldUSide(actor, 9, left, 0, ka, kb, &kr)) {
            const float km[3] = { (ka[0] + kb[0]) * 0.5f, (ka[1] + kb[1]) * 0.5f, (ka[2] + kb[2]) * 0.5f };
            swapEnds = V3Dist2(ra, km) < V3Dist2(rb, km);
        } else if (GrabDiag::ReadCapsuleWorldUSide(actor, 11, false, 0, ka, kb, &kr)) {
            const float pm[3] = { (ka[0] + kb[0]) * 0.5f, (ka[1] + kb[1]) * 0.5f, (ka[2] + kb[2]) * 0.5f };
            swapEnds = V3Dist2(rb, pm) < V3Dist2(ra, pm);
        }
        if (swapEnds) for (int k = 0; k < 3; ++k) { const float tmp = ra[k]; ra[k] = rb[k]; rb[k] = tmp; }
    }
    float ca[3], cb[3], cr = 0.f;
    const bool childOk = ct.child > 0 && GrabDiag::ReadCapsuleWorldUSide(actor, 8, left, ct.child, ca, cb, &cr);
    if (!childOk) { for (int k = 0; k < 3; ++k) { ca[k] = ra[k]; cb[k] = rb[k]; } cr = rr; }
    const int wantCls = (ct.sourceKind <= PPBAPI::kSourceGrab)   ? 0     // PpbApi kClsHand
                      : (ct.sourceKind == PPBAPI::kSourceWeapon) ? 1     // kClsWeapon
                                                                 : 2;    // kClsObject
    float bestD = 12.f, bestS = -1.f;   // a probe farther than 12u from that capsule did not make this contact
    for (int k = 0; k < np; ++k) {
        const PpbApi::ProbeView& v = pv[k];
        if (v.cls != wantCls || v.wand != (int)ct.wand) continue;
        if (v.cls == 1 && v.grabActorId == actor->GetFormID()) continue;   // the per-target grab mute (PpbApi.h)
        float s = 0.f, d = 0.f;
        if (v.seg) {
            d = SegSegClosest(ca, cb, v.p, v.q, &s);
        } else {
            s = SegParamOf(ca, cb, v.p);
            const float pt[3] = { ca[0] + (cb[0] - ca[0]) * s, ca[1] + (cb[1] - ca[1]) * s, ca[2] + (cb[2] - ca[2]) * s };
            d = std::sqrt(V3Dist2(v.p, pt));
        }
        d -= cr + v.pad;
        if (d < bestD) { bestD = d; bestS = s; }
    }
    if (wantCls == 0) {                                   // the palm: HIGGS's own hand box, read live
        GrabDiag::ObjBoxU ob{};
        if (GrabDiag::HandSlabBoxU(ct.wand == 1, ob)) {
            const float s = SegParamOf(ca, cb, ob.c);
            const float pt[3] = { ca[0] + (cb[0] - ca[0]) * s, ca[1] + (cb[1] - ca[1]) * s, ca[2] + (cb[2] - ca[2]) * s };
            const float hMin = (std::min)(ob.h[0], (std::min)(ob.h[1], ob.h[2]));
            const float d = std::sqrt(V3Dist2(ob.c, pt)) - cr - hMin;
            if (d < bestD) { bestD = d; bestS = s; }
        }
    }
    float pt[3];
    int how;
    if (bestS >= 0.f) { for (int k = 0; k < 3; ++k) pt[k] = ca[k] + (cb[k] - ca[k]) * bestS; how = 1; }
    else              { for (int k = 0; k < 3; ++k) pt[k] = (ca[k] + cb[k]) * 0.5f;           how = childOk ? 2 : 3; }
    *fracOut = SegParamOf(ra, rb, pt);
    return how;
}

// v9.9: push one gap sample and return the rate over the OLDEST sample still inside winS —
// 0 while the history spans less than half the window (a fresh touch has no speed yet; the
// escalation pass will read it a few frames on). Caller holds g_walkMx.
static void RingReset(WalkState& ws) { ws.ringN = 0; ws.ringHead = 0; ws.rateW = 0.f; }
static float RingPush(WalkState& ws, float mag, double nowS, float winS)
{
    ws.ringMag[ws.ringHead] = mag; ws.ringS[ws.ringHead] = nowS;
    ws.ringHead = (ws.ringHead + 1) % WalkState::kRing;
    if (ws.ringN < WalkState::kRing) ++ws.ringN;
    float oMag = mag; double oS = nowS; bool found = false;
    for (int k = 1; k < ws.ringN; ++k) {                       // newest-1 … oldest
        const int idx = (ws.ringHead - 1 - k + 2 * WalkState::kRing) % WalkState::kRing;
        if (nowS - ws.ringS[idx] > (double)winS) break;        // past the window (samples are ordered)
        oMag = ws.ringMag[idx]; oS = ws.ringS[idx]; found = true;
    }
    const double span = nowS - oS;
    ws.rateW = (found && span >= 0.5 * (double)winS) ? (float)((mag - oMag) / span) : 0.f;
    return ws.rateW;
}

double NowS()
{
    using clk = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               clk::now().time_since_epoch()).count();
}

// Per-region walk distance: the user's stiffness map — a push on the hips/COM moves the whole
// body furthest, the upper chest least (the shoulders give before the feet answer).
// v4: last instant we DROVE this actor, surviving the full-release erase of its WalkState —
// the settle gate must hold across releases or a fresh touch measures her own locomotion lag.
static std::unordered_map<std::uint32_t, double> g_lastDriveS;
// Reaction cooldown that OUTLIVES the WalkState — the ragdoll branch erases the
// WalkState, which used to reset the in-state cooldown and fire a second reaction
// ~159 ms later on every knockdown (measured 3/3, Ragdoll Research 05 section 1).
static std::unordered_map<std::uint32_t, double> g_reactCoolS;
// v19: the last time a RAGDOLL specifically fired. Separate from g_reactCoolS so a knockdown may
// pre-empt a stagger's cooldown while knockdown-to-knockdown stays fully gated.
static std::unordered_map<std::uint32_t, double> g_reactRagS;
static double LastDriveS(std::uint32_t id)
{
    std::scoped_lock lk(g_walkMx);
    auto it = g_lastDriveS.find(id);
    return it == g_lastDriveS.end() ? -1e9 : it->second;
}
static void StampDriveS(std::uint32_t id, double nowS)
{
    std::scoped_lock lk(g_walkMx);
    if (g_lastDriveS.size() > 512) g_lastDriveS.clear();
    g_lastDriveS[id] = nowS;
}

// v6: the speed envelope. Phase 0 ramps 15% -> 100% of targetSpeed over pushStepRampInS;
// phase 1 fades the frozen decel speed to 0 over pushStepRampOutS. Pure function of state+time.
static float EnvelopeSpeed(const WalkState& w, double nowS)
{
    if (ObjectHold::PushStepSpeedTrack() != 0.f) return w.cmdSpeedU;   // v10.0: the follower owns the speed
    if (w.phase == 1) {
        const float T = (std::max)(0.05f, ObjectHold::PushStepRampOutS());
        const float f = 1.f - (float)((nowS - w.decelStartS) / (double)T);
        return w.decelSpeedU * (f > 0.f ? f : 0.f);
    }
    const float T = (std::max)(0.05f, w.rampTS);           // v8.0: aggro-scaled per episode
    float r = (float)((nowS - w.engageS) / (double)T);
    float floor = w.rampFloorE;
    if (floor < 0.f) floor = 0.f;
    if (floor > 1.f) floor = 1.f;
    if (r < floor) r = floor;
    if (r > 1.f)   r = 1.f;
    return w.targetSpeedU * r;
}

std::uint32_t g_lastTouchedActor = 0;   // v8.6: the idle probe's subject

// v9.7: actors knocked down by a push, and the instant their settle window ends. While the
// window is open the frame tick caps their body speed so the deep-intruder separation impulse
// (a blade inside her chest at the moment she goes dynamic) cannot launch her.
std::unordered_map<std::uint32_t, double> g_ragSettle;

// v8.5 forensics: all seven sensed bone gaps in one string ("s4:0.21 s5:1.80 ...").
static void GapDump(std::uint32_t id, char* out, std::size_t cap)
{
    static constexpr int kS[7] = { 4, 5, 6, 11, 8, 7, 3 };
    out[0] = 0;
    for (int k = 0; k < 7; ++k) {
        float dx, dy, m;
        const std::size_t len = std::strlen(out);
        if (ArmIK::GetPushSenseGap(id, kS[k], 0.001f, dx, dy, m))
            std::snprintf(out + len, cap - len, "%ss%d:%.2f", k ? " " : "", kS[k], m);
        else
            std::snprintf(out + len, cap - len, "%ss%d:-", k ? " " : "", kS[k]);
    }
}

// ★ v9.5 THE REACTION TIERS. Vanilla's own stagger: two graph floats + the "staggerStart"
// event; the direction is the heading angle TO THE AGGRESSOR normalised to 0..1, which is the
// convention every stagger mod uses. Ragdoll is the graph's own "Ragdoll" event (the same one
// the get-up pose matcher listens for - ABX research doc 02).
// ⛔ QUEUED THROUGH THE TASK INTERFACE, never called inline: engine calls from inside the
// movement-evaluation hook are what froze the game on 2026-08-29 (EvaluatePackage).
// Actor_CanBeKnockedDown — VR 0x5EB870. PLANCK's own binding (offsets.cpp:306, "checks stuff
// like the kNoKnockdowns flag"); CommonLibVR does not expose it. Guards the engine-first onset:
// a race that cannot be knocked down must never be handed KnockExplosion.
using Fn_CanBeKnockedDown = bool (*)(RE::Actor*);
static REL::Relocation<Fn_CanBeKnockedDown> g_CanBeKnockedDown{ REL::Offset(0x5EB870) };
static bool CanBeKnockedDown(RE::Actor* a)
{
    if (!a) return false;
    return g_CanBeKnockedDown(a);
}

// v11 15:45: retreatHeading == kNoDir means "no push direction" (a LIFT or an UNSUPPORTED fall) — the knock
// then throws her away from the PLAYER instead of from a direction we do not have.
static constexpr float kNoDir = -1.0e9f;

// ★★ 2.2.0 THE PUSH EVENT (user, 2026-09-12): "Player's push/shove/dropped/sweeped (NPC's name) each time it happens,
// through the API — another event exposed, like the equip, but for push." One SKSE mod event, published alongside the
// gesture bus in PpbTouchAPI.h (same contract, same append-only rule):
//   PPB_PushReaction   strArg "<kind>|<NPC display name>|<wand R|L>|<slot>|<child>|<leftTwin>"   numArg 0   sender = the NPC
//   ★ 2026-09-13 (VRTE GearGestures R6, build 20105): the four PUSHER fields are APPENDED — the player's hand and the
//   capsule it last pressed (a leg sweep: the contact the lift was credited to). Empty when no contact is on record.
//   The name is '|'-free (a '|' becomes '/'), so the payload splits on every '|'.
//     kind  push     a push walk ENGAGED (she starts stepping back) — once per engage, never per frame
//           shove    a push STUMBLE played
//           dropped  a SHOVE KNOCKDOWN played
//           sweeped  a LEG SWEEP knockdown played (both feet lifted; also the unsupported-fall path, which ships off)
// ⛔ MAIN THREAD ONLY, and only for a reaction the game ACCEPTED: the stumble/knockdown send it from inside
// QueueReaction's task after the graph/knock call returned ok, the walk queues it onto the task interface. A refused
// knock sends nothing, so a consumer never narrates something the player did not see.
struct PushWho { int wand = -1; int slot = -1; int child = 0; bool left = false; };

// The hand that pressed most recently, and the capsule it pressed on.
static PushWho PusherOf(const WalkState& w)
{
    PushWho p;
    int h = -1;
    for (int k = 0; k < 2; ++k)
        if (w.lastPressWandS[k] > -1e8 && (h < 0 || w.lastPressWandS[k] > w.lastPressWandS[h])) h = k;
    if (h >= 0) { p.wand = h; p.slot = w.pressSlot[h]; p.child = w.pressChild[h]; p.left = w.pressLeft[h]; }
    return p;
}

static void SendPushReactionOnMain(RE::Actor* a, const char* kind, const PushWho& who)
{
    auto* src = SKSE::GetModCallbackEventSource();
    if (!a || !src || !kind) return;
    const char* nm = a->GetDisplayFullName();
    char nmF[128];
    {
        std::size_t i = 0;
        for (; nm && nm[i] && i + 1 < sizeof nmF; ++i) nmF[i] = (nm[i] == '|') ? '/' : nm[i];
        nmF[i] = '\0';
    }
    char slotS[8] = "", childS[8] = "", leftS[4] = "";
    if (who.slot >= 0) {
        std::snprintf(slotS, sizeof slotS, "%d", who.slot);
        std::snprintf(childS, sizeof childS, "%d", who.child);
        std::snprintf(leftS, sizeof leftS, "%d", who.left ? 1 : 0);
    }
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s|%s|%s|%s|%s|%s", kind, nmF,
                  who.wand < 0 ? "" : (who.wand ? "L" : "R"), slotS, childS, leftS);
    SKSE::ModCallbackEvent ev{};
    ev.eventName = "PPB_PushReaction";
    ev.strArg    = buf;
    ev.numArg    = 0.f;
    ev.sender    = a;
    src->SendEvent(&ev);
    logger::info("PUSHEVENT {:08X} PPB_PushReaction \"{}\"", a->GetFormID(), buf);
}

static void QueueReaction(std::uint32_t id, float relDeg, float mag, bool ragdoll, const char* kind,
                          PushWho who, float retreatHeading = kNoDir)
{
    // v11: the sensor stands down for the reaction we are about to play (user, 2026-09-07 — the stumble
    // itself was re-triggering the tier). Ragdolls are additionally covered by the knock state in PPBHook.
    ArmIK::SenseStandDown(id, ObjectHold::PushSenseStandDownS());
    auto* task = SKSE::GetTaskInterface();
    if (!task) return;
    task->AddTask([id, relDeg, mag, ragdoll, kind, who, retreatHeading]() {   // `kind` is always a string literal
        auto* f = RE::TESForm::LookupByID(id);
        auto* a = f ? f->As<RE::Actor>() : nullptr;
        if (!a) return;
        if (ragdoll) {
            // ── THE THREE ONSETS (2026-09-03, Ragdoll Research 04 §1c / 05 §4 / 07 §2.1) ──────
            // Two of these are the discriminating EXPERIMENTS and two are the candidate FIXES,
            // which is why they are one knob rather than a code change per test.
            //   0 baseline  — the graph event. The Master wildcard cross-fades into FullyRagdoll
            //                 over 0.2 s (bound to `blendDefault`), and PLANCK has NO branch for
            //                 "fading into ragdoll": for ~13 frames it keeps driving her toward a
            //                 target that has just jumped and is sweeping to a lying pose, motors
            //                 at 500, gravity zeroed, and on layer 8 with no floor and no walls.
            //                 The engine's own knockdown only arrives when PLANCK's watchdog
            //                 fires PushActorAway(0), measured >= 159 ms later.
            //   1 engine-first — AIProcess::KnockExplosion(actor, pos, 0). The engine sets its
            //                 knock state and IsInRagdollState on the EVENT frame, so PLANCK's
            //                 next drive stamps getUpMaxForce 0 and returns BEFORE zeroing
            //                 gravity, and its x0.3 ragdolled hit multiplier applies from frame 0
            //                 instead of ~13 frames late. Force 0 is four other projects'
            //                 "collapse in place", and it is what PLANCK itself calls for every
            //                 deliberate ragdoll. Predicted milder under H10 AND H3.
            //   2 instant   — "RagdollInstant" has NO transition effect at all, so the driven
            //                 sweep disappears (the one-frame target step remains). Predicted
            //                 much milder under H10 only — which is exactly what separates it.
            const int onset = (int)ObjectHold::PushStepOnset();
            bool ok = false;
            const char* what = "Ragdoll";
            if (onset == 1) {
                what = "KnockExplosion(0)";
                auto* proc = a->GetActorRuntimeData().currentProcess;
                auto* cc   = a->GetCharController();
                // the preconditions the open-source callers all use (Trick-Death, More-Ragdoll,
                // ALYSLC — Ragdoll Research G01 §2.2): high process, controller actually in the
                // Havok world, the race may be knocked down at all, 3D loaded.
                const bool high    = proc && proc->InHighProcess();
                const bool ccWorld = cc && cc->GetHavokWorld() != nullptr;
                const bool canKnock = CanBeKnockedDown(a);
                const bool loaded  = a->Is3DLoaded();
                if (proc && high && ccWorld && canKnock && loaded) {
                    // ⛔ v11 15:45 (user: "I pushed sofia, I backed away, and she ragdoll in front of me going
                    // FORWARD"). This passed HER OWN position as the explosion origin: the engine derives the
                    // throw direction from (actor - pos), which is then a ZERO vector, and falls back to her
                    // facing — so every knockdown threw her forward. It was invisible on a push-tier ragdoll
                    // (she already carried backward momentum from the walk) and obvious on a lift/unsupported
                    // ragdoll from standing. The origin must sit on the AGGRESSOR's side: opposite the retreat
                    // for a push, the player's own position when we have no push direction (lift / unsupported).
                    RE::NiPoint3 src = a->GetPosition();
                    const float back = ObjectHold::PushStepKnockSrcU();
                    if (retreatHeading > kNoDir * 0.5f) {
                        src.x -= std::sin(retreatHeading) * back;   // heading convention: dir = (sin h, cos h)
                        src.y -= std::cos(retreatHeading) * back;
                    } else if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                        const RE::NiPoint3 pp = pl->GetPosition();
                        const float dx = pp.x - src.x, dy = pp.y - src.y;
                        const float d  = std::sqrt(dx * dx + dy * dy);
                        if (d > 1.f) { src.x += (dx / d) * back; src.y += (dy / d) * back; }
                    }
                    proc->KnockExplosion(a, src, 0.0f);
                    ok = true;
                } else {
                    // ⛔ every refusal speaks — a silent fallback here would look exactly like
                    // "engine-first made no difference" and quietly invalidate the experiment.
                    logger::info("PUSHREACT {:08X} engine-first onset REFUSED (highProcess={} "
                                 "ccInWorld={} canBeKnockedDown={} 3dLoaded={}) — falling back to "
                                 "the graph event", id, high ? 1 : 0, ccWorld ? 1 : 0,
                                 canKnock ? 1 : 0, loaded ? 1 : 0);
                    what = "Ragdoll(fallback)";
                    ok = a->NotifyAnimationGraph("Ragdoll");
                }
            } else if (onset == 2) {
                what = "RagdollInstant";
                ok = a->NotifyAnimationGraph("RagdollInstant");
            } else {
                ok = a->NotifyAnimationGraph("Ragdoll");
            }
            logger::info("PUSHREACT {:08X} RAGDOLL onset={} ({}) (accepted={})",
                         id, onset, what, ok ? 1 : 0);
            if (ok) SendPushReactionOnMain(a, kind, who);     // 2.2.0: "dropped" / "sweeped"
            return;
        }
        // aggressor sits opposite the retreat bearing; normalise to the graph's 0..1
        float agg = relDeg + 180.f;
        while (agg > 180.f)  agg -= 360.f;
        while (agg < -180.f) agg += 360.f;
        float dir01 = agg / 360.f;
        if (dir01 < 0.f) dir01 += 1.f;
        // ★ v10.1 STAGGER FACING (user ruling 2026-09-06): "face the player if the shove comes from the
        // front or side, face away if it comes from behind." The direction was computed against her
        // facing at the FIRE instant — 55 ms after engage, before the walk's turn had moved her — so a
        // side shove played a sideways shuffle in place. Now she is turned FIRST (Actor::SetRotationZ —
        // SE id 36248 → VR 0x5D95A0 in the address library, the engine's own actor yaw setter) and the
        // stagger is fired from the FRONT (stumble backward) or from BEHIND (stumble forward); the parked
        // walk's locked facing follows so a resumed walk does not turn her back.
        char faced[224] = "";
        if (ObjectHold::PushStepStaggerFace() != 0.f) {
            // ★ v10.3 (user ruling 2026-09-06 23:00): "the push/shove is from where the push is acting on the
            // NPC, not where the player is relative to the NPC — I may face her front and push from her left
            // with my weapon." The aggressor is OPPOSITE the push's retreat direction (retreatHeading = the
            // validated engage retreat, or the walk's retreat during a walk — never the live mid-walk gap).
            // pushStepStaggerFaceSrc 1 = v10.2's player-position rule, kept for A/B.
            const float before = a->data.angle.z;
            float aggHeading = retreatHeading + 3.14159265f;
            const char* srcName = "push";
            if (ObjectHold::PushStepStaggerFaceSrc() != 0.f) {
                if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                    const auto pp = pl->GetPosition(); const auto ap = a->GetPosition();
                    const RE::NiPoint3 d{ pp.x - ap.x, pp.y - ap.y, 0.f };
                    if (d.x * d.x + d.y * d.y > 1.f) { aggHeading = GetHeadingFromVector(d); srcName = "player"; }
                }
            }
            float rel = (aggHeading - before) * 57.2957795f;      // where the push comes from, from her front
            while (rel >  180.f) rel -= 360.f;
            while (rel < -180.f) rel += 360.f;
            const bool fromBehind = std::fabs(rel) > ObjectHold::PushStepStaggerFaceDeg();
            float yaw = fromBehind ? aggHeading + 3.14159265f : aggHeading;   // face away | face the push
            while (yaw >  3.14159265f) yaw -= 6.28318531f;
            while (yaw < -3.14159265f) yaw += 6.28318531f;
            // staggerDirection: 0 = from the front (stumble back), 0.5 = from behind (stumble forward);
            // pushStepStaggerDirFlip swaps the two HOT. pushStepStaggerTurnMode: 0 = Actor::SetRotationZ
            // (VR 0x5D95A0; the yaw is verified to land), 1 = TESObjectREFR::SetAngle + Update3DPosition(true)
            // (the Papyrus/console path), 2 = NO turn — vanilla directional stagger in her current facing
            // (she stumbles away from the push wherever she faces; the walk's FacePush turns her over time).
            const bool flip = ObjectHold::PushStepStaggerDirFlip() != 0.f;
            const int  mode = (int)ObjectHold::PushStepStaggerTurnMode();
            float after = before;
            if (mode == 2) {
                float d01 = rel / 360.f; if (d01 < 0.f) d01 += 1.f;        // aggressor bearing → the graph's 0..1
                dir01 = flip ? (d01 + 0.5f > 1.f ? d01 - 0.5f : d01 + 0.5f) : d01;
            } else {
                if (mode == 1) { auto ang = a->data.angle; ang.z = yaw; a->SetAngle(ang); a->Update3DPosition(true); }
                else           { a->SetHeading(yaw); }
                after = a->data.angle.z;
                dir01 = (fromBehind != flip) ? 0.5f : 0.f;
            }
            std::snprintf(faced, sizeof faced,
                          " [%s at %+.0f° from her front -> %s; yaw %.0f -> wrote %.0f -> reads %.0f deg; mode %d%s]",
                          srcName, rel,
                          mode == 2 ? "no turn, directional stagger" : (fromBehind ? "faced AWAY, stumble forward" : "faced the push, stumble back"),
                          before * 57.2957795f, (mode == 2 ? before : yaw) * 57.2957795f, after * 57.2957795f, mode, flip ? " (dir flipped)" : "");
            if (mode != 2) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) it->second.angleZ = yaw;
            }
        }
        a->SetGraphVariableFloat("staggerDirection", dir01);
        a->SetGraphVariableFloat("staggerMagnitude", mag);
        const bool ok = a->NotifyAnimationGraph("staggerStart");
        logger::info("PUSHREACT {:08X} STAGGER mag {:.2f} dir {:.2f} (aggressor {:+.0f}° from her "
                     "front; graph event accepted={}){}", id, mag, dir01, agg, ok ? 1 : 0, faced);
        if (ok) SendPushReactionOnMain(a, kind, who);    // 2.2.0: "shove"
    });
}

float WalkIdealFor(int slot)
{
    const float base = ObjectHold::PushStepWalkU();
    switch (slot) {                                       // v7.1: all multipliers are knobs
    case 11: return base * ObjectHold::PushStepMulCom();    // com / hips — the whole-body shove
    case 8:
    case 108: return base * ObjectHold::PushStepMulThigh();  // either thigh — where a hip press lands
    case 4:  return base * ObjectHold::PushStepMulWaist();  // spine0 / waist
    case 5:  return base * ObjectHold::PushStepMulBelly();  // spine1 / belly
    case 6:  return base * ObjectHold::PushStepMulChest();  // spine2 / chest
    case 7:  return base * ObjectHold::PushStepMulNeck();   // v8.3 neck
    case 3:  return base * ObjectHold::PushStepMulHead();   // v8.3 head
    default: return base;
    }
}

// ══ v27 (2026-09-10) GET-UP PROBE — DIAGNOSTIC ONLY: no gate reads anything below ══════════════════════════
// User: "why a timing? can we just track when a NPC completed their recovery animation, and close the gate once
// that happened?" Research this session: the engine's own completion signal is the ANIMATION EVENT "GetUpEnd" — a
// clip trigger on every vanilla humanoid get-up/reanimate clip, and the event the engine keys its own
// GetUpEndHandler to. It fires only when a get-up CLIP plays (not for death, a PLANCK-held actor, a cancelled
// knockdown, or after a graph rebuild), so the knock-state edge stays the confirm. This probe settles what no
// static read could (the VR exe is DRM-packed): (1) does GetUpEnd reach a THIRD-PARTY sink on an NPC in VR, and
// (2) when does the 3-bit knock field flip back to kNormal relative to it.
// It also closes the ambiguity that cost the 12:40 session: while she is down the lift block is skipped and says
// NOTHING, so "she was already on the ground" read exactly like "the gate rejected it".
// ⛔ THREADS. ProcessEvent runs on whatever thread raised the animation event, holding that event source's spin
// lock. It copies a POD into a ring under its OWN mutex and returns: no allocation, no logging, no PPB lock, no
// engine call. That mutex is never held around anything else, so it cannot invert against g_walkMx or the engine.
// Everything else here runs on the MAIN thread (PushStep::OnFrame, ClearOnLoad).
struct GetUpEvt {
    std::uint32_t id    = 0;
    int           knock = -1;
    double        t     = 0.0;
    char          tag[48] = {};
};
constexpr int  kGetUpRing = 128;
GetUpEvt       g_getupRing[kGetUpRing];
int            g_getupN = 0;
std::uint32_t  g_getupDropped = 0;
std::mutex     g_getupMx;

// The get-up family is recorded ALWAYS; every other event only while she is knocked down. Recording the family
// regardless of the knock state IS the positive control: if the field flips to kNormal BEFORE GetUpEnd, a probe
// filtered on "knock != 0" would miss the event and wrongly conclude it never arrives (Part 08 failure class 1 —
// a diagnostic's own filter encoding an assumption).
bool IsGetUpFamily(const char* t)
{
    if (!t) return false;
    return _strnicmp(t, "getup", 5) == 0 ||                          // GetUpEnd / GetUpExit / GetUpStart / GetUpBegin / Getup
           _strnicmp(t, "ragdoll", 7) == 0 ||                        // Ragdoll / RagdollInstant
           _stricmp(t, "AddCharacterControllerToWorld") == 0 ||
           _stricmp(t, "RemoveCharacterControllerFromWorld") == 0;
}

class GetUpSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* ev,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
    {
        // ⛔ kContinue ALWAYS: kStop breaks the engine's dispatch loop and starves every sink behind us.
        if (!ev || !ev->holder) return RE::BSEventNotifyControl::kContinue;
        auto* a = ev->holder->As<RE::Actor>();
        if (!a) return RE::BSEventNotifyControl::kContinue;
        const char* tag = ev->tag.c_str();
        int knock = -1;
        if (auto* st = a->AsActorState()) knock = static_cast<int>(st->GetKnockState());
        if (knock == 0 && !IsGetUpFamily(tag)) return RE::BSEventNotifyControl::kContinue;
        GetUpEvt e;
        e.id    = a->GetFormID();
        e.knock = knock;
        e.t     = NowS();
        if (tag) {
            std::size_t i = 0;
            for (; i + 1 < sizeof(e.tag) && tag[i]; ++i) e.tag[i] = tag[i];
            e.tag[i] = 0;
        }
        std::scoped_lock lk(g_getupMx);
        if (g_getupN < kGetUpRing) g_getupRing[g_getupN++] = e;
        else ++g_getupDropped;
        return RE::BSEventNotifyControl::kContinue;
    }
};
// Process lifetime ON PURPOSE. A graph that outlives our interest in her (unload, cell change) still holds a
// pointer to this object, and a static can never be a dead vtable. Nothing removes it; the engine drops it with
// the graph, and the 1 Hz re-attach below puts it back on the new graph.
GetUpSink g_getupSink;

struct GetUpProbe {
    int    prevKnock    = -1;
    bool   prevRag      = false;
    double leftNormalS  = 0.0;   // when the knock field last left kNormal (0)
    double backNormalS  = 0.0;   // when it last came back to kNormal
    double lastAttachS  = -1e9;
    double lastSkipLogS = 0.0;
    char   seq[64]      = {};    // the knock sequence of the current episode, e.g. "0>2>1>5>6>0"
};
std::unordered_map<std::uint32_t, GetUpProbe> g_getupProbe;   // MAIN THREAD ONLY (OnFrame + ClearOnLoad)

void GetUpSeqAppend(char* s, std::size_t cap, int v)
{
    std::size_t n = 0;
    while (n < cap && s[n]) ++n;
    if (n + 3 >= cap) return;                              // full: keep what we have
    if (n) s[n++] = '>';
    s[n++] = static_cast<char>('0' + ((v >= 0 && v <= 9) ? v : 9));
    s[n] = 0;
}

void GetUpProbeClear()
{
    g_getupProbe.clear();
    std::scoped_lock lk(g_getupMx);
    g_getupN = 0;
    g_getupDropped = 0;
}

void GetUpProbeTick(double nowS)
{
    // Scoped to the lift feature: with pushStepLiftRag 0 there is no gate to explain. Drain-and-discard so a
    // later re-enable never prints stale timings.
    if (!ObjectHold::PushStepLiftRagOn()) {
        std::scoped_lock lk(g_getupMx);
        g_getupN = 0;
        g_getupDropped = 0;
        return;
    }
    // 1) the actors the lift gate knows, with their last player contact — copy, then DROP the lock before any
    //    engine call (AddAnimationGraphEventSink takes the event source's spin lock).
    struct Tgt { std::uint32_t id; double lastTouch; };
    Tgt tg[24];
    int tn = 0;
    {
        std::scoped_lock lk(g_walkMx);
        for (auto& [lid, L] : g_lift) {
            if (tn < 24) tg[tn++] = Tgt{ lid, L.lastAnyTouchS };   // v28: any contact, exactly as v27 (lastTouchS is lift-zone only now)
        }
    }
    const double grace = (double)ObjectHold::PushStepFallGraceS();
    if (g_getupProbe.size() > 64) g_getupProbe.clear();

    for (int i = 0; i < tn; ++i) {
        auto* f = RE::TESForm::LookupByID(tg[i].id);
        auto* a = f ? f->As<RE::Actor>() : nullptr;
        if (!a || !a->Get3D()) continue;
        auto& P = g_getupProbe[tg[i].id];

        // 2) attach. Actor::AddAnimationGraphEventSink de-duplicates across the graph manager's graphs and returns
        //    true only when it actually added, so asking once a second is cheap AND self-heals a graph rebuild
        //    (cell load, race swap, resurrect), which silently drops a sink. The receipt prints on every real add.
        if (nowS - P.lastAttachS >= 1.0) {
            P.lastAttachS = nowS;
            if (a->AddAnimationGraphEventSink(&g_getupSink))
                logger::info("PUSHLIFT {:08X} GETUP-PROBE sink ATTACHED — her animation events now reach PPB "
                             "(positive control: a knockdown must now print ANIM lines)", tg[i].id);
        }

        // 3) the knock-state edge, every frame (main-thread resolution: one frame, ~11 ms at 90 Hz)
        const int  knock = a->AsActorState() ? static_cast<int>(a->AsActorState()->GetKnockState()) : -1;
        const bool rag   = a->IsInRagdollState();
        if (P.prevKnock < 0) {
            P.prevKnock = knock;
            P.prevRag   = rag;
        } else if (knock != P.prevKnock || rag != P.prevRag) {
            if (P.prevKnock == 0 && knock != 0) {
                P.leftNormalS = nowS;
                P.seq[0] = 0;
                GetUpSeqAppend(P.seq, sizeof(P.seq), 0);
            }
            if (knock != P.prevKnock) GetUpSeqAppend(P.seq, sizeof(P.seq), knock);
            logger::info("PUSHLIFT {:08X} KNOCK {} -> {}  rag {} -> {} | episode {} | {:.0f} ms since she left kNormal",
                         tg[i].id, P.prevKnock, knock, P.prevRag ? 1 : 0, rag ? 1 : 0, P.seq,
                         P.leftNormalS > 0.0 ? (nowS - P.leftNormalS) * 1000.0 : 0.0);
            if (P.prevKnock != 0 && knock == 0) {
                P.backNormalS = nowS;
                logger::info("PUSHLIFT {:08X} RECOVERED — knock back to 0 after {:.2f}s down (episode {}); the lift gate "
                             "can evaluate her again from THIS frame{}",
                             tg[i].id, P.leftNormalS > 0.0 ? (nowS - P.leftNormalS) : 0.0, P.seq,
                             rag ? " — WARNING: IsInRagdollState still reads 1, so the gate stays shut" : "");
            }
            P.prevKnock = knock;
            P.prevRag   = rag;
        }

        // 4) SKIPPED — a player is touching her and the lift gate cannot evaluate her. This is the line the 12:40
        //    session needed: there, silence meant "she was already down", not "the gate said no".
        const bool touched = (nowS - tg[i].lastTouch) <= grace;
        if (touched && (rag || knock != 0) && (nowS - P.lastSkipLogS) >= 1.0) {
            P.lastSkipLogS = nowS;
            logger::info("PUSHLIFT {:08X} SKIPPED — the lift gate cannot evaluate her: {} (knock={} rag={}) | player "
                         "contact {:.2f}s ago",
                         tg[i].id, rag ? "she is RAGDOLLED" : "her knock state is not 0 (down or getting up)",
                         knock, rag ? 1 : 0, nowS - tg[i].lastTouch);
        }
    }

    // 5) drain the animation events. Copy out under our own mutex, then log with no lock held.
    GetUpEvt      buf[kGetUpRing];
    int           bn = 0;
    std::uint32_t dropped = 0;
    {
        std::scoped_lock lk(g_getupMx);
        bn = g_getupN;
        for (int i = 0; i < bn; ++i) buf[i] = g_getupRing[i];
        g_getupN = 0;
        dropped = g_getupDropped;
        g_getupDropped = 0;
    }
    for (int i = 0; i < bn; ++i) {
        const GetUpEvt& e = buf[i];
        double      rel = 0.0;
        const char* ref = "no knockdown seen on her yet";
        if (auto it = g_getupProbe.find(e.id); it != g_getupProbe.end()) {
            const GetUpProbe& P = it->second;
            if (P.prevKnock != 0 && P.leftNormalS > 0.0) {
                rel = (e.t - P.leftNormalS) * 1000.0;
                ref = "ms into the knockdown (still down)";
            } else if (P.backNormalS > 0.0) {
                rel = (e.t - P.backNormalS) * 1000.0;
                ref = "ms vs the knock->0 edge (negative = BEFORE she recovered)";
            }
        }
        logger::info("PUSHLIFT {:08X} ANIM '{}' knock={} | {:+.0f} {}", e.id, e.tag, e.knock, rel, ref);
    }
    if (dropped)
        logger::info("PUSHLIFT GETUP-PROBE ring overflow — {} animation event(s) dropped this frame", dropped);
}

}   // namespace

namespace PushStep {

void OnFrame()
{
    // ★ RAGFRAME (2026-09-03, read-only) — ABOVE the pushStep early-return ON PURPOSE. The
    // receipt keeps the last ~10 frames of its subject so that when a knockdown fires, the
    // window BEFORE it survives: that is where H2 (contact velocity transfer) and the
    // hit-as-trigger case both live, and neither can be reconstructed afterwards. If this sat
    // below the return, arming `ragFrame 1` with `pushStep 0` — the obvious way to isolate a
    // knockdown from PPB's own walk — would produce total silence and read as "the instrument
    // is broken". Silence is only ever an answer from an instrument whose positive control
    // fired. It picks its own subject when push is off (see RagFrame::OnFrame).
    RagFrame::OnFrame();

    {   // knob receipt, once per state change
        static int s_last = -1;
        const int en = ObjectHold::PushStepEnabled() ? 1 : 0;
        if (en != s_last) {
            s_last = en;
            logger::info("PUSHSTEP knob -> {} (pushStep {})", en ? "ARMED" : "off", en);
        }
        if (!en) return;
    }
    if (HandBox::IsSceneSuspended()) return;

    const double nowS    = NowS();
    const bool   travelModeScan = ObjectHold::PushStepTravelMode() != 0.f;   // v13d
    const float  nearU   = ObjectHold::PushStepNearU();    // v7.1: the push's OWN contact distance

    GetUpProbeTick(nowS);   // v27 (2026-09-10) DIAGNOSTIC ONLY: get-up events + knock edges + SKIPPED receipt

    // One scan, all actors: newest trunk pressure per actor this frame.
    PPBAPI::PpbTouchContact c[32];
    const int n = PpbApi::CopyContacts(c, 32);
    const bool weaponOk = ObjectHold::PushStepWeapon();

    // ★★ v28 THE LIFT ZONE (2026-09-10) — which contacts may ATTRIBUTE a feet lift. Decided HERE, outside
    // g_walkMx (capsule reads are engine reads). The push below reads none of it: every contact still arms
    // the push exactly as before. Calf, foot and pelvis (COM) are lift zone; the thigh is split at
    // pushStepLiftThighSplit along hip->knee; arms, head and trunk are push only. pushStepLiftZoneRule 0 =
    // v11 ("any contact").
    bool        liftZone[32];
    float       liftFrac[32];
    signed char liftHow[32];
    {
        const bool  zoneRule = ObjectHold::PushStepLiftZoneRule();
        const float split    = ObjectHold::PushStepLiftThighSplit();
        PpbApi::ProbeView pvz[15];
        int npz = -1;                                         // fetched on the first thigh contact only
        for (int i = 0; i < n; ++i) {
            liftZone[i] = !zoneRule;
            liftFrac[i] = -1.f;
            liftHow[i]  = 0;
            if (!zoneRule || c[i].distU > nearU || c[i].sourceKind > PPBAPI::kSourceObject) continue;
            const int s = c[i].slot;
            if (s == 9 || s == 10 || s == 11) { liftZone[i] = true; continue; }   // calf, foot, pelvis (COM)
            if (s != 8) continue;                                                 // arms, head, trunk: push only
            auto* zf = RE::TESForm::LookupByID(c[i].actorFormId);
            auto* za = zf ? zf->As<RE::Actor>() : nullptr;
            if (!za) continue;
            if (npz < 0) npz = PpbApi::CopyProbes(pvz, 15);
            float fr = -1.f;
            const int how = ThighContactFrac(za, c[i], pvz, npz, &fr);
            liftHow[i]  = (signed char)how;
            liftFrac[i] = fr;
            // ★ v29: a ring child by IDENTITY; the rod by the probe's position (no probe evidence = PUSH).
            liftZone[i] = (c[i].child > 0) ? ThighChildLifts(c[i].child) : (how == 1 && fr >= split);
            // receipt, 1 Hz per actor: the split is a new rule and it is tested by feel
            static std::unordered_map<std::uint32_t, double> s_zoneLogS;
            if (s_zoneLogS.size() > 64) s_zoneLogS.clear();
            double& zLast = s_zoneLogS[c[i].actorFormId];
            if (nowS - zLast >= 1.0) {
                zLast = nowS;
                static const char* const kHow[4] = { "thigh did not read", "probe point", "capsule centre", "rod centre" };
                logger::info("PUSHLIFT {:08X} zone — '{}' (thigh C{}{}, src {}) sits {:.2f} hip->knee via {} | decided by {} "
                             "→ {}", c[i].actorFormId, c[i].bodyPart, c[i].child, c[i].leftTwin ? " L" : " R",
                             (int)c[i].sourceKind, fr, kHow[how & 3],
                             c[i].child > 0 ? "capsule identity (pushStepLiftThighMask)" : "rod position vs pushStepLiftThighSplit",
                             liftZone[i] ? "LIFT zone (may attribute a feet lift)" : "PUSH zone (no lift attribution)");
            }
        }
    }

    // ★★ v29 THE COLLISION CAPSULE ATTRIBUTES THE LIFT (user 2026-09-10: "Use the collision capsule, we own them").
    // The 21:16 session: after a get-up Havok reported the sword on her calf for 1.5 s (21:31:21-23) while the 4 Hz geometric
    // digest published nothing - the lift was deaf. The engine stamp names the exact capsule the hand, palm or weapon touched
    // THIS frame and attributes exactly like a digest contact (same zone rule). The digest stays the second source (held
    // objects; a resting hand Havok has stopped re-reporting). Outside g_walkMx for the rod placement (capsule reads); the
    // stamps go in under the lock. Runs BEFORE the digest stamp so the collision capsule names the attribution this frame.
    {
        PpbApi::EngineTouch etl[16];
        int  ntl = 0;
        bool etlZone[16] = {};
        if (ObjectHold::PushStepLiftEngineAttr()) {
            ntl = PpbApi::EngineTouchList(etl, 16);
            PpbApi::ProbeView pve[15];
            int npe = -1;
            for (int i = 0; i < ntl; ++i) {
                if (nowS - etl[i].tS > 0.05) continue;                                // this frame's stamp only
                const int s = etl[i].slot;
                if (s == 9 || s == 10 || s == 11) { etlZone[i] = true; continue; }    // calf, foot, pelvis (COM)
                if (s != 8) continue;                                                  // arms, head, trunk: push only
                if (etl[i].child > 0) { etlZone[i] = ThighChildLifts(etl[i].child); continue; }
                auto* ef = RE::TESForm::LookupByID(etl[i].actorId);
                auto* ea = ef ? ef->As<RE::Actor>() : nullptr;
                if (!ea) continue;
                if (npe < 0) npe = PpbApi::CopyProbes(pve, 15);
                PPBAPI::PpbTouchContact ct{};
                ct.actorFormId = etl[i].actorId;
                ct.slot        = 8;
                ct.child       = 0;
                ct.leftTwin    = etl[i].left ? 1 : 0;
                ct.wand        = (unsigned char)(etl[i].hand ? 1 : 0);
                ct.sourceKind  = (unsigned char)(etl[i].src == 1 ? PPBAPI::kSourceWeapon
                                               : etl[i].src == 2 ? PPBAPI::kSourceFinger : PPBAPI::kSourcePalm);
                float fr = -1.f;
                const int how = ThighContactFrac(ea, ct, pve, npe, &fr);
                etlZone[i] = how == 1 && fr >= ObjectHold::PushStepLiftThighSplit();
            }
        }
        if (ntl > 0) {
            std::scoped_lock lk(g_walkMx);
            for (int i = 0; i < ntl; ++i) {
                if (nowS - etl[i].tS > 0.05) continue;
                if (g_lift.size() > 64) g_lift.clear();
                auto& Le = g_lift[etl[i].actorId];
                Le.lastAnyTouchS = nowS;                     // any engine contact also freezes the rest latch
                if (!etlZone[i]) continue;
                Le.lastTouchS = nowS;
                const int es = etl[i].slot;
                if (es == 8 || es == 9 || es == 10) Le.legTouchS[etl[i].left ? 1 : 0] = nowS;
                Le.attWand   = etl[i].hand ? 1 : 0;
                Le.attSlot   = es;
                Le.attChild  = etl[i].child;
                Le.attLeft   = etl[i].left;
                Le.attSrc    = etl[i].src;
                Le.attHow    = 0;
                Le.attFrac   = -1.f;
                Le.attDistU  = -1.0e9f;                      // the collision capsule names this frame's attribution
                Le.attEngine = 1;
                std::snprintf(Le.attPart, sizeof Le.attPart, "%s on slot %d C%d%s",
                              etl[i].src == 1 ? "weapon" : etl[i].src == 2 ? "finger box" : "palm", es, etl[i].child,
                              etl[i].left ? " L" : "");
            }
        }
    }

    // ── PUSHWALK (2026-08-29): the pressure set for the 0x5E0885 hook ────────────────────
    // The hook does the actual driving (PLANCK's ritual); OnFrame only maintains membership +
    // direction. Direction: away from the player, horizontal, refreshed while pressed.
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        std::scoped_lock lk(g_walkMx);
        if (g_walk.size() > 64) g_walk.clear();
        for (auto& [wid, ww] : g_walk) {                        // v8.4/8.5: fresh each frame
            ww.pressDistU = 99.f;
            ww.contactsDbg[0] = 0;
            ww.contactsN = 0;
            ww.pressHand = false;
            ww.pressObj  = false;   // v11: re-decided every frame, like pressDistU
            ww.pressWand[0] = ww.pressWand[1] = false;   // v11.1b
        }
        for (int i = 0; i < n; ++i) {
            const bool hand2   = c[i].sourceKind <= 3;
            const bool weapon2 = weaponOk && c[i].sourceKind == PPBAPI::kSourceWeapon;
            // ★ v11 rule 7 ("any hand box, weapon or object held by the player"), behind its own master,
            // which SHIPS 0 — see Tuning.h: an armed object push walks her away during a 1 s equip dwell.
            const bool object2 = ObjectHold::PushStepObjectPush() != 0.f &&
                                 c[i].sourceKind == PPBAPI::kSourceObject;
            // ⛔ THE PLAYER'S HEAD IS NOT A PUSH SOURCE (user ruling 2026-09-03, reversing the
            // same-day "head pressure" addition). "The only reason I'm adding the head as a
            // collision capsule is to prevent the player's head going INSIDE the NPC." The head
            // box therefore stays a physical keyframed collider (HandBox.cpp) and a touch SOURCE
            // for the API (kSourceHead, so VRTE can narrate a face-to-body contact) — but it
            // never stamps lastPressureS, so it can neither engage a push-walk nor attribute a
            // fall. Only hands and a drawn weapon do that.
            // v10.3b LIFT ATTRIBUTION (user spec, verbatim: "if I take a weapon and lift both an NPC's feet
            // with that weapon, that NPC should fall. Period."): ANY player contact — hand classes, GRAB,
            // WEAPON, held OBJECT — on her legs OR trunk, stamped BEFORE the push-pressure filter below
            // (GRAB is class 4 and weapons need weaponOk there; a lift is neither). The five false lifts
            // of 22:37 were walk-onset transients, and those are gated by the not-while-walking rule in
            // the sensor itself — not by which slot was touched. (v10.3 briefly excluded WEAPON/OBJECT
            // by writing `<= kSourceGrab`; the sword sweep on her calves at 22:44:57 fell through it.)
            // ★ v11 rule 5 (user, verbatim): "Just track the feet lift whenever there is a contact with
            // the player. Any contact." v10.3b still excluded the head (3) and neck (7); the slot list is
            // gone entirely. The false-lift defence is the not-while-walking gate in the sensor itself,
            // never which slot was touched.
            if (c[i].distU <= nearU && c[i].sourceKind <= PPBAPI::kSourceObject) {
                if (g_lift.size() > 64) g_lift.clear();
                auto& Lz = g_lift[c[i].actorFormId];
                Lz.lastAnyTouchS = nowS;                    // v28: the rest latch still freezes on ANY contact
                if (liftZone[i]) {                          // v28: only a LIFT-ZONE contact attributes a lift
                    const int zs = c[i].slot;               // v29: remember which LEG was touched (the one-leg rule)
                    if (zs == 8 || zs == 9 || zs == 10) Lz.legTouchS[c[i].leftTwin ? 1 : 0] = nowS;
                    if (Lz.lastTouchS < nowS || c[i].distU < Lz.attDistU) {   // the nearest one this frame
                        Lz.attEngine = 0;
                        Lz.attWand  = c[i].wand ? 1 : 0;
                        Lz.attSlot  = c[i].slot;
                        Lz.attChild = c[i].child;
                        Lz.attLeft  = c[i].leftTwin != 0;
                        Lz.attSrc   = (int)c[i].sourceKind;
                        Lz.attHow   = liftHow[i];
                        Lz.attFrac  = liftFrac[i];
                        Lz.attDistU = c[i].distU;
                        std::memcpy(Lz.attPart, c[i].bodyPart, sizeof Lz.attPart);
                        Lz.attPart[sizeof Lz.attPart - 1] = 0;
                    }
                    Lz.lastTouchS = nowS;
                }
            }
            if (!hand2 && !weapon2 && !object2) continue;
            if (c[i].distU > nearU) continue;
            // ⛔ ARMS DO NOT PUSH (user ruling, 2026-08-30, killing the 1-hour-old v8.2
            // conduit): "if i push an arm, the arm's gonna move away, not the body." Arms
            // yield; they never transmit. LEGS (calf/foot) wait for upper-body confirmation;
            // thigh slot 8 stays as the measured HIP-press landing zone, not a leg wire.
            // v8.3: HEAD (3) and NECK (7) join - "if the head gets pushed back, the NPC needs
            // to move back too."
            const bool trunk2 = c[i].slot == 3 || c[i].slot == 4 || c[i].slot == 5 ||
                                c[i].slot == 6 || c[i].slot == 7 || c[i].slot == 8 ||
                                c[i].slot == 11;
            // ★★ v13d: the 2026-08-30 "ARMS DO NOT PUSH" ruling above was written for the sensor that measured
            // the TOUCHED bone — an arm touch measured the arm, and an arm yields. v13 measures her trunk JOINTS
            // no matter where the hand is, so pushing through her arm is a real push exactly when her trunk
            // actually moves, and nothing at all when it does not. The filter is obsolete in travel mode.
            if (!trunk2 && !travelModeScan) continue;
            // ★ v12 (user): "belly and breast got FSMP physic, they MUST be excluded."
            // ⚠ v13 NOTE: this now SHIPS OFF (pushStepSoftExclude 0). The reason it existed was that the
            // touched capsule decided which bone was measured, so a soft contact produced a soft number.
            // Under v13 the contact decides nothing — it only says "the player is pushing" — and the number
            // comes from the six trunk JOINTS, which are rigid Havok/XP32 bones no FSMP bone can move. Set it
            // back to 1 to stop a belly/breast contact ARMING a push at all.
            if (ObjectHold::PushStepSoftExclude() != 0.f &&
                (c[i].subRegion == (unsigned char)PPBAPI::kSubBreast ||
                 c[i].subRegion == (unsigned char)PPBAPI::kSubBelly)) continue;
            auto& w = g_walk[c[i].actorFormId];
            const double prevPressureS = w.lastPressureS;
            w.lastPressureS = nowS;
            if (object2) w.pressObj = true; else w.pressHand = true;    // v11 rule 7
            { const int hw = c[i].wand ? 1 : 0; w.pressWand[hw] = true; w.lastPressWandS[hw] = nowS;   // v11.1b per-hand clocks
              w.pressSlot[hw] = c[i].slot; w.pressChild[hw] = c[i].child; w.pressLeft[hw] = c[i].leftTwin != 0; }   // R6
            // ★ v12: the contact IS the trigger — capture the origin the first frame we see it (idempotent
            // while the contact holds; a lapse of pushStepOriginHoldS starts a new push).
            if (ObjectHold::PushStepTravelMode() != 0.f)
                CaptureTravelOriginLocked(c[i].actorFormId, nowS, false, "hand contact");
            // ★ v11 HAND TRAVEL: a rigid point on the pushing hand (the 3-finger slab centre, else the index tip).
            // A contact that lapsed for > 0.5 s starts a new episode and re-anchors. Plain distance, on purpose —
            // "how far has my hand moved since it touched her" is the user's own definition.
            {
                float H[3];
                const int hand = c[i].wand ? 1 : 0;
                if (HandBox::BoxCenterWorldU(hand, 2, H) || HandBox::TipWorldU(hand, 1, H)) {
                    if (!w.handP0Valid || (nowS - prevPressureS) > 0.5) {
                        w.handP0[0] = H[0]; w.handP0[1] = H[1]; w.handP0[2] = H[2];
                        w.handP0Valid = true; w.handTravelU = 0.f; w.handP0S = nowS;
                    } else {
                        const float hx = H[0] - w.handP0[0], hy = H[1] - w.handP0[1], hz = H[2] - w.handP0[2];
                        w.handTravelU = std::sqrt(hx * hx + hy * hy + hz * hz);
                    }
                }
            }
            // v9.4: a LEFT-thigh contact is reported as slot 8 with leftTwin - route it to the
            // left thigh's own sensed bone (108) or we measure the wrong leg entirely.
            w.slot = (c[i].slot == 8 && c[i].leftTwin) ? 108 : c[i].slot;
            // ★ v12c: LATCH the measured bone to the first qualifying contact after the capture. Without this the
            // reading follows the hand as it slides across her capsules, and every bone carries a different
            // accumulated travel — the 19:19:05 engage jumped 1.65u (s8) -> 12.84u (s4) on a slot flip alone.
            if (ObjectHold::PushStepTravelMode() != 0.f) {
                auto& tv = g_travel[c[i].actorFormId];
                if (tv.lockSlot < 0) {
                    tv.lockSlot = w.slot;   // receipt only — the JOINTS decide, not this
                    logger::info("PUSHTRAVEL {:08X} contact on s{} — the push is measured on her trunk JOINTS, "
                                 "wherever the hand is", c[i].actorFormId, w.slot);
                }
            }
            if (c[i].distU < w.pressDistU) w.pressDistU = c[i].distU;   // v8.4 press gate feed
            g_lastTouchedActor = c[i].actorFormId;                      // v8.6 idle probe subject
            // v8.0/8.1: a hold pressing ONLY soft jiggle surfaces (breast, belly, glute) gets
            // a raised trigger - their springs lever 1-2u of real body motion out of a mere
            // touch (breast 13:22; belly/butt walked her SIDEWAYS 15:40).
            // v8.5: a non-soft contact must itself be PRESSING to unlock the sensitive bar -
            // a breast press GRAZING the adjacent chest ring at 1u must not defeat the soft bar.
            const bool softSub = c[i].subRegion == (unsigned char)PPBAPI::kSubBreast ||
                                 c[i].subRegion == (unsigned char)PPBAPI::kSubBelly ||
                                 c[i].subRegion == (unsigned char)PPBAPI::kSubGlute;
            if (!softSub && c[i].distU <= ObjectHold::PushStepPressU()) w.sawHard = true;
            // v8.5 forensics: bank up to 4 qualifying contacts for the engage receipts.
            if (w.contactsN < 4) {
                const std::size_t len = std::strlen(w.contactsDbg);
                std::snprintf(w.contactsDbg + len, sizeof w.contactsDbg - len,
                              "%ss%d/%s@%.2f", w.contactsN ? " " : "",
                              c[i].slot, c[i].bodyPart[0] ? c[i].bodyPart : "?", c[i].distU);
                ++w.contactsN;
            }
            // v4: the away-from-player steer is GONE - direction comes only from the measured
            // displacement at engage. A touch that displaces nothing walks nothing.
            (void)player;
        }
    }

    // ── ★ v11.1 ENGINE ANCHOR (2026-09-07, user design — report 33 §8.3) ─────────────────────
    // "When my hand touch her havok capsule, ANY capsule, the engine react … That's our trigger. Then start
    // tracking movement distance." The stamps PpbApi resolved THIS frame plant / refresh the anchor; every
    // valid anchor then re-measures the hand's travel from the LIVE hand box, every frame. The 3 u gate in
    // the hook reads this when pushStepAnchorSrc is 1 and falls back to the API-snapshot anchor otherwise.
    if (ObjectHold::PushStepAnchorSrc() != 0.f) {
        PpbApi::EngineTouch et[16];
        const int nt = PpbApi::EngineTouchList(et, 16);
        const bool trunkOnly = ObjectHold::PushStepAnchorTrunkOnly() != 0.f;
        std::scoped_lock lk(g_walkMx);
        if (g_engAnchor.size() > 64) g_engAnchor.clear();
        // 1) LAPSE first, per hand: an anchor whose engine clock AND whose hand's API pressure are both quiet for
        //    kAnchorLapseS is over — INVALIDATE it (v11.1b; the review's major: a lapsed anchor that could not read
        //    its hand box kept the previous episode's travel and refreshed itself forever). Entries nobody has
        //    touched for 2 s are erased.
        for (auto it = g_engAnchor.begin(); it != g_engAnchor.end();) {
            double pressS[2] = { -1e9, -1e9 };
            if (auto w2 = g_walk.find(it->first); w2 != g_walk.end()) { pressS[0] = w2->second.lastPressWandS[0]; pressS[1] = w2->second.lastPressWandS[1]; }
            bool anyRecent = false;
            for (int hnd = 0; hnd < 2; ++hnd) {
                EngAnchor& a = it->second.h[hnd];
                if (a.valid && (nowS - a.lastEngS) > kAnchorLapseS && (nowS - pressS[hnd]) > kAnchorLapseS) {
                    a.valid = false; a.travelU = 0.f;
                }
                if ((nowS - a.lastEngS) <= 2.0 || (nowS - pressS[hnd]) <= 2.0) anyRecent = true;
            }
            if (!anyRecent) { it = g_engAnchor.erase(it); continue; }
            ++it;
        }
        // 2) this frame's stamps plant (only an UNSET hand) or refresh, per hand
        for (int i = 0; i < nt; ++i) {
            if (nowS - et[i].tS > 0.05) continue;                  // not this frame's stamp
            if (et[i].hand < 0 || et[i].hand > 1) continue;
            const int sslot = (et[i].slot == 8 && et[i].left) ? 108 : et[i].slot;
            const bool trunk = sslot == 3 || sslot == 4 || sslot == 5 || sslot == 6 || sslot == 7 ||
                               sslot == 8 || sslot == 108 || sslot == 11;
            if (trunkOnly && !trunk) continue;
            EngAnchor& a = g_engAnchor[et[i].actorId].h[et[i].hand];
            if (!a.valid) {
                float H[3];
                if (HandBox::BoxCenterWorldU(et[i].hand, 2, H) || HandBox::TipWorldU(et[i].hand, 1, H)) {
                    a.p0[0] = H[0]; a.p0[1] = H[1]; a.p0[2] = H[2];
                    a.travelU = 0.f; a.valid = true; a.firstS = nowS;
                    logger::info("PUSHWALK {:08X} engine contact — hand {} ({}) on slot {} child {}: hand-travel clock STARTED here",
                                 et[i].actorId, et[i].hand ? "L" : "R",
                                 et[i].src == 1 ? "weapon" : et[i].src == 2 ? "finger box" : "HIGGS hand",
                                 sslot, et[i].child);
                }
                // no hand box readable this frame: genuinely unset (fail closed) — the API anchor stands in
            }
            a.lastEngS = nowS; a.slot = sslot; a.src = et[i].src;
        }
        // ★★ v13c THE CLOCK AND THE EVALUATION MUST START TOGETHER (measured 14:09:57.594 -> 14:09:58.624).
        // The origin was captured on the engine's contact, but PushStep can only evaluate an actor that has a
        // WalkState — and that is created by the pressure scan, which reads the touch API's DIGEST snapshot at
        // apiHz 4. So the number accumulated for 1.02 s with nothing looking at it, and the first evaluation saw
        // spine1 already at 13.7u: an instant stumble with no walk ever attempted. The engine stamp now creates
        // and refreshes the WalkState itself, so the frame the clock starts is the frame the evaluation starts.
        if (ObjectHold::PushStepTravelMode() != 0.f) {
            const bool trunkOnlyE = ObjectHold::PushStepAnchorTrunkOnly() != 0.f;
            for (int i = 0; i < nt; ++i) {
                if (nowS - et[i].tS > 0.05) continue;
                const int sslotE = (et[i].slot == 8 && et[i].left) ? 108 : et[i].slot;
                const bool trunkE = sslotE == 3 || sslotE == 4 || sslotE == 5 || sslotE == 6 ||
                                    sslotE == 7 || sslotE == 8 || sslotE == 108 || sslotE == 11;
                // ★★ v13d (user, verbatim): "It doesn't matter if i touch her arms or anywhere else, the
                // displacement is calculated AT THE NODE. If i push thru the arms, the fact my hand push 6u
                // further is not calculated, simply that contact is there." So ANY capsule arms the push — the
                // JOINTS decide whether anything happened. v13c had it backwards: it refused an arm contact,
                // which is the touched-bone thinking this sensor replaced.
                if (trunkOnlyE && !trunkE) continue;    // knob, ships 0 = any capsule
                CaptureTravelOriginLocked(et[i].actorId, nowS, false, "engine contact");
                if (g_walk.size() > 64) g_walk.clear();
                auto& we = g_walk[et[i].actorId];
                we.lastPressureS = nowS;                // the actor is TRACKED from this frame on
                we.slot          = sslotE;
                { const int hwE = et[i].hand ? 1 : 0; we.pressWand[hwE] = true; we.lastPressWandS[hwE] = nowS;
                  we.pressSlot[hwE] = et[i].slot; we.pressChild[hwE] = et[i].child; we.pressLeft[hwE] = et[i].left; }   // R6
                auto& tvE = g_travel[et[i].actorId];
                if (tvE.lockSlot < 0) {
                    tvE.lockSlot = sslotE;   // receipt only — the JOINTS decide, not this
                    logger::info("PUSHTRAVEL {:08X} contact on s{} (the capsule the ENGINE saw the hand hit) — the push "
                                 "is now measured on her trunk JOINTS, wherever the hand is",
                                 et[i].actorId, sslotE);
                }
            }
        }
        // 3) per-frame travel from the LIVE hand for every valid anchor
        for (auto& kv : g_engAnchor)
            for (int hnd = 0; hnd < 2; ++hnd) {
                EngAnchor& a = kv.second.h[hnd];
                if (!a.valid) continue;
                float H[3];
                if (HandBox::BoxCenterWorldU(hnd, 2, H) || HandBox::TipWorldU(hnd, 1, H)) {
                    const float hx = H[0] - a.p0[0], hy = H[1] - a.p0[1], hz = H[2] - a.p0[2];
                    a.travelU = std::sqrt(hx * hx + hy * hy + hz * hz);
                }
            }
    }

    // ── GRAB-FOLLOW TELEMETRY (v7.2, 2026-08-30) — the user pulls a HIGGS-held actor and asks
    // "how much movement did that create?" PLANCK's drag machinery owns the movement; we just
    // MEASURE it: displacement at 1 Hz while held, total on release. The numbers are the
    // reference PushWalk's feel gets cloned against.
    {
        struct GrabTrack { RE::NiPoint3 start, last; double lastLogS; };
        static std::unordered_map<std::uint32_t, GrabTrack> s_grabTrack;
        PpbApi::ProbeView pv[15];      // 15: the head probe is appended last (CopyProbes)
        const int np = PpbApi::CopyProbes(pv, 15);
        std::uint32_t held[2] = { 0, 0 };
        int nHeld = 0;
        for (int i = 0; i < np && nHeld < 2; ++i)
            if (pv[i].grabActorId) {
                bool dup = false;
                for (int k = 0; k < nHeld; ++k) dup |= (held[k] == pv[i].grabActorId);
                if (!dup) held[nHeld++] = pv[i].grabActorId;
            }
        for (int k = 0; k < nHeld; ++k) {
            auto* f = RE::TESForm::LookupByID(held[k]);
            auto* a = f ? f->As<RE::Actor>() : nullptr;
            if (!a) continue;
            const auto pos = a->GetPosition();
            auto it = s_grabTrack.find(held[k]);
            if (it == s_grabTrack.end()) {
                s_grabTrack[held[k]] = { pos, pos, nowS };
                logger::info("GRABFOLLOW {:08X} HIGGS hold began — tracking her movement", held[k]);
            } else if (nowS - it->second.lastLogS >= 1.0) {
                const float ddx = pos.x - it->second.last.x, ddy = pos.y - it->second.last.y;
                logger::info("GRABFOLLOW {:08X} moved {:.1f}u this second (dir {:+.0f}° world)",
                             held[k], std::sqrt(ddx * ddx + ddy * ddy),
                             std::atan2(ddx, ddy) * 57.2958f);
                it->second.last = pos; it->second.lastLogS = nowS;
            }
        }
        for (auto it = s_grabTrack.begin(); it != s_grabTrack.end();) {
            bool still = false;
            for (int k = 0; k < nHeld; ++k) still |= (it->first == held[k]);
            if (still) { ++it; continue; }
            float tx = 0.f, ty = 0.f;
            if (auto* f = RE::TESForm::LookupByID(it->first))
                if (auto* a = f->As<RE::Actor>()) {
                    tx = a->GetPosition().x - it->second.start.x;
                    ty = a->GetPosition().y - it->second.start.y;
                }
            logger::info("GRABFOLLOW {:08X} hold ended — total displacement {:.1f}u",
                         it->first, std::sqrt(tx * tx + ty * ty));
            it = s_grabTrack.erase(it);
        }
    }

    // ★ v9.7 KNOCKDOWN SETTLE — cap her body speed for a moment after a push knocks her down.
    // Without it the solver resolves the deep overlap with the player's blade/hand by ejecting
    // her ("straight to the ceiling with a sword"); with it, she collapses.
    if (!g_ragSettle.empty()) {
        std::vector<std::uint32_t> done;
        {
            std::scoped_lock lk(g_walkMx);
            for (auto& [rid, until] : g_ragSettle) if (nowS >= until) done.push_back(rid);
        }
        std::vector<std::pair<std::uint32_t, double>> active;
        {
            std::scoped_lock lk(g_walkMx);
            for (auto d : done) g_ragSettle.erase(d);
            for (auto& kv : g_ragSettle) active.push_back(kv);
        }
        for (auto& [rid, until] : active) {
            auto* f = RE::TESForm::LookupByID(rid);
            auto* a = f ? f->As<RE::Actor>() : nullptr;
            if (!a) continue;
            const int n2 = GrabDiag::ClampRagdollSpeed(a, ObjectHold::PushStepRagMaxVelU());
            // ⚠ UNTHROTTLED into the receipt (the 0.5 s log throttle below is for the LOG, not
            // the measurement). This clamp has never been observed running in VR — its
            // per-frame count inside the window is the only receipt that it did act at all.
            RagFrame::NoteSettle(rid, n2);
            if (n2 > 0) {
                static double s_lastClampLog = 0.0;
                if (nowS - s_lastClampLog > 0.5) {
                    s_lastClampLog = nowS;
                    logger::info("PUSHREACT {:08X} settle: capped {} body/bodies over {:.0f} u/s "
                                 "(knockdown launch guard)", rid, n2,
                                 ObjectHold::PushStepRagMaxVelU());
                }
            }
        }
    }

    RagFrame::NoteSubject(g_lastTouchedActor);   // the freshest subject when push IS armed

    // ★ v8.6 IDLE JITTER PROBE (diagnostic, knob-gated, read-only). The eye cannot tell a
    // trembling BODY from a trembling REFERENCE — both read as deviation, and deviation is
    // what the trigger consumes. At 1 Hz this prints all seven deviations for the last-touched
    // actor EVEN WITH NO HAND ON HER: steady near 0 = healthy; oscillating = the deviation
    // itself is unstable (the real bug); one bone large+steady = that bone's latch/anatomy.
    if (ObjectHold::PushStepIdleProbe() != 0.f && g_lastTouchedActor) {
        // v8.7: sample EVERY FRAME, report PEAK-TO-PEAK per bone. A shiver is high-frequency
        // (user: "a 1 per second take won't show nothing") - the OSCILLATION AMPLITUDE is the
        // measurement, not a snapshot of it.
        static constexpr int kS[7] = { 4, 5, 6, 11, 8, 7, 3 };
        static float  s_mn[7] = {}, s_mx[7] = {};
        static bool   s_have[7] = {};
        static int    s_n = 0;
        static double s_lastIdle = 0.0;
        for (int k = 0; k < 7; ++k) {
            float dx, dy, m;
            if (!ArmIK::GetPushSenseGap(g_lastTouchedActor, kS[k], 0.f, dx, dy, m)) continue;
            if (!s_have[k]) { s_mn[k] = m; s_mx[k] = m; s_have[k] = true; }
            else { if (m < s_mn[k]) s_mn[k] = m; if (m > s_mx[k]) s_mx[k] = m; }
        }
        ++s_n;
        if (nowS - s_lastIdle > 1.0) {
            s_lastIdle = nowS;
            char buf[224]; buf[0] = 0;
            for (int k = 0; k < 7; ++k) {
                const std::size_t len = std::strlen(buf);
                if (s_have[k])
                    std::snprintf(buf + len, sizeof buf - len, "%ss%d:%.2f-%.2f(p2p %.2f)",
                                  k ? " " : "", kS[k], s_mn[k], s_mx[k], s_mx[k] - s_mn[k]);
                else
                    std::snprintf(buf + len, sizeof buf - len, "%ss%d:-", k ? " " : "", kS[k]);
                s_have[k] = false;
            }
            logger::info("PUSHSENSE IDLE {:08X} {} frames [{}] (s11=COM s4=spine0 s5=spine1 "
                         "s6=spine2 s8=thigh s7=neck s3=head)", g_lastTouchedActor, s_n, buf);
            s_n = 0;
        }
    }
}


// ═══════════════════════════════════════════════════════════════════════════════════════
//  OnMotionDrivenCheck — called from the 0x5E0885 chain hook (Hooks.cpp) for EVERY actor the
//  engine re-evaluates. Returns true when WE drove this actor (the chained original — and
//  PLANCK's hook — must then be SKIPPED, exactly as PLANCK skips the original for its grabbed
//  actors: the original would clear planner-direct-control every frame).
// ═══════════════════════════════════════════════════════════════════════════════════════
// ★ v11 (2026-09-07): the two readers the per-frame tier check uses.
// ReadChainGap — the LARGEST published gap over the touched bone and the whole trunk chain, with its
// direction and slot. A bend concentrates up the chain: the 12:09:02 chest push read 54 u at the chest
// and 91 u at the head. The touched bone is consulted first so 8/108 (the thighs) only ever enter
// through the touch; the trunk is always consulted.
static bool ReadChainGap(std::uint32_t id, int touched, float& mag, float& dx, float& dy, int& slot)
{
    // ★ v11 13:03 session (user: "she ragdoll almost immediately ... it seem to prime now"): a WAIST push read
    // 14.93 u at the waist (a stumble by rule 3) but the CHEST above it swung to 20.77 u and the chain-max
    // knocked her down on a bone the hand was not on. Rule 4 says the ladder reads "the Havok joint that
    // activated the push" — the touched bone. Default is the touched bone; pushStepTierChain 1 = the
    // largest over the trunk (kept for A/B — the 91 u head reading on a 54 u chest push was exciting, not needed).
    static constexpr int kTrunk[6] = { 11, 4, 5, 6, 7, 3 };
    mag = 0.f; dx = 0.f; dy = 0.f; slot = -1;
    auto consider = [&](int s) {
        float x = 0.f, y = 0.f, m = 0.f;
        if (ArmIK::GetPushSenseGap(id, s, 0.01f, x, y, m) && m > mag) { mag = m; dx = x; dy = y; slot = s; }
    };
    if (touched >= 0) consider(touched);
    if (ObjectHold::PushStepTierChain() != 0.f)
        for (int s : kTrunk) if (s != touched) consider(s);
    return slot >= 0;
}
// ★ v11 15:45 (user): the bars are PER CHAIN GROUP, because the same push moves a bone further the higher up
// the chain it sits — measured 12:09:02, one chest push: waist 14.93 u, chest 20.77 u, head 54.74 u. One flat
// bar therefore means "the trunk stumbles late and the head stumbles instantly". Three groups:
//   LOW  = COM 11, Spine0 4, thighs 8/108  — the base knobs (BarU / StaggerU / RagdollU)
//   MID  = Spine1 5, Spine2 6              — ...MidU
//   HIGH = Neck 7, Head 3                  — ...HighU
// A slot we do not sense falls through to LOW.
// v11 16:20: ONE set of bars for every bone. The three-group split was derived from a gradient measured on
// an actor who was 4 s into a STAGGER and going down (14:12:19) — bad evidence, and the user never asked for
// it. `pushStepTierChain 1` still exists if the chain maximum is ever wanted. The Mid/High knobs stay parsed
// but unread, so an existing tuning file keeps loading.
static void BarsForSlot(int, float& engageU, float& stagU, float& ragU)
{
    engageU = ObjectHold::PushStepBarU();
    stagU   = ObjectHold::PushStepStaggerU();
    ragU    = ObjectHold::PushStepRagdollU();
}
// LatKFor — the v9.9 lateral factor, one definition (it was written out three times).
static float LatKFor(RE::Actor* actor, float dx, float dy, float mag)
{
    if (mag <= 0.01f || ObjectHold::PushStepLatTrig() == 0.f || ObjectHold::PushStepSideGain() <= 1.f) return 1.f;
    const float gh = GetHeadingFromVector(RE::NiPoint3{ dx / mag, dy / mag, 0.f });
    return 1.f + (ObjectHold::PushStepSideGain() - 1.f) * std::fabs(std::sin(gh - actor->data.angle.z));
}

// ★ v9.8 THE ESCALATION FIX (user, 2026-09-06; the gap was user-found 09-03, report 28 §3).
// The v9.5 stagger/ragdoll tiers used to live only inside the ENTER block, so once a gentle
// push had engaged a walk they were never looked at again until the whole episode ended: a
// push that BUILDS could never promote, only the first engage's magnitude decided. This is the
// one evaluator both sites call — the enter block (as before) and, with pushStepEscalate on,
// the DRIVING branch every frame with the LIVE gap. Returns 0 none / 1 stagger / 2 ragdoll.
// On a fire it does everything the reaction needs: cooldown stamps, the RAGFRAME arm, the
// queued reaction, and — because a walk and an animation must never overlap — tears the walk
// down (planner released, package re-evaluated, WalkState erased for a ragdoll / parked for a
// stagger) so the animation owns her. Callers return immediately after a fire.
// v11: stagU/ragU are passed in — they are per chain group now (BarsForSlot), not globals.
static int FireReactionTier(RE::Actor* actor, std::uint32_t id, const WalkState& w, void* mc,
                            float stagU, float ragU,
                            float mmag, float mdx, float mdy, float rate, bool usedPeak,
                            float latK, bool v5, double nowS, const char* where)
{
    if (ObjectHold::PushStepReaction() == 0.f || actor->IsInRagdollState() || mmag <= 0.01f) return 0;
    if (PushReactionsBlocked(actor)) return 0;   // v24: fighting, or mid-killmove — not ours to interrupt
    // ★★ 2.2.0 THE INSTALL CHOICES (user, 2026-09-12): stumble and shove-knockdown are separate options. A shove past the
    // knockdown bar with the knockdown OFF but the stumble ON plays a STUMBLE (she was still shoved hard); with both OFF
    // nothing fires here (the walk, if chosen, is decided elsewhere and is untouched).
    const bool ragAllowed = Ini::FeatureShoveRagdoll();
    const bool stgAllowed = Ini::FeaturePushStumble();
    if (!ragAllowed && !stgAllowed) return 0;
    // the cooldown is the LATER of the in-state stamp and the outliving side map (the
    // duplicate-fire fix) — a fresh WalkState must not read as "never reacted".
    double lastReact = w.lastReactS;
    {
        std::scoped_lock lk(g_walkMx);
        if (auto it = g_reactCoolS.find(id); it != g_reactCoolS.end() && it->second > lastReact)
            lastReact = it->second;
    }
    // ★★ v19: a HIGHER tier may pre-empt the cooldown. The reading climbs continuously, so she crosses
    // the stumble bar on the way to the knockdown bar every time; without this the stagger fires at the
    // lower bar, stamps the cooldown, and the knockdown she goes on to earn can never land. Escalation
    // is one-way — ragdoll after a stagger is allowed, ragdoll after a ragdoll is not.
    const bool wouldRag = ragAllowed && (mmag >= ragU);   // 2.2.0: a knockdown that is switched off can never escalate
    double lastRag = -1e9;
    {
        std::scoped_lock lk(g_walkMx);
        if (auto it = g_reactRagS.find(id); it != g_reactRagS.end()) lastRag = it->second;
    }
    const bool escalate = wouldRag
                       && ObjectHold::PushStepRagEscalate() != 0.f
                       && (nowS - lastRag)   >  (double)ObjectHold::PushStepReactCoolS()
                       && (nowS - lastReact) >= (double)ObjectHold::PushStepRagEscalateMinS();
    if (!escalate && nowS - lastReact <= (double)ObjectHold::PushStepReactCoolS()) return 0;
    // v9.9: `rate` is the WINDOWED rate (a latched peak brings its own); a peak no longer bypasses
    // the speed test — the log staggered her at 8.57u / +0.8 u/s [PEAK], a firm slow lean. Big AND
    // fast is a shove; big and slow is a lean and stays a walk. The stagger bar sees the depth
    // through the lateral factor (pushStepLatTrig: a side shove bends her less for the same effort);
    // the knockdown bar stays literal — 20u is 20u whichever way she is thrown.
    // ★ v11 rule 3 (user 2026-09-06 23:30): "at 10 u it's a shove, so the NPC should do a stumble.
    // 10 u means she bends backward, that's unstable" — 10 u is a shove even when SLOW, so the rate no
    // longer gates the stagger; it feeds the walk RAMP instead (rule 2). pushStepStaggerRate is KEPT and
    // now ships at 0 = gate off; set it back to 10 in the tuning file to restore the v9.9 speed test HOT.
    const float stagRate = ObjectHold::PushStepStaggerRate();
    const bool  fast  = (stagRate <= 0.f) || (rate >= stagRate);
    const float smag  = mmag * (std::max)(1.f, latK);
    const bool  doRag = ragAllowed && mmag >= ragU;                       // 2.2.0: bShoveRagdoll
    const bool  doStg = stgAllowed && !doRag && smag >= stagU && fast;    // 2.2.0: bPushStumble (also catches a
                                                                          // knockdown-sized shove when ragdoll is off)
    if (!doRag && !doStg) return 0;

    float relD = (GetHeadingFromVector(RE::NiPoint3{ mdx / mmag, mdy / mmag, 0.f })
                  - actor->data.angle.z) * 57.2958f;
    while (relD > 180.f)  relD -= 360.f;
    while (relD < -180.f) relD += 360.f;
    float mag = (smag - stagU) / (std::max)(1.f, ragU - stagU);   // 0..1 across the band
    { const float mmin = ObjectHold::PushStepStaggerMagMin(); if (mag < mmin) mag = mmin; }   // v10.1 knob (was the 0.25 literal; below ~0.25 the vanilla anim barely reads)
    if (mag > 1.f)   mag = 1.f;
    {
        std::scoped_lock lk(g_walkMx);
        if (auto it = g_walk.find(id); it != g_walk.end()) it->second.lastReactS = nowS;
        // ⛔ THE DUPLICATE-FIRE FIX (Ragdoll Research 05 §1/§5): the cooldown must outlive the
        // WalkState the ragdoll branch erases, so it gets its own map.
        if (g_reactCoolS.size() > 64) g_reactCoolS.clear();
        g_reactCoolS[id] = nowS;
        if (doRag) {                                   // v19: knockdown-to-knockdown stays fully gated
            if (g_reactRagS.size() > 64) g_reactRagS.clear();
            g_reactRagS[id] = nowS;
        }
    }
    // F0: arm the receipt BEFORE the event so the ring flush lands ahead of the graph notify.
    RagFrame::Arm(id, mmag, w.slot, usedPeak ? "peak" : (v5 ? "raw" : "legacy"), relD, doRag);
    // v10.3: the facing reads the PUSH's acting direction (user ruling: "where the push is acting on the
    // NPC, not where the player is"). At engage that is this frame's HemiDot-validated retreat; during a
    // walk it is the walk's own retreat direction — the live gap's direction mid-walk is a lag artefact
    // (the 22:13 "+7° chest push"). pushStepStaggerFaceSrc 1 reads the player's position instead (v10.2).
    const float faceHeading = (w.driving && (w.dirX * w.dirX + w.dirY * w.dirY) > 0.01f)
                            ? GetHeadingFromVector(RE::NiPoint3{ w.dirX, w.dirY, 0.f })
                            : GetHeadingFromVector(RE::NiPoint3{ mdx / mmag, mdy / mmag, 0.f });
    QueueReaction(id, relD, mag, doRag, doRag ? "dropped" : "shove", PusherOf(w), faceHeading);
    // ★ v11.1 (user ruling 2026-09-07 evening): the tiers stay on HER displacement; the hand's travel is PRINTED
    // on every fire so the next session can judge from data whether they should ever wait for it.
    float tierHandU = -1.f; const char* tierAnchor = "none";
    {
        std::scoped_lock lk(g_walkMx);
        if (auto ea = g_engAnchor.find(id); ea != g_engAnchor.end())
            for (int hnd = 0; hnd < 2; ++hnd)
                if (ea->second.h[hnd].valid && ea->second.h[hnd].travelU > tierHandU) { tierHandU = ea->second.h[hnd].travelU; tierAnchor = hnd ? "engine L" : "engine R"; }
        if (tierHandU < 0.f)
            if (auto it = g_walk.find(id); it != g_walk.end() && it->second.handP0Valid) { tierHandU = it->second.handTravelU; tierAnchor = "api"; }
    }
    logger::info("PUSHREACT {:08X} {} {} — displaced {:.2f}u (×lat {:.2f} = {:.2f}u) rate(win) {:+.1f}u/s{} bearing {:+.0f}° "
                 "(stagger>={:.1f}u & rate>={:.0f}, ragdoll>={:.1f}u raw) | hand travel {:.2f}u ({} anchor; tiers do not wait for it)",
                 id, doRag ? "RAGDOLL" : "STAGGER", where, mmag, latK, smag, rate, usedPeak ? " [PEAK]" : "",
                 relD, stagU, ObjectHold::PushStepStaggerRate(), ragU, tierHandU, tierAnchor);

    const bool walking = mc && PlannerActive(mc) && w.driving;
    if (walking) {
        // ⛔ A REACTION ENDS THE WALK — they are mutually exclusive (2026-08-30, the warp-into-
        // walls bug: the planner kept commanding 90 u/s on a limp body for two seconds).
        PlannerCtl(mc)->ClearPlannerDirectControl();
        if (auto* task = SKSE::GetTaskInterface())   // deferred — inline recursion froze the game
            task->AddTask([id]() {
                if (auto* f = RE::TESForm::LookupByID(id))
                    if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
            });
    }
    std::scoped_lock lk(g_walkMx);
    if (doRag) {
        if (g_ragSettle.size() > 64) g_ragSettle.clear();
        g_ragSettle[id] = nowS + (double)ObjectHold::PushStepRagSettleS();
        g_walk.erase(id);          // full stand-down; a fresh touch re-measures
        g_lastDriveS[id] = nowS;   // and the settle gate holds for her get-up
        logger::info("PUSHWALK {:08X} knocked down{} — walk torn down, planner released", id,
                     walking ? " mid-walk" : "");
        return 2;
    }
    // stagger: the animation owns her for the cooldown; the WalkState stays tracked (parked) so
    // a push that keeps building can promote to a knockdown on the next evaluation.
    if (auto it = g_walk.find(id); it != g_walk.end()) {
        it->second.driving         = false;
        it->second.phase           = 0;
        it->second.capped          = false;
        it->second.reengageAfterS  = nowS + (double)ObjectHold::PushStepReactCoolS();
        it->second.lastEpisodeEndS = nowS;
    }
    g_lastDriveS[id] = nowS;
    if (walking) logger::info("PUSHWALK {:08X} staggered mid-walk — walk ended, the animation owns her", id);
    return 1;
}

// ★ v29d EQUIP SETTLE — stamped by DeviceGesture for every equip and every removal PPB performs (PushStep.h).
// It only records the moment; the lift block re-bases the floor when the window closes, and TravelMax /
// PushReactionsBlocked hold the walk and the tiers off until then.
void NoteEquipEvent(std::uint32_t actorFormId)
{
    if (!actorFormId) return;
    const double nowS = NowS();
    std::scoped_lock lk(g_equipMx);
    if (g_equipSettle.size() > 64) g_equipSettle.clear();
    g_equipSettle[actorFormId] = EquipSettle{ nowS, false };
}

// v11.1b (review): FormID-keyed engine anchors must not cross a load. Called from main.cpp's kPreLoadGame block.
void ClearOnLoad()
{
    {
        std::scoped_lock lk(g_walkMx);
        g_engAnchor.clear();
        g_travel.clear();   // v12
        g_lift.clear();     // v29: FormIDs recycle across a load (floor / trip / leg-touch state is per actor)
    }
    { std::scoped_lock lk(g_equipMx); g_equipSettle.clear(); }   // v29d
    GetUpProbeClear();   // v27: main-thread probe state + the animation-event ring (FormIDs recycle across a load)
}

bool IsDrivingActor(std::uint32_t actorFormId)
{
    std::scoped_lock lk(g_walkMx);
    auto it = g_walk.find(actorFormId);
    return it != g_walk.end() && it->second.driving;
}

bool OnMotionDrivenCheck(RE::Actor* actor)
{
    if (!actor) return false;
    // ★ v11.1: the player is never a push subject — but the lift block below was running its rest latch on him
    // (16:45:18 `PUSHLIFT 00000014 rest LATCHED`). Nothing here has ever applied to 0x14; skip outright.
    if (actor == static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton())) return false;
    const std::uint32_t id = actor->GetFormID();

    WalkState w{};
    bool tracked = false;
    {
        std::scoped_lock lk(g_walkMx);
        auto it = g_walk.find(id);
        if (it != g_walk.end()) { w = it->second; tracked = true; }
    }
    const double nowS  = NowS();
    const double stopS = ObjectHold::PushStepStopS();
    void* mc = McOf(actor);

    // ★★ 2026-09-11 THE FEATURE MASTER — and it must UNDO, not merely STOP (Part 08 failure
    // class 3, "stopping is not undoing"). OnFrame's gate stops NEW pushes, but THIS entry is
    // called from the 0x5E0885 chain hook for every actor every frame and never consulted the
    // master. An actor already walking under planner direct control when the switch went off
    // would keep it forever — OnFrame, the only thing that releases it, has already returned.
    // Release once, drop her state, then stand down and let the vanilla chain run (returning
    // false means the original runs, and the original clears planner control every frame).
    if (!ObjectHold::PushStepEnabled()) {
        if (tracked) {
            if (mc && PlannerActive(mc) && w.driving) {
                PlannerCtl(mc)->ClearPlannerDirectControl();
                // ⛔ EvaluatePackage stays DEFERRED here for the same reason as the grab handoff
                // below: inline from inside the movement-evaluation hook recurses the evaluator.
                if (auto* task = SKSE::GetTaskInterface())
                    task->AddTask([id]() {
                        if (auto* f = RE::TESForm::LookupByID(id))
                            if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
                    });
                logger::info("PUSHWALK {:08X} RELEASED — push/shove feature is OFF "
                             "(planner cleared, walk state dropped)", id);
            }
            std::scoped_lock lk(g_walkMx);
            g_walk.erase(id);
        }
        return false;
    }

    const bool travelMode = ObjectHold::PushStepTravelMode() != 0.f;   // ★ v12
    // ★ v12a HER OWN GAIT IS NOT A PUSH (2026-09-07 19:06 belly push). While she moves under her own power her
    // trunk bones travel metres in her own frame — the log measured 17.29 u of "push" on the waist over 1.73 s
    // of Sofia walking, and `animDriven=1` refused the walk anyway, so the stumble was the only thing that could
    // fire. Same rule the user gave for the feet lift: ask the ACTOR. While she self-moves the origin is
    // re-captured every frame (so the reading follows her gait and stays ~0) and nothing may fire; the instant
    // she stops, the origin is fresh and the next push measures true.
    // ⚠⚠ v12b (the 19:06 log, read carefully): `[gait W1 R0 S0 M0]` on a STANDING Sofia. `actorState1.walking` is
    // the GAIT STANCE (walk-vs-run), not motion — it reads 1 on almost every standing NPC. Only Actor::IsMoving()
    // (VR 0x6116C0) answers "is she actually moving". Gating on the stance flags would have suppressed the push on
    // nearly everyone, and it is ALSO why her feet-lift rest latch never converged this session (`rest R 0.0 L 0.0
    // (heel fallback)`): v11.1b's lift gate read W1 and never sampled. Both gates use IsMoving; all four flags are
    // still printed, because the receipt is how this was caught.
    bool gaitW = false, gaitR = false, gaitS = false, gaitM = false;
    if (auto* st = actor->AsActorState()) { gaitW = st->IsWalking(); gaitR = st->IsRunning(); gaitS = st->IsSprinting(); }
    gaitM = actor->IsMoving();
    // *** v16 THE WALK WINDOW. While she is DRIVING under our own walk, re-stamp the travel origin on a
    // cadence. Without this the origin is the single capture-at-engage and her gait + servo lag climb the
    // bars on their own (measured: head 20-26u, 0.30-0.41 s after engage, three walks in a row). With it,
    // the number always answers "how far has the player pushed her SINCE I last looked", so a stumble
    // during a walk means the push out-ran her retreat - which is what a stumble should mean.
    // ⚠ Silent by design (why = nullptr): the v13a self-move re-capture printed 1751 lines in one session
    // and buried every receipt that mattered. CaptureTravelOriginLocked self-throttles because it stamps
    // capturedS, so the elapsed test below is the whole cadence.
    // ⛔ driving is read from the MAP, never from the stale WalkState copy (report 33 s9.3).
    if (travelMode && ObjectHold::PushStepWalkReCapS() > 0.f) {
        std::scoped_lock lk(g_walkMx);
        auto iw = g_walk.find(id);
        if (iw != g_walk.end() && iw->second.driving) {
            if (auto it = g_travel.find(id); it != g_travel.end() &&
                (nowS - it->second.capturedS) >= (double)ObjectHold::PushStepWalkReCapS())
                CaptureTravelOriginLocked(id, nowS, true, nullptr);
        }
    }
    const bool selfMovingRaw = gaitM || gaitS;      // sprinting implies moving; the stance flags do not
    bool selfMoving = false;
    if (travelMode && ObjectHold::PushStepSelfMoveGate() != 0.f) {
        selfMoving = selfMovingRaw;
        if (selfMoving) {
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_travel.find(id); it != g_travel.end()) {
                // v13a: SILENT re-capture — this runs every frame she moves and printed 1751 lines in one
                // session, burying the receipts that matter. The 1 Hz line below is the visible one.
                CaptureTravelOriginLocked(id, nowS, true, nullptr);
                if (nowS - it->second.lastLogS > 1.0) {
                    it->second.lastLogS = nowS;
                    logger::info("PUSHTRAVEL {:08X} she is WALKING on her own — the push cannot be measured through her "
                                 "gait (her trunk travels ~10-17u per stride); origin re-captured, no walk, no tier",
                                 id);
                }
            }
        }
    }

    // ══ v10.3 LIFT → RAGDOLL — ABOVE the untracked return and the HIGGS-grab return ═══════════════════
    // 22:37 session: v10.2's sensor sat below both, so a real lift (a GRAB — class 4, never tracked, and
    // the tick returns while HIGGS holds her) could never reach it, while five FALSE lifts fired from a
    // palm on her waist/chest 0.2–0.3 s after a walk engaged: at walk onset the ragdoll feet read 13–21 u
    // above her origin (the same transient that corrupts the gap sensor). Rules now: attribution = a hand
    // under her LEGS (8/9/10) or a GRAB anywhere (stamped in the pressure scan before its hand filter);
    // NEVER while she is walking or within pushStepLiftSettleS of a walk ending; both FOOT bodies above
    // pushStepLiftFeetU for pushStepFallFrames frames. Speaks at 1 Hz while a lift is being watched.
    // v11: GetKnockState covers the Ragdoll->GetUp window where IsInRagdollState already reads FALSE — three of
    // the five LIFTED of 12:27 were her legs swinging through the get-up 1.5 s after a real knockdown.
    if (ObjectHold::PushStepLiftRagOn() && !actor->IsInRagdollState() &&
        actor->AsActorState()->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal) {
        double lastTouch = 0.0, lastAnyTouch = 0.0;
        {
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_lift.find(id); it != g_lift.end()) {
                lastTouch    = it->second.lastTouchS;       // v28: a LIFT-ZONE contact
                lastAnyTouch = it->second.lastAnyTouchS;    // v28: any contact
            }
        }
        const bool attributed = (nowS - lastTouch) <= (double)ObjectHold::PushStepFallGraceS();
        // v28: the rest latch still freezes on ANY player contact ("a latch must never absorb a push"); only
        // the lift VERDICT narrowed to the lift zone. With pushStepLiftZoneRule 0 the two are the same stamp.
        const bool touchedAny = (nowS - lastAnyTouch) <= (double)ObjectHold::PushStepFallGraceS();
        // ★ v11.1 (2026-09-07, report 33 §8.4): ask the ACTOR too. `tracked && w.driving` only knows PPB's own walk;
        // 16:45:48 Sofia was walking under her own AI, the gate stood open, and a normal gait cleared the 1.5 u rest
        // bar from a thigh brush. Actor::IsMoving (VR 0x6116C0 — PLANCK's own binding) + the gait flags are the
        // engine's answer; the same test keeps her own gait out of the REST LATCH below (her latch read a 6 u spread).
        // v12b: the flags are read once at the top of the tick now (see the stance-vs-motion note there); the lift
        // gate consumes the same IsMoving answer instead of the walk/run STANCE, which is set on standing NPCs.
        const bool liftSelfMoving = (ObjectHold::PushStepLiftGait() != 0.f) && selfMovingRaw;
        const bool walking    = liftSelfMoving ||
                                (tracked && (w.driving || (nowS - w.lastEpisodeEndS) < (double)ObjectHold::PushStepLiftSettleS()));
        // ★ v29 THE SEAT GUARD: sitting, sleeping, furniture, swimming or a killmove pose her legs on purpose - never a lift.
        bool seated = false;
        if (ObjectHold::PushStepLiftSitGuard()) {
            bool swimming = false;
            if (auto* stS = actor->AsActorState()) swimming = stS->IsSwimming();
            // ★★ 2026-09-12 THE SIX-STATE RULE + LEAN (ArmIK::PushFurnitureVerdict, PPBHook.h). This used to read
            // "sit state != Normal OR an occupied-furniture handle": the handle is a RESERVATION, and on 2026-09-12 it
            // held every sweep in `floor WAITING SEATED` while she WALKED to a chair (19:11:52, 251u from the seat, sit
            // Normal) and for 70 s after she STOOD UP (19:12:13, a handle to another chair 932u away). Each seated frame
            // below also wipes the rise floor (`epRise +0.0 ... FLAT` at feet +44u) and re-arms the floor wait. Now only
            // her body's state counts; a lean stays liftable. pushStepFurnSitState 0 = the old rule, exactly. Cache read
            // only — this runs inside the 0x5E0885 movement hook, where no 3D may be walked.
            seated = ArmIK::PushFurnitureVerdict(actor) == ArmIK::kFurnBusy || swimming || actor->IsInKillMove();
        }
        // ★★ v29 THE TRIP BEATS THE WALK (user 2026-09-10: "keep the walk, but ragdoll condition trump walk ... if she walk and
        // her feet end up lifting higher than the intended walk location, than ragdoll, cause she got tripped"). While a PPB
        // walk runs (or settles), or she moves on her own, each foot is judged against where the ANIMATION puts it.
        const bool tripMode = ObjectHold::PushStepLiftWalkVerdict() && (walking || selfMovingRaw);
        float ag[2] = { 0.f, 0.f };
        const bool agOk[2] = { ArmIK::GetFootAnimGap(id, 0, ag[0]), ArmIK::GetFootAnimGap(id, 1, ag[1]) };

        // Both FOOT bodies: the BOTTOM of the sole capsule, relative to her origin.
        float fa[3], fb[3], fr = 0.f; float lo[2] = { 1e9f, 1e9f }; bool okF = true;
        for (int side = 0; side < 2 && okF; ++side) {
            if (GrabDiag::ReadCapsuleWorldUSide(actor, 10, side == 1, 0, fa, fb, &fr))
                lo[side] = (std::min)(fa[2], fb[2]) - fr;
            else okF = false;
        }
        const float floorZ = actor->GetPosition().z;
        const float h0 = lo[0] - floorZ, h1 = lo[1] - floorZ;

        // Her heel lift — the same node and the same band check HEELFIX uses (PPBHook.cpp). It is the
        // rest FALLBACK before the latch converges, and a change in it re-arms the latch (new shoes).
        float heelU = 0.f;
        if (auto* root = actor->Get3D())
            if (auto* npcNode = root->GetObjectByName("NPC")) {
                const float hz = npcNode->local.translate.z;
                if (hz > 0.01f && hz <= 40.f) heelU = hz;
            }

        // ★★ v29d HER SHOES ARE NOT A PUSH — and this check may NOT live inside the latch block below.
        // It used to (v26), but that block is frozen while ANY contact is live, so on 2026-09-11 the user
        // hand-equipped heeled boots WITH A HAND ON HER THIGH: the heel change was never seen, the barefoot rest
        // survived, and the boots' ~8 u lift on BOTH feet read as a lift (20:46:00.461, feet +8.5 / +8.1 → LIFTED).
        // Contact freezes a MEASUREMENT; it must never freeze the invalidation of a reference that just became wrong.
        {
            std::scoped_lock lk(g_walkMx);
            if (g_lift.size() > 64) g_lift.clear();
            auto& Lh = g_lift[id];
            if (Lh.restOk && std::fabs(heelU - Lh.restHeelU) > 0.25f) {
                logger::info("PUSHLIFT {:08X} heel changed {:.1f}u -> {:.1f}u — rest latch DROPPED (her shoes moved "
                             "her feet, nobody pushed her); the heel height is the rest until it re-latches",
                             id, Lh.restHeelU, heelU);
                Lh.restOk = false; Lh.restN = 0; Lh.restHeelU = heelU;   // restHeelU now, so this speaks once
                Lh.epOk = false; Lh.minOk = false; Lh.frames = 0; Lh.a0Ok[0] = Lh.a0Ok[1] = false;
            }
        }
        // ★★ v29d THE EQUIP SETTLE, second half (the user's rule): while the window is open PushReactionsBlocked
        // holds every verdict off; the moment it closes, THIS height is the new floor — for the feet and, via
        // CaptureTravelOriginLocked, for every sensed trunk node, so the equip cannot read as a push either.
        {
            const float wS = ObjectHold::PushStepEquipSettleS();
            bool doRebase = false;
            if (wS > 0.f) {
                std::scoped_lock lk(g_equipMx);
                auto it = g_equipSettle.find(id);
                if (it != g_equipSettle.end() && !it->second.rebased && (nowS - it->second.stampS) >= (double)wS) {
                    it->second.rebased = true; doRebase = true;
                }
            }
            if (doRebase && okF) {
                std::scoped_lock lk(g_walkMx);
                if (g_lift.size() > 64) g_lift.clear();
                auto& Le = g_lift[id];
                Le.restZ[0] = h0; Le.restZ[1] = h1; Le.restHeelU = heelU; Le.restOk = true;
                Le.restOriginZ = floorZ; Le.restN = 0;
                Le.epOk = false; Le.minOk = false; Le.frames = 0; Le.a0Ok[0] = Le.a0Ok[1] = false;
                Le.needGround = false; Le.groundSinceS = 0.0;
                CaptureTravelOriginLocked(id, nowS, true, "equip settled");
                logger::info("PUSHLIFT {:08X} EQUIP SETTLED — new floor R {:.2f}u L {:.2f}u (heelZ {:.1f}u); every sensed "
                             "node re-based {:.2f}s after the equip. Lift / stumble / ragdoll / walk were held off until now",
                             id, h0, h1, heelU, (double)wS);
            }
        }

        // ── ★ THE REST LATCH — samples ONLY while UNTOUCHED and not walking ───────────────
        // "A latch must never absorb a push": attribution IS the freeze here, exactly as the push rest
        // latch freezes on a player probe within 20 u.
        if (okF && !touchedAny && !walking && h0 > -20.f && h1 > -20.f) {   // v28: ANY contact freezes it, as before
            std::scoped_lock lk(g_walkMx);
            if (g_lift.size() > 64) g_lift.clear();
            auto& L = g_lift[id];   // v29d: the heel check moved ABOVE, out of this contact-frozen block
            if (!L.restOk && nowS - L.restLastS >= 0.25) {                 // 4 Hz
                L.restLastS = nowS;
                L.restBuf[0][L.restN] = h0;
                L.restBuf[1][L.restN] = h1;
                if (++L.restN >= LiftState::kRestN) {
                    float med[2] = { 0.f, 0.f }, spread[2] = { 0.f, 0.f };
                    for (int s = 0; s < 2; ++s) {
                        float t[LiftState::kRestN];
                        for (int i = 0; i < LiftState::kRestN; ++i) t[i] = L.restBuf[s][i];
                        for (int i = 1; i < LiftState::kRestN; ++i) {       // insertion sort: no <algorithm> in this TU
                            const float key = t[i]; int j = i - 1;
                            while (j >= 0 && t[j] > key) { t[j + 1] = t[j]; --j; }
                            t[j + 1] = key;
                        }
                        med[s]    = t[LiftState::kRestN / 2];
                        spread[s] = t[LiftState::kRestN - 1] - t[0];
                    }
                    // SANITY only, not precision: a huge spread means she ragdolled or fell mid-window (the
                    // 09-07 log has a -34 u sample), so throw it away and re-sample. The true UNTOUCHED
                    // spread is unknown — no log has ever contained untouched foot data, because the watch
                    // receipt is gated on a recent touch. This receipt is how we finally learn it.
                    const float worst = spread[0] > spread[1] ? spread[0] : spread[1];
                    if (worst < ObjectHold::PushStepLiftRestSpreadU()) {   // v23: was a hard-coded 8
                        L.restZ[0] = med[0]; L.restZ[1] = med[1]; L.restHeelU = heelU; L.restOk = true;
                        L.restOriginZ = floorZ;                      // v26: the reference frame itself
                        logger::info("PUSHLIFT {:08X} rest LATCHED — R {:.2f}u L {:.2f}u above her origin "
                                     "(spread R {:.2f} L {:.2f} over {} samples, heelZ {:.1f}u) — bar is rest + {:.2f}u",
                                     id, med[0], med[1], spread[0], spread[1], (int)LiftState::kRestN, heelU,
                                     ObjectHold::PushStepLiftRestU());
                    } else {
                        logger::info("PUSHLIFT {:08X} rest REJECTED — spread R {:.2f}u L {:.2f}u over {} samples "
                                     "(she moved or fell mid-window); re-sampling", id, spread[0], spread[1],
                                     (int)LiftState::kRestN);
                    }
                    L.restN = 0;
                }
            }
        }

        // ★★ v29 THE FLOOR (user 2026-09-10: "can we do a floor level check ... make sure the getup animation is made from the
        // floor level"). After a knockdown or a seat the lift re-arms only once BOTH feet are back at floor level (inside
        // ±pushStepLiftGroundU of her rest, char controller supported) for pushStepLiftGroundS - the get-up's own leg motion
        // (+3 to +7 u within 30 ms of RECOVERED, measured 18:31) and its pop can no longer read as a lift.
        bool grounded = true;
        {
            const float gU = ObjectHold::PushStepLiftGroundU();
            bool ccSupported = true;
            if (auto* ccG = actor->GetCharController()) ccSupported = (int)ccG->surfaceInfo.supportedState.underlying() == 2;
            std::scoped_lock lk(g_walkMx);
            if (auto itg = g_lift.find(id); itg != g_lift.end()) {
                auto& Lg = itg->second;
                if (seated) {
                    Lg.frames = 0; Lg.epOk = false; Lg.minOk = false; Lg.a0Ok[0] = Lg.a0Ok[1] = false;
                    Lg.needSinceS = nowS;                                    // v29e: the deadline cannot run while she is seated
                    if (gU > 0.f) { Lg.needGround = true; Lg.groundSinceS = 0.0; }
                }
                if (Lg.needGround && gU <= 0.f) Lg.needGround = false;
                // ★★ v29e THE DEADLINE. The floor wait covers the GET-UP's own leg motion, nothing more — so it may not
                // outlive the get-up. If the player keeps her feet in the air the band is never satisfied and the lift
                // goes deaf for as long as he keeps lifting (measured 21:42:50: +36.3 / +32.3 u, 35 qualifying frames,
                // `floor WAITING`, ignored). Once she has been back on her feet this long, arm regardless.
                if (Lg.needGround) {
                    const float maxS = ObjectHold::PushStepLiftGroundMaxS();
                    if (maxS > 0.f && Lg.needSinceS > 0.0 && (nowS - Lg.needSinceS) > (double)maxS) {
                        Lg.needGround = false;
                        Lg.frames = 0; Lg.epOk = false; Lg.minOk = false; Lg.a0Ok[0] = Lg.a0Ok[1] = false;
                        logger::info("PUSHLIFT {:08X} FLOOR WAIT EXPIRED — {:.2f}s back on her feet and they never returned "
                                     "to the band (feet R {:+.1f} L {:+.1f} vs rest); the get-up is long over, so the lift arms "
                                     "anyway (pushStepLiftGroundMaxS {:.2f}s)",
                                     id, nowS - Lg.needSinceS, h0 - (Lg.restOk ? Lg.restZ[0] : heelU),
                                     h1 - (Lg.restOk ? Lg.restZ[1] : heelU), (double)maxS);
                    }
                }
                if (Lg.needGround) {
                    const float r0 = Lg.restOk ? Lg.restZ[0] : heelU;
                    const float r1 = Lg.restOk ? Lg.restZ[1] : heelU;
                    const bool atFloor = okF && !seated && ccSupported &&
                                         (Lg.restOk ? (std::fabs(h0 - r0) <= gU && std::fabs(h1 - r1) <= gU) : true);
                    if (!atFloor) Lg.groundSinceS = 0.0;
                    else if (Lg.groundSinceS <= 0.0) Lg.groundSinceS = nowS;
                    if (atFloor && (nowS - Lg.groundSinceS) >= (double)ObjectHold::PushStepLiftGroundS()) {
                        Lg.needGround = false;
                        Lg.frames = 0; Lg.epOk = false; Lg.minOk = false; Lg.a0Ok[0] = Lg.a0Ok[1] = false;
                        logger::info("PUSHLIFT {:08X} FLOOR — both feet back at floor level (R {:+.1f} L {:+.1f} vs rest, band {:.1f}u{}) "
                                     "for {:.2f}s; the lift can fire again", id, h0 - r0, h1 - r1, gU,
                                     Lg.restOk ? "" : ", rest not latched - char controller support only",
                                     ObjectHold::PushStepLiftGroundS());
                    }
                }
                grounded = !Lg.needGround;
            }
        }
        if (attributed) {
            bool restOk = false; float rest[2] = { 0.f, 0.f }; float latchZ = 0.f;   // v26
            {
                std::scoped_lock lk(g_walkMx);
                auto& L = g_lift[id];
                restOk = L.restOk; rest[0] = L.restZ[0]; rest[1] = L.restZ[1];
                latchZ = L.restOriginZ;                                                 // v26
            }
            if (!restOk) { rest[0] = heelU; rest[1] = heelU; }   // on a heeled NPC the heel IS the rest
            const float liftU = ObjectHold::PushStepLiftRestU();
            const float d0 = h0 - rest[0], d1 = h1 - rest[1];
            // ★ BOTH feet, and that is the real discriminator, not the bar: a walking or shifting human
            // always plants one. The 09-07 log shows one foot high and the other at its floor over and over.
            // ★★ v23 THE GATE. Height is necessary but nowhere near sufficient - the sitting false
            // positive was HIGHER than a real lift. A lift is both feet leaving together and still
            // rising; anything else is a pose.
            const float symU  = ObjectHold::PushStepLiftSymU();
            const float riseU = ObjectHold::PushStepLiftRiseU();
            const bool  riseFromContact = ObjectHold::PushStepLiftRiseFromContact();   // v28
            const bool  highEnough = okF && d0 > liftU && d1 > liftU;
            const bool  symmetric  = highEnough && std::fabs(d0 - d1) <= symU;
            bool  rising = true;
            float liftMin0 = d0, liftMin1 = d1;   // captured for the receipt
            bool  liftMinOk = false;                       // v26: is the qualifying floor set?
            float epRise0 = 0.f, epRise1 = 0.f;            // v26: rise since this contact began
            char  attPart[48] = {};                        // v28: the contact that attributed it
            float  tripRise[2] = { -1.0e9f, -1.0e9f };      // v29: foot rise above the animation since contact (-1e9 = unknown)
            double legS[2] = { 0.0, 0.0 };                   // v29: last lift-zone contact per leg
            int    attEng = 0;                               // v29: 1 = engine (collision capsule) attribution
            int   attSlot = -1, attChild = 0, attSrc = 0, attHow = 0, attWand = -1;
            bool  attLeft = false;
            float attFrac = -1.f;
            {
                std::scoped_lock lk(g_walkMx);
                auto& L = g_lift[id];
                if (!symmetric) { L.minOk = false; }             // episode broken: forget the floor
                else if (!L.minOk) { L.minOk = true; L.minD[0] = d0; L.minD[1] = d1; }
                else {
                    if (d0 < L.minD[0]) L.minD[0] = d0;
                    if (d1 < L.minD[1]) L.minD[1] = d1;
                }
                liftMin0 = L.minD[0]; liftMin1 = L.minD[1];
                liftMinOk = L.minOk;                                       // v26 (display)
                // v26 episode floor (v28: the rise gate reads it). ★ v29 THE FLOOR: the floor can never sit below floor level
                // (a stride dip or a knockdown's -8.6 u must not inflate the rise), and the trip reference a0[] is captured
                // with it - each foot's offset from the animation at the moment this lift-zone contact began.
                const float flo = -ObjectHold::PushStepLiftGroundU();
                const float cl0 = (flo < 0.f && d0 < flo) ? flo : d0;
                const float cl1 = (flo < 0.f && d1 < flo) ? flo : d1;
                if (!L.epOk) { L.epOk = true; L.epMin[0] = cl0; L.epMin[1] = cl1; L.a0Ok[0] = L.a0Ok[1] = false; }
                else { if (cl0 < L.epMin[0]) L.epMin[0] = cl0; if (cl1 < L.epMin[1]) L.epMin[1] = cl1; }
                for (int f = 0; f < 2; ++f)
                    if (!L.a0Ok[f] && agOk[f]) { L.a0[f] = ag[f]; L.a0Ok[f] = true; }
                epRise0 = d0 - L.epMin[0]; epRise1 = d1 - L.epMin[1];
                tripRise[0] = (agOk[0] && L.a0Ok[0]) ? ag[0] - L.a0[0] : -1.0e9f;
                tripRise[1] = (agOk[1] && L.a0Ok[1]) ? ag[1] - L.a0[1] : -1.0e9f;
                legS[0] = L.legTouchS[0]; legS[1] = L.legTouchS[1];
                attEng = L.attEngine;
                std::memcpy(attPart, L.attPart, sizeof attPart);                           // v28 (receipts)
                attPart[sizeof attPart - 1] = 0;
                attSlot = L.attSlot; attChild = L.attChild; attSrc = L.attSrc; attHow = L.attHow;
                attLeft = L.attLeft; attFrac = L.attFrac; attWand = L.attWand;
                // ★★ v28 THE RISE COUNTS FROM THE CONTACT. v23 counted it from minD[], whose floor is set only on
                // the first frame BOTH feet are already over the bar, so the rise stacked ON the bar (8 + 3 = 11u)
                // and 0 of 4 real sweeps fired (17:39). epMin[] is the floor since this lift-zone contact began:
                // replayed on that log it fires 3 of 4, and a frozen foot still reads 0 rise.
                rising = riseFromContact
                       ? (symmetric && epRise0 >= riseU && epRise1 >= riseU)
                       : (symmetric && L.minOk &&
                          (d0 - L.minD[0]) >= riseU && (d1 - L.minD[1]) >= riseU);
            }
            // ★★ v29 THE TWO RULES, on the reference the moment calls for.
            //   FLOOR (standing still): height = d (above her rest), rise = from the contact floor - v28, unchanged.
            //   TRIP  (walking / moving on her own): height AND rise = how far the foot sits above where the ANIMATION puts it,
            //         since the contact began - a stride moves both, so only a real trip or lift counts.
            const bool tripOk0 = tripRise[0] > -1.0e8f, tripOk1 = tripRise[1] > -1.0e8f;
            bool bothRule = symmetric && rising;                                   // floor mode (as computed above)
            if (tripMode) {
                const bool hiT = okF && tripOk0 && tripOk1 && tripRise[0] > liftU && tripRise[1] > liftU;
                const bool syT = hiT && std::fabs(tripRise[0] - tripRise[1]) <= symU;
                bothRule = syT && tripRise[0] >= riseU && tripRise[1] >= riseU;
            }
            // ★ v29b THE PELVIS BAR (user 2026-09-10 "Pelvis bar 5 u", from the 22:47 log): a 4.8 s finger touch on her pelvis
            // (clitoris / groin crease) lifted both feet +3.3 / +2.8 against the 3 u bar - 0.2 u from a false ragdoll - while the
            // real 12:09 crotch lift reached 26-34 u. A both-feet lift credited to a PELVIS capsule (slot 11) must clear
            // pushStepLiftComU on BOTH feet, in height AND in rise since the contact (floor mode: above her rest; trip mode: above
            // the animation). Leg contacts keep the normal bar; the one-leg rule below is legs-only by construction.
            if (bothRule && attSlot == 11) {
                const float comU = ObjectHold::PushStepLiftComU();
                const float hR = tripMode ? tripRise[0] : d0,      hL = tripMode ? tripRise[1] : d1;
                const float rR = tripMode ? tripRise[0] : epRise0, rL = tripMode ? tripRise[1] : epRise1;
                if (comU > 0.f && (hR < comU || hL < comU || rR < comU || rL < comU)) {
                    bothRule = false;
                    bool sayCom = false;
                    {
                        std::scoped_lock lk(g_walkMx);
                        auto& Lc = g_lift[id];
                        if (nowS - Lc.comLogS >= 1.0) { Lc.comLogS = nowS; sayCom = true; }
                    }
                    if (sayCom)
                        logger::info("PUSHLIFT {:08X} pelvis bar — feet R {:+.1f} L {:+.1f}, rise R {:+.1f} L {:+.1f} ({}) clear the "
                                     "leg bar, but a lift credited to her PELVIS needs {:.1f}u on both (pushStepLiftComU) - no lift",
                                     id, hR, hL, rR, rL, tripMode ? "above the animation" : "above her rest", comU);
                }
            }
            // ★★ v29 ONE LEG (user 2026-09-10, "Yes, ~5 u"): the foot of a leg the player is touching (a lift-zone contact on
            // THAT leg within pushStepFallGraceS) rises pushStepLiftOneLegU from the contact - the other foot may stay down.
            int oneLegSide = -1;
            const float oneU = ObjectHold::PushStepLiftOneLegU();
            if (oneU > 0.f && okF) {
                const double grc = (double)ObjectHold::PushStepFallGraceS();
                for (int f = 0; f < 2 && oneLegSide < 0; ++f) {
                    if ((nowS - legS[f]) > grc) continue;
                    const bool  okT = f ? tripOk1 : tripOk0;
                    const float hF  = tripMode ? (f ? tripRise[1] : tripRise[0]) : (f ? d1 : d0);
                    const float rF  = tripMode ? hF : (f ? epRise1 : epRise0);
                    if ((!tripMode || okT) && hF >= oneU && rF >= oneU) oneLegSide = f;
                }
            }
            const bool bothUp = bothRule || oneLegSide >= 0;
            const int  needFrames = tripMode ? (int)ObjectHold::PushStepLiftWalkFrames() : (int)ObjectHold::PushStepFallFrames();
            char tripBuf0[16], tripBuf1[16];
            if (tripOk0) std::snprintf(tripBuf0, sizeof tripBuf0, "%+.1f", tripRise[0]); else std::snprintf(tripBuf0, sizeof tripBuf0, "-");
            if (tripOk1) std::snprintf(tripBuf1, sizeof tripBuf1, "%+.1f", tripRise[1]); else std::snprintf(tripBuf1, sizeof tripBuf1, "-");
            int frames = 0; bool speak = false;
            const float logHz = ObjectHold::PushStepLiftLogHz();
            {
                std::scoped_lock lk(g_walkMx);
                auto& L = g_lift[id];
                frames = bothUp ? ++L.frames : (L.frames = 0);
                const double every = (logHz > 0.f) ? (1.0 / (double)logHz) : 0.0;
                if (nowS - L.lastLogS >= every) { L.lastLogS = nowS; speak = true; }
            }
            // ⛔ A sensor must SPEAK while it watches, not only when it fires — v10.2's lift sensor was
            // silent until it fired, so five false verdicts looked like one mystery. This line prints
            // THROUGH a walk, where the verdict is still withheld: it is the report 32 §2.3 data set.
            // v26: the three gate terms are now reported INDEPENDENTLY. Previously `symmetric`
            // was chained off `highEnough` and `risen` was printed only when symmetric, so a
            // below-the-bar frame always read "SPLIT ... FLAT" no matter what the feet did.
            const int   nHigh  = okF ? ((d0 > liftU ? 1 : 0) + (d1 > liftU ? 1 : 0)) : 0;
            const float symGap = std::fabs(d0 - d1);
            const bool  symOnly = okF && symGap <= ObjectHold::PushStepLiftSymU();  // height-independent
            const char* riseWord = rising ? "OK" : ((riseFromContact || liftMinOk) ? "FLAT" : "no-floor");   // v28
            // v28: WHICH contact attributed this lift, and which floor the rise term counted from
            char attBuf[128];
            if (attSlot == 8 && attHow > 0)
                std::snprintf(attBuf, sizeof attBuf, "'%s' (thigh C%d%s, %.2f hip->knee via %s, src %d)", attPart,
                              attChild, attLeft ? " L" : " R", attFrac,
                              attHow == 1 ? "probe" : (attHow == 2 ? "capsule centre" : "rod centre"), attSrc);
            else if (attSlot >= 0)
                std::snprintf(attBuf, sizeof attBuf, "'%s' (slot %d C%d%s, %s src %d)", attPart, attSlot, attChild,
                              attLeft ? " L" : "", attEng ? "ENGINE" : "digest", attSrc);
            else
                std::snprintf(attBuf, sizeof attBuf, "(no contact recorded)");
            const char* riseFrom = riseFromContact ? "from contact" : "from bar (v23)";
            const float riseR0 = riseFromContact ? epRise0 : (liftMinOk ? d0 - liftMin0 : 0.f);
            const float riseR1 = riseFromContact ? epRise1 : (liftMinOk ? d1 - liftMin1 : 0.f);
            if (speak)
                logger::info("PUSHLIFT {:08X} watching — feet R {:.1f} L {:.1f} above origin, rest R {:.1f} L {:.1f} ({}) "
                             "→ lift R {:+.1f} L {:+.1f} vs bar {:.2f}u | high {}/2 sym {:.1f}/{:.1f} {} risen {:+.1f}/{:+.1f} {} "
                             "| originZ {:.1f} latch {:.1f} d{:+.1f}{} | epRise R {:+.1f} L {:+.1f} "
                             "| {} frame(s) up, touch {:.2f}s ago, {} [gait W{} R{} S{} M{}]{} | rise {} | by {} "
                             "| v29 ref {} trip R {} L {} one-leg {} need {} floor {}{}",
                             id, h0, h1, rest[0], rest[1], restOk ? "latched" : "heel fallback",
                             d0, d1, liftU,
                             nHigh,
                             symGap, ObjectHold::PushStepLiftSymU(), symOnly ? "OK" : "SPLIT",
                             liftMinOk ? (d0 - liftMin0) : 0.f, liftMinOk ? (d1 - liftMin1) : 0.f,
                             riseWord,
                             floorZ, latchZ, restOk ? (floorZ - latchZ) : 0.f, restOk ? "" : " (unlatched)",
                             epRise0, epRise1,
                             frames, nowS - lastTouch,
                             walking ? (liftSelfMoving ? "MOVING on her own (no verdict)" : "WALKING (PPB walk, no verdict)") : "still",
                             gaitW ? 1 : 0, gaitR ? 1 : 0, gaitS ? 1 : 0, gaitM ? 1 : 0,
                             okF ? "" : " (a foot body did not read)", riseFrom, attBuf,
                             tripMode ? "TRIP(anim)" : "floor", tripBuf0, tripBuf1,
                             oneLegSide == 0 ? "R" : (oneLegSide == 1 ? "L" : "-"), needFrames,
                             grounded ? "ok" : "WAITING", seated ? " SEATED" : "");
            double lastR = tracked ? w.lastReactS : 0.0;
            {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_reactCoolS.find(id); it != g_reactCoolS.end() && it->second > lastR) lastR = it->second;
            }
            // The VERDICT keeps v10.3's not-while-walking gate until report 32 §2.3 is decided from the
            // data the receipt above now produces.
            // ★ v29: walking no longer withholds the verdict when the trip mode judged it (the trip beats the walk); a trip must
            // hold pushStepLiftWalkFrames (strides and servo lag are transient). Never while seated, never before her feet are
            // back on the floor after a knockdown or a seat.
            const bool walkOk    = !walking || tripMode;
            const bool liftWould = walkOk && grounded && !seated && frames >= needFrames &&
                                   nowS - lastR > (double)ObjectHold::PushStepReactCoolS() &&
                                   !PushReactionsBlocked(actor, /*feetPath*/ true);   // v24; v36: a LEAN keeps the sweep
            // ★★ v22 WATCH-ONLY (pushStepLiftRag 2): every condition is evaluated and the moment it
            // WOULD have fired is logged, but she is never knocked down. This is how the gate gets
            // designed against real negative controls instead of theory - the sensor's own comment at
            // the rest latch says it plainly: no log has ever contained untouched foot data.
            if (liftWould && !ObjectHold::PushStepLiftRagFires()) {
                { std::scoped_lock lk(g_walkMx); g_lift[id].frames = 0; }   // re-arm; report each episode once
                logger::info("PUSHLIFT {:08X} *** WOULD FIRE *** (watch-only) — {} | feet R {:+.1f}u L {:+.1f}u above her REST, "
                             "trip R {} L {} for {} frames (need {}), {} | lift-zone contact {:.2f}s ago by {}, rise {} "
                             "(R {:+.1f} L {:+.1f}). Set pushStepLiftRag 1 to arm.",
                             id, oneLegSide >= 0 ? (oneLegSide ? "ONE LEG (L)" : "ONE LEG (R)") : "BOTH FEET",
                             d0, d1, tripBuf0, tripBuf1, frames, needFrames, tripMode ? "TRIP (animation)" : "floor",
                             nowS - lastTouch, attBuf, riseFrom, riseR0, riseR1);
            }
            if (liftWould && ObjectHold::PushStepLiftRagFires()) {
                {
                    std::scoped_lock lk(g_walkMx);
                    if (g_reactCoolS.size() > 64) g_reactCoolS.clear();
                    g_reactCoolS[id] = nowS;
                    if (g_ragSettle.size() > 64) g_ragSettle.clear();
                    g_ragSettle[id] = nowS + (double)ObjectHold::PushStepRagSettleS();
                    g_lift[id].frames = 0;
                }
                logger::info("PUSHREACT {:08X} LIFTED — {} | feet R {:+.1f}u L {:+.1f}u above her REST, trip R {} L {} (above the "
                             "animation since contact) for {} frames (need {}), {} | lift-zone contact {:.2f}s ago by {}, rise {} "
                             "(R {:+.1f} L {:+.1f}); ragdolling",
                             id, oneLegSide >= 0 ? (oneLegSide ? "ONE LEG (L)" : "ONE LEG (R)") : "BOTH FEET",
                             d0, d1, tripBuf0, tripBuf1, frames, needFrames,
                             tripMode ? "judged against the ANIMATION (walking / moving)" : "judged against the floor",
                             nowS - lastTouch, attBuf, riseFrom, riseR0, riseR1);
                RagFrame::Arm(id, -1.f, tracked ? w.slot : 6, "lifted", 0.f, true);
                QueueReaction(id, 0.f, 1.f, true, "sweeped",
                              PushWho{ attWand, attSlot, attChild, attLeft });   // R6: the contact the lift was credited to
                if (tracked && mc && PlannerActive(mc) && w.driving) {     // cannot be, by the walking gate — kept for safety
                    PlannerCtl(mc)->ClearPlannerDirectControl();
                    if (auto* task = SKSE::GetTaskInterface())
                        task->AddTask([id]() {
                            if (auto* f = RE::TESForm::LookupByID(id))
                                if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
                        });
                    std::scoped_lock lk(g_walkMx);
                    g_walk.erase(id);
                }
                return false;
            }
        } else {
            bool say = false; float qr0 = 0.f, qr1 = 0.f;
            {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_lift.find(id); it != g_lift.end()) {
                    it->second.frames = 0;
                    it->second.epOk = false;                                               // v26
                    it->second.a0Ok[0] = it->second.a0Ok[1] = false;                       // v29
                    qr0 = it->second.restOk ? it->second.restZ[0] : heelU;
                    qr1 = it->second.restOk ? it->second.restZ[1] : heelU;
                    // v29: the feet stay VISIBLE while a contact is live but none is a lift zone (10 Hz) - the 21:31 session
                    // had 1.5 s of sword-on-calf with no receipt at all, so "not attributed" was indistinguishable from "idle".
                    if (touchedAny && okF && (nowS - it->second.quietLogS) >= 0.1) { it->second.quietLogS = nowS; say = true; }
                }
            }
            if (say)
                logger::info("PUSHLIFT {:08X} contact, not a lift zone — feet R {:+.1f} L {:+.1f} vs rest, foot-vs-animation raw "
                             "R {:+.1f}{} L {:+.1f}{}, {}{} | no lift-zone contact within {:.2f}s",
                             id, h0 - qr0, h1 - qr1, agOk[0] ? ag[0] : 0.f, agOk[0] ? "" : "(none)",
                             agOk[1] ? ag[1] : 0.f, agOk[1] ? "" : "(none)",
                             walking ? "WALKING" : (selfMovingRaw ? "moving" : "still"), seated ? " SEATED" : "",
                             ObjectHold::PushStepFallGraceS());
        }
    } else if (ObjectHold::PushStepLiftRagOn()) {
        // ★ v29 THE FLOOR, part 1: she is ragdolled or in a knock / get-up state - forget the episode and require her feet back
        // at floor level before the lift can fire again (the get-up animation must finish on the floor).
        std::scoped_lock lk(g_walkMx);
        if (auto it = g_lift.find(id); it != g_lift.end()) {
            auto& Lb = it->second;
            Lb.frames = 0; Lb.epOk = false; Lb.minOk = false; Lb.a0Ok[0] = Lb.a0Ok[1] = false;
            Lb.needSinceS = nowS;   // v29e: she is DOWN this frame — the floor wait's deadline starts when this stops
            if (ObjectHold::PushStepLiftGroundU() > 0.f && !Lb.needGround) { Lb.needGround = true; Lb.groundSinceS = 0.0; }
        }
    }
    if (!tracked) return false;

    // ── HIGGS grab: INSTANT handoff — PLANCK's drag machinery takes this same planner mode,
    // and two drivers on one mode is the forbidden state. No fade here, by design. ────────
    if (Interop::IsActorGrabbedByPlayer(actor)) {
        if (mc && PlannerActive(mc) && w.driving) {
            PlannerCtl(mc)->ClearPlannerDirectControl();
            // ⛔ EvaluatePackage is DEFERRED (2026-08-29, after a freeze): inline from inside
            // the movement-evaluation hook recurses the evaluator. PLANCK defers it too.
            if (auto* task = SKSE::GetTaskInterface())
                task->AddTask([id]() {
                    if (auto* f = RE::TESForm::LookupByID(id))
                        if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
                });
            logger::info("PUSHWALK {:08X} released to HIGGS grab (planner cleared)", id);
        }
        std::scoped_lock lk(g_walkMx);
        g_walk.erase(id);
        return false;
    }
    // ═══ ★ v11 THE TIER CHECK IS INDEPENDENT OF THE WALK (user session 2026-09-07 12:27) ══════════════
    // Until now stumble/knockdown were asked only AT ENGAGE and, behind pushStepEscalate, during a walk.
    // A refused engage was never asked at all (12:09:03: 86 u on the head, "no walk" — the walk's own
    // refractory), nor was a standing NPC bent past the bar without a walk. Engage decides whether she
    // WALKS; it must not decide whether she FALLS. Now: every frame the sensor is live, read the CHAIN
    // MAXIMUM and ask the one question. The sensor publishes ZERO through its own reaction stand-down,
    // so this cannot re-fire on the stumble it just caused; the walk-onset grace still applies while
    // DRIVING. rateW is last frame's (the ring is fed by the walk logic below) — cosmetic while
    // pushStepStaggerRate is 0. pushStepEscalate is RETIRED — this is always on.
    if (ObjectHold::PushStepReaction() != 0.f) {
        float cmag = 0.f, cdx = 0.f, cdy = 0.f; int cslot = -1;
        bool tierGot = false;
        if (travelMode) {   // ★ v12: the tiers read the SAME number the walk does — distance from origin
            // ★ v12a: never on her own gait — that is what fired every "nothing happened, then she stumbled".
            // ★★ v13: the WHOLE BODY decides, never the touched bone.
            tierGot = !selfMoving && TravelMax(id, actor->data.angle.z, cdx, cdy, cmag, cslot);
        } else {
            tierGot = ReadChainGap(id, w.slot, cmag, cdx, cdy, cslot);
        }
        const bool graceOver = !w.driving || (nowS - w.engageS) >= (double)ObjectHold::PushStepReactGraceS();
        // ★ v11 TIER HOLD: a jolt crosses the bar and rebounds inside ~100 ms; a shove or a lean stays. The
        // timers live on the WalkState and are cleared the instant the gap is under the bar — the stand-down
        // publishes zero, so a stumble we just fired can never keep its own timer alive.
        const float latKc = (tierGot && !travelMode) ? LatKFor(actor, cdx, cdy, cmag) : 1.f;   // v12: no lateral fudge
        const float smagC = cmag * (std::max)(1.f, latKc);
        // v11: the bars belong to the bone the verdict is being read from.
        float cEng = 0.f, cStag = 0.f, cRag = 0.f;
        BarsForSlot(tierGot ? cslot : w.slot, cEng, cStag, cRag);
        // v29c: one call, so a node's own stumble / ragdoll override is honoured here exactly as in the receipt
        if (travelMode) TravelBarsFor(tierGot ? cslot : w.slot, w.driving, cEng, cStag, cRag);
        const bool  aboveRag  = tierGot && cmag  >= cRag;
        const bool  aboveStag = tierGot && smagC >= cStag;
        double heldRagS = 0.0, heldStagS = 0.0;
        {
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_walk.find(id); it != g_walk.end()) {
                WalkState& ws = it->second;
                if (aboveRag)  { if (ws.ragAboveS  <= 0.0) ws.ragAboveS  = nowS; heldRagS  = nowS - ws.ragAboveS;  } else ws.ragAboveS  = 0.0;
                if (aboveStag) { if (ws.stagAboveS <= 0.0) ws.stagAboveS = nowS; heldStagS = nowS - ws.stagAboveS; } else ws.stagAboveS = 0.0;
            }
        }
        const double holdS = travelMode ? 0.0 : (double)ObjectHold::PushStepTierHoldS();   // v12: no hold — the distance IS the evidence
        const bool   ripe  = (aboveRag && heldRagS >= holdS) || (!aboveRag && aboveStag && heldStagS >= holdS);
        if (tierGot && graceOver && ripe) {
            char whereBuf[48];   // the receipt names the slot and the hold that ripened it
            std::snprintf(whereBuf, sizeof whereBuf, "%s s%d%s held %.2fs", w.driving ? "during walk" : "standing", cslot,
                          cslot == w.slot ? "" : (travelMode ? " (worst joint)" : " (chain)"), aboveRag ? heldRagS : heldStagS);
            const int fired = FireReactionTier(actor, id, w, mc, cStag, cRag, cmag, cdx, cdy, w.rateW, false, latKc, true, nowS, whereBuf);
            if (fired) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) { it->second.ragAboveS = 0.0; it->second.stagAboveS = 0.0; }
                return false;   // the evaluator tore the walk down (or parked it); nothing of ours may still command her
            }
        }
    }
    // v6: pressure gone no longer stops her dead. A DRIVING actor enters the decel fade
    // (pushStepRampOutS, default 2s) and the exit ritual runs when the fade completes.
    // A tracked-but-never-engaged entry (touch that displaced nothing) just expires.
    if (nowS - w.lastPressureS >= stopS) {
        if (!w.driving) {
            std::scoped_lock lk(g_walkMx);
            g_walk.erase(id);
            return false;
        }
        if (w.phase == 0) {
            const float cur = EnvelopeSpeed(w, nowS);
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_walk.find(id); it != g_walk.end()) {
                it->second.phase       = 1;
                it->second.decelStartS = nowS;
                it->second.decelSpeedU = cur;
                w = it->second;
            }
            logger::info("PUSHWALK {:08X} pressure gone — fading out from {:.0f} u/s {}",
                         id, cur, ObjectHold::PushStepSpeedTrack() != 0.f
                                  ? "(v10 down-slew at pushStepSpeedDownU)" : "over pushStepRampOutS");
        }
    } else if (w.driving && w.phase == 1 && !w.capped &&
               nowS - w.lastPressureS < (double)ObjectHold::PushStepResumeS() &&
               [&]{ float rdx, rdy, rmag;   // v7: resuming needs a REAL re-push, not mere
                    // v7.8: the resume threshold is clamped ABOVE the stop threshold so no
                    // knob combination can re-invert the hysteresis (the 12:31:49 flutter).
                    const float thr = (std::max)(ObjectHold::PushStepDispU(),
                                                 ObjectHold::PushStepStopU() + 0.2f);
                    return ArmIK::GetPushSenseGap(id, w.slot, thr, rdx, rdy, rmag); }()) {
        // (v6.1: a cap-final fade ignores resumes — the 00:21 ping-pong marched her 15m:
        //  cap said stop, resume said go, 90x a second, and the fade never advanced. v7 adds
        //  the gap requirement so the stop-at-zero rule and the resume rule cannot ping-pong.)
        // v6: pressure RESUMED mid-fade — re-accelerate from the current speed, no pop:
        // place the ramp clock so the ramp-in curve passes through the present speed.
        const float cur = EnvelopeSpeed(w, nowS);
        std::scoped_lock lk(g_walkMx);
        if (auto it = g_walk.find(id); it != g_walk.end()) {
            float frac = it->second.targetSpeedU > 1.f ? cur / it->second.targetSpeedU : 0.f;
            if (frac < 0.f) frac = 0.f;
            if (frac > 1.f) frac = 1.f;
            it->second.phase   = 0;
            it->second.engageS = nowS - (double)(ObjectHold::PushStepRampInS() * frac);
            w = it->second;
        }
        logger::info("PUSHWALK {:08X} pressure resumed mid-fade — re-accelerating from {:.0f} u/s",
                     id, cur);
    }

    if (!mc) return false;
    if (actor->IsInRagdollState() || HandBox::IsSceneSuspended()) {
        // Down or mid-get-up: make sure nothing of ours is still commanding her, then stay out.
        if (mc && PlannerActive(mc) && w.driving) {
            PlannerCtl(mc)->ClearPlannerDirectControl();
            if (auto* task = SKSE::GetTaskInterface())
                task->AddTask([id]() {
                    if (auto* f = RE::TESForm::LookupByID(id))
                        if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
                });
            std::scoped_lock lk(g_walkMx);
            g_walk.erase(id);
            logger::info("PUSHWALK {:08X} entered ragdoll/scene while driven — released", id);
        }
        return false;
    }

    // ══ UNSUPPORTED -> RAGDOLL (2026-09-03, user spec) ══════════════════════════════════════
    // "If both feet IK are lifted from the ground together ... she should go ragdoll, cause it's
    // obvious that NPC can't support itself. Or the player sweeps the NPC's feet from under
    // them, and they should fall."
    //   ⛔ THIS SITS OUTSIDE THE `!PlannerActive` GUARD BELOW, DELIBERATELY. Everything in the
    // enter block — including the stagger/ragdoll tiers — is skipped while she is already being
    // push-walked, which is why a push cannot currently ESCALATE (user-diagnosed, confirmed in
    // source). Lifting her off the ground very often happens DURING a walk, so a fall check
    // inside that guard would be dead exactly when it matters.
    //   Being airborne also flips her animation-driven, where planner control is refused and the
    // old path logged "displaced 11.50u but animation-driven — standing down". The user's point
    // is that this was backwards: nothing under her feet is the clearest possible case FOR a
    // knockdown.
    //   THE SIGNAL IS THE ENGINE'S OWN, not foot IK: bhkCharacterController::surfaceInfo
    // .supportedState (kUnsupported 0 / kSliding 1 / kSupported 2) — ask the authority that
    // decides whether she is standing, rather than inferring it from bones.
    // v36: the FURNITURE half of the gate only (a feet path: BUSY stands down, a LEAN still falls). Combat / killmove /
    // equip were never gates on this path and deliberately stay that way — that is not what the furniture ruling decided.
    if (ObjectHold::PushStepFallRagOn() && !actor->IsInRagdollState() && !PushFurnitureBlock(actor, /*feetPath*/ true)) {
        auto* cc = actor->GetCharController();
        const int sup = cc ? (int)cc->surfaceInfo.supportedState.underlying() : 2;
        // ⚠ PLAYER-CAUSED ONLY. Without this every NPC who steps off a rock, walks down stairs
        // or jumps would collapse. `lastPressureS` is stamped by the trunk-pressure scan, so a
        // recent player contact on THIS actor is what makes her fall ours.
        const bool ours = (nowS - w.lastPressureS) <= (double)ObjectHold::PushStepFallGraceS();
        int fall = 0;
        {
            std::scoped_lock lk(g_walkMx);
            auto it = g_walk.find(id);
            if (it != g_walk.end()) {
                if (sup == 0 && ours) fall = ++it->second.fallFrames;
                else                  it->second.fallFrames = 0;
            }
        }
        double lastR = w.lastReactS;
        {
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_reactCoolS.find(id); it != g_reactCoolS.end() && it->second > lastR)
                lastR = it->second;
        }
        if (fall >= (int)ObjectHold::PushStepFallFrames() &&
            nowS - lastR > (double)ObjectHold::PushStepReactCoolS()) {
            {
                std::scoped_lock lk(g_walkMx);
                if (g_reactCoolS.size() > 64) g_reactCoolS.clear();
                g_reactCoolS[id] = nowS;
                if (g_ragSettle.size() > 64) g_ragSettle.clear();
                g_ragSettle[id] = nowS + (double)ObjectHold::PushStepRagSettleS();
            }
            logger::info("PUSHREACT {:08X} UNSUPPORTED for {} frames with a player contact "
                         "{:.2f}s ago — she has nothing under her feet; ragdolling",
                         id, fall, nowS - w.lastPressureS);
            RagFrame::Arm(id, -1.f, w.slot, "unsupported", 0.f, true);
            QueueReaction(id, 0.f, 1.f, true, "sweeped", PusherOf(w));   // the feet path (bFeetLift); ships off (pushStepFallRag 0)
            // a ragdoll and a driven walk must never overlap (the warp-into-walls bug)
            if (mc && PlannerActive(mc)) {
                PlannerCtl(mc)->ClearPlannerDirectControl();
                if (auto* task = SKSE::GetTaskInterface())
                    task->AddTask([id]() {
                        if (auto* f = RE::TESForm::LookupByID(id))
                            if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
                    });
            }
            std::scoped_lock lk(g_walkMx);
            g_walk.erase(id);
            g_lastDriveS[id] = nowS;
            return false;
        }
    }

    // ── enter — v4 (user spec 2026-08-29): DISPLACEMENT IS THE TRIGGER ───────────────────
    // "What I want is not the hand sinking in to be the trigger, I want it to be when her body
    // get pushed away from what she is supposed to be." Contact (the pressure set) only says
    // the PLAYER is the cause; the ENGAGE itself requires a trunk bone measurably off its XP32
    // home, and the walk distance is PROPORTIONAL to that displacement (3u push → ~10u step),
    // capped by pushStepWalkU × region. A touch that displaces nothing walks nothing — the
    // away-from-player fallback engage is GONE (it produced the 60u marches and, after a fresh
    // re-touch, the lag-poisoned FORWARD walks the user observed).
    if (!PlannerActive(mc)) {
        auto* mo = MotionCtl(mc);
        // v4.1: direction = the displacement of the bone the hand is TOUCHING (w.slot from the
        // pressure set), never the displaced-most bone -- the pelvis counter-lean on a chest
        // push points AT the player and was winning the vote (bearings -5/+18/-1 deg measured).
        float mdx = 0.f, mdy = 0.f, mmag = 0.f; const int mslot = w.slot;
        int mslotEff = mslot;   // v8.7: a latched peak may have come from another chain link
        int travelWinSlot = -1;  // v14d: the VOTING node that won, so its own bar can be used below
        // v5 (2026-08-29): the sensor of record measures the touched bone against the
        // ANIMATION'S INTENT (FK of the drive pose), not against the visible node — the node
        // follows the body (postPhysics write-back), so body-vs-node measured our own glue.
        // pushStepSensor 4 falls back to the legacy sensor for A/B.
        const bool v5 = ObjectHold::PushStepSensor() >= 4.5f;
        bool got = v5
            ? ArmIK::GetPushSenseGap(id, mslot, 0.01f, mdx, mdy, mmag)
            : GrabDiag::GetSlotPushDisplacement(id, mslot, 0.01f, mdx, mdy, mmag);
        // ★ v12 THE TRAVEL SENSOR OWNS THE ENGAGE. One number, one comparison: how far the touched bone has
        // moved from where it was when the hand landed. Everything the gap sensor needed to defend itself
        // against — a moving reference, servo lag, soft-flesh levers, the walk's own motion — is answered by
        // the capture, so the gates below are all skipped in this mode.
        if (travelMode) {
            // ★★ v13: the WHOLE BODY decides. The touched slot is carried only for the receipt and the
            // per-region walk-distance cap.
            int lslot = -1;
            got = !selfMoving && TravelMax(id, actor->data.angle.z, mdx, mdy, mmag, lslot);
            if (got) TravelNoteRise(id, mmag, nowS);   // v14a: re-arm the crossing clock while she is at rest
            travelWinSlot = lslot;                    // v14d
            if (!got) { mmag = 0.f; lslot = mslot; }
            else       mslotEff = lslot;
            // ★ v12a: the progress receipt lived in the refusal branch, so it printed ONCE all session — exactly
            // the mistake report 33 §8.5 names (a receipt throttled below the event it describes). It speaks here,
            // 5 Hz, whether the push is winning or not.
            if (!selfMoving) {
                bool speak = false; double capAgo = 0.0;
                {   // the lock is CLOSED before TravelDump, which takes the same mutex per bone
                    std::scoped_lock lk(g_walkMx);
                    if (auto it = g_travel.find(id); it != g_travel.end() && nowS - it->second.lastLogS > 0.2) {
                        it->second.lastLogS = nowS; capAgo = nowS - it->second.capturedS; speak = true;
                    }
                }
                if (speak) {
                    char tdump[176];
                    TravelDump(id, actor->data.angle.z, tdump, sizeof tdump);
                    const float rOff = TravelBarOffsetFor(lslot, w.driving);   // v14d + v16/v20
                    float rWalk = 0.f, rStag = 0.f, rRag = 0.f;                // v29c per-node rungs
                    TravelBarsFor(lslot, w.driving, rWalk, rStag, rRag);
                    logger::info("PUSHTRAVEL {:08X} worst joint s{} moved {:.2f}u from its animation point since "
                                 "contact ({:.2f}s ago) — ITS bars: walk {:.1f}, stumble {:.1f}, ragdoll {:.1f}"
                                 "{} | all joints [{}] (hand on s{})",
                                 id, lslot, mmag, capAgo,
                                 rWalk, rStag, rRag,
                                 rOff != 0.f ? fmt::format(" (node offset {:+.1f})", rOff) : std::string(),
                                 tdump, mslot);
                }
            }
        }
        // v7.3/v7.4 RESPONSIVENESS: the gap's GROWTH RATE counts toward the trigger. ⚠ v7.4:
        // the history updates on EVERY tracked pass — BEFORE the settle/refractory gates —
        // because v7.3 read it after them and the rate was starved to zero on almost every
        // engage (measured 11:17: one non-zero rate in a whole session; the push built during
        // the gated window, unobserved).
        float rate = 0.f, rateW = 0.f;   // v9.9: rate = one-frame (the engage bar, unchanged); rateW = windowed (the tiers)
        // ⛔ v11 16:20 THE STALE-COPY BUG (user: "I moved her head by 7u fully, my hand moved. If you recorded 0,
        // something is wrong and that gate is not doing its job"). `w` is a COPY snapshotted at the top of this
        // tick (line ~911) so the long function reads consistent state without holding the mutex — but the
        // PRESSURE SCAN runs AFTER that snapshot and writes hand travel through `g_walk[...]`, a different `w`.
        // The gate below therefore read handP0Valid=false / handTravelU=0 forever: it NEVER passed, and only the
        // stumble tier (which has no hand gate) could fire — "no movement, then straight to stumble".
        // Every field the scan PRODUCES must be re-read here, exactly as rate/rateW already are.
        {
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_walk.find(id); it != g_walk.end()) {
                w.handP0Valid = it->second.handP0Valid;
                w.handTravelU = it->second.handTravelU;
                w.handP0S     = it->second.handP0S;           // v11.1b (review): the same rule — every scan-produced field
                w.pressWand[0] = it->second.pressWand[0];  w.pressWand[1] = it->second.pressWand[1];
                w.lastPressWandS[0] = it->second.lastPressWandS[0];  w.lastPressWandS[1] = it->second.lastPressWandS[1];
            }
        }
        if (got) {
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_walk.find(id); it != g_walk.end()) {
                if (it->second.lastGapS > 0.0 && nowS > it->second.lastGapS &&
                    nowS - it->second.lastGapS < 0.25)
                    rate = (mmag - it->second.lastGapMag) / (float)(nowS - it->second.lastGapS);
                it->second.lastGapMag = mmag;
                it->second.lastGapS   = nowS;
                rateW = RingPush(it->second, mmag, nowS, ObjectHold::PushStepRateWinS());
            }
        }
        float eff = mmag + (rate > 0.f ? rate : 0.f) * ObjectHold::PushStepVelGainS();
        // v8.2 SIDE GAIN: the trunk is mechanically stiffer sideways, and TWIST never moves
        // the lever point - so a lateral gap under-reports the hand's real effort. Scale the
        // felt intensity by lateralness (|sin| of the gap bearing vs her facing): forward/back
        // x1, pure side x pushStepSideGain, smooth in between. Feeds trigger, speed, distance.
        if (got && mmag > 0.01f) {
            const float sg = ObjectHold::PushStepSideGain();
            if (sg > 1.f) {
                const float gapHeading = GetHeadingFromVector(RE::NiPoint3{ mdx / mmag, mdy / mmag, 0.f });
                const float relR = gapHeading - actor->data.angle.z;
                const float lat  = std::fabs(std::sin(relR));
                eff *= 1.f + (sg - 1.f) * lat;
            }
        }
        // v7.6 CHAIN (user design): a trunk push expresses along the whole spine — whichever
        // LINK registers first may trigger. Neighbors are SPINE bones only; the pelvis joins
        // only when directly touched (its balance counter-swing points AT the player — the
        // v4.1 lesson). Direction/magnitude come from the winning bone; the region cap stays
        // with the TOUCHED slot (the stiffness map is about where the hand is).
        int trigSlot = mslot;
        // ⛔ v13b: NEVER in travel mode. This block runs AFTER the travel read and overwrites mmag with
        // ArmIK::GetPushSenseGap — the LEGACY baseline-subtracted gap, which carries each joint's resting servo
        // lag (3-4u at the head) and is not zeroed at contact. It could therefore hijack a fresh 1u travel
        // reading with a stale 4u gap from a neighbour bone. The whole-body TravelMax already reads every joint.
        if (!travelMode && ObjectHold::PushStepChain() >= 0.5f && v5) {
            static constexpr int kNb[12][2] = { {-1,-1},{-1,-1},{-1,-1},
                                                {7,6},    // touched 3 (head): neck/chest (v8.3)
                                                {5,-1},   // touched 4 (waist): belly may fire
                                                {4,6},    // touched 5 (belly): waist/chest
                                                {5,7},    // touched 6 (chest): belly/neck
                                                {6,3},    // touched 7 (neck): chest/head (v8.3)
                                                {-1,-1},  // touched 8 (thigh): solo
                                                {-1,-1},{-1,-1},
                                                {4,-1} }; // touched 11 (com): waist
            const int* nb = (mslot >= 0 && mslot < 12) ? kNb[mslot] : kNb[0];
            for (int k2 = 0; k2 < 2; ++k2) {
                if (nb[k2] < 0) continue;
                float cdx, cdy, cmag;
                if (ArmIK::GetPushSenseGap(id, nb[k2], 0.01f, cdx, cdy, cmag) && cmag > eff) {
                    got = true; trigSlot = nb[k2];
                    mdx = cdx; mdy = cdy; mmag = cmag; eff = cmag;   // neighbor: raw gap only
                }
            }
        }
        // ★ v8.7 LATCH THE PEAK — before any gate can return. A fast push is an IMPULSE: it
        // spikes and collapses inside ~0.2s while the settle/refractory gates may still be
        // shut, and the v8.1 rebound veto then reads the collapsing tail and kills it (user:
        // "gentle touch works great, anything faster is kinda not working"). Only PRESSED
        // frames may build a peak, so hover and soft-part jiggle can never latch one.
        if (got && !travelMode && w.pressDistU <= ObjectHold::PushStepPressU()) {   // v12: no peak latch — the distance does not collapse
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_walk.find(id); it != g_walk.end()) {
                if (nowS - it->second.peakS > (double)ObjectHold::PushStepPeakWindowS())
                    it->second.peakMag = 0.f;
                if (mmag > it->second.peakMag) {
                    it->second.peakMag  = mmag;
                    it->second.peakDx   = mdx;
                    it->second.peakDy   = mdy;
                    it->second.peakRate = rate > 0.f ? rate : 0.f;
                    it->second.peakRateW = rateW > 0.f ? rateW : 0.f;   // v9.9: the peak carries its own windowed speed
                    it->second.peakSlot = trigSlot;
                    it->second.peakS    = nowS;
                }
                w.peakMag  = it->second.peakMag;  w.peakDx   = it->second.peakDx;
                w.peakDy   = it->second.peakDy;   w.peakRate = it->second.peakRate;
                w.peakRateW = it->second.peakRateW;
                w.peakSlot = it->second.peakSlot; w.peakS    = it->second.peakS;
            }
        }
        // v2 refractory + v4 settle gate — AFTER the history update, so the rate survives them.
        if (nowS < w.reengageAfterS) return false;
        const double settleS = (double)ObjectHold::PushStepSettleS();
        if (nowS - w.lastEpisodeEndS <= settleS) return false;
        if (nowS - LastDriveS(id) <= settleS) return false;
        // v8.7: prefer the latched PEAK when it is fresh and stronger — this is what makes a
        // fast impulse survive gate timing. Direction comes from the peak too (the moment the
        // hand was driving her), never from the rebound tail.
        bool usedPeak = false;
        if (!travelMode && w.peakMag > mmag && (nowS - w.peakS) <= (double)ObjectHold::PushStepPeakWindowS()) {
            mmag = w.peakMag; mdx = w.peakDx; mdy = w.peakDy;
            rate = w.peakRate; rateW = w.peakRateW;   // v9.9: the peak's own speeds
            if (w.peakSlot >= 0) mslotEff = w.peakSlot;
            eff  = mmag + rate * ObjectHold::PushStepVelGainS();
            got  = true;
            usedPeak = true;
        }
        // ★ v9.3 VELOCITY-SCALED TRIGGER (user): "if the touch is low velocity, wait more space
        // before moving... gate the distance between .5u at fast speed and 3u at low speed."
        // A gentle press must travel; a shove barely has to. Interpolated on the gap's GROWTH
        // RATE, which is the honest measure of how fast the hand is driving her.
        // v10.1 (user in VR: the finger is "still super prime"): the bar interpolated on the ONE-FRAME
        // rate — one 0.6 u wobble frame reads +45 u/s and dropped the bar to DispFastU instantly. With
        // pushStepEngageWinRate the bar reads the WINDOWED rate: a poke must show 100 ms of real growth
        // to earn the low bar; a slow press needs the slow bar. (eff — the speed/budget intent — keeps rate.)
        // ★ v11 THE FLAT LADDER (report 32 §1, user 2026-09-06 23:30): ONE bar per bone, no rate
        // interpolation, no soft/head multipliers. "let's do 2 u for all" — the rate is the RAMP, not
        // the gate (rule 2). The head keeps a raised ABSOLUTE bar until its 3–4 u body-vs-intent offset
        // is found and fixed (report 32 §2.4). Retired here, still parsed: DispSlowU, DispFastU,
        // RateFastU, BreastMul, HeadTrig, EngageWinRate.
        // v11 15:45: per chain group (BarsForSlot). pushStepBarHeadU is RETIRED — the head is the HIGH group now.
        float dispUeff = 0.f; { float sU, rU; BarsForSlot(mslotEff, dispUeff, sU, rU); }
        if (travelMode) dispUeff = ObjectHold::PushStepTravelWalkU()
                                 + TravelBarOffsetFor(travelWinSlot, w.driving);   // v12 ladder + v14d/v20 offsets
        // Rule 7: a held object needs a higher bar — "most go on the chest". Only when nothing else is
        // pressing, so a hand and an object together read as the hand.
        if (w.pressObj && !w.pressHand) dispUeff *= ObjectHold::PushStepObjectBarMul();
        float latK = 1.f;
        if (got && !travelMode && mmag > 0.01f && ObjectHold::PushStepSideGain() > 1.f) {
            const float gapHeading = GetHeadingFromVector(RE::NiPoint3{ mdx / mmag, mdy / mmag, 0.f });
            latK = 1.f + (ObjectHold::PushStepSideGain() - 1.f) * std::fabs(std::sin(gapHeading - actor->data.angle.z));
        }
        const float depthK = (!travelMode && ObjectHold::PushStepLatTrig() != 0.f) ? latK : 1.f;
        if (got && (mmag * depthK < dispUeff || mmag < 0.15f)) got = false;
        // v24: no walk-back either — "no push reactions AT ALL on an NPC in combat".
        if (got && PushReactionsBlocked(actor)) {
            static thread_local double s_lastCombatSayS = 0.0;
            if (nowS - s_lastCombatSayS > 3.0) {
                s_lastCombatSayS = nowS;
                // v36: name the REAL reason — this receipt used to say "IN COMBAT / KILLMOVE" for every stand-down.
                const char* furn = PushFurnitureBlock(actor, false);
                const char* why  = furn ? furn
                                 : actor->IsInCombat()   ? "she is IN COMBAT (pushStepCombatGate)"
                                 : actor->IsInKillMove() ? "she is in a KILLMOVE (pushStepKillMoveGate)"
                                                         : "an EQUIP is settling (pushStepEquipSettleS)";
                logger::info("PUSHWALK {:08X} stood down — {}", id, why);
            }
            got = false;
        }
        // ★ v11 HAND TRAVEL GATE (user 14:20: "wait 3u of hand shove before she start moving. I shove 1u and she
        // move"): the body bar is a poor proxy for the shove — the keyframed hand throws her 3–4 u after ~1 u of
        // its own motion (PUSHTRIAD 14:00: node-org moved 3.5 u on a barely-touch, intent-org 0.2 u). So the walk
        // also waits for the HAND to have travelled pushStepHandTravelU since it first touched her. Sideways
        // (more than ~45° off her front/back axis) uses pushStepHandTravelSideU — 0 = exempt, the user's call.
        // Tiers are NOT gated by this: a stumble or a knockdown is judged on what happened to her, not the hand.
        if (got && !travelMode) {   // v12: no hand-travel gate — the bone's own distance is the measure
            bool side = false;
            if (mmag > 0.01f) {
                const float gh = GetHeadingFromVector(RE::NiPoint3{ mdx / mmag, mdy / mmag, 0.f });
                side = std::fabs(std::sin(gh - actor->data.angle.z)) > 0.7071f;
            }
            const float need = side ? ObjectHold::PushStepHandTravelSideU() : ObjectHold::PushStepHandTravelU();
            // ★ v11.1 (2026-09-07, report 33 §8.3): the anchor of record is the ENGINE's (planted the frame after
            // Havok saw the contact); the API-snapshot anchor stands in while no engine stamp has landed, and is
            // the only one with pushStepAnchorSrc 0. The receipt says which one it read.
            bool   p0Valid = w.handP0Valid;
            float  travel  = w.handTravelU;
            double p0S     = w.handP0S;
            const char* aSrc = w.handP0Valid ? "api" : "none";
            int    aSlot   = -1;
            if (ObjectHold::PushStepAnchorSrc() != 0.f) {
                std::scoped_lock lk(g_walkMx);
                if (auto ea = g_engAnchor.find(id); ea != g_engAnchor.end()) {
                    // v11.1b: the PRESSING hand's clock decides — a hand is pressing when the API scan saw it this frame
                    // or the engine stamped it inside the lapse window. Both pressing: the one that has travelled
                    // further is the pusher (a resting hand reads ~0 and must not veto the other).
                    float best = -1.f;
                    for (int hnd = 0; hnd < 2; ++hnd) {
                        const EngAnchor& a = ea->second.h[hnd];
                        if (!a.valid) continue;
                        const bool pressing = w.pressWand[hnd] || (nowS - a.lastEngS) <= kAnchorLapseS;
                        if (!pressing || a.travelU <= best) continue;
                        best = a.travelU; p0S = a.firstS; aSlot = a.slot;
                        aSrc = a.src == 1 ? (hnd ? "engine L/weapon" : "engine R/weapon")
                             : a.src == 2 ? (hnd ? "engine L/finger" : "engine R/finger")
                                          : (hnd ? "engine L/hand"   : "engine R/hand");
                    }
                    if (best >= 0.f) { p0Valid = true; travel = best; }
                }
            }
            if (need > 0.f && (!p0Valid || travel < need)) {
                // ★ the receipt prints EVERY FRAME for the first second after the anchor was planted — the event it
                // describes lasts ~150 ms (report 33 §8.5) — then settles to 4 Hz. v11.1b: on its OWN throttle stamp
                // (waitLogS): sharing lastFallbackS silenced the press/rebound/hemisphere/not-drivable receipts for 2 s
                // after every waiting line, which is exactly why the 17:58:04 pelvis push refused a walk with no reason printed.
                const double every = (p0Valid && (nowS - p0S) < 1.0) ? 0.0 : 0.25;
                if (nowS - w.waitLogS >= every) {
                    std::scoped_lock lk(g_walkMx);
                    if (auto it = g_walk.find(id); it != g_walk.end()) it->second.waitLogS = nowS;
                    logger::info("PUSHWALK {:08X} gap {:.2f}u past the bar but the hand has travelled only {:.2f}u of {:.2f}u since it touched her ({}; anchor {} slot {} age {:.3f}s) — waiting",
                                 id, mmag, p0Valid ? travel : 0.f, need, side ? "side" : "front/back",
                                 p0Valid ? aSrc : "NOT SET (no hand box read)", aSlot, p0Valid ? nowS - p0S : 0.0);
                }
                got = false;
            }
        }
        // v8.4 PRESS GATE: crowding is not pushing. FBG's close-quarters shrink lets the
        // player close in and HIGGS's RESTING hand-slabs (hip height, infinite mass) genuinely
        // plow her capsules - hands near + real displacement = a walk from just standing
        // close (user-reported "almost every time"). A DELIBERATE push PRESSES the surface;
        // a resting hand HOVERS. Tracking still starts at nearU (rate priming, fleeing-chest);
        // the ENGAGE demands a contact actually touching.
        if (got && !travelMode && !usedPeak && w.pressDistU > ObjectHold::PushStepPressU()) {
            if (nowS - w.lastFallbackS > 2.0) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) it->second.lastFallbackS = nowS;
                char gaps[128];
                GapDump(id, gaps, sizeof gaps);
                logger::info("PUSHWALK {:08X} gap {:.2f}u but the hand only HOVERS (closest "
                             "contact {:.2f}u > {:.2f}u) — crowding, not a push; ignored | "
                             "contacts[{}] gaps[{}]",
                             id, mmag, w.pressDistU, ObjectHold::PushStepPressU(),
                             w.contactsDbg[0] ? w.contactsDbg : "-", gaps);
            }
            got = false;
        }
        // v8.1 REBOUND VETO: a COLLAPSING gap means the push already ended and the body is
        // springing home (measured: an 81 u/s sideways engage at rate -42). A real push has a
        // growing or holding gap.
        if (got && !travelMode && !usedPeak && rate < -ObjectHold::PushStepRateVetoU()) {
            if (nowS - w.lastFallbackS > 2.0) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) it->second.lastFallbackS = nowS;
                logger::info("PUSHWALK {:08X} gap {:.2f}u but COLLAPSING at {:.1f} u/s — rebound "
                             "echo, not a push; ignored", id, mmag, rate);
            }
            got = false;
        }
        // ★ v7.8 HEMISPHERE GUARD (user find, 2026-08-30): A PUSH CAN NEVER PULL. Breast/belly
        // capsules hang on the trunk bodies the sensor measures - pressing them rings the body
        // like a spring, and the spring-back OVERSHOOTS forward past the intent, reading as a
        // push toward the player. Any measured direction pointing into the player's hemisphere
        // is physically impossible for a push and is rejected here, receipt included. Lateral
        // and angled pushes stay fully measured (the user's "134 degrees" spec).
        if (got && !travelMode) {   // v12: the direction comes from the bone's own travel; nothing to clamp
            const float hemi = ObjectHold::PushStepHemiDot();
            if (hemi > -0.99f) {
                if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                    const auto ap = actor->GetPosition();
                    const auto pp = pl->GetPosition();
                    float ax = ap.x - pp.x, ay = ap.y - pp.y;
                    const float al = std::sqrt(ax * ax + ay * ay);
                    if (al > 1.f) {
                        ax /= al; ay /= al;
                        const float d = (mdx / mmag) * ax + (mdy / mmag) * ay;
                        if (d < hemi) {
                            // ★ v9.4 CLAMP, DON'T REJECT (measured: waist side-pushes of 16.18u
                            // and 5.66u thrown away at dot -0.47/-0.61). Shoving her waist
                            // sideways PIVOTS her, so the joint swings partly toward the hand -
                            // the push is real, only its direction is contaminated. A push can
                            // never PULL, but it certainly can be LATERAL: slide the direction to
                            // the nearest legal one (its component perpendicular to the
                            // away-vector) instead of binning the whole push. Only a true
                            // reversal - nothing left after the slide - is still refused.
                            const float px = (mdx / mmag) - ax * d;
                            const float py = (mdy / mmag) - ay * d;
                            const float pl = std::sqrt(px * px + py * py);
                            if (pl > 0.2f) {
                                mdx = px / pl; mdy = py / pl;   // unit, lateral, legal
                                mmag = mmag;                    // magnitude is untouched
                                if (nowS - w.lastFallbackS > 2.0) {
                                    std::scoped_lock lk(g_walkMx);
                                    if (auto it = g_walk.find(id); it != g_walk.end())
                                        it->second.lastFallbackS = nowS;
                                    logger::info("PUSHWALK {:08X} gap {:.2f}u leaned into the "
                                                 "player's hemisphere (dot {:+.2f}) — CLAMPED to "
                                                 "lateral, push kept", id, mmag, d);
                                }
                            } else {
                                if (nowS - w.lastFallbackS > 2.0) {
                                    std::scoped_lock lk(g_walkMx);
                                    if (auto it = g_walk.find(id); it != g_walk.end())
                                        it->second.lastFallbackS = nowS;
                                    logger::info("PUSHWALK {:08X} gap {:.2f}u points STRAIGHT at the "
                                                 "player (dot {:+.2f}) — a push cannot pull; ignored",
                                                 id, mmag, d);
                                }
                                got = false;
                            }
                        }
                    }
                }
            }
        }
        if (!got) {
            if (travelMode) return false;   // v12a: the receipt now speaks above, every frame the origin is live
            // Touch held but nothing displaced past the threshold: receipt, throttled — never silent.
            if (nowS - w.lastFallbackS > 2.0) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) it->second.lastFallbackS = nowS;
                logger::info("PUSHWALK {:08X} touch held, slot {} gap {:.2f}u ×lat {:.2f} = {:.2f}u vs bar {:.2f}u "
                             "(rate {:+.1f} win {:+.1f} u/s, eff {:.2f}u) — no walk (sensor v{})", id, mslot, mmag,
                             depthK, mmag * depthK, dispUeff, rate, rateW, eff, v5 ? 5 : 4);
            }
            return false;
        }
        // v11: the "at engage" tier call that lived here moved to the per-frame check above the walk logic
        // (chain-max, every frame, independent of whether this engage is accepted or refused).
        // Animation-driven / allow-rotation actors cannot be motion driven (base-game rule).
        // ⛔ The bump fallback (PLANCK's GetBumpedEx replica) was DELETED 2026-08-29: it fired
        // for the first time all session on an animation-driven actor at 22:27:38 and the game
        // froze within a frame, before the task's first receipt. Such an actor now simply
        // RESISTS — the capsules push back, no step — until she returns to a drivable state.
        if (mo->IsAnimationDriven() || mo->IsAllowRotation()) {
            if (nowS - w.lastFallbackS > 2.0) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) it->second.lastFallbackS = nowS;
                logger::info("PUSHWALK {:08X} displaced {:.2f}u but NOT DRIVABLE — animDriven={} "
                             "allowRotation={} — standing down (the engine forbids planner control in this state; "
                             "an NPC walking or playing an AI-package animation is animation-driven, which is why "
                             "only the ANIMATION-side stumble can fire on her; our bump fallback was removed after "
                             "the 22:27 freeze)",
                             id, mmag, mo->IsAnimationDriven() ? 1 : 0, mo->IsAllowRotation() ? 1 : 0);
            }
            return false;
        }

        // ★★ 2.2.0 THE WALK IS AN INSTALL CHOICE (user, PPB.ini [Features] bPushWalk). Checked HERE, at the one place a
        // walk is started — after every gate that decides a push was real — so turning the walk off changes nothing
        // else: the stumble / knockdown tiers are evaluated per frame above and still fire on the same push.
        if (!Ini::FeaturePushWalk()) {
            if (nowS - w.lastFallbackS > 2.0) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) it->second.lastFallbackS = nowS;
                logger::info("PUSHWALK {:08X} displaced {:.2f}u — would step back, but the push WALK is switched off "
                             "(PPB.ini [Features] bPushWalk=0); stumble / knockdown still apply", id, mmag);
            }
            return false;
        }
        mo->MoveToHigh();
        PlannerCtl(mc)->SetPlannerDirectControl();
        mo->SetMotionDriven();
        // ★ v12: the walk moves her bones by itself, so the push is re-measured from HERE. Without this the
        // gait's own swing would climb to the stumble bar and she would stumble out of every walk.
        // *** v14 THE LEVER, measured HERE and nowhere else.
        // crossU = (the walk bar's distance) / (time since the origin was captured) = how fast she crossed it.
        // ORDER IS LOAD-BEARING: CaptureTravelOriginLocked(force) on the next line REWRITES capturedS, so
        // reading the clock after it would divide by ~0 and hand every push a maximum lever. This is the exact
        // lifecycle class that has bitten this file three times.
        const bool  leverOn = travelMode && ObjectHold::PushStepLever() != 0.f;
        float crossU = 0.f, leverNorm = 0.f;
        if (leverOn) {
            double t0 = 0.0;
            {
                std::scoped_lock lk(g_walkMx);
                if (auto itT = g_travel.find(id); itT != g_travel.end())
                    // v14a: the LATER of the two — the crossing's own start, never the contact's. Dividing by
                    // time-since-contact charged every gate delay and every resting hand against the lever.
                    t0 = (std::max)(itT->second.capturedS, itT->second.riseS);
            }
            const double dtC = (t0 > 0.0) ? (nowS - t0) : 0.0;
            crossU = (dtC > 0.001) ? (float)((double)ObjectHold::PushStepTravelWalkU() / dtC)
                                   : ObjectHold::PushStepCrossRefU();   // sub-ms cross = treat as a full shove
            float n = crossU / ObjectHold::PushStepCrossRefU();
            if (n < 0.f) n = 0.f;
            if (n > 1.f) n = 1.f;
            leverNorm = std::pow(n, ObjectHold::PushStepCrossExp());
        }
        if (travelMode) { std::scoped_lock lk(g_walkMx); CaptureTravelOriginLocked(id, nowS, true, "walk engaged"); }
        // Budget + speed from the EFFECTIVE push (v7.4): the rate term triggers a firm push
        // EARLY, at a small raw gap — classifying by the raw gap then gave the fastest pushes
        // the weakest response (18 u/s, 2u budgets — measured 11:17). eff carries the intent.
        // v14: when the lever is on, BOTH speed and distance are interpolations across their own bands,
        // driven by how hard the shove was (the crossing rate) rather than by eff. Neither can pin.
        const float idealU = WalkIdealFor(mslotEff);
        float budget = 0.f, tspd = 0.f;
        if (leverOn) {
            const float mf = ObjectHold::PushStepCrossMinFrac();
            budget = idealU * (mf + (1.f - mf) * leverNorm);
            const float lo = ObjectHold::PushStepSpeedMinU(), hi = ObjectHold::PushStepSpeedMaxU();
            tspd = lo + ((hi > lo ? hi - lo : 0.f) * leverNorm);
        } else {
            budget = (std::min)(eff * ObjectHold::PushStepDispGain(), idealU);
            tspd = ObjectHold::PushStepSpeedU() *
                   (eff / (std::max)(0.5f, ObjectHold::PushStepSpeedRefU()));
            if (tspd < ObjectHold::PushStepSpeedMinU()) tspd = ObjectHold::PushStepSpeedMinU();
            if (tspd > ObjectHold::PushStepSpeedMaxU()) tspd = ObjectHold::PushStepSpeedMaxU();
        }
        {
            const auto pos = actor->GetPosition();
            std::scoped_lock lk(g_walkMx);
            if (auto it = g_walk.find(id); it != g_walk.end()) {
                // v8.0 AGGRO RAMP (user: "she should start moving faster, relative to hand
                // speed"): eff already encodes intensity (depth + growth rate), so the ramp-in
                // time shrinks with it and the starting floor rises. A reference push keeps
                // the gentle 1s buildup; a violent one is near-instant.
                const float inten = eff / (std::max)(0.5f, ObjectHold::PushStepSpeedRefU());
                float frac = inten > 1.f ? 1.f / inten : 1.f;
                const float minFrac = (std::max)(0.05f, ObjectHold::PushStepRampMinFrac());
                if (frac < minFrac) frac = minFrac;
                it->second.rampTS     = ObjectHold::PushStepRampInS() * frac;
                float fl = ObjectHold::PushStepRampFloor() * (inten > 1.f ? inten : 1.f);
                if (fl > 0.6f) fl = 0.6f;
                it->second.rampFloorE = fl;
                float ndx = mdx / mmag, ndy = mdy / mmag;
                if (const float offDeg = ObjectHold::PushStepDirOffDeg(); offDeg != 0.f) {
                    const float a = offDeg * 0.0174533f;   // v7.1 TEST lever: rotate the walk dir
                    const float c = std::cos(a), sn = std::sin(a);
                    const float rx = ndx * c - ndy * sn, ry = ndx * sn + ndy * c;
                    ndx = rx; ndy = ry;
                }
                it->second.dirX         = ndx;
                it->second.dirY         = ndy;
                it->second.driving      = true;
                it->second.startX       = pos.x;             // the budget is measured from here
                it->second.startY       = pos.y;
                // ★ v9.3 TURN INTO THE PUSH (user): "she is getting pushed, so she will go
                // backward - turn toward the push and back away." Facing = toward where the
                // push came FROM (the reverse of the retreat vector), so the engine plays a
                // BACKWARD walk instead of a sideways strafe with locked facing.
                // ★ v11 rule 9 (report 32 §3.7): the same 220/140 split the stagger uses now applies to
                // the WALK. "all walk-backward stumbles face the direction of the push and go back,
                // except if it's from the back: 220° from the front, 140° from the back." Front sector
                // (|rel| <= pushStepStaggerFaceDeg, 110 → 220° wide) → face the push, walk BACKWARD (the
                // v9.3 behaviour). Rear sector (the remaining 140°) → face the RETREAT and walk FORWARD;
                // before this she spun ~180° to face a push that came from behind.
                if (ObjectHold::PushStepFacePush() != 0.f) {
                    const float aggZ = GetHeadingFromVector(RE::NiPoint3{ -ndx, -ndy, 0.f });
                    float relW = (aggZ - actor->data.angle.z) * 57.2958f;
                    while (relW > 180.f)  relW -= 360.f;
                    while (relW < -180.f) relW += 360.f;
                    const bool fromBehind = std::fabs(relW) > ObjectHold::PushStepStaggerFaceDeg();
                    const bool faceAway = (ObjectHold::PushStepWalkFaceSplit() != 0.f && fromBehind);
                    it->second.walkFaceAway = faceAway;   // v17: the steering keeps this rule
                    if (faceAway)
                        it->second.angleZ = GetHeadingFromVector(RE::NiPoint3{ ndx, ndy, 0.f });
                    else
                        it->second.angleZ = aggZ;
                } else {
                    it->second.angleZ = actor->data.angle.z;   // legacy: strafe, facing locked
                }
                it->second.budgetU      = budget;
                it->second.budgetU      = budget;
                it->second.peakMag      = 0.f;   // v8.7: the impulse has been spent
                RingReset(it->second);           // v9.9: fresh rate history — the walk moves the sensor's basis
                it->second.peakS        = -1e9;
                it->second.engageS      = nowS;
                it->second.targetSpeedU = tspd;
                it->second.liveTargetU  = tspd;   // v10.0: the first target; the follower starts from 0
                // *** v14 THE START RAMP is aggressive in proportion to the lever: the follower's up-slew
                // multiplier runs 0.5x (a slow lean) to 2.5x (a full shove), so a hard push reaches speed in
                // ~0.4 s and a lean builds gently. The DOWN-slew stays a constant and is dialled smooth in
                // the tuning file (pushStepSpeedDownU) -- "the ending ramp should be smooth".
                it->second.crossU       = crossU;
                it->second.leverNorm    = leverNorm;
                it->second.liveRatio    = leverOn ? (0.5f + 2.0f * leverNorm) : 1.f;
                it->second.cmdSpeedU    = 0.f;
                it->second.lastSpeedS   = nowS;
                it->second.lastSpeedLogS= nowS;
                it->second.phase        = 0;
                it->second.extended     = false;
                it->second.capped       = false;
                w = it->second;
            }
        }
        // Log it in the user's own framing: bearing relative to her front.
        const float pushHeading = GetHeadingFromVector(RE::NiPoint3{ w.dirX, w.dirY, 0.f });
        float rel = (pushHeading - actor->data.angle.z) * 57.2958f;
        while (rel > 180.f)  rel -= 360.f;
        while (rel < -180.f) rel += 360.f;
        logger::info("PUSHWALK {:08X} ENGAGED{} — touched {} trig {} displaced {:.2f}u eff {:.2f}u rate {:+.1f}u/s (win {:+.1f}) (sensor v{}), bearing "
                     "{:+.0f}° from her front → walk {:.0f}u (cap {:.0f}u) at {:.0f} u/s "
                     "(norm {:.3f}; ramp-in {:.2f}s floor {:.0f}%, extendable to {:.0f}u)",
                     id, usedPeak ? " [PEAK]" : "", mslot, mslotEff, mmag, eff, rate, rateW,
                     v5 ? 5 : 4, rel, w.budgetU, WalkIdealFor(mslotEff),
                     w.targetSpeedU, NormSpeedFor(actor, w.targetSpeedU),
                     w.rampTS, w.rampFloorE * 100.f, w.budgetU * ObjectHold::PushStepExtendMul());
        // 2.2.0 PPB_PushReaction "push": one per ENGAGE (this block runs once, when the planner is taken), sent on the
        // main thread. The planner control above has already been written, so the walk IS starting.
        if (auto* task = SKSE::GetTaskInterface()) {
            const PushWho who = PusherOf(w);
            task->AddTask([id, who]() {
                if (auto* f = RE::TESForm::LookupByID(id))
                    if (auto* a = f->As<RE::Actor>()) SendPushReactionOnMain(a, "push", who);
            });
        }
        if (leverOn)
            logger::info("PUSHWALK {:08X} LEVER - crossed the {:.1f}u bar at {:.1f}u/s (norm {:.2f} of ref {:.0f}u/s) "
                         "-> speed {:.0f} u/s (band {:.0f}-{:.0f}), distance {:.1f}u of {:.1f}u cap, start-ramp x{:.2f}",
                         id, ObjectHold::PushStepTravelWalkU(), crossU, leverNorm, ObjectHold::PushStepCrossRefU(),
                         tspd, ObjectHold::PushStepSpeedMinU(), ObjectHold::PushStepSpeedMaxU(),
                         budget, idealU, 0.5f + 2.0f * leverNorm);
        // A/B receipt: the OTHER sensor's simultaneous reading on the same bone, for tuning.
        {
            float odx = 0.f, ody = 0.f, omag = 0.f;
            const bool other = v5
                ? GrabDiag::GetSlotPushDisplacement(id, mslot, 0.01f, odx, ody, omag)
                : ArmIK::GetPushSenseGap(id, mslot, 0.01f, odx, ody, omag);
            if (other) {
                float orel = (GetHeadingFromVector(RE::NiPoint3{ odx / omag, ody / omag, 0.f })
                              - actor->data.angle.z) * 57.2958f;
                while (orel > 180.f)  orel -= 360.f;
                while (orel < -180.f) orel += 360.f;
                logger::info("PUSHWALK {:08X} A/B: sensor v{} read {:.2f}u @{:+.0f}° on the same engage",
                             id, v5 ? 4 : 5, omag, orel);
            }
        }
        {   // v8.5 forensics: the evidence line - contacts + all 7 gaps, every engage.
            char gaps[128];
            GapDump(id, gaps, sizeof gaps);
            logger::info("PUSHWALK {:08X} FORENSICS contacts[{}] gaps[{}] press={:.2f}u sawHard={}",
                         id, w.contactsDbg[0] ? w.contactsDbg : "-", gaps, w.pressDistU,
                         w.sawHard ? 1 : 0);
        }
    }

    // ── v6 envelope: budget → extension (sustained contact) → decel fade → exit ritual ────
    if (w.driving) {
        const auto pos = actor->GetPosition();
        const float dx = pos.x - w.startX, dy = pos.y - w.startY;
        const float walked = std::sqrt(dx * dx + dy * dy);
        // v7 (user): "stop when the push is 0" — the LIVE gap on the touched bone IS the
        // push. Below pushStepStopU the push has ended even if the hand still hovers in
        // contact; walking itself relieves the gap, so she stops when she has yielded enough.
        if (w.phase == 0) {
            float ldx = 0.f, ldy = 0.f, lmag = 0.f;
            const bool v5live  = ObjectHold::PushStepSensor() >= 4.5f;
            const bool liveGot = v5live
                ? ArmIK::GetPushSenseGap(id, w.slot, 0.01f, ldx, ldy, lmag)
                : GrabDiag::GetSlotPushDisplacement(id, w.slot, 0.01f, ldx, ldy, lmag);
            // ★ v9.8 ESCALATION (user, 2026-09-06): the tiers are re-evaluated EVERY FRAME of the
            // walk with the live gap, so a push that keeps building promotes the walk into a
            // stagger or a knockdown mid-episode. Same history/rate and peak-latch rules as the
            // enter block (a PRESSED frame may build a peak; hover cannot); same cooldown.
            // ★ v10.1 ONSET GRACE (user in VR 2026-09-06: "the shove happens almost right away"): 20 of 27
            // mid-walk staggers fired 55–130 ms after engage on a windowed rate the displacement flatly
            // contradicted (16.97 → 5.58 u yet +32.7 u/s). The walk's own onset moves the sensor's
            // reference — the gap collapses and re-grows — and the window restarted at engage read that
            // rebound as a shove. For pushStepReactGraceS after engage the tiers are not consulted AND the
            // ring is not fed, so the first samples that can ever fire are post-transient.
            // v11: the during-walk tier evaluation (and its peak latch) moved to the per-frame check above
            // the walk logic; pushStepEscalate is retired. The ring is still fed here so w.rateW stays live
            // through the walk for the receipts (and for pushStepStaggerRate, if it is ever re-armed).
            if (liveGot) {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) {
                    WalkState& ws = it->second;
                    ws.lastGapMag = lmag;
                    ws.lastGapS   = nowS;
                    RingPush(ws, lmag, nowS, ObjectHold::PushStepRateWinS());
                }
            }
            // ★ v10.0 THE LIVE TARGET: depth × the lateral factor (the same measure the bars use),
            // mapped exactly as the engage speed was — but EVERY frame, so a push that keeps building
            // keeps raising it and one that eases lowers it. A sensor miss keeps the last target.
            if (liveGot && ObjectHold::PushStepSpeedTrack() != 0.f) {
                float latKs = 1.f;
                if (lmag > 0.01f && ObjectHold::PushStepLatTrig() != 0.f && ObjectHold::PushStepSideGain() > 1.f) {
                    const float gh = GetHeadingFromVector(RE::NiPoint3{ ldx / lmag, ldy / lmag, 0.f });
                    latKs = 1.f + (ObjectHold::PushStepSideGain() - 1.f) * std::fabs(std::sin(gh - actor->data.angle.z));
                }
                // v10.1 (user: "exponential to the push — the more I push, the faster she should start
                // walking"): target = SpeedU × ratio^exp, and the up-slew scales by the same factor.
                // *** v14 THE LEVER, live half: re-based on THE BARS THEMSELVES. Displacement from the walk
                // bar to the stumble bar maps onto the whole speed band, so the walk's speed range is exactly
                // the walk's displacement range -- and nothing can pin before she stumbles out of the walk
                // anyway. The engage lever is the FLOOR. (The old mapping, SpeedU x (disp/SpeedRefU)^exp,
                // reached the ceiling at 2.39 u, below the walk bar itself.)
                const bool leverLive = ObjectHold::PushStepTravelMode() != 0.f
                                    && ObjectHold::PushStepLever() != 0.f;
                // *** v14a: the lever's live half must read the SAME number the bars are calibrated on — the
                // TRAVEL from the animation point — NOT `lmag`, which is the LEGACY baseline-subtracted sense
                // gap (GetPushSenseGap / GetSlotPushDisplacement, just above). Mapping the legacy gap onto the
                // travel bars is the "old machinery still running on a new number" class that the v7.6 chain
                // block already produced once (report 34 s6d.1). Computed BEFORE the lock: TravelMax ->
                // TravelOf takes g_walkMx, and std::mutex is NOT recursive.
                float tmag = 0.f, tdx = 0.f, tdy = 0.f;   // v17: tdx/tdy hoisted - the steering below reads them
                if (leverLive) {
                    int tslot = -1;
                    if (!TravelMax(id, actor->data.angle.z, tdx, tdy, tmag, tslot)) { tmag = 0.f; tdx = tdy = 0.f; }
                }
                float t = 0.f, gain = 1.f;
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) {
                    if (leverLive) {
                        const float bar  = ObjectHold::PushStepTravelWalkU();
                        const float stum = ObjectHold::PushStepTravelStumbleU();
                        float ln = (stum > bar + 0.01f) ? (tmag - bar) / (stum - bar) : 0.f;
                        if (ln < 0.f) ln = 0.f;
                        if (ln > 1.f) ln = 1.f;
                        // read leverNorm from the MAP, never from the stale WalkState copy (report 33 s9.3)
                        const float useN = (std::max)(it->second.leverNorm, ln);
                        const float lo = ObjectHold::PushStepSpeedMinU(), hi = ObjectHold::PushStepSpeedMaxU();
                        t = lo + ((hi > lo ? hi - lo : 0.f) * useN);
                        gain = 0.5f + 2.0f * useN;
                    } else {
                        const float ratio = lmag * latKs / (std::max)(0.5f, ObjectHold::PushStepSpeedRefU());
                        gain = std::pow((std::max)(0.f, ratio), ObjectHold::PushStepSpeedExp());
                        t = ObjectHold::PushStepSpeedU() * gain;
                        if (t < ObjectHold::PushStepSpeedMinU()) t = ObjectHold::PushStepSpeedMinU();
                        if (t > ObjectHold::PushStepSpeedMaxU()) t = ObjectHold::PushStepSpeedMaxU();
                    }
                    it->second.liveTargetU = t; it->second.liveRatio = gain;
                    w.liveTargetU = t;          w.liveRatio = gain;
                    // *** v17 STEER THE WALK toward the LIVE push direction, rate-limited.
                    // tdx/tdy are the world-space components TravelMax already returned; they were being
                    // discarded. Slewing rather than snapping keeps her from pirouetting on sensor noise,
                    // and the MinU floor ignores the frames just after a re-capture when the reading is
                    // ~0 and its direction means nothing. Her FACING follows by the same rule she engaged
                    // with, so a front-sector push keeps walking her backwards as it swings around her.
                    if (leverLive && ObjectHold::PushStepDirTrack() != 0.f && it->second.driving) {
                        const float hlen = std::sqrt(tdx * tdx + tdy * tdy);
                        if (tmag > ObjectHold::PushStepDirTrackMinU() && hlen > 0.01f) {
                            const float nx = tdx / hlen, ny = tdy / hlen;
                            float ox = it->second.dirX, oy = it->second.dirY;
                            const float olen = std::sqrt(ox * ox + oy * oy);
                            if (olen > 0.01f) {
                                ox /= olen; oy /= olen;
                                double dtD = it->second.lastDirS > 0.0 ? nowS - it->second.lastDirS : 0.0;
                                if (dtD < 0.0) dtD = 0.0;
                                if (dtD > 0.25) dtD = 0.25;      // a hitch may not teleport the heading
                                it->second.lastDirS = nowS;
                                float ang = std::atan2(ox * ny - oy * nx, ox * nx + oy * ny);
                                const float maxA = ObjectHold::PushStepDirTrackDeg() * 0.0174533f * (float)dtD;
                                if (ang >  maxA) ang =  maxA;
                                if (ang < -maxA) ang = -maxA;
                                const float cs = std::cos(ang), sn = std::sin(ang);
                                const float rx = ox * cs - oy * sn, ry = ox * sn + oy * cs;
                                it->second.dirX = rx; it->second.dirY = ry;
                                w.dirX = rx;          w.dirY = ry;
                                if (ObjectHold::PushStepFacePush() != 0.f) {
                                    const float az = it->second.walkFaceAway
                                        ? GetHeadingFromVector(RE::NiPoint3{  rx,  ry, 0.f })
                                        : GetHeadingFromVector(RE::NiPoint3{ -rx, -ry, 0.f });
                                    it->second.angleZ = az;  w.angleZ = az;
                                }
                            } else {
                                it->second.lastDirS = nowS;
                            }
                        }
                    }
                }
            }
            if (v5live && liveGot && lmag < ObjectHold::PushStepStopU()) {
                const float cur = EnvelopeSpeed(w, nowS);
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) {
                    it->second.phase       = 1;
                    it->second.decelStartS = nowS;
                    it->second.decelSpeedU = cur;
                    it->second.capped      = false;      // a real re-push may resume
                    w = it->second;
                }
                logger::info("PUSHWALK {:08X} push released (gap {:.2f}u < {:.2f}u) — fading out "
                             "from {:.0f} u/s", id, lmag, ObjectHold::PushStepStopU(), cur);
            }
        }
        // ★ v10.0 THE FOLLOWER: the commanded speed slews toward the target at pushStepSpeedUpU
        // (rising) / pushStepSpeedDownU (falling), game u/s per second, every driving frame in BOTH
        // phases. In the fade the target is 0, so "stop as soon as the push stops" is the down-slew,
        // not a timer. dt is clamped to 0.1 s so a hitch cannot jump the speed.
        if (ObjectHold::PushStepSpeedTrack() != 0.f) {
            float logCmd = -1.f, logTgt = 0.f; int logPhase = 0;
            {
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) {
                    WalkState& ws = it->second;
                    double dt = ws.lastSpeedS > 0.0 ? nowS - ws.lastSpeedS : 0.0;
                    if (dt < 0.0) dt = 0.0;
                    if (dt > 0.1) dt = 0.1;
                    ws.lastSpeedS = nowS;
                    const float target = ws.phase == 1 ? 0.f : ws.liveTargetU;
                    // v10.1: the up-slew scales with the push (ratio^exp, floored at 0.25 so a light push
                    // still climbs); the down-slew stays a constant — letting go is letting go.
                    const float up = ObjectHold::PushStepSpeedUpU() * (std::max)(0.25f, ws.liveRatio) * (float)dt;
                    // ★★ v21 THE STOP IS A DISTANCE. Latch the target on the fade's first frame, then
                    // solve the deceleration that lands her exactly there: a = v^2 / 2d. Reset while she
                    // is driving so a resumed push re-latches from wherever she has got to.
                    if (ws.phase != 1) {
                        ws.stopDistU = 0.f; ws.stopAccelU = 0.f;
                    } else if (ws.stopDistU <= 0.f && ObjectHold::PushStepStopFrac() > 0.f) {
                        // ★★ v21a: LATCH BOTH, on the fade's first frame. v^2/2d is the CONSTANT
                        // deceleration that covers d - it must be solved from the speed she had when the
                        // push ended, once. Re-solving it every frame from the speed it has already
                        // produced makes it decay with the square of that speed, and she crawls.
                        ws.stopDistU = (std::max)(ObjectHold::PushStepStopMinU(),
                                                  walked * ObjectHold::PushStepStopFrac());
                        const float v0 = (std::max)(1.f, ws.cmdSpeedU);
                        float a = (v0 * v0) / (2.f * ws.stopDistU);
                        const float amax = ObjectHold::PushStepStopMaxA();
                        if (a > amax) a = amax;
                        if (a < 5.f)  a = 5.f;
                        ws.stopAccelU = a;
                    }
                    float dnRate = ObjectHold::PushStepSpeedDownU();
                    if (ws.phase == 1 && ws.stopAccelU > 0.f) dnRate = ws.stopAccelU;
                    const float dn = dnRate * (float)dt;
                    float d = target - ws.cmdSpeedU;
                    if (d > up) d = up; else if (d < -dn) d = -dn;
                    ws.cmdSpeedU += d;
                    if (ws.cmdSpeedU < 0.f) ws.cmdSpeedU = 0.f;
                    if (nowS - ws.lastSpeedLogS >= 1.0) {
                        ws.lastSpeedLogS = nowS;
                        logCmd = ws.cmdSpeedU; logTgt = target; logPhase = ws.phase;
                    }
                    w = ws;
                }
            }
            if (logCmd >= 0.f)
                logger::info("PUSHWALK {:08X} speed {:.0f} → target {:.0f} u/s ({}{}) — v10 live follower",
                             id, logCmd, logTgt, logPhase == 1 ? "fading" : "walking",
                             (logPhase == 1 && w.stopDistU > 0.1f)
                                 ? fmt::format(", stopping over {:.0f}u", w.stopDistU) : std::string());
        }
        // *** v18: the walk ends when the PUSH ends, not when a distance allowance runs out.
        // noCap -> the only exit is pressure going quiet for pushStepStopS, and that fade is never
        // final, so a renewed push picks the walk straight back up. The distance knobs stay parsed and
        // pushStepNoDistCap 0 restores every bit of the old budget/extend/hard-cap behaviour.
        const bool  noCapV18 = ObjectHold::PushStepNoDistCap() != 0.f;
        const bool  freshV18 = (nowS - w.lastPressureS) < stopS;
        if (w.phase == 0 && (noCapV18 ? !freshV18 : (walked >= w.budgetU))) {
            const float extendCap = w.budgetU * (std::max)(1.f, ObjectHold::PushStepExtendMul());
            const bool  fresh     = freshV18;
            if (!noCapV18 && fresh && walked < extendCap) {
                // The hand is still on her: keep walking. This replaces the old
                // stop-pause-re-engage chain — a gentle sustained push moves her WITH the player.
                if (!w.extended) {
                    std::scoped_lock lk(g_walkMx);
                    if (auto it = g_walk.find(id); it != g_walk.end()) it->second.extended = true;
                    logger::info("PUSHWALK {:08X} budget {:.0f}u reached, contact sustained — "
                                 "extending (hard cap {:.0f}u)", id, w.budgetU, extendCap);
                }
            } else {
                const float cur      = EnvelopeSpeed(w, nowS);
                // v18: with no distance cap a fade can never be "final" — only the push ending stops
                // her, and a renewed push must be able to resume the same walk.
                const bool  capFinal = !noCapV18 && (walked >= extendCap);
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) {
                    it->second.phase       = 1;
                    it->second.decelStartS = nowS;
                    it->second.decelSpeedU = cur;
                    it->second.capped      = capFinal;
                    w = it->second;
                }
                logger::info("PUSHWALK {:08X} {} ({:.0f}u walked) — fading out from {:.0f} u/s{}",
                             id, noCapV18 ? "push ended"
                                          : (capFinal ? "hard cap reached" : "budget reached, hand gone"),
                             walked, cur, capFinal ? " (final — no resume)" : "");
            }
        }
        if (w.phase == 1) {
            const float cur = EnvelopeSpeed(w, nowS);
            // v10.0: under speed tracking the down-slew ends the walk (cur reaches the exit speed by
            // construction); the timer is only a 3 s backstop. v6 keeps its timed fade.
            const float fadeT = ObjectHold::PushStepSpeedTrack() != 0.f ? 3.0f : ObjectHold::PushStepRampOutS();
            if (cur <= ObjectHold::PushStepExitSpeedU() ||
                nowS - w.decelStartS >= (double)fadeT) {
                PlannerCtl(mc)->ClearPlannerDirectControl();
                if (auto* task = SKSE::GetTaskInterface())   // deferred — see the freeze note above
                    task->AddTask([id]() {
                        if (auto* f = RE::TESForm::LookupByID(id))
                            if (auto* a = f->As<RE::Actor>()) a->EvaluatePackage(false, false);
                    });
                logger::info("PUSHWALK {:08X} stopped (fade complete; {:.0f}u walked total)", id, walked);
                std::scoped_lock lk(g_walkMx);
                if (auto it = g_walk.find(id); it != g_walk.end()) {
                    it->second.driving         = false;
                    it->second.phase           = 0;
                    it->second.capped          = false;
                    it->second.reengageAfterS  = nowS + (double)ObjectHold::PushStepRefracS();
                    it->second.lastEpisodeEndS = nowS;
                }
                return false;
            }
        }
    }

    // ── feed (PLANCK main.cpp:6747-6757) — locked direction + locked facing, no wander ────
    StampDriveS(id, nowS);          // v4: the settle gate reads this even after a full release
    RE::NiPoint3 dir{ w.dirX, w.dirY, 0.f };
    const float heading = GetHeadingFromVector(dir);
    PlannerCtl(mc)->SetTargetSpeed(NormSpeedFor(actor, EnvelopeSpeed(w, nowS)));   // v7.2: NORMALIZED (the unit fix)
    PlannerCtl(mc)->SetTargetDirection(RE::NiPoint3{ 0.f, 0.f, heading });
    RE::NiPoint3 lockedAngle = actor->data.angle;
    lockedAngle.z = w.angleZ;
    PlannerCtl(mc)->SetTargetAngle(lockedAngle);         // she keeps her engage-time facing
    return true;
}

}   // namespace PushStep
