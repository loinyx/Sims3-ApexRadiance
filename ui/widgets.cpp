// Widgets of the Violet menu (see widgets.h).
#include "widgets.h"
#include "violet_theme.h"
#include "i18n.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ApexUi {
namespace {

using VioletTheme::Col;

// The text in the menu's language (i18n.h): every text a widget draws goes through these
inline const char* T(const char* s) { return I18n::Tr(s); }

constexpr unsigned kFrameBg = 0x26272D;      // the style's FrameBg (segmented track)
constexpr unsigned kButtonBorder = 0x34353D; // secondary button border

ImU32 U32(unsigned rgb, float alpha = 1.0f) { return ImGui::GetColorU32(Col(rgb, alpha)); }

// A packed colour with the current style alpha applied (disabled rows, the menu's fade), like GetColorU32 does
ImU32 WithStyleAlpha(ImU32 c) {
    const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * ImGui::GetStyle().Alpha;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::lround(std::fmin(std::fmax(a, 0.0f), 1.0f) * 255.0f)) << IM_COL32_A_SHIFT);
}

// ---- search filter (BeginFilter / EndFilter) ----
struct FilterState {
    bool active = false;
    std::vector<std::string> words; // lower case
    std::string crumb;
    int crumbId = -1;
    int clicked = -1;
    int drawn = 0;
};
FilterState g_filter;
bool g_inControlRow = false; // between BeginControlRow (visible) and EndControlRow: its buttons are part of the row
bool g_nextRaw = false;      // SetNextRowUntranslated: the next row's label is shown as it is
bool g_rowRaw = false;       // the current row's label is shown as it is

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// The search compares texts folded: lower case and without accents ("Ç" and "ç" find "c", "é" finds "e"), for the
// Latin-1 letters Portuguese, Spanish and French use (UTF-8 C3 80..C3 BF) and "œ"; other characters stay as they are.
const char* FoldLatin1(unsigned char second) {
    static const char* const kFold[64] = {
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  // C0..CF
        "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "ss", // D0..DF
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  // E0..EF
        "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "y",  // F0..FF
    };
    return second >= 0x80 && second <= 0xBF ? kFold[second - 0x80] : nullptr;
}

void AppendLower(std::string& out, const char* s, const char* end = nullptr) {
    if (!s) return;
    for (const char* c = s; end ? c < end : *c; ++c) {
        const unsigned char b = static_cast<unsigned char>(*c);
        const bool second = end ? c + 1 < end : c[1] != '\0';
        if (second) {
            const unsigned char n = static_cast<unsigned char>(c[1]);
            const char* folded = b == 0xC3 ? FoldLatin1(n) : (b == 0xC5 && (n == 0x92 || n == 0x93)) ? "oe" : nullptr;
            if (folded) {
                out += folded;
                ++c;
                continue;
            }
        }
        out.push_back(LowerAscii(*c));
    }
}

void SplitWords(const char* query, std::vector<std::string>& out) {
    out.clear();
    std::string word;
    for (const char* c = query ? query : ""; ; ++c) {
        if (*c == '\0' || *c == ' ' || *c == '\t') {
            if (!word.empty()) out.push_back(word);
            word.clear();
            if (*c == '\0') break;
        } else {
            // one UTF-8 character at a time, folded like the texts searched
            const char* next = c + 1;
            while ((static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
            AppendLower(word, c, next);
            c = next - 1;
        }
    }
}

bool WordsIn(const std::vector<std::string>& words, const std::string& hay) {
    for (const std::string& w : words)
        if (hay.find(w) == std::string::npos) return false;
    return true;
}

// Elements that are not rows draw nothing in filter mode (buttons inside a visible control row still draw)
bool Hidden() { return g_filter.active && !g_inControlRow; }

// ---- change reports ----
bool g_reportOn = true;
bool g_changePending = false;
std::string g_changeText;    // as shown (menu language)
std::string g_changeEnglish; // for the log

// ---- next row badge ----
const char* g_nextBadge = nullptr;
const char* g_nextBadgeTip = nullptr;

// ---- slider drag state ----
int g_sliderActiveFrame = -10;
bool g_keepActiveOpaque = false;

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Pushes a font at a multiple of the base size (1.92: the size is before the global scale factors); nullptr = the
// current font
void PushSized(ImFont* font, float factor) { ImGui::PushFont(font, VioletTheme::BaseFontSize() * factor); }

// End of the visible part of a label ("Text##id" shows "Text")
const char* VisibleEnd(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? hash : label + std::strlen(label);
}

float Spacing() { return ImGui::GetStyle().ItemSpacing.y; }

// Screen y -> window-local y (the space of GetCursorPosY / SetCursorPosY)
float LocalY(float screenY) {
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    return screenY - w->Pos.y + w->Scroll.y;
}

constexpr float kSwitchW = 32.0f, kSwitchH = 18.0f;

bool g_lastSliderCommitted = false;

// ---- row stacking ----
// Rows (switch, slider, segmented, control and overview rows) that follow each other directly are kRowGap apart with a
// hairline divider in the middle. Where the last row of this window ended, this frame (window-local y).
ImGuiID g_rowWindow = 0;
int g_rowFrame = -1;
float g_rowEndY = 0.0f;

bool FollowsRow(float y) {
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (g_rowWindow != w->ID || g_rowFrame != ImGui::GetFrameCount()) return false;
    return std::fabs(y - (g_rowEndY + Spacing())) <= 1.0f;
}

// A 1 px hairline across the content width at window-local y (x from the cursor's line start)
void HairlineAt(float localY) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float y = std::floor(w->Pos.y - w->Scroll.y + localY);
    const float x0 = w->Pos.x - w->Scroll.x + ImGui::GetCursorPosX();
    const float x1 = w->WorkRect.Max.x;
    w->DrawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 1.0f), U32(VioletTheme::kCardBorder));
}

// Where a row's content starts: kRowGap below the previous row (with the divider in the middle) when it follows one
// directly, else at the cursor
float RowTop() {
    const float y = ImGui::GetCursorPosY();
    if (!FollowsRow(y)) return y;
    const float gap = kRowGap * Unit();
    HairlineAt(g_rowEndY + gap * 0.5f);
    return g_rowEndY + gap;
}

// Ends a row whose content reaches `bottom` (window-local y): the next item starts right below it
void RowFinish(float startX, float bottom) {
    ImGui::SetCursorPos(ImVec2(startX, bottom));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    g_rowWindow = ImGui::GetCurrentWindow()->ID;
    g_rowFrame = ImGui::GetFrameCount();
    g_rowEndY = bottom;
}

// Anything that is not a row keeps `units` of space from a row right above it
void PadAfterRow(float units = kSpace2) {
    if (!FollowsRow(ImGui::GetCursorPosY())) return;
    ImGui::SetCursorPosY(g_rowEndY + units * Unit() - Spacing());
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// The description under a row's label: smaller, muted, wrapped at wrapX (window coordinates)
void RowDescription(const char* text, float wrapX) {
    if (!text || !*text) return;
    PushSized(nullptr, kSmallScale);
    ImGui::PushTextWrapPos(wrapX);
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
    ImGui::TextWrapped("%s", T(text));
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

// Where a row's label ended (screen coordinates): right end of its text, top of its first line, line height
struct LabelInfo {
    float right = 0.0f;
    float top = 0.0f;
    float lineH = 0.0f;
};

// Single-line action text is centred by its visible glyph bounds, including translated accents.
float CenteredTextY(std::string_view shown, float cy) {
    float inkTop = ImGui::GetFontSize(), inkBottom = 0.0f;
    ImFontBaked* baked = ImGui::GetFontBaked();
    const float fontScale = ImGui::GetFontSize() / baked->Size;
    for (const char* at = shown.data(), *end = at + shown.size(); at < end;) {
        unsigned codepoint = 0;
        const int bytes = ImTextCharFromUtf8(&codepoint, at, end);
        if (bytes <= 0) break;
        at += bytes;
        const ImFontGlyph* glyph = baked->FindGlyph(static_cast<ImWchar>(codepoint));
        if (glyph && glyph->Visible) {
            inkTop = std::fmin(inkTop, glyph->Y0 * fontScale);
            inkBottom = std::fmax(inkBottom, glyph->Y1 * fontScale);
        }
    }
    return std::round(inkBottom > inkTop ? cy - (inkTop + inkBottom) * 0.5f : cy - ImGui::GetTextLineHeight() * 0.5f);
}

// ---- text that may not fit (long translations: German compounds, Finnish, CJK without spaces) ----
// Single-line texts in a box of fixed width (tabs, segments, buttons, sidebar items, group labels, slider end labels)
// are drawn whole when they fit, which is always the case in English, so the English look does not change. A longer
// translation is cut at a character boundary and ends with "…"; the caller shows the full text as a tooltip.

// The widest a self-sized control may be here: the content region's free width, or no limit in a window that grows to
// fit its content (popups, tooltips, auto-resizing children), whose free width only reflects the previous frame's size
float FitLimit() {
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    if ((w->Flags & (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Tooltip)) || (w->ChildFlags & ImGuiChildFlags_AutoResizeX) || w->AutoFitFramesX > 0)
        return FLT_MAX;
    return ImGui::GetContentRegionAvail().x;
}

// The part of `text` to draw in maxW pixels with the current font: the text itself when it fits (returns false), else
// its longest prefix that fits with "…" appended, into `cut` (returns true)
bool FitText(std::string_view text, float maxW, std::string& cut) {
    const char* b = text.data();
    const char* e = b + text.size();
    if (ImGui::CalcTextSize(b, e).x <= maxW + 0.5f) return false;
    static constexpr const char kEllipsis[] = "\xE2\x80\xA6";
    ImFontBaked* baked = ImGui::GetFontBaked();
    const float scale = ImGui::GetFontSize() / baked->Size;
    const float room = maxW - ImGui::CalcTextSize(kEllipsis).x;
    float w = 0.0f;
    const char* end = b;
    for (const char* s = b; s < e;) {
        unsigned c = 0;
        const int n = ImTextCharFromUtf8(&c, s, e);
        if (n <= 0) break;
        w += baked->GetCharAdvance(static_cast<ImWchar>(c)) * scale;
        if (w > room) break;
        s += n;
        end = s;
    }
    while (end > b && end[-1] == ' ') --end; // "Lumière au…", not "Lumière au …"
    cut.assign(b, end);
    cut += kEllipsis;
    return true;
}

// Draws `text` at pos, fitted into maxW (see FitText); true when it was cut
bool DrawFitted(ImDrawList* dl, ImVec2 pos, ImU32 col, std::string_view text, float maxW) {
    std::string cut;
    if (!FitText(text, maxW, cut)) {
        dl->AddText(pos, col, text.data(), text.data() + text.size());
        return false;
    }
    dl->AddText(pos, col, cut.c_str());
    return true;
}

// The full text of a cut label, as a tooltip (appended to the item's own tooltip when it has one)
void FullTextTooltip(std::string_view text) {
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// For drawn text that is not an item: the full text while the mouse is over the cut text's box
void FullTextTooltipAt(ImVec2 pos, float width, std::string_view text) {
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + width, pos.y + ImGui::GetFontSize())))
        FullTextTooltip(text);
}

