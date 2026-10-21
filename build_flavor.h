#pragma once
// One binary. Normal mode by default; developer mode is selected at startup from [ui].
// Atomic because D3D callbacks can run while the initialization thread loads settings.
#include <atomic>
#define S3SS_TR(pt, en) en
inline std::atomic<bool> kPublicBuild{true}; // legacy name: true means developer mode is off

// Two flavours (since 2.7.0): the players' build (ApexFlavorDefines=APEX_NO_DEV_TOOLS, PublicApexRadiance.asi) leaves
// out the developer tools and never turns developer mode on; the developer build (ReleaseApexRadiance.asi) has them all.
#ifdef APEX_NO_DEV_TOOLS
inline constexpr bool kDevToolsBuild = false;
#else
inline constexpr bool kDevToolsBuild = true;
#endif

// Light through doors and windows (LevelLightShare::SetRealisticOpenings): on by default (the realistic
// result was preferred in testing); the setting can still turn it off
inline constexpr bool kRealisticOpeningsDefault = true;

// Bounded memory and geometry policies ship independently of test notices and developer collectors.
// It also gates lighting paths, so turning it off changes the light: the inner-room share scale (level_light_share
// BasisLightHook, InnerNormScaleAt), the mesh occlusion test of indoor lamps and its publish and resets (InnerPass,
// IndoorPassImpl, OnPresent), the ghost solve (IndoorPassImpl), the solid mesh collection that feeds that test
// (lot_light_bridge NoteSolidMeshImpl, PumpSolidMeshes) and the smooth room map on packed room floors (OnDrawInnerCore).
inline constexpr bool kMemoryOptimizationsEnabled = true;
// Numbered test version and first-run test notice only.
#ifdef APEX_MEMORY_TEST
inline constexpr bool kMemoryOptimizationTest = true;
#else
inline constexpr bool kMemoryOptimizationTest = false;
#endif
