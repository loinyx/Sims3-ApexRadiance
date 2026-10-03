// Apex's keyboard shortcuts in presets (see hotkeys.h).
#include "hotkeys.h"
#include "build_flavor.h"
#include <atomic>

namespace Hotkeys {
namespace {

using ApexConfig::KeyChord;
constexpr int kActions = static_cast<int>(Action::Count);
std::atomic<bool> g_pending[kActions] = {};

KeyChord Chord(UINT vk) {
    KeyChord c;
    c.vk = vk;
    c.ctrl = true;
    c.shift = true;
    c.alt = false;
    return c;
}

// [preset][action]: Compare, Refresh, Probe, Diagnostics, Recorder, Screenshot (custom only), FrameCapture
constexpr UINT kKeys[static_cast<int>(Preset::Count)][kActions] = {
    {'T', 'G', 'V', 'B', 'X', 0, 'F'},
    {'2', '3', '4', '5', '6', 0, '7'},
    {VK_F10, VK_F9, VK_F7, VK_F8, VK_F6, 0, VK_F5},
};
constexpr UINT kMenu[static_cast<int>(Preset::Count)] = {'R', '1', VK_F11};

Preset Current() {
    const int p = ApexConfig::GetUi().hotkeyPreset;
    if (p == kMine) {
        const int b = ApexConfig::GetUi().minePresetBase;
        return b >= 0 && b < static_cast<int>(Preset::Count) ? static_cast<Preset>(b) : Preset::Letters;
    }
    return p >= 0 && p < static_cast<int>(Preset::Count) ? static_cast<Preset>(p) : Preset::FKeys;
}

bool Held(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

} // namespace

KeyChord PresetMenu(Preset p) { return Chord(kMenu[static_cast<int>(p)]); }
KeyChord PresetKey(Preset p, Action a) { return Chord(kKeys[static_cast<int>(p)][static_cast<int>(a)]); }

const char* PresetName(Preset p) {
    switch (p) {
    case Preset::Letters: return "Letter row";
    case Preset::Numbers: return "Number row";
    default: return "Function row";
    }
}
const char* PresetDescription(Preset p) {
    switch (p) {
    case Preset::Letters: return "Nearby letter keys keep your shortcuts together; no Fn key needed";
    case Preset::Numbers: return "Easy to remember: 1 opens the menu, 2 compares, 3 refreshes";
    default: return "The familiar function-key shortcuts from earlier versions";
    }
}

KeyChord Key(Action a) {
    const ApexConfig::UiSettings ui = ApexConfig::GetUi();
    if (a == Action::Screenshot) return ui.screenshotKey;
    switch (a) {
    case Action::Compare: if (ui.compareKey.vk) return ui.compareKey; break;
    case Action::Refresh: if (ui.refreshKey.vk) return ui.refreshKey; break;
    case Action::Probe: if (ui.probeKey.vk) return ui.probeKey; break;
    case Action::Diagnostics: if (ui.diagnosticsKey.vk) return ui.diagnosticsKey; break;
    case Action::Recorder: if (ui.recorderKey.vk) return ui.recorderKey; break;
    case Action::FrameCapture: if (ui.frameCaptureKey.vk) return ui.frameCaptureKey; break;
    default: break;
    }
    return PresetKey(Current(), a);
}

const char* ActionName(Action a) {
    switch (a) {
    case Action::Compare: return "Compare with the game";
    case Action::Refresh: return "Refresh the lighting";
    case Action::Probe: return "Light capture";
    case Action::Diagnostics: return "Lighting snapshot";
    case Action::Recorder: return "Recording";
    case Action::Screenshot: return "Take a filtered screenshot";
    default: return "Frame Capture";
    }
}

bool OnKeyDown(WPARAM vk, bool repeat) {
    const bool ctrl = Held(VK_CONTROL), shift = Held(VK_SHIFT), alt = Held(VK_MENU);
    for (int i = 0; i < kActions; i++) {
        if (kPublicBuild && i == static_cast<int>(Action::FrameCapture)) continue;
        if (i == static_cast<int>(Action::Screenshot) && !ApexConfig::GetUi().screenshotShortcutEnabled) continue;
        const KeyChord c = Key(static_cast<Action>(i));
        if (!c.vk || c.vk != vk || c.ctrl != ctrl || c.shift != shift || c.alt != alt) continue;
        if (!repeat) g_pending[i].store(true);
        return true;
    }
    return false;
}

bool Take(Action a) { return g_pending[static_cast<int>(a)].exchange(false); }

} // namespace Hotkeys
