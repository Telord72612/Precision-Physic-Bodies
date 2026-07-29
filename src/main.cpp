#include "PCH.h"
#include "Hooks.h"
#include "PPBHook.h"   // kPreLoadGame teardown: bind-rel cache + statue set
#include "PivFix.h"    // kPreLoadGame teardown: per-actor pivot state
#include "Tuning.h"    // ObjectHold::InitHeelFixDefault / ToggleHeelFix
#include "CapFix.h"    // CapFixConsole — the in-game hand-capsule editor
#include "Natives.h"   // PPB_Native.SetStatuePose / SetHeelFix / ToggleHeelFix
#include "Interop.h"   // Interop::AcquireHiggs — the PivFix grab gate's HIGGS handshake
#include "PerfSys.h"
#include "FsmpLink.h"   // SMP engine plugin-interface handshake (stage 1, 2026-07-11)   // PerfSys::RegisterHiggs / ClearOnLoad — the collision perf system
#include "Probe.h"     // Probe::DumpActorState — the `probe` console command
#include "Diag.h"      // Diag — the read-only perf/contact/census instrument (`perf` + `capdis`)
#include "NpcFingerTest.h"  // NpcFinger — the `nfing` console command + kPreLoadGame teardown
#include "HandBox.h"   // HandBox — kPreLoadGame teardown (registration rides PerfSys::RegisterHiggs)
#include "DismemberGuard.h"  // DF/NGD ↔ PLANCK guard: death-grace ignore + head-clone exclusion (2026-07-26)

#include <Windows.h>  // GetModuleHandleA — sibling-mod presence checks for the trace
#include <cstdio>     // snprintf — the environment fingerprint line
#include <cstdlib>    // strtoul — PPB_skeletons.txt npcCap FormID parse
#include <algorithm>  // std::find — npcName distinct-race collection
#include <fstream>    // PPB_skeletons.txt reader
#include <sstream>    // line tokenizer
#include <string>
#include <vector>     // npcName mapped-race list

namespace logger = SKSE::log;

SKSEPluginInfo(
    .Version              = { 0, 1, 0 },
    .Name                 = "PPB",
    .Author               = "mad72",
    .StructCompatibility  = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary
)

// PPB.log lives in <Documents>\My Games\Skyrim VR\SKSE\ (same dir as every
// other C++ spdlog plugin). Identical setup to AIHands: one file sink,
// truncate-on-launch (a fresh log every run), info level, flush every line so
// nothing is lost on a CTD, [HH:MM:SS.mmm] [level] message pattern.
static void InitLogging()
{
    auto path = SKSE::log::log_directory();
    if (!path) {
        return;
    }
    *path /= "PPB.log";

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log  = std::make_shared<spdlog::logger>("PPB", sink);
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
}

// ── Console command: HeelFix (alias hf) ──────────────────────────────────────
// SkyrimVR's console has no cgf/CallGlobalFunction, so runtime toggles get REAL
// console commands instead (user rule 2026-07-02: in-game controls, never file
// edits). Standard technique: repurpose an unused vanilla console-command slot
// ("TestSeenData") — rewrite its name/help/executeFunction at kDataLoaded.
static bool HeelFixConsoleExec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                               RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                               double&, std::uint32_t&)
{
    const bool on = ObjectHold::ToggleHeelFix();
    if (auto* console = RE::ConsoleLog::GetSingleton())
        console->Print(on ? "PPB: HEEL FIX ON - every heeled NPC's ragdoll is raised to match her body."
                          : "PPB: HEEL FIX OFF - ragdolls back at barefoot height.");
    logger::info("HeelFix toggled {} (console command)", on ? "ON" : "OFF");
    return true;
}

// Console command: CapFix — ALL in-game, no file (user rule). Click the NPC in the console, then:
//   capfix                       → print her current R-hand collision capsule (A, B, radius)
//   capfix ax ay az bx by bz r   → rewrite it NOW (body-local skyrim units); other driven NPCs follow.
static RE::SCRIPT_PARAMETER kCapFixParams[] = {
    { "ax", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "ay", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "az", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "bx", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "by", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "bz", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "r",  RE::SCRIPT_PARAM_TYPE::kFloat, true },
};

static bool CapFixConsoleExec(const RE::SCRIPT_PARAMETER* a_paramInfo, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData,
                              RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR* a_containingObj, RE::Script* a_scriptObj,
                              RE::ScriptLocals* a_locals, double&, std::uint32_t& a_opcodeOffsetPtr)
{
    constexpr float kUnset = 3.0e8f;
    float ax = kUnset, ay = kUnset, az = kUnset, bx = kUnset, by = kUnset, bz = kUnset, r = kUnset;
    RE::Script::ParseParameters(a_paramInfo, a_scriptData, a_opcodeOffsetPtr, a_thisObj, a_containingObj,
                                a_scriptObj, a_locals, &ax, &ay, &az, &bx, &by, &bz, &r);
    const bool haveAll = (ax < 1.0e8f && ay < 1.0e8f && az < 1.0e8f &&
                          bx < 1.0e8f && by < 1.0e8f && bz < 1.0e8f && r < 1.0e8f);
    auto* actor = a_thisObj ? a_thisObj->As<RE::Actor>() : nullptr;
    // Entry log — 2026-07-02: a console session showed ZERO CapFix log lines after args were typed,
    // meaning the command died in the console compiler before reaching us. Log EVERY arrival.
    logger::info("CapFix console exec: thisObj={:08X} haveAll={} args=[{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f}]",
                 a_thisObj ? a_thisObj->GetFormID() : 0, haveAll,
                 ax < 1.0e8f ? ax : -999.f, ay < 1.0e8f ? ay : -999.f, az < 1.0e8f ? az : -999.f,
                 bx < 1.0e8f ? bx : -999.f, by < 1.0e8f ? by : -999.f, bz < 1.0e8f ? bz : -999.f,
                 r < 1.0e8f ? r : -999.f);
    char msg[512] = {};
    GrabDiag::CapFixConsole(actor, haveAll, ax, ay, az, bx, by, bz, r, msg, sizeof(msg));
    if (auto* console = RE::ConsoleLog::GetSingleton())
        console->Print("%s", msg);
    return true;
}

// Console command: statue — the ESP-FREE way to drive the calibration statue (bind-pose stomp) that
// auto-seat measures against. The A-pose spell (Precision Physic Bodies.esp) does the same via
// PPB_Native.SetStatuePose, but a plain console command needs no plugin, no addspell, no FE-id hunt —
// same recipe as trf/trg: referenceFunction=false + zero params + pull the console's SELECTED ref.
static RE::Actor* StatueTargetActor(RE::TESObjectREFR* a_thisObj)
{
    if (a_thisObj) if (auto* a = a_thisObj->As<RE::Actor>()) return a;
    if (auto* ui = RE::UI::GetSingleton(); ui) {
        if (auto console = ui->GetMenu<RE::Console>(); console) {
            auto handle = RE::Console::GetSelectedRef();
            if (auto ref = handle.get(); ref) return ref->As<RE::Actor>();
        }
    }
    return nullptr;
}

