# 22 — The Runtime Skeleton Map + Per-NPC Capsule Layer (+ NPC dossiers) — 2026-07-17

**Status: DEPLOYED (DLL 2026-07-17 04:00), UNTESTED in-game. READ THIS before any per-race or
per-NPC skeleton/capsule work.** This replaces the ESP-override approach to per-race skeletons
(user directive: *"no .esp dependence"*) and adds a per-NPC geometry layer. Verification checklist
at the bottom.

## 1. Why (the user's design)

*"Whenever a skeleton is given to an NPC by the engine, if it's a different race, we override that
with our own. No .esp dependence — and that way we solve our specific-NPC problem: we can create a
custom skeleton for a specific NPC, like Auri, and at runtime give her her own skeleton regardless
of record."* This is strictly better than ESP race overrides: no masters (the scoping run showed
the elf/orc plan would have mastered `PAN_NPCs CuttingRoomFloor PATCH.esp` — a compat patch, the
most delete-prone plugin class), no whole-record composition (a race override replaces ALL ~50
fields and silently reverts other mods' face/stat edits unless composed), and it reaches CUSTOM
follower races without touching their plugins.

## 2. The race map — as built

- **Mechanism:** `TESRace::skeletonModels[SEXES::kFemale]` (TESRace.h:317, the ANAM field) is the
  live in-memory record the engine reads at EVERY actor 3D build. Its `TESModel` has a virtual
  `SetModel(const char*)`. One write at **kDataLoaded** (all plugin winners final, no 3D exists yet)
  redirects every subsequent skeleton load for that race. The behavior graph is a SEPARATE field
  (TESRace.h:323) → animation untouched. Nothing else in this load order writes skeleton paths at
  runtime (scoped 2026-07-17), and SkyPatcher **cannot** (its race patcher has no skeleton op —
  verified from the bundled grammar reference, not guessed).
- **Code:** `main.cpp::ApplySkeletonMap()` (called in the kDataLoaded case, after
  `RegisterConsoleCommands`). Race lookup = `RE::TESForm::LookupByEditorID<RE::TESRace>` (races
  natively retain editor IDs). Config resolve = the PPB_tuning two-path pattern (dev path, then
  `Data/SKSE/Plugins/` USVFS fallback).
- **Config:** `PPB_skeletons.txt` in the PPB mod's SKSE/Plugins — read ONCE at kDataLoaded (NOT
  hot-reloaded; a change needs a game restart). Format:
  `race <RaceEditorID> <path relative to Data\meshes\>`. Receipts: PPB.log `SKELMAP:` lines
  (before→after path per race + a totals line).
- **ACTIVE entries (as of 2026-07-19 night):** `MRTMrissiRace → skeleton_female.nif` (dossier §5),
  `zzzCrbHalfDragonRace → skeleton_female.nif` (Ulliss/Austella/Hoarfrost Witch — their
  `actors\Crb\HalfDragon\skeleton.nif` verified a STRICT SUBSET of ours: 648 nodes, 0 extras, chain
  locals identical 0.0000u; equipable CrbTail skins vanilla TailBone01..05 ✓, CrbHorn head-rigid ✓),
  and `DraeneiRace → skeleton_female_draenei.nif` (Yvanni — personal NIF, byte-copy of ours incl.
  the 23-child head, for her hoof/horn capsule dial+bake; her old skeleton was pre-Softbody XP32
  with stock collision; every worn mesh verified to skin only bones we carry; Mal_Tail SMP bones
  are tail-NIF-carried). **capHeadC knobs extended 16 → 22 (2026-07-19 night, DLL 3f41bc88; the
  four coupled edits done)** so all 8 head seeds are LIVE-dialable behind a sculpt gate — Yvanni's
  horns bake into HER NIF; Auri's antlers get dialed the same way then TRANSCRIBED to npcCap lines.
  The `OrcRace` line was REMOVED — orc females use the default `skeleton_female.nif` (user: "orc jaw
  not different enough"); `skeleton_female_orc.nif` and `skeleton_female_elf.nif` are DELETED (archived
  to `tools/ppb-scratch/deleted_skeletons/`). Elves already fell back to the human skeleton.
  Verification script for future race repoints: `tools/ppb-scratch/verify_swap_safety.py` (node-set
  diff + chain-local proportions + per-mesh skinned-bone subset check — run it BEFORE any repoint).
- **STAGED (commented out):** Auri's npcCap head lines (children 15..22 of the human head — the 8 new
  seeds; §5). No sequencing gate remains for elves/orc (they just share the human skeleton).