float RowText(const char* label, const char* description, float wrapX, float labelWrapX = -1.0f, LabelInfo* info = nullptr, float minimumHeight = 0.0f) {
    const float top = ImGui::GetCursorScreenPos().y;
    const std::string_view shown = g_rowRaw ? std::string_view(label, VisibleEnd(label) - label) : I18n::TrLabel(label);
    if (minimumHeight > 0.0f) {
        const float x = ImGui::GetCursorPosX();
        const float labelWidth = std::fmax(1.0f, (labelWrapX >= 0.0f ? labelWrapX : wrapX) - x);
        float height = ImGui::CalcTextSize(shown.data(), shown.data() + shown.size(), false, labelWidth).y;
        if (description && *description) {
            PushSized(nullptr, kSmallScale);
            height += 2.0f * Unit() + ImGui::CalcTextSize(T(description), nullptr, false, std::fmax(1.0f, wrapX - x)).y;
            ImGui::PopFont();
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::fmax(0.0f, (minimumHeight - height) * 0.5f));
    }
    ImGui::BeginGroup();
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * Unit());
    ImGui::PushTextWrapPos(labelWrapX >= 0.0f ? labelWrapX : wrapX);
    ImGui::TextWrapped("%.*s", static_cast<int>(shown.size()), shown.data());
    ImGui::PopTextWrapPos();
    if (info) {
        info->right = ImGui::GetItemRectMax().x;
        info->top = ImGui::GetItemRectMin().y;
        info->lineH = ImGui::GetTextLineHeight();
    }
    RowDescription(description, wrapX);
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    return ImGui::GetItemRectMax().y - top;
}

// Window-local position -> screen
ImVec2 ToScreen(float localX, float localY) {
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    return ImVec2(w->Pos.x - w->Scroll.x + localX, w->Pos.y - w->Scroll.y + localY);
}

// A change report from an English format ("{} turned on") and the row's visible label, both translated
void ReportLabel(const char* label, const char* format) {
    if (!g_reportOn) return;
    const std::string english(label, VisibleEnd(label));
    const std::string shown(g_rowRaw ? std::string_view(english) : I18n::TrLabel(label));
    g_changeText = I18n::Trf(format, shown);
    g_changeEnglish = std::vformat(format, std::make_format_args(english));
    g_changePending = true;
}

// The breadcrumb link above a matching row in the search results (and a hairline between results)
void FilterCrumb() {
    const float u = Unit();
    if (g_filter.drawn > 0) {
        ImGui::Dummy(ImVec2(0.0f, kSpace1 * u));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, std::floor(p.y)), ImVec2(p.x + ImGui::GetContentRegionAvail().x, std::floor(p.y) + 1.0f),
                                                  U32(VioletTheme::kCardBorder));
        ImGui::Dummy(ImVec2(0.0f, kSpace2 * u));
    }
    g_filter.drawn++;
    if (g_filter.crumb.empty()) return;
    PushSized(nullptr, kSmallScale);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(g_filter.crumb.c_str());
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 3.0f * u);
    ImGui::PushID("##Crumb");
    ImGui::PushID(g_filter.drawn);
    const bool clicked = ImGui::InvisibleButton("##Link", ImVec2(std::fmax(ts.x, 1.0f), ts.y), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopStyleVar();
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 col = hovered ? U32(VioletTheme::kAccentLight) : U32(VioletTheme::kTextMuted);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, col, g_filter.crumb.c_str());
    if (hovered) dl->AddLine(ImVec2(p.x, p.y + ts.y), ImVec2(p.x + ts.x, p.y + ts.y), col, 1.0f);
    ImGui::PopFont();
    Tooltip("Open this page");
    if (clicked) g_filter.clicked = g_filter.crumbId;
}

// Rows call it first: false = the search filter hides this row (draw nothing). A matching row gets its breadcrumb.
bool RowVisible(const char* label, const char* description) {
    g_rowRaw = g_nextRaw;
    g_nextRaw = false;
    if (!g_filter.active) return true;
    std::string hay;
    AppendLower(hay, label, VisibleEnd(label));
    hay.push_back(' ');
    AppendLower(hay, description);
    if (I18n::Current() != I18n::Lang::English) { // the search finds a row by its English or its shown text
        const std::string_view shown = g_rowRaw ? std::string_view(label, VisibleEnd(label) - label) : I18n::TrLabel(label);
        hay.push_back(' ');
        AppendLower(hay, shown.data(), shown.data() + shown.size());
        hay.push_back(' ');
        AppendLower(hay, T(description));
    }
    if (!WordsIn(g_filter.words, hay)) return false;
    FilterCrumb();
    return true;
}

// What a row shows after its label: the "Reload save" style badge, the changed dot and (on hover) its Reset button
struct RowDecor {
    const char* badge = nullptr;
    const char* badgeTip = nullptr;
    bool hasDefault = false;
    bool changed = false;
};

// Takes the pending SetNextRowBadge (every row consumes it, drawn or not)
RowDecor TakeDecor() {
    RowDecor d;
    d.badge = g_nextBadge;
    d.badgeTip = g_nextBadgeTip;
    g_nextBadge = g_nextBadgeTip = nullptr;
    return d;
}

constexpr float kResetButton = 18.0f; // units

// Room the decorations need after the label (pixels)
float DecorWidth(const RowDecor& d) {
    const float u = Unit();
    float w = 0.0f;
    if (d.badge) w += 6.0f * u + ChipSize(d.badge).x;
    if (d.hasDefault) w += 6.0f * u + 6.0f * u + 4.0f * u + kResetButton * u;
    return w;
}

bool IconButtonImpl(const char* id, IconId icon, const char* tooltip, bool active, float sizeUnits);
void ChipImpl(const char* text, unsigned rgb);

// Draws the decorations after the label (li) of a row spanning rowMin..rowMax (screen). True when Reset was clicked.
// Submits items at their own positions: call it before RowFinish (which puts the cursor back).
bool RowDecorations(const RowDecor& d, const LabelInfo& li, ImVec2 rowMin, ImVec2 rowMax) {
    if (!d.badge && !(d.hasDefault && d.changed)) return false;
    const float u = Unit();
    float x = li.right + 6.0f * u;
    const float cy = li.top + li.lineH * 0.5f;
    if (d.badge) {
        const ImVec2 cs = ChipSize(d.badge);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - cs.y * 0.5f));
        ChipImpl(d.badge, VioletTheme::kWarning);
        Tooltip(d.badgeTip);
        x += cs.x + 6.0f * u;
    }
    if (!(d.hasDefault && d.changed)) return false;
    const float r = 3.0f * u;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(x + r, cy), r, U32(VioletTheme::kAccent));
    ImGui::SetCursorScreenPos(ImVec2(x, cy - r));
    ImGui::Dummy(ImVec2(2.0f * r, 2.0f * r));
    Tooltip("Changed from the default");
    x += 2.0f * r + 4.0f * u;
    const bool rowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(rowMin, rowMax, false);
    if (!rowHovered) return false;
    const float s = kResetButton * u;
    ImGui::SetCursorScreenPos(ImVec2(x, cy - s * 0.5f));
    return IconButtonImpl("##ResetRow", IconId::RotateCcw, "Reset to default", false, kResetButton);
}

