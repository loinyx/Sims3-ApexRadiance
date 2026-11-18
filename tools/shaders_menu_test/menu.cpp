// Draws Image > Shaders and Image > Capture in every language, at two widths and two text scales, in each state, and
// checks the layout of the last of four frames from the recorded controls (rec.h) and the text in the draw lists.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"
#include "imgui_internal.h"
#include "rec.h"
#undef ImGui
#include "ui/violet_theme.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "fixture.h"
#include "patches/looks.h"
#include "build_flavor.h"

void ShadersPage();
void CapturePage();
extern int g_shadersTab, g_captureTab;
extern bool g_menuGameAaOn;
extern class ApexPatch* g_looksPatch;
extern bool g_fixtureTwin;
#include "raster.inc"

namespace {
struct State { const char* name; int page; int tab; bool compiling, edited, offer; int batch; int video, options; bool gameAa; bool openAll; bool twin = false; int confirm = 0; const char* modal = nullptr; };
// page 0 Shaders (tab 0 Look, 1 Installed shaders), 1 Capture (tab 0 Screenshots, 1 Video)
const State kStates[] = {
    { "look", 0, 0, false, false, true, 0, 0, 0, false, false },
    { "look-open-compiling", 0, 0, true, false, false, 1, 0, 0, false, true },
    { "look-open-edited", 0, 0, false, true, true, 0, 0, 0, false, true },
    { "look-doubled-lut", 0, 0, false, false, false, 0, 0, 0, false, false, true },
    { "look-downloading", 0, 0, false, false, false, 1, 0, 0, false, false },
    { "look-open", 0, 0, false, false, false, 0, 0, 0, false, true },
    { "look-finished", 0, 0, false, false, false, 2, 0, 0, false, false },
    { "installed", 0, 1, false, false, false, 0, 0, 0, false, true },
    { "shots", 1, 0, false, false, false, 0, 0, 0, false, false },
    { "shots-advanced", 1, 0, false, false, false, 0, 0, 1, false, true },
    { "video-idle-advanced", 1, 1, false, false, false, 0, 0, 1, true, true },
    { "video-recording", 1, 1, false, false, false, 0, 1, 0, false, true },
    { "video-saving", 1, 1, false, false, false, 0, 2, 1, false, false },
    { "look-doubled", 0, 0, false, false, false, 0, 0, 0, false, false, true },
    { "installed-remove-pack", 0, 1, false, false, false, 0, 0, 0, false, false, false, 1 },
    { "installed-remove-file", 0, 1, false, false, false, 0, 0, 0, false, false, false, 2 },
    { "look-add", 0, 0, false, false, false, 1, 0, 0, false, false, false, 0, "##AddShaders" },
};

struct Run { ImRect r; std::string text; };
std::unordered_map<uint64_t, unsigned> g_glyphs; // atlas U0,V0 -> codepoint

uint64_t UvKey(ImVec2 uv) { return (uint64_t)(uint32_t)std::lround(uv.x * 1e6f) << 32 | (uint32_t)std::lround(uv.y * 1e6f); }
void IndexGlyphs()
{
    g_glyphs.clear();
    ImFontAtlasBuilder* b = ImGui::GetIO().Fonts->Builder;
    for (int i = 0; b && i < b->BakedPool.Size; ++i)
        for (const ImFontGlyph& gl : b->BakedPool[i].Glyphs) g_glyphs[UvKey(ImVec2(gl.U0, gl.V0))] = gl.Codepoint;
}
void AppendUtf8(std::string& s, unsigned c)
{
    char buf[5] = {};
    ImTextCharToUtf8(buf, c);
    s += buf;
}

// The text runs of a draw list: glyph quads (4 vertices, 6 indices, atlas UVs other than the white pixel), one run per line
std::vector<Run> TextRuns(const ImDrawList* dl)
{
    std::vector<Run> runs;
    const ImVec2 white = dl->_Data->TexUvWhitePixel;
    for (const ImDrawCmd& cmd : dl->CmdBuffer)
    {
        if (cmd.UserCallback) continue;
        const ImDrawIdx* idx = dl->IdxBuffer.Data + cmd.IdxOffset;
        int lastBase = -10;
        float lastMinX = 0.0f;
        for (unsigned k = 0; k + 6 <= cmd.ElemCount;)
        {
            const unsigned a = idx[k];
            const bool quad = idx[k + 1] == a + 1 && idx[k + 2] == a + 2 && idx[k + 3] == a && idx[k + 4] == a + 2 && idx[k + 5] == a + 3;
            const ImDrawVert* v = dl->VtxBuffer.Data + cmd.VtxOffset + a;
            if (!quad || v[0].pos.y != v[1].pos.y || v[1].pos.x != v[2].pos.x || v[2].pos.y != v[3].pos.y || v[3].pos.x != v[0].pos.x ||
                (v[0].uv.x == white.x && v[0].uv.y == white.y) || v[0].uv.x == v[2].uv.x || v[0].uv.y == v[2].uv.y)
            {
                k += 3;
                continue;
            }
            const ImRect r(v[0].pos, v[2].pos);
            const auto found = g_glyphs.find(UvKey(v[0].uv));
            const unsigned c = found == g_glyphs.end() ? '?' : found->second;
            Run* last = runs.empty() ? nullptr : &runs.back();
            // one run: glyphs written one after the other on one line, each after the one before (a word gap at most) or, a
            // combining mark (Thai vowels and tones), back over it
            const bool mark = c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E) || (c >= 0x0300 && c <= 0x036F);
            const bool joins = last && (int)a == lastBase + 4 &&
                               (mark ? r.Min.x >= last->r.Min.x - 1.0f && r.Min.y < last->r.Max.y + r.GetHeight() && r.Max.y > last->r.Min.y - r.GetHeight()
                                     : r.Min.y < last->r.Max.y && r.Max.y > last->r.Min.y && r.Min.x >= lastMinX) &&
                               r.Min.x - last->r.Max.x < r.GetHeight() * 0.6f;
            if (joins)
            {
                if (r.Min.x - last->r.Max.x > r.GetHeight() * 0.2f) last->text += ' ';
                last->r.Add(r);
                AppendUtf8(last->text, c);
            }
            else
            {
                runs.push_back({ r, "" });
                AppendUtf8(runs.back().text, c);
            }
            if (!mark) lastMinX = r.Min.x;
            lastBase = (int)a;
            k += 6;
        }
    }
    return runs;
}

