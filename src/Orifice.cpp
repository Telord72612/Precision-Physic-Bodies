// ─────────────────────────────────────────────────────────────────────────────
// Orifice.cpp — the NATIVE ORIFICE DRIVE (2026-08-19). See Orifice.h for the
// contract and for why an absolute (rest + offset) write is the only safe form.
//
// THE PIPELINE, one actor, one frame:
//
//   probes ──► PENETRATION GATE ──► depth level + intruder radius ──► openness
//   (PpbApi)   (the ellipse test)    (the pelvic sensor ladder)        (ease)
//                                                                        │
//                                            bone ring ◄─ rest + dir·U ◄─┘
//
// ★ THE PENETRATION GATE is the correctness core, and it is NOT a proximity
//   test. Every sensor level is a PAIR of capsules straddling the centreline.
//   A contact with ONE of a pair is AMBIGUOUS: an object that clipped through
//   the mesh can graze C22 from the OUTSIDE and read "deep". Being BETWEEN the
//   twins is unambiguous proof of being inside the cavity, so the gate is the
//   scale-proof ellipse sum
//
//        distL + distR  <=  separation * (1 + orificeGateTol)
//
//   (a point between two axes sums to ~the separation; a point outside the pair
//   sums to much more, and the sum grows in EVERY direction — off-axis, and off
//   the ends too, because the distances are to the clamped segments). It needs
//   no absolute units, so it survives ReScale/ReShape and every body morph: the
//   separation is measured live from the same two capsules every frame.
//
// ARBITRATION with Penetration Physics (PPA) is BY MEASUREMENT, not by ABI.
//   PPA drives these exact bones during a scene. We do not blind-call its
//   AccuratePenetration_GetAPI_V1 listener: guessing an unpublished struct
//   layout to call through is precisely how this project has earned CTDs, and
//   there is a signal that costs nothing and cannot be wrong — the bones
//   themselves. Nothing in the behaviour graph poses them, so if a bone holds a
//   value we did not write, and it is not the rest value either, ANOTHER WRITER
//   OWNS IT. We then stand down and, critically, do NOT restore (restoring
//   would fight PPA's pose). Ownership returns only after the bones have been
//   completely still for orificeForeignHoldS — at which point the value they
//   are sitting at is a legitimate rest to capture. On top of that the OStim /
//   SexLab scene flag PPB already maintains (HandBox::IsSceneSuspended) is a
//   hard gate, so the common case never even reaches the measurement.
//
// COST. Two cheap culls (probes live at all, then a probe within orificeRangeU
// of her PELVIS — see PelvisWorldU; measuring from actor->GetPosition() puts the
// anchor at her FEET, ~67u below the sensors) mean the 10 capsule reads only
// happen when a hand/weapon/object
// is actually at her pelvis. Sensing runs on the pre-drive seam; the EASE runs
// every frame regardless, so a ~4 Hz probe refresh (apiHz) never reads as steps.
// Nothing here touches a collision-thread callback.
// ─────────────────────────────────────────────────────────────────────────────

#include "Orifice.h"
#include "CapFix.h"     // GrabDiag::ReadCapsuleWorldU — live sensor capsule geometry
#include "HandBox.h"    // HandBox::IsSceneSuspended — the OStim/SexLab scene flag
#include "PpbApi.h"     // PpbApi::CopyProbes — the ALREADY-ASSEMBLED probe set
#include "Tuning.h"     // ObjectHold::Orifice* knobs

#include <Windows.h>    // GetModuleHandleW — PPA presence, for ONE log line (never called into)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace logger = SKSE::log;

namespace {

    // ── bone rings ───────────────────────────────────────────────────────────
    // Node name + the direction the node's LOCAL translation is pushed along, read
    // from the live PPA toml (accurate-penetration.toml) so both mods deform the same
    // rings the same way. The vectors are kept RAW (they are not unit length — e.g.
    // VaginaB1 is 0.79 long, the Anus2 tops 0.86): PPA multiplies the raw vector by
    // the openness distance and its numbers are dialled against that, so normalising
    // them here would silently change every gape.
    struct BoneDef { const char* node; float dir[3]; };

    constexpr int kRingBones = 4;

    const BoneDef kVaginalRing[kRingBones] = {
        { "NPC L Pussy02", { -1.000f,  0.000f,  0.000f } },
        { "NPC R Pussy02", {  1.000f,  0.000f,  0.000f } },
        { "VaginaB1",      {  0.000f, -0.750f,  0.250f } },   // back wall
        { "Clitoral1",     {  0.000f,  0.125f, -0.250f } },   // top
    };
    const BoneDef kAnalRing[kRingBones] = {
        { "NPC LB Anus2",  {  0.000f,  1.000f,  0.000f } },
        { "NPC RB Anus2",  {  0.000f, -1.000f,  0.000f } },
        { "NPC RT Anus2",  {  0.800f,  0.000f,  0.320f } },
        { "NPC LT Anus2",  { -0.800f,  0.000f, -0.320f } },
    };
    // MALE (PPB/sculpt.nif) carries the 4 Anus2 bones and NONE of the 4 vaginal ones.
    // That is handled by EXISTENCE — the lookup below simply fails and the channel
    // parks itself — exactly like the per-race capsule children. No gender gate.

