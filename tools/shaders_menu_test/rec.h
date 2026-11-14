#pragma once
// Included first by the units under test: every ImGui:: call below goes through ImGuiRec, which records each control's
// rectangle and window (and the IDs of the open/closed toggles) and then calls ImGui itself.
#include "imgui.h"
#include "imgui_internal.h"
#include <string>
#include <vector>

struct RecItem { std::string id; ImRect rect; ImGuiWindow* window; bool disabled; };
inline std::vector<RecItem>& RecItems() { static std::vector<RecItem> v; return v; }
struct RecToggle { ImGuiWindow* window; ImGuiID id; std::string key; };
inline std::vector<RecToggle>& RecToggles() { static std::vector<RecToggle> v; return v; }

namespace ImGuiRec {
using namespace ImGui;
inline void Record(const char* id)
{
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.LastItemData.Rect.GetWidth() > 0.0f && g.LastItemData.Rect.GetHeight() > 0.0f)
        RecItems().push_back({ id ? id : "", g.LastItemData.Rect, g.CurrentWindow, (g.LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0 });
}
inline ImGuiID GetID(const char* s)
{
    const ImGuiID id = ImGui::GetID(s);
    if (!strcmp(s, "##Open") || !strcmp(s, "##AdvancedOpen") || !strcmp(s, "##GroupOpen")) RecToggles().push_back({ ImGui::GetCurrentWindow(), id, s });
    return id;
}
inline ImGuiID GetID(const char* b, const char* e) { return ImGui::GetID(b, e); }
inline ImGuiID GetID(const void* p) { return ImGui::GetID(p); }
inline ImGuiID GetID(int n) { return ImGui::GetID(n); }
inline bool InvisibleButton(const char* id, const ImVec2& s, ImGuiButtonFlags f = 0) { const bool r = ImGui::InvisibleButton(id, s, f); Record(id); return r; }
inline bool Checkbox(const char* l, bool* v) { const bool r = ImGui::Checkbox(l, v); Record(l); return r; }
inline bool BeginCombo(const char* l, const char* p, ImGuiComboFlags f = 0) { const bool r = ImGui::BeginCombo(l, p, f); if (!r) Record(l); return r; }
inline bool DragFloat(const char* l, float* v, float s = 1.0f, float a = 0.0f, float b = 0.0f, const char* fmt = "%.3f", ImGuiSliderFlags f = 0) { const bool r = ImGui::DragFloat(l, v, s, a, b, fmt, f); Record(l); return r; }
inline bool SliderFloat(const char* l, float* v, float a, float b, const char* fmt = "%.3f", ImGuiSliderFlags f = 0) { const bool r = ImGui::SliderFloat(l, v, a, b, fmt, f); Record(l); return r; }
inline bool SliderInt(const char* l, int* v, int a, int b, const char* fmt = "%d", ImGuiSliderFlags f = 0) { const bool r = ImGui::SliderInt(l, v, a, b, fmt, f); Record(l); return r; }
inline bool InputText(const char* l, char* buf, size_t n, ImGuiInputTextFlags f = 0, ImGuiInputTextCallback cb = nullptr, void* u = nullptr) { const bool r = ImGui::InputText(l, buf, n, f, cb, u); Record(l); return r; }
inline bool InputTextWithHint(const char* l, const char* h, char* buf, size_t n, ImGuiInputTextFlags f = 0, ImGuiInputTextCallback cb = nullptr, void* u = nullptr) { const bool r = ImGui::InputTextWithHint(l, h, buf, n, f, cb, u); Record(l); return r; }
inline bool TreeNode(const char* l) { const bool r = ImGui::TreeNode(l); Record(l); return r; }
inline bool Selectable(const char* l, bool s = false, ImGuiSelectableFlags f = 0, const ImVec2& z = ImVec2(0, 0)) { const bool r = ImGui::Selectable(l, s, f, z); Record(l); return r; }
inline bool Selectable(const char* l, bool* s, ImGuiSelectableFlags f = 0, const ImVec2& z = ImVec2(0, 0)) { const bool r = ImGui::Selectable(l, s, f, z); Record(l); return r; }
} // namespace ImGuiRec
#define ImGui ImGuiRec
