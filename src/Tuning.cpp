#include "PCH.h"
#include "Tuning.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>   // mtime precheck (2026-07-13 perf: parse only on real file change)
#include <fstream>      // live-tunable config (hot-reload)
#include <sstream>      // live-tunable config (hot-reload)
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>   // logVerbose: live spdlog-level flip (2026-07-13 log demotion)

namespace logger = SKSE::log;

// ============================================================================
//  Tuning.cpp — the PPB half of AIHands' GrabTune parser (PalmCollider.cpp),
//  ported verbatim: same key names, same 1 Hz CapFixPollFile generation
//  mechanism. Only the file name/path pair changed (PPB_tuning.txt) plus the
//  per-joint world-Z lift knobs (piv*UpU) and the finger tracking knobs.
//
//  2026-07-08 — TABLE-DRIVEN PARSER. The original was 8 independent else-if
//  chains, hand-split to dodge MSVC C1061 (~128 nested-block limit). The wave-2
//  torso bake adds 400 child keys, which NO chain form can carry. The table
//  below is semantically identical to the chains it replaces:
//     * every key name is unique (the chains already relied on this)
//     * unknown keys are ignored
//     * duplicate keys are last-wins (the file loop overwrites)
//     * unparsable lines are skipped by `if (!(ss >> key >> val)) continue;`
//  ...and it is O(1) per line instead of ~277 string compares.
// ============================================================================
namespace ObjectHold {

    GrabTune g_tune;                  // mutable — overwritten by the hot-reload

    namespace {

        struct TuneKey    { const char* name; float* p; bool snap; };
        struct ChildArray { const char* stem; CapChild* arr; int n; };

// `snap` = the field participates in the 1 Hz change-detection snapshot that arms a new CapFix
// generation. heelFix is EXCLUDED (the console `hf` owns it live), as are the finger knobs (the
// per-frame writer reads them directly — they must never re-dress all 12 capsule slots).
#define PK(n)        TuneKey{ #n, &g_tune.n, true }
#define PK_NOSNAP(n) TuneKey{ #n, &g_tune.n, false }
#define PK8(s)       PK(s##Enable), PK(s##AX), PK(s##AY), PK(s##AZ), PK(s##BX), PK(s##BY), PK(s##BZ), PK(s##R)
#define PK4(s)       PK(s##Enable), PK(s##X),  PK(s##Y),  PK(s##Z)

        struct TuneIndex {
            std::unordered_map<std::string, float*> byName;      // every key -> its field
            std::vector<float*>                     snapFields;  // the change-watch set (ordered, stable)
        };