// Section labels use the regular menu font without artificial glyph tracking.
void SectionLabelText(const char* text, float indent, float extraBelow) {
    text = T(text);
    PushSized(VioletTheme::RegularFont(), 1.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float maxW = std::fmax(FitLimit() - indent, 1.0f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at(p.x + indent, p.y);
    if (DrawFitted(ImGui::GetWindowDrawList(), at, U32(VioletTheme::kTextMuted), text, maxW)) FullTextTooltipAt(at, maxW, text);
    ImGui::Dummy(ImVec2(indent + std::fmin(size.x, maxW), size.y + extraBelow));
    ImGui::PopFont();
}

// Control rows (BeginControlRow / EndControlRow)
float g_ctrlStartX = 0.0f;
float g_ctrlBottom = 0.0f;

} // namespace

float Unit() {
    const float s = ImGui::GetStyle().FontScaleMain;
    return s > 0.0f ? s : 1.0f;
}

// ---- badges, search filter, change reports, slider drag state ----

void SetNextRowUntranslated() { g_nextRaw = true; }

void SetNextRowBadge(const char* text, const char* tooltip) {
    g_nextBadge = text;
    g_nextBadgeTip = tooltip;
}

void BeginFilter(const char* query) {
    g_filter.active = true;
    SplitWords(query, g_filter.words);
    g_filter.crumb.clear();
    g_filter.crumbId = -1;
    g_filter.clicked = -1;
    g_filter.drawn = 0;
    g_inControlRow = false;
}

void SetFilterCrumb(const char* crumb, int id) {
    g_filter.crumb = crumb ? crumb : "";
    g_filter.crumbId = id;
}

int EndFilter(int* clickedCrumb) {
    if (clickedCrumb) *clickedCrumb = g_filter.clicked;
    const int drawn = g_filter.drawn;
    g_filter = FilterState{};
    g_inControlRow = false;
    return drawn;
}

bool FilterActive() { return g_filter.active; }

bool MatchesQuery(const char* query, const char* text) {
    std::vector<std::string> words;
    SplitWords(query, words);
    std::string hay;
    AppendLower(hay, text);
    return WordsIn(words, hay);
}

void ReportChange(const char* text) {
    if (!g_reportOn || !text || !*text) return;
    g_changeText = T(text);
    g_changeEnglish = text;
    g_changePending = true;
}

void SetChangeReporting(bool on) { g_reportOn = on; }

bool TakeChange(std::string& text, std::string* english) {
    if (!g_changePending) return false;
    g_changePending = false;
    text = g_changeText;
    if (english) *english = g_changeEnglish;
    return true;
}

bool SliderDragging() { return g_sliderActiveFrame >= ImGui::GetFrameCount() - 1; }

void SetKeepActiveSliderOpaque(bool on) { g_keepActiveOpaque = on; }

// ---- text ----

void Tooltip(const char* text) {
    if (!text || !*text || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) return;
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(T(text));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void PageTitle(const char* title, const char* subtitle) {
    if (Hidden()) return;
    const float u = Unit();
    // The spacing below an item is the one in effect when it is submitted: tight under the title only
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
    PushSized(VioletTheme::BoldFont(), kPageTitleScale);
    ImGui::TextWrapped("%s", T(title)); // a long translated title wraps instead of running past the page
    ImGui::PopFont();
    ImGui::PopStyleVar();
    if (subtitle && *subtitle) MutedText(subtitle);
    ImGui::Dummy(ImVec2(0.0f, std::fmax(0.0f, kSpace3 * u - 2.0f * Spacing())));
}

void MutedText(const char* text) {
    if (Hidden()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
    ImGui::TextWrapped("%s", T(text));
    ImGui::PopStyleColor();
}

void SectionLabel(const char* text) {
    if (Hidden()) return;
    PushSized(VioletTheme::BoldFont(), 1.0f);
    ImGui::TextWrapped("%s", T(text));
    ImGui::PopFont();
}

void GroupLabel(const char* text) {
    if (Hidden()) return;
    PadAfterRow(kSpace3);
    SectionLabelText(text, 0.0f, 3.0f * Unit());
}

void IconNote(IconId icon, const char* text, unsigned rgb) {
    if (Hidden()) return;
    text = T(text);
    PadAfterRow();
    const float u = Unit();
    const float s = kIconSmall * u;
    const float padX = 10.0f * u, padY = 6.0f * u, gap = kSpace2 * u;
    const float startX = ImGui::GetCursorPosX(), startY = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    const float textW = std::fmax(width - 2.0f * padX - s - gap, 20.0f * u);
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, textW);
    const float h = std::fmax(ts.y, lineH) + 2.0f * padY;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), U32(rgb, 0.10f), 6.0f * u);
    DrawIcon(dl, icon, ImVec2(p.x + padX, p.y + padY + (lineH - s) * 0.5f), s, U32(rgb));
    ImGui::SetCursorPos(ImVec2(startX + padX + s + gap, startY + padY));
    ImGui::PushTextWrapPos(startX + width - padX);
    ImGui::PushStyleColor(ImGuiCol_Text, Col(rgb));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    // The whole box is the item (hover it for a tooltip)
    ImGui::SetCursorPos(ImVec2(startX, startY));
    ImGui::Dummy(ImVec2(width, h));
}

void Gap(float units) {
    if (Hidden()) return;
    ImGui::Dummy(ImVec2(0.0f, units * Unit()));
}

// ---- controls ----

ImVec2 ToggleSwitchSize() { return ImVec2(kSwitchW * Unit(), kSwitchH * Unit()); }

bool ToggleSwitch(const char* id, bool* v) {
    const float u = Unit();
    const ImVec2 size(kSwitchW * u, kSwitchH * u);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    if (clicked) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();

    // Knob position eases towards its side (about 0.1 s); kept per switch in the window's state storage
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetItemID();
    const float target = *v ? 1.0f : 0.0f;
    float t = storage->GetFloat(key, target);
    const float step = ImGui::GetIO().DeltaTime * 10.0f;
    t = t < target ? std::fmin(target, t + step) : std::fmax(target, t - step);
    storage->SetFloat(key, t);

    ImVec4 track = Mix(Col(VioletTheme::kToggleOff), Col(VioletTheme::kAccent), t);
    if (hovered) track = Mix(track, Col(0xFFFFFF), 0.08f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(track), r);
    const float knobR = r - 2.5f * u;
    const ImVec2 knob(p.x + r + (size.x - 2.0f * r) * t, p.y + r);
    dl->AddCircleFilled(knob, knobR, U32(0xFFFFFF));
    ImGui::RenderNavCursor(ImGui::GetCurrentContext()->LastItemData.Rect, ImGui::GetItemID());
    return clicked;
}

bool SwitchRow(const char* label, bool* v, const char* tooltip, BoolDefault def, IconId icon) {
    RowDecor d = TakeDecor();
    if (!RowVisible(label, tooltip)) return false;
    d.hasDefault = def.value >= 0;
    d.changed = d.hasDefault && (*v != (def.value == 1));
    const float u = Unit();
    const ImVec2 sw(kSwitchW * u, kSwitchH * u);
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop();

    // Label (after the optional icon) with its description below, both wrapped short of the switch
    float textX = startX;
    if (icon != IconId::None) {
        const float is = kIconSmall * u;
        const ImGuiWindow* w = ImGui::GetCurrentWindow();
        DrawIcon(ImGui::GetWindowDrawList(), icon,
                 ImVec2(w->Pos.x - w->Scroll.x + startX, w->Pos.y - w->Scroll.y + top + (ImGui::GetTextLineHeight() - is) * 0.5f), is,
                 U32(VioletTheme::kAccent));
        textX = startX + is + kSpace2 * u;
    }
    const float wrapX = startX + width - sw.x - kSpace3 * u;
    ImGui::SetCursorPos(ImVec2(textX, top));
    LabelInfo li;
    const float rowH = std::fmax(RowText(label, tooltip, wrapX, std::fmax(wrapX - DecorWidth(d), textX + 40.0f * u), &li), sw.y);

    // The switch, centred on the text block
    ImGui::SetCursorPos(ImVec2(startX + width - sw.x, top + (rowH - sw.y) * 0.5f));
    ImGui::PushID(label);
    bool clicked = ToggleSwitch("##Switch", v);
    const ImVec2 rowMin = ToScreen(startX, top);
    if (RowDecorations(d, li, rowMin, ImVec2(rowMin.x + width, rowMin.y + rowH))) {
        *v = def.value == 1;
        clicked = true;
        ReportLabel(label, "{} reset");
    } else if (clicked) {
        ReportLabel(label, *v ? "{} turned on" : "{} turned off");
    }
    ImGui::PopID();

    RowFinish(startX, top + rowH);
    return clicked;
}

bool BeginControlRow(const char* label, const char* description, float controlsWidth, IconId icon, float controlsHeight) {
    if (!RowVisible(label, description)) return false;
    g_inControlRow = true;
    const float u = Unit();
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop();
    const float frameH = controlsHeight > 0.0f ? controlsHeight : ImGui::GetFrameHeight();
    const float minimumHeight = ImGui::GetFrameHeight();
    const bool stacked = width - controlsWidth - kSpace3 * u < 120.0f * u;
    const float wrapX = stacked ? startX + width : startX + width - controlsWidth - kSpace3 * u;
    // Keep the divider and right cluster anchored to the full row; only the text is indented.
    const bool hasIcon = icon != IconId::None;
    const float iconBox = 22.0f * u;
    const float textX = startX + (hasIcon ? iconBox + kSpace3 * u : 0.0f);
    ImGui::SetCursorPos(ImVec2(textX, top));
    const float rowH = std::fmax(RowText(label, description, std::fmax(wrapX, textX + 40.0f * u), -1.0f, nullptr, stacked ? 0.0f : minimumHeight), stacked ? 0.0f : minimumHeight);
    if (hasIcon) {
        const float size = kIconMedium * u;
        DrawIcon(ImGui::GetWindowDrawList(), icon,
                 ToScreen(startX + (iconBox - size) * 0.5f, top + (rowH - size) * 0.5f), size, U32(VioletTheme::kAccent));
    }
    g_ctrlStartX = startX;
    const float controlsTop = stacked ? top + rowH + kSpace2 * u : top + (rowH - frameH) * 0.5f;
    g_ctrlBottom = stacked ? controlsTop + frameH : top + rowH;
    ImGui::SetCursorPos(ImVec2(startX + std::fmax(0.0f, width - controlsWidth), controlsTop));
    return true;
}

void EndControlRow() {
    g_inControlRow = false;
    RowFinish(g_ctrlStartX, g_ctrlBottom);
}

namespace {
constexpr float kChipPadX = 6.0f; // units
}

// A chip's text is at most this many em wide (far above any English chip); a longer translation is cut with "…"
// so a badge cannot push a row's label or a card's title into a sliver
constexpr float kChipMaxEm = 16.0f;