static bool StatueConsoleExec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                              RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                              double&, std::uint32_t&)
{
    auto* actor = StatueTargetActor(a_thisObj);
    logger::info("statue console exec: thisObj={:08X} resolvedActor={:08X}",
                 a_thisObj ? a_thisObj->GetFormID() : 0, actor ? actor->GetFormID() : 0);
    if (!actor) {
        if (auto* console = RE::ConsoleLog::GetSingleton())
            console->Print("PPB: no NPC selected - click one in the console first, then type 'statue'.");
        return true;
    }
    const bool on = ArmIK::ToggleStatuePose(actor);
    if (auto* console = RE::ConsoleLog::GetSingleton())
        console->Print(on ? "PPB: STATUE ON %08X - bind-pose held; auto-seat will walk the joints onto their bones."
                          : "PPB: STATUE OFF %08X - released (auto-seat measurement stays sticky).",
                       actor->GetFormID());
    return true;
}

// Console command: probe — read-only state dump of the console-selected NPC to PPB.log (per-body
// motion type/velocity/worldZ, ragdoll/knock/life/sitsleep/killmove states, bAnimationDriven,
// occupied furniture, charController support, heel/statue context). Zero writes anywhere.
static bool ProbeConsoleExec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                             RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                             double&, std::uint32_t&)
{
    auto* actor = StatueTargetActor(a_thisObj);
    if (!actor) {
        if (auto* console = RE::ConsoleLog::GetSingleton())
            console->Print("PPB: no NPC selected - click one in the console first, then type 'probe'.");
        return true;
    }
    Probe::DumpActorState(actor);
    if (auto* console = RE::ConsoleLog::GetSingleton()) {
        auto* base = actor->GetActorBase();
        const char* nm = (base && base->GetFullName()) ? base->GetFullName() : "<unnamed>";
        console->Print("PROBE %s (%08X) -> full state dumped to PPB.log", nm, actor->GetFormID());
    }
    return true;
}

// Console command: perf — the read-only wave-2 gating instrument (zero params, referenceFunction=false,
// the trf/trg/statue recipe). 1st press: reset + arm the contact counter + step timer, dump the census +
// collision-filter for the selected NPC. 2nd press: disarm + dump maxPointsInOnePair (the ship gate),
// step ms, and the projection table to PPB.log. Select an NPC before the FIRST press so the census runs.
static bool PerfConsoleExec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                            RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                            double&, std::uint32_t&)
{
    auto* actor = StatueTargetActor(a_thisObj);
    char msg[512] = {};
    Diag::TogglePerf(actor, msg, sizeof(msg));
    if (auto* console = RE::ConsoleLog::GetSingleton())
        console->Print("%s", msg);
    return true;
}

// Console command: nfing — the NPC FINGER TEST toggle (2026-07-09). Click the NPC in the console,
// then `nfing`: 4 runtime DYNAMIC capsules attach to her right-hand fingers on her next driven
// frame; `nfing` again removes them. Zero params + referenceFunction=false (the trf/trg recipe);
// pulls the console-SELECTED ref like `statue`/`probe`. The knob path (npcFingerEnable 1 -> nearest
// driven NPC) is the guaranteed entry if every donor below turns out to be missing.
static bool NpcFingerConsoleExec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                                 RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                                 double&, std::uint32_t&)
{
    auto* actor = StatueTargetActor(a_thisObj);
    logger::info("nfing console exec: thisObj={:08X} resolvedActor={:08X}",
                 a_thisObj ? a_thisObj->GetFormID() : 0, actor ? actor->GetFormID() : 0);
    if (!actor) {
        if (auto* console = RE::ConsoleLog::GetSingleton())
            console->Print("PPB: no NPC selected - click one in the console first, then type 'nfing'.");
        return true;
    }
    const bool on = NpcFinger::RequestToggle(actor);
    if (auto* console = RE::ConsoleLog::GetSingleton())
        console->Print(on ? "PPB: NPC FINGER TEST pinned %08X - 4 dynamic capsules attach on her next driven frame."
                          : "PPB: NPC FINGER TEST unpinned %08X - capsules removed on the next frame.",
                       actor->GetFormID());
    return true;
}

// Console command: jtrack — read-only Havok-joint vs XP32-node correlation dump of the console-selected
// NPC to PPB.log (per-joint pivot-vs-bone gap for all 17 ragdoll joints + a scale-law summary). Settles
// the actor-scale pivot-drift question. Writes NOTHING to any constraint. Zero params + referenceFunction
// =false (the trf/trg recipe); pulls the console-SELECTED ref like `statue`/`probe`.
static bool JTrackConsoleExec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                              RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                              double&, std::uint32_t&)
{
    auto* actor = StatueTargetActor(a_thisObj);
    if (!actor) {
        if (auto* console = RE::ConsoleLog::GetSingleton())
            console->Print("PPB: no NPC selected - click one in the console first, then type 'jtrack'.");
        return true;
    }
    ObjectHold::PivJointTrackDump(actor);
    if (auto* console = RE::ConsoleLog::GetSingleton()) {
        auto* base = actor->GetActorBase();
        const char* nm = (base && base->GetFullName()) ? base->GetFullName() : "<unnamed>";
        console->Print("JTRACK %s (%08X) -> joint-vs-bone correlation dumped to PPB.log", nm, actor->GetFormID());
    }
    return true;
}