        const TuneIndex& Index() {
            static TuneIndex t = [] {
                TuneIndex x;
                const TuneKey kScalars[] = {
                    PK_NOSNAP(heelFix),                                   // console `hf` owns it — never bumps the gen
                    PK8(capHand), PK8(capFore), PK8(capUpper), PK8(capHead),
                    PK8(capHandC1), PK8(capHandC2), PK8(capHandC3), PK8(capHandC4), PK8(capHandC5),
                    PK8(capSpine0), PK8(capSpine1), PK8(capSpine2), PK8(capNeck),
                    PK8(capThigh), PK8(capCalf), PK8(capFoot), PK8(capCom),
                    PK8(capUpperC1), PK8(capUpperC2), PK8(capUpperC3), PK8(capForeC1),
                    // 2026-08-22 male-sculpt limb headroom + the two landmark CHORD probes. Registered
                    // with PK8 (SNAPPED) like every sibling capsule knob: an edit must arm a CapFix gen.
                    PK8(capUpperC4), PK8(capUpperC5), PK8(capForeC2), PK8(capForeC3),
                    PK8(capHandC6), PK8(capHandC7),
                    PK8(capThighC7), PK8(capThighC8), PK8(capThighC9),
                    PK8(capCalfC5), PK8(capCalfC6), PK8(capFootC4), PK8(capFootC5),
                    PK8(capThighC1), PK8(capThighC2), PK8(capThighC3), PK8(capThighC4), PK8(capThighC5), PK8(capThighC6),
                    PK8(capCalfC1), PK8(capCalfC2), PK8(capCalfC3), PK8(capCalfC4),
                    PK8(capFootC1), PK8(capFootC2), PK8(capFootC3),
                    PK(capMirrorL), PK(pivGrabGate),
                    PK4(pivWrist), PK4(pivElbow), PK4(pivShoulder),
                    PK4(pivSpine0), PK4(pivSpine1), PK4(pivSpine2), PK4(pivNeck), PK4(pivHead),
                    PK4(pivHip), PK4(pivKnee), PK4(pivAnkle),
                    PK(pivAutoSeat), PK(pivClavFollow), PK(capAutoFit),
                    PK(pivWristUpU), PK(pivElbowUpU), PK(pivShoulderUpU),
                    PK(pivSpine0UpU), PK(pivSpine1UpU), PK(pivSpine2UpU),
                    PK(pivNeckUpU), PK(pivHeadUpU), PK(pivHipUpU), PK(pivKneeUpU), PK(pivAnkleUpU),
                    // 2026-07-08 NEW (not snapshotted — the per-frame finger writer reads them live)
                    PK_NOSNAP(fingerCapTrack), PK_NOSNAP(fingerCapR),
                    // 2026-07-08 perf diagnostic: lodSpike gate (read by Diag's deferred disableChild spike,
                    // never a capsule dial) — excluded from the change-detection snapshot.
                    PK_NOSNAP(lodSpike),
                    // 2026-07-09 PERF SYSTEM (PerfSys.cpp) — all excluded from the capsule change-watch.
                    PK_NOSNAP(perfPreset), PK_NOSNAP(filterSelfThigh), PK_NOSNAP(filterCrossPelvis),
                    PK_NOSNAP(lodEnable), PK_NOSNAP(lodNearDist), PK_NOSNAP(lodFarDist),
                    PK_NOSNAP(lodHystPct), PK_NOSNAP(lodSettleFrames), PK_NOSNAP(lodStageBits),
                    // 2026-08-22 RAYCAST TELEMETRY (RayTel.cpp) — a diagnostic arm flag, never a
                    // capsule dial: NOSNAP so flipping it mid-scene cannot re-dress the slots.
                    PK_NOSNAP(rayTel),
                    // 2026-07-10 CONTACT DECIMATION (Diag.cpp contact listener) — hot-swappable live.
                    PK_NOSNAP(perfContactKeep),
                    // 2026-07-09 PIVOT DESCALE (PivFix.cpp) — SUPERSEDED by pivReScale (default off, inert).
                    PK_NOSNAP(pivDescale),
                    // 2026-07-14 UNIFORM RE-SCALE (PivFix.cpp PivScaleCorrect) — correctness fix, default on;
                    // read live so a flip can disable the mis-scale fixer without a restart.
                    PK_NOSNAP(pivReScale),
                    // 2026-07-15 DIAGNOSTIC-ONLY gate (PivFix.cpp) — 0 = measure+log, skip the pivot write.
                    PK_NOSNAP(pivReScaleApply),
                    // 2026-07-10 JOINT-TRACK TRIGGER (PivFix.cpp) — edge-detected live; a trigger
                    // flip must never re-dress the capsule slots.
                    PK_NOSNAP(jtrackNow),
                    // 2026-07-10 XP32 POSE-CONFORM (PPBHook.cpp) — read live per frame / edge-
                    // detected; excluded from the capsule change-watch.
                    PK_NOSNAP(poseConform), PK_NOSNAP(poseConformDump), PK_NOSNAP(poseConformRoot), PK_NOSNAP(poseConformEveryN),
                    // 2026-07-09 NPC FINGER TEST v2 (NpcFingerTest.cpp) — read live per frame,
                    // excluded from the capsule change-watch.
                    PK_NOSNAP(npcFingerEnable), PK_NOSNAP(npcFingerCount), PK_NOSNAP(npcFingerR), PK_NOSNAP(npcFingerOwnBody), PK_NOSNAP(npcFingerFollow),
                    PK_NOSNAP(npcTailTest), PK_NOSNAP(npcTailR), PK_NOSNAP(npcTailOffScale), PK_NOSNAP(nodeCensusNow),
                    // 2026-07-11 FSMP PUSH (FsmpLink.cpp) -- physics-thread reads, live.
                    PK_NOSNAP(fsmpPush), PK_NOSNAP(fsmpPushForce), PK_NOSNAP(fsmpPushMaxForce),
                    PK_NOSNAP(fsmpPushMinDispU),
                    // 2026-07-13 MULTI-RIG (NpcFingerTest.cpp): global push multiplier +
                    // the always-on garment-rig master switch — read live per frame.
                    PK_NOSNAP(fsmpPushMult), PK_NOSNAP(npcFollower), PK_NOSNAP(fsmpFlexCompat),
                    PK_NOSNAP(npcGenCap), PK(npcGenMassKg), PK(npcGenR), PK(npcGenAlpha),
                    PK_NOSNAP(genBend), PK_NOSNAP(genBendPlayer), PK_NOSNAP(genBendMax),
                    PK_NOSNAP(genBendUpMs), PK_NOSNAP(genBendHoldSec), PK_NOSNAP(genBendDecaySec),
                    PK_NOSNAP(genBendArousal), PK_NOSNAP(genBendTouchU),
                    PK_NOSNAP(npcRigRangeU), PK_NOSNAP(npcRigRangeHystU), PK_NOSNAP(npcRigMaxActors),
                    PK_NOSNAP(apiTouch), PK_NOSNAP(apiHz), PK_NOSNAP(apiTouchU), PK_NOSNAP(apiExitPadU),
                    PK_NOSNAP(apiMaxActors), PK_NOSNAP(apiRangeU), PK_NOSNAP(apiFistTipPalmU),
                    PK_NOSNAP(apiEvents), PK_NOSNAP(apiLog), PK_NOSNAP(apiHairTarget),
                    PK_NOSNAP(apiRawEvents), PK_NOSNAP(apiWeaponRMaxU), PK_NOSNAP(apiObjectRMaxU),
                    PK_NOSNAP(apiSubRegionInEvent),
                    PK_NOSNAP(apiSuppressHeldHand),
                    PK_NOSNAP(apiDwellS), PK_NOSNAP(apiDwellHeadS), PK_NOSNAP(apiDwellComS),
                    PK_NOSNAP(apiDwellSensorS), PK_NOSNAP(apiDwellTailS),
                    PK_NOSNAP(higgsPokeFix),
                    PK_NOSNAP(npcFingerTipU), PK_NOSNAP(npcFingerMassKg), PK_NOSNAP(npcGarmentMassKg), PK_NOSNAP(npcTailMassKg), PK_NOSNAP(npcSilentVelMS), PK_NOSNAP(npcGarmentMat), PK_NOSNAP(npcFingerAlpha),
                    PK_NOSNAP(npcFingerCurlGain), PK_NOSNAP(npcFingerCurlDecay), PK_NOSNAP(npcFingerCurlMax), PK_NOSNAP(npcFingerCurlMode), PK_NOSNAP(npcFingerCurlLagGate),
                    PK_NOSNAP(fsmpMassScale), PK_NOSNAP(fsmpContactGate), PK_NOSNAP(fsmpContactDevU), PK_NOSNAP(fsmpContactHoldMS),
                    PK_NOSNAP(npcFingerVsClutter), PK_NOSNAP(npcFingerVsWorld), PK_NOSNAP(npcFingerVsNpc),
                    PK_NOSNAP(npcFingerSnapU), PK_NOSNAP(npcFingerMaxVel), PK_NOSNAP(npcFingerGravity),
                    PK_NOSNAP(npcFingerLog),
                    // 2026-07-13 PERF log demotion: flips the spdlog level (CapFixPollFile applies
                    // it) — a log verbosity change must never re-dress the capsule slots.
                    PK_NOSNAP(logVerbose),
                    // 2026-07-13 measured-scale master switch — NOSNAP (edge-detected in
                    // CapFixPollFile bumps the gen itself; snapshot would double-fire).
                    PK_NOSNAP(measuredScaleEnable), PK_NOSNAP(measuredScaleRefId), PK_NOSNAP(refCapNow),
                    // 2026-07-14 BODY SCALE (CapFix CapRegionScaleOf) — master gate + per-region gains +
                    // clamp bounds are SNAP so a factor/clamp edit bumps the gen and re-dresses every driven
                    // NPC; the dump trigger is NOSNAP (a diagnostic flip must not itself re-dress).
                    PK(bodyScale), PK(bodyScaleChest), PK(bodyScaleBreasts), PK(bodyScaleBelly),
                    PK(bodyScaleWaist), PK(bodyScaleButt), PK(bodyScaleThighs), PK(bodyScaleArms),
                    PK(bodyScaleHead), PK(bodyScaleClampLo), PK(bodyScaleClampHi), PK_NOSNAP(bodyScaleDump),
                    // 2026-07-18 morph translation + shrink boost + body material — all SNAP so an edit
                    // re-dresses every driven NPC immediately (that's the point of dialing them live).
                    PK(bodyShiftBreastsUpU), PK(bodyShiftBreastsFwdU), PK(bodyShiftButtBackU),
                    PK(bodyScaleLoBoost), PK(npcBodyMat),
                    // 2026-07-18 ROUTE B measured trueShape: master + neutrals re-dress (PK);
                    // bands + dump are calibration-session live reads (NOSNAP).
                    PK(meshShape), PK(meshNeutralChest), PK(meshNeutralBreasts), PK(meshNeutralBelly),
                    PK(meshNeutralWaist), PK(meshNeutralButt), PK(meshNeutralThighs), PK(meshNeutralArms),
                    PK_NOSNAP(meshShapeDump), PK_NOSNAP(meshArmFrac), PK_NOSNAP(meshMarkers), PK_NOSNAP(meshBoneTest),
                    PK(boneShape), PK(boneNeutralChest), PK(boneNeutralBreasts), PK(boneNeutralBelly),
                    PK(boneNeutralWaist), PK(boneNeutralButt), PK(boneNeutralThighs), PK(boneNeutralArms),
                    PK(boneNeutralCup), PK(boneNeutralBreastZ), PK(boneNeutralBreastAbs), PK(boneNeutralButtAbs),
                    PK_NOSNAP(lmNipU), PK_NOSNAP(lmNipV), PK_NOSNAP(lmBrUpU), PK_NOSNAP(lmBrUpV),
                    PK_NOSNAP(lmBrDnU), PK_NOSNAP(lmBrDnV), PK_NOSNAP(lmChestU), PK_NOSNAP(lmChestV),
                    PK_NOSNAP(lmBellyU), PK_NOSNAP(lmBellyV), PK_NOSNAP(lmWaistU), PK_NOSNAP(lmWaistV),
                    PK_NOSNAP(lmButtU), PK_NOSNAP(lmButtV),
                    PK_NOSNAP(lmLegAU), PK_NOSNAP(lmLegAV), PK_NOSNAP(lmLegBU), PK_NOSNAP(lmLegBV),
                    PK_NOSNAP(lmArmAU), PK_NOSNAP(lmArmAV), PK_NOSNAP(lmArmBU), PK_NOSNAP(lmArmBV),
                    PK_NOSNAP(lmNoseU), PK_NOSNAP(lmNoseV), PK_NOSNAP(lmChinU), PK_NOSNAP(lmChinV),
                    PK(lmButtFit), PK_NOSNAP(lmButtOutU), PK_NOSNAP(lmButtPosGain),
                    PK_NOSNAP(lmButtNeutX), PK_NOSNAP(lmButtNeutY), PK_NOSNAP(lmButtNeutZ),
                    // NEUTRAL SHAPE (Lydia) — 21 floats, live so it can be re-captured
                    PK_NOSNAP(lmNeutNipX),   PK_NOSNAP(lmNeutNipY),   PK_NOSNAP(lmNeutNipZ),
                    PK_NOSNAP(lmNeutBrUpX),  PK_NOSNAP(lmNeutBrUpY),  PK_NOSNAP(lmNeutBrUpZ),
                    PK_NOSNAP(lmNeutBrDnX),  PK_NOSNAP(lmNeutBrDnY),  PK_NOSNAP(lmNeutBrDnZ),
                    PK_NOSNAP(lmNeutChestX), PK_NOSNAP(lmNeutChestY), PK_NOSNAP(lmNeutChestZ),
                    PK_NOSNAP(lmNeutBellyX), PK_NOSNAP(lmNeutBellyY), PK_NOSNAP(lmNeutBellyZ),
                    PK_NOSNAP(lmNeutWaistX), PK_NOSNAP(lmNeutWaistY), PK_NOSNAP(lmNeutWaistZ),
                    PK_NOSNAP(lmNeutButtX),  PK_NOSNAP(lmNeutButtY),  PK_NOSNAP(lmNeutButtZ),
                    // UV ReShape — master switch, ring gains, breast model
                    PK(lmReShape),
                    PK(lmGainMChest), PK(lmGainMBelly), PK(lmGainMWaist),
                    PK(lmGainMButt), PK(lmGainMThigh), PK(lmGainMArm),
                    PK_NOSNAP(lmGainChest), PK_NOSNAP(lmGainBelly), PK_NOSNAP(lmGainWaist),
                    PK_NOSNAP(lmGainButt),  PK_NOSNAP(lmClampLo),   PK_NOSNAP(lmClampHi),
                    PK_NOSNAP(lmGainThigh), PK_NOSNAP(lmGainArm),
                    PK_NOSNAP(lmNeutLegChord), PK_NOSNAP(lmNeutArmChord),
                    // MALE NEUTRAL SHAPE (2026-08-21) — 14 floats, live like the female set
                    PK_NOSNAP(lmNeutMChestX), PK_NOSNAP(lmNeutMChestY), PK_NOSNAP(lmNeutMChestZ),
                    PK_NOSNAP(lmNeutMBellyX), PK_NOSNAP(lmNeutMBellyY), PK_NOSNAP(lmNeutMBellyZ),
                    PK_NOSNAP(lmNeutMWaistX), PK_NOSNAP(lmNeutMWaistY), PK_NOSNAP(lmNeutMWaistZ),
                    PK_NOSNAP(lmNeutMButtX),  PK_NOSNAP(lmNeutMButtY),  PK_NOSNAP(lmNeutMButtZ),
                    PK_NOSNAP(lmNeutMLegChord), PK_NOSNAP(lmNeutMArmChord),
                    PK_NOSNAP(lmNeutNoseChin), PK_NOSNAP(lmGainHead),
                    // ReTouch ghost zones + mouth touch
                    PK_NOSNAP(ghostZones), PK_NOSNAP(ghostViz), PK_NOSNAP(ghostRangeU),
                    PK_NOSNAP(ghostBreastPadU), PK_NOSNAP(ghostButtPadU),
                    PK_NOSNAP(mouthGhostAX), PK_NOSNAP(mouthGhostAY), PK_NOSNAP(mouthGhostAZ),
                    PK_NOSNAP(mouthGhostBX), PK_NOSNAP(mouthGhostBY), PK_NOSNAP(mouthGhostBZ),
                    PK_NOSNAP(mouthGhostR), PK_NOSNAP(mouthEnterU), PK_NOSNAP(mouthExitU),
                    PK_NOSNAP(mouthPhonemeIdx), PK_NOSNAP(mouthPhonemeMax),
                    PK_NOSNAP(mouthPhoneme2Idx), PK_NOSNAP(mouthPhoneme2Max), PK_NOSNAP(mouthNeedFinger),
                    PK_NOSNAP(mouthDeepChinU), PK_NOSNAP(mouthDeepPalU),
                    PK_NOSNAP(mouthThroatChild), PK_NOSNAP(mouthThroatU),
                    PK_NOSNAP(mouthKhaE1), PK_NOSNAP(mouthKhaE2), PK_NOSNAP(mouthKhaE3),
                    PK_NOSNAP(mouthKhaC1), PK_NOSNAP(mouthKhaC2),
                    PK_NOSNAP(mouthArgE1), PK_NOSNAP(mouthArgE2), PK_NOSNAP(mouthArgE3),
                    PK_NOSNAP(mouthArgC1), PK_NOSNAP(mouthArgC2), PK_NOSNAP(mouthBeastEnterU),
                    PK_NOSNAP(mouthRampIn), PK_NOSNAP(mouthRampOut), PK_NOSNAP(mouthFingerBox),
                    PK_NOSNAP(touchProbe), PK_NOSNAP(touchProbeRangeU),
                    // 2026-08-19 ORIFICE DRIVE (Orifice.cpp) — read live per frame, and a gape
                    // dial must never re-dress capsules, so the whole block is NOSNAP.
                    PK_NOSNAP(orificeEnable), PK_NOSNAP(orificeVaginal), PK_NOSNAP(orificeAnal),
                    PK_NOSNAP(orificeOral), PK_NOSNAP(orificeGateTol), PK_NOSNAP(orificeEaseHz),
                    PK_NOSNAP(orificeRadiusGain), PK_NOSNAP(orificeRefRadU),
                    PK_NOSNAP(orificeFingerRadU),
                    PK_NOSNAP(orificeVagMinU), PK_NOSNAP(orificeVagMaxU),
                    PK_NOSNAP(orificeAnalMinU), PK_NOSNAP(orificeAnalMaxU),
                    PK_NOSNAP(orificeRangeU), PK_NOSNAP(orificeForeignHoldS),
                    PK_NOSNAP(orificeLog),
                    // 2026-07-26 DISMEMBER GUARD (DF/NGD ↔ PLANCK) — NOSNAP (never re-dress capsules)
                    PK_NOSNAP(dgEnable), PK_NOSNAP(dgGraceDeadS), PK_NOSNAP(dgLog),
                    PK_NOSNAP(dgDeathIgnore), PK_NOSNAP(dgCloneStrip), PK_NOSNAP(dgStripDelayS),
                    PK_NOSNAP(dgDeferDf), PK_NOSNAP(dgHeadTrack),
                    PK_NOSNAP(dgDeathCut), PK_NOSNAP(dgDeathCutDelayS), PK_NOSNAP(dgDeathNodeTries),
                    PK_NOSNAP(dgHitLocated), PK_NOSNAP(dgHitMaxDistU),
                    PK_NOSNAP(dgHeadSkel), PK_NOSNAP(dgHeadSkelHoldS), PK_NOSNAP(dgHeadPark), PK_NOSNAP(dgHeadStripHair), PK_NOSNAP(dgHeadGrabFix), PK_NOSNAP(dgHeadPlanck), PK_NOSNAP(dgVictimPlanck), PK_NOSNAP(genProbe), PK(maleGeometry), PK_NOSNAP(handBoxRelAlpha), PK_NOSNAP(handBoxPhaseLog), PK_NOSNAP(handBoxStepDt), PK_NOSNAP(handBoxTrack), PK_NOSNAP(handBoxWarp), PK_NOSNAP(playerWand), PK_NOSNAP(playerWandR), PK_NOSNAP(playerWandPart), PK_NOSNAP(playerWandLog), PK_NOSNAP(handBoxPrivGroup), PK_NOSNAP(handBoxPrivGroupId), PK_NOSNAP(handBoxNullUserData), PK_NOSNAP(sceneSuspendHands), PK_NOSNAP(sceneFirstDistU), PK_NOSNAP(apiWeaponDrawnOnly), PK_NOSNAP(weaponSheathedColOff), PK_NOSNAP(pivGuardCombatLoose), PK_NOSNAP(bumperSceneOff), PK_NOSNAP(bumperSceneExcite), PK_NOSNAP(planckLoosenOurs), PK_NOSNAP(planckLoosenGlobal), PK_NOSNAP(touchProbeHud), PK_NOSNAP(touchProbeHudU),
                    PK_NOSNAP(lmBrAYc), PK_NOSNAP(lmBrAYm), PK_NOSNAP(lmBrBYc), PK_NOSNAP(lmBrBYm),
                    PK_NOSNAP(lmBrAZc), PK_NOSNAP(lmBrAZm), PK_NOSNAP(lmBrBZc), PK_NOSNAP(lmBrBZm),
                    PK_NOSNAP(lmBrRc),  PK_NOSNAP(lmBrRm),  PK_NOSNAP(lmBrAX),
                    PK_NOSNAP(lmBrHK), PK_NOSNAP(lmBrMidZ0), PK_NOSNAP(lmBrCupSat),
                    PK_NOSNAP(lmBrSagLo), PK_NOSNAP(lmBrSagHi),
                    PK_NOSNAP(lmBrLatK), PK_NOSNAP(lmBrLatX), PK_NOSNAP(lmBrLatY0),
                    PK_NOSNAP(tipProbeX), PK_NOSNAP(tipProbeY), PK_NOSNAP(tipProbeZ),
                    PK_NOSNAP(nipUVu), PK_NOSNAP(nipUVv),
                    PK(meshRadiusGain), PK(meshShiftButtK), PK(meshShiftBreastFwdK),
                    PK(meshShiftBreastZK), PK(meshNeutralBreastZ), PK(meshNeutralBreastCup), PK_NOSNAP(meshCupFrac),
                    PK_NOSNAP(meshBandChestLo), PK_NOSNAP(meshBandChestHi),
                    PK_NOSNAP(meshBandBreastLo), PK_NOSNAP(meshBandBreastHi),
                    PK_NOSNAP(meshBandBellyLo), PK_NOSNAP(meshBandBellyHi),
                    PK_NOSNAP(meshBandWaistLo), PK_NOSNAP(meshBandWaistHi),
                    PK_NOSNAP(meshBandButtLo), PK_NOSNAP(meshBandButtHi),
                    PK_NOSNAP(meshBandThighLo), PK_NOSNAP(meshBandThighHi),
                    PK_NOSNAP(meshBandArmLo), PK_NOSNAP(meshBandArmHi),
                    // 2026-07-09 PLAYER HAND 4-BOX (HandBox.cpp) — read live per frame,
                    // excluded from the capsule change-watch.
                    PK_NOSNAP(higgsSlabHalfX), PK_NOSNAP(higgsSlabHalfY), PK_NOSNAP(higgsSlabHalfZ),
                    PK_NOSNAP(higgsSlabAllowGrow),
                    PK_NOSNAP(handBoxEnable), PK_NOSNAP(handBoxArm), PK_NOSNAP(handBoxFollowMode),
                    PK_NOSNAP(handBoxTipFrac), PK_NOSNAP(handBoxPlateTipFrac), PK_NOSNAP(handBoxPlateLenFrac),
                    PK_NOSNAP(handBoxSlabLenFrac),
                    PK_NOSNAP(handBoxPlateTiltDeg), PK_NOSNAP(handBoxPlateTiltAxis),
                    PK_NOSNAP(handBoxPlateTilt2Deg), PK_NOSNAP(handBoxPlateTilt2Axis),
                    PK_NOSNAP(handBoxIdxHalfW), PK_NOSNAP(handBoxIdxHalfT),
                    PK_NOSNAP(handBoxSlabHalfT), PK_NOSNAP(handBoxPad), PK_NOSNAP(handBoxSubLayer),
                    PK_NOSNAP(handBoxMaxVel), PK_NOSNAP(handBoxLeashU), PK_NOSNAP(handBoxRebuildFrac), PK_NOSNAP(handBoxBeast), PK_NOSNAP(handBoxDump),
                    // 2026-07-10 per-box LIVE DIALS (HandBox.cpp) — read live per frame, PK_NOSNAP.
                    PK_NOSNAP(handBoxPlateOffX), PK_NOSNAP(handBoxPlateOffY), PK_NOSNAP(handBoxPlateOffZ),
                    PK_NOSNAP(handBoxPlateHalfW), PK_NOSNAP(handBoxPlateHalfT),
                    PK_NOSNAP(handBoxIdxOffX), PK_NOSNAP(handBoxIdxOffY), PK_NOSNAP(handBoxIdxOffZ),
                    PK_NOSNAP(handBoxIdxTiltDeg), PK_NOSNAP(handBoxIdxTiltAxis), PK_NOSNAP(handBoxDumpNow),
                };   // 318 base + 5 (2026-07-10: plate-tilt2 x2 + higgsSlabAllowGrow + jtrackNow + handBoxSlabLenFrac)
                     // + 3 (2026-07-10: poseConform trio) = 326 keys; 276 snapshotted (unchanged set)
                x.byName.reserve(1024);
                x.snapFields.reserve(896);   // wave-2b: 276 snapped scalars + 496 child floats = 772
                for (const auto& k : kScalars) {
                    x.byName.emplace(k.name, k.p);
                    if (k.snap) x.snapFields.push_back(k.p);
                }
                // Torso bhkListShape children (wave-2b bake): 62 blocks x 8 keys = 496, ZERO branches.
                // spine0C/spine1C stay at 10 (MAX capacity): the wave-2b NIF has only 7/6 children, but
                // keeping the registration at 10 keeps the parked spare tuning lines (spine0 C8-C10 /
                // spine1 C7-C10) parseable as inert no-ops (the apply loop never reaches them). spine2C
                // is 12 (2 breast children added); capHeadC (10) is NEW — head is now a bhkListShape.
                // 2026-08-22: raised in lock-step with Tuning.h array sizes, CapFixChildKnobs and
                // CapFixChildSlot ChildPtr bounds for the male SCULPT headroom (5 coupled edits).
                // ⚠ The TOP index of four of these rows is a LANDMARK PROBE — reference geometry,
                // never shipped. Authoritative strip list (knobs AND nif children):
                // Report/Precision Physic Bodies Module/23_Male_Body_Build_And_Sculpt.md §"LANDMARK PROBES".
                const ChildArray kArrays[] = {
                    { "capSpine0C", g_tune.spine0C, 12 },   // 2026-08-22: 10 -> 12; C12 = WAISTLINE probe — STRIP BEFORE SHIP
                    { "capSpine1C", g_tune.spine1C, 13 },   // 2026-08-22: 12 -> 13; C13 = BELLY probe — STRIP BEFORE SHIP
                    { "capSpine2C", g_tune.spine2C, 24 },   // 2026-07-16: 14 -> 16 (+Argonian ridge); 2026-08-22: 20 -> 24; C24 = CHEST probe — STRIP BEFORE SHIP
                    { "capHeadC",   g_tune.headC,   34 },   // 2026-07-19: 16 -> 22 (horn/antler seeds; C15/C16 = Khajiit ears on beast heads); 2026-08-22: 24 -> 34 (spares only, no probe)
                    { "capNeckC",   g_tune.neckC,    1 },   // 2026-08-22 C1 = "front neck" (choking target)
                    { "capComC",    g_tune.comC,    37 },   // 2026-08-22: 34 -> 37; C37 = BUTTCHEEK-R probe — STRIP BEFORE SHIP
                };
                static const char* kSuf[8] = { "Enable", "AX", "AY", "AZ", "BX", "BY", "BZ", "R" };
                char buf[40];
                for (const auto& ca : kArrays) {
                    for (int i = 0; i < ca.n; ++i) {
                        float* base = &ca.arr[i].enable;             // 8 contiguous floats (static_assert'd)
                        for (int s = 0; s < 8; ++s) {
                            std::snprintf(buf, sizeof buf, "%s%d%s", ca.stem, i + 1, kSuf[s]);
                            x.byName.emplace(buf, base + s);
                            x.snapFields.push_back(base + s);
                        }
                    }
                }
                return x;
            }();
            return t;
        }

#undef PK4
#undef PK8
#undef PK_NOSNAP
#undef PK

    }   // anonymous namespace

