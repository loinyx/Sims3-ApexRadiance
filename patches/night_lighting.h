#pragma once
// Night Lighting (NightTerrainRelight): what the menu may read and draw of it. The settings, their TOML keys, defaults
// and live install/uninstall rules stay in patches/night_terrain_relight_patch.cpp; these functions only draw them.
// All of them: render thread, inside the menu's ImGui frame. The Draw*Card functions open and close their own card
// (ApexUi::BeginCard / EndCard, one PushID each) and do nothing when the feature does not exist.
#include <string>

namespace NightLighting {

// The game's night level as Night Lighting read it at the last Present (lightMgr+0xF0: 0 = day, 1 = night; the feature
// treats > 0.99 as night). False while no world is loaded or the feature is off. Render thread (the menu).
bool MenuNightLevel(float& level);

// Every-Story Ground Light (SplitLevelGroundLight) is on only because official Sims3SettingsSetter's own fix already
// does the same (Apex wrote nothing). Implemented in patches/split_level_ground_light_patch.cpp.
bool SplitLevelProvidedByS3SS();

// The "Refresh the lighting" shortcut (Hotkeys): the terrain, every lot and every room light again, the object rigs gather
// again (what turning Night Lighting off and on did). Nothing while Night Lighting is off. Render thread.
void RefreshAll(const char* why = "shortcut", bool terrain = true); // terrain false: lots, rooms and rigs only (after a load)
// The same once the changes rest (1 s after the last call): any lighting setting changed (menu, profile, undo, reset, the
// upper floors switch). Nothing while Night Lighting is off or no world is loaded when it runs. Render thread.
void RefreshSoon();
// Queue a deep lighting restart at the next frame boundary. Does not change settings.
void Restart();
// The world is on screen after a load (false during load screens; only kept while Night Lighting runs). Render thread.
bool WorldLive();
// The load is over (the after-load refresh ran, or 10 s after WorldLive): false during the game's interactive loading
// screen, which already draws terrain. Gates the start note, the menu and Depth Blur. Render thread.
bool LoadSettled();

// ---- menu: Lighting page (tabs Lamps / Ground / Objects / Buildings / Stories) and Water & Snow page (tabs Water / Snow) ----
// Overview tab: three balanced intensity choices, custom status and explicit choice undo.
void DrawLightingBalance();
void DrawRefreshCard();
// Ground tab
void DrawGroundCard();
// Stories tab: lamp light between the floors of a house. drawUpperFloorRow draws the "Upper floors light the ground" row
// (the SplitLevelGroundLight feature's own switch, owned by the menu), first in the card.
void DrawStoriesCard(void (*drawUpperFloorRow)());
void DrawObjectsCard();   // Objects tab (every option shown, in two groups)
void DrawBuildingsCard(); // Buildings tab (walls and roofs)
void DrawRoomsCard();     // Buildings tab (rooms with every lamp off)
void DrawLightDetailCard(); // Buildings tab (lamp light detail on walls and floors, read at startup)
void DrawWaterCard();     // Water tab: lamp glow on ponds (not the shore reflection)
void DrawSnowCard();      // Snow tab

// ---- menu: Water & Snow page, Water tab (Water Reflections card) ----
// Shore reflection strength (reflexoNoLago, 0..3, default 1.56; 0 = off). It is drawn in the lake pass of Night Lighting,
// which reads the scene depth of Depth Blur. Set saves the config like any Night Lighting option.
float ShoreReflection();
void SetShoreReflection(float strength);

// ---- menu: Performance page, Lamp lighting card ----
// The mode (lampLightingMode): 0 Classic (the game's room solves with every correction), 1 Instant (the default since 14/10; the
// active lot's outdoor lamps drawn on the GPU with shadows into every story's outside maps). Set saves the config.
int LampMode();
void SetLampMode(int mode);
// Developer Tester tab: the player options it shows, by TOML key ("luzDeForaPelasAberturas", "luzInternaPelasAberturas",
// "portasBloqueiamLuz"); set as their own switch would (saved, pushed at the next Present). Unknown keys: false, nothing.
bool TesterOption(const char* key);
void SetTesterOption(const char* key, bool value);
// Why this device cannot run Instant (English, for the log and a translated note), empty when it can or it is not known yet
std::string InstantUnavailable();
// The last measured GPU time of an Instant relight (ms), -1 while Instant is off or nothing was measured
float InstantGpuMs();
// Why Instant is chosen but does nothing now (English, translated by the menu), empty when it runs or is not chosen
std::string InstantInactive();

// ---- menu: Developer page (development build) ----
// Status, census, false colour, rebuild / relight buttons, light diagnostics, the light probe, counters and the generic
// list of every individual option.
void DrawDeveloper();

} // namespace NightLighting
