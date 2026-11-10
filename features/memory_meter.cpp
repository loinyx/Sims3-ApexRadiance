// Game memory meter (see memory_meter.h). Credits: @loinyx
#include "memory_meter.h"
#include "imgui.h"
#include "ui/i18n.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>

namespace MemoryMeter {
using VioletTheme::Col;
namespace {

constexpr int kCount = 120;
constexpr uint64_t kLow = 320ull << 20;      // Room to save empties the game's idle cache below this largest block
constexpr uint64_t kCritical = 160ull << 20; // ...and lets its reserve go below this one

struct Sample {
    float usedMb = 0, largestMb = 0;
};
std::mutex g_lock;
int g_count = 0, g_next = 0;
float g_peakMb = 0;
uint64_t g_free = 0, g_largest = 0, g_reserved = 0;
unsigned long long g_lastPush = 0;

uint64_t Total() {
    static const uint64_t total = [] {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) - reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress)) + 1;
    }();
    return total;
}

// The menu's own sample while Room to save is off (render thread, at most every 2 s)
void SampleNow() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uint64_t freeBytes = 0, largest = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t at = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
         at < hi && VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi) == sizeof mbi;) {
        if (mbi.State == MEM_FREE) {
            freeBytes += mbi.RegionSize;
            largest = std::max<uint64_t>(largest, mbi.RegionSize);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) break;
        at = next;
    }
    uint64_t reserved;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        reserved = g_reserved;
    }
    Push(freeBytes, largest, reserved);
}

// Every figure in whole MB (no decimal point to misread as a thousands separator); 4 GB shows as 4096
std::string Mb(uint64_t bytes) { return std::to_string((bytes + (1ull << 19)) >> 20); }
// GB with two decimals, as Sims3SettingsSetter shows it ("1.25"); 4 GB of address space reads "4.00"
std::string Gb(double bytes) { return std::format("{:.2f}", bytes / 1073741824.0); }

} // namespace

void Push(uint64_t free, uint64_t largest, uint64_t reserved) {
    const uint64_t total = Total();
    free = std::min(free, total);
    reserved = std::min(reserved, total - free);
    std::lock_guard<std::mutex> lock(g_lock);
    const Sample s{static_cast<float>((total - free - reserved) >> 20), static_cast<float>(largest >> 20)}; // the game's own use
    g_reserved = reserved;
    g_next = (g_next + 1) % kCount;
    g_count = std::min(g_count + 1, kCount);
    g_peakMb = std::max(g_peakMb, s.usedMb);
    g_free = free;
    g_largest = largest;
    g_lastPush = GetTickCount64();
}

Status Check(unsigned maxAgeMs) {
    static unsigned long long s_ownAt = 0;
    const unsigned long long now = GetTickCount64();
    bool stale;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        stale = !g_lastPush || now - g_lastPush > maxAgeMs; // Room to save off: sample here
    }
    if (stale && now - s_ownAt >= maxAgeMs) {
        s_ownAt = now;
        SampleNow();
    }
    std::lock_guard<std::mutex> lock(g_lock);
    Status s;
    s.known = g_count > 0;
    s.low = s.known && g_largest < kLow;
    s.critical = s.known && g_largest < kCritical;
    return s;
}