ImVec2 ChipSize(const char* text) {
    const float u = Unit();
    PushSized(nullptr, kGroupScale);
    ImVec2 ts = ImGui::CalcTextSize(T(text));
    ts.x = std::fmin(ts.x, kChipMaxEm * ImGui::GetFontSize());
    const float h = ImGui::GetFontSize() + 4.0f * u;
    ImGui::PopFont();
    return ImVec2(ts.x + 2.0f * kChipPadX * u, h);
}

namespace {
void ChipImpl(const char* text, unsigned rgb) {
    const float u = Unit();
    const ImVec2 size = ChipSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(rgb, 0.14f), size.y * 0.5f);
    PushSized(nullptr, kGroupScale);
    if (DrawFitted(dl, ImVec2(p.x + kChipPadX * u, p.y + (size.y - ImGui::GetFontSize()) * 0.5f), U32(rgb), T(text), size.x - 2.0f * kChipPadX * u)) {
        ImGui::PopFont();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) FullTextTooltip(T(text));
        return;
    }
    ImGui::PopFont();
}
} // namespace

void Chip(const char* text, unsigned rgb) {
    if (Hidden()) return;
    ChipImpl(text, rgb);
}

bool CostChipText(float ms, char* buf, int size) {
    if (!buf || size <= 0) return false;
    buf[0] = '\0';
    if (!(ms >= 0.0f)) return false;
    if (ms < 0.1f) std::snprintf(buf, static_cast<size_t>(size), "~%.2f ms", ms);
    else std::snprintf(buf, static_cast<size_t>(size), "~%.1f ms", ms);
    return true;
}

ImVec2 PillSize(const char* text, bool withIcon) {
    const float u = Unit();
    PushSized(nullptr, kSmallScale);
    float w = std::fmin(ImGui::CalcTextSize(T(text)).x, kChipMaxEm * ImGui::GetFontSize()); // same cap as chips
    if (withIcon) w += kIconSmall * u + kSpace1 * u;
    const float h = ImGui::GetFontSize() + 6.0f * u;
    ImGui::PopFont();
    return ImVec2(w + 2.0f * 9.0f * u, h);
}

void Pill(const char* text, bool highlighted, IconId icon) {
    if (Hidden()) return;
    const float u = Unit();
    const bool withIcon = icon != IconId::None;
    const ImVec2 size = PillSize(text, withIcon);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), highlighted ? U32(VioletTheme::kAccentDark) : U32(VioletTheme::kSelectedBg), size.y * 0.5f);
    const ImU32 textCol = highlighted ? U32(VioletTheme::kAccentLight) : U32(VioletTheme::kTextMuted);
    PushSized(nullptr, kSmallScale);
    const float y = p.y + (size.y - ImGui::GetFontSize()) * 0.5f;
    float x = p.x + 9.0f * u;
    if (withIcon) {
        const float s = kIconSmall * u;
        DrawIcon(dl, icon, ImVec2(x, p.y + (size.y - s) * 0.5f), s, textCol);
        x += s + kSpace1 * u;
    }
    const bool fitted = DrawFitted(dl, ImVec2(x, y), textCol, T(text), p.x + size.x - 9.0f * u - x);
    ImGui::PopFont();
    if (fitted && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) FullTextTooltip(T(text));
}

bool IconButton(const char* id, IconId icon, const char* tooltip, bool active, float sizeUnits) {
    if (Hidden()) return false;
    return IconButtonImpl(id, icon, tooltip, active, sizeUnits);
}

namespace {
bool IconButtonImpl(const char* id, IconId icon, const char* tooltip, bool active, float sizeUnits) {
    const float u = Unit();
    const float s = sizeUnits * u;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // The small in-row Reset buttons (under 20 units) appear on hover only: kept out of keyboard navigation
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s), sizeUnits >= 20.0f ? ImGuiButtonFlags_EnableNav : ImGuiButtonFlags_None);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    Tooltip(tooltip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || held) dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), U32(held ? VioletTheme::kToggleOff : VioletTheme::kSelectedBg), 6.0f * u);
    const ImU32 col = active ? U32(VioletTheme::kAccent) : hovered ? U32(VioletTheme::kText) : U32(VioletTheme::kTextMuted);
    const float is = (sizeUnits >= 24.0f ? kIconMedium : kIconSmall) * u;
    DrawIcon(dl, icon, ImVec2(p.x + (s - is) * 0.5f, p.y + (s - is) * 0.5f), is, col);
    ImGui::RenderNavCursor(ImGui::GetCurrentContext()->LastItemData.Rect, ImGui::GetItemID());
    return clicked;
}
} // namespace

ControlSizeScope::ControlSizeScope(ControlSize size) {
    const float height = (size == ControlSize::Primary ? VioletTheme::kControlPrimary : VioletTheme::kControlCompact) * Unit();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2((size == ControlSize::Primary ? VioletTheme::kPrimaryPadding : VioletTheme::kControlPadding) * Unit(), std::fmax(0.0f, (height - ImGui::GetFontSize()) * 0.5f)));
}

ControlSizeScope::~ControlSizeScope() { ImGui::PopStyleVar(); }

bool Checkbox(const char* label, bool* value) {
    const float padding = std::fmax(0.0f, (VioletTheme::kCheckboxSize * Unit() - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, padding));
    const bool changed = ImGui::Checkbox(label, value);
    ImGui::PopStyleVar();
    return changed;
}

bool DiagnosticIntRow(const char* label, int* value, int min, int max, const char* description, ImGuiSliderFlags flags) {
    const float width = 180.0f * Unit();
    if (!BeginControlRow(label, description, width)) return false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::SliderInt("##Value", value, min, max, "%d", flags);
    ImGui::PopID();
    EndControlRow();
    return changed;
}

float ButtonWidth(const char* label, bool withIcon, float minWidth) {
    const float u = Unit();
    const std::string_view shown = I18n::TrLabel(label);
    float w = 2.0f * ImGui::GetStyle().FramePadding.x + ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x;
    if (withIcon) w += VioletTheme::kControlIcon * u + VioletTheme::kControlIconGap * u;
    return std::fmax(w, minWidth);
}

namespace {
bool DrawButton(const char* label, IconId icon, const char* tooltip, ButtonKind kind, float minWidth) {
    if (Hidden()) return false;
    PadAfterRow();
    const float u = Unit();
    const bool withIcon = icon != IconId::None;
    std::string_view shown = I18n::TrLabel(label);
    const float is = VioletTheme::kControlIcon * u, gap = VioletTheme::kControlIconGap * u;
    const float padX = ImGui::GetStyle().FramePadding.x;
    // A button never runs past the edge of its window: a translation wider than the free space is cut with "…"
    // (callers lay buttons out with ButtonWidth, so this only happens when even the stacked control row is too narrow)
    const float limit = FitLimit();
    const float natural = ButtonWidth(label, withIcon, minWidth);
    const float minimum = 2.0f * padX + (withIcon ? is + gap : 0.0f) + ImGui::GetFontSize() * 2.0f;
    const ImVec2 size(natural > limit && limit >= minimum ? limit : natural, ImGui::GetFrameHeight());
    std::string cut;
    const bool fitted = size.x < natural && FitText(shown, size.x - 2.0f * padX - (withIcon ? is + gap : 0.0f), cut);
    const std::string_view full = shown;
    if (fitted) shown = cut;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##Button", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    if (fitted && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) FullTextTooltip(full);
    Tooltip(tooltip);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const float r = ImGui::GetStyle().FrameRounding;
    ImU32 textCol, iconCol;
    if (kind == ButtonKind::Primary) {
        const ImVec4 fill = held ? Mix(Col(VioletTheme::kAccentDark), Col(VioletTheme::kAccent), 0.5f) : hovered ? Col(VioletTheme::kAccent) : Col(VioletTheme::kAccentDark);
        dl->AddRectFilled(p, q, ImGui::GetColorU32(fill), r);
        textCol = U32(0xFFFFFF);
        iconCol = U32(0xFFFFFF);
    } else {
        const ImVec4 fill = held ? Col(VioletTheme::kAccentDark, 0.35f) : hovered ? Col(0x2A2B33) : Col(VioletTheme::kSelectedBg);
        dl->AddRectFilled(p, q, ImGui::GetColorU32(fill), r);
        dl->AddRect(p, q, hovered || held ? U32(VioletTheme::kAccent) : U32(kButtonBorder), r, 0, 1.0f);
        textCol = U32(VioletTheme::kText);
        iconCol = U32(VioletTheme::kAccent);
    }
    const ImVec2 ts = ImGui::CalcTextSize(shown.data(), shown.data() + shown.size());
    const float contentW = ts.x + (withIcon ? is + gap : 0.0f);
    float x = p.x + (size.x - contentW) * 0.5f;
    const float cy = p.y + size.y * 0.5f;
    if (withIcon) {
        DrawIcon(dl, icon, ImVec2(x, cy - is * 0.5f), is, iconCol);
        x += is + gap;
    }
    const float textY = CenteredTextY(shown, cy);
    dl->AddText(ImVec2(x, std::round(textY)), textCol, shown.data(), shown.data() + shown.size());
    ImGui::RenderNavCursor(ImGui::GetCurrentContext()->LastItemData.Rect, ImGui::GetItemID());
    return clicked;
}
} // namespace

bool IconTextButton(const char* label, IconId icon, const char* tooltip, ButtonKind kind) { return DrawButton(label, icon, tooltip, kind, 0.0f); }

bool TextButton(const char* label, const char* tooltip, ButtonKind kind, float minWidth) { return DrawButton(label, IconId::None, tooltip, kind, minWidth); }