- **Migration TODO (after the map is proven):** move `KhajiitRace` into the map and delete the
  PPB.esp race override → drops the `fluffy khajiit.esp` master; PPB.esp shrinks back to spells only.

## 3. The per-NPC capsule layer — as built

"A personal skeleton per NPC" at the only layer PPB cares about: collision children. The engine
gives every actor a **private clone** of the skeleton's shapes at 3D load (long-proven — the reason
dial writes never leak between NPCs), so per-NPC geometry = per-actor writes into seed children:

- **Config:** `npcCap <localHexID>:<Plugin.esp> <slot> <childIdx> ax ay az bx by bz r` in
  PPB_skeletons.txt. Slots by name: hand/forearm/upperarm/head/spine0/1/2/thigh/calf/foot/com
  (`SlotIndexByName`, main.cpp). FormID resolved at kDataLoaded via
  `TESDataHandler::LookupForm(local, plugin)` → runtime FormID.
- **Store:** `Tuning.cpp` — `s_npcCap` map keyed `formId<<12 | slot<<8 | child`; accessors
  `ClearNpcCapOverrides / AddNpcCapOverride / NpcCapOverride` (`ObjectHold` ns). MAIN THREAD only.
- **Apply:** `CapFix.cpp` child loop — the override is fetched next to `CapFixChildSlot` and **WINS
  over the global knob, and applies even where no knob block exists** (`haveKnob || haveNpc`).
  Everyone else keeps the NIF's buried seeds.
- **Seeds:** extra children shipped in the group NIF as TINY capsules — 0.5u rod, r 0.05u, at the
  bone origin (never A==B, the degenerate-invisible trap; NOT clones of a sibling — a clone would
  duplicate live face geometry on every user of the file).

## 4. The NIF variants (all in the PPB mod, `character assets female/`)

| file | head | spine0/1/2 | carries | who |
|---|---|---|---|---|
| skeleton_female.nif | **23** | 8/7/15 | C1..14 human skull (baked) + **C15..22 = 8 head-joint seeds** (horns/antlers, buried; 2u rod r0.5u at origin) | ALL non-beast females + M'rissi + Auri |
| skeletonbeast_female_khajiit.nif | 17 | 8/7/15 | C15/C16 = ears (BAKED) | Khajiit (ESP) |
| skeletonbeast_female.nif | 17 | **10/9/17** | horns + dorsal-ridge (BAKED) | Argonian (record) |

~~skeleton_female_elf.nif / skeleton_female_orc.nif~~ DELETED 2026-07-19 (archived to
`tools/ppb-scratch/deleted_skeletons/`). The elf NIF's 2 antler seeds are SUPERSEDED by the 8
head-joint seeds now in `skeleton_female.nif`.
Creation scripts: `tools/ppb-scratch/extend_beast_lists.py` (khajiit/argonian) and
`tools/ppb-scratch/extend_head_seeds8.py` (the 15→23 human head seed run — clone child[1] then
overwrite geometry to a tiny buried rod; reload-verified; backup `skeleton_female.nif.bak_pre_headseeds8`).

## 5. Per-NPC dossiers (scoped 2026-07-17 — houseCARL + pynifly, all verified)