std::string ShortName(const char* name)
{
    std::string s = name, out;
    for (size_t i = 0; i < s.size(); ++i) // "##Card_1A2B3C4D" -> "##Card"
    {
        if (s[i] == '_' && i + 9 <= s.size() && std::all_of(s.begin() + i + 1, s.begin() + i + 9, [](char c) { return isxdigit((unsigned char)c); }) &&
            (i + 9 == s.size() || s[i + 9] == '/'))
        {
            i += 8;
            continue;
        }
        out += s[i];
    }
    return out;
}
std::string Clip(const std::string& s) { return s.size() > 48 ? s.substr(0, 45) + "..." : s; }
bool Overlaps(const ImRect& a, const ImRect& b, float tol = 1.0f)
{
    return std::min(a.Max.x, b.Max.x) - std::max(a.Min.x, b.Min.x) > tol && std::min(a.Max.y, b.Max.y) - std::max(a.Min.y, b.Min.y) > tol;
}
bool Inside(const ImRect& in, const ImRect& out, float tol = 1.0f)
{
    return in.Min.x >= out.Min.x - tol && in.Min.y >= out.Min.y - tol && in.Max.x <= out.Max.x + tol && in.Max.y <= out.Max.y + tol;
}

// Invisible hit areas laid over a row's own text (the name side of a shader row, a group label, Advanced)
bool HitArea(const std::string& id) { return id == "##Toggle" || id == "##Group" || id == "##Advanced" || id == "##HeaderLink" || id == "##Link"; }