    // Hot-reload the PPB params from a plain "key value" text file. Lines starting with
    // '#'/';' are comments. Missing/unparseable -> keep current value (safe). DEV path first
    // (the exact staging file we edit live), then the shipped virtual path.
    // Shared by ReloadGrabTune (open+parse) and CapFixPollFile (mtime precheck).
    static const char* kTunePaths[] = {
        "D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins/PPB_tuning.txt",  // dev live-edit
        "Data/SKSE/Plugins/PPB_tuning.txt",                                             // shipped (USVFS)
    };

    // ── EARLY ONE-KEY READ (2026-07-29) ─────────────────────────────────────────────
    // The full parse (ReloadGrabTune) is driven by CapFixPollFile from the pre-drive hook,
    // which is installed at kDataLoaded. Anything that must consult a knob EARLIER than
    // that — notably FsmpLink's SMP handshake, which runs at the engine's kPostPostLoad
    // MSG_STARTUP — would otherwise read the compiled default and silently ignore the
    // user's file. (Exactly the class of bug that made handBoxRebuildFrac's "0" mean 2%.)
    // This reads ONE key straight off disk using the same kTunePaths, mutates no global
    // state, and does not touch the mtime the poller commits.
    float EarlyReadKnob(const char* key, float fallback)
    {
        if (!key || !*key) return fallback;
        std::ifstream f;
        for (auto* p : kTunePaths) { f.open(p); if (f.is_open()) break; f.clear(); }
        if (!f.is_open()) return fallback;
        std::string line, k; float v;
        float found = fallback;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            std::istringstream ss(line);
            if (!(ss >> k >> v)) continue;
            if (k == key) found = v;      // last occurrence wins, matching the full parser
        }
        return found;
    }

    // Returns the kTunePaths entry actually opened (nullptr if none could be opened) so
    // CapFixPollFile commits the mtime only after a SUCCESSFUL parse of the SAME file
    // (2026-07-13 review MEDIUM: committing on stat-success alone wedged stale values
    // when the stat read attributes but the open transiently failed on a locked file).
    const char* ReloadGrabTune() {
        std::ifstream f; const char* opened = nullptr;
        for (auto* p : kTunePaths) { f.open(p); if (f.is_open()) { opened = p; break; } }
        if (!f.is_open()) {
            static bool s_warned = false;
            if (!s_warned) { s_warned = true; logger::info("GrabTune: tuning file NOT FOUND (using compiled defaults)"); }
            return nullptr;
        }
        const TuneIndex& idx = Index();
        std::string line, key; float val;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            std::istringstream ss(line);
            if (!(ss >> key >> val)) continue;
            auto it = idx.byName.find(key);
            if (it != idx.byName.end()) *it->second = val;
        }
        static bool s_loggedLoad = false;
        if (!s_loggedLoad) {   // one-time proof the file IS read + the active values
            s_loggedLoad = true;
            logger::info("GrabTune parser: table-driven, {} keys indexed, {} snapshot fields "
                         "(heelFix + fingerCap* excluded from the generation watch)",
                         idx.byName.size(), idx.snapFields.size());
            logger::info("GrabTune FILE LOADED from '{}': heelFix={:.0f} pivAutoSeat={:.0f} pivClavFollow={:.0f} capAutoFit={:.0f} "
                         "capEnables=[hand{:.0f} fore{:.0f} upper{:.0f} head{:.0f} c1{:.0f} c2{:.0f} c3{:.0f} s0{:.0f} s1{:.0f} s2{:.0f} "
                         "neck{:.0f} thigh{:.0f} calf{:.0f} foot{:.0f} com{:.0f}] "
                         "pivEnables=[W{:.0f} E{:.0f} S{:.0f} s0{:.0f} s1{:.0f} s2{:.0f} N{:.0f} H{:.0f} hip{:.0f} knee{:.0f} ankle{:.0f}] "
                         "upU=[W{:.1f} E{:.1f} S{:.1f} s0{:.1f} s1{:.1f} s2{:.1f} N{:.1f} H{:.1f} hip{:.1f} knee{:.1f} ankle{:.1f}]",
                         opened ? opened : "?", g_tune.heelFix, g_tune.pivAutoSeat, g_tune.pivClavFollow, g_tune.capAutoFit,
                         g_tune.capHandEnable, g_tune.capForeEnable, g_tune.capUpperEnable, g_tune.capHeadEnable,
                         g_tune.capHandC1Enable, g_tune.capHandC2Enable, g_tune.capHandC3Enable,
                         g_tune.capSpine0Enable, g_tune.capSpine1Enable, g_tune.capSpine2Enable,
                         g_tune.capNeckEnable, g_tune.capThighEnable, g_tune.capCalfEnable, g_tune.capFootEnable, g_tune.capComEnable,
                         g_tune.pivWristEnable, g_tune.pivElbowEnable, g_tune.pivShoulderEnable,
                         g_tune.pivSpine0Enable, g_tune.pivSpine1Enable, g_tune.pivSpine2Enable,
                         g_tune.pivNeckEnable, g_tune.pivHeadEnable, g_tune.pivHipEnable, g_tune.pivKneeEnable, g_tune.pivAnkleEnable,
                         g_tune.pivWristUpU, g_tune.pivElbowUpU, g_tune.pivShoulderUpU,
                         g_tune.pivSpine0UpU, g_tune.pivSpine1UpU, g_tune.pivSpine2UpU,
                         g_tune.pivNeckUpU, g_tune.pivHeadUpU, g_tune.pivHipUpU, g_tune.pivKneeUpU, g_tune.pivAnkleUpU);
        }
        return opened;
    }

    // ── CAP FIX control (console `capfix` = re-read the tuning file + arm a new generation; the hook
    // applies the values to each driven NPC's RIGHT-hand capsule once per generation). ──
    static std::atomic<unsigned> g_capFixGen{ 0 };
    unsigned CapFixGen() { return g_capFixGen.load(std::memory_order_relaxed); }
    // Console-args path (2026-07-02 rework): store the values + arm a generation WITHOUT touching the
    // file — the console IS the source of truth now ("no big config files" user rule).
    void CapFixSet(const float a[3], const float b[3], float r) {
        g_tune.capHandEnable = 1.f;
        g_tune.capHandAX = a[0]; g_tune.capHandAY = a[1]; g_tune.capHandAZ = a[2];
        g_tune.capHandBX = b[0]; g_tune.capHandBY = b[1]; g_tune.capHandBZ = b[2];
        g_tune.capHandR  = r;
        g_capFixGen.fetch_add(1, std::memory_order_relaxed);
    }

    // CLAUDE-DRIVEN loop (2026-07-02): poll the tuning file ~1 Hz even while IDLE (kills the "idle file
    // edits are silently ignored" trap for every knob) and, when the capHand* values change, bump the
    // capfix generation so CapFixApply re-applies them to every driven NPC. Workflow: Claude edits the
    // file → ≤1s → capsules update → user toggles the visualizer to see. NOTE: while this poll runs, the
    // FILE is authoritative for capHand* (console args get overwritten within a second).
    void CapFixPollFile() {
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last).count() < 1000) return;
        s_last = now;
        // PERF fix (2026-07-13 audit 3b): mtime precheck — this poll used to open + parse
        // the whole file every second (100-400µs synchronous on the game thread) even when
        // nothing changed. Parse only when the file actually changed on disk. Behavior
        // note: a console CapFixSet now stays live until the NEXT real file edit (it arms
        // its own generation); previously the file overwrote console values within 1 s.
        //
        // 2026-07-13 review MEDIUM: commit the mtime ONLY after a successful parse of the
        // SAME path. last_write_time reads attributes (never blocked by share modes) while
        // ifstream::open needs read access — a stat-success/open-fail tick during a file
        // lock used to store the new mtime and then early-return forever, wedging the edit.
        {
            namespace fs = std::filesystem;
            static fs::file_time_type s_mtime{};
            static const char*        s_lastPath = nullptr;   // kTunePaths entry last parsed (stable literal)
            static bool               s_everParsed = false;

            // Pre-stat: the first path that stats OK (cheap; drives the skip decision).
            const char* statPath = nullptr;
            fs::file_time_type mt{};
            for (auto* p : kTunePaths) {
                std::error_code e; auto t = fs::last_write_time(p, e);
                if (!e) { statPath = p; mt = t; break; }
            }
            // Skip the parse only when we have parsed this SAME file before, unchanged.
            if (s_everParsed && statPath && statPath == s_lastPath && mt == s_mtime) return;

            const char* opened = ReloadGrabTune();
            if (opened) {
                std::error_code e2; auto mt2 = fs::last_write_time(opened, e2);
                s_mtime      = e2 ? mt : mt2;   // prefer the opened file's own mtime
                s_lastPath   = opened;
                s_everParsed = true;
            }
            // opened == nullptr: leave s_mtime/s_lastPath/s_everParsed untouched so the
            // NEXT tick retries (no wedge). No skip happened, so nothing was lost.
        }
        // Log-verbosity apply (2026-07-13 log demotion): per-slot CapFix geometry lines are
        // debug-level now; logVerbose 1 in the tuning file surfaces them live (~1 s).
        {
            const auto want = g_tune.logVerbose > 0.5f ? spdlog::level::debug : spdlog::level::info;
            auto log = spdlog::default_logger();
            if (log && log->level() != want) { log->set_level(want); log->flush_on(want); }
        }
        // measuredScaleEnable is NOSNAP (a scale-mode flip must not ride the capsule-value
        // snapshot), but flipping it DOES change every capsule's applied scale — so
        // edge-detect it here and bump the gen once so all driven NPCs re-dress live.
        {
            static float s_lastMSE = -12345.f;
            if (s_lastMSE != g_tune.measuredScaleEnable) {
                if (s_lastMSE != -12345.f) {   // skip the first-load baseline
                    g_capFixGen.fetch_add(1, std::memory_order_relaxed);
                    logger::info("CapFix measuredScaleEnable -> {:.0f} (gen {}) — re-dressing at {} scale",
                                 g_tune.measuredScaleEnable,
                                 g_capFixGen.load(std::memory_order_relaxed),
                                 g_tune.measuredScaleEnable > 0.5f ? "MEASURED" : "GetScale");
                }
                s_lastMSE = g_tune.measuredScaleEnable;
            }
        }
        // Change-detection snapshot (2026-07-08: driven off the parser's snapFields table instead of a
        // 276-element hand-written literal — the wave-2 torso bake adds 400 child floats). The set is
        // UNCHANGED for every pre-existing key; `heelFix` is still excluded (a change to it never bumps
        // the generation — the console `hf` owns it live), and so are the per-frame fingerCap* knobs.
        // memcmp (bitwise), NOT `!=`, so today's NaN behaviour is preserved exactly. First call: snap is
        // empty -> size mismatch -> gen 1, same as the old `{-12345.f}` sentinel.
        static std::vector<float> snap;
        static std::vector<float> cur;                       // reused scratch — no per-tick allocation
        const TuneIndex& idx = Index();
        cur.clear();
        cur.reserve(idx.snapFields.size());
        for (const float* p : idx.snapFields) cur.push_back(*p);
        if (snap.size() != cur.size() ||
            std::memcmp(snap.data(), cur.data(), cur.size() * sizeof(float)) != 0) {
            snap = cur;
            g_capFixGen.fetch_add(1, std::memory_order_relaxed);
            logger::info("CapFix FILE-CHANGE detected -> gen {} (enables: hand={:.0f} fore={:.0f} upper={:.0f} head={:.0f} c1={:.0f} c2={:.0f} c3={:.0f} pivW={:.0f} pivE={:.0f} pivS={:.0f})",
                         g_capFixGen.load(std::memory_order_relaxed),
                         g_tune.capHandEnable, g_tune.capForeEnable, g_tune.capUpperEnable, g_tune.capHeadEnable,
                         g_tune.capHandC1Enable, g_tune.capHandC2Enable, g_tune.capHandC3Enable,
                         g_tune.pivWristEnable, g_tune.pivElbowEnable, g_tune.pivShoulderEnable);
        }
    }

    // Slot accessor for the full-arm+head sweep: 0=hand, 1=forearm, 2=upper arm, 3=head. Returns enable.
    bool CapFixSlot(int slot, float a[3], float b[3], float& r) {
        const float* f = nullptr;
        switch (slot) {
        case 0: f = &g_tune.capHandEnable;  break;
        case 1: f = &g_tune.capForeEnable;  break;
        case 2: f = &g_tune.capUpperEnable; break;
        case 3: f = &g_tune.capHeadEnable;  break;
        case 4: f = &g_tune.capSpine0Enable; break;
        case 5: f = &g_tune.capSpine1Enable; break;
        case 6: f = &g_tune.capSpine2Enable; break;
        case 7: f = &g_tune.capNeckEnable; break;
        case 8: f = &g_tune.capThighEnable; break;
        case 9: f = &g_tune.capCalfEnable; break;
        case 10: f = &g_tune.capFootEnable; break;
        case 11: f = &g_tune.capComEnable; break;
        default: return false;
        }
        // fields are laid out contiguously per slot: Enable, AX, AY, AZ, BX, BY, BZ, R
        a[0] = f[1]; a[1] = f[2]; a[2] = f[3];
        b[0] = f[4]; b[1] = f[5]; b[2] = f[6];
        r = f[7];
        return f[0] > 0.5f;
    }
    // bhkListShape child accessor (generalized 2026-07-07 from the hand-only original).
    // slot = CapFixSlot index; child = LIST child index. Hand keeps its 07-03 bake layout
    // (C1..C3 = children 0..2). The flesh-fit slots put the MAIN capsule at child 0 — driven
    // by the SLOT knobs so the dialed main values survive the list bake — and the ADDED
    // capsules at children 1..N (cap<Slot>C1..CN). Same contiguous 8-float layout everywhere.
    //
    // Torso helper (2026-07-08 wave-2): child 0 = the MAIN capsule = the named slot block;
    // children 1..N = the CapChild array. Both sides share the 8-float layout (Tuning.h static_assert).
    static const float* ChildPtr(const CapChild* arr, int n, int child, const float* mainSlot) {
        if (child == 0)               return mainSlot;
        if (child >= 1 && child <= n) return &arr[child - 1].enable;
        return nullptr;                                     // child index out of range -> caller skips
    }

    bool CapFixChildSlot(int slot, int child, float a[3], float b[3], float& r) {
        const float* f = nullptr;
        switch (slot) {
        case 0:   // hand: 3 palm rods (C1=center, C2=thumb, C3=pinky = children 0..2)
            switch (child) {
            case 0: f = &g_tune.capHandC1Enable; break;
            case 1: f = &g_tune.capHandC2Enable; break;
            case 2: f = &g_tune.capHandC3Enable; break;
            case 3: f = &g_tune.capHandC4Enable; break;   // 2026-07-12 palm fill
            case 4: f = &g_tune.capHandC5Enable; break;
            case 5: f = &g_tune.capHandC6Enable; break;   // 2026-08-22 male-sculpt spare (OFFSET: child 5 -> C6)
            case 6: f = &g_tune.capHandC7Enable; break;   // 2026-08-22 male-sculpt spare (OFFSET: child 6 -> C7)
            }
            break;
        case 1:   // forearm: main + wrist-ward taper
            switch (child) {
            case 0: f = &g_tune.capForeEnable;   break;
            case 1: f = &g_tune.capForeC1Enable; break;
            case 2: f = &g_tune.capForeC2Enable; break;   // 2026-08-22 male-sculpt spare
            case 3: f = &g_tune.capForeC3Enable; break;   // 2026-08-22 male-sculpt spare
            }
            break;
        case 2:   // upper arm: main + elbow-ward taper + 2 shoulder-lock children (2026-07-09)
            switch (child) {
            case 0: f = &g_tune.capUpperEnable;   break;
            case 1: f = &g_tune.capUpperC1Enable; break;
            case 2: f = &g_tune.capUpperC2Enable; break;
            case 3: f = &g_tune.capUpperC3Enable; break;
            case 4: f = &g_tune.capUpperC4Enable; break;   // 2026-08-22 male-sculpt spare
            case 5: f = &g_tune.capUpperC5Enable; break;   // 2026-08-22 ARM LANDMARK CHORD probe (STRIP BEFORE SHIP)
            }
            break;
        case 8:   // thigh: main + 5 superposed rings
            switch (child) {
            case 0: f = &g_tune.capThighEnable;   break;
            case 1: f = &g_tune.capThighC1Enable; break;
            case 2: f = &g_tune.capThighC2Enable; break;
            case 3: f = &g_tune.capThighC3Enable; break;
            case 4: f = &g_tune.capThighC4Enable; break;
            case 5: f = &g_tune.capThighC5Enable; break;
            case 6: f = &g_tune.capThighC6Enable; break;   // 2026-07-09 knee hook
            case 7: f = &g_tune.capThighC7Enable; break;   // 2026-08-22 male-sculpt spare
            case 8: f = &g_tune.capThighC8Enable; break;   // 2026-08-22 male-sculpt spare
            case 9: f = &g_tune.capThighC9Enable; break;   // 2026-08-22 LEG LANDMARK CHORD probe (STRIP BEFORE SHIP)
            }
            break;
        case 9:   // calf: main + 3 superposed rings
            switch (child) {
            case 0: f = &g_tune.capCalfEnable;   break;
            case 1: f = &g_tune.capCalfC1Enable; break;
            case 2: f = &g_tune.capCalfC2Enable; break;
            case 3: f = &g_tune.capCalfC3Enable; break;
            case 4: f = &g_tune.capCalfC4Enable; break;   // 2026-07-09 knee hook
            case 5: f = &g_tune.capCalfC5Enable; break;   // 2026-08-22 male-sculpt spare
            case 6: f = &g_tune.capCalfC6Enable; break;   // 2026-08-22 male-sculpt spare
            }
            break;
        case 10:  // foot: main + 3 parallel sole rods (2026-07-09: +1 ankle lock)
            switch (child) {
            case 0: f = &g_tune.capFootEnable;   break;
            case 1: f = &g_tune.capFootC1Enable; break;
            case 2: f = &g_tune.capFootC2Enable; break;
            case 3: f = &g_tune.capFootC3Enable; break;
            case 4: f = &g_tune.capFootC4Enable; break;   // 2026-08-22 male-sculpt spare
            case 5: f = &g_tune.capFootC5Enable; break;   // 2026-08-22 male-sculpt spare
            }
            break;
        // ── TORSO (wave-2b bake): main + 10/7/6/12/20 buried seed children ──
        // (head=10, spine0=7 & spine1=6 in the NIF but the arrays keep MAX capacity 10; spine2=12; com=20)
        case 3:  f = ChildPtr(g_tune.headC,   34, child, &g_tune.capHeadEnable);   break;   // 2026-07-19: 16 -> 22 (horn/antler); 2026-08-22: 24 -> 34
        case 4:  f = ChildPtr(g_tune.spine0C, 12, child, &g_tune.capSpine0Enable); break;   // 2026-08-22: 10 -> 12
        case 5:  f = ChildPtr(g_tune.spine1C, 13, child, &g_tune.capSpine1Enable); break;   // 2026-07-29: 10 -> 12; 2026-08-22: -> 13
        case 6:  f = ChildPtr(g_tune.spine2C, 24, child, &g_tune.capSpine2Enable); break;   // 2026-07-29: 16 -> 20; 2026-08-22: -> 24
        // NECK (2026-08-22): 1-element array, so ChildPtr routes child 0 -> the main capNeck
        // block and child 1 -> capNeckC1, exactly like every torso slot.
        case 7:  f = ChildPtr(g_tune.neckC,     1, child, &g_tune.capNeckEnable);  break;
        case 11: f = ChildPtr(g_tune.comC,    37, child, &g_tune.capComEnable);    break;   // 2026-07-29: 20 -> 32; 2026-08-22: 34 -> 37
        default: break;
        }
        if (!f) return false;
        a[0] = f[1]; a[1] = f[2]; a[2] = f[3];
        b[0] = f[4]; b[1] = f[5]; b[2] = f[6];
        r = f[7];
        return f[0] > 0.5f;
    }

    // Knob-addressable list-child count per slot (0 = the slot has no list-shape support).
    int CapFixChildKnobs(int slot) {
        switch (slot) {
        // 2026-08-22 male-sculpt headroom. Each bound here is the APPLY-LOOP bound and must move in
        // lock-step with CapFixChildSlot VALUE-FETCHER bound (ChildPtr for torso, switch cases for limbs).
        case 0:  return 7;   // hand: 5 palm rods + 2 spares (2026-08-22; OFFSET: child N -> capHandC(N+1))
        case 1:  return 4;   // forearm: main + taper + 2 spares (2026-08-22)
        case 2:  return 6;   // upper arm: main + taper + 2 shoulder-lock + 1 spare + ARM CHORD probe (2026-08-22)
        case 3:  return 35;  // head: main + 34          (2026-07-19 horn/antler seeds; 2026-08-22 +C25..C34 spares)
        case 4:  return 13;  // spine0: main + 12        (2026-08-22: +C11 spare, C12 = WAISTLINE probe — STRIP BEFORE SHIP)
        case 5:  return 14;  // spine1: main + 13        (2026-07-29 BACK L/R; 2026-08-22 C13 = BELLY probe — STRIP BEFORE SHIP)
        case 6:  return 25;  // spine2: main + 24        (2026-07-29 SHOULDER BLADE; 2026-08-22 C24 = CHEST probe — STRIP BEFORE SHIP)
        case 7:  return 2;   // neck: main + C1 "front neck"  (2026-08-22; beast vehicle only —
                             // a skeleton whose neck is still a bare capsule simply has 1 child
                             // live and the C1 knob is skipped, per-race-geometry-by-existence)
        case 8:  return 10;  // thigh: main + 9          (2026-08-22: +C7/C8 spares, C9 = LEG CHORD probe)
        case 9:  return 7;   // calf:  main + 6          (2026-08-22: +C5/C6 spares)
        case 10: return 6;   // foot: main + 5           (2026-08-22: +C4/C5 spares)
        case 11: return 38;  // com: main + 37           (2026-07-29 pelvis sensors; 2026-08-22 C37 = BUTTCHEEK-R probe — STRIP BEFORE SHIP)
        default: return 0;
        }
    }

    bool CapMirrorLEnabled()  { return g_tune.capMirrorL  > 0.5f; }
    bool PivGrabGateEnabled() { return g_tune.pivGrabGate > 0.5f; }

    // Pivot knob accessor: 0=wrist 1=elbow 2=shoulder 3=spine0 4=spine1 5=spine2 6=neck 7=head
    // 8=hipR 9=kneeR 10=ankleR. 4 contiguous floats (Enable,X,Y,Z). Returns enable.
    bool PivFixSlot(int joint, float p[3]) {
        const float* f = nullptr;
        switch (joint) {
        case 0:  f = &g_tune.pivWristEnable;    break;
        case 1:  f = &g_tune.pivElbowEnable;    break;
        case 2:  f = &g_tune.pivShoulderEnable; break;
        case 3:  f = &g_tune.pivSpine0Enable;   break;
        case 4:  f = &g_tune.pivSpine1Enable;   break;
        case 5:  f = &g_tune.pivSpine2Enable;   break;
        case 6:  f = &g_tune.pivNeckEnable;     break;
        case 7:  f = &g_tune.pivHeadEnable;     break;
        case 8:  f = &g_tune.pivHipEnable;      break;
        case 9:  f = &g_tune.pivKneeEnable;     break;
        case 10: f = &g_tune.pivAnkleEnable;    break;
        default: return false;
        }
        p[0] = f[1]; p[1] = f[2]; p[2] = f[3];
        return f[0] > 0.5f;
    }

    // NEW (PPB): per-joint world-Z lift knob (game units; same 0..10 joint order as PivFixSlot).
    float PivFixUpU(int joint) {
        switch (joint) {
        case 0:  return g_tune.pivWristUpU;
        case 1:  return g_tune.pivElbowUpU;
        case 2:  return g_tune.pivShoulderUpU;
        case 3:  return g_tune.pivSpine0UpU;
        case 4:  return g_tune.pivSpine1UpU;
        case 5:  return g_tune.pivSpine2UpU;
        case 6:  return g_tune.pivNeckUpU;
        case 7:  return g_tune.pivHeadUpU;
        case 8:  return g_tune.pivHipUpU;
        case 9:  return g_tune.pivKneeUpU;
        case 10: return g_tune.pivAnkleUpU;
        default: return 0.f;
        }
    }

    // jtrackNow (2026-07-10): raw value — PivJointTrackTick (PivFix.cpp) edge-detects the 0 -> non-0
    // transition and arms a ~2 s all-driven-actors dump window. PK_NOSNAP (a trigger flip must never
    // re-dress the capsule slots).
    float JTrackNow() { return g_tune.jtrackNow; }

    // ── XP32 POSE-CONFORM (2026-07-10, PPBHook.cpp) — read live per frame; all PK_NOSNAP. ──
    bool  PoseConformEnabled() { return g_tune.poseConform > 0.5f; }
    float PoseConformDump()    { return g_tune.poseConformDump; }
    bool  PoseConformRoot()     { return g_tune.poseConformRoot > 0.5f; }
    int   PoseConformEveryN()  { const int n = (int)(g_tune.poseConformEveryN + 0.5f); return n < 1 ? 1 : (n > 16 ? 16 : n); }

    bool CapAutoFitEnabled() { return g_tune.capAutoFit > 0.5f; }
    bool MeasuredScaleEnabled() { return g_tune.measuredScaleEnable > 0.5f; }
    float MeasuredScaleRefId()  { return g_tune.measuredScaleRefId; }
    bool RefCapNow()            { return g_tune.refCapNow > 0.5f; }   // deterministic one-shot reference read

    // ── BODY SCALE (2026-07-14) — read live at capsule apply. The clamp accessors RE-BOUND the file
    // value to a sane band so a mistyped line can never invert the clamp or explode a capsule. ──
    bool  BodyScaleEnabled()    { return g_tune.bodyScale > 0.5f; }
    float BodyScaleRegionFactor(int region) {
        switch (region) {
        case 0:  return g_tune.bodyScaleChest;     // BodyScale::kChest
        case 1:  return g_tune.bodyScaleBreasts;   // kBreasts
        case 2:  return g_tune.bodyScaleBelly;     // kBelly
        case 3:  return g_tune.bodyScaleWaist;     // kWaist
        case 4:  return g_tune.bodyScaleButt;      // kButt
        case 5:  return g_tune.bodyScaleThighs;    // kThighs
        case 6:  return g_tune.bodyScaleArms;      // kUpperArms
        case 7:  return g_tune.bodyScaleHead;      // kHead
        default: return 1.f;
        }
    }
    float BodyScaleClampLo() { const float v = g_tune.bodyScaleClampLo; return v < 0.3f ? 0.3f : (v > 1.f ? 1.f : v); }
    float BodyScaleClampHi() { const float v = g_tune.bodyScaleClampHi; return v < 1.f ? 1.f : (v > 3.f ? 3.f : v); }
    float BodyScaleDump()    { return g_tune.bodyScaleDump; }

    // FINGER ENDPOINT TRACKING (2026-07-08) — read live by the per-frame writer (GrabDiag::FingerCapTrack).
    // Deliberately NOT in the generation snapshot: a radius tweak must not re-dress all 12 capsule slots.
    bool  FingerCapTrackEnabled() { return g_tune.fingerCapTrack > 0.5f; }
    float FingerCapR()            { return g_tune.fingerCapR; }

    // lodSpike (2026-07-08 perf diagnostic): the safety gate for the `capdis` disableChild spike. Default
    // 0 -> the console command is inert. Read live by Diag::ApplyPendingSpike each pre-drive fire.
    bool  LodSpikeEnabled()       { return g_tune.lodSpike > 0.5f; }

    // ── PERF SYSTEM (2026-07-09): -1 knobs resolve from the preset ──
    int PerfPreset() {
        const int p = (int)(g_tune.perfPreset + 0.5f);
        return (p == 50 || p == 80 || p == 90) ? p : 0;
    }
    static bool ResolveTri(float knob, bool presetDefault) {   // -1 = follow preset, else 0/1
        if (knob < -0.5f) return presetDefault;
        return knob > 0.5f;
    }
    bool PerfFilterSelfThigh()   { return ResolveTri(g_tune.filterSelfThigh,  PerfPreset() > 0); }
    bool PerfFilterCrossPelvis() { return ResolveTri(g_tune.filterCrossPelvis, false); }   // validate first
    int PerfContactKeepN() {   // 0 = decimation off, else keep 1 NEW contact point in N
        const float k = g_tune.perfContactKeep;
        int n;
        if (k < -0.5f) {                    // -1: follow preset (50 -> 2, 80 -> 5, 90 -> 10)
            const int p = PerfPreset();
            n = p == 50 ? 2 : p == 80 ? 5 : p == 90 ? 10 : 0;
        } else {
            n = (int)(k + 0.5f);
        }
        return n > 1 ? (n > 100 ? 100 : n) : 0;
    }
    bool PerfLodEnabled()        { return ResolveTri(g_tune.lodEnable,         PerfPreset() > 0); }
    float PerfLodNearDist()      { return g_tune.lodNearDist; }
    float PerfLodFarDist()       { return g_tune.lodFarDist; }
    float PerfLodHystPct()       { return g_tune.lodHystPct; }
    float PerfLodSettleFrames()  { return g_tune.lodSettleFrames; }
    float PerfLodStageBits()     { return g_tune.lodStageBits; }
    // rayTel (2026-08-22, RayTel.cpp): 0 off (shipping default) / 1 counters / 2 + __rdtsc timing.
    // Read every frame on the main thread — the arm/disarm is edge-detected there, never in a callback.
    int   RayTelMode()           { const int m = (int)(g_tune.rayTel + 0.5f); return m < 0 ? 0 : (m > 2 ? 2 : m); }

    // ── NPC FINGER TEST v2 (2026-07-09, NpcFingerTest.cpp) — read live per frame; the clamps
    // keep a mistyped tuning line from creating a degenerate/super-critical dynamic body. ──
    bool  NpcFingerEnabled()    { return g_tune.npcFingerEnable > 0.5f; }
    bool  NpcTailTest()         { return g_tune.npcTailTest > 0.5f; }
    int   NpcTailMode()         { const int m = (int)(g_tune.npcTailTest + 0.5f); return m < 0 ? 0 : (m > 3 ? 3 : m); }
    float NpcTailR()            { return g_tune.npcTailR < 0.3f ? 0.3f : (g_tune.npcTailR > 3.f ? 3.f : g_tune.npcTailR); }
    float NpcTailOffScale()     { return g_tune.npcTailOffScale < 0.f ? 0.f : (g_tune.npcTailOffScale > 2.f ? 2.f : g_tune.npcTailOffScale); }
    bool  NpcFingerOwnBody()    { return g_tune.npcFingerOwnBody > 0.5f; }
    float NpcFingerFollow()     { return g_tune.npcFingerFollow < 0.f ? 0.f : (g_tune.npcFingerFollow > 1.f ? 1.f : g_tune.npcFingerFollow); }
    float NodeCensusNow()       { return g_tune.nodeCensusNow; }
    bool  FsmpPushEnabled()     { return g_tune.fsmpPush > 0.5f; }
    float FsmpPushForce()       { return g_tune.fsmpPushForce < 0.f ? 0.f : g_tune.fsmpPushForce; }       // LEGACY (unused by the per-target path)
    float FsmpPushMaxForce()    { return g_tune.fsmpPushMaxForce < 1.f ? 1.f : g_tune.fsmpPushMaxForce; } // LEGACY (unused by the per-target path)
    float FsmpPushMinDispU()    { return g_tune.fsmpPushMinDispU < 0.f ? 0.f : g_tune.fsmpPushMinDispU; }
    float FsmpPushMult()        { return g_tune.fsmpPushMult < 0.f ? 0.f : (g_tune.fsmpPushMult > 10.f ? 10.f : g_tune.fsmpPushMult); }
    bool  NpcFollowerEnabled()  { return g_tune.npcFollower > 0.5f; }
    bool  NpcGenCapEnabled()    { return g_tune.npcGenCap > 0.5f; }
    float NpcGenMassKg()        { return g_tune.npcGenMassKg < 0.01f ? 0.01f : g_tune.npcGenMassKg; }
    float NpcGenR()             { return g_tune.npcGenR < 0.1f ? 0.1f : g_tune.npcGenR; }
    float NpcGenAlpha()         { float a = g_tune.npcGenAlpha; return a < 0.05f ? 0.05f : (a > 1.f ? 1.f : a); }
    bool  GenBendEnabled()        { return g_tune.genBend > 0.5f; }
    bool  GenBendPlayerEnabled()  { return g_tune.genBendPlayer > 0.5f; }
    int   GenBendMax()            { int m = (int)g_tune.genBendMax; return m < 0 ? 0 : (m > 9 ? 9 : m); }
    float GenBendUpMs()           { return g_tune.genBendUpMs < 100.f ? 100.f : g_tune.genBendUpMs; }
    float GenBendHoldSec()        { return g_tune.genBendHoldSec < 0.f ? 0.f : g_tune.genBendHoldSec; }
    float GenBendDecaySec()       { return g_tune.genBendDecaySec < 0.5f ? 0.5f : g_tune.genBendDecaySec; }
    float GenBendArousal()        { return g_tune.genBendArousal; }
    float GenBendTouchU()         { return g_tune.genBendTouchU < 1.f ? 1.f : g_tune.genBendTouchU; }
    // Garment-rig budget (2026-07-30). 0 disables each limit; negatives are clamped away so a
    // typo can never mean "range zero = no rigs at all".
    float HiggsPokeFix()        { return g_tune.higgsPokeFix; }
    bool  ApiTouchEnabled()     { return g_tune.apiTouch > 0.5f; }
    float ApiHz()               { return g_tune.apiHz < 1.f ? 1.f : (g_tune.apiHz > 90.f ? 90.f : g_tune.apiHz); }
    float ApiTouchU()           { return g_tune.apiTouchU < 0.05f ? 0.05f : g_tune.apiTouchU; }
    float ApiExitPadU()         { return g_tune.apiExitPadU < 0.f ? 0.f : g_tune.apiExitPadU; }
    int   ApiMaxActors()        { const int n = (int)(g_tune.apiMaxActors + 0.5f); return n < 1 ? 1 : (n > 8 ? 8 : n); }
    float ApiRangeU()           { return g_tune.apiRangeU < 0.f ? 0.f : g_tune.apiRangeU; }
    float ApiFistTipPalmU()     { return g_tune.apiFistTipPalmU < 0.f ? 0.f : g_tune.apiFistTipPalmU; }
    bool  ApiEventsEnabled()    { return g_tune.apiEvents > 0.5f; }
    // contactLog from PPB_Skeletons_Added_Race.ini — OR'd with the dev knob apiLog, so either
    // switch turns contact logging on and neither can silently cancel the other.
    static std::atomic<bool> s_contactLogIni{ false };
    void  SetContactLogIni(bool on) { s_contactLogIni.store(on, std::memory_order_relaxed); }
    bool  ApiLogEnabled()       { return g_tune.apiLog > 0.5f || s_contactLogIni.load(std::memory_order_relaxed); }
    float ApiDwellS()           { return g_tune.apiDwellS       < 0.f ? 0.f : g_tune.apiDwellS; }
    float ApiDwellHeadS()       { return g_tune.apiDwellHeadS   < 0.f ? 0.f : g_tune.apiDwellHeadS; }
    float ApiDwellComS()        { return g_tune.apiDwellComS    < 0.f ? 0.f : g_tune.apiDwellComS; }
    float ApiDwellSensorS()     { return g_tune.apiDwellSensorS < 0.f ? 0.f : g_tune.apiDwellSensorS; }
    float ApiDwellTailS()       { return g_tune.apiDwellTailS   < 0.f ? 0.f : g_tune.apiDwellTailS; }
    bool  ApiRawEventsEnabled() { return g_tune.apiRawEvents > 0.5f; }
    float ApiWeaponRMaxU() { return g_tune.apiWeaponRMaxU; }
    float ApiObjectRMaxU() { return g_tune.apiObjectRMaxU; }
    bool  ApiSubRegionInEvent() { return g_tune.apiSubRegionInEvent > 0.5f; }
    bool  ApiSuppressHeldHand() { return g_tune.apiSuppressHeldHand > 0.5f; }
    bool  ApiSuppressHeldHandStrict() { return g_tune.apiSuppressHeldHand > 1.5f; }
    bool  ApiHairTarget()       { return g_tune.apiHairTarget > 0.5f; }
    float NpcRigRangeU()        { return g_tune.npcRigRangeU     < 0.f ? 0.f : g_tune.npcRigRangeU; }
    float NpcRigRangeHystU()    { return g_tune.npcRigRangeHystU < 0.f ? 0.f : g_tune.npcRigRangeHystU; }
    int   NpcRigMaxActors()     { const int n = (int)(g_tune.npcRigMaxActors + 0.5f); return n < 0 ? 0 : n; }
    int   NpcFingerCount()      { const int c = (int)(g_tune.npcFingerCount + 0.5f); return c < 1 ? 1 : (c > 4 ? 4 : c); }
    float NpcFingerR()          { return g_tune.npcFingerR < 0.2f ? 0.2f : g_tune.npcFingerR; }
    float NpcFingerTipU()       { return g_tune.npcFingerTipU < 0.f ? 0.f : (g_tune.npcFingerTipU > 5.f ? 5.f : g_tune.npcFingerTipU); }
    float NpcFingerMassKg()     { return g_tune.npcFingerMassKg < 0.01f ? 0.01f : g_tune.npcFingerMassKg; }
    float NpcGarmentMassKg()    { return g_tune.npcGarmentMassKg < 0.01f ? 0.01f : g_tune.npcGarmentMassKg; }
    float NpcTailMassKg()       { return g_tune.npcTailMassKg    < 0.01f ? 0.01f : g_tune.npcTailMassKg; }
    float NpcSilentVelMS()      { return g_tune.npcSilentVelMS; }
    float NpcGarmentMat()       { return g_tune.npcGarmentMat; }
    float NpcBodyMat()          { return g_tune.npcBodyMat; }
    float BodyShiftBreastsUpU() { return g_tune.bodyShiftBreastsUpU; }
    float BodyShiftBreastsFwdU(){ return g_tune.bodyShiftBreastsFwdU; }
    float BodyShiftButtBackU()  { return g_tune.bodyShiftButtBackU; }
    float BodyScaleLoBoost()    { const float v = g_tune.bodyScaleLoBoost; return v < 1.f ? 1.f : (v > 5.f ? 5.f : v); }
    bool  MeshShapeEnabled()    { return g_tune.meshShape > 0.5f; }
    float MeshShapeDump()       { return g_tune.meshShapeDump; }
    float MeshBoneTest()        { return g_tune.meshBoneTest; }
    bool  BoneShapeEnabled()    { return g_tune.boneShape > 0.5f; }
    float BoneNeutralCup()      { return g_tune.boneNeutralCup; }
    float BoneNeutralBreastZ()  { return g_tune.boneNeutralBreastZ; }
    float BoneNeutralBreastAbs(){ return g_tune.boneNeutralBreastAbs; }
    float BoneNeutralButtAbs()  { return g_tune.boneNeutralButtAbs; }
    float LmUV(int i, int a) {
        switch (i) {
        case 0: return a ? g_tune.lmNipV   : g_tune.lmNipU;
        case 1: return a ? g_tune.lmBrUpV  : g_tune.lmBrUpU;
        case 2: return a ? g_tune.lmBrDnV  : g_tune.lmBrDnU;
        case 3: return a ? g_tune.lmChestV : g_tune.lmChestU;
        case 4: return a ? g_tune.lmBellyV : g_tune.lmBellyU;
        case 5: return a ? g_tune.lmWaistV : g_tune.lmWaistU;
        case 6: return a ? g_tune.lmButtV  : g_tune.lmButtU;
        case 7: return a ? g_tune.lmLegAV  : g_tune.lmLegAU;
        case 8: return a ? g_tune.lmLegBV  : g_tune.lmLegBU;
        case 9: return a ? g_tune.lmArmAV  : g_tune.lmArmAU;
        case 10:return a ? g_tune.lmArmBV  : g_tune.lmArmBU;
        default: return 0.f;
        }
    }
    bool  LmButtFitEnabled()    { return g_tune.lmButtFit > 0.5f; }
    float LmButtOutU()          { return g_tune.lmButtOutU; }
    float LmNoseUV(int a)       { return a ? g_tune.lmNoseV : g_tune.lmNoseU; }
    float LmChinUV(int a)       { return a ? g_tune.lmChinV : g_tune.lmChinU; }
    float LmButtPosGain()       { return g_tune.lmButtPosGain; }
    void  LmButtNeutral(float o[3]) {
        o[0] = g_tune.lmButtNeutX; o[1] = g_tune.lmButtNeutY; o[2] = g_tune.lmButtNeutZ;
    }
    bool  LmReShapeEnabled()    { return g_tune.lmReShape > 0.5f; }
    float LmClampLo()           { return g_tune.lmClampLo; }
    float LmClampHi()           { return g_tune.lmClampHi; }
    float LmRegionGain(int r) {
        // indices mirror BodyScale:: in Interop.h — chest 0, breasts 1, belly 2, waist 3, butt 4,
        // thighs 5, upperArms 6. Spelled numerically here so Tuning stays free of that header.
        switch (r) {
        case 0: return g_tune.lmGainChest;
        case 2: return g_tune.lmGainBelly;
        case 3: return g_tune.lmGainWaist;
        case 4: return g_tune.lmGainButt;
        case 5: return g_tune.lmGainThigh;
        case 6: return g_tune.lmGainArm;
        case 7: return g_tune.lmGainHead;
        default: return 0.f;    // 0 = this region has no UV channel yet (thighs/arms/head)
        }
    }
    float LmRegionGainM(int r) {
        switch (r) {
        case 0: return g_tune.lmGainMChest;
        case 2: return g_tune.lmGainMBelly;
        case 3: return g_tune.lmGainMWaist;
        case 4: return g_tune.lmGainMButt;
        case 5: return g_tune.lmGainMThigh;
        case 6: return g_tune.lmGainMArm;
        default: return 0.f;    // males: no breast (1) / head (7) channel
        }
    }
    float LmNeutNoseChin()      { return g_tune.lmNeutNoseChin; }
    bool  GhostZonesEnabled()   { return g_tune.ghostZones > 0.5f; }
    bool  DgEnabled()           { return g_tune.dgEnable > 0.5f; }
    float DgGraceDeadS()        { return g_tune.dgGraceDeadS; }
    bool  DgLogOn()             { return g_tune.dgLog > 0.5f; }
    bool  DgDeathIgnoreOn()     { return g_tune.dgDeathIgnore > 0.5f; }
    bool  DgCloneStripOn()      { return g_tune.dgCloneStrip > 0.5f; }
    float DgStripDelayS()       { return g_tune.dgStripDelayS; }
    bool  DgDeferDfOn()         { return g_tune.dgDeferDf > 0.5f; }
    bool  DgHeadTrackOn()       { return g_tune.dgHeadTrack > 0.5f; }
    bool  DgDeathCutOn()        { return g_tune.dgDeathCut > 0.5f; }
    float DgDeathCutDelayS()    { return g_tune.dgDeathCutDelayS; }
    float DgDeathNodeTries()    { return g_tune.dgDeathNodeTries; }
    bool  DgHitLocatedOn()      { return g_tune.dgHitLocated > 0.5f; }
    float DgHitMaxDistU()       { return g_tune.dgHitMaxDistU; }
    bool  DgHeadSkelOn()        { return g_tune.dgHeadSkel > 0.5f; }
    float DgHeadSkelHoldS()     { return g_tune.dgHeadSkelHoldS; }
    bool  DgHeadParkOn()        { return g_tune.dgHeadPark > 0.5f; }
    bool  DgHeadGrabFixOn()     { return g_tune.dgHeadGrabFix > 0.5f; }
    bool  DgHeadPlanckOn()      { return g_tune.dgHeadPlanck > 0.5f; }
    bool  DgVictimPlanckOn()    { return g_tune.dgVictimPlanck > 0.5f; }
    bool  GenProbeOn()          { return g_tune.genProbe > 0.5f; }
    bool  GenProbeFast()        { return g_tune.genProbe > 1.5f; }
    // >0 means "the geometry writers may touch males AT ALL". Mode 2 still returns true here —
    // the ReScale/ReShape suppression is applied at those two sites, not by this gate.
    bool  MaleGeometryOn()      { return g_tune.maleGeometry > 0.5f; }
    int   MaleGeometryMode()    {
        int v = (int)(g_tune.maleGeometry + 0.5f);
        return (v < 0 || v > 2) ? 0 : v;
    }
    float HandBoxRelAlpha()     { return std::clamp(g_tune.handBoxRelAlpha, 0.f, 1.f); }
    bool  HandBoxPhaseLogOn()   { return g_tune.handBoxPhaseLog > 0.5f; }
    bool  HandBoxStepDtOn()     { return g_tune.handBoxStepDt > 0.5f; }
    float HandBoxTrack()        { return std::clamp(g_tune.handBoxTrack, 0.05f, 1.f); }
    bool  HandBoxWarpOn()       { return g_tune.handBoxWarp > 0.5f; }
    bool  PlayerWandOn()        { return g_tune.playerWand > 0.5f; }
    float PlayerWandR()         { return (std::max)(0.3f, g_tune.playerWandR); }
    unsigned PlayerWandPart()   {
        int v = (int)(g_tune.playerWandPart + 0.5f);
        // forbidden: 0 (non-part), 2 (PerfSys cross-pelvis), 3/5 (HIGGS hands), 4 (our boxes),
        // 6 (HIGGS declared), 8/14 (PerfSys self-thigh pair), 29 (markers), 30 (rig signature)
        if (v < 1 || v > 28 || v == 2 || v == 3 || v == 4 || v == 5 || v == 6 || v == 8 || v == 14)
            v = 9;
        return (unsigned)v;
    }
    bool  PlayerWandLogOn()     { return g_tune.playerWandLog > 0.5f; }
    int   HandBoxPrivGroupMode() {
        int v = (int)(g_tune.handBoxPrivGroup + 0.5f);
        return (v < 0 || v > 2) ? 0 : v;
    }
    unsigned HandBoxPrivGroupId() {
        unsigned v = (unsigned)(int)(g_tune.handBoxPrivGroupId + 0.5f) & 0xFFFFu;
        return v ? v : 0xFFF0u;                       // 0 would alias "ungrouped" -> never allow
    }
    int   HandBoxNullUserDataMode() {
        int v = (int)(g_tune.handBoxNullUserData + 0.5f);
        return (v < 0 || v > 2) ? 0 : v;   // 0 off / 1 always-on / 2 auto (on after first load)
    }
    int   SceneSuspendHandsMode()  { return (int)(g_tune.sceneSuspendHands + 0.5f); }
    float SceneFirstDistU()        { return g_tune.sceneFirstDistU; }
    bool  ApiWeaponDrawnOnly()     { return g_tune.apiWeaponDrawnOnly > 0.5f; }
    bool  WeaponSheathedColOff()   { return g_tune.weaponSheathedColOff > 0.5f; }
    bool  PivGuardCombatLooseOn()  { return g_tune.pivGuardCombatLoose > 0.5f; }
    bool  BumperSceneOff()      { return g_tune.bumperSceneOff > 0.5f; }
    float BumperSceneExcite()   { return g_tune.bumperSceneExcite; }
    bool  PlanckLoosenOursOn()  { return g_tune.planckLoosenOurs > 0.5f; }
    float PlanckLoosenGlobal() { return g_tune.planckLoosenGlobal; }
    bool  TouchProbeHudOn()     { return g_tune.touchProbeHud > 0.5f; }
    float TouchProbeHudU()      { return g_tune.touchProbeHudU; }
    bool  DgHeadStripHairOn()   { return g_tune.dgHeadStripHair > 0.5f; }
    bool  GhostVizEnabled()     { return g_tune.ghostViz > 0.5f; }
    float GhostRangeU()         { return g_tune.ghostRangeU; }
    float GhostBreastPadU()     { return g_tune.ghostBreastPadU; }
    float GhostButtPadU()       { return g_tune.ghostButtPadU; }
    float MouthGhost(int i) {
        switch (i) {
        case 0: return g_tune.mouthGhostAX; case 1: return g_tune.mouthGhostAY;
        case 2: return g_tune.mouthGhostAZ; case 3: return g_tune.mouthGhostBX;
        case 4: return g_tune.mouthGhostBY; case 5: return g_tune.mouthGhostBZ;
        default: return g_tune.mouthGhostR;
        }
    }
    float MouthEnterU()         { return g_tune.mouthEnterU; }
    float MouthExitU()          { return g_tune.mouthExitU; }
    float MouthPhonemeIdx()     { return g_tune.mouthPhonemeIdx; }
    float MouthPhonemeMax()     { return g_tune.mouthPhonemeMax; }
    float MouthPhoneme2Idx()    { return g_tune.mouthPhoneme2Idx; }
    float MouthPhoneme2Max()    { return g_tune.mouthPhoneme2Max; }
    bool  MouthNeedFinger()     { return g_tune.mouthNeedFinger > 0.5f; }
    float MouthDeepChinU()      { return g_tune.mouthDeepChinU; }
    float MouthDeepPalU()       { return g_tune.mouthDeepPalU; }
    int   MouthThroatChild()    { return static_cast<int>(g_tune.mouthThroatChild + 0.5f); }
    float MouthThroatU()        { return g_tune.mouthThroatU; }
    int   MouthBeastChild(bool khajiit, int which)
    {
        const float* kha[5] = { &g_tune.mouthKhaE1, &g_tune.mouthKhaE2, &g_tune.mouthKhaE3,
                                &g_tune.mouthKhaC1, &g_tune.mouthKhaC2 };
        const float* arg[5] = { &g_tune.mouthArgE1, &g_tune.mouthArgE2, &g_tune.mouthArgE3,
                                &g_tune.mouthArgC1, &g_tune.mouthArgC2 };
        if (which < 0 || which > 4) return -1;
        return static_cast<int>(*(khajiit ? kha : arg)[which] + 0.5f);
    }
    float MouthBeastEnterU()    { return g_tune.mouthBeastEnterU; }
    float MouthRampIn()         { return g_tune.mouthRampIn; }
    float MouthRampOut()        { return g_tune.mouthRampOut; }
    float MouthFingerBox()      { return g_tune.mouthFingerBox; }
    bool  TouchProbeEnabled()   { return g_tune.touchProbe > 0.5f; }
    float TouchProbeRangeU()    { return g_tune.touchProbeRangeU; }

    // ── ORIFICE DRIVE (2026-08-19) ──────────────────────────────────────────────────────────
    // Every accessor clamps. The written value lands on a BONE, and a NaN there is a body bent
    // to infinity that no restore can undo (rest + NaN*dir = NaN, and NaN != rest forever), so
    // the `!(v > lo)` form is used deliberately — it catches NaN as well as under-range.
    static inline float OrifCl(float v, float lo, float hi) {
        if (!(v > lo)) return lo;            // NaN-safe: NaN fails every comparison
        return v > hi ? hi : v;
    }
    bool  OrificeEnabled()      { return g_tune.orificeEnable  > 0.5f; }
    bool  OrificeVaginal()      { return g_tune.orificeVaginal > 0.5f; }
    bool  OrificeAnal()         { return g_tune.orificeAnal    > 0.5f; }
    bool  OrificeOral()         { return g_tune.orificeOral    > 0.5f; }
    float OrificeGateTol()      { return OrifCl(g_tune.orificeGateTol,     0.f,  2.f); }
    float OrificeEaseHz()       { return OrifCl(g_tune.orificeEaseHz,      0.1f, 60.f); }
    float OrificeRadiusGain()   { return OrifCl(g_tune.orificeRadiusGain,  0.f,  1.f); }
    float OrificeRefRadU()      { return OrifCl(g_tune.orificeRefRadU,     0.1f, 20.f); }
    float OrificeFingerRadU()   { return OrifCl(g_tune.orificeFingerRadU,  0.f,  10.f); }
    float OrificeOpenMinU(int kind) {
        return OrifCl(kind == 1 ? g_tune.orificeAnalMinU : g_tune.orificeVagMinU, 0.f, 20.f);
    }
    float OrificeOpenMaxU(int kind) {
        // Max is floored at Min so an inverted pair can never produce a NEGATIVE gape (which
        // would pull the ring inward and read as a foreign write on the next frame).
        const float lo = OrificeOpenMinU(kind);
        return OrifCl(kind == 1 ? g_tune.orificeAnalMaxU : g_tune.orificeVagMaxU, lo, 20.f);
    }
    float OrificeRangeU()       { return OrifCl(g_tune.orificeRangeU,        1.f,  400.f); }
    float OrificeForeignHoldS() { return OrifCl(g_tune.orificeForeignHoldS,  0.1f, 30.f); }
    bool  OrificeLogEnabled()   { return g_tune.orificeLog > 0.5f; }
    float LmNeutLimbChord(int r) {
        if (r == 5) return g_tune.lmNeutLegChord;   // thighs
        if (r == 6) return g_tune.lmNeutArmChord;   // upperArms
        return 0.f;
    }
    float LmNeutLimbChordM(int r) {                 // MALE mirror (2026-08-21)
        if (r == 5) return g_tune.lmNeutMLegChord;  // thighs
        if (r == 6) return g_tune.lmNeutMArmChord;  // upperArms
        return 0.f;
    }
    void  LmBreastModel(float o[14]) {
        o[0]=g_tune.lmBrAYc; o[1]=g_tune.lmBrAYm; o[2]=g_tune.lmBrBYc; o[3]=g_tune.lmBrBYm;
        o[4]=g_tune.lmBrAZc; o[5]=g_tune.lmBrAZm; o[6]=g_tune.lmBrBZc; o[7]=g_tune.lmBrBZm;
        o[8]=g_tune.lmBrRc;  o[9]=g_tune.lmBrRm;  o[10]=g_tune.lmBrAX;
        o[11]=g_tune.lmBrLatK; o[12]=g_tune.lmBrLatX; o[13]=g_tune.lmBrLatY0;
    }
    void  LmBreastModelEx(float o[5]) {
        o[0]=g_tune.lmBrHK; o[1]=g_tune.lmBrMidZ0; o[2]=g_tune.lmBrCupSat;
        o[3]=g_tune.lmBrSagLo; o[4]=g_tune.lmBrSagHi;
    }
    void  LmNeutralPos(int i, float o[3]) {
        // idx order matches kUvLandmarks: nipple, brUp, brDn, chest, belly, waist, butt
        const float* t = nullptr;
        const float v[7][3] = {
            { g_tune.lmNeutNipX,   g_tune.lmNeutNipY,   g_tune.lmNeutNipZ   },
            { g_tune.lmNeutBrUpX,  g_tune.lmNeutBrUpY,  g_tune.lmNeutBrUpZ  },
            { g_tune.lmNeutBrDnX,  g_tune.lmNeutBrDnY,  g_tune.lmNeutBrDnZ  },
            { g_tune.lmNeutChestX, g_tune.lmNeutChestY, g_tune.lmNeutChestZ },
            { g_tune.lmNeutBellyX, g_tune.lmNeutBellyY, g_tune.lmNeutBellyZ },
            { g_tune.lmNeutWaistX, g_tune.lmNeutWaistY, g_tune.lmNeutWaistZ },
            { g_tune.lmNeutButtX,  g_tune.lmNeutButtY,  g_tune.lmNeutButtZ  },
        };
        if (i < 0 || i > 6) { o[0] = o[1] = o[2] = 0.f; return; }
        t = v[i];
        o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
    }
    void  LmNeutralPosM(int i, float o[3]) {
        // MALE mirror (2026-08-21). Same index order as kUvLandmarks; indices 0..2 (the
        // breast landmarks) return 0 BY DESIGN — the male table has no breast channel, and a
        // zero neutral is the established "channel absent/self-disabled" value everywhere.
        o[0] = o[1] = o[2] = 0.f;
        switch (i) {
        case 3: o[0]=g_tune.lmNeutMChestX; o[1]=g_tune.lmNeutMChestY; o[2]=g_tune.lmNeutMChestZ; break;
        case 4: o[0]=g_tune.lmNeutMBellyX; o[1]=g_tune.lmNeutMBellyY; o[2]=g_tune.lmNeutMBellyZ; break;
        case 5: o[0]=g_tune.lmNeutMWaistX; o[1]=g_tune.lmNeutMWaistY; o[2]=g_tune.lmNeutMWaistZ; break;
        case 6: o[0]=g_tune.lmNeutMButtX;  o[1]=g_tune.lmNeutMButtY;  o[2]=g_tune.lmNeutMButtZ;  break;
        default: break;
        }
    }
    float TipProbe(int a)       { return a == 0 ? g_tune.tipProbeX : (a == 1 ? g_tune.tipProbeY : g_tune.tipProbeZ); }
    float NipUV(int a)          { return a == 0 ? g_tune.nipUVu : g_tune.nipUVv; }
    float BoneNeutral(int region) {
        switch (region) {
        case 0: return g_tune.boneNeutralChest;   case 1: return g_tune.boneNeutralBreasts;
        case 2: return g_tune.boneNeutralBelly;   case 3: return g_tune.boneNeutralWaist;
        case 4: return g_tune.boneNeutralButt;    case 5: return g_tune.boneNeutralThighs;
        case 6: return g_tune.boneNeutralArms;    default: return 0.f;
        }
    }
    bool  MeshMarkersEnabled()  { return g_tune.meshMarkers > 0.5f; }
    float MeshRadiusGain()      { const float v = g_tune.meshRadiusGain; return v < 0.f ? 0.f : (v > 1.5f ? 1.5f : v); }
    float MeshShiftButtK()      { return g_tune.meshShiftButtK; }
    float MeshShiftBreastFwdK() { return g_tune.meshShiftBreastFwdK; }
    float MeshShiftBreastZK()   { return g_tune.meshShiftBreastZK; }
    float MeshNeutralBreastZ()  { return g_tune.meshNeutralBreastZ; }
    float MeshNeutralBreastCup(){ return g_tune.meshNeutralBreastCup; }
    float MeshCupFrac()         { const float v = g_tune.meshCupFrac; return v < 0.2f ? 0.2f : (v > 0.9f ? 0.9f : v); }
    float MeshNeutral(int region) {
        switch (region) {
        case 0: return g_tune.meshNeutralChest;   case 1: return g_tune.meshNeutralBreasts;
        case 2: return g_tune.meshNeutralBelly;   case 3: return g_tune.meshNeutralWaist;
        case 4: return g_tune.meshNeutralButt;    case 5: return g_tune.meshNeutralThighs;
        case 6: return g_tune.meshNeutralArms;    default: return 0.f;
        }
    }
    float MeshBandLo(int region) {
        switch (region) {
        case 0: return g_tune.meshBandChestLo;  case 1: return g_tune.meshBandBreastLo;
        case 2: return g_tune.meshBandBellyLo;  case 3: return g_tune.meshBandWaistLo;
        case 4: return g_tune.meshBandButtLo;   case 5: return g_tune.meshBandThighLo;
        case 6: return g_tune.meshBandArmLo;    default: return 0.f;
        }
    }
    float MeshBandHi(int region) {
        switch (region) {
        case 0: return g_tune.meshBandChestHi;  case 1: return g_tune.meshBandBreastHi;
        case 2: return g_tune.meshBandBellyHi;  case 3: return g_tune.meshBandWaistHi;
        case 4: return g_tune.meshBandButtHi;   case 5: return g_tune.meshBandThighHi;
        case 6: return g_tune.meshBandArmHi;    default: return 1.f;
        }
    }
    float MeshArmFrac()         { const float v = g_tune.meshArmFrac; return v < 0.1f ? 0.1f : (v > 0.9f ? 0.9f : v); }
    float FsmpMassScale()       { return g_tune.fsmpMassScale; }
    float FsmpContactGate()     { return g_tune.fsmpContactGate; }
    float FsmpContactDevU()     { return g_tune.fsmpContactDevU; }
    float FsmpContactHoldMS()   { const float v = g_tune.fsmpContactHoldMS;      // clamp before the (int)
                                  return !(v >= 0.f) ? 0.f                       // cast: a huge/NaN file
                                       : (v > 5000.f ? 5000.f : v); }            // value is float->int UB

    // ── SCULPT GATE storage (see Tuning.h) — written once at kDataLoaded (main thread),
    // read by CapFix on the main thread. Plain char buffer: no allocation, no locking.
    static char s_sculptTarget[96] = {};
    static int  s_sculptSlot = -1;
    void SetSculptTarget(const char* f, int slot) {
        s_sculptTarget[0] = 0;
        s_sculptSlot = slot;
        if (f && *f) {
            // Basename-normalize (review 2026-07-17): a path-formatted value copied from the
            // race-line format would otherwise never match the basename compare in SculptAllows.
            const char* base = f;
            for (const char* p = f; *p; ++p)
                if (*p == '\\' || *p == '/') base = p + 1;
            std::snprintf(s_sculptTarget, sizeof(s_sculptTarget), "%s", base);
        }
    }
    const char* SculptTarget()     { return s_sculptTarget; }
    static const void* s_biasRace[8] = {};
    static float       s_biasMult[8] = {};
    static int         s_biasN = 0;
    void  ClearRaceBias() { s_biasN = 0; }
    void  AddRaceBias(const void* race, float mult)
    {
        if (race && s_biasN < 8) { s_biasRace[s_biasN] = race; s_biasMult[s_biasN] = mult; ++s_biasN; }
    }
    float RaceBiasOf(const void* race)
    {
        for (int i = 0; i < s_biasN; ++i)
            if (s_biasRace[i] == race) return s_biasMult[i];
        return 1.f;
    }
    int         SculptTargetSlot() { return s_sculptSlot; }

    // ── PER-NPC CAPSULE OVERRIDES (2026-07-17) — see Tuning.h. Key = formId<<12 | slot<<8 | child
    // (slot < 16, child < 256; both hold: max slot 11, max child kMaxListChildren 24).
    namespace {
        std::unordered_map<std::uint64_t, std::array<float, 7>> s_npcCap;
        inline std::uint64_t NpcCapKey(std::uint32_t formId, int slot, int child) {
            return (static_cast<std::uint64_t>(formId) << 12)
                 | (static_cast<std::uint64_t>(slot & 0xF) << 8)
                 | static_cast<std::uint64_t>(child & 0xFF);
        }
    }
    void ClearNpcCapOverrides() { s_npcCap.clear(); }
    void AddNpcCapOverride(std::uint32_t formId, int slot, int child,
                           const float a[3], const float b[3], float r)
    {
        s_npcCap[NpcCapKey(formId, slot, child)] = { a[0], a[1], a[2], b[0], b[1], b[2], r };
    }
    bool NpcCapOverride(std::uint32_t formId, int slot, int child,
                        float a[3], float b[3], float* r)
    {
        if (s_npcCap.empty()) return false;                  // the everyone-else fast path
        auto it = s_npcCap.find(NpcCapKey(formId, slot, child));
        if (it == s_npcCap.end()) return false;
        const auto& v = it->second;
        a[0] = v[0]; a[1] = v[1]; a[2] = v[2];
        b[0] = v[3]; b[1] = v[4]; b[2] = v[5];
        if (r) *r = v[6];
        return true;
    }
    int NpcCapOverrideCount() { return static_cast<int>(s_npcCap.size()); }
    float NpcFingerCurlGain()   { return g_tune.npcFingerCurlGain; }
    float NpcFingerCurlDecay()  { return g_tune.npcFingerCurlDecay < 0.f ? 0.f : g_tune.npcFingerCurlDecay; }
    float NpcFingerCurlMax()    { return g_tune.npcFingerCurlMax < 0.f ? 0.f : (g_tune.npcFingerCurlMax > 1.f ? 1.f : g_tune.npcFingerCurlMax); }
    float NpcFingerCurlMode()   { return g_tune.npcFingerCurlMode; }
    float NpcFingerCurlLagGate(){ return g_tune.npcFingerCurlLagGate; }
    float NpcFingerVsClutter()  { return g_tune.npcFingerVsClutter; }
    float NpcFingerVsWorld()    { return g_tune.npcFingerVsWorld; }
    float NpcFingerVsNpc()      { return g_tune.npcFingerVsNpc; }
    float NpcFingerAlpha()      { return g_tune.npcFingerAlpha < 0.05f ? 0.05f : (g_tune.npcFingerAlpha > 1.f ? 1.f : g_tune.npcFingerAlpha); }
    float NpcFingerSnapU()      { return g_tune.npcFingerSnapU < 5.f ? 5.f : g_tune.npcFingerSnapU; }
    float NpcFingerMaxVel()     { return g_tune.npcFingerMaxVel < 0.5f ? 0.5f : g_tune.npcFingerMaxVel; }
    float NpcFingerGravity()    { return g_tune.npcFingerGravity; }
    bool  NpcFingerLogEnabled() { return g_tune.npcFingerLog > 0.5f; }

    // ── PLAYER HAND 4-BOX (2026-07-09, HandBox.cpp) — read live per frame. ──
    float HiggsSlabHalfX()      { return g_tune.higgsSlabHalfX; }   // -1 = leave alone (HandBox interprets)
    float HiggsSlabHalfY()      { return g_tune.higgsSlabHalfY; }
    float HiggsSlabHalfZ()      { return g_tune.higgsSlabHalfZ; }
    bool  HiggsSlabAllowGrow()  { return g_tune.higgsSlabAllowGrow > 0.5f; }   // X may exceed baseline (2x cap); Y/Z shrink-only
    bool  HandBoxEnabled()      { return g_tune.handBoxEnable > 0.5f && g_tune.handBoxArm > 0.5f; }   // review N9 double gate
    int   HandBoxFollowMode()   { const int m = (int)(g_tune.handBoxFollowMode + 0.5f);
                                    return m < 0 ? 0 : (m > 2 ? 2 : m); }   // 2 = HIGGS-anchored (2026-07-12)
    float HandBoxTipFrac()      { return g_tune.handBoxTipFrac < 0.f ? 0.f : (g_tune.handBoxTipFrac > 2.f ? 2.f : g_tune.handBoxTipFrac); }
    // 2026-07-10: box-3 (tip plate) knobs — decoupled from the index boxes (index long, plate short).
    float HandBoxPlateTipFrac() { return g_tune.handBoxPlateTipFrac < 0.f ? 0.f : (g_tune.handBoxPlateTipFrac > 2.f ? 2.f : g_tune.handBoxPlateTipFrac); }
    float HandBoxPlateLenFrac() { return g_tune.handBoxPlateLenFrac < 0.2f ? 0.2f : (g_tune.handBoxPlateLenFrac > 1.f ? 1.f : g_tune.handBoxPlateLenFrac); }
    // 2026-07-10: box-2 (3-finger slab) twin of PlateLenFrac — base (knuckle-ward) face pinned in SolveGeometry.
    float HandBoxSlabLenFrac()  { return g_tune.handBoxSlabLenFrac < 0.2f ? 0.2f : (g_tune.handBoxSlabLenFrac > 1.f ? 1.f : g_tune.handBoxSlabLenFrac); }
    float HandBoxPlateTiltDeg()  { const float d = g_tune.handBoxPlateTiltDeg; return d < -90.f ? -90.f : (d > 90.f ? 90.f : d); }
    int   HandBoxPlateTiltAxis() { const int a = (int)(g_tune.handBoxPlateTiltAxis + 0.5f); return (a >= 0 && a <= 2) ? a : 2; }
    // Stage 2 (2026-07-10): composed after stage 1 in SolveGeometry. ±180 clamp; axis default 1 (Y).
    float HandBoxPlateTilt2Deg()  { const float d = g_tune.handBoxPlateTilt2Deg; return d < -180.f ? -180.f : (d > 180.f ? 180.f : d); }
    int   HandBoxPlateTilt2Axis() { const int a = (int)(g_tune.handBoxPlateTilt2Axis + 0.5f); return (a >= 0 && a <= 2) ? a : 1; }
    float HandBoxIdxHalfW()     { return g_tune.handBoxIdxHalfW < 0.004f ? 0.004f : g_tune.handBoxIdxHalfW; }
    // review N10: half-THICKNESS floored at 0.010 m — 0.6-0.7u sits below every proven
    // tunneling figure; the flesh capsule's own radius pads the envelope, but never bet on it.
    float HandBoxIdxHalfT()     { return g_tune.handBoxIdxHalfT < 0.010f ? 0.010f : g_tune.handBoxIdxHalfT; }
    float HandBoxSlabHalfT()    { return g_tune.handBoxSlabHalfT < 0.010f ? 0.010f : g_tune.handBoxSlabHalfT; }
    float HandBoxPad()          { return g_tune.handBoxPad < 0.f ? 0.f : g_tune.handBoxPad; }
    unsigned HandBoxSubLayer() {
        int v = (int)(g_tune.handBoxSubLayer + 0.5f);
        if (v < 0 || v > 31) v = 4;
        // 2/3/5/6 are HIGGS's OWN parts (hand 3/5, declared 2/6); 30 is the NPC-finger
        // signature part. A box stamped with any of those aliases another system's
        // identity checks. Remap SILENTLY to SkipBoth (no logging — see below).
        // ⚠ MAIN THREAD ONLY (corrected 2026-07-16 — this comment used to say the opposite):
        // this accessor does FLOAT work, so it must NEVER be called from a collision-thread
        // callback. Its value reaches FilterDecision via NpcFinger's g_filterKnobs cache and
        // HandBox's g_boxPart — both sampled on the main thread. The hazard is NOT just
        // logger/fmt (the narrow 07-09 framing): HIGGS's comparator trampoline calls us with
        // RSP 8-mod-16, so ANY float/SIMD there can spill xmm6 via `movaps` and #GP.
        // PROVEN 2026-07-16 (CTD on load). See KNOWLEDGEBASE + Pitfall Ledger.
        if (v == 2 || v == 3 || v == 5 || v == 6 || v == 30) v = 4;
        return (unsigned)v;
    }
    float HandBoxLeashU()       { return g_tune.handBoxLeashU; }
    // handBoxRebuildFrac semantics, corrected 2026-07-29:
    //   <= 0  -> DISABLED: never rebuild the rig on a scale change (returns an unreachable
    //            fraction). Previously 0 fell through the floor clamp and became 0.02 = 2%, i.e.
    //            the intuitive "off" made the churn WORSE than the 15% it replaced. A knob whose
    //            zero value is more aggressive than its default is a footgun; fixed.
    //   > 0   -> the fraction, floored at 0.02 so a typo like 0.001 cannot reinstate the churn.
    float HandBoxRebuildFrac()
    {
        if (g_tune.handBoxRebuildFrac <= 0.f) return 1.0e9f;   // never reached by any real scale
        return g_tune.handBoxRebuildFrac < 0.02f ? 0.02f : g_tune.handBoxRebuildFrac;
    }
    float HandBoxMaxVel()       { return g_tune.handBoxMaxVel < 1.f ? 1.f : g_tune.handBoxMaxVel; }
    bool  HandBoxBeast()        { return g_tune.handBoxBeast > 0.5f; }
    float HandBoxDump()         { return g_tune.handBoxDump; }

    // ── per-box LIVE DIALS (2026-07-10) — read live per frame by SolveGeometry/ResolveGeometryLive.
    // Offsets clamp ±10u; plate half overrides clamp 0.05..5u with -1 passthrough (= keep solved);
    // index tilt clamps ±180 deg; axis sanitized to 0/1/2 (default 2 = down-the-bone). ──
    static float ClampBoxOffU(float v)  { return v < -10.f ? -10.f : (v > 10.f ? 10.f : v); }
    static float ClampBoxHalfU(float v) { return v < 0.f ? -1.f : (v < 0.05f ? 0.05f : (v > 5.f ? 5.f : v)); }   // -1 = solved
    float HandBoxPlateOffX()    { return ClampBoxOffU(g_tune.handBoxPlateOffX); }
    float HandBoxPlateOffY()    { return ClampBoxOffU(g_tune.handBoxPlateOffY); }
    float HandBoxPlateOffZ()    { return ClampBoxOffU(g_tune.handBoxPlateOffZ); }
    float HandBoxPlateHalfW()   { return ClampBoxHalfU(g_tune.handBoxPlateHalfW); }
    float HandBoxPlateHalfT()   { return ClampBoxHalfU(g_tune.handBoxPlateHalfT); }
    float HandBoxIdxOffX()      { return ClampBoxOffU(g_tune.handBoxIdxOffX); }
    float HandBoxIdxOffY()      { return ClampBoxOffU(g_tune.handBoxIdxOffY); }
    float HandBoxIdxOffZ()      { return ClampBoxOffU(g_tune.handBoxIdxOffZ); }
    float HandBoxIdxTiltDeg()   { const float d = g_tune.handBoxIdxTiltDeg; return d < -180.f ? -180.f : (d > 180.f ? 180.f : d); }
    int   HandBoxIdxTiltAxis()  { const int a = (int)(g_tune.handBoxIdxTiltAxis + 0.5f); return (a >= 0 && a <= 2) ? a : 2; }
    float HandBoxDumpNow()      { return g_tune.handBoxDumpNow; }

    // HEEL FIX control — a RUNTIME flag flipped from the in-game console (`hf`), NOT the tuning file:
    // the file only sets the startup default. User rule: runtime switches are console/spell, never file edits.
    static std::atomic<bool> g_heelFixOn{ false };
    bool HeelFixEnabled() { return g_heelFixOn.load(std::memory_order_relaxed); }
    bool SetHeelFix(bool on) { g_heelFixOn.store(on, std::memory_order_relaxed); return on; }
    bool ToggleHeelFix() { return SetHeelFix(!HeelFixEnabled()); }
    // Startup default (2026-07-02, fix VERIFIED): read `heelFix` from the tuning file ONCE at DataLoaded —
    // per-NPC detection is automatic (the live "NPC"-node offset: stuck/failed heels read 0 → correctly
    // skipped; late-applying heels are picked up the same frame). The console `hf` stays the live override.
    void InitHeelFixDefault() {
        ReloadGrabTune();
        SetHeelFix(g_tune.heelFix > 0.5f);
        logger::info("HeelFix startup default: {} (tuning file heelFix={:.0f}; toggle live with 'hf')",
                     HeelFixEnabled() ? "ON" : "OFF", g_tune.heelFix);
    }
}