- **AURI** (Song of the Green): NPC `000D63:018Auri.esp`, record winner `_018Auri.esp` ("Auri - A
  High Poly Replacer"). **Vanilla WoodElfRace** — NOT a custom race, shares skeleton_female.nif, so
  she already has the full PPB bake and per-race tricks can't isolate her → she is THE per-NPC-layer
  case. Antlers = **boneless HEAD PART** (`auriantlers`, Type=Scars) baked into her FaceGen
  (`.../FaceGenData/FaceGeom/018Auri.esp/00000D63.nif`, shape skinned 100% to `NPC Head [Head]`);
  an antler ARMO exists in her mod but nothing wears it here. **Plan (revised 2026-07-19):** she
  loads the shared `skeleton_female.nif`, which now carries **8 head-joint seed capsules at child
  indices 15..22**; dial up to 8 of them onto her antlers by eye and write the values to
  `npcCap 000D63:018Auri.esp head 15..22 …` in PPB_skeletons.txt (per-NPC → only her clone shows
  them; every other elf/human keeps them buried). Base FormID to summon: `player.placeatme` her base
  (018Auri.esp local 000D63). Measure endpoints from the facegen shape; re-derive if her replacer
  is swapped. ⚠ npcCap is parsed once at kDataLoaded → a RESTART per change (not hot-reloaded like
  cap* knobs), so consider dialing live via a sculpt-gated capHeadC knob session first, then bake
  the settled values into her npcCap lines.
- **M'RISSI**: NPC `00AA19:MrissiTailOfTroubles.esp`; EXCLUSIVE race `MRTMrissiRace` ("Magically
  Alterated Khajiit"). ⚠ MEMORY CORRECTION: her `HDT TailBone001..0011` are **ARMOR-carried**
  (inside `Fluffy\tailHDT.nif`, xml foxtail.xml) — the old "skeleton-carried/custom skeleton" fact
  is FALSE (binary-grep of all four skeleton candidates; Reports 14 §1 + the SMP runbook corrected).
  Her race pointed at the SHARED `skeletonBeast_female.nif` → she was about to inherit the Argonian
  horns/ridge. Fix ACTIVE: map her to the human `skeleton_female.nif` (user: human face, half-Khajiit
  features). Bone trees are byte-copy-identical so ONLY the collision children change; her tail rig
  (table B) binds armor bones = skeleton-independent; her cat ears are boneless mesh on the head.
- **LIZZ** (Unslaad half-dragon child): NPC `0CE5F5:Unslaad.esm`, EXCLUSIVE race
  `zzzCrbHalfDragonChildRace`, custom skeleton `actors/Crb/HalfDragon/Child/skeleton_female.nif`
  won by VRTouchEvents V2.0's conform — **STOCK collision (single-capsule COM, no PPB bake)** → the
  bake gate skips her entirely today. Giving her the body = a CONFORM-GRAFT (the one heavy item):
  PPB's 19 baked bodies onto her skeleton, preserving her **340 extra bones + VRTouchEvents' 3 CME
  touch nodes** (CME Back/Face/Head — the winner-vs-original diff), shipped in PPB at her path (PPB
  outranks). Horns = EQUIPPED boneless armor rigid to `NPC Head [Head]` (`CrbHornChild.nif`) → head
  children in HER file. Tail = equipped mesh skinned to **skeleton-carried bare `TailBone01..05`**
  (bone-animated, no SMP) — the retired tbl-5 chain; gate any binding on tail GEOMETRY (every
  humanoid carries those bones dormant). No wings (bones exist, no meshes). ⚠ `PG Patcher Output`
  (modlist line 7) wins several of her meshes — edit ITS copies or re-run ParallaxGen after edits.

## 6. ★ SEQUENCING — the shared-knob trap that gates activation

The global `capHeadC15/C16` knobs currently hold the **Khajiit EAR dial values** (from the 07-16/17
sculpt; Enable 1). Any skeleton whose head has ≥17 children receives them — that's the Khajiit AND
Argonian files today, and the elf file the moment its race lines activate (every elf woman would
wear Khajiit ear capsules). Order of operations, no exceptions:
1. User confirms the ears in-game (knob-driven, live now).
2. **BAKE** C15/C16 (and the C1–C14 head dial, which is the Khajiit muzzle) into
   `skeletonbeast_female_khajiit.nif`'s head children — knob values are scale-1 base, written as
   havok metres (×0.0142875) into the child capsules (the established float-patch/pynifly class).
3. **Restore the HUMAN head knob values** from `PPB_tuning.txt.bak_prehead20` (pre-sculpt backup) —
   Lydia and every human woman is currently wearing the Khajiit muzzle dial; this is what fixes her.
   ⚠ Do NOT just set capHeadEnable 0 — her NIF holds a stale skull (the 7.46-vs-6.90 gotcha,
   Report 19 §4); RESTORE VALUES, don't kill the knob.
4. Disable/park the C15/C16 knob blocks (Enable 0) — seeds everywhere else stay buried.
5. Uncomment the elf race lines + Auri's npcCap lines; dial her antlers via the npcCap values
   (npcCap config needs a RESTART per change — it is kDataLoaded-parsed, unlike PPB_tuning).
6. Then the Argonian session re-dials C15/C16 as horns → bake into skeletonbeast_female.nif → park.

## 7. Verification checklist (first launch after 04:00 2026-07-17)

- [ ] PPB.log `SKELMAP: 'file' applied — 2 race repoint(s), 0 npcCap override(s), 0 bad line(s)`
      + per-race before→after lines.
- [ ] Spawn an Orc female (`player.placeatme 00045806 1`) → her CapFix census reads **15 head
      children** (the orc file) — mechanism proven.
- [ ] M'rissi loads the human skeleton (head 15; later: no horns when the Argonian gets them).
- [ ] Tail push: base feel unchanged at `fsmpPushMult 1.0`, tip no longer oscillating
      (`fsmpMassScale 1` — see Report 15's mass-scale entry). `fsmpMassScale 0` = live revert.

## 8. Reusable techniques recorded here

1. **In-memory record-field writes at kDataLoaded beat ESP overrides** for single-field record
   changes: no masters, no whole-record composition, survives load-order churn. Requirements: the
   field must be read live by the engine (not baked at plugin-load), a setter/layout must be
   verified in CommonLibVR headers, and nothing else may write it (scope first!).
2. **Per-actor shape clones turn one NIF into N skeletons**: seed children (tiny, buried,
   non-degenerate) + FormID-keyed value overrides = per-NPC geometry with zero engine hooks and no
   per-NPC files. Strictly safer than swapping skeleton paths per-NPC (which would race the async
   3D loader on a shared race).
3. **Custom followers split three ways** — vanilla race (Auri-class → per-NPC layer), exclusive
   race + shared skeleton (M'rissi-class → race map line), exclusive race + custom skeleton
   (Lizz-class → conform-graft, the only expensive one). Dossier the NPC FIRST (race exclusivity,
   skeleton winner, COM shape, where the appendage geometry actually lives) — every one of the
   three surprised us vs the remembered "facts".

---

## ADDENDUM 2026-07-17 (night) — ALL FOUR HEADS BAKED + THE SCULPT GATE

**HEAD SYSTEM COMPLETE (pending in-game verify):** the race NIFs carry their BAKED heads —
human `skeleton_female.nif` (was 15 children, the 7.46 skull, source `.bak_prehead20`; **now 23 after
the 2026-07-19 head-seed run — C15..22 are the 8 horn/antler seeds, backup `.bak_pre_headseeds8`**),
khajiit (17, ears), argonian (17, horns) — via `tools/ppb-scratch/bake_head_from_tuning.py`
(binary-patch + round-trip verify; handles 15/17-child lists). **Every capHead* knob is OFF** in
the live tuning; each race now reads its own NIF. ⚠ The tuning's capHead VALUES still hold the
ARGONIAN bake — never re-enable a capHead knob without a sculpt line. VERIFY next launch: spawn
Khajiit + Argonian + M'rissi + Lydia together — four distinct heads, no cross-bleed.
Known accepted limit (review LOW): knob-less head capsules engine-scale by GetScale, not arc
trueScale — a few % off on misreported-scale NPCs, consistent with all other baked-only children.

**THE SCULPT GATE (user directive — deployed):** `sculpt <nif> [slot]` in PPB_skeletons.txt.
When present, GLOBAL cap* knobs apply ONLY to actors whose race female skeleton basename matches;
npcCap stays exempt. **Prefer the slot-scoped form** (`sculpt skeletonbeast_female.nif head`) —
a slot-less line gates ALL slots and non-target actors then revert to bare NIF geometry (losing
arc re-scale + body-shape dressing) on their next ragdoll rebuild (review MEDIUM). Receipts:
"SCULPT GATE ON" at parse + a matched-race count ("matches NO race" = typo'd filename = every
dial dead — the classic silent knobs-do-nothing). Gated sites: CapFixApply main-slot loop,
ApplyListSlot children, CapAutoFitArms. Parse-side: basename-normalized; unknown slot = bad line.
**Dial-session workflow**: uncomment ONE sculpt line (restart) → re-enable the relevant knobs →
dial (only the target race moves) → bake with bake_head_from_tuning.py → knobs off → comment the
sculpt line out.
Also fixed in the same pass (review): npcCap can now target children BEYOND the knob count
(ApplyListSlot loops all live children; the knob accessor self-bounds) — the Auri-antler use-case.

---

## 9. ★ THE PER-NPC FINALIZATION WORKFLOW — bake vs npcCap vs disable (2026-07-20, EXECUTED end-to-end)

After a by-voice dial (Report 06) the shaped capsules live in GLOBAL `cap*` knobs, scoped to one actor
only by the sculpt gate. Making them PERMANENT for that ONE NPC has THREE routes — and picking the wrong
one either corrupts a face or spreads one NPC's appendage onto every woman. The decision:

| The dialed capsules are… | Finalize by | Why not the other routes |
|---|---|---|
| DISABLED global seeds (Enable 0), slot has NO ReShape response, and the NPC owns her OWN skeleton NIF | **BAKE into her NIF** (targeted — ONLY the seed children) | Yvanni's horns → her draenei NIF |
| ENABLED global knobs that ALSO carry a ReShape response (calf / foot / spine2-breasts) | **npcCap**, then reset the global knobs to the human base | can't bake+disable — disabling a ReShape-region knob kills that response for EVERY woman |
| on a SHARED skeleton (a vanilla-race NPC — Auri on `skeleton_female.nif`) | **npcCap only**; reset the global head knobs to buried seed + Enable 0 | baking would give EVERY human woman the antlers |

- **Targeted bake** (Yvanni horns): write ONLY head children 15..22; NEVER touch 0..14 — the `capHead`
  C1..C14 knob values hold stale ARGONIAN skull data that would wreck her face. `bake_yvanni_horns.py`
  verifies skull-intact. After the bake, reset the 8 head knobs to seed + Enable 0 so she shows her
  BAKED horns (and they're re-buried for everyone else).
- **npcCap finalize** (Yvanni hooves/breasts, Auri antlers): transcribe the dialed values to
  `npcCap <base>:<Plugin.esp> <slot> <child> …` lines; RESET the global knobs those came from back to the
  human base (else every other woman keeps the dialed legs/breasts); comment the sculpt gate OFF. npcCap
  WINS over the global knob AND preserves ReShape riding on top. `capMirrorL` mirrors npcCap LIMB writes to
  the left leg (`wroteChild` is set for npcCap too — CapFix.cpp:1520), so only the RIGHT calf/foot lines
  are needed; centerline slots (head/spine2) need both sides written.
- npcCap is kDataLoaded-parsed → the finalize is GAME-CLOSED work and needs a RESTART to verify.
- **As-shipped 2026-07-20:** Yvanni = horns baked into `skeleton_female_draenei.nif` + 10 npcCap lines
  (calf main+C1..4, foot main+C1..2, breasts C11/C12); Auri = 8 npcCap head lines (antlers). Config =
  3 race maps + 18 npcCap + 0 active sculpt (normal play). DLL 9e7a8cfb.

## 10. ★ TWO npcCap BUGS — surfaced on the FIRST REAL per-NPC use (2026-07-20) — also in the Ledger

npcCap had only ever *parsed* before; Yvanni/Auri were the first overrides to actually drive an NPC, and
both bugs had been latent since it was written:
1. **Keyed on the BASE FormID, looked up by the REFERENCE FormID → silent no-op.** The store resolves
   `LookupForm(local, plugin)` = the base NPC_ FormID (Yvanni FE61C800). CapFix looked it up with
   `actor->GetFormID()` = the REFERENCE / placed-instance FormID (FF00xxxx, a placeatme clone) — which
   NEVER equals the base → miss, every actor. The SKELMAP receipt logged "10 npcCap override(s)" (parse OK),
   which HID the miss. **FIX:** look up by `actor->GetActorBase()->GetFormID()` (CapFix.cpp — `npcBaseId`
   computed once before the child loop). The classic base-vs-reference confusion; **parse-success ≠
   apply-success.**
2. **The `FormID:Plugin` token breaks on a SPACED plugin name.** `"Yvanni Follower.esp"` split at the
   space under `>>`. **FIX:** read tokens until the ref ends in `.esp/.esm/.esl` (main.cpp ~589) — the
   same fix the `race` parser already carried; the npcCap parser predated it and repeated the mistake.

## 11. Follower dossiers completed 2026-07-20 (join §5)

- **YVANNI** (Draenei follower, `000800:Yvanni Follower.esp`, base FE61C800 — an ESPFE/placeatme-class
  actor). Own `skeleton_female_draenei.nif` (byte-copy of ours + the 8 head seeds). Horns BAKED into it;
  hooves (calf/foot) + enlarged breasts (spine2 C11/C12) via npcCap; tail = Mal_Tail (table 6, trimmed to
  bone 7 — Report 07). Her own body fingerprint (vc 12888) + neutral row fixed the +40% spine0 (Ledger 11).
- **ULLISS** (Unslaad half-dragon, `zzzCrbHalfDragonRace` → `skeleton_female.nif`; base `zzzCrbCrossbreed`,
  display Name "Uliizkaan" — why `help ulliss` returns nothing). Her custom skeleton is a STRICT SUBSET of
  ours (verify_swap_safety.py), so the swap is a visual no-op that gains her the full PPB body (Austella +
  Hoarfrost Witch too). Her tail is an EQUIPPABLE mesh, bone-animated (idle sway, NOT SMP) → it can be
  touched but not pushed; SMP-conversion is OUT OF SCOPE for PPB (user call). tbl-5 grip rig REMOVED.