struct Finding { int count = 0; std::string example; std::set<std::string> where; };
std::map<std::string, Finding> g_findings;
long long g_controls = 0, g_runs = 0; // checked
int g_newFindings = 0; // findings first seen in this case (its preview is saved)

void Report(const std::string& key, const std::string& example, const std::string& where)
{
    Finding& f = g_findings[key];
    if (!f.count) { f.example = example + " [" + where + "]"; ++g_newFindings; }
    ++f.count;
    f.where.insert(where);
}

// Horizontal overflow, controls overlapping each other, text over a control, text over text
const char* g_checkOnly = nullptr; // CheckLayout looks only at this window and its children (the modal sweep)
std::string g_dump; // "<state> <lang>/<units>/<scale>%": that case's controls and text runs are printed
void CheckLayout(const std::string& state, const std::string& where)
{
    const bool dump = g_dump == state.substr(state.find(' ') + 1) + " " + where;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    IndexGlyphs();
    std::map<ImGuiWindow*, std::vector<const RecItem*>> controls;
    for (const RecItem& it : RecItems()) controls[it.window].push_back(&it);
    for (ImGuiWindow* w : g.Windows)
    {
        if (!w->WasActive || w->Hidden || (g_checkOnly && strncmp(w->Name, g_checkOnly, strlen(g_checkOnly)) != 0)) continue;
        const float right = w->ContentRegionRect.Max.x, bottom = w->InnerClipRect.Max.y;
        const bool scrolls = w->ScrollMax.y > 0.0f; // a list that scrolls inside its card
        const std::string win = ShortName(w->Name);
        auto& cs = controls[w];
        g_controls += (long long)cs.size();
        char buf[512];
        for (size_t i = 0; i < cs.size(); ++i)
        {
            const RecItem& a = *cs[i];
            if (!scrolls && a.rect.Max.y > bottom + 1.0f)
            {
                snprintf(buf, sizeof buf, "control %.0f..%.0f, window bottom %.0f", a.rect.Min.y, a.rect.Max.y, bottom);
                Report(state + " | cut off at the bottom | " + win + " | control " + a.id, buf, where);
            }
            if (a.rect.Max.x > right + 1.0f)
            {
                snprintf(buf, sizeof buf, "%.0f px past the right edge (control %.0f..%.0f, edge %.0f)", a.rect.Max.x - right, a.rect.Min.x, a.rect.Max.x, right);
                Report(state + " | overflow | " + win + " | control " + a.id, buf, where);
            }
            for (size_t j = i + 1; j < cs.size(); ++j)
            {
                const RecItem& b = *cs[j];
                if (!Overlaps(a.rect, b.rect)) continue;
                snprintf(buf, sizeof buf, "%s %.0f,%.0f-%.0f,%.0f and %s %.0f,%.0f-%.0f,%.0f", a.id.c_str(), a.rect.Min.x, a.rect.Min.y, a.rect.Max.x, a.rect.Max.y, b.id.c_str(),
                         b.rect.Min.x, b.rect.Min.y, b.rect.Max.x, b.rect.Max.y);
                Report(state + " | controls overlap | " + win + " | " + a.id + " / " + b.id, buf, where);
            }
        }
        const std::vector<Run> runs = TextRuns(w->DrawList);
        g_runs += (long long)runs.size();
        if (dump)
        {
            printf("window %s content %.0f..%.0f\n", win.c_str(), w->ContentRegionRect.Min.x, right);
            for (const RecItem* c : cs) printf("  control %s %.0f,%.0f-%.0f,%.0f\n", c->id.c_str(), c->rect.Min.x, c->rect.Min.y, c->rect.Max.x, c->rect.Max.y);
            for (const Run& t : runs) printf("  text \"%s\" %.0f,%.0f-%.0f,%.0f\n", t.text.c_str(), t.r.Min.x, t.r.Min.y, t.r.Max.x, t.r.Max.y);
        }
        for (size_t i = 0; i < runs.size(); ++i)
        {
            const Run& t = runs[i];
            if (!scrolls && t.r.Min.y > bottom)
            {
                snprintf(buf, sizeof buf, "\"%s\" below the window bottom %.0f", Clip(t.text).c_str(), bottom);
                Report(state + " | text cut off at the bottom | " + win, buf, where);
            }
            if (t.r.Max.x > right + 1.0f)
            {
                snprintf(buf, sizeof buf, "\"%s\" ends %.0f px past the right edge", Clip(t.text).c_str(), t.r.Max.x - right);
                Report(state + " | text overflow | " + win, buf, where);
            }
            for (const RecItem* c : cs)
                if (!HitArea(c->id) && Overlaps(t.r, c->rect) && !Inside(t.r, c->rect))
                {
                    snprintf(buf, sizeof buf, "\"%s\" %.0f,%.0f-%.0f,%.0f crosses %s %.0f,%.0f-%.0f,%.0f", Clip(t.text).c_str(), t.r.Min.x, t.r.Min.y, t.r.Max.x, t.r.Max.y,
                             c->id.c_str(), c->rect.Min.x, c->rect.Min.y, c->rect.Max.x, c->rect.Max.y);
                    Report(state + " | text over control | " + win + " | " + c->id, buf, where);
                }
            for (size_t j = i + 1; j < runs.size(); ++j)
                if (Overlaps(t.r, runs[j].r, std::min(t.r.GetHeight(), runs[j].r.GetHeight()) * 0.25f))
                {
                    snprintf(buf, sizeof buf, "\"%s\" over \"%s\"", Clip(t.text).c_str(), Clip(runs[j].text).c_str());
                    Report(state + " | text over text | " + win, buf, where);
                }
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) return 2;
    if (argc == 3) g_dump = argv[2];
    const std::filesystem::path out = std::filesystem::absolute(argv[1]);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // as the game's DX9 backend: glyphs rasterised on demand
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
    VioletTheme::ApplyStyle(ImGui::GetStyle());
    VioletTheme::LoadFonts(io);
    g_looksPatch = Fixture::StartLooks();
    if (!Fixture::SaveRoundTrip()) return 1;
    kPublicBuild = false; // I18n notes missing translations only in developer mode
    {
        // self-check: a frame with each fault must give each finding
        io.DisplaySize = ImVec2(400, 300);
        io.DeltaTime = 1.0f / 60.0f;
        for (int frame = 0; frame < 2; ++frame)
        {
            RecItems().clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("SelfCheck", nullptr, ImGuiWindowFlags_NoTitleBar);
            ImGuiRec::InvisibleButton("##Wide", ImVec2(500, 30));        // past the right edge
            ImGui::SetCursorPos(ImVec2(50, 20));
            ImGuiRec::InvisibleButton("##Under", ImVec2(60, 30));        // over ##Wide
            ImGui::SetCursorPos(ImVec2(90, 80));
            ImGuiRec::InvisibleButton("##Box", ImVec2(60, 30));
            ImGui::SetCursorPos(ImVec2(40, 85));
            ImGui::TextUnformatted("Crossing the box");                 // text over a control
            ImGui::SetCursorPos(ImVec2(40, 150));
            ImGui::TextUnformatted("First text");
            ImGui::SetCursorPos(ImVec2(44, 152));
            ImGui::TextUnformatted("Second text");                      // text over text
            ImGui::End();
            ImGui::Render();
            if (ImVector<ImTextureData*>* textures = ImGui::GetDrawData()->Textures)
                for (ImTextureData* t : *textures)
                    if (t->Status != ImTextureStatus_OK && t->Status != ImTextureStatus_Destroyed) { t->SetTexID((ImTextureID)1); t->SetStatus(ImTextureStatus_OK); }
        }
        CheckLayout("self check", "-");
        int kinds = 0;
        for (const char* k : { "| overflow |", "| controls overlap |", "| text over control |", "| text over text |" })
            kinds += std::any_of(g_findings.begin(), g_findings.end(), [&](const auto& f) { return f.first.find(k) != std::string::npos; });
        if (kinds != 4) { printf("FAIL self check: %d of 4 kinds of finding detected\n", kinds); return 13; }
        g_findings.clear();
        g_newFindings = 0;
        g_controls = g_runs = 0;
    }
    const char* kCodes[] = { "en", "pt", "es", "fr", "de", "it", "nl", "pl", "ru", "cs", "hu", "el", "da", "sv", "no", "fi", "ja", "ko", "zh_hans", "zh_hant", "th" };
    int cases = 0, failures = 0;
    std::set<std::string> missing;
    const float height = 16000.0f;
    for (int lang = 0; lang < 21; ++lang)
        for (int units : { 343, 400, 900 })
            for (float scale : { 1.0f, 2.0f })
                for (int st = 0; st < (int)IM_COUNTOF(kStates); ++st)
                {
                    const State& s = kStates[st];
                    I18n::SetChoice(lang);
                    I18n::ClearMissing();
                    VioletTheme::UpdateFonts(io); // the language's script merged, as the menu does between frames
                    ImGui::GetStyle().FontScaleMain = scale;
                    Fixture::LooksState(s.compiling, s.edited, s.offer, s.batch, s.twin);
                    Fixture::PacksState(s.confirm);
                    g_fixtureTwin = s.twin;
                    Fixture::CaptureState(s.video, s.options);
                    g_menuGameAaOn = s.gameAa;
                    g_recOpenAll = s.openAll;
                    g_recModal = s.modal;
                    ImGui::GetCurrentContext()->OpenPopupStack.resize(0); // a modal of the last case closes
                    (s.page ? g_captureTab : g_shadersTab) = s.tab;
                    // no toggle stays open from the case before
                    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) w->StateStorage.Clear();
                    const std::string winName = "Apex";
                    const float pad = ImGui::GetStyle().WindowPadding.x;
                    const float width = units * scale + 2.0f * pad;
                    io.DisplaySize = ImVec2(width, height);
                    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
                    io.DeltaTime = 1.0f / 60.0f;
                    char where[96];
                    snprintf(where, sizeof where, "%s/%d/%.0f%%", kCodes[lang], units, scale * 100.0f);
                    for (int frame = 0; frame < 4; ++frame)
                    {
                        RecItems().clear();
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(io.DisplaySize);
                        ImGui::Begin(winName.c_str(), nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
                        ImGuiContext& g = *ImGui::GetCurrentContext();
                        const int vars = g.StyleVarStack.Size, cols = g.ColorStack.Size, ids = g.CurrentWindow->IDStack.Size;
                        if (s.page) CapturePage();
                        else
                        {
                            ShadersPage();
                            // the download bar as MainWindow floats it, here over the end of the page (MainWindow
                            // leaves the bar's last height free there)
                            if (const float barH = Looks::PacksBarHeight(); barH > 0.0f) ImGui::Dummy(ImVec2(0.0f, barH));
                            const float left = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
                            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                            const float end = ImGui::GetCursorScreenPos().y;
                            Looks::PacksBar(left, right, end);
                        }
                        if (g.StyleVarStack.Size != vars || g.ColorStack.Size != cols || g.CurrentWindow->IDStack.Size != ids)
                        {
                            printf("FAIL stack %s %s frame %d: style vars %d->%d, colours %d->%d, ids %d->%d\n", s.name, where, frame, vars, g.StyleVarStack.Size, cols,
                                   g.ColorStack.Size, ids, g.CurrentWindow->IDStack.Size);
                            return 10;
                        }
                        const float used = g.CurrentWindow->DC.CursorMaxPos.y + ImGui::GetStyle().WindowPadding.y;
                        ImGui::End();
                        ImGui::Render();
                        if (ImVector<ImTextureData*>* textures = ImGui::GetDrawData()->Textures) // the CPU copy is all the previews need
                            for (ImTextureData* t : *textures)
                                if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates)
                                {
                                    t->SetTexID((ImTextureID)1);
                                    t->SetStatus(ImTextureStatus_OK);
                                }
                                else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0)
                                {
                                    t->SetTexID(ImTextureID_Invalid);
                                    t->SetStatus(ImTextureStatus_Destroyed);
                                }
                        if (g.ErrorCountCurrentFrame)
                        {
                            printf("FAIL ImGui error recovery %s %s frame %d\n", s.name, where, frame);
                            return 11;
                        }
                        if (used > height - 10.0f)
                        {
                            printf("FAIL the page is taller than the test window %s %s\n", s.name, where);
                            return 12;
                        }
                        if (frame == 3)
                        {
                            const int before = g_newFindings;
                            CheckLayout(std::string(s.page ? "capture " : "shaders ") + s.name, where);
                            const bool preview = g_newFindings != before || ((lang == 0 || lang == 1) && (st == 0 || st == 1 || st == 3 || st == 4 || st == 7 || st == 9 || st >= 13) && (scale == 1.0f || (lang == 1 && units <= 400)));
                            if (preview)
                            {
                                const auto file = out / (std::string(kCodes[lang]) + "-" + std::to_string(units) + "-" + std::to_string((int)(scale * 100)) + "-" + s.name + ".ppm");
                                Rasterize(file, (int)width, (int)std::min(height, used + 8.0f));
                            }
                        }
                    }
                    if (I18n::MissingCount())
                    {
                        // Skip a text that is already a translation (a caller translates before a widget does) and the
                        // fixture's .fx author data (heap strings in the game, so never noted there)
                        std::set<std::string_view> values;
                        for (std::string_view k : I18n::AllKeys()) values.insert(I18n::Tr(k));
                        static const std::set<std::string_view> kFixtureText = { "Film print colors", "Exposure and saturation", "Black and white points",
                            "Where the effect is centred on the screen", "Screen", "Overlay", "Multiply", "Linear dodge (add)", "Off also shades the sky and far mountains",
                            "Multiplied into the highlights", "More samples look smoother", "Made by the author of this shader. Higher values of the radius cost more speed.",
                            "How strongly the shader mixes into the picture", "Each sample costs a little speed; most looks use between 8 and 32 samples.", "Any whole number", "Everything.fx", "MyFilmGrain.fx",
                            "A long tooltip from the .fx file that explains every detail of what this value changes in the picture", "Apex", "HEVC", "H.264" };
                        std::string list, line;
                        std::istringstream lines(I18n::MissingList(4000));
                        while (std::getline(lines, line))
                            if (!line.empty() && !values.count(line) && !kFixtureText.count(line)) list += line + "\n";
                        if (!list.empty()) missing.insert(std::string(kCodes[lang]) + ":\n" + list);
                    }
                    ++cases;
                }
    // The modals on real screens: each language, screens from 800x600 to 4K, text 100%, 150% and 200%. A modal must
    // fit on the screen (its window inside the display) besides the layout checks above.
    int modalCases = 0;
    for (int lang = 0; lang < 21; ++lang)
        for (ImVec2 display : { ImVec2(800, 600), ImVec2(1280, 720), ImVec2(1366, 768), ImVec2(1920, 1080), ImVec2(3840, 2160) })
            for (float scale : { 1.0f, 1.5f, 2.0f })
                for (const char* modal : { "##AddShaders", "##NewLook", "##SaveLook" })
                {
                    I18n::SetChoice(lang);
                    VioletTheme::UpdateFonts(io);
                    ImGui::GetStyle().FontScaleMain = scale;
                    Fixture::LooksState(false, false, false, 1, false);
                    Fixture::PacksState(0);
                    g_recOpenAll = false;
                    g_recModal = modal;
                    ImGui::GetCurrentContext()->OpenPopupStack.resize(0);
                    g_shadersTab = 0;
                    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) w->StateStorage.Clear();
                    io.DisplaySize = display;
                    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
                    char where[96];
                    snprintf(where, sizeof where, "%s/%.0fx%.0f/%.0f%%", kCodes[lang], display.x, display.y, scale * 100.0f);
                    for (int frame = 0; frame < 4; ++frame)
                    {
                        RecItems().clear();
                        ImGui::NewFrame();
                        // the page at the menu's default width at the left, the modal centred on the screen as in game
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(std::min(display.x, 343.0f * scale + 2.0f * ImGui::GetStyle().WindowPadding.x), height)); // the page as tall as its content (it scrolls in game)
                        ImGui::Begin("Apex", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
                        ShadersPage();
                        ImGui::End();
                        ImGui::Render();
                        if (ImVector<ImTextureData*>* textures = ImGui::GetDrawData()->Textures)
                            for (ImTextureData* t : *textures)
                                if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) { t->SetTexID((ImTextureID)1); t->SetStatus(ImTextureStatus_OK); }
                                else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) { t->SetTexID(ImTextureID_Invalid); t->SetStatus(ImTextureStatus_Destroyed); }
                        if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame)
                        {
                            printf("FAIL ImGui error recovery modal %s %s frame %d\n", modal, where, frame);
                            return 11;
                        }
                        if (frame != 3) continue;
                        ImGuiWindow* w = ImGui::FindWindowByName(modal);
                        if (!w || !w->WasActive) { Report(std::string("modal ") + modal + " | did not open", "-", where); continue; }
                        g_checkOnly = modal; // the page behind is clipped by the screen here (it scrolls in game)
                        CheckLayout(std::string("modal ") + (modal + 2), where);
                        g_checkOnly = nullptr;
                        if (w->Pos.x < -0.5f || w->Pos.y < -0.5f || w->Pos.x + w->Size.x > display.x + 0.5f || w->Pos.y + w->Size.y > display.y + 0.5f)
                        {
                            char buf[160];
                            snprintf(buf, sizeof buf, "window %.0f,%.0f %.0fx%.0f on a %.0fx%.0f screen", w->Pos.x, w->Pos.y, w->Size.x, w->Size.y, display.x, display.y);
                            Report(std::string("modal ") + modal + " | off the screen", buf, where);
                        }
                        if (w->ScrollMax.y > 0.5f) // its buttons below the screen's edge
                        {
                            char buf[160];
                            snprintf(buf, sizeof buf, "%.0f px to scroll in a %.0f px window", w->ScrollMax.y, w->Size.y);
                            Report(std::string("modal ") + modal + " | taller than the screen", buf, where);
                        }
                        if (lang == 1 && scale == 2.0f && display.y == 720.0f)
                            Rasterize(out / (std::string("pt-720p-200-") + (modal + 2) + ".ppm"), (int)display.x, (int)display.y);
                    }
                    ++modalCases;
                }
    for (const std::string& m : missing) printf("Missing translations:\n%s\n", m.c_str());
    for (const auto& [key, f] : g_findings)
    {
        ++failures;
        std::string w;
        int n = 0;
        for (const std::string& x : f.where) { if (n++ < 6) w += (w.empty() ? "" : ", ") + x; }
        printf("%s\n    %d cases (%s%s)\n    e.g. %s\n", key.c_str(), f.count, w.c_str(), f.where.size() > 6 ? ", ..." : "", f.example.c_str());
    }
    printf("%d cases (21 languages, widths 343 (the default window), 400 and 900 units, text 100%% and 200%%, %d states) and %d modal cases (3 modals, 5 screens, text 100%%, 150%% and 200%%), four frames each; %lld controls and %lld text runs checked: %d layout findings, %d missing-translation sets\n",
           cases, (int)IM_COUNTOF(kStates), modalCases, g_controls, g_runs, failures, (int)missing.size());
    ImGui::DestroyContext();
    return failures || !missing.empty() ? 1 : 0;
}