    // ── the pelvic sensor ladder (COM = slot 11, measured, female PPB_tuning.txt) ──
    // Shallow -> deep, each level an L/R pair straddling the centreline. The two chains
    // are DISTINCT in Y (vaginal mid -0.5..-0.9, anal mid -5.2..-5.7, 4.30u apart, no
    // overlap), so a probe can never be "in" both at once by accident.
    constexpr int kSlotCom       = 11;
    constexpr int kVagLevels     = 3;
    constexpr int kAnalLevels    = 2;
    const int kVagLadder[kVagLevels][2]   = { { 22, 23 }, { 24, 25 }, { 26, 27 } };
    const int kAnalLadder[kAnalLevels][2] = { { 28, 29 }, { 30, 31 } };

    constexpr int kMaxProbes = 14;     // 2 wands x (4 boxes + weapon + object) + the 2 PLAYER
                                       // GENITAL WAND segments (2026-08-23). The wand was always
                                       // a real body — it pushes loose objects — but it lived
                                       // outside the probe export, so this gate could not see the
                                       // one intruder it most obviously exists for.
    constexpr float kRestEpsU   = 0.01f;   // below this the channel counts as CLOSED
    constexpr float kSameEpsU   = 0.002f;  // node-translation equality (foreign-write test)

    // ── STALE SWEEP. OnPreDrive is the ONLY place an actor is eased shut, and it is reached
    //    through a chain of per-actor gates (DismemberGuard exclusion, the driven set itself).
    //    An actor that stops arriving while a ring is armed would keep her offset for the life
    //    of the 3D, so a seam that runs regardless of every per-actor gate (Orifice::Sweep, from
    //    the HIGGS frame callback) drops anything we have not seen for kStaleMs — after putting
    //    rest back. Both seams are the MAIN thread (NpcFingerTest.cpp:1097 / :2803), so g_state
    //    stays lock-free.
    constexpr std::uint64_t kStaleMs      = 500;
    constexpr std::uint64_t kSweepEveryMs = 250;