// Console command: capdis — GATED live disableChild spike. `capdis` (no args) = children 3,4,5 of R Thigh
// (slot 8, mask 56); `capdis <slot> <mask>` = arbitrary. mask bit i set -> child i DISABLED; mask 0 =
// re-enable all. NOTHING happens unless `lodSpike 1` is set in PPB_tuning.txt. referenceFunction=true +
// 2 float params (the arg'd-command recipe).
static RE::SCRIPT_PARAMETER kCapDisParams[] = {
    { "slot", RE::SCRIPT_PARAM_TYPE::kFloat, true },
    { "mask", RE::SCRIPT_PARAM_TYPE::kFloat, true },
};
static bool CapDisConsoleExec(const RE::SCRIPT_PARAMETER* a_paramInfo, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData,
                              RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR* a_containingObj, RE::Script* a_scriptObj,
                              RE::ScriptLocals* a_locals, double&, std::uint32_t& a_opcodeOffsetPtr)
{
    constexpr float kUnset = 3.0e8f;
    float fslot = kUnset, fmask = kUnset;
    RE::Script::ParseParameters(a_paramInfo, a_scriptData, a_opcodeOffsetPtr, a_thisObj, a_containingObj,
                                a_scriptObj, a_locals, &fslot, &fmask);
    auto* actor = a_thisObj ? a_thisObj->As<RE::Actor>() : nullptr;
    const int          slot = (fslot < 1.0e8f) ? static_cast<int>(fslot + 0.5f) : 8;                        // default R Thigh
    const std::uint32_t mask = (fmask < 1.0e8f) ? static_cast<std::uint32_t>(fmask + 0.5f) : 56u;           // default children 3,4,5
    char msg[512] = {};
    Diag::CapDisRequest(actor, slot, mask, msg, sizeof(msg));
    if (auto* console = RE::ConsoleLog::GetSingleton())
        console->Print("%s", msg);
    return true;
}

