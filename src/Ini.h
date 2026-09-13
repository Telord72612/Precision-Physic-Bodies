#pragma once
#include <cstdint>

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  PPB.ini — the general settings file (Data/SKSE/Plugins/PPB.ini), born 2026-08-30 for the
//  equip slot-occupied rule and intended to grow (user: "the .ini might get used for a lot of
//  things in the future").
//
//  Design contract:
//  * Sectioned INI ("[Equip]" + "key=value"), ';' or '#' comments, whitespace-tolerant.
//  * HOT: the file's mtime is checked at most once per second on any read; a saved edit
//    applies within ~1s of the next query — same feel as PPB_tuning.txt.
//  * Defaults live at the CALL SITE (the second argument), never in the file — a missing
//    file or key silently yields the coded default, so the ini can ship sparse.
//  * Readers are cheap (hash-map lookup) and safe from any thread that already reads the
//    tuning knobs (same main-thread-poll pattern; a lock guards the reparse swap).
// ═══════════════════════════════════════════════════════════════════════════════════════════
namespace Ini {
    // "section" and "key" are case-insensitive. GetBool accepts 1/0/true/false/on/off.
    bool  GetBool (const char* section, const char* key, bool def);
    float GetFloat(const char* section, const char* key, float def);
    // Copies into out (always NUL-terminated); returns false (and copies def) when absent.
    bool  GetString(const char* section, const char* key, const char* def,
                    char* out, std::size_t cap);

    // ── [Features] — the FOMOD-level feature switches (2026-09-11, user) ───────────────────
    //  A SHIP SWITCH, not a dial. Chosen once at install and delivered as a file, which is why
    //  it lives here and not among PPB_tuning.txt's 1906 knobs: a FOMOD installs FILES, so a
    //  knob there would mean shipping two copies of a 3283-line tuning file and letting them
    //  drift (and that file is already edited by more than one session).
    //
    //  Layered, never replacing: the feature switch is ANDed with the existing tuning master
    //  (`pushStep`, the gesture cfg's `enabled`). The ini decides whether the feature SHIPPED;
    //  the knob stays the dial. Neither overrides the other.
    //
    //  Both default TRUE — an existing install with no [Features] section behaves exactly as it
    //  does today, so an upgrade surprises nobody.
    //
    //  ⚠ Cached in atomics behind a ~1 Hz staleness check because these are read from the
    //  per-actor PRE-DRIVE HOOK: GetBool builds two std::strings per call, and an allocation on
    //  that path is the same defect already logged against WearsDismemberGear. Still hot — a
    //  saved edit applies within ~1 s, so the switch can be tested without a rebuild.
    bool  FeaturePushShove();        // push / shove / stagger / knockdown (the whole mechanism)
    bool  FeatureEquipGestures();    // press-to-equip / two-hand undress / fingertip plug
    // ★ 2026-09-12: the FEET-LIFT RAGDOLL as its own opt-out, per the user: "there is a high chance
    // it will cause problem, so let's have the user decide." A SUB-feature of push/shove — with
    // bPushShove=0 it is already off (PushStep::OnFrame returns first), so this only matters when
    // push is on. Covers BOTH feet paths: the lift (both feet raised by a player contact) and the
    // char-controller-unsupported fall. Push, shove, stumble and the push-tier knockdown are untouched.
    bool  FeatureFeetLift();
    // ★★ 2.2.0 (2026-09-12, user): every push OUTCOME is its own install choice — "if someone chose push/shove,
    // give option between push/walk, push/stumble, shove/ragdoll and leg sweep/ragdoll". Each is a SUB-feature of
    // bPushShove (moot when it is 0) and ANDed with its tuning dial, like the others. All default TRUE.
    //   bPushWalk     — a push that crosses the walk bar makes her step back (PPB's planner-controlled walk)
    //   bPushStumble  — a firm push plays the vanilla stagger, facing the push
    //   bShoveRagdoll — a hard shove knocks her down. OFF + stumble ON: the shove plays a stumble instead
    //   bFeetLift     — (above) the leg sweep / feet-lift knockdown
    bool  FeaturePushWalk();
    bool  FeaturePushStumble();
    bool  FeatureShoveRagdoll();
}
