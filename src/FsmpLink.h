#pragma once

// ─────────────────────────────────────────────────────────────────────────
// FsmpLink — STAGE 1 (2026-07-11): read-only handshake with the SMP engine's
// plugin interface (FSMP 4.0.1 / SMP Flex 0.8.x — both ship hdt::PluginInterface).
//
// Registers SKSE-messaging listeners for BOTH known sender names at kPostLoad;
// on MSG_STARTUP, version-gates (interface major must equal the compiled header's)
// and, when accepted, attaches a PostStep listener that only counts objects.
// ZERO Bullet-layout assumptions beyond btAlignedObjectArray's size field —
// the whole point of stage 1 is to prove the handshake + event flow + threading
// on the user's exact DLL before stage 2 (capsule-displacement -> applyForce)
// touches any physics. See Report/PPB Module/14 §3 and the staged-build note.
// ─────────────────────────────────────────────────────────────────────────
namespace FsmpLink {

    // Call during SKSE kPostLoad (MUST precede the engine's kPostPostLoad dispatch).
    void Register();
    // Exposed 2026-08-01: SKSE allows exactly ONE handler per (listener, sender) pair, and a
    // wildcard RegisterListener(nullptr,...) claims PPB's slot in EVERY plugin's list. So the
    // SMP handshake cannot own its own registration — main.cpp installs one unified handler
    // and forwards here. Ignores anything that is not the engine's MSG_STARTUP.
    void HandleEngineMessage(SKSE::MessagingInterface::Message* msg);

    // Main-thread frame tick: deferred logging (the PostStep callback runs on the
    // engine's TBB worker with its world lock held — it must never log).
    void OnFrame();

    bool Connected();

    // ── STAGE 2 (2026-07-11): the capsule-sensor -> SMP-force actuator ──
    // DriveRig (main thread) publishes, per tail chord, the HOST BONE's world
    // position (the position-match key — SMP's world runs in plain game units)
    // and the capsule's current displacement off its chord (the push the
    // player's hand/glove/weapon imparted). The PreStep listener (engine
    // physics thread) matches dynamic SMP bone rigids by position (<1.5u)
    // and applies fsmpPushForce x displacement as a central force — inside
    // the API's sanctioned forces-only window. No FSMP-internal layout used.
    // ── THE push-stage size, THE single source of truth (2026-07-30) ──────────────────
    // Was three hand-synchronized literals (kMaxSensors, g_pubStage[14], TargetBuf::t[14] +
    // the PublishTargets clamp) with comments begging "the three sizes must move in
    // lock-step". Every other size in this seam now derives from this constant.
    //
    // 28, not 14: the stage carries the SAME-ACTOR MERGE of all garment rigs. M'rissi is
    // the proven worst case — her foxtail is the one table that claims all 14 slots by
    // itself (2026-07-17 user dial), so with a covered wig on the same actor the wig
    // published NOTHING and her hair never reacted (user-reported 2026-07-30). 14 + 14
    // covers a max-sensor tail plus a max-sensor wig, both in full. Per-RIG publish counts
    // are unchanged (each table still publishes at most its own TuneOf sensors ≤ 14), so
    // this does not touch the 19-chord-whip regression class; PreSink's per-step scan is
    // O(bodies x targets) and only fills past 14 on a tail+wig actor.
    inline constexpr int kMaxPushTargets = 28;

    struct PushTarget {
        float bonePosU[3];
        float dispU[3];
        // 2026-07-13 per-target feel (multi-rig): gain + clamp ride WITH the target
        // (from NpcFingerTest's TuneOf(rig.tbl) locked dial numbers). The PreStep
        // listener applies effForce = force * fsmpPushMult (same for maxF) — the
        // legacy fsmpPushForce/fsmpPushMaxForce knobs are no longer read there.
        float force;
        float maxF;
        float massRef = 0.4f;   // 2026-07-17 per-TABLE mass-scale reference (kg): PreSink scales
                                // force by boneMass/massRef. 0.4 = the fluffy-tail base the mass
                                // scale was derived on; the foxtail's dial was FELT on ~0.1 kg
                                // bones, so its targets carry 0.1 (else mid/tip derate 4-8x and
                                // read dead). <=0.01 falls back to 0.4.
    };
    void PublishTargets(const PushTarget* t, int n);   // main thread, per frame
}