    // ── helpers ──────────────────────────────────────────────────────────────
    inline std::uint64_t NowMs() {
        return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    RE::NiAVObject* FindNode(RE::Actor* actor, const char* name) {
        if (!actor || !name) return nullptr;
        auto* root = actor->Get3D();
        if (!root) return nullptr;
        return root->GetObjectByName(RE::BSFixedString(name));
    }

    // ── THE CULL ANCHOR: the PELVIS, in world game units ─────────────────────
    // NOT actor->GetPosition(). The ref origin sits at the FEET; the sensor ladder is a set of
    // COM children ~67-69u ABOVE it (Report 03: COM bind 68.911, live model-space ~66.85), so a
    // cull of a few tens of units taken from the origin rejects a probe that is literally inside
    // her — the radius would have to exceed her HEIGHT to guard a target at her pelvis. Measured
    // from the COM body itself (slot 11 child 0 = the main capsule), which is the very body every
    // ladder capsule is a child of, so the cull and the sensors can never disagree about where
    // the target is. Falls back to the COM node, then reports failure (the caller then senses
    // nothing and eases every armed ring shut, which is the safe direction).
    bool PelvisWorldU(RE::Actor* a, float out[3])
    {
        float pa[3], pb[3], r;
        if (GrabDiag::ReadCapsuleWorldU(a, kSlotCom, 0, pa, pb, &r)) {
            out[0] = (pa[0] + pb[0]) * 0.5f;
            out[1] = (pa[1] + pb[1]) * 0.5f;
            out[2] = (pa[2] + pb[2]) * 0.5f;
            return true;
        }
        if (auto* nd = FindNode(a, "NPC COM [COM ]")) {
            out[0] = nd->world.translate.x;
            out[1] = nd->world.translate.y;
            out[2] = nd->world.translate.z;
            return true;
        }
        return false;
    }

    // Distance from p to the CLAMPED segment a->b (the capsule AXIS, not its surface —
    // the ellipse gate's foci are the axes).
    inline float SegPointDistU(const float a[3], const float b[3], const float p[3]) {
        const float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float ap[3] = { p[0]-a[0], p[1]-a[1], p[2]-a[2] };
        const float len2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
        float t = len2 > 1e-8f ? (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / len2 : 0.f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        const float dx = p[0]-(a[0]+ab[0]*t), dy = p[1]-(a[1]+ab[1]*t), dz = p[2]-(a[2]+ab[2]*t);
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    // The point of segment a->b nearest to `to`. A segment probe (weapon blade, the
    // 2026-08-19 held-object bound-box segment) is reduced to ITS point nearest the
    // cavity centre — i.e. the most-inserted part of it, which is what "how deep" means.
    inline void ClosestOnSeg(const float a[3], const float b[3], const float to[3], float out[3]) {
        const float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float ap[3] = { to[0]-a[0], to[1]-a[1], to[2]-a[2] };
        const float len2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
        float t = len2 > 1e-8f ? (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / len2 : 0.f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        out[0] = a[0] + ab[0]*t; out[1] = a[1] + ab[1]*t; out[2] = a[2] + ab[2]*t;
    }

    inline bool NearV(const RE::NiPoint3& v, const float f[3]) {
        return std::fabs(v.x - f[0]) < kSameEpsU &&
               std::fabs(v.y - f[1]) < kSameEpsU &&
               std::fabs(v.z - f[2]) < kSameEpsU;
    }

    // ── per-actor state ──────────────────────────────────────────────────────
    // ONE channel per bone ring. `rest` is the captured pose we are obliged to put back;
    // `wrote` is what we last put there, and is the whole foreign-write test.
    struct Chan {
        bool  armed   = false;   // rest captured — we own these bones and MUST restore them
        bool  foreign = false;   // another writer (PPA) owns them: stand down, do NOT restore
        bool  missing = false;   // ring absent on this skeleton (male vaginal) — parked
        bool  loggedForeign = false;
        float curU   = 0.f;      // eased openness distance, game units
        float rest [kRingBones][3]{};
        float wrote[kRingBones][3]{};
        float seen [kRingBones][3]{};   // foreign-release stillness watch
        std::uint64_t stillSinceMs = 0;
        int   lastLevel = -1;    // for the log receipt / edge detection
    };

    struct ActorState {
        Chan  ring[2];               // [kVaginal] [kAnal]
        float oralU = 0.f;           // eased 0..1 — oral has NO bones (see NoteMouthStage)
        float oralTarget = 0.f;
        std::uint64_t oralNoteMs = 0;
        std::uint64_t lastLogMs = 0;
        std::uint64_t lastSeenMs = 0;   // last OnPreDrive for this actor — the stale sweep's clock
    };
    std::unordered_map<std::uint32_t, ActorState> g_state;

    // The "no ring on this skeleton" one-shot, keyed (FormID << 2 | kind). It CANNOT live in
    // ActorState: that entry is created and destroyed around the frame, so a per-entry guard
    // re-fires at frame rate for every male / child / creature inside the cull — and main.cpp
    // sets flush_on(info), making each one a synchronous file flush from the drive seam.
    std::unordered_set<std::uint64_t> g_parkedLogged;

    const BoneDef* RingOf(int kind) { return kind == Orifice::kAnal ? kAnalRing : kVaginalRing; }
    const char*    KindName(int kind) {
        return kind == Orifice::kVaginal ? "vaginal" : kind == Orifice::kAnal ? "anal" : "oral";
    }

    // ── restore ──────────────────────────────────────────────────────────────
    // The one obligation of this module. Writes rest back on every bone we armed and
    // disarms. `foreign` channels are deliberately skipped: their current pose belongs
    // to PPA and our captured rest is stale.
    void RestoreChan(RE::Actor* a, Chan& c, int kind)
    {
        if (!c.armed) { c.curU = 0.f; return; }
        const BoneDef* ring = RingOf(kind);
        for (int i = 0; i < kRingBones; ++i) {
            if (auto* nd = FindNode(a, ring[i].node)) {
                nd->local.translate.x = c.rest[i][0];
                nd->local.translate.y = c.rest[i][1];
                nd->local.translate.z = c.rest[i][2];
                c.wrote[i][0] = c.rest[i][0];
                c.wrote[i][1] = c.rest[i][1];
                c.wrote[i][2] = c.rest[i][2];
            }
        }
        c.armed = false; c.curU = 0.f; c.lastLevel = -1;
    }

    void RestoreActor(RE::Actor* a, ActorState& st)
    {
        for (int k = 0; k < 2; ++k) RestoreChan(a, st.ring[k], k);
        st.oralU = st.oralTarget = 0.f;
    }

    // Knob-off / scene-handoff / load teardown. Resolves every held actor FRESH from its
    // FormID (a stored Actor* must never survive a frame boundary here) and puts the rest
    // pose back before the map is dropped.
    void RestoreAllAndClear(const char* why)
    {
        if (g_state.empty()) return;
        int n = 0;
        for (auto& [id, st] : g_state) {
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) { RestoreActor(a, st); ++n; }
        }
        logger::info("ORIFICE: released {} actor(s) ({}) — every armed bone ring restored to its "
                     "captured rest before the map was dropped", n, why);
        g_state.clear();
    }

    // ── THE PENETRATION GATE ────────────────────────────────────────────────
    // Deepest ladder level any probe is unambiguously INSIDE, plus that probe's radius.
    // Returns -1 = nothing inside.
    int SenseLadder(RE::Actor* a, const int (*ladder)[2], int nLevels,
                    const PpbApi::ProbeView* pv, int nP, float tol, float fingerRadU,
                    float* radOut)
    {
        int   best = -1;
        float bestRad = 0.f;
        for (int lvl = 0; lvl < nLevels; ++lvl) {
            float aL[3], bL[3], rL, aR[3], bR[3], rR;
            if (!GrabDiag::ReadCapsuleWorldU(a, kSlotCom, ladder[lvl][0], aL, bL, &rL)) continue;
            if (!GrabDiag::ReadCapsuleWorldU(a, kSlotCom, ladder[lvl][1], aR, bR, &rR)) continue;
            const float midL[3] = { (aL[0]+bL[0])*0.5f, (aL[1]+bL[1])*0.5f, (aL[2]+bL[2])*0.5f };
            const float midR[3] = { (aR[0]+bR[0])*0.5f, (aR[1]+bR[1])*0.5f, (aR[2]+bR[2])*0.5f };
            const float sx = midL[0]-midR[0], sy = midL[1]-midR[1], sz = midL[2]-midR[2];
            const float sep = std::sqrt(sx*sx + sy*sy + sz*sz);
            if (!(sep > 0.05f)) continue;              // degenerate pair (NaN-safe form)
            const float limit = sep * (1.f + tol);
            const float cav[3] = { (midL[0]+midR[0])*0.5f, (midL[1]+midR[1])*0.5f,
                                   (midL[2]+midR[2])*0.5f };
            for (int i = 0; i < nP; ++i) {
                const PpbApi::ProbeView& p = pv[i];
                float pt[3];
                if (p.seg) ClosestOnSeg(p.p, p.q, cav, pt);
                else { pt[0] = p.p[0]; pt[1] = p.p[1]; pt[2] = p.p[2]; }
                const float dL = SegPointDistU(aL, bL, pt);
                const float dR = SegPointDistU(aR, bR, pt);
                if (dL + dR > limit) continue;         // outside the ellipsoid = not inside her
                // The intruder's RADIUS. Hand boxes carry no pad (they ARE the finger), so they
                // use the fingertip knob; a weapon/object carries its own measured radius, which
                // is why a fingertip and a 3u plug can now produce different gapes at the same
                // depth — the thing PPA cannot express, because it only has depth.
                const float rad = p.pad > 0.f ? p.pad : fingerRadU;
                if (lvl > best) { best = lvl; bestRad = rad; }
                else if (lvl == best && rad > bestRad) bestRad = rad;
            }
        }
        if (radOut) *radOut = bestRad;
        return best;
    }

    // ── the drive ────────────────────────────────────────────────────────────
    // Absolute write, ALWAYS: local.translate = rest + dir * curU. Never accumulated.
    void DriveChan(RE::Actor* a, ActorState& st, int kind, float targetU, float dt,
                   std::uint64_t now)
    {
        Chan& c = st.ring[kind];
        if (c.missing) return;          // parked — and `missing` now SURVIVES the frame (see the
                                        // erase condition in OnPreDrive), so this is one lookup
                                        // per actor per 3D, not four per frame forever.
        // Nothing held, nothing to watch, nothing to close: leave BEFORE the node lookups. Without
        // this, every actor inside the cull paid 8 GetObjectByName tree searches every frame just
        // to discover there was nothing to do.
        if (!c.armed && !c.foreign && !(targetU > 0.f) && c.curU <= kRestEpsU) return;

        const BoneDef*  ring = RingOf(kind);
        RE::NiAVObject* nd[kRingBones]{};
        for (int i = 0; i < kRingBones; ++i) {
            nd[i] = FindNode(a, ring[i].node);
            if (!nd[i]) {
                // By EXISTENCE: no ring, no channel. Male vaginal lands here every time.
                c.missing = true;
                // The negative result is logged from a PERSISTENT set (and only with orificeLog
                // on): a guard living on the map entry re-fired every frame for every uncovered
                // actor in range.
                if (ObjectHold::OrificeLogEnabled()) {
                    const std::uint64_t key =
                        ((std::uint64_t)a->GetFormID() << 2) | (std::uint64_t)(unsigned)kind;
                    if (g_parkedLogged.insert(key).second)
                        logger::info("ORIFICE {:08X}: no {} ring on this skeleton (missing '{}') — "
                                     "channel parked for this actor",
                                     a->GetFormID(), KindName(kind), ring[i].node);
                }
                return;
            }
        }

        // ── FOREIGN-WRITE ARBITRATION (see the file header) ──────────────────
        if (c.foreign) {
            bool still = true;
            for (int i = 0; i < kRingBones; ++i) {
                if (!NearV(nd[i]->local.translate, c.seen[i])) still = false;
                c.seen[i][0] = nd[i]->local.translate.x;
                c.seen[i][1] = nd[i]->local.translate.y;
                c.seen[i][2] = nd[i]->local.translate.z;
            }
            if (!still) { c.stillSinceMs = now; return; }
            const std::uint64_t holdMs = (std::uint64_t)(ObjectHold::OrificeForeignHoldS() * 1000.f);
            if (now - c.stillSinceMs < holdMs) return;
            // Nobody has moved these bones for the hold window: whatever they sit at now IS a
            // legitimate rest, so we may take them back (and re-capture below).
            c.foreign = false; c.loggedForeign = false;
            logger::info("ORIFICE {:08X}: {} ring still for {:.1f}s — ownership reclaimed",
                         a->GetFormID(), KindName(kind), ObjectHold::OrificeForeignHoldS());
        }
        if (c.armed) {
            bool foreign = false, reverted = false;
            for (int i = 0; i < kRingBones; ++i) {
                if (NearV(nd[i]->local.translate, c.wrote[i])) continue;
                // Not what we wrote. Two possibilities, and they are distinguishable:
                //   back at REST  -> the 3D was rebuilt (cell change, race swap) and the node
                //                    is fresh: re-arm cleanly, nothing was stolen.
                //   anything else -> ANOTHER writer owns the bone. Stand down and do NOT
                //                    restore; our rest is now stale against their pose.
                if (NearV(nd[i]->local.translate, c.rest[i])) reverted = true;
                else { foreign = true; break; }
            }
            if (foreign) {
                // ★ THE RESTORE OBLIGATION SURVIVES THE STAND-DOWN. "Do not restore" means "do
                //   not fight their pose" — it never meant "walk away still wearing our offset".
                //   A bone that STILL holds exactly what we wrote is, by definition, one the
                //   other writer has not touched: putting rest back there REMOVES OUR
                //   CONTRIBUTION and touches nothing of theirs. Without this, the members of the
                //   ring the foreign writer never poked stayed at rest+offset for the life of the
                //   3D (RestoreChan bails on !armed, so every teardown path skipped them), and
                //   the reclaim path then captured that deformed pose as the NEW rest — a
                //   permanent gape that a correct future release would restore her TO.
                for (int i = 0; i < kRingBones; ++i) {
                    if (!NearV(nd[i]->local.translate, c.wrote[i])) continue;   // theirs now
                    nd[i]->local.translate.x = c.rest[i][0];
                    nd[i]->local.translate.y = c.rest[i][1];
                    nd[i]->local.translate.z = c.rest[i][2];
                    c.wrote[i][0] = c.rest[i][0];
                    c.wrote[i][1] = c.rest[i][1];
                    c.wrote[i][2] = c.rest[i][2];
                }
                c.armed = false; c.curU = 0.f; c.lastLevel = -1;
                c.foreign = true; c.stillSinceMs = now;
                for (int i = 0; i < kRingBones; ++i) {
                    c.seen[i][0] = nd[i]->local.translate.x;
                    c.seen[i][1] = nd[i]->local.translate.y;
                    c.seen[i][2] = nd[i]->local.translate.z;
                }
                if (!c.loggedForeign) {
                    c.loggedForeign = true;
                    logger::info("ORIFICE {:08X}: {} ring is being driven by ANOTHER writer "
                                 "(Penetration Physics?) — standing down WITHOUT restoring "
                                 "(restoring would fight their pose)",
                                 a->GetFormID(), KindName(kind));
                }
                return;
            }
            if (reverted) { c.armed = false; c.curU = 0.f; c.lastLevel = -1; }
        }

        // ── arm (capture rest) ───────────────────────────────────────────────
        if (!c.armed) {
            if (!(targetU > 0.f) && c.curU <= kRestEpsU) return;   // nothing to do, stay out
            for (int i = 0; i < kRingBones; ++i) {
                c.rest[i][0] = nd[i]->local.translate.x;
                c.rest[i][1] = nd[i]->local.translate.y;
                c.rest[i][2] = nd[i]->local.translate.z;
            }
            c.armed = true; c.curU = 0.f;
        }

        // ── ease ─────────────────────────────────────────────────────────────
        // The SENSE refreshes at the touch API's rate (apiHz, ~4 Hz by design); the ease
        // runs at frame rate, which is what stops the gape reading as steps.
        float k = ObjectHold::OrificeEaseHz() * dt;
        if (!(k > 0.f)) k = 0.f;                    // NaN-safe
        if (k > 1.f) k = 1.f;
        c.curU += (targetU - c.curU) * k;
        if (!(targetU > 0.f) && c.curU < kRestEpsU) c.curU = 0.f;

        // ── write (ABSOLUTE) ─────────────────────────────────────────────────
        for (int i = 0; i < kRingBones; ++i) {
            const float tx = c.rest[i][0] + ring[i].dir[0] * c.curU;
            const float ty = c.rest[i][1] + ring[i].dir[1] * c.curU;
            const float tz = c.rest[i][2] + ring[i].dir[2] * c.curU;
            nd[i]->local.translate.x = tx;
            nd[i]->local.translate.y = ty;
            nd[i]->local.translate.z = tz;
            c.wrote[i][0] = tx; c.wrote[i][1] = ty; c.wrote[i][2] = tz;
        }
        // curU hit zero: the write above WAS the restore. Disarm so a later contact
        // re-captures rest fresh (her body may have been re-shaped in between).
        if (c.curU == 0.f) { c.armed = false; c.lastLevel = -1; }
    }

}   // anonymous namespace

namespace Orifice {

    void OnPreDrive(RE::Actor* actor, float deltaTime)
    {
        if (!actor) return;

        // ── master gate. KNOB-OFF = UNDO, NOT STOP (the PPB idiom): a flip to 0 must put
        //    every held ring back, not merely stop updating it.
        if (!ObjectHold::OrificeEnabled()) {
            RestoreAllAndClear("orificeEnable 0");
            return;
        }

        // One-shot environment line: is PPA even installed? PRESENCE ONLY — we never call
        // into AccuratePenetration_GetAPI_V1 (see the file header: the arbitration is by
        // measurement, so no unpublished ABI is on our crash path).
        static bool s_envLogged = false;
        if (!s_envLogged) {
            s_envLogged = true;
            logger::info("ORIFICE armed: vaginal={} anal={} oral={} | gateTol={:.2f} easeHz={:.1f} "
                         "radiusGain={:.2f} | Penetration Physics {} — arbitration is by MEASURED "
                         "bone ownership, no cross-plugin call",
                         ObjectHold::OrificeVaginal(), ObjectHold::OrificeAnal(),
                         ObjectHold::OrificeOral(), ObjectHold::OrificeGateTol(),
                         ObjectHold::OrificeEaseHz(), ObjectHold::OrificeRadiusGain(),
                         GetModuleHandleW(L"AccuratePenetration.dll") ? "PRESENT" : "absent");
        }

        const std::uint32_t id = actor->GetFormID();
        const std::uint64_t now = NowMs();

        // ── SCENE HANDOFF. While an OStim/SexLab scene runs, PPA owns these bones. Restore
        //    and stand down COMPLETELY — including dropping the entry, so the next contact
        //    after the scene re-captures rest from whatever PPA left behind.
        if (HandBox::IsSceneSuspended()) {
            auto it = g_state.find(id);
            if (it != g_state.end()) {
                RestoreActor(actor, it->second);
                g_state.erase(it);
                logger::info("ORIFICE {:08X}: scene started — restored and handed the rings off",
                             id);
            }
            return;
        }
        if (!actor->Get3D()) {                       // unloaded: no nodes to restore or hold
            g_state.erase(id);
            return;
        }

        // ── probes. These are the SAME probes the touch API assembles (hand boxes / HIGGS
        //    weapon segment / the 2026-08-19 held-object bound-box segment) — reused, never
        //    re-derived, so a fix to probe assembly fixes this module too.
        PpbApi::ProbeView pv[kMaxProbes];
        int nP = PpbApi::CopyProbes(pv, kMaxProbes);

        // ── THE GRAB MUTE, mirrored from PpbApi::ScanActor (PpbApi.cpp:891-892) ──────────────
        //    A hand HIGGS-grabbing THIS actor is HOLDING her, not wielding at her: the weapon
        //    body rides that grip wherever the grab goes, so its "contacts" on HER are pure
        //    phantoms (the recorded case, PpbApi.cpp:131-137 — an axe in the leg-holding hand
        //    reported cervix -16.9u for 13.6s while the OTHER hand did the touching). The mute is
        //    per-TARGET, so CopyProbes cannot apply it and the consumer must. It matters far more
        //    here than in the scan: a phantom does not merely mis-report a contact any more, it
        //    physically deforms her.
        {
            int keep = 0;
            for (int i = 0; i < nP; ++i) {
                if (pv[i].cls == 1 /* kClsWeapon */ && pv[i].grabActorId == id) continue;
                if (keep != i) pv[keep] = pv[i];
                ++keep;
            }
            nP = keep;
        }

        // cull 1: nothing in either hand is live -> everything eases shut.
        // cull 2: no probe within orificeRangeU of her PELVIS -> same. Only past BOTH do we
        //         pay for the 10 live capsule reads.  ★ The anchor is the pelvis, NOT
        //         actor->GetPosition(): see PelvisWorldU — the ref origin is at the feet, ~67u
        //         below the sensors this cull is guarding.
        bool inRange = false;
        float pelvis[3];
        const bool havePelvis = PelvisWorldU(actor, pelvis);
        if (nP > 0 && havePelvis) {
            const float rng = ObjectHold::OrificeRangeU();
            const float r2 = rng * rng;
            for (int i = 0; i < nP && !inRange; ++i) {
                const float* pts[2] = { pv[i].p, pv[i].q };
                for (int e = 0; e < (pv[i].seg ? 2 : 1); ++e) {
                    const float dx = pts[e][0]-pelvis[0], dy = pts[e][1]-pelvis[1],
                                dz = pts[e][2]-pelvis[2];
                    if (dx*dx + dy*dy + dz*dz < r2) { inRange = true; break; }
                }
            }
        }
        // A SILENT cull is how the whole feature can read as "the gate never passes". With
        // orificeLog on, say so — throttled globally, so a crowd cannot turn it into spam.
        if (ObjectHold::OrificeLogEnabled() && nP > 0 && !inRange) {
            static std::uint64_t s_farLogMs = 0;
            if (now - s_farLogMs > 2000) {
                s_farLogMs = now;
                logger::info("ORIFICE {:08X}: {} probe(s) live, none within {:.0f}u of the PELVIS{}"
                             " — nothing sensed",
                             id, nP, ObjectHold::OrificeRangeU(),
                             havePelvis ? "" : " (COM body unreadable — actor not PPB-rigged?)");
            }
        }

        auto it = g_state.find(id);
        if (!inRange && it == g_state.end()) return;        // never touched, nothing armed: free
        ActorState& st = (it != g_state.end()) ? it->second : g_state[id];
        st.lastSeenMs = now;                                // the stale sweep's proof of life

        const float tol   = ObjectHold::OrificeGateTol();
        const float fRad  = ObjectHold::OrificeFingerRadU();
        const float gain  = ObjectHold::OrificeRadiusGain();
        const float refR  = ObjectHold::OrificeRefRadU();

        // ── openness target, per ring ────────────────────────────────────────
        // OPENNESS = f(depth, intruder radius). depthT is the deepest ladder level that
        // passed the gate, normalised; radT is the intruder radius against orificeRefRadU.
        // orificeRadiusGain blends them: 0 = depth only (PPA's behaviour exactly),
        // 1 = radius only. The result rides between the ring's Scale and ScaleMax.
        float tgt[2] = { 0.f, 0.f };
        int   lvl[2] = { -1, -1 };
        float rad[2] = { 0.f, 0.f };
        const bool want[2] = { ObjectHold::OrificeVaginal(), ObjectHold::OrificeAnal() };
        for (int k = 0; k < 2; ++k) {
            if (!inRange || !want[k] || nP <= 0) continue;   // knob off -> target 0 -> eases shut
            const int nLv = (k == kVaginal) ? kVagLevels : kAnalLevels;
            lvl[k] = SenseLadder(actor, (k == kVaginal) ? kVagLadder : kAnalLadder, nLv,
                                 pv, nP, tol, fRad, &rad[k]);
            if (lvl[k] < 0) continue;
            const float minU = ObjectHold::OrificeOpenMinU(k);
            const float maxU = ObjectHold::OrificeOpenMaxU(k);
            const float depthT = (float)(lvl[k] + 1) / (float)nLv;
            float radT = refR > 0.01f ? rad[k] / refR : 0.f;
            if (!(radT > 0.f)) radT = 0.f;                // NaN-safe
            if (radT > 1.f)    radT = 1.f;
            float t = depthT * (1.f - gain) + radT * gain;
            if (!(t > 0.f)) t = 0.f;
            if (t > 1.f)    t = 1.f;
            tgt[k] = minU + (maxU - minU) * t;            // clamped to [Scale, ScaleMax]
        }

        // ── drive ────────────────────────────────────────────────────────────
        float dt = deltaTime;
        if (!(dt > 0.f)) dt = 1.f / 90.f;                 // NaN/zero-safe (VR nominal)
        if (dt > 0.1f)   dt = 0.1f;
        for (int k = 0; k < 2; ++k) DriveChan(actor, st, k, tgt[k], dt, now);

        // ── ORAL. No bones: the mouth is a POSE problem, not a ring problem, and PPB's
        //    GhostTouchFrame already owns the phoneme/MFG ramp. This channel exists only
        //    to turn the mouth gate's stages into the same published 0..1 openness the
        //    other two carry, so a consumer reads one API for all three.
        if (!ObjectHold::OrificeOral()) st.oralTarget = 0.f;
        // Watchdog: the gate is EDGE-driven, so a lost EXIT edge (actor swap in
        // GhostTouchFrame's single-target tracker) would leave her reading "open" forever.
        if (st.oralTarget > 0.f && now - st.oralNoteMs > 30000) st.oralTarget = 0.f;
        {
            float k = ObjectHold::OrificeEaseHz() * dt;
            if (!(k > 0.f)) k = 0.f;
            if (k > 1.f) k = 1.f;
            st.oralU += (st.oralTarget - st.oralU) * k;
            if (st.oralTarget <= 0.f && st.oralU < 0.01f) st.oralU = 0.f;
        }

        // ── receipt ──────────────────────────────────────────────────────────
        if (ObjectHold::OrificeLogEnabled()) {
            for (int k = 0; k < 2; ++k) {
                if (lvl[k] != st.ring[k].lastLevel) {
                    st.ring[k].lastLevel = lvl[k];
                    logger::info("ORIFICE {:08X} {} level {} -> target {:.2f}u (radius {:.2f}u, "
                                 "gateTol {:.2f})", id, KindName(k), lvl[k], tgt[k], rad[k], tol);
                }
            }
            if (now - st.lastLogMs > 1000 &&
                (st.ring[0].curU > 0.f || st.ring[1].curU > 0.f || st.oralU > 0.f)) {
                st.lastLogMs = now;
                logger::info("ORIFICE {:08X} open: vaginal {:.2f}u anal {:.2f}u oral {:.2f} "
                             "(probes {})", id, st.ring[0].curU, st.ring[1].curU, st.oralU, nP);
            }
        }

        // Fully closed on every channel, nothing armed, AND no latch that has to survive the
        // frame. `foreign` and `missing` are STATE, not scratch: erasing on `armed` alone threw
        // away foreign/stillSinceMs/seen[]/rest[] on the very frame the arbitration set them, so
        // the next frame rebuilt a virgin entry, captured the OTHER writer's mid-animation pose
        // as "rest", and wrote our offset on top of it — every frame, forever. It also threw away
        // `missing`, which is what made the parked-channel path re-run its node lookups (and its
        // one-shot log) at frame rate for every uncovered actor in range.
        const Chan& r0 = st.ring[0];
        const Chan& r1 = st.ring[1];
        if (!r0.armed && !r1.armed && !r0.foreign && !r1.foreign && !r0.missing && !r1.missing &&
            st.oralU == 0.f && st.oralTarget == 0.f)
            g_state.erase(id);
    }

    void NoteMouthStage(std::uint32_t actorId, int stage, bool entered)
    {
        if (!ObjectHold::OrificeEnabled() || !ObjectHold::OrificeOral()) return;
        if (stage < 0 || stage > 2 || !actorId) return;
        // The gate's own ladder, collapsed to an amplitude: lips brushed / in the mouth /
        // at the back of the throat. A FALL on the deeper stage drops back to the shallower
        // one rather than to zero, because the mouth is still open when the finger retreats
        // from the throat to the middle — the same asymmetry the gate's deep-hold encodes.
        static const float kAmp[3] = { 0.25f, 0.70f, 1.00f };
        // An EXIT edge for an actor we are not already tracking must not create a map entry —
        // otherwise every mouth release on a never-touched NPC leaks one (the mouth gate runs
        // independently of anything this module started).
        if (!entered && g_state.find(actorId) == g_state.end()) return;
        auto& st = g_state[actorId];
        st.oralNoteMs = NowMs();
        st.lastSeenMs = st.oralNoteMs;   // a mouth-created entry must not read as stale at birth
        if (entered) {
            if (kAmp[stage] > st.oralTarget) st.oralTarget = kAmp[stage];
        } else if (stage == 2) {
            st.oralTarget = kAmp[1];   // left the throat but still in the mouth
        } else {
            st.oralTarget = 0.f;       // left the mouth (or the lips) entirely
        }
    }

    float Openness(std::uint32_t actorId, int kind)
    {
        auto it = g_state.find(actorId);
        if (it == g_state.end()) return 0.f;
        const ActorState& st = it->second;
        if (kind == kOral) return st.oralU;
        if (kind != kVaginal && kind != kAnal) return 0.f;
        const float maxU = ObjectHold::OrificeOpenMaxU(kind);
        if (!(maxU > 0.01f)) return 0.f;
        float v = st.ring[kind].curU / maxU;
        if (!(v > 0.f)) return 0.f;
        return v > 1.f ? 1.f : v;
    }

    // ── LAST-FRAME TEARDOWN ──────────────────────────────────────────────────────────────
    // Called from PPBHook the instant DismemberGuard excludes an actor — i.e. from ABOVE the
    // gate that would otherwise mean OnPreDrive is never called for her again. Without it, an
    // NPC killed and dismembered (or turned into an NGD head clone) mid-touch keeps her offset
    // for the life of the 3D: `armed` stays true and no teardown path can reach her.
    void ReleaseActor(RE::Actor* actor)
    {
        if (!actor) return;
        auto it = g_state.find(actor->GetFormID());
        if (it == g_state.end()) return;
        RestoreActor(actor, it->second);
        g_state.erase(it);
    }

    // ── STALE SWEEP ──────────────────────────────────────────────────────────────────────
    // The general form of the same hole: ANY actor that stops reaching OnPreDrive while a ring
    // is armed (left the driven set, PLANCK dropped her, an enrollment race) would never be
    // eased shut. This runs from the HIGGS frame callback, which is behind no per-actor gate at
    // all. Main thread, same as OnPreDrive — no lock needed, and none may be added (see the
    // house rule on the drive seam).
    void Sweep()
    {
        if (g_state.empty()) return;
        const std::uint64_t now = NowMs();
        static std::uint64_t s_lastSweepMs = 0;
        if (now - s_lastSweepMs < kSweepEveryMs) return;
        s_lastSweepMs = now;
        const bool log = ObjectHold::OrificeLogEnabled();
        for (auto it = g_state.begin(); it != g_state.end(); ) {
            if (now - it->second.lastSeenMs < kStaleMs) { ++it; continue; }
            const std::uint32_t sid = it->first;
            // Resolve FRESH from the FormID — a stored Actor* must never cross a frame here.
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(sid)) {
                const bool held = it->second.ring[0].armed || it->second.ring[1].armed;
                RestoreActor(a, it->second);
                if (log && held)
                    logger::info("ORIFICE {:08X}: stopped reaching the drive seam — restored and "
                                 "dropped (stale sweep)", sid);
            }
            it = g_state.erase(it);
        }
    }

    void ClearOnLoad()
    {
        // PRE-load: the world and the 3D are still alive, so this is the last moment a
        // restore can actually land. Clear on kNewGame too (a new game never fires
        // kPreLoadGame, and FormIDs recycle).
        RestoreAllAndClear("kPreLoadGame/kNewGame");
        g_parkedLogged.clear();   // FormIDs recycle across saves — so must the one-shot log set
    }
}