static void RegisterConsoleCommands()
{
    // statue — try a list of unused vanilla donor slots; the first one present wins (guards against a
    // wrong donor name leaving the command unregistered). NONE overlap the four already in use
    // (TestSeenData/DumpNiUpdates here, ToggleMagicStats/ToggleCombatStats in collviz_markers).
    {
        static const char* kStatueDonors[] = { "ShowScenegraph", "TestAllCells", "ToggleWaterSystem", "ShowQuestStages" };
        bool done = false;
        for (const char* donor : kStatueDonors) {
            if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(donor)) {
                cmd->functionName        = "Statue";
                cmd->shortName           = "statue";
                cmd->helpString          = "Select an NPC, then 'statue' — toggle the PPB bind-pose calibration statue (auto-seat stance)";
                cmd->referenceFunction   = false;
                cmd->SetParameters();                       // zero parameters (the trf/trg recipe)
                cmd->executeFunction     = &StatueConsoleExec;
                cmd->conditionFunction   = nullptr;
                cmd->editorFilter        = false;
                cmd->invalidatesCellList = false;
                logger::info("Console command registered: 'statue' (on donor '{}') — select an NPC, type 'statue' to toggle.", donor);
                done = true;
                break;
            }
        }
        if (!done) logger::info("Console-command donors for 'statue' all missing - use the 'PPB Statue Toggle' spell instead.");
    }

    // probe — registered AFTER statue (donor sharing: statue's first hit renames its slot, so
    // LocateConsoleCommand on that original name fails here and the fallback walk skips it).
    {
        static const char* kProbeDonors[] = { "TestAllCells", "ToggleWaterSystem", "ShowQuestStages" };
        bool done = false;
        for (const char* donor : kProbeDonors) {
            if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(donor)) {
                cmd->functionName        = "PPBProbe";
                cmd->shortName           = "probe";
                cmd->helpString          = "Select an NPC, then 'probe' — dump her full physics/animation state to PPB.log";
                cmd->referenceFunction   = false;
                cmd->SetParameters();                       // zero parameters (the trf/trg recipe)
                cmd->executeFunction     = &ProbeConsoleExec;
                cmd->conditionFunction   = nullptr;
                cmd->editorFilter        = false;
                cmd->invalidatesCellList = false;
                logger::info("Console command registered: 'probe' (on donor '{}') — select an NPC, type 'probe' to dump her state.", donor);
                done = true;
                break;
            }
        }
        if (!done) logger::info("Console-command donors for 'probe' all missing - command unavailable.");
    }

    if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand("TestSeenData")) {   // unused vanilla slot (po3 pattern)
        cmd->functionName        = "HeelFix";
        cmd->shortName           = "hf";
        cmd->helpString          = "Toggle PPB heel fix (heeled NPCs' ragdolls raised to match their bodies)";
        cmd->referenceFunction   = false;
        cmd->SetParameters();                       // zero parameters
        cmd->executeFunction     = &HeelFixConsoleExec;
        cmd->conditionFunction   = nullptr;
        cmd->editorFilter        = false;
        cmd->invalidatesCellList = false;
        logger::info("Console command registered: 'HeelFix' (alias 'hf') — type it in the console to toggle.");
    } else {
        logger::info("Console-command donor 'TestSeenData' NOT found — 'HeelFix' command unavailable.");
    }

    if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand("DumpNiUpdates")) {  // second unused donor slot
        cmd->functionName        = "CapFix";
        cmd->shortName           = "capfix";
        cmd->helpString          = "Select an NPC, then: capfix [ax ay az bx by bz r] — show/edit her hand collision capsule";
        cmd->referenceFunction   = true;                    // operates on the console-SELECTED reference
        cmd->SetParameters(kCapFixParams);                  // 7 optional floats
        cmd->executeFunction     = &CapFixConsoleExec;
        cmd->conditionFunction   = nullptr;
        cmd->editorFilter        = false;
        cmd->invalidatesCellList = false;
        logger::info("Console command registered: 'CapFix' — select an NPC, 'capfix' shows, 'capfix ax ay az bx by bz r' edits.");
    } else {
        logger::info("Console-command donor 'DumpNiUpdates' NOT found — 'CapFix' command unavailable.");
    }

    // perf — the read-only wave-2 gate (zero params, referenceFunction=false). Donor ToggleWaterSystem
    // (fallback ShowQuestStages). These are the two donors the report reserved: statue took ShowScenegraph
    // and probe took TestAllCells, so ToggleWaterSystem/ShowQuestStages remain free (and collviz owns
    // ToggleMagicStats/ToggleCombatStats — no collision).
    {
        static const char* kPerfDonors[] = { "ToggleWaterSystem", "ShowQuestStages" };
        bool done = false;
        for (const char* donor : kPerfDonors) {
            if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(donor)) {
                cmd->functionName        = "PPBPerf";
                cmd->shortName           = "perf";
                cmd->helpString          = "Select an NPC, then 'perf' — arm the read-only contact/step instrument; 'perf' again dumps the ship gate";
                cmd->referenceFunction   = false;
                cmd->SetParameters();                       // zero parameters (the trf/trg recipe)
                cmd->executeFunction     = &PerfConsoleExec;
                cmd->conditionFunction   = nullptr;
                cmd->editorFilter        = false;
                cmd->invalidatesCellList = false;
                logger::info("Console command registered: 'perf' (on donor '{}') — select an NPC, 'perf' to arm, 'perf' again to dump.", donor);
                done = true;
                break;
            }
        }
        if (!done) logger::info("Console-command donors for 'perf' all missing — perf command unavailable.");
    }

    // capdis — the GATED disableChild spike (referenceFunction=true, 2 float params). Donor ShowQuestStages
    // (perf took ToggleWaterSystem above; its rename makes LocateConsoleCommand skip it here automatically).
    {
        static const char* kCapDisDonors[] = { "ShowQuestStages", "ToggleWaterSystem" };
        bool done = false;
        for (const char* donor : kCapDisDonors) {
            if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(donor)) {
                cmd->functionName        = "PPBCapDis";
                cmd->shortName           = "capdis";
                cmd->helpString          = "GATED (needs lodSpike 1): select an NPC, capdis [slot mask] — disableChild spike; default = R Thigh children 3,4,5";
                cmd->referenceFunction   = true;            // operates on the console-SELECTED reference
                cmd->SetParameters(kCapDisParams);          // 2 optional floats (slot, mask)
                cmd->executeFunction     = &CapDisConsoleExec;
                cmd->conditionFunction   = nullptr;
                cmd->editorFilter        = false;
                cmd->invalidatesCellList = false;
                logger::info("Console command registered: 'capdis' (on donor '{}') — GATED live disableChild spike.", donor);
                done = true;
                break;
            }
        }
        if (!done) logger::info("Console-command donors for 'capdis' all missing — capdis command unavailable.");
    }

    // nfing — the NPC finger test toggle (zero params, referenceFunction=false). All six VERIFIED
    // donors are consumed (statue/probe/hf/capfix/perf/capdis here, ToggleMagicStats/ToggleCombatStats
    // in collviz), so these candidates are UNVERIFIED (spec §5) — the walk logs and degrades
    // gracefully; the npcFingerEnable knob is the guaranteed entry either way.
    {
        static const char* kNfingDonors[] = { "ShowFullQuestLog", "DumpAniBindings", "TestPath" };
        bool done = false;
        for (const char* donor : kNfingDonors) {
            if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(donor)) {
                cmd->functionName        = "PPBNpcFinger";
                cmd->shortName           = "nfing";
                cmd->helpString          = "Select an NPC, then 'nfing' — toggle the 4 dynamic finger-test capsules on her right hand";
                cmd->referenceFunction   = false;
                cmd->SetParameters();                       // zero parameters (the trf/trg recipe)
                cmd->executeFunction     = &NpcFingerConsoleExec;
                cmd->conditionFunction   = nullptr;
                cmd->editorFilter        = false;
                cmd->invalidatesCellList = false;
                logger::info("Console command registered: 'nfing' (on donor '{}') — select an NPC, type 'nfing' to toggle.", donor);
                done = true;
                break;
            }
        }
        if (!done) logger::info("Console-command donors for 'nfing' all missing — use npcFingerEnable 1 in PPB_tuning.txt instead.");
    }

    // jtrack — the read-only joint-vs-bone audit (zero params, referenceFunction=false). Shares nfing's
    // UNVERIFIED donor pool (spec §5) and registers AFTER nfing: nfing's first hit renames its slot, so
    // LocateConsoleCommand on that original name fails here and the fallback walk skips it (the same
    // donor-sharing mechanism probe uses with statue). Every VERIFIED donor is already consumed; if the
    // whole pool is missing the command degrades gracefully (jtrack is a dev-only diagnostic).
    {
        static const char* kJTrackDonors[] = { "DumpAniBindings", "TestPath", "ShowFullQuestLog" };
        bool done = false;
        for (const char* donor : kJTrackDonors) {
            if (auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(donor)) {
                cmd->functionName        = "PPBJTrack";
                cmd->shortName           = "jtrack";
                cmd->helpString          = "Select an NPC, then 'jtrack' — dump Havok-joint vs XP32-bone gaps for all 17 ragdoll joints (READ-ONLY) to PPB.log";
                cmd->referenceFunction   = false;
                cmd->SetParameters();                       // zero parameters (the trf/trg recipe)
                cmd->executeFunction     = &JTrackConsoleExec;
                cmd->conditionFunction   = nullptr;
                cmd->editorFilter        = false;
                cmd->invalidatesCellList = false;
                logger::info("Console command registered: 'jtrack' (on donor '{}') — select an NPC, type 'jtrack' to dump joint-vs-bone gaps.", donor);
                done = true;
                break;
            }
        }
        if (!done) logger::info("Console-command donors for 'jtrack' all missing — joint-track command unavailable.");
    }

    // (Console donor recipe, for the record: arg'd commands need referenceFunction=true; zero-param
    // toggles must use referenceFunction=false — the ref+zero-param combo silently dies in the console
    // compiler (the trf/trg v1 lesson). PPB's donors: TestSeenData→hf, DumpNiUpdates→capfix. The trf/trg
    // visualizer toggles live in their own DLL on donors ToggleMagicStats/ToggleCombatStats — no collision.)
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// RUNTIME SKELETON MAP (2026-07-17, user design: "whenever a skeleton is given to an NPC by the
// engine, if it's a different race, we override it with our own — no .esp dependence").
//
// Config: PPB_skeletons.txt (dev path first, USVFS fallback — the PPB_tuning resolve pattern).
//   race  <RaceEditorID>  <path relative to Data\meshes\>     — repoint that race's FEMALE skeleton
//   npcCap  <localHexID>:<Plugin.esp>  <slotName>  <childIdx>  ax ay az bx by bz r
//                                                              — per-NPC capsule child override
//
// WHY IT WORKS: TESRace::skeletonModels[kFemale] (TESRace.h:317, the ANAM field) is the live
// in-memory record the engine reads at EVERY actor 3D build; its TESModel has a first-class
// virtual SetModel(). One write at kDataLoaded — after every plugin has resolved its winners,
// before any 3D exists — and all subsequent skeleton loads for that race use OUR file. The
// behavior graph is a SEPARATE field (TESRace.h:323), so animation is untouched. No plugin in
// this load order writes skeleton paths at runtime (scoped 2026-07-17), so nothing fights us.
// This replaces ESP race overrides (no masters, no winner-theft, works on custom follower races).
// VERIFY on first use: the CapFix "N children live" log line names the loaded head child count.
// ═══════════════════════════════════════════════════════════════════════════════════════════
// ── OBODY PUSH RE-FIT (2026-07-18, Report 21): OBody fires the SKSE ModEvent "Obody_ApplyMorph"
// (sender = the Actor) every time morphs are ACTUALLY applied — the cleanest re-fit moment (the
// O-key picker, SPID-time generation, outfit refits). The sink runs on the Papyrus VM thread, so
// it only queues the formID; CapFix's main-thread sweep drains it (re-latch -> re-dress). Ends
// the manual bodyScaleDump-pulse era.
class ObodyEventSink : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev,
                                          RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
    {
        if (ev && ev->sender && ev->eventName.c_str() &&
            _stricmp(ev->eventName.c_str(), "Obody_ApplyMorph") == 0)
            if (auto* a = ev->sender->As<RE::Actor>())
                GrabDiag::InvalidateBodyScale(a->GetFormID());   // CapFix.h's legacy namespace
        return RE::BSEventNotifyControl::kContinue;
    }
};
static ObodyEventSink g_obodySink;

