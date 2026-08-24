// PpbHairTables.inc — GENERATED hair chord tables, #included inside NpcFingerTest.cpp's
// anonymous namespace right after kWigPairs (so FingerPair is in scope).
//
// Full per-segment coverage of the user's two custom-follower hairs: every skinned SMP bone
// gets a capsule, chords span ADJACENT skinned bones (endpoints ON the nodes). Produced offline
// by tools/ppb-scratch/gen_tables.py from each hair's NIF (bone world-positions + skin weights).
// Rows 0..13 = the push SENSORS (mid-strand on the LONGEST strands — spread + whip-safe, never
// root-bunched). Safe to be this dense: hair strands are SHORT + INDEPENDENT, so there is no
// long single-chain servo-lag whip (the 19-chord tail regression, Report 15), and rig capsules
// never self-collide (Report 08). Bone names are PLAIN — the equipped-hair AutoRename prefix is
// resolved by FindBoneSuffix's ends-with match. Regenerate: python gen_tables.py.
//   Jeeta     = HG_wing1111 (Xing HG wig) — 52 chords
//   DreadGirl = dH02_11 (DINT999 dreadlocks, incl. the b2opt/b3pot layers = real distinct
//               dreads, not ghosts) — 164 chords
constexpr FingerPair kJeetaPairs[52] = {
  { "HG_wing1111_B2 5", "HG_wing1111_B2 6", "B2.5" },  // sensor
  { "HG_wing1111_B1 5", "HG_wing1111_B1 6", "B1.5" },  // sensor
  { "HG_wing1111_F01 3", "HG_wing1111_F01 4", "F01.3" },  // sensor
  { "HG_wing1111_F04 3", "HG_wing1111_F04 4", "F04.3" },  // sensor
  { "HG_wing1111_F10 3", "HG_wing1111_F10 4", "F10.3" },  // sensor
  { "HG_wing1111_F12 3", "HG_wing1111_F12 4", "F12.3" },  // sensor
  { "HG_wing1111_F11 3", "HG_wing1111_F11 4", "F11.3" },  // sensor
  { "HG_wing1111_F09 3", "HG_wing1111_F09 4", "F09.3" },  // sensor
  { "HG_wing1111_F02 2", "HG_wing1111_F02 3", "F02.2" },  // sensor
  { "HG_wing1111_F03 2", "HG_wing1111_F03 3", "F03.2" },  // sensor
  { "HG_wing1111_F05 2", "HG_wing1111_F05 3", "F05.2" },  // sensor
  { "HG_wing1111_F08 2", "HG_wing1111_F08 3", "F08.2" },  // sensor
  { "HG_wing1111_F07 2", "HG_wing1111_F07 3", "F07.2" },  // sensor
  { "HG_wing1111_F06 2", "HG_wing1111_F06 3", "F06.2" },  // sensor
  { "HG_wing1111_B2 1", "HG_wing1111_B2 2", "B2.1" },
  { "HG_wing1111_B2 2", "HG_wing1111_B2 3", "B2.2" },
  { "HG_wing1111_B2 3", "HG_wing1111_B2 4", "B2.3" },
  { "HG_wing1111_B2 4", "HG_wing1111_B2 5", "B2.4" },
  { "HG_wing1111_B2 6", "HG_wing1111_B2 7", "B2.6" },
  { "HG_wing1111_B2 7", "HG_wing1111_B2 8", "B2.7" },
  { "HG_wing1111_B2 8", "HG_wing1111_B2 9", "B2.8" },
  { "HG_wing1111_B1 1", "HG_wing1111_B1 2", "B1.1" },
  { "HG_wing1111_B1 2", "HG_wing1111_B1 3", "B1.2" },
  { "HG_wing1111_B1 3", "HG_wing1111_B1 4", "B1.3" },
  { "HG_wing1111_B1 4", "HG_wing1111_B1 5", "B1.4" },
  { "HG_wing1111_B1 6", "HG_wing1111_B1 7", "B1.6" },
  { "HG_wing1111_B1 7", "HG_wing1111_B1 8", "B1.7" },
  { "HG_wing1111_B1 8", "HG_wing1111_B1 9", "B1.8" },
  { "HG_wing1111_F01 1", "HG_wing1111_F01 2", "F01.1" },
  { "HG_wing1111_F01 2", "HG_wing1111_F01 3", "F01.2" },
  { "HG_wing1111_F02 1", "HG_wing1111_F02 2", "F02.1" },
  { "HG_wing1111_F03 1", "HG_wing1111_F03 2", "F03.1" },
  { "HG_wing1111_F04 1", "HG_wing1111_F04 2", "F04.1" },
  { "HG_wing1111_F04 2", "HG_wing1111_F04 3", "F04.2" },
  { "HG_wing1111_F05 1", "HG_wing1111_F05 2", "F05.1" },
  { "HG_wing1111_F06 1", "HG_wing1111_F06 2", "F06.1" },
  { "HG_wing1111_F07 1", "HG_wing1111_F07 2", "F07.1" },
  { "HG_wing1111_F08 1", "HG_wing1111_F08 2", "F08.1" },
  { "HG_wing1111_F09 1", "HG_wing1111_F09 2", "F09.1" },
  { "HG_wing1111_F09 2", "HG_wing1111_F09 3", "F09.2" },
  { "HG_wing1111_F10 1", "HG_wing1111_F10 2", "F10.1" },
  { "HG_wing1111_F10 2", "HG_wing1111_F10 3", "F10.2" },
  { "HG_wing1111_F11 1", "HG_wing1111_F11 2", "F11.1" },
  { "HG_wing1111_F11 2", "HG_wing1111_F11 3", "F11.2" },
  { "HG_wing1111_F12 1", "HG_wing1111_F12 2", "F12.1" },
  { "HG_wing1111_F12 2", "HG_wing1111_F12 3", "F12.2" },
  { "HG_wing1111_F13 1", "HG_wing1111_F13 2", "F13.1" },
  { "HG_wing1111_F13 2", "HG_wing1111_F13 3", "F13.2" },
  { "HG_wing1111_F14 1", "HG_wing1111_F14 2", "F14.1" },
  { "HG_wing1111_F14 2", "HG_wing1111_F14 3", "F14.2" },
  { "HG_wing1111_F15 1", "HG_wing1111_F15 2", "F15.1" },
  { "HG_wing1111_F16 1", "HG_wing1111_F16 2", "F16.1" },
};

