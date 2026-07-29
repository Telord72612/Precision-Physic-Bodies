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
