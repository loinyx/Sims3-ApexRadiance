#pragma once
// Apex's keyboard shortcuts, in presets (user, 30/09: "laptops often have the F keys behind Fn"; "presets, well thought").
// Every shortcut is eaten by the overlay's window procedure before the game sees it (Overlay::Client::HotkeyDown), so
// no key of the game can clash with one. The menu key is ApexConfig's [ui] toggle_key (the overlay's toggle chord);
// the other actions follow the preset ([ui] hotkey_preset) unless the player assigned an override in Shortcuts.
//
// Presets (docs/ui.md "Shortcuts"):
//   Letter keys (recommended): Ctrl+Shift+R menu, T compare, G refresh; dev V probe, B diagnostics, X recorder, F frame
//     capture. Nearby left-hand keys; no Fn.
//   Number row: Ctrl+Shift+1 menu, 2 compare, 3 refresh; dev 4 probe, 5 diagnostics, 6 recorder, 7 frame capture.
//   Function keys (classic layout): Ctrl+Shift+F11 menu, F10 compare, F9 refresh; dev F7 probe, F8 diagnostics, F6
//     recorder, F5 frame capture.
#include "apex_config.h"
#include <windows.h>

namespace Hotkeys {

enum class Action : int { Compare, Refresh, Probe, Diagnostics, Recorder, Screenshot, FrameCapture, Count };
enum class Preset : int { Letters, Numbers, FKeys, Count };
inline constexpr int kMine = 3; // [ui] hotkey_preset "mine": the player's own keys (compare_key, refresh_key), the rest from mine_base

// The keys of a preset (Menu comes from ApexConfig::UiSettings::toggle once chosen)
ApexConfig::KeyChord PresetMenu(Preset p);
ApexConfig::KeyChord PresetKey(Preset p, Action a);
const char* PresetName(Preset p);        // "Letter row", "Number row", "Function row" (translated where drawn)
const char* PresetDescription(Preset p); // one line for the preset card

// The key an action has now (preset or the player's own override)
ApexConfig::KeyChord Key(Action a);
const char* ActionName(Action a); // "Compare", "Refresh lighting", ...

// Window thread: vk pressed with the modifiers held now; true = an Apex shortcut (queued for Take)
bool OnKeyDown(WPARAM vk, bool repeat);
// Render thread: true once per press of that action's shortcut
bool Take(Action a);

}
