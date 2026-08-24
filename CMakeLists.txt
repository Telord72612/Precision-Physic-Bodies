cmake_minimum_required(VERSION 3.21)

project(PPB LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# PPB — Precision Physic Bodies (2026-07-07), split out of AIHands.
# The whole-population body-calibration stack: heel fix (0xA26C05 + 0xB268DC),
# CapFix capsule editor, PivFix joint-pivot re-seat + clavicle-follow, statue
# mode, and its own 0xB266AB pre-drive chain hook. AIHands keeps the arm IK /
# grab / give systems; this DLL owns everything that runs for EVERY driven NPC.
# ---------------------------------------------------------------------------

# CommonLibSSE-NG (VR-enabled) as a local subdirectory.
set(COMMONLIB_PATH "D:/Games/My Skyrim/Tools/CommonLibVR-4.14.0" CACHE PATH "Path to CommonLibSSE-NG source")

# VR-only runtime (smaller; this plugin is VR-exclusive).
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON  CACHE BOOL "" FORCE)
set(BUILD_TESTS      OFF CACHE BOOL "" FORCE)

add_subdirectory("${COMMONLIB_PATH}" CommonLibSSE EXCLUDE_FROM_ALL)

# --- The plugin DLL ---
add_library(PPB SHARED
    src/main.cpp        # SKSE entry, logging, console hf/capfix, install hooks at kDataLoaded
    src/Hooks.cpp       # 0xB266AB pre-drive chain + 0xA26C05 inner-drive + 0xB268DC post-physics
    src/PPBHook.cpp     # per-actor pipeline: heel fix + CapFix/PivFix drive + statue + bind-rel math
    src/CapFix.cpp      # 12-slot live capsule editor (RE:: types only)
    src/PivFix.cpp      # joint-pivot re-seat + clavicle-follow (Havok SDK, scoped below)
    src/Tuning.cpp      # PPB_tuning.txt: tune struct, parser, 1 Hz poll, heel-fix flag
    src/Natives.cpp     # PPB_Native.SetStatuePose / SetHeelFix / ToggleHeelFix
    src/Interop.cpp     # HIGGS interface handshake + IsActorGrabbedByPlayer (PivFix grab gate)
    src/PpbApi.cpp      # public touch API: contact engine + messaging interface + PPB_Touch natives
    src/DismemberGuard.cpp # 2026-07-26 DF/NGD ↔ PLANCK guard: death-grace ignore + head-clone exclusion
    src/GenitalProbe.cpp   # 2026-08-02 research instrument: which signal tracks schlong exposure (genProbe)
    src/Orifice.cpp        # 2026-08-19 native orifice drive: penetration gate -> eased bone-ring write

    src/Probe.cpp       # `probe` console command: read-only actor physics/animation state dump
    src/Diag.cpp        # 2026-07-08 wave-2 gate: read-only contact counter + step timer + census + capdis spike
    src/PerfSys.cpp     # 2026-07-09 perf system: HIGGS filter callback (thigh-x-thigh) + child-LOD 50/80/90 presets
    src/RayTel.cpp      # 2026-08-22 raycast/query telemetry: 5 bhkCollisionFilter vtable slots -> measured per-child walks
    src/NpcFingerTest.cpp
    src/FsmpLink.cpp  # 2026-07-09 NPC finger test: 4 runtime DYNAMIC capsules, `nfing` (Havok SDK, scoped below)
    src/HandBox.cpp     # 2026-07-09 player-hand 4-box collider + HIGGS slab dial (Havok SDK, scoped below)
)

target_precompile_headers(PPB PRIVATE src/PCH.h)

# PivFix.cpp, NpcFingerTest.cpp and HandBox.cpp are the TUs that include the
# Havok 2010.2 SDK (global-namespace hkp* types). Scope the SDK include path to
# them ONLY. The PCH stays ACTIVE (it provides std + RE::); the SDK globals
# coexist with RE::hkp* because the latter are namespaced and our SDK uses are
# unqualified. Exactly the AIHands PalmCollider.cpp proven setup.
# NOTE: the namespace-scope hk link stubs (hkReferencedObject::addReference etc.)
# are defined in PivFix.cpp ONLY — the other SDK TUs must never repeat them
# (duplicate-symbol link error; FINGER_BUILD_PLAN hard rule).
set(PPB_HAVOK_SDK_SOURCE "F:/CHIM Emotion/Havok/Source")
set_source_files_properties(src/PivFix.cpp src/NpcFingerTest.cpp
    src/HandBox.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${PPB_HAVOK_SDK_SOURCE}"
)

# Bullet 3.24 headers (vendored closure, extern/bullet324) — scoped to the SMP
# link TU only. Same per-TU isolation discipline as the Havok SDK above; the
# two SDKs never meet in one TU.
set_source_files_properties(src/FsmpLink.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/extern/bullet324/src"
)

target_link_libraries(PPB PRIVATE CommonLibSSE::CommonLibSSE)

# Always produce a PDB, even in Release, WITHOUT changing codegen/layout:
# /Zi adds debug info only; /DEBUG defaults the linker to /OPT:NOREF,NOICF which WOULD move
# functions, so force REF+ICF back to match a plain Release layout (crash-log RVAs stay valid).
target_compile_options(PPB PRIVATE "$<$<CONFIG:Release>:/Zi>")
target_link_options(PPB PRIVATE "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")


set_target_properties(PPB PROPERTIES
    OUTPUT_NAME "PPB"
    DEBUG_POSTFIX ""
)

# Copy the built DLL into the Precision Physic Bodies mod's SKSE/Plugins folder.
set(MOD_OUTPUT "D:/Games/My Skyrim/mods/Precision Physic Bodies/SKSE/Plugins")
add_custom_command(TARGET PPB POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MOD_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:PPB>" "${MOD_OUTPUT}/"
    COMMENT "Copying PPB.dll to mod folder"
)