constexpr FingerPair kDreadPairs[164] = {
  { "dH02_11_b3pot_01 5", "dH02_11_b3pot_01 6", "b3pot_01.5" },  // sensor
  { "dH02_11_b3pot_04 4", "dH02_11_b3pot_04 5", "b3pot_04.4" },  // sensor
  { "dH02_11_b3_02 4", "dH02_11_b3_02 5", "b3_02.4" },  // sensor
  { "dH02_11_b3_03 4", "dH02_11_b3_03 5", "b3_03.4" },  // sensor
  { "dH02_11_b3_07 4", "dH02_11_b3_07 5", "b3_07.4" },  // sensor
  { "dH02_11_b3_05 4", "dH02_11_b3_05 5", "b3_05.4" },  // sensor
  { "dH02_11_b3_06 4", "dH02_11_b3_06 5", "b3_06.4" },  // sensor
  { "dH02_11_b3_04 4", "dH02_11_b3_04 5", "b3_04.4" },  // sensor
  { "dH02_11_b3pot_02 4", "dH02_11_b3pot_02 5", "b3pot_02.4" },  // sensor
  { "dH02_11_b3pot_09 4", "dH02_11_b3pot_09 5", "b3pot_09.4" },  // sensor
  { "dH02_11_b3pot_06 4", "dH02_11_b3pot_06 5", "b3pot_06.4" },  // sensor
  { "dH02_11_b3_09 4", "dH02_11_b3_09 5", "b3_09.4" },  // sensor
  { "dH02_11_b3_01 4", "dH02_11_b3_01 5", "b3_01.4" },  // sensor
  { "dH02_11_b3_08 4", "dH02_11_b3_08 5", "b3_08.4" },  // sensor
  { "dH02_11_b2opt_01 1", "dH02_11_b2opt_01 2", "b2opt_01.1" },
  { "dH02_11_b2opt_01 2", "dH02_11_b2opt_01 3", "b2opt_01.2" },
  { "dH02_11_b2opt_01 3", "dH02_11_b2opt_01 4", "b2opt_01.3" },
  { "dH02_11_b2opt_01 4", "dH02_11_b2opt_01 5", "b2opt_01.4" },
  { "dH02_11_b2opt_01 5", "dH02_11_b2opt_01 6", "b2opt_01.5" },
  { "dH02_11_b2opt_03 1", "dH02_11_b2opt_03 2", "b2opt_03.1" },
  { "dH02_11_b2opt_03 2", "dH02_11_b2opt_03 3", "b2opt_03.2" },
  { "dH02_11_b2opt_03 3", "dH02_11_b2opt_03 4", "b2opt_03.3" },
  { "dH02_11_b2opt_03 4", "dH02_11_b2opt_03 5", "b2opt_03.4" },
  { "dH02_11_b2opt_04 1", "dH02_11_b2opt_04 2", "b2opt_04.1" },
  { "dH02_11_b2opt_04 2", "dH02_11_b2opt_04 3", "b2opt_04.2" },
  { "dH02_11_b2opt_04 3", "dH02_11_b2opt_04 4", "b2opt_04.3" },
  { "dH02_11_b2opt_04 4", "dH02_11_b2opt_04 5", "b2opt_04.4" },
  { "dH02_11_b2opt_05 1", "dH02_11_b2opt_05 2", "b2opt_05.1" },
  { "dH02_11_b2opt_05 2", "dH02_11_b2opt_05 3", "b2opt_05.2" },
  { "dH02_11_b2opt_05 3", "dH02_11_b2opt_05 4", "b2opt_05.3" },
  { "dH02_11_b2opt_07 1", "dH02_11_b2opt_07 2", "b2opt_07.1" },
  { "dH02_11_b2opt_07 2", "dH02_11_b2opt_07 3", "b2opt_07.2" },
  { "dH02_11_b2opt_07 3", "dH02_11_b2opt_07 4", "b2opt_07.3" },
  { "dH02_11_b2opt_07 4", "dH02_11_b2opt_07 5", "b2opt_07.4" },
  { "dH02_11_b2opt_08 1", "dH02_11_b2opt_08 2", "b2opt_08.1" },
  { "dH02_11_b2opt_08 2", "dH02_11_b2opt_08 3", "b2opt_08.2" },
  { "dH02_11_b2opt_08 3", "dH02_11_b2opt_08 4", "b2opt_08.3" },
  { "dH02_11_b2opt_09 1", "dH02_11_b2opt_09 2", "b2opt_09.1" },
  { "dH02_11_b2opt_09 2", "dH02_11_b2opt_09 3", "b2opt_09.2" },
  { "dH02_11_b2opt_09 3", "dH02_11_b2opt_09 4", "b2opt_09.3" },
  { "dH02_11_b2opt_09 4", "dH02_11_b2opt_09 5", "b2opt_09.4" },
  { "dH02_11_b2opt_09 5", "dH02_11_b2opt_09 6", "b2opt_09.5" },
  { "dH02_11_b3_01 1", "dH02_11_b3_01 2", "b3_01.1" },
  { "dH02_11_b3_01 2", "dH02_11_b3_01 3", "b3_01.2" },
  { "dH02_11_b3_01 3", "dH02_11_b3_01 4", "b3_01.3" },
  { "dH02_11_b3_01 5", "dH02_11_b3_01 6", "b3_01.5" },
  { "dH02_11_b3_02 1", "dH02_11_b3_02 2", "b3_02.1" },
  { "dH02_11_b3_02 2", "dH02_11_b3_02 3", "b3_02.2" },
  { "dH02_11_b3_02 3", "dH02_11_b3_02 4", "b3_02.3" },
  { "dH02_11_b3_02 5", "dH02_11_b3_02 6", "b3_02.5" },
  { "dH02_11_b3_02 6", "dH02_11_b3_02 7", "b3_02.6" },
  { "dH02_11_b3_03 1", "dH02_11_b3_03 2", "b3_03.1" },
  { "dH02_11_b3_03 2", "dH02_11_b3_03 3", "b3_03.2" },
  { "dH02_11_b3_03 3", "dH02_11_b3_03 4", "b3_03.3" },
  { "dH02_11_b3_03 5", "dH02_11_b3_03 6", "b3_03.5" },
  { "dH02_11_b3_03 6", "dH02_11_b3_03 7", "b3_03.6" },
  { "dH02_11_b3_04 1", "dH02_11_b3_04 2", "b3_04.1" },
  { "dH02_11_b3_04 2", "dH02_11_b3_04 3", "b3_04.2" },
  { "dH02_11_b3_04 3", "dH02_11_b3_04 4", "b3_04.3" },
  { "dH02_11_b3_04 5", "dH02_11_b3_04 6", "b3_04.5" },
  { "dH02_11_b3_04 6", "dH02_11_b3_04 7", "b3_04.6" },
  { "dH02_11_b3_05 1", "dH02_11_b3_05 2", "b3_05.1" },
  { "dH02_11_b3_05 2", "dH02_11_b3_05 3", "b3_05.2" },
  { "dH02_11_b3_05 3", "dH02_11_b3_05 4", "b3_05.3" },
  { "dH02_11_b3_05 5", "dH02_11_b3_05 6", "b3_05.5" },
  { "dH02_11_b3_05 6", "dH02_11_b3_05 7", "b3_05.6" },
  { "dH02_11_b3_06 1", "dH02_11_b3_06 2", "b3_06.1" },
  { "dH02_11_b3_06 2", "dH02_11_b3_06 3", "b3_06.2" },
  { "dH02_11_b3_06 3", "dH02_11_b3_06 4", "b3_06.3" },
  { "dH02_11_b3_06 5", "dH02_11_b3_06 6", "b3_06.5" },
  { "dH02_11_b3_06 6", "dH02_11_b3_06 7", "b3_06.6" },
  { "dH02_11_b3_07 1", "dH02_11_b3_07 2", "b3_07.1" },
  { "dH02_11_b3_07 2", "dH02_11_b3_07 3", "b3_07.2" },
  { "dH02_11_b3_07 3", "dH02_11_b3_07 4", "b3_07.3" },
  { "dH02_11_b3_07 5", "dH02_11_b3_07 6", "b3_07.5" },
  { "dH02_11_b3_07 6", "dH02_11_b3_07 7", "b3_07.6" },
  { "dH02_11_b3_08 1", "dH02_11_b3_08 2", "b3_08.1" },
  { "dH02_11_b3_08 2", "dH02_11_b3_08 3", "b3_08.2" },
  { "dH02_11_b3_08 3", "dH02_11_b3_08 4", "b3_08.3" },
  { "dH02_11_b3_08 5", "dH02_11_b3_08 6", "b3_08.5" },
  { "dH02_11_b3_09 1", "dH02_11_b3_09 2", "b3_09.1" },
  { "dH02_11_b3_09 2", "dH02_11_b3_09 3", "b3_09.2" },
  { "dH02_11_b3_09 3", "dH02_11_b3_09 4", "b3_09.3" },
  { "dH02_11_b3_09 5", "dH02_11_b3_09 6", "b3_09.5" },
  { "dH02_11_b3pot_01 1", "dH02_11_b3pot_01 2", "b3pot_01.1" },
  { "dH02_11_b3pot_01 2", "dH02_11_b3pot_01 3", "b3pot_01.2" },
  { "dH02_11_b3pot_01 3", "dH02_11_b3pot_01 4", "b3pot_01.3" },
  { "dH02_11_b3pot_01 4", "dH02_11_b3pot_01 5", "b3pot_01.4" },
  { "dH02_11_b3pot_01 6", "dH02_11_b3pot_01 7", "b3pot_01.6" },
  { "dH02_11_b3pot_01 7", "dH02_11_b3pot_01 8", "b3pot_01.7" },
  { "dH02_11_b3pot_02 1", "dH02_11_b3pot_02 2", "b3pot_02.1" },
  { "dH02_11_b3pot_02 2", "dH02_11_b3pot_02 3", "b3pot_02.2" },
  { "dH02_11_b3pot_02 3", "dH02_11_b3pot_02 4", "b3pot_02.3" },
  { "dH02_11_b3pot_02 5", "dH02_11_b3pot_02 6", "b3pot_02.5" },
  { "dH02_11_b3pot_02 6", "dH02_11_b3pot_02 7", "b3pot_02.6" },
  { "dH02_11_b3pot_03 1", "dH02_11_b3pot_03 2", "b3pot_03.1" },
  { "dH02_11_b3pot_03 2", "dH02_11_b3pot_03 3", "b3pot_03.2" },
  { "dH02_11_b3pot_03 3", "dH02_11_b3pot_03 4", "b3pot_03.3" },
  { "dH02_11_b3pot_03 4", "dH02_11_b3pot_03 5", "b3pot_03.4" },
  { "dH02_11_b3pot_03 5", "dH02_11_b3pot_03 6", "b3pot_03.5" },
  { "dH02_11_b3pot_04 1", "dH02_11_b3pot_04 2", "b3pot_04.1" },
  { "dH02_11_b3pot_04 2", "dH02_11_b3pot_04 3", "b3pot_04.2" },
  { "dH02_11_b3pot_04 3", "dH02_11_b3pot_04 4", "b3pot_04.3" },
  { "dH02_11_b3pot_04 5", "dH02_11_b3pot_04 6", "b3pot_04.5" },
  { "dH02_11_b3pot_04 6", "dH02_11_b3pot_04 7", "b3pot_04.6" },
  { "dH02_11_b3pot_05 1", "dH02_11_b3pot_05 2", "b3pot_05.1" },
  { "dH02_11_b3pot_05 2", "dH02_11_b3pot_05 3", "b3pot_05.2" },
  { "dH02_11_b3pot_05 3", "dH02_11_b3pot_05 4", "b3pot_05.3" },
  { "dH02_11_b3pot_05 4", "dH02_11_b3pot_05 5", "b3pot_05.4" },
  { "dH02_11_b3pot_05 5", "dH02_11_b3pot_05 6", "b3pot_05.5" },
  { "dH02_11_b3pot_06 1", "dH02_11_b3pot_06 2", "b3pot_06.1" },
  { "dH02_11_b3pot_06 2", "dH02_11_b3pot_06 3", "b3pot_06.2" },
  { "dH02_11_b3pot_06 3", "dH02_11_b3pot_06 4", "b3pot_06.3" },
  { "dH02_11_b3pot_06 5", "dH02_11_b3pot_06 6", "b3pot_06.5" },
  { "dH02_11_b3pot_06 6", "dH02_11_b3pot_06 7", "b3pot_06.6" },
  { "dH02_11_b3pot_07 1", "dH02_11_b3pot_07 2", "b3pot_07.1" },
  { "dH02_11_b3pot_07 2", "dH02_11_b3pot_07 3", "b3pot_07.2" },
  { "dH02_11_b3pot_07 3", "dH02_11_b3pot_07 4", "b3pot_07.3" },
  { "dH02_11_b3pot_07 4", "dH02_11_b3pot_07 5", "b3pot_07.4" },
  { "dH02_11_b3pot_07 5", "dH02_11_b3pot_07 6", "b3pot_07.5" },
  { "dH02_11_b3pot_08 1", "dH02_11_b3pot_08 2", "b3pot_08.1" },
  { "dH02_11_b3pot_08 2", "dH02_11_b3pot_08 3", "b3pot_08.2" },
  { "dH02_11_b3pot_08 3", "dH02_11_b3pot_08 4", "b3pot_08.3" },
  { "dH02_11_b3pot_08 4", "dH02_11_b3pot_08 5", "b3pot_08.4" },
  { "dH02_11_b3pot_08 5", "dH02_11_b3pot_08 6", "b3pot_08.5" },
  { "dH02_11_b3pot_09 1", "dH02_11_b3pot_09 2", "b3pot_09.1" },
  { "dH02_11_b3pot_09 2", "dH02_11_b3pot_09 3", "b3pot_09.2" },
  { "dH02_11_b3pot_09 3", "dH02_11_b3pot_09 4", "b3pot_09.3" },
  { "dH02_11_b3pot_09 5", "dH02_11_b3pot_09 6", "b3pot_09.5" },
  { "dH02_11_b3pot_09 6", "dH02_11_b3pot_09 7", "b3pot_09.6" },
  { "dH02_11_b2_03 1", "dH02_11_b2_03 2", "b2_03.1" },
  { "dH02_11_b2_03 2", "dH02_11_b2_03 3", "b2_03.2" },
  { "dH02_11_b2_03 3", "dH02_11_b2_03 4", "b2_03.3" },
  { "dH02_11_b2_03 4", "dH02_11_b2_03 5", "b2_03.4" },
  { "dH02_11_b2_03 5", "dH02_11_b2_03 6", "b2_03.5" },
  { "dH02_11_b2_03 6", "dH02_11_b2_03 7", "b2_03.6" },
  { "dH02_11_b2_02 1", "dH02_11_b2_02 2", "b2_02.1" },
  { "dH02_11_b2_02 2", "dH02_11_b2_02 3", "b2_02.2" },
  { "dH02_11_b2_02 3", "dH02_11_b2_02 4", "b2_02.3" },
  { "dH02_11_b2_02 4", "dH02_11_b2_02 5", "b2_02.4" },
  { "dH02_11_b2_09 1", "dH02_11_b2_09 2", "b2_09.1" },
  { "dH02_11_b2_09 2", "dH02_11_b2_09 3", "b2_09.2" },
  { "dH02_11_b2_09 3", "dH02_11_b2_09 4", "b2_09.3" },
  { "dH02_11_b2_06 1", "dH02_11_b2_06 2", "b2_06.1" },
  { "dH02_11_b2_06 2", "dH02_11_b2_06 3", "b2_06.2" },
  { "dH02_11_b2_06 3", "dH02_11_b2_06 4", "b2_06.3" },
  { "dH02_11_b2_07 1", "dH02_11_b2_07 2", "b2_07.1" },
  { "dH02_11_b2_07 2", "dH02_11_b2_07 3", "b2_07.2" },
  { "dH02_11_b2_07 3", "dH02_11_b2_07 4", "b2_07.3" },
  { "dH02_11_b2_08 1", "dH02_11_b2_08 2", "b2_08.1" },
  { "dH02_11_b2_08 2", "dH02_11_b2_08 3", "b2_08.2" },
  { "dH02_11_b2_08 3", "dH02_11_b2_08 4", "b2_08.3" },
  { "dH02_11_b2_05 1", "dH02_11_b2_05 2", "b2_05.1" },
  { "dH02_11_b2_05 2", "dH02_11_b2_05 3", "b2_05.2" },
  { "dH02_11_b2_05 3", "dH02_11_b2_05 4", "b2_05.3" },
  { "dH02_11_b2_01 1", "dH02_11_b2_01 2", "b2_01.1" },
  { "dH02_11_b2_01 2", "dH02_11_b2_01 3", "b2_01.2" },
  { "dH02_11_b2_01 3", "dH02_11_b2_01 4", "b2_01.3" },
  { "dH02_11_b2_01 4", "dH02_11_b2_01 5", "b2_01.4" },
  { "dH02_11_b2_01 5", "dH02_11_b2_01 6", "b2_01.5" },
  { "dH02_11_b2_01 6", "dH02_11_b2_01 7", "b2_01.6" },
  { "dH02_11_b2_04 1", "dH02_11_b2_04 2", "b2_04.1" },
  { "dH02_11_b2_04 2", "dH02_11_b2_04 3", "b2_04.2" },
  { "dH02_11_b2_04 3", "dH02_11_b2_04 4", "b2_04.3" },
};