bool SidebarItem(IconId icon, const char* label, bool selected, bool collapsed) {
    const float u = Unit();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 32.0f * u);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##SidebarItem", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    if (collapsed) {
        // Icon-only rail: the label is the tooltip
        Tooltip(label);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 q(p.x + size.x, p.y + size.y);
        const float rounding = kSpace2 * u;
        if (selected) {
            dl->AddRectFilled(p, q, U32(VioletTheme::kSelectedBg), rounding);
            dl->AddRectFilled(ImVec2(p.x, p.y + kSpace2 * u), ImVec2(p.x + 3.0f * u, q.y - kSpace2 * u), U32(VioletTheme::kAccent), 1.5f * u);
        } else if (hovered) {
            dl->AddRectFilled(p, q, U32(VioletTheme::kHoverBg), rounding);
        }
        if (icon != IconId::None) {
            const float s = kIconMedium * u;
            const ImU32 col = selected ? U32(VioletTheme::kAccent) : hovered ? U32(VioletTheme::kText) : U32(VioletTheme::kTextMuted);
            DrawIcon(dl, icon, ImVec2(p.x + (size.x - s) * 0.5f, p.y + (size.y - s) * 0.5f), s, col);
        }
        return clicked;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const float rounding = kSpace2 * u;
    if (selected) {
        dl->AddRectFilled(p, q, U32(VioletTheme::kSelectedBg), rounding);
        dl->AddRectFilled(ImVec2(p.x, p.y + kSpace2 * u), ImVec2(p.x + 3.0f * u, q.y - kSpace2 * u), U32(VioletTheme::kAccent), 1.5f * u);
    } else if (hovered) {
        dl->AddRectFilled(p, q, U32(VioletTheme::kHoverBg), rounding);
    }
    const ImU32 textCol = selected ? U32(0xFFFFFF) : hovered ? U32(VioletTheme::kText) : U32(VioletTheme::kTextMuted);
    float x = p.x + kSpace3 * u;
    if (icon != IconId::None) {
        const float s = kIconMedium * u;
        DrawIcon(dl, icon, ImVec2(x, p.y + (size.y - s) * 0.5f), s, selected ? U32(VioletTheme::kAccent) : textCol);
        x += s + 10.0f * u;
    }
    // A label longer than the sidebar is cut with "…" (full name on hover) instead of running into the page
    const char* shown = T(label);
    if (DrawFitted(dl, ImVec2(x, p.y + (size.y - ImGui::GetFontSize()) * 0.5f), textCol, shown, q.x - kSpace2 * u - x) &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        FullTextTooltip(shown);
    return clicked;
}

void SidebarGroup(const char* text, bool collapsed) {
    const float u = Unit();
    ImGui::Dummy(ImVec2(0.0f, std::fmax(0.0f, kSpace3 * u - 2.0f * Spacing())));
    if (collapsed) {
        // A short hairline where the group label would be
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float y = std::floor(p.y + 3.0f * u);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + w * 0.25f, y), ImVec2(p.x + w * 0.75f, y + 1.0f), U32(VioletTheme::kCardBorder));
        ImGui::Dummy(ImVec2(w, 6.0f * u));
        return;
    }
    SectionLabelText(text, kSpace3 * u, 2.0f * u);
}

bool TabBar(const char* id, int* current, const char* const* labels, int count, const IconId* icons) {
    if (count <= 0 || Hidden()) return false;
    const float u = Unit();
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float padX = 10.0f * u;
    const float h = ImGui::GetTextLineHeight() + kSpace3 * u;
    const float is = kIconSmall * u, iconGap = 6.0f * u;
    const float sp = Spacing();
    auto hasIcon = [&](int i) { return icons && icons[i] != IconId::None; };

    // Layout first (which tabs share a line), so the hairlines go under the underlines
    std::vector<float> widths(static_cast<size_t>(count));
    std::vector<bool> sameLine(static_cast<size_t>(count), false);
    int lines = 1;
    float x = 0.0f;
    for (int i = 0; i < count; i++) {
        // A tab wider than the whole line (a long translation) takes the line and cuts its label
        widths[i] = std::fmin(ImGui::CalcTextSize(T(labels[i])).x + 2.0f * padX + (hasIcon(i) ? is + iconGap : 0.0f), std::fmax(avail, 1.0f));
        if (i > 0 && x + widths[i] <= avail + 0.5f) {
            sameLine[i] = true;
            x += widths[i];
        } else {
            if (i > 0) lines++;
            x = widths[i];
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int l = 0; l < lines; l++) {
        const float y = std::floor(origin.y + l * (h + sp) + h - 1.0f);
        dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + avail, y + 1.0f), U32(VioletTheme::kCardBorder));
    }

    bool changed = false;
    for (int i = 0; i < count; i++) {
        if (sameLine[i]) ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##Tab", ImVec2(widths[i], h), ImGuiButtonFlags_EnableNav);
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (clicked && *current != i) {
            *current = i;
            changed = true;
        }
        const bool selected = *current == i;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImU32 col = selected ? U32(0xFFFFFF) : hovered ? U32(VioletTheme::kText) : U32(VioletTheme::kTextMuted);
        const float ty = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f - 1.0f * u;
        float tx = a.x + padX;
        if (hasIcon(i)) {
            DrawIcon(dl, icons[i], ImVec2(tx, ty + (ImGui::GetTextLineHeight() - is) * 0.5f), is, selected ? U32(VioletTheme::kAccent) : col);
            tx += is + iconGap;
        }
        if (DrawFitted(dl, ImVec2(tx, ty), col, T(labels[i]), b.x - padX - tx) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) FullTextTooltip(T(labels[i]));
        if (selected) dl->AddRectFilled(ImVec2(a.x + 4.0f * u, b.y - 2.0f * u), ImVec2(b.x - 4.0f * u, b.y), U32(VioletTheme::kAccent), 1.0f * u);
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, std::fmax(0.0f, kSpace3 * u - 2.0f * sp)));
    return changed;
}

bool Segmented(const char* id, int* current, const char* const* labels, int count, const char* const* tooltips, bool compact, const IconId* icons) {
    if (count <= 0) return false;
    const float u = Unit();
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float h = ImGui::GetFrameHeight();
    const float is = VioletTheme::kControlIcon * u, iconGap = VioletTheme::kControlIconGap * u;
    auto hasIcon = [&](int i) { return icons && icons[i] != IconId::None; };
    std::vector<float> widths(static_cast<size_t>(count));
    float natural = 0.0f;
    for (int i = 0; i < count; i++) {
        widths[i] = ImGui::CalcTextSize(T(labels[i])).x + 2.0f * padX + (hasIcon(i) ? is + iconGap : 0.0f);
        natural += widths[i];
        if (compact) widths[i] = std::fmin(widths[i], std::fmax(avail, 1.0f)); // a pill never wider than the line
    }
    const bool vertical = !compact && count > 1 && natural > avail + 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;

    auto segment = [&](int i, ImVec2 size, float rounding) {
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##Segment", size, ImGuiButtonFlags_EnableNav);
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (tooltips && tooltips[i]) Tooltip(tooltips[i]);
        if (clicked && *current != i) {
            *current = i;
            changed = true;
        }
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool selected = *current == i;
        const float inset = vertical ? 0.0f : 2.0f * u;
        if (selected) dl->AddRectFilled(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), U32(VioletTheme::kAccentDark), rounding);
        else if (hovered) dl->AddRectFilled(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), U32(VioletTheme::kHoverBg), rounding);
        const ImU32 textCol = selected ? U32(VioletTheme::kAccentLight) : hovered ? U32(VioletTheme::kText) : U32(VioletTheme::kTextMuted);
        // A label longer than its segment (only possible one per line or as a lone pill wider than the line) is cut
        // with "…" and shown whole on hover
        std::string_view shown = T(labels[i]);
        std::string cut;
        const bool fitted = FitText(shown, b.x - a.x - 2.0f * padX - (hasIcon(i) ? is + iconGap : 0.0f), cut);
        if (fitted) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) FullTextTooltip(shown);
            shown = cut;
        }
        const ImVec2 ts = ImGui::CalcTextSize(shown.data(), shown.data() + shown.size());
        const float contentW = ts.x + (hasIcon(i) ? is + iconGap : 0.0f);
        float tx = vertical ? a.x + padX : a.x + (b.x - a.x - contentW) * 0.5f;
        if (hasIcon(i)) {
            DrawIcon(dl, icons[i], ImVec2(tx, a.y + (b.y - a.y - is) * 0.5f), is, textCol);
            tx += is + iconGap;
        }
        dl->AddText(ImVec2(tx, CenteredTextY(shown, (a.y + b.y) * 0.5f)), textCol, shown.data(), shown.data() + shown.size());
    };

    if (compact) {
        // Pills at their own width, flowing onto the next row when the line is full
        const float gap = 3.0f * u;
        float lineW = 0.0f;
        for (int i = 0; i < count; i++) {
            if (i > 0 && lineW + gap + widths[i] <= avail) {
                ImGui::SameLine(0.0f, gap);
                lineW += gap + widths[i];
            } else {
                lineW = widths[i];
            }
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + widths[i], p.y + h), U32(kFrameBg), h * 0.5f);
            segment(i, ImVec2(widths[i], h), h * 0.5f);
        }
    } else if (!vertical) {
        // One row on a rounded track; the segments share the whole width
        const float extra = (avail - natural) / static_cast<float>(count);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + h), U32(kFrameBg), 6.0f * u);
        for (int i = 0; i < count; i++) {
            if (i > 0) ImGui::SameLine(0.0f, 0.0f);
            segment(i, ImVec2(widths[i] + extra, h), 5.0f * u);
        }
    } else {
        // Too wide for one row: one choice per line
        ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
        for (int i = 0; i < count; i++) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + avail, p.y + h), U32(kFrameBg), 5.0f * u);
            segment(i, ImVec2(avail, h), 5.0f * u);
        }
        ImGui::PopStyleVar();
    }
    ImGui::PopID();
    return changed;
}