bool RenderTile() {
    Check(1000); // while the card is on screen: a fresh sample every second (a walk of the address space, ~1 ms)
    int count;
    float peak;
    uint64_t freeBytes, largest, reserved;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        count = g_count;
        peak = g_peakMb;
        freeBytes = g_free;
        largest = g_largest;
        reserved = g_reserved;
    }
    const uint64_t total = Total();
    const uint64_t used = total - std::min(freeBytes + reserved, total); // the game's own use, without Room to save's reserve
    const bool critical = count && largest < kCritical, low = count && largest < kLow;
    const unsigned stateRgb = critical ? VioletTheme::kError : low ? VioletTheme::kWarning : VioletTheme::kSuccess;
    const char* state = critical ? "Critical" : low ? "Getting low" : "Healthy";
    const float u = ApexUi::Unit();

    // What is shown glides to the new value (a sample every 4 s would otherwise jump), and the bar's colour follows the
    // state: violet while healthy, amber while low, red when critical
    static float s_used = -1.0f, s_reserved = 0.0f;
    static ImVec4 s_colour = Col(VioletTheme::kAccent);
    const float k = 1.0f - std::exp(-ImGui::GetIO().DeltaTime * 8.0f);
    if (s_used < 0.0f) s_used = static_cast<float>(used);
    s_used += (static_cast<float>(used) - s_used) * k;
    s_reserved += (static_cast<float>(reserved) - s_reserved) * k;
    const ImVec4 target = Col(critical ? VioletTheme::kError : low ? VioletTheme::kWarning : VioletTheme::kAccent);
    s_colour = ImVec4(s_colour.x + (target.x - s_colour.x) * k, s_colour.y + (target.y - s_colour.y) * k, s_colour.z + (target.z - s_colour.z) * k, 1.0f);

    const ImVec2 halfMin = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    // Game memory | the health state, the game's use large, one thin bar (use, Room to save's reserve, free, the session's
    // peak as a tick), one line
    ApexUi::StatusHeader("Game memory", state, stateRgb, true);
    const std::string of = I18n::Trf("of {} GB", Gb(static_cast<double>(total)));
    ApexUi::StatusValue(count ? Gb(s_used).c_str() : "-", of.c_str());
    const ImVec2 p0 = ApexUi::StatusBand();
    const float h = ApexUi::StatusBandHeight(), r = h * 0.5f;
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto share = [&](double bytes) { return total ? std::clamp(static_cast<float>(bytes / static_cast<double>(total)), 0.0f, 1.0f) : 0.0f; };
    const float xUsed = p0.x + w * share(s_used), xReserved = std::min(p1.x, xUsed + w * share(s_reserved));
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Col(VioletTheme::kToggleOff)), r);
    if (count) {
        if (s_reserved >= 1.0f) { // the reserve: a lighter grey with diagonal hatching (////), after the use
            const ImVec2 a(xUsed, p0.y), b(std::max(xReserved, xUsed + h), p1.y);
            dl->AddRectFilled(a, b, ImGui::GetColorU32(Col(VioletTheme::kTextMuted, 0.35f)), r);
            dl->PushClipRect(a, b, true);
            for (float x = a.x - h; x < b.x; x += 3.0f * u) dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), ImGui::GetColorU32(Col(VioletTheme::kText, 0.45f)), 1.0f * u);
            dl->PopClipRect();
        }
        dl->AddRectFilled(p0, ImVec2(std::max(xUsed, p0.x + h), p1.y), ImGui::GetColorU32(s_colour), r);
        if (peak > 0.0f) { // the session's peak: a thin tick through the bar
            const float xp = p0.x + w * share(static_cast<double>(peak) * 1048576.0);
            dl->AddRectFilled(ImVec2(xp - 0.75f * u, p0.y - 3.0f * u), ImVec2(xp + 0.75f * u, p1.y + 3.0f * u), ImGui::GetColorU32(Col(VioletTheme::kText, 0.55f)));
        }
    }
    ApexUi::StatusFooter(count ? I18n::Trf("Largest free block {} GB \xC2\xB7 peak {} GB", Gb(static_cast<double>(largest)), Gb(static_cast<double>(peak) * 1048576.0))
                               : std::string());

    // The whole half: click to open Room to save's settings (Performance > Memory handling)
    const ImVec2 halfMax(halfMin.x + w, ImGui::GetCursorScreenPos().y);
    const bool hovered = count && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(halfMin, halfMax, false);
    if (!hovered) return false;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // over the hatched part: what it is, with its size
    if (s_reserved >= 1.0f && ImGui::IsMouseHoveringRect(ImVec2(xUsed, p0.y - 6.0f * u), ImVec2(std::max(xReserved, xUsed + 6.0f * u), p1.y + 6.0f * u))) {
        const std::string tip = std::string(I18n::Tr("Room to save")) + " \xC2\xB7 " + Mb(reserved) + " MB";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

} // namespace MemoryMeter