static int SlotIndexByName(const std::string& s)
{
    if (s == "hand")   return 0;
    if (s == "forearm")return 1;
    if (s == "upperarm")return 2;
    if (s == "head")   return 3;
    if (s == "spine0") return 4;
    if (s == "spine1") return 5;
    if (s == "spine2") return 6;
    if (s == "thigh")  return 8;
    if (s == "calf")   return 9;
    if (s == "foot")   return 10;
    if (s == "com")    return 11;
    return -1;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════
// PPB_Skeletons_Added_Race.ini — USER-FACING SKELETON ASSIGNMENT (2026-07-21; renamed 2026-07-26,
// the old PPB_Skeletons.ini name collided with the dev-tool PPB_skeletons.txt)
// ══════════════════════════════════════════════════════════════════════════════════════════════
// Neither SPID nor SkyPatcher can do this: SPID distributes FORMS (10 types, none is a skeleton)
// and SkyPatcher's race patcher has no skeleton operation — it can only FILTER by the path
// (filterByFemaleModelContains), which is the idea we borrowed here. The only mechanism that
// works is our own kDataLoaded write to TESRace::skeletonModels[kFemale], so the ini is a config
// layer over that, wearing SPID/SkyPatcher-style ergonomics so users already know the shape of it.
//
// Sections = the PPB skeleton to hand out (path under Data\meshes\). Entries:
//   race = <EditorID> · npc = <local>|<Plugin.esp> · npcName = <full name>
//   · femaleModelContains = <substring>
// LAST MATCH WINS, so a broad catch-all can be narrowed by a later explicit race line.
// No file = no writes = every race keeps its vanilla skeleton, and ActorCarriesBake then makes
// PPB inert on them. That is the opt-out, and it costs nothing to implement — it is just absence.
static void ApplySkeletonIni()
{
    static const char* kPaths[] = {
        "D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB_Skeletons_Added_Race.ini",  // dev
        "Data/SKSE/Plugins/PPB_Skeletons_Added_Race.ini",                                             // shipped
        // Legacy name (pre-2026-07-26) — read so an unrenamed copy keeps working; the log names
        // which file won so a stale duplicate is visible at a glance.
        "D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB_Skeletons.ini",
        "Data/SKSE/Plugins/PPB_Skeletons.ini",
    };
    std::ifstream f;
    const char* used = nullptr;
    for (const char* p : kPaths) { f.open(p); if (f.is_open()) { used = p; break; } f.clear(); }
    if (!used) {
        logger::info("SKELINI: no PPB_Skeletons_Added_Race.ini — every race keeps its VANILLA "
                     "skeleton (PPB stays inert on them by design).");
        return;
    }

    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) { logger::info("SKELINI: no TESDataHandler — skipped."); return; }
    auto& races = dh->GetFormArray<RE::TESRace>();

    auto setSkel = [](RE::TESRace* rc, const std::string& path) -> const char* {
        if (!rc) return nullptr;
        auto& m = rc->skeletonModels[RE::SEXES::kFemale];
        static std::string before; before = m.GetModel() ? m.GetModel() : "";
        m.SetModel(path.c_str());
        return before.c_str();
    };
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    std::string line, section;
    int nApplied = 0, nBad = 0, nSec = 0;
    while (std::getline(f, line)) {
        if (const auto h = line.find_first_of(";#"); h != std::string::npos) line.erase(h);
        const auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        const auto e = line.find_last_not_of(" \t\r");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        if (line.front() == '[') {                       // ── section = target skeleton path
            const auto close = line.find(']');
            if (close == std::string::npos) { ++nBad; continue; }
            section = line.substr(1, close - 1);
            ++nSec;
            logger::info("SKELINI: section -> '{}'", section);
            continue;
        }
        if (section.empty()) { ++nBad; continue; }

        const auto eq = line.find('=');
        if (eq == std::string::npos) { ++nBad; continue; }
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            const auto x = s.find_first_not_of(" \t\r"); if (x == std::string::npos) { s.clear(); return; }
            const auto y = s.find_last_not_of(" \t\r"); s = s.substr(x, y - x + 1);
        };
        trim(key); trim(val);
        if (key.empty() || val.empty()) { ++nBad; continue; }

        if (key == "race") {
            auto* rc = RE::TESForm::LookupByEditorID<RE::TESRace>(val);
            if (!rc) { logger::info("SKELINI:   race '{}' NOT FOUND — skipped", val); ++nBad; continue; }
            const char* was = setSkel(rc, section);
            logger::info("SKELINI:   race {} : '{}' -> section", val, was ? was : "?");
            ++nApplied;
        } else if (key == "femaleModelContains") {
            const std::string needle = lower(val);
            int hit = 0;
            for (auto* rc : races) {
                if (!rc) continue;
                const char* cur = rc->skeletonModels[RE::SEXES::kFemale].GetModel();
                if (!cur || !*cur) continue;
                if (lower(cur).find(needle) == std::string::npos) continue;
                setSkel(rc, section);
                ++hit;
            }
            logger::info("SKELINI:   femaleModelContains '{}' -> {} race(s) repointed", val, hit);
            nApplied += hit;
        } else if (key == "npc") {
            // A skeleton is a RACE-level field — an NPC cannot carry her own without an exclusive
            // race. So we map HER race and report how many others ride it, rather than pretending
            // to be per-NPC (and rather than swapping paths at 3D load, which races the async
            // loader — Report 10 §8.2).
            const auto bar = val.find('|');
            if (bar == std::string::npos) { logger::info("SKELINI:   npc '{}' needs <local>|<Plugin.esp>", val); ++nBad; continue; }
            const std::string plugin = val.substr(bar + 1);
            std::uint32_t local = 0;
            try { local = static_cast<std::uint32_t>(std::stoul(val.substr(0, bar), nullptr, 16)); }
            catch (...) { ++nBad; continue; }
            auto* npc = dh->LookupForm<RE::TESNPC>(local, plugin);
            if (!npc || !npc->GetRace()) { logger::info("SKELINI:   npc '{}' NOT FOUND — skipped", val); ++nBad; continue; }
            auto* rc = npc->GetRace();
            int shared = 0;
            for (auto* other : dh->GetFormArray<RE::TESNPC>())
                if (other && other->GetRace() == rc) ++shared;
            setSkel(rc, section);
            logger::info("SKELINI:   npc {} -> mapping her race '{}' (SHARED by {} NPC(s) — "
                         "they all get this skeleton)", val,
                         rc->GetFormEditorID() ? rc->GetFormEditorID() : "?", shared);
            ++nApplied;
        } else if (key == "npcName") {
            // BY-NAME assignment (2026-07-26, user request): users know "Lydia", not FormIDs.
            // Case-insensitive EXACT full-name match over every TESNPC record. A skeleton is
            // still RACE-level (same constraint as the npc key above), so each DISTINCT race
            // among the matches is mapped once, with the shared-rider receipt per race. A
            // generic name ("Bandit") legitimately maps every race its bearers use — the log
            // makes that visible rather than silently narrowing to the first hit.
            int hits = 0;
            std::vector<RE::TESRace*> mappedRaces;
            for (auto* npc : dh->GetFormArray<RE::TESNPC>()) {
                if (!npc) continue;
                const char* fn = npc->GetFullName();
                if (!fn || !*fn || _stricmp(fn, val.c_str()) != 0) continue;
                ++hits;
                auto* rc = npc->GetRace();
                if (!rc) continue;
                if (std::find(mappedRaces.begin(), mappedRaces.end(), rc) != mappedRaces.end()) continue;
                mappedRaces.push_back(rc);
                int shared = 0;
                for (auto* other : dh->GetFormArray<RE::TESNPC>())
                    if (other && other->GetRace() == rc) ++shared;
                setSkel(rc, section);
                logger::info("SKELINI:   npcName '{}' -> mapping race '{}' (SHARED by {} NPC(s) — "
                             "they all get this skeleton)", val,
                             rc->GetFormEditorID() ? rc->GetFormEditorID() : "?", shared);
                ++nApplied;
            }
            if (!hits) {
                logger::info("SKELINI:   npcName '{}' matched NO NPC — skipped", val);
                ++nBad;
            } else if (mappedRaces.size() > 1) {
                logger::info("SKELINI:   npcName '{}' matched {} NPC(s) across {} races — "
                             "ALL of those races were mapped (use race= lines to narrow)",
                             val, hits, mappedRaces.size());
            }
        } else { ++nBad; continue; }
    }
    logger::info("SKELINI: '{}' — {} section(s), {} race assignment(s), {} bad line(s).",
                 used, nSec, nApplied, nBad);
}