bool SegmentedRow(const char* label, const char* description, const char* id, int* current, const char* const* labels, int count,
                  const char* const* tooltips, const IconId* icons, int defaultIndex) {
    RowDecor d = TakeDecor();
    if (!RowVisible(label, description)) return false;
    d.hasDefault = defaultIndex != kNoDefaultIndex;
    d.changed = d.hasDefault && *current != defaultIndex;
    const float u = Unit();
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop();
    ImGui::SetCursorPos(ImVec2(startX, top));
    LabelInfo li;
    RowText(label, description, startX + width, std::fmax(startX + width - DecorWidth(d), startX + 40.0f * u), &li);
    ImGui::PushID(label);
    bool changed = Segmented(id, current, labels, count, tooltips, false, icons);
    const float bottom = LocalY(ImGui::GetItemRectMax().y);
    const ImVec2 rowMin = ToScreen(startX, top);
    if (RowDecorations(d, li, rowMin, ImVec2(rowMin.x + width, ToScreen(startX, bottom).y))) {
        *current = defaultIndex;
        changed = true;
        ReportLabel(label, "{} reset");
    } else if (changed) {
        ReportLabel(label, "{} changed");
    }
    ImGui::PopID();
    RowFinish(startX, bottom);
    return changed;
}

namespace {
bool g_selectOpen = false; // SelectRowOpen
}

bool SelectRowOpen() { return g_selectOpen; }

bool SelectRow(const char* label, const char* description, const char* id, int* current, const char* const* labels, int count,
               float controlWidth, int defaultIndex) {
    g_selectOpen = false;
    RowDecor d = TakeDecor();
    if (!RowVisible(label, description)) return false;
    if (!current || !labels || count <= 0) return false;
    *current = std::clamp(*current, 0, count - 1);
    d.hasDefault = defaultIndex != kNoDefaultIndex;
    d.changed = d.hasDefault && *current != defaultIndex;
    const float u = Unit();
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop();
    const float comboWidth = std::fmin(controlWidth * u, std::fmax(120.0f * u, width * 0.5f));
    const float wrapX = startX + width - comboWidth - kSpace3 * u;
    ImGui::SetCursorPos(ImVec2(startX, top));
    LabelInfo li;
    const float rowH = std::fmax(RowText(label, description, wrapX, std::fmax(wrapX - DecorWidth(d), startX + 40.0f * u), &li), ImGui::GetFrameHeight());
    const float comboY = top + (rowH - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorPos(ImVec2(startX + width - comboWidth, comboY));
    ImGui::SetNextItemWidth(comboWidth);
    const ImVec2 comboPos = ImGui::GetCursorScreenPos();
    const float comboHeight = ImGui::GetFrameHeight();
    ImDrawList* const rowDrawList = ImGui::GetWindowDrawList();
    ImGui::PushID(label);
    bool changed = false;
    // The chosen item's text stops short of the chevron; a longer translation is cut with "…" (ImGui would only clip
    // it under the chevron) and shown whole on hover
    const float arrowSize = 12.0f * u;
    const char* preview = I18n::Tr(labels[*current]);
    std::string cut;
    const bool fitted = FitText(preview, comboWidth - ImGui::GetStyle().FramePadding.x - 9.0f * u - arrowSize - kSpace1 * u, cut);
    // The list grows to its longest entry (and scrolls past eight); its width is at least the closed control's
    const bool comboOpen = ImGui::BeginCombo(id, fitted ? cut.c_str() : preview, ImGuiComboFlags_NoArrowButton);
    g_selectOpen = comboOpen;
    const bool comboHovered = ImGui::IsItemHovered();
    if (fitted && !comboOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) FullTextTooltip(preview);
    DrawIcon(rowDrawList, IconId::ChevronDown,
             ImVec2(comboPos.x + comboWidth - 9.0f * u - arrowSize, comboPos.y + (comboHeight - arrowSize) * 0.5f),
             arrowSize, U32(comboOpen || comboHovered ? VioletTheme::kAccent : VioletTheme::kTextMuted));
    if (comboOpen) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(I18n::Tr(labels[i]), *current == i)) {
                changed = *current != i;
                *current = i;
            }
            if (*current == i) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    d.changed = d.hasDefault && *current != defaultIndex;
    const ImVec2 rowMin = ToScreen(startX, top);
    if (RowDecorations(d, li, rowMin, ImVec2(rowMin.x + width, rowMin.y + rowH))) {
        *current = defaultIndex;
        changed = true;
        ReportLabel(label, "{} reset");
    } else if (changed) {
        ReportLabel(label, "{} changed");
    }
    ImGui::PopID();
    RowFinish(startX, top + rowH);
    return changed;
}

namespace {
ImGuiID g_dividerWindow = 0;
int g_dividerFrame = -1;
float g_dividerBodyY = 0.0f;
}
bool BeginAdvanced(const char* id, const char* label) {
    if (Hidden()) return true; // search: the contents are searched too, without the header
    const float u = Unit();
    ImGui::PushID(id);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("##AdvancedOpen");
    bool open = storage->GetBool(key, false); // collapsed by default

    // A hairline above: the row divider when it follows a row, else kSpace2 above and below it
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    float top;
    if (g_dividerFrame == ImGui::GetFrameCount() && g_dividerWindow == ImGui::GetCurrentWindow()->ID && std::fabs(ImGui::GetCursorPosY() - g_dividerBodyY) < 0.5f) {
        top = ImGui::GetCursorPosY(); // the card header already supplied the divider
    } else if (FollowsRow(ImGui::GetCursorPosY())) {
        top = RowTop();
    } else {
        const float lineY = ImGui::GetCursorPosY() - Spacing() + kSpace2 * u;
        HairlineAt(lineY);
        top = lineY + 1.0f + kSpace2 * u;
    }
    ImGui::SetCursorPos(ImVec2(startX, top));

    const float is = kIconSmall * u, gap = 6.0f * u;
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##Advanced", ImVec2(width, lineH), ImGuiButtonFlags_EnableNav)) {
        open = !open;
        storage->SetBool(key, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = U32(hovered ? VioletTheme::kAccentLight : VioletTheme::kAccent);
    DrawIcon(dl, open ? IconId::ChevronDown : IconId::ChevronRight, ImVec2(p.x, p.y + (lineH - is) * 0.5f), is, col);
    const std::string_view shown = I18n::TrLabel(label);
    if (DrawFitted(dl, ImVec2(p.x + is + gap, CenteredTextY(shown, p.y + lineH * 0.5f)), col, shown, width - is - gap) &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        FullTextTooltip(shown);
    ImGui::RenderNavCursor(ImGui::GetCurrentContext()->LastItemData.Rect, ImGui::GetItemID());
    return open;
}

void EndAdvanced() {} // the contents are not indented: nothing to undo

bool AdvancedNode(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kAccent));
    const bool open = ImGui::TreeNode(label);
    ImGui::PopStyleColor();
    return open;
}

