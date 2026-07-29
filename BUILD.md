# Building PPB

PPB is a single SKSE VR plugin (`PPB.dll`). Nothing else needs compiling.

## Requirements

| | |
|---|---|
| Visual Studio 2022+ | with the C++ desktop workload |
| CMake 3.21+ | the copy bundled with Visual Studio is fine |
| **CommonLibSSE-NG (VR-enabled)** | source checkout — not vendored here |
| **Havok 2010.2 SDK** | ⚠ proprietary, not included, not redistributable |

### Why the Havok SDK

Most VR plugins re-declare the handful of Havok structs they touch. PPB uses the **real SDK headers**,
because it manipulates capsule shapes, list shapes, constraints and motion state directly, and getting
those layouts wrong is a crash rather than a compile error.

The SDK include path is scoped to **only the translation units that need it** (`PivFix.cpp`,
`FsmpLink.cpp`, `HandBox.cpp`, `NpcFingerTest.cpp`), so the rest of the codebase builds against
CommonLib alone. If you cannot obtain the SDK you can still read and modify everything else, but the
project will not link.

## Configure

Both paths are CMake cache variables with no default — set them for your machine:

```bash
cmake --preset vr ^
  -DCOMMONLIB_PATH="C:/path/to/CommonLibVR" ^
  -DPPB_HAVOK_SDK_SOURCE="C:/path/to/Havok/Source"
```

Optionally have the build drop the DLL straight into your mod folder:

```bash
  -DMOD_OUTPUT="C:/path/to/MO2/mods/Precision Physic Bodies/SKSE/Plugins"
```

## Build

```bash
cmake --build --preset vr
```

Release builds produce a PDB alongside the DLL. That is deliberate: `/Zi` adds debug info while
`/OPT:REF /OPT:ICF` are forced back on so the binary layout matches a plain Release — crash-log
addresses then resolve to real function names instead of hex.

> **Build with the game closed.** Windows locks a loaded DLL, and the copy step will fail with
> "Permission denied" while Skyrim is running.

## Layout

```
src/        the plugin
data/       shipped config files (tuning, skeleton map, race map)
fomod/      installer definition
docs/       engineering reference — read docs/01 (pitfalls) before changing physics code
```

## Working on it

`PPB_tuning.txt` is polled roughly once a second at runtime, so nearly every value can be dialled
live without restarting the game. `PPB_skeletons.txt` is parsed once at load and needs a restart.

The engineering docs in `docs/` are the real map. `docs/01_Pitfall_Ledger.md` in particular is a list
of expensive lessons — several of them are crash classes that look like innocent code.