static void ApplySkeletonMap()
{
    static const char* kPaths[] = {
        "D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB_skeletons.txt",  // dev live-edit
        "Data/SKSE/Plugins/PPB_skeletons.txt",                                             // shipped (USVFS)
    };
    std::ifstream f;
    const char* used = nullptr;
    for (const char* p : kPaths) {
        f.open(p);
        if (f.is_open()) { used = p; break; }
        f.clear();
    }
    if (!used) { logger::info("SKELMAP: no PPB_skeletons.txt — runtime skeleton map inactive."); return; }

    ObjectHold::ClearNpcCapOverrides();
    ObjectHold::SetSculptTarget("", -1);             // gate defaults OFF each parse
    int nRace = 0, nNpc = 0, nBad = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (const auto h = line.find('#'); h != std::string::npos) line.erase(h);
        std::istringstream ls(line);
        std::string kind;
        if (!(ls >> kind)) continue;

        if (kind == "raceBias") {
            std::string edid; float mult = 1.f;
            if (!(ls >> edid >> mult) || mult < 0.5f || mult > 1.5f) { ++nBad; continue; }
            auto* rcb = RE::TESForm::LookupByEditorID<RE::TESRace>(edid);
            if (!rcb) {
                logger::info("SKELMAP: raceBias race '{}' not found — line skipped.", edid);
                ++nBad; continue;
            }
            ObjectHold::AddRaceBias(rcb, mult);
            logger::info("SKELMAP: raceBias {} x{:.3f} (flat capsule bias — fur-race measurement correction)",
                         edid, mult);
            continue;
        }
        if (kind == "race") {
            std::string edid, path;
            if (!(ls >> edid)) { ++nBad; continue; }
            // Path = the REST of the line: skeleton paths contain spaces ("Character Assets
            // Female"), so `>>` truncated at the first space and repointed races at a
            // nonexistent NIF — actors whose skeleton can't load never build 3D (the
            // 2026-07-17 invisible-M'rissi bug; the SKELMAP receipt showed the cut path).
            std::getline(ls, path);
            const auto pb = path.find_first_not_of(" \t\r");
            if (pb == std::string::npos) { ++nBad; continue; }
            const auto pe = path.find_last_not_of(" \t\r");
            path = path.substr(pb, pe - pb + 1);
            auto* race = RE::TESForm::LookupByEditorID<RE::TESRace>(edid);
            if (!race) {
                logger::info("SKELMAP: race '{}' not found — line skipped.", edid);
                ++nBad; continue;
            }
            // COPY the old path before SetModel: for a custom race this BSFixedString may be
            // the pool's only reference — SetModel releases it, and logging the raw pointer
            // afterwards reads freed memory (review 2026-07-17).
            const char* beforeRaw = race->skeletonModels[RE::SEXES::kFemale].GetModel();
            const std::string before = beforeRaw ? beforeRaw : "<null>";
            race->skeletonModels[RE::SEXES::kFemale].SetModel(path.c_str());
            logger::info("SKELMAP: {} FEMALE skeleton '{}' -> '{}'", edid, before, path);
            ++nRace;
        } else if (kind == "sculpt") {
            // SCULPT GATE (2026-07-17): `sculpt <skeleton filename>` — during a capsule dial
            // session, global cap* knobs land ONLY on actors whose race female skeleton is this
            // NIF. Finished/baked work on every other skeleton stays untouched.
            std::string f;
            std::getline(ls, f);
            const auto fb = f.find_first_not_of(" \t\r");
            if (fb == std::string::npos) { ++nBad; continue; }
            const auto fe = f.find_last_not_of(" \t\r");
            f = f.substr(fb, fe - fb + 1);
            // Optional trailing SLOT name (`sculpt <nif> head`): gates only that slot, so
            // non-target actors keep their body dressing (review 2026-07-17). A token with a
            // '.' is part of the filename, never a slot.
            int sslot = -1;
            if (const auto sp = f.find_last_of(" \t");
                sp != std::string::npos && f.find('.', sp) == std::string::npos) {
                const std::string tok = f.substr(sp + 1);
                sslot = SlotIndexByName(tok);
                if (sslot < 0) {
                    logger::info("SKELMAP: sculpt slot '{}' unknown — line skipped.", tok);
                    ++nBad; continue;
                }
                f = f.substr(0, f.find_last_not_of(" \t", sp) + 1);
            }
            ObjectHold::SetSculptTarget(f.c_str(), sslot);
            logger::info("SKELMAP: SCULPT GATE ON — global cap* knobs ({}) apply ONLY to actors on '{}'",
                         sslot < 0 ? "ALL slots" : "one slot", f);
        } else if (kind == "npcCap") {
            std::string ref, slotName;
            int child = -1;
            float a[3], b[3], r;
            // Plugin names contain spaces ("Yvanni Follower.esp") — a plain `>>` truncates the ref
            // at the first space and LookupForm then fails (same class as the race-path bug).
            // Read tokens until the ref ends in an esp/esm/esl extension; the FormID's "LOCALHEX:"
            // prefix never has a space, so the boundary is unambiguous.
            if (!(ls >> ref)) { ++nBad; continue; }
            auto endsWithExt = [](const std::string& s) {
                if (s.size() < 4) return false;
                auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
                return lc(s[s.size() - 4]) == '.' && lc(s[s.size() - 3]) == 'e' &&
                       lc(s[s.size() - 2]) == 's' &&
                       (lc(s[s.size() - 1]) == 'p' || lc(s[s.size() - 1]) == 'm' || lc(s[s.size() - 1]) == 'l');
            };
            std::string tok;
            while (!endsWithExt(ref) && (ls >> tok)) ref += " " + tok;
            if (!endsWithExt(ref)) { ++nBad; continue; }
            if (!(ls >> slotName >> child >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2] >> r)) { ++nBad; continue; }
            const auto colon = ref.find(':');
            const int  slot  = SlotIndexByName(slotName);
            if (colon == std::string::npos || slot < 0) { ++nBad; continue; }
            const std::uint32_t local = std::strtoul(ref.substr(0, colon).c_str(), nullptr, 16);
            auto* dh   = RE::TESDataHandler::GetSingleton();
            auto* form = dh ? dh->LookupForm(local, ref.substr(colon + 1)) : nullptr;
            if (!form) {
                logger::info("SKELMAP: npcCap ref '{}' did not resolve — line skipped.", ref);
                ++nBad; continue;
            }
            ObjectHold::AddNpcCapOverride(form->GetFormID(), slot, child, a, b, r);
            logger::info("SKELMAP: npcCap {:08X} ({}) {}.C{} r={:.2f}", form->GetFormID(), ref, slotName, child, r);
            ++nNpc;
        } else {
            ++nBad;
        }
    }
    // SCULPT GATE receipt (review 2026-07-17): a typo'd/stale sculpt filename silently kills
    // every global dial for the session ("knobs do nothing"). Count matching races and say so.
    if (*ObjectHold::SculptTarget()) {
        int matched = 0;
        if (auto* dh = RE::TESDataHandler::GetSingleton())
            for (auto* rc : dh->GetFormArray<RE::TESRace>())
                if (rc)
                    if (const char* mm = rc->skeletonModels[RE::SEXES::kFemale].GetModel(); mm && *mm) {
                        const char* bb = mm;
                        for (const char* p = mm; *p; ++p)
                            if (*p == '\\' || *p == '/') bb = p + 1;
                        if (_stricmp(bb, ObjectHold::SculptTarget()) == 0) ++matched;
                    }
        if (matched == 0)
            logger::info("SKELMAP: !! SCULPT GATE matches NO race — every global cap* dial is DEAD "
                         "this session (typo'd filename?)");
        else
            logger::info("SKELMAP: sculpt gate covers {} race(s).", matched);
    }
    logger::info("SKELMAP: '{}' applied — {} race repoint(s), {} npcCap override(s), {} bad line(s).",
                 used, nRace, nNpc, nBad);
}