bool Slider(const char* label, float* v, float min, float max, const SliderOptions& o) {
    RowDecor d = TakeDecor();
    if (!RowVisible(label, o.tooltip)) return false;
    d.hasDefault = !std::isnan(o.defaultValue);
    d.changed = d.hasDefault && std::fabs(*v - o.defaultValue) > std::fmax(std::fabs(max - min) * 1e-4f, 1e-6f);
    const float u = Unit();
    ImGui::PushID(label);
    // While the menu fades for a drag, the dragged slider's row stays fully opaque
    const bool opaque = g_keepActiveOpaque && ImGui::GetActiveID() == ImGui::GetID("##Slider");
    if (opaque) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop();
    ImGui::SetCursorPos(ImVec2(startX, top));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 lineP = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();

    // The value (muted, right-aligned on the label's line; drawn, not an item) and its optional swatch. A translated
    // value text is copied whole (a fixed buffer could cut a UTF-8 character of a long CJK or Greek name in half).
    char number[48];
    if (!o.valueText) std::snprintf(number, sizeof number, o.format ? o.format : "%.2f", *v * o.displayScale + o.displayOffset);
    const char* value = o.valueText ? T(o.valueText) : number;
    const float valueW = ImGui::CalcTextSize(value).x;
    const float swatchS = o.swatch ? lineH * 0.7f : 0.0f;
    const float swatchW = o.swatch ? swatchS + 6.0f * u : 0.0f;
    const float right = lineP.x + width;
    if (o.swatch)
        dl->AddRectFilled(ImVec2(right - valueW - swatchW, lineP.y + (lineH - swatchS) * 0.5f), ImVec2(right - valueW - swatchW + swatchS, lineP.y + (lineH + swatchS) * 0.5f),
                          WithStyleAlpha(o.swatch), 3.0f * u);
    dl->AddText(ImVec2(right - valueW, lineP.y), U32(VioletTheme::kTextMuted), value);

    // Label (after the optional icon), the description under it
    float textX = startX;
    if (o.icon != IconId::None) {
        const float is = kIconSmall * u;
        DrawIcon(dl, o.icon, ImVec2(lineP.x, lineP.y + (lineH - is) * 0.5f), is, ImGui::GetColorU32(ImGuiCol_Text));
        textX = startX + is + 7.0f * u;
    }
    ImGui::SetCursorPosX(textX);
    LabelInfo li;
    {
        const std::string_view shown = g_rowRaw ? std::string_view(label, VisibleEnd(label) - label) : I18n::TrLabel(label);
        ImGui::BeginGroup();
        ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
        ImGui::PushTextWrapPos(std::fmax(startX + width - valueW - swatchW - kSpace3 * u - DecorWidth(d), textX + 40.0f * u));
        ImGui::TextWrapped("%.*s", static_cast<int>(shown.size()), shown.data());
        li.right = ImGui::GetItemRectMax().x;
        li.top = ImGui::GetItemRectMin().y;
        li.lineH = lineH;
        ImGui::PopTextWrapPos();
        RowDescription(o.tooltip, startX + width);
        ImGui::PopStyleVar();
        ImGui::EndGroup();
    }

    // The stock slider does the input (drag, keyboard, deactivate-after-edit); only its look is replaced
    ImGui::SetCursorPosX(startX);
    const ImGuiCol hidden[] = {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive};
    for (ImGuiCol c : hidden) ImGui::PushStyleColor(c, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderFloat("##Slider", v, min, max, "", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(IM_COUNTOF(hidden));
    g_lastSliderCommitted = ImGui::IsItemDeactivatedAfterEdit();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (active) g_sliderActiveFrame = ImGui::GetFrameCount();

    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float cy = (a.y + b.y) * 0.5f;
    // Same mapping as ImGui's slider: the grab centre travels between half a grab from each end (plus its fixed 2 px
    // padding), so the drawn grab sits exactly where ImGui's hit-tested grab is
    const float grabHalf = ImGui::GetStyle().GrabMinSize * 0.5f + 2.0f;
    const float x0 = a.x + grabHalf, x1 = b.x - grabHalf;
    const float t = max > min ? std::fmin(std::fmax((*v - min) / (max - min), 0.0f), 1.0f) : 0.0f;
    const float gx = x0 + (x1 - x0) * t;
    const float track = 2.0f * u;
    if (o.hueTrack) {
        // The hue circle over the whole track (red, yellow, green, cyan, blue, magenta, red)
        static const ImU32 kHues[7] = {IM_COL32(255, 0, 0, 255),   IM_COL32(255, 255, 0, 255), IM_COL32(0, 255, 0, 255), IM_COL32(0, 255, 255, 255),
                                       IM_COL32(0, 0, 255, 255),   IM_COL32(255, 0, 255, 255), IM_COL32(255, 0, 0, 255)};
        const float h = 1.5f * track;
        for (int i = 0; i < 6; i++) {
            const float xa = x0 + (x1 - x0) * static_cast<float>(i) / 6.0f, xb = x0 + (x1 - x0) * static_cast<float>(i + 1) / 6.0f;
            const ImU32 ca = WithStyleAlpha(kHues[i]), cb = WithStyleAlpha(kHues[i + 1]);
            dl->AddRectFilledMultiColor(ImVec2(xa, cy - h), ImVec2(xb, cy + h), ca, cb, cb, ca);
        }
    } else if (o.trackFrom && o.trackTo) {
        // A colour gradient over the whole track
        const float h = 1.5f * track;
        const ImU32 ca = WithStyleAlpha(o.trackFrom), cb = WithStyleAlpha(o.trackTo);
        dl->AddRectFilled(ImVec2(x0 - h, cy - h), ImVec2(x0 + h, cy + h), ca, h);
        dl->AddRectFilled(ImVec2(x1 - h, cy - h), ImVec2(x1 + h, cy + h), cb, h);
        dl->AddRectFilledMultiColor(ImVec2(x0, cy - h), ImVec2(x1, cy + h), ca, cb, cb, ca);
    } else {
        dl->AddRectFilled(ImVec2(x0, cy - track), ImVec2(x1, cy + track), U32(VioletTheme::kToggleOff), track);
        dl->AddRectFilled(ImVec2(x0, cy - track), ImVec2(gx, cy + track), U32(VioletTheme::kAccent), track);
    }
    const float grabR = std::fmax(ImGui::GetStyle().GrabMinSize * 0.5f, 6.0f * u) + (active ? 1.0f : hovered ? 0.5f : 0.0f) * u;
    if (o.hueTrack || (o.trackFrom && o.trackTo)) dl->AddCircle(ImVec2(gx, cy), grabR + 1.0f * u, U32(0x000000, 0.35f), 0, 1.5f * u); // readable on light colours
    dl->AddCircleFilled(ImVec2(gx, cy), grabR, U32(0xFFFFFF));

    // End labels under the track
    if (o.leftLabel || o.rightLabel) {
        // Right under the track (the slider frame's own padding is the gap)
        PushSized(nullptr, kSmallScale);
        const float fs = ImGui::GetFontSize();
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(width, fs));
        const ImU32 col = U32(VioletTheme::kTextMuted);
        // Long translations of the two end labels must not run into each other: when both do not fit with a gap,
        // each gets half the track and is cut with "…" (full text on hover)
        const char* left = o.leftLabel ? T(o.leftLabel) : nullptr;
        const char* right = o.rightLabel ? T(o.rightLabel) : nullptr;
        const float leftW = left ? ImGui::CalcTextSize(left).x : 0.0f, rightW = right ? ImGui::CalcTextSize(right).x : 0.0f;
        const float gapW = kSpace3 * u, trackW = b.x - a.x;
        const bool crowded = leftW + rightW + (left && right ? gapW : 0.0f) > trackW;
        const float half = left && right ? (trackW - gapW) * 0.5f : trackW;
        if (left) {
            const float maxW = crowded ? half : leftW;
            if (DrawFitted(dl, ImVec2(a.x, b.y), col, left, maxW)) FullTextTooltipAt(ImVec2(a.x, b.y), maxW, left);
        }
        if (right) {
            std::string cut;
            std::string_view shown = right;
            const bool fitted = crowded && FitText(shown, half, cut);
            if (fitted) shown = cut;
            const ImVec2 at(b.x - ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x, b.y); // right-aligned
            dl->AddText(at, col, shown.data(), shown.data() + shown.size());
            if (fitted) FullTextTooltipAt(at, b.x - at.x, right);
        }
        ImGui::PopFont();
    }
    const float bottom = LocalY(ImGui::GetItemRectMax().y);
    if (g_lastSliderCommitted) ReportLabel(label, "{} changed");
    const ImVec2 rowMin = ToScreen(startX, top);
    if (RowDecorations(d, li, rowMin, ImVec2(rowMin.x + width, ToScreen(startX, bottom).y))) {
        *v = o.defaultValue;
        changed = true;
        g_lastSliderCommitted = true; // callers that save on release save now
        ReportLabel(label, "{} reset");
    }
    RowFinish(startX, bottom);
    if (opaque) ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

bool SliderCommitted() { return g_lastSliderCommitted; }

bool SliderPercent(const char* label, float* v, float min, float max, const char* tooltip, float def, IconId icon) {
    SliderOptions o;
    o.format = "%.0f%%";
    o.displayScale = 100.0f;
    o.tooltip = tooltip;
    o.icon = icon;
    o.defaultValue = def;
    return Slider(label, v, min, max, o);
}

bool SliderFloat(const char* label, float* v, float min, float max, const char* format) {
    SliderOptions o;
    o.format = format;
    return Slider(label, v, min, max, o);
}

// ---- cards ----

bool BeginCard(const char* id, bool warning) {
    if (Hidden()) return true; // search: no frame, the rows go straight into the results
    const float u = Unit();
    ImVec4 background = Col(VioletTheme::kCardBg), border = Col(VioletTheme::kCardBorder);
    if (warning) {
        const ImVec4 tint = Col(VioletTheme::kWarning);
        background.x += (tint.x - background.x) * 0.12f;
        background.y += (tint.y - background.y) * 0.12f;
        background.z += (tint.z - background.z) * 0.12f;
        border.x += (tint.x - border.x) * 0.35f;
        border.y += (tint.y - border.y) * 0.35f;
        border.z += (tint.z - border.z) * 0.35f;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kSpace4 * u, kSpace3 * u));
    // Wheel scrolling goes on to the page (the card never scrolls itself)
    const bool visible = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // Read by BeginChild; popped at once so popups, tooltips and nested children opened inside keep the theme
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return visible;
}

void EndCard() {
    if (Hidden()) return;
    ImGui::EndChild();
    // kSpace3 between cards (the spacing after the card and after this dummy make up the rest)
    ImGui::Dummy(ImVec2(0.0f, std::fmax(0.0f, kSpace3 * Unit() - 2.0f * Spacing())));
}

void CardDivider() {
    if (Hidden()) return;
    const float u = Unit();
    const float lineY = ImGui::GetCursorPosY() - Spacing() + kSpace3 * u; // kSpace3 below the header
    HairlineAt(lineY);
    ImGui::SetCursorPosY(lineY + 1.0f + kSpace3 * u - Spacing());
    ImGui::Dummy(ImVec2(0.0f, 0.0f)); // the body starts kSpace3 below the line
    g_dividerWindow = ImGui::GetCurrentWindow()->ID;
    g_dividerFrame = ImGui::GetFrameCount();
    g_dividerBodyY = ImGui::GetCursorPosY();
}

bool CardHeader(IconId icon, const char* title, const char* subtitle, const char* tooltip, bool* toggle, bool toggleEnabled, HeaderExtra* extra,
                const char* chip) {
    if (extra) {
        extra->clicked = false;
        extra->held = false;
    }
    if (Hidden()) {
        // Search: a header with a switch is a searchable row (title + subtitle); anything else draws nothing
        if (!toggle) return false;
        ImGui::BeginDisabled(!toggleEnabled);
        const bool clicked = SwitchRow(title, toggle, subtitle);
        ImGui::EndDisabled();
        return clicked;
    }
    const float u = Unit();
    const float startX = ImGui::GetCursorPosX();
    const float startY = ImGui::GetCursorPosY();
    const ImVec2 rowScreen = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 toggleSize(kSwitchW * u, kSwitchH * u);
    const bool hasIcon = icon != IconId::None;
    const float iconBox = hasIcon ? 22.0f * u : 0.0f;
    const float bs = 24.0f * u; // header icon buttons

    // The right cluster, laid out from the right edge: [chip] [hold] [before / after] [badge] [switch]
    float x = startX + width;
    float toggleX = 0.0f, badgeX = 0.0f, extraX = 0.0f, holdX = 0.0f, chipX = 0.0f;
    const bool hasBadge = extra && extra->badge && *extra->badge;
    const ImVec2 badgeSize = hasBadge ? ChipSize(extra->badge) : ImVec2(0.0f, 0.0f);
    if (toggle) {
        toggleX = x - toggleSize.x;
        x = toggleX - kSpace3 * u;
    }
    if (hasBadge) {
        badgeX = x - badgeSize.x;
        x = badgeX - kSpace2 * u;
    }
    const bool hasExtra = extra && extra->value;
    if (hasExtra) {
        extraX = x - bs;
        x = extraX - kSpace1 * u;
    }
    const bool hasHold = extra && extra->holdIcon != IconId::None;
    if (hasHold) {
        holdX = x - bs;
        x = holdX - kSpace1 * u;
    }
    const ImVec2 chipSize = chip && *chip ? ChipSize(chip) : ImVec2(0.0f, 0.0f);
    if (chipSize.x > 0.0f) {
        chipX = x - chipSize.x;
        x = chipX - kSpace2 * u;
    }
    const float rightW = startX + width - x;

    // Icon column (drawn after the text, centred on it)
    if (hasIcon) {
        ImGui::Dummy(ImVec2(iconBox, 1.0f));
        ImGui::SameLine(0.0f, 10.0f * u);
    }
    ImGui::BeginGroup();
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 1.0f * u);
    // Title and subtitle wrap short of the right cluster (switch, badges, chips), so a long translated title takes a
    // second line instead of running under the switch
    const float textWrapX = std::fmax(startX + width - rightW, startX + iconBox + 60.0f * u);
    PushSized(VioletTheme::BoldFont(), kCardTitleScale);
    ImGui::PushTextWrapPos(textWrapX);
    ImGui::TextUnformatted(T(title));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (subtitle && *subtitle) {
        ImGui::PushTextWrapPos(textWrapX);
        ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
        ImGui::TextWrapped("%s", T(subtitle));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    Tooltip(tooltip);
    const float rowH = ImGui::GetItemRectMax().y - rowScreen.y;

    if (hasIcon) {
        const float s = kIconMedium * u;
        DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(rowScreen.x + (iconBox - s) * 0.5f, rowScreen.y + (rowH - s) * 0.5f), s, U32(VioletTheme::kAccent));
    }

    if (chipSize.x > 0.0f) {
        ImGui::SetCursorPos(ImVec2(chipX, startY + std::fmax(0.0f, (rowH - chipSize.y) * 0.5f)));
        ChipImpl(chip, VioletTheme::kTextMuted);
        Tooltip(kCostChipTooltip);
    }
    if (hasBadge) {
        ImGui::SetCursorPos(ImVec2(badgeX, startY + std::fmax(0.0f, (rowH - badgeSize.y) * 0.5f)));
        ChipImpl(extra->badge, VioletTheme::kWarning);
        Tooltip(extra->badgeTooltip);
    }
    if (hasHold) {
        ImGui::SetCursorPos(ImVec2(holdX, startY + std::fmax(0.0f, (rowH - bs) * 0.5f)));
        ImGui::BeginDisabled(!extra->enabled);
        IconButtonImpl("##HeaderHold", extra->holdIcon, extra->holdTooltip, false, 24.0f);
        extra->held = extra->enabled && ImGui::IsItemActive();
        ImGui::EndDisabled();
    }
    if (hasExtra) {
        ImGui::SetCursorPos(ImVec2(extraX, startY + std::fmax(0.0f, (rowH - bs) * 0.5f)));
        ImGui::BeginDisabled(!extra->enabled);
        extra->clicked = IconButtonImpl("##HeaderExtra", *extra->value ? extra->iconOn : extra->iconOff, extra->tooltip, *extra->value, 24.0f);
        if (extra->clicked) *extra->value = !*extra->value;
        ImGui::EndDisabled();
    }

    bool clicked = false;
    if (toggle) {
        ImGui::SetCursorPos(ImVec2(toggleX, startY + std::fmax(0.0f, (rowH - toggleSize.y) * 0.5f)));
        ImGui::BeginDisabled(!toggleEnabled);
        clicked = ToggleSwitch("##Enabled", toggle);
        ImGui::EndDisabled();
        Tooltip(tooltip);
        if (clicked) ReportLabel(title, *toggle ? "{} turned on" : "{} turned off");
    }
    // The next item starts below the whole header
    ImGui::SetCursorPos(ImVec2(startX, startY + rowH));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return clicked;
}

