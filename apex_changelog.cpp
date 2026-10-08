// The menu's What's new list (see apex_changelog.h). One short line per change, in the player's words.
#include "apex_changelog.h"
#include <iterator>

namespace ApexChangelog {

namespace {

constexpr const char* k2110Added[] = {
    "Color groups have their own switches and keep your adjustments",
    "Right-click a filter to assign an optional keyboard shortcut",
    nullptr};
constexpr const char* k2110Improved[] = {nullptr};
constexpr const char* k2110Fixed[] = {
    "Hidden-interface effects recognize both recorded color-write states",
    nullptr};

constexpr const char* k2101Added[] = {nullptr};
constexpr const char* k2101Improved[] = {
    "Placing and moving outdoor lamps updates wall lighting faster, especially on multi-story lots",
    "Lighting colors, brightness, shadows and reach are unchanged",
    nullptr};
constexpr const char* k2101Fixed[] = {nullptr};

constexpr const char* k2100Added[] = {
    "Light behind balconies: softer light and shadows on walls above",
    nullptr};
constexpr const char* k2100Improved[] = {
    "Captures use less storage, and editing lamps takes less repeated work",
    nullptr};
constexpr const char* k2100Fixed[] = {
    "Ground lamps light upper-story walls with smoother falloff",
    "Doors and windows on foundations receive lamp light; walls keep it out of enclosed yards",
    nullptr};

constexpr const char* k290Added[] = {
    "The menu now supports all 21 game languages",
    nullptr};
constexpr const char* k290Improved[] = {
    "Lamp cones and foundation lighting are more accurate",
    "All Lighting options are enabled in the starting profiles",
    nullptr};
constexpr const char* k290Fixed[] = {
    "Apex catches its own errors instead of closing the game",
    nullptr};

constexpr const char* k282Added[] = {nullptr};
constexpr const char* k282Improved[] = {
    "What's new shows the last 8 versions, with a scroll bar",
    nullptr};
constexpr const char* k282Fixed[] = {nullptr};

constexpr const char* k281Added[] = {nullptr};
constexpr const char* k281Improved[] = {
    "Banding Fix on by default in every profile, with Smooth gradients at 8%",
    nullptr};
constexpr const char* k281Fixed[] = {
    "Doors no longer take the light of the room behind them",
    "Snowy ground under rugs is no longer darker than the rest of the lot",
    nullptr};

constexpr const char* k280Added[] = {
    "Light through doors and windows: walled yards without a roof lit only through their openings",
    "Lamp colors stay near lamps: a colored lamp no longer tints far floors and shiny objects",
    "Windows take outdoor light: their outer side lit by the lamps outside",
    nullptr};
constexpr const char* k280Improved[] = {
    "Lamp glow and shore reflections on lakes in rain and snow",
    "Shore reflections on Twinbrook's sea in rain",
    nullptr};
constexpr const char* k280Fixed[] = {
    "Indoor lamp light with Light detail High: no more 1 m squares or light through walls",
    "Lamp posts light the deck of a house on a foundation next to them",
    "Lamps no longer light floors above them or plants outside their beam",
    "Furniture behind a wall no longer takes the lamp of the next room",
    "Moving a lamp updates the light faster",
    "Wall lamp light at the right height on tall walls and houses on a foundation (a game bug)",
    "Color works in Edit in Game without turning it off and on",
    "Fence and curb tops no longer turn dark at night",
    "Color and screen effects no longer come and go with the camera in small worlds",
    nullptr};

// 2.7.1: 2.7.0 with the build an antivirus engine flagged changed (many players never got 2.7.0: Nexus held it), so it lists
// 2.7.0's lines too
constexpr const char* k271Fixed[] = {
    "No false alarm from antivirus programs that use BitDefender's engine",
    "Color settings and filters work again with Ambient Occlusion, Edge Smoothing or Depth Blur on",
    "No crash after the first loading screen with dxwrapper",
    "Screen effects wait until the world is drawn, never on menus or loading screens",
    "The pie menu no longer leaves an unfiltered box or a grey square",
    "Depth Blur no longer blurs the save screen",
    "The unused AMD Vulkan driver stays out of the game on PCs with AMD integrated graphics",
    nullptr};

constexpr const char* k270Added[] = {
    "Filters in Color: 26 looks to stack, each with its own strength",
    "LUT filter: your own look-up tables from the LUTs folder",
    "Lamp switches all at once: rooms, furniture and ground change together",
    "Lot Streaming page, off by default: more lots in full detail (by idavidveiga)",
    "Ambient Occlusion: Temporal smoothing, Half resolution and Thin object detail",
    "Light detail: sharper lamp light on walls and floors",
    "Room to save, Lighter window updates and Faster scripts in Performance",
    nullptr};
constexpr const char* k270Improved[] = {
    "The ground around switched lamps changes in one frame",
    "Furniture takes the light of lamps on other stories through openings",
    "A ground floor wall lamp also lights the half wall of the balcony above it",
    "Walls block lamp light on outdoor floors, objects and fences",
    "A lot you enter corrects its lighting sooner",
    "Moving or switching lamps in Build mode updates rooms sooner",
    "Lot edges stay seamless in rain, snow and melting snow",
    "Shore reflections on the water of Twinbrook, Bridgeport and Moonlight Falls",
    "Banding Fix has its own page; Smooth gradients is off by default",
    "The menu and start note open from the world selector on",
    "Built-in profiles leave Water & Snow, Banding Fix and Lot Streaming as you set them",
    "Sim Occlusion is on by default, and Ambient Occlusion has less grain",
    "Fewer hitches when lots stream in: lot lighting and new objects spread over frames",
    "Faster object and game file lookups",
    "No long frame when the ground light is rebuilt at load or with Refresh lighting",
    "Apex uses less memory and does less work while drawing",
    nullptr};
constexpr const char* k270Fixed[] = {
    "Screen effects wait until the world is drawn, never on menus or loading screens",
    "The pie menu no longer leaves an unfiltered box or a grey square",
    "Depth Blur no longer blurs the save screen",
    "The unused AMD Vulkan driver stays out of the game on PCs with AMD integrated graphics",
    nullptr};

constexpr const char* k260Added[] = {
    "Sim Occlusion: soft contact shadows on Sims, with separate body and hair strength",
    "A Distance slider for Ambient Occlusion",
    "A welcome page with ready-made profiles: Performance, Default and Quality",
    "An Attention page that appears only when something blocks an effect",
    "Filtered screenshots with F8, saved in the game's Screenshots folder or in Apex Radiance's",
    "Refresh lighting in Lighting > Overview, without Developer mode",
    "What's new: click the version at the bottom of the menu",
    nullptr};
constexpr const char* k260Improved[] = {
    "A cleaner menu: one-line header, footer with the version in the middle",
    "Hold Alt over the menu to hide it completely",
    "Buttons, fields and rows share the same sizes and alignment on every page",
    "Shortcuts: pick a set (letters, numbers or F keys) and change any shortcut",
    "Profiles: the built-in ones are always listed, with 30 new icons",
    "Profiles: choose which settings to apply from a clear selection card",
    "Clearer Lighting page, with fine tuning moved under Advanced",
    "Rooms at Night takes a room color saved in Sims3SettingsSetter into account",
    "Indoor lamps light several stories through stairwells and open floors",
    "Indoor light updates sooner after adding a story, a roof or closing a room",
    "The Optimize rendering switch is gone: Apex renders as it did before 2.5.6",
    "The page and card reset buttons are gone; every control keeps its own default",
    nullptr};
constexpr const char* k260Fixed[] = {
    "Lighting updates after editing lamps and after day and night changes in Build mode",
    "Ground brightness works on every kind of terrain, with no edge at lot borders",
    "Lamp light no longer leaks through walls or into raised rooms on other stories",
    "Depth Blur and the start note wait until the world has finished loading",
    "Effects keep working when the game interface is hidden",
    "Typing in the cheat console no longer triggers Apex shortcuts",
    nullptr};

constexpr const char* k256Added[] = {"Lamp light on alpha-blended sidewalks", nullptr};
constexpr const char* k256Improved[] = {"Rendering optimizations are on by default", nullptr};
constexpr const char* k256Fixed[] = {"Street lamps outside lots update the lighting after their color changes", nullptr};

constexpr Release kReleases[] = {
    {"2.11.0", "2026-10-07", k2110Added, k2110Improved, k2110Fixed},
    {"2.10.1", "2026-10-07", k2101Added, k2101Improved, k2101Fixed},
    {"2.10.0", "2026-10-07", k2100Added, k2100Improved, k2100Fixed},
    {"2.9.0", "2026-10-07", k290Added, k290Improved, k290Fixed},
    {"2.8.2", "2026-10-07", k282Added, k282Improved, k282Fixed},
    {"2.8.1", "2026-10-07", k281Added, k281Improved, k281Fixed},
    {"2.8.0", "2026-10-07", k280Added, k280Improved, k280Fixed},
    {"2.7.1", "2026-10-06", k270Added, k270Improved, k271Fixed},
    {"2.7.0", "2026-10-06", k270Added, k270Improved, k270Fixed},
    {"2.6.0", "2026-10-05", k260Added, k260Improved, k260Fixed},
    {"2.5.6", "2026-10-03", k256Added, k256Improved, k256Fixed},
};

} // namespace

int Count() { return static_cast<int>(std::size(kReleases)); }
const Release& Get(int index) { return kReleases[index < 0 || index >= Count() ? 0 : index]; }

} // namespace ApexChangelog
