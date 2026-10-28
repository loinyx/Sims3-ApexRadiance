// The menu's What's new list (see apex_changelog.h). One short line per change, in the player's words.
#include "apex_changelog.h"
#include <iterator>

namespace ApexChangelog {

namespace {

constexpr const char* k2171Improved[] = {
    "Performance: lamp light on fences and outdoor objects is reused instead of worked out again",
    nullptr};
constexpr const char* k2171Fixed[] = {
    "Night Lights: a lamp no longer lights the next room's floor and furniture through the wall",
    "Night Lights: street light no longer reaches counters and furniture inside the house",
    nullptr};

constexpr const char* k2170Added[] = {
    "Performance: Room to save asks once, after updating, whether to keep it on",
    nullptr};
constexpr const char* k2170Improved[] = {
    "Performance: changing a lamp no longer relights rooms on other lots",
    nullptr};

constexpr const char* k2163Fixed[] = {
    "Night Lights: no more dark squares on lit floors under furniture",
    nullptr};

constexpr const char* k2162Improved[] = {
    "Lot Streaming: lots that load together share the work, so panning stutters less",
    "Night Lights: fence shadows on the lawn update in a fraction of the time",
    nullptr};
constexpr const char* k2162Fixed[] = {
    "Lot Streaming: Keep lot visibility stable keeps lots at the edge in full detail",
    "Lot Streaming: Smooth lot streaming keeps working with up to 16 detailed lots",
    nullptr};

constexpr const char* k2161Empty[] = {nullptr};
constexpr const char* k2161Fixed[] = {
    "Night Lights: extra safety so old lamp light cannot stay stuck on the ground",
    nullptr};

constexpr const char* k2160Added[] = {
    "Banding Fix: High-precision scene for smoother gradients, on by default",
    nullptr};
constexpr const char* k2160Improved[] = {
    "Banding Fix: redesigned card with a finer grain that leaves the menu alone",
    "Night Lights: strong lamps light the ground and walls beside them at full strength",
    "Night Lights: lamp light through doors and windows fades naturally into rooms",
    "Night Lights: wall lamps light the wall they hang on",
    nullptr};
constexpr const char* k2160Fixed[] = {
    "Night Lights: room light has no lines or steps beside walls and door frames",
    "Night Lights: coloured lamps keep their colour where their light is strongest",
    "Night Lights: window light stays the same as the camera turns",
    "Night Lights: lamp edits update the rooms behind doors and windows",
    nullptr};

constexpr const char* k2156Empty[] = {nullptr};
constexpr const char* k2156Improved[] = {
    "Night Lights: lighting calculations reuse unchanged data without reducing quality",
    "Performance: cache compression avoids unnecessary match searches",
    nullptr};
constexpr const char* k2156Fixed[] = {
    "Night Lights: frozen and melting terrain keeps continuous light at lot edges",
    "Night Lights: trees follow lamp brightness through dusk and dawn",
    "Edit Town restores missing outdoor lighting work while other rooms update",
    nullptr};

constexpr const char* k2155Empty[] = {nullptr};
constexpr const char* k2155Fixed[] = {
    "Edit Town finishes loading after leaving Build or Buy mode",
    nullptr};

constexpr const char* k2154Added[] = {
    "Restart lighting rebuilds temporary light maps and keeps your settings",
    nullptr};
constexpr const char* k2154Improved[] = {nullptr};
constexpr const char* k2154Fixed[] = {
    "Night Lights: snowy lot edges stay continuous as snow melts",
    "Performance: effects and animal fur update safely while lots load",
    "Night Lights: gables and upper trim keep continuous lamp light",
    "Night Lights: short pillars and stairs respect solid floors beneath lamps",
    nullptr};

constexpr const char* k2153Added[] = {
    "Rooms at Night: window and door light adds to garden lamps on the ground",
    nullptr};
constexpr const char* k2153Improved[] = {
    "Wall and fence shadows on the ground take away only the light they block",
    "Ground shadows show sooner, nearest lot first, with no seam at the lot's edge",
    "Windows let lamp light through at their real size, with glass",
    "No moonlight by default: at night only lamps and the sky light the town",
    "Less work at night: smoother frames with the camera still and while moving",
    "The screen effects share one copy of the picture, using less video memory",
    nullptr};
constexpr const char* k2153Fixed[] = {
    "Trees on lots with a basement are no longer flat and white at night",
    "Ceiling lamps no longer light the closed rooms on the story above",
    "Closed gates and narrow fence gaps no longer let lamp light through",
    "Fences and gates take as much lamp light as the ground beside them",
    "Colored lamps keep their color near the lamp on objects and fences",
    "Gables and eaves no longer show a hard step of light where they start",
    "Snowy lots take wall and fence shadows like the grass beside them",
    "Lamp light on rectangular lots sits under its lamp, no longer shifted",
    "Area lights light only the side they face, not whole tree crowns",
    nullptr};
constexpr const char* k2152Fixed[] = {
    "Moving the camera at night is smooth again with Instant lamp lighting",
    nullptr};
constexpr const char* k2151Added[] = {
    "Filtered screenshots can be saved to any folder you choose",
    nullptr};
constexpr const char* k2151Improved[] = {
    "Moonlight is softer by default and in every preset",
    "Lamps brighten smoothly at dusk and dim smoothly at dawn",
    "Loading lots is smoother: the extra lighting work follows your frame rate",
    "Instant lamp lighting does less work while a lot keeps relighting",
    nullptr};
constexpr const char* k2151Fixed[] = {
    "A lot placed in Edit Town no longer stays on Processing",
    "Area lights of a lot light the ground past the lot's border",
    "Lamps hidden behind objects no longer reflect on water",
    "Lamps placed on the ground light the lawn around them",
    "Lamps keep their night strength in Build mode at night",
    "The moon lights the town from higher up: no more dark stripes on roads at night",
    nullptr};
constexpr const char* k2150Added[] = {
    "Steam Deck and Linux: Proton is detected, effects need no shader compiler, and the Deck suggests the Performance preset",
    nullptr};
constexpr const char* k2150Improved[] = {
    "Indoor lights update sooner when you enter a lot or change floors",
    "Lamps on other lots no longer delay the lights of the lot you play",
    "Furniture takes the color of its room's lamps, like the walls",
    "Objects and their floor show a lamp's new light together",
    "Apex frees the memory of lots you have left",
    "Presets set every Performance option, including the lamp lighting mode; Default is now Balanced",
    "Rooms lit through arches and stairwells are balanced, never darker than an empty room",
    "Turning all effects off keeps Room to save on",
    "Rooms at Night: the background light is 20% by default and in every preset",
    "Options on by default for several versions no longer show an Experimental badge",
    "Ambient Occlusion and Depth Blur show why they are not running",
    nullptr};
constexpr const char* k2150Fixed[] = {
    "Lamp light from another floor no longer passes through the walls under an opening",
    "Light keeps passing through arches and doors after Night Lights is turned off and on",
    "Rooms that take almost no light stay dark, without a doubled glow",
    nullptr};

constexpr const char* k2140Added[] = {
    "Indoor light between floors: light bounces up stairwells onto the walls and floors above",
    "Realistic lamp reach lights trees and bushes from each lamp's side, through the leaves",
    "Trees and bushes shade lamp light on the ground, like walls",
    nullptr};
constexpr const char* k2140Improved[] = {
    "Room to save releases idle buffers earlier during saving",
    "Screen effects avoid unused image copies and repeated timing work",
    "Object and compression caches reduce repeated work with bounded memory",
    "Rooms lit only through a stairwell look brighter, as eyes adjust to the dark",
    "Light through stairwell openings has soft edges and stops at the walls in its way",
    "Stairs take their room's light and color and blend into the floor above",
    "Floors beside an arch blend into the next room's light, with soft edges",
    "Lamps that turn on by themselves update their own room first",
    "A small lamp lights only the nearby branches of a big tree, not its whole crown",
    "Lighting updates repeat less work and avoid waiting on the graphics card",
    "The game trims its unused memory every minute and before saving (Steam)",
    nullptr};
constexpr const char* k2140Fixed[] = {
    "Night Lights improves lamp blocking near stairs and closed floors",
    "Indoor light no longer changes as the camera moves",
    "A lamp switched off over a stairwell no longer leaves its light below",
    "The floor beside a stairwell opening no longer shows a lit strip",
    "Room to save prepares saves with sims3-modernization installed too",
    nullptr};

constexpr const char* k2132Improved[] = {
    "The latest lighting features also work on the EA app version 1.69",
    nullptr};
constexpr const char* k2132Added[] = {nullptr};
constexpr const char* k2132Fixed[] = {nullptr};

constexpr const char* k2131Improved[] = {
    "Walls shade lamp light on grass, streets and sidewalks outside the lot too",
    "Garden walls and fences shade lamp light on the ground by their height",
    "Lamp light on lot grass has smooth shadow edges with Instant (GPU)",
    "Realistic lamp reach also dims lamp light on trees and bushes by distance",
    "Indoor lamp edits show the lamp's room and furniture sooner",
    "Lighting tabs show the main choices and fold the game fixes",
    nullptr};
constexpr const char* k2131Fixed[] = {
    "Large trees next to a lamp take its light",
    nullptr};
constexpr const char* k2131Added[] = {nullptr};

constexpr const char* k2130Added[] = {
    "Lamp lighting: Instant (GPU) updates every floor at once with real shadows",
    "Outdoor light through openings: garden lamps light rooms through doors, windows and arches",
    "Indoor light through openings: lit rooms shine onto the garden and the next room",
    "Doors block light: closed doors stop lamp light, glass doors let it through",
    "Walls shade lamp light near the ground: grass, paths and curbs behind walls stay dark",
    "Shadow softness: light edges soften with the lamp's size",
    "Smooth light maps: lamp light in rooms without blocks, bands or rings",
    nullptr};
constexpr const char* k2130Improved[] = {
    "Classic lamp lighting is lighter: light updates after release, all floors together",
    "Bushes and trees behind walls and houses get no garden lamp light",
    "Dragging a lamp is smoother",
    "Less work per frame for drawing and lighting",
    nullptr};
constexpr const char* k2130Fixed[] = {
    "Disabled lamps no longer light nearby objects",
    "Outdoor lamps no longer light foliage by day",
    "Garden lamps reach colonnades and porches on a foundation",
    "Columns inside arches get lamp light",
    "Light under houses on foundations is no longer stopped by a false ceiling",
    "Upper-story railings no longer pick up light from ground lamps",
    nullptr};

constexpr const char* k2123Added[] = {
    "Realistic lamp reach: lamp light on outside walls fades with distance",
    nullptr};
constexpr const char* k2123Improved[] = {
    "Lamp edits all at once: the lamp's floor updates first, other floors together",
    "Objects, fences and windows take the same lamp light as nearby walls",
    "Walls block light on objects is now called Buildings block light on objects",
    "Removed Light behind balconies; balcony floors now block lamps below them",
    nullptr};
constexpr const char* k2123Fixed[] = {
    "A lamp under a balcony no longer lights walls and objects above it",
    "Fence and railing tops above the lamps no longer take ground light",
    "Outdoor lamps no longer light walls, objects and ground in daylight",
    nullptr};

constexpr const char* k2122Added[] = {"Presets can have their own keyboard shortcuts", nullptr};
constexpr const char* k2122Improved[] = {"Keyboard shortcuts now apply presets instead of single filters", nullptr};
constexpr const char* k2122Fixed[] = {nullptr};

constexpr const char* k2121Added[] = {nullptr};
constexpr const char* k2121Improved[] = {"Exterior walls respond a little faster to lighting changes", nullptr};
constexpr const char* k2121Fixed[] = {
    "Fence lighting better follows the position of lamps",
    "Gates receive lighting more consistent with fences",
    nullptr};

constexpr const char* k2120Added[] = {
    "3D CUBE LUTs with adjustable strength",
    "Presets have their own page and can be shared as TOML or ZIP",
    "Restart Apex graphics without losing your settings",
    nullptr};
constexpr const char* k2120Improved[] = {nullptr};
constexpr const char* k2120Fixed[] = {
    "Screen effects preserve transparency and restore graphics state",
    nullptr};

constexpr const char* k2111Added[] = {nullptr};
constexpr const char* k2111Improved[] = {
    "Removed Temporal smoothing and Thin object detail from Ambient Occlusion",
    "Ambient Occlusion and Sim Occlusion remain available",
    nullptr};
constexpr const char* k2111Fixed[] = {nullptr};

constexpr const char* k2110Added[] = {
    "Color groups have their own switches and keep your adjustments",
    "Right-click a filter to assign an optional keyboard shortcut",
    nullptr};
constexpr const char* k2110Improved[] = {nullptr};
constexpr const char* k2110Fixed[] = {
    "Corrected screen-effect timing when hiding the game interface",
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
    {"2.17.1", "2026-10-27", nullptr, k2171Improved, k2171Fixed},
    {"2.17.0", "2026-10-27", k2170Added, k2170Improved, nullptr},
    {"2.16.3", "2026-10-27", nullptr, nullptr, k2163Fixed},
    {"2.16.2", "2026-10-27", nullptr, k2162Improved, k2162Fixed},
    {"2.16.1", "2026-10-27", k2161Empty, k2161Empty, k2161Fixed},
    {"2.16.0", "2026-10-27", k2160Added, k2160Improved, k2160Fixed},
    {"2.15.6", "", k2156Empty, k2156Improved, k2156Fixed},
    {"2.15.5", "", k2155Empty, k2155Empty, k2155Fixed},
    {"2.15.4", "2026-10-24", k2154Added, k2154Improved, k2154Fixed},
    {"2.15.3", "2026-10-23", k2153Added, k2153Improved, k2153Fixed},
    {"2.15.2", "2026-10-22", nullptr, nullptr, k2152Fixed},
    {"2.15.1", "2026-10-22", k2151Added, k2151Improved, k2151Fixed},
    {"2.15.0", "2026-10-21", k2150Added, k2150Improved, k2150Fixed},
    {"2.14.0", "2026-10-20", k2140Added, k2140Improved, k2140Fixed},
    {"2.13.2", "2026-10-15", k2132Added, k2132Improved, k2132Fixed},
    {"2.13.1", "2026-10-15", k2131Added, k2131Improved, k2131Fixed},
    {"2.13.0", "2026-10-14", k2130Added, k2130Improved, k2130Fixed},
    {"2.12.3", "2026-10-13", k2123Added, k2123Improved, k2123Fixed},
    {"2.12.2", "2026-10-12", k2122Added, k2122Improved, k2122Fixed},
    {"2.12.1", "2026-10-12", k2121Added, k2121Improved, k2121Fixed},
    {"2.12.0", "2026-10-09", k2120Added, k2120Improved, k2120Fixed},
    {"2.11.1", "2026-10-08", k2111Added, k2111Improved, k2111Fixed},
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