bool OverviewRow(const char* id, IconId icon, const char* name, const char* phrase, const char* tooltip, bool* on, bool enabled, const char* rightText,
                 bool* nameClicked, const char* chip, const char* summary, bool attention) {
    if (nameClicked) *nameClicked = false;
    if (Hidden()) return false;
    const float u = Unit();
    ImGui::PushID(id);
    const float startX = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop();
    const ImVec2 sw(kSwitchW * u, kSwitchH * u);
    const ImVec2 pill = rightText ? PillSize(rightText, false) : ImVec2(0.0f, 0.0f);
    const ImVec2 rightSize = on ? sw : pill;
    const ImVec2 chipSize = chip && *chip ? ChipSize(chip) : ImVec2(0.0f, 0.0f);
    const float chipW = chipSize.x > 0.0f ? chipSize.x + kSpace2 * u : 0.0f;
    const float iconBox = 22.0f * u;
    const float textX = startX + iconBox + kSpace3 * u;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorPos(ImVec2(startX, top));
    const ImVec2 rowScreen = ImGui::GetCursorScreenPos();
    ImGui::SetCursorPos(ImVec2(textX, top));
    ImGui::BeginGroup();
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
    // The name is a link to the feature's page
    const ImVec2 np = ImGui::GetCursorScreenPos();
    const float nameWidth = std::fmax(20.0f * u, startX + width - rightSize.x - chipW - kSpace3 * u - textX);
    const ImVec2 ns = ImGui::CalcTextSize(T(name), nullptr, false, nameWidth);
    const bool clicked = ImGui::InvisibleButton("##Name", ns, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 nameCol = hovered ? U32(VioletTheme::kAccentLight) : U32(VioletTheme::kText);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), np, nameCol, T(name), nullptr, nameWidth);
    if (hovered) dl->AddLine(ImVec2(np.x, np.y + ns.y), ImVec2(np.x + ns.x, np.y + ns.y), nameCol, 1.0f);
    Tooltip(tooltip);
    if (nameClicked) *nameClicked = clicked;
    RowDescription(phrase, startX + width - rightSize.x - chipW - kSpace3 * u);
    if (summary && *summary) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(attention ? VioletTheme::kWarning : VioletTheme::kTextMuted));
        ImGui::PushTextWrapPos(startX + width - rightSize.x - chipW - kSpace3 * u);
        ImGui::TextUnformatted(T(summary));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const float rowH = std::fmax(ImGui::GetItemRectMax().y - rowScreen.y, rightSize.y);
    const float is = kIconMedium * u;
    DrawIcon(dl, icon, ImVec2(rowScreen.x + (iconBox - is) * 0.5f, rowScreen.y + (rowH - is) * 0.5f), is, U32(VioletTheme::kAccent));

    if (chipSize.x > 0.0f) {
        ImGui::SetCursorPos(ImVec2(startX + width - rightSize.x - chipW, top + (rowH - chipSize.y) * 0.5f));
        ChipImpl(chip, VioletTheme::kTextMuted);
        Tooltip(kCostChipTooltip);
    }
    bool toggled = false;
    if (on) {
        ImGui::SetCursorPos(ImVec2(startX + width - sw.x, top + (rowH - sw.y) * 0.5f));
        ImGui::BeginDisabled(!enabled);
        toggled = ToggleSwitch("##On", on);
        ImGui::EndDisabled();
        Tooltip(tooltip);
        if (toggled) ReportLabel(name, *on ? "{} turned on" : "{} turned off");
    } else if (rightText) {
        ImGui::SetCursorPos(ImVec2(startX + width - pill.x, top + (rowH - pill.y) * 0.5f));
        Pill(rightText, false);
        Tooltip(tooltip);
    }
    RowFinish(startX, top + rowH);
    ImGui::PopID();
    return toggled;
}

// Profile choices share Overview typography, icon geometry and row spacing.
bool ProfileChoiceRow(const char* id, IconId icon, const char* name, const char* description, bool selected) {
    if (Hidden()) return false;
    ImGui::PushID(id);
    const float u = Unit(), startX = ImGui::GetCursorPosX(), width = ImGui::GetContentRegionAvail().x;
    const float top = RowTop(), textX = startX + 22.0f * u + kSpace3 * u;
    ImGui::SetCursorPos(ImVec2(startX, top));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorPos(ImVec2(textX, top));
    ImGui::BeginGroup();
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
    ImGui::PushTextWrapPos(startX + width - 32.0f * u - kSpace3 * u); // short of the radio circle, like the description
    ImGui::TextUnformatted(T(name));
    ImGui::PopTextWrapPos();
    RowDescription(description, startX + width - 32.0f * u - kSpace3 * u);
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const float h = std::fmax(ImGui::GetItemRectMax().y - p.y, 22.0f * u);
    ImGui::SetCursorPos(ImVec2(startX, top));
    const bool clicked = ImGui::InvisibleButton("##Choice", ImVec2(width, h), ImGuiButtonFlags_EnableNav);
    auto* draw = ImGui::GetWindowDrawList();
    const float size = kIconMedium * u;
    DrawIcon(draw, icon, ImVec2(p.x + (22.0f * u - size) * 0.5f, p.y + (h - size) * 0.5f), size, U32(VioletTheme::kAccent));
    const ImVec2 center(p.x + width - 11.0f * u, p.y + h * 0.5f);
    draw->AddCircle(center, 8.0f * u, U32(selected || ImGui::IsItemHovered() ? VioletTheme::kAccent : VioletTheme::kCardBorder), 24, 1.5f * u);
    if (selected) draw->AddCircleFilled(center, 4.0f * u, U32(VioletTheme::kAccent));
    RowFinish(startX, top + h);
    ImGui::PopID();
    return clicked;
}

} // namespace ApexUi