// Report which sibling mods PPB cooperates with are actually loaded, so the
// log makes "everything running smooth" obvious at a glance. By kPostPostLoad
// every SKSE plugin DLL is mapped, so GetModuleHandleA is reliable here.
static void LogEnvironment()
{
    auto present = [](const char* dll) -> const char* {
        return ::GetModuleHandleA(dll) ? "loaded" : "ABSENT";
    };
    logger::info("Environment: PLANCK(activeragdoll.dll)={}  HIGGS(higgs_vr.dll)={}  CBPC(cbp.dll)={}  AIHands(AIHands.dll)={}",
                 present("activeragdoll.dll"), present("higgs_vr.dll"), present("cbp.dll"), present("AIHands.dll"));
    logger::info("Environment (Body-Scale): SKEE(skeevr.dll)={} SKEE(skee64.dll)={}",
                 present("skeevr.dll"), present("skee64.dll"));
    // Fingerprints (Report 17A-14, the highest-leverage triage addition): file size +
    // PE link timestamp per sibling DLL — a behavior regression after a mod update
    // correlates instantly against the previous session's log header. (HIGGS also logs
    // its own build number via Interop::AcquireHiggs right after this.)
    auto detail = [](const char* dll) -> std::string {
        HMODULE h = ::GetModuleHandleA(dll);
        if (!h) return "ABSENT";
        std::uint64_t size = 0;
        char path[MAX_PATH]{};
        if (::GetModuleFileNameA(h, path, MAX_PATH)) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
                size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
        std::uint32_t stamp = 0;
        auto* base = reinterpret_cast<const std::uint8_t*>(h);
        auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) stamp = nt->FileHeader.TimeDateStamp;
        }
        char out[96];
        std::snprintf(out, sizeof out, "%llu bytes, linkstamp 0x%08X",
                      static_cast<unsigned long long>(size), stamp);
        return out;
    };
    logger::info("Environment fingerprints: PLANCK={}  HIGGS={}  CBPC={}  FSMP(hdtSMP64.dll)={}",
                 detail("activeragdoll.dll"), detail("higgs_vr.dll"), detail("cbp.dll"), detail("hdtSMP64.dll"));
}

static void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
{
    switch (msg->type) {
    case SKSE::MessagingInterface::kPostLoad:
        logger::info("PostLoad.");
        FsmpLink::Register();   // MUST precede the SMP engine's kPostPostLoad MSG_STARTUP dispatch
        break;

    case SKSE::MessagingInterface::kPostPostLoad:
        logger::info("PostPostLoad.");
        LogEnvironment();
        Interop::AcquireHiggs();   // grab-gate handshake (idempotent; retried at kDataLoaded)
        Interop::AcquireSkee();    // Body-Scale morph handshake (idempotent; retried at kDataLoaded)
        PerfSys::RegisterHiggs();  // perf filter + frame callback (idempotent; retried at kDataLoaded)
        DismemberGuard::AcquirePlanck();  // PLANCK ignore-API handshake (idempotent; retried at kDataLoaded)
        break;

    case SKSE::MessagingInterface::kDataLoaded:
        // Install the chain-over hook at VR 0xB266AB — the CALL inside
        // BShkbAnimationGraph::UpdateAnimation to hkbRagdollDriver::driveToPose.
        //
        // MUST install at kDataLoaded, not SKSEPlugin_Load: PLANCK patches the
        // same site via Write5Call during its own SKSEPlugin_Load, so by
        // kDataLoaded write_call<5> picks up the current head of the chain
        // (PLANCK's hook, or AIHands' if it installed first) as the previous
        // target and we land ON TOP of the chain. Our hook runs first, then
        // tail-calls the saved head.
        logger::info("DataLoaded - installing PPB chain hooks.");
        Hooks::InstallPreDriveHook();
        // READ-ONLY check of the post-physics seam address (no patch). See ADDR-CHECK lines.
        Hooks::VerifyPostPhysicsAddresses();
        // Heel fix v3: the inner hkaRagdollRigidBodyController::driveToPose call — the ONLY spot where a
        // worldFromModel bias actually reaches the bone targets (track/character writes proven no-ops).
        Hooks::InstallInnerDriveHook();
        // Heel fix v4: the compensation half — after postPhysics maps the raised ragdoll back into the
        // pose, subtract heelZ from the root bone so the rendered body stays planted on her heels.
        Hooks::InstallPostPhysicsDriverHook();
        // Diagnostic (2026-07-08): the physics-step timer. Chains ON TOP of HIGGS at 0xDFB722 with a
        // mandatory p[0]!=0xE8 self-abort — feeds Diag's step ms only while `perf` is armed. Read-only.
        Hooks::InstallPhysicsStepHook();
        RegisterConsoleCommands();  // 'HeelFix' / 'hf' + 'CapFix' / 'capfix' + 'statue' + 'probe' + 'perf' + 'capdis' + 'nfing' + 'jtrack'
        // RUNTIME SKELETON MAP (2026-07-17): repoint race female skeletons + load per-NPC capsule
        // overrides from PPB_skeletons.txt. Runs at kDataLoaded — every plugin's record winners are
        // final, no actor 3D exists yet, and nothing else in this load order writes skeleton paths.
        ApplySkeletonIni();   // user-facing skeleton assignment (runs FIRST)
            ApplySkeletonMap();
        if (auto* mcSrc = SKSE::GetModCallbackEventSource())
            mcSrc->AddEventSink(&g_obodySink);   // OBody push re-fit (Obody_ApplyMorph)
        Interop::AcquireHiggs();    // grab-gate handshake retry (the AIHands kPostPostLoad+kDataLoaded pattern)
        Interop::AcquireSkee();     // Body-Scale morph handshake retry (SKEE may register after kPostPostLoad)
        PerfSys::RegisterHiggs();   // perf filter + frame callback retry (idempotent)
        Diag::RegisterFrameCallback();  // wire the perf steps/frame boundary to HIGGS (idempotent; retried on perf-arm)
        ObjectHold::InitHeelFixDefault();  // startup state from the tuning file (heelFix 1 = auto-on for all NPCs)
        ArmIK::ResolveHeelsFix();          // Heels Fix (HeelsFix.esp) is the sole heel authority — resolve its spell now (no-op if absent)
        DismemberGuard::AcquirePlanck();   // retry (PLANCK registers its listener during its own load)
        DismemberGuard::AcquireDfNgd();    // DF + NGD plugin APIs (retried at kPostLoadGame below)
        DismemberGuard::Install();         // TESDeathEvent sink + DF ProcessDismemberment defer-hook
        logger::info("PPB init complete. Watching for first driveToPose fire "
                     "(expect FIRST-FIRE, then HEELFIX/CapFix/PivFix lines near any humanoid NPC).");
        break;

    case SKSE::MessagingInterface::kPostLoadGame:
        // DF/NGD headers acquire their APIs at kPostLoadGame/kNewGame — retry here so IsHead()
        // detection is live even if the APIs weren't ready at kDataLoaded. Idempotent.
        DismemberGuard::AcquireDfNgd();
        break;

    case SKSE::MessagingInterface::kNewGame:
        DismemberGuard::AcquireDfNgd();   // same API retry on a fresh game
        // 2026-07-13 review LOW: New Game does NOT fire kPreLoadGame, so without this the
        // FormID-keyed latches from a save played earlier this session survive into the
        // new game — a persistent/recycled FormID then wears the previous save's measured
        // scale for the full re-measure window. Same teardown; the body-removal clears are
        // no-ops at the menu (no live world/bodies) and forget their stale pointers safely.
        [[fallthrough]];
    case SKSE::MessagingInterface::kPreLoadGame:
        // The Havok world is torn down + rebuilt across a load. Drop every cached
        // pointer into it (constraint instances, drivers) so we never touch a
        // freed world after the load.
        ObjectHold::PivFixClearAll();   // per-actor pivot state (sticky auto-seats included)
        ObjectHold::PivDescaleClearAll();  // descaled-instance pointers dangle across loads
        ObjectHold::PivReScaleClearAll();  // re-scale scaled-instance pointers dangle across loads too
        ArmIK::ClearBindRelCache();     // per-driver bind-relation / clavicle-follow cache
        ArmIK::ClearStatueSet();        // statue flags don't survive a load
        ArmIK::ClearPoseConformCache(); // pose-conform node/ragdoll map (root + ragdoll pointers dangle across a load)
        Diag::ClearOnLoad();            // drop the cached Havok world (rebuilt across a load) + any pending spike
        PerfSys::ClearOnLoad();         // wipe per-actor LOD state (fresh bodies reload from the NIF fully enabled)
        NpcFinger::ClearOnLoad();       // remove the finger-test capsules while the world is still alive, drop the pin
        HandBox::ClearOnLoad();         // remove the hand boxes + drop slab baselines (HIGGS rebuilds its bodies)
        GrabDiag::CapFixClearOnLoad();  // FormID-keyed latches (measured effScale + identities) must not cross saves
        DismemberGuard::ClearOnLoad();  // actor handles + PLANCK-ignore bookkeeping don't survive a load
        break;

    default:
        break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    InitLogging();
    SKSE::Init(skse);

    logger::info("==================================================");
    logger::info("PPB v0.1.0 (Precision Physic Bodies) loaded.");
    if (auto dir = SKSE::log::log_directory()) {
        logger::info("Log file: {}\\PPB.log", dir->string());
    }
    logger::info("==================================================");

    const auto msg = SKSE::GetMessagingInterface();
    if (msg) {
        msg->RegisterListener(OnSKSEMessage);
    }

    // Papyrus bridge: PPB_Native.SetStatuePose (the A-pose statue spell backend) +
    // SetHeelFix / ToggleHeelFix. See Natives.cpp.
    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(PapyrusNatives::Register);
    }

    return true;
}
